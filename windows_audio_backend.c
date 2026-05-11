#include "windows_audio_backend.h"
#include "core.h"
#include "playback_renderer.h"
#include "windows_debug_log.h"
#include "waveform_peaks.h"

#ifdef _WIN32

#ifndef COBJMACROS
#define COBJMACROS
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include "platform_windows.h"
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <objbase.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void windows_audio_debug(const char *message) {
  if (!message) {
    return;
  }
  windows_debug_log(message);
}

static void windows_audio_debug_format(const char *prefix, const WAVEFORMATEX *format, size_t snapshot_len) {
  char buffer[256];

  if (!prefix) {
    prefix = "format";
  }

  if (!format) {
    wsprintfA(buffer, "%s: format=null snapshot=%lu", prefix, (unsigned long)snapshot_len);
  } else {
    wsprintfA(buffer,
              "%s: snapshot=%lu rate=%lu block=%u avg=%lu channels=%u bits=%u tag=%u float=%d",
              prefix,
              (unsigned long)snapshot_len,
              (unsigned long)format->nSamplesPerSec,
              (unsigned int)format->nBlockAlign,
              (unsigned long)format->nAvgBytesPerSec,
              (unsigned int)format->nChannels,
              (unsigned int)format->wBitsPerSample,
              (unsigned int)format->wFormatTag,
              windows_audio_format_is_float_impl(format));
  }

  windows_audio_debug(buffer);
}

static const GUID kCLSID_MMDeviceEnumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
static const GUID kIID_IMMDeviceEnumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
static const GUID kIID_IAudioClient = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
static const GUID kIID_IAudioRenderClient = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
static const GUID kIID_IAudioCaptureClient = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
static const GUID kKSDATAFORMAT_SUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

typedef struct {
  CRITICAL_SECTION lock;
  HANDLE capture_stop_event;
  HANDLE playback_stop_event;
  HANDLE capture_thread;
  HANDLE playback_thread;
  BOOL capture_active;
  BOOL playback_active;
  BOOL playback_buffer_ready;
  BOOL loop_enabled;
  BOOL loop_region_set;
  gdouble loop_start_frames;
  gdouble loop_end_frames;
  size_t playback_cursor_bytes;
  size_t playback_queued_cursor_bytes;
  size_t playback_anchor_cursor_bytes;
  size_t playback_total_bytes;
  size_t playback_avg_bytes_per_sec;
  size_t playback_block_align;
  ULONGLONG playback_anchor_ms;
  gdouble playback_prepared_speed;
  gdouble playback_rendered_to_source_ratio;
  gdouble render_progress;
  BOOL render_active;
  unsigned int render_generation;
  size_t playback_pcm_len;
  size_t playback_pcm_cap;
  size_t playback_source_offset_bytes;
  BYTE *playback_pcm;
  BYTE *pcm;
  size_t pcm_len;
  size_t pcm_cap;
  uint16_t *wave_peaks;
  size_t wave_peak_len;
  size_t wave_peak_cap;
  uint64_t captured_frames;
  WAVEFORMATEX *wave_format;
} WindowsAudioBackendState;

static WindowsAudioBackendState *windows_audio_get_state(PlatformWindowsContext *context, BOOL create) {
  WindowsAudioBackendState *state = NULL;

  if (!context) {
    return NULL;
  }

  state = (WindowsAudioBackendState *)context->audio_state;
  if (!state && create) {
    state = (WindowsAudioBackendState *)calloc(1, sizeof(*state));
    if (!state) {
      return NULL;
    }
    InitializeCriticalSection(&state->lock);
    state->capture_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    state->playback_stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    state->playback_prepared_speed = 1.0;
    state->playback_rendered_to_source_ratio = 1.0;
    if (!state->capture_stop_event || !state->playback_stop_event) {
      if (state->capture_stop_event) {
        CloseHandle(state->capture_stop_event);
      }
      if (state->playback_stop_event) {
        CloseHandle(state->playback_stop_event);
      }
      DeleteCriticalSection(&state->lock);
      free(state);
      windows_audio_debug("failed to create stop event");
      return NULL;
    }
    context->audio_state = state;
  }

  return state;
}

static void windows_audio_free_format(WAVEFORMATEX *format) {
  if (format) {
    CoTaskMemFree(format);
  }
}

static int windows_audio_format_is_float_impl(const WAVEFORMATEX *format) {
  if (!format) {
    return 0;
  }

  if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
    return 1;
  }

  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)format;
    return IsEqualGUID(&ext->SubFormat, &kKSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
  }

  (void)kKSDATAFORMAT_SUBTYPE_PCM;
  return 0;
}

static WaveformPcmFormat windows_audio_waveform_format(const WAVEFORMATEX *format) {
  if (!format) {
    return 0;
  }

  if (windows_audio_format_is_float_impl(format)) {
    return WAVEFORM_PCM_FLOAT32LE;
  }

  if (format->wFormatTag == WAVE_FORMAT_PCM) {
    switch (format->wBitsPerSample) {
      case 8: return WAVEFORM_PCM_U8;
      case 16: return WAVEFORM_PCM_S16LE;
      case 24: return WAVEFORM_PCM_S24LE;
      case 32: return WAVEFORM_PCM_S32LE;
      default: return 0;
    }
  }

  if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
    const WAVEFORMATEXTENSIBLE *ext = (const WAVEFORMATEXTENSIBLE *)format;
    if (IsEqualGUID(&ext->SubFormat, &kKSDATAFORMAT_SUBTYPE_PCM)) {
      switch (format->wBitsPerSample) {
        case 8: return WAVEFORM_PCM_U8;
        case 16: return WAVEFORM_PCM_S16LE;
        case 24: return WAVEFORM_PCM_S24LE;
        case 32: return WAVEFORM_PCM_S32LE;
        default: return 0;
      }
    }
  }

  return 0;
}

static double windows_audio_read_sample_as_double(const BYTE *frame, unsigned int channel, const WAVEFORMATEX *format) {
  const unsigned int channels = format && format->nChannels > 0 ? format->nChannels : 1;
  const WaveformPcmFormat pcm_format = windows_audio_waveform_format(format);
  const BYTE *sample = frame + (size_t)channel * waveform_pcm_bytes_per_sample(pcm_format);

  (void)channels;

  switch (pcm_format) {
    case WAVEFORM_PCM_U8:
      return ((double)sample[0] - 128.0) / 128.0;
    case WAVEFORM_PCM_S16LE: {
      int16_t value;
      memcpy(&value, sample, sizeof(value));
      return (double)value / 32768.0;
    }
    case WAVEFORM_PCM_S24LE: {
      int32_t value = (int32_t)((uint32_t)sample[0] | ((uint32_t)sample[1] << 8) | ((uint32_t)sample[2] << 16));
      if (value & 0x00800000) {
        value |= (int32_t)0xff000000;
      }
      return (double)value / 8388608.0;
    }
    case WAVEFORM_PCM_S32LE: {
      int32_t value;
      memcpy(&value, sample, sizeof(value));
      return (double)value / 2147483648.0;
    }
    case WAVEFORM_PCM_FLOAT32LE: {
      float value;
      memcpy(&value, sample, sizeof(value));
      return (double)value;
    }
    default:
      return 0.0;
  }
}

static int16_t windows_audio_double_to_s16(double sample) {
  long value;
  if (sample > 1.0) {
    sample = 1.0;
  } else if (sample < -1.0) {
    sample = -1.0;
  }
  value = lround(sample * 32767.0);
  if (value > 32767) {
    value = 32767;
  } else if (value < -32768) {
    value = -32768;
  }
  return (int16_t)value;
}

static void windows_audio_write_sample_from_double(BYTE *frame, unsigned int channel, const WAVEFORMATEX *format, double sample_value) {
  const WaveformPcmFormat pcm_format = windows_audio_waveform_format(format);
  BYTE *sample = frame + (size_t)channel * waveform_pcm_bytes_per_sample(pcm_format);

  if (sample_value > 1.0) {
    sample_value = 1.0;
  } else if (sample_value < -1.0) {
    sample_value = -1.0;
  }

  switch (pcm_format) {
    case WAVEFORM_PCM_U8:
      sample[0] = (BYTE)lround((sample_value * 127.0) + 128.0);
      break;
    case WAVEFORM_PCM_S16LE: {
      int16_t value = windows_audio_double_to_s16(sample_value);
      memcpy(sample, &value, sizeof(value));
      break;
    }
    case WAVEFORM_PCM_S24LE: {
      int32_t value = (int32_t)lround(sample_value * 8388607.0);
      sample[0] = (BYTE)(value & 0xff);
      sample[1] = (BYTE)((value >> 8) & 0xff);
      sample[2] = (BYTE)((value >> 16) & 0xff);
      break;
    }
    case WAVEFORM_PCM_S32LE: {
      int32_t value = (int32_t)lround(sample_value * 2147483647.0);
      memcpy(sample, &value, sizeof(value));
      break;
    }
    case WAVEFORM_PCM_FLOAT32LE: {
      float value = (float)sample_value;
      memcpy(sample, &value, sizeof(value));
      break;
    }
    default:
      break;
  }
}

typedef struct {
  WindowsAudioBackendState *state;
  unsigned int generation;
} WindowsRenderProgressContext;

static void windows_audio_render_progress_cb(double progress, void *user_data) {
  WindowsRenderProgressContext *context = (WindowsRenderProgressContext *)user_data;

  if (!context || !context->state) {
    return;
  }
  if (progress < 0.0) {
    progress = 0.0;
  } else if (progress > 1.0) {
    progress = 1.0;
  }

  EnterCriticalSection(&context->state->lock);
  if (context->state->render_generation == context->generation) {
    context->state->render_progress = progress;
    context->state->render_active = TRUE;
  }
  LeaveCriticalSection(&context->state->lock);
}

static BYTE *windows_audio_convert_format_to_s16(const BYTE *pcm, size_t pcm_len, const WAVEFORMATEX *format, size_t *out_len) {
  const WaveformPcmFormat pcm_format = windows_audio_waveform_format(format);
  const unsigned int channels = format ? format->nChannels : 0;
  const size_t source_frame_bytes = format ? format->nBlockAlign : 0;
  size_t frames;
  BYTE *out;

  if (out_len) {
    *out_len = 0;
  }
  if (!pcm || !format || !out_len || pcm_format == WAVEFORM_PCM_INVALID || channels == 0 || source_frame_bytes == 0) {
    return NULL;
  }

  frames = pcm_len / source_frame_bytes;
  if (frames > SIZE_MAX / ((size_t)channels * sizeof(int16_t))) {
    return NULL;
  }
  out = (BYTE *)malloc(frames * (size_t)channels * sizeof(int16_t));
  if (!out) {
    return NULL;
  }

  for (size_t i = 0; i < frames; ++i) {
    const BYTE *frame = pcm + (i * source_frame_bytes);
    int16_t *dst = (int16_t *)out + (i * channels);
    for (unsigned int c = 0; c < channels; ++c) {
      dst[c] = windows_audio_double_to_s16(windows_audio_read_sample_as_double(frame, c, format));
    }
  }

  *out_len = frames * (size_t)channels * sizeof(int16_t);
  return out;
}

static BYTE *windows_audio_convert_s16_to_format(const BYTE *pcm, size_t pcm_len, unsigned int channels, const WAVEFORMATEX *format, size_t *out_len) {
  const WaveformPcmFormat pcm_format = windows_audio_waveform_format(format);
  const size_t dest_frame_bytes = format ? format->nBlockAlign : 0;
  size_t frames;
  BYTE *out;

  if (out_len) {
    *out_len = 0;
  }
  if (!pcm || !format || !out_len || pcm_format == WAVEFORM_PCM_INVALID || channels == 0 || dest_frame_bytes == 0) {
    return NULL;
  }

  frames = pcm_len / ((size_t)channels * sizeof(int16_t));
  if (frames > SIZE_MAX / dest_frame_bytes) {
    return NULL;
  }
  out = (BYTE *)calloc(frames, dest_frame_bytes);
  if (!out) {
    return NULL;
  }

  for (size_t i = 0; i < frames; ++i) {
    BYTE *frame = out + (i * dest_frame_bytes);
    const int16_t *src = (const int16_t *)pcm + (i * channels);
    for (unsigned int c = 0; c < channels; ++c) {
      windows_audio_write_sample_from_double(frame, c, format, (double)src[c] / 32768.0);
    }
  }

  *out_len = frames * dest_frame_bytes;
  return out;
}

static void windows_audio_reset_buffer_locked(WindowsAudioBackendState *state) {
  if (!state) {
    return;
  }

  state->pcm_len = 0;
  state->wave_peak_len = 0;
  state->captured_frames = 0;
  state->playback_cursor_bytes = 0;
  state->playback_queued_cursor_bytes = 0;
  state->playback_anchor_cursor_bytes = 0;
  state->playback_total_bytes = 0;
  state->playback_avg_bytes_per_sec = 0;
  state->playback_block_align = 0;
  state->playback_anchor_ms = 0;
  state->playback_pcm_len = 0;
  state->playback_source_offset_bytes = 0;
  state->playback_buffer_ready = FALSE;
  state->playback_prepared_speed = 1.0;
  if (state->playback_pcm) {
    free(state->playback_pcm);
    state->playback_pcm = NULL;
    state->playback_pcm_cap = 0;
  }
}

static const BYTE *windows_audio_source_pcm_data(PlatformWindowsContext *context, WindowsAudioBackendState *state) {
  if (context && context->recorder_core && recorder_core_pcm_len(context->recorder_core) > 0) {
    return recorder_core_pcm_data(context->recorder_core);
  }
  return state ? state->pcm : NULL;
}

static size_t windows_audio_source_pcm_len(PlatformWindowsContext *context, WindowsAudioBackendState *state) {
  if (context && context->recorder_core && recorder_core_pcm_len(context->recorder_core) > 0) {
    return recorder_core_pcm_len(context->recorder_core);
  }
  return state ? state->pcm_len : 0;
}

static uint64_t windows_audio_source_captured_frames(PlatformWindowsContext *context, WindowsAudioBackendState *state) {
  if (context && context->recorder_core && context->recorder_core->audio.captured_frames > 0) {
    return context->recorder_core->audio.captured_frames;
  }
  return state ? state->captured_frames : 0;
}

static BOOL windows_audio_prepare_playback_buffer_locked(PlatformWindowsContext *context, WindowsAudioBackendState *state, gdouble speed) {
  BYTE *new_pcm;
  const BYTE *source_pcm;
  size_t source_len;
  if (!state) {
    return FALSE;
  }

  source_pcm = windows_audio_source_pcm_data(context, state);
  source_len = windows_audio_source_pcm_len(context, state);

  if (!source_pcm || source_len == 0) {
    state->playback_source_offset_bytes = 0;
    state->playback_pcm_len = 0;
    state->playback_buffer_ready = FALSE;
    return FALSE;
  }

  state->playback_rendered_to_source_ratio = 1.0;

  if (source_len > state->playback_pcm_cap) {
    new_pcm = (BYTE *)realloc(state->playback_pcm, source_len);
    if (!new_pcm) {
      return FALSE;
    }
    state->playback_pcm = new_pcm;
    state->playback_pcm_cap = source_len;
  }

  memcpy(state->playback_pcm, source_pcm, source_len);
  state->playback_source_offset_bytes = 0;
  state->playback_pcm_len = source_len;
  state->playback_prepared_speed = speed > 0.0 ? speed : 1.0;
  state->playback_buffer_ready = TRUE;
  return TRUE;
}

static BOOL windows_audio_install_prepared_buffer_locked(WindowsAudioBackendState *state,
                                                        const BYTE *pcm,
                                                        size_t pcm_len,
                                                        gdouble rendered_to_source_ratio,
                                                        gdouble speed) {
  BYTE *new_pcm;

  if (!state || !pcm || pcm_len == 0) {
    return FALSE;
  }

  if (pcm_len > state->playback_pcm_cap) {
    new_pcm = (BYTE *)realloc(state->playback_pcm, pcm_len);
    if (!new_pcm) {
      return FALSE;
    }
    state->playback_pcm = new_pcm;
    state->playback_pcm_cap = pcm_len;
  }

  memcpy(state->playback_pcm, pcm, pcm_len);
  state->playback_source_offset_bytes = 0;
  state->playback_pcm_len = pcm_len;
  state->playback_rendered_to_source_ratio = rendered_to_source_ratio > 0.0 ? rendered_to_source_ratio : 1.0;
  state->playback_prepared_speed = speed > 0.0 ? speed : 1.0;
  state->playback_buffer_ready = TRUE;
  return TRUE;
}

static size_t windows_audio_frame_align_bytes(size_t bytes, size_t block_align) {
  if (block_align == 0) {
    return bytes;
  }
  return bytes - (bytes % block_align);
}

static size_t windows_audio_audible_cursor_locked(const WindowsAudioBackendState *state) {
  size_t cursor;
  ULONGLONG elapsed_ms;
  size_t elapsed_bytes;

  if (!state) {
    return 0;
  }

  cursor = state->playback_cursor_bytes;
  if (!state->playback_active || state->playback_avg_bytes_per_sec == 0 || state->playback_anchor_ms == 0) {
    return cursor;
  }

  elapsed_ms = GetTickCount64() - state->playback_anchor_ms;
  elapsed_bytes = (size_t)(((unsigned long long)state->playback_avg_bytes_per_sec * elapsed_ms) / 1000ULL);
  elapsed_bytes = windows_audio_frame_align_bytes(elapsed_bytes, state->playback_block_align);
  cursor = state->playback_anchor_cursor_bytes + elapsed_bytes;

  if (cursor > state->playback_queued_cursor_bytes) {
    cursor = state->playback_queued_cursor_bytes;
  }
  if (state->playback_total_bytes > 0 && cursor > state->playback_total_bytes) {
    cursor = state->playback_total_bytes;
  }
  return windows_audio_frame_align_bytes(cursor, state->playback_block_align);
}

static size_t windows_audio_source_bytes_to_output_bytes(size_t source_bytes, size_t block_align, gdouble rendered_to_source_ratio) {
  size_t source_frames;
  size_t output_frames;

  if (block_align == 0) {
    return source_bytes;
  }
  if (rendered_to_source_ratio <= 0.0) {
    rendered_to_source_ratio = 1.0;
  }
  source_frames = source_bytes / block_align;
  output_frames = (size_t)((gdouble)source_frames / rendered_to_source_ratio);
  if (output_frames > SIZE_MAX / block_align) {
    return SIZE_MAX - (SIZE_MAX % block_align);
  }
  return output_frames * block_align;
}

static size_t windows_audio_output_bytes_to_source_bytes(size_t output_bytes, size_t block_align, gdouble rendered_to_source_ratio, size_t source_total_bytes) {
  size_t output_frames;
  size_t source_frames;
  size_t source_bytes;

  if (block_align == 0) {
    return output_bytes;
  }
  if (rendered_to_source_ratio <= 0.0) {
    rendered_to_source_ratio = 1.0;
  }
  output_frames = output_bytes / block_align;
  source_frames = (size_t)((gdouble)output_frames * rendered_to_source_ratio);
  if (source_frames > SIZE_MAX / block_align) {
    source_bytes = source_total_bytes;
  } else {
    source_bytes = source_frames * block_align;
  }
  if (source_bytes > source_total_bytes) {
    source_bytes = source_total_bytes;
  }
  return windows_audio_frame_align_bytes(source_bytes, block_align);
}

static BOOL windows_audio_append_peak_locked(WindowsAudioBackendState *state, uint16_t peak) {
  size_t needed;
  uint16_t *new_peaks;

  if (!state) {
    return FALSE;
  }

  needed = state->wave_peak_len + 1;
  if (needed > state->wave_peak_cap) {
    size_t new_cap = state->wave_peak_cap ? state->wave_peak_cap : 1024;
    while (new_cap < needed) {
      new_cap *= 2;
    }
    new_peaks = (uint16_t *)realloc(state->wave_peaks, new_cap * sizeof(*new_peaks));
    if (!new_peaks) {
      return FALSE;
    }
    state->wave_peaks = new_peaks;
    state->wave_peak_cap = new_cap;
  }

  state->wave_peaks[state->wave_peak_len++] = peak;
  return TRUE;
}

static void windows_audio_append_peaks_for_buffer_locked(WindowsAudioBackendState *state,
                                                         const BYTE *data,
                                                         size_t bytes,
                                                         const WAVEFORMATEX *format) {
  const unsigned int channels = (format && format->nChannels > 0) ? format->nChannels : 2;
  const WaveformPcmFormat waveform_format = windows_audio_waveform_format(format);
  const unsigned int bytes_per_sample = waveform_pcm_bytes_per_sample(waveform_format);
  const size_t frame_size = (size_t)channels * (size_t)bytes_per_sample;
  const size_t chunk_frames = 256;
  const size_t chunk_bytes = chunk_frames * frame_size;

  if (!state || !data || bytes == 0 || frame_size == 0 || waveform_format == WAVEFORM_PCM_INVALID) {
    return;
  }

  for (size_t offset = 0; offset < bytes; offset += chunk_bytes) {
    const size_t remaining = bytes - offset;
    const size_t this_bytes = remaining < chunk_bytes ? remaining : chunk_bytes;
    const uint16_t peak = waveform_peak_from_pcm_chunk(data + offset, this_bytes, channels, waveform_format);
    if (!windows_audio_append_peak_locked(state, peak)) {
      return;
    }
  }
}

static BOOL windows_audio_append_locked(WindowsAudioBackendState *state, const BYTE *data, size_t bytes) {
  size_t needed;
  BYTE *new_pcm;

  if (!state || !data || bytes == 0) {
    return TRUE;
  }

  needed = state->pcm_len + bytes;
  if (needed > state->pcm_cap) {
    size_t new_cap = state->pcm_cap ? state->pcm_cap : 65536;
    while (new_cap < needed) {
      new_cap *= 2;
    }
    new_pcm = (BYTE *)realloc(state->pcm, new_cap);
    if (!new_pcm) {
      return FALSE;
    }
    state->pcm = new_pcm;
    state->pcm_cap = new_cap;
  }

  memcpy(state->pcm + state->pcm_len, data, bytes);
  state->pcm_len += bytes;
  return TRUE;
}

static HRESULT windows_audio_get_mix_format(IAudioClient *client, WAVEFORMATEX **format) {
  if (!client || !format) {
    return E_POINTER;
  }
  *format = NULL;
  return IAudioClient_GetMixFormat(client, format);
}

static HRESULT windows_audio_init_device_client(EDataFlow flow, IAudioClient **out_client, WAVEFORMATEX **out_format) {
  HRESULT hr;
  IMMDeviceEnumerator *enumerator = NULL;
  IMMDevice *device = NULL;
  IAudioClient *client = NULL;

  if (!out_client || !out_format) {
    return E_POINTER;
  }

  *out_client = NULL;
  *out_format = NULL;

  hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &kIID_IMMDeviceEnumerator, (void **)&enumerator);
  if (FAILED(hr)) {
    return hr;
  }

  hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator, flow, eConsole, &device);
  if (SUCCEEDED(hr)) {
    hr = IMMDevice_Activate(device, &kIID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
  }

  if (SUCCEEDED(hr)) {
    hr = windows_audio_get_mix_format(client, out_format);
  }

  if (SUCCEEDED(hr)) {
    *out_client = client;
    client = NULL;
  }

  if (device) {
    IMMDevice_Release(device);
  }
  if (enumerator) {
    IMMDeviceEnumerator_Release(enumerator);
  }
  if (client) {
    IAudioClient_Release(client);
  }

  return hr;
}

static DWORD WINAPI windows_capture_thread(LPVOID param) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)param;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  IAudioClient *client = NULL;
  IAudioCaptureClient *capture_client = NULL;
  WAVEFORMATEX *format = NULL;
  WAVEFORMATEX *capture_format = NULL;
  UINT32 bytes_per_frame = 0;
  HANDLE mm_task = NULL;
  HRESULT hr;

  if (!state) {
    return 1;
  }

  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  hr = windows_audio_init_device_client(eRender, &client, &format);
  if (FAILED(hr) || !client || !format) {
    goto cleanup;
  }

  hr = IAudioClient_Initialize(client,
                               AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK,
                               10000000,
                               0,
                               format,
                               NULL);
  if (FAILED(hr)) {
    goto cleanup;
  }

  bytes_per_frame = format->nBlockAlign;

  hr = IAudioClient_GetService(client, &kIID_IAudioCaptureClient, (void **)&capture_client);
  if (FAILED(hr)) {
    goto cleanup;
  }

  hr = IAudioClient_Start(client);
  if (FAILED(hr)) {
    goto cleanup;
  }

  EnterCriticalSection(&state->lock);
  windows_audio_reset_buffer_locked(state);
  if (context->recorder_core) {
    recorder_core_reset_session(context->recorder_core, (gint64)GetTickCount64() * 1000);
  }
  if (state->wave_format) {
    windows_audio_free_format(state->wave_format);
  }
  state->wave_format = format;
  capture_format = state->wave_format;
  format = NULL;
  state->capture_active = TRUE;
  LeaveCriticalSection(&state->lock);

  while (WaitForSingleObject(state->capture_stop_event, 10) == WAIT_TIMEOUT) {
    UINT32 packet_length = 0;

    if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture_client, &packet_length))) {
      break;
    }
    while (packet_length > 0) {
      BYTE *data = NULL;
      UINT32 frames = 0;
      DWORD flags = 0;
      UINT32 bytes = 0;

      hr = IAudioCaptureClient_GetBuffer(capture_client, &data, &frames, &flags, NULL, NULL);
      if (FAILED(hr)) {
        goto cleanup;
      }

      bytes = frames * bytes_per_frame;
      EnterCriticalSection(&state->lock);
      if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
        BYTE *silence = (BYTE *)calloc(1, bytes);
        if (silence) {
          if ((context->recorder_core && recorder_core_append_pcm(context->recorder_core, silence, bytes, frames)) ||
              (!context->recorder_core && windows_audio_append_locked(state, silence, bytes))) {
            windows_audio_append_peaks_for_buffer_locked(state, silence, bytes, capture_format);
            state->captured_frames += frames;
            if (context->recorder_core) {
              recorder_core_set_sample_rate(context->recorder_core, capture_format->nSamplesPerSec);
              context->recorder_core->audio.channels = capture_format->nChannels;
            }
          }
          free(silence);
        }
      } else {
        if ((context->recorder_core && recorder_core_append_pcm(context->recorder_core, data, bytes, frames)) ||
            (!context->recorder_core && windows_audio_append_locked(state, data, bytes))) {
          windows_audio_append_peaks_for_buffer_locked(state, data, bytes, capture_format);
          state->captured_frames += frames;
          if (context->recorder_core) {
            recorder_core_set_sample_rate(context->recorder_core, capture_format->nSamplesPerSec);
            context->recorder_core->audio.channels = capture_format->nChannels;
          }
        }
      }
      LeaveCriticalSection(&state->lock);

      hr = IAudioCaptureClient_ReleaseBuffer(capture_client, frames);
      if (FAILED(hr)) {
        goto cleanup;
      }

      if (FAILED(IAudioCaptureClient_GetNextPacketSize(capture_client, &packet_length))) {
        goto cleanup;
      }
    }
  }

cleanup:
  if (client) {
    IAudioClient_Stop(client);
  }
  (void)mm_task;
  if (capture_client) {
    IAudioCaptureClient_Release(capture_client);
  }
  if (client) {
    IAudioClient_Release(client);
  }
  if (format) {
    windows_audio_free_format(format);
  }
  EnterCriticalSection(&state->lock);
  state->capture_active = FALSE;
  LeaveCriticalSection(&state->lock);
  CoUninitialize();
  return 0;
}

static DWORD WINAPI windows_playback_thread(LPVOID param) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)param;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  IAudioClient *client = NULL;
  IAudioRenderClient *render_client = NULL;
  WAVEFORMATEX *source_format = NULL;
  WAVEFORMATEX *render_format = NULL;
  BYTE *snapshot = NULL;
  size_t snapshot_len = 0;
  size_t source_total_bytes = 0;
  size_t playback_base_offset = 0;
  size_t loop_start_rel = 0;
  size_t loop_end_rel = 0;
  size_t cursor = 0;
  gdouble rendered_to_source_ratio = 1.0;
  BOOL loop_valid = FALSE;
  HRESULT hr;

  if (!state) {
    return 1;
  }

  windows_audio_debug("playback thread start");

  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  EnterCriticalSection(&state->lock);
  if (state->playback_buffer_ready && state->playback_pcm_len > 0 && state->playback_pcm) {
    source_total_bytes = windows_audio_source_pcm_len(context, state);
    if (state->playback_pcm_len > 0) {
      snapshot = (BYTE *)malloc(state->playback_pcm_len);
      if (snapshot) {
        memcpy(snapshot, state->playback_pcm, state->playback_pcm_len);
        snapshot_len = state->playback_pcm_len;
        playback_base_offset = state->playback_source_offset_bytes;
        rendered_to_source_ratio = state->playback_rendered_to_source_ratio > 0.0 ? state->playback_rendered_to_source_ratio : 1.0;
      }
    } else {
      snapshot_len = 0;
      playback_base_offset = state->playback_source_offset_bytes;
    }
  }
  source_format = state->wave_format;
  if (source_format) {
    source_format = (WAVEFORMATEX *)malloc(sizeof(WAVEFORMATEX) + source_format->cbSize);
    if (source_format) {
      memcpy(source_format, state->wave_format, sizeof(WAVEFORMATEX) + state->wave_format->cbSize);
    }
  }
  LeaveCriticalSection(&state->lock);

  if (!snapshot || !source_format) {
    goto cleanup;
  }

  windows_audio_debug_format("playback source", source_format, snapshot_len);
  {
    const unsigned long source_ms = source_format->nAvgBytesPerSec > 0 ? (unsigned long)((1000.0 * (double)snapshot_len / (double)source_format->nAvgBytesPerSec) + 0.5) : 0UL;
    char duration_message[160];
    wsprintfA(duration_message, "playback source duration=%lu ms snapshot_bytes=%lu", source_ms, (unsigned long)snapshot_len);
    windows_audio_debug(duration_message);
  }

  render_format = NULL;
  hr = windows_audio_init_device_client(eRender, &client, &render_format);
  if (FAILED(hr) || !client || !render_format) {
    goto cleanup;
  }

  windows_audio_debug_format("playback render", render_format, snapshot_len);
  {
    const unsigned long render_ms = render_format->nAvgBytesPerSec > 0 ? (unsigned long)((1000.0 * (double)snapshot_len / (double)render_format->nAvgBytesPerSec) + 0.5) : 0UL;
    char duration_message[160];
    wsprintfA(duration_message, "playback render duration=%lu ms snapshot_bytes=%lu", render_ms, (unsigned long)snapshot_len);
    windows_audio_debug(duration_message);
  }

  hr = IAudioClient_Initialize(client,
                               AUDCLNT_SHAREMODE_SHARED,
                               0,
                               10000000,
                               0,
                               render_format,
                               NULL);
  if (FAILED(hr)) {
    goto cleanup;
  }

  hr = IAudioClient_GetService(client, &kIID_IAudioRenderClient, (void **)&render_client);
  if (FAILED(hr)) {
    goto cleanup;
  }

  hr = IAudioClient_Start(client);
  if (FAILED(hr)) {
    goto cleanup;
  }

  EnterCriticalSection(&state->lock);
  state->playback_active = TRUE;
  state->playback_total_bytes = source_total_bytes;
  cursor = windows_audio_source_bytes_to_output_bytes(state->playback_cursor_bytes, source_format->nBlockAlign, rendered_to_source_ratio);
  if (cursor > snapshot_len) {
    cursor = snapshot_len;
    state->playback_cursor_bytes = source_total_bytes;
  }
  state->playback_queued_cursor_bytes = windows_audio_output_bytes_to_source_bytes(cursor, source_format->nBlockAlign, rendered_to_source_ratio, source_total_bytes);
  state->playback_anchor_cursor_bytes = state->playback_cursor_bytes;
  state->playback_avg_bytes_per_sec = (size_t)((gdouble)source_format->nAvgBytesPerSec * rendered_to_source_ratio);
  state->playback_block_align = source_format->nBlockAlign;
  state->playback_anchor_ms = GetTickCount64();
  LeaveCriticalSection(&state->lock);

  {
    UINT32 buffer_frames = 0;
    UINT32 bytes_per_frame = render_format->nBlockAlign;

    if (FAILED(IAudioClient_GetBufferSize(client, &buffer_frames))) {
      goto cleanup;
    }

  while (WaitForSingleObject(state->playback_stop_event, 10) == WAIT_TIMEOUT) {
      UINT32 padding = 0;
      UINT32 available = 0;
      BYTE *render_data = NULL;
      BOOL wrapped = FALSE;

      loop_valid = FALSE;
      loop_start_rel = 0;
      loop_end_rel = 0;
      if (source_format->nBlockAlign > 0) {
        const gdouble source_total_frames = (gdouble)source_total_bytes / (gdouble)source_format->nBlockAlign;
        LoopState loop = {0};
        gdouble loop_start_frames = 0.0;
        gdouble loop_end_frames = 0.0;

        EnterCriticalSection(&state->lock);
        loop.enabled = state->loop_enabled;
        loop.region_set = state->loop_region_set;
        loop.start_frames = state->loop_start_frames;
        loop.end_frames = state->loop_end_frames;
        LeaveCriticalSection(&state->lock);

        if (loop.enabled && core_get_effective_loop_region(&loop, source_total_frames, &loop_start_frames, &loop_end_frames)) {
          const size_t bytes_per_frame = (size_t)source_format->nBlockAlign;
          const size_t loop_start_bytes = (size_t)loop_start_frames * bytes_per_frame;
          size_t loop_end_bytes = (size_t)loop_end_frames * bytes_per_frame;

          if (loop_end_bytes > source_total_bytes) {
            loop_end_bytes = source_total_bytes;
          }

          if (loop_start_bytes < source_total_bytes && loop_end_bytes > loop_start_bytes && loop_start_bytes >= playback_base_offset) {
            loop_start_rel = windows_audio_source_bytes_to_output_bytes(loop_start_bytes - playback_base_offset, source_format->nBlockAlign, rendered_to_source_ratio);
            loop_end_rel = windows_audio_source_bytes_to_output_bytes(loop_end_bytes - playback_base_offset, source_format->nBlockAlign, rendered_to_source_ratio);
            if (loop_end_rel > snapshot_len) {
              loop_end_rel = snapshot_len;
            }
            loop_valid = loop_end_rel > loop_start_rel;
          }
        }
      }

      if (FAILED(IAudioClient_GetCurrentPadding(client, &padding))) {
        break;
      }
      available = buffer_frames - padding;
      if (available == 0) {
        continue;
      }

      if (!loop_valid) {
        const size_t remaining_bytes = (cursor < snapshot_len) ? (snapshot_len - cursor) : 0;
        UINT32 remaining_frames = bytes_per_frame > 0 ? (UINT32)(remaining_bytes / bytes_per_frame) : 0;
        if (remaining_frames == 0) {
          cursor = snapshot_len;
          break;
        }
        if (available > remaining_frames) {
          available = remaining_frames;
        }
      }

      if (FAILED(IAudioRenderClient_GetBuffer(render_client, available, &render_data))) {
        break;
      }

      {
        size_t bytes = (size_t)available * bytes_per_frame;
        size_t written = 0;

        while (written < bytes) {
          size_t segment_end = snapshot_len;
          size_t segment_remaining;
          size_t to_copy;

          if (loop_valid) {
            if (cursor >= loop_end_rel) {
              cursor = loop_start_rel;
            }
            segment_end = loop_end_rel;
          }

          if (cursor >= snapshot_len || cursor >= segment_end) {
            break;
          }

          segment_remaining = segment_end - cursor;
          to_copy = bytes - written;
          if (to_copy > segment_remaining) {
            to_copy = segment_remaining;
          }

          memcpy(render_data + written, snapshot + cursor, to_copy);
          cursor += to_copy;
          written += to_copy;

          if (loop_valid && cursor >= loop_end_rel) {
            cursor = loop_start_rel;
            wrapped = TRUE;
          }
        }

        if (written < bytes) {
          memset(render_data + written, 0, bytes - written);
          if (!loop_valid && cursor >= snapshot_len) {
            cursor = snapshot_len;
          }
        }
      }

      IAudioRenderClient_ReleaseBuffer(render_client, available, 0);
      EnterCriticalSection(&state->lock);
        state->playback_queued_cursor_bytes = windows_audio_output_bytes_to_source_bytes(cursor, source_format->nBlockAlign, rendered_to_source_ratio, source_total_bytes);
        if (wrapped) {
          state->playback_cursor_bytes = state->playback_queued_cursor_bytes;
          state->playback_anchor_cursor_bytes = state->playback_cursor_bytes;
          state->playback_anchor_ms = GetTickCount64();
        } else {
          state->playback_cursor_bytes = windows_audio_audible_cursor_locked(state);
        }
        state->playback_total_bytes = source_total_bytes;
        LeaveCriticalSection(&state->lock);
        if (!loop_valid && cursor >= snapshot_len) {
        {
          char end_message[160];
          wsprintfA(end_message, "playback exit cursor=%lu snapshot_bytes=%lu", (unsigned long)cursor, (unsigned long)snapshot_len);
          windows_audio_debug(end_message);
        }
        break;
      }
    }
  }

cleanup:
  if (client) {
    IAudioClient_Stop(client);
  }
  if (render_client) {
    IAudioRenderClient_Release(render_client);
  }
  if (client) {
    IAudioClient_Release(client);
  }
  free(snapshot);
  if (source_format) {
    free(source_format);
  }
  if (render_format) {
    free(render_format);
  }
  EnterCriticalSection(&state->lock);
  state->playback_queued_cursor_bytes = windows_audio_output_bytes_to_source_bytes(cursor, source_format ? source_format->nBlockAlign : 0, rendered_to_source_ratio, source_total_bytes);
  state->playback_cursor_bytes = windows_audio_audible_cursor_locked(state);
  if (!loop_valid && cursor >= snapshot_len) {
    state->playback_cursor_bytes = source_total_bytes;
  }
  state->playback_total_bytes = source_total_bytes;
  state->playback_active = FALSE;
  LeaveCriticalSection(&state->lock);

  CoUninitialize();
  return 0;
}

static void windows_audio_signal_stop(HANDLE event_handle) {
  if (event_handle) {
    SetEvent(event_handle);
  }
}

static gboolean windows_audio_join_thread(HANDLE *thread_handle) {
  if (!thread_handle || !*thread_handle) {
    return TRUE;
  }
  WaitForSingleObject(*thread_handle, INFINITE);
  CloseHandle(*thread_handle);
  *thread_handle = NULL;
  return TRUE;
}

static gboolean windows_start_capture(void *user_data, gboolean reset_buffers) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, TRUE);

  if (!state) {
    return FALSE;
  }

  EnterCriticalSection(&state->lock);
  if (reset_buffers) {
    windows_audio_reset_buffer_locked(state);
    free(state->wave_peaks);
    state->wave_peaks = NULL;
    state->wave_peak_cap = 0;
  }
  if (state->capture_active) {
    LeaveCriticalSection(&state->lock);
    return TRUE;
  }
  ResetEvent(state->capture_stop_event);
  state->capture_thread = CreateThread(NULL, 0, windows_capture_thread, context, 0, NULL);
  LeaveCriticalSection(&state->lock);
  return state->capture_thread != NULL;
}

static void windows_stop_capture(void *user_data, gboolean force_stopped) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  (void)force_stopped;

  if (!state) {
    return;
  }
  windows_audio_signal_stop(state->capture_stop_event);
  windows_audio_join_thread(&state->capture_thread);
}

static gboolean windows_start_playback(void *user_data) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, TRUE);

  if (!state) {
    return FALSE;
  }

  if (state->capture_active || state->capture_thread) {
    windows_stop_capture(user_data, TRUE);
  }

  EnterCriticalSection(&state->lock);
  if (state->playback_active ||
      !state->playback_buffer_ready ||
      !state->playback_pcm ||
      state->playback_pcm_len == 0 ||
      !state->wave_format ||
      !state->playback_buffer_ready) {
    LeaveCriticalSection(&state->lock);
    return FALSE;
  }
  ResetEvent(state->playback_stop_event);
  state->playback_thread = CreateThread(NULL, 0, windows_playback_thread, context, 0, NULL);
  LeaveCriticalSection(&state->lock);
  return state->playback_thread != NULL;
}

void windows_audio_backend_set_loop_state(void *user_data,
                                         gboolean enabled,
                                         gboolean region_set,
                                         gdouble start_frames,
                                         gdouble end_frames) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  if (!state) {
    return;
  }

  EnterCriticalSection(&state->lock);
  state->loop_enabled = enabled;
  state->loop_region_set = region_set;
  state->loop_start_frames = start_frames;
  state->loop_end_frames = end_frames;
  LeaveCriticalSection(&state->lock);
}

static void windows_stop_playback(void *user_data, gboolean reset_cursor) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  if (!state) {
    return;
  }

  windows_audio_debug("playback stop requested");
  windows_audio_signal_stop(state->playback_stop_event);
  windows_audio_join_thread(&state->playback_thread);

  EnterCriticalSection(&state->lock);
  if (reset_cursor) {
    state->playback_cursor_bytes = 0;
  }
  LeaveCriticalSection(&state->lock);
}

void windows_audio_backend_seek_playback_frames(void *user_data, gdouble cursor_frames) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  size_t cursor_bytes = 0;

  if (!state) {
    return;
  }

  EnterCriticalSection(&state->lock);
  if (!state->wave_format || state->wave_format->nBlockAlign == 0 || cursor_frames <= 0.0) {
    cursor_bytes = 0;
  } else {
    const double frames = cursor_frames < 0.0 ? 0.0 : cursor_frames;
    const size_t frame_index = (size_t)frames;
    const size_t source_len = windows_audio_source_pcm_len(context, state);
    if (frame_index > source_len / state->wave_format->nBlockAlign) {
      cursor_bytes = source_len;
    } else if (frame_index > ((size_t)-1) / state->wave_format->nBlockAlign) {
      cursor_bytes = 0;
    } else {
      cursor_bytes = frame_index * state->wave_format->nBlockAlign;
    }
  }
  if (cursor_bytes > windows_audio_source_pcm_len(context, state)) {
    cursor_bytes = windows_audio_source_pcm_len(context, state);
  }
  state->playback_cursor_bytes = cursor_bytes;
  LeaveCriticalSection(&state->lock);
}

int windows_audio_backend_prepare_playback_buffer_from_source(void *user_data,
                                                             const unsigned char *pcm,
                                                             size_t pcm_len,
                                                             const void *format,
                                                             uint64_t captured_frames,
                                                             gdouble speed) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  BYTE *pcm_snapshot = NULL;
  WAVEFORMATEX *format_snapshot = NULL;
  BYTE *source_s16 = NULL;
  BYTE *rendered_format = NULL;
  size_t source_s16_len = 0;
  size_t rendered_format_len = 0;
  gdouble rendered_to_source_ratio = 1.0;
  unsigned int generation = 0;
  PlaybackRenderResult render_result = {0};
  WindowsRenderProgressContext progress_context = {0};
  char render_error[256] = {0};
  int ready = 0;

  if (!state || !pcm || pcm_len == 0 || !format) {
    return 0;
  }
  if (speed <= 0.0) {
    speed = 1.0;
  }

  EnterCriticalSection(&state->lock);
  if (state->playback_cursor_bytes > pcm_len) {
    state->playback_cursor_bytes = pcm_len;
  }
  if (state->playback_buffer_ready &&
      state->playback_pcm &&
      state->playback_pcm_len > 0 &&
      fabs(state->playback_prepared_speed - speed) <= 0.001) {
    state->render_active = FALSE;
    state->render_progress = 1.0;
    LeaveCriticalSection(&state->lock);
    return 1;
  }
  state->playback_buffer_ready = FALSE;
  if (!(speed > 0.0 && fabs(speed - 1.0) > 0.001)) {
    ready = windows_audio_install_prepared_buffer_locked(state, pcm, pcm_len, 1.0, speed);
    state->render_active = FALSE;
    state->render_progress = ready ? 1.0 : 0.0;
    LeaveCriticalSection(&state->lock);
    return ready;
  }

  if (!((const WAVEFORMATEX *)format)->nChannels) {
    state->render_active = FALSE;
    state->render_progress = 0.0;
    LeaveCriticalSection(&state->lock);
    return 0;
  }

  pcm_snapshot = (BYTE *)malloc(pcm_len);
  if (pcm_snapshot) {
    memcpy(pcm_snapshot, pcm, pcm_len);
  }
  {
    const WAVEFORMATEX *source_format = (const WAVEFORMATEX *)format;
    size_t format_bytes = sizeof(WAVEFORMATEX) + source_format->cbSize;
    format_snapshot = (WAVEFORMATEX *)malloc(format_bytes);
    if (format_snapshot) {
      memcpy(format_snapshot, source_format, format_bytes);
    }
  }
  generation = ++state->render_generation;
  state->render_active = TRUE;
  state->render_progress = 0.0;
  LeaveCriticalSection(&state->lock);

  if (!pcm_snapshot || !format_snapshot) {
    goto cleanup;
  }

  source_s16 = windows_audio_convert_format_to_s16(pcm_snapshot, pcm_len, format_snapshot, &source_s16_len);
  progress_context.state = state;
  progress_context.generation = generation;
  if (source_s16 && playback_renderer_render_s16(source_s16,
                                                 source_s16_len,
                                                 format_snapshot->nSamplesPerSec,
                                                 format_snapshot->nChannels,
                                                 0,
                                                 captured_frames,
                                                 speed,
                                                 NULL,
                                                 NULL,
                                                 windows_audio_render_progress_cb,
                                                 &progress_context,
                                                 &render_result,
                                                 render_error,
                                                 sizeof render_error) &&
      render_result.data && render_result.len > 0 && render_result.rendered_frames > 0) {
    rendered_format = windows_audio_convert_s16_to_format(render_result.data,
                                                          render_result.len,
                                                          format_snapshot->nChannels,
                                                          format_snapshot,
                                                          &rendered_format_len);
    if (rendered_format && rendered_format_len > 0) {
      rendered_to_source_ratio = render_result.source_frames > 0
        ? (gdouble)render_result.source_frames / (gdouble)render_result.rendered_frames
        : 1.0;
      ready = 1;
    }
  }

cleanup:
  EnterCriticalSection(&state->lock);
  if (generation == state->render_generation) {
    state->render_active = FALSE;
    state->render_progress = ready ? 1.0 : 0.0;
    if (ready) {
      ready = windows_audio_install_prepared_buffer_locked(state,
                                                          rendered_format,
                                                          rendered_format_len,
                                                          rendered_to_source_ratio,
                                                          speed);
    }
  } else {
    ready = 0;
  }
  LeaveCriticalSection(&state->lock);

  free(pcm_snapshot);
  free(format_snapshot);
  free(source_s16);
  free(rendered_format);
  playback_renderer_result_clear(&render_result);
  return ready;
}

int windows_audio_backend_prepare_playback_buffer(void *user_data, gdouble speed) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  unsigned char *source_copy = NULL;
  WAVEFORMATEX *format_copy = NULL;
  size_t source_len = 0;
  uint64_t source_frames = 0;

  if (!state) {
    return 0;
  }

  EnterCriticalSection(&state->lock);
  source_len = windows_audio_source_pcm_len(context, state);
  source_frames = windows_audio_source_captured_frames(context, state);
  if (source_len > 0 && windows_audio_source_pcm_data(context, state)) {
    source_copy = (unsigned char *)malloc(source_len);
    if (source_copy) {
      memcpy(source_copy, windows_audio_source_pcm_data(context, state), source_len);
    }
  }
  if (state->wave_format) {
    size_t format_bytes = sizeof(WAVEFORMATEX) + state->wave_format->cbSize;
    format_copy = (WAVEFORMATEX *)malloc(format_bytes);
    if (format_copy) {
      memcpy(format_copy, state->wave_format, format_bytes);
    }
  }
  LeaveCriticalSection(&state->lock);
  {
    int ready = windows_audio_backend_prepare_playback_buffer_from_source(user_data,
                                                                         source_copy,
                                                                         source_len,
                                                                         format_copy,
                                                                         source_frames,
                                                                         speed);
    free(source_copy);
    free(format_copy);
    return ready;
  }
}

static const AudioBackendVTable windows_audio_vtable_impl = {
  .start_capture = windows_start_capture,
  .stop_capture = windows_stop_capture,
  .start_playback = windows_start_playback,
  .stop_playback = windows_stop_playback,
};

const AudioBackendVTable *windows_audio_backend_vtable(void) {
  return &windows_audio_vtable_impl;
}

int windows_audio_backend_has_capture(void *user_data) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  int active = FALSE;

  if (state) {
    EnterCriticalSection(&state->lock);
    active = state->capture_active;
    LeaveCriticalSection(&state->lock);
  }

  return active;
}

int windows_audio_backend_has_playback(void *user_data) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  int active = FALSE;

  if (state) {
    EnterCriticalSection(&state->lock);
    active = state->playback_active;
    LeaveCriticalSection(&state->lock);
  }

  return active;
}

int windows_audio_backend_format_is_float(const void *format_ptr) {
  return windows_audio_format_is_float_impl((const WAVEFORMATEX *)format_ptr);
}

int windows_audio_backend_render_progress(void *user_data, gdouble *out_progress, int *out_active) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  if (out_progress) {
    *out_progress = 0.0;
  }
  if (out_active) {
    *out_active = 0;
  }
  if (!state) {
    return 0;
  }

  EnterCriticalSection(&state->lock);
  if (out_progress) {
    *out_progress = state->render_progress;
  }
  if (out_active) {
    *out_active = state->render_active;
  }
  LeaveCriticalSection(&state->lock);
  return 1;
}

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
                                   uint64_t *out_captured_frames) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  if (out_pcm) {
    *out_pcm = NULL;
  }
  if (out_pcm_len) {
    *out_pcm_len = 0;
  }
  if (out_format) {
    *out_format = NULL;
  }
  if (out_capture_active) {
    *out_capture_active = 0;
  }
  if (out_playback_active) {
    *out_playback_active = 0;
  }
  if (out_playback_cursor_bytes) {
    *out_playback_cursor_bytes = 0;
  }
  if (out_playback_total_bytes) {
    *out_playback_total_bytes = 0;
  }
  if (out_captured_frames) {
    *out_captured_frames = 0;
  }
  if (out_wave_peaks) {
    *out_wave_peaks = NULL;
  }
  if (out_wave_peak_count) {
    *out_wave_peak_count = 0;
  }

  if (!state) {
    return 0;
  }

  EnterCriticalSection(&state->lock);
  if (out_pcm && windows_audio_source_pcm_data(context, state) && windows_audio_source_pcm_len(context, state) > 0) {
    const size_t source_len = windows_audio_source_pcm_len(context, state);
    *out_pcm = (unsigned char *)malloc(source_len);
    if (*out_pcm) {
      memcpy(*out_pcm, windows_audio_source_pcm_data(context, state), source_len);
      if (out_pcm_len) {
        *out_pcm_len = source_len;
      }
    }
  }
  if (out_format && state->wave_format) {
    size_t bytes = sizeof(WAVEFORMATEX) + state->wave_format->cbSize;
    WAVEFORMATEX *copy = (WAVEFORMATEX *)malloc(bytes);
    if (copy) {
      memcpy(copy, state->wave_format, bytes);
      *out_format = copy;
    }
  }
  if (out_capture_active) {
    *out_capture_active = state->capture_active;
  }
  if (out_playback_active) {
    *out_playback_active = state->playback_active;
  }
  if (out_playback_cursor_bytes) {
    *out_playback_cursor_bytes = windows_audio_audible_cursor_locked(state);
  }
  if (out_playback_total_bytes) {
    *out_playback_total_bytes = state->playback_total_bytes;
  }
  if (out_captured_frames) {
    *out_captured_frames = windows_audio_source_captured_frames(context, state);
  }
  if (out_wave_peaks && state->wave_peaks && state->wave_peak_len > 0) {
    uint16_t *copy = (uint16_t *)malloc(state->wave_peak_len * sizeof(*copy));
    if (copy) {
      memcpy(copy, state->wave_peaks, state->wave_peak_len * sizeof(*copy));
      *out_wave_peaks = copy;
      if (out_wave_peak_count) {
        *out_wave_peak_count = state->wave_peak_len;
      }
    }
  }
  LeaveCriticalSection(&state->lock);

  return 1;
}

void windows_audio_backend_destroy(void *user_data) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  if (!state || !context) {
    return;
  }

  windows_audio_signal_stop(state->capture_stop_event);
  windows_audio_signal_stop(state->playback_stop_event);
  windows_audio_join_thread(&state->capture_thread);
  windows_audio_join_thread(&state->playback_thread);
  EnterCriticalSection(&state->lock);
  free(state->pcm);
  state->pcm = NULL;
  state->pcm_len = 0;
  state->pcm_cap = 0;
  free(state->playback_pcm);
  state->playback_pcm = NULL;
  state->playback_pcm_len = 0;
  state->playback_pcm_cap = 0;
  state->playback_source_offset_bytes = 0;
  state->playback_buffer_ready = FALSE;
  free(state->wave_peaks);
  state->wave_peaks = NULL;
  state->wave_peak_len = 0;
  state->wave_peak_cap = 0;
  windows_audio_free_format(state->wave_format);
  state->wave_format = NULL;
  LeaveCriticalSection(&state->lock);
  DeleteCriticalSection(&state->lock);
  if (state->capture_stop_event) {
    CloseHandle(state->capture_stop_event);
  }
  if (state->playback_stop_event) {
    CloseHandle(state->playback_stop_event);
  }
  free(state);
  context->audio_state = NULL;
}

#endif
