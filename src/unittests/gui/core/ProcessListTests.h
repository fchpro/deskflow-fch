/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class ProcessListTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Test are run in order top to bottom
  void dedupeKeepsOnePerExePreferringTitle();
  void filterMatchesExeOrTitleCaseInsensitive();
  void filterEmptyQueryKeepsAll();
  void sortedPutsTitledFirstThenAlphabetical();
  void displayTextIncludesTitleWhenPresent();
  void runningListsOwnProcess();
};
