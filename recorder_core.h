#ifndef RECORDER_CORE_H
#define RECORDER_CORE_H

#include "core.h"

#include <stddef.h>

typedef struct {
  AudioBuffer audio;
  unsigned char *portable_pcm;
  size_t portable_pcm_len;
  size_t portable_pcm_cap;
  gdouble speed;
  gdouble playback_cursor_frames;
  gdouble playback_anchor_frames;
  gint64 playback_anchor_us;
  gdouble display_playhead_frames;
  LoopState loop;
  gboolean scrubbing;
  gboolean resume_after_scrub;

  AppMode mode;
  gboolean render_pending;
  AppMode render_source_mode;
  RenderIntent render_intent;
  guint render_generation;
  gint64 render_started_us;
  gdouble render_estimated_total_us;
} RecorderCore;

typedef struct {
  gboolean valid_generation;
  gboolean should_play;
  gboolean seek_valid;
  gdouble seek_pos;
} RecorderCoreRenderCompletion;

typedef struct {
  gboolean changed;
  gboolean cancel_render;
  AppMode cancel_next_mode;
  gboolean start_render;
  gboolean render_should_play;
  AppMode fallback_mode;
} RecorderCoreSpeedChange;

typedef struct {
  gboolean has_audio;
  gboolean already_playing;
  gboolean buffer_ready;
  gboolean render_pending;
  gboolean should_render;
  gboolean should_start_ready_buffer;
} RecorderCorePlaybackRequest;

#ifdef __cplusplus
extern "C" {
#endif

void recorder_core_init(RecorderCore *core, gint64 now_us);
void recorder_core_dispose(RecorderCore *core);
void recorder_core_reset_session(RecorderCore *core, gint64 now_us);
gboolean recorder_core_append_pcm(RecorderCore *core, const unsigned char *data, size_t bytes, guint frames);
const unsigned char *recorder_core_pcm_data(const RecorderCore *core);
size_t recorder_core_pcm_len(const RecorderCore *core);
void recorder_core_set_mode(RecorderCore *core, AppMode mode);
void recorder_core_set_captured_frames(RecorderCore *core, gdouble frames);
void recorder_core_set_sample_rate(RecorderCore *core, gdouble rate);
gdouble recorder_core_captured_frames(const RecorderCore *core);
gdouble recorder_core_sample_rate(const RecorderCore *core);
gdouble recorder_core_captured_seconds(const RecorderCore *core);
gdouble recorder_core_playhead_ratio(const RecorderCore *core);
void recorder_core_reset_playhead(RecorderCore *core, gdouble frames, gint64 now_us);
CoreTransportDecision recorder_core_transport_decision(const RecorderCore *core,
                                                       gboolean capture_running,
                                                       TransportAction action);
void recorder_core_apply_record_result(RecorderCore *core,
                                       const CoreTransportPlan *plan,
                                       gboolean started,
                                       gint64 now_us);
void recorder_core_apply_stop_plan(RecorderCore *core, const CoreTransportPlan *plan, gint64 now_us);
gdouble recorder_core_prepare_play_from_idle(RecorderCore *core, gint64 now_us);
void recorder_core_apply_play_pause_action(RecorderCore *core, CorePlayPauseAction action, gint64 now_us);
void recorder_core_toggle_render_intent(RecorderCore *core);
gdouble recorder_core_seek_fraction(RecorderCore *core, gdouble fraction, gboolean update_render_intent);
gboolean recorder_core_begin_scrub(RecorderCore *core, gboolean *out_resume_after_scrub);
gboolean recorder_core_end_scrub(RecorderCore *core);
void recorder_core_set_loop_enabled(RecorderCore *core, gboolean enabled);
void recorder_core_set_loop_region(RecorderCore *core, gdouble start_frames, gdouble end_frames, gboolean set);
void recorder_core_materialize_loop_region(RecorderCore *core);
gdouble recorder_core_loop_min_width_frames(const RecorderCore *core);
LoopSnapshot recorder_core_loop_snapshot(const RecorderCore *core);
gdouble recorder_core_loop_start_ratio(const RecorderCore *core);
gdouble recorder_core_loop_end_ratio(const RecorderCore *core);
CoreWaveformPressAction recorder_core_resolve_waveform_press(const RecorderCore *core,
                                                             gdouble target_frames,
                                                             gdouble handle_window,
                                                             gboolean shift);
gboolean recorder_core_begin_loop_drag(RecorderCore *core,
                                       CoreWaveformPressAction action,
                                       gdouble target_frames);
gboolean recorder_core_update_loop_drag(RecorderCore *core, gdouble current_frames);
void recorder_core_clear_loop_drag(RecorderCore *core);
void recorder_core_tick(RecorderCore *core, gdouble elapsed_seconds, gint64 now_us);
gboolean recorder_core_playback_buffer_ready(const RecorderCore *core);
void recorder_core_invalidate_playback_buffer(RecorderCore *core);
RecorderCoreSpeedChange recorder_core_apply_speed_change(RecorderCore *core, gdouble speed);
RecorderCorePlaybackRequest recorder_core_request_playback(RecorderCore *core,
                                                           gboolean playback_running,
                                                           gint64 now_us);
gboolean recorder_core_begin_ready_playback(RecorderCore *core,
                                           gboolean playback_running,
                                           gint64 now_us);
guint recorder_core_begin_render(RecorderCore *core, gint64 now_us);
gboolean recorder_core_render_is_current(const RecorderCore *core, guint generation);
void recorder_core_set_render_estimate(RecorderCore *core,
                                       guint generation,
                                       gint64 started_us,
                                       gdouble estimated_total_us);
void recorder_core_cancel_render(RecorderCore *core, AppMode next_mode);
void recorder_core_clear_current_render(RecorderCore *core, guint generation);
RecorderCoreRenderCompletion recorder_core_complete_render(RecorderCore *core,
                                                           guint generation,
                                                           RenderOutcome outcome);
GByteArray *recorder_core_install_rendered_playback(RecorderCore *core,
                                                   GByteArray *playback_pcm,
                                                   guint64 source_frames,
                                                   guint64 rendered_frames,
                                                   gdouble speed,
                                                   const RecorderCoreRenderCompletion *completion);
gboolean recorder_core_finish_render(RecorderCore *core,
                                     guint generation,
                                     gboolean prepared,
                                     gboolean playback_started,
                                     AppMode fallback_mode);

#ifdef __cplusplus
}
#endif

#endif
