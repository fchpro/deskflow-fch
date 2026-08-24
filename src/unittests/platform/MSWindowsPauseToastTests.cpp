/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsPauseToastTests.h"

#include "platform/MSWindowsPauseToast.h"

namespace {
void pumpFor(int ms)
{
  const DWORD end = GetTickCount() + ms;
  MSG msg;
  while (GetTickCount() < end) {
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessage(&msg);
    }
    Sleep(10);
  }
}
} // namespace

void MSWindowsPauseToastTests::showCreatesVisibleWindow()
{
  MSWindowsPauseToast toast;
  toast.show(L"Deskflow paused \u2014 bf6.exe", 60000);
  QVERIFY(toast.window() != nullptr);
  QVERIFY(IsWindowVisible(toast.window()));
}

void MSWindowsPauseToastTests::windowIsBottomRightOfWorkArea()
{
  MSWindowsPauseToast toast;
  toast.show(L"Deskflow paused \u2014 bf6.exe", 60000);
  RECT workArea = {};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
  RECT rect = {};
  GetWindowRect(toast.window(), &rect);
  const int midX = (workArea.left + workArea.right) / 2;
  const int midY = (workArea.top + workArea.bottom) / 2;
  QVERIFY(rect.left > midX);
  QVERIFY(rect.top > midY);
  QVERIFY(rect.right <= workArea.right);
  QVERIFY(rect.bottom <= workArea.bottom);
}

void MSWindowsPauseToastTests::windowDoesNotActivate()
{
  MSWindowsPauseToast toast;
  toast.show(L"Deskflow paused \u2014 bf6.exe", 60000);
  const LONG_PTR exStyle = GetWindowLongPtrW(toast.window(), GWL_EXSTYLE);
  QVERIFY((exStyle & WS_EX_NOACTIVATE) != 0);
  QVERIFY((exStyle & WS_EX_TOPMOST) != 0);
  QVERIFY(GetForegroundWindow() != toast.window());
}

void MSWindowsPauseToastTests::windowClosesAfterDuration()
{
  MSWindowsPauseToast toast;
  toast.show(L"Deskflow paused \u2014 bf6.exe", 200);
  QVERIFY(toast.window() != nullptr);
  pumpFor(600); // wait signal: WM_TIMER close, budget 3x the duration
  QVERIFY(toast.window() == nullptr);
}

void MSWindowsPauseToastTests::showReplacesTextWhileVisible()
{
  MSWindowsPauseToast toast;
  toast.show(L"Deskflow paused \u2014 bf6.exe", 60000);
  const HWND first = toast.window();
  toast.show(L"Deskflow resumed \u2014 bf6.exe", 60000);
  QCOMPARE(toast.window(), first);
  QCOMPARE(toast.text(), std::wstring(L"Deskflow resumed \u2014 bf6.exe"));
}

QTEST_MAIN(MSWindowsPauseToastTests)
