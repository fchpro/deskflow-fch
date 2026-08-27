/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ExcludedAppsDialogTests.h"

#include "gui/core/ForegroundAppMonitor.h"
#include "gui/dialogs/ExcludedAppsDialog.h"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>

using deskflow::gui::ProcessInfo;

namespace {
ProcessInfo make(const QString &exe, const QString &title = {}, quint32 pid = 0)
{
  ProcessInfo info;
  info.exe = exe;
  info.title = title;
  info.pid = pid;
  return info;
}

QList<ProcessInfo> sampleProcesses()
{
  return {make("bf6.exe", "Battlefield 6", 100), make("brave.exe", "Reddit", 200), make("svchost.exe", {}, 300)};
}
} // namespace

void ExcludedAppsDialogTests::structureHasListsSearchAndButtons()
{
  ExcludedAppsDialog dialog;
  dialog.setExcludedApps({"bf6.exe"});
  dialog.setProcesses(sampleProcesses());

  QVERIFY(dialog.findChild<QListWidget *>("excludedList") != nullptr);
  QVERIFY(dialog.findChild<QListWidget *>("processList") != nullptr);
  QVERIFY(dialog.findChild<QLineEdit *>("searchBox") != nullptr);
  QVERIFY(dialog.findChild<QPushButton *>("btnAdd") != nullptr);
  QVERIFY(dialog.findChild<QPushButton *>("btnRemove") != nullptr);
  QVERIFY(dialog.findChild<QPushButton *>("btnRefresh") != nullptr);
  QVERIFY(dialog.findChild<QDialogButtonBox *>("buttonBox") != nullptr);

  QCOMPARE(dialog.excludedList()->count(), 1);
  QCOMPARE(dialog.excludedList()->item(0)->text(), QString("bf6.exe"));
  QCOMPARE(dialog.processList()->count(), 3);
  // titled processes first
  QVERIFY(dialog.processList()->item(0)->text().startsWith("bf6.exe"));
  QCOMPARE(dialog.processList()->item(2)->text(), QString("svchost.exe"));
  QCOMPARE(dialog.excludedApps(), QStringList{"bf6.exe"});
}

void ExcludedAppsDialogTests::searchFiltersProcessList()
{
  ExcludedAppsDialog dialog;
  dialog.setProcesses(sampleProcesses());
  dialog.searchBox()->setText("redd");
  QCOMPARE(dialog.processList()->count(), 1);
  QVERIFY(dialog.processList()->item(0)->text().startsWith("brave.exe"));
  dialog.searchBox()->setText("BF");
  QCOMPARE(dialog.processList()->count(), 1);
  dialog.searchBox()->clear();
  QCOMPARE(dialog.processList()->count(), 3);
}

void ExcludedAppsDialogTests::addSelectedProcessAppendsExe()
{
  ExcludedAppsDialog dialog;
  dialog.setExcludedApps({});
  dialog.setProcesses(sampleProcesses());
  dialog.searchBox()->setText("battle");
  dialog.processList()->setCurrentRow(0);
  dialog.findChild<QPushButton *>("btnAdd")->click();
  QCOMPARE(dialog.excludedApps(), QStringList{"bf6.exe"});
}

void ExcludedAppsDialogTests::addIgnoresDuplicatesCaseInsensitive()
{
  ExcludedAppsDialog dialog;
  dialog.setExcludedApps({"BF6.exe"});
  dialog.addExe("bf6.exe");
  dialog.addExe("  ");
  QCOMPARE(dialog.excludedApps(), QStringList{"BF6.exe"});
}

void ExcludedAppsDialogTests::removeSelectedAppDeletesEntry()
{
  ExcludedAppsDialog dialog;
  dialog.setExcludedApps({"bf6.exe", "valorant.exe"});
  dialog.excludedList()->setCurrentRow(0);
  dialog.findChild<QPushButton *>("btnRemove")->click();
  QCOMPARE(dialog.excludedApps(), QStringList{"valorant.exe"});
}

void ExcludedAppsDialogTests::foregroundMonitorMatchesExcludedApps()
{
  QVERIFY(ForegroundAppMonitor::isExcludedApp("BF6.exe", {"bf6.exe"}));
  QVERIFY(ForegroundAppMonitor::isExcludedApp("bf6.exe", {" bf6 "}));
  QVERIFY(!ForegroundAppMonitor::isExcludedApp("bf61.exe", {"bf6"}));
  QVERIFY(!ForegroundAppMonitor::isExcludedApp("brave.exe", {"bf6.exe", ""}));
}

void ExcludedAppsDialogTests::renderProofScreenshots()
{
  const auto dir = qEnvironmentVariable("DESKFLOW_PROOF_DIR");
  if (dir.isEmpty()) {
    QSKIP("DESKFLOW_PROOF_DIR not set");
  }
  ExcludedAppsDialog dialog;
  dialog.setExcludedApps({"bf6.exe"});
  dialog.refreshProcesses(); // real running processes of this machine
  dialog.show();
  QVERIFY(QTest::qWaitForWindowExposed(&dialog));
  QVERIFY(dialog.processList()->count() > 3);
  QVERIFY(dialog.grab().save(dir + "/gui-excluded-apps-dialog.png"));

  dialog.searchBox()->setText("brave");
  QVERIFY(dialog.grab().save(dir + "/gui-excluded-apps-dialog-search.png"));
  for (int i = 0; i < dialog.processList()->count(); ++i) {
    QVERIFY(dialog.processList()->item(i)->text().contains("brave", Qt::CaseInsensitive));
  }
}

QTEST_MAIN(ExcludedAppsDialogTests)
