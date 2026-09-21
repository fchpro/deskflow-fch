// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/streaming/StreamDialog.h"
#include "gui/streaming/StreamViewer.h"
#include "gui/streaming/SenderWorker.h"
#include "streaming/SessionBroker.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QComboBox>
#include <QLabel>
#include <QJsonArray>
#include <QPushButton>
#include <QSignalSpy>
#include <QTest>
#ifdef Q_OS_MACOS
#include "streaming/Audio.h"
#include <CoreAudio/CoreAudio.h>
#endif

using namespace deskflow::streaming;
using namespace deskflow::gui;

class StreamingAutoAcceptTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
#ifdef Q_OS_MACOS
  void nativeDefaultOutputMatchesCoreAudio() {
    AudioDeviceID device = kAudioObjectUnknown;
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice,
      kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = sizeof(device);
    QCOMPARE(AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device), noErr);
    QVERIFY(device != kAudioObjectUnknown);
    address = {kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef uid = nullptr; size = sizeof(uid);
    QCOMPARE(AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &uid), noErr);
    QVERIFY(uid);
    const auto expected = QString::fromCFString(uid); CFRelease(uid);
    QString error; const auto endpoints = audioOutputEndpoints(error);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    int defaults = 0;
    for (const auto &endpoint : endpoints) if (endpoint.isDefault) {
      ++defaults; QCOMPARE(endpoint.id, expected);
      qInfo() << "Verified native default output:" << endpoint.name << endpoint.id;
    }
    QCOMPARE(defaults, 1);
  }
#endif
  void trustedOfferStartsWithoutClick() {
    const QString sender(64, 'a'), receiver(64, 'b'), generation(32, 'c');
    const auto session = randomId(), source = randomId();
    const QJsonObject caps{{"sources", QJsonArray{"screen"}}, {"receive", true},
      {"audio", QJsonArray{"off"}}, {"control", false}};
    PrivateIpcServer senderIpc, receiverIpc;
    SessionClient senderClient, receiverClient;
    SessionBroker broker;
    QElapsedTimer clock; clock.start();
    const auto senderEndpoint = randomId(), receiverEndpoint = randomId();
    QVERIFY(senderIpc.listen(senderEndpoint)); QVERIFY(receiverIpc.listen(receiverEndpoint));
    connect(&broker, &SessionBroker::deliver, this, [&](const QString &peer, const QJsonObject &frame) {
      (peer == sender ? senderIpc : receiverIpc).send(frame);
    });
    auto wire = [&](PrivateIpcServer &ipc, const QString &id) {
      connect(&ipc, &PrivateIpcServer::attachedChanged, &broker, [&, id](bool attached) {
        if (!attached) { broker.detach(id); return; }
        (id == sender ? senderIpc : receiverIpc).send(message("Identity", {{"id", id}, {"address", "127.0.0.1"}}));
        QVERIFY(broker.attach({id, id == sender ? "Owned sender" : "Owned receiver",
          generation, QHostAddress::LocalHost, caps}));
      });
      connect(&ipc, &PrivateIpcServer::received, &broker, [&, id](const QJsonObject &frame) {
        // Service consumes this core-local notification; it is not broker signaling.
        if (frame["type"] == "ViewerFocus") return;
        broker.dispatch(id, generation, frame, clock.elapsed());
      });
    };
    wire(senderIpc, sender); wire(receiverIpc, receiver);
    SenderController controller(&receiverClient);
    StreamLauncher launcher(&controller, nullptr, true); launcher.show();
    QSignalSpy received(&senderClient, &SessionClient::received);
    senderClient.start(senderEndpoint); receiverClient.start(receiverEndpoint);
    QVERIFY(QTest::qWaitFor([&] { return controller.inventory()["peers"].toArray().size() == 2; }, 3000));
    QVERIFY(senderClient.send(message("Offer", {{"session", session}, {"source", source}, {"to", receiver},
      {"kind", "screen"}, {"audio", "off"}, {"preset", "low"}, {"interactive", false},
      {"title", "Owned screen"}, {"playback", false}})));
    auto receivedType = [&](const QString &type) {
      for (const auto &arguments : received)
        if (arguments[0].toJsonObject()["type"] == type) return true;
      return false;
    };
    QVERIFY(QTest::qWaitFor([&] { return receivedType("Accept"); }, 3000));
    auto *viewer = launcher.findChild<StreamViewer *>(); QVERIFY(viewer);
    QVERIFY(viewer->isVisible());
    QVERIFY(!viewer->findChild<QPushButton *>("acceptStream")->isVisible());
    QVERIFY(QTest::qWaitFor([&] { return !viewer->findChild<QWidget *>("receiverConsent")->isVisible(); }, 3000));
    QVERIFY(!receivedType("GrantControl"));
    const auto proof = qEnvironmentVariable("STREAMING_AUTO_ACCEPT_PROOF");
    if (!proof.isEmpty()) QVERIFY(viewer->grab().save(proof));
    QTest::mouseClick(viewer->findChild<QPushButton *>("receiverStop"), Qt::LeftButton);
    QVERIFY(QTest::qWaitFor([&] { return receivedType("Stopped") && !controller.active(); }, 3000));
    senderClient.shutdown(); receiverClient.shutdown();
  }
  void repeatedAcceptanceIsIdempotent() {
    SenderWorker worker;
    QSignalSpy out(&worker, &SenderWorker::outgoing);
    worker.connection(true);
    worker.receive(message("Identity", {{"id", QString(64, 'b')}, {"address", "127.0.0.1"}}));
    auto offer = [&] {
      worker.receive(message("Offer", {{"session", randomId()}, {"source", randomId()},
        {"from", QString(64, 'a')}, {"to", QString(64, 'b')}, {"kind", "screen"},
        {"audio", "off"}, {"preset", "low"}, {"interactive", false}}));
    };
    offer(); out.clear(); worker.accept({}); worker.accept({});
    QCOMPARE(out.count(), 1);
    QCOMPARE(out.first()[0].toJsonObject()["type"].toString(), QString("Accept"));
    worker.stop();
    QCOMPARE(out.last()[0].toJsonObject()["type"].toString(), QString("Stop"));
    offer(); out.clear(); worker.accept({});
    QCOMPARE(out.count(), 1);
    QCOMPARE(out.first()[0].toJsonObject()["type"].toString(), QString("Accept"));
  }
  void defaultOutputAndMissingOutput_data() {
    QTest::addColumn<bool>("hasDefault");
    QTest::newRow("default-device") << true;
    QTest::newRow("no-default") << false;
  }
  void defaultOutputAndMissingOutput() {
    QFETCH(bool, hasDefault);
    SessionClient client; SenderController controller(&client);
    StreamViewer viewer(&controller, {{"audio", "file"}, {"endpoints", QJsonArray{
      QJsonObject{{"id", "other"}, {"name", "Other"}, {"default", false}},
      QJsonObject{{"id", "system"}, {"name", "System output"}, {"default", hasDefault}}}}}, nullptr, true);
    viewer.show();
    QCOMPARE(viewer.findChild<QComboBox *>("receiverEndpoint")->currentData().toString(),
      hasDefault ? QString("system") : QString{});
    QVERIFY(!viewer.findChild<QPushButton *>("acceptStream")->isVisible());
    if (!hasDefault) QVERIFY(viewer.findChild<QLabel *>("receiverStatus")->text().contains("Select an audio output"));
    QVERIFY(!controller.active()); // A display-only or stale offer cannot authorize a session.
  }
};
QTEST_MAIN(StreamingAutoAcceptTests)
#include "StreamingAutoAcceptTests.moc"
