#ifndef PLATFORM_WINDOWS_H
#define PLATFORM_WINDOWS_H

#include "platform.h"
#include "recorder_core.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*PlatformWindowsCommandHandler)(void *user_data, int command_id);

typedef struct PlatformWindowsContext {
  PlatformAdapters adapters;
#ifdef _WIN32
  HINSTANCE instance;
  HWND window;
  ATOM window_class;
  RecorderCore *recorder_core;
  void *audio_state;
  HWND status_label;
  HWND speed_value_label;
  HWND time_label;
  HWND waveform_view;
  HWND record_button;
  HWND play_pause_button;
  HWND loop_button;
  HWND stop_button;
  HWND progress_bar;
  PlatformWindowsCommandHandler command_handler;
  void *command_handler_user_data;
#endif
} PlatformWindowsContext;

PlatformAdapters platform_windows_build(PlatformWindowsContext *context);

#ifdef __cplusplus
}
#endif

#endif
