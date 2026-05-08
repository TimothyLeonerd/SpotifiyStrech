#pragma once

#include <QMainWindow>
#include <QVector>

class QPaintEvent;

class QLabel;
class QProgressBar;
class QPushButton;
class QSlider;
class QWidget;

class WaveformWidget : public QWidget {
 public:
  explicit WaveformWidget(QWidget *parent = nullptr);

  void setPeaks(const QVector<int> &peaks);
  void setPlayheadRatio(double ratio);
  void setLoopRegion(double start_ratio, double end_ratio, bool enabled);

 protected:
  void paintEvent(QPaintEvent *event) override;

 private:
  QVector<int> peaks_;
  double playhead_ratio_ = 0.0;
  double loop_start_ratio_ = 0.0;
  double loop_end_ratio_ = 1.0;
  bool loop_enabled_ = false;
};

class RecorderWindow : public QMainWindow {
 public:
  explicit RecorderWindow(QWidget *parent = nullptr);

 private:
  QVector<int> buildDemoPeaks() const;
  void refreshWaveform();
  void updateSpeedLabel();
  void updatePlayPauseLabel();

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
  bool playing_ = false;
  bool loop_enabled_ = false;
  double speed_ = 1.0;
};
