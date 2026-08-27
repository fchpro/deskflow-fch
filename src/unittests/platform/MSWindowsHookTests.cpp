/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsHookTests.h"

#include "platform/MSWindowsHook.h"

#include <vector>

void MSWindowsHookTests::excludedPidsEmptyByDefault()
{
  QVERIFY(!MSWindowsHook::isPidExcluded(0));
  QVERIFY(!MSWindowsHook::isPidExcluded(1234));
  QVERIFY(!MSWindowsHook::isForegroundExcluded());
}

void MSWindowsHookTests::excludedPidsMatchListedPid()
{
  const DWORD pids[] = {4444, 8888};
  MSWindowsHook::setExcludedPids(pids, 2);
  QVERIFY(MSWindowsHook::isPidExcluded(4444));
  QVERIFY(MSWindowsHook::isPidExcluded(8888));
  QVERIFY(!MSWindowsHook::isPidExcluded(4448));
  QVERIFY(!MSWindowsHook::isPidExcluded(0));
}

void MSWindowsHookTests::excludedPidsClearedByEmptyList()
{
  const DWORD pids[] = {4444};
  MSWindowsHook::setExcludedPids(pids, 1);
  QVERIFY(MSWindowsHook::isPidExcluded(4444));
  MSWindowsHook::setExcludedPids(nullptr, 0);
  QVERIFY(!MSWindowsHook::isPidExcluded(4444));
}

void MSWindowsHookTests::excludedPidsCappedAtMax()
{
  std::vector<DWORD> pids;
  for (DWORD i = 1; i <= 100; ++i) {
    pids.push_back(i * 4);
  }
  MSWindowsHook::setExcludedPids(pids.data(), pids.size());
  QVERIFY(MSWindowsHook::isPidExcluded(4));
  QVERIFY(MSWindowsHook::isPidExcluded(64 * 4));
  QVERIFY(!MSWindowsHook::isPidExcluded(65 * 4));
  MSWindowsHook::setExcludedPids(nullptr, 0);
}

void MSWindowsHookTests::foregroundExcludedFollowsForegroundPid()
{
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  if (pid == 0) {
    QSKIP("no foreground window");
  }
  QVERIFY(!MSWindowsHook::isForegroundExcluded());
  MSWindowsHook::setExcludedPids(&pid, 1);
  QVERIFY(MSWindowsHook::isForegroundExcluded());
  MSWindowsHook::setExcludedPids(nullptr, 0);
  QVERIFY(!MSWindowsHook::isForegroundExcluded());
}

QTEST_MAIN(MSWindowsHookTests)
