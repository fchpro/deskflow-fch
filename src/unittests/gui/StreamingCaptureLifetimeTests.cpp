// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/streaming/SenderWorker.h"
#include <QSignalSpy>
#include <QTest>

using namespace deskflow::streaming;
namespace deskflow::gui {
namespace {
struct Observations {
  bool destroyed = false;
  int statusReadsDuringDestruction = 0;
};
// Native boundary: MacCapture emits statusChanged while its destructor stops it.
class DestructionCapture final : public CaptureDevice {
public:
  explicit DestructionCapture(Observations &observations) : m_observations(observations) {}
  ~DestructionCapture() override { m_destroying = true; stop(); m_observations.destroyed = true; }
  QVector<CaptureSource> sources() override { return {}; }
  bool start(const QString &, const QString &, int) override { return false; }
  void stop() override { Q_EMIT statusChanged(); }
  std::optional<VideoFrame> takeFrame() override { return {}; }
  CaptureStatus status() const override {
    if (m_destroying) ++m_observations.statusReadsDuringDestruction;
    return {CaptureState::Stopped, "Owned capture stopped"};
  }
private:
  Observations &m_observations;
  bool m_destroying = false;
};
}
class StreamingCaptureLifetimeTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void destructionCannotReenterWorker() {
    Observations observations;
    {
      SenderWorker worker;
      worker.m_capture = std::make_unique<DestructionCapture>(observations);
      worker.observeCapture();
    }
    QVERIFY(observations.destroyed);
    QCOMPARE(observations.statusReadsDuringDestruction, 0);
  }
  void liveCaptureStillStopsSession() {
    Observations observations;
    SenderWorker worker;
    worker.m_capture = std::make_unique<DestructionCapture>(observations);
    worker.observeCapture();
    worker.m_session = QString(32, 'a'); worker.m_source = QString(32, 'b');
    worker.m_state = "streaming"; worker.m_selection = {{"kind", "screen"}};
    QSignalSpy outgoing(&worker, &SenderWorker::outgoing);
    worker.m_capture->stop();
    QVERIFY(worker.m_session.isEmpty());
    QCOMPARE(outgoing.count(), 1);
    QCOMPARE(outgoing.first()[0].toJsonObject()["type"].toString(), QString("Stop"));
  }
};
}
QTEST_GUILESS_MAIN(deskflow::gui::StreamingCaptureLifetimeTests)
#include "StreamingCaptureLifetimeTests.moc"
