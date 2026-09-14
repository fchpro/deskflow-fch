// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/streaming/StreamViewer.h"
#include "gui/streaming/SenderWorker.h"
#include "gui/streaming/StreamDialog.h"
#include "streaming/SessionBroker.h"
#include "streaming/GstCapturePipeline.h"
#include "streaming/AudioClipping.h"
#include "streaming/PacketMetadata.h"
#include "streaming/VideoKeyframe.h"
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/video.h>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QLabel>
#include <QLocalServer>
#include <QProcess>
#include <QPushButton>
#include <QSignalSpy>
#include <QSlider>
#include <QScreen>
#include <QWindow>
#include <QVBoxLayout>
#include <QPainter>
#include <cmath>
#include <QTemporaryDir>
#include <QTest>
using namespace deskflow::streaming;
namespace deskflow::gui {
namespace {
const QString senderId(64, 'a'), receiver(64, 'b'), generation(32, 'c'), session(32, 'd'), source(32, 'e');
QJsonObject caps() { return {{"sources", QJsonArray{"file"}}, {"receive", true}, {"audio", QJsonArray{"off", "file"}}, {"control", false}}; }
void save(QWidget &widget, const QString &name) {
  const auto path = qEnvironmentVariable("STREAMING_VIEWER_PROOF");
  if (!path.isEmpty() && !widget.grab().save(path + "/" + name + ".png")) qFatal("Screenshot save failed");
}
int senderEndpoint(QCoreApplication &app) {
  SessionClient client; SenderController controller(&client); bool started = false, active = false;
  QObject::connect(&controller, &SenderController::inventoryChanged, &app, [&] {
    if (!started && controller.inventory()["peers"].toArray().size() == 2) {
      started = true;
      controller.start({{"to", receiver}, {"kind", "file"}, {"path", app.arguments()[3]}, {"audio", app.arguments().value(4,"off")},
        {"preset", "low"}, {"interactive", false}, {"playback", true}});
    }
  });
  QObject::connect(&controller, &SenderController::statusChanged, &app, [&] {
    if (controller.active()) active = true;
    else if (active) { qInfo() << "Sender ended:" << controller.status(); app.exit(0); }
  });
  client.start(app.arguments()[2]); QTimer::singleShot(20000, &app, [&] { app.exit(9); }); return app.exec();
}
#ifdef Q_OS_WIN
int captureEndpoint(QCoreApplication &app) {
  QLocalSocket socket; AudioInput capture; QTimer timer; timer.setInterval(20);
  const auto pid=app.arguments()[3].toUInt();
  socket.connectToServer(app.arguments()[2]); if (!socket.waitForConnected(3000)) return 2;
  const auto birth=audioProcessBirth(pid);
  if (!capture.start({"application",{},pid,birth},session,source)) { qWarning() << capture.error(); return 3; }
  socket.write(FrameReader::encode(message("Identity",{{"ready",true},{"scope","application"},{"pid",qint64(pid)},
    {"processBirth",QString::number(birth)},{"captureProcess",QCoreApplication::applicationPid()}})));
  QObject::connect(&socket,&QLocalSocket::disconnected,&app,&QCoreApplication::quit);
  QObject::connect(&timer,&QTimer::timeout,&app,[&] {
    double energy=0; int count=0; qint64 firstHost=-1,lastHost=0,duration=0;
    while (auto block=capture.takeAudio()) {
      if(firstHost<0)firstHost=block->captureTimeNs;lastHost=block->captureTimeNs+block->durationNs;duration+=block->durationNs;
      const auto *samples=reinterpret_cast<const float *>(block->samples.constData());
      for (int i=0;i<block->samples.size()/4;++i) { energy+=samples[i]*samples[i]; ++count; }
    }
    if (count) socket.write(FrameReader::encode(message("Identity",{{"rms",std::sqrt(energy/count)},{"firstHostNs",firstHost},{"lastHostNs",lastHost},{"durationNs",duration}})));
    if (!capture.error().isEmpty()) { qWarning()<<capture.error(); app.exit(4); }
  }); timer.start(); QTimer::singleShot(20000,&app,[&] { app.exit(5); }); return app.exec();
}
#endif
}
class StreamingViewerTests : public QObject {
  Q_OBJECT
  QTemporaryDir files;
private Q_SLOTS:
  void initTestCase() {
    QProcess generate;
    generate.start("ffmpeg", {"-hide_banner", "-loglevel", "error", "-nostdin", "-y", "-f", "lavfi", "-i",
      "color=c=blue:s=320x180:r=30:d=3", "-f", "lavfi", "-i", "color=c=orange:s=320x180:r=30:d=9",
      "-filter_complex", "[0:v][1:v]concat=n=2:v=1:a=0,drawbox=x=2:y=2:w=316:h=176:c=white:t=2,drawbox=x=40:y=40:w=40:h=40:c=white:t=3,drawbox=x=270:y=15:w=30:h=30:c=lime:t=fill", "-c:v", "libvpx", "-deadline", "realtime", files.filePath("owned.webm")});
    if (!generate.waitForFinished(10000) || generate.exitCode()) qFatal("Owned media fixture generation failed");
    generate.start("ffmpeg",{"-hide_banner","-loglevel","error","-nostdin","-y","-i",files.filePath("owned.webm"),
      "-f","lavfi","-i","sine=frequency=440:sample_rate=48000:duration=12","-af","volume=0.16","-c:v","copy","-c:a","libopus",files.filePath("owned-audio.webm")});
    if (!generate.waitForFinished(10000) || generate.exitCode()) qFatal("Owned audio fixture generation failed");
    const auto proof=qEnvironmentVariable("STREAMING_VIEWER_PROOF");
    if (!proof.isEmpty()) {
      FileSource reference;
      if (!reference.open(files.filePath("owned.webm"),session,source,false) ||
          !QTest::qWaitFor([&] { return reference.state()==FilePlaybackState::Paused; },3000)) qFatal("Reference decode failed");
      auto frame=reference.takePreroll();
      if (!frame || !frame->pixels.save(proof+"/source-reference-blue.png")) qFatal("Reference save failed");
      if (!reference.command(session,source,"seek",5000*GST_MSECOND) ||
          !QTest::qWaitFor([&] { return reference.state()==FilePlaybackState::Paused; },3000)) qFatal("Reference seek failed");
      frame=reference.takePreroll();
      if (!frame || !frame->pixels.save(proof+"/source-reference-orange.png")) qFatal("Reference save failed");
    }
  }
  void brokerPermission_data() {
    QTest::addColumn<QString>("aspect");
    for (const char *name : {"allowed", "denied", "sender", "epoch", "negative", "fractional", "outside", "atEnd", "unseekable", "action", "rate", "fields", "scope", "staleSource", "staleGeneration"})
      QTest::newRow(name) << QString(name);
  }
  void brokerPermission() {
    QFETCH(QString, aspect); SessionBroker broker;
    if (!broker.attach({senderId,"Owned sender",generation,QHostAddress::LocalHost,caps()}) ||
        !broker.attach({receiver,"Owned receiver",generation,QHostAddress::LocalHost,caps()})) qFatal("Broker attach failed");
    const QJsonObject offer{{"session",session},{"source",source},{"to",receiver},{"kind","file"},{"audio","off"},
      {"preset","low"},{"interactive",false},{"playback",aspect != "denied"}};
    if (!broker.dispatch(senderId,generation,message("Offer",offer),0)) qFatal("Offer setup failed");
    auto &sessions = const_cast<QHash<QString, Session>&>(broker.m_sessions);
    auto &current = sessions[session]; current.state = "streaming";
    if (aspect == "scope") current.offer["kind"] = "window";
    current.playback = {{"epoch", 2}, {"durationMs", 12000}, {"seekable", aspect != "unseekable"}};
    QJsonObject command{{"session",session},{"source",source},{"action","seek"},{"positionMs",5000},{"epoch",2}};
    if (aspect == "epoch") command["epoch"] = 1;
    if (aspect == "negative") command["positionMs"] = -1;
    if (aspect == "fractional") command["positionMs"] = 1.5;
    if (aspect == "outside") command["positionMs"] = 12001;
    if (aspect == "atEnd") command["positionMs"] = 12000;
    if (aspect == "action") { command["action"] = "inject"; command["positionMs"]=0; }
    if (aspect == "rate") current.lastPlaybackCommand = 950;
    if (aspect == "fields") command["unknown"] = true;
    if (aspect == "staleSource") command["source"] = QString(32,'f');
    QCOMPARE(broker.dispatch(aspect == "sender" ? senderId : receiver, aspect == "staleGeneration" ? QString(32,'f') : generation,
      message("PlaybackCommand",command),1000), aspect == "allowed");
  }
  void brokerTimeline_data() {
    QTest::addColumn<QString>("aspect");
    for(const char *name:{"allowed","role","kind","state","fields","duration","position","epoch","paused","seekable","stale"}) QTest::newRow(name)<<QString(name);
  }
  void brokerTimeline() {
    QFETCH(QString,aspect); SessionBroker broker;
    if(!broker.attach({senderId,"Sender",generation,QHostAddress::LocalHost,caps()}) ||
       !broker.attach({receiver,"Receiver",generation,QHostAddress::LocalHost,caps()}) ||
       !broker.dispatch(senderId,generation,message("Offer",{{"session",session},{"source",source},{"to",receiver},
         {"kind","file"},{"audio","off"},{"preset","low"},{"interactive",false}}),0))qFatal("Timeline broker setup failed");
    auto &current=broker.m_sessions[session]; current.state=aspect=="state" ? "awaitingConsent" : "streaming";
    if(aspect=="kind")current.offer["kind"]="window";
    current.playback={{"epoch",1}};
    if(aspect=="epoch") current.playback={};
    QJsonObject state{{"session",session},{"source",source},{"positionMs",5000},{"durationMs",12000},{"epoch",1},{"paused",true},{"seekable",true}};
    if(aspect=="fields")state["extra"]=true;
    if(aspect=="duration")state["durationMs"]=604800001;
    if(aspect=="position")state["positionMs"]=12001;
    if(aspect=="epoch")state["epoch"]=1.5;
    if(aspect=="paused")state["paused"]=1;
    if(aspect=="seekable")state["seekable"]=1;
    if(aspect=="stale")state["epoch"]=0;
    QCOMPARE(broker.dispatch(aspect=="role" ? receiver : senderId,generation,message("PlaybackState",state),1000),aspect=="allowed");
  }
  void surfaceAspect_data() {
    QTest::addColumn<QSize>("size"); QTest::addColumn<QRect>("expected");
    QTest::newRow("wide") << QSize(900,300) << QRect(183,0,533,300);
    QTest::newRow("tall") << QSize(400,600) << QRect(0,187,400,225);
  }
  void surfaceAspect() {
    QFETCH(QSize,size); VideoSurface surface; surface.resize(size); QImage frame(320,180,QImage::Format_ARGB32); frame.fill(Qt::blue); surface.setFrame(frame);
    QFETCH(QRect,expected); QCOMPARE(surface.videoRect(), expected);
  }
  void staleViewerPixels() {
    SessionClient client; SenderController controller(&client); StreamLauncher launcher(&controller);
    Q_EMIT controller.incoming({{"session",session},{"source",source},{"kind","file"},{"audio","off"}});
    auto *old = launcher.findChild<StreamViewer *>();
    Q_EMIT controller.viewerFrame(QImage{});
    Q_EMIT controller.incoming({{"session",QString(32,'f')},{"source",source},{"kind","file"},{"audio","off"}});
    QImage image(320,180,QImage::Format_ARGB32); image.fill(Qt::blue); Q_EMIT controller.viewerFrame(image);
    save(*old,"stale-viewer");
    QVERIFY(old->findChild<VideoSurface *>()->videoRect().isEmpty());
  }
  void unavailableOutputRetry() {
    SessionClient client; SenderController controller(&client); StreamLauncher launcher(&controller);
    QMetaObject::invokeMethod(controller.m_worker,[&] {
      controller.m_worker->connection(true);
      controller.m_worker->receive(message("Identity",{{"id",receiver},{"address","127.0.0.1"}}));
      controller.m_worker->receive(message("Offer",{{"session",session},{"source",source},{"from",senderId},{"to",receiver},
        {"kind","file"},{"audio","file"},{"title","Owned test file"},{"preset","low"}}));
    },Qt::BlockingQueuedConnection);
    if(!QTest::qWaitFor([&] {return launcher.findChild<StreamViewer *>();},3000))qFatal("Incoming fixture unavailable");
    auto *viewer=launcher.findChild<StreamViewer *>(); auto *endpoint=viewer->findChild<QComboBox *>("receiverEndpoint");
    endpoint->addItem("Disconnected fixture endpoint","disappeared"); endpoint->setCurrentIndex(endpoint->count()-1);
    viewer->findChild<QPushButton *>("acceptStream")->click();
    QVERIFY(QTest::qWaitFor([&] { return bool(controller.status().contains("Select an available audio output")); },3000));
    save(*viewer,"output-retry");
    QVERIFY(viewer->findChild<QWidget *>("receiverConsent")->isVisible());
  }
  void chosenMuteSurvivesAccept() {
    SenderWorker worker; worker.m_receiving = true; worker.m_session = session; worker.m_source = source;
    worker.m_inventory = {{"address","127.0.0.1"},{"peers",QJsonArray{QJsonObject{{"id",senderId},{"address","127.0.0.1"}}}}};
    worker.m_selection = {{"to",senderId},{"preset","low"},{"audio","off"}};
    worker.volume(0.25,true); worker.begin();
    QCOMPARE(worker.m_muted,true); worker.stop({},false);
  }
  void chosenGainSurvivesAccept() {
    SenderWorker worker; worker.m_receiving=true; worker.m_session=session; worker.m_source=source;
    worker.m_inventory={{"address","127.0.0.1"},{"peers",QJsonArray{QJsonObject{{"id",senderId},{"address","127.0.0.1"}}}}};
    worker.m_selection={{"to",senderId},{"preset","low"},{"audio","off"}};
    worker.volume(0.25,true); worker.begin();
    QCOMPARE(worker.m_gain,0.25); worker.stop({},false);
  }
  void presentation_data() {
    QTest::addColumn<QString>("aspect");
    for (const char *name:{"due","future","outstanding","next","latest","epoch","oldEpoch","pause","resume"}) QTest::newRow(name)<<QString(name);
  }
  void clipping_data() {
    QTest::addColumn<QString>("aspect");
    for (const char *name:{"start","end","duration","pts","epoch","bounds","wireStart","wireEnd","wireMagic"}) QTest::newRow(name)<<QString(name);
  }
  void clipping() {
    QFETCH(QString,aspect); AudioClipping clip{312,24};
    if (aspect.startsWith("wire")) {
      auto bytes=encodeAudioClipping(clip); if (aspect=="wireMagic") bytes[0]=0;
      auto result=decodeAudioClipping(bytes);
      if (aspect=="wireMagic") QVERIFY(!result);
      else QCOMPARE(result ? (aspect=="wireStart" ? result->start : result->end) : 999U,aspect=="wireStart" ? 312U : 24U);
      return;
    }
    AudioBlock block{QByteArray(960*8,'\0'),session,source,2,5000000000,20*GST_MSECOND,0};
    auto *samples=reinterpret_cast<float *>(block.samples.data()); for(int i=0;i<1920;++i)samples[i]=float(i);
    if (aspect=="bounds") { clip.start=960; QVERIFY(!clipDecodedAudio(block,clip)); return; }
    if (!clipDecodedAudio(block,clip)) qFatal("Valid clipping rejected");
    if (aspect=="start") QCOMPARE(reinterpret_cast<const float *>(block.samples.constData())[0],624.0f);
    if (aspect=="end") QCOMPARE(reinterpret_cast<const float *>(block.samples.constData())[block.samples.size()/4-1],1871.0f);
    if (aspect=="duration") QCOMPARE(block.durationNs,qint64(13000000));
    if (aspect=="pts") QCOMPARE(block.mediaTimeNs,qint64(5000000000));
    if (aspect=="epoch") QCOMPARE(block.timelineEpoch,quint64(2));
  }
  void packetDecode_data() {
    QTest::addColumn<QString>("aspect");
    for(const char *name:{"old","new","loss","encodedOld","encodedNew"})QTest::newRow(name)<<QString(name);
  }
  void packetDecode() {
    QFETCH(QString,aspect); QString error;
    if(!GstCapturePipeline::initialize(error))qFatal("Codec runtime unavailable");
    auto release=[](GstElement *graph) {gst_element_set_state(graph,GST_STATE_NULL);gst_object_unref(graph);};
    std::unique_ptr<GstElement,decltype(release)> encoder(gst_parse_launch(
      "appsrc name=raw format=time ! video/x-raw,format=RGBA,width=320,height=180,framerate=30/1 ! videoconvert ! vp8enc deadline=1 keyframe-max-dist=1 ! rtpvp8pay mtu=128 pt=96 ! appsink name=packets sync=false wait-on-eos=false",nullptr),release);
    std::unique_ptr<GstElement,decltype(release)> decoder(gst_parse_launch(
      "appsrc name=wire format=time ! rtpvp8depay wait-for-keyframe=true ! vp8dec ! videoconvert ! video/x-raw,format=BGRA ! appsink name=decoded sync=false wait-on-eos=false",nullptr),release);
    if(!encoder || !decoder)qFatal("Codec fixture graph unavailable");
    auto *raw=GST_APP_SRC(gst_bin_get_by_name(GST_BIN(encoder.get()),"raw"));
    auto *encoded=GST_APP_SINK(gst_bin_get_by_name(GST_BIN(encoder.get()),"packets"));
    auto *wire=GST_APP_SRC(gst_bin_get_by_name(GST_BIN(decoder.get()),"wire"));
    auto *decoded=GST_APP_SINK(gst_bin_get_by_name(GST_BIN(decoder.get()),"decoded"));
    gst_element_set_state(encoder.get(),GST_STATE_PLAYING);
    PacketMetadata metadata;
    for(int i=0;i<2;++i) {
      QImage image(320,180,QImage::Format_RGBA8888);image.fill(i ? QColor(255,165,0) : QColor(Qt::blue));
      {QPainter marks(&image);for(int x=0;x<320;x+=16)for(int y:{8,24,148,164})marks.fillRect(x,y,8,8,Qt::white);}
      auto *buffer=gst_buffer_new_allocate(nullptr,image.sizeInBytes(),nullptr);gst_buffer_fill(buffer,0,image.constBits(),image.sizeInBytes());
      GST_BUFFER_PTS(buffer)=i*GST_SECOND/30;GST_BUFFER_DURATION(buffer)=GST_SECOND/30;
      QByteArray identity(80,'\0');qToBigEndian(quint64(i),identity.data()+24);
      if(!metadata.attach(buffer,identity))qFatal("Raw frame metadata failed");
      if(gst_app_src_push_buffer(raw,buffer)!=GST_FLOW_OK)qFatal("Encoder fixture input failed");
    }
    gst_app_src_end_of_stream(raw); QList<GstBuffer *> packets;GstCaps *caps=nullptr;
    if(!QTest::qWaitFor([&] {
      while(auto *sample=gst_app_sink_try_pull_sample(encoded,0)) {
        if(!caps)caps=gst_caps_ref(gst_sample_get_caps(sample));
        packets.append(gst_buffer_copy_deep(gst_sample_get_buffer(sample)));gst_sample_unref(sample);
      }
      return gst_app_sink_is_eos(encoded);
    },3000)||packets.size()<4)qFatal("Fragmented RTP fixture unavailable");
    gst_app_src_set_caps(wire,caps);gst_caps_unref(caps);gst_element_set_state(decoder.get(),GST_STATE_PLAYING);
    quint32 firstTimestamp=0;int firstPackets=0;
    for(int i=0;i<packets.size();++i) {
      auto *buffer=packets[i];GstRTPBuffer rtp=GST_RTP_BUFFER_INIT;gst_rtp_buffer_map(buffer,GST_MAP_READ,&rtp);
      const auto timestamp=gst_rtp_buffer_get_timestamp(&rtp);gst_rtp_buffer_unmap(&rtp);if(i==0)firstTimestamp=timestamp;
      const quint64 epoch=timestamp==firstTimestamp ? 0 : 1;if(epoch==0)++firstPackets;
      if(aspect=="loss" && i==1){gst_buffer_unref(buffer);continue;}
      GST_BUFFER_PTS(buffer)=123; // Reproduce actual jitter PTS clamping across distinct RTP timestamps.
      QByteArray identity(80,'\0');qToBigEndian(epoch,identity.data()+24);
      if(!aspect.startsWith("encoded") && !metadata.attach(buffer,identity))qFatal("Decoder RTP metadata failed");
      qInfo()<<"Encoded RTP packet"<<i<<"RTP timestamp"<<timestamp<<"local PTS"<<GST_BUFFER_PTS(buffer)<<"epoch"<<epoch;
      if(gst_app_src_push_buffer(wire,buffer)!=GST_FLOW_OK)qFatal("Decoder RTP input failed");
    }
    if(firstPackets<3)qFatal("Missing multi-fragment stimulus");
    gst_app_src_end_of_stream(wire);QList<GstSample *> samples;
    if(!QTest::qWaitFor([&] {while(auto *sample=gst_app_sink_try_pull_sample(decoded,0))samples.append(sample);return gst_app_sink_is_eos(decoded);},3000)||samples.isEmpty())qFatal("No decoded RTP fixture frames");
    const int index=aspect=="new" || aspect=="encodedNew" ? 1 : 0;if(index>=samples.size())qFatal("Required decoded frame unavailable");
    auto *buffer=gst_sample_get_buffer(samples[index]);const auto identity=metadata.read(buffer);
    const auto epoch=identity.size()==80 ? qFromBigEndian<quint64>(identity.constData()+24) : 999;
    GstVideoInfo info;GstMapInfo map;gst_video_info_from_caps(&info,gst_sample_get_caps(samples[index]));gst_buffer_map(buffer,&map,GST_MAP_READ);
    const QImage image=QImage(map.data,info.width,info.height,info.stride[0],QImage::Format_ARGB32).copy();gst_buffer_unmap(buffer,&map);
    const quint64 contentEpoch=image.pixelColor(160,90).red()>200 ? 1 : 0;
    qInfo()<<"Real fragmented VP8 decoded content epoch"<<contentEpoch<<"metadata epoch"<<epoch<<"decoded local PTS"<<GST_BUFFER_PTS(buffer)<<"packets"<<packets.size()<<"first-frame fragments"<<firstPackets<<"dropped middle packet"<<(aspect=="loss");
    const auto proof=qEnvironmentVariable("STREAMING_VIEWER_PROOF");if(!proof.isEmpty()&&!image.save(proof+"/packet-decode-"+aspect+".png"))qFatal("Decoded proof save failed");
    for(auto *sample:samples)gst_sample_unref(sample);
    for(auto *object:{GST_OBJECT(raw),GST_OBJECT(encoded),GST_OBJECT(wire),GST_OBJECT(decoded)})gst_object_unref(object);
    QCOMPARE(epoch,contentEpoch);
  }
  void packetOwnership_data() {
    QTest::addColumn<bool>("oldFrame"); QTest::newRow("old")<<true; QTest::newRow("new")<<false;
  }
  void packetOwnership() {
    QFETCH(bool,oldFrame); FileSource file;
    if(!file.open(files.filePath("owned.webm"),session,source,false) ||
       !QTest::qWaitFor([&]{return file.state()==FilePlaybackState::Paused;},3000))qFatal("Owned decoded frame unavailable");
    const auto old=file.takePreroll();
    if(!old || !file.command(session,source,"seek",5000*GST_MSECOND) ||
       !QTest::qWaitFor([&]{return file.state()==FilePlaybackState::Paused;},3000))qFatal("Owned seek frame unavailable");
    const auto next=file.takePreroll(); if(!next)qFatal("Owned seek preroll unavailable");
    PacketMetadata metadata; auto buffer=[&](const VideoFrame &frame) {
      auto *result=gst_buffer_new_allocate(nullptr,frame.pixels.sizeInBytes(),nullptr);
      gst_buffer_fill(result,0,frame.pixels.constBits(),frame.pixels.sizeInBytes()); GST_BUFFER_PTS(result)=123;
      QByteArray identity(80,'\0'); qToBigEndian(frame.timelineEpoch,identity.data()+24);
      if(!metadata.attach(result,identity))qFatal("Metadata attachment failed"); return result;
    };
    auto *first=buffer(*old); auto *second=buffer(*next);
    auto *retained=gst_buffer_copy_deep(oldFrame ? first : second);
    const auto bytes=metadata.read(retained);
    const auto epoch=bytes.size()==80 ? qFromBigEndian<quint64>(bytes.constData()+24) : 999;
    QWidget view; auto *layout=new QVBoxLayout(&view); layout->addWidget(new QLabel(QString("Decoded frame epoch: %1").arg(epoch)));
    auto *surface=new VideoSurface(&view); surface->setFrame(oldFrame ? old->pixels : next->pixels); layout->addWidget(surface);
    view.resize(640,400); view.show(); save(view,oldFrame ? "buffer-metadata-old" : "buffer-metadata-new");
    qInfo()<<"Decoded pixel/metadata identity"<<"expected epoch"<<(oldFrame ? old->timelineEpoch : next->timelineEpoch)<<"actual epoch"<<epoch
      <<"old/new buffer local PTS"<<GST_BUFFER_PTS(first)<<GST_BUFFER_PTS(second)<<"retained local PTS"<<GST_BUFFER_PTS(retained);
    gst_buffer_unref(retained);gst_buffer_unref(first);gst_buffer_unref(second);
    QCOMPARE(epoch,oldFrame ? old->timelineEpoch : next->timelineEpoch);
  }
  void metadataBinding_data() {
    QTest::addColumn<QString>("aspect");
    for(const char *name:{"first","second","epoch","clipping","unmatched","copy","invalidSize"}) QTest::newRow(name)<<QString(name);
  }
  void metadataBinding() {
    QFETCH(QString,aspect); PacketMetadata metadata;
    QString error; if (!GstCapturePipeline::initialize(error)) qFatal("GStreamer unavailable: %s",qPrintable(error));
    auto *first=gst_buffer_new(); auto *second=gst_buffer_new(); GST_BUFFER_PTS(first)=123; GST_BUFFER_PTS(second)=123;
    QByteArray firstBytes(96,'\0'),secondBytes(96,'\0'); qToBigEndian(quint64(5260*GST_MSECOND),firstBytes.data()+16);
    qToBigEndian(quint64(5280*GST_MSECOND),secondBytes.data()+16);qToBigEndian(quint64(2),secondBytes.data()+24);qToBigEndian(quint32(24),secondBytes.data()+92);
    if(!metadata.attach(first,firstBytes)||!metadata.attach(second,secondBytes))qFatal("Valid metadata refused");
    if(aspect=="unmatched") {gst_buffer_unref(first);first=gst_buffer_new();}
    auto firstResult=metadata.read(first),secondResult=metadata.read(second);
    auto *copy=gst_buffer_copy_deep(second); const auto copyResult=metadata.read(copy); gst_buffer_unref(copy);
    const bool invalidAccepted=aspect=="invalidSize" && metadata.attach(first,QByteArray(1,'x'));
    gst_buffer_unref(first);gst_buffer_unref(second);
    if(aspect=="first") QCOMPARE(firstResult.size()==96 ? qFromBigEndian<quint64>(firstResult.constData()+16) : 0,quint64(5260*GST_MSECOND));
    if(aspect=="second") QCOMPARE(secondResult.size()==96 ? qFromBigEndian<quint64>(secondResult.constData()+16) : 0,quint64(5280*GST_MSECOND));
    if(aspect=="epoch") QCOMPARE(secondResult.size()==96 ? qFromBigEndian<quint64>(secondResult.constData()+24) : 0,quint64(2));
    if(aspect=="clipping") QCOMPARE(secondResult.size()==96 ? qFromBigEndian<quint32>(secondResult.constData()+92) : 0,quint32(24));
    if(aspect=="unmatched") QVERIFY(firstResult.isEmpty());
    if(aspect=="copy") QCOMPARE(copyResult,secondBytes);
    if(aspect=="invalidSize") QVERIFY(!invalidAccepted);
  }
  void epochAudioReadiness() {
    QString error; if (!GstCapturePipeline::initialize(error)) qFatal("GStreamer unavailable");
    SenderWorker worker; worker.m_receiving=true; worker.m_session=session; worker.m_source=source;
    worker.m_paused=true; worker.m_audioEpochPending=true; worker.m_output=std::make_unique<AudioOutput>();
    if (!worker.m_output->startSink(gst_element_factory_make("fakesink",nullptr),session,source,1,5000*GST_MSECOND) ||
        !worker.m_output->pause(true)) qFatal("Paused output unavailable");
    worker.receive(message("PlaybackState",{{"session",session},{"source",source},{"paused",false},{"epoch",1}}));
    AudioBlock block{QByteArray(960*8,'\0'),session,source,1,5000*GST_MSECOND,20*GST_MSECOND,0};
    QVERIFY(!worker.m_output->push(block));
  }
  void permanentAudioRefusal() {
    QString error; if (!GstCapturePipeline::initialize(error)) qFatal("GStreamer unavailable");
    SenderWorker worker; worker.m_session=session; worker.m_source=source; worker.m_receiving=true; worker.m_clockSet=true;
    worker.m_media=std::make_unique<MediaTransport>(); worker.m_output=std::make_unique<AudioOutput>();
    if (!worker.m_output->startSink(gst_element_factory_make("fakesink",nullptr),session,source,0)) qFatal("Output fixture unavailable");
    AudioBlock block{QByteArray(960*8,'\0'),session,source,0,0,20*GST_MSECOND,0};
    if (!worker.m_output->push(block)) qFatal("Initial output failed");
    block.mediaTimeNs=10*GST_MSECOND; worker.m_pendingAudio.push_back(block); worker.receivePoll();
    QVERIFY(worker.m_session.isEmpty());
  }
  void presentation() {
    QFETCH(QString,aspect);
    if (aspect=="pause" || aspect=="resume") {
      ReceiverClock clock; clock.reset(500,100); clock.pause(true,120);
      if (aspect=="resume") clock.pause(false,1000);
      QCOMPARE(clock.position(1010),qint64(aspect=="pause" ? 520 : 530)); return;
    }
    ReceiverVideoQueue queue;
    VideoFrame frame; frame.timelineEpoch=0; frame.mediaTimeNs=100*GST_MSECOND; frame.sequence=1; queue.push(frame);
    if (aspect=="next" || aspect=="latest") {
      frame.sequence=2; frame.mediaTimeNs=120*GST_MSECOND; queue.push(frame);
      frame.sequence=3; frame.mediaTimeNs=130*GST_MSECOND; queue.push(frame);
    }
    if (aspect=="epoch" || aspect=="oldEpoch") {
      frame.timelineEpoch=1; frame.sequence=4; frame.mediaTimeNs=50*GST_MSECOND; queue.push(frame);
      if (aspect=="oldEpoch") { frame.timelineEpoch=0; frame.sequence=5; queue.push(frame); }
    }
    auto result=queue.take((aspect=="future" ? 90 : 100)*GST_MSECOND,aspect=="outstanding");
    if (aspect=="latest") result=queue.take(130*GST_MSECOND,false);
    const quint64 expected=aspect=="future" || aspect=="outstanding" ? 0 : aspect=="latest" ? 3 : aspect=="epoch" || aspect=="oldEpoch" ? 4 : 1;
    QCOMPARE(result ? result->sequence : 0,expected);
  }
  void fullscreenLabelFits() {
    SessionClient client; SenderController controller(&client); StreamViewer viewer(&controller,{{"audio","off"}});
    viewer.show(); viewer.toggleFullscreen(); auto *button=viewer.findChild<QPushButton *>("receiverFullscreen");
    save(viewer,"fullscreen-label");
    QVERIFY(button->width() >= button->minimumSizeHint().width());
  }
  void staleViewerClose() {
    SessionClient client; SenderController controller(&client); StreamLauncher launcher(&controller);
    QMetaObject::invokeMethod(controller.m_worker,[] {},Qt::BlockingQueuedConnection); QCoreApplication::processEvents();
    Q_EMIT controller.incoming({{"session",session},{"source",source},{"kind","file"},{"audio","off"}});
    auto *old = launcher.findChild<StreamViewer *>();
    Q_EMIT controller.incoming({{"session",QString(32,'f')},{"source",source},{"kind","file"},{"audio","off"}});
    QSignalSpy changes(&controller,&SenderController::statusChanged); old->close();
    QMetaObject::invokeMethod(controller.m_worker,[] {},Qt::BlockingQueuedConnection); QCoreApplication::processEvents();
    QCOMPARE(changes.count(),0);
  }
  void silentFileSeek() {
    FileSource file;
    if (!file.open(files.filePath("owned.webm"),session,source,false) ||
        !QTest::qWaitFor([&] { return file.state()==FilePlaybackState::Paused; },3000)) qFatal("Silent file setup failed");
    QVERIFY(file.command(session,source,"seek",5000*GST_MSECOND));
  }
  void outputGain_data() {
    QTest::addColumn<bool>("muted"); QTest::newRow("gain") << false; QTest::newRow("mute") << true;
  }
  void outputGain() {
    QFETCH(bool,muted); SenderWorker worker; worker.m_receiving = true;
    QString error; if (!GstCapturePipeline::initialize(error)) qFatal("GStreamer unavailable");
    worker.m_output = std::make_unique<AudioOutput>(); auto *sink = gst_element_factory_make("appsink",nullptr);
    if (!sink) qFatal("PCM inspection sink unavailable"); gst_object_ref(sink);
    if (!worker.m_output->startSink(sink,session,source,0)) qFatal("PCM output setup failed");
    worker.volume(0.25,muted);
    AudioBlock block{QByteArray(960*8,'\0'),session,source,0,0,20*GST_MSECOND,0};
    auto *samples = reinterpret_cast<float *>(block.samples.data()); for (int i=0;i<1920;++i) samples[i]=0.5f;
    if (!worker.m_output->push(block)) qFatal("PCM output submission failed");
    GstSample *sample=nullptr;
    if (!QTest::qWaitFor([&] { sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),0); return sample; },3000)) qFatal("No rendered PCM");
    GstMapInfo mapped; gst_buffer_map(gst_sample_get_buffer(sample),&mapped,GST_MAP_READ);
    const float actual = reinterpret_cast<const float *>(mapped.data)[0];
    gst_buffer_unmap(gst_sample_get_buffer(sample),&mapped); gst_sample_unref(sample); gst_object_unref(sink);
    qInfo() << "Worker receiver gain 0.25 mute" << muted << "actual output PCM" << actual;
    QCOMPARE(actual,muted ? 0.0f : 0.125f);
  }
  void receiverGuards_data() {
    QTest::addColumn<QString>("aspect");
    for (const char *name : {"capability", "busy", "consent", "endpoint", "permission", "error", "decline"}) QTest::newRow(name) << QString(name);
  }
  void receiverGuards() {
    QFETCH(QString,aspect); SenderWorker worker; QSignalSpy out(&worker,&SenderWorker::outgoing); worker.connection(true);
    worker.receive(message("Identity", {{"id",receiver},{"address","127.0.0.1"}}));
    worker.receive(message("Roster",{{"peers",QJsonArray{QJsonObject{{"id",senderId},{"address","127.0.0.1"}}}}}));
    if (aspect == "capability") { QCOMPARE(out.last()[0].toJsonObject()["data"].toObject()["receive"].toBool(),true); return; }
    worker.receive(message("Offer", {{"session",session},{"source",source},{"from",senderId},{"to",receiver},{"kind","file"},
      {"audio",aspect == "endpoint" ? "file" : "off"},{"preset","low"},{"playback",false}}));
    if (aspect == "busy") { QCOMPARE(worker.m_inventory["busy"].toBool(),true); return; }
    if (aspect == "consent") { QVERIFY(!worker.m_media); return; }
    out.clear();
    if (aspect == "endpoint") { worker.accept("nonexistent-endpoint"); QCOMPARE(out.count(),0); }
    if (aspect == "permission") { worker.m_playback = {{"epoch",0}}; worker.playbackCommand("pause"); QCOMPARE(out.count(),0); }
    if (aspect == "error") { worker.receive(message("Error",{{"reason","invalidPlaybackCommand"}})); QVERIFY(!worker.m_session.isEmpty()); }
    if (aspect == "decline") { worker.stop(); QCOMPARE(out.first()[0].toJsonObject()["type"].toString(),QString("Decline")); }
  }
  void realReceiverWorkflow_data() {
    QTest::addColumn<QString>("ending");
    QTest::newRow("close") << QString("close"); QTest::newRow("stop") << QString("stop"); QTest::newRow("disconnect") << QString("disconnect");
  }
  void realReceiverWorkflow() {
    QFETCH(QString,ending);
    runReceiverWorkflow(ending,false);
  }
#ifdef Q_OS_WIN
  void nativeReceiverAudio_data() {
    QTest::addColumn<bool>("initialMuted"); QTest::newRow("gain")<<false; QTest::newRow("mute")<<true;
  }
  void nativeReceiverAudio() { QFETCH(bool,initialMuted); runReceiverWorkflow("close",true,initialMuted); }
#endif
private:
  void runReceiverWorkflow(const QString &ending,bool nativeAudio,bool initialMuted=false) {
    PrivateIpcServer senderIpc, receiverIpc; SessionClient client; SessionBroker broker;
    const auto senderEndpoint = randomId(), receiverEndpoint = randomId();
    if (!senderIpc.listen(senderEndpoint) || !receiverIpc.listen(receiverEndpoint)) qFatal("Private endpoints unavailable");
    QElapsedTimer clock; clock.start(); QTimer expiry; expiry.setInterval(100);
    connect(&expiry,&QTimer::timeout,&broker,[&] { broker.expire(clock.elapsed()); }); expiry.start();
    connect(&broker,&SessionBroker::deliver,this,[&](const QString &peer,const QJsonObject &frame) {
      (peer == senderId ? senderIpc : receiverIpc).send(frame);
    });
    auto attach = [&](PrivateIpcServer &ipc,const QString &id) {
      connect(&ipc,&PrivateIpcServer::attachedChanged,this,[&,id](bool attached) {
        if (!attached) { broker.detach(id); return; }
        auto &endpoint = id == senderId ? senderIpc : receiverIpc;
        endpoint.send(message("Identity",{{"id",id},{"address","127.0.0.1"}}));
        if (!broker.attach({id,id == senderId ? "Owned sender" : "Owned receiver",generation,QHostAddress::LocalHost,caps()})) qFatal("Broker attach failed");
      });
      connect(&ipc,&PrivateIpcServer::received,this,[&,id](const QJsonObject &frame) {
        if (!broker.dispatch(id,generation,frame,clock.elapsed())) qWarning() << "Rejected" << frame;
      });
    }; attach(senderIpc,senderId); attach(receiverIpc,receiver);
    SenderController controller(&client); StreamLauncher launcher(&controller); QSignalSpy pixels(&controller,&SenderController::viewerFrame);
    connect(&controller,&SenderController::statusChanged,this,[&] { qInfo() << "Receiver status" << controller.status(); });
    connect(&controller,&SenderController::playbackChanged,this,[&] { qInfo() << "Receiver timeline" << controller.playbackState(); });
    client.start(receiverEndpoint); QProcess child; child.start(QCoreApplication::applicationFilePath(),{"--sender",senderEndpoint,
      files.filePath(nativeAudio ? "owned-audio.webm" : "owned.webm"),nativeAudio ? "file" : "off"});
    connect(&child,&QProcess::readyReadStandardOutput,this,[&] { qInfo().noquote() << child.readAllStandardOutput(); });
    connect(&child,&QProcess::readyReadStandardError,this,[&] { qInfo().noquote() << child.readAllStandardError(); });
    auto viewer = [&] { return launcher.findChild<StreamViewer *>(); };
    auto frameCount = [&] { int count = 0; for (const auto &entry : pixels) if (!entry[0].value<QImage>().isNull()) ++count; return count; };
    QVERIFY(QTest::qWaitFor([&] { return bool(viewer() && viewer()->isVisible()); },3000));
    QCOMPARE(frameCount(),0);
    if (nativeAudio) {
      auto *box=viewer()->findChild<QComboBox *>("receiverEndpoint"); int selected=-1;
      for (int i=0;i<box->count();++i) if (box->itemText(i).contains("Steam Streaming Speakers")) selected=i;
      if (selected<0) qFatal("Required explicitly named Steam virtual output is unavailable; no other endpoint is permitted");
      box->setCurrentIndex(selected); qInfo()<<"Explicit receiver native output"<<box->currentText()<<box->currentData();
      viewer()->findChild<QSlider *>("receiverVolume")->setValue(25);
      viewer()->findChild<QCheckBox *>("receiverMute")->setChecked(initialMuted);
    }
    save(*viewer(),"incoming-consent"); viewer()->findChild<QPushButton *>("acceptStream")->click();
    const bool framesReady=QTest::qWaitFor([&] {return frameCount()>=4;},10000);
    if(!framesReady) save(*viewer(),"receiver-presentation-before");
    QVERIFY(framesReady);
    if(nativeAudio) { qint64 margin=-1; QMetaObject::invokeMethod(controller.m_worker,[&]{if(controller.m_worker->m_output)margin=controller.m_worker->m_output->playoutMarginNs();},Qt::BlockingQueuedConnection); QCOMPARE(margin,receiverPlayoutMarginNs); }
    QVERIFY(QTest::qWaitFor([&] { return bool(controller.playbackState()["positionMs"].toInt() > 0); },3000));
    save(*viewer(),"received-windowed");
    QLocalServer captureServer; QProcess capture; QLocalSocket *captureSocket=nullptr; FrameReader captureReader;
    // One signed 16-bit LSB bounds observed virtual-endpoint dither after mute.
    QList<double> rms; bool captureReady=false; int captureReports=0; qint64 silenceAfter=0,silentPcm=0,silentEnd=0;
    auto audible = [&](double low,double high) {
      if (rms.size()<4) return false;
      for (const auto value:rms.last(4)) if (value<low || value>high) return false;
      return true;
    };
    if (nativeAudio) {
      const auto captureName=randomId(); if (!captureServer.listen(captureName)) qFatal("Capture endpoint unavailable");
      connect(&captureServer,&QLocalServer::newConnection,this,[&] {
        captureSocket=captureServer.nextPendingConnection();
        connect(captureSocket,&QLocalSocket::readyRead,this,[&] {
          captureReader.feed(captureSocket->readAll(),[&](const QJsonObject &frame) {
            const auto data=frame["data"].toObject();
            if (data["ready"].toBool()) { captureReady=true; qInfo()<<"Started native process-loopback API"<<data<<"receiver test PID"<<QCoreApplication::applicationPid(); }
            if (data.contains("rms")) { const auto first=data["firstHostNs"].toInteger(),last=data["lastHostNs"].toInteger(),duration=data["durationNs"].toInteger();
              if(silenceAfter && first>=silenceAfter) {
                const bool quiet=std::isfinite(data["rms"].toDouble()) && data["rms"].toDouble()<=1.0/32768;
                if(!quiet || (silentEnd && std::abs(first-silentEnd)>GST_MSECOND))silentPcm=0;
                if(quiet) silentPcm+=duration; silentEnd=last;
                qInfo()<<"Post-frame owned PCM interval"<<first<<last<<"duration"<<duration<<"after"<<silenceAfter<<"RMS"<<data["rms"].toDouble()<<"quiet duration"<<silentPcm;
              }
              rms.append(data["rms"].toDouble()); if (rms.size()>10) rms.removeFirst();
              if (++captureReports%25==0) qInfo()<<"Captured owned receiver PCM RMS"<<rms; }
          });
        });
      });
      capture.start(QCoreApplication::applicationFilePath(),{"--capture",captureName,QString::number(QCoreApplication::applicationPid())});
      if (!QTest::qWaitFor([&] { return captureReady || capture.state()==QProcess::NotRunning; },3000) || !captureReady)
        qFatal("Owned receiver process loopback failed: %s",capture.readAllStandardError().constData());
      if(initialMuted) {
        QVERIFY(QTest::qWaitFor([&] { return bool(audible(0,1.0/32768)); },3000)); qInfo()<<"Native receiver first output preserves pre-accept mute RMS"<<rms;
        rms.clear(); viewer()->findChild<QCheckBox *>("receiverMute")->setChecked(false);
      } else {
        QVERIFY(QTest::qWaitFor([&] { return bool(audible(0.001,0.006)); },3000)); qInfo()<<"Native receiver first output preserves pre-accept gain RMS"<<rms;
      }
      rms.clear(); viewer()->findChild<QSlider *>("receiverVolume")->setValue(100);
      QVERIFY(QTest::qWaitFor([&] { return bool(audible(0.005,0.03)); },3000)); qInfo()<<"Native receiver baseline RMS"<<rms;
      rms.clear(); viewer()->findChild<QSlider *>("receiverVolume")->setValue(25);
      QVERIFY(QTest::qWaitFor([&] { return bool(audible(0.001,0.006)); },3000)); qInfo()<<"Native receiver 25-percent RMS"<<rms;
      rms.clear(); viewer()->findChild<QCheckBox *>("receiverMute")->setChecked(true);
      QVERIFY(QTest::qWaitFor([&] { return bool(audible(0,1.0/32768)); },3000)); qInfo()<<"Native receiver muted RMS"<<rms;
      save(*viewer(),"received-native-muted"); rms.clear(); viewer()->findChild<QCheckBox *>("receiverMute")->setChecked(false);
      QVERIFY(QTest::qWaitFor([&] { return bool(audible(0.001,0.006)); },3000)); qInfo()<<"Native receiver unmuted RMS"<<rms;
    }
    viewer()->findChild<QPushButton *>("filePlayPause")->click();
    QVERIFY(QTest::qWaitFor([&] { return bool(controller.playbackState()["paused"].toBool()); },3000));
    if (nativeAudio) { rms.clear(); QVERIFY(QTest::qWaitFor([&] { return bool(audible(0,1.0/32768)); },3000)); qInfo()<<"Native paused RMS"<<rms; }
    // Wait for broker's explicit minimum command interval, using its command timestamp signal.
    if (!QTest::qWaitFor([&] { return !broker.sessions().isEmpty() && clock.elapsed()-broker.sessions().first().lastPlaybackCommand >= 100; },1000)) qFatal("Playback command rate did not reopen");
    viewer()->findChild<QSlider *>("fileSeek")->setValue(5000);
    QVERIFY(QTest::qWaitFor([&] { return bool(controller.playbackState()["epoch"].toInteger() == 1); },3000));
    const bool seekPixels=QTest::qWaitFor([&] {return pixels.last()[0].value<QImage>().pixelColor(160,90).red()>200;},3000);
    if(!seekPixels)save(*viewer(),"receiver-paused-seek-before");
    QVERIFY(seekPixels);
    QVERIFY(controller.playbackState()["paused"].toBool());
    if (nativeAudio) { rms.clear(); silenceAfter=audioHostTimeNs(); silentPcm=0; silentEnd=0; QVERIFY(QTest::qWaitFor([&] { return silentPcm>=300*GST_MSECOND; },3000)); qInfo()<<"Native paused seek RMS"<<rms<<"captured quiet ns"<<silentPcm; silenceAfter=0; }
    save(*viewer(),"received-paused-seek");
    viewer()->toggleFullscreen();
    QVERIFY(viewer()->isFullScreen());
    if (!QTest::qWaitFor([&] { return viewer()->windowHandle()->visibility()==QWindow::FullScreen && viewer()->geometry()==viewer()->screen()->geometry(); },3000)) qFatal("Fullscreen geometry was not applied");
    save(*viewer(),"received-fullscreen");
    QTest::keyClick(viewer(),Qt::Key_Escape);
    QVERIFY(!viewer()->isFullScreen());
    if (!QTest::qWaitFor([&] { return clock.elapsed()-broker.sessions().first().lastPlaybackCommand >= 100; },1000)) qFatal("Playback rate did not reopen");
    viewer()->findChild<QPushButton *>("filePlayPause")->click();
    QVERIFY(QTest::qWaitFor([&] { return bool(!controller.playbackState().isEmpty() && !controller.playbackState()["paused"].toBool()); },3000));
    QVERIFY(QTest::qWaitFor([&] { return bool(controller.playbackState()["positionMs"].toInt()>5000); },3000));
    if (nativeAudio) { rms.clear(); QVERIFY(QTest::qWaitFor([&] { return bool(audible(0.001,0.006)); },3000)); qInfo()<<"Native resumed after seek RMS"<<rms; }
    qInfo() << "Real sender FileSource -> VP8 DTLS-SRTP -> receiving worker/viewer frames" << pixels.count() << "paused seek timeline" << controller.playbackState();
    if (ending=="close") viewer()->close();
    else if (ending=="stop") viewer()->findChild<QPushButton *>("receiverStop")->click();
    else { broker.detach(senderId); QVERIFY(QTest::qWaitFor([&] { return bool(!controller.active()); },3000)); save(*viewer(),"received-disconnected"); }
    QVERIFY(QTest::qWaitFor([&] { return bool(broker.sessions().isEmpty()); },3000));
    QVERIFY(QTest::qWaitFor([&] { return bool(child.state() == QProcess::NotRunning); },3000));
    QCOMPARE(child.exitCode(),0); qInfo().noquote() << child.readAll(); client.shutdown();
    if (captureSocket) captureSocket->disconnectFromServer();
    if (nativeAudio && !QTest::qWaitFor([&] { return capture.state()==QProcess::NotRunning; },3000)) qFatal("Loopback process did not stop");
  }
private Q_SLOTS:
  void queuedEpochKeyframe_data() { QTest::addColumn<bool>("decode");QTest::newRow("encoded")<<false;QTest::newRow("decoded")<<true; }
  void queuedEpochKeyframe() {
    QFETCH(bool,decode);
    QString error;if(!GstCapturePipeline::initialize(error))qFatal("Codec unavailable");
    auto release=[](GstElement *p){gst_element_set_state(p,GST_STATE_NULL);gst_object_unref(p);};
    std::unique_ptr<GstElement,decltype(release)> graph(gst_parse_launch(
      "appsrc name=raw format=time ! video/x-raw,format=I420,width=320,height=180,framerate=30/1 ! vp8enc name=encoder deadline=1 lag-in-frames=0 keyframe-mode=disabled ! appsink name=encoded sync=false wait-on-eos=false",nullptr),release);
    auto *raw=GST_APP_SRC(gst_bin_get_by_name(GST_BIN(graph.get()),"raw"));
    auto *encoder=gst_bin_get_by_name(GST_BIN(graph.get()),"encoder");
    auto *sink=GST_APP_SINK(gst_bin_get_by_name(GST_BIN(graph.get()),"encoded"));
    gst_element_set_state(graph.get(),GST_STATE_PLAYING);
    GstSample *selected=nullptr;
    auto submit=[&](qint64 pts) {
      auto *buffer=gst_buffer_new_allocate(nullptr,320*180*3/2,nullptr);gst_buffer_memset(buffer,0,128,320*180*3/2);
      GST_BUFFER_PTS(buffer)=pts;GST_BUFFER_DURATION(buffer)=GST_SECOND/30;
      if(gst_app_src_push_buffer(raw,buffer)!=GST_FLOW_OK)qFatal("Codec input rejected");
      GstSample *sample=nullptr;if(!QTest::qWaitFor([&]{sample=gst_app_sink_try_pull_sample(sink,0);return sample;},3000))qFatal("Codec output missing");
      const bool key=!GST_BUFFER_FLAG_IS_SET(gst_sample_get_buffer(sample),GST_BUFFER_FLAG_DELTA_UNIT);
      qInfo()<<"Encoded epoch-order input PTS"<<pts<<"output PTS"<<GST_BUFFER_PTS(gst_sample_get_buffer(sample))<<"keyframe"<<key;
      if(pts==2*GST_SECOND/30)selected=gst_sample_ref(sample);
      gst_sample_unref(sample);return key;
    };
    submit(0);
    requestVideoKeyframe(encoder,2*GST_SECOND/30);
    submit(GST_SECOND/30); // Old queued buffer reaches the encoder after the seek request.
    const bool seekKeyframe=submit(2*GST_SECOND/30);
    for(auto *object:{GST_OBJECT(raw),GST_OBJECT(encoder),GST_OBJECT(sink)})gst_object_unref(object);
    if(!decode) {gst_sample_unref(selected);QVERIFY(seekKeyframe);return;}
    std::unique_ptr<GstElement,decltype(release)> decoder(gst_parse_launch(
      "appsrc name=wire format=time ! vp8dec ! videoconvert ! video/x-raw,format=BGRA ! appsink name=pixels sync=false wait-on-eos=false",nullptr),release);
    auto *wire=GST_APP_SRC(gst_bin_get_by_name(GST_BIN(decoder.get()),"wire"));auto *pixels=GST_APP_SINK(gst_bin_get_by_name(GST_BIN(decoder.get()),"pixels"));
    gst_app_src_set_caps(wire,gst_sample_get_caps(selected));gst_element_set_state(decoder.get(),GST_STATE_PLAYING);
    gst_app_src_push_buffer(wire,gst_buffer_ref(gst_sample_get_buffer(selected)));gst_sample_unref(selected);gst_app_src_end_of_stream(wire);
    auto *bus=gst_element_get_bus(decoder.get());GstMessage *terminal=nullptr;
    if(!QTest::qWaitFor([&]{terminal=gst_bus_pop_filtered(bus,GstMessageType(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));return terminal;},3000))qFatal("Decoder did not finish");
    qInfo()<<"Seek-only receiver decoder terminal"<<GST_MESSAGE_TYPE_NAME(terminal);gst_message_unref(terminal);gst_object_unref(bus);
    QImage image;if(auto *sample=gst_app_sink_try_pull_sample(pixels,0)) {
      GstVideoInfo info;GstMapInfo map;gst_video_info_from_caps(&info,gst_sample_get_caps(sample));auto *buffer=gst_sample_get_buffer(sample);
      gst_buffer_map(buffer,&map,GST_MAP_READ);image=QImage(map.data,info.width,info.height,info.stride[0],QImage::Format_ARGB32).copy();gst_buffer_unmap(buffer,&map);gst_sample_unref(sample);
    }
    for(auto *object:{GST_OBJECT(wire),GST_OBJECT(pixels)})gst_object_unref(object);
    VideoSurface surface;surface.resize(480,270);surface.setFrame(image);surface.show();save(surface,"seek-only-decoded");
    qInfo()<<"Seek-only actual decoded size"<<image.size();QVERIFY(!image.isNull());
  }
  void queuedPlayback_data() {
    QTest::addColumn<bool>("stale");QTest::newRow("stale")<<true;QTest::newRow("busy")<<false;
  }
  void queuedPlayback() {
    QFETCH(bool,stale);SenderWorker worker;worker.m_session=session;worker.m_source=source;worker.m_selection={{"kind","file"}};
    worker.m_state="streaming";worker.m_fileStarted=true;worker.m_file=std::make_unique<FileSource>();
    if(!worker.m_file->open(files.filePath("owned.webm"),session,source,false) ||
      !QTest::qWaitFor([&]{return worker.m_file->state()==FilePlaybackState::Paused;},3000))qFatal("File unavailable");
    if(!worker.m_file->command(session,source,"seek",5*GST_SECOND))qFatal("Initial seek rejected");
    if(stale) {
      if(!QTest::qWaitFor([&]{return worker.m_file->state()==FilePlaybackState::Paused;},3000))qFatal("Seek did not preroll");
      worker.receive(message("PlaybackCommand",{{"session",session},{"source",source},{"epoch",0},{"action","resume"},{"positionMs",0}}));
      QCOMPARE(worker.m_file->state(),FilePlaybackState::Paused);
    } else {
      worker.playbackCommand("seek",6000);
      QVERIFY(!worker.m_session.isEmpty());
    }
  }
  void playout_data() {
    QTest::addColumn<QString>("aspect");
    for(const char *name:{"begin","elapsed","negative","excess","initial","seek","pause","default"})QTest::newRow(name)<<QString(name);
  }
  void playout() {
    QFETCH(QString,aspect);
    if(aspect=="begin" || aspect=="elapsed") {
      QCOMPARE(audioPlayoutPosition((aspect=="begin" ? 20 : 70)*GST_MSECOND,40*GST_MSECOND),qint64((aspect=="begin" ? 0 : 30)*GST_MSECOND));return;
    }
    QString error;if(!GstCapturePipeline::initialize(error))qFatal("GStreamer unavailable");
    AudioOutput output;
    if(aspect=="negative" || aspect=="excess") {
      QVERIFY(!output.startSink(gst_element_factory_make("fakesink",nullptr),session,source,0,0,(aspect=="negative" ? -1 : 101)*GST_MSECOND));return;
    }
    auto *sink=gst_element_factory_make("appsink",nullptr);gst_object_ref(sink);
    const bool started=aspect=="default" ? output.startSink(sink,session,source,0,5*GST_SECOND)
      : output.startSink(sink,session,source,0,5*GST_SECOND,receiverPlayoutMarginNs);
    if(!started)qFatal("Playout fixture failed");
    quint64 epoch=0;qint64 origin=5*GST_SECOND;
    if(aspect=="seek") {epoch=1;origin=8*GST_SECOND;if(!output.reset(epoch,origin))qFatal("Playout seek reset failed");}
    AudioBlock block{QByteArray(7680,'\0'),session,source,epoch,origin,20*GST_MSECOND,0};
    if(!output.push(block))qFatal("Playout PCM rejected");
    GstSample *sample=nullptr;if(!QTest::qWaitFor([&]{sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),0);return sample;},3000))qFatal("Playout PCM missing");
    const auto timestamp=GST_BUFFER_PTS(gst_sample_get_buffer(sample));gst_sample_unref(sample);gst_object_unref(sink);
    if(aspect=="pause") {
      if(!output.pause(true))qFatal("Playout pause failed");const auto frozen=output.presentationTimeNs(),start=audioHostTimeNs();
      if(!QTest::qWaitFor([&]{return audioHostTimeNs()-start>=20*GST_MSECOND;},1000))qFatal("Clock measurement interval unavailable");
      QCOMPARE(output.presentationTimeNs(),frozen);
    } else QCOMPARE(timestamp,quint64(aspect=="default" ? 0 : receiverPlayoutMarginNs));
  }
  void timelineEndExcluded() {
    SessionClient client;SenderController controller(&client);PlaybackPanel panel(&controller);
    controller.m_playback={{"durationMs",12000},{"positionMs",0},{"seekable",true},{"allowed",true}};
    Q_EMIT controller.playbackChanged();
    QCOMPARE(panel.findChild<QSlider *>("fileSeek")->maximum(),11999);
  }
  void receiverPlayoutClock() {
    QString error;if(!GstCapturePipeline::initialize(error))qFatal("GStreamer unavailable");
    SenderWorker worker;worker.m_receiving=true;worker.m_session=session;worker.m_source=source;worker.m_clockSet=true;
    worker.m_media=std::make_unique<MediaTransport>();worker.m_output=std::make_unique<AudioOutput>();
    if(!worker.m_output->startSink(gst_element_factory_make("fakesink",nullptr),session,source,0,0,receiverPlayoutMarginNs))qFatal("Clock fixture unavailable");
    if(!QTest::qWaitFor([&]{return worker.m_output->runningTimeNs()>=60*GST_MSECOND;},1000))qFatal("Clock did not advance");
    VideoFrame frame;frame.mediaTimeNs=worker.m_output->runningTimeNs();frame.pixels=QImage(16,16,QImage::Format_ARGB32);frame.pixels.fill(Qt::blue);
    worker.m_videoQueue.push(frame);QSignalSpy frames(&worker,&SenderWorker::viewerFrame);worker.receivePoll();
    QCOMPARE(frames.count(),0);
  }
};
} // namespace deskflow::gui
int main(int argc,char **argv) {
  qputenv("QT_FORCE_STDERR_LOGGING","1");
#ifdef Q_OS_WIN
  if (argc>1 && QByteArray(argv[1])=="--capture") { QCoreApplication app(argc,argv); return deskflow::gui::captureEndpoint(app); }
#endif
  if (argc>1 && QByteArray(argv[1])=="--list-audio") {
    QCoreApplication app(argc,argv); QString error; if (!GstCapturePipeline::initialize(error)) return 2;
    for (const auto &endpoint : audioOutputEndpoints(error)) qInfo().noquote() << endpoint.id << endpoint.name;
    return error.isEmpty() ? 0 : 1;
  }
  if (argc>1 && QByteArray(argv[1])=="--sender") { QCoreApplication app(argc,argv); return deskflow::gui::senderEndpoint(app); }
  QApplication app(argc,argv); deskflow::gui::StreamingViewerTests test; return QTest::qExec(&test,argc,argv);
}
#include "StreamingViewerTests.moc"
