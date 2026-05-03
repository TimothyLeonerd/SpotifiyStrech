#ifndef CORE_H
#define CORE_H

#include <glib.h>

typedef enum {
  MODE_IDLE = 0,
  MODE_RECORDING,
  MODE_PREPARING,
  MODE_PLAYING,
  MODE_PAUSED,
  MODE_RENDERING,
} AppMode;

typedef enum {
  TRANSPORT_ACTION_RECORD,
  TRANSPORT_ACTION_STOP,
  TRANSPORT_ACTION_PLAY_PAUSE,
} TransportAction;

typedef enum {
  LOOP_DRAG_NONE = 0,
  LOOP_DRAG_CREATE,
  LOOP_DRAG_START,
  LOOP_DRAG_END,
  LOOP_DRAG_MOVE,
} LoopDragMode;

typedef enum {
  RENDER_OUTCOME_SUCCESS,
  RENDER_OUTCOME_CANCELLED,
  RENDER_OUTCOME_FAILED,
} RenderOutcome;

typedef struct {
  gboolean enabled;
  gboolean explicit_region_set;
  gboolean effective_region_set;
  gdouble total_frames;
  gdouble start_frames;
  gdouble end_frames;
} LoopSnapshot;

typedef struct {
  gboolean enabled;
  gboolean region_set;
  gdouble start_frames;
  gdouble end_frames;
  LoopDragMode drag_mode;
  gdouble drag_anchor_frames;
  gdouble drag_offset_frames;
} LoopState;

typedef struct {
  GByteArray *pcm;
  GByteArray *playback_pcm;
  GArray *wave_peaks;
  guint sample_rate;
  guint channels;
  guint64 captured_frames;
  gdouble playback_rendered_to_source_ratio;
  gdouble playback_speed;
  gboolean playback_valid;
} AudioBuffer;

typedef struct {
  gboolean should_play;
  gboolean seek_valid;
  gdouble seek_pos;
} RenderIntent;

typedef enum {
  CORE_PLAY_PAUSE_IGNORED = 0,
  CORE_PLAY_PAUSE_START_FROM_IDLE,
  CORE_PLAY_PAUSE_PAUSE,
  CORE_PLAY_PAUSE_RESUME,
  CORE_PLAY_PAUSE_TOGGLE_RENDER_INTENT,
} CorePlayPauseAction;

typedef struct {
  gboolean should_start;
  gboolean should_stop_playback;
  gboolean should_stop_capture;
  gboolean should_cancel_render;
  gboolean reset_buffers;
  gboolean preserve_cursor;
  AppMode next_mode;
} CoreTransportPlan;

typedef struct {
  gboolean record_enabled;
  gboolean play_pause_enabled;
  gboolean loop_enabled;
  gboolean stop_enabled;
  const char *play_pause_label;
} CoreUiState;

typedef struct {
  char text[512];
} CoreStatusState;

const char *core_mode_to_text(AppMode mode);
gboolean core_mode_allows_record(AppMode mode);
gboolean core_mode_allows_play_pause(AppMode mode);
gboolean core_mode_allows_stop(AppMode mode);
gboolean core_mode_allows_loop(AppMode mode);
const char *core_play_pause_label_for_mode(AppMode mode, gboolean render_should_play);
CoreUiState core_build_ui_state(AppMode mode, gboolean render_should_play);
CoreStatusState core_build_status_state(AppMode mode,
                                       gdouble seconds,
                                       const char *error,
                                       gboolean loop_enabled,
                                       gboolean loop_region_set);
CorePlayPauseAction core_transport_play_pause_action(AppMode mode);
CoreTransportPlan core_transport_record_plan(AppMode mode, gboolean render_pending, gboolean capture_running);
CoreTransportPlan core_transport_stop_plan(AppMode mode, gboolean render_pending);

gboolean core_get_effective_loop_region(const LoopState *loop, gdouble total_frames, gdouble *start_frames, gdouble *end_frames);
LoopSnapshot core_get_loop_snapshot(const LoopState *loop, gdouble total_frames);
void core_finalize_loop_region(LoopState *loop, gdouble total_frames, gdouble start_frames, gdouble end_frames, gdouble min_width);

void core_reset_recording_session(AudioBuffer *audio,
                                  LoopState *loop,
                                  RenderIntent *intent,
                                  gdouble *playback_cursor_frames,
                                  gdouble *playback_anchor_frames,
                                  gint64 *playback_anchor_us,
                                  gdouble *display_playhead_frames);

gdouble core_get_idle_resume_cursor(const RenderIntent *intent, gdouble display_playhead_frames);
gdouble core_get_playhead_ratio(gdouble display_playhead_frames, gdouble total_frames);
gdouble core_compute_target_frames(gdouble total_frames, gdouble fraction);
gdouble core_compute_current_playback_frames(AppMode mode,
                                             gboolean scrubbing,
                                             gdouble cursor_frames,
                                             gdouble anchor_frames,
                                             gint64 anchor_us,
                                             guint rate,
                                             gdouble speed,
                                             gint64 now_us);

#endif
