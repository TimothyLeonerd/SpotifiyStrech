#include "core.h"
#include "platform_windows_win32.h"
#include "windows_audio_backend.h"
#include "windows_debug_log.h"

#ifdef _WIN32
#include <commctrl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#endif

#ifdef _WIN32

static HWND platform_windows_create_child(HWND parent, HINSTANCE instance, const wchar_t *class_name, const wchar_t *text, DWORD style, int x, int y, int w, int h, int id) {
  return CreateWindowExW(0, class_name, text, style, x, y, w, h, parent, (HMENU)(INT_PTR)id, instance, NULL);
}

static void platform_windows_notify_command(PlatformWindowsHost *host, int command_id) {
  if (host && host->context && host->context->command_handler) {
    host->context->command_handler(host->context->command_handler_user_data, command_id);
  }
}

static void platform_windows_update_backend_ui(PlatformWindowsHost *host) {
  void *format_ptr = NULL;
  int capture_active = 0;
  int playback_active = 0;
  uint16_t *wave_peaks = NULL;
  size_t wave_peak_count = 0;
  uint64_t captured_frames = 0;
  wchar_t status[128];
  wchar_t time_text[64];
  wchar_t play_pause_text[16];
  WAVEFORMATEX *format = NULL;

  if (!host || !host->context) {
    return;
  }

  if (windows_audio_backend_snapshot(host->context,
                                      NULL,
                                      NULL,
                                      &format_ptr,
                                      &capture_active,
                                      &playback_active,
                                      NULL,
                                       NULL,
                                       &wave_peaks,
                                       &wave_peak_count,
                                       &captured_frames)) {
    format = (WAVEFORMATEX *)format_ptr;
    const double seconds = (format && format->nSamplesPerSec > 0)
      ? (double)captured_frames / (double)format->nSamplesPerSec
      : 0.0;
    const AppMode mode = capture_active ? MODE_RECORDING : (playback_active ? MODE_PLAYING : MODE_IDLE);
    const CoreStatusState status_state = core_build_status_state(mode, seconds, NULL, FALSE, FALSE);
    const CoreUiState ui_state = core_build_ui_state(mode, FALSE);

    swprintf(status, sizeof status / sizeof status[0], L"%hs", status_state.text);
    swprintf(time_text, sizeof time_text / sizeof time_text[0], L"%.1f / %.1fs", seconds, seconds);
    swprintf(play_pause_text, sizeof play_pause_text / sizeof play_pause_text[0], L"%hs", ui_state.play_pause_label);

    if (host->context->status_label) {
      SetWindowTextW(host->context->status_label, status);
    }
    if (host->context->time_label) {
      SetWindowTextW(host->context->time_label, time_text);
    }
    if (host->context->play_pause_button) {
      SetWindowTextW(host->context->play_pause_button, play_pause_text);
    }
    if (host->context->waveform_view) {
      InvalidateRect(host->context->waveform_view, NULL, TRUE);
    }
  }

  free(format_ptr);
  free(wave_peaks);
}

static void platform_windows_paint_waveform(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc;
  PlatformWindowsHost *host = (PlatformWindowsHost *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
  void *format_ptr = NULL;
  int capture_active = 0;
  int playback_active = 0;
  uint16_t *wave_peaks = NULL;
  size_t wave_peak_count = 0;

  GetClientRect(hwnd, &rc);

  FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
  if (host && host->context && windows_audio_backend_snapshot(host->context,
                                                                NULL,
                                                                NULL,
                                                                &format_ptr,
                                                                &capture_active,
                                                                &playback_active,
                                                                NULL,
                                                                NULL,
                                                                &wave_peaks,
                                                                &wave_peak_count,
                                                                NULL)) {
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    const int mid_y = height / 2;

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(230, 230, 235));

    if (wave_peaks && wave_peak_count > 0 && width > 0 && height > 0) {
      HPEN wave_pen = CreatePen(PS_SOLID, 1, RGB(120, 220, 160));
      HPEN old_pen = (HPEN)SelectObject(hdc, wave_pen);
      for (int x = 0; x < width; ++x) {
        size_t peak_idx = (size_t)x * wave_peak_count / (size_t)width;
        if (peak_idx >= wave_peak_count) {
          peak_idx = wave_peak_count - 1;
        }

        {
          const double amp = (double)wave_peaks[peak_idx] / 32768.0;
          const int y1 = mid_y - (int)(amp * (height * 0.42));
          const int y2 = mid_y + (int)(amp * (height * 0.42));
          MoveToEx(hdc, x, y1, NULL);
          LineTo(hdc, x, y2);
        }
      }

      SelectObject(hdc, old_pen);
      DeleteObject(wave_pen);
    } else {
      const wchar_t *msg = capture_active ? L"Capturing..." : (playback_active ? L"Playing..." : L"Ready");
      DrawTextW(hdc, msg, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
  }

  free(format_ptr);
  free(wave_peaks);
  EndPaint(hwnd, &ps);
}

static void platform_windows_layout_children(PlatformWindowsHost *host, int width, int height) {
  const int margin = 16;
  const int gap = 8;
  const int button_h = 28;
  const int row_y = margin + 24;
  const int waveform_y = row_y + button_h + gap + 20;
  const int waveform_h = height - waveform_y - 80;
  const int button_w = 96;
  const int small_w = 72;
  int x = margin;

  MoveWindow(host->context->record_button, x, row_y, button_w, button_h, TRUE); x += button_w + gap;
  MoveWindow(host->context->stop_button, x, row_y, button_w, button_h, TRUE); x += button_w + gap;
  MoveWindow(host->context->play_pause_button, x, row_y, button_w, button_h, TRUE); x += button_w + gap;
  MoveWindow(host->context->loop_button, x, row_y, small_w, button_h, TRUE);

  MoveWindow(host->context->speed_value_label, width - margin - 48, row_y, 48, button_h, TRUE);
  MoveWindow(host->context->time_label, margin, waveform_y + waveform_h + gap, width - 2 * margin, 20, TRUE);
  MoveWindow(host->context->status_label, margin, waveform_y + waveform_h + gap + 22, width - 2 * margin, 20, TRUE);
  MoveWindow(host->context->progress_bar, width - margin - 160, row_y + button_h + gap, 160, 18, TRUE);
  MoveWindow(host->context->waveform_view, margin, waveform_y, width - 2 * margin, waveform_h, TRUE);
}

static void platform_windows_init_children(PlatformWindowsHost *host) {
  HINSTANCE instance = host->instance;
  HWND parent = host->window;

  host->context->status_label = platform_windows_create_child(parent, instance, L"STATIC", L"Idle | 0.0s captured", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1001);
  host->context->speed_value_label = platform_windows_create_child(parent, instance, L"STATIC", L"1.0x", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1002);
  host->context->time_label = platform_windows_create_child(parent, instance, L"STATIC", L"0.0 / 0.0s", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1003);
  host->context->waveform_view = platform_windows_create_child(parent, instance, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_OWNERDRAW, 0, 0, 0, 0, 1004);
  host->context->record_button = platform_windows_create_child(parent, instance, L"BUTTON", L"Record", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1005);
  host->context->play_pause_button = platform_windows_create_child(parent, instance, L"BUTTON", L"Play", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1006);
  host->context->loop_button = platform_windows_create_child(parent, instance, L"BUTTON", L"Loop", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, 1007);
  host->context->stop_button = platform_windows_create_child(parent, instance, L"BUTTON", L"Stop", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1008);
  host->context->progress_bar = platform_windows_create_child(parent, instance, PROGRESS_CLASSW, NULL, WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1009);
  SendMessageW(host->context->progress_bar, PBM_SETRANGE, 0, MAKELPARAM(0, 1000));
  SendMessageW(host->context->progress_bar, PBM_SETPOS, 0, 0);
}

static LRESULT CALLBACK platform_windows_wndproc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  PlatformWindowsHost *host = (PlatformWindowsHost *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

  switch (msg) {
    case WM_NCCREATE: {
      const CREATESTRUCTW *create = (const CREATESTRUCTW *)lparam;
      host = (PlatformWindowsHost *)create->lpCreateParams;
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)host);
      if (host) {
        host->window = hwnd;
      }
      return DefWindowProcW(hwnd, msg, wparam, lparam);
    }
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
    case WM_COMMAND:
      platform_windows_notify_command(host, LOWORD(wparam));
      platform_windows_update_backend_ui(host);
      return 0;
    case WM_SIZE:
      if (host && host->context) {
        RECT rc;
        GetClientRect(hwnd, &rc);
        platform_windows_layout_children(host, rc.right - rc.left, rc.bottom - rc.top);
      }
      return 0;
    case WM_PAINT:
      if (host && host->context && hwnd == host->context->waveform_view) {
        platform_windows_paint_waveform(hwnd);
        return 0;
      }
      break;
    case WM_TIMER:
      platform_windows_update_backend_ui(host);
      return 0;
    default:
      break;
  }

  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

static ATOM platform_windows_register_class(HINSTANCE instance) {
  WNDCLASSW wc = {0};

  wc.lpfnWndProc = platform_windows_wndproc;
  wc.hInstance = instance;
  wc.lpszClassName = L"SpotifyRecorderWindow";
  wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_ARROW);

  return RegisterClassW(&wc);
}

BOOL platform_windows_host_init(PlatformWindowsHost *host, PlatformWindowsContext *context) {
  if (!host || !context) {
    return FALSE;
  }

  host->context = context;
  host->instance = GetModuleHandleW(NULL);
  if (!host->instance) {
    return FALSE;
  }

  host->window_class = platform_windows_register_class(host->instance);
  if (!host->window_class) {
    return FALSE;
  }

  host->window = CreateWindowExW(0,
                                 L"SpotifyRecorderWindow",
                                 L"Spotify Audio Recorder",
                                 WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT,
                                 CW_USEDEFAULT,
                                 960,
                                 640,
                                 NULL,
                                 NULL,
                                 host->instance,
                                 host);
  if (!host->window) {
    return FALSE;
  }

  platform_windows_init_children(host);
  SetTimer(host->window, 1, 100, NULL);
  ShowWindow(host->window, SW_SHOWDEFAULT);
  UpdateWindow(host->window);
  context->instance = host->instance;
  context->window = host->window;
  context->window_class = host->window_class;
  return TRUE;
}

int platform_windows_host_run(PlatformWindowsHost *host) {
  MSG msg;

  (void)host;

  while (GetMessageW(&msg, NULL, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  return (int)msg.wParam;
}

void platform_windows_host_shutdown(PlatformWindowsHost *host) {
  if (!host) {
    return;
  }

  if (host->window) {
    KillTimer(host->window, 1);
    DestroyWindow(host->window);
    host->window = NULL;
  }
  if (host->window_class && host->instance) {
    UnregisterClassW(L"SpotifyRecorderWindow", host->instance);
    host->window_class = 0;
  }
  if (host->context && host->context->audio_state) {
    windows_audio_backend_destroy(host->context);
  }
}

void platform_windows_host_invalidate_waveform(PlatformWindowsHost *host) {
  if (host && host->context && host->context->waveform_view) {
    InvalidateRect(host->context->waveform_view, NULL, TRUE);
  }
}

#endif
