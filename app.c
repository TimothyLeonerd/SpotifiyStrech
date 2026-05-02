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

#include "third_party/rubberband/rubberband/rubberband-c.h"

typedef enum {
  MODE_IDLE = 0,
  MODE_RECORDING,
  MODE_PREPARING,
  MODE_PLAYING,
  MODE_PAUSED,
} AppMode;

typedef enum {
  LOOP_DRAG_NONE = 0,
  LOOP_DRAG_CREATE,
  LOOP_DRAG_START,
  LOOP_DRAG_END,
  LOOP_DRAG_MOVE,
} LoopDragMode;

typedef struct {
  gboolean enabled;
  gboolean explicit_region_set;
  gboolean effective_region_set;
  gdouble total_frames;
  gdouble start_frames;
  gdouble end_frames;
} LoopSnapshot;

typedef struct {
  GtkWidget *status_label;
  GtkWidget *speed_value_label;
  GtkWidget *time_label;
  GtkWidget *waveform_base;
  GtkWidget *record_button;
  GtkWidget *play_pause_button;
  GtkWidget *loop_button;
  GtkWidget *stop_button;

  GMutex mutex;
  GByteArray *pcm;
  GArray *wave_peaks;
  guint sample_rate;
  guint channels;
  guint64 captured_frames;
  gdouble speed;
  gdouble playback_cursor_frames;
  gdouble playback_anchor_frames;
  gint64 playback_anchor_us;
  gdouble display_playhead_frames;
  gboolean loop_enabled;
  gboolean loop_region_set;
  gdouble loop_start_frames;
  gdouble loop_end_frames;
  LoopDragMode loop_drag_mode;
  gdouble loop_drag_anchor_frames;
  gdouble loop_drag_offset_frames;
  gboolean scrubbing;
  gboolean resume_after_scrub;

  AppMode mode;
  gboolean stop_requested;
  gboolean capture_running;
  GThread *capture_thread;
  gboolean playback_running;
  gboolean playback_stop_requested;
  GThread *playback_thread;
  pa_threaded_mainloop *playback_ml;
  guint tick_callback_id;
  gint last_playhead_x;
  char last_error[256];
} Recorder;

static const char *mode_to_text(AppMode mode) {
  switch (mode) {
    case MODE_RECORDING: return "Recording";
    case MODE_PREPARING: return "Preparing";
    case MODE_PLAYING: return "Playing";
    case MODE_PAUSED: return "Paused";
    case MODE_IDLE:
    default: return "Idle";
  }
}

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
  gboolean record_enabled = FALSE;
  gboolean play_pause_enabled = FALSE;
  gboolean loop_enabled = FALSE;
  gboolean stop_enabled = FALSE;
  const char *play_pause_label = "Play";

  g_mutex_lock(&r->mutex);
  switch (r->mode) {
    case MODE_IDLE:
      record_enabled = TRUE;
      play_pause_enabled = TRUE;
      loop_enabled = TRUE;
      play_pause_label = "Play";
      break;
    case MODE_RECORDING:
      stop_enabled = TRUE;
      play_pause_label = "Play";
      break;
    case MODE_PREPARING:
      stop_enabled = TRUE;
      play_pause_label = "Play";
      break;
    case MODE_PLAYING:
      play_pause_enabled = TRUE;
      loop_enabled = TRUE;
      stop_enabled = TRUE;
      play_pause_label = "Pause";
      break;
    case MODE_PAUSED:
      play_pause_enabled = TRUE;
      loop_enabled = TRUE;
      stop_enabled = TRUE;
      play_pause_label = "Play";
      break;
  }
  g_mutex_unlock(&r->mutex);

  gtk_widget_set_sensitive(r->record_button, record_enabled);
  gtk_widget_set_sensitive(r->play_pause_button, play_pause_enabled);
  gtk_widget_set_sensitive(r->loop_button, loop_enabled);
  gtk_widget_set_sensitive(r->stop_button, stop_enabled);
  gtk_button_set_label(GTK_BUTTON(r->play_pause_button), play_pause_label);
}

static void set_mode(Recorder *r, AppMode mode) {
  g_mutex_lock(&r->mutex);
  r->mode = mode;
  g_mutex_unlock(&r->mutex);
}

static gdouble get_current_playback_frames(Recorder *r);
static double get_playhead_ratio(Recorder *r);
static gboolean start_playback_thread(Recorder *r);
static void stop_playback_thread(Recorder *r, gboolean reset_cursor);

static void update_display_playhead(Recorder *r) {
  AppMode mode;
  gboolean scrubbing = FALSE;
  gdouble current_frames = 0.0;
  gdouble display_frames = 0.0;

  current_frames = get_current_playback_frames(r);

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  scrubbing = r->scrubbing;
  display_frames = r->display_playhead_frames;
  if (scrubbing) {
    display_frames = current_frames;
  } else if (mode == MODE_PLAYING) {
    gdouble delta = current_frames - display_frames;
    if (delta < 0.0) {
      delta = 0.0;
    }
    display_frames += delta * 0.35;
    if (current_frames - display_frames < 0.5) {
      display_frames = current_frames;
    }
  } else {
    display_frames = current_frames;
  }
  r->display_playhead_frames = display_frames;
  g_mutex_unlock(&r->mutex);
}

static void update_time_label(Recorder *r) {
  guint rate = 1;
  gdouble cursor_frames = 0.0;
  gdouble total_frames = 0.0;
  char *time_text = NULL;

  g_mutex_lock(&r->mutex);
  rate = r->sample_rate ? r->sample_rate : 1;
  cursor_frames = r->display_playhead_frames;
  total_frames = (gdouble)r->captured_frames;
  g_mutex_unlock(&r->mutex);

  time_text = g_strdup_printf("%.1f / %.1fs", cursor_frames / (double)rate, total_frames / (double)rate);
  gtk_label_set_text(GTK_LABEL(r->time_label), time_text);
  g_free(time_text);
}

static gdouble get_current_playback_frames(Recorder *r) {
  AppMode mode;
  gboolean scrubbing = FALSE;
  gdouble cursor_frames = 0.0;
  gdouble anchor_frames = 0.0;
  gint64 anchor_us = 0;
  guint rate = 44100;
  gdouble speed = 1.0;

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  scrubbing = r->scrubbing;
  cursor_frames = r->playback_cursor_frames;
  anchor_frames = r->playback_anchor_frames;
  anchor_us = r->playback_anchor_us;
  rate = r->sample_rate ? r->sample_rate : 1;
  speed = r->speed;
  g_mutex_unlock(&r->mutex);

  if (scrubbing) {
    return cursor_frames;
  }

  if (mode == MODE_PLAYING) {
    gdouble elapsed_sec = (g_get_monotonic_time() - anchor_us) / 1000000.0;
    gdouble estimated = anchor_frames + elapsed_sec * (gdouble)rate * speed;
    if (estimated < cursor_frames) {
      return cursor_frames;
    }
    return estimated;
  }

  return cursor_frames;
}

static void refresh_ui(Recorder *r) {
  AppMode mode;
  guint64 frames;
  guint rate;
  gboolean loop_enabled = FALSE;
  gboolean loop_region_set = FALSE;
  char error[256];
  double seconds;

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  frames = r->captured_frames;
  rate = r->sample_rate ? r->sample_rate : 1;
  loop_enabled = r->loop_enabled;
  loop_region_set = r->loop_region_set;
  g_strlcpy(error, r->last_error, sizeof error);
  g_mutex_unlock(&r->mutex);

  seconds = (double)frames / (double)rate;
  if (error[0] != '\0') {
    char text[512];
    g_snprintf(text, sizeof text, "%s | %.1fs captured | %s | Loop %s%s", mode_to_text(mode), seconds, error, loop_enabled ? "on" : "off", loop_region_set ? " (set)" : "");
    gtk_label_set_text(GTK_LABEL(r->status_label), text);
  } else {
    char text[256];
    g_snprintf(text, sizeof text, "%s | %.1fs captured | Loop %s%s", mode_to_text(mode), seconds, loop_enabled ? "on" : "off", loop_region_set ? " (set)" : "");
    gtk_label_set_text(GTK_LABEL(r->status_label), text);
  }

  {
    update_time_label(r);
  }

  update_button_sensitivity(r);
  gtk_widget_queue_draw(r->waveform_base);

  {
    gint width = gtk_widget_get_allocated_width(r->waveform_base);
    gint height = gtk_widget_get_allocated_height(r->waveform_base);
    gint new_x = (gint)lrint(get_playhead_ratio(r) * (double)width);
    gint old_x = r->last_playhead_x;
    gint x1 = MIN(old_x, new_x) - 6;
    gint x2 = MAX(old_x, new_x) + 6;

    if (width > 0 && height > 0) {
      if (x1 < 0) x1 = 0;
      if (x2 > width) x2 = width;
      if (x2 > x1) {
        gtk_widget_queue_draw_area(r->waveform_base, x1, 0, x2 - x1, height);
      }
    }

    r->last_playhead_x = new_x;
  }
}

static void set_playback_cursor_locked(Recorder *r, gdouble frames) {
  r->playback_cursor_frames = frames;
  r->playback_anchor_frames = frames;
  r->playback_anchor_us = g_get_monotonic_time();
  r->display_playhead_frames = frames;
}

static void seek_to_fraction(Recorder *r, double fraction) {
  AppMode mode;
  gboolean scrubbing = FALSE;
  gdouble total_frames = 0.0;
  gdouble target_frames = 0.0;

  if (fraction < 0.0) {
    fraction = 0.0;
  }
  if (fraction > 1.0) {
    fraction = 1.0;
  }

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  scrubbing = r->scrubbing;
  total_frames = (gdouble)r->captured_frames;
  target_frames = total_frames * fraction;
  set_playback_cursor_locked(r, target_frames);
  g_mutex_unlock(&r->mutex);

  g_printerr("[seek] fraction=%.3f target_frames=%.1f mode=%d\n", fraction, target_frames, mode);

  if (mode == MODE_PLAYING && !scrubbing) {
    stop_playback_thread(r, FALSE);
    if (!start_playback_thread(r)) {
      return;
    }
  } else {
    refresh_ui(r);
  }
}

static gboolean grab_scrub_pointer(GtkWidget *widget, GdkEventButton *event) {
  GdkWindow *window = gtk_widget_get_window(widget);
  GdkDevice *device = gdk_event_get_device((GdkEvent *)event);
  GdkSeat *seat;

  if (!window || !device) {
    return FALSE;
  }

  seat = gdk_device_get_seat(device);
  if (!seat) {
    return FALSE;
  }

  return gdk_seat_grab(seat,
                       window,
                       GDK_SEAT_CAPABILITY_POINTER,
                       FALSE,
                       NULL,
                       (GdkEvent *)event,
                       NULL,
                       NULL) == GDK_GRAB_SUCCESS;
}

static void release_scrub_pointer(GtkWidget *widget, GdkEventButton *event) {
  GdkWindow *window = gtk_widget_get_window(widget);
  GdkDevice *device = gdk_event_get_device((GdkEvent *)event);
  GdkSeat *seat;

  (void)window;

  if (!device) {
    return;
  }

  seat = gdk_device_get_seat(device);
  if (!seat) {
    return;
  }

  gdk_seat_ungrab(seat);
}

static void begin_scrub(Recorder *r) {
  gboolean was_playing = FALSE;

  g_mutex_lock(&r->mutex);
  if (r->scrubbing) {
    g_mutex_unlock(&r->mutex);
    return;
  }
  r->scrubbing = TRUE;
  was_playing = (r->mode == MODE_PLAYING);
  r->resume_after_scrub = was_playing;
  g_mutex_unlock(&r->mutex);

  g_printerr("[scrub] begin resume=%d\n", was_playing ? 1 : 0);
  if (was_playing) {
    stop_playback_thread(r, FALSE);
  }
}

static void update_scrub(Recorder *r, double fraction) {
  AppMode mode;
  gdouble total_frames = 0.0;
  gdouble target_frames = 0.0;

  if (fraction < 0.0) {
    fraction = 0.0;
  }
  if (fraction > 1.0) {
    fraction = 1.0;
  }

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  total_frames = (gdouble)r->captured_frames;
  target_frames = total_frames * fraction;
  set_playback_cursor_locked(r, target_frames);
  g_mutex_unlock(&r->mutex);

  g_printerr("[scrub] fraction=%.3f target_frames=%.1f mode=%d\n", fraction, target_frames, mode);
  update_time_label(r);
  gtk_widget_queue_draw(r->waveform_base);
}

static void end_scrub(Recorder *r) {
  gboolean resume = FALSE;

  g_mutex_lock(&r->mutex);
  if (!r->scrubbing) {
    g_mutex_unlock(&r->mutex);
    return;
  }
  r->scrubbing = FALSE;
  resume = r->resume_after_scrub;
  r->resume_after_scrub = FALSE;
  g_mutex_unlock(&r->mutex);

  g_printerr("[scrub] end resume=%d\n", resume ? 1 : 0);
  if (resume) {
    start_playback_thread(r);
  }
}

static double get_playhead_ratio(Recorder *r) {
  double cursor_frames = 0.0;
  double total_frames = 0.0;

  g_mutex_lock(&r->mutex);
  cursor_frames = r->display_playhead_frames;
  total_frames = (double)r->captured_frames;
  g_mutex_unlock(&r->mutex);

  if (total_frames <= 0.0) {
    return 0.0;
  }

  if (cursor_frames < 0.0) {
    cursor_frames = 0.0;
  }
  if (cursor_frames > total_frames) {
    cursor_frames = total_frames;
  }

  return cursor_frames / total_frames;
}

static void set_loop_region(Recorder *r, gdouble start_frames, gdouble end_frames) {
  gdouble total_frames = 0.0;

  if (start_frames > end_frames) {
    gdouble tmp = start_frames;
    start_frames = end_frames;
    end_frames = tmp;
  }

  g_mutex_lock(&r->mutex);
  total_frames = (gdouble)r->captured_frames;
  if (start_frames < 0.0) {
    start_frames = 0.0;
  }
  if (end_frames < 0.0) {
    end_frames = 0.0;
  }
  if (start_frames > total_frames) {
    start_frames = total_frames;
  }
  if (end_frames > total_frames) {
    end_frames = total_frames;
  }
  r->loop_start_frames = start_frames;
  r->loop_end_frames = end_frames;
  r->loop_region_set = TRUE;
  g_mutex_unlock(&r->mutex);
}

static gdouble loop_min_width_frames(Recorder *r) {
  gdouble rate = 44100.0;

  g_mutex_lock(&r->mutex);
  rate = r->sample_rate ? (gdouble)r->sample_rate : 44100.0;
  g_mutex_unlock(&r->mutex);

  return MAX(0.25 * rate, 1.0);
}

static gdouble clamp_loop_frame(Recorder *r, gdouble frame) {
  gdouble total_frames = 0.0;

  g_mutex_lock(&r->mutex);
  total_frames = (gdouble)r->captured_frames;
  g_mutex_unlock(&r->mutex);

  if (frame < 0.0) {
    frame = 0.0;
  }
  if (frame > total_frames) {
    frame = total_frames;
  }
  return frame;
}

static gboolean get_effective_loop_region_locked(Recorder *r, gdouble total_frames, gdouble *start_frames, gdouble *end_frames) {
  gdouble start = r->loop_region_set ? r->loop_start_frames : 0.0;
  gdouble end = r->loop_region_set ? r->loop_end_frames : total_frames;

  if (total_frames <= 0.0) {
    return FALSE;
  }

  if (start < 0.0) {
    start = 0.0;
  }
  if (end < 0.0) {
    end = 0.0;
  }
  if (start > total_frames) {
    start = total_frames;
  }
  if (end > total_frames) {
    end = total_frames;
  }
  if (start > end) {
    gdouble tmp = start;
    start = end;
    end = tmp;
  }

  if (start_frames) {
    *start_frames = start;
  }
  if (end_frames) {
    *end_frames = end;
  }

  return end > start;
}

static LoopSnapshot get_loop_snapshot(Recorder *r) {
  LoopSnapshot snapshot = {0};

  g_mutex_lock(&r->mutex);
  snapshot.enabled = r->loop_enabled;
  snapshot.explicit_region_set = r->loop_region_set;
  snapshot.total_frames = (gdouble)r->captured_frames;
  snapshot.effective_region_set = get_effective_loop_region_locked(r,
                                                                    snapshot.total_frames,
                                                                    &snapshot.start_frames,
                                                                    &snapshot.end_frames);
  g_mutex_unlock(&r->mutex);

  return snapshot;
}

static void finalize_loop_region(Recorder *r, gdouble start_frames, gdouble end_frames) {
  gdouble min_width = loop_min_width_frames(r);
  gdouble total_frames = 0.0;

  if (start_frames > end_frames) {
    gdouble tmp = start_frames;
    start_frames = end_frames;
    end_frames = tmp;
  }

  start_frames = clamp_loop_frame(r, start_frames);
  end_frames = clamp_loop_frame(r, end_frames);

  g_mutex_lock(&r->mutex);
  total_frames = (gdouble)r->captured_frames;
  g_mutex_unlock(&r->mutex);

  if (end_frames - start_frames < min_width) {
    gdouble center = (start_frames + end_frames) * 0.5;
    start_frames = center - (min_width * 0.5);
    end_frames = center + (min_width * 0.5);
    if (start_frames < 0.0) {
      end_frames -= start_frames;
      start_frames = 0.0;
    }
    if (end_frames > total_frames) {
      gdouble delta = end_frames - total_frames;
      end_frames = total_frames;
      start_frames -= delta;
      if (start_frames < 0.0) {
        start_frames = 0.0;
      }
    }
  }

  set_loop_region(r, start_frames, end_frames);
}

static void set_loop_drag(Recorder *r, LoopDragMode mode, gdouble anchor_frames, gdouble offset_frames) {
  g_mutex_lock(&r->mutex);
  r->loop_drag_mode = mode;
  r->loop_drag_anchor_frames = anchor_frames;
  r->loop_drag_offset_frames = offset_frames;
  g_mutex_unlock(&r->mutex);
}

static void clear_loop_drag(Recorder *r) {
  g_mutex_lock(&r->mutex);
  r->loop_drag_mode = LOOP_DRAG_NONE;
  r->loop_drag_anchor_frames = 0.0;
  r->loop_drag_offset_frames = 0.0;
  g_mutex_unlock(&r->mutex);
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
  gtk_widget_queue_draw(r->waveform_base);
  return G_SOURCE_CONTINUE;
}

static gboolean refresh_ui_idle_cb(gpointer user_data) {
  refresh_ui((Recorder *)user_data);
  return G_SOURCE_REMOVE;
}

static void update_speed_label(Recorder *r, double speed) {
  char text[32];
  g_snprintf(text, sizeof text, "%.1fx", speed);
  gtk_label_set_text(GTK_LABEL(r->speed_value_label), text);
}

static inline gint16 clamp_i16(gint32 value) {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return (gint16)value;
}

static gboolean playback_should_stop(Recorder *rec) {
  gboolean stop_requested = FALSE;

  g_mutex_lock(&rec->mutex);
  stop_requested = rec->playback_stop_requested;
  g_mutex_unlock(&rec->mutex);

  return stop_requested;
}

static void interleave_float_planes_to_s16(GByteArray *out, const float *const *planes, guint frames, guint channels) {
  gsize start = out->len;
  gint16 *dst;

  g_byte_array_set_size(out, start + (gsize)frames * channels * sizeof(gint16));
  dst = (gint16 *)(out->data + start);

  for (guint i = 0; i < frames; ++i) {
    for (guint c = 0; c < channels; ++c) {
      float sample = planes[c][i];
      if (sample > 1.0f) {
        sample = 1.0f;
      } else if (sample < -1.0f) {
        sample = -1.0f;
      }
      dst[(gsize)i * channels + c] = clamp_i16((gint32)lrintf(sample * 32767.0f));
    }
  }
}

static gboolean fill_float_planes_from_s16(const guint8 *src, guint64 start_frame, guint frames, guint channels, float **planes) {
  const gint16 *input = (const gint16 *)src;

  for (guint i = 0; i < frames; ++i) {
    const gint16 *frame = input + (((gsize)start_frame + i) * channels);
    for (guint c = 0; c < channels; ++c) {
      planes[c][i] = (float)frame[c] / 32768.0f;
    }
  }

  return TRUE;
}

static GByteArray *render_stretched_pcm(Recorder *rec,
                                        const GByteArray *snapshot,
                                        guint sample_rate,
                                        guint channels,
                                        guint64 start_frame,
                                        guint64 end_frame,
                                        double speed,
                                        gboolean *cancelled) {
  const guint64 input_frames = (start_frame < end_frame) ? (end_frame - start_frame) : 0;
  const guint chunk_frames = 1024;
  const RubberBandOptions options = RubberBandOptionProcessOffline |
                                    RubberBandOptionEngineFiner |
                                    RubberBandOptionChannelsTogether;
  RubberBandState state = NULL;
  GByteArray *output = NULL;
  float **input_planes = NULL;
  float *input_storage = NULL;
  float **output_planes = NULL;
  float *output_storage = NULL;
  guint output_capacity = 0;
  gboolean cancelled_local = FALSE;

  if (cancelled) {
    *cancelled = FALSE;
  }

  if (input_frames == 0) {
    return g_byte_array_new();
  }

  state = rubberband_new(sample_rate, channels, options, (speed > 0.0) ? (1.0 / speed) : 1.0, 1.0);
  if (!state) {
    set_error(rec, "Failed to create Rubber Band stretcher");
    return NULL;
  }

  rubberband_set_expected_input_duration(state, (unsigned int)MIN(input_frames, (guint64)G_MAXUINT));

  output = g_byte_array_new();
  input_storage = g_new(float, chunk_frames * channels);
  input_planes = g_new0(float *, channels);
  output_planes = g_new0(float *, channels);

  for (guint c = 0; c < channels; ++c) {
    input_planes[c] = input_storage + (c * chunk_frames);
  }

  for (guint pass = 0; pass < 2; ++pass) {
    guint64 offset = 0;

    while (offset < input_frames) {
      guint frames = (guint)MIN((guint64)chunk_frames, input_frames - offset);
      gboolean final = (offset + frames) >= input_frames;

      if (playback_should_stop(rec)) {
        cancelled_local = TRUE;
        goto cleanup;
      }

      fill_float_planes_from_s16(snapshot->data, start_frame + offset, frames, channels, input_planes);

      if (pass == 0) {
        rubberband_study(state, (const float *const *)input_planes, frames, final);
      } else {
        rubberband_process(state, (const float *const *)input_planes, frames, final);

        while (rubberband_available(state) > 0) {
          guint avail = (guint)rubberband_available(state);

          if (avail > output_capacity) {
            g_free(output_storage);
            output_storage = g_new(float, (gsize)avail * channels);
            for (guint c = 0; c < channels; ++c) {
              output_planes[c] = output_storage + (c * avail);
            }
            output_capacity = avail;
          }

          rubberband_retrieve(state, (float *const *)output_planes, avail);
          interleave_float_planes_to_s16(output, (const float *const *)output_planes, avail, channels);
        }
      }

      offset += frames;
    }

    if (pass == 0) {
      rubberband_calculate_stretch(state);
    }
  }

cleanup:
  rubberband_delete(state);
  g_free(input_storage);
  g_free(input_planes);
  g_free(output_storage);
  g_free(output_planes);

  if (cancelled) {
    *cancelled = cancelled_local;
  }

  if (cancelled_local) {
    g_byte_array_unref(output);
    return NULL;
  }

  return output;
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
      const guint frame_size = rec->channels * sizeof(gint16);
      const guint chunk_frames = 256;
      const guint chunk_bytes = chunk_frames * frame_size;

      g_mutex_lock(&rec->mutex);
      g_byte_array_append(rec->pcm, buffer, buffer_size);
      rec->captured_frames += buffer_size / (rec->channels * sizeof(gint16));

      for (gsize offset = 0; offset < buffer_size; offset += chunk_bytes) {
        gsize remaining = buffer_size - offset;
        gsize this_bytes = remaining < chunk_bytes ? remaining : chunk_bytes;
        guint16 peak = 0;

        for (gsize i = 0; i + frame_size <= this_bytes; i += frame_size) {
          const gint16 *frame = (const gint16 *)(buffer + offset + i);
          for (guint c = 0; c < rec->channels; c++) {
            gint16 sample = frame[c];
            guint16 abs_sample = (sample < 0) ? (guint16)(-sample) : (guint16)sample;
            if (abs_sample > peak) {
              peak = abs_sample;
            }
          }
        }

        g_array_append_val(rec->wave_peaks, peak);
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
  GByteArray *rendered = NULL;
  guint sample_rate = 44100;
  guint channels = 2;
  gdouble speed = 1.0;
  gdouble cursor_frames = 0.0;
  gdouble source_cursor_frame = 0.0;
  gdouble final_cursor_frames = 0.0;
  gboolean loop_enabled = FALSE;
  gboolean loop_region_set = FALSE;
  pa_threaded_mainloop *ml = NULL;
  pa_context *context = NULL;
  pa_stream *stream = NULL;
  pa_sample_spec ss;
  pa_buffer_attr attr;
  gboolean reached_end = FALSE;
  gboolean flush_on_exit = FALSE;
  gboolean cancelled = FALSE;
  guint64 total_frames = 0;
  PulseQuery query = {0};

  g_printerr("[playback] thread started\n");

  g_mutex_lock(&rec->mutex);
  if (rec->pcm->len > 0) {
    g_byte_array_append(snapshot, rec->pcm->data, rec->pcm->len);
  }
  sample_rate = rec->sample_rate;
  channels = rec->channels;
  speed = rec->speed;
  cursor_frames = rec->playback_cursor_frames;
  loop_enabled = rec->loop_enabled;
  loop_region_set = rec->loop_region_set;
  g_mutex_unlock(&rec->mutex);

  if (snapshot->len == 0) {
    set_error(rec, "Nothing to play yet");
    goto cleanup;
  }

  {
    total_frames = snapshot->len / (channels * sizeof(gint16));
    if (cursor_frames < 0.0) {
      cursor_frames = 0.0;
    }
    if (cursor_frames > (gdouble)total_frames) {
      cursor_frames = (gdouble)total_frames;
    }
    source_cursor_frame = cursor_frames;
    final_cursor_frames = source_cursor_frame;

    g_printerr("[playback] source setup start=%.1f end=%" G_GUINT64_FORMAT " loop=%d region=%d\n",
               source_cursor_frame, total_frames, loop_enabled ? 1 : 0, loop_region_set ? 1 : 0);
  }

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

  g_printerr("[playback] opened async stream at %.1fx\n", speed);

  {
    const guint frame_size = channels * sizeof(gint16);
    const guint8 *rendered_data = NULL;
    guint64 segment_start_frame = 0;
    guint64 segment_end_frame = 0;
    guint64 segment_rendered_frames = 0;
    guint64 output_frames_played = 0;
    gdouble segment_progress_ratio = 1.0;
    gboolean loop_armed = FALSE;

    g_mutex_lock(&rec->mutex);
    rec->playback_cursor_frames = source_cursor_frame;
    rec->playback_anchor_frames = source_cursor_frame;
    rec->playback_anchor_us = g_get_monotonic_time();
    rec->display_playhead_frames = source_cursor_frame;
    rec->mode = MODE_PLAYING;
    g_mutex_unlock(&rec->mutex);
    g_idle_add(refresh_ui_idle_cb, rec);

    while (1) {
      gboolean stop_requested;
      gboolean live_loop_enabled = FALSE;
      gboolean live_loop_region_set = FALSE;
      gdouble live_loop_start = 0.0;
      gdouble live_loop_end = 0.0;
      guint64 live_loop_start_frame = 0;
      guint64 live_loop_end_frame = 0;
      gboolean live_loop_valid = FALSE;
      guint64 desired_start_frame = 0;
      guint64 desired_end_frame = total_frames;

      g_mutex_lock(&rec->mutex);
      live_loop_enabled = rec->loop_enabled;
      live_loop_region_set = get_effective_loop_region_locked(rec, (gdouble)total_frames, &live_loop_start, &live_loop_end);
      g_mutex_unlock(&rec->mutex);

      live_loop_start_frame = (guint64)live_loop_start;
      live_loop_end_frame = (guint64)live_loop_end;

      live_loop_valid = live_loop_enabled && live_loop_region_set && live_loop_end_frame > live_loop_start_frame;

      if (live_loop_valid && loop_armed && source_cursor_frame >= (gdouble)live_loop_end_frame) {
        source_cursor_frame = (gdouble)live_loop_start_frame;
        final_cursor_frames = source_cursor_frame;
        rendered_data = NULL;
        if (rendered) {
          g_byte_array_unref(rendered);
          rendered = NULL;
        }
        g_mutex_lock(&rec->mutex);
        rec->playback_cursor_frames = source_cursor_frame;
        rec->playback_anchor_frames = source_cursor_frame;
        rec->playback_anchor_us = g_get_monotonic_time();
        rec->display_playhead_frames = source_cursor_frame;
        g_mutex_unlock(&rec->mutex);
        g_idle_add(refresh_ui_idle_cb, rec);
        continue;
      }

      if (!live_loop_valid) {
        loop_armed = FALSE;
      }

      if (source_cursor_frame >= (gdouble)total_frames) {
        reached_end = TRUE;
        break;
      }

      desired_start_frame = (guint64)source_cursor_frame;
      desired_end_frame = total_frames;
      if (live_loop_valid && source_cursor_frame < (gdouble)live_loop_end_frame) {
        loop_armed = TRUE;
        desired_end_frame = live_loop_end_frame;
      }

      if (desired_start_frame >= desired_end_frame) {
        source_cursor_frame = (gdouble)desired_end_frame;
        continue;
      }

      if (!rendered ||
          output_frames_played >= segment_rendered_frames ||
          source_cursor_frame < (gdouble)segment_start_frame ||
          source_cursor_frame > (gdouble)segment_end_frame) {
        if (rendered) {
          g_byte_array_unref(rendered);
          rendered = NULL;
        }

        rendered = render_stretched_pcm(rec, snapshot, sample_rate, channels, desired_start_frame, desired_end_frame, speed, &cancelled);
        if (cancelled) {
          goto fail_locked;
        }
        if (!rendered) {
          goto fail_locked;
        }

        segment_rendered_frames = rendered->len / frame_size;
        if (segment_rendered_frames == 0) {
          set_error(rec, "Rubber Band produced no playback audio");
          goto fail_locked;
        }

        segment_start_frame = desired_start_frame;
        segment_end_frame = desired_end_frame;
        segment_progress_ratio = (gdouble)(segment_end_frame - segment_start_frame) / (gdouble)segment_rendered_frames;
        output_frames_played = 0;
        rendered_data = rendered->data;
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
      guint out_frames = MIN(128u, writable / frame_size);
      if (out_frames == 0) {
        continue;
      }

      if (output_frames_played + out_frames > segment_rendered_frames) {
        out_frames = (guint)(segment_rendered_frames - output_frames_played);
      }

      if (out_frames == 0) {
        break;
      }

      if (pa_stream_write(stream,
                          rendered_data + (output_frames_played * frame_size),
                          out_frames * frame_size,
                          NULL,
                          0,
                          PA_SEEK_RELATIVE) < 0) {
        set_error(rec, "PulseAudio playback write failed");
        break;
      }

      output_frames_played += out_frames;
      source_cursor_frame = (gdouble)segment_start_frame + ((gdouble)output_frames_played * segment_progress_ratio);
      if (source_cursor_frame > (gdouble)segment_end_frame) {
        source_cursor_frame = (gdouble)segment_end_frame;
      }

      g_mutex_lock(&rec->mutex);
      rec->playback_cursor_frames = source_cursor_frame;
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
  if (rendered) {
    g_byte_array_unref(rendered);
  }

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
  if (reached_end) {
    rec->mode = MODE_IDLE;
  } else if (rec->mode == MODE_PREPARING) {
    rec->mode = MODE_IDLE;
  }
  rec->playback_running = FALSE;
  rec->playback_thread = NULL;
  rec->playback_stop_requested = FALSE;
  g_mutex_unlock(&rec->mutex);

  g_idle_add(refresh_ui_idle_cb, rec);

  g_printerr("[playback] thread finished\n");
  return NULL;
}

static gboolean start_capture_thread(Recorder *r, gboolean reset_buffers) {
  g_mutex_lock(&r->mutex);
  if (r->capture_running) {
    g_mutex_unlock(&r->mutex);
    return TRUE;
  }

  if (reset_buffers) {
    g_byte_array_set_size(r->pcm, 0);
    g_array_set_size(r->wave_peaks, 0);
    r->captured_frames = 0;
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

static gboolean start_playback_thread(Recorder *r) {
  g_mutex_lock(&r->mutex);
  if (r->playback_running) {
    g_mutex_unlock(&r->mutex);
    return TRUE;
  }

  if (r->pcm->len == 0) {
    g_mutex_unlock(&r->mutex);
    set_error(r, "Nothing has been recorded yet");
    return FALSE;
  }

  r->playback_stop_requested = FALSE;
  r->playback_running = TRUE;
  r->mode = MODE_PREPARING;
  r->playback_anchor_frames = r->playback_cursor_frames;
  r->playback_anchor_us = g_get_monotonic_time();
  r->display_playhead_frames = r->playback_cursor_frames;
  g_mutex_unlock(&r->mutex);

  clear_error(r);

  g_printerr("[ui] starting playback thread\n");
  r->playback_thread = g_thread_new("pulse-playback", playback_thread_main, r);
  return TRUE;
}

static void stop_playback_thread(Recorder *r, gboolean reset_cursor) {
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
    set_playback_cursor_locked(r, 0.0);
  } else {
    set_playback_cursor_locked(r, preserved_display_frames);
  }
  r->playback_thread = NULL;
  r->playback_running = FALSE;
  r->playback_stop_requested = FALSE;
  g_mutex_unlock(&r->mutex);
}

static void stop_capture_thread(Recorder *r, gboolean force_stopped) {
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
  if (force_stopped || r->mode != MODE_PAUSED) {
    r->mode = MODE_IDLE;
  }
  g_mutex_unlock(&r->mutex);
}

static void transport_stop(Recorder *r) {
  g_printerr("[ui] stop clicked\n");
  stop_playback_thread(r, TRUE);
  stop_capture_thread(r, TRUE);
  set_mode(r, MODE_IDLE);
  refresh_ui(r);
}

static void transport_start_recording(Recorder *r) {
  gboolean should_start = FALSE;
  gboolean reset_buffers = FALSE;

  g_mutex_lock(&r->mutex);
  if (r->mode == MODE_IDLE && !r->capture_running) {
    should_start = TRUE;
    reset_buffers = TRUE;
  }
  g_mutex_unlock(&r->mutex);

  if (should_start) {
    g_printerr("[ui] record clicked: starting\n");
    stop_playback_thread(r, TRUE);
    if (!start_capture_thread(r, reset_buffers)) {
      set_error(r, "Failed to start capture thread");
    }
  }

  refresh_ui(r);
}

static gboolean transport_play_from_idle(Recorder *r) {
  stop_capture_thread(r, TRUE);
  return start_playback_thread(r);
}

static void transport_pause(Recorder *r) {
  gdouble paused_frames = 0.0;

  g_mutex_lock(&r->mutex);
  paused_frames = r->display_playhead_frames;
  r->mode = MODE_PAUSED;
  set_playback_cursor_locked(r, paused_frames);
  g_printerr("[ui] playback paused\n");
  g_mutex_unlock(&r->mutex);

  stop_playback_thread(r, FALSE);
}

static void transport_resume(Recorder *r) {
  gdouble resumed_frames = 0.0;

  g_mutex_lock(&r->mutex);
  resumed_frames = r->display_playhead_frames;
  set_playback_cursor_locked(r, resumed_frames);
  g_printerr("[ui] playback resumed\n");
  g_mutex_unlock(&r->mutex);

  if (!start_playback_thread(r)) {
    set_error(r, "Failed to resume playback");
  }
}

static void transport_play_pause(Recorder *r) {
  AppMode mode;

  g_mutex_lock(&r->mutex);
  mode = r->mode;
  g_mutex_unlock(&r->mutex);

  switch (mode) {
    case MODE_IDLE:
      if (!transport_play_from_idle(r)) {
        return;
      }
      break;
    case MODE_PLAYING:
      transport_pause(r);
      break;
    case MODE_PAUSED:
      transport_resume(r);
      break;
    case MODE_RECORDING:
    case MODE_PREPARING:
    default:
      return;
  }

  refresh_ui(r);
}

static void transport_set_speed(Recorder *r, gdouble speed) {
  gboolean restart_playback = FALSE;

  g_mutex_lock(&r->mutex);
  r->speed = speed;
  restart_playback = (r->mode == MODE_PLAYING);
  g_mutex_unlock(&r->mutex);

  update_speed_label(r, speed);

  if (restart_playback) {
    stop_playback_thread(r, FALSE);
    if (!start_playback_thread(r)) {
      set_error(r, "Failed to restart playback at new speed");
      refresh_ui(r);
    }
  }
}

static void on_record_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  Recorder *r = user_data;
  transport_start_recording(r);
}

static void on_stop_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  Recorder *r = user_data;
  transport_stop(r);
}

static void on_play_pause_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  Recorder *r = user_data;
  transport_play_pause(r);
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
  r->loop_enabled = active;
  g_mutex_unlock(&r->mutex);

  refresh_ui(r);
}

static guint16 *copy_wave_peaks(Recorder *r, gsize *peak_count) {
  guint16 *peaks = NULL;

  g_mutex_lock(&r->mutex);
  *peak_count = r->wave_peaks->len;
  if (*peak_count > 0) {
    peaks = g_memdup2(r->wave_peaks->data, *peak_count * sizeof *peaks);
  }
  g_mutex_unlock(&r->mutex);

  return peaks;
}

static void draw_waveform_background(cairo_t *cr, double width, double height) {
  const double mid_y = height * 0.5;

  cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
  cairo_paint(cr);

  cairo_set_source_rgb(cr, 0.18, 0.18, 0.22);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, 0, mid_y);
  cairo_line_to(cr, width, mid_y);
  cairo_stroke(cr);
}

static void draw_waveform_empty(cairo_t *cr, double width, double height) {
  cairo_text_extents_t extents;
  const char *text = "Waveform appears as you record";

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.65);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 18.0);
  cairo_text_extents(cr, text, &extents);
  cairo_move_to(cr, (width - extents.width) * 0.5 - extents.x_bearing, (height - extents.height) * 0.5 - extents.y_bearing);
  cairo_show_text(cr, text);
}

static void draw_waveform_peaks(cairo_t *cr, const guint16 *peaks, gsize peak_count, double width, double height) {
  const double mid_y = height * 0.5;

  cairo_set_source_rgb(cr, 0.35, 0.75, 0.50);
  cairo_set_line_width(cr, 2.0);

  for (int x = 0; x < (int)width; x++) {
    gsize peak_idx = (gsize)x * peak_count / (gsize)width;
    if (peak_idx >= peak_count) {
      peak_idx = peak_count - 1;
    }

    double amp = (double)peaks[peak_idx] / 32768.0;
    double top = mid_y - (amp * (height * 0.42));
    double bottom = mid_y + (amp * (height * 0.42));
    cairo_move_to(cr, x + 0.5, top);
    cairo_line_to(cr, x + 0.5, bottom);
  }

  cairo_stroke(cr);
}

static void draw_loop_region(cairo_t *cr, const LoopSnapshot *loop, double width, double height) {
  const gboolean active = loop->enabled || loop->explicit_region_set;
  const double alpha = active ? 0.22 : 0.08;
  const double color = active ? 0.45 : 0.55;
  double start_x;
  double end_x;

  if (!loop->effective_region_set || loop->total_frames <= 0.0) {
    return;
  }

  start_x = (loop->start_frames / loop->total_frames) * width;
  end_x = (loop->end_frames / loop->total_frames) * width;

  cairo_set_source_rgba(cr, color, color, color, alpha);
  cairo_rectangle(cr, start_x, 0, MAX(end_x - start_x, 0.0), height);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, color, color, color, active ? 0.55 : 0.20);
  cairo_set_line_width(cr, 3.0);
  cairo_move_to(cr, start_x + 0.5, 0);
  cairo_line_to(cr, start_x + 0.5, height);
  cairo_move_to(cr, end_x + 0.5, 0);
  cairo_line_to(cr, end_x + 0.5, height);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, color, color, color, active ? 0.9 : 0.35);
  cairo_move_to(cr, start_x - 7.0, 2.0);
  cairo_line_to(cr, start_x + 7.0, 2.0);
  cairo_line_to(cr, start_x, 13.0);
  cairo_close_path(cr);
  cairo_fill(cr);

  cairo_move_to(cr, end_x - 7.0, 2.0);
  cairo_line_to(cr, end_x + 7.0, 2.0);
  cairo_line_to(cr, end_x, 13.0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

static void draw_playhead(cairo_t *cr, double playhead_ratio, double width, double height) {
  double playhead_x = playhead_ratio * width;

  cairo_set_source_rgba(cr, 1.0, 0.55, 0.0, 0.16);
  cairo_rectangle(cr, 0, 0, playhead_x, height);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 1.0, 0.55, 0.0);
  cairo_set_line_width(cr, 3.0);
  cairo_move_to(cr, playhead_x + 0.5, 0);
  cairo_line_to(cr, playhead_x + 0.5, height);
  cairo_stroke(cr);
}

static gboolean on_waveform_base_draw(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
  Recorder *r = user_data;
  GtkAllocation allocation;
  LoopSnapshot loop;
  guint16 *peaks = NULL;
  gsize peak_count = 0;
  double width;
  double height;

  gtk_widget_get_allocation(widget, &allocation);
  width = allocation.width;
  height = allocation.height;
  loop = get_loop_snapshot(r);
  peaks = copy_wave_peaks(r, &peak_count);

  draw_waveform_background(cr, width, height);

  if (peak_count == 0 || width <= 1.0) {
    draw_waveform_empty(cr, width, height);
    g_free(peaks);
    return FALSE;
  }

  draw_waveform_peaks(cr, peaks, peak_count, width, height);
  g_free(peaks);

  draw_loop_region(cr, &loop, width, height);
  draw_playhead(cr, get_playhead_ratio(r), width, height);

  return FALSE;
}

static gboolean on_waveform_button_press(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
  Recorder *r = user_data;
  GtkAllocation allocation;
  gdouble total_frames = 0.0;
  gdouble target_frames = 0.0;
  gdouble loop_start = 0.0;
  gdouble loop_end = 0.0;
  gboolean shift = FALSE;
  gboolean effective_region_set = FALSE;
  gboolean near_start = FALSE;
  gboolean near_end = FALSE;
  gdouble handle_window = 0.0;

  if (event->button != 1) {
    return FALSE;
  }

  gtk_widget_get_allocation(widget, &allocation);
  if (allocation.width <= 0) {
    return FALSE;
  }

  g_mutex_lock(&r->mutex);
  total_frames = (gdouble)r->captured_frames;
  shift = (event->state & GDK_SHIFT_MASK) != 0;
  effective_region_set = get_effective_loop_region_locked(r, total_frames, &loop_start, &loop_end);
  g_mutex_unlock(&r->mutex);

  target_frames = (event->x / (double)allocation.width) * total_frames;
  handle_window = MAX(loop_min_width_frames(r) * 0.25, total_frames * 10.0 / (double)allocation.width);

  if (shift && total_frames > 0.0) {
    set_loop_drag(r, LOOP_DRAG_CREATE, target_frames, 0.0);
    if (!grab_scrub_pointer(widget, event)) {
      g_printerr("[loop] pointer grab failed\n");
    }
    gtk_widget_queue_draw(r->waveform_base);
    return TRUE;
  }

  if (effective_region_set) {
    near_start = fabs(target_frames - loop_start) <= handle_window;
    near_end = fabs(target_frames - loop_end) <= handle_window;

    if (near_start) {
      set_loop_drag(r, LOOP_DRAG_START, 0.0, 0.0);
      if (!grab_scrub_pointer(widget, event)) {
        g_printerr("[loop] pointer grab failed\n");
      }
      return TRUE;
    }

    if (near_end) {
      set_loop_drag(r, LOOP_DRAG_END, 0.0, 0.0);
      if (!grab_scrub_pointer(widget, event)) {
        g_printerr("[loop] pointer grab failed\n");
      }
      return TRUE;
    }
  }

  begin_scrub(r);
  if (!grab_scrub_pointer(widget, event)) {
    g_printerr("[scrub] pointer grab failed\n");
  }
  if (total_frames > 0.0) {
    seek_to_fraction(r, target_frames / total_frames);
  }
  return TRUE;
}

static gboolean on_waveform_button_release(GtkWidget *widget, GdkEventButton *event, gpointer user_data) {
  Recorder *r = user_data;
  LoopDragMode drag_mode = LOOP_DRAG_NONE;
  gdouble drag_anchor = 0.0;
  gdouble drag_offset = 0.0;
  gdouble loop_start = 0.0;
  gdouble loop_end = 0.0;
  gdouble total_frames = 0.0;
  gdouble current_frames = 0.0;

  if (event->button != 1) {
    return FALSE;
  }

  release_scrub_pointer(widget, event);

  g_mutex_lock(&r->mutex);
  drag_mode = r->loop_drag_mode;
  drag_anchor = r->loop_drag_anchor_frames;
  drag_offset = r->loop_drag_offset_frames;
  total_frames = (gdouble)r->captured_frames;
  get_effective_loop_region_locked(r, total_frames, &loop_start, &loop_end);
  g_mutex_unlock(&r->mutex);

  if (drag_mode != LOOP_DRAG_NONE) {
    if (gtk_widget_get_allocated_width(widget) > 0) {
      current_frames = (event->x / (double)gtk_widget_get_allocated_width(widget)) * total_frames;
    }

    switch (drag_mode) {
      case LOOP_DRAG_CREATE:
        finalize_loop_region(r, drag_anchor, current_frames);
        break;
      case LOOP_DRAG_START:
        finalize_loop_region(r, current_frames, loop_end);
        break;
      case LOOP_DRAG_END:
        finalize_loop_region(r, loop_start, current_frames);
        break;
      case LOOP_DRAG_MOVE: {
        gdouble width = loop_end - loop_start;
        gdouble new_start = current_frames - drag_offset;
        finalize_loop_region(r, new_start, new_start + width);
        break;
      }
      case LOOP_DRAG_NONE:
        break;
    }

    clear_loop_drag(r);
    refresh_ui(r);
    gtk_widget_queue_draw(r->waveform_base);

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
  gdouble drag_anchor = 0.0;
  gdouble drag_offset = 0.0;
  gdouble loop_start = 0.0;
  gdouble loop_end = 0.0;
  gdouble total_frames = 0.0;
  gdouble current_frames = 0.0;

  g_mutex_lock(&r->mutex);
  scrubbing = r->scrubbing;
  drag_mode = r->loop_drag_mode;
  drag_anchor = r->loop_drag_anchor_frames;
  drag_offset = r->loop_drag_offset_frames;
  total_frames = (gdouble)r->captured_frames;
  get_effective_loop_region_locked(r, total_frames, &loop_start, &loop_end);
  g_mutex_unlock(&r->mutex);

  if (drag_mode != LOOP_DRAG_NONE) {
    gtk_widget_get_allocation(widget, &allocation);
    if (allocation.width <= 0) {
      return FALSE;
    }

    fraction = x / (double)allocation.width;
    current_frames = fraction * total_frames;

    switch (drag_mode) {
      case LOOP_DRAG_CREATE:
        finalize_loop_region(r, drag_anchor, current_frames);
        break;
      case LOOP_DRAG_START:
        finalize_loop_region(r, current_frames, loop_end);
        break;
      case LOOP_DRAG_END:
        finalize_loop_region(r, loop_start, current_frames);
        break;
      case LOOP_DRAG_MOVE: {
        gdouble width = loop_end - loop_start;
        gdouble new_start = current_frames - drag_offset;
        finalize_loop_region(r, new_start, new_start + width);
        break;
      }
      case LOOP_DRAG_NONE:
        break;
    }

    gtk_widget_queue_draw(r->waveform_base);
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
    gtk_widget_remove_tick_callback(r->waveform_base, r->tick_callback_id);
  }

  stop_playback_thread(r, TRUE);
  stop_capture_thread(r, TRUE);

  g_mutex_clear(&r->mutex);
  if (r->pcm) {
    g_byte_array_unref(r->pcm);
  }
  if (r->wave_peaks) {
    g_array_unref(r->wave_peaks);
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
  GtkWidget *status = gtk_label_new("Stopped | 0.0s captured");

  r->status_label = status;
  r->speed_value_label = speed_value;
  r->waveform_base = waveform_base;
  r->time_label = time_label;
  r->record_button = record_button;
  r->play_pause_button = play_pause_button;
  r->loop_button = loop_button;
  r->stop_button = stop_button;
  r->sample_rate = 44100;
  r->channels = 2;
  r->pcm = g_byte_array_new();
  r->wave_peaks = g_array_new(FALSE, FALSE, sizeof(guint16));
  r->speed = 1.0;
  r->playback_cursor_frames = 0.0;
  r->playback_anchor_frames = 0.0;
  r->playback_anchor_us = g_get_monotonic_time();
  r->display_playhead_frames = 0.0;
  g_mutex_init(&r->mutex);

  gtk_window_set_default_size(GTK_WINDOW(window), 960, 640);
  gtk_window_set_title(GTK_WINDOW(window), "Spotify Audio Recorder");
  gtk_container_set_border_width(GTK_CONTAINER(window), 16);

  gtk_widget_set_halign(title, GTK_ALIGN_START);
  gtk_widget_set_margin_bottom(title, 4);

  gtk_box_pack_start(GTK_BOX(controls), record_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(controls), stop_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(controls), play_pause_button, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(controls), loop_button, FALSE, FALSE, 0);

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
  g_signal_connect(loop_button, "toggled", G_CALLBACK(on_loop_toggled), r);
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
