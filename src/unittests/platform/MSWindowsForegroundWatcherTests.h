/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class MSWindowsForegroundWatcherTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Test are run in order top to bottom
  void exeBaseNameStripsDirectories();
  void exeBaseNameKeepsPlainName();
  void isExcludedMatchesCaseInsensitive();
  void isExcludedMatchesEntryWithoutExtension();
  void isExcludedRejectsUnlistedApp();
  void isExcludedRejectsPartialName();
  void processImageBaseNameResolvesOwnProcess();
  void processImageBaseNameFailsForBadPid();
  void findExcludedPidsFindsOwnProcess();
  void findExcludedPidsEmptyForUnknownApp();
  void deciderPausesImmediately();
  void deciderIgnoresBriefFocusFlicker();
  void deciderResumesAfterStableNonExcluded();
  void deciderUnknownForegroundKeepsState();
  void deciderUnknownResetsResumeTimer();
  void watcherStartupDetectsForeground();
};
