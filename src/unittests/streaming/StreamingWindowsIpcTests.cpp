// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/WindowsPrivateIpc.h"
#include "streaming/PrivateIpc.h"
#include <aclapi.h>
#include <sddl.h>
#include <QSignalSpy>
#include <QTest>
using namespace deskflow::streaming;
class StreamingWindowsIpcTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void policy_data() {
    QTest::addColumn<QString>("aspect");
    for(const char *name:{"read-write","no-server-instance","no-world","no-other-login","session-zero"})QTest::newRow(name)<<QString(name);
  }
  void policy() {
    QFETCH(QString,aspect);auto login=windowsInteractiveLogin();
    if(login.key().isEmpty())qFatal("Real interactive login unavailable");
    if(aspect=="session-zero") {login.session=0;QVERIFY(windowsPrivatePolicy(login,true).isEmpty());return;}
    if(aspect=="no-other-login")login.logon=login.user;
    const auto text=windowsPrivatePolicy(login,true);
    LPWSTR owner=nullptr;
    if(!ConvertSidToStringSidW(const_cast<char *>(login.user.constData()),&owner))qFatal("Current owner SID unavailable");
    const auto complete=QString("O:%1G:%1").arg(QString::fromWCharArray(owner))+text;LocalFree(owner);
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(reinterpret_cast<LPCWSTR>(complete.utf16()),SDDL_REVISION_1,&descriptor,nullptr))qFatal("Policy descriptor invalid");
    if(aspect=="no-world") {
      BOOL present=FALSE,defaulted=FALSE;PACL acl=nullptr;
      if(!GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted) || !present || !acl)qFatal("Missing policy DACL");
      bool world=false;
      for(DWORD i=0;i<acl->AceCount;++i){void *raw=nullptr;if(!GetAce(acl,i,&raw))qFatal("Invalid DACL ACE");
        const auto *ace=static_cast<ACCESS_ALLOWED_ACE *>(raw);world|=IsWellKnownSid(const_cast<DWORD *>(&ace->SidStart),WinWorldSid)!=FALSE;}
      LocalFree(descriptor);QCOMPARE(world,false);return;
    }
    HANDLE process=nullptr,token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&process) ||
      !DuplicateToken(process,SecurityImpersonation,&token))qFatal("Owned token query failed");
    CloseHandle(process);
    if(aspect=="no-other-login"){
      // A process user SID is not a logon SID. Assert the production descriptor
      // contains the exact real logon rather than substituting this broader SID.
      const auto actual=windowsPrivatePolicy(windowsInteractiveLogin(),true);
      CloseHandle(token);LocalFree(descriptor);QVERIFY(actual!=text);return;
    }
    GENERIC_MAPPING mapping{FILE_GENERIC_READ,FILE_GENERIC_WRITE,FILE_GENERIC_EXECUTE,FILE_ALL_ACCESS};
    // AccessCheck requires generic ACE masks expanded for this object type;
    // the named-pipe kernel path performs this mapping when creating an object.
    BOOL daclPresent=FALSE,daclDefaulted=FALSE;PACL mappedAcl=nullptr;
    if(!GetSecurityDescriptorDacl(descriptor,&daclPresent,&mappedAcl,&daclDefaulted) || !daclPresent || !mappedAcl)
      qFatal("AccessCheck policy DACL unavailable");
    for(DWORD i=0;i<mappedAcl->AceCount;++i){
      void *raw=nullptr;if(!GetAce(mappedAcl,i,&raw))qFatal("AccessCheck policy ACE unavailable");
      MapGenericMask(&static_cast<ACCESS_ALLOWED_ACE *>(raw)->Mask,&mapping);
    }
    const DWORD desired=aspect=="no-server-instance"?FILE_CREATE_PIPE_INSTANCE:FILE_READ_DATA|FILE_WRITE_DATA;
    DWORD granted=0,size=sizeof(PRIVILEGE_SET)+256;QByteArray privileges(size,'\0');BOOL allowed=FALSE;
    if(!AccessCheck(descriptor,token,desired,&mapping,reinterpret_cast<PRIVILEGE_SET *>(privileges.data()),&size,&granted,&allowed))qFatal("Real AccessCheck failed");
    CloseHandle(token);LocalFree(descriptor);
    qInfo()<<"Real current-token AccessCheck delegated pipe policy"<<aspect<<bool(allowed);
    QCOMPARE(bool(allowed),aspect=="read-write");
  }
  void realPipe_data(){QTest::addColumn<QString>("aspect");for(const char *name:{"client-to-core","core-to-client","reconnect","single-owner","acl-logon","acl-principals"})QTest::newRow(name)<<QString(name);}
  void impersonationLimit(){
    WindowsPrivateListener listener;const auto endpoint=randomId();QLocalSocket sender,receiver;
    int level=-1;
    listener.accepted=[&](HANDLE handle){
      if(!receiver.setSocketDescriptor(reinterpret_cast<qintptr>(handle),QLocalSocket::ConnectedState))qFatal("Owned native receiver adoption failed");
      connect(&receiver,&QLocalSocket::readyRead,this,[&,handle]{
        receiver.readAll();
        if(!ImpersonateNamedPipeClient(handle))qFatal("Owned same-user pipe impersonation query failed");
        HANDLE token=nullptr;DWORD size=0;SECURITY_IMPERSONATION_LEVEL actual=SecurityAnonymous;
        const bool opened=OpenThreadToken(GetCurrentThread(),TOKEN_QUERY,TRUE,&token);
        const bool queried=opened && GetTokenInformation(token,TokenImpersonationLevel,&actual,sizeof(actual),&size);
        if(token)CloseHandle(token);RevertToSelf();
        if(!queried)qFatal("Owned impersonation level query failed");
        level=int(actual);
      });
    };
    if(!listener.listen(endpoint))qFatal("Owned native listener failed");
    const auto pipe=windowsOpenPrivatePipe(endpoint);
    if(pipe==INVALID_HANDLE_VALUE || !sender.setSocketDescriptor(reinterpret_cast<qintptr>(pipe),QLocalSocket::ConnectedState))qFatal("Owned client adoption failed");
    sender.write("owned");
    if(!QTest::qWaitFor([&]{return level>=0;},3000))qFatal("Owned pipe request not received");
    qInfo()<<"Actual client token impersonation level"<<level;
    QCOMPARE(level,int(SecurityIdentification));
  }
  void realPipe(){
    QFETCH(QString,aspect);PrivateIpcServer server;SessionClient client;const auto endpoint=randomId();
    if(!server.listen(endpoint))qFatal("Native private listener unavailable");
    if(aspect=="single-owner"){PrivateIpcServer rival;QCOMPARE(rival.listen(endpoint),false);return;}
    if(aspect.startsWith("acl-")){
      const auto pipe=windowsOpenPrivatePipe(endpoint);if(pipe==INVALID_HANDLE_VALUE)qFatal("Owned pipe open failed");
      PACL acl=nullptr;PSECURITY_DESCRIPTOR descriptor=nullptr;
      if(GetSecurityInfo(pipe,SE_KERNEL_OBJECT,DACL_SECURITY_INFORMATION,nullptr,nullptr,&acl,nullptr,&descriptor)!=ERROR_SUCCESS || !acl)qFatal("Actual pipe ACL query failed");
      const auto login=windowsInteractiveLogin();bool exact=true;bool found=false;
      for(DWORD i=0;i<acl->AceCount;++i){void *raw=nullptr;if(!GetAce(acl,i,&raw))qFatal("Pipe ACE query failed");
        const auto *ace=static_cast<ACCESS_ALLOWED_ACE *>(raw);auto *sid=const_cast<DWORD *>(&ace->SidStart);
        const bool matches=EqualSid(sid,const_cast<char *>(login.logon.constData()));found|=matches;
        exact&=matches || IsWellKnownSid(sid,WinLocalSystemSid);
      }
      LocalFree(descriptor);CloseHandle(pipe);
      qInfo()<<"Actual native pipe DACL has only exact logon and LocalSystem"<<exact<<"logon ACE present"<<found;
      QCOMPARE(aspect=="acl-logon"?found:exact,true);return;
    }
    QSignalSpy core(&server,&PrivateIpcServer::received),gui(&client,&SessionClient::received);
    client.start(endpoint);
    if(!QTest::qWaitFor([&]{return client.connected() && server.attached();},3000))qFatal("Actual private attachment failed");
    if(aspect=="reconnect"){
      client.shutdown();
      if(!QTest::qWaitFor([&]{return !server.attached();},3000))qFatal("Actual private detach failed");
      client.start(endpoint);
      QTRY_VERIFY_WITH_TIMEOUT(server.attached(),3000);return;
    }
    const auto frame=message("Error",{{"reason","owned-roundtrip"}});
    if(aspect=="core-to-client")server.send(frame);else client.send(frame);
    auto &received=aspect=="core-to-client"?gui:core;
    QTRY_COMPARE_WITH_TIMEOUT(received.count(),1,3000);
    qInfo()<<"Actual restricted native pipe delivered"<<aspect;
  }
};
QTEST_GUILESS_MAIN(StreamingWindowsIpcTests)
#include "StreamingWindowsIpcTests.moc"
