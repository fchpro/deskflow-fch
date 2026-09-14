// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/PrivateIpc.h"
#include "streaming/SecureChannel.h"
#include "streaming/Service.h"
#include "streaming/SessionBroker.h"
#include "base/Log.h"
#include <QFile>
#include <QSignalSpy>
#include <QSslKey>
#include <QTemporaryDir>
#include <QTest>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>

using namespace deskflow::streaming;
namespace {
struct Identity
{
  EVP_PKEY *key = EVP_RSA_gen(2048);
  X509 *certificate = X509_new();
  QSslCertificate qtCertificate;
  QSslKey qtKey;
  Identity()
  {
    if (!key || !certificate)
      qFatal("Cannot generate TLS identity");
    X509_set_version(certificate, 2);
    ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1);
    X509_gmtime_adj(X509_get_notBefore(certificate), -60);
    X509_gmtime_adj(X509_get_notAfter(certificate), 3600);
    X509_set_pubkey(certificate, key);
    auto *name = X509_get_subject_name(certificate);
    X509_NAME_add_entry_by_txt(
        name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("localhost"), -1, -1, 0
    );
    X509_set_issuer_name(certificate, name);
    if (!X509_sign(certificate, key, EVP_sha256()))
      qFatal("Cannot sign TLS identity");
    auto *bio = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(bio, certificate);
    char *data = nullptr;
    const auto length = BIO_get_mem_data(bio, &data);
    qtCertificate = QSslCertificate(QByteArray(data, length));
    BIO_free(bio);
    bio = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(bio, key, nullptr, nullptr, 0, nullptr, nullptr);
    const auto keyLength = BIO_get_mem_data(bio, &data);
    qtKey = QSslKey(QByteArray(data, keyLength), QSsl::Rsa);
    BIO_free(bio);
  }
  ~Identity()
  {
    EVP_PKEY_free(key);
    X509_free(certificate);
  }
  QString id() const
  {
    return QString::fromLatin1(qtCertificate.digest(QCryptographicHash::Sha256).toHex());
  }
  void save(const QString &path) const
  {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(qtCertificate.toPem() + qtKey.toPem()) <= 0)
      qFatal("Cannot save ephemeral test TLS identity");
  }
};
struct Pair
{
  std::shared_ptr<InputBinding> server = std::make_shared<InputBinding>();
  std::shared_ptr<InputBinding> client = std::make_shared<InputBinding>();
  Pair(Identity &serverIdentity, Identity &clientIdentity, bool wrongContext = false)
  {
    // Real mutual TLS handshake over OpenSSL's in-memory duplex transport. The
    // sidecar below uses independent real TCP/TLS loopback sockets and these
    // actual input exporters, not a substituted authenticator.
    auto *serverContext = SSL_CTX_new(TLS_method());
    auto *clientContext = SSL_CTX_new(TLS_method());
    SSL_CTX_set_verify(serverContext, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, [](int, X509_STORE_CTX *) {
      return 1;
    });
    SSL_CTX_use_certificate(serverContext, serverIdentity.certificate);
    SSL_CTX_use_PrivateKey(serverContext, serverIdentity.key);
    SSL_CTX_use_certificate(clientContext, clientIdentity.certificate);
    SSL_CTX_use_PrivateKey(clientContext, clientIdentity.key);
    auto *serverSsl = SSL_new(serverContext);
    auto *clientSsl = SSL_new(clientContext);
    BIO *serverBio = nullptr, *clientBio = nullptr;
    if (BIO_new_bio_pair(&serverBio, 0, &clientBio, 0) != 1)
      qFatal("Cannot create TLS duplex transport");
    SSL_set_bio(serverSsl, serverBio, serverBio);
    SSL_set_bio(clientSsl, clientBio, clientBio);
    SSL_set_accept_state(serverSsl);
    SSL_set_connect_state(clientSsl);
    for (int step = 0; step < 20 && (!SSL_is_init_finished(serverSsl) || !SSL_is_init_finished(clientSsl)); ++step) {
      SSL_do_handshake(clientSsl);
      SSL_do_handshake(serverSsl);
    }
    if (!SSL_is_init_finished(serverSsl) || !SSL_is_init_finished(clientSsl))
      qFatal("Real input TLS handshake failed");
    server->secret = inputExporter(serverSsl, "client");
    client->secret = inputExporter(clientSsl, wrongContext ? "different-client" : "client");
    if (server->secret.size() != 32 || client->secret.size() != 32)
      qFatal("TLS input exporter failed");
    server->localId = client->peerId = serverIdentity.id();
    server->peerId = client->localId = clientIdentity.id();
    server->generation = client->generation = randomId();
    server->name = "Client";
    client->name = "Server";
    server->address = client->address = QHostAddress::LocalHost;
    server->localAddress = client->localAddress = QHostAddress::LocalHost;
    server->server = true;
    SSL_free(serverSsl);
    SSL_free(clientSsl);
    SSL_CTX_free(serverContext);
    SSL_CTX_free(clientContext);
  }
};
struct Harness
{
  Identity serverIdentity, clientIdentity;
  Pair pair;
  TlsListener listener;
  QPointer<SecureChannel> server;
  SecureChannel *client = nullptr;
  int established = 0, ended = 0;
  explicit Harness(const QString &scenario = {}) : pair(serverIdentity, clientIdentity, scenario == "wrongExporter")
  {
    listener.configuration = tlsConfiguration(serverIdentity.qtCertificate, serverIdentity.qtKey);
    QObject::connect(&listener, &TlsListener::accepted, &listener, [&](QSslSocket *socket) {
      server = new SecureChannel(
          socket,
          [&](const QString &id) { return id == pair.server->peerId ? pair.server : std::shared_ptr<InputBinding>{}; },
          &listener
      );
      QObject::connect(server, &SecureChannel::established, &listener, [&] { ++established; });
      QObject::connect(server, &SecureChannel::ended, &listener, [&] { ++ended; });
    });
    if (!listener.listen(QHostAddress::LocalHost, 0))
      qFatal("Cannot listen on loopback");
    if (scenario == "wrongCertificate")
      pair.server->peerId = QString(64, 'a');
    if (scenario == "staleBinding")
      pair.server->active = false;
    auto *socket = new QSslSocket;
    socket->setSslConfiguration(tlsConfiguration(clientIdentity.qtCertificate, clientIdentity.qtKey));
    client = new SecureChannel(
        socket,
        [&](const QString &id) { return id == pair.client->peerId ? pair.client : std::shared_ptr<InputBinding>{}; },
        &listener
    );
    QObject::connect(client, &SecureChannel::established, &listener, [&] { ++established; });
    QObject::connect(client, &SecureChannel::ended, &listener, [&] { ++ended; });
    socket->connectToHostEncrypted("127.0.0.1", listener.serverPort());
  }
};
} // namespace
class StreamingTransportTests : public QObject
{
  Q_OBJECT
  Log m_log;
private Q_SLOTS:
  void initTestCase()
  {
    qInfo() << "Qt TLS backend:" << QSslSocket::activeBackend() << "available:" << QSslSocket::availableBackends();
  }
  void tlsNegotiation()
  {
    Harness harness;
    QTRY_COMPARE_WITH_TIMEOUT(harness.established, 2, 3000);
  }
  void tlsRejected_data()
  {
    QTest::addColumn<QString>("scenario");
    for (const auto *scenario : {"wrongExporter", "wrongCertificate", "staleBinding"})
      QTest::newRow(scenario) << QString(scenario);
  }
  void tlsRejected()
  {
    QFETCH(QString, scenario);
    Harness harness(scenario);
    QSignalSpy rejected(&harness.listener, &QTcpServer::acceptError);
    // Wait for the actual close signal, never a guessed sleep.
    QTRY_VERIFY_WITH_TIMEOUT(harness.ended > 0, 3000);
    QCOMPARE(harness.established, 0);
  }
  void encryptedDelivery()
  {
    Harness harness;
    if (!QTest::qWaitFor([&] { return harness.established == 2; }, 3000))
      qFatal("TLS fixture handshake failed");
    QSignalSpy received(harness.server, &SecureChannel::received);
    harness.client->send(message("Stop", {{"session", randomId()}, {"source", randomId()}}));
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), 1, 3000);
  }
  void disconnectInvalidatesSend()
  {
    Harness harness;
    if (!QTest::qWaitFor([&] { return harness.established == 2; }, 3000))
      qFatal("TLS fixture handshake failed");
    harness.pair.client->active = false;
    QVERIFY(!harness.client->send(message("Stop")));
  }
  void privateConsent()
  {
    PrivateIpcServer core;
    SessionClient gui;
    const auto endpoint = privateEndpoint() + "-test-" + randomId();
    if (!core.listen(endpoint))
      qFatal("Private IPC fixture could not listen");
    QSignalSpy received(&core, &PrivateIpcServer::received);
    gui.start(endpoint);
    if (!QTest::qWaitFor([&] { return gui.connected() && core.attached(); }, 3000))
      qFatal("Credential-checked private IPC fixture failed");
    gui.send(message("Accept", {{"session", randomId()}, {"source", randomId()}}));
    QTRY_COMPARE_WITH_TIMEOUT(received.size(), 1, 3000);
  }
  void serviceConsent()
  {
    Identity serverIdentity, clientIdentity;
    Pair pair(serverIdentity, clientIdentity);
    QTcpServer reservation;
    if (!reservation.listen(QHostAddress::LocalHost, 0))
      qFatal("Cannot reserve ephemeral broker port");
    pair.server->brokerPort = pair.client->brokerPort = reservation.serverPort();
    reservation.close();
    QTemporaryDir temporary;
    const auto serverPem = temporary.filePath("server.pem"), clientPem = temporary.filePath("client.pem");
    serverIdentity.save(serverPem);
    clientIdentity.save(clientPem);
    const auto serverEndpoint = privateEndpoint() + "-server-" + randomId();
    const auto clientEndpoint = privateEndpoint() + "-client-" + randomId();
    Service server(serverPem, "Server", nullptr, serverEndpoint);
    Service client(clientPem, "Client", nullptr, clientEndpoint);
    SessionClient sender, receiver;
    const auto capabilities = QJsonObject{
        {"sources", QJsonArray{"screen"}}, {"receive", true}, {"audio", QJsonArray{"off"}}, {"control", false}
    };
    int negotiating = 0;
    bool visibleOffer = false, rosterReady = false;
    QObject::connect(&receiver, &SessionClient::received, &receiver, [&](const QJsonObject &frame) {
      const auto data = frame["data"].toObject();
      if (frame["type"] == "Offer") {
        visibleOffer = true;
        receiver.send(message("Accept", {{"session", data["session"]}, {"source", data["source"]}}));
      }
      if (frame["type"] == "State" && data["state"] == "negotiating")
        ++negotiating;
    });
    QObject::connect(&sender, &SessionClient::received, &sender, [&](const QJsonObject &frame) {
      const auto data = frame["data"].toObject();
      if (frame["type"] == "Roster" && data["peers"].toArray().size() == 2) {
        bool ready = true;
        for (const auto &peer : data["peers"].toArray())
          ready &= peer.toObject()["capabilities"].toObject()["receive"].toBool();
        rosterReady = ready;
      }
      if (frame["type"] == "State" && data["state"] == "negotiating")
        ++negotiating;
    });
    sender.start(serverEndpoint);
    receiver.start(clientEndpoint);
    if (!QTest::qWaitFor([&] { return sender.connected() && receiver.connected(); }, 3000))
      qFatal("GUI/core IPC did not connect");
    sender.send(message("Capabilities", capabilities));
    receiver.send(message("Capabilities", capabilities));
    publishBinding(pair.server);
    publishBinding(pair.client);
    if (!QTest::qWaitFor([&] { return rosterReady; }, 3000))
      qFatal("Real broker did not publish compatible authenticated peers");
    sender.send(message(
        "Offer", {{"session", randomId()},
                  {"to", clientIdentity.id()},
                  {"source", randomId()},
                  {"kind", "screen"},
                  {"audio", "off"},
                  {"preset", "balanced"},
                  {"interactive", false}}
    ));
    QTRY_VERIFY_WITH_TIMEOUT(visibleOffer && negotiating == 2, 3000);
    pair.server->active = pair.client->active = false;
  }
};
QTEST_GUILESS_MAIN(StreamingTransportTests)
#include "StreamingTransportTests.moc"
