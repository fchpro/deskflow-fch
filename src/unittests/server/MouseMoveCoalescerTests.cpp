/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "MouseMoveCoalescerTests.h"

#include "server/MouseMoveCoalescer.h"

namespace {
constexpr int64_t kInterval = 4000; // 4 ms = 250 Hz
}

void MouseMoveCoalescerTests::disabled_passesEveryDelta()
{
  MouseMoveCoalescer c(0);
  QVERIFY(!c.enabled());
  auto a = c.add(1, 2, 0);
  auto b = c.add(3, 4, 1);
  QVERIFY(a.has_value());
  QVERIFY(b.has_value());
  QCOMPARE(a->dx, 1);
  QCOMPARE(a->dy, 2);
  QCOMPARE(b->dx, 3);
  QCOMPARE(b->dy, 4);
  QVERIFY(!c.hasPending());
}

void MouseMoveCoalescerTests::firstDelta_sentImmediately()
{
  MouseMoveCoalescer c(kInterval);
  auto a = c.add(5, -3, 1'000'000);
  QVERIFY(a.has_value());
  QCOMPARE(a->dx, 5);
  QCOMPARE(a->dy, -3);
  QVERIFY(!c.hasPending());
}

void MouseMoveCoalescerTests::withinInterval_accumulatesPending()
{
  MouseMoveCoalescer c(kInterval);
  c.add(1, 1, 0);
  auto b = c.add(2, 3, 1000);
  auto d = c.add(4, 5, 2000);
  QVERIFY(!b.has_value());
  QVERIFY(!d.has_value());
  QVERIFY(c.hasPending());
}

void MouseMoveCoalescerTests::afterInterval_sendsAccumulatedSum()
{
  MouseMoveCoalescer c(kInterval);
  c.add(1, 1, 0);
  c.add(2, 3, 1000);
  c.add(4, 5, 2000);
  auto out = c.add(10, 20, kInterval);
  QVERIFY(out.has_value());
  QCOMPARE(out->dx, 16);
  QCOMPARE(out->dy, 28);
  QVERIFY(!c.hasPending());
}

void MouseMoveCoalescerTests::flush_releasesPendingAndResetsInterval()
{
  MouseMoveCoalescer c(kInterval);
  c.add(1, 1, 0);
  c.add(7, 9, 1000);
  auto out = c.flush(3000);
  QCOMPARE(out.dx, 7);
  QCOMPARE(out.dy, 9);
  QVERIFY(!c.hasPending());
  // interval restarts at flush time: 3000 + 4000 = 7000
  QVERIFY(!c.add(1, 1, 6999).has_value());
  QVERIFY(c.add(1, 1, 7000).has_value());
}

void MouseMoveCoalescerTests::timeUntilFlush_countsDown()
{
  MouseMoveCoalescer c(kInterval);
  c.add(1, 1, 10'000);
  QCOMPARE(c.timeUntilFlushUs(10'000), kInterval);
  QCOMPARE(c.timeUntilFlushUs(11'000), kInterval - 1000);
  QCOMPARE(c.timeUntilFlushUs(14'000), 0);
  QCOMPARE(c.timeUntilFlushUs(99'000), 0);
}

void MouseMoveCoalescerTests::reset_dropsPending()
{
  MouseMoveCoalescer c(kInterval);
  c.add(1, 1, 0);
  c.add(5, 5, 1000);
  QVERIFY(c.hasPending());
  c.reset();
  QVERIFY(!c.hasPending());
  auto out = c.add(2, 2, kInterval);
  QCOMPARE(out->dx, 2);
  QCOMPARE(out->dy, 2);
}

void MouseMoveCoalescerTests::rateLimit_1000HzInputTo250HzOutput()
{
  MouseMoveCoalescer c(kInterval);
  int sent = 0;
  int64_t sumX = 0;
  int64_t sentX = 0;
  // 1 second of 1000 Hz input, 1 px per event
  for (int64_t t = 0; t < 1'000'000; t += 1000) {
    sumX += 1;
    if (auto d = c.add(1, 0, t)) {
      ++sent;
      sentX += d->dx;
    }
  }
  QCOMPARE(sent, 250);
  QCOMPARE(sentX + (c.hasPending() ? c.flush(1'000'000).dx : 0), sumX);
}

QTEST_MAIN(MouseMoveCoalescerTests)
