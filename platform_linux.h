#ifndef PLATFORM_LINUX_H
#define PLATFORM_LINUX_H

#include "platform.h"

typedef struct _GtkWidget GtkWidget;

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
} PlatformLinuxUiContext;

PlatformAdapters platform_linux_build(PlatformLinuxUiContext *ui);

#endif
