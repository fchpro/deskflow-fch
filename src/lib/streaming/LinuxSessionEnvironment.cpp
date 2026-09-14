// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SessionEnvironment.h"
#include <QCoreApplication>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QElapsedTimer>
namespace deskflow::streaming {
class LinuxEnvironment final:public QObject,public SessionEnvironment {
  Q_OBJECT
public:
  QString start()override{
    m_failure.clear();m_path.clear();
    QDBusInterface manager("org.freedesktop.login1","/org/freedesktop/login1","org.freedesktop.login1.Manager",QDBusConnection::systemBus());manager.setTimeout(100);
    QDBusReply<QDBusObjectPath> reply=manager.call("GetSessionByPID",uint(QCoreApplication::applicationPid()));
    if(!reply.isValid())return m_failure="Cannot identify the active login session for streaming.";
    m_path=reply.value().path();
    auto bus=QDBusConnection::systemBus();
    if(!bus.connect("org.freedesktop.login1","/org/freedesktop/login1","org.freedesktop.login1.Manager","PrepareForSleep",this,SLOT(sleep(bool))) ||
      !bus.connect("org.freedesktop.login1",m_path,"org.freedesktop.login1.Session","Lock",this,SLOT(lock())))
      return m_failure="Streaming login/power notifications are unavailable.";
    m_query.start();m_gap.start();return verify();
  }
  QString poll()override{
    if(!m_failure.isEmpty())return m_failure;
    if(!m_gap.isValid() || m_gap.restart()>3000)return m_failure="Media processing was interrupted. Start a new stream.";
    if(m_query.elapsed()>=100){m_query.restart();return verify();}return {};
  }
private Q_SLOTS:
  void sleep(bool preparing){if(preparing)m_failure="Login suspended. Start a new stream after returning.";}
  void lock(){m_failure="Login locked. Unlock it and start a new stream.";}
private:
  QString verify(){
    QDBusInterface session("org.freedesktop.login1",m_path,"org.freedesktop.login1.Session",QDBusConnection::systemBus());session.setTimeout(100);
    const auto locked=session.property("LockedHint"),active=session.property("Active");
    if(!locked.isValid() || !active.isValid() || locked.toBool() || !active.toBool())m_failure="Login is locked, inactive or unavailable. Start a new stream after restoring it.";
    return m_failure;
  }
  QString m_path,m_failure;
  QElapsedTimer m_query,m_gap;
};
std::unique_ptr<SessionEnvironment> createSessionEnvironment(){return std::make_unique<LinuxEnvironment>();}
}
#include "LinuxSessionEnvironment.moc"
