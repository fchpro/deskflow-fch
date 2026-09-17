// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "PrivateIpc.h"
#include <QCryptographicHash>
#ifdef Q_OS_WIN
#include "WindowsPrivateIpc.h"
#else
#include <sys/socket.h>
#include <unistd.h>
#ifdef Q_OS_MACOS
#include <sys/un.h>
#endif
#endif

namespace deskflow::streaming {
namespace {
bool write(QLocalSocket *socket, const QJsonObject &frame)
{
  const auto bytes = FrameReader::encode(frame);
  if (!socket || bytes.isEmpty() || socket->bytesToWrite() + bytes.size() > 2 * FrameReader::limit)
    return false;
  return socket->write(bytes) == bytes.size();
}
#ifndef Q_OS_WIN
// macOS TMPDIR is long and sun_path holds 104 bytes; UserAccessOption adds a private
// directory level. Map the logical endpoint to a short, deterministic socket name there.
QString socketName(const QString &endpoint)
{
#ifdef Q_OS_MACOS
  if (!endpoint.isEmpty())
    return "dfs-" + QString::fromLatin1(QCryptographicHash::hash(endpoint.toUtf8(), QCryptographicHash::Sha256).toHex().left(24));
#endif
  return endpoint;
}
#endif
} // namespace
QString privateEndpoint()
{
#ifdef Q_OS_WIN
  const auto credentials = windowsInteractiveLogin().key();
#else
  const auto credentials = QByteArray::number(getuid()) + ':' + QByteArray::number(getsid(0));
#endif
  if (credentials.isEmpty())
    return {};
  return "deskflow-stream-v1-" +
         QString::fromLatin1(QCryptographicHash::hash(credentials, QCryptographicHash::Sha256).toHex());
}
bool sameLogin(QLocalSocket *socket, bool serverEnd)
{
#ifdef Q_OS_WIN
  return windowsSameLogin(socket, serverEnd);
#elif defined(Q_OS_LINUX)
  Q_UNUSED(serverEnd)
  struct ucred credential{};
  socklen_t length = sizeof(credential);
  return getsockopt(socket->socketDescriptor(), SOL_SOCKET, SO_PEERCRED, &credential, &length) == 0 &&
         credential.uid == getuid() && getsid(credential.pid) == getsid(0);
#elif defined(Q_OS_MACOS)
  Q_UNUSED(serverEnd)
  uid_t uid = 0;
  gid_t gid = 0;
  pid_t pid = 0;
  socklen_t length = sizeof(pid);
  return getpeereid(socket->socketDescriptor(), &uid, &gid) == 0 && uid == getuid() &&
         getsockopt(socket->socketDescriptor(), SOL_LOCAL, LOCAL_PEERPID, &pid, &length) == 0 &&
         getsid(pid) == getsid(0);
#else
  Q_UNUSED(socket)
  Q_UNUSED(serverEnd)
  return false;
#endif
}
PrivateIpcServer::PrivateIpcServer(QObject *parent) : QObject(parent)
{
  m_server.setSocketOptions(QLocalServer::UserAccessOption);
  m_server.setMaxPendingConnections(1);
  connect(&m_server, &QLocalServer::newConnection, this, [this] {
    attachSocket(m_server.nextPendingConnection());
  });
#ifdef Q_OS_WIN
  m_native=std::make_unique<WindowsPrivateListener>();
  m_native->accepted=[this](HANDLE pipe){
    auto *socket=new QLocalSocket(this);
    if(!socket->setSocketDescriptor(reinterpret_cast<qintptr>(pipe),QLocalSocket::ConnectedState)){
      CloseHandle(pipe);delete socket;m_native->rearm();return;
    }
    attachSocket(socket);
  };
  connect(&m_loginCheck,&QTimer::timeout,this,[this]{
    if(windowsInteractiveLogin().key()!=m_native->login().key()){
      m_native->close();m_loginCheck.stop();if(m_client)m_client->abort();
    }
  });
#endif
}
void PrivateIpcServer::attachSocket(QLocalSocket *socket)
{
    if (!socket)
      return;
    if (m_client || !sameLogin(socket, true)) {
      socket->abort();
      socket->deleteLater();
#ifdef Q_OS_WIN
      QTimer::singleShot(0,this,[this]{m_native->rearm();});
#endif
      return;
    }
    m_client = socket;
    m_reader = {};
    socket->setReadBufferSize(FrameReader::limit + 4);
    connect(socket, &QLocalSocket::readyRead, this, [this, socket] {
      if (!sameLogin(socket,true) || !m_reader.feed(socket->read(FrameReader::limit + 4), [this](const auto &value) { Q_EMIT received(value); }))
        socket->abort();
    });
    connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
      m_client = nullptr;
      socket->deleteLater();
      Q_EMIT attachedChanged(false);
#ifdef Q_OS_WIN
      QTimer::singleShot(0,this,[this]{m_native->rearm();});
#endif
    });
    Q_EMIT attachedChanged(true);
}
bool PrivateIpcServer::listen(const QString &endpoint)
{
#ifdef Q_OS_WIN
  if(!m_native->listen(endpoint))return false;
  m_loginCheck.start(100);return true;
#else
  const auto name = socketName(endpoint);
  if (name.isEmpty())
    return false;
  if (m_server.listen(name))
    return true;
  if (m_server.serverError() != QAbstractSocket::AddressInUseError)
    return false;
  // A crashed core leaves its socket file behind; replace it only when nobody answers.
  QLocalSocket probe;
  probe.connectToServer(name);
  if (probe.waitForConnected(200))
    return false;
  QLocalServer::removeServer(name);
  return m_server.listen(name);
#endif
}
PrivateIpcServer::~PrivateIpcServer()
{
  disconnect(&m_server, nullptr, this, nullptr);
  if (m_client)
    disconnect(m_client, nullptr, this, nullptr);
  m_server.close();
#ifdef Q_OS_WIN
  m_loginCheck.stop();m_native->close();
#endif
}
bool PrivateIpcServer::send(const QJsonObject &frame)
{
  if (write(m_client, frame))
    return true;
  if (m_client)
    m_client->abort();
  return false;
}
SessionClient::SessionClient(QObject *parent) : QObject(parent)
{
  m_retry.setInterval(1000);
  connect(&m_retry, &QTimer::timeout, this, &SessionClient::connectNow);
  connect(&m_socket, &QLocalSocket::connected, this, [this] {
    if (!sameLogin(&m_socket, false)) {
      m_socket.abort();
      return;
    }
    m_verified = true;
    m_reader = {};
    Q_EMIT connectedChanged(true);
  });
  connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
    m_verified = false;
    Q_EMIT connectedChanged(false);
  });
  connect(&m_socket, &QLocalSocket::readyRead, this, [this] {
    if (!m_verified || !sameLogin(&m_socket,false) ||
        !m_reader.feed(m_socket.read(FrameReader::limit + 4), [this](const auto &value) { Q_EMIT received(value); }))
      m_socket.abort();
  });
  m_socket.setReadBufferSize(FrameReader::limit + 4);
}
void SessionClient::start(const QString &endpoint)
{
  m_endpoint = endpoint;
  connectNow();
  m_retry.start();
}
void SessionClient::connectNow()
{
  if (!m_endpoint.isEmpty() && m_socket.state() == QLocalSocket::UnconnectedState) {
#ifdef Q_OS_WIN
    const auto pipe=windowsOpenPrivatePipe(m_endpoint);
    if(pipe==INVALID_HANDLE_VALUE)return;
    if(!m_socket.setSocketDescriptor(reinterpret_cast<qintptr>(pipe),QLocalSocket::ConnectedState)){
      CloseHandle(pipe);return;
    }
    if(!sameLogin(&m_socket,false)){m_socket.abort();return;}
    m_verified=true;m_reader={};Q_EMIT connectedChanged(true);
#else
    m_socket.connectToServer(socketName(m_endpoint));
#endif
  }
}
void SessionClient::shutdown()
{
  m_retry.stop();
  m_socket.abort();
}
bool SessionClient::send(const QJsonObject &frame)
{
  return m_verified && write(&m_socket, frame);
}
} // namespace deskflow::streaming
