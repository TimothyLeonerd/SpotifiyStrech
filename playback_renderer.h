#ifndef PLAYBACK_RENDERER_H
#define PLAYBACK_RENDERER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int (*PlaybackRenderCancelFunc)(void *user_data);
typedef void (*PlaybackRenderProgressFunc)(double progress, void *user_data);

typedef struct {
  unsigned char *data;
  size_t len;
  uint64_t source_frames;
  uint64_t rendered_frames;
} PlaybackRenderResult;

int playback_renderer_render_s16(const unsigned char *pcm,
                                 size_t pcm_len,
                                 unsigned int sample_rate,
                                 unsigned int channels,
                                 uint64_t start_frame,
                                 uint64_t end_frame,
                                 double speed,
                                 PlaybackRenderCancelFunc cancel_func,
                                 void *cancel_user_data,
                                 PlaybackRenderProgressFunc progress_func,
                                 void *progress_user_data,
                                 PlaybackRenderResult *out_result,
                                 char *error,
                                 size_t error_len);

void playback_renderer_result_clear(PlaybackRenderResult *result);

#ifdef __cplusplus
}
#endif

#endif
