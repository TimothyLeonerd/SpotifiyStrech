#include "core.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

#ifndef CORE_HAS_GLIB
#ifdef _WIN32
#include <windows.h>
#endif

static int core_snprintf(char *buffer, size_t size, const char *format, ...) {
  int written;
  va_list args;

  va_start(args, format);
#ifdef _MSC_VER
  written = _vsnprintf_s(buffer, size, _TRUNCATE, format, args);
#else
  written = vsnprintf(buffer, size, format, args);
#endif
  va_end(args);
  return written;
}

static void core_byte_array_set_size(GByteArray *array, size_t size) {
  (void)array;
  (void)size;
}

static void core_array_set_size(GArray *array, size_t size) {
  (void)array;
  (void)size;
}

static gint64 core_get_monotonic_time_us(void) {
#ifdef _WIN32
  return (gint64)GetTickCount64() * 1000;
#else
  return 0;
#endif
}
#else
static int core_snprintf(char *buffer, size_t size, const char *format, ...) {
  int written;
  va_list args;

  va_start(args, format);
  written = g_vsnprintf(buffer, size, format, args);
  va_end(args);
  return written;
}

static void core_byte_array_set_size(GByteArray *array, size_t size) {
  g_byte_array_set_size(array, size);
}

static void core_array_set_size(GArray *array, size_t size) {
  g_array_set_size(array, size);
}

static gint64 core_get_monotonic_time_us(void) {
  return g_get_monotonic_time();
}
#endif

const char *core_mode_to_text(AppMode mode) {
  switch (mode) {
    case MODE_RECORDING: return "Recording";
    case MODE_PREPARING: return "Preparing";
    case MODE_PLAYING: return "Playing";
    case MODE_PAUSED: return "Paused";
    case MODE_RENDERING: return "Rendering";
    case MODE_IDLE:
    default: return "Idle";
  }
}

gboolean core_mode_allows_record(AppMode mode) {
  return mode == MODE_IDLE;
}

gboolean core_mode_allows_play_pause(AppMode mode) {
  return mode == MODE_IDLE || mode == MODE_PLAYING || mode == MODE_PAUSED || mode == MODE_RENDERING;
}

gboolean core_mode_allows_stop(AppMode mode) {
  return mode == MODE_RECORDING || mode == MODE_PREPARING || mode == MODE_PLAYING || mode == MODE_PAUSED || mode == MODE_RENDERING;
}

gboolean core_mode_allows_loop(AppMode mode) {
  return mode == MODE_IDLE || mode == MODE_RECORDING || mode == MODE_PLAYING || mode == MODE_PAUSED || mode == MODE_RENDERING;
}

const char *core_play_pause_label_for_mode(AppMode mode, gboolean render_should_play) {
  if (mode == MODE_PLAYING) {
    return "Pause";
  }
  if (mode == MODE_RENDERING) {
    return render_should_play ? "Pause" : "Play";
  }
  return "Play";
}

CoreUiState core_build_ui_state(AppMode mode, gboolean render_should_play) {
  CoreUiState state = {0};

  state.record_enabled = core_mode_allows_record(mode);
  state.play_pause_enabled = core_mode_allows_play_pause(mode);
  state.loop_enabled = core_mode_allows_loop(mode);
  state.stop_enabled = core_mode_allows_stop(mode);
  state.play_pause_label = core_play_pause_label_for_mode(mode, render_should_play);
  return state;
}

CoreStatusState core_build_status_state(AppMode mode,
                                       gdouble seconds,
                                       const char *error,
                                       gboolean loop_enabled,
                                       gboolean loop_region_set) {
  CoreStatusState state = {{0}};

  if (error && error[0] != '\0') {
    core_snprintf(state.text,
                  sizeof state.text,
                  "%s | %.1fs captured | %s | Loop %s%s",
                  core_mode_to_text(mode),
                  seconds,
                  error,
                  loop_enabled ? "on" : "off",
                  loop_region_set ? " (set)" : "");
  } else {
    core_snprintf(state.text,
                  sizeof state.text,
                  "%s | %.1fs captured | Loop %s%s",
                  core_mode_to_text(mode),
                  seconds,
                  loop_enabled ? "on" : "off",
                  loop_region_set ? " (set)" : "");
  }

  return state;
}

CorePlayPauseAction core_transport_play_pause_action(AppMode mode) {
  switch (mode) {
    case MODE_IDLE:
      return CORE_PLAY_PAUSE_START_FROM_IDLE;
    case MODE_PLAYING:
      return CORE_PLAY_PAUSE_PAUSE;
    case MODE_PAUSED:
      return CORE_PLAY_PAUSE_RESUME;
    case MODE_RENDERING:
      return CORE_PLAY_PAUSE_TOGGLE_RENDER_INTENT;
    case MODE_RECORDING:
    case MODE_PREPARING:
    default:
      return CORE_PLAY_PAUSE_IGNORED;
  }
}

CoreTransportPlan core_transport_record_plan(AppMode mode, gboolean render_pending, gboolean capture_running) {
  CoreTransportPlan plan = {0};

  if (render_pending) {
    plan.should_start = TRUE;
    plan.should_stop_playback = TRUE;
    plan.should_cancel_render = TRUE;
    plan.reset_buffers = TRUE;
    plan.next_mode = MODE_RECORDING;
    return plan;
  }

  if (mode == MODE_IDLE && !capture_running) {
    plan.should_start = TRUE;
    plan.should_stop_playback = TRUE;
    plan.reset_buffers = TRUE;
    plan.next_mode = MODE_RECORDING;
  }

  return plan;
}

CoreTransportPlan core_transport_stop_plan(AppMode mode, gboolean render_pending) {
  CoreTransportPlan plan = {0};

  if (render_pending) {
    plan.next_mode = MODE_RENDERING;
    return plan;
  }

  plan.should_stop_playback = TRUE;
  plan.should_stop_capture = TRUE;
  plan.next_mode = MODE_IDLE;
  plan.preserve_cursor = (mode == MODE_RECORDING);
  return plan;
}

CoreTransportDecision core_transport_decision(AppMode mode, gboolean render_pending, gboolean capture_running, TransportAction action) {
  CoreTransportDecision decision = {0};

  switch (action) {
    case TRANSPORT_ACTION_RECORD:
      decision.plan = core_transport_record_plan(mode, render_pending, capture_running);
      break;
    case TRANSPORT_ACTION_STOP:
      decision.plan = core_transport_stop_plan(mode, render_pending);
      break;
    case TRANSPORT_ACTION_PLAY_PAUSE:
      decision.play_pause_action = core_transport_play_pause_action(mode);
      break;
    default:
      break;
  }

  return decision;
}

static gdouble clamp_loop_frame(gdouble frame, gdouble total_frames) {
  if (frame < 0.0) {
    return 0.0;
  }
  if (frame > total_frames) {
    return total_frames;
  }
  return frame;
}

gboolean core_get_effective_loop_region(const LoopState *loop, gdouble total_frames, gdouble *start_frames, gdouble *end_frames) {
  gdouble start = loop && loop->region_set ? loop->start_frames : 0.0;
  gdouble end = loop && loop->region_set ? loop->end_frames : total_frames;

  if (total_frames <= 0.0) {
    return FALSE;
  }

  start = clamp_loop_frame(start, total_frames);
  end = clamp_loop_frame(end, total_frames);

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

LoopSnapshot core_get_loop_snapshot(const LoopState *loop, gdouble total_frames) {
  LoopSnapshot snapshot = {0};

  snapshot.enabled = loop ? loop->enabled : FALSE;
  snapshot.explicit_region_set = loop ? loop->region_set : FALSE;
  snapshot.total_frames = total_frames;
  snapshot.effective_region_set = core_get_effective_loop_region(loop, total_frames, &snapshot.start_frames, &snapshot.end_frames);
  return snapshot;
}

gboolean core_compute_loop_drag_region(LoopDragMode drag_mode,
                                       gdouble drag_anchor_frames,
                                       gdouble drag_offset_frames,
                                       gdouble loop_start_frames,
                                       gdouble loop_end_frames,
                                       gdouble current_frames,
                                       gdouble *start_frames,
                                       gdouble *end_frames) {
  if (!start_frames || !end_frames) {
    return FALSE;
  }

  switch (drag_mode) {
    case LOOP_DRAG_CREATE:
      *start_frames = drag_anchor_frames;
      *end_frames = current_frames;
      return TRUE;
    case LOOP_DRAG_START:
      *start_frames = current_frames;
      *end_frames = loop_end_frames;
      return TRUE;
    case LOOP_DRAG_END:
      *start_frames = loop_start_frames;
      *end_frames = current_frames;
      return TRUE;
    case LOOP_DRAG_MOVE: {
      gdouble width = loop_end_frames - loop_start_frames;
      gdouble new_start = current_frames - drag_offset_frames;
      *start_frames = new_start;
      *end_frames = new_start + width;
      return TRUE;
    }
    case LOOP_DRAG_NONE:
    default:
      return FALSE;
  }
}

CoreWaveformPressAction core_resolve_waveform_press(AppMode mode,
                                                    gboolean shift,
                                                    gboolean effective_region_set,
                                                    gboolean near_start,
                                                    gboolean near_end,
                                                    gboolean has_frames) {
  if (mode == MODE_RENDERING && !shift && has_frames) {
    return CORE_WAVEFORM_PRESS_RENDER_SEEK;
  }
  if (shift && has_frames) {
    return CORE_WAVEFORM_PRESS_LOOP_CREATE;
  }
  if (effective_region_set) {
    if (near_start) {
      return CORE_WAVEFORM_PRESS_LOOP_START;
    }
    if (near_end) {
      return CORE_WAVEFORM_PRESS_LOOP_END;
    }
  }
  if (has_frames) {
    return CORE_WAVEFORM_PRESS_SCRUB;
  }
  return CORE_WAVEFORM_PRESS_IGNORE;
}

void core_set_loop_drag(LoopState *loop, LoopDragMode mode, gdouble anchor_frames, gdouble offset_frames) {
  if (!loop) {
    return;
  }

  loop->drag_mode = mode;
  loop->drag_anchor_frames = anchor_frames;
  loop->drag_offset_frames = offset_frames;
}

void core_clear_loop_drag(LoopState *loop) {
  if (!loop) {
    return;
  }

  loop->drag_mode = LOOP_DRAG_NONE;
  loop->drag_anchor_frames = 0.0;
  loop->drag_offset_frames = 0.0;
  loop->drag_anchor_ratio = 0.0;
  loop->drag_offset_ratio = 0.0;
}

void core_finalize_loop_region(LoopState *loop, gdouble total_frames, gdouble start_frames, gdouble end_frames, gdouble min_width) {
  gdouble start = start_frames;
  gdouble end = end_frames;

  if (!loop) {
    return;
  }

  if (start > end) {
    gdouble tmp = start;
    start = end;
    end = tmp;
  }

  start = clamp_loop_frame(start, total_frames);
  end = clamp_loop_frame(end, total_frames);

  if (end - start < min_width) {
    gdouble center = (start + end) * 0.5;
    start = center - (min_width * 0.5);
    end = center + (min_width * 0.5);
    if (start < 0.0) {
      end -= start;
      start = 0.0;
    }
    if (end > total_frames) {
      gdouble delta = end - total_frames;
      end = total_frames;
      start -= delta;
      if (start < 0.0) {
        start = 0.0;
      }
    }
  }

  loop->region_set = TRUE;
  loop->start_frames = start;
  loop->end_frames = end;
  loop->start_ratio = total_frames > 0.0 ? start / total_frames : 0.0;
  loop->end_ratio = total_frames > 0.0 ? end / total_frames : 1.0;
}

void core_reset_recording_session(AudioBuffer *audio,
                                  LoopState *loop,
                                  RenderIntent *intent,
                                  gdouble *playback_cursor_frames,
                                  gdouble *playback_anchor_frames,
                                  gint64 *playback_anchor_us,
                                  gdouble *display_playhead_frames) {
  if (audio) {
    core_byte_array_set_size(audio->pcm, 0);
    core_array_set_size(audio->wave_peaks, 0);
    audio->captured_frames = 0;
    audio->playback_valid = FALSE;
  }

  if (loop) {
    loop->enabled = FALSE;
    loop->region_set = TRUE;
    loop->start_frames = 0.0;
    loop->end_frames = 0.0;
    loop->start_ratio = 0.0;
    loop->end_ratio = 1.0;
    loop->drag_mode = LOOP_DRAG_NONE;
    loop->drag_anchor_frames = 0.0;
    loop->drag_offset_frames = 0.0;
    loop->drag_anchor_ratio = 0.0;
    loop->drag_offset_ratio = 0.0;
  }

  if (intent) {
    intent->should_play = FALSE;
    intent->seek_valid = FALSE;
    intent->seek_pos = 0.0;
  }

  if (playback_cursor_frames) {
    *playback_cursor_frames = 0.0;
  }
  if (playback_anchor_frames) {
    *playback_anchor_frames = 0.0;
  }
  if (playback_anchor_us) {
    *playback_anchor_us = core_get_monotonic_time_us();
  }
  if (display_playhead_frames) {
    *display_playhead_frames = 0.0;
  }
}

gdouble core_get_idle_resume_cursor(const RenderIntent *intent, gdouble display_playhead_frames) {
  if (intent && intent->seek_valid) {
    return intent->seek_pos;
  }

  return display_playhead_frames;
}

gdouble core_get_playhead_ratio(gdouble display_playhead_frames, gdouble total_frames) {
  if (total_frames <= 0.0) {
    return 0.0;
  }

  return display_playhead_frames / total_frames;
}

gdouble core_compute_target_frames(gdouble total_frames, gdouble fraction) {
  if (fraction < 0.0) {
    fraction = 0.0;
  }
  if (fraction > 1.0) {
    fraction = 1.0;
  }
  if (total_frames < 0.0) {
    total_frames = 0.0;
  }

  return total_frames * fraction;
}

void core_set_playback_cursor_state(gdouble frames,
                                    gdouble *playback_cursor_frames,
                                    gdouble *playback_anchor_frames,
                                    gint64 *playback_anchor_us,
                                    gdouble *display_playhead_frames) {
  if (frames < 0.0) {
    frames = 0.0;
  }
  if (playback_cursor_frames) {
    *playback_cursor_frames = frames;
  }
  if (playback_anchor_frames) {
    *playback_anchor_frames = frames;
  }
  if (playback_anchor_us) {
    *playback_anchor_us = core_get_monotonic_time_us();
  }
  if (display_playhead_frames) {
    *display_playhead_frames = frames;
  }
}

gdouble core_apply_seek_fraction(AppMode mode,
                                  gdouble total_frames,
                                  gdouble fraction,
                                  gdouble *playback_cursor_frames,
                                 gdouble *playback_anchor_frames,
                                  gint64 *playback_anchor_us,
                                  gdouble *display_playhead_frames,
                                  RenderIntent *intent) {
  gdouble target_frames = core_compute_target_frames(total_frames, fraction);

  core_set_playback_cursor_state(target_frames,
                                 playback_cursor_frames,
                                 playback_anchor_frames,
                                 playback_anchor_us,
                                 display_playhead_frames);
  if (intent && (mode == MODE_RECORDING || mode == MODE_RENDERING)) {
    intent->seek_valid = TRUE;
    intent->seek_pos = target_frames;
  }

  return target_frames;
}

gboolean core_begin_scrub(AppMode mode, gboolean already_scrubbing, gboolean *resume_after_scrub) {
  if (already_scrubbing || mode == MODE_RENDERING) {
    return FALSE;
  }

  if (resume_after_scrub) {
    *resume_after_scrub = (mode == MODE_PLAYING);
  }

  return TRUE;
}

gboolean core_end_scrub(gboolean *resume_after_scrub) {
  gboolean resume = FALSE;

  if (resume_after_scrub) {
    resume = *resume_after_scrub;
    *resume_after_scrub = FALSE;
  }

  return resume;
}

AppMode core_capture_final_mode(AppMode current_mode, gboolean force_stopped) {
  if (force_stopped || current_mode != MODE_PAUSED) {
    return MODE_IDLE;
  }

  return MODE_PAUSED;
}

AppMode core_playback_final_mode(AppMode current_mode, gboolean reached_end) {
  if (reached_end) {
    return MODE_IDLE;
  }
  if (current_mode == MODE_PREPARING) {
    return MODE_IDLE;
  }
  if (current_mode == MODE_PLAYING) {
    return MODE_PAUSED;
  }

  return current_mode;
}

gdouble core_compute_current_playback_frames(AppMode mode,
                                              gboolean scrubbing,
                                              gdouble cursor_frames,
                                              gdouble anchor_frames,
                                              gint64 anchor_us,
                                              guint rate,
                                              gdouble speed,
                                              gint64 now_us) {
  if (scrubbing) {
    return cursor_frames;
  }

  if (mode == MODE_PLAYING) {
    gdouble elapsed_sec = (now_us - anchor_us) / 1000000.0;
    gdouble estimated = anchor_frames + elapsed_sec * (gdouble)rate * speed;
    if (estimated < cursor_frames) {
      return cursor_frames;
    }
    return estimated;
  }

  return cursor_frames;
}

gdouble core_update_display_playhead(AppMode mode,
                                     gboolean scrubbing,
                                     gdouble display_playhead_frames,
                                     gdouble playback_cursor_frames,
                                     gdouble playback_anchor_frames,
                                     gint64 playback_anchor_us,
                                     guint rate,
                                     gdouble speed,
                                     gint64 now_us) {
  gdouble current_frames = core_compute_current_playback_frames(mode,
                                                                scrubbing,
                                                                playback_cursor_frames,
                                                                playback_anchor_frames,
                                                                playback_anchor_us,
                                                                rate,
                                                                speed,
                                                                now_us);

  if (scrubbing) {
    return current_frames;
  }

  if (mode == MODE_PLAYING) {
    gdouble delta = current_frames - display_playhead_frames;
    if (delta < 0.0) {
      delta = 0.0;
    }
    display_playhead_frames += delta * 0.35;
    if (current_frames - display_playhead_frames < 0.5) {
      display_playhead_frames = current_frames;
    }
    return display_playhead_frames;
  }

  return current_frames;
}

static guint64 core_source_frame_to_output_frame(gdouble source_frame,
                                                 guint64 base_source_frame,
                                                 gdouble rendered_to_source_ratio,
                                                 guint64 playback_frames) {
  gdouble relative_source;
  guint64 output_frame;

  if (rendered_to_source_ratio <= 0.0) {
    rendered_to_source_ratio = 1.0;
  }
  if (source_frame <= (gdouble)base_source_frame) {
    return 0;
  }

  relative_source = source_frame - (gdouble)base_source_frame;
  output_frame = (guint64)(relative_source / rendered_to_source_ratio);
  if (output_frame > playback_frames) {
    output_frame = playback_frames;
  }
  return output_frame;
}

static gdouble core_output_frame_to_source_frame(guint64 output_frame,
                                                guint64 base_source_frame,
                                                gdouble rendered_to_source_ratio,
                                                guint64 total_source_frames) {
  gdouble source_frame;

  if (rendered_to_source_ratio <= 0.0) {
    rendered_to_source_ratio = 1.0;
  }
  source_frame = (gdouble)base_source_frame + ((gdouble)output_frame * rendered_to_source_ratio);
  if (source_frame > (gdouble)total_source_frames) {
    source_frame = (gdouble)total_source_frames;
  }
  return source_frame;
}

void core_playback_timeline_init(CorePlaybackTimeline *timeline,
                                 gdouble start_source_frame,
                                 guint64 total_source_frames,
                                 guint64 playback_frames,
                                 gdouble rendered_to_source_ratio,
                                 guint64 base_source_frame) {
  if (!timeline) {
    return;
  }

  if (rendered_to_source_ratio <= 0.0) {
    rendered_to_source_ratio = 1.0;
  }
  if (start_source_frame < 0.0) {
    start_source_frame = 0.0;
  }
  if (start_source_frame > (gdouble)total_source_frames) {
    start_source_frame = (gdouble)total_source_frames;
  }
  if (base_source_frame > total_source_frames) {
    base_source_frame = total_source_frames;
  }

  timeline->rendered_to_source_ratio = rendered_to_source_ratio;
  timeline->base_source_frame = base_source_frame;
  timeline->total_source_frames = total_source_frames;
  timeline->playback_frames = playback_frames;
  timeline->source_cursor_frame = start_source_frame;
  timeline->output_cursor_frame = core_source_frame_to_output_frame(start_source_frame,
                                                                   base_source_frame,
                                                                   rendered_to_source_ratio,
                                                                   playback_frames);
  timeline->loop_valid = FALSE;
  timeline->loop_armed = FALSE;
  timeline->loop_wrapped = FALSE;
  timeline->loop_start_source_frame = 0;
  timeline->loop_end_source_frame = 0;
  timeline->loop_start_output_frame = 0;
  timeline->loop_end_output_frame = 0;
}

void core_playback_timeline_update_loop(CorePlaybackTimeline *timeline,
                                        const LoopState *loop) {
  gdouble loop_start = 0.0;
  gdouble loop_end = 0.0;

  if (!timeline) {
    return;
  }

  timeline->loop_valid = FALSE;
  timeline->loop_start_source_frame = 0;
  timeline->loop_end_source_frame = 0;
  timeline->loop_start_output_frame = 0;
  timeline->loop_end_output_frame = 0;

  if (!loop || !loop->enabled || !core_get_effective_loop_region(loop,
                                                                 (gdouble)timeline->total_source_frames,
                                                                 &loop_start,
                                                                 &loop_end)) {
    timeline->loop_armed = FALSE;
    timeline->loop_wrapped = FALSE;
    return;
  }

  timeline->loop_start_source_frame = (guint64)loop_start;
  timeline->loop_end_source_frame = (guint64)loop_end;
  if (timeline->loop_end_source_frame <= timeline->loop_start_source_frame ||
      timeline->loop_start_source_frame < timeline->base_source_frame) {
    timeline->loop_armed = FALSE;
    timeline->loop_wrapped = FALSE;
    return;
  }

  timeline->loop_start_output_frame = core_source_frame_to_output_frame((gdouble)timeline->loop_start_source_frame,
                                                                       timeline->base_source_frame,
                                                                       timeline->rendered_to_source_ratio,
                                                                       timeline->playback_frames);
  timeline->loop_end_output_frame = core_source_frame_to_output_frame((gdouble)timeline->loop_end_source_frame,
                                                                     timeline->base_source_frame,
                                                                     timeline->rendered_to_source_ratio,
                                                                     timeline->playback_frames);
  timeline->loop_valid = timeline->loop_end_output_frame > timeline->loop_start_output_frame;
  if (!timeline->loop_valid) {
    timeline->loop_armed = FALSE;
    timeline->loop_wrapped = FALSE;
    return;
  }

  if (!timeline->loop_armed && timeline->source_cursor_frame < (gdouble)timeline->loop_end_source_frame) {
    timeline->loop_armed = TRUE;
  } else if (!timeline->loop_armed) {
    timeline->loop_wrapped = FALSE;
  }

  if (timeline->loop_armed &&
      (timeline->source_cursor_frame >= (gdouble)timeline->loop_end_source_frame ||
       timeline->output_cursor_frame >= timeline->loop_end_output_frame)) {
    timeline->source_cursor_frame = (gdouble)timeline->loop_start_source_frame;
    timeline->output_cursor_frame = timeline->loop_start_output_frame;
    timeline->loop_wrapped = TRUE;
  }
}

guint64 core_playback_timeline_limit_write_frames(CorePlaybackTimeline *timeline,
                                                  guint64 requested_frames) {
  guint64 remaining_frames;

  if (!timeline || requested_frames == 0) {
    return 0;
  }

  if (timeline->loop_valid && timeline->loop_armed) {
    if (timeline->output_cursor_frame >= timeline->loop_end_output_frame) {
      timeline->source_cursor_frame = (gdouble)timeline->loop_end_source_frame;
      return 0;
    }
    remaining_frames = timeline->loop_end_output_frame - timeline->output_cursor_frame;
  } else {
    if (timeline->output_cursor_frame >= timeline->playback_frames) {
      return 0;
    }
    remaining_frames = timeline->playback_frames - timeline->output_cursor_frame;
  }

  return requested_frames < remaining_frames ? requested_frames : remaining_frames;
}

void core_playback_timeline_advance(CorePlaybackTimeline *timeline,
                                    guint64 written_frames) {
  if (!timeline || written_frames == 0) {
    return;
  }

  timeline->output_cursor_frame += written_frames;
  if (timeline->output_cursor_frame > timeline->playback_frames) {
    timeline->output_cursor_frame = timeline->playback_frames;
  }
  timeline->source_cursor_frame = core_output_frame_to_source_frame(timeline->output_cursor_frame,
                                                                   timeline->base_source_frame,
                                                                   timeline->rendered_to_source_ratio,
                                                                   timeline->total_source_frames);

  if (timeline->loop_valid &&
      timeline->loop_armed &&
      timeline->source_cursor_frame >= (gdouble)timeline->loop_end_source_frame) {
    timeline->source_cursor_frame = (gdouble)timeline->loop_start_source_frame;
    timeline->output_cursor_frame = timeline->loop_start_output_frame;
    timeline->loop_wrapped = TRUE;
  }
}

gboolean core_playback_timeline_reached_end(const CorePlaybackTimeline *timeline) {
  return timeline &&
    (!timeline->loop_valid || !timeline->loop_armed) &&
    timeline->output_cursor_frame >= timeline->playback_frames;
}

guint64 core_playback_timeline_rewind_output_cursor(guint64 tail_frame,
                                                    guint64 queued_frames,
                                                    guint64 loop_start_frame,
                                                    guint64 loop_end_frame,
                                                    gboolean loop_valid,
                                                    gboolean loop_wrapped) {
  if (!loop_valid || !loop_wrapped) {
    return queued_frames >= tail_frame ? 0 : tail_frame - queued_frames;
  }

  if (loop_end_frame <= loop_start_frame) {
    return queued_frames >= tail_frame ? 0 : tail_frame - queued_frames;
  }

  {
    const guint64 loop_len = loop_end_frame - loop_start_frame;
    const guint64 queued_mod = queued_frames % loop_len;
    guint64 offset;

    if (tail_frame < loop_start_frame || tail_frame >= loop_end_frame) {
      tail_frame = loop_start_frame;
    }

    offset = tail_frame - loop_start_frame;
    if (queued_mod <= offset) {
      return tail_frame - queued_mod;
    }
    return loop_end_frame - (queued_mod - offset);
  }
}
