// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/SessionBroker.h"
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTest>
#include <QtEndian>
using namespace deskflow::streaming;

namespace {
QJsonObject caps()
{
  return {{"sources", QJsonArray{"screen"}}, {"receive", true}, {"audio", QJsonArray{"off"}}, {"control", false}};
}
struct Fixture
{
  SessionBroker broker;
  QString sender = QString(64, 'a'), receiver = QString(64, 'b');
  QString generation = QString(32, 'c'), session = QString(32, 'd'), source = QString(32, 'e');
  Fixture()
  {
    if (!broker.attach({sender, "Sender", generation, QHostAddress::LocalHost, caps()}) ||
        !broker.attach({receiver, "Receiver", generation, QHostAddress::LocalHost, caps()}))
      qFatal("Fixture could not attach peers");
  }
  QJsonObject identity() const
  {
    return {{"session", session}, {"source", source}};
  }
  QJsonObject offer() const
  {
    return message(
        "Offer", {{"session", session},
                  {"to", receiver},
                  {"source", source},
                  {"kind", "screen"},
                  {"audio", "off"},
                  {"preset", "balanced"},
                  {"interactive", false}}
    );
  }
  void offered()
  {
    if (!broker.dispatch(sender, generation, offer(), 100))
      qFatal("Fixture offer failed");
  }
};
} // namespace
class StreamingProtocolTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void fragmentedFrame()
  {
    FrameReader reader;
    int delivered = 0;
    const auto bytes = FrameReader::encode(message("Stop"));
    for (const auto byte : bytes) {
      if (!reader.feed(QByteArray(1, byte), [&](const auto &) { ++delivered; }))
        qFatal("Valid fragmented frame rejected");
    }
    QCOMPARE(delivered, 1);
  }
  void malformed_data()
  {
    QTest::addColumn<QByteArray>("body");
    QTest::newRow("duplicate") << QByteArray("{\"data\":{},\"type\":\"Stop\",\"v\":1,\"v\":1}");
    QTest::newRow("unknown") << QByteArray("{\"data\":{},\"type\":\"Inject\",\"v\":1}");
    QTest::newRow("version") << QByteArray("{\"data\":{},\"type\":\"Stop\",\"v\":2}");
    QTest::newRow("oversized") << QJsonDocument(
                                      message("Stop", {{"padding", QString(FrameReader::limit, 'x')}})
    ).toJson(QJsonDocument::Compact);
  }
  void malformed()
  {
    QFETCH(QByteArray, body);
    QByteArray bytes(4, '\0');
    qToBigEndian<quint32>(static_cast<quint32>(body.size()), bytes.data());
    FrameReader reader;
    QVERIFY(!reader.feed(bytes + body, [](const auto &) {}));
  }
  void consent()
  {
    Fixture fixture;
    fixture.offered();
    fixture.broker.dispatch(fixture.receiver, fixture.generation, message("Accept", fixture.identity()), 200);
    QCOMPARE(fixture.broker.sessions().first().state, QString("negotiating"));
  }
  void reject_data()
  {
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario :
         {"unauthorized", "generation", "capability", "receiverCapability", "senderConsent", "staleSource", "control",
          "replay", "busy"})
      QTest::newRow(scenario) << QString(scenario);
  }
  void reject()
  {
    QFETCH(QString, scenario);
    Fixture fixture;
    auto origin = fixture.sender;
    auto generation = fixture.generation;
    auto command = fixture.offer();
    if (scenario == "unauthorized") {
      origin = QString(64, 'f');
      command = message("Capabilities", caps());
    }
    if (scenario == "generation")
      generation = QString(32, 'f');
    if (scenario == "capability" || scenario == "receiverCapability") {
      auto supported = caps();
      supported["audio"] = QJsonArray{"off", "system"};
      fixture.broker.dispatch(
          scenario == "capability" ? fixture.receiver : fixture.sender, generation, message("Capabilities", supported),
          50
      );
      auto data = command["data"].toObject();
      data["audio"] = "system";
      command["data"] = data;
    }
    if (scenario == "replay" || scenario == "busy") {
      fixture.offered();
      if (scenario == "replay")
        fixture.broker.dispatch(origin, generation, message("Stop", fixture.identity()), 150);
      else {
        auto data = command["data"].toObject();
        data["session"] = randomId();
        command["data"] = data;
      }
    }
    if (scenario == "senderConsent" || scenario == "staleSource" || scenario == "control") {
      fixture.offered();
      command = message(scenario == "control" ? "GrantControl" : "Accept", fixture.identity());
      if (scenario == "staleSource") {
        origin = fixture.receiver;
        auto data = command["data"].toObject();
        data["source"] = QString(32, 'f');
        command["data"] = data;
      }
    }
    QVERIFY(!fixture.broker.dispatch(origin, generation, command, 200));
  }
  void sessionCleanup_data()
  {
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario : {"decline", "stop", "timeout", "disconnect"})
      QTest::newRow(scenario) << QString(scenario);
  }
  void sessionCleanup()
  {
    QFETCH(QString, scenario);
    Fixture fixture;
    fixture.offered();
    if (scenario == "decline" || scenario == "stop")
      fixture.broker.dispatch(
          fixture.receiver, fixture.generation, message(scenario == "decline" ? "Decline" : "Stop", fixture.identity()),
          200
      );
    if (scenario == "timeout")
      fixture.broker.expire(30100);
    if (scenario == "disconnect")
      fixture.broker.detach(fixture.sender);
    QVERIFY(fixture.broker.sessions().isEmpty());
  }
  void signaling_data()
  {
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<bool>("accepted");
    QTest::newRow("audioVideoSdp") << QString("audioVideoSdp") << true;
    QTest::newRow("mismatchedFingerprint") << QString("mismatchedFingerprint") << false;
    QTest::newRow("audioForbidden") << QString("audioForbidden") << false;
    QTest::newRow("hostExtensions") << QString("hostExtensions") << true;
    QTest::newRow("thirdPartyCandidate") << QString("thirdPartyCandidate") << false;
    QTest::newRow("outsidePortRange") << QString("outsidePortRange") << false;
  }
  void signaling()
  {
    QFETCH(QString, scenario);
    QFETCH(bool, accepted);
    Fixture fixture;
    if (scenario == "audioVideoSdp" || scenario == "mismatchedFingerprint") {
      auto capabilities = caps();
      capabilities["audio"] = QJsonArray{"off", "system"};
      fixture.broker.dispatch(fixture.sender, fixture.generation, message("Capabilities", capabilities), 50);
      fixture.broker.dispatch(fixture.receiver, fixture.generation, message("Capabilities", capabilities), 50);
      auto offer = fixture.offer();
      auto data = offer["data"].toObject();
      data["audio"] = "system";
      offer["data"] = data;
      if (!fixture.broker.dispatch(fixture.sender, fixture.generation, offer, 100))
        qFatal("Fixture audio offer failed");
    } else {
      fixture.offered();
    }
    if (!fixture.broker.dispatch(fixture.receiver, fixture.generation, message("Accept", fixture.identity()), 200))
      qFatal("Fixture consent failed");
    auto data = fixture.identity();
    QString type;
    if (scenario == "audioVideoSdp" || scenario == "mismatchedFingerprint" || scenario == "audioForbidden") {
      type = "SdpOffer";
      QString fingerprint;
      for (int i = 0; i < 32; ++i)
        fingerprint += (i ? ":AB" : "AB");
      // Standard bundled WebRTC SDP layout: media-scoped fingerprints occur
      // once per audio/video m-line and must all match the signaled identity.
      const auto prefix =
          QString("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\na=group:BUNDLE video0 audio1\r\n");
      const auto video = "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video0\r\na=rtcp-mux\r\na=fingerprint:sha-256 " +
                         fingerprint + "\r\na=setup:actpass\r\na=rtpmap:96 VP8/90000\r\n";
      const auto audio =
          "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio1\r\na=rtcp-mux\r\na=fingerprint:sha-256 " +
          (scenario == "mismatchedFingerprint" ? QString(fingerprint).replace("AB", "CD") : fingerprint) +
          "\r\na=setup:actpass\r\na=rtpmap:111 opus/48000/2\r\n";
      data["sdp"] = prefix + video + audio;
      data["fingerprint"] = fingerprint;
    } else {
      type = "IceCandidate";
      data["candidate"] = QString("candidate:1 1 UDP 2015363327 %1 %2 typ host generation 0 network-cost 999")
                              .arg(scenario == "thirdPartyCandidate" ? "192.0.2.1" : "127.0.0.1")
                              .arg(scenario == "outsidePortRange" ? 443 : 24802);
    }
    QCOMPARE(fixture.broker.dispatch(fixture.sender, fixture.generation, message(type, data), 300), accepted);
  }
};
QTEST_GUILESS_MAIN(StreamingProtocolTests)
#include "StreamingProtocolTests.moc"
