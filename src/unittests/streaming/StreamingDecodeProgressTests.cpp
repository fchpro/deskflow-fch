// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/MediaTransport.h"
#include "streaming/SessionBroker.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QNetworkDatagram>
#include <QProcess>
#include <QRegularExpression>
#include <QTest>
#include <QTimer>
#include <QUdpSocket>
#include <cmath>
using namespace deskflow::streaming;
namespace {
QString session(32,'d'),source(32,'e');
const QString generation(32,'c'),senderId(64,'a'),receiverId(64,'b');
int endpoint(QCoreApplication &app) {
  qInstallMessageHandler(+[](QtMsgType,const QMessageLogContext &,const QString &text){fprintf(stderr,"%s\n",qPrintable(text));fflush(stderr);});
  const auto args=app.arguments();const bool sending=args[3]=="sender";const auto scenario=args[4];
  QLocalSocket socket;FrameReader reader;MediaTransport media;QElapsedTimer clock;clock.start();
  bool holding=false,ready=false,reused=false,awaitingRestart=false;int decoded=0,sequence=0;qint64 lastVideo=-40,lastAudio=-20,connected=-1,lastReport=0;double energy=0;VideoFrame saved;
  QObject::connect(&media,&MediaTransport::signaling,&socket,[&](const auto &frame){socket.write(FrameReader::encode(frame));});
  QObject::connect(&media,&MediaTransport::failed,&app,[&](const QString &reason){
    if(scenario=="reuse"&&!reused&&reason=="Video stopped updating. Start a new stream to try again."){
      qInfo()<<"First-session decode stall before same-instance restart"<<reason;media.stop();reused=true;awaitingRestart=true;
      socket.write(FrameReader::encode(message("Identity",{{"restart",true}})));return;
    }
    qInfo().noquote()<<"RESULT"<<reason;app.exit(2);
  });
  QObject::connect(&socket,&QLocalSocket::readyRead,&app,[&]{reader.feed(socket.readAll(),[&](const QJsonObject &frame){
    const auto data=frame["data"].toObject();
    if(frame["type"]=="Identity"&&data["hold"].toBool()){holding=true;qInfo()<<"Owned source pause requested after receiver progress";return;}
    if(frame["type"]=="State"&&data["state"]=="negotiating"&&!media.active()){
      session=data["session"].toString();source=data["source"].toString();
      awaitingRestart=false;ready=false;sequence=0;decoded=0;saved={};connected=-1;holding=false;lastVideo=-40;lastAudio=-20;clock.restart();
      if(!media.start({session,source,"low",QHostAddress::LocalHost,QHostAddress::LocalHost,sending,true}))app.exit(3);
    }else if(frame["type"]=="Stopped"){media.receive(frame);qInfo()<<"Peer Stop active"<<media.active();if(scenario=="reuse"&&(!reused||awaitingRestart)){reused=true;awaitingRestart=true;return;}app.exit(0);
    }else if(QStringList{"SdpOffer","SdpAnswer","IceCandidate"}.contains(frame["type"].toString())){
      if(!media.receive(frame))app.exit(4);
    }
  });});
  socket.connectToServer(args[2]);if(!socket.waitForConnected(3000))return 5;
  QTimer feed;feed.setInterval(10);
  QObject::connect(&feed,&QTimer::timeout,&app,[&]{
    const auto ms=clock.elapsed();if(ms>10000){qInfo()<<"Fixture endpoint deadline";app.exit(6);return;}
    if(!media.active())return;
    if(sending){
      if(ms-lastVideo>=40&&!holding){lastVideo=ms;VideoFrame frame;
        if((scenario=="static"||reused)&&!saved.pixels.isNull())frame=saved;
        else {frame.pixels=QImage(320,180,QImage::Format_ARGB32);quint32 random=quint32(sequence+1);
          for(int y=0;y<180;++y){auto *row=reinterpret_cast<QRgb *>(frame.pixels.scanLine(y));for(int x=0;x<320;++x){random^=random<<13;random^=random>>17;random^=random<<5;row[x]=0xff000000|(random&0xffffff);}}
          frame.session=session;frame.source=source;frame.sequence=++sequence;frame.timelineEpoch=1;frame.captureTimeNs=audioHostTimeNs();saved=frame;
        }
        frame.mediaTimeNs=ms*GST_MSECOND;media.pushVideo(frame);
      }
      if(ms-lastAudio>=20&&!holding){lastAudio=ms;AudioBlock block{QByteArray(960*8,Qt::Uninitialized),session,source,1,ms*GST_MSECOND,20*GST_MSECOND,audioHostTimeNs()};
        auto *samples=reinterpret_cast<float *>(block.samples.data());for(int i=0;i<960;++i)samples[2*i]=samples[2*i+1]=float(0.05*std::sin((ms*48+i)*440*2*3.141592653589793/48000));media.pushAudio(block);
      }
    }else {
      while(auto frame=media.takeVideo()){++decoded;qInfo()<<"Actual decoded source progress"<<decoded<<frame->sequence<<frame->mediaTimeNs;
        socket.write(FrameReader::encode(message("Identity",{{"decoded",decoded},{"sequence",double(frame->sequence)}})));
      }
      while(auto block=media.takeAudio()){const auto *samples=reinterpret_cast<const float *>(block->samples.constData());for(int i=0;i<block->samples.size()/4;++i)energy+=samples[i]*samples[i];}
    }
    const auto stats=media.statistics();if(stats.connected&&connected<0)connected=ms;
    if(!ready&&stats.connected&&(sending?stats.sentPackets>0:decoded>0)){ready=true;socket.write(FrameReader::encode(message("Ready",{{"session",session},{"source",source}})));}
    if(ms-lastReport>=1000){lastReport=ms;qInfo()<<"Actual source/decoded/PCM progress"<<(sending?"sender":"receiver")<<ms<<"submitted"<<stats.videoSubmitted<<"decoded"<<decoded<<"energy"<<energy<<"packets"<<stats.sentPackets<<stats.receivedPackets<<"requests"<<stats.keyframeRequests;}
    if(connected>=0&&ms-connected>=5500){if(!sending)qInfo().noquote()<<"RESULT remains-active";app.exit(0);}
  });feed.start();const int result=app.exec();media.stop();qInfo()<<"Actual stopped active"<<media.active();return result;
}
}
class StreamingDecodeProgressTests:public QObject {
 Q_OBJECT
private Q_SLOTS:
 void progress_data(){
  QTest::addColumn<QString>("scenario");QTest::addColumn<QString>("aspect");
  for(const auto *name:{"outcome","sender-stop","receiver-cleanup","receiver-exit"})QTest::newRow(name)<<QString("moving-loss")<<QString(name);
  QTest::newRow("same-instance")<<QString("reuse")<<QString("outcome");
  QTest::newRow("static")<<QString("static")<<QString("outcome");QTest::newRow("paused")<<QString("paused")<<QString("outcome");
 }
 void progress(){
  QFETCH(QString,scenario);QFETCH(QString,aspect);session=QString(32,'d');source=QString(32,'e');QLocalServer server;const auto endpointName=randomId();if(!server.listen(endpointName))qFatal("Owned signaling unavailable");
  SessionBroker broker;QProcess sending,receiving;QElapsedTimer clock;clock.start();
  QTimer expiry;expiry.setInterval(100);connect(&expiry,&QTimer::timeout,this,[&]{broker.expire(clock.elapsed());});expiry.start();
  QUdpSocket front,back;quint16 senderPort=0,receiverPort=0;int lost=0,forwardedVideo=0;bool lossActive=false,observed=false,restarted=false;
  if(!front.bind(QHostAddress::LocalHost,24830)||!back.bind(QHostAddress::LocalHost,24831))qFatal("Owned encrypted relay unavailable");
  connect(&front,&QUdpSocket::readyRead,this,[&]{while(front.hasPendingDatagrams()){
    auto packet=front.receiveDatagram();if(!receiverPort)continue;
    const bool video=packet.data().size()>=12&&(quint8(packet.data()[0])&0xc0)==0x80&&(quint8(packet.data()[1])&0x7f)==96;
    if(video&&lossActive&&(quint8(packet.data()[1])&0x80)){++lost;continue;}
    if(video&&lossActive)++forwardedVideo;
    back.writeDatagram(packet.data(),QHostAddress::LocalHost,receiverPort);
  }});
  connect(&back,&QUdpSocket::readyRead,this,[&]{while(back.hasPendingDatagrams()){const auto packet=back.receiveDatagram();if(senderPort)front.writeDatagram(packet.data(),QHostAddress::LocalHost,senderPort);}});
  const QJsonObject caps{{"sources",QJsonArray{"screen"}},{"receive",true},{"audio",QJsonArray{"off","system"}},{"control",false}};
  if(!broker.attach({senderId,"Owned sender",generation,QHostAddress::LocalHost,caps})||!broker.attach({receiverId,"Owned receiver",generation,QHostAddress::LocalHost,caps}))qFatal("Broker fixture unavailable");
  std::vector<QLocalSocket *> clients;std::vector<std::unique_ptr<FrameReader>> readers;
  connect(&server,&QLocalServer::newConnection,this,[&]{
    auto *socket=server.nextPendingConnection();const auto id=clients.empty()?senderId:receiverId;clients.push_back(socket);readers.push_back(std::make_unique<FrameReader>());auto *reader=readers.back().get();
    connect(socket,&QLocalSocket::readyRead,this,[&,socket,id,reader]{reader->feed(socket->readAll(),[&](const QJsonObject &frame){
      if(frame["type"]=="Identity"){
        if(id==receiverId&&frame["data"].toObject()["restart"].toBool()){
          restarted=true;observed=false;lossActive=false;session=QString(32,'f');source=QString(32,'1');
          qInfo()<<"Actual fresh offer/Accept for retained MediaTransport instance"<<session<<source;
          if(!broker.dispatch(senderId,generation,message("Offer",{{"session",session},{"to",receiverId},{"source",source},{"kind","screen"},{"audio","system"},{"preset","low"},{"interactive",false}}),clock.elapsed())||!broker.dispatch(receiverId,generation,message("Accept",{{"session",session},{"source",source}}),clock.elapsed()))qFatal("Fresh reuse consent unavailable");
          return;
        }
        if(id==receiverId&&!observed&&frame["data"].toObject()["decoded"].toInt()>=(scenario=="static"||restarted?1:5)){
          observed=true;lossActive=scenario=="moving-loss"||(scenario=="reuse"&&!restarted);qInfo()<<"Actual first decoded phase observed"<<frame<<"encrypted marker loss active"<<lossActive;
          if(scenario=="paused")clients[0]->write(FrameReader::encode(message("Identity",{{"hold",true}})));
        }return;
      }
      auto forwarded=frame;
      if(frame["type"]=="IceCandidate"){
        auto data=frame["data"].toObject();auto tokens=data["candidate"].toString().split(' ');(id==senderId?senderPort:receiverPort)=tokens[5].toUShort();tokens[5]=id==senderId?"24831":"24830";data["candidate"]=tokens.join(' ');forwarded["data"]=data;
      }
      if(!broker.dispatch(id,generation,forwarded,clock.elapsed())&&!broker.sessions().isEmpty())qFatal("Owned broker signaling rejected");
    });});
  });
  connect(&broker,&SessionBroker::deliver,this,[&](const QString &id,const QJsonObject &frame){const int index=id==senderId?0:1;if(clients.size()>size_t(index))clients[index]->write(FrameReader::encode(frame));});
  const auto launch=[&](QProcess &process,const QString &role){process.start(QCoreApplication::applicationFilePath(),{"--endpoint",endpointName,role,scenario});if(!process.waitForStarted(3000))qFatal("Owned endpoint unavailable");};
  launch(sending,"sender");if(!QTest::qWaitFor([&]{return clients.size()==1;},3000))qFatal("Sender signaling unavailable");
  launch(receiving,"receiver");if(!QTest::qWaitFor([&]{return clients.size()==2;},3000))qFatal("Receiver signaling unavailable");
  if(!broker.dispatch(senderId,generation,message("Offer",{{"session",session},{"to",receiverId},{"source",source},{"kind","screen"},{"audio","system"},{"preset","low"},{"interactive",false}}),clock.elapsed())||!broker.dispatch(receiverId,generation,message("Accept",{{"session",session},{"source",source}}),clock.elapsed()))qFatal("Explicit consent unavailable");
  const bool ended=QTest::qWaitFor([&]{return sending.state()==QProcess::NotRunning&&receiving.state()==QProcess::NotRunning;},scenario=="reuse"?15000:12000);
  if(!ended){sending.kill();receiving.kill();if(!sending.waitForFinished(3000)||!receiving.waitForFinished(3000))qFatal("Owned endpoint process cleanup exceeded3s");}
  const auto senderLog=sending.readAllStandardError(),receiverLog=receiving.readAllStandardError();qInfo().noquote()<<senderLog<<receiverLog;
  qInfo()<<"Actual encrypted loss/forwarded video"<<lost<<forwardedVideo<<"exit codes"<<sending.exitCode()<<receiving.exitCode();
  if(!ended||(!observed&&scenario!="reuse")||((scenario=="moving-loss"||scenario=="reuse")&&(!lost||!forwardedVideo))||(scenario=="reuse"&&!restarted))qFatal("Selective loss fixture did not exercise required path");
  const auto match=QRegularExpression("RESULT ([^\\r\\n]+)").match(QString::fromUtf8(receiverLog));
  if(aspect=="sender-stop")QVERIFY(senderLog.contains("Peer Stop active false"));
  else if(aspect=="receiver-cleanup")QVERIFY(receiverLog.contains("Actual stopped active false"));
  else if(aspect=="receiver-exit")QCOMPARE(receiving.exitCode(),2);
  else QCOMPARE(match.captured(1).trimmed(),scenario=="moving-loss"?QString("Video stopped updating. Start a new stream to try again."):QString("remains-active"));
 }
};
int main(int argc,char **argv){QCoreApplication app(argc,argv);if(app.arguments().contains("--endpoint"))return endpoint(app);StreamingDecodeProgressTests tests;return QTest::qExec(&tests,argc,argv);}
#include "StreamingDecodeProgressTests.moc"
