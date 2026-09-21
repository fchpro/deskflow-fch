// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "platform/MSWindowsMouseMotion.h"
#include "server/MouseMoveCoalescer.h"

#include <QTest>

class MSWindowsMouseMotionTests : public QObject
{
  Q_OBJECT

private Q_SLOTS:
  void queuedLeftwardMotionDoesNotBounce_data()
  {
    QTest::addColumn<qint64>("interval");
    QTest::newRow("250Hz") << qint64(4000);
    QTest::newRow("unlimited") << qint64(0);
  }

  void queuedLeftwardMotionDoesNotBounce()
  {
    QFETCH(qint64, interval);
    // A 3840px Windows screen feeds a 1496px Mac to its left. All hook
    // positions below are left of center; Windows suppresses every event.
    int32_t previous = 1920;
    int32_t clientX = 1495;
    MouseMoveCoalescer coalescer(interval);
    int64_t time = 0;
    for (const int32_t position : {1904, 1919, 1919, 1918, 1919}) {
      const auto delta = windowsMouseDelta(position, previous, 1920, false);
      QVERIFY(delta < 0);
      if (const auto motion = coalescer.add(delta, 0, time)) {
        clientX += motion->dx;
        QVERIFY(clientX < 1496);
      }
      previous = position; // PRE_WARP has not been dispatched yet.
      time += 1000;
    }
    QCOMPARE(clientX, 1474);
  }

  void repeatedAndReversedMovement()
  {
    QCOMPARE(windowsMouseDelta(1919, 1919, 1920, false), -1);
    QCOMPARE(windowsMouseDelta(1920, 1919, 1920, false), 0);
    QCOMPARE(windowsMouseDelta(1921, 1919, 1920, false), 1);
    QCOMPARE(windowsMouseDelta(1079, 1064, 1080, false), -1);
    QCOMPARE(windowsMouseDelta(1081, 1096, 1080, false), 1);
    // PRE_WARP's baseline update must not change a queued event's delta.
    QCOMPARE(windowsMouseDelta(1919, 1920, 1920, false), -1);
  }

  void localMotionUsesPreviousPosition()
  {
    QCOMPARE(windowsMouseDelta(100, 110, 1920, true), -10);
    QCOMPARE(windowsMouseDelta(110, 100, 1920, true), 10);
    QCOMPARE(windowsMouseDelta(100, 100, 1920, true), 0);
    QCOMPARE(windowsMouseDelta(-101, -100, 1920, true), -1);
  }
};

QTEST_GUILESS_MAIN(MSWindowsMouseMotionTests)
#include "MSWindowsMouseMotionTests.moc"
