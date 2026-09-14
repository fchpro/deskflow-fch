// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "WindowsPrivateIpc.h"
#include "Protocol.h"
#include <sddl.h>
#include <wtsapi32.h>
#include <utility>

namespace deskflow::streaming {
namespace {
constexpr DWORD clientRights=FILE_READ_DATA|FILE_WRITE_DATA|FILE_READ_EA|FILE_WRITE_EA|
  FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES|READ_CONTROL|SYNCHRONIZE;
QString pipeName(const QString &endpoint){return "\\\\.\\pipe\\"+endpoint;}
WindowsLogin tokenLogin(HANDLE token,DWORD session) {
  WindowsLogin result;result.session=session;
  DWORD size=0;GetTokenInformation(token,TokenUser,nullptr,0,&size);
  QByteArray data(qsizetype(size),'\0');
  if(!size || !GetTokenInformation(token,TokenUser,data.data(),size,&size))return {};
  const auto sid=reinterpret_cast<TOKEN_USER *>(data.data())->User.Sid;
  if(!IsValidSid(sid))return {};
  result.system=IsWellKnownSid(sid,WinLocalSystemSid);
  result.user=QByteArray(static_cast<const char *>(sid),GetLengthSid(sid));
  size=0;GetTokenInformation(token,TokenGroups,nullptr,0,&size);data.resize(size);
  if(!size || !GetTokenInformation(token,TokenGroups,data.data(),size,&size))return {};
  const auto *groups=reinterpret_cast<const TOKEN_GROUPS *>(data.constData());
  for(DWORD i=0;i<groups->GroupCount;++i){
    if((groups->Groups[i].Attributes&SE_GROUP_LOGON_ID)!=SE_GROUP_LOGON_ID)continue;
    const auto logon=groups->Groups[i].Sid;
    if(IsValidSid(logon))result.logon=QByteArray(static_cast<const char *>(logon),GetLengthSid(logon));
    break;
  }
  return result;
}
}
QByteArray WindowsLogin::key() const {
  if(!session || user.isEmpty() || logon.isEmpty())return {};
  return user+QByteArray::number(session)+logon;
}
WindowsLogin windowsProcessLogin(DWORD pid) {
  DWORD session=0;if(!ProcessIdToSessionId(pid,&session) || !session)return {};
  const auto process=OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid);
  if(!process)return {};
  HANDLE token=nullptr;
  const bool opened=OpenProcessToken(process,TOKEN_QUERY,&token);CloseHandle(process);
  if(!opened)return {};
  const auto result=tokenLogin(token,session);CloseHandle(token);return result;
}
WindowsLogin windowsInteractiveLogin() {
  auto own=windowsProcessLogin(GetCurrentProcessId());
  if(!own.system)return own;
  // This executes only inside an already service-launched LocalSystem core.
  // The GUI is never elevated and the daemon's session zero remains excluded.
  HANDLE token=nullptr;
  if(!own.session || !WTSQueryUserToken(own.session,&token))return {};
  const auto user=tokenLogin(token,own.session);CloseHandle(token);
  return user.system?WindowsLogin{}:user;
}
bool windowsSameLogin(QLocalSocket *socket,bool serverEnd) {
  ULONG pid=0;const auto pipe=reinterpret_cast<HANDLE>(socket->socketDescriptor());
  if(!(serverEnd?GetNamedPipeClientProcessId(pipe,&pid):GetNamedPipeServerProcessId(pipe,&pid)))return false;
  const auto peer=windowsProcessLogin(pid);
  if(serverEnd){
    const auto own=windowsInteractiveLogin();
    return !own.key().isEmpty() && !peer.system && peer.key()==own.key();
  }
  const auto own=windowsProcessLogin(GetCurrentProcessId());
  if(own.key().isEmpty() || own.system)return false;
  // LocalSystem is an OS-authenticated server identity; session zero and other
  // interactive sessions are rejected even when their server is privileged.
  return peer.system?peer.session==own.session:peer.key()==own.key();
}
HANDLE windowsOpenPrivatePipe(const QString &endpoint) {
  const auto name=pipeName(endpoint);
  return CreateFileW(reinterpret_cast<LPCWSTR>(name.utf16()),clientRights,0,nullptr,OPEN_EXISTING,
    FILE_FLAG_OVERLAPPED|SECURITY_SQOS_PRESENT|SECURITY_IDENTIFICATION,nullptr);
}
QString windowsPrivatePolicy(const WindowsLogin &login,bool privileged) {
  if(login.key().isEmpty())return {};
  LPWSTR sid=nullptr;
  if(!ConvertSidToStringSidW(const_cast<char *>(login.logon.constData()),&sid))return {};
  // Current-login desktop servers retain Qt client compatibility. Privileged
  // servers grant clients individual data rights, never pipe-instance creation.
  const auto rights=privileged?QString("0x0012019b"):QString("GA");
  const auto sddl=QString("D:P(A;;GA;;;SY)(A;;%1;;;%2)").arg(rights,QString::fromWCharArray(sid));LocalFree(sid);
  return sddl;
}
WindowsPrivateListener::WindowsPrivateListener(QObject *parent):QObject(parent){}
WindowsPrivateListener::~WindowsPrivateListener(){close();}
bool WindowsPrivateListener::listen(const QString &endpoint) {
  if(!m_endpoint.isEmpty() || endpoint.isEmpty() || endpoint.contains('\\') || endpoint.contains('/'))return false;
  m_login=windowsInteractiveLogin();
  if(m_login.key().isEmpty())return false;
  m_endpoint=endpoint;
  if(rearm())return true;
  m_endpoint.clear();return false;
}
bool WindowsPrivateListener::rearm() {
  if(m_endpoint.isEmpty() || m_pipe!=INVALID_HANDLE_VALUE || windowsInteractiveLogin().key()!=m_login.key())return false;
  const auto sddl=windowsPrivatePolicy(m_login,windowsProcessLogin(GetCurrentProcessId()).system);
  PSECURITY_DESCRIPTOR descriptor=nullptr;
  if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(sddl.utf16()),SDDL_REVISION_1,&descriptor,nullptr))return false;
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor,FALSE};
  m_event=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  if(!m_event){LocalFree(descriptor);return false;}
  m_overlap={};m_overlap.hEvent=m_event;
  const auto name=pipeName(m_endpoint);
  m_pipe=CreateNamedPipeW(reinterpret_cast<LPCWSTR>(name.utf16()),PIPE_ACCESS_DUPLEX|FILE_FLAG_OVERLAPPED|FILE_FLAG_FIRST_PIPE_INSTANCE,
    PIPE_TYPE_BYTE|PIPE_READMODE_BYTE|PIPE_WAIT|PIPE_REJECT_REMOTE_CLIENTS,1,FrameReader::limit+4,FrameReader::limit+4,0,&security);
  LocalFree(descriptor);
  if(m_pipe==INVALID_HANDLE_VALUE){CloseHandle(m_event);m_event=nullptr;return false;}
  m_notifier=std::make_unique<QWinEventNotifier>(m_event,this);
  connect(m_notifier.get(),&QWinEventNotifier::activated,this,[this]{complete();});
  if(ConnectNamedPipe(m_pipe,&m_overlap))SetEvent(m_event);
  else {
    const auto error=GetLastError();
    if(error==ERROR_PIPE_CONNECTED)SetEvent(m_event);
    else if(error!=ERROR_IO_PENDING){close();return false;}
  }
  return true;
}
void WindowsPrivateListener::complete() {
  if(m_pipe==INVALID_HANDLE_VALUE)return;
  DWORD bytes=0;
  const bool complete=GetOverlappedResult(m_pipe,&m_overlap,&bytes,FALSE);
  if(!complete && GetLastError()!=ERROR_PIPE_CONNECTED){close();return;}
  m_notifier.reset();CloseHandle(m_event);m_event=nullptr;
  const auto pipe=std::exchange(m_pipe,INVALID_HANDLE_VALUE);
  if(accepted)accepted(pipe);else CloseHandle(pipe);
}
void WindowsPrivateListener::close() {
  m_notifier.reset();
  if(m_pipe!=INVALID_HANDLE_VALUE){
    CancelIoEx(m_pipe,&m_overlap);
    DWORD bytes=0;GetOverlappedResult(m_pipe,&m_overlap,&bytes,TRUE);
    CloseHandle(m_pipe);m_pipe=INVALID_HANDLE_VALUE;
  }
  if(m_event){CloseHandle(m_event);m_event=nullptr;}
  m_endpoint.clear();
}
}
