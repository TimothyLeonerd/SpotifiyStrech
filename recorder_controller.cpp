#include "recorder_controller.h"

#include <chrono>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {
std::int64_t nowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
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
  switch (mode_) {
    case RecorderMode::Idle:
      mode_ = RecorderMode::Playing;
      playback_anchor_frames_ = playback_cursor_frames_;
      playback_anchor_us_ = nowUs();
      break;
    case RecorderMode::Playing:
      mode_ = RecorderMode::Paused;
      playback_cursor_frames_ = display_playhead_frames_;
      playback_anchor_frames_ = playback_cursor_frames_;
      playback_anchor_us_ = nowUs();
      break;
    case RecorderMode::Paused:
      mode_ = RecorderMode::Playing;
      playback_anchor_frames_ = playback_cursor_frames_;
      playback_anchor_us_ = nowUs();
      break;
    case RecorderMode::Recording:
    case RecorderMode::Preparing:
    case RecorderMode::Rendering:
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

void RecorderController::seekFraction(double fraction) {
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;

  const double target_frames = captured_frames_ * fraction;
  resetPlayhead(target_frames);
  if (mode_ == RecorderMode::Playing) {
    playback_anchor_frames_ = target_frames;
    playback_anchor_us_ = nowUs();
  }
}

void RecorderController::tick(double elapsed_seconds) {
  if (elapsed_seconds > 0.0 && mode_ == RecorderMode::Recording) {
    captured_frames_ += elapsed_seconds * sample_rate_;
  }

  if (mode_ != RecorderMode::Playing) {
    display_playhead_frames_ = playback_cursor_frames_;
    return;
  }

  const double estimated = playback_anchor_frames_ + elapsedSecondsSinceAnchor() * sample_rate_ * speed_;
  display_playhead_frames_ = std::max(playback_cursor_frames_, estimated);
}

RecorderUiState RecorderController::uiState() const {
  RecorderUiState state;
  state.record_enabled = allowsRecord(mode_);
  state.play_pause_enabled = allowsPlayPause(mode_);
  state.loop_enabled = allowsLoop(mode_);
  state.stop_enabled = allowsStop(mode_);
  state.play_pause_label = playPauseLabel(mode_);
  return state;
}

RecorderStatusState RecorderController::statusState() const {
  RecorderStatusState state;
  std::ostringstream out;

  out << modeText(mode_) << " | " << std::fixed << std::setprecision(1) << capturedSeconds() << "s captured | Loop "
      << (loop_enabled_ ? "on" : "off");
  if (loop_region_set_) {
    out << " (set)";
  }
  state.text = out.str();
  return state;
}

double RecorderController::playheadRatio() const {
  if (captured_frames_ <= 0.0) {
    return 0.0;
  }
  return std::max(0.0, std::min(1.0, display_playhead_frames_ / captured_frames_));
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

double RecorderController::loopStartRatio() const {
  return captured_frames_ > 0.0 ? std::max(0.0, std::min(1.0, loop_start_frames_ / captured_frames_)) : 0.0;
}

double RecorderController::loopEndRatio() const {
  return captured_frames_ > 0.0 ? std::max(0.0, std::min(1.0, loop_end_frames_ / captured_frames_)) : 1.0;
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
  switch (mode) {
    case RecorderMode::Recording: return "Recording";
    case RecorderMode::Preparing: return "Preparing";
    case RecorderMode::Playing: return "Playing";
    case RecorderMode::Paused: return "Paused";
    case RecorderMode::Rendering: return "Rendering";
    case RecorderMode::Idle:
    default: return "Stopped";
  }
}

bool RecorderController::allowsRecord(RecorderMode mode) {
  return mode == RecorderMode::Idle;
}

bool RecorderController::allowsPlayPause(RecorderMode mode) {
  return mode == RecorderMode::Idle || mode == RecorderMode::Playing || mode == RecorderMode::Paused || mode == RecorderMode::Rendering;
}

bool RecorderController::allowsStop(RecorderMode mode) {
  return mode == RecorderMode::Recording || mode == RecorderMode::Preparing || mode == RecorderMode::Playing || mode == RecorderMode::Paused || mode == RecorderMode::Rendering;
}

bool RecorderController::allowsLoop(RecorderMode mode) {
  return mode == RecorderMode::Idle || mode == RecorderMode::Recording || mode == RecorderMode::Playing || mode == RecorderMode::Paused || mode == RecorderMode::Rendering;
}

const char *RecorderController::playPauseLabel(RecorderMode mode) {
  switch (mode) {
    case RecorderMode::Playing: return "Pause";
    case RecorderMode::Paused: return "Play";
    case RecorderMode::Idle:
    case RecorderMode::Recording:
    case RecorderMode::Preparing:
    case RecorderMode::Rendering:
    default: return "Play";
  }
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
