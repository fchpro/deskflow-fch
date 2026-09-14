// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#define NOMINMAX
#include "streaming/SessionEnvironment.h"
#include <Windows.h>
#include <wtsapi32.h>
#include <QTest>
using namespace deskflow::streaming;
class StreamingEnvironmentTests:public QObject {
  Q_OBJECT
private Q_SLOTS:
  void notifications_data(){
    QTest::addColumn<QString>("aspect");
    for(const char *name:{"lock","logoff","console-disconnect","remote-disconnect","other-session","sticky-unlock","fresh-start"})QTest::newRow(name)<<QString(name);
  }
  void notifications(){
    QFETCH(QString,aspect);auto environment=createSessionEnvironment();
    if(const auto error=environment->start();!error.isEmpty())qFatal("Real session monitor startup failed: %s",qPrintable(error));
    const auto window=FindWindowExW(HWND_MESSAGE,nullptr,L"DeskflowStreamingSessionV1",nullptr);
    DWORD owner=0,session=0;
    if(!window || !GetWindowThreadProcessId(window,&owner) || owner!=GetCurrentProcessId() ||
      !ProcessIdToSessionId(owner,&session))qFatal("Owned message-only monitor window not found");
    const WPARAM event=aspect=="logoff"?WTS_SESSION_LOGOFF:aspect=="console-disconnect"?WTS_CONSOLE_DISCONNECT:
      aspect=="remote-disconnect"?WTS_REMOTE_DISCONNECT:WTS_SESSION_LOCK;
    SendMessageW(window,WM_WTSSESSION_CHANGE,event,aspect=="other-session"?session+1:session);
    if(aspect=="sticky-unlock")SendMessageW(window,WM_WTSSESSION_CHANGE,WTS_SESSION_UNLOCK,session);
    if(aspect=="fresh-start"){
      const auto error=environment->start();
      if(!error.isEmpty())qFatal("Fresh native monitor registration failed");
      const auto fresh=environment->poll();
      qInfo()<<"Explicit new monitor start clears prior interruption"<<fresh.isEmpty();
      QCOMPARE(fresh.isEmpty(),true);return;
    }
    const auto reason=environment->poll();
    qInfo()<<"Owned message-only session notification"<<aspect<<"interrupts"<<!reason.isEmpty();
    QCOMPARE(reason.isEmpty(),aspect=="other-session");
  }
  void cleanup(){
    const auto window=FindWindowExW(HWND_MESSAGE,nullptr,L"DeskflowStreamingSessionV1",nullptr);
    QCOMPARE(window,HWND(nullptr));
  }
};
QTEST_GUILESS_MAIN(StreamingEnvironmentTests)
#include "StreamingEnvironmentTests.moc"
