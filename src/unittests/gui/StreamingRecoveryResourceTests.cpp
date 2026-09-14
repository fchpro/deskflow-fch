// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#define NOMINMAX
#include "gui/streaming/SenderController.h"
#include "gui/streaming/SenderWorker.h"
#include "streaming/SessionBroker.h"
#include <QApplication>
#include <QDir>
#include <QLocalServer>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#ifdef Q_OS_WIN
#include <winsock2.h>
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <iphlpapi.h>
#endif
using namespace deskflow::streaming;
namespace deskflow::gui {
class StreamingRecoveryTests : public QObject {
  Q_OBJECT
  void resources(int cycle,const char *phase,qint64 elapsed){
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);DWORD handles=0,threads=0,sockets=0;
    if(!GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory)))qFatal("Process memory telemetry unavailable");
    if(!GetProcessHandleCount(GetCurrentProcess(),&handles))qFatal("Process handle telemetry unavailable");
    const auto snapshot=CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD,0);THREADENTRY32 entry{};entry.dwSize=sizeof(entry);
    if(snapshot==INVALID_HANDLE_VALUE||!Thread32First(snapshot,&entry))qFatal("Process thread telemetry unavailable");
    do{threads+=entry.th32OwnerProcessID==GetCurrentProcessId();}while(Thread32Next(snapshot,&entry));CloseHandle(snapshot);
    ULONG bytes=0;if(GetExtendedUdpTable(nullptr,&bytes,FALSE,AF_INET,UDP_TABLE_OWNER_PID,0)!=ERROR_INSUFFICIENT_BUFFER)qFatal("UDP telemetry size unavailable");QByteArray table(bytes,'\0');
    if(GetExtendedUdpTable(table.data(),&bytes,FALSE,AF_INET,UDP_TABLE_OWNER_PID,0)==NO_ERROR){
      const auto *rows=reinterpret_cast<MIB_UDPTABLE_OWNER_PID *>(table.data());
      for(DWORD i=0;i<rows->dwNumEntries;++i)sockets+=rows->table[i].dwOwningPid==GetCurrentProcessId();
    }else qFatal("UDP telemetry unavailable");
    qInfo()<<"Owned persistent-controller cycle"<<cycle<<phase<<"elapsedMs"<<elapsed<<"handles"<<handles<<"threads"<<threads
      <<"workingSet"<<memory.WorkingSetSize<<"privateBytes"<<memory.PrivateUsage<<"UDP sockets"<<sockets;
#else
    qInfo()<<"Owned persistent-controller cycle"<<cycle<<phase<<"elapsedMs"<<elapsed;
#endif
  }
private Q_SLOTS:
  void repeatedSessions(){
    QTemporaryDir files;QProcess generate;
    generate.start("ffmpeg",{"-hide_banner","-loglevel","error","-nostdin","-y","-f","lavfi","-i","color=c=blue:s=160x96:r=30:d=12",
      "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=12","-c:v","libvpx","-deadline","realtime","-c:a","libopus",files.filePath("owned.webm")});
    if(!generate.waitForFinished(10000)||generate.exitCode())qFatal("Owned media generator failed");
    const QString local(64,'a'),remote(64,'b'),generation(32,'c');
    PrivateIpcServer ipc;SessionClient client;SessionBroker broker;QLocalServer receiverServer;
    const auto endpoint=randomId(),receiverName=randomId();
    if(!ipc.listen(endpoint)||!receiverServer.listen(receiverName))qFatal("Owned IPC endpoints unavailable");
    QElapsedTimer clock;clock.start();QLocalSocket *receiverSocket=nullptr;FrameReader reader;QJsonObject offer,probe;bool peerAttached=true;
    connect(&receiverServer,&QLocalServer::newConnection,this,[&]{
      receiverSocket=receiverServer.nextPendingConnection();reader={};auto *socket=receiverSocket;
      connect(socket,&QLocalSocket::disconnected,socket,&QObject::deleteLater);
      connect(socket,&QLocalSocket::disconnected,this,[&]{broker.detach(remote);peerAttached=false;});
      connect(socket,&QLocalSocket::readyRead,this,[&]{reader.feed(receiverSocket->readAll(),[&](const QJsonObject &frame){
        if(frame["type"]=="Identity"){probe=frame["data"].toObject();return;}
        if(!broker.dispatch(remote,generation,frame,clock.elapsed())&&!broker.sessions().isEmpty())qFatal("Owned receiver signaling failed");
      });});
    });
    const QJsonObject caps{{"sources",QJsonArray{"file"}},{"receive",true},{"audio",QJsonArray{"off","file"}},{"control",false}};
    connect(&broker,&SessionBroker::deliver,this,[&](const QString &peer,const QJsonObject &frame){
      if(peer==local)ipc.send(frame);else if(frame["type"]=="Offer")offer=frame["data"].toObject();
      else if(receiverSocket)receiverSocket->write(FrameReader::encode(frame));
    });
    connect(&ipc,&PrivateIpcServer::attachedChanged,this,[&](bool attached){
      if(!attached){broker.detach(local);return;}
      ipc.send(message("Identity",{{"id",local},{"address","127.0.0.1"}}));
      if(!broker.attach({local,"Owned sender",generation,QHostAddress::LocalHost,caps})||
        !broker.attach({remote,"Owned receiver",generation,QHostAddress::LocalHost,caps}))qFatal("Owned broker attach failed");
    });
    connect(&ipc,&PrivateIpcServer::received,this,[&](const QJsonObject &frame){if(!broker.dispatch(local,generation,frame,clock.elapsed())&&!broker.sessions().isEmpty())qFatal("Owned sender signaling failed");});
    SenderController controller(&client);client.start(endpoint);
    if(!QTest::qWaitFor([&]{return controller.inventory()["id"]==local&&controller.inventory()["peers"].toArray().size()==2;},3000))qFatal("Owned IPC inventory unavailable");
    resources(0,"before-warmup",0);
    for(int cycle=1;cycle<=12;++cycle){
      offer={};probe={};QProcess receiver;
      if(!peerAttached){if(!broker.attach({remote,"Owned receiver",generation,QHostAddress::LocalHost,caps}))qFatal("Fresh receiver reconnect failed");peerAttached=true;}
      receiver.start(QDir(QCoreApplication::applicationDirPath()).filePath("StreamingSenderTests.exe"),{"--receiver",receiverName});
      receiverSocket=nullptr;
      if(!QTest::qWaitFor([&]{return receiverSocket!=nullptr;},3000))qFatal("Owned receiver process unavailable");
      if(!QTest::qWaitFor([&]{for(const auto &value:controller.inventory()["peers"].toArray())if(value.toObject()["id"]==remote)return true;return false;},3000))qFatal("Reconnected peer inventory unavailable");
      controller.start({{"kind","file"},{"path",files.filePath("owned.webm")},{"to",remote},{"audio","file"},{"preset","low"},{"interactive",false}});
      if(!QTest::qWaitFor([&]{return !offer.isEmpty();},3000))qFatal("Fresh explicit offer unavailable");
      qInfo()<<"Actual fresh offer before consent"<<cycle<<"session"<<offer["session"]<<"source"<<offer["source"]
        <<"broker state"<<broker.sessions().first().state;
      if(!broker.dispatch(remote,generation,message("Accept",{{"session",offer["session"]},{"source",offer["source"]}}),clock.elapsed()))qFatal("Fresh explicit consent rejected");
      qInfo()<<"Actual explicit receiver Accept"<<cycle<<"session"<<offer["session"]<<"source"<<offer["source"]
        <<"broker state"<<broker.sessions().first().state;
      if(!QTest::qWaitFor([&]{return probe["decoded"].toInt()>=3&&probe["energy"].toDouble()>0.1;},10000))qFatal("Real decoded file/PCM fixture unavailable");
      qInfo()<<"Actual owned receiver cycle"<<cycle<<"decoded"<<probe["decoded"]<<"PCM energy"<<probe["energy"];
      resources(cycle,"streaming",0);QElapsedTimer stop;stop.start();
      const auto action=cycle%3==0?"receiver-process-exit":cycle%3==1?"sender-stop":"receiver-stop";
      qInfo()<<"Actual lifecycle action"<<cycle<<action;
      if(cycle%3==0)receiver.kill();else if(cycle%3==1)controller.stop();else broker.dispatch(remote,generation,message("Stop",{{"session",offer["session"]},{"source",offer["source"]}}),clock.elapsed());
      if(!QTest::qWaitFor([&]{return !controller.active()&&broker.sessions().isEmpty()&&receiver.state()==QProcess::NotRunning&&!peerAttached;},3000))qFatal("Owned Stop and private peer detach completion unavailable");
      bool released=false;QMetaObject::invokeMethod(controller.m_worker,[&]{released=!controller.m_worker->m_media;},Qt::BlockingQueuedConnection);
      resources(cycle,"stopped",stop.elapsed());
      QCOMPARE(released,true);
      receiverSocket=nullptr;QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    }
    client.shutdown();resources(12,"private-IPC-detached",0);
  }
};
}
QTEST_MAIN(deskflow::gui::StreamingRecoveryTests)
#include "StreamingRecoveryResourceTests.moc"
