#include "linux_audio_backend.h"

static gboolean linux_audio_start_capture(void *user_data, gboolean reset_buffers) {
  return start_capture_thread((Recorder *)user_data, reset_buffers);
}

static void linux_audio_stop_capture(void *user_data, gboolean force_stopped) {
  stop_capture_thread((Recorder *)user_data, force_stopped);
}

static gboolean linux_audio_start_playback(void *user_data) {
  return start_playback_thread((Recorder *)user_data);
}

static void linux_audio_stop_playback(void *user_data, gboolean reset_cursor) {
  stop_playback_thread((Recorder *)user_data, reset_cursor);
}

static const PlatformAudioVTable linux_audio_vtable_impl = {
  .start_capture = linux_audio_start_capture,
  .stop_capture = linux_audio_stop_capture,
  .start_playback = linux_audio_start_playback,
  .stop_playback = linux_audio_stop_playback,
};

const PlatformAudioVTable *linux_audio_backend_vtable(void) {
  return &linux_audio_vtable_impl;
}
