// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "PrivateIpc.h"
#include "SecureChannel.h"
#include "SessionBroker.h"
#include "Control.h"
#include <QElapsedTimer>
#include <QSet>

namespace deskflow::streaming {
QHostAddress publishedMediaAddress(const QHostAddress &listener, const QHostAddress &binding);
// Construct on the signaling worker thread. No network work enters input dispatch.
class Service : public QObject
{
  Q_OBJECT
public:
  Service(
      const QString &certificatePath, const QString &name, QObject *parent = nullptr,
      const QString &endpoint = privateEndpoint(), std::unique_ptr<NativeControl> native = createNativeControl()
  );
  ~Service() override;
  void configureControl(const QStringList &excluded,const QString &swapTarget,int motionHz);

private:
  void poll();
  void channel(QSslSocket *socket, bool server, const std::shared_ptr<InputBinding> &binding = {});
  void local(const QJsonObject &frame);
  void attachLocal();
  void delivered(const QJsonObject &frame);
  void revokeControl();
  std::shared_ptr<InputBinding> lookup(const QString &id) const;
  SessionBroker m_broker;
  PrivateIpcServer m_ipc;
  TlsListener m_listener;
  QSslConfiguration m_configuration;
  QHash<QString, QPointer<SecureChannel>> m_channels;
  QPointer<SecureChannel> m_upstream;
  QSet<QString> m_probed;
  QJsonObject m_capabilities;
  QString m_id, m_name, m_generation;
  QTimer m_timer;
  QElapsedTimer m_clock;
  bool m_server = false, m_localAttached = false;
  ControlOwner m_control;
  QTimer m_controlTimer;
  QJsonObject m_controlOffer;
  QString m_controlState;
  bool m_hadControl=false;
  bool m_releaseReported=false;
  QString m_leftSwapName;
  QHash<QString,QString> m_controlPeerNames;
};
} // namespace deskflow::streaming
