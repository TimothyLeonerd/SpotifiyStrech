#ifndef WINDOWS_DEBUG_LOG_H
#define WINDOWS_DEBUG_LOG_H

#ifdef _WIN32

#include <windows.h>

static inline void windows_debug_log(const char *message) {
  static SRWLOCK lock = SRWLOCK_INIT;
  static char log_path[MAX_PATH];
  static int log_path_ready = 0;

  if (!message) {
    return;
  }

  OutputDebugStringA(message);
  OutputDebugStringA("\n");

  AcquireSRWLockExclusive(&lock);

  if (!log_path_ready) {
    DWORD written = GetTempPathA(MAX_PATH - 32, log_path);
    if (written == 0 || written >= MAX_PATH - 32) {
      lstrcpyA(log_path, "spotifystretch-debug.log");
    } else {
      lstrcatA(log_path, "spotifystretch-debug.log");
    }
    log_path_ready = 1;
  }

  {
    HANDLE file = CreateFileA(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file != INVALID_HANDLE_VALUE) {
      DWORD ignored = 0;
      WriteFile(file, message, (DWORD)lstrlenA(message), &ignored, NULL);
      WriteFile(file, "\r\n", 2, &ignored, NULL);
      CloseHandle(file);
    }
  }

  ReleaseSRWLockExclusive(&lock);
}

#else

static inline void windows_debug_log(const char *message) {
  (void)message;
}

#endif

#endif
