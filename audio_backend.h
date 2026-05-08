#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H

#include <stddef.h>

#if defined(GLIB_MAJOR_VERSION)
#include <glib.h>
#else
#include <stdint.h>
typedef int gboolean;
typedef double gdouble;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
#endif

typedef struct {
  gboolean (*start_capture)(void *user_data, gboolean reset_buffers);
  void (*stop_capture)(void *user_data, gboolean force_stopped);
  gboolean (*start_playback)(void *user_data);
  void (*stop_playback)(void *user_data, gboolean reset_cursor);
} AudioBackendVTable;

#endif
