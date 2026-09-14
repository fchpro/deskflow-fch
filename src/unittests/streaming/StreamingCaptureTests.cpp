// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/Capture.h"
#include "streaming/CaptureDeadline.h"
#include <QTest>
using namespace deskflow::streaming;
class StreamingCaptureTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void firstFrameDeadlineExpires()
  {
    CaptureDeadline deadline;
    deadline.start(0);
    QVERIFY(deadline.expired(3001));
  }
  void staticSourceDoesNotExpireAfterFirstFrame()
  {
    CaptureDeadline deadline;
    deadline.start(0);
    deadline.frame(100);
    QVERIFY(!deadline.expired(10000));
  }
  void acceptsCanonicalGeneration() { QVERIFY(validCaptureGeneration("0123456789abcdef0123456789abcdef")); }
  void rejectsShortGeneration() { QVERIFY(!validCaptureGeneration("0123456789abcdef")); }
  void rejectsUppercaseGeneration() { QVERIFY(!validCaptureGeneration("0123456789ABCDEF0123456789ABCDEF")); }
  void rejectsNonHexGeneration() { QVERIFY(!validCaptureGeneration(QString(32, 'g'))); }
  void unknownSourceNeverStarts()
  {
    auto capture = createCaptureDevice();
    QVERIFY(!capture->start(QString(32, 'a'), QString(32, 'b')));
  }
  void unknownSourceHasExplicitState()
  {
    auto capture = createCaptureDevice();
    capture->start(QString(32, 'a'), QString(32, 'b'));
    QCOMPARE(capture->status().state, CaptureState::SourceGone);
  }
  void stopResetsStatus()
  {
    auto capture = createCaptureDevice();
    capture->start(QString(32, 'a'), QString(32, 'b'));
    capture->stop();
    QCOMPARE(capture->status().state, CaptureState::Stopped);
  }
};
QTEST_GUILESS_MAIN(StreamingCaptureTests)
#include "StreamingCaptureTests.moc"
