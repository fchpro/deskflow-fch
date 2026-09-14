// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#define NOMINMAX
#include "SessionEnvironment.h"
#include <Windows.h>
#include <wtsapi32.h>
#include <powrprof.h>
#include <QElapsedTimer>
#include <atomic>
namespace deskflow::streaming {
namespace {
class WindowsEnvironment final : public SessionEnvironment {
public:
  ~WindowsEnvironment()override{close();}
  QString start()override {
    close();m_interrupted=false;m_failure.clear();
    if(!ProcessIdToSessionId(GetCurrentProcessId(),&m_session) || !m_session)return fail("Streaming requires an interactive login session.");
    WNDCLASSW cls{};cls.lpfnWndProc=windowProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"DeskflowStreamingSessionV1";
    if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return fail("Streaming session notifications could not be registered.");
    m_window=CreateWindowExW(0,cls.lpszClassName,L"",0,0,0,0,0,HWND_MESSAGE,nullptr,cls.hInstance,this);
    if(!m_window || !WTSRegisterSessionNotification(m_window,NOTIFY_FOR_THIS_SESSION))return fail("Streaming session notifications are unavailable.");
    m_powerParameters.Callback=powerCallback;m_powerParameters.Context=this;
    if(PowerRegisterSuspendResumeNotification(DEVICE_NOTIFY_CALLBACK,&m_powerParameters,&m_power)!=ERROR_SUCCESS)
      return fail("Streaming power notifications are unavailable.");
    m_query.start();m_gap.start();return verify();
  }
  QString poll()override {
    if(!m_failure.isEmpty())return m_failure;
    if(m_interrupted)return fail("Login locked, disconnected or suspended. Start a new stream after returning.");
    if(!m_gap.isValid() || m_gap.restart()>3000)return fail("Media processing was interrupted. Start a new stream.");
    if(m_query.elapsed()>=100){m_query.restart();return verify();}
    return {};
  }
private:
  QString fail(const char *reason){m_failure=QString::fromLatin1(reason);return m_failure;}
  QString verify(){
    LPWSTR raw=nullptr;DWORD bytes=0;
    if(!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE,m_session,WTSSessionInfoEx,&raw,&bytes))
      return fail("Cannot verify the unlocked login session. Start again after restoring it.");
    bool allowed=false;
    if(bytes>=sizeof(WTSINFOEXW)){
      const auto *info=reinterpret_cast<const WTSINFOEXW *>(raw);
      allowed=info->Level==1 && info->Data.WTSInfoExLevel1.SessionState==WTSActive &&
        info->Data.WTSInfoExLevel1.SessionFlags==WTS_SESSIONSTATE_UNLOCK;
    }
    WTSFreeMemory(raw);
    return allowed?QString{}:fail("Login is locked or inactive. Unlock it and start a new stream.");
  }
  static LRESULT CALLBACK windowProc(HWND window,UINT message,WPARAM wparam,LPARAM lparam){
    if(message==WM_NCCREATE)SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lparam)->lpCreateParams));
    auto *self=reinterpret_cast<WindowsEnvironment *>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(self && message==WM_WTSSESSION_CHANGE && DWORD(lparam)==self->m_session &&
      (wparam==WTS_SESSION_LOCK || wparam==WTS_SESSION_LOGOFF || wparam==WTS_CONSOLE_DISCONNECT || wparam==WTS_REMOTE_DISCONNECT))
      self->m_interrupted=true;
    return DefWindowProcW(window,message,wparam,lparam);
  }
  static ULONG CALLBACK powerCallback(PVOID context,ULONG type,PVOID){
    if(type==PBT_APMSUSPEND || type==PBT_APMRESUMEAUTOMATIC || type==PBT_APMRESUMESUSPEND)
      static_cast<WindowsEnvironment *>(context)->m_interrupted=true;
    return ERROR_SUCCESS;
  }
  void close(){
    if(m_power){PowerUnregisterSuspendResumeNotification(m_power);m_power=nullptr;}
    if(m_window){WTSUnRegisterSessionNotification(m_window);DestroyWindow(m_window);m_window=nullptr;}
  }
  DWORD m_session=0;
  HWND m_window=nullptr;
  HPOWERNOTIFY m_power=nullptr;
  DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS m_powerParameters{};
  std::atomic_bool m_interrupted=false;
  QString m_failure;
  QElapsedTimer m_query,m_gap;
};
}
std::unique_ptr<SessionEnvironment> createSessionEnvironment(){return std::make_unique<WindowsEnvironment>();}
}
