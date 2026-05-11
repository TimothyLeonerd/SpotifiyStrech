#include "playback_renderer.h"

#include "third_party/rubberband/rubberband/rubberband-c.h"

#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

static int clamp_i16(int value) {
  if (value > 32767) {
    return 32767;
  }
  if (value < -32768) {
    return -32768;
  }
  return value;
}

static uint64_t min_u64(uint64_t a, uint64_t b) {
  return a < b ? a : b;
}

static int append_bytes(unsigned char **data, size_t *len, size_t *cap, const void *src, size_t src_len) {
  unsigned char *new_data;
  size_t needed;
  size_t new_cap;

  if (!data || !len || !cap || (!src && src_len > 0)) {
    return 0;
  }
  if (src_len == 0) {
    return 1;
  }
  if (*len > SIZE_MAX - src_len) {
    return 0;
  }

  needed = *len + src_len;
  if (needed > *cap) {
    new_cap = *cap ? *cap : 65536;
    while (new_cap < needed) {
      if (new_cap > SIZE_MAX / 2) {
        new_cap = needed;
        break;
      }
      new_cap *= 2;
    }
    new_data = (unsigned char *)realloc(*data, new_cap);
    if (!new_data) {
      return 0;
    }
    *data = new_data;
    *cap = new_cap;
  }

  memcpy(*data + *len, src, src_len);
  *len = needed;
  return 1;
}

static void fill_float_planes_from_s16(const unsigned char *src,
                                       uint64_t start_frame,
                                       unsigned int frames,
                                       unsigned int channels,
                                       float **planes) {
  const int16_t *input = (const int16_t *)src;

  for (unsigned int i = 0; i < frames; ++i) {
    const int16_t *frame = input + (((size_t)start_frame + i) * channels);
    for (unsigned int c = 0; c < channels; ++c) {
      planes[c][i] = (float)frame[c] / 32768.0f;
    }
  }
}

static int append_float_planes_as_s16(unsigned char **data,
                                      size_t *len,
                                      size_t *cap,
                                      const float *const *planes,
                                      unsigned int frames,
                                      unsigned int channels) {
  int16_t *interleaved;
  size_t sample_count;
  int ok;

  if (channels == 0 || frames == 0) {
    return 1;
  }
  if (frames > SIZE_MAX / channels) {
    return 0;
  }
  sample_count = (size_t)frames * channels;
  interleaved = (int16_t *)malloc(sample_count * sizeof(*interleaved));
  if (!interleaved) {
    return 0;
  }

  for (unsigned int i = 0; i < frames; ++i) {
    for (unsigned int c = 0; c < channels; ++c) {
      float sample = planes[c][i];
      if (sample > 1.0f) {
        sample = 1.0f;
      } else if (sample < -1.0f) {
        sample = -1.0f;
      }
      interleaved[(size_t)i * channels + c] = (int16_t)clamp_i16((int)lrintf(sample * 32767.0f));
    }
  }

  ok = append_bytes(data, len, cap, interleaved, sample_count * sizeof(*interleaved));
  free(interleaved);
  return ok;
}

static void set_error(char *error, size_t error_len, const char *message) {
  if (error && error_len > 0) {
    if (!message) {
      message = "";
    }
    strncpy(error, message, error_len - 1);
    error[error_len - 1] = '\0';
  }
}

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
                                 size_t error_len) {
  const uint64_t input_frames = (start_frame < end_frame) ? (end_frame - start_frame) : 0;
  const unsigned int chunk_frames = 1024;
  const RubberBandOptions options = RubberBandOptionProcessOffline |
                                    RubberBandOptionEngineFiner |
                                    RubberBandOptionChannelsTogether;
  RubberBandState state = NULL;
  float **input_planes = NULL;
  float *input_storage = NULL;
  float **output_planes = NULL;
  float *output_storage = NULL;
  unsigned int output_capacity = 0;
  unsigned char *output = NULL;
  size_t output_len = 0;
  size_t output_cap = 0;
  uint64_t total_frames;

  if (out_result) {
    memset(out_result, 0, sizeof(*out_result));
  }
  set_error(error, error_len, "");

  if (!pcm || !out_result || sample_rate == 0 || channels == 0) {
    set_error(error, error_len, "Invalid renderer input");
    return 0;
  }
  total_frames = pcm_len / ((size_t)channels * sizeof(int16_t));
  if (start_frame > total_frames) {
    start_frame = total_frames;
  }
  if (end_frame > total_frames) {
    end_frame = total_frames;
  }

  if (input_frames == 0 || start_frame >= end_frame) {
    out_result->source_frames = 0;
    out_result->rendered_frames = 0;
    return 1;
  }

  state = rubberband_new(sample_rate, channels, options, (speed > 0.0) ? (1.0 / speed) : 1.0, 1.0);
  if (!state) {
    set_error(error, error_len, "Failed to create Rubber Band stretcher");
    return 0;
  }

  rubberband_set_expected_input_duration(state, (unsigned int)min_u64(input_frames, UINT_MAX));

  input_storage = (float *)malloc((size_t)chunk_frames * channels * sizeof(float));
  input_planes = (float **)calloc(channels, sizeof(float *));
  output_planes = (float **)calloc(channels, sizeof(float *));
  if (!input_storage || !input_planes || !output_planes) {
    set_error(error, error_len, "Failed to allocate renderer buffers");
    goto fail;
  }

  for (unsigned int c = 0; c < channels; ++c) {
    input_planes[c] = input_storage + ((size_t)c * chunk_frames);
  }

  for (unsigned int pass = 0; pass < 2; ++pass) {
    uint64_t offset = 0;

    while (offset < input_frames) {
      unsigned int frames = (unsigned int)min_u64(chunk_frames, input_frames - offset);
      int final = (offset + frames) >= input_frames;

      if (cancel_func && cancel_func(cancel_user_data)) {
        set_error(error, error_len, "Render cancelled");
        goto fail;
      }

      if (progress_func && input_frames > 0) {
        const double pass_base = pass == 0 ? 0.0 : 0.5;
        const double pass_fraction = (double)offset / (double)input_frames;
        progress_func(pass_base + (pass_fraction * 0.5), progress_user_data);
      }

      fill_float_planes_from_s16(pcm, start_frame + offset, frames, channels, input_planes);

      if (pass == 0) {
        rubberband_study(state, (const float *const *)input_planes, frames, final);
      } else {
        rubberband_process(state, (const float *const *)input_planes, frames, final);

        while (rubberband_available(state) > 0) {
          unsigned int avail = (unsigned int)rubberband_available(state);

          if (avail > output_capacity) {
            free(output_storage);
            output_storage = (float *)malloc((size_t)avail * channels * sizeof(float));
            if (!output_storage) {
              set_error(error, error_len, "Failed to allocate renderer output");
              goto fail;
            }
            for (unsigned int c = 0; c < channels; ++c) {
              output_planes[c] = output_storage + ((size_t)c * avail);
            }
            output_capacity = avail;
          }

          rubberband_retrieve(state, (float *const *)output_planes, avail);
          if (!append_float_planes_as_s16(&output, &output_len, &output_cap, (const float *const *)output_planes, avail, channels)) {
            set_error(error, error_len, "Failed to append renderer output");
            goto fail;
          }
        }
      }

      offset += frames;
    }

    if (pass == 0) {
      rubberband_calculate_stretch(state);
    }
  }

  rubberband_delete(state);
  free(input_storage);
  free(input_planes);
  free(output_storage);
  free(output_planes);

  out_result->data = output;
  out_result->len = output_len;
  out_result->source_frames = input_frames;
  out_result->rendered_frames = channels > 0 ? output_len / ((size_t)channels * sizeof(int16_t)) : 0;
  if (progress_func) {
    progress_func(1.0, progress_user_data);
  }
  return 1;

fail:
  if (state) {
    rubberband_delete(state);
  }
  free(input_storage);
  free(input_planes);
  free(output_storage);
  free(output_planes);
  free(output);
  return 0;
}

void playback_renderer_result_clear(PlaybackRenderResult *result) {
  if (!result) {
    return;
  }
  free(result->data);
  memset(result, 0, sizeof(*result));
}
