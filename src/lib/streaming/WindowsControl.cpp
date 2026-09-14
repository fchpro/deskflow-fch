// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Control.h"
#include "common/StreamingInputGate.h"
#include <QFileInfo>
#include <windows.h>
#include <dwmapi.h>
namespace deskflow::streaming {
namespace {
quint64 birth(DWORD pid) {
  HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return 0;
  FILETIME a{},b{},c{},d{};const bool ok=GetProcessTimes(process,&a,&b,&c,&d);CloseHandle(process);
  return ok?(quint64(a.dwHighDateTime)<<32)|a.dwLowDateTime:0;
}
bool inputDesktop() {
  const HDESK desktop=OpenInputDesktop(0,FALSE,DESKTOP_READOBJECTS);if(!desktop)return false;
  wchar_t a[256]{},b[256]{};DWORD size=0;
  const bool ok=GetUserObjectInformationW(desktop,UOI_NAME,a,sizeof(a),&size) &&
    GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,b,sizeof(b),&size) && wcscmp(a,b)==0;
  CloseDesktop(desktop);return ok;
}
class WindowsControl final:public NativeControl {
public:
  WindowsControl() {
    m_dpi=SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if(!instance){instance=this;m_keyboard=SetWindowsHookExW(WH_KEYBOARD_LL,keyboard,GetModuleHandleW(nullptr),0);
      m_mouse=SetWindowsHookExW(WH_MOUSE_LL,mouse,GetModuleHandleW(nullptr),0);
      m_destroy=SetWinEventHook(EVENT_OBJECT_DESTROY,EVENT_OBJECT_DESTROY,nullptr,destroyed,0,0,WINEVENT_OUTOFCONTEXT);}

  }
  ~WindowsControl() override {
    if(m_destroy)UnhookWinEvent(m_destroy);
    if(m_keyboard)UnhookWindowsHookEx(m_keyboard);if(m_mouse)UnhookWindowsHookEx(m_mouse);
    if(instance==this)instance=nullptr;if(m_dpi)SetThreadDpiAwarenessContext(m_dpi);
  }
  void exclusions(const QStringList &apps)override{m_excluded=apps;}
  bool available()const override{return m_keyboard && m_mouse && m_destroy && m_dpi && inputDesktop();}
  bool bind(const QJsonObject &t) override {
    if(!fields(t,{"session","source","kind","handle","pid","birth","device"}))return false;
    bool handleOk=false,birthOk=false;
    const auto handle=t["handle"].toString().toULongLong(&handleOk,16),stamp=t["birth"].toString().toULongLong(&birthOk,16);
    if(!handleOk || !birthOk || !handle || !controlInteger(t["pid"],0,MAXDWORD))return false;
    if(t["kind"]!="window" && t["kind"]!="screen")return false;
    m_dead=false;m_target=t;m_handle=handle;m_birth=stamp;m_pid=t["pid"].toInteger();return true;
  }
  bool verify(const QRect &rect,const std::optional<QPoint> &point,bool keyboardEvent)override {
    if(!available() || m_dead || excluded())return false;
    RECT actual{};
    if(m_target["kind"]=="window") {
      HWND target=reinterpret_cast<HWND>(m_handle);DWORD pid=0;GetWindowThreadProcessId(target,&pid);
      if(!IsWindow(target) || pid!=m_pid || !m_birth || birth(pid)!=m_birth || !IsWindowVisible(target) || IsIconic(target) ||
        FAILED(DwmGetWindowAttribute(target,DWMWA_EXTENDED_FRAME_BOUNDS,&actual,sizeof(actual))))return false;
      if(keyboardEvent && GetAncestor(GetForegroundWindow(),GA_ROOT)!=target)return false;
      if(point && GetAncestor(WindowFromPoint({point->x(),point->y()}),GA_ROOT)!=target)return false;
    }else if(m_target["kind"]=="screen") {
      MONITORINFOEXW info{};info.cbSize=sizeof(info);
      if(!GetMonitorInfoW(reinterpret_cast<HMONITOR>(m_handle),&info) || QString::fromWCharArray(info.szDevice)!=m_target["device"])return false;
      actual=info.rcMonitor;
      if(keyboardEvent && MonitorFromWindow(GetForegroundWindow(),MONITOR_DEFAULTTONULL)!=reinterpret_cast<HMONITOR>(m_handle))return false;
    }else return false;
    return rect==QRect(actual.left,actual.top,actual.right-actual.left,actual.bottom-actual.top);
  }
  bool idle()const override {
    for(int key=1;key<256;++key)if(GetAsyncKeyState(key)&0x8000)return false;
    return true;
  }
  bool physicalInput()override{return std::exchange(m_physical,false);}
  bool inject(const QJsonObject &e)override {
    INPUT input{};const auto kind=e["kind"].toString();
    if(kind=="key" || kind=="repeat"){
      input.type=INPUT_KEYBOARD;input.ki.wVk=WORD(e["code"].toInt());input.ki.dwExtraInfo=controlInputMarker;
      const int code=e["code"].toInt();
      if(code==VK_RCONTROL || code==VK_RMENU || code==VK_LWIN || code==VK_RWIN || (code>=VK_PRIOR && code<=VK_DOWN) || code==VK_INSERT || code==VK_DELETE)
        input.ki.dwFlags|=KEYEVENTF_EXTENDEDKEY;
      if(!e["down"].toBool())input.ki.dwFlags|=KEYEVENTF_KEYUP;
      return SendInput(1,&input,sizeof(input))==1;
    }
    input.type=INPUT_MOUSE;input.mi.dwExtraInfo=controlInputMarker;
    if(!e["release"].toBool()){
      const int x=GetSystemMetrics(SM_XVIRTUALSCREEN),y=GetSystemMetrics(SM_YVIRTUALSCREEN),w=GetSystemMetrics(SM_CXVIRTUALSCREEN),h=GetSystemMetrics(SM_CYVIRTUALSCREEN);
      if(w<2 || h<2)return false;
      input.mi.dx=LONG((e["x"].toInteger()-x)*65535/(w-1));input.mi.dy=LONG((e["y"].toInteger()-y)*65535/(h-1));
      input.mi.dwFlags=MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK|MOUSEEVENTF_MOVE;
    }
    if(kind=="button"){
      const DWORD down[]={0,MOUSEEVENTF_LEFTDOWN,MOUSEEVENTF_RIGHTDOWN,MOUSEEVENTF_MIDDLEDOWN};
      const DWORD up[]={0,MOUSEEVENTF_LEFTUP,MOUSEEVENTF_RIGHTUP,MOUSEEVENTF_MIDDLEUP};
      const int code=e["code"].toInt();if(code<1 || code>3)return false;
      input.mi.dwFlags|=e["down"].toBool()?down[code]:up[code];
    }
    if(SendInput(1,&input,sizeof(input))!=1)return false;
    if(kind=="wheel")for(const auto *axis:{"dx","dy"})if(e[axis].toInt()){
      input.mi.dwFlags=QString(axis)=="dx"?MOUSEEVENTF_HWHEEL:MOUSEEVENTF_WHEEL;
      input.mi.mouseData=DWORD(e[axis].toInt());if(SendInput(1,&input,sizeof(input))!=1)return false;
    }
    return true;
  }
private:
  bool excluded() const {
    if(m_excluded.isEmpty())return false;
    DWORD pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&pid);
    HANDLE process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);if(!process)return true;
    wchar_t path[32768]{};DWORD size=32768;const bool ok=QueryFullProcessImageNameW(process,0,path,&size);CloseHandle(process);
    if(!ok)return true;const QFileInfo file(QString::fromWCharArray(path,size));
    for(const auto &name:m_excluded)if(name.compare(file.fileName(),Qt::CaseInsensitive)==0 || name.compare(file.completeBaseName(),Qt::CaseInsensitive)==0)return true;
    return false;
  }
  static void CALLBACK destroyed(HWINEVENTHOOK,DWORD,HWND window,LONG object,LONG child,DWORD,DWORD){
    if(instance && object==OBJID_WINDOW && child==CHILDID_SELF && instance->m_target["kind"]=="window" && quintptr(window)==instance->m_handle){
      instance->m_dead=true;if(instance->physicalPriority)instance->physicalPriority();
    }
  }
  static LRESULT CALLBACK keyboard(int code,WPARAM w,LPARAM l){
    if(code>=0 && instance){const auto *event=reinterpret_cast<KBDLLHOOKSTRUCT*>(l);
      if(!(event->flags&LLKHF_INJECTED)){instance->m_physical=true;if(instance->physicalPriority)instance->physicalPriority();}}
    return CallNextHookEx(nullptr,code,w,l);
  }
  static LRESULT CALLBACK mouse(int code,WPARAM w,LPARAM l){
    if(code>=0 && instance){const auto *event=reinterpret_cast<MSLLHOOKSTRUCT*>(l);
      if(!(event->flags&LLMHF_INJECTED)){instance->m_physical=true;if(instance->physicalPriority)instance->physicalPriority();}}
    return CallNextHookEx(nullptr,code,w,l);
  }
  static thread_local WindowsControl *instance;
  HWINEVENTHOOK m_destroy=nullptr;bool m_dead=false;
  HHOOK m_keyboard=nullptr,m_mouse=nullptr;DPI_AWARENESS_CONTEXT m_dpi=nullptr;
  QJsonObject m_target;QStringList m_excluded;quint64 m_handle=0,m_birth=0;DWORD m_pid=0;bool m_physical=false;
};
thread_local WindowsControl *WindowsControl::instance=nullptr;
}
std::unique_ptr<NativeControl> createNativeControl(){return std::make_unique<WindowsControl>();}
}
