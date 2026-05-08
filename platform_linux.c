#include "platform_linux.h"

#include <math.h>
#include <gtk/gtk.h>

static void linux_draw_waveform_background(cairo_t *cr, double width, double height) {
  const double mid_y = height * 0.5;

  cairo_set_source_rgb(cr, 0.10, 0.10, 0.12);
  cairo_paint(cr);

  cairo_set_source_rgb(cr, 0.18, 0.18, 0.22);
  cairo_set_line_width(cr, 1.0);
  cairo_move_to(cr, 0, mid_y);
  cairo_line_to(cr, width, mid_y);
  cairo_stroke(cr);
}

static void linux_draw_waveform_empty(cairo_t *cr, double width, double height) {
  cairo_text_extents_t extents;
  const char *text = "Waveform appears as you record";

  cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.65);
  cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
  cairo_set_font_size(cr, 18.0);
  cairo_text_extents(cr, text, &extents);
  cairo_move_to(cr, (width - extents.width) * 0.5 - extents.x_bearing, (height - extents.height) * 0.5 - extents.y_bearing);
  cairo_show_text(cr, text);
}

static void linux_draw_waveform_peaks(cairo_t *cr, const guint16 *peaks, gsize peak_count, double width, double height) {
  const double mid_y = height * 0.5;

  cairo_set_source_rgb(cr, 0.35, 0.75, 0.50);
  cairo_set_line_width(cr, 2.0);

  for (int x = 0; x < (int)width; x++) {
    gsize peak_idx = (gsize)x * peak_count / (gsize)width;
    if (peak_idx >= peak_count) {
      peak_idx = peak_count - 1;
    }

    double amp = (double)peaks[peak_idx] / 32768.0;
    double top = mid_y - (amp * (height * 0.42));
    double bottom = mid_y + (amp * (height * 0.42));
    cairo_move_to(cr, x + 0.5, top);
    cairo_line_to(cr, x + 0.5, bottom);
  }

  cairo_stroke(cr);
}

static void linux_draw_loop_region(cairo_t *cr, const PlatformLoopSnapshot *loop, double width, double height) {
  const gboolean active = loop->enabled || loop->explicit_region_set;
  const double alpha = active ? 0.22 : 0.08;
  const double color = active ? 0.45 : 0.55;
  double start_x;
  double end_x;

  if (!loop->effective_region_set || loop->total_frames <= 0.0) {
    return;
  }

  start_x = (loop->start_frames / loop->total_frames) * width;
  end_x = (loop->end_frames / loop->total_frames) * width;

  cairo_set_source_rgba(cr, color, color, color, alpha);
  cairo_rectangle(cr, start_x, 0, MAX(end_x - start_x, 0.0), height);
  cairo_fill(cr);

  cairo_set_source_rgba(cr, color, color, color, active ? 0.55 : 0.20);
  cairo_set_line_width(cr, 3.0);
  cairo_move_to(cr, start_x + 0.5, 0);
  cairo_line_to(cr, start_x + 0.5, height);
  cairo_move_to(cr, end_x + 0.5, 0);
  cairo_line_to(cr, end_x + 0.5, height);
  cairo_stroke(cr);

  cairo_set_source_rgba(cr, color, color, color, active ? 0.9 : 0.35);
  cairo_move_to(cr, start_x - 7.0, 2.0);
  cairo_line_to(cr, start_x + 7.0, 2.0);
  cairo_line_to(cr, start_x, 13.0);
  cairo_close_path(cr);
  cairo_fill(cr);

  cairo_move_to(cr, end_x - 7.0, 2.0);
  cairo_line_to(cr, end_x + 7.0, 2.0);
  cairo_line_to(cr, end_x, 13.0);
  cairo_close_path(cr);
  cairo_fill(cr);
}

static void linux_draw_playhead(cairo_t *cr, double playhead_ratio, double width, double height) {
  double playhead_x = playhead_ratio * width;

  cairo_set_source_rgba(cr, 1.0, 0.55, 0.0, 0.16);
  cairo_rectangle(cr, 0, 0, playhead_x, height);
  cairo_fill(cr);

  cairo_set_source_rgb(cr, 1.0, 0.55, 0.0);
  cairo_set_line_width(cr, 3.0);
  cairo_move_to(cr, playhead_x + 0.5, 0);
  cairo_line_to(cr, playhead_x + 0.5, height);
  cairo_stroke(cr);
}

static void linux_set_status_text(void *user_data, const char *text) {
  PlatformLinuxUiContext *ui = user_data;
  gtk_label_set_text(GTK_LABEL(ui->status_label), text);
}

static void linux_set_time_text(void *user_data, const char *text) {
  PlatformLinuxUiContext *ui = user_data;
  gtk_label_set_text(GTK_LABEL(ui->time_label), text);
}

static void linux_set_play_pause_label(void *user_data, const char *text) {
  PlatformLinuxUiContext *ui = user_data;
  gtk_button_set_label(GTK_BUTTON(ui->play_pause_button), text);
}

static void linux_set_progress_visible(void *user_data, gboolean visible) {
  PlatformLinuxUiContext *ui = user_data;
  if (visible) {
    gtk_widget_show(ui->progress_bar);
  } else {
    gtk_widget_hide(ui->progress_bar);
  }
}

static void linux_set_progress_fraction(void *user_data, gdouble fraction) {
  PlatformLinuxUiContext *ui = user_data;
  gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(ui->progress_bar), fraction);
}

static void linux_set_controls_sensitive(void *user_data,
                                         gboolean record_enabled,
                                         gboolean play_pause_enabled,
                                         gboolean loop_enabled,
                                         gboolean stop_enabled) {
  PlatformLinuxUiContext *ui = user_data;
  gtk_widget_set_sensitive(ui->record_button, record_enabled);
  gtk_widget_set_sensitive(ui->play_pause_button, play_pause_enabled);
  gtk_widget_set_sensitive(ui->loop_button, loop_enabled);
  gtk_widget_set_sensitive(ui->stop_button, stop_enabled);
}

static void linux_queue_waveform_redraw(void *user_data) {
  PlatformLinuxUiContext *ui = user_data;
  gtk_widget_queue_draw(ui->waveform_base);
}

static gboolean linux_grab_pointer(void *user_data, GtkWidget *widget, GdkEventButton *event) {
  PlatformLinuxUiContext *ui = user_data;
  GdkWindow *window = gtk_widget_get_window(widget);
  GdkDevice *device = gdk_event_get_device((GdkEvent *)event);
  GdkSeat *seat;

  (void)ui;

  if (!window || !device) {
    return FALSE;
  }

  seat = gdk_device_get_seat(device);
  if (!seat) {
    return FALSE;
  }

  return gdk_seat_grab(seat,
                       window,
                       GDK_SEAT_CAPABILITY_POINTER,
                       FALSE,
                       NULL,
                       (GdkEvent *)event,
                       NULL,
                       NULL) == GDK_GRAB_SUCCESS;
}

static void linux_release_pointer(void *user_data, GtkWidget *widget, GdkEventButton *event) {
  PlatformLinuxUiContext *ui = user_data;
  GdkWindow *window = gtk_widget_get_window(widget);
  GdkDevice *device = gdk_event_get_device((GdkEvent *)event);
  GdkSeat *seat;

  (void)ui;
  (void)window;

  if (!device) {
    return;
  }

  seat = gdk_device_get_seat(device);
  if (!seat) {
    return;
  }

  gdk_seat_ungrab(seat);
}

static void linux_draw_waveform(void *user_data,
                                cairo_t *cr,
                                double width,
                                double height,
                                const guint16 *peaks,
                                gsize peak_count,
                                const PlatformLoopSnapshot *loop,
                                gdouble playhead_ratio) {
  PlatformLinuxUiContext *ui = user_data;

  (void)ui;

  linux_draw_waveform_background(cr, width, height);

  if (peak_count == 0 || width <= 1.0) {
    linux_draw_waveform_empty(cr, width, height);
    return;
  }

  linux_draw_waveform_peaks(cr, peaks, peak_count, width, height);
  linux_draw_loop_region(cr, loop, width, height);
  linux_draw_playhead(cr, playhead_ratio, width, height);
}

static const PlatformUiVTable linux_ui_vtable = {
  .set_status_text = linux_set_status_text,
  .set_time_text = linux_set_time_text,
  .set_play_pause_label = linux_set_play_pause_label,
  .set_progress_visible = linux_set_progress_visible,
  .set_progress_fraction = linux_set_progress_fraction,
  .set_controls_sensitive = linux_set_controls_sensitive,
  .queue_waveform_redraw = linux_queue_waveform_redraw,
  .draw_waveform = linux_draw_waveform,
  .grab_pointer = linux_grab_pointer,
  .release_pointer = linux_release_pointer,
};

PlatformAdapters platform_linux_build(PlatformLinuxUiContext *ui) {
  PlatformAdapters adapters = {0};
  adapters.backend.kind = PLATFORM_KIND_LINUX;
  adapters.backend.name = "linux-gtk3-pulseaudio";
  adapters.backend.user_data = ui;
  adapters.backend.audio_user_data = NULL;
  adapters.ui = &linux_ui_vtable;
  return adapters;
}
