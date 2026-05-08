#include "platform_windows.h"
#include "windows_audio_backend.h"

static const PlatformUiVTable windows_ui_vtable = {0};

PlatformAdapters platform_windows_build(PlatformWindowsContext *context) {
  PlatformAdapters adapters = {0};

  (void)context;

  adapters.backend.kind = PLATFORM_KIND_WINDOWS;
  adapters.backend.name = "windows";
  adapters.backend.user_data = context;
  adapters.backend.audio_user_data = context;
  adapters.audio = windows_audio_backend_vtable();
  adapters.ui = &windows_ui_vtable;
  return adapters;
}
