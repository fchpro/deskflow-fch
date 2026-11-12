/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ScreenEntryGuardTests.h"

#include "server/ScreenEntryGuard.h"

namespace {
constexpr int32_t kThreshold = 64;
// Mac client left of the PC: 1496 x 967 logical pixels.
constexpr int32_t kW = 1496;
constexpr int32_t kH = 967;
constexpr int32_t kEdge = kW - 1;

ScreenEntryGuard enteredFromRight()
{
  // PC -> Mac moving Left; going back means leaving the Mac on its Right edge.
  ScreenEntryGuard g;
  g.arm(Direction::Right, kThreshold);
  return g;
}
} // namespace

void ScreenEntryGuardTests::smallPushAtEntryEdge_held()
{
  auto g = enteredFromRight();
  QVERIFY(g.armed());
  QCOMPARE(g.backDirection(), Direction::Right);
  QVERIFY(g.holdLeave(Direction::Right, 30));
  QCOMPARE(g.accumulatedPush(), 30);
  QVERIFY(g.armed());
}

void ScreenEntryGuardTests::cumulativePushOverThreshold_releases()
{
  auto g = enteredFromRight();
  QVERIFY(g.holdLeave(Direction::Right, 30));
  QVERIFY(g.holdLeave(Direction::Right, 34)); // 64 == threshold, still held
  QVERIFY(!g.holdLeave(Direction::Right, 1)); // 65 > threshold
  QVERIFY(!g.armed());
  QVERIFY(!g.holdLeave(Direction::Right, 1)); // disarmed: normal switching
}

void ScreenEntryGuardTests::inwardMove_resetsAccumulatedPush()
{
  auto g = enteredFromRight();
  QVERIFY(g.holdLeave(Direction::Right, 40));
  g.observeInside(0); // still on the edge pixel: push kept
  QCOMPARE(g.accumulatedPush(), 40);
  g.observeInside(5);
  QCOMPARE(g.accumulatedPush(), 0);
  QVERIFY(g.armed());
  QVERIFY(g.holdLeave(Direction::Right, 40)); // 40 again, not 80
}

void ScreenEntryGuardTests::inwardThreshold_disarms()
{
  auto g = enteredFromRight();
  g.observeInside(kThreshold - 1);
  QVERIFY(g.armed());
  g.observeInside(kThreshold);
  QVERIFY(!g.armed());
  QVERIFY(!g.holdLeave(Direction::Right, 1));
}

void ScreenEntryGuardTests::otherDirections_unaffected()
{
  auto g = enteredFromRight();
  QVERIFY(!g.holdLeave(Direction::Left, 1));
  QVERIFY(!g.holdLeave(Direction::Top, 1));
  QVERIFY(!g.holdLeave(Direction::Bottom, 1));
  QVERIFY(g.armed());
  QCOMPARE(g.accumulatedPush(), 0);
}

void ScreenEntryGuardTests::zeroThreshold_neverHolds()
{
  ScreenEntryGuard g;
  g.arm(Direction::Right, 0);
  QVERIFY(!g.armed());
  QVERIFY(!g.holdLeave(Direction::Right, 1));
  g.arm(Direction::Right, -5);
  QVERIFY(!g.armed());
  QVERIFY(!g.holdLeave(Direction::Right, 1));
}

void ScreenEntryGuardTests::reset_disarms()
{
  auto g = enteredFromRight();
  QVERIFY(g.holdLeave(Direction::Right, 10));
  g.reset();
  QVERIFY(!g.armed());
  QCOMPARE(g.accumulatedPush(), 0);
  QVERIFY(!g.holdLeave(Direction::Right, 1));
}

void ScreenEntryGuardTests::distanceInsideEdge_allEdges()
{
  using enum Direction;
  QCOMPARE(ScreenEntryGuard::distanceInsideEdge(Right, kEdge, 10, 0, 0, kW, kH), 0);
  QCOMPARE(ScreenEntryGuard::distanceInsideEdge(Right, kEdge + 30, 10, 0, 0, kW, kH), -30);
  QCOMPARE(ScreenEntryGuard::distanceInsideEdge(Right, kEdge - 64, 10, 0, 0, kW, kH), 64);
  QCOMPARE(ScreenEntryGuard::distanceInsideEdge(Left, -7, 10, 0, 0, kW, kH), -7);
  QCOMPARE(ScreenEntryGuard::distanceInsideEdge(Top, 0, 12, 0, 0, kW, kH), 12);
  QCOMPARE(ScreenEntryGuard::distanceInsideEdge(Bottom, 0, kH + 3, 0, 0, kW, kH), -4);
  QCOMPARE(ScreenEntryGuard::opposite(Left), Right);
  QCOMPARE(ScreenEntryGuard::opposite(Right), Left);
  QCOMPARE(ScreenEntryGuard::opposite(Top), Bottom);
  QCOMPARE(ScreenEntryGuard::opposite(Bottom), Top);
}

void ScreenEntryGuardTests::macLeftOfPc_reversalAfterFlick()
{
  // Mirrors Server::applyMouseMoveSecondary: x beyond the right edge means
  // "leave Right"; a held leave clamps x back to the edge.
  int32_t x = kEdge; // switch PC -> Mac lands on the Mac's right edge
  auto g = enteredFromRight();
  const auto move = [&](int32_t dx) {
    x += dx;
    const int32_t inside = ScreenEntryGuard::distanceInsideEdge(Direction::Right, x, 0, 0, 0, kW, kH);
    if (inside >= 0) {
      g.observeInside(inside);
      return false;
    }
    if (g.holdLeave(Direction::Right, -inside)) {
      x = kEdge;
      return false;
    }
    return true; // would switch back to the PC
  };

  QVERIFY(!move(+30)); // gaming-mouse reversal right after the flick
  QCOMPARE(x, kEdge);
  QVERIFY(move(+40)); // deliberate push: 70 px > 64 -> back on the PC

  // after a deliberate inward move the edge behaves normally again
  x = kEdge;
  g = enteredFromRight();
  QVERIFY(!move(-kThreshold));
  QVERIFY(!g.armed());
  QVERIFY(!move(+kThreshold));
  QVERIFY(move(+1));
}

QTEST_MAIN(ScreenEntryGuardTests)
