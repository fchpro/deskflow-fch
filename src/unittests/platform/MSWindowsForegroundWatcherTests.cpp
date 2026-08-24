/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MSWindowsForegroundWatcherTests.h"

#include "platform/MSWindowsForegroundWatcher.h"

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

QTEST_MAIN(MSWindowsForegroundWatcherTests)
