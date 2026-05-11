#ifdef _WIN32

#include "windows_audio_backend.h"
#include "platform_windows_win32.h"

static void windows_host_command_dispatch(void *user_data, int command_id) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;

  if (!context || !context->adapters.audio) {
    return;
  }

  switch (command_id) {
    case 1005:
      if (context->adapters.audio->start_capture) {
        context->adapters.audio->start_capture(context->adapters.backend.audio_user_data, TRUE);
      }
      break;
    case 1006:
      if (windows_audio_backend_has_playback(context->adapters.backend.audio_user_data)) {
        if (context->adapters.audio->stop_playback) {
          context->adapters.audio->stop_playback(context->adapters.backend.audio_user_data, FALSE);
        }
      } else if (windows_audio_backend_prepare_playback_buffer(context->adapters.backend.audio_user_data, 1.0) && context->adapters.audio->start_playback) {
        context->adapters.audio->start_playback(context->adapters.backend.audio_user_data);
      }
      break;
    case 1008:
      if (context->adapters.audio->stop_capture) {
        context->adapters.audio->stop_capture(context->adapters.backend.audio_user_data, TRUE);
      }
      if (context->adapters.audio->stop_playback) {
        context->adapters.audio->stop_playback(context->adapters.backend.audio_user_data, TRUE);
      }
      break;
    default:
      break;
  }
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, PWSTR command_line, int show_command) {
  PlatformWindowsContext context = {0};
  PlatformWindowsHost host = {0};

  (void)prev_instance;
  (void)command_line;
  (void)show_command;

  context.instance = instance;
  context.adapters.backend.user_data = &context;
  context.adapters.backend.audio_user_data = &context;
  context.adapters.audio = windows_audio_backend_vtable();
  context.command_handler = windows_host_command_dispatch;
  context.command_handler_user_data = &context;
  if (!platform_windows_host_init(&host, &context)) {
    return 1;
  }

  return platform_windows_host_run(&host);
}

#endif
