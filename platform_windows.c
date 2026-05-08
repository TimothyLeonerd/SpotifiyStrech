#include "platform_windows.h"

static const PlatformAudioVTable windows_audio_vtable = {0};
static const PlatformUiVTable windows_ui_vtable = {0};

PlatformAdapters platform_windows_build(PlatformWindowsContext *context) {
  PlatformAdapters adapters = {0};

  (void)context;

  adapters.backend.kind = PLATFORM_KIND_WINDOWS;
  adapters.backend.name = "windows";
  adapters.backend.user_data = context;
  adapters.audio = &windows_audio_vtable;
  adapters.ui = &windows_ui_vtable;
  return adapters;
}
