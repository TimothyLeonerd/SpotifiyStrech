#include "qt_recorder_window.h"
#include "core.h"

#include <QBoxLayout>
#include <QColor>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPalette>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QSlider>
#include <QTimer>
#include <QMetaObject>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <thread>

#ifdef _WIN32
#include <mmreg.h>
#endif

namespace {
gint64 nowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

double mouseX(const QMouseEvent *event) {
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
  return event->position().x();
#else
  return event->localPos().x();
#endif
}
}  // namespace

class JumpSlider : public QSlider {
 public:
  using QSlider::QSlider;

 protected:
  void mousePressEvent(QMouseEvent *event) override {
    if (orientation() == Qt::Horizontal && event->button() == Qt::LeftButton && width() > 0) {
      const double ratio = std::clamp(mouseX(event) / static_cast<double>(width()), 0.0, 1.0);
      setValue(minimum() + static_cast<int>(std::round(ratio * (maximum() - minimum()))));
    }
    QSlider::mousePressEvent(event);
  }
};

WaveformWidget::WaveformWidget(RecorderWindow *owner, QWidget *parent) : QWidget(parent), owner_(owner) {
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

  if (loop_end_ratio_ > loop_start_ratio_) {
    QRectF loop_rect(loop_start_ratio_ * r.width(), 0,
                     (loop_end_ratio_ - loop_start_ratio_) * r.width(), r.height());
    if (loop_enabled_) {
      painter.fillRect(loop_rect, QColor(90, 110, 130, 50));
    }
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

void WaveformWidget::mousePressEvent(QMouseEvent *event) {
  if (owner_ && owner_->waveformMousePress(mouseX(event), width(), event->modifiers())) {
    event->accept();
    return;
  }
  QWidget::mousePressEvent(event);
}

void WaveformWidget::mouseMoveEvent(QMouseEvent *event) {
  if (owner_ && owner_->waveformMouseMove(mouseX(event), width(), event->buttons())) {
    event->accept();
    return;
  }
  QWidget::mouseMoveEvent(event);
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent *event) {
  if (owner_ && owner_->waveformMouseRelease(mouseX(event), width())) {
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

RecorderWindow::RecorderWindow(QWidget *parent) : QMainWindow(parent) {
  recorder_core_init(&recorder_, nowUs());

  setWindowTitle(QStringLiteral("Spotify Audio Recorder"));
  resize(1100, 720);

#ifdef _WIN32
  windows_audio_context_.adapters.backend.user_data = &windows_audio_context_;
  windows_audio_context_.adapters.backend.audio_user_data = &windows_audio_context_;
  windows_audio_context_.adapters.audio = windows_audio_backend_vtable();
  windows_audio_context_.recorder_core = &recorder_;
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
  speed_slider_ = new JumpSlider(Qt::Horizontal, central_);
  speed_slider_->setRange(50, 200);
  speed_slider_->setValue(100);
  speed_slider_->setMinimumWidth(320);
  speed_value_label_ = new QLabel(QStringLiteral("1.0x"), central_);
  speed_value_label_->setMinimumWidth(52);

  speed_row->addWidget(speed_label);
  speed_row->addWidget(speed_slider_, 1);
  speed_row->addWidget(speed_value_label_);

  waveform_ = new WaveformWidget(this, central_);

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
    const CoreTransportDecision decision = recorder_core_transport_decision(&recorder_, capture_running, TRANSPORT_ACTION_RECORD);
    bool started = false;

    if (decision.plan.should_stop_playback && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
      windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, !decision.plan.preserve_cursor);
    }
    if (decision.plan.should_start && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->start_capture) {
      started = windows_audio_context_.adapters.audio->start_capture(windows_audio_context_.adapters.backend.audio_user_data, decision.plan.reset_buffers) != 0;
    }
    recorder_core_apply_record_result(&recorder_, &decision.plan, started ? TRUE : FALSE, nowUs());
#else
    CoreTransportPlan plan = {0};
    plan.should_start = TRUE;
    recorder_core_apply_record_result(&recorder_, &plan, TRUE, nowUs());
#endif
    syncUi();
  });

  connect(stop_button_, &QPushButton::clicked, this, [this]() {
    const gboolean capture_running =
#ifdef _WIN32
      windows_audio_backend_has_capture(windows_audio_context_.adapters.backend.audio_user_data) != 0;
#else
      FALSE;
#endif
    const CoreTransportDecision decision = recorder_core_transport_decision(&recorder_, capture_running, TRANSPORT_ACTION_STOP);
#ifdef _WIN32
    if (decision.plan.should_stop_playback && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
      windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, !decision.plan.preserve_cursor);
    }
    if (decision.plan.should_stop_capture && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_capture) {
      windows_audio_context_.adapters.audio->stop_capture(windows_audio_context_.adapters.backend.audio_user_data, TRUE);
    }
#endif
    recorder_core_apply_stop_plan(&recorder_, &decision.plan, nowUs());
    if (capture_running && decision.plan.should_stop_capture) {
      recorder_core_materialize_loop_region(&recorder_);
      startWindowsPreparePlayback(MODE_IDLE);
    }
    syncUi();
  });

  connect(play_pause_button_, &QPushButton::clicked, this, [this]() {
#ifdef _WIN32
    const bool capture_running = windows_audio_backend_has_capture(windows_audio_context_.adapters.backend.audio_user_data) != 0;
    const bool playback_active = windows_audio_backend_has_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
    const CoreTransportDecision decision = recorder_core_transport_decision(&recorder_, capture_running, TRANSPORT_ACTION_PLAY_PAUSE);

    switch (decision.play_pause_action) {
      case CORE_PLAY_PAUSE_START_FROM_IDLE:
        if (capture_running && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_capture) {
          windows_audio_context_.adapters.audio->stop_capture(windows_audio_context_.adapters.backend.audio_user_data, FALSE);
        }
        recorder_core_prepare_play_from_idle(&recorder_, nowUs());
        {
          const RecorderCorePlaybackRequest request = recorder_core_request_playback(&recorder_, playback_active ? TRUE : FALSE, nowUs());
          if (!request.has_audio) {
            break;
          } else if (request.buffer_ready) {
            startWindowsReadyPlayback(CORE_PLAY_PAUSE_START_FROM_IDLE);
          } else if (request.should_render) {
            startWindowsPreparePlayback(MODE_IDLE, windows_render_generation_);
          } else if (request.render_pending) {
            syncUi();
          }
        }
        break;
      case CORE_PLAY_PAUSE_PAUSE:
        if (playback_active && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
          windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, FALSE);
        }
        recorder_core_apply_play_pause_action(&recorder_, decision.play_pause_action, nowUs());
        break;
      case CORE_PLAY_PAUSE_RESUME:
        if (!startWindowsReadyPlayback(CORE_PLAY_PAUSE_RESUME)) {
          recorder_.render_intent.should_play = TRUE;
          startWindowsPreparePlayback(MODE_PAUSED);
        }
        break;
      case CORE_PLAY_PAUSE_TOGGLE_RENDER_INTENT:
        recorder_core_toggle_render_intent(&recorder_);
        break;
      case CORE_PLAY_PAUSE_IGNORED:
      default:
        break;
    }
#else
    recorder_core_apply_play_pause_action(&recorder_, core_transport_play_pause_action(mode_), nowUs());
#endif
    syncUi();
  });

  connect(loop_button_, &QPushButton::toggled, this, [this](bool checked) {
    loop_enabled_ = checked;
    syncWindowsLoopState();
    syncUi();
  });

  connect(speed_slider_, &QSlider::valueChanged, this, [this](int value) {
    pending_speed_ = value / 100.0;
#ifdef _WIN32
    if (!speed_slider_->isSliderDown()) {
      commitWindowsSpeedChange();
    }
#endif
    syncUi();
  });

  connect(speed_slider_, &QSlider::sliderReleased, this, [this]() {
#ifdef _WIN32
    commitWindowsSpeedChange();
#endif
    syncUi();
  });

  auto *ticker = new QTimer(this);
  ticker->setInterval(33);
  connect(ticker, &QTimer::timeout, this, [this]() {
#ifdef _WIN32
    refreshFromWindowsBackend();
#else
    tickState(0.033);
    syncUi();
#endif
  });
  ticker->start();

  resetPlayhead(0.0);
  setCapturedFrames(0.0);
  syncUi();
}

RecorderWindow::~RecorderWindow() {
#ifdef _WIN32
  windows_audio_backend_destroy(&windows_audio_context_);
#endif
  recorder_core_dispose(&recorder_);
}

void RecorderWindow::setMode(AppMode mode) {
  recorder_core_set_mode(&recorder_, mode);
}

void RecorderWindow::setCapturedFrames(gdouble frames) {
  recorder_core_set_captured_frames(&recorder_, frames);
}

void RecorderWindow::setSampleRate(gdouble rate) {
  recorder_core_set_sample_rate(&recorder_, rate);
}

gdouble RecorderWindow::capturedFrames() const {
  return recorder_core_captured_frames(&recorder_);
}

gdouble RecorderWindow::sampleRate() const {
  return recorder_core_sample_rate(&recorder_);
}

void RecorderWindow::setLoopRegion(gdouble start_frames, gdouble end_frames, gboolean set) {
  recorder_core_set_loop_region(&recorder_, start_frames, end_frames, set);
  syncWindowsLoopState();
}

void RecorderWindow::syncWindowsLoopState() {
#ifdef _WIN32
  windows_audio_backend_set_loop_state(windows_audio_context_.adapters.backend.audio_user_data,
                                       loop_enabled_ ? TRUE : FALSE,
                                       loop_region_set_,
                                       loop_start_frames_,
                                       loop_end_frames_);
#endif
}

void RecorderWindow::startWindowsPreparePlayback(AppMode fallback_mode, unsigned int generation) {
#ifdef _WIN32
  if (generation == 0) {
    generation = recorder_core_begin_render(&recorder_, nowUs());
  }
  progress_bar_->setRange(0, 1000);
  progress_bar_->setValue(0);
  progress_bar_->setVisible(true);
  syncUi();

  std::thread([this, generation, fallback_mode]() {
    unsigned char *pcm = nullptr;
    size_t pcm_len = 0;
    void *format = nullptr;
    uint64_t captured_frames = 0;
    bool prepared = false;

    if (windows_audio_backend_snapshot(&windows_audio_context_,
                                       &pcm,
                                       &pcm_len,
                                       &format,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       nullptr,
                                       &captured_frames)) {
      prepared = windows_audio_backend_prepare_playback_buffer_from_source(windows_audio_context_.adapters.backend.audio_user_data,
                                                                          pcm,
                                                                          pcm_len,
                                                                          format,
                                                                          captured_frames,
                                                                          speed_) != 0;
    }
    free(pcm);
    free(format);
    QMetaObject::invokeMethod(this, [this, generation, fallback_mode, prepared]() {
      finishWindowsPreparePlayback(generation, fallback_mode, prepared);
    }, Qt::QueuedConnection);
  }).detach();
#else
  (void)fallback_mode;
#endif
}

void RecorderWindow::finishWindowsPreparePlayback(unsigned int generation, AppMode fallback_mode, bool prepared) {
#ifdef _WIN32
  const bool should_start = recorder_.render_intent.should_play;
  bool started = false;
  progress_bar_->setRange(0, 1000);
  progress_bar_->setValue(0);

  if (prepared && should_start && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->start_playback) {
    started = windows_audio_context_.adapters.audio->start_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
  }

  if (!recorder_core_finish_render(&recorder_, generation, prepared, started, fallback_mode)) {
    return;
  }

  syncUi();
#else
  (void)generation;
  (void)fallback_mode;
  (void)prepared;
#endif
}

bool RecorderWindow::startWindowsReadyPlayback(CorePlayPauseAction action) {
#ifdef _WIN32
  const bool playback_active = windows_audio_backend_has_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
  if (!recorder_core_begin_ready_playback(&recorder_, playback_active ? TRUE : FALSE, nowUs())) {
    return false;
  }

  if (!windows_audio_context_.adapters.audio || !windows_audio_context_.adapters.audio->start_playback) {
    recorder_core_set_mode(&recorder_, action == CORE_PLAY_PAUSE_RESUME ? MODE_PAUSED : MODE_IDLE);
    return false;
  }

  if (!windows_audio_context_.adapters.audio->start_playback(windows_audio_context_.adapters.backend.audio_user_data)) {
    recorder_core_set_mode(&recorder_, action == CORE_PLAY_PAUSE_RESUME ? MODE_PAUSED : MODE_IDLE);
    return false;
  }

  recorder_core_apply_play_pause_action(&recorder_, action, nowUs());
  return true;
#else
  (void)action;
  return false;
#endif
}

void RecorderWindow::commitWindowsSpeedChange() {
#ifdef _WIN32
  const bool playback_active = windows_audio_backend_has_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
  RecorderCoreSpeedChange change = recorder_core_apply_speed_change(&recorder_, pending_speed_);

  if (!change.changed) {
    return;
  }

  if (change.cancel_render) {
    recorder_core_cancel_render(&recorder_, change.cancel_next_mode);
  }

  if (playback_active && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
    windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, FALSE);
  }
  if (change.start_render) {
    startWindowsPreparePlayback(change.fallback_mode);
  }
#endif
}

void RecorderWindow::resetPlayhead(gdouble frames) {
  recorder_core_reset_playhead(&recorder_, frames, nowUs());
}

void RecorderWindow::seekFraction(gdouble fraction) {
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;

  const gdouble target_frames = core_compute_target_frames(capturedFrames(), fraction);
  const bool playback_active =
#ifdef _WIN32
    windows_audio_backend_has_playback(windows_audio_context_.adapters.backend.audio_user_data) != 0;
#else
    false;
#endif

#ifdef _WIN32
  if (playback_active && !waveform_scrubbing_ && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
    windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, FALSE);
  }
#endif

  recorder_core_seek_fraction(&recorder_, fraction, FALSE);
#ifdef _WIN32
  windows_audio_backend_seek_playback_frames(windows_audio_context_.adapters.backend.audio_user_data, target_frames);
  if (waveform_scrubbing_) {
    return;
  }
  if (playback_active && !waveform_scrubbing_ && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->start_playback) {
    if (!startWindowsReadyPlayback(CORE_PLAY_PAUSE_RESUME)) {
      recorder_.render_intent.should_play = TRUE;
      startWindowsPreparePlayback(MODE_PAUSED);
    }
  }
#endif
}

gdouble RecorderWindow::loopMinWidthFrames() const {
  return recorder_core_loop_min_width_frames(&recorder_);
}

gdouble RecorderWindow::waveformFraction(double x, double width) const {
  if (width <= 0.0) {
    return 0.0;
  }
  double fraction = x / width;
  if (fraction < 0.0) fraction = 0.0;
  if (fraction > 1.0) fraction = 1.0;
  return fraction;
}

bool RecorderWindow::waveformMousePress(double x, double width, Qt::KeyboardModifiers modifiers) {
  const gdouble total_frames = capturedFrames();
  if (width <= 0.0 || total_frames <= 0.0) {
    return false;
  }

  const gdouble fraction = waveformFraction(x, width);
  const gdouble target_frames = core_compute_target_frames(total_frames, fraction);
  const gdouble handle_window = std::max(loopMinWidthFrames() * 0.25, total_frames * 10.0 / width);
  const gboolean shift = (modifiers & Qt::ShiftModifier) != 0;

  const CoreWaveformPressAction press_action = recorder_core_resolve_waveform_press(&recorder_,
                                                                                    target_frames,
                                                                                    handle_window,
                                                                                    shift);

  switch (press_action) {
    case CORE_WAVEFORM_PRESS_LOOP_CREATE:
      recorder_core_begin_loop_drag(&recorder_, press_action, target_frames);
      syncWindowsLoopState();
      syncUi();
      return true;
    case CORE_WAVEFORM_PRESS_LOOP_START:
      recorder_core_begin_loop_drag(&recorder_, press_action, target_frames);
      syncWindowsLoopState();
      syncUi();
      return true;
    case CORE_WAVEFORM_PRESS_LOOP_END:
      recorder_core_begin_loop_drag(&recorder_, press_action, target_frames);
      syncWindowsLoopState();
      syncUi();
      return true;
    case CORE_WAVEFORM_PRESS_SCRUB:
      if (recorder_core_begin_scrub(&recorder_, nullptr)) {
#ifdef _WIN32
        if (waveform_resume_after_scrub_ && windows_audio_context_.adapters.audio && windows_audio_context_.adapters.audio->stop_playback) {
          windows_audio_context_.adapters.audio->stop_playback(windows_audio_context_.adapters.backend.audio_user_data, FALSE);
        }
#endif
      }
      seekFraction(fraction);
      syncUi();
      return true;
    case CORE_WAVEFORM_PRESS_RENDER_SEEK:
    case CORE_WAVEFORM_PRESS_IGNORE:
    default:
      return false;
  }
}

bool RecorderWindow::waveformMouseMove(double x, double width, Qt::MouseButtons buttons) {
  if (waveform_drag_mode_ == LOOP_DRAG_NONE) {
    if (!waveform_scrubbing_ || !(buttons & Qt::LeftButton)) {
      return false;
    }
    seekFraction(waveformFraction(x, width));
    syncUi();
    return true;
  }

  const gdouble total_frames = capturedFrames();
  if (width <= 0.0 || total_frames <= 0.0) {
    return false;
  }

  const gdouble current_frames = core_compute_target_frames(total_frames, waveformFraction(x, width));
  if (recorder_core_update_loop_drag(&recorder_, current_frames)) {
    syncWindowsLoopState();
    waveform_->setLoopRegion(loopStartRatio(), loopEndRatio(), loop_enabled_);
    syncUi();
    return true;
  }

  return false;
}

bool RecorderWindow::waveformMouseRelease(double x, double width) {
  if (waveform_drag_mode_ == LOOP_DRAG_NONE) {
    if (!waveform_scrubbing_) {
      return false;
    }
    const gboolean resume = recorder_core_end_scrub(&recorder_);
    if (resume) {
#ifdef _WIN32
      if (!startWindowsReadyPlayback(CORE_PLAY_PAUSE_RESUME)) {
        recorder_.render_intent.should_play = TRUE;
        startWindowsPreparePlayback(MODE_PAUSED);
      }
#endif
    }
    syncUi();
    return true;
  }

  const gdouble total_frames = capturedFrames();
  if (width <= 0.0 || total_frames <= 0.0) {
    waveform_drag_mode_ = LOOP_DRAG_NONE;
    return false;
  }

  const gdouble current_frames = core_compute_target_frames(total_frames, waveformFraction(x, width));
  recorder_core_update_loop_drag(&recorder_, current_frames);
  recorder_core_clear_loop_drag(&recorder_);
  syncWindowsLoopState();
  waveform_->setLoopRegion(loopStartRatio(), loopEndRatio(), loop_enabled_);
  syncUi();
  return true;
}

void RecorderWindow::tickState(gdouble elapsed_seconds) {
  recorder_core_tick(&recorder_, elapsed_seconds, nowUs());
}

CoreUiState RecorderWindow::uiState() const {
  const CoreUiState core_state = core_build_ui_state(mode_, recorder_.render_intent.should_play);
  CoreUiState state;
  state.record_enabled = core_state.record_enabled;
  state.play_pause_enabled = core_state.play_pause_enabled;
  state.loop_enabled = core_state.loop_enabled;
  state.stop_enabled = core_state.stop_enabled ||
    (mode_ == MODE_IDLE && capturedFrames() > 0.0 && display_playhead_frames_ > 0.5);
  state.play_pause_label = core_state.play_pause_label ? core_state.play_pause_label : "";
  return state;
}

CoreStatusState RecorderWindow::statusState() const {
  CoreStatusState state;
  const CoreStatusState core_state = core_build_status_state(mode_,
                                                             capturedSeconds(),
                                                             nullptr,
                                                             loop_enabled_,
                                                             loop_region_set_);
  std::snprintf(state.text, sizeof(state.text), "%s", core_state.text);
  return state;
}

gdouble RecorderWindow::playheadRatio() const {
  return recorder_core_playhead_ratio(&recorder_);
}

gdouble RecorderWindow::capturedSeconds() const {
  return recorder_core_captured_seconds(&recorder_);
}

gdouble RecorderWindow::loopStartRatio() const {
  return recorder_core_loop_start_ratio(&recorder_);
}

gdouble RecorderWindow::loopEndRatio() const {
  return recorder_core_loop_end_ratio(&recorder_);
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
    const size_t bytes_per_frame = (format && format->nBlockAlign > 0) ? format->nBlockAlign : 4;
    (void)capture_active;

    setSampleRate(sample_rate);
    setCapturedFrames(static_cast<gdouble>(captured_frames));
    if (playback_active && playback_total_bytes > 0) {
      const gdouble cursor_frames = static_cast<gdouble>(playback_cursor_bytes) / static_cast<gdouble>(bytes_per_frame);
      core_set_playback_cursor_state(cursor_frames,
                                     &playback_cursor_frames_,
                                     &playback_anchor_frames_,
                                     &playback_anchor_us_,
                                     &display_playhead_frames_);
    }

    if (wave_peaks && wave_peak_count > 0) {
      qt_peaks.reserve(static_cast<int>(wave_peak_count));
      for (size_t i = 0; i < wave_peak_count; ++i) {
        qt_peaks.push_back(static_cast<int>(wave_peaks[i]));
      }
    }

    waveform_->setPeakScale(32768.0);
    waveform_->setPeaks(qt_peaks);
    waveform_->setPlayheadRatio(playheadRatio());
    waveform_->update();
  }
  free(format_ptr);
  free(wave_peaks);
  return;
#endif
  QVector<int> qt_peaks;
  waveform_->setPeakScale(100.0);
  waveform_->setPeaks(qt_peaks);
  waveform_->update();
}

void RecorderWindow::refreshFromWindowsBackend() {
#ifdef _WIN32
  if (windows_rendering_) {
    gdouble progress = 0.0;
    int active = 0;
    if (windows_audio_backend_render_progress(windows_audio_context_.adapters.backend.audio_user_data, &progress, &active)) {
      if (progress < 0.0) progress = 0.0;
      if (progress > 1.0) progress = 1.0;
      progress_bar_->setRange(0, 1000);
      progress_bar_->setValue(static_cast<int>(progress * 1000.0));
      (void)active;
    }
    syncUi();
    return;
  }
  refreshWaveform();
  syncUi();
#endif
}

void RecorderWindow::updateSpeedLabel() {
  speed_value_label_->setText(QString::number(pending_speed_, 'f', 1) + QStringLiteral("x"));
}

void RecorderWindow::updatePlayPauseLabel() {
  const CoreUiState ui = core_build_ui_state(mode_, recorder_.render_intent.should_play);
  play_pause_button_->setText(!ui.play_pause_label || !ui.play_pause_label[0]
                                ? QStringLiteral("Play")
                                : QString::fromLatin1(ui.play_pause_label));
}

void RecorderWindow::syncUi() {
  const CoreUiState ui = uiState();
  const CoreStatusState status = statusState();

  record_button_->setEnabled(ui.record_enabled);
  play_pause_button_->setEnabled(ui.play_pause_enabled);
  loop_button_->setEnabled(ui.loop_enabled);
  stop_button_->setEnabled(ui.stop_enabled);
  progress_bar_->setVisible(windows_rendering_ || mode_ != MODE_IDLE);
  status_label_->setText(QString::fromLatin1(status.text));
  const double total_seconds = capturedSeconds();
  const double play_seconds = playheadRatio() * total_seconds;
  time_label_->setText(QString::number(play_seconds, 'f', 1) + QStringLiteral(" / ") + QString::number(total_seconds, 'f', 1) + QStringLiteral("s"));
  speed_value_label_->setText(QString::number(pending_speed_, 'f', 1) + QStringLiteral("x"));
  play_pause_button_->setText(QString::fromLatin1(ui.play_pause_label));
  loop_button_->setChecked(loop_enabled_);
  waveform_->setLoopRegion(loopStartRatio(), loopEndRatio(), loop_enabled_);
  waveform_->setPlayheadRatio(playheadRatio());
#ifndef _WIN32
  refreshWaveform();
#endif
}
