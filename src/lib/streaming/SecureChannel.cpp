// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SecureChannel.h"
#include <QMessageAuthenticationCode>
#include <QSslConfiguration>
#include <QSslKey>
#include <mutex>
#include <openssl/ssl.h>

namespace deskflow::streaming {
namespace {
std::mutex bindingsMutex;
QList<std::weak_ptr<InputBinding>> bindings;
QByteArray nonce()
{
  return QByteArray::fromHex((randomId() + randomId()).toLatin1());
}
bool equalSecret(const QByteArray &a, const QByteArray &b)
{
  if (a.size() != b.size())
    return false;
  unsigned char difference = 0;
  for (qsizetype i = 0; i < a.size(); ++i)
    difference |= static_cast<unsigned char>(a[i] ^ b[i]);
  return difference == 0;
}
} // namespace
InputBinding::~InputBinding()
{
  secret.detach();
  volatile char *bytes = secret.data();
  for (qsizetype i = 0; i < secret.size(); ++i)
    bytes[i] = 0;
}
QByteArray inputExporter(ssl_st *ssl, const QString &clientName)
{
  if (!ssl || !SSL_is_init_finished(ssl) || clientName.isEmpty() || clientName.size() > 255)
    return {};
  QByteArray output(32, '\0');
  const auto context = QByteArray("1.8:") + clientName.toUtf8();
  constexpr auto label = "EXPORTER-Deskflow-Streaming-v1";
  if (SSL_export_keying_material(
          ssl, reinterpret_cast<unsigned char *>(output.data()), output.size(), label,
          std::char_traits<char>::length(label), reinterpret_cast<const unsigned char *>(context.constData()),
          context.size(), 1
      ) != 1)
    return {};
  return output;
}
void publishBinding(const std::shared_ptr<InputBinding> &binding)
{
  std::scoped_lock lock(bindingsMutex);
  bindings.append(binding);
}
QList<std::shared_ptr<InputBinding>> activeBindings()
{
  std::scoped_lock lock(bindingsMutex);
  QList<std::shared_ptr<InputBinding>> result;
  for (auto it = bindings.begin(); it != bindings.end();) {
    if (auto binding = it->lock(); binding && binding->active) {
      result.append(binding);
      ++it;
    } else {
      it = bindings.erase(it);
    }
  }
  return result;
}
QSslConfiguration tlsConfiguration(const QSslCertificate &certificate, const QSslKey &key)
{
  auto configuration = QSslConfiguration::defaultConfiguration();
  configuration.setProtocol(QSsl::TlsV1_3);
  configuration.setLocalCertificate(certificate);
  configuration.setPrivateKey(key);
  configuration.setPeerVerifyMode(QSslSocket::QueryPeer);
  configuration.setSslOption(QSsl::SslOptionDisableSessionTickets, true);
  return configuration;
}
void TlsListener::incomingConnection(qintptr descriptor)
{
  auto *socket = new QSslSocket(this);
  if (!socket->setSocketDescriptor(descriptor)) {
    socket->deleteLater();
    return;
  }
  socket->setSslConfiguration(configuration);
  Q_EMIT accepted(socket);
  socket->startServerEncryption();
}
SecureChannel::SecureChannel(QSslSocket *socket, Lookup lookup, QObject *parent)
    : QObject(parent),
      m_socket(socket),
      m_lookup(std::move(lookup)),
      m_nonce(nonce())
{
  socket->setParent(this);
  socket->setReadBufferSize(FrameReader::limit + 4);
  m_timeout.setSingleShot(true);
  m_timeout.setInterval(3000);
  connect(&m_timeout, &QTimer::timeout, this, &SecureChannel::close);
  connect(socket, &QSslSocket::encrypted, this, &SecureChannel::encrypted);
  connect(socket, &QSslSocket::disconnected, this, &SecureChannel::close);
  connect(socket, &QSslSocket::errorOccurred, this, [this] { close(); });
  connect(socket, &QSslSocket::readyRead, this, [this] {
    if (!m_socket->isEncrypted() ||
        !m_reader.feed(m_socket->read(FrameReader::limit + 4), [this](const auto &value) { receive(value); }))
      close();
  });
  m_timeout.start();
}
void SecureChannel::encrypted()
{
  const auto id = QString::fromLatin1(m_socket->peerCertificate().digest(QCryptographicHash::Sha256).toHex());
  m_binding = m_lookup(id);
  const auto local = QString::fromLatin1(m_socket->localCertificate().digest(QCryptographicHash::Sha256).toHex());
  if (!m_binding || !m_binding->active || m_binding->secret.size() != 32 || m_binding->peerId != id ||
      m_binding->localId != local || m_socket->sessionProtocol() != QSsl::TlsV1_3) {
    close();
    return;
  }
  write(message("Challenge", {{"nonce", QString::fromLatin1(m_nonce.toHex())}, {"generation", m_binding->generation}}));
}
QByteArray
SecureChannel::proof(const QByteArray &senderNonce, const QByteArray &receiverNonce, const QString &senderId) const
{
  const auto receiverId = senderId == m_binding->localId ? m_binding->peerId : m_binding->localId;
  return QMessageAuthenticationCode::hash(
      "Deskflow-streaming-proof-v1\0" + senderNonce + receiverNonce + senderId.toLatin1() + receiverId.toLatin1() +
          m_binding->generation.toLatin1(),
      m_binding->secret, QCryptographicHash::Sha256
  );
}
void SecureChannel::receive(const QJsonObject &frame)
{
  if (m_closed || !m_binding || !m_binding->active) {
    close();
    return;
  }
  const auto type = frame["type"].toString();
  const auto data = frame["data"].toObject();
  if (!m_authenticated) {
    if (type == "Challenge" && m_remoteNonce.isEmpty() && fields(data, {"nonce", "generation"}) &&
        identifier(data["nonce"].toString(), 64) && data["generation"] == m_binding->generation) {
      m_remoteNonce = QByteArray::fromHex(data["nonce"].toString().toLatin1());
      write(
          message("Proof", {{"hmac", QString::fromLatin1(proof(m_nonce, m_remoteNonce, m_binding->localId).toHex())}})
      );
      return;
    }
    if (type == "Proof" && m_remoteNonce.size() == 32 && fields(data, {"hmac"}) &&
        identifier(data["hmac"].toString(), 64) &&
        equalSecret(
            QByteArray::fromHex(data["hmac"].toString().toLatin1()), proof(m_remoteNonce, m_nonce, m_binding->peerId)
        )) {
      m_authenticated = true;
      m_timeout.stop();
      Q_EMIT established();
      return;
    }
    close();
    return;
  }
  if (type != "Authenticated" || !fields(data, {"sequence", "message"}) ||
      data["sequence"].toDouble() != m_receiveSequence + 1 || m_receiveSequence >= 9007199254740990LL ||
      !data["message"].isObject() || !validMessage(data["message"].toObject())) {
    close();
    return;
  }
  ++m_receiveSequence;
  Q_EMIT received(data["message"].toObject());
}
bool SecureChannel::write(const QJsonObject &frame)
{
  const auto bytes = FrameReader::encode(frame);
  if (m_closed || bytes.isEmpty() || m_socket->bytesToWrite() + bytes.size() > 2 * FrameReader::limit ||
      m_socket->write(bytes) != bytes.size()) {
    close();
    return false;
  }
  return true;
}
bool SecureChannel::send(const QJsonObject &frame)
{
  if (!m_authenticated || !m_binding || !m_binding->active || !validMessage(frame) ||
      m_sendSequence >= 9007199254740990LL)
    return false;
  return write(message("Authenticated", {{"sequence", ++m_sendSequence}, {"message", frame}}));
}
void SecureChannel::close()
{
  if (m_closed)
    return;
  m_closed = true;
  m_authenticated = false;
  m_timeout.stop();
  m_socket->abort();
  Q_EMIT ended();
}
} // namespace deskflow::streaming
