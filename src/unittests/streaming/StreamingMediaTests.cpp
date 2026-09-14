// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#define NOMINMAX
#include "streaming/MediaTransport.h"
#include "streaming/SessionBroker.h"
#include <QCoreApplication>
#include <QDir>
#include <QLocalServer>
#include <QLocalSocket>
#include <QJsonDocument>
#include <QProcess>
#include <QTest>
#include <QTimer>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <algorithm>
#include <ctime>
#include <deque>
#include <cmath>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#endif
using namespace deskflow::streaming;
namespace {
const QString session(32,'d'), source(32,'e'), generation(32,'c'), senderId(64,'a'), receiver(64,'b');
void logResources() {
#ifdef Q_OS_WIN
      PROCESS_MEMORY_COUNTERS_EX memory{};memory.cb=sizeof(memory);
      if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),sizeof(memory)))
        qInfo()<<"Process workingSet/private/peak bytes"<<memory.WorkingSetSize<<memory.PrivateUsage<<memory.PeakWorkingSetSize;
      FILETIME create,exit,kernel,user;
      if(GetProcessTimes(GetCurrentProcess(),&create,&exit,&kernel,&user)){
        const quint64 ticks=(quint64(kernel.dwHighDateTime)<<32)+kernel.dwLowDateTime+(quint64(user.dwHighDateTime)<<32)+user.dwLowDateTime;
        qInfo()<<"Process actual kernel+user CPU ms"<<ticks/10000.0;
      }
#else
      qInfo()<<"Process CPU ms"<<1000.0*std::clock()/CLOCKS_PER_SEC;
#endif
}
int endpoint(QCoreApplication &app) {
  qInstallMessageHandler(+[](QtMsgType,const QMessageLogContext &,const QString &text){fprintf(stderr,"%s\n",qPrintable(text));fflush(stderr);});
  const auto args=app.arguments();const bool sending=args[3]=="sender", audio=args[4]=="on";
  const bool staticSource=qEnvironmentVariableIsSet("STREAMING_MEDIA_STATIC");
  const bool seek=qEnvironmentVariableIsSet("STREAMING_MEDIA_SEEK");
  const bool slow=qEnvironmentVariableIsSet("STREAMING_MEDIA_SLOW");
  const bool bandwidth=qEnvironmentVariableIsSet("STREAMING_MEDIA_BANDWIDTH");
  const bool overload=qEnvironmentVariableIsSet("STREAMING_MEDIA_OVERLOAD");
  const QString filePath=qEnvironmentVariable("STREAMING_MEDIA_FILE");
  FileSource file;bool fileStarted=false;
  QLocalSocket socket;FrameReader reader;MediaTransport transport;
  QObject::connect(&transport,&MediaTransport::signaling,&socket,[&](const auto &frame){socket.write(FrameReader::encode(frame));});
  QObject::connect(&transport,&MediaTransport::failed,&app,[&](const auto &){app.exit(2);});
  QObject::connect(&socket,&QLocalSocket::readyRead,&app,[&]{reader.feed(socket.readAll(),[&](const QJsonObject &frame){
    if(frame["type"]=="State"&&frame["data"].toObject()["state"]=="negotiating"&&!transport.active()){
      if(!transport.start({session,source,"low",QHostAddress::LocalHost,QHostAddress::LocalHost,sending,audio}))app.exit(3);
    }else if(frame["type"]=="Stopped"){transport.receive(frame);qInfo()<<"Peer Stop propagated active"<<transport.active();app.exit(0);
    }else if(QStringList{"SdpOffer","SdpAnswer","IceCandidate"}.contains(frame["type"].toString())){
      if(!transport.receive(frame)){qWarning()<<"Rejected signaling"<<frame["type"]<<transport.error();app.exit(4);}
    }
  });});
  socket.connectToServer(args[2]);if(!socket.waitForConnected(3000))return 5;
  QElapsedTimer clock;clock.start();QTimer feed;feed.setInterval(10);
  bool ready=false;int sequence=0; qint64 lastVideo=-40,lastAudio=-20,connected=-1;bool red=false,blue=false,sawNewEpoch=false,sawResize=false;double energy=0;int decoded=0;
  std::vector<double> latency;quint64 lastSequence=0;
  QObject::connect(&feed,&QTimer::timeout,&app,[&]{
    const auto ms=clock.elapsed();if(ms>9000){qWarning()<<"Endpoint deadline";app.exit(6);return;}
    if(!transport.active())return;
    if(sending&&!filePath.isEmpty()){
      if(!fileStarted){fileStarted=true;if(!file.open(filePath,session,source,audio)){qWarning()<<file.error();app.exit(9);return;}}
      if(file.state()==FilePlaybackState::Paused)file.command(session,source,"resume");
      if(auto frame=file.takeFrame())transport.pushVideo(*frame);
      while(auto block=file.takeAudio())transport.pushAudio(fileAudioBlock(*block));
    }else if(sending){
      if(ms-lastVideo>=34&&(!staticSource||sequence==0)){lastVideo=ms;VideoFrame frame;
        const bool changed=seek&&connected>=0&&ms-connected>=700;
        frame.pixels=QImage(changed?400:320,changed?220:180,QImage::Format_ARGB32);frame.pixels.fill((sequence/10)%2?QColor(0,40,255):QColor(255,20,0));
        if(bandwidth){quint32 random=quint32(sequence+1);for(int y=0;y<frame.pixels.height();++y){auto *row=reinterpret_cast<QRgb *>(frame.pixels.scanLine(y));for(int x=0;x<frame.pixels.width();++x){if(x>=80&&x<=120&&y>=60&&y<=100)continue;random^=random<<13;random^=random>>17;random^=random<<5;row[x]=0xff000000|(random&0xffffff);}}}
        frame.session=session;frame.source=source;frame.sequence=++sequence;frame.mediaTimeNs=(changed?ms-600:ms)*GST_MSECOND;frame.captureTimeNs=audioHostTimeNs();frame.timelineEpoch=changed?2:1;frame.geometryGeneration=7;frame.physicalGeometry=QRect(20,30,640,360);frame.scale=2;frame.coordinateMappingValid=true;
        transport.pushVideo(frame);
      }
      if(audio&&ms-lastAudio>=20){lastAudio=ms;AudioBlock block{QByteArray(960*8,Qt::Uninitialized),session,source,quint64(seek&&connected>=0&&ms-connected>=700?2:1),(seek&&connected>=0&&ms-connected>=700?ms-600:ms)*GST_MSECOND,20*GST_MSECOND,audioHostTimeNs()};
        auto *pcm=reinterpret_cast<float *>(block.samples.data());for(int i=0;i<960;++i)pcm[2*i]=pcm[2*i+1]=float(0.05*std::sin((ms*48+i)*440*2*3.141592653589793/48000));transport.pushAudio(block);
      }
    }else if(!(slow&&connected>=0&&ms-connected>=400&&ms-connected<900)){
      while(auto frame=transport.takeVideo()){
        ++decoded;if(frame->captureTimeNs>0)latency.push_back((audioHostTimeNs()-frame->captureTimeNs)/1000000.0);const auto pixel=frame->pixels.pixelColor(100,80);
        if(frame->sequence<=lastSequence||frame->captureTimeNs<=0||frame->session!=session||frame->source!=source||(filePath.isEmpty()&&(frame->timelineEpoch<(sawNewEpoch?2U:1U)||frame->geometryGeneration!=7||frame->physicalGeometry!=QRect(20,30,640,360)||frame->scale!=2||!frame->coordinateMappingValid))){qWarning()<<"Metadata mismatch";app.exit(7);return;}
        lastSequence=frame->sequence;
        if(frame->timelineEpoch==2&&!sawNewEpoch){sawNewEpoch=true;if(!args[5].isEmpty())frame->pixels.save(args[5]+"/decoded-resized.png");}
        if(frame->pixels.size()==QSize(400,220))sawResize=true;
        if(pixel.red()>200&&pixel.blue()<50&&!red){red=true;if(!args[5].isEmpty())frame->pixels.save(args[5]+"/decoded-red.png");qInfo()<<"Decoded red sequence"<<frame->sequence<<"mediaNs"<<frame->mediaTimeNs<<"captureNs"<<frame->captureTimeNs;}
        if(pixel.blue()>200&&pixel.red()<50&&!blue){blue=true;if(!args[5].isEmpty())frame->pixels.save(args[5]+"/decoded-blue.png");qInfo()<<"Decoded blue sequence"<<frame->sequence<<"mediaNs"<<frame->mediaTimeNs<<"captureNs"<<frame->captureTimeNs;}
      }
      while(auto block=transport.takeAudio()){const auto *pcm=reinterpret_cast<const float *>(block->samples.constData());for(int i=0;i<block->samples.size()/4;++i)energy+=pcm[i]*pcm[i];}
    }
    const auto stats=transport.statistics();if(stats.connected&&connected<0)connected=ms;
    if(!ready&&stats.connected&&(sending?stats.sentPackets>0:decoded>0)){ready=true;socket.write(FrameReader::encode(message("Ready",{{"session",session},{"source",source}})));}
    if(connected>=0&&ms-connected>=(overload?3500:1500)){
      qInfo()<<"Endpoint result"<<(sending?"sender":"receiver")<<"epoch2"<<sawNewEpoch<<"resized"<<sawResize<<"decoded"<<decoded<<"red"<<red<<"blue"<<blue<<"audioEnergy"<<energy<<"packets"<<stats.sentPackets<<stats.receivedPackets<<"audioPackets"<<stats.audioPackets<<"GCC"<<stats.congestionControl<<"keyframeRequests"<<stats.keyframeRequests<<"startupRetries"<<stats.startupRetries;
      if(!latency.empty()){std::sort(latency.begin(),latency.end());qInfo()<<"Same-host source handoff to decoded consumption ms n/p50/p95/max"<<latency.size()<<latency[latency.size()/2]<<latency[size_t((latency.size()-1)*0.95)]<<latency.back();}
      const bool ok=sending?(stats.sentPackets>0&&stats.startupRetries>=2&&stats.keyframeRequests>=3&&stats.congestionControl&&stats.queuedVideo<=2&&stats.queuedAudioBytes<=23040&&stats.pacerBytes<=37500&&(audio?stats.audioPackets>0:stats.audioPackets==0)):(stats.boundedJitterBuffers==quint64(audio?2:1)&&decoded>=(staticSource?1:bandwidth?5:10)&&red&&(staticSource||blue)&&(!seek||(sawNewEpoch&&sawResize))&&(audio?energy>0.1:energy==0));
      app.exit(ok?0:8);
    }
  });feed.start();const int result=app.exec();logResources();QElapsedTimer teardown;teardown.start();transport.stop();qInfo()<<"Media resources released active"<<transport.active()<<"teardownMs"<<teardown.elapsed();return teardown.elapsed()>2000?10:result;
}
}
class StreamingMediaTests:public QObject {
 Q_OBJECT
private Q_SLOTS:
 void epochZeroAudioOutput(){
  gst_init(nullptr,nullptr);AudioOutput output;auto *sink=gst_element_factory_make("appsink",nullptr);gst_object_ref(sink);
  const bool started=output.startSink(sink,session,source,0,0);
  AudioBlock block{QByteArray(480*8,'\0'),session,source,0,0,10*GST_MSECOND,0};
  const bool pushed=started&&output.push(block);auto *sample=pushed?gst_app_sink_try_pull_sample(GST_APP_SINK(sink),3*GST_SECOND):nullptr;
  const bool rendered=started&&pushed&&sample&&gst_buffer_get_size(gst_sample_get_buffer(sample))==quint64(block.samples.size());
  if(sample)gst_sample_unref(sample);gst_object_unref(sink);QVERIFY(rendered);
 }
 void invalidNegotiation_data(){QTest::addColumn<QString>("fault");for(const char *name:{"codec","direction","twcc","pli","source","session","audio","candidate-index","candidate-address"})QTest::newRow(name)<<QString(name);}
 void invalidNegotiation(){
  QFETCH(QString,fault);MediaTransport transport;QSignalSpy signals(&transport,&MediaTransport::signaling);
  if(!transport.start({session,source,"low",QHostAddress::LocalHost,QHostAddress::LocalHost,true,fault=="audio"}))qFatal("Negotiation fixture failed");
  VideoFrame frame;frame.pixels=QImage(8,8,QImage::Format_ARGB32);frame.pixels.fill(Qt::red);frame.session=session;frame.source=source;transport.pushVideo(frame);
  QJsonObject offer;
  if(!QTest::qWaitFor([&]{for(const auto &entry:signals){auto value=entry[0].toJsonObject();if(value["type"]=="SdpOffer")offer=value;}return !offer.isEmpty();},3000))qFatal("Real SDP offer unavailable");
  transport.stop();if(!transport.start({session,source,"low",QHostAddress::LocalHost,QHostAddress::LocalHost,false,false}))qFatal("Receiver fixture failed");
  auto data=offer["data"].toObject();auto sdp=data["sdp"].toString();
  if(fault=="codec")sdp.replace("VP8/90000","H264/90000");
  if(fault=="direction")sdp.replace("a=sendonly","a=sendrecv");
  if(fault=="pli")sdp.replace("nack pli","x-no-keyframe");
  if(fault=="twcc")sdp.replace("transport-cc","x-unsupported-feedback");
  if(fault=="source")data["source"]=QString(32,'f');
  if(fault=="session")data["session"]=QString(32,'f');
  data["sdp"]=sdp;offer["data"]=data;
  if(fault.startsWith("candidate"))offer=message("IceCandidate",{{"session",session},{"source",source},{"candidate",QString("candidate:1 1 UDP 2015363327 %1 24802 typ host").arg(fault=="candidate-address"?"192.0.2.99":"127.0.0.1")},{"mline",fault=="candidate-index"?1:0}});
  QVERIFY(!transport.receive(offer));
 }
 void quantizedThirtyFps(){
  MediaTransport transport;if(!transport.start({session,source,"low",QHostAddress::LocalHost,QHostAddress::LocalHost,true,false}))qFatal("Pacing graph failed");
  VideoFrame frame;frame.pixels=QImage(8,8,QImage::Format_ARGB32);frame.pixels.fill(Qt::red);frame.session=session;frame.source=source;
  for(int i=0;i<30;++i){frame.sequence=i;frame.mediaTimeNs=qRound64(i*1000.0/30)*GST_MSECOND;transport.pushVideo(frame);}
  QCOMPARE(transport.statistics().videoSubmitted,quint64(30));
 }
 void transport_data(){
  QTest::addColumn<bool>("audio");QTest::addColumn<QString>("scenario");
  QTest::newRow("video-only")<<false<<QString();QTest::newRow("audio-video")<<true<<QString();
  QTest::newRow("seek-resize")<<true<<QString("seek");QTest::newRow("slow-receiver")<<true<<QString("slow");
  QTest::newRow("file-epoch-zero")<<true<<QString("file");QTest::newRow("static-startup-loss")<<false<<QString("static");QTest::newRow("startup-loss")<<true<<QString("startup");QTest::newRow("loss")<<true<<QString("loss");QTest::newRow("limited-bandwidth")<<true<<QString("bandwidth");QTest::newRow("overload")<<true<<QString("overload");
 }
 void transport(){
  QFETCH(bool,audio);QFETCH(QString,scenario);QLocalServer server;const auto endpoint=randomId();if(!server.listen(endpoint))qFatal("Local test signaling failed");
  QProcess sending,receiving;SessionBroker broker;QElapsedTimer clock;clock.start();
  QTimer expiry;expiry.setInterval(100);QObject::connect(&expiry,&QTimer::timeout,&server,[&]{broker.expire(clock.elapsed());});expiry.start();
  QTemporaryDir files;
  if(scenario=="file"){
    QProcess generate;generate.start("ffmpeg",{"-hide_banner","-loglevel","error","-nostdin","-y","-f","lavfi","-i","color=c=red:s=160x96:r=30:d=1","-f","lavfi","-i","color=c=blue:s=160x96:r=30:d=3","-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=4","-filter_complex","[0:v][1:v]concat=n=2:v=1:a=0[v]","-map","[v]","-map","2:a","-c:v","libvpx","-deadline","realtime","-c:a","libopus",files.filePath("owned.webm")});
    if(!generate.waitForFinished(10000)||generate.exitCode()!=0)qFatal("Owned real-file fixture generation failed");
  }
  // Test-only network emulator forwards actual encrypted UDP. Product transport
  // retains its original pinned direct-peer candidate policy; no media mock.
  QUdpSocket front,back;quint16 senderPort=0,receiverPort=0;int mediaPackets=0,lost=0;QByteArray firstVideoTimestamp;
  const bool network=scenario=="static"||scenario=="startup"||scenario=="loss"||scenario=="bandwidth"||scenario=="overload";
  const int rate=scenario=="overload"?1000000:10000000;
  std::deque<std::pair<qint64,QByteArray>> queued,returnQueued; qint64 networkDue=0;quint64 forwardedBytes=0;QTimer networkPacer;networkPacer.setInterval(1);
  QObject::connect(&networkPacer,&QTimer::timeout,&server,[&]{while(!queued.empty()&&queued.front().first<=clock.nsecsElapsed()){back.writeDatagram(queued.front().second,QHostAddress::LocalHost,receiverPort);forwardedBytes+=queued.front().second.size();queued.pop_front();}while(!returnQueued.empty()&&returnQueued.front().first<=clock.nsecsElapsed()){front.writeDatagram(returnQueued.front().second,QHostAddress::LocalHost,senderPort);returnQueued.pop_front();}});networkPacer.start();
  if(network){
    if(!front.bind(QHostAddress::LocalHost,24830)||!back.bind(QHostAddress::LocalHost,24831))qFatal("Network emulator ports unavailable");
    QObject::connect(&front,&QUdpSocket::readyRead,&server,[&]{while(front.hasPendingDatagrams()){
      auto packet=front.receiveDatagram();if(!receiverPort)continue;
      if((scenario=="startup"||scenario=="static")&&packet.data().size()>=12&&(quint8(packet.data()[1])&0x7f)==96){const auto stamp=packet.data().mid(4,4);if(firstVideoTimestamp.isEmpty())firstVideoTimestamp=stamp;if(stamp==firstVideoTimestamp){++lost;continue;}}
      if(!packet.data().isEmpty()&&(quint8(packet.data()[0])&0xc0)==0x80 && ++mediaPackets%(scenario=="loss"?20:100)==0&&scenario!="overload"){++lost;continue;}
      if(scenario!="loss"&&scenario!="startup"&&scenario!="static"&&!packet.data().isEmpty()&&(quint8(packet.data()[0])&0xc0)==0x80){
        const auto now=clock.nsecsElapsed();const auto finish=(std::max)(now,networkDue)+qint64(packet.data().size())*8*GST_SECOND/rate;
        const auto due=finish+15*GST_MSECOND;
        if(due-now>50*GST_MSECOND||queued.size()>=64){++lost;continue;}
        networkDue=finish;queued.emplace_back(due,packet.data());
      }else back.writeDatagram(packet.data(),QHostAddress::LocalHost,receiverPort);
    }});
    QObject::connect(&back,&QUdpSocket::readyRead,&server,[&]{while(back.hasPendingDatagrams()){
      auto packet=back.receiveDatagram();if(senderPort){if(scenario=="loss"||scenario=="startup"||scenario=="static")front.writeDatagram(packet.data(),QHostAddress::LocalHost,senderPort);else if(returnQueued.size()<64)returnQueued.emplace_back(clock.nsecsElapsed()+15*GST_MSECOND,packet.data());}
    }});
  }

  const auto caps=QJsonObject{{"sources",QJsonArray{"screen"}},{"receive",true},{"audio",QJsonArray{"off","system"}},{"control",false}};
  if(!broker.attach({senderId,"Sender",generation,QHostAddress::LocalHost,caps})||!broker.attach({receiver,"Receiver",generation,QHostAddress::LocalHost,caps}))qFatal("Broker attach failed");
  std::vector<QLocalSocket *> clients;std::vector<std::unique_ptr<FrameReader>> readers;bool rejected=false;
  QObject::connect(&server,&QLocalServer::newConnection,&server,[&]{
    auto *socket=server.nextPendingConnection();const auto id=clients.empty()?senderId:receiver;clients.push_back(socket);readers.push_back(std::make_unique<FrameReader>());auto *reader=readers.back().get();
    QObject::connect(socket,&QLocalSocket::readyRead,&server,[&,socket,id,reader]{
      if(!reader->feed(socket->readAll(),[&](const auto &frame){
        auto forwarded=frame;
        if(network&&frame["type"]=="IceCandidate"){
          auto data=frame["data"].toObject();auto tokens=data["candidate"].toString().split(' ');
          (id==senderId?senderPort:receiverPort)=tokens[5].toUShort();tokens[5]=id==senderId?"24831":"24830";
          data["candidate"]=tokens.join(' ');forwarded["data"]=data;
        }
        if(!broker.dispatch(id,generation,forwarded,clock.elapsed())){qWarning()<<"Broker rejected"<<frame["type"];rejected=true;}
      }))qFatal("Malformed local test signaling");
    });
  });
  QObject::connect(&broker,&SessionBroker::deliver,&server,[&](const auto &id,const auto &frame){
    if(frame["type"]=="Error")qWarning()<<"Broker error"<<frame;
    const int index=id==senderId?0:1;if(clients.size()>size_t(index))clients[index]->write(FrameReader::encode(frame));
  });
  const QString proof=qEnvironmentVariable("STREAMING_MEDIA_PROOF");if(!proof.isEmpty())QDir().mkpath(proof);
  const auto launch=[&](QProcess &process,const QString &role){
    auto env=QProcessEnvironment::systemEnvironment();
    if(scenario=="static")env.insert("STREAMING_MEDIA_STATIC","1");
    if(scenario=="seek")env.insert("STREAMING_MEDIA_SEEK","1");
    if(scenario=="bandwidth"||scenario=="overload")env.insert("STREAMING_MEDIA_BANDWIDTH","1");
    if(scenario=="overload")env.insert("STREAMING_MEDIA_OVERLOAD","1");
    if(scenario=="slow")env.insert("STREAMING_MEDIA_SLOW","1");
    if(scenario=="file")env.insert("STREAMING_MEDIA_FILE",files.filePath("owned.webm"));
    process.setProcessEnvironment(env);process.start(QCoreApplication::applicationFilePath(),{"--endpoint",endpoint,role,audio?"on":"off",proof});if(!process.waitForStarted(3000))qFatal("Endpoint process failed");};
  launch(sending,"sender");if(!QTest::qWaitFor([&]{return clients.size()==1;},3000))qFatal("Sender IPC failed");
  launch(receiving,"receiver");if(!QTest::qWaitFor([&]{return clients.size()==2;},3000))qFatal("Receiver IPC failed");
  if(!broker.dispatch(senderId,generation,message("Offer",{{"session",session},{"to",receiver},{"source",source},{"kind","screen"},{"audio",audio?"system":"off"},{"preset","low"},{"interactive",false}}),clock.elapsed())||!broker.dispatch(receiver,generation,message("Accept",{{"session",session},{"source",source}}),clock.elapsed()))qFatal("Consent failed");
  const bool ended=QTest::qWaitFor([&]{return sending.state()==QProcess::NotRunning&&receiving.state()==QProcess::NotRunning;},12000);
  if(!ended){sending.kill();receiving.kill();sending.waitForFinished();receiving.waitForFinished();}
  qInfo()<<"Network emulator encrypted media packets/lost"<<mediaPackets<<lost<<"forwardedBytes"<<forwardedBytes<<"limitBps"<<(scenario=="loss"?0:rate);
  qInfo()<<"Exit codes"<<sending.exitCode()<<receiving.exitCode();
  const auto senderLog=sending.readAllStandardError(),receiverLog=receiving.readAllStandardError();
  qInfo().noquote()<<senderLog<<receiverLog;
  const bool stoppedOverload=scenario=="overload"&&receiving.exitCode()==2&&receiverLog.contains("First decoded video frame timed out")&&senderLog.contains("Peer Stop propagated active false")&&(sending.exitCode()==0||sending.exitCode()==2);
  QVERIFY2(ended&&!rejected&&senderLog.contains("Media resources released active false")&&receiverLog.contains("Media resources released active false")&&((sending.exitCode()==0&&receiving.exitCode()==0)||stoppedOverload)&&(!network||lost>0),"Real WebRTC endpoints must exchange changing VP8 pixels and selected Opus PCM through broker-validated signaling");
 }
};
int main(int argc,char **argv){QCoreApplication app(argc,argv);if(app.arguments().contains("--endpoint"))return endpoint(app);StreamingMediaTests tests;return QTest::qExec(&tests,argc,argv);}
#include "StreamingMediaTests.moc"
