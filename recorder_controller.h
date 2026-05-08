#pragma once

#include <cstdint>
#include <vector>
#include <string>

enum class RecorderMode {
  Idle = 0,
  Recording,
  Preparing,
  Playing,
  Paused,
  Rendering,
};

struct RecorderUiState {
  bool record_enabled = false;
  bool play_pause_enabled = false;
  bool loop_enabled = false;
  bool stop_enabled = false;
  std::string play_pause_label;
};

struct RecorderStatusState {
  std::string text;
};

class RecorderController {
 public:
  RecorderController();

  void record();
  void stop();
  void playPause();
  void toggleLoop(bool enabled);
  void setLoopRegion(double start_frames, double end_frames, bool set);
  void setSpeed(double speed);
  void seekFraction(double fraction);
  void tick(double elapsed_seconds = 0.0);

  RecorderUiState uiState() const;
  RecorderStatusState statusState() const;

  double playheadRatio() const;
  double speed() const;
  bool loopEnabled() const;
  RecorderMode mode() const;
  double capturedSeconds() const;
  void setCapturedFrames(double frames);
  double loopStartRatio() const;
  double loopEndRatio() const;
  bool loopRegionSet() const;
  std::vector<int> waveformPeaks() const;

 private:
  static const char *modeText(RecorderMode mode);
  static bool allowsRecord(RecorderMode mode);
  static bool allowsPlayPause(RecorderMode mode);
  static bool allowsStop(RecorderMode mode);
  static bool allowsLoop(RecorderMode mode);
  static const char *playPauseLabel(RecorderMode mode);
  void resetPlayhead(double frames);
  double elapsedSecondsSinceAnchor() const;

  RecorderMode mode_;
  bool loop_enabled_;
  bool loop_region_set_;
  double loop_start_frames_;
  double loop_end_frames_;
  double captured_frames_;
  double sample_rate_;
  double speed_;
  double playback_cursor_frames_;
  double playback_anchor_frames_;
  double display_playhead_frames_;
  std::int64_t playback_anchor_us_;
};
