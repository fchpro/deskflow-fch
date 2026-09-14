// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Protocol.h"
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QTimer>

namespace deskflow::streaming {
#ifdef Q_OS_WIN
class WindowsPrivateListener;
#endif
QString privateEndpoint();
bool sameLogin(QLocalSocket *socket, bool serverEnd);
class PrivateIpcServer : public QObject
{
  Q_OBJECT
public:
  explicit PrivateIpcServer(QObject *parent = nullptr);
  ~PrivateIpcServer() override;
  bool listen(const QString &endpoint = privateEndpoint());
  bool send(const QJsonObject &message);
  bool attached() const
  {
    return !m_client.isNull();
  }
Q_SIGNALS:
  void attachedChanged(bool attached);
  void received(const QJsonObject &message);

private:
  void attachSocket(QLocalSocket *socket);
  QLocalServer m_server;
#ifdef Q_OS_WIN
  std::unique_ptr<WindowsPrivateListener> m_native;
  QTimer m_loginCheck;
#endif
  QPointer<QLocalSocket> m_client;
  FrameReader m_reader;
};
// GUI API: publish verified adapter capabilities, offer, explicitly accept/decline,
// relay SDP/candidates, and stop. Disconnection synchronously invalidates UI state.
class SessionClient : public QObject
{
  Q_OBJECT
public:
  explicit SessionClient(QObject *parent = nullptr);
  void start(const QString &endpoint = privateEndpoint());
  void shutdown();
  bool send(const QJsonObject &message);
  bool connected() const
  {
    return m_verified;
  }
Q_SIGNALS:
  void connectedChanged(bool connected);
  void received(const QJsonObject &message);

private:
  void connectNow();
  QLocalSocket m_socket;
  QTimer m_retry;
  FrameReader m_reader;
  QString m_endpoint;
  bool m_verified = false;
};
} // namespace deskflow::streaming
