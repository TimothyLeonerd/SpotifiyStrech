#ifndef WAVEFORM_PEAKS_H
#define WAVEFORM_PEAKS_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum {
  WAVEFORM_PCM_INVALID = 0,
  WAVEFORM_PCM_U8,
  WAVEFORM_PCM_S16LE,
  WAVEFORM_PCM_S24LE,
  WAVEFORM_PCM_S32LE,
  WAVEFORM_PCM_FLOAT32LE,
} WaveformPcmFormat;

static inline unsigned int waveform_pcm_bytes_per_sample(WaveformPcmFormat format) {
  switch (format) {
    case WAVEFORM_PCM_U8: return 1;
    case WAVEFORM_PCM_S16LE: return 2;
    case WAVEFORM_PCM_S24LE: return 3;
    case WAVEFORM_PCM_S32LE: return 4;
    case WAVEFORM_PCM_FLOAT32LE: return 4;
    case WAVEFORM_PCM_INVALID:
    default: return 0;
  }
}

static inline uint16_t waveform_peak_from_pcm_chunk(const unsigned char *buffer,
                                                    size_t bytes,
                                                    unsigned int channels,
                                                    WaveformPcmFormat format) {
  const unsigned int bytes_per_sample = waveform_pcm_bytes_per_sample(format);
  double peak = 0.0;

  if (!buffer || bytes == 0 || channels == 0 || bytes_per_sample == 0) {
    return 0;
  }

  for (size_t i = 0; i + (size_t)(channels * bytes_per_sample) <= bytes; i += (size_t)(channels * bytes_per_sample)) {
    for (unsigned int c = 0; c < channels; ++c) {
      const size_t offset = i + (size_t)c * bytes_per_sample;
      double sample = 0.0;

      if (format == WAVEFORM_PCM_FLOAT32LE) {
        float value = 0.0f;
        memcpy(&value, buffer + offset, sizeof(value));
        sample = value;
      } else if (format == WAVEFORM_PCM_S32LE) {
        int32_t value = 0;
        memcpy(&value, buffer + offset, sizeof(value));
        sample = (double)value / 2147483648.0;
      } else if (format == WAVEFORM_PCM_S24LE) {
        const unsigned char *frame = buffer + offset;
        int32_t value = ((int32_t)frame[2] << 16) | ((int32_t)frame[1] << 8) | (int32_t)frame[0];
        if (value & 0x00800000) value |= ~0x00FFFFFF;
        sample = (double)value / 8388608.0;
      } else if (format == WAVEFORM_PCM_S16LE) {
        int16_t value = 0;
        memcpy(&value, buffer + offset, sizeof(value));
        sample = (double)value / 32768.0;
      } else if (format == WAVEFORM_PCM_U8) {
        const unsigned char *frame = buffer + offset;
        sample = ((double)frame[0] - 128.0) / 128.0;
      } else {
        return 0;
      }

      if (sample < 0.0) {
        sample = -sample;
      }
      if (sample > peak) {
        peak = sample;
      }
    }
  }

  if (peak < 0.0) {
    peak = 0.0;
  }
  if (peak > 1.0) {
    peak = 1.0;
  }
  return (uint16_t)(peak * 32768.0);
}

#endif
