// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "gui/streaming/SenderWorker.h"
#include "gui/streaming/SenderController.h"
#include "gui/streaming/StreamDialog.h"
#include "streaming/Protocol.h"
#include "streaming/GstCapturePipeline.h"
#include <QApplication>
#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <atomic>

using namespace deskflow::streaming;
namespace deskflow::gui {
namespace {
const QString local(64, 'a'), remote(64, 'b'), session(32, 'c'), source(32, 'd');
QJsonObject selection() {
  return {{"kind", "file"}, {"path", "owned.webm"}, {"to", remote}, {"audio", "off"},
    {"preset", "low"}, {"interactive", false}};
}
class CleanupCapture final : public CaptureDevice {
public:
  std::function<void()> cleanup;
  CaptureStatus current{CaptureState::Available, {}};
  QVector<CaptureSource> sources() override { return {}; }
  bool start(const QString &, const QString &, int) override {
    stop(); current={CaptureState::PermissionRequired,"Owned source awaiting permission"};
    Q_EMIT statusChanged(); return true;
  }
  void stop() override { if (cleanup) cleanup();current={CaptureState::Stopped,{}};Q_EMIT statusChanged(); }
  std::optional<VideoFrame> takeFrame() override { return {}; }
  CaptureStatus status() const override { return current; }
};
class OwnedEnvironment final : public SessionEnvironment {
public:
  QString failure;
  QString start()override{return failure;}
  QString poll()override{return failure;}
};
}
class StreamingRecoveryTests : public QObject {
  Q_OBJECT
  void inventory(SenderWorker &worker) {
    worker.m_inventory = {{"built", true}, {"connected", true}, {"id", local}, {"address", "127.0.0.1"},
      {"peers", QJsonArray{QJsonObject{{"id", remote}, {"address", "127.0.0.1"},
        {"capabilities", QJsonObject{{"receive", true}, {"audio", QJsonArray{"off"}}}}}}}};
  }
  void receiver(SenderWorker &worker) {
    inventory(worker); worker.m_session=session; worker.m_source=source;
    worker.m_selection=selection(); worker.m_receiving=true; worker.m_state="negotiating";
    worker.begin();
    if (!worker.m_media || !worker.m_media->active()) qFatal("Real receiver transport setup failed");
  }
private Q_SLOTS:
  void initTestCase(){gst_init(nullptr,nullptr);}
  void mediaBackendPrivacy(){
    SenderWorker worker;receiver(worker);auto *bus=gst_bus_new();
    const QString sentinel="PRIVATE-OWNED-MEDIA-ENDPOINT-2099";
    auto *error=g_error_new_literal(GST_RESOURCE_ERROR,GST_RESOURCE_ERROR_FAILED,qPrintable(sentinel));
    gst_bus_post(bus,gst_message_new_error(nullptr,error,"owned media backend fault"));g_error_free(error);
    if(!worker.m_media->consumeBusError(bus))qFatal("Owned media bus fault not consumed");
    gst_object_unref(bus);const auto reason=worker.m_media->error();
    qInfo()<<"Actual media bus diagnostic"<<reason;
    QVERIFY(!reason.contains(sentinel));worker.stop();
  }
  void captureBackendStatus(){
    SessionClient client;SenderController controller(&client);StreamLauncher launcher(&controller);
    launcher.resize(1280,100);launcher.show();
    QMetaObject::invokeMethod(controller.m_worker,[&]{
      GstCapturePipeline pipeline;QString error;auto *sourceElement=gst_element_factory_make("videotestsrc",nullptr);
      if(!sourceElement)qFatal("Owned video generator unavailable");
      g_object_set(sourceElement,"is-live",TRUE,nullptr);gst_object_ref(sourceElement);
      if(!pipeline.start(sourceElement,30,error))qFatal("Owned capture pipeline unavailable");
      auto *failure=g_error_new_literal(GST_RESOURCE_ERROR,GST_RESOURCE_ERROR_FAILED,"PRIVATE-OWNED-CAPTURE-2099");
      gst_element_post_message(sourceElement,gst_message_new_error(GST_OBJECT(sourceElement),failure,"owned capture diagnostic"));
      g_error_free(failure);pipeline.pull(error);gst_object_unref(sourceElement);
      if(error.isEmpty())qFatal("Owned capture error was not consumed");
      auto &worker=*controller.m_worker;receiver(worker);worker.m_receiving=false;worker.m_selection["kind"]="window";
      auto capture=std::make_unique<CleanupCapture>();auto *owned=capture.get();worker.m_capture=std::move(capture);worker.observeCapture();
      owned->current={CaptureState::TemporarilyUnavailable,error};Q_EMIT owned->statusChanged();
    },Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&controller,QEvent::MetaCall);
    const auto path=qEnvironmentVariable("STREAMING_RECOVERY_SCREENSHOT");
    if(!path.isEmpty() && !launcher.grab().save(path))qFatal("Capture status screenshot failed");
    qInfo()<<"Actual capture error rendered in launcher"<<controller.status();
    QCOMPARE(controller.status(),QString("Capture backend failed. Check source permission and installed capture components."));
  }
  void backendStatus_data(){QTest::addColumn<bool>("audio");QTest::newRow("media")<<false;QTest::newRow("audio")<<true;}
  void backendStatus(){
    QFETCH(bool,audio);SessionClient client;SenderController controller(&client);StreamLauncher launcher(&controller);
    launcher.resize(1280,100);launcher.show();
    QMetaObject::invokeMethod(controller.m_worker,[&]{
      auto &worker=*controller.m_worker;receiver(worker);
      auto *failure=g_error_new_literal(GST_RESOURCE_ERROR,GST_RESOURCE_ERROR_FAILED,"PRIVATE-OWNED-BACKEND-2099");
      if(audio){
        worker.m_output=std::make_unique<AudioOutput>();auto *sink=gst_element_factory_make("fakesink",nullptr);
        if(!worker.m_output->startSink(sink,session,source,0))qFatal("Owned status output setup failed");
        worker.m_clockSet=true;
        gst_element_post_message(sink,gst_message_new_error(GST_OBJECT(sink),failure,"owned audio diagnostic"));worker.receivePoll();
      }else{
        auto *bus=gst_bus_new();gst_bus_post(bus,gst_message_new_error(nullptr,failure,"owned media diagnostic"));
        if(!worker.m_media->consumeBusError(bus))qFatal("Owned media diagnostic not consumed");gst_object_unref(bus);
      }
      g_error_free(failure);
    },Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(controller.m_worker,[]{},Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&controller,QEvent::MetaCall);
    const auto path=qEnvironmentVariable("STREAMING_RECOVERY_SCREENSHOT");
    if(!path.isEmpty() && !launcher.grab().save(path))qFatal("Backend status screenshot failed");
    qInfo()<<"Actual backend error rendered in launcher"<<controller.status();
    QCOMPARE(controller.status(),audio?QString("Audio output failed: Audio backend failed. Check the selected device and installed audio components."):
      QString("Media stopped: Media backend failed. Check the selected source, connection and installed codec components."));
  }
  void earlyEpochAudio(){
    SenderWorker worker;worker.m_receiveEpoch=0;worker.m_origin=0;
    const AudioBlock audio{QByteArray(7680,'\0'),session,source,1,5000*GST_MSECOND,20*GST_MSECOND,0};
    worker.admitReceivedAudio(audio);
    qInfo()<<"New epoch PCM arriving before video retained"<<worker.m_futureAudio.has_value();
    QCOMPARE(worker.m_futureAudio.has_value(),true);
  }
  void epochAudioPolicy_data(){QTest::addColumn<QString>("aspect");for(const char *name:{"first-block","release","old-epoch","stop","timeout","pause-output"})QTest::newRow(name)<<QString(name);}
  void epochAudioPolicy(){
    QFETCH(QString,aspect);SenderWorker worker;receiver(worker);worker.m_receiveEpoch=0;worker.m_origin=0;
    AudioBlock audio{QByteArray(7680,'\0'),session,source,1,5000*GST_MSECOND,20*GST_MSECOND,0};
    if(aspect=="pause-output"){
      worker.m_output=std::make_unique<AudioOutput>();
      auto *sink=gst_element_factory_make("fakesink",nullptr);
      if(!worker.m_output->startSink(sink,session,source,0))qFatal("Owned output setup failed");
    }
    worker.admitReceivedAudio(audio);
    if(aspect=="first-block"){
      audio.mediaTimeNs+=20*GST_MSECOND;worker.admitReceivedAudio(audio);
      QCOMPARE(worker.m_futureAudio?worker.m_futureAudio->mediaTimeNs:qint64(-1),qint64(5000*GST_MSECOND));
    }
    if(aspect=="release"){
      worker.m_receiveEpoch=1;worker.m_origin=5000*GST_MSECOND;worker.releaseFutureAudio();
      QCOMPARE(worker.m_pendingAudio.empty()?qint64(-1):worker.m_pendingAudio.front().mediaTimeNs,qint64(5000*GST_MSECOND));
    }
    if(aspect=="old-epoch"){
      worker.m_receiveEpoch=2;worker.releaseFutureAudio();
      QCOMPARE(worker.m_pendingAudio.empty(),true);
    }
    if(aspect=="stop"){worker.stop();QCOMPARE(worker.m_futureAudio.has_value(),false);}
    if(aspect=="timeout"){
      worker.m_futureAudioSince=audioHostTimeNs()-3*GST_SECOND;worker.receivePoll();
      QCOMPARE(worker.m_session.isEmpty(),true);
    }
    if(aspect=="pause-output"){
      audio.timelineEpoch=0;audio.mediaTimeNs=0;
      QCOMPARE(worker.m_output->push(audio),false);
    }
    worker.stop();
  }
  void queuedLease(){
    SessionClient client;SenderController controller(&client);QStringList sent;
    QMetaObject::invokeMethod(controller.m_worker,[&]{
      auto &worker=*controller.m_worker;inventory(worker);worker.m_session=session;worker.m_source=source;
      worker.m_state="streaming";worker.m_receiving=true;worker.m_controlLease=QString(32,'e');worker.publish();
    },Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&controller,QEvent::MetaCall);
    connect(controller.m_worker,&SenderWorker::outgoing,controller.m_worker,[&](const QJsonObject &frame){sent.append(frame["type"].toString());},Qt::DirectConnection);
    QMetaObject::invokeMethod(controller.m_worker,[&]{controller.m_worker->m_controlLease=QString(32,'f');},Qt::QueuedConnection);
    VideoFrame frame;frame.session=session;frame.source=source;frame.coordinateMappingValid=true;
    controller.controlInput({{"kind","key"},{"code",65},{"down",true}},frame);
    QMetaObject::invokeMethod(controller.m_worker,[]{},Qt::BlockingQueuedConnection);
    QCOMPARE(sent.isEmpty(),true);
  }
  void immediateStop_data(){QTest::addColumn<bool>("repeated");QTest::newRow("single-start")<<false;QTest::newRow("repeated-start")<<true;}
  void immediateStop(){
    QFETCH(bool,repeated);SessionClient client;SenderController controller(&client);
    QMetaObject::invokeMethod(controller.m_worker,[&]{inventory(*controller.m_worker);controller.m_worker->publish();},Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&controller,QEvent::MetaCall);
    controller.start(selection());if(repeated)controller.start(selection());controller.stop();
    bool active=true;QMetaObject::invokeMethod(controller.m_worker,[&]{active=!controller.m_worker->m_session.isEmpty();},Qt::BlockingQueuedConnection);
    qInfo()<<"Immediate Stop after queued start"<<"repeated"<<repeated<<"still active"<<active;
    QCOMPARE(active,false);
  }
  void interruptionStatus(){
    SessionClient client;SenderController controller(&client);StreamLauncher launcher(&controller);launcher.resize(1120,100);launcher.show();
    QMetaObject::invokeMethod(controller.m_worker,[&]{
      auto &worker=*controller.m_worker;receiver(worker);
      worker.receive(message("State",{{"session",session},{"source",source},{"sender",remote},{"receiver",local},{"state","streaming"}}));
      auto environment=std::make_unique<OwnedEnvironment>();environment->failure="Owned login locked. Start a new stream after returning.";
      worker.m_environment=std::move(environment);worker.poll();
    },Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&controller,QEvent::MetaCall);
    const auto path=qEnvironmentVariable("STREAMING_RECOVERY_SCREENSHOT");
    if(!path.isEmpty() && !launcher.grab().save(path))qFatal("Interruption widget screenshot failed");
    qInfo()<<"Rendered interruption status"<<controller.status();
    QCOMPARE(controller.active(),false);
  }
  void queuedAction_data(){QTest::addColumn<QString>("action");for(const char *name:{"stop","accept","volume","playback","grant","revoke","focus"})QTest::newRow(name)<<QString(name);}
  void queuedAction(){
    QFETCH(QString,action);SessionClient client;SenderController controller(&client);QStringList sent;
    const QString next(32,'f');
    auto prepare=[&](const QString &id){
      auto &worker=*controller.m_worker;inventory(worker);worker.m_session=id;worker.m_source=source;
      worker.m_state="streaming";worker.m_receiving=true;worker.m_selection=selection();worker.m_selection["playback"]=true;
      worker.m_playback={{"epoch",0}};worker.m_controlLease=QString(32,'e');
      worker.m_inventory["session"]=id;worker.m_inventory["state"]="streaming";worker.publish();
    };
    QMetaObject::invokeMethod(controller.m_worker,[&]{prepare(session);},Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&controller,QEvent::MetaCall);
    connect(controller.m_worker,&SenderWorker::outgoing,controller.m_worker,[&](const QJsonObject &frame){sent.append(frame["type"].toString());},Qt::DirectConnection);
    QMetaObject::invokeMethod(controller.m_worker,[&]{
      controller.m_worker->stop({},false);prepare(next);
      if(action=="accept")controller.m_worker->m_state="awaitingConsent";
      if(action=="grant"){controller.m_worker->m_receiving=false;controller.m_worker->m_controlLease.clear();controller.m_worker->m_selection["interactive"]=true;}
    },Qt::QueuedConnection);
    if(action=="stop")controller.stop();
    if(action=="accept")controller.accept({});
    if(action=="volume")controller.volume(0.25,true);
    if(action=="playback")controller.playbackCommand("pause");
    if(action=="grant")controller.grantControl();
    if(action=="revoke")controller.revokeControl();
    if(action=="focus")controller.viewerFocus(true);
    QString active,lease;double gain=0;
    QMetaObject::invokeMethod(controller.m_worker,[&]{active=controller.m_worker->m_session;lease=controller.m_worker->m_controlLease;gain=controller.m_worker->m_gain;},Qt::BlockingQueuedConnection);
    qInfo()<<"Queued retired viewer action"<<action<<"new session retained"<<(active==next)<<"messages"<<sent;
    if(action=="stop")QCOMPARE(active,next);
    else if(action=="volume")QCOMPARE(gain,1.0);
    else if(action=="revoke")QCOMPARE(lease,QString(32,'e'));
    else QCOMPARE(sent.isEmpty(),true);
  }
  void sessionInterruption_data(){QTest::addColumn<bool>("receiving");QTest::newRow("file-sender")<<false;QTest::newRow("receiver")<<true;}
  void sessionInterruption(){
    QFETCH(bool,receiving);SenderWorker worker;receiver(worker);worker.m_receiving=receiving;
    auto environment=std::make_unique<OwnedEnvironment>();environment->failure="Owned login locked";
    worker.m_environment=std::move(environment);worker.poll();
    qInfo()<<"OS-session boundary interruption left session active"<<!worker.m_session.isEmpty()<<"receiver"<<receiving;
    QCOMPARE(worker.m_session.isEmpty(),true);worker.stop();
  }
  void environmentAdmission(){
    SenderWorker worker;inventory(worker);worker.m_session=session;worker.m_source=source;
    worker.m_selection=selection();worker.m_receiving=true;worker.m_state="negotiating";
    auto environment=std::make_unique<OwnedEnvironment>();environment->failure="Owned login already locked";
    worker.m_environment=std::move(environment);worker.begin();
    QCOMPARE(bool(worker.m_media),false);worker.stop();
  }
  void controllerExitRevokesFirst() {
    SessionClient client;
    // Use a named owned endpoint rather than the user's singleton.
    PrivateIpcServer owned;const auto endpoint=randomId();
    if(!owned.listen(endpoint))qFatal("Owned listener unavailable");
    std::atomic_bool connected=false;
    connect(&client,&SessionClient::connectedChanged,this,[&](bool value){connected=value;});
    client.start(endpoint);
    if(!QTest::qWaitFor([&]{return connected.load();},3000))qFatal("Owned IPC attachment failed");
    auto controller=std::make_unique<SenderController>(&client);
    bool cleanupConnected=true;
    QMetaObject::invokeMethod(controller->m_worker,[&]{
      auto capture=std::make_unique<CleanupCapture>();
      capture->cleanup=[&]{cleanupConnected=connected.load();};
      controller->m_worker->m_capture=std::move(capture);
    },Qt::BlockingQueuedConnection);
    controller.reset();
    qInfo()<<"Controller exit native cleanup observed attached IPC"<<cleanupConnected;
    QCOMPARE(cleanupConnected,false);client.shutdown();
  }
  void backendAudioPrivacy() {
    const QString sentinel="PRIVATE-OWNED-DEVICE-2099";
    AudioOutput output;
    auto *sink=gst_element_factory_make("fakesink",nullptr);
    if(!sink || !output.startSink(sink,session,source,0))qFatal("Owned silent sink setup failed");
    auto *failure=g_error_new_literal(GST_RESOURCE_ERROR,GST_RESOURCE_ERROR_FAILED,qPrintable(sentinel));
    if(!gst_element_post_message(sink,gst_message_new_error(GST_OBJECT(sink),failure,"owned backend diagnostic")))qFatal("Owned bus fault post failed");
    g_error_free(failure);
    const auto reason=output.error();
    if(reason.isEmpty())qFatal("Posted bus error was not consumed");
    qInfo()<<"Actual asynchronous audio backend error"<<reason;
    QVERIFY(!reason.contains(sentinel));
  }
  void retiredFailureStatus() {
    SessionClient client;SenderController controller(&client);StreamLauncher launcher(&controller);
    launcher.resize(1120,100);launcher.show();
    QMetaObject::invokeMethod(controller.m_worker,[&]{
      receiver(*controller.m_worker);
      Q_EMIT controller.m_worker->m_media->failed("Owned retired encoder failure");
      controller.m_worker->stop();controller.m_worker->start(selection());
    },Qt::BlockingQueuedConnection);
    QMetaObject::invokeMethod(controller.m_worker,[]{},Qt::BlockingQueuedConnection);
    QCoreApplication::sendPostedEvents(&controller,QEvent::MetaCall);
    const auto path=qEnvironmentVariable("STREAMING_RECOVERY_SCREENSHOT");
    if(!path.isEmpty() && !launcher.grab().save(path))qFatal("Actual widget screenshot save failed");
    qInfo()<<"Visible recovery status"<<controller.status();
    QCOMPARE(controller.active(),true);
  }
  void retiredMediaFailure() {
    SenderWorker worker; receiver(worker);
    // Exactly the queued native/media error signal used by the product owner.
    Q_EMIT worker.m_media->failed("Owned encoder failure");
    worker.stop(); worker.start(selection());
    const auto next=worker.m_session;
    if (next.isEmpty()) qFatal("Fresh user Offer setup failed");
    QCoreApplication::sendPostedEvents(&worker, QEvent::MetaCall);
    qInfo()<<"Retired queued failure delivered; fresh offer session retained"<<!worker.m_session.isEmpty();
    QCOMPARE(worker.m_session, next);
  }
  void stopOrdering() {
    SenderWorker worker; receiver(worker);
    QStringList order;
    auto capture=std::make_unique<CleanupCapture>();
    capture->cleanup=[&]{order.append("capture-cleanup");}; worker.m_capture=std::move(capture);
    connect(&worker,&SenderWorker::outgoing,this,[&](const QJsonObject &frame){
      if(frame["type"]=="Stop")order.append("stop-command");
    });
    worker.stop();
    qInfo()<<"Stop/native cleanup ordering"<<order;
    QCOMPARE(order, QStringList({"stop-command","capture-cleanup"}));
  }
  void captureInitialCleanup() {
    SenderWorker worker; inventory(worker);
    worker.m_capture=std::make_unique<CleanupCapture>();worker.observeCapture();
    worker.m_session=session;worker.m_source=source;worker.m_selection=selection();worker.m_selection["kind"]="window";
    worker.m_state="negotiating";
    // Native adapters synchronously announce Stopped from start's initial cleanup.
    // Model only the native API boundary; use the real worker/transport owner.
    worker.begin();
    qInfo()<<"Native boundary initial cleanup retains accepted source"<<!worker.m_session.isEmpty();
    QCOMPARE(worker.m_session, session);
    worker.stop();
  }
  void waitingFirstFrame_data() {
    QTest::addColumn<QString>("aspect");
    for(const char *name:{"signal","poll","state-name"})QTest::newRow(name)<<QString(name);
  }
  void waitingFirstFrame() {
    QFETCH(QString,aspect);
    if(aspect=="state-name"){QCOMPARE(captureStateName(CaptureState::Starting),QString("starting"));return;}
    SenderWorker worker;receiver(worker);worker.m_receiving=false;worker.m_selection["kind"]="window";
    auto capture=std::make_unique<CleanupCapture>();capture->current={CaptureState::Starting,"Waiting for first owned frame"};
    worker.m_capture=std::move(capture);worker.observeCapture();
    if(aspect=="signal")Q_EMIT worker.m_capture->statusChanged();else worker.poll();
    qInfo()<<"First-frame pending admission"<<aspect<<!worker.m_session.isEmpty();
    QCOMPARE(worker.m_session,session);worker.stop();
  }
};
}
int main(int argc,char **argv){QApplication app(argc,argv);deskflow::gui::StreamingRecoveryTests test;return QTest::qExec(&test,argc,argv);}
#include "StreamingRecoveryTests.moc"
