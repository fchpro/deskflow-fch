/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsForegroundWatcherTests.h"

#include "platform/MSWindowsForegroundWatcher.h"

#include <algorithm>

void MSWindowsForegroundWatcherTests::exeBaseNameStripsDirectories()
{
  QCOMPARE(MSWindowsForegroundWatcher::exeBaseName(L"C:\\Games\\BF6\\bf6.exe"), std::wstring(L"bf6.exe"));
  QCOMPARE(MSWindowsForegroundWatcher::exeBaseName(L"C:/Games/BF6/bf6.exe"), std::wstring(L"bf6.exe"));
}

void MSWindowsForegroundWatcherTests::exeBaseNameKeepsPlainName()
{
  QCOMPARE(MSWindowsForegroundWatcher::exeBaseName(L"bf6.exe"), std::wstring(L"bf6.exe"));
}

void MSWindowsForegroundWatcherTests::isExcludedMatchesCaseInsensitive()
{
  const std::vector<std::wstring> apps = {L"bf6.exe", L"valorant.exe"};
  QVERIFY(MSWindowsForegroundWatcher::isExcluded(L"BF6.exe", apps));
  QVERIFY(MSWindowsForegroundWatcher::isExcluded(L"VALORANT.EXE", apps));
}

void MSWindowsForegroundWatcherTests::isExcludedMatchesEntryWithoutExtension()
{
  const std::vector<std::wstring> apps = {L"bf6"};
  QVERIFY(MSWindowsForegroundWatcher::isExcluded(L"bf6.exe", apps));
}

void MSWindowsForegroundWatcherTests::isExcludedRejectsUnlistedApp()
{
  const std::vector<std::wstring> apps = {L"bf6.exe", L"valorant.exe"};
  QVERIFY(!MSWindowsForegroundWatcher::isExcluded(L"notepad.exe", apps));
}

void MSWindowsForegroundWatcherTests::isExcludedRejectsPartialName()
{
  const std::vector<std::wstring> apps = {L"bf6.exe"};
  QVERIFY(!MSWindowsForegroundWatcher::isExcluded(L"bf61.exe", apps));
  QVERIFY(!MSWindowsForegroundWatcher::isExcluded(L"abf6.exe", apps));
}

void MSWindowsForegroundWatcherTests::processImageBaseNameResolvesOwnProcess()
{
  const auto name = MSWindowsForegroundWatcher::processImageBaseName(GetCurrentProcessId());
  QVERIFY(!name.empty());
  QVERIFY(MSWindowsForegroundWatcher::toLower(name).find(L".exe") != std::wstring::npos);
}

void MSWindowsForegroundWatcherTests::processImageBaseNameFailsForBadPid()
{
  // windows pids are multiples of 4, so pid 3 can never exist
  QVERIFY(MSWindowsForegroundWatcher::processImageBaseName(3).empty());
}

void MSWindowsForegroundWatcherTests::findExcludedPidsFindsOwnProcess()
{
  const auto self = MSWindowsForegroundWatcher::processImageBaseName(GetCurrentProcessId());
  const auto pids = MSWindowsForegroundWatcher::findExcludedPids({self});
  QVERIFY(std::find(pids.begin(), pids.end(), GetCurrentProcessId()) != pids.end());
}

void MSWindowsForegroundWatcherTests::findExcludedPidsEmptyForUnknownApp()
{
  QVERIFY(MSWindowsForegroundWatcher::findExcludedPids({L"no-such-app-deskflow-test.exe"}).empty());
}

void MSWindowsForegroundWatcherTests::deciderPausesImmediately()
{
  using F = ExclusionDecider::Foreground;
  using T = ExclusionDecider::Transition;
  ExclusionDecider decider(300);
  QCOMPARE(decider.update(F::NotExcluded, 1000), T::None);
  QVERIFY(!decider.paused());
  QCOMPARE(decider.update(F::Excluded, 1001), T::Pause);
  QVERIFY(decider.paused());
  QCOMPARE(decider.update(F::Excluded, 1002), T::None);
}

void MSWindowsForegroundWatcherTests::deciderIgnoresBriefFocusFlicker()
{
  // the bug seen live: bf6.exe pause, explorer.exe 13 ms later, then bf6
  // regains focus with no further event
  using F = ExclusionDecider::Foreground;
  using T = ExclusionDecider::Transition;
  ExclusionDecider decider(300);
  QCOMPARE(decider.update(F::Excluded, 1000), T::Pause);
  QCOMPARE(decider.update(F::NotExcluded, 1013), T::None);
  QVERIFY(decider.paused());
  QCOMPARE(decider.update(F::Excluded, 1100), T::None);
  QVERIFY(decider.paused());
  // a later non-excluded period must start counting from scratch
  QCOMPARE(decider.update(F::NotExcluded, 1200), T::None);
  QCOMPARE(decider.update(F::NotExcluded, 1400), T::None);
  QVERIFY(decider.paused());
}

void MSWindowsForegroundWatcherTests::deciderResumesAfterStableNonExcluded()
{
  using F = ExclusionDecider::Foreground;
  using T = ExclusionDecider::Transition;
  ExclusionDecider decider(300);
  QCOMPARE(decider.update(F::Excluded, 1000), T::Pause);
  QCOMPARE(decider.update(F::NotExcluded, 2000), T::None);
  QCOMPARE(decider.update(F::NotExcluded, 2299), T::None);
  QCOMPARE(decider.update(F::NotExcluded, 2300), T::Resume);
  QVERIFY(!decider.paused());
  QCOMPARE(decider.update(F::NotExcluded, 2400), T::None);
}

void MSWindowsForegroundWatcherTests::deciderUnknownForegroundKeepsState()
{
  using F = ExclusionDecider::Foreground;
  using T = ExclusionDecider::Transition;
  ExclusionDecider decider(300);
  QCOMPARE(decider.update(F::Unknown, 1000), T::None);
  QVERIFY(!decider.paused());
  QCOMPARE(decider.update(F::Excluded, 1001), T::Pause);
  QCOMPARE(decider.update(F::Unknown, 5000), T::None);
  QVERIFY(decider.paused());
}

void MSWindowsForegroundWatcherTests::deciderUnknownResetsResumeTimer()
{
  using F = ExclusionDecider::Foreground;
  using T = ExclusionDecider::Transition;
  ExclusionDecider decider(300);
  QCOMPARE(decider.update(F::Excluded, 1000), T::Pause);
  QCOMPARE(decider.update(F::NotExcluded, 2000), T::None);
  QCOMPARE(decider.update(F::Unknown, 2100), T::None);
  // 300 ms since the first non-excluded sample, but the unknown sample
  // broke the streak
  QCOMPARE(decider.update(F::NotExcluded, 2300), T::None);
  QVERIFY(decider.paused());
  QCOMPARE(decider.update(F::NotExcluded, 2600), T::Resume);
}

void MSWindowsForegroundWatcherTests::watcherStartupDetectsForeground()
{
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  const auto exe = MSWindowsForegroundWatcher::processImageBaseName(pid);
  if (exe.empty()) {
    QSKIP("no resolvable foreground window");
  }

  int pauses = 0;
  int resumes = 0;
  std::wstring reported;
  std::vector<std::vector<DWORD>> pidLists;
  MSWindowsForegroundWatcher watcher(
      {exe},
      [&](bool excluded, const std::wstring &name) {
        excluded ? ++pauses : ++resumes;
        reported = name;
      },
      nullptr, [&](const std::vector<DWORD> &pids) { pidLists.push_back(pids); }
  );

  // detected during construction, without any foreground event
  QCOMPARE(pauses, 1);
  QCOMPARE(resumes, 0);
  QVERIFY(watcher.isForegroundExcluded());
  QVERIFY(watcher.isForegroundPidExcluded());
  QCOMPARE(MSWindowsForegroundWatcher::toLower(reported), MSWindowsForegroundWatcher::toLower(exe));

  // the pid snapshot was handed out and contains the foreground pid
  QVERIFY(!pidLists.empty());
  const auto &pids = pidLists.front();
  QVERIFY(std::find(pids.begin(), pids.end(), pid) != pids.end());

  // the poll timer keeps running on the message pump; a stable foreground
  // must not produce spurious transitions
  QTest::qWait(450);
  QCOMPARE(pauses, 1);
  QCOMPARE(resumes, 0);
  QVERIFY(watcher.isForegroundExcluded());
}

QTEST_MAIN(MSWindowsForegroundWatcherTests)
