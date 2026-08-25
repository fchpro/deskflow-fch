/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class MouseMoveCoalescerTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void disabled_passesEveryDelta();
  void firstDelta_sentImmediately();
  void withinInterval_accumulatesPending();
  void afterInterval_sendsAccumulatedSum();
  void flush_releasesPendingAndResetsInterval();
  void timeUntilFlush_countsDown();
  void reset_dropsPending();
  void rateLimit_1000HzInputTo250HzOutput();
};
