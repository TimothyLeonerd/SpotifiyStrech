#include "core.h"

#include <math.h>

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
    g_snprintf(state.text,
               sizeof state.text,
               "%s | %.1fs captured | %s | Loop %s%s",
               core_mode_to_text(mode),
               seconds,
               error,
               loop_enabled ? "on" : "off",
               loop_region_set ? " (set)" : "");
  } else {
    g_snprintf(state.text,
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
    plan.should_cancel_render = TRUE;
    plan.next_mode = MODE_IDLE;
    return plan;
  }

  plan.should_stop_playback = TRUE;
  plan.should_stop_capture = TRUE;
  plan.next_mode = MODE_IDLE;
  plan.preserve_cursor = (mode == MODE_RECORDING);
  return plan;
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

  loop->enabled = TRUE;
  loop->region_set = TRUE;
  loop->start_frames = start;
  loop->end_frames = end;
}

void core_reset_recording_session(AudioBuffer *audio,
                                  LoopState *loop,
                                  RenderIntent *intent,
                                  gdouble *playback_cursor_frames,
                                  gdouble *playback_anchor_frames,
                                  gint64 *playback_anchor_us,
                                  gdouble *display_playhead_frames) {
  if (audio) {
    g_byte_array_set_size(audio->pcm, 0);
    g_array_set_size(audio->wave_peaks, 0);
    audio->captured_frames = 0;
    audio->playback_valid = FALSE;
  }

  if (loop) {
    loop->enabled = FALSE;
    loop->region_set = FALSE;
    loop->drag_mode = LOOP_DRAG_NONE;
    loop->drag_anchor_frames = 0.0;
    loop->drag_offset_frames = 0.0;
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
    *playback_anchor_us = g_get_monotonic_time();
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
