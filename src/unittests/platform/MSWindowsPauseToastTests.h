/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class MSWindowsPauseToastTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  // Test are run in order top to bottom
  void showCreatesVisibleWindow();
  void windowIsBottomRightOfWorkArea();
  void windowDoesNotActivate();
  void windowClosesAfterDuration();
  void showReplacesTextWhileVisible();
};
