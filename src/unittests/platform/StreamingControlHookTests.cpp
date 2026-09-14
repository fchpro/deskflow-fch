// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "platform/MSWindowsHook.h"
#include "common/StreamingInputGate.h"
#include "base/Log.h"
#include <QTest>
namespace {
int forwarded=0;
BOOL WINAPI ownedPost(DWORD,UINT message,WPARAM,LPARAM){if(message>=DESKFLOW_MSG_INPUT_FIRST && message<=DESKFLOW_MSG_INPUT_LAST)++forwarded;return TRUE;}
LRESULT WINAPI ownedNext(HHOOK,int,WPARAM,LPARAM){return 42;}
}
// Execute the real low-level dispatch functions with only their external queue/
// hook-chain boundary substituted. Never install hooks or inject OS input.
#undef PostThreadMessage
#define PostThreadMessage ownedPost
#define CallNextHookEx ownedNext
#include "platform/MSWindowsHook.cpp"
#undef PostThreadMessage
#undef CallNextHookEx
using namespace deskflow::streaming;
class StreamingControlHookTests:public QObject {
  Q_OBJECT
  Log log;
private Q_SLOTS:
  void cleanup(){controlOwnsInput=false;viewerOwnsInput=false;g_mode=kHOOK_DISABLE;}
  void isolated_data(){QTest::addColumn<bool>("keyboard");QTest::addColumn<int>("owner");for(bool keyboard:{false,true})for(int owner=0;owner<3;++owner)QTest::newRow(qPrintable(QString(keyboard?"key-":"mouse-")+QString::number(owner)))<<keyboard<<owner;}
  void isolated(){QFETCH(bool,keyboard);QFETCH(int,owner);forwarded=0;g_mode=kHOOK_RELAY_EVENTS;g_isPrimary=TRUE;g_fakeServerInput=false;
    controlOwnsInput=owner==0;viewerOwnsInput=owner==1;
    KBDLLHOOKSTRUCT key{};key.vkCode=VK_F5;key.scanCode=0x3f;key.dwExtraInfo=owner==2?controlInputMarker:0;
    MSLLHOOKSTRUCT mouse{};mouse.pt={100,100};mouse.dwExtraInfo=key.dwExtraInfo;
    const auto result=keyboard?keyboardLLHook(HC_ACTION,WM_KEYDOWN,reinterpret_cast<LPARAM>(&key)):mouseLLHook(HC_ACTION,WM_LBUTTONDOWN,reinterpret_cast<LPARAM>(&mouse));
    QCOMPARE(result,LRESULT(42));QCOMPARE(forwarded,0);
  }
};
QTEST_GUILESS_MAIN(StreamingControlHookTests)
#include "StreamingControlHookTests.moc"
