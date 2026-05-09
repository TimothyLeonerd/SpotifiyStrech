#ifndef WINDOWS_AUDIO_BACKEND_H
#define WINDOWS_AUDIO_BACKEND_H

#include "audio_backend.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

const AudioBackendVTable *windows_audio_backend_vtable(void);
int windows_audio_backend_has_capture(void *user_data);
int windows_audio_backend_has_playback(void *user_data);
int windows_audio_backend_format_is_float(const void *format_ptr);
int windows_audio_backend_snapshot(void *user_data,
                                   unsigned char **out_pcm,
                                   size_t *out_pcm_len,
                                   void **out_format,
                                   int *out_capture_active,
                                   int *out_playback_active,
                                   size_t *out_playback_cursor_bytes,
                                   size_t *out_playback_total_bytes,
                                   uint16_t **out_wave_peaks,
                                   size_t *out_wave_peak_count,
                                   uint64_t *out_captured_frames);
void windows_audio_backend_destroy(void *user_data);

#ifdef __cplusplus
}
#endif

#endif
