#include "recorder_core.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void recorder_core_init(RecorderCore *core, gint64 now_us) {
  if (!core) {
    return;
  }

  memset(core, 0, sizeof(*core));
  core->audio.sample_rate = 44100;
  core->audio.channels = 2;
  core->audio.playback_rendered_to_source_ratio = 1.0;
  core->audio.playback_speed = 1.0;
  core->speed = 1.0;
  core->loop.region_set = TRUE;
  core->loop.start_ratio = 0.0;
  core->loop.end_ratio = 1.0;
  core->playback_anchor_us = now_us;
  core->mode = MODE_IDLE;
  core->render_source_mode = MODE_IDLE;
}

void recorder_core_dispose(RecorderCore *core) {
  if (!core) {
    return;
  }

  free(core->portable_pcm);
  core->portable_pcm = NULL;
  core->portable_pcm_len = 0;
  core->portable_pcm_cap = 0;
}

void recorder_core_reset_session(RecorderCore *core, gint64 now_us) {
  if (!core) {
    return;
  }

  core_reset_recording_session(&core->audio,
                               &core->loop,
                               &core->render_intent,
                               &core->playback_cursor_frames,
                               &core->playback_anchor_frames,
                               &core->playback_anchor_us,
                               &core->display_playhead_frames);
  core->playback_anchor_us = now_us;
  core->scrubbing = FALSE;
  core->resume_after_scrub = FALSE;
  core->render_source_mode = MODE_IDLE;
  core->portable_pcm_len = 0;
}

gboolean recorder_core_append_pcm(RecorderCore *core, const unsigned char *data, size_t bytes, guint frames) {
  size_t needed;
  size_t new_cap;
  unsigned char *new_data;

  if (!core || !data || bytes == 0) {
    return FALSE;
  }

#ifdef CORE_HAS_GLIB
  if (core->audio.pcm) {
    g_byte_array_append(core->audio.pcm, data, bytes);
    core->audio.captured_frames += frames;
    recorder_core_invalidate_playback_buffer(core);
    return TRUE;
  }
#endif

  if (core->portable_pcm_len > ((size_t)-1) - bytes) {
    return FALSE;
  }

  needed = core->portable_pcm_len + bytes;
  if (needed > core->portable_pcm_cap) {
    new_cap = core->portable_pcm_cap ? core->portable_pcm_cap : 65536;
    while (new_cap < needed) {
      if (new_cap > ((size_t)-1) / 2) {
        new_cap = needed;
        break;
      }
      new_cap *= 2;
    }
    new_data = (unsigned char *)realloc(core->portable_pcm, new_cap);
    if (!new_data) {
      return FALSE;
    }
    core->portable_pcm = new_data;
    core->portable_pcm_cap = new_cap;
  }

  memcpy(core->portable_pcm + core->portable_pcm_len, data, bytes);
  core->portable_pcm_len = needed;
  core->audio.captured_frames += frames;
  recorder_core_invalidate_playback_buffer(core);
  return TRUE;
}

const unsigned char *recorder_core_pcm_data(const RecorderCore *core) {
  return core ? core->portable_pcm : NULL;
}

size_t recorder_core_pcm_len(const RecorderCore *core) {
  return core ? core->portable_pcm_len : 0;
}

void recorder_core_set_mode(RecorderCore *core, AppMode mode) {
  if (!core) {
    return;
  }

  core->mode = mode;
}

void recorder_core_set_captured_frames(RecorderCore *core, gdouble frames) {
  if (!core) {
    return;
  }

  if (frames < 0.0) {
    frames = 0.0;
  }
  core->audio.captured_frames = (guint64)llround(frames);
}

void recorder_core_set_sample_rate(RecorderCore *core, gdouble rate) {
  if (!core || rate <= 0.0) {
    return;
  }

  core->audio.sample_rate = (guint)llround(rate);
}

gdouble recorder_core_captured_frames(const RecorderCore *core) {
  return core ? (gdouble)core->audio.captured_frames : 0.0;
}

gdouble recorder_core_sample_rate(const RecorderCore *core) {
  if (!core || core->audio.sample_rate == 0) {
    return 44100.0;
  }

  return (gdouble)core->audio.sample_rate;
}

gdouble recorder_core_captured_seconds(const RecorderCore *core) {
  return recorder_core_captured_frames(core) / recorder_core_sample_rate(core);
}

gdouble recorder_core_playhead_ratio(const RecorderCore *core) {
  if (!core) {
    return 0.0;
  }

  return core_get_playhead_ratio(core->display_playhead_frames, recorder_core_captured_frames(core));
}

void recorder_core_reset_playhead(RecorderCore *core, gdouble frames, gint64 now_us) {
  if (!core) {
    return;
  }

  if (frames < 0.0) {
    frames = 0.0;
  }

  core->playback_cursor_frames = frames;
  core->playback_anchor_frames = frames;
  core->display_playhead_frames = frames;
  core->playback_anchor_us = now_us;
}

CoreTransportDecision recorder_core_transport_decision(const RecorderCore *core,
                                                       gboolean capture_running,
                                                       TransportAction action) {
  if (!core) {
    CoreTransportDecision decision = {0};
    return decision;
  }

  return core_transport_decision(core->mode, core->render_pending, capture_running, action);
}

void recorder_core_apply_record_result(RecorderCore *core,
                                       const CoreTransportPlan *plan,
                                       gboolean started,
                                       gint64 now_us) {
  if (!core || !plan || !plan->should_start) {
    return;
  }

  if (started) {
    core->mode = MODE_RECORDING;
  } else {
    core->mode = MODE_IDLE;
  }
  recorder_core_reset_playhead(core, 0.0, now_us);
}

void recorder_core_apply_stop_plan(RecorderCore *core, const CoreTransportPlan *plan, gint64 now_us) {
  if (!core || !plan) {
    return;
  }

  core->mode = plan->next_mode;
  if (core->mode == MODE_RENDERING) {
    core->render_intent.should_play = FALSE;
  }
  if (!plan->preserve_cursor) {
    recorder_core_reset_playhead(core, 0.0, now_us);
    if (core->mode == MODE_RENDERING) {
      core->render_intent.seek_valid = TRUE;
      core->render_intent.seek_pos = 0.0;
      core->render_source_mode = MODE_IDLE;
    }
  }
}

gdouble recorder_core_prepare_play_from_idle(RecorderCore *core, gint64 now_us) {
  gdouble cursor = 0.0;

  if (!core) {
    return cursor;
  }

  cursor = core_get_idle_resume_cursor(&core->render_intent, core->display_playhead_frames);
  core_set_playback_cursor_state(cursor,
                                 &core->playback_cursor_frames,
                                 &core->playback_anchor_frames,
                                 &core->playback_anchor_us,
                                 &core->display_playhead_frames);
  core->playback_anchor_us = now_us;
  return cursor;
}

void recorder_core_apply_play_pause_action(RecorderCore *core, CorePlayPauseAction action, gint64 now_us) {
  if (!core) {
    return;
  }

  switch (action) {
    case CORE_PLAY_PAUSE_START_FROM_IDLE:
      core->mode = MODE_PLAYING;
      core->playback_anchor_frames = core->playback_cursor_frames;
      core->playback_anchor_us = now_us;
      break;
    case CORE_PLAY_PAUSE_PAUSE:
      core->mode = MODE_PAUSED;
      core->playback_cursor_frames = core->display_playhead_frames;
      core->playback_anchor_frames = core->playback_cursor_frames;
      core->playback_anchor_us = now_us;
      break;
    case CORE_PLAY_PAUSE_RESUME:
      core->mode = MODE_PLAYING;
      core->playback_anchor_frames = core->playback_cursor_frames;
      core->playback_anchor_us = now_us;
      break;
    case CORE_PLAY_PAUSE_TOGGLE_RENDER_INTENT:
    case CORE_PLAY_PAUSE_IGNORED:
    default:
      break;
  }
}

void recorder_core_toggle_render_intent(RecorderCore *core) {
  if (!core) {
    return;
  }

  core->render_intent.should_play = !core->render_intent.should_play;
  if (core->render_intent.should_play) {
    core->render_intent.seek_valid = TRUE;
    core->render_intent.seek_pos = core->display_playhead_frames;
  }
}

gdouble recorder_core_seek_fraction(RecorderCore *core, gdouble fraction, gboolean update_render_intent) {
  if (!core) {
    return 0.0;
  }

  if (fraction < 0.0) {
    fraction = 0.0;
  } else if (fraction > 1.0) {
    fraction = 1.0;
  }

  return core_apply_seek_fraction(core->mode,
                                  recorder_core_captured_frames(core),
                                  fraction,
                                  &core->playback_cursor_frames,
                                  &core->playback_anchor_frames,
                                  &core->playback_anchor_us,
                                  &core->display_playhead_frames,
                                  update_render_intent ? &core->render_intent : NULL);
}

gboolean recorder_core_begin_scrub(RecorderCore *core, gboolean *out_resume_after_scrub) {
  gboolean resume = FALSE;

  if (!core || !core_begin_scrub(core->mode, core->scrubbing, &resume)) {
    return FALSE;
  }

  core->scrubbing = TRUE;
  core->resume_after_scrub = resume;
  if (out_resume_after_scrub) {
    *out_resume_after_scrub = resume;
  }
  return TRUE;
}

gboolean recorder_core_end_scrub(RecorderCore *core) {
  if (!core || !core->scrubbing) {
    return FALSE;
  }

  core->scrubbing = FALSE;
  return core_end_scrub(&core->resume_after_scrub);
}

void recorder_core_set_loop_enabled(RecorderCore *core, gboolean enabled) {
  if (!core) {
    return;
  }

  core->loop.enabled = enabled;
}

void recorder_core_set_loop_region(RecorderCore *core, gdouble start_frames, gdouble end_frames, gboolean set) {
  gdouble total_frames;

  if (!core) {
    return;
  }

  if (start_frames < 0.0) {
    start_frames = 0.0;
  }
  if (end_frames < 0.0) {
    end_frames = 0.0;
  }
  if (start_frames > end_frames) {
    gdouble tmp = start_frames;
    start_frames = end_frames;
    end_frames = tmp;
  }

  core->loop.region_set = set;
  core->loop.start_frames = start_frames;
  core->loop.end_frames = end_frames;
  total_frames = recorder_core_captured_frames(core);
  core->loop.start_ratio = total_frames > 0.0 ? start_frames / total_frames : 0.0;
  core->loop.end_ratio = total_frames > 0.0 ? end_frames / total_frames : 1.0;
}

static gdouble recorder_core_clamp_ratio(gdouble ratio) {
  if (ratio < 0.0) {
    return 0.0;
  }
  if (ratio > 1.0) {
    return 1.0;
  }
  return ratio;
}

static void recorder_core_set_loop_ratios(RecorderCore *core, gdouble start_ratio, gdouble end_ratio) {
  gdouble total_frames;

  if (!core) {
    return;
  }

  start_ratio = recorder_core_clamp_ratio(start_ratio);
  end_ratio = recorder_core_clamp_ratio(end_ratio);
  core->loop.region_set = TRUE;
  core->loop.start_ratio = start_ratio;
  core->loop.end_ratio = end_ratio;

  total_frames = recorder_core_captured_frames(core);
  core->loop.start_frames = start_ratio * total_frames;
  core->loop.end_frames = end_ratio * total_frames;
}

void recorder_core_materialize_loop_region(RecorderCore *core) {
  gdouble total_frames;
  gdouble start;
  gdouble end;

  if (!core) {
    return;
  }

  total_frames = recorder_core_captured_frames(core);
  start = core->loop.start_ratio * total_frames;
  end = core->loop.end_ratio * total_frames;
  if (start > end) {
    gdouble tmp = start;
    start = end;
    end = tmp;
  }
  core->loop.region_set = TRUE;
  core->loop.start_frames = start;
  core->loop.end_frames = end;
}

gdouble recorder_core_loop_min_width_frames(const RecorderCore *core) {
  return fmax(0.25 * recorder_core_sample_rate(core), 1.0);
}

LoopSnapshot recorder_core_loop_snapshot(const RecorderCore *core) {
  LoopState empty = {0};
  LoopState loop;
  gdouble total_frames;
  if (!core) {
    return core_get_loop_snapshot(&empty, 0.0);
  }

  total_frames = recorder_core_captured_frames(core);
  loop = core->loop;
  if (core->mode == MODE_RECORDING) {
    loop.region_set = TRUE;
    loop.start_frames = loop.start_ratio * total_frames;
    loop.end_frames = loop.end_ratio * total_frames;
  }
  return core_get_loop_snapshot(&loop, total_frames);
}

gdouble recorder_core_loop_start_ratio(const RecorderCore *core) {
  return core ? core->loop.start_ratio : 0.0;
}

gdouble recorder_core_loop_end_ratio(const RecorderCore *core) {
  return core ? core->loop.end_ratio : 1.0;
}

CoreWaveformPressAction recorder_core_resolve_waveform_press(const RecorderCore *core,
                                                             gdouble target_frames,
                                                             gdouble handle_window,
                                                             gboolean shift) {
  gdouble loop_start = 0.0;
  gdouble loop_end = 0.0;
  gboolean effective_region_set = FALSE;
  gdouble total_frames = recorder_core_captured_frames(core);
  LoopSnapshot snapshot;

  if (!core) {
    return CORE_WAVEFORM_PRESS_IGNORE;
  }

  snapshot = recorder_core_loop_snapshot(core);
  effective_region_set = snapshot.effective_region_set;
  loop_start = snapshot.start_frames;
  loop_end = snapshot.end_frames;
  return core_resolve_waveform_press(core->mode,
                                     shift,
                                     effective_region_set,
                                     fabs(target_frames - loop_start) <= handle_window,
                                     fabs(target_frames - loop_end) <= handle_window,
                                     total_frames > 0.0);
}

gboolean recorder_core_begin_loop_drag(RecorderCore *core,
                                       CoreWaveformPressAction action,
                                       gdouble target_frames) {
  gdouble total_frames;
  gdouble target_ratio;

  if (!core) {
    return FALSE;
  }

  total_frames = recorder_core_captured_frames(core);
  target_ratio = total_frames > 0.0 ? target_frames / total_frames : 0.0;
  target_ratio = recorder_core_clamp_ratio(target_ratio);

  switch (action) {
    case CORE_WAVEFORM_PRESS_LOOP_CREATE:
      core_set_loop_drag(&core->loop, LOOP_DRAG_CREATE, target_frames, 0.0);
      core->loop.drag_anchor_ratio = target_ratio;
      recorder_core_set_loop_ratios(core, target_ratio, target_ratio);
      return TRUE;
    case CORE_WAVEFORM_PRESS_LOOP_START:
      core_set_loop_drag(&core->loop, LOOP_DRAG_START, 0.0, 0.0);
      return TRUE;
    case CORE_WAVEFORM_PRESS_LOOP_END:
      core_set_loop_drag(&core->loop, LOOP_DRAG_END, 0.0, 0.0);
      return TRUE;
    default:
      return FALSE;
  }
}

gboolean recorder_core_update_loop_drag(RecorderCore *core, gdouble current_frames) {
  gdouble total_frames = recorder_core_captured_frames(core);
  gdouble current_ratio;

  if (!core || core->loop.drag_mode == LOOP_DRAG_NONE) {
    return FALSE;
  }

  current_ratio = total_frames > 0.0 ? current_frames / total_frames : 0.0;
  current_ratio = recorder_core_clamp_ratio(current_ratio);

  switch (core->loop.drag_mode) {
    case LOOP_DRAG_CREATE:
      recorder_core_set_loop_ratios(core, core->loop.drag_anchor_ratio, current_ratio);
      return TRUE;
    case LOOP_DRAG_START:
      recorder_core_set_loop_ratios(core, current_ratio, core->loop.end_ratio);
      return TRUE;
    case LOOP_DRAG_END:
      recorder_core_set_loop_ratios(core, core->loop.start_ratio, current_ratio);
      return TRUE;
    default:
      return FALSE;
  }
}

void recorder_core_clear_loop_drag(RecorderCore *core) {
  gdouble start_ratio;
  gdouble end_ratio;

  if (!core) {
    return;
  }

  start_ratio = core->loop.start_ratio;
  end_ratio = core->loop.end_ratio;
  if (start_ratio > end_ratio) {
    gdouble tmp = start_ratio;
    start_ratio = end_ratio;
    end_ratio = tmp;
    recorder_core_set_loop_ratios(core, start_ratio, end_ratio);
  }

  core_clear_loop_drag(&core->loop);
  core->scrubbing = FALSE;
}

void recorder_core_tick(RecorderCore *core, gdouble elapsed_seconds, gint64 now_us) {
  if (!core) {
    return;
  }

  if (elapsed_seconds > 0.0 && core->mode == MODE_RECORDING) {
    recorder_core_set_captured_frames(core,
                                      recorder_core_captured_frames(core) + elapsed_seconds * recorder_core_sample_rate(core));
  }

  core->display_playhead_frames = core_update_display_playhead(core->mode,
                                                               core->scrubbing,
                                                               core->display_playhead_frames,
                                                               core->playback_cursor_frames,
                                                               core->playback_anchor_frames,
                                                               core->playback_anchor_us,
                                                               core->audio.sample_rate ? core->audio.sample_rate : 1,
                                                               core->speed,
                                                               now_us);
}

gboolean recorder_core_playback_buffer_ready(const RecorderCore *core) {
  return core &&
    core->audio.playback_valid &&
    core->audio.playback_speed == core->speed &&
    (core->audio.playback_pcm || core->portable_pcm_len > 0);
}

void recorder_core_invalidate_playback_buffer(RecorderCore *core) {
  if (!core) {
    return;
  }

  core->audio.playback_valid = FALSE;
}

RecorderCoreSpeedChange recorder_core_apply_speed_change(RecorderCore *core, gdouble speed) {
  RecorderCoreSpeedChange change = {0};

  if (!core) {
    return change;
  }
  if (speed <= 0.0) {
    speed = 1.0;
  }
  if (core->speed == speed) {
    return change;
  }

  change.changed = TRUE;
  core->speed = speed;
  recorder_core_invalidate_playback_buffer(core);

  if (core->mode == MODE_RENDERING) {
    change.cancel_render = TRUE;
    change.cancel_next_mode = core->render_source_mode;
    change.start_render = TRUE;
    change.fallback_mode = core->render_source_mode;
  } else if (core->mode == MODE_PLAYING) {
    change.start_render = TRUE;
    change.fallback_mode = MODE_PAUSED;
    change.render_should_play = TRUE;
  } else if (core->mode == MODE_PAUSED || core->mode == MODE_IDLE) {
    change.start_render = TRUE;
    change.fallback_mode = core->mode;
  }

  if (core->mode == MODE_RENDERING) {
    change.render_should_play = core->render_intent.should_play;
  }
  core->render_intent.should_play = change.render_should_play;
  return change;
}

RecorderCorePlaybackRequest recorder_core_request_playback(RecorderCore *core,
                                                           gboolean playback_running,
                                                           gint64 now_us) {
  RecorderCorePlaybackRequest request = {0};

  if (!core) {
    return request;
  }

  request.has_audio = recorder_core_captured_frames(core) > 0.0;
  request.already_playing = playback_running;
  request.buffer_ready = recorder_core_playback_buffer_ready(core);
  request.render_pending = core->render_pending && core->mode == MODE_RENDERING;

  if (playback_running || !request.has_audio) {
    return request;
  }

  core->render_intent.should_play = TRUE;
  if (request.buffer_ready) {
    request.should_start_ready_buffer = TRUE;
    return request;
  }

  if (request.render_pending) {
    return request;
  }
  (void)now_us;
  return request;
}

gboolean recorder_core_begin_ready_playback(RecorderCore *core,
                                           gboolean playback_running,
                                           gint64 now_us) {
  if (!core || playback_running || !recorder_core_playback_buffer_ready(core)) {
    return FALSE;
  }

  core->mode = MODE_PREPARING;
  core->playback_anchor_frames = core->playback_cursor_frames;
  core->playback_anchor_us = now_us;
  core->display_playhead_frames = core->playback_cursor_frames;
  return TRUE;
}

guint recorder_core_begin_render(RecorderCore *core, gint64 now_us) {
  if (!core) {
    return 0;
  }

  core->render_source_mode = core->mode;
  core->render_generation++;
  core->render_pending = TRUE;
  core->render_started_us = 0;
  core->render_estimated_total_us = 0.0;
  core->mode = MODE_RENDERING;
  (void)now_us;
  return core->render_generation;
}

gboolean recorder_core_render_is_current(const RecorderCore *core, guint generation) {
  return core && core->render_pending && core->render_generation == generation;
}

void recorder_core_set_render_estimate(RecorderCore *core,
                                       guint generation,
                                       gint64 started_us,
                                       gdouble estimated_total_us) {
  if (!recorder_core_render_is_current(core, generation)) {
    return;
  }

  core->render_started_us = started_us;
  core->render_estimated_total_us = estimated_total_us;
}

void recorder_core_cancel_render(RecorderCore *core, AppMode next_mode) {
  if (!core || !core->render_pending) {
    return;
  }

  core->render_pending = FALSE;
  core->render_generation++;
  core->mode = next_mode;
}

void recorder_core_clear_current_render(RecorderCore *core, guint generation) {
  if (!recorder_core_render_is_current(core, generation)) {
    return;
  }

  core->render_pending = FALSE;
}

RecorderCoreRenderCompletion recorder_core_complete_render(RecorderCore *core,
                                                           guint generation,
                                                           RenderOutcome outcome) {
  RecorderCoreRenderCompletion completion = {0};

  if (!core || core->render_generation != generation) {
    return completion;
  }

  completion.valid_generation = TRUE;
  core->render_pending = FALSE;
  if (outcome == RENDER_OUTCOME_FAILED) {
    core->mode = MODE_IDLE;
  }

  completion.should_play = core->render_intent.should_play;
  completion.seek_valid = core->render_intent.seek_valid;
  completion.seek_pos = core->render_intent.seek_pos;
  core->render_intent.should_play = FALSE;
  core->render_intent.seek_valid = FALSE;
  return completion;
}

GByteArray *recorder_core_install_rendered_playback(RecorderCore *core,
                                                   GByteArray *playback_pcm,
                                                   guint64 source_frames,
                                                   guint64 rendered_frames,
                                                   gdouble speed,
                                                   const RecorderCoreRenderCompletion *completion) {
  GByteArray *old_playback_pcm = NULL;

  if (!core || !completion || !completion->valid_generation) {
    return NULL;
  }

  old_playback_pcm = core->audio.playback_pcm;
  core->audio.playback_pcm = playback_pcm;
  core->audio.playback_rendered_to_source_ratio = rendered_frames > 0
    ? (gdouble)source_frames / (gdouble)rendered_frames
    : 1.0;
  core->audio.playback_speed = speed;
  core->audio.playback_valid = TRUE;

  if (completion->seek_valid) {
    core->playback_cursor_frames = completion->seek_pos;
    core->display_playhead_frames = completion->seek_pos;
  }

  if (completion->should_play) {
    core->mode = MODE_IDLE;
  } else {
    core->mode = (core->render_source_mode == MODE_PLAYING) ? MODE_PAUSED : core->render_source_mode;
  }

  return old_playback_pcm;
}

gboolean recorder_core_finish_render(RecorderCore *core,
                                     guint generation,
                                     gboolean prepared,
                                     gboolean playback_started,
                                     AppMode fallback_mode) {
  gboolean should_play;
  gdouble seek_pos;

  if (!core || generation != core->render_generation) {
    return FALSE;
  }

  should_play = core->render_intent.should_play;
  if (core->render_intent.seek_valid) {
    seek_pos = core->render_intent.seek_pos;
    core_set_playback_cursor_state(seek_pos,
                                   &core->playback_cursor_frames,
                                   &core->playback_anchor_frames,
                                   &core->playback_anchor_us,
                                   &core->display_playhead_frames);
  }
  core->render_pending = FALSE;

  if (prepared) {
    core->audio.playback_valid = TRUE;
    core->audio.playback_speed = core->speed;
  }

  if (prepared && should_play) {
    core->mode = playback_started ? MODE_PLAYING : fallback_mode;
  } else {
    core->mode = prepared
      ? ((core->render_source_mode == MODE_IDLE) ? MODE_IDLE : fallback_mode)
      : MODE_IDLE;
  }
  core->render_intent.should_play = FALSE;
  core->render_intent.seek_valid = FALSE;

  return TRUE;
}
