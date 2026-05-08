#ifndef LINUX_AUDIO_BACKEND_H
#define LINUX_AUDIO_BACKEND_H

#include "recorder_state.h"

const PlatformAudioVTable *linux_audio_backend_vtable(void);

gboolean start_capture_thread(Recorder *r, gboolean reset_buffers);
void stop_capture_thread(Recorder *r, gboolean force_stopped);
gboolean start_playback_thread(Recorder *r);
void stop_playback_thread(Recorder *r, gboolean reset_cursor);

#endif
