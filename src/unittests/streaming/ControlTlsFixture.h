// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "streaming/Service.h"
#include "common/StreamingInputGate.h"
#include <QTest>
#include <QFile>
#include <QSslKey>
#include <QTemporaryDir>
#include <openssl/pem.h>
#include <openssl/ssl.h>
using namespace deskflow::streaming;
namespace control_fixture {
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

class ChannelFixture {
public:
  Identity serverIdentity,clientIdentity;Pair pair;
  QTemporaryDir directory;std::unique_ptr<Service> service;SessionClient local;
  QPointer<SecureChannel> remote;
  QList<QJsonObject> localMessages,remoteMessages;
  QString session=randomId(),source=randomId(),lease=randomId();
  int sequence=0;bool localSource=true;
  explicit ChannelFixture(std::unique_ptr<NativeControl> native,bool interactive=true,bool sourceIsLocal=true):pair(serverIdentity,clientIdentity),localSource(sourceIsLocal){
    controlOwnsInput=false;viewerOwnsInput=false;ordinaryInputLocal=true;
    if(!directory.isValid())qFatal("Temporary control TLS directory unavailable");
    serverIdentity.save(directory.filePath("identity.pem"));
    QTcpServer port;if(!port.listen(QHostAddress::LocalHost,0))qFatal("Loopback port allocation failed");
    pair.server->brokerPort=port.serverPort();port.close();publishBinding(pair.server);
    const auto endpoint=privateEndpoint()+"-control-"+randomId();
    service=std::make_unique<Service>(directory.filePath("identity.pem"),"Source",nullptr,endpoint,std::move(native));
    QObject::connect(&local,&SessionClient::received,&local,[this](const QJsonObject &m){localMessages.append(m);});
    local.start(endpoint);
    wait([&]{return has(localMessages,"Roster");},"local credential-checked broker attachment");
    auto *socket=new QSslSocket;socket->setSslConfiguration(tlsConfiguration(clientIdentity.qtCertificate,clientIdentity.qtKey));
    remote=new SecureChannel(socket,[this](const QString &id){return id==pair.client->peerId?pair.client:std::shared_ptr<InputBinding>{};});
    QObject::connect(remote,&SecureChannel::received,remote,[this](const QJsonObject &m){remoteMessages.append(m);});
    socket->connectToHostEncrypted("127.0.0.1",pair.server->brokerPort);
    wait([&]{return remote->authenticated();},"TLS input-exporter authentication");
    const QJsonObject caps{{"sources",QJsonArray{"window"}},{"receive",true},{"audio",QJsonArray{"off"}},{"control",true}};
    remote->send(message("Capabilities",caps));local.send(message("Capabilities",caps));
    wait([&]{for(const auto &m:localMessages)if(m["type"]=="Roster" && m["data"].toObject()["peers"].toArray().size()==2)return true;return false;},"two authenticated peers");
    sendSource(message("Offer",{{"session",session},{"source",source},{"to",localSource?clientIdentity.id():serverIdentity.id()},{"kind","window"},{"audio","off"},{"preset","balanced"},{"interactive",interactive}}));
    wait([&]{return has(receiverMessages(),"Offer");},"authenticated offer");
    sendReceiver(message("Accept",identity()));wait([&]{return has(senderMessages(),"Accept");},"explicit receiver consent");
    const QString fp=QString("AA:").repeated(31)+"AA";
    auto sdp=identity();sdp["fingerprint"]=fp;sdp["sdp"]="v=0\r\na=group:BUNDLE 0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtcp-mux\r\na=fingerprint:sha-256 "+fp+"\r\n";
    sendSource(message("SdpOffer",sdp));wait([&]{return has(receiverMessages(),"SdpOffer");},"broker SDP offer");
    sendReceiver(message("SdpAnswer",sdp));wait([&]{return has(senderMessages(),"SdpAnswer");},"broker SDP answer");
    local.send(message("Ready",identity()));remote->send(message("Ready",identity()));
    wait([&]{for(const auto &m:localMessages)if(m["type"]=="State" && m["data"].toObject()["state"]=="streaming")return true;return false;},"bilateral ready");
  }
  ~ChannelFixture(){local.shutdown();pair.server->active=false;pair.client->active=false;delete remote;service.reset();}
  static bool has(const QList<QJsonObject> &messages,const QString &type){for(const auto &m:messages)if(m["type"]==type)return true;return false;}
  static void wait(const std::function<bool()> &signal,const char *name){if(!QTest::qWaitFor(signal,3000))qFatal("Missing signal: %s",name);}
  QList<QJsonObject> &senderMessages(){return localSource?localMessages:remoteMessages;}
  QList<QJsonObject> &receiverMessages(){return localSource?remoteMessages:localMessages;}
  bool sendSource(const QJsonObject &event){return localSource?local.send(event):remote->send(event);}
  bool sendReceiver(const QJsonObject &event){return localSource?remote->send(event):local.send(event);}
  QJsonObject identity()const{return {{"session",session},{"source",source}};}
  QJsonObject geometry()const{auto data=identity();data["frame"]=1;data["epoch"]=0;data["geometry"]=1;data["x"]=0;data["y"]=0;data["width"]=640;data["height"]=400;data["valid"]=true;return data;}
  QJsonObject input(){auto data=identity();data["lease"]=lease;data["sequence"]=++sequence;data["frame"]=1;data["epoch"]=0;data["geometry"]=1;data["kind"]="key";data["code"]=65;data["down"]=true;return data;}
  void target(QJsonObject targetData={},QJsonObject mapping={}){targetData["session"]=session;targetData["source"]=source;if(mapping.isEmpty())mapping=geometry();mapping["session"]=session;mapping["source"]=source;if(localSource)local.send(message("ControlTarget",targetData));sendSource(message("ControlGeometry",mapping));wait([&]{return has(receiverMessages(),"ControlGeometry");},"core-verified geometry publication");}
  void grant(){auto data=identity();data["lease"]=lease;sendSource(message("GrantControl",data));wait([&]{return has(receiverMessages(),"GrantControl");},"core-approved sender grant");}
};
}
