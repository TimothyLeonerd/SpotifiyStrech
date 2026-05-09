#include "recorder_controller.h"

#include "core.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
std::int64_t nowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

AppMode toAppMode(RecorderMode mode) {
  switch (mode) {
    case RecorderMode::Recording: return MODE_RECORDING;
    case RecorderMode::Preparing: return MODE_PREPARING;
    case RecorderMode::Playing: return MODE_PLAYING;
    case RecorderMode::Paused: return MODE_PAUSED;
    case RecorderMode::Rendering: return MODE_RENDERING;
    case RecorderMode::Idle:
    default: return MODE_IDLE;
  }
}
}  // namespace

RecorderController::RecorderController()
    : mode_(RecorderMode::Idle),
      loop_enabled_(false),
      loop_region_set_(false),
      loop_start_frames_(0.0),
      loop_end_frames_(0.0),
      captured_frames_(0.0),
      sample_rate_(44100.0),
      speed_(1.0),
      playback_cursor_frames_(0.0),
      playback_anchor_frames_(0.0),
      display_playhead_frames_(0.0),
      playback_anchor_us_(nowUs()) {}

void RecorderController::record() {
  mode_ = RecorderMode::Recording;
  resetPlayhead(0.0);
}

void RecorderController::stop() {
  mode_ = RecorderMode::Idle;
  resetPlayhead(0.0);
}

void RecorderController::playPause() {
  switch (core_transport_play_pause_action(toAppMode(mode_))) {
    case CORE_PLAY_PAUSE_START_FROM_IDLE:
      mode_ = RecorderMode::Playing;
      playback_anchor_frames_ = playback_cursor_frames_;
      playback_anchor_us_ = nowUs();
      break;
    case CORE_PLAY_PAUSE_PAUSE:
      mode_ = RecorderMode::Paused;
      playback_cursor_frames_ = display_playhead_frames_;
      playback_anchor_frames_ = playback_cursor_frames_;
      playback_anchor_us_ = nowUs();
      break;
    case CORE_PLAY_PAUSE_RESUME:
      mode_ = RecorderMode::Playing;
      playback_anchor_frames_ = playback_cursor_frames_;
      playback_anchor_us_ = nowUs();
      break;
    case CORE_PLAY_PAUSE_TOGGLE_RENDER_INTENT:
    case CORE_PLAY_PAUSE_IGNORED:
    default:
      break;
  }
}

void RecorderController::toggleLoop(bool enabled) {
  loop_enabled_ = enabled;
}

void RecorderController::setLoopRegion(double start_frames, double end_frames, bool set) {
  if (start_frames < 0.0) start_frames = 0.0;
  if (end_frames < 0.0) end_frames = 0.0;
  if (start_frames > end_frames) std::swap(start_frames, end_frames);
  loop_region_set_ = set;
  loop_start_frames_ = start_frames;
  loop_end_frames_ = end_frames;
}

void RecorderController::setSpeed(double speed) {
  if (speed < 0.5) speed = 0.5;
  if (speed > 2.0) speed = 2.0;
  speed_ = speed;
}

void RecorderController::setMode(RecorderMode mode) {
  mode_ = mode;
}

void RecorderController::seekFraction(double fraction) {
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;

  core_apply_seek_fraction(toAppMode(mode_),
                           captured_frames_,
                           fraction,
                           &playback_cursor_frames_,
                           &playback_anchor_frames_,
                           &playback_anchor_us_,
                           &display_playhead_frames_,
                           nullptr);
}

void RecorderController::tick(double elapsed_seconds) {
  if (elapsed_seconds > 0.0 && mode_ == RecorderMode::Recording) {
    captured_frames_ += elapsed_seconds * sample_rate_;
  }

  display_playhead_frames_ = core_update_display_playhead(toAppMode(mode_),
                                                          FALSE,
                                                          display_playhead_frames_,
                                                          playback_cursor_frames_,
                                                          playback_anchor_frames_,
                                                          playback_anchor_us_,
                                                          static_cast<guint>(sample_rate_),
                                                          speed_,
                                                          nowUs());
}

RecorderUiState RecorderController::uiState() const {
  RecorderUiState state;
  const CoreUiState core_state = core_build_ui_state(toAppMode(mode_), FALSE);

  state.record_enabled = core_state.record_enabled;
  state.play_pause_enabled = core_state.play_pause_enabled;
  state.loop_enabled = core_state.loop_enabled;
  state.stop_enabled = core_state.stop_enabled;
  state.play_pause_label = core_state.play_pause_label ? core_state.play_pause_label : "";
  return state;
}

RecorderStatusState RecorderController::statusState() const {
  RecorderStatusState state;
  const CoreStatusState core_state = core_build_status_state(toAppMode(mode_),
                                                             capturedSeconds(),
                                                             nullptr,
                                                             loop_enabled_,
                                                             loop_region_set_);
  state.text = core_state.text;
  return state;
}

double RecorderController::playheadRatio() const {
  return core_get_playhead_ratio(display_playhead_frames_, captured_frames_);
}

double RecorderController::speed() const {
  return speed_;
}

bool RecorderController::loopEnabled() const {
  return loop_enabled_;
}

RecorderMode RecorderController::mode() const {
  return mode_;
}

double RecorderController::capturedSeconds() const {
  return captured_frames_ / sample_rate_;
}

void RecorderController::setCapturedFrames(double frames) {
  if (frames < 0.0) frames = 0.0;
  captured_frames_ = frames;
}

void RecorderController::setSampleRate(double sample_rate) {
  if (sample_rate > 0.0) {
    sample_rate_ = sample_rate;
  }
}

double RecorderController::loopStartRatio() const {
  LoopState loop = {0};
  loop.enabled = loop_enabled_;
  loop.region_set = loop_region_set_;
  loop.start_frames = loop_start_frames_;
  loop.end_frames = loop_end_frames_;
  const LoopSnapshot snapshot = core_get_loop_snapshot(&loop, captured_frames_);
  return snapshot.total_frames > 0.0 ? snapshot.start_frames / snapshot.total_frames : 0.0;
}

double RecorderController::loopEndRatio() const {
  LoopState loop = {0};
  loop.enabled = loop_enabled_;
  loop.region_set = loop_region_set_;
  loop.start_frames = loop_start_frames_;
  loop.end_frames = loop_end_frames_;
  const LoopSnapshot snapshot = core_get_loop_snapshot(&loop, captured_frames_);
  return snapshot.total_frames > 0.0 ? snapshot.end_frames / snapshot.total_frames : 1.0;
}

bool RecorderController::loopRegionSet() const {
  return loop_region_set_;
}

std::vector<int> RecorderController::waveformPeaks() const {
  std::vector<int> peaks;
  peaks.reserve(480);

  const double seconds = std::max(0.1, capturedSeconds());
  const double play = playheadRatio();
  const double speed_value = speed_;

  for (int i = 0; i < 480; ++i) {
    const double x = static_cast<double>(i) / 480.0;
    const double envelope = 0.35 + 0.35 * std::sin((x * 8.0) + seconds * 0.75);
    const double ripple = 0.2 * std::sin((x * 46.0) + play * 12.0) + 0.12 * std::cos((x * 89.0) + speed_value * 3.0);
    const double base = 0.5 + envelope + ripple;
    const double shaped = std::max(0.06, std::min(1.0, base));
    peaks.push_back(static_cast<int>(shaped * 100.0));
  }

  return peaks;
}

const char *RecorderController::modeText(RecorderMode mode) {
  return core_mode_to_text(toAppMode(mode));
}

bool RecorderController::allowsRecord(RecorderMode mode) {
  return core_mode_allows_record(toAppMode(mode));
}

bool RecorderController::allowsPlayPause(RecorderMode mode) {
  return core_mode_allows_play_pause(toAppMode(mode));
}

bool RecorderController::allowsStop(RecorderMode mode) {
  return core_mode_allows_stop(toAppMode(mode));
}

bool RecorderController::allowsLoop(RecorderMode mode) {
  return core_mode_allows_loop(toAppMode(mode));
}

const char *RecorderController::playPauseLabel(RecorderMode mode) {
  return core_play_pause_label_for_mode(toAppMode(mode), FALSE);
}

void RecorderController::resetPlayhead(double frames) {
  if (frames < 0.0) frames = 0.0;
  playback_cursor_frames_ = frames;
  playback_anchor_frames_ = frames;
  display_playhead_frames_ = frames;
  playback_anchor_us_ = nowUs();
}

double RecorderController::elapsedSecondsSinceAnchor() const {
  using namespace std::chrono;
  const std::int64_t elapsed_us = nowUs() - playback_anchor_us_;
  return static_cast<double>(elapsed_us) / 1000000.0;
}
