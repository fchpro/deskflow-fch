// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Protocol.h"
#include <QHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QObject>
#include <QSet>
namespace deskflow::gui { class StreamingViewerTests; }

namespace deskflow::streaming {
struct Peer
{
  QString id;
  QString name;
  QString generation;
  QHostAddress address;
  QJsonObject capabilities;
};
struct Session
{
  QString id, sender, receiver, source;
  QString senderGeneration, receiverGeneration;
  QString state;
  QJsonObject offer;
  qint64 deadline = 0;
  qint64 senderHeartbeat = 0, receiverHeartbeat = 0;
  bool senderReady = false, receiverReady = false;
  QJsonObject playback;
  qint64 lastPlaybackCommand = -100;
  QJsonObject controlGeometry;
  QString controlLease;
  qint64 controlSequence = 0;
};
// Only authenticated transports or the credential-checked local GUI may call
// dispatch. Origin and connection generation are supplied by that transport.
class SessionBroker : public QObject
{
  Q_OBJECT
  friend class deskflow::gui::StreamingViewerTests;
public:
  using QObject::QObject;
  bool attach(const Peer &peer);
  void detach(const QString &id);
  bool dispatch(const QString &origin, const QString &generation, const QJsonObject &message, qint64 now);
  void expire(qint64 now);
  QList<Peer> peers() const
  {
    return m_peers.values();
  }
  QList<Session> sessions() const
  {
    return m_sessions.values();
  }
  static bool validCapabilities(const QJsonObject &value);
Q_SIGNALS:
  void deliver(const QString &peer, const QJsonObject &message);
  void controlRevoked(const QString &session);

private:
  void roster();
  void stop(const QString &id, const QString &reason);
  void state(Session &session, const QString &state, qint64 deadline);
  bool fail(const QString &peer, const QString &reason);
  QHash<QString, Peer> m_peers;
  QHash<QString, Session> m_sessions;
  QSet<QString> m_usedSessions;
};
} // namespace deskflow::streaming
