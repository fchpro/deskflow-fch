// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Protocol.h"
#include <QHostAddress>
#include <QSslConfiguration>
#include <QSslSocket>
#include <QTcpServer>
#include <QTimer>
#include <atomic>
#include <memory>
struct ssl_st;

namespace deskflow::streaming {
struct InputBinding
{
  QByteArray secret;
  QString localId, peerId, generation, name;
  QHostAddress address;
  QHostAddress localAddress;
  quint16 brokerPort = 24801;
  bool server = false;
  std::atomic_bool active{true};
  ~InputBinding();
};
void publishBinding(const std::shared_ptr<InputBinding> &binding);
QByteArray inputExporter(ssl_st *ssl, const QString &clientName);
QList<std::shared_ptr<InputBinding>> activeBindings();

class SecureChannel : public QObject
{
  Q_OBJECT
public:
  using Lookup = std::function<std::shared_ptr<InputBinding>(const QString &)>;
  SecureChannel(QSslSocket *socket, Lookup lookup, QObject *parent = nullptr);
  bool send(const QJsonObject &message);
  void close();
  bool authenticated() const
  {
    return m_authenticated;
  }
  std::shared_ptr<InputBinding> binding() const
  {
    return m_binding;
  }
Q_SIGNALS:
  void established();
  void received(const QJsonObject &message);
  void ended();

private:
  void encrypted();
  void receive(const QJsonObject &message);
  bool write(const QJsonObject &message);
  QByteArray proof(const QByteArray &senderNonce, const QByteArray &receiverNonce, const QString &senderId) const;
  QSslSocket *m_socket;
  Lookup m_lookup;
  std::shared_ptr<InputBinding> m_binding;
  FrameReader m_reader;
  QTimer m_timeout;
  QByteArray m_nonce, m_remoteNonce;
  bool m_authenticated = false, m_closed = false;
  qint64 m_sendSequence = 0, m_receiveSequence = 0;
};
class TlsListener : public QTcpServer
{
  Q_OBJECT
public:
  using QTcpServer::QTcpServer;
  QSslConfiguration configuration;
Q_SIGNALS:
  void accepted(QSslSocket *socket);

protected:
  void incomingConnection(qintptr descriptor) override;
};
QSslConfiguration tlsConfiguration(const QSslCertificate &certificate, const QSslKey &key);
} // namespace deskflow::streaming
