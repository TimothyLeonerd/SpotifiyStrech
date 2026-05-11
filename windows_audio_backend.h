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
void windows_audio_backend_set_loop_state(void *user_data,
                                          gboolean enabled,
                                          gboolean region_set,
                                          gdouble start_frames,
                                          gdouble end_frames);
int windows_audio_backend_render_progress(void *user_data, gdouble *out_progress, int *out_active);
void windows_audio_backend_seek_playback_frames(void *user_data, gdouble cursor_frames);
int windows_audio_backend_prepare_playback_buffer(void *user_data, gdouble speed);
int windows_audio_backend_prepare_playback_buffer_from_source(void *user_data,
                                                             const unsigned char *pcm,
                                                             size_t pcm_len,
                                                             const void *format,
                                                             uint64_t captured_frames,
                                                             gdouble speed);
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
