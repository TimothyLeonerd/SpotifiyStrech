#ifdef _WIN32

#include "platform_windows_win32.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, PWSTR command_line, int show_command) {
  PlatformWindowsContext context = {0};
  PlatformWindowsHost host = {0};

  (void)prev_instance;
  (void)command_line;
  (void)show_command;

  context.instance = instance;
  if (!platform_windows_host_init(&host, &context)) {
    return 1;
  }

  return platform_windows_host_run(&host);
}

#endif
