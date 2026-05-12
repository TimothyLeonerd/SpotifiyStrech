#define _POSIX_C_SOURCE 200809L

#include <gtk/gtk.h>
#include <math.h>
#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/introspect.h>
#include <pulse/mainloop.h>
#include <pulse/sample.h>
#include <pulse/thread-mainloop.h>
#include <pulse/stream.h>
#include <pulse/simple.h>

#include "core.h"
#include "linux_audio_backend.h"
#include "platform.h"
#include "platform_linux.h"
#include "playback_renderer.h"
#include "recorder_core.h"
#include "recorder_state.h"

typedef struct {
  Recorder *rec;
  guint generation;
  RenderOutcome outcome;
  GByteArray *playback_pcm;
  guint64 source_frames;
  guint64 rendered_frames;
  gdouble speed;
  char error[256];
} RenderCompletion;

typedef struct {
  Recorder *rec;
  guint generation;
} RenderProgressTick;

static void set_error(Recorder *r, const char *msg) {
  g_mutex_lock(&r->mutex);
  g_strlcpy(r->last_error, msg ? msg : "", sizeof r->last_error);
  g_mutex_unlock(&r->mutex);
  if (msg && msg[0] != '\0') {
    g_printerr("[error] %s\n", msg);
  }
}

static void clear_error(Recorder *r) {
  set_error(r, "");
}

static void update_button_sensitivity(Recorder *r) {
  CoreUiState ui_state;
  void *ui_user_data = r->platform.backend.user_data;

  g_mutex_lock(&r->mutex);
  ui_state = core_build_ui_state(r->mode, r->render_intent.should_play);
  g_mutex_unlock(&r->mutex);

  if (r->platform.ui && r->platform.ui->set_controls_sensitive) {
    r->platform.ui->set_controls_sensitive(ui_user_data,
                                            ui_state.record_enabled,
                                            ui_state.play_pause_enabled,
                                            ui_state.loop_enabled,
                                            ui_state.stop_enabled);
  }
  if (r->platform.ui && r->platform.ui->set_play_pause_label) {
    r->platform.ui->set_play_pause_label(ui_user_data, ui_state.play_pause_label);
  }
}

static void invalidate_playback_buffer_locked(Recorder *r);
gboolean start_capture_thread(Recorder *r, gboolean reset_buffers);
void stop_capture_thread(Recorder *r, gboolean force_stopped);
gboolean start_playback_thread(Recorder *r);
void stop_playback_thread(Recorder *r, gboolean reset_cursor);

static GThread *cancel_render_locked(Recorder *r, AppMode next_mode) {
  GThread *render_thread = NULL;

  if (!r->render_pending) {
    return NULL;
  }

  render_thread = r->render_thread;
  recorder_core_cancel_render(&r->core, next_mode);
  r->render_thread = NULL;
  return render_thread;
}

static void reset_recording_session_locked(Recorder *r) {
  core_reset_recording_session(&r->audio,
                               &r->loop,
                               &r->render_intent,
                               &r->playback_cursor_frames,
                               &r->playback_anchor_frames,
                               &r->playback_anchor_us,
                               &r->display_playhead_frames);
  invalidate_playback_buffer_locked(r);
  r->scrubbing = FALSE;
  r->resume_after_scrub = FALSE;
}

static double get_playhead_ratio(Recorder *r);
static gboolean start_playback_with_ready_buffer(Recorder *r);
static gpointer playback_thread_main(gpointer user_data);
static void on_loop_toggled(GtkToggleButton *button, gpointer user_data);

static void update_display_playhead(Recorder *r) {
  AppMode mode;
  gboolean scrubbing = FALSE;
  gdouble display_frames = 0.0;

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  scrubbing = r->scrubbing;
  display_frames = r->display_playhead_frames;
  display_frames = core_update_display_playhead(mode,
                                                scrubbing,
                                                display_frames,
                                                r->playback_cursor_frames,
                                                r->playback_anchor_frames,
                                                r->playback_anchor_us,
                                                r->audio.sample_rate ? r->audio.sample_rate : 1,
                                                r->speed,
                                                g_get_monotonic_time());
  r->display_playhead_frames = display_frames;
  g_mutex_unlock(&r->mutex);
}

static void update_time_label(Recorder *r) {
  guint rate = 1;
  gdouble cursor_frames = 0.0;
  gdouble total_frames = 0.0;
  char *time_text = NULL;

  g_mutex_lock(&r->mutex);
  rate = r->audio.sample_rate ? r->audio.sample_rate : 1;
  cursor_frames = r->display_playhead_frames;
  total_frames = (gdouble)r->audio.captured_frames;
  g_mutex_unlock(&r->mutex);

  time_text = g_strdup_printf("%.1f / %.1fs", cursor_frames / (double)rate, total_frames / (double)rate);
  if (r->platform.ui && r->platform.ui->set_time_text) {
    r->platform.ui->set_time_text(r->platform.backend.user_data, time_text);
  }
  g_free(time_text);
}

static void refresh_ui(Recorder *r) {
  AppMode mode;
  guint64 frames;
  guint rate;
  gboolean loop_enabled = FALSE;
  gboolean loop_region_set = FALSE;
  char error[256];
  CoreStatusState status;
  double seconds;

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  frames = r->audio.captured_frames;
  rate = r->audio.sample_rate ? r->audio.sample_rate : 1;
  loop_enabled = r->loop.enabled;
  loop_region_set = r->loop.region_set;
  g_strlcpy(error, r->last_error, sizeof error);
  g_mutex_unlock(&r->mutex);

  seconds = (double)frames / (double)rate;
  status = core_build_status_state(mode, seconds, error, loop_enabled, loop_region_set);
  if (r->platform.ui && r->platform.ui->set_status_text) {
    r->platform.ui->set_status_text(r->platform.backend.user_data, status.text);
  }

  {
    update_time_label(r);
  }

  update_button_sensitivity(r);
  if (r->loop_toggled_handler_id != 0) {
    g_signal_handler_block(r->widgets.loop_button, r->loop_toggled_handler_id);
  }
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(r->widgets.loop_button), loop_enabled);
  if (r->loop_toggled_handler_id != 0) {
    g_signal_handler_unblock(r->widgets.loop_button, r->loop_toggled_handler_id);
  }
  if (r->platform.ui && r->platform.ui->queue_waveform_redraw) {
    r->platform.ui->queue_waveform_redraw(r->platform.backend.user_data);
  }

  {
    gint width = gtk_widget_get_allocated_width(r->widgets.waveform_base);
    gint height = gtk_widget_get_allocated_height(r->widgets.waveform_base);
    gint new_x = (gint)lrint(get_playhead_ratio(r) * (double)width);
    gint old_x = r->last_playhead_x;
    gint x1 = MIN(old_x, new_x) - 6;
    gint x2 = MAX(old_x, new_x) + 6;

    if (width > 0 && height > 0) {
      if (x1 < 0) x1 = 0;
      if (x2 > width) x2 = width;
      if (x2 > x1) {
        gtk_widget_queue_draw_area(r->widgets.waveform_base, x1, 0, x2 - x1, height);
      }
    }

    r->last_playhead_x = new_x;
  }
}

static void seek_to_fraction(Recorder *r, double fraction) {
  AppMode mode;
  gboolean scrubbing = FALSE;

  if (fraction < 0.0) {
    fraction = 0.0;
  }
  if (fraction > 1.0) {
    fraction = 1.0;
  }

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  scrubbing = r->scrubbing;
  recorder_core_seek_fraction(&r->core, fraction, TRUE);
  g_mutex_unlock(&r->mutex);

  if (mode == MODE_PLAYING && !scrubbing) {
    if (r->platform.audio && r->platform.audio->stop_playback) {
      r->platform.audio->stop_playback(r->platform.backend.audio_user_data, FALSE);
    }
    if (!r->platform.audio || !r->platform.audio->start_playback || !r->platform.audio->start_playback(r->platform.backend.audio_user_data)) {
      return;
    }
  } else {
    refresh_ui(r);
  }
}

static void begin_scrub(Recorder *r) {
  gboolean was_playing = FALSE;

  g_mutex_lock(&r->mutex);
  if (!recorder_core_begin_scrub(&r->core, &was_playing)) {
    g_mutex_unlock(&r->mutex);
    return;
  }
  g_mutex_unlock(&r->mutex);

  g_printerr("[scrub] begin resume=%d\n", was_playing ? 1 : 0);
  if (was_playing && r->platform.audio && r->platform.audio->stop_playback) {
    r->platform.audio->stop_playback(r->platform.backend.audio_user_data, FALSE);
  }
}

static void update_scrub(Recorder *r, double fraction) {
  AppMode mode;
  gdouble target_frames = 0.0;

  if (fraction < 0.0) {
    fraction = 0.0;
  }
  if (fraction > 1.0) {
    fraction = 1.0;
  }

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  target_frames = recorder_core_seek_fraction(&r->core, fraction, FALSE);
  g_mutex_unlock(&r->mutex);

  g_printerr("[scrub] fraction=%.3f target_frames=%.1f mode=%d\n", fraction, target_frames, mode);
  update_time_label(r);
  if (r->platform.ui && r->platform.ui->queue_waveform_redraw) {
    r->platform.ui->queue_waveform_redraw(r->platform.backend.user_data);
  }
}

static void end_scrub(Recorder *r) {
  gboolean resume = FALSE;
  gboolean was_scrubbing = FALSE;

  g_mutex_lock(&r->mutex);
  was_scrubbing = r->scrubbing;
  if (!was_scrubbing) {
    g_mutex_unlock(&r->mutex);
    return;
  }
  resume = recorder_core_end_scrub(&r->core);
  g_mutex_unlock(&r->mutex);

  g_printerr("[scrub] end resume=%d\n", resume ? 1 : 0);
  if (resume) {
    if (r->platform.audio && r->platform.audio->start_playback) {
      r->platform.audio->start_playback(r->platform.backend.audio_user_data);
    }
  }
}

static double get_playhead_ratio(Recorder *r) {
  double ratio = 0.0;

  g_mutex_lock(&r->mutex);
  ratio = recorder_core_playhead_ratio(&r->core);
  g_mutex_unlock(&r->mutex);

  return ratio;
}

static gdouble loop_min_width_frames(Recorder *r) {
  gdouble min_width = 1.0;

  g_mutex_lock(&r->mutex);
  min_width = recorder_core_loop_min_width_frames(&r->core);
  g_mutex_unlock(&r->mutex);

  return min_width;
}

static LoopSnapshot get_loop_snapshot(Recorder *r) {
  g_mutex_lock(&r->mutex);
  LoopSnapshot snapshot = recorder_core_loop_snapshot(&r->core);
  g_mutex_unlock(&r->mutex);

  return snapshot;
}

static PlatformLoopSnapshot get_platform_loop_snapshot(Recorder *r) {
  LoopSnapshot snapshot = get_loop_snapshot(r);
  PlatformLoopSnapshot platform_snapshot = {0};

  platform_snapshot.enabled = snapshot.enabled;
  platform_snapshot.explicit_region_set = snapshot.explicit_region_set;
  platform_snapshot.effective_region_set = snapshot.effective_region_set;
  platform_snapshot.total_frames = snapshot.total_frames;
  platform_snapshot.start_frames = snapshot.start_frames;
  platform_snapshot.end_frames = snapshot.end_frames;
  return platform_snapshot;
}

static gboolean playhead_tick_cb(GtkWidget *widget, GdkFrameClock *frame_clock, gpointer user_data) {
  (void)widget;
  (void)frame_clock;
  Recorder *r = user_data;
  gboolean scrubbing = FALSE;
  gboolean playing = FALSE;

  g_mutex_lock(&r->mutex);
  scrubbing = r->scrubbing;
  playing = (r->mode == MODE_PLAYING);
  g_mutex_unlock(&r->mutex);

  if (scrubbing) {
    return G_SOURCE_CONTINUE;
  }

  if (playing) {
    update_display_playhead(r);
  }

  update_time_label(r);
  if (r->platform.ui && r->platform.ui->queue_waveform_redraw) {
    r->platform.ui->queue_waveform_redraw(r->platform.backend.user_data);
  }
  return G_SOURCE_CONTINUE;
}

static gboolean refresh_ui_idle_cb(gpointer user_data) {
  refresh_ui((Recorder *)user_data);
  return G_SOURCE_REMOVE;
}

static void update_speed_label(Recorder *r, double speed) {
  char text[32];
  g_snprintf(text, sizeof text, "%.1fx", speed);
  gtk_label_set_text(GTK_LABEL(r->widgets.speed_value_label), text);
}

static gboolean playback_should_stop(Recorder *rec) {
  gboolean stop_requested = FALSE;

  g_mutex_lock(&rec->mutex);
  stop_requested = rec->playback_stop_requested;
  g_mutex_unlock(&rec->mutex);

  return stop_requested;
}

static int render_cancel_requested(void *user_data) {
  return playback_should_stop((Recorder *)user_data) ? 1 : 0;
}

static void invalidate_playback_buffer_locked(Recorder *r) {
  recorder_core_invalidate_playback_buffer(&r->core);
}

static gboolean render_progress_tick_cb(gpointer data) {
  RenderProgressTick *tick = data;
  Recorder *r = tick->rec;
  gdouble progress = 0.0;
  gboolean keep_running = TRUE;
  gboolean valid_generation = FALSE;
  gint64 now_us = 0;
  gint64 elapsed_us = 0;
  gdouble estimated_total_us = 0.0;

  g_mutex_lock(&r->mutex);
  valid_generation = recorder_core_render_is_current(&r->core, tick->generation);
  keep_running = r->render_pending;
  now_us = g_get_monotonic_time();
  elapsed_us = now_us - r->render_started_us;
  estimated_total_us = r->render_estimated_total_us;
  g_mutex_unlock(&r->mutex);

  if (!valid_generation || !keep_running) {
    return FALSE;
  }

  if (estimated_total_us > 0.0 && elapsed_us > 0) {
    progress = (gdouble)elapsed_us / estimated_total_us;
    if (progress > 0.99) {
      progress = 0.99;
    }
    if (progress < 0.0) {
      progress = 0.0;
    }
  }

  if (r->platform.ui && r->platform.ui->set_progress_fraction) {
    r->platform.ui->set_progress_fraction(r->platform.backend.user_data, progress);
  }
  return TRUE;
}

static gboolean render_completion_idle_cb(gpointer data) {
  RenderCompletion *completion = data;
  Recorder *r = completion->rec;
  gboolean valid_generation = FALSE;
  gboolean should_play = FALSE;
  gboolean seek_valid = FALSE;
  gdouble seek_pos = 0.0;

  g_mutex_lock(&r->mutex);
    RecorderCoreRenderCompletion render_completion = recorder_core_complete_render(&r->core,
                                                                                   completion->generation,
                                                                                   completion->outcome);
    valid_generation = render_completion.valid_generation;
    if (valid_generation) {
      r->render_thread = NULL;
      r->render_pulse_source = 0;
      should_play = render_completion.should_play;
      seek_valid = render_completion.seek_valid;
      seek_pos = render_completion.seek_pos;
    }
  g_mutex_unlock(&r->mutex);

  if (!valid_generation) {
    if (completion->playback_pcm) {
      g_byte_array_unref(completion->playback_pcm);
    }
    g_free(completion);
    return FALSE;
  }

  if (r->platform.ui && r->platform.ui->set_progress_visible) {
    r->platform.ui->set_progress_visible(r->platform.backend.user_data, FALSE);
  }

  if (completion->outcome == RENDER_OUTCOME_FAILED) {
    set_error(r, completion->error);
    refresh_ui(r);
  } else if (completion->outcome == RENDER_OUTCOME_CANCELLED) {
    refresh_ui(r);
  } else {
    RecorderCoreRenderCompletion render_completion = {0};
    GByteArray *old_playback_pcm = NULL;
    render_completion.valid_generation = TRUE;
    render_completion.should_play = should_play;
    render_completion.seek_valid = seek_valid;
    render_completion.seek_pos = seek_pos;
    g_mutex_lock(&r->mutex);
    old_playback_pcm = recorder_core_install_rendered_playback(&r->core,
                                                              completion->playback_pcm,
                                                              completion->source_frames,
                                                              completion->rendered_frames,
                                                              completion->speed,
                                                              &render_completion);
    completion->playback_pcm = NULL;
    g_mutex_unlock(&r->mutex);
    if (old_playback_pcm) {
      g_byte_array_unref(old_playback_pcm);
    }
    if (should_play) {
      if (!start_playback_with_ready_buffer(r)) {
        set_error(r, "Failed to start playback after render");
        refresh_ui(r);
      }
    } else {
      refresh_ui(r);
    }
  }

  if (completion->playback_pcm) {
    g_byte_array_unref(completion->playback_pcm);
  }
  g_free(completion);
  return FALSE;
}

static gpointer render_thread_main(gpointer data) {
  Recorder *r = data;
  GByteArray *source_snapshot = NULL;
  GByteArray *probe = NULL;
  GByteArray *combined = NULL;
  guint sample_rate = 44100;
  guint channels = 2;
  guint64 source_frames = 0;
  gdouble speed = 1.0;
  guint my_generation = 0;
  RenderCompletion *completion = NULL;
  PlaybackRenderResult render_result = {0};
  gint64 probe_elapsed_us = 0;
  guint64 probe_frames = 0;
  guint64 probe_start = 0;
  guint64 probe_end = 0;
  gdouble estimated_total_us = 0.0;
  gboolean cancelled = FALSE;
  char render_error[256] = {0};

  g_mutex_lock(&r->mutex);
  my_generation = r->render_generation;
  source_snapshot = g_byte_array_new();
  g_byte_array_append(source_snapshot, r->audio.pcm->data, r->audio.pcm->len);
  sample_rate = r->audio.sample_rate;
  channels = r->audio.channels;
  source_frames = r->audio.captured_frames;
  speed = r->speed;
  g_mutex_unlock(&r->mutex);

  if (source_frames == 0) {
    g_byte_array_unref(source_snapshot);
    completion = g_new0(RenderCompletion, 1);
    completion->rec = r;
    completion->generation = my_generation;
    completion->outcome = RENDER_OUTCOME_FAILED;
    g_strlcpy(completion->error, "No audio to render", sizeof completion->error);
    g_idle_add(render_completion_idle_cb, completion);
    return NULL;
  }

  probe_frames = MIN(source_frames, (guint64)sample_rate * 2);
  if (probe_frames > 0 && source_frames > probe_frames) {
    probe_start = (source_frames - probe_frames) / 2;
    probe_end = probe_start + probe_frames;
    gint64 probe_t0 = g_get_monotonic_time();
    if (playback_renderer_render_s16(source_snapshot->data,
                                     source_snapshot->len,
                                     sample_rate,
                                     channels,
                                     probe_start,
                                     probe_end,
                                     speed,
                                     render_cancel_requested,
                                     r,
                                     NULL,
                                     NULL,
                                     &render_result,
                                     render_error,
                                     sizeof render_error)) {
      probe = g_byte_array_new();
      if (render_result.data && render_result.len > 0) {
        g_byte_array_append(probe, render_result.data, render_result.len);
      }
      playback_renderer_result_clear(&render_result);
    } else {
      cancelled = playback_should_stop(r);
    }
    probe_elapsed_us = g_get_monotonic_time() - probe_t0;
    if (probe) {
      g_byte_array_unref(probe);
      probe = NULL;
    }
    if (cancelled || probe_elapsed_us <= 0) {
      g_byte_array_unref(source_snapshot);
      completion = g_new0(RenderCompletion, 1);
      completion->rec = r;
      completion->generation = my_generation;
      completion->outcome = RENDER_OUTCOME_CANCELLED;
      g_idle_add(render_completion_idle_cb, completion);
      return NULL;
    }
    estimated_total_us = (gdouble)probe_elapsed_us * ((gdouble)source_frames / (gdouble)probe_frames);
  }

  g_mutex_lock(&r->mutex);
  if (!recorder_core_render_is_current(&r->core, my_generation)) {
    g_mutex_unlock(&r->mutex);
    g_byte_array_unref(source_snapshot);
    if (probe) g_byte_array_unref(probe);
    return NULL;
  }
  recorder_core_set_render_estimate(&r->core, my_generation, g_get_monotonic_time(), estimated_total_us);
  g_mutex_unlock(&r->mutex);

  if (playback_renderer_render_s16(source_snapshot->data,
                                   source_snapshot->len,
                                   sample_rate,
                                   channels,
                                   0,
                                   source_frames,
                                   speed,
                                   render_cancel_requested,
                                   r,
                                   NULL,
                                   NULL,
                                   &render_result,
                                   render_error,
                                   sizeof render_error)) {
    combined = g_byte_array_new();
    if (render_result.data && render_result.len > 0) {
      g_byte_array_append(combined, render_result.data, render_result.len);
    }
    playback_renderer_result_clear(&render_result);
  } else {
    cancelled = playback_should_stop(r);
  }
  g_byte_array_unref(source_snapshot);

  if (cancelled || !combined) {
    g_mutex_lock(&r->mutex);
    gboolean still_pending = recorder_core_render_is_current(&r->core, my_generation);
    guint source = r->render_pulse_source;
    if (still_pending) {
      recorder_core_clear_current_render(&r->core, my_generation);
      r->render_thread = NULL;
      r->render_pulse_source = 0;
    }
    g_mutex_unlock(&r->mutex);
    if (source) g_source_remove(source);
    if (combined) g_byte_array_unref(combined);
    completion = g_new0(RenderCompletion, 1);
    completion->rec = r;
    completion->generation = my_generation;
    completion->outcome = RENDER_OUTCOME_CANCELLED;
    g_idle_add(render_completion_idle_cb, completion);
    return NULL;
  }

  guint64 rendered_frames = combined->len / (channels * sizeof(gint16));
  if (rendered_frames == 0) {
    g_byte_array_unref(combined);
    completion = g_new0(RenderCompletion, 1);
    completion->rec = r;
    completion->generation = my_generation;
    completion->outcome = RENDER_OUTCOME_FAILED;
    g_strlcpy(completion->error,
              render_error[0] ? render_error : "Rubber Band produced no playback audio",
              sizeof completion->error);
    g_idle_add(render_completion_idle_cb, completion);
    return NULL;
  }

  completion = g_new0(RenderCompletion, 1);
  completion->rec = r;
  completion->generation = my_generation;
  completion->outcome = RENDER_OUTCOME_SUCCESS;
  completion->playback_pcm = combined;
  completion->source_frames = source_frames;
  completion->rendered_frames = rendered_frames;
  completion->speed = speed;
  combined = NULL;
  g_idle_add(render_completion_idle_cb, completion);

  return NULL;
}

static void start_render_worker(Recorder *r) {
  if (r->platform.ui && r->platform.ui->set_progress_visible) {
    r->platform.ui->set_progress_visible(r->platform.backend.user_data, TRUE);
  }
  if (r->platform.ui && r->platform.ui->set_progress_fraction) {
    r->platform.ui->set_progress_fraction(r->platform.backend.user_data, 0.0);
  }
  clear_error(r);
  g_printerr("[render] starting async render thread\n");
  {
    RenderProgressTick *tick = g_new0(RenderProgressTick, 1);
    tick->rec = r;
    tick->generation = r->render_generation;
    r->render_pulse_source = g_timeout_add_full(G_PRIORITY_DEFAULT, 100, render_progress_tick_cb, tick, g_free);
  }
  r->render_thread = g_thread_new("rubberband-render", render_thread_main, r);

  refresh_ui(r);
}

static gboolean ensure_playback_buffer(Recorder *r) {
  g_mutex_lock(&r->mutex);
  if (recorder_core_playback_buffer_ready(&r->core)) {
    g_mutex_unlock(&r->mutex);
    return TRUE;
  }

  if (r->audio.pcm->len == 0) {
    g_mutex_unlock(&r->mutex);
    set_error(r, "Nothing has been recorded yet");
    return FALSE;
  }

  recorder_core_begin_render(&r->core, g_get_monotonic_time());
  g_mutex_unlock(&r->mutex);

  start_render_worker(r);
  return TRUE;
}

static gboolean start_playback_with_ready_buffer(Recorder *r) {
  g_mutex_lock(&r->mutex);
  if (r->playback_running) {
    g_mutex_unlock(&r->mutex);
    return TRUE;
  }

  if (!recorder_core_begin_ready_playback(&r->core, r->playback_running, g_get_monotonic_time())) {
    g_mutex_unlock(&r->mutex);
    set_error(r, "Playback buffer is not ready");
    return FALSE;
  }

  r->playback_stop_requested = FALSE;
  r->playback_running = TRUE;
  g_mutex_unlock(&r->mutex);

  clear_error(r);

  g_printerr("[ui] starting playback thread\n");
  if (r->platform.audio && r->platform.audio->start_playback) {
    return r->platform.audio->start_playback(r->platform.backend.audio_user_data);
  }

  r->playback_thread = g_thread_new("pulse-playback", playback_thread_main, r);
  return TRUE;
}

typedef struct {
  gboolean ready;
  gboolean done;
  gboolean failed;
  char *default_sink;
  pa_threaded_mainloop *ml;
} PulseQuery;

static void pulse_async_context_state_cb(pa_context *c, void *userdata) {
  PulseQuery *query = userdata;
  pa_context_state_t state = pa_context_get_state(c);

  if (state == PA_CONTEXT_READY) {
    query->ready = TRUE;
  } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
    query->failed = TRUE;
  }

  if (query->ml) {
    pa_threaded_mainloop_signal(query->ml, 0);
  }
}

static void pulse_async_stream_state_cb(pa_stream *s, void *userdata) {
  PulseQuery *query = userdata;
  pa_stream_state_t state = pa_stream_get_state(s);

  if (state == PA_STREAM_READY) {
    query->ready = TRUE;
  } else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
    query->failed = TRUE;
  }

  if (query->ml) {
    pa_threaded_mainloop_signal(query->ml, 0);
  }
}

static void pulse_async_stream_write_cb(pa_stream *s, size_t nbytes, void *userdata) {
  (void)s;
  (void)nbytes;
  PulseQuery *query = userdata;
  if (query->ml) {
    pa_threaded_mainloop_signal(query->ml, 0);
  }
}

static void pulse_async_success_cb(pa_stream *s, int success, void *userdata) {
  (void)s;
  PulseQuery *query = userdata;
  if (!success) {
    query->failed = TRUE;
  }
  query->done = TRUE;
  if (query->ml) {
    pa_threaded_mainloop_signal(query->ml, 0);
  }
}

static void wait_for_pulse_async_done(pa_threaded_mainloop *ml, PulseQuery *query) {
  while (!query->done && !query->failed) {
    pa_threaded_mainloop_wait(ml);
  }
}

static void on_context_state_changed(pa_context *c, void *userdata) {
  PulseQuery *query = userdata;
  pa_context_state_t state = pa_context_get_state(c);
  g_printerr("[pulse] context state: %d\n", (int)state);

  if (state == PA_CONTEXT_READY) {
    query->ready = TRUE;
  } else if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
    query->failed = TRUE;
  }
}

static void on_server_info(pa_context *c, const pa_server_info *i, void *userdata) {
  (void)c;
  PulseQuery *query = userdata;

  if (i && i->default_sink_name) {
    g_printerr("[pulse] default sink: %s\n", i->default_sink_name);
    g_free(query->default_sink);
    query->default_sink = g_strdup(i->default_sink_name);
  } else {
    query->failed = TRUE;
  }
  query->ready = TRUE;
}

static int get_default_sink_name(char **out_sink_name) {
  int ret = -1;
  pa_mainloop *mainloop = NULL;
  pa_mainloop_api *api = NULL;
  pa_context *context = NULL;
  PulseQuery query = {0};

  *out_sink_name = NULL;

  mainloop = pa_mainloop_new();
  if (!mainloop) {
    return -1;
  }

  api = pa_mainloop_get_api(mainloop);
  context = pa_context_new(api, "spotify-recorder");
  if (!context) {
    goto cleanup;
  }

  pa_context_set_state_callback(context, on_context_state_changed, &query);

  if (pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) {
    goto cleanup;
  }

  while (!query.ready && !query.failed) {
    if (pa_mainloop_iterate(mainloop, 1, NULL) < 0) {
      goto cleanup;
    }
  }

  if (query.failed) {
    goto cleanup;
  }

  query.ready = FALSE;
  pa_context_get_server_info(context, on_server_info, &query);

  while (!query.ready && !query.failed) {
    if (pa_mainloop_iterate(mainloop, 1, NULL) < 0) {
      goto cleanup;
    }
  }

  if (query.failed || !query.default_sink) {
    goto cleanup;
  }

  *out_sink_name = query.default_sink;
  query.default_sink = NULL;
  ret = 0;

cleanup:
  g_free(query.default_sink);
  if (context) {
    pa_context_disconnect(context);
    pa_context_unref(context);
  }
  if (mainloop) {
    pa_mainloop_free(mainloop);
  }
  return ret;
}

static gpointer capture_thread_main(gpointer user_data) {
  Recorder *rec = user_data;
  char *sink_name = NULL;
  char *source_name = NULL;
  pa_simple *stream = NULL;
  pa_sample_spec ss;
  int pa_error = 0;
  guint8 *buffer = NULL;
  const size_t buffer_size = 4096 * 4;

  g_printerr("[capture] thread started\n");

  if (get_default_sink_name(&sink_name) < 0 || !sink_name) {
    set_error(rec, "Unable to resolve default sink");
    goto cleanup;
  }

  g_printerr("[capture] sink resolved: %s\n", sink_name);

  source_name = g_strdup_printf("%s.monitor", sink_name);
  g_free(sink_name);
  sink_name = NULL;

  ss.format = PA_SAMPLE_S16LE;
  ss.rate = 44100;
  ss.channels = 2;

  stream = pa_simple_new(
      NULL,
      "spotify-recorder",
      PA_STREAM_RECORD,
      source_name,
      "capture",
      &ss,
      NULL,
      NULL,
      &pa_error);

  if (!stream) {
    char msg[256];
    g_snprintf(msg, sizeof msg, "PulseAudio open failed: %s", pa_strerror(pa_error));
    set_error(rec, msg);
    goto cleanup;
  }

  g_printerr("[capture] stream opened\n");

  buffer = g_malloc(buffer_size);
  while (1) {
    gboolean stop_requested;
    AppMode mode;

    g_mutex_lock(&rec->mutex);
    stop_requested = rec->stop_requested;
    mode = rec->mode;
    g_mutex_unlock(&rec->mutex);

    if (stop_requested) {
      break;
    }

    if (pa_simple_read(stream, buffer, buffer_size, &pa_error) < 0) {
      if (stop_requested) {
        break;
      }
      char msg[256];
      g_snprintf(msg, sizeof msg, "PulseAudio read failed: %s", pa_strerror(pa_error));
      set_error(rec, msg);
      break;
    }

    g_printerr("[capture] chunk read: %zu bytes\n", buffer_size);

    if (mode == MODE_RECORDING) {
      const guint frame_size = rec->audio.channels * sizeof(gint16);
      const guint chunk_frames = 256;
      const guint chunk_bytes = chunk_frames * frame_size;

      g_mutex_lock(&rec->mutex);
      recorder_core_append_pcm(&rec->core, buffer, buffer_size, buffer_size / (rec->audio.channels * sizeof(gint16)));

      for (gsize offset = 0; offset < buffer_size; offset += chunk_bytes) {
        gsize remaining = buffer_size - offset;
        gsize this_bytes = remaining < chunk_bytes ? remaining : chunk_bytes;
        guint16 peak = 0;

        for (gsize i = 0; i + frame_size <= this_bytes; i += frame_size) {
          const gint16 *frame = (const gint16 *)(buffer + offset + i);
          for (guint c = 0; c < rec->audio.channels; c++) {
            gint16 sample = frame[c];
            guint16 abs_sample = (sample < 0) ? (guint16)(-sample) : (guint16)sample;
            if (abs_sample > peak) {
              peak = abs_sample;
            }
          }
        }

        g_array_append_val(rec->audio.wave_peaks, peak);
      }

      g_mutex_unlock(&rec->mutex);
    }
  }

cleanup:
  g_free(buffer);
  g_free(source_name);
  if (stream) {
    pa_simple_free(stream);
  }

  g_mutex_lock(&rec->mutex);
  rec->capture_running = FALSE;
  rec->capture_thread = NULL;
  rec->stop_requested = FALSE;
  if (rec->mode != MODE_PAUSED) {
    rec->mode = MODE_IDLE;
  }
  g_mutex_unlock(&rec->mutex);
  return NULL;
}

static gpointer playback_thread_main(gpointer user_data) {
  Recorder *rec = user_data;
  GByteArray *snapshot = g_byte_array_new();
  guint sample_rate = 44100;
  guint channels = 2;
  gdouble cursor_frames = 0.0;
  gdouble final_cursor_frames = 0.0;
  gdouble rendered_to_source_ratio = 1.0;
  gboolean loop_enabled = FALSE;
  gboolean loop_region_set = FALSE;
  pa_threaded_mainloop *ml = NULL;
  pa_context *context = NULL;
  pa_stream *stream = NULL;
  pa_sample_spec ss;
  pa_buffer_attr attr;
  gboolean reached_end = FALSE;
  gboolean flush_on_exit = FALSE;
  guint64 total_frames = 0;
  guint64 playback_frames = 0;
  CorePlaybackTimeline timeline = {0};
  PulseQuery query = {0};

  g_printerr("[playback] thread started\n");

  g_mutex_lock(&rec->mutex);
  if (rec->audio.playback_pcm && rec->audio.playback_pcm->len > 0) {
    g_byte_array_append(snapshot, rec->audio.playback_pcm->data, rec->audio.playback_pcm->len);
  }
  sample_rate = rec->audio.sample_rate;
  channels = rec->audio.channels;
  cursor_frames = rec->playback_cursor_frames;
  rendered_to_source_ratio = rec->audio.playback_rendered_to_source_ratio > 0.0 ? rec->audio.playback_rendered_to_source_ratio : 1.0;
  total_frames = rec->audio.captured_frames;
  loop_enabled = rec->loop.enabled;
  loop_region_set = rec->loop.region_set;
  g_mutex_unlock(&rec->mutex);

  if (snapshot->len == 0) {
    set_error(rec, "Nothing to play yet");
    goto cleanup;
  }

  playback_frames = snapshot->len / (channels * sizeof(gint16));
  if (cursor_frames < 0.0) {
    cursor_frames = 0.0;
  }
  if (cursor_frames > (gdouble)total_frames) {
    cursor_frames = (gdouble)total_frames;
  }
  core_playback_timeline_init(&timeline, cursor_frames, total_frames, playback_frames, rendered_to_source_ratio, 0);
  final_cursor_frames = timeline.source_cursor_frame;

  g_printerr("[playback] source setup start=%.1f end=%" G_GUINT64_FORMAT " loop=%d region=%d\n",
             timeline.source_cursor_frame, total_frames, loop_enabled ? 1 : 0, loop_region_set ? 1 : 0);

  ss.format = PA_SAMPLE_S16LE;
  ss.rate = sample_rate;
  ss.channels = channels;

  ml = pa_threaded_mainloop_new();
  if (!ml) {
    set_error(rec, "Failed to create PulseAudio mainloop");
    goto cleanup;
  }

  query.ml = ml;
  pa_threaded_mainloop_lock(ml);
  if (pa_threaded_mainloop_start(ml) < 0) {
    set_error(rec, "Failed to start PulseAudio threaded mainloop");
    goto fail_locked;
  }

  g_mutex_lock(&rec->mutex);
  rec->playback_ml = ml;
  g_mutex_unlock(&rec->mutex);

  context = pa_context_new(pa_threaded_mainloop_get_api(ml), "spotify-recorder");
  if (!context) {
    set_error(rec, "Failed to create PulseAudio context");
    goto fail_locked;
  }

  pa_context_set_state_callback(context, pulse_async_context_state_cb, &query);
  if (pa_context_connect(context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) < 0) {
    set_error(rec, "PulseAudio context connect failed");
    goto fail_locked;
  }

  while (!query.ready && !query.failed) {
    pa_threaded_mainloop_wait(ml);
  }
  if (query.failed) {
    set_error(rec, "PulseAudio context failed");
    goto fail_locked;
  }

  stream = pa_stream_new(context, "playback", &ss, NULL);
  if (!stream) {
    set_error(rec, "Failed to create PulseAudio stream");
    goto fail_locked;
  }

  query.ready = FALSE;
  query.failed = FALSE;
  pa_stream_set_state_callback(stream, pulse_async_stream_state_cb, &query);
  pa_stream_set_write_callback(stream, pulse_async_stream_write_cb, &query);

  attr.maxlength = (uint32_t)-1;
  attr.tlength = pa_usec_to_bytes(20000, &ss);
  attr.prebuf = 0;
  attr.minreq = (uint32_t)-1;
  attr.fragsize = (uint32_t)-1;

  if (pa_stream_connect_playback(stream, NULL, &attr,
                                 PA_STREAM_START_CORKED | PA_STREAM_ADJUST_LATENCY,
                                 NULL, NULL) < 0) {
    set_error(rec, "PulseAudio stream connect failed");
    goto fail_locked;
  }

  while (!query.ready && !query.failed) {
    pa_threaded_mainloop_wait(ml);
  }
  if (query.failed) {
    set_error(rec, "PulseAudio stream failed");
    goto fail_locked;
  }

  query.done = FALSE;
  query.failed = FALSE;
  {
    pa_operation *op = pa_stream_cork(stream, 0, pulse_async_success_cb, &query);
    wait_for_pulse_async_done(ml, &query);
    if (op) {
      pa_operation_unref(op);
    }
  }

  if (query.failed) {
    set_error(rec, "PulseAudio stream uncork failed");
    goto fail_locked;
  }

  g_printerr("[playback] opened async stream\n");

  {
    const guint frame_size = channels * sizeof(gint16);
    g_mutex_lock(&rec->mutex);
    rec->playback_cursor_frames = timeline.source_cursor_frame;
    rec->playback_anchor_frames = timeline.source_cursor_frame;
    rec->playback_anchor_us = g_get_monotonic_time();
    rec->display_playhead_frames = timeline.source_cursor_frame;
    rec->mode = MODE_PLAYING;
    g_mutex_unlock(&rec->mutex);
    g_idle_add(refresh_ui_idle_cb, rec);

    while (1) {
      gboolean stop_requested;
      LoopState live_loop = {0};

      g_mutex_lock(&rec->mutex);
      live_loop = rec->loop;
      g_mutex_unlock(&rec->mutex);

      core_playback_timeline_update_loop(&timeline, &live_loop);

      if (core_playback_timeline_reached_end(&timeline)) {
        reached_end = TRUE;
        break;
      }

      stop_requested = playback_should_stop(rec);
      while (!stop_requested && pa_stream_writable_size(stream) == 0) {
        pa_threaded_mainloop_wait(ml);
        stop_requested = playback_should_stop(rec);
      }

      if (stop_requested) {
        flush_on_exit = TRUE;
        break;
      }

      guint writable = pa_stream_writable_size(stream);
      guint out_frames = (guint)core_playback_timeline_limit_write_frames(&timeline, MIN(128u, writable / frame_size));
      if (out_frames == 0) {
        continue;
      }

      if (pa_stream_write(stream,
                          snapshot->data + (timeline.output_cursor_frame * frame_size),
                          out_frames * frame_size,
                          NULL,
                          0,
                          PA_SEEK_RELATIVE) < 0) {
        set_error(rec, "PulseAudio playback write failed");
        break;
      }

      core_playback_timeline_advance(&timeline, out_frames);

      g_mutex_lock(&rec->mutex);
      rec->playback_cursor_frames = timeline.source_cursor_frame;
      final_cursor_frames = rec->playback_cursor_frames;
      rec->playback_anchor_frames = rec->playback_cursor_frames;
      rec->playback_anchor_us = g_get_monotonic_time();
      g_mutex_unlock(&rec->mutex);
    }
  }

  if (flush_on_exit) {
    query.done = FALSE;
    query.failed = FALSE;
    {
      pa_operation *op = pa_stream_cork(stream, 1, pulse_async_success_cb, &query);
      wait_for_pulse_async_done(ml, &query);
      if (op) {
        pa_operation_unref(op);
      }
    }

    if (!query.failed) {
      query.done = FALSE;
      query.failed = FALSE;
      {
        pa_operation *op = pa_stream_flush(stream, pulse_async_success_cb, &query);
        wait_for_pulse_async_done(ml, &query);
        if (op) {
          pa_operation_unref(op);
        }
      }
    }
  }

fail_locked:
  if (stream && pa_stream_get_state(stream) == PA_STREAM_READY) {
    pa_stream_disconnect(stream);
  }
  if (context) {
    pa_context_disconnect(context);
  }
  pa_threaded_mainloop_unlock(ml);

cleanup:
  g_byte_array_unref(snapshot);

  if (ml) {
    pa_threaded_mainloop_stop(ml);
    if (stream) {
      pa_stream_unref(stream);
    }
    if (context) {
      pa_context_unref(context);
    }
    pa_threaded_mainloop_free(ml);
  }

  g_mutex_lock(&rec->mutex);
  rec->playback_ml = NULL;
  rec->playback_cursor_frames = reached_end ? 0.0 : final_cursor_frames;
  rec->playback_anchor_frames = rec->playback_cursor_frames;
  rec->playback_anchor_us = g_get_monotonic_time();
  if (!flush_on_exit) {
    rec->display_playhead_frames = rec->playback_cursor_frames;
  }
  rec->mode = core_playback_final_mode(rec->mode, reached_end);
  rec->playback_running = FALSE;
  rec->playback_thread = NULL;
  rec->playback_stop_requested = FALSE;
  g_mutex_unlock(&rec->mutex);

  g_idle_add(refresh_ui_idle_cb, rec);

  g_printerr("[playback] thread finished\n");
  return NULL;
}

gboolean start_capture_thread(Recorder *r, gboolean reset_buffers) {
  g_mutex_lock(&r->mutex);
  if (r->capture_running) {
    g_mutex_unlock(&r->mutex);
    return TRUE;
  }

  if (reset_buffers) {
    reset_recording_session_locked(r);
  }
  r->stop_requested = FALSE;
  r->capture_running = TRUE;
  r->mode = MODE_RECORDING;
  g_mutex_unlock(&r->mutex);

  clear_error(r);

  g_printerr("[ui] starting capture thread\n");

  r->capture_thread = g_thread_new("pulse-capture", capture_thread_main, r);

  return TRUE;
}

gboolean start_playback_thread(Recorder *r) {
  RecorderCorePlaybackRequest request = {0};

  g_mutex_lock(&r->mutex);
  request = recorder_core_request_playback(&r->core, r->playback_running, g_get_monotonic_time());
  g_mutex_unlock(&r->mutex);

  if (request.already_playing) {
    return TRUE;
  }

  if (!request.has_audio) {
    set_error(r, "Nothing has been recorded yet");
    return FALSE;
  }

  if (request.should_render) {
    start_render_worker(r);
    return TRUE;
  }

  if (request.render_pending && !request.buffer_ready) {
    return TRUE;
  }

  return start_playback_with_ready_buffer(r);
}

void stop_playback_thread(Recorder *r, gboolean reset_cursor) {
  GThread *thread = NULL;
  gdouble preserved_display_frames = 0.0;
  gboolean preserve_display = !reset_cursor;

  g_mutex_lock(&r->mutex);
  if (preserve_display) {
    preserved_display_frames = r->display_playhead_frames;
  }
  if (r->playback_running) {
    r->playback_stop_requested = TRUE;
    if (r->playback_ml) {
      pa_threaded_mainloop_signal(r->playback_ml, 0);
    }
    thread = r->playback_thread;
  }
  g_mutex_unlock(&r->mutex);

  if (thread) {
    g_thread_join(thread);
  }

  g_mutex_lock(&r->mutex);
  if (reset_cursor) {
    core_set_playback_cursor_state(0.0,
                                   &r->playback_cursor_frames,
                                   &r->playback_anchor_frames,
                                   &r->playback_anchor_us,
                                   &r->display_playhead_frames);
  } else {
    core_set_playback_cursor_state(preserved_display_frames,
                                   &r->playback_cursor_frames,
                                   &r->playback_anchor_frames,
                                   &r->playback_anchor_us,
                                   &r->display_playhead_frames);
  }
  r->playback_thread = NULL;
  r->playback_running = FALSE;
  r->playback_stop_requested = FALSE;
  g_mutex_unlock(&r->mutex);
}

void stop_capture_thread(Recorder *r, gboolean force_stopped) {
  GThread *thread = NULL;

  g_mutex_lock(&r->mutex);
  if (r->capture_running) {
    r->stop_requested = TRUE;
    thread = r->capture_thread;
  }
  g_mutex_unlock(&r->mutex);

  if (thread) {
    g_thread_join(thread);
  }

  g_mutex_lock(&r->mutex);
  r->capture_thread = NULL;
  r->capture_running = FALSE;
  r->stop_requested = FALSE;
  r->mode = core_capture_final_mode(r->mode, force_stopped);
  g_mutex_unlock(&r->mutex);
}

static void transport_stop(Recorder *r, const CoreTransportPlan *plan) {
  GThread *render_thread = NULL;

  if (plan->should_cancel_render) {
    g_mutex_lock(&r->mutex);
    render_thread = cancel_render_locked(r, plan->next_mode);
    g_mutex_unlock(&r->mutex);
  }

  if (render_thread) {
    g_thread_unref(render_thread);
    if (r->platform.ui && r->platform.ui->set_progress_visible) {
      r->platform.ui->set_progress_visible(r->platform.backend.user_data, FALSE);
    }
  } else {
    if (plan->should_stop_playback) {
      if (r->platform.audio && r->platform.audio->stop_playback) {
        r->platform.audio->stop_playback(r->platform.backend.audio_user_data, !plan->preserve_cursor);
      }
    }
    if (plan->should_stop_capture) {
      if (r->platform.audio && r->platform.audio->stop_capture) {
        r->platform.audio->stop_capture(r->platform.backend.audio_user_data, FALSE);
      }
    }
    g_mutex_lock(&r->mutex);
    recorder_core_apply_stop_plan(&r->core, plan, g_get_monotonic_time());
    g_mutex_unlock(&r->mutex);
  }
  refresh_ui(r);
}

static void transport_start_recording(Recorder *r, const CoreTransportPlan *plan) {
  GThread *render_thread = NULL;

  if (plan->should_cancel_render) {
    g_mutex_lock(&r->mutex);
    render_thread = cancel_render_locked(r, plan->next_mode);
    g_mutex_unlock(&r->mutex);
  }

  if (render_thread) {
    g_thread_unref(render_thread);
    if (r->platform.ui && r->platform.ui->set_progress_visible) {
      r->platform.ui->set_progress_visible(r->platform.backend.user_data, FALSE);
    }
  }

  if (plan->should_start) {
    g_printerr("[ui] record clicked: starting\n");
    gboolean started = FALSE;
    if (plan->should_stop_playback) {
      if (r->platform.audio && r->platform.audio->stop_playback) {
        r->platform.audio->stop_playback(r->platform.backend.audio_user_data, TRUE);
      }
    }
    started = r->platform.audio && r->platform.audio->start_capture && r->platform.audio->start_capture(r->platform.backend.audio_user_data, plan->reset_buffers);
    if (!started) {
      set_error(r, "Failed to start capture thread");
    }
    g_mutex_lock(&r->mutex);
    recorder_core_apply_record_result(&r->core, plan, started, g_get_monotonic_time());
    g_mutex_unlock(&r->mutex);
  }

  refresh_ui(r);
}

static gboolean transport_play_from_idle(Recorder *r) {
  g_mutex_lock(&r->mutex);
  recorder_core_prepare_play_from_idle(&r->core, g_get_monotonic_time());
  g_mutex_unlock(&r->mutex);

  if (r->platform.audio && r->platform.audio->stop_capture) {
    r->platform.audio->stop_capture(r->platform.backend.audio_user_data, FALSE);
  }

  return r->platform.audio && r->platform.audio->start_playback
    ? r->platform.audio->start_playback(r->platform.backend.audio_user_data)
    : start_playback_thread(r);
}

static void transport_pause(Recorder *r) {
  g_mutex_lock(&r->mutex);
  recorder_core_apply_play_pause_action(&r->core, CORE_PLAY_PAUSE_PAUSE, g_get_monotonic_time());
  g_printerr("[ui] playback paused\n");
  g_mutex_unlock(&r->mutex);

  if (r->platform.audio && r->platform.audio->stop_playback) {
    r->platform.audio->stop_playback(r->platform.backend.audio_user_data, FALSE);
  }
}

static void transport_resume(Recorder *r) {
  g_mutex_lock(&r->mutex);
  recorder_core_apply_play_pause_action(&r->core, CORE_PLAY_PAUSE_RESUME, g_get_monotonic_time());
  g_printerr("[ui] playback resumed\n");
  g_mutex_unlock(&r->mutex);

  if (!r->platform.audio || !r->platform.audio->start_playback || !r->platform.audio->start_playback(r->platform.backend.audio_user_data)) {
    set_error(r, "Failed to resume playback");
  }
}

static void transport_set_speed(Recorder *r, gdouble speed) {
  RecorderCoreSpeedChange change = {0};
  GThread *render_thread = NULL;

  g_mutex_lock(&r->mutex);
  change = recorder_core_apply_speed_change(&r->core, speed);
  g_mutex_unlock(&r->mutex);

  if (!change.changed) {
    return;
  }

  update_speed_label(r, speed);

  if (change.cancel_render) {
    g_mutex_lock(&r->mutex);
    render_thread = cancel_render_locked(r, change.cancel_next_mode);
    g_mutex_unlock(&r->mutex);
    if (render_thread) {
      g_thread_unref(render_thread);
    }
  }

  if (change.start_render) {
    if (!ensure_playback_buffer(r)) {
      set_error(r, change.cancel_render ? "Failed to restart render after speed change" : "Failed to start render after speed change");
    }
    return;
  }

  if (change.restart_playback) {
    if (r->platform.audio && r->platform.audio->stop_playback) {
      r->platform.audio->stop_playback(r->platform.backend.audio_user_data, FALSE);
    }
    if (!r->platform.audio || !r->platform.audio->start_playback || !r->platform.audio->start_playback(r->platform.backend.audio_user_data)) {
      set_error(r, "Failed to restart playback at new speed");
      refresh_ui(r);
    }
  }
}

static void transport_dispatch(Recorder *r, TransportAction action) {
  CoreTransportDecision decision;

  g_mutex_lock(&r->mutex);
  decision = recorder_core_transport_decision(&r->core, r->capture_running, action);
  g_mutex_unlock(&r->mutex);

  switch (action) {
    case TRANSPORT_ACTION_RECORD:
      transport_start_recording(r, &decision.plan);
      return;
    case TRANSPORT_ACTION_STOP:
      transport_stop(r, &decision.plan);
      return;
    case TRANSPORT_ACTION_PLAY_PAUSE:
      switch (decision.play_pause_action) {
        case CORE_PLAY_PAUSE_START_FROM_IDLE:
          if (transport_play_from_idle(r)) {
            refresh_ui(r);
          }
          return;
        case CORE_PLAY_PAUSE_PAUSE:
          transport_pause(r);
          refresh_ui(r);
          return;
        case CORE_PLAY_PAUSE_RESUME:
          transport_resume(r);
          refresh_ui(r);
          return;
        case CORE_PLAY_PAUSE_TOGGLE_RENDER_INTENT:
          g_mutex_lock(&r->mutex);
          recorder_core_toggle_render_intent(&r->core);
          g_mutex_unlock(&r->mutex);
          refresh_ui(r);
          return;
        case CORE_PLAY_PAUSE_IGNORED:
        default:
          return;
      }
      return;
  }
}

static void on_record_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  Recorder *r = user_data;
  transport_dispatch(r, TRANSPORT_ACTION_RECORD);
}

static void on_stop_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  Recorder *r = user_data;
  transport_dispatch(r, TRANSPORT_ACTION_STOP);
}

static void on_play_pause_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  Recorder *r = user_data;
  transport_dispatch(r, TRANSPORT_ACTION_PLAY_PAUSE);
}

static void on_speed_changed(GtkRange *range, gpointer user_data) {
  Recorder *r = user_data;
  gdouble speed = gtk_range_get_value(range);
  transport_set_speed(r, speed);
}

static void on_loop_toggled(GtkToggleButton *button, gpointer user_data) {
  Recorder *r = user_data;
  gboolean active = gtk_toggle_button_get_active(button);

  g_mutex_lock(&r->mutex);
  recorder_core_set_loop_enabled(&r->core, active);
  g_mutex_unlock(&r->mutex);

  refresh_ui(r);
}

static guint16 *copy_wave_peaks(Recorder *r, gsize *peak_count) {
  guint16 *peaks = NULL;

  g_mutex_lock(&r->mutex);
  *peak_count = r->audio.wave_peaks->len;
  if (*peak_count > 0) {
    peaks = g_memdup2(r->audio.wave_peaks->data, *peak_count * sizeof *peaks);
  }
  g_mutex_unlock(&r->mutex);

  return peaks;
}

static gboolean on_waveform_base_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
  Recorder *r = user_data;
  GtkAllocation allocation;
  PlatformLoopSnapshot loop;
  guint16 *peaks = NULL;
  gsize peak_count = 0;
  double width;
  double height;

  gtk_widget_get_allocation(widget, &allocation);
  width = allocation.width;
  height = allocation.height;
  loop = get_platform_loop_snapshot(r);
  peaks = copy_wave_peaks(r, &peak_count);

  if (r->platform.ui && r->platform.ui->draw_waveform) {
    r->platform.ui->draw_waveform(r->platform.backend.user_data,
                                  cr,
                                  width,
                                  height,
                                  peaks,
                                  peak_count,
                                  &loop,
                                  get_playhead_ratio(r));
  }

  g_free(peaks);

  return FALSE;
}

static gboolean on_waveform_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
  Recorder *r = user_data;
  GtkAllocation allocation;
  gdouble total_frames = 0.0;
  gdouble target_frames = 0.0;
  gboolean shift = FALSE;
  gdouble handle_window = 0.0;
  CoreWaveformPressAction press_action = CORE_WAVEFORM_PRESS_IGNORE;

  if (event->button != 1) {
    return FALSE;
  }

  gtk_widget_get_allocation(widget, &allocation);
  if (allocation.width <= 0) {
    return FALSE;
  }

  g_mutex_lock(&r->mutex);
  total_frames = (gdouble)r->audio.captured_frames;
  shift = (event->state & GDK_SHIFT_MASK) != 0;
  g_mutex_unlock(&r->mutex);

  target_frames = core_compute_target_frames(total_frames, event->x / (double)allocation.width);
  handle_window = MAX(loop_min_width_frames(r) * 0.25, total_frames * 10.0 / (double)allocation.width);

  g_mutex_lock(&r->mutex);
  press_action = recorder_core_resolve_waveform_press(&r->core, target_frames, handle_window, shift);
  g_mutex_unlock(&r->mutex);

  if (press_action == CORE_WAVEFORM_PRESS_RENDER_SEEK) {
    g_mutex_lock(&r->mutex);
    recorder_core_seek_fraction(&r->core, event->x / (double)allocation.width, TRUE);
    g_mutex_unlock(&r->mutex);
    update_time_label(r);
    if (r->platform.ui && r->platform.ui->queue_waveform_redraw) {
      r->platform.ui->queue_waveform_redraw(r->platform.backend.user_data);
    }
    refresh_ui(r);
    return TRUE;
  }

  if (press_action == CORE_WAVEFORM_PRESS_LOOP_CREATE) {
    g_mutex_lock(&r->mutex);
    recorder_core_begin_loop_drag(&r->core, press_action, target_frames);
    g_mutex_unlock(&r->mutex);
    if (!r->platform.ui || !r->platform.ui->grab_pointer || !r->platform.ui->grab_pointer(r->platform.backend.user_data, widget, event)) {
      g_printerr("[loop] pointer grab failed\n");
    }
    if (r->platform.ui && r->platform.ui->queue_waveform_redraw) {
      r->platform.ui->queue_waveform_redraw(r->platform.backend.user_data);
    }
    return TRUE;
  }

  if (press_action == CORE_WAVEFORM_PRESS_LOOP_START) {
    g_mutex_lock(&r->mutex);
    recorder_core_begin_loop_drag(&r->core, press_action, target_frames);
    g_mutex_unlock(&r->mutex);
    if (!r->platform.ui || !r->platform.ui->grab_pointer || !r->platform.ui->grab_pointer(r->platform.backend.user_data, widget, event)) {
      g_printerr("[loop] pointer grab failed\n");
    }
    return TRUE;
  }

  if (press_action == CORE_WAVEFORM_PRESS_LOOP_END) {
    g_mutex_lock(&r->mutex);
    recorder_core_begin_loop_drag(&r->core, press_action, target_frames);
    g_mutex_unlock(&r->mutex);
    if (!r->platform.ui || !r->platform.ui->grab_pointer || !r->platform.ui->grab_pointer(r->platform.backend.user_data, widget, event)) {
      g_printerr("[loop] pointer grab failed\n");
    }
    return TRUE;
  }

  if (press_action == CORE_WAVEFORM_PRESS_SCRUB) {
    begin_scrub(r);
    if (!r->platform.ui || !r->platform.ui->grab_pointer || !r->platform.ui->grab_pointer(r->platform.backend.user_data, widget, event)) {
      g_printerr("[scrub] pointer grab failed\n");
    }
    if (total_frames > 0.0) {
      seek_to_fraction(r, target_frames / total_frames);
    }
    return TRUE;
  }

  return FALSE;
}

static gboolean on_waveform_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
  Recorder *r = user_data;
  LoopDragMode drag_mode = LOOP_DRAG_NONE;
  gdouble total_frames = 0.0;
  gdouble current_frames = 0.0;

  if (event->button != 1) {
    return FALSE;
  }

  if (r->platform.ui && r->platform.ui->release_pointer) {
    r->platform.ui->release_pointer(r->platform.backend.user_data, widget, event);
  }

  g_mutex_lock(&r->mutex);
  drag_mode = r->loop.drag_mode;
  total_frames = (gdouble)r->audio.captured_frames;
  g_mutex_unlock(&r->mutex);

  if (drag_mode != LOOP_DRAG_NONE) {
    if (gtk_widget_get_allocated_width(widget) > 0) {
      current_frames = (event->x / (double)gtk_widget_get_allocated_width(widget)) * total_frames;
    }

    g_mutex_lock(&r->mutex);
    recorder_core_update_loop_drag(&r->core, current_frames);
    recorder_core_clear_loop_drag(&r->core);
    g_mutex_unlock(&r->mutex);
    refresh_ui(r);
    if (r->platform.ui && r->platform.ui->queue_waveform_redraw) {
      r->platform.ui->queue_waveform_redraw(r->platform.backend.user_data);
    }

    return TRUE;
  }

  end_scrub(r);
  return TRUE;
}

static gboolean on_waveform_motion(GtkWidget *widget, GdkEventMotion *event, gpointer user_data) {
  Recorder *r = user_data;
  GtkAllocation allocation;
  double x = event->x;
  double fraction = 0.0;
  gboolean scrubbing = FALSE;
  LoopDragMode drag_mode = LOOP_DRAG_NONE;
  gdouble total_frames = 0.0;
  gdouble current_frames = 0.0;

  g_mutex_lock(&r->mutex);
  scrubbing = r->scrubbing;
  drag_mode = r->loop.drag_mode;
  total_frames = (gdouble)r->audio.captured_frames;
  g_mutex_unlock(&r->mutex);

  if (drag_mode != LOOP_DRAG_NONE) {
    gtk_widget_get_allocation(widget, &allocation);
    if (allocation.width <= 0) {
      return FALSE;
    }

    fraction = x / (double)allocation.width;
    current_frames = fraction * total_frames;

    g_mutex_lock(&r->mutex);
    recorder_core_update_loop_drag(&r->core, current_frames);
    g_mutex_unlock(&r->mutex);

    if (r->platform.ui && r->platform.ui->queue_waveform_redraw) {
      r->platform.ui->queue_waveform_redraw(r->platform.backend.user_data);
    }
    return TRUE;
  }

  if (!scrubbing) {
    return FALSE;
  }

  gtk_widget_get_allocation(widget, &allocation);
  if (allocation.width <= 0) {
    return FALSE;
  }

  fraction = x / (double)allocation.width;
  update_scrub(r, fraction);
  return TRUE;
}

static void on_window_destroy(GtkWidget *widget, gpointer user_data) {
  (void)widget;
  Recorder *r = user_data;

  if (r->tick_callback_id) {
    gtk_widget_remove_tick_callback(r->widgets.waveform_base, r->tick_callback_id);
  }

  if (r->platform.audio && r->platform.audio->stop_playback) {
    r->platform.audio->stop_playback(r->platform.backend.audio_user_data, TRUE);
  }
  if (r->platform.audio && r->platform.audio->stop_capture) {
    r->platform.audio->stop_capture(r->platform.backend.audio_user_data, TRUE);
  }

  g_mutex_clear(&r->mutex);
  if (r->audio.pcm) {
    g_byte_array_unref(r->audio.pcm);
  }
  if (r->audio.playback_pcm) {
    g_byte_array_unref(r->audio.playback_pcm);
  }
  if (r->audio.wave_peaks) {
    g_array_unref(r->audio.wave_peaks);
  }
  g_free(r);
}

static void activate(GtkApplication *app, gpointer user_data) {
  (void)user_data;

  Recorder *r = g_new0(Recorder, 1);
  GtkWidget *window = gtk_application_window_new(app);
  GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
  GtkWidget *title = gtk_label_new("Spotify Audio Recorder");
  GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *record_button = gtk_button_new_with_label("Record");
  GtkWidget *stop_button = gtk_button_new_with_label("Stop");
  GtkWidget *play_pause_button = gtk_button_new_with_label("Play");
  GtkWidget *loop_button = gtk_toggle_button_new_with_label("Loop");
  GtkWidget *speed_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  GtkWidget *speed_label = gtk_label_new("Playback speed");
  GtkWidget *speed_value = gtk_label_new("1.0x");
  GtkWidget *speed_scale = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.5, 2.0, 0.1);
  GtkWidget *waveform_base = gtk_drawing_area_new();
  GtkWidget *time_label = gtk_label_new("0.0 / 0.0s");
  GtkWidget *status = gtk_label_new("Idle | 0.0s captured");
  GtkWidget *progress_bar = gtk_progress_bar_new();
  gtk_progress_bar_pulse(GTK_PROGRESS_BAR(progress_bar));
  gtk_widget_set_no_show_all(progress_bar, TRUE);
  gtk_widget_hide(progress_bar);

  r->widgets.status_label = status;
  r->widgets.progress_bar = progress_bar;
  r->widgets.speed_value_label = speed_value;
  r->widgets.waveform_base = waveform_base;
  r->widgets.time_label = time_label;
  r->widgets.record_button = record_button;
  r->widgets.play_pause_button = play_pause_button;
  r->widgets.loop_button = loop_button;
  r->widgets.stop_button = stop_button;
  r->linux_ui.status_label = status;
  r->linux_ui.speed_value_label = speed_value;
  r->linux_ui.time_label = time_label;
  r->linux_ui.waveform_base = waveform_base;
  r->linux_ui.record_button = record_button;
  r->linux_ui.play_pause_button = play_pause_button;
  r->linux_ui.loop_button = loop_button;
  r->linux_ui.stop_button = stop_button;
  r->linux_ui.progress_bar = progress_bar;
  r->platform = platform_linux_build(&r->linux_ui);
  r->platform.backend.audio_user_data = r;
  r->platform.audio = linux_audio_backend_vtable();
  recorder_core_init(&r->core, g_get_monotonic_time());
  r->audio.pcm = g_byte_array_new();
  r->audio.playback_pcm = NULL;
  r->audio.wave_peaks = g_array_new(FALSE, FALSE, sizeof(guint16));
  g_mutex_init(&r->mutex);
  r->render_thread = NULL;
  r->render_pulse_source = 0;
  r->loop_toggled_handler_id = 0;

  gtk_window_set_default_size(GTK_WINDOW(window), 960, 640);
  gtk_window_set_title(GTK_WINDOW(window), "Spotify Audio Recorder");
  gtk_container_set_border_width(GTK_CONTAINER(window), 16);

  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(title, 4);

  gtk_box_pack_start(GTK_BOX(controls), record_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(controls), stop_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(controls), play_pause_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(controls), loop_button, FALSE, FALSE, 0);
  gtk_widget_set_size_request(progress_bar, 120, -1);
  gtk_box_pack_start(GTK_BOX(controls), progress_bar, FALSE, FALSE, 8);

  gtk_scale_set_draw_value(GTK_SCALE(speed_scale), FALSE);
  gtk_range_set_value(GTK_RANGE(speed_scale), 1.0);
  gtk_widget_set_hexpand(speed_scale, TRUE);

  gtk_box_pack_start(GTK_BOX(speed_row), speed_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(speed_row), speed_scale, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(speed_row), speed_value, FALSE, FALSE, 0);

  gtk_widget_set_vexpand(waveform_base, TRUE);
  gtk_widget_set_hexpand(waveform_base, TRUE);
  gtk_widget_set_size_request(waveform_base, -1, 320);
  gtk_widget_add_events(waveform_base, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_BUTTON_MOTION_MASK | GDK_POINTER_MOTION_MASK);

  gtk_box_pack_start(GTK_BOX(root), title, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), controls, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), speed_row, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), waveform_base, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(root), time_label, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), status, FALSE, FALSE, 0);

  gtk_container_add(GTK_CONTAINER(window), root);

  g_signal_connect(record_button, "clicked", G_CALLBACK(on_record_clicked), r);
  g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_clicked), r);
  g_signal_connect(play_pause_button, "clicked", G_CALLBACK(on_play_pause_clicked), r);
  r->loop_toggled_handler_id = g_signal_connect(loop_button, "toggled", G_CALLBACK(on_loop_toggled), r);
  g_signal_connect(speed_scale, "value-changed", G_CALLBACK(on_speed_changed), r);
  g_signal_connect(waveform_base, "draw", G_CALLBACK(on_waveform_base_draw), r);
  g_signal_connect(waveform_base, "button-press-event", G_CALLBACK(on_waveform_button_press), r);
  g_signal_connect(waveform_base, "button-release-event", G_CALLBACK(on_waveform_button_release), r);
  g_signal_connect(waveform_base, "motion-notify-event", G_CALLBACK(on_waveform_motion), r);
  g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), r);

  r->tick_callback_id = gtk_widget_add_tick_callback(waveform_base, playhead_tick_cb, r, NULL);
  refresh_ui(r);

  gtk_widget_show_all(window);
}

int main(int argc, char **argv) {
  GtkApplication *app = gtk_application_new("com.tyler.spotifyrecorder", G_APPLICATION_FLAGS_NONE);
  int status;

  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  status = g_application_run(G_APPLICATION(app), argc, argv);
  g_object_unref(app);

  return status;
}
