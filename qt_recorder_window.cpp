#include "qt_recorder_window.h"
#include "core.h"

#include <QBoxLayout>
#include <QColor>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTimer>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

namespace {
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
}

WaveformWidget::WaveformWidget(QWidget *parent) : QWidget(parent) {
  setMinimumHeight(280);
  setAutoFillBackground(true);
  peaks_.reserve(480);
}

void WaveformWidget::setPeaks(const QVector<int> &peaks) {
  peaks_ = peaks;
  update();
}

void WaveformWidget::setPeakScale(double scale) {
  peak_scale_ = scale > 0.0 ? scale : 1.0;
  update();
}

void WaveformWidget::setPlayheadRatio(double ratio) {
  if (ratio < 0.0) ratio = 0.0;
  if (ratio > 1.0) ratio = 1.0;
  playhead_ratio_ = ratio;
  update();
}

void WaveformWidget::setLoopRegion(double start_ratio, double end_ratio, bool enabled) {
  loop_enabled_ = enabled;
  loop_start_ratio_ = start_ratio;
  loop_end_ratio_ = end_ratio;
  if (loop_start_ratio_ < 0.0) loop_start_ratio_ = 0.0;
  if (loop_end_ratio_ > 1.0) loop_end_ratio_ = 1.0;
  if (loop_start_ratio_ > loop_end_ratio_) {
    std::swap(loop_start_ratio_, loop_end_ratio_);
  }
  update();
}

void WaveformWidget::paintEvent(QPaintEvent *event) {
  (void)event;
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const QRect r = rect();
  painter.fillRect(r, QColor(16, 18, 24));

  painter.setPen(QPen(QColor(44, 48, 62), 1));
  painter.drawLine(r.left(), r.center().y(), r.right(), r.center().y());

  if (peaks_.isEmpty()) {
    painter.setPen(QColor(190, 190, 195, 160));
    painter.drawText(r, Qt::AlignCenter, QStringLiteral("Waveform appears as you record"));
    return;
  }

  if (loop_enabled_ && loop_end_ratio_ > loop_start_ratio_) {
    QRectF loop_rect(loop_start_ratio_ * r.width(), 0,
                     (loop_end_ratio_ - loop_start_ratio_) * r.width(), r.height());
    painter.fillRect(loop_rect, QColor(90, 110, 130, 50));
    painter.setPen(QPen(QColor(110, 140, 160, 140), 2));
    painter.drawLine(loop_rect.left(), 0, loop_rect.left(), r.height());
    painter.drawLine(loop_rect.right(), 0, loop_rect.right(), r.height());
  }

  const int mid_y = r.center().y();
  const int count = peaks_.size();
  painter.setPen(QPen(QColor(78, 199, 132), 1));

  for (int i = 0; i < count; ++i) {
    const double ratio = (count > 1) ? static_cast<double>(i) / static_cast<double>(count - 1) : 0.0;
    const double amp = std::clamp(std::fabs(peaks_.at(i)) / peak_scale_, 0.0, 1.0);
    const int x = static_cast<int>(ratio * (r.width() - 1));
    const int y1 = mid_y - static_cast<int>((r.height() * 0.42) * amp);
    const int y2 = mid_y + static_cast<int>((r.height() * 0.42) * amp);
    painter.drawLine(x + 0.5, y1, x + 0.5, y2);
  }

  painter.fillRect(QRectF(0, 0, playhead_ratio_ * r.width(), r.height()), QColor(255, 140, 0, 36));
  painter.setPen(QPen(QColor(255, 153, 51), 3));
  painter.drawLine(playhead_ratio_ * r.width(), 0, playhead_ratio_ * r.width(), r.height());
}

RecorderWindow::RecorderWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Spotify Audio Recorder"));
  resize(1100, 720);

#ifdef _WIN32
  windows_audio_context_.adapters.backend.user_data = &windows_audio_context_;
  windows_audio_context_.adapters.backend.audio_user_data = &windows_audio_context_;
  windows_audio_context_.adapters.audio = windows_audio_backend_vtable();
#endif

  central_ = new QWidget(this);
  auto *root = new QVBoxLayout(central_);
  root->setContentsMargins(20, 18, 20, 20);
  root->setSpacing(14);

  title_ = new QLabel(QStringLiteral("Spotify Audio Recorder"), central_);
  title_->setObjectName(QStringLiteral("titleLabel"));

  auto *controls_row = new QHBoxLayout();
  controls_row->setSpacing(8);

  record_button_ = new QPushButton(QStringLiteral("Record"), central_);
  stop_button_ = new QPushButton(QStringLiteral("Stop"), central_);
  play_pause_button_ = new QPushButton(QStringLiteral("Play"), central_);
  loop_button_ = new QPushButton(QStringLiteral("Loop"), central_);
  loop_button_->setCheckable(true);

  progress_bar_ = new QProgressBar(central_);
  progress_bar_->setRange(0, 1000);
  progress_bar_->setValue(0);
  progress_bar_->setVisible(false);
  progress_bar_->setFixedWidth(170);

  controls_row->addWidget(record_button_);
  controls_row->addWidget(stop_button_);
  controls_row->addWidget(play_pause_button_);
  controls_row->addWidget(loop_button_);
  controls_row->addStretch(1);
  controls_row->addWidget(progress_bar_);

  auto *speed_row = new QHBoxLayout();
  speed_row->setSpacing(10);

  auto *speed_label = new QLabel(QStringLiteral("Playback speed"), central_);
  speed_slider_ = new QSlider(Qt::Horizontal, central_);
  speed_slider_->setRange(50, 200);
  speed_slider_->setValue(100);
  speed_slider_->setMinimumWidth(320);
  speed_value_label_ = new QLabel(QStringLiteral("1.0x"), central_);
  speed_value_label_->setMinimumWidth(52);

  speed_row->addWidget(speed_label);
  speed_row->addWidget(speed_slider_, 1);
  speed_row->addWidget(speed_value_label_);

  waveform_ = new WaveformWidget(central_);
  setDefaultLoopRegion();

  time_label_ = new QLabel(QStringLiteral("0.0 / 0.0s"), central_);
  status_label_ = new QLabel(QStringLiteral("Idle | 0.0s captured"), central_);

  root->addWidget(title_);
  root->addLayout(controls_row);
  root->addLayout(speed_row);
  root->addWidget(waveform_, 1);
  root->addWidget(time_label_);
  root->addWidget(status_label_);

  setCentralWidget(central_);

  const QString dark_css = QStringLiteral(
    "QMainWindow, QWidget { background: #0f1117; color: #e7e7ea; }"
    "QLabel { color: #e7e7ea; font-size: 13px; }"
    "#titleLabel { font-size: 24px; font-weight: 600; padding-bottom: 4px; }"
    "QPushButton { background: #1b2230; border: 1px solid #2a3446; padding: 8px 14px; border-radius: 8px; }"
    "QPushButton:hover { background: #222b3b; }"
    "QPushButton:pressed { background: #16202d; }"
    "QPushButton:checked { background: #26364a; border-color: #4a6b8c; }"
    "QSlider::groove:horizontal { height: 6px; background: #242d3d; border-radius: 3px; }"
    "QSlider::handle:horizontal { width: 18px; margin: -6px 0; border-radius: 9px; background: #78c7ff; }"
    "QProgressBar { background: #18202d; border: 1px solid #2a3446; border-radius: 7px; text-align: center; }"
    "QProgressBar::chunk { background: #78c7ff; border-radius: 7px; }"
  );
  setStyleSheet(dark_css);

  connect(record_button_, &QPushButton::clicked, this, [this]() {
#ifdef _WIN32
    const bool capture_running = windows_audio_backend_has_capture(windows_audio_context_.adapters.backend.audio_user_data) != 0;
    const CoreTransportDecision decision = core_transport_decision(toAppMode(controller_.mode()), FALSE, capture_running, TRANSPORT_ACTION_RECORD);
    bool started = false;

    if (decision.plan.should_stop_playback && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
      windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, !decision.plan.preserve_cursor);
    }
    if (decision.plan.should_start && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->start_capture) {
      started = windows_audio_context_.adapters.audio->start_capture(windows_audio_context_.adapters.backend.audio_user_data, decision.plan.reset_buffers) != 0;
    }
    if (started) {
      controller_.record();
    } else if (decision.plan.should_start) {
      controller_.stop();
    }
#else
    controller_.record();
#endif
    syncFromController();
  });

  connect(stop_button_, &QPushButton::clicked, this, [this]() {
#ifdef _WIN32
    const CoreTransportDecision decision = core_transport_decision(toAppMode(controller_.mode()), FALSE, windows_audio_backend_has_capture(windows_audio_context_.adapters.backend.audio_user_data) != 0, TRANSPORT_ACTION_STOP);
    if (decision.plan.should_stop_playback && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
      windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, !decision.plan.preserve_cursor);
    }
    if (decision.plan.should_stop_capture && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_capture) {
      windows_audio_context_.adapters.audio->stop_capture(windows_audio_context_.adapters.backend.audio_user_data, TRUE);
    }
#endif
    if (decision.plan.preserve_cursor) {
      controller_.setMode(RecorderMode::Idle);
    } else {
      controller_.stop();
    }
    syncFromController();
  });

  connect(play_pause_button_, &QPushButton::clicked, this, [this]() {
#ifdef _WIN32
    const bool capture_running = windows_audio_backend_has_capture(windows_audio_context_.adapters.backend.audio_user_data) != 0;
    const bool playback_active = windows_audio_backend_has_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
    const CoreTransportDecision decision = core_transport_decision(toAppMode(controller_.mode()), FALSE, capture_running, TRANSPORT_ACTION_PLAY_PAUSE);
    bool started = false;

    switch (decision.play_pause_action) {
      case CORE_PLAY_PAUSE_START_FROM_IDLE:
        if (capture_running && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_capture) {
          windows_audio_context_.adapters.audio->stop_capture(windows_audio_context_.adapters.backend.audio_user_data, FALSE);
        }
        if (windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->start_playback) {
          started = windows_audio_context_.adapters.audio->start_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
        }
        controller_.setMode(started ? RecorderMode::Playing : RecorderMode::Idle);
        break;
      case CORE_PLAY_PAUSE_PAUSE:
        if (playback_active && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
          windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, FALSE);
        }
        controller_.setMode(RecorderMode::Paused);
        break;
      case CORE_PLAY_PAUSE_RESUME:
        if (windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->start_playback) {
          started = windows_audio_context_.adapters.audio->start_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
        }
        controller_.setMode(started ? RecorderMode::Playing : RecorderMode::Paused);
        break;
      case CORE_PLAY_PAUSE_TOGGLE_RENDER_INTENT:
      case CORE_PLAY_PAUSE_IGNORED:
      default:
        break;
    }
#else
    controller_.playPause();
#endif
    syncFromController();
  });

  connect(loop_button_, &QPushButton::toggled, this, [this](bool checked) {
    controller_.toggleLoop(checked);
    setDefaultLoopRegion();
    syncFromController();
  });

  connect(speed_slider_, &QSlider::valueChanged, this, [this](int value) {
    controller_.setSpeed(value / 100.0);
    syncFromController();
  });

  auto *ticker = new QTimer(this);
  ticker->setInterval(33);
  connect(ticker, &QTimer::timeout, this, [this]() {
#ifdef _WIN32
    refreshFromWindowsBackend();
#else
    controller_.tick(0.033);
    syncFromController();
#endif
  });
  ticker->start();

  controller_.setCapturedFrames(0.0);
  setDefaultLoopRegion();
  syncFromController();
}

void RecorderWindow::refreshWaveform() {
#ifdef _WIN32
  void *format_ptr = nullptr;
  int capture_active = 0;
  int playback_active = 0;
  size_t playback_cursor_bytes = 0;
  size_t playback_total_bytes = 0;
  uint16_t *wave_peaks = nullptr;
  size_t wave_peak_count = 0;
  uint64_t captured_frames = 0;

  if (windows_audio_backend_snapshot(&windows_audio_context_,
                                     nullptr,
                                     nullptr,
                                     &format_ptr,
                                     &capture_active,
                                     &playback_active,
                                     &playback_cursor_bytes,
                                     &playback_total_bytes,
                                     &wave_peaks,
                                     &wave_peak_count,
                                     &captured_frames)) {
    const auto *format = static_cast<WAVEFORMATEX *>(format_ptr);
    QVector<int> qt_peaks;
    const double sample_rate = (format && format->nSamplesPerSec > 0)
                                 ? static_cast<double>(format->nSamplesPerSec)
                                 : 44100.0;
    const RecorderMode backend_mode = capture_active ? RecorderMode::Recording
                                                     : (playback_active ? RecorderMode::Playing : RecorderMode::Idle);

    controller_.setSampleRate(sample_rate);
    controller_.setCapturedFrames(static_cast<double>(captured_frames));
    controller_.setMode(backend_mode);
    if (playback_active && playback_total_bytes > 0) {
      controller_.seekFraction(static_cast<double>(playback_cursor_bytes) / static_cast<double>(playback_total_bytes));
    } else if (!capture_active && !playback_active) {
      controller_.seekFraction(0.0);
    }

    if (wave_peaks && wave_peak_count > 0) {
      qt_peaks.reserve(static_cast<int>(wave_peak_count));
      for (size_t i = 0; i < wave_peak_count; ++i) {
        qt_peaks.push_back(static_cast<int>(wave_peaks[i]));
      }
    }

    waveform_->setPeakScale(32768.0);
    waveform_->setPeaks(qt_peaks);
    waveform_->setPlayheadRatio(controller_.playheadRatio());
    waveform_->update();
  }
  free(format_ptr);
  free(wave_peaks);
  return;
#endif
  const std::vector<int> peaks = controller_.waveformPeaks();
  QVector<int> qt_peaks;
  qt_peaks.reserve(static_cast<int>(peaks.size()));
  for (int peak : peaks) {
    qt_peaks.push_back(peak);
  }
  waveform_->setPeakScale(100.0);
  waveform_->setPeaks(qt_peaks);
  waveform_->update();
}

void RecorderWindow::refreshFromWindowsBackend() {
#ifdef _WIN32
  refreshWaveform();
  syncFromController();
#endif
}

void RecorderWindow::updateSpeedLabel() {
  speed_value_label_->setText(QString::number(controller_.speed(), 'f', 1) + QStringLiteral("x"));
}

void RecorderWindow::updatePlayPauseLabel() {
  play_pause_button_->setText(controller_.uiState().play_pause_label.empty()
                                ? QStringLiteral("Play")
                                : QString::fromStdString(controller_.uiState().play_pause_label));
}

void RecorderWindow::setDefaultLoopRegion() {
}

void RecorderWindow::syncFromController() {
  const auto ui = controller_.uiState();
  const auto status = controller_.statusState();

  record_button_->setEnabled(ui.record_enabled);
  play_pause_button_->setEnabled(ui.play_pause_enabled);
  loop_button_->setEnabled(ui.loop_enabled);
  stop_button_->setEnabled(ui.stop_enabled);
  progress_bar_->setVisible(controller_.mode() != RecorderMode::Idle);
  status_label_->setText(QString::fromStdString(status.text));
  const double total_seconds = controller_.capturedSeconds();
  const double play_seconds = controller_.playheadRatio() * total_seconds;
  time_label_->setText(QString::number(play_seconds, 'f', 1) + QStringLiteral(" / ") + QString::number(total_seconds, 'f', 1) + QStringLiteral("s"));
  speed_value_label_->setText(QString::number(controller_.speed(), 'f', 1) + QStringLiteral("x"));
  play_pause_button_->setText(QString::fromStdString(ui.play_pause_label));
  loop_button_->setChecked(controller_.loopEnabled());
  waveform_->setLoopRegion(controller_.loopStartRatio(), controller_.loopEndRatio(), controller_.loopEnabled());
  waveform_->setPlayheadRatio(controller_.playheadRatio());
#ifndef _WIN32
  refreshWaveform();
#endif
}
