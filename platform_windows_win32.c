#include "platform_windows_win32.h"

#ifdef _WIN32
#include <commctrl.h>
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

static void platform_windows_paint_waveform(HWND hwnd) {
  PAINTSTRUCT ps;
  HDC hdc = BeginPaint(hwnd, &ps);
  RECT rc;

  GetClientRect(hwnd, &rc);
  FillRect(hdc, &rc, (HBRUSH)(COLOR_WINDOW + 1));
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

  host->context->status_label = platform_windows_create_child(parent, instance, L"STATIC", L"Stopped | 0.0s captured", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, 1001);
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
  wc.hCursor = LoadCursorW(NULL, IDC_ARROW);

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
    DestroyWindow(host->window);
    host->window = NULL;
  }
  if (host->window_class && host->instance) {
    UnregisterClassW(L"SpotifyRecorderWindow", host->instance);
    host->window_class = 0;
  }
}

void platform_windows_host_invalidate_waveform(PlatformWindowsHost *host) {
  if (host && host->context && host->context->waveform_view) {
    InvalidateRect(host->context->waveform_view, NULL, TRUE);
  }
}

#endif
