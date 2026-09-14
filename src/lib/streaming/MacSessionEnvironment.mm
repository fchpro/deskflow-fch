// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SessionEnvironment.h"
#include <AppKit/AppKit.h>
#include <ApplicationServices/ApplicationServices.h>
#include <QElapsedTimer>
#include <atomic>
namespace deskflow::streaming {
namespace {
class MacEnvironment final:public SessionEnvironment {
public:
  ~MacEnvironment()override{close();}
  QString start()override{
    close();m_interrupted=false;m_failure.clear();
    const auto center=[[NSWorkspace sharedWorkspace] notificationCenter];
    m_sleep=[center addObserverForName:NSWorkspaceWillSleepNotification object:nil queue:nil usingBlock:^(NSNotification *){m_interrupted=true;}];
    m_inactive=[center addObserverForName:NSWorkspaceSessionDidResignActiveNotification object:nil queue:nil usingBlock:^(NSNotification *){m_interrupted=true;}];
    m_query.start();m_gap.start();return verify();
  }
  QString poll()override{
    if(!m_failure.isEmpty())return m_failure;
    if(m_interrupted || !m_gap.isValid() || m_gap.restart()>3000)return m_failure="Login or media processing was interrupted. Start a new stream.";
    if(m_query.elapsed()>=100){m_query.restart();return verify();}return {};
  }
private:
  QString verify(){
    NSDictionary *session=CFBridgingRelease(CGSessionCopyCurrentDictionary());
    // This lock property is not part of Apple's public session-key contract.
    // An absent value cannot establish that capture/playback is private.
    if(!session || !session[@"CGSSessionScreenIsLocked"])
      return m_failure="This macOS login does not expose a verifiable lock state for streaming.";
    if(![session[(__bridge NSString *)kCGSessionOnConsoleKey] boolValue] || [session[@"CGSSessionScreenIsLocked"] boolValue])
      m_failure="Login is locked or inactive. Unlock it and start a new stream.";
    return m_failure;
  }
  void close(){const auto center=[[NSWorkspace sharedWorkspace] notificationCenter];if(m_sleep)[center removeObserver:m_sleep];if(m_inactive)[center removeObserver:m_inactive];m_sleep=nil;m_inactive=nil;}
  id m_sleep=nil,m_inactive=nil;
  std::atomic_bool m_interrupted=false;
  QString m_failure;
  QElapsedTimer m_query,m_gap;
};
}
std::unique_ptr<SessionEnvironment> createSessionEnvironment(){return std::make_unique<MacEnvironment>();}
}
