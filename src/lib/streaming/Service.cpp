// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Service.h"
#include "base/Log.h"
#include "common/StreamingInputGate.h"
#include <QFile>
#include <QSslKey>

namespace deskflow::streaming {
QHostAddress publishedMediaAddress(const QHostAddress &listener, const QHostAddress &binding)
{
  return listener.isNull() ? binding : listener;
}

Service::Service(const QString &certificatePath, const QString &name, QObject *parent, const QString &endpoint,std::unique_ptr<NativeControl> native)
    : QObject(parent),
      m_name(name),
      m_generation(randomId()), m_control(std::move(native))
{
  if (QSslSocket::activeBackend() != "openssl" && !QSslSocket::setActiveBackend("openssl")) {
    LOG_WARN("streaming unavailable: the required Qt OpenSSL TLS backend is missing");
    return;
  }
  m_capabilities = {{"sources", QJsonArray{}}, {"receive", false}, {"audio", QJsonArray{"off"}}, {"control", false}};
  QFile file(certificatePath);
  if (!file.open(QIODevice::ReadOnly)) {
    LOG_WARN("streaming unavailable: cannot read configured TLS identity");
    return;
  }
  const auto pem = file.readAll();
  const QSslCertificate certificate(pem);
  const QSslKey key(pem, QSsl::Rsa);
  if (certificate.isNull() || key.isNull()) {
    LOG_WARN("streaming unavailable: invalid TLS identity");
    return;
  }
  m_configuration = tlsConfiguration(certificate, key);
  m_id = QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex());
  m_clock.start();
  connect(&m_broker, &SessionBroker::deliver, this, [this](const QString &id, const auto &frame) {
    if (id == m_id)
      delivered(frame);
    else if (m_channels.value(id))
      m_channels[id]->send(frame);
  });
  connect(&m_ipc, &PrivateIpcServer::received, this, &Service::local);
  connect(&m_ipc, &PrivateIpcServer::attachedChanged, this, [this](bool attached) {
    if (attached) {
      m_ipc.send(message("ControlAvailability",{{"available",m_control.available()}}));
      for (const auto &binding : activeBindings())
        if (binding->localId == m_id) {
          m_ipc.send(message("Identity", {{"id", m_id}, {"address", publishedMediaAddress(m_listener.serverAddress(), binding->localAddress).toString()}}));
          break;
        }
      attachLocal();
      if (m_upstream)
        m_upstream->send(message("Capabilities", m_capabilities));
    } else {
      revokeControl(); viewerOwnsInput=false; m_controlOffer={};
      m_broker.detach(m_id);
      m_localAttached = false;
      if (m_upstream)
        m_upstream->close();
      m_capabilities = {
          {"sources", QJsonArray{}}, {"receive", false}, {"audio", QJsonArray{"off"}}, {"control", false}
      };
    }
  });
  connect(&m_listener, &TlsListener::accepted, this, [this](QSslSocket *socket) { channel(socket, true); });
  if (!m_ipc.listen(endpoint)) {
    LOG_WARN("streaming unavailable: private user-session IPC could not be created");
    return;
  }
  connect(&m_timer, &QTimer::timeout, this, &Service::poll);
  m_timer.start(100);
  connect(&m_controlTimer,&QTimer::timeout,this,[this]{
    m_control.poll(m_clock.elapsed());
    if(!m_releaseReported && m_control.releasePending()){
      LOG_WARN("streaming control revoked but native release is pending; ordinary input remains suspended");
      m_ipc.send(message("Error",{{"reason","inputReleasePending"}}));
    }
    m_releaseReported=m_control.releasePending();
    if(m_hadControl && !m_control.active()){m_hadControl=false;revokeControl();}
  });
  m_controlTimer.start(4);
}
void Service::configureControl(const QStringList &excluded,const QString &swapTarget,int motionHz){
  m_control.exclusions(excluded);m_leftSwapName=swapTarget;m_control.motionRate(motionHz);
}
std::shared_ptr<InputBinding> Service::lookup(const QString &id) const
{
  std::shared_ptr<InputBinding> result;
  for (const auto &binding : activeBindings()) {
    if (binding->peerId == id && binding->localId == m_id) {
      if (result)
        return {}; // Certificate identity must identify exactly one active peer.
      result = binding;
    }
  }
  return result;
}
Service::~Service()
{
  m_controlTimer.stop();m_control.revoke();viewerOwnsInput=false;
  if(m_control.releasePending())LOG_WARN("streaming shutdown could not confirm native key/button release");
  m_timer.stop();
  disconnect(&m_ipc, nullptr, this, nullptr);
  disconnect(&m_listener, nullptr, this, nullptr);
  for (auto *link : findChildren<SecureChannel *>(QString(), Qt::FindDirectChildrenOnly)) {
    disconnect(link, nullptr, this, nullptr);
    delete link;
  }
}
void Service::attachLocal()
{
  if (!m_server || !m_ipc.attached() || m_localAttached)
    return;
  m_localAttached = m_broker.attach({m_id, m_name, m_generation, m_listener.serverAddress(), m_capabilities});
}
void Service::poll()
{
  QSet<QString> current;
  for (const auto &binding : activeBindings()) {
    if (binding->localId != m_id)
      continue;
    current.insert(binding->generation);
    if (m_probed.contains(binding->generation))
      continue;
    m_probed.insert(binding->generation);
    m_ipc.send(message("Identity", {{"id", m_id}, {"address", publishedMediaAddress(m_listener.serverAddress(), binding->localAddress).toString()}}));
    m_server = binding->server;
    if (m_server) {
      if (!m_listener.isListening()) {
        m_listener.configuration = m_configuration;
        if (!m_listener.listen(binding->localAddress, binding->brokerPort))
          LOG_WARN("streaming broker could not listen on authenticated input interface");
      }
      attachLocal();
    } else {
      auto *socket = new QSslSocket;
      socket->setSslConfiguration(m_configuration);
      channel(socket, false, binding);
      socket->connectToHostEncrypted(binding->address.toString(), binding->brokerPort);
    }
  }
  m_probed.intersect(current);
  for (auto *entry : findChildren<SecureChannel *>(QString(), Qt::FindDirectChildrenOnly)) {
    if (auto binding = entry->binding(); binding && !binding->active)
      entry->close();
  }
  m_broker.expire(m_clock.elapsed());
}
void Service::channel(QSslSocket *socket, bool server, const std::shared_ptr<InputBinding> &expected)
{
  if (findChildren<SecureChannel *>(QString(), Qt::FindDirectChildrenOnly).size() >= 64) {
    socket->abort();
    socket->deleteLater();
    return;
  }
  auto *link = new SecureChannel(
      socket,
      [this, expected](const QString &id) {
        if (expected)
          return expected->active && expected->peerId == id ? expected : std::shared_ptr<InputBinding>{};
        return lookup(id);
      },
      this
  );
  connect(link, &SecureChannel::established, this, [this, link, server] {
    const auto binding = link->binding();
    if (m_channels.value(binding->peerId)) {
      link->close();
      return;
    }
    m_channels.insert(binding->peerId, link);
    if (!server) {
      m_upstream = link;
      if (m_ipc.attached())
        link->send(message("Capabilities", m_capabilities));
    }
  });
  connect(link, &SecureChannel::received, this, [this, link, server](const QJsonObject &frame) {
    const auto binding = link->binding();
    if (!binding || !binding->active) {
      link->close();
      return;
    }
    if (!server) {
      delivered(frame);
      return;
    }
    bool attached = false;
    for (const auto &peer : m_broker.peers())
      attached |= peer.id == binding->peerId;
    if (!attached) {
      if (frame["type"] != "Capabilities" ||
          !m_broker.attach(
              {binding->peerId, binding->name, binding->generation, binding->address, frame["data"].toObject()}
          ))
        link->close();
      return;
    }
    if (!m_broker.dispatch(binding->peerId, binding->generation, frame, m_clock.elapsed())) {
      // A rejected request receives its reason. It obtains no media/control.
      LOG_DEBUG("streaming broker rejected peer request");
    }
  });
  connect(link, &SecureChannel::ended, this, [this, link] {
    if (const auto binding = link->binding(); binding && m_channels.value(binding->peerId) == link) {
      m_broker.detach(binding->peerId);
      m_channels.remove(binding->peerId);
    }
    if (m_upstream == link) {
      m_control.revoke();viewerOwnsInput=false;m_controlOffer={};
      m_upstream = nullptr;
      m_ipc.send(message("Error", {{"reason", "peerDisconnected"}}));
      m_ipc.send(message("Roster", {{"peers", QJsonArray{}}}));
    }
    link->deleteLater();
  });
}
void Service::local(const QJsonObject &frame)
{
  const auto type=frame["type"].toString();const auto data=frame["data"].toObject();
  if(type=="Offer")m_controlOffer=data;
  const bool matches=data["session"]==m_controlOffer["session"] && data["source"]==m_controlOffer["source"] && !m_controlOffer.isEmpty();
  const bool sender=!m_controlOffer.contains("from");
  if(type=="ViewerFocus") {
    if(matches && !sender && fields(data,{"session","source","focused"}) && data["focused"].isBool()) {
      viewerOwnsInput=data["focused"].toBool() && ordinaryInputLocal && !controlOwnsInput;
      if(!ordinaryInputLocal)viewerOwnsInput=false;
      if(data["focused"].toBool() && !viewerOwnsInput)m_ipc.send(message("Error",{{"reason","invalidControlFocus"}}));
      if(!data["focused"].toBool())revokeControl();
    }
    return;
  }
  if(type=="ControlTarget") {
    if(!matches || !sender || m_controlOffer["kind"]=="file" || !m_control.target(data))
      m_ipc.send(message("Error",{{"reason","invalidControlTarget"}}));
    return;
  }
  if(type=="ControlGeometry" && (!matches || !sender || !m_control.geometry(data)))return;
  if(type=="GrantControl" && (!matches || !sender || m_controlState!="streaming" || !m_controlOffer["interactive"].toBool() ||
      !m_control.grant(data["lease"].toString(),m_clock.elapsed()))) {
    m_ipc.send(message("Error",{{"reason","invalidControlGrant"}}));return;
  }
  if(type=="GrantControl")m_hadControl=m_control.active();
  if(type=="RevokeControl" || type=="Stop" || type=="Decline") {m_control.revoke();if(type!="RevokeControl")viewerOwnsInput=false;}
  if(type=="ControlInput" && (!matches || sender || !viewerOwnsInput)){m_ipc.send(message("Error",{{"reason","invalidControlFocus"}}));return;}
  if (frame["type"] == "Capabilities") {
    if (!SessionBroker::validCapabilities(frame["data"].toObject())) {
      m_ipc.send(message("Error", {{"reason", "malformedCapabilities"}}));
      return;
    }
    m_capabilities = frame["data"].toObject();
    m_capabilities["control"]=m_capabilities["control"].toBool() && m_control.available();
  }
  auto routed=frame;
#ifdef Q_OS_WIN
  if(type=="ControlInput" && data["kind"]=="key" && !m_leftSwapName.isEmpty() && m_controlPeerNames.value(m_controlOffer["from"].toString())==m_leftSwapName) {
    auto event=data;const int key=event["code"].toInt();if(key==162)event["code"]=91;else if(key==91)event["code"]=162;
    routed["data"]=event;
  }
#endif
  if (m_server && m_localAttached)
    m_broker.dispatch(m_id, m_generation, type=="Capabilities"?message(type,m_capabilities):routed, m_clock.elapsed());
  else if (m_upstream && m_upstream->authenticated())
    m_upstream->send(type=="Capabilities"?message(type,m_capabilities):routed);
  else
    m_ipc.send(message("Error", {{"reason", "peerUnsupportedOrInsufficientTrust"}}));
}
void Service::revokeControl() {
  m_control.revoke();
  if(m_controlOffer.isEmpty())return;
  const auto event=message("RevokeControl",{{"session",m_controlOffer["session"]},{"source",m_controlOffer["source"]}});
  if(m_server && m_localAttached)m_broker.dispatch(m_id,m_generation,event,m_clock.elapsed());
  else if(m_upstream && m_upstream->authenticated())m_upstream->send(event);
}
void Service::delivered(const QJsonObject &frame) {
  const auto type=frame["type"].toString();const auto data=frame["data"].toObject();
  if(type=="Offer")m_controlOffer=data;
  if(type=="Roster"){m_controlPeerNames.clear();for(const auto &entry:data["peers"].toArray())m_controlPeerNames.insert(entry.toObject()["id"].toString(),entry.toObject()["name"].toString());}
  const bool matches=!m_controlOffer.isEmpty() && data["session"]==m_controlOffer["session"] && data["source"]==m_controlOffer["source"];
  if(matches && type=="State")m_controlState=data["state"].toString();
  if(matches && type=="Heartbeat")m_control.heartbeat(m_clock.elapsed());
  if(matches && (type=="RevokeControl" || type=="Stopped"))m_control.revoke();
  if(type=="Error")m_control.revoke();
  if(matches && type=="Stopped"){viewerOwnsInput=false;m_controlOffer={};m_controlState.clear();}
  if(type=="ControlInput") {
    if(!matches || m_controlOffer.contains("from") || !m_control.input(data,m_clock.elapsed()))revokeControl();
    return;
  }
  m_ipc.send(frame);
}
} // namespace deskflow::streaming
