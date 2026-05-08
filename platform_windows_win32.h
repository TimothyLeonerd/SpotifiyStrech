#ifndef PLATFORM_WINDOWS_WIN32_H
#define PLATFORM_WINDOWS_WIN32_H

#include "platform_windows.h"

#ifdef _WIN32

typedef struct PlatformWindowsHost {
  PlatformWindowsContext *context;
  HINSTANCE instance;
  HWND window;
  ATOM window_class;
} PlatformWindowsHost;

BOOL platform_windows_host_init(PlatformWindowsHost *host, PlatformWindowsContext *context);
int platform_windows_host_run(PlatformWindowsHost *host);
void platform_windows_host_shutdown(PlatformWindowsHost *host);
LRESULT platform_windows_host_message(PlatformWindowsHost *host, HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
void platform_windows_host_invalidate_waveform(PlatformWindowsHost *host);

#endif

#endif
