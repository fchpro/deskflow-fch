/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsPauseToast.h"

namespace {
const wchar_t *const kToastClass = L"DeskflowPauseToast";
const int kToastWidth = 340;
const int kToastHeight = 56;
const int kToastMargin = 16;
const UINT_PTR kCloseTimerId = 1;
} // namespace

MSWindowsPauseToast::MSWindowsPauseToast()
{
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &MSWindowsPauseToast::wndProc;
  wc.hInstance = GetModuleHandle(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = kToastClass;
  m_class = RegisterClassExW(&wc);

  m_font = CreateFontW(
      -16, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
      CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI"
  );
}

MSWindowsPauseToast::~MSWindowsPauseToast()
{
  if (m_window != nullptr) {
    DestroyWindow(m_window);
  }
  if (m_font != nullptr) {
    DeleteObject(m_font);
  }
  if (m_class != 0) {
    UnregisterClassW(kToastClass, GetModuleHandle(nullptr));
  }
}

void MSWindowsPauseToast::show(const std::wstring &text, unsigned durationMs)
{
  if (m_class == 0) {
    return;
  }
  m_text = text;

  if (m_window == nullptr) {
    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int x = workArea.right - kToastWidth - kToastMargin;
    const int y = workArea.bottom - kToastHeight - kToastMargin;

    // topmost + no-activate + tool window: never steals focus, no taskbar
    // button, and no sound is ever played.
    m_window = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kToastClass, L"", WS_POPUP, x, y, kToastWidth,
        kToastHeight, nullptr, nullptr, GetModuleHandle(nullptr), this
    );
    if (m_window == nullptr) {
      return;
    }
    SetWindowRgn(m_window, CreateRoundRectRgn(0, 0, kToastWidth, kToastHeight, 12, 12), TRUE);
  } else {
    InvalidateRect(m_window, nullptr, TRUE);
  }

  ShowWindow(m_window, SW_SHOWNOACTIVATE);
  SetTimer(m_window, kCloseTimerId, durationMs, nullptr);
}

LRESULT CALLBACK MSWindowsPauseToast::wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  auto *self = reinterpret_cast<MSWindowsPauseToast *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  switch (msg) {
  case WM_NCCREATE: {
    const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    break;
  }
  case WM_PAINT:
    if (self != nullptr) {
      self->paint(hwnd);
      return 0;
    }
    break;
  case WM_TIMER:
    if (wParam == kCloseTimerId) {
      KillTimer(hwnd, kCloseTimerId);
      DestroyWindow(hwnd);
      return 0;
    }
    break;
  case WM_DESTROY:
    if (self != nullptr) {
      self->m_window = nullptr;
    }
    break;
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  default:
    break;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MSWindowsPauseToast::paint(HWND hwnd) const
{
  PAINTSTRUCT ps = {};
  HDC dc = BeginPaint(hwnd, &ps);

  RECT rect = {};
  GetClientRect(hwnd, &rect);

  HBRUSH background = CreateSolidBrush(RGB(32, 32, 36));
  FillRect(dc, &rect, background);
  DeleteObject(background);

  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(240, 240, 240));
  HGDIOBJ oldFont = SelectObject(dc, m_font);
  RECT textRect = rect;
  textRect.left += 16;
  textRect.right -= 16;
  DrawTextW(
      dc, m_text.c_str(), static_cast<int>(m_text.size()), &textRect,
      DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX
  );
  SelectObject(dc, oldFont);

  EndPaint(hwnd, &ps);
}
