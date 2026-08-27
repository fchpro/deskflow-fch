/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ProcessListTests.h"

#include "gui/core/ProcessList.h"

#include <QCoreApplication>

using deskflow::gui::ProcessInfo;
using deskflow::gui::ProcessList;

namespace {
ProcessInfo make(const QString &exe, const QString &title = {}, quint32 pid = 0)
{
  ProcessInfo info;
  info.exe = exe;
  info.title = title;
  info.pid = pid;
  return info;
}
} // namespace

void ProcessListTests::dedupeKeepsOnePerExePreferringTitle()
{
  const QList<ProcessInfo> list = {
      make("bf6.exe", {}, 100), make("BF6.exe", "Battlefield 6", 104), make("brave.exe", "Tab", 200),
      make("brave.exe", "Other", 204)
  };
  const auto result = ProcessList::dedupeByExe(list);
  QCOMPARE(result.size(), 2);
  QCOMPARE(result[0].title, QString("Battlefield 6"));
  QCOMPARE(result[0].pid, 104u);
  QCOMPARE(result[1].exe, QString("brave.exe"));
  QCOMPARE(result[1].title, QString("Tab"));
}

void ProcessListTests::filterMatchesExeOrTitleCaseInsensitive()
{
  const QList<ProcessInfo> list = {make("bf6.exe", "Battlefield 6"), make("brave.exe", "Reddit"), make("svchost.exe")};
  QCOMPARE(ProcessList::filter(list, "BF6").size(), 1);
  QCOMPARE(ProcessList::filter(list, "battle").size(), 1);
  QCOMPARE(ProcessList::filter(list, "reddit")[0].exe, QString("brave.exe"));
  QCOMPARE(ProcessList::filter(list, ".exe").size(), 3);
  QCOMPARE(ProcessList::filter(list, "valorant").size(), 0);
}

void ProcessListTests::filterEmptyQueryKeepsAll()
{
  const QList<ProcessInfo> list = {make("a.exe"), make("b.exe")};
  QCOMPARE(ProcessList::filter(list, "").size(), 2);
  QCOMPARE(ProcessList::filter(list, "   ").size(), 2);
}

void ProcessListTests::sortedPutsTitledFirstThenAlphabetical()
{
  const QList<ProcessInfo> list = {
      make("zeta.exe"), make("brave.exe", "Tab"), make("alpha.exe"), make("Bf6.exe", "Battlefield 6")
  };
  const auto result = ProcessList::sorted(list);
  QCOMPARE(result[0].exe, QString("Bf6.exe"));
  QCOMPARE(result[1].exe, QString("brave.exe"));
  QCOMPARE(result[2].exe, QString("alpha.exe"));
  QCOMPARE(result[3].exe, QString("zeta.exe"));
}

void ProcessListTests::displayTextIncludesTitleWhenPresent()
{
  QCOMPARE(ProcessList::displayText(make("bf6.exe")), QString("bf6.exe"));
  const auto text = ProcessList::displayText(make("bf6.exe", "Battlefield 6"));
  QVERIFY(text.startsWith("bf6.exe"));
  QVERIFY(text.endsWith("Battlefield 6"));
}

void ProcessListTests::runningListsOwnProcess()
{
#ifdef Q_OS_WIN
  const auto self = QCoreApplication::applicationFilePath().section(QLatin1Char('/'), -1);
  const auto pid = static_cast<quint32>(QCoreApplication::applicationPid());
  bool found = false;
  for (const auto &info : ProcessList::running()) {
    if (info.pid == pid) {
      QCOMPARE(info.exe.toLower(), self.toLower());
      found = true;
    }
  }
  QVERIFY(found);
#else
  QVERIFY(ProcessList::running().isEmpty());
#endif
}

QTEST_MAIN(ProcessListTests)
