#ifndef PLATFORM_H
#define PLATFORM_H

#include <stddef.h>

#if defined(GLIB_MAJOR_VERSION)
#include <glib.h>
#else
#include <stdint.h>
typedef int gboolean;
typedef double gdouble;
typedef size_t gsize;
typedef uint16_t guint16;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#endif

typedef struct _GtkWidget GtkWidget;
typedef struct _GdkEventButton GdkEventButton;
typedef struct _cairo cairo_t;

#include "audio_backend.h"

typedef struct {
  gboolean enabled;
  gboolean explicit_region_set;
  gboolean effective_region_set;
  gdouble total_frames;
  gdouble start_frames;
  gdouble end_frames;
} PlatformLoopSnapshot;

typedef enum {
  PLATFORM_KIND_LINUX = 0,
  PLATFORM_KIND_WINDOWS,
} PlatformKind;

typedef struct {
  PlatformKind kind;
  const char *name;
  void *user_data;
  void *audio_user_data;
} PlatformBackend;

typedef AudioBackendVTable PlatformAudioVTable;

typedef struct {
  void (*set_status_text)(void *user_data, const char *text);
  void (*set_time_text)(void *user_data, const char *text);
  void (*set_play_pause_label)(void *user_data, const char *text);
  void (*set_progress_visible)(void *user_data, gboolean visible);
  void (*set_progress_fraction)(void *user_data, gdouble fraction);
  void (*set_controls_sensitive)(void *user_data,
                                 gboolean record_enabled,
                                 gboolean play_pause_enabled,
                                 gboolean loop_enabled,
                                 gboolean stop_enabled);
  void (*queue_waveform_redraw)(void *user_data);
  void (*draw_waveform)(void *user_data,
                        cairo_t *cr,
                        double width,
                        double height,
                        const guint16 *peaks,
                        gsize peak_count,
                        const PlatformLoopSnapshot *loop,
                        gdouble playhead_ratio);
  gboolean (*grab_pointer)(void *user_data, GtkWidget *widget, GdkEventButton *event);
  void (*release_pointer)(void *user_data, GtkWidget *widget, GdkEventButton *event);
} PlatformUiVTable;

typedef struct {
  PlatformBackend backend;
  const PlatformAudioVTable *audio;
  const PlatformUiVTable *ui;
} PlatformAdapters;

#endif
