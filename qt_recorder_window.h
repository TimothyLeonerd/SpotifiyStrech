#pragma once

#include <QMainWindow>
#include <QVector>

#include "core.h"
#include "recorder_core.h"

#ifdef _WIN32
#include "platform_windows.h"
#include "windows_audio_backend.h"
#endif

class QPaintEvent;
class QMouseEvent;

class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QWidget;

class RecorderWindow;

class WaveformWidget : public QWidget {
 public:
  explicit WaveformWidget(RecorderWindow *owner, QWidget *parent = nullptr);

  void setPeaks(const QVector<int> &peaks);
  void setPeakScale(double scale);
  void setPlayheadRatio(double ratio);
  void setLoopRegion(double start_ratio, double end_ratio, bool enabled);

 protected:
  void paintEvent(QPaintEvent *event) override;
  void mousePressEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;

 private:
  RecorderWindow *owner_ = nullptr;
  QVector<int> peaks_;
  double peak_scale_ = 100.0;
  double playhead_ratio_ = 0.0;
  double loop_start_ratio_ = 0.0;
  double loop_end_ratio_ = 1.0;
  bool loop_enabled_ = false;
};

class RecorderWindow : public QMainWindow {
 public:
  explicit RecorderWindow(QWidget *parent = nullptr);
  ~RecorderWindow() override;

 private:
  void setMode(AppMode mode);
  void setCapturedFrames(gdouble frames);
  void setSampleRate(gdouble rate);
  gdouble capturedFrames() const;
  gdouble sampleRate() const;
  void setLoopRegion(gdouble start_frames, gdouble end_frames, gboolean set);
  void syncWindowsLoopState();
  void resetPlayhead(gdouble frames);
  void seekFraction(gdouble fraction);
  void tickState(gdouble elapsed_seconds);
  bool waveformMousePress(double x, double width, Qt::KeyboardModifiers modifiers);
  bool waveformMouseMove(double x, double width, Qt::MouseButtons buttons);
  bool waveformMouseRelease(double x, double width);
  gdouble loopMinWidthFrames() const;
  gdouble waveformFraction(double x, double width) const;
  CoreUiState uiState() const;
  CoreStatusState statusState() const;
  gdouble playheadRatio() const;
  gdouble capturedSeconds() const;
  gdouble loopStartRatio() const;
  gdouble loopEndRatio() const;
  void refreshWaveform();
  void refreshFromWindowsBackend();
  void updateSpeedLabel();
  void updatePlayPauseLabel();
  void startWindowsPreparePlayback(bool restart_playback, AppMode fallback_mode);
  void finishWindowsPreparePlayback(unsigned int generation, bool restart_playback, AppMode fallback_mode, bool prepared);
  void commitWindowsSpeedChange();
  void syncUi();

  QWidget *central_ = nullptr;
  QLabel *title_ = nullptr;
  QLabel *status_label_ = nullptr;
  QLabel *time_label_ = nullptr;
  QLabel *speed_value_label_ = nullptr;
  QPushButton *record_button_ = nullptr;
  QPushButton *stop_button_ = nullptr;
  QPushButton *play_pause_button_ = nullptr;
  QPushButton *loop_button_ = nullptr;
  QSlider *speed_slider_ = nullptr;
  QProgressBar *progress_bar_ = nullptr;
  WaveformWidget *waveform_ = nullptr;
  RecorderCore recorder_{};
  AppMode &mode_ = recorder_.mode;
  gboolean &loop_enabled_ = recorder_.loop.enabled;
  gboolean &loop_region_set_ = recorder_.loop.region_set;
  gdouble &loop_start_frames_ = recorder_.loop.start_frames;
  gdouble &loop_end_frames_ = recorder_.loop.end_frames;
  gdouble &speed_ = recorder_.speed;
  gdouble &playback_cursor_frames_ = recorder_.playback_cursor_frames;
  gdouble &playback_anchor_frames_ = recorder_.playback_anchor_frames;
  gdouble &display_playhead_frames_ = recorder_.display_playhead_frames;
  gint64 &playback_anchor_us_ = recorder_.playback_anchor_us;
  LoopDragMode &waveform_drag_mode_ = recorder_.loop.drag_mode;
  gboolean &waveform_scrubbing_ = recorder_.scrubbing;
  gboolean &waveform_resume_after_scrub_ = recorder_.resume_after_scrub;
  gdouble &waveform_drag_anchor_frames_ = recorder_.loop.drag_anchor_frames;
  gdouble &waveform_drag_offset_frames_ = recorder_.loop.drag_offset_frames;
  guint &windows_render_generation_ = recorder_.render_generation;
  gboolean &windows_rendering_ = recorder_.render_pending;

#ifdef _WIN32
  PlatformWindowsContext windows_audio_context_{};
#endif

  friend class WaveformWidget;
};
