#include "windows_audio_backend.h"
#include "windows_debug_log.h"

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
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static void windows_audio_debug(const char *message) {
  if (!message) {
    return;
  }
  windows_debug_log(message);
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
  BYTE *pcm;
  size_t pcm_len;
  size_t pcm_cap;
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

static void windows_audio_reset_buffer_locked(WindowsAudioBackendState *state) {
  if (!state) {
    return;
  }

  state->pcm_len = 0;
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
  UINT32 bytes_per_frame = 0;
  HANDLE mm_task = NULL;
  HRESULT hr;

  if (!state) {
    windows_audio_debug("capture thread no state");
    return 1;
  }

  windows_audio_debug("capture thread entered");
  CoInitializeEx(NULL, COINIT_MULTITHREADED);

  windows_audio_debug("capture thread opening loopback device");
  hr = windows_audio_init_device_client(eRender, &client, &format);
  if (FAILED(hr) || !client || !format) {
    windows_audio_debug("capture thread device init failed");
    goto cleanup;
  }

  windows_audio_debug("capture thread initializing client");
  hr = IAudioClient_Initialize(client,
                               AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK,
                               10000000,
                               0,
                               format,
                               NULL);
  if (FAILED(hr)) {
    windows_audio_debug("capture thread initialize failed");
    goto cleanup;
  }

  windows_audio_debug("capture thread initialize succeeded");

  bytes_per_frame = format->nBlockAlign;

  windows_audio_debug("capture thread getting service");
  hr = IAudioClient_GetService(client, &kIID_IAudioCaptureClient, (void **)&capture_client);
  if (FAILED(hr)) {
    windows_audio_debug("capture thread get service failed");
    goto cleanup;
  }

  windows_audio_debug("capture thread got service");

  windows_audio_debug("capture thread setting mm task characteristics");
  mm_task = NULL;
  windows_audio_debug("capture thread skipping mm task characteristics");
  windows_audio_debug("capture thread starting client");
  hr = IAudioClient_Start(client);
  if (FAILED(hr)) {
    windows_audio_debug("capture thread start failed");
    goto cleanup;
  }

  windows_audio_debug("capture thread start succeeded");

  EnterCriticalSection(&state->lock);
  windows_audio_reset_buffer_locked(state);
  if (state->wave_format) {
    windows_audio_free_format(state->wave_format);
  }
  state->wave_format = format;
  format = NULL;
  state->capture_active = TRUE;
  LeaveCriticalSection(&state->lock);

  windows_audio_debug("windows capture thread started");

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
          windows_audio_append_locked(state, silence, bytes);
          free(silence);
        }
      } else {
        windows_audio_append_locked(state, data, bytes);
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
  windows_audio_debug("windows capture thread stopped");
  CoUninitialize();
  return 0;
}

static DWORD WINAPI windows_playback_thread(LPVOID param) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)param;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);
  IAudioClient *client = NULL;
  IAudioRenderClient *render_client = NULL;
  WAVEFORMATEX *format = NULL;
  BYTE *snapshot = NULL;
  size_t snapshot_len = 0;
  HRESULT hr;

  if (!state) {
    return 1;
  }

  windows_audio_debug("windows playback thread starting");

  CoInitializeEx(NULL, COINIT_MULTITHREADED);
  windows_audio_debug("playback thread initialized COM");

  EnterCriticalSection(&state->lock);
  if (state->pcm_len > 0) {
    snapshot = (BYTE *)malloc(state->pcm_len);
    if (snapshot) {
      memcpy(snapshot, state->pcm, state->pcm_len);
      snapshot_len = state->pcm_len;
    }
  }
  format = state->wave_format;
  if (format) {
    format = (WAVEFORMATEX *)malloc(sizeof(WAVEFORMATEX) + format->cbSize);
    if (format) {
      memcpy(format, state->wave_format, sizeof(WAVEFORMATEX) + state->wave_format->cbSize);
    }
  }
  LeaveCriticalSection(&state->lock);

  if (!snapshot || !format) {
    windows_audio_debug("playback thread missing snapshot or format");
    goto cleanup;
  }

  windows_audio_debug("playback thread opening render device");
  hr = windows_audio_init_device_client(eRender, &client, &format);
  if (FAILED(hr) || !client || !format) {
    windows_audio_debug("playback thread device init failed");
    goto cleanup;
  }

  windows_audio_debug("playback thread initializing client");
  hr = IAudioClient_Initialize(client,
                               AUDCLNT_SHAREMODE_SHARED,
                               0,
                               10000000,
                               0,
                               format,
                               NULL);
  if (FAILED(hr)) {
    windows_audio_debug("playback thread initialize failed");
    goto cleanup;
  }

  windows_audio_debug("playback thread initialize succeeded");

  windows_audio_debug("playback thread getting render service");
  hr = IAudioClient_GetService(client, &kIID_IAudioRenderClient, (void **)&render_client);
  if (FAILED(hr)) {
    windows_audio_debug("playback thread get service failed");
    goto cleanup;
  }

  windows_audio_debug("playback thread got render service");

  windows_audio_debug("playback thread starting client");
  hr = IAudioClient_Start(client);
  if (FAILED(hr)) {
    windows_audio_debug("playback thread start failed");
    goto cleanup;
  }

  windows_audio_debug("playback thread start succeeded");

  EnterCriticalSection(&state->lock);
  state->playback_active = TRUE;
  LeaveCriticalSection(&state->lock);

  {
    UINT32 buffer_frames = 0;
    UINT32 bytes_per_frame = format->nBlockAlign;
    size_t cursor = 0;

    if (FAILED(IAudioClient_GetBufferSize(client, &buffer_frames))) {
      windows_audio_debug("playback thread buffer size failed");
      goto cleanup;
    }

    windows_audio_debug("playback thread entered render loop");

  while (WaitForSingleObject(state->playback_stop_event, 10) == WAIT_TIMEOUT) {
      UINT32 padding = 0;
      UINT32 available = 0;
      BYTE *render_data = NULL;

      if (FAILED(IAudioClient_GetCurrentPadding(client, &padding))) {
        windows_audio_debug("playback thread current padding failed");
        break;
      }
      available = buffer_frames - padding;
      if (available == 0) {
        continue;
      }

      if (FAILED(IAudioRenderClient_GetBuffer(render_client, available, &render_data))) {
        windows_audio_debug("playback thread get buffer failed");
        break;
      }

      {
        size_t bytes = (size_t)available * bytes_per_frame;
        size_t remaining = (cursor < snapshot_len) ? (snapshot_len - cursor) : 0;
        size_t to_copy = remaining < bytes ? remaining : bytes;
        if (to_copy > 0) {
          memcpy(render_data, snapshot + cursor, to_copy);
        }
        if (to_copy < bytes) {
          memset(render_data + to_copy, 0, bytes - to_copy);
        }
        cursor += to_copy;
      }

      IAudioRenderClient_ReleaseBuffer(render_client, available, 0);
      if (cursor >= snapshot_len) {
        windows_audio_debug("playback thread reached end of snapshot");
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
  if (format) {
    free(format);
  }
  EnterCriticalSection(&state->lock);
  state->playback_active = FALSE;
  LeaveCriticalSection(&state->lock);
  windows_audio_debug("windows playback thread stopped");
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
  }
  if (state->capture_active) {
    LeaveCriticalSection(&state->lock);
    return TRUE;
  }
  ResetEvent(state->capture_stop_event);
  state->capture_thread = CreateThread(NULL, 0, windows_capture_thread, context, 0, NULL);
  LeaveCriticalSection(&state->lock);
  windows_audio_debug(state->capture_thread ? "capture thread created" : "capture thread create failed");
  return state->capture_thread != NULL;
}

static void windows_stop_capture(void *user_data, gboolean force_stopped) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  (void)force_stopped;

  if (!state) {
    return;
  }
  windows_audio_debug("stopping capture");
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
    windows_audio_debug("finalizing capture before playback");
    windows_stop_capture(user_data, TRUE);
  }

  EnterCriticalSection(&state->lock);
  if (state->playback_active || state->pcm_len == 0 || !state->wave_format) {
    LeaveCriticalSection(&state->lock);
    return FALSE;
  }
  ResetEvent(state->playback_stop_event);
  state->playback_thread = CreateThread(NULL, 0, windows_playback_thread, context, 0, NULL);
  LeaveCriticalSection(&state->lock);
  windows_audio_debug(state->playback_thread ? "playback thread created" : "playback thread create failed");
  return state->playback_thread != NULL;
}

static void windows_stop_playback(void *user_data, gboolean reset_cursor) {
  PlatformWindowsContext *context = (PlatformWindowsContext *)user_data;
  WindowsAudioBackendState *state = windows_audio_get_state(context, FALSE);

  (void)reset_cursor;

  if (!state) {
    return;
  }
  windows_audio_debug("stopping playback");
  windows_audio_signal_stop(state->playback_stop_event);
  windows_audio_join_thread(&state->playback_thread);
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

int windows_audio_backend_snapshot(void *user_data,
                                  unsigned char **out_pcm,
                                  size_t *out_pcm_len,
                                  void **out_format,
                                  int *out_capture_active,
                                  int *out_playback_active) {
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

  if (!state) {
    return 0;
  }

  EnterCriticalSection(&state->lock);
  if (out_pcm && state->pcm && state->pcm_len > 0) {
    *out_pcm = (unsigned char *)malloc(state->pcm_len);
    if (*out_pcm) {
      memcpy(*out_pcm, state->pcm, state->pcm_len);
      if (out_pcm_len) {
        *out_pcm_len = state->pcm_len;
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
