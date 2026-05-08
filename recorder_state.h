#ifndef RECORDER_STATE_H
#define RECORDER_STATE_H

#include <gtk/gtk.h>
#include <pulse/thread-mainloop.h>

#include "core.h"
#include "platform.h"
#include "platform_linux.h"

typedef struct {
  GtkWidget *status_label;
  GtkWidget *speed_value_label;
  GtkWidget *time_label;
  GtkWidget *waveform_base;
  GtkWidget *record_button;
  GtkWidget *play_pause_button;
  GtkWidget *loop_button;
  GtkWidget *stop_button;
  GtkWidget *progress_bar;
} AppWidgets;

typedef struct Recorder {
  AppWidgets widgets;
  PlatformLinuxUiContext linux_ui;
  PlatformAdapters platform;

  GMutex mutex;
  AudioBuffer audio;
  gdouble speed;
  gdouble playback_cursor_frames;
  gdouble playback_anchor_frames;
  gint64 playback_anchor_us;
  gdouble display_playhead_frames;
  LoopState loop;
  gboolean scrubbing;
  gboolean resume_after_scrub;

  AppMode mode;
  gboolean stop_requested;
  gboolean capture_running;
  GThread *capture_thread;
  gboolean playback_running;
  gboolean playback_stop_requested;
  GThread *playback_thread;
  pa_threaded_mainloop *playback_ml;
  guint tick_callback_id;
  gint last_playhead_x;
  char last_error[256];
  gboolean render_pending;
  AppMode render_source_mode;
  RenderIntent render_intent;
  GThread *render_thread;
  guint render_pulse_source;
  guint render_generation;
  gint64 render_started_us;
  gdouble render_estimated_total_us;
  gulong loop_toggled_handler_id;
} Recorder;

#endif
