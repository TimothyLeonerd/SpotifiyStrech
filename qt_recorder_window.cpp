#include "qt_recorder_window.h"

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

#include <cmath>
#include <algorithm>

WaveformWidget::WaveformWidget(QWidget *parent) : QWidget(parent) {
  setMinimumHeight(280);
  setAutoFillBackground(true);
  peaks_.reserve(480);
}

void WaveformWidget::setPeaks(const QVector<int> &peaks) {
  peaks_ = peaks;
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
  painter.setPen(QPen(QColor(78, 199, 132), 2));

  for (int x = 0; x < r.width(); ++x) {
    int idx = (x * count) / qMax(1, r.width());
    if (idx >= count) idx = count - 1;
    const double amp = peaks_.at(idx) / 100.0;
    const int half = static_cast<int>((r.height() * 0.42) * amp);
    painter.drawLine(x, mid_y - half, x, mid_y + half);
  }

  painter.fillRect(QRectF(0, 0, playhead_ratio_ * r.width(), r.height()), QColor(255, 140, 0, 36));
  painter.setPen(QPen(QColor(255, 153, 51), 3));
  painter.drawLine(playhead_ratio_ * r.width(), 0, playhead_ratio_ * r.width(), r.height());
}

RecorderWindow::RecorderWindow(QWidget *parent) : QMainWindow(parent) {
  setWindowTitle(QStringLiteral("Spotify Audio Recorder"));
  resize(1100, 720);

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
  waveform_->setPeaks(buildDemoPeaks());
  setDefaultLoopRegion();

  time_label_ = new QLabel(QStringLiteral("0.0 / 0.0s"), central_);
  status_label_ = new QLabel(QStringLiteral("Stopped | 0.0s captured"), central_);

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
    controller_.record();
    syncFromController();
  });

  connect(stop_button_, &QPushButton::clicked, this, [this]() {
    controller_.stop();
    syncFromController();
  });

  connect(play_pause_button_, &QPushButton::clicked, this, [this]() {
    controller_.playPause();
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
    controller_.tick(0.033);
    syncFromController();
  });
  ticker->start();

  controller_.setCapturedFrames(0.0);
  setDefaultLoopRegion();
  syncFromController();
}

QVector<int> RecorderWindow::buildDemoPeaks() const {
  QVector<int> peaks;
  peaks.reserve(480);
  for (int i = 0; i < 480; ++i) {
    const double x = static_cast<double>(i) / 480.0;
    const double base = std::sin(x * 18.0) * 0.42 + std::sin(x * 57.0) * 0.18 + 0.5;
    const double shaped = std::max(0.06, std::min(1.0, base));
    peaks.push_back(static_cast<int>(shaped * 100.0));
  }
  return peaks;
}

void RecorderWindow::refreshWaveform() {
  waveform_->update();
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
  controller_.setLoopRegion(0.2, 0.58, controller_.loopEnabled());
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
  waveform_->update();
}
