// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/streaming/StreamDialog.h"
#include "gui/streaming/SenderWorker.h"
#include "streaming/SessionBroker.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QJsonArray>
#include <QLabel>
#include <QLocalServer>
#include <QListWidget>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QTabBar>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
using namespace deskflow::streaming;
namespace deskflow::gui {
namespace {
const QString local(64, 'a'), remote(64, 'b'), generation(32, 'c'), source(32, 'd'), session(32, 'e');
QJsonObject inventory() {
  return {{"built", true}, {"connected", true}, {"id", local}, {"address", "127.0.0.1"},
    {"peers", QJsonArray{QJsonObject{{"id", remote}, {"name", "Owned receiver"}, {"address", "127.0.0.1"},
      {"capabilities", QJsonObject{{"receive", true}, {"audio", QJsonArray{"off", "file", "system", "application"}}}}}}},
    {"sources", QJsonArray{QJsonObject{{"id", source}, {"kind", "window"}, {"available", true}}}}};
}
QJsonObject selection() {
  return {{"to", remote}, {"kind", "window"}, {"source", source}, {"audio", "off"}, {"preset", "low"}, {"interactive", false}};
}
void save(QWidget &widget, const QString &name) {
  const auto path = qEnvironmentVariable("STREAMING_SENDER_PROOF");
  if (!path.isEmpty() && !widget.grab().save(path + "/" + name + ".png")) qFatal("Screenshot save failed");
}
int receiverEndpoint(QCoreApplication &app) {
  QLocalSocket socket; FrameReader reader; MediaTransport media;
  QString acceptedSession, acceptedSource; bool ready = false; int decoded = 0;
  qint64 firstVideo = -1, firstAudio = -1; double energy = 0;
  QObject::connect(&media, &MediaTransport::signaling, &socket, [&](const auto &frame) { socket.write(FrameReader::encode(frame)); });
  QObject::connect(&media, &MediaTransport::failed, &app, [&](const QString &error) { qWarning() << error; app.exit(2); });
  QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&] {
    reader.feed(socket.readAll(), [&](const QJsonObject &frame) {
      const auto data = frame["data"].toObject();
      if (frame["type"] == "State" && data["state"] == "negotiating" && !media.active()) {
        acceptedSession = data["session"].toString(); acceptedSource = data["source"].toString();
        if (!media.start({acceptedSession, acceptedSource, "low", QHostAddress::LocalHost, QHostAddress::LocalHost, false, true})) app.exit(3);
      } else if (frame["type"] == "Stopped") {
        media.stop(); app.exit(0);
      } else if (QStringList{"SdpOffer", "SdpAnswer", "IceCandidate"}.contains(frame["type"].toString())) {
        if (!media.receive(frame)) app.exit(4);
      }
    });
  });
  socket.connectToServer(app.arguments()[2]); if (!socket.waitForConnected(3000)) return 5;
  QTimer poll; poll.setInterval(10);
  QObject::connect(&poll, &QTimer::timeout, &app, [&] {
    while (auto frame = media.takeVideo()) {
      ++decoded; if (firstVideo < 0) firstVideo = frame->mediaTimeNs;
      if (!ready) { ready = true; socket.write(FrameReader::encode(message("Ready", {{"session", acceptedSession}, {"source", acceptedSource}}))); }
    }
    while (auto block = media.takeAudio()) {
      if (firstAudio < 0) firstAudio = block->mediaTimeNs;
      const auto *samples = reinterpret_cast<const float *>(block->samples.constData());
      for (int i = 0; i < block->samples.size() / 4; ++i) energy += samples[i] * samples[i];
    }
    if (decoded > 0) socket.write(FrameReader::encode(message("Identity", {{"decoded", decoded}, {"energy", energy},
      {"firstVideo", double(firstVideo)}, {"firstAudio", double(firstAudio)}})));
  }); poll.start();
  QTimer::singleShot(15000, &app, [&] { app.exit(6); });
  return app.exec();
}
}
class StreamingSenderTests : public QObject {
  Q_OBJECT
  QTemporaryDir files;
private Q_SLOTS:
  void initTestCase() {
    QProcess generate;
    generate.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
      "color=c=blue:s=160x96:r=30:d=12", "-f", "lavfi", "-i", "sine=frequency=440:sample_rate=48000:duration=12",
      "-c:v", "libvpx", "-deadline", "realtime", "-c:a", "libopus", files.filePath("owned.webm")});
    if (!generate.waitForFinished(10000) || generate.exitCode() != 0) qFatal("Owned media generation failed");
  }
  void unavailableSource() {
    auto data = inventory(); auto entries = data["sources"].toArray(); auto value = entries[0].toObject();
    value["available"] = false; entries[0] = value; data["sources"] = entries;
    QVERIFY(!senderSelectionError(data, selection()).isEmpty());
  }
  void connectionStatus_data() {
    QTest::addColumn<bool>("connected"); QTest::newRow("idle") << false; QTest::newRow("ready") << true;
  }
  void connectionStatus() {
    QFETCH(bool, connected); SenderWorker worker; QSignalSpy status(&worker, &SenderWorker::status);
    worker.connection(false); if (connected) worker.connection(true);
    QVERIFY(!status.last()[0].toString().contains("lost", Qt::CaseInsensitive));
  }
  void validation_data() {
    QTest::addColumn<QString>("fault");
    for (const char *name : {"valid", "build", "connection", "busy", "peer", "source", "file", "audio", "application", "endpoint", "quality", "control"})
      QTest::newRow(name) << QString(name);
  }
  void validation() {
    QFETCH(QString, fault); auto data = inventory(), choice = selection();
    if (fault == "build") data["built"] = false;
    if (fault == "connection") data["connected"] = false;
    if (fault == "busy") data["busy"] = true;
    if (fault == "peer") choice["to"] = QString(64, 'f');
    if (fault == "source") choice["source"] = QString(32, 'f');
    if (fault == "file") choice["kind"] = "file";
    if (fault == "audio") choice["audio"] = "unknown";
    if (fault == "application") choice["audio"] = "application";
    if (fault == "endpoint") { data["systemAudio"] = true; data["endpointRequired"] = true; choice["audio"] = "system"; }
    if (fault == "quality") choice["preset"] = "unknown";
    if (fault == "control") choice["interactive"] = true;
    QCOMPARE(senderSelectionError(data, choice).isEmpty(), fault == "valid");
  }
  void preservesPausedFile() {
    SenderWorker worker; worker.m_session = session; worker.m_source = source;
    worker.m_media = std::make_unique<MediaTransport>();
    if (!worker.m_media->start({session, source, "low", QHostAddress::LocalHost, QHostAddress::LocalHost, true, false})) qFatal("Media fixture failed");
    worker.m_file = std::make_unique<FileSource>();
    if (!worker.m_file->open(files.filePath("owned.webm"), session, source, false)) qFatal("File fixture failed");
    if (!QTest::qWaitFor([&] { return worker.m_file->state() == FilePlaybackState::Paused; }, 3000)) qFatal("File preroll missing");
    worker.poll();
    QCOMPARE(worker.m_file->state(), FilePlaybackState::Paused);
    worker.stop({}, false);
  }
  void hundredMillisecondAudio() {
    SenderWorker worker;
    AudioBlock block{QByteArray(4800 * 8, '\0'), session, source, 0, 0, 100 * GST_MSECOND, 0};
    worker.queueAudio(block);
    // A new input chunk must retain its initial PTS instead of dropping 40 ms before first submission.
    QCOMPARE(worker.m_pendingAudio.front().mediaTimeNs, qint64(0));
  }
  void audioChunks_data() {
    QTest::addColumn<QString>("aspect");
    for (const char *name : {"count", "samples", "pts", "capture", "file-capture", "duration", "backpressure", "bound"}) QTest::newRow(name) << QString(name);
  }
  void audioChunks() {
    QFETCH(QString, aspect); SenderWorker worker;
    AudioBlock block{QByteArray(4800 * 8, '\0'), session, source, 0, 0, 100 * GST_MSECOND, 0};
    if (aspect == "capture") block.captureTimeNs = 100 * GST_MSECOND;
    for (int i = 0; i < block.samples.size(); ++i) block.samples[i] = char(i % 127);
    worker.queueAudio(block);
    if (aspect == "backpressure") {
      worker.drainAudio([](const AudioBlock &) { return false; });
      QCOMPARE(worker.m_pendingAudio.empty() ? QByteArray{} : worker.m_pendingAudio.front().samples, block.samples); return;
    }
    if (aspect == "bound") { worker.queueAudio(block); QCOMPARE(worker.m_pendingAudio.size(), size_t(1)); return; }
    QList<AudioBlock> chunks; worker.drainAudio([&](const AudioBlock &part) { chunks.append(part); return true; });
    if (aspect == "count") QCOMPARE(chunks.size(), 5);
    if (chunks.size() != 5) qFatal("Five chunks required for offset checks");
    if (aspect == "samples") { QByteArray combined; for (const auto &part : chunks) combined += part.samples; QCOMPARE(combined, block.samples); }
    if (aspect == "pts") QCOMPARE(chunks.last().mediaTimeNs, qint64(80 * GST_MSECOND));
    if (aspect == "capture") QCOMPARE(chunks.last().captureTimeNs, qint64(180 * GST_MSECOND));
    if (aspect == "file-capture") QCOMPARE(chunks.last().captureTimeNs, qint64(0));
    if (aspect == "duration") QCOMPARE(chunks.last().durationNs, qint64(20 * GST_MSECOND));
  }
  void launcherStructure() {
    SessionClient client; SenderController controller(&client); StreamLauncher launcher(&controller);
    launcher.resize(760, 90); launcher.show();
    auto *button = launcher.findChild<QPushButton *>("streamButton");
    if (!button) qFatal("Launcher button missing");
    button->click();
    auto *dialog = launcher.findChild<StreamDialog *>();
    QTRY_VERIFY_WITH_TIMEOUT(dialog && dialog->isVisible(), 3000);
    save(launcher, "launcher-component"); save(*dialog, "disconnected-selector");
  }
  void uiSafety_data() {
    QTest::addColumn<QString>("aspect");
    for (const char *name : {"start", "view", "control", "plain", "tabs", "audio"}) QTest::newRow(name) << QString(name);
  }
  void uiSafety() {
    QFETCH(QString, aspect); SessionClient client; SenderController controller(&client); StreamDialog dialog(&controller);
    if (aspect == "start") QVERIFY(!dialog.findChild<QPushButton *>("startStream")->isEnabled());
    if (aspect == "view") QVERIFY(dialog.findChild<QCheckBox *>("viewingOnly")->isChecked());
    if (aspect == "control") QVERIFY(!dialog.findChild<QCheckBox *>("interactiveControl")->isEnabled());
    if (aspect == "plain") QCOMPARE(dialog.findChild<QLabel *>("streamPreview")->textFormat(), Qt::PlainText);
    if (aspect == "tabs") QCOMPARE(dialog.findChild<QTabBar *>("sourceTabs")->count(), 3);
    if (aspect == "audio") QCOMPARE(dialog.findChild<QComboBox *>("streamAudio")->currentData().toString(), QString("off"));
  }
  void fileSelectionInformation() {
    SessionClient client; SenderController controller(&client); StreamDialog dialog(&controller);
    dialog.findChild<QTabBar *>("sourceTabs")->setCurrentIndex(2);
    dialog.selectLocalFile(files.filePath("owned.webm")); dialog.show();
    save(dialog, "file-selection-information");
    QVERIFY(dialog.findChild<QLabel *>("streamPreview")->text().contains("owned.webm"));
  }
  void realFileWorkflow() {
    PrivateIpcServer ipc; SessionClient client; SessionBroker broker; QLocalServer receiverServer;
    const auto endpoint = randomId(), receiverName = randomId();
    if (!ipc.listen(endpoint) || !receiverServer.listen(receiverName)) qFatal("Private endpoints unavailable");
    QElapsedTimer clock; clock.start(); QTimer expiry; expiry.setInterval(100);
    connect(&expiry, &QTimer::timeout, &broker, [&] { broker.expire(clock.elapsed()); }); expiry.start();
    QLocalSocket *receiverSocket = nullptr; FrameReader receiverReader; QJsonObject offer, probe;
    connect(&receiverServer, &QLocalServer::newConnection, this, [&] {
      receiverSocket = receiverServer.nextPendingConnection();
      connect(receiverSocket, &QLocalSocket::readyRead, this, [&] {
        receiverReader.feed(receiverSocket->readAll(), [&](const QJsonObject &frame) {
          if (frame["type"] == "Identity") { probe = frame["data"].toObject(); return; }
          if (!broker.dispatch(remote, generation, frame, clock.elapsed()) && !broker.sessions().isEmpty()) qFatal("Receiver broker rejected signaling");
        });
      });
    });
    const QJsonObject caps{{"sources", QJsonArray{"file"}}, {"receive", true}, {"audio", QJsonArray{"off", "file"}}, {"control", false}};
    connect(&broker, &SessionBroker::deliver, this, [&](const QString &peer, const QJsonObject &frame) {
      if (peer == local) ipc.send(frame);
      else if (frame["type"] == "Offer") offer = frame["data"].toObject();
      else if (receiverSocket) receiverSocket->write(FrameReader::encode(frame));
    });
    connect(&ipc, &PrivateIpcServer::attachedChanged, this, [&](bool attached) {
      if (!attached) { broker.detach(local); return; }
      ipc.send(message("Identity", {{"id", local}, {"address", "127.0.0.1"}}));
      if (!broker.attach({local, "Owned sender", generation, QHostAddress::LocalHost, caps}) ||
          !broker.attach({remote, "Owned receiver", generation, QHostAddress::LocalHost, caps})) qFatal("Broker fixture attach failed");
    });
    connect(&ipc, &PrivateIpcServer::received, this, [&](const QJsonObject &frame) {
      if (!broker.dispatch(local, generation, frame, clock.elapsed())) qFatal("Sender broker rejected signaling");
    });
    SenderController controller(&client); StreamLauncher launcher(&controller); launcher.resize(760, 95); launcher.show();
    StreamDialog dialog(&controller); dialog.show();
    client.start(endpoint);
    QProcess receiver;
    receiver.start(QCoreApplication::applicationFilePath(), {"--receiver", receiverName});
    if (!QTest::qWaitFor([&] { return receiverSocket && controller.inventory()["id"] == local &&
      controller.inventory()["peers"].toArray().size() == 2; }, 3000)) qFatal("Authenticated IPC fixture not ready");
    dialog.findChild<QListWidget *>("streamSources")->setCurrentRow(0);
    save(dialog, "real-source-information");
    dialog.findChild<QTabBar *>("sourceTabs")->setCurrentIndex(2);
    dialog.selectLocalFile(files.filePath("owned.webm"));
    dialog.findChild<QComboBox *>("streamDestination")->setCurrentIndex(1);
    dialog.findChild<QComboBox *>("streamAudio")->setCurrentIndex(1);
    dialog.findChild<QComboBox *>("streamQuality")->setCurrentIndex(0);
    auto *start = dialog.findChild<QPushButton *>("startStream");
    QTRY_VERIFY_WITH_TIMEOUT(start->isEnabled(), 3000);
    QSignalSpy pixels(&controller, &SenderController::preview);
    save(dialog, "file-selected"); start->click();
    QTRY_VERIFY_WITH_TIMEOUT(!offer.isEmpty(), 3000);
    // Barrier: another explicit broker event has traversed IPC while consent is pending.
    ipc.send(message("Identity", {{"id", local}, {"address", "127.0.0.1"}}));
    QTRY_VERIFY_WITH_TIMEOUT(controller.active(), 3000);
    QCOMPARE(broker.sessions().first().state, QString("awaitingConsent"));
    QCOMPARE(pixels.count(), 0);
    save(dialog, "awaiting-consent");
    if (!broker.dispatch(remote, generation, message("Accept", {{"session", offer["session"]}, {"source", offer["source"]}}), clock.elapsed())) qFatal("Explicit receiver consent failed");
    QTRY_VERIFY_WITH_TIMEOUT(probe["decoded"].toInt() >= 10, 10000);
    QTRY_VERIFY_WITH_TIMEOUT(probe["energy"].toDouble() > 0.1, 3000);
    QTRY_VERIFY_WITH_TIMEOUT(controller.status().startsWith("Streaming"), 3000);
    QVERIFY(dialog.findChild<QLabel *>("streamPreview")->pixmap().height() <= dialog.findChild<QLabel *>("streamPreview")->contentsRect().height());
    save(dialog, "live-file-preview"); save(launcher, "running-launcher-component");
    qInfo() << "Real private IPC + broker + separate DTLS receiver decoded" << probe["decoded"].toInt()
      << "audio energy" << probe["energy"].toDouble() << "first video/audio PTS ns" << probe["firstVideo"] << probe["firstAudio"];
    QVERIFY(probe["firstVideo"].toDouble() < 100 * GST_MSECOND);
    QVERIFY(probe["firstAudio"].toDouble() < 100 * GST_MSECOND);
    dialog.findChild<QPushButton *>("stopStream")->click();
    QTRY_VERIFY_WITH_TIMEOUT(!controller.active(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(broker.sessions().isEmpty(), 3000);
    QTRY_VERIFY_WITH_TIMEOUT(receiver.state() == QProcess::NotRunning, 3000);
    QCOMPARE(receiver.exitCode(), 0); save(dialog, "stopped-selector");
    client.shutdown();
  }
};
} // namespace deskflow::gui
int main(int argc, char **argv) {
  if (argc > 1 && QByteArray(argv[1]) == "--receiver") {
    QCoreApplication app(argc, argv); return deskflow::gui::receiverEndpoint(app);
  }
  QApplication app(argc, argv); deskflow::gui::StreamingSenderTests test; return QTest::qExec(&test, argc, argv);
}
#include "StreamingSenderTests.moc"
