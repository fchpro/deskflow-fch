/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <string>

//! Small silent toast shown in the bottom-right corner of the work area
/*!
Not a Windows notification: a plain topmost, non-activating popup window with
no sound that destroys itself after the given duration.  Must be used on a
thread that pumps messages (WM_TIMER drives the auto-close).
*/
class MSWindowsPauseToast
{
public:
  MSWindowsPauseToast();
  ~MSWindowsPauseToast();
  MSWindowsPauseToast(const MSWindowsPauseToast &) = delete;
  MSWindowsPauseToast &operator=(const MSWindowsPauseToast &) = delete;

  //! Show the toast with the given text; replaces any toast still visible
  void show(const std::wstring &text, unsigned durationMs = 1000);

  //! Window handle of the visible toast, or nullptr when hidden
  HWND window() const
  {
    return m_window;
  }

  //! Text of the last show() call
  const std::wstring &text() const
  {
    return m_text;
  }

private:
  static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
  void paint(HWND hwnd) const;

  ATOM m_class = 0;
  HWND m_window = nullptr;
  HFONT m_font = nullptr;
  std::wstring m_text;
};
