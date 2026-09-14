// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/Control.h"
#include "streaming/SessionBroker.h"
#include "gui/streaming/StreamViewer.h"
#include "common/StreamingInputGate.h"
#include <QTest>
#include <QSignalSpy>
#include <QPainter>
#include <QDir>
#include <QLabel>
using namespace deskflow::streaming;
namespace {
const QString session(32,'a'),source(32,'b'),lease(32,'c');
QJsonObject geometry(){return {{"session",session},{"source",source},{"frame",5},{"epoch",0},{"geometry",1},
  {"x",-1600},{"y",80},{"width",1600},{"height",900},{"valid",true}};}
QJsonObject input(QString kind="key",int sequence=1){
  QJsonObject e{{"session",session},{"source",source},{"lease",lease},{"sequence",sequence},{"frame",5},{"epoch",0},{"geometry",1},{"kind",kind}};
  if(kind=="key") {e["code"]=65;e["down"]=true;}else{e["x"]=-800;e["y"]=530;}
  if(kind=="button"){e["code"]=1;e["down"]=true;}
  if(kind=="wheel"){e["dx"]=120;e["dy"]=-120;}
  return e;
}
struct Native:NativeControl {
  bool supported=true,valid=true,free=true,physical=false,working=true;QList<QJsonObject> events;
  bool available()const override{return supported;}
  bool bind(const QJsonObject &)override{return valid;}
  bool verify(const QRect &,const std::optional<QPoint> &,bool)override{return valid;}
  bool idle()const override{return free;}
  bool inject(const QJsonObject &event)override{events.append(event);return working;}
  bool physicalInput()override{return std::exchange(physical,false);}
};
struct OwnerFixture {
  Native *native;ControlOwner owner;
  OwnerFixture():native(new Native),owner(std::unique_ptr<NativeControl>(native)){
    controlOwnsInput=false;viewerOwnsInput=false;ordinaryInputLocal=true;
    if(!owner.target({{"session",session},{"source",source}}) || !owner.geometry(geometry()))qFatal("Target setup failed");
  }
  void grant(){if(!owner.grant(lease,0))qFatal("Lease setup failed");}
};
struct BrokerFixture {
  SessionBroker broker;QString sender=QString(64,'d'),receiver=QString(64,'e'),generation=QString(32,'f');
  BrokerFixture(bool interactive=true){
    const QJsonObject caps{{"sources",QJsonArray{"window"}},{"receive",true},{"audio",QJsonArray{"off"}},{"control",true}};
    if(!broker.attach({sender,"Sender",generation,QHostAddress::LocalHost,caps}) || !broker.attach({receiver,"Receiver",generation,QHostAddress::LocalHost,caps}))qFatal("Peer setup failed");
    send(sender,"Offer",{{"session",session},{"source",source},{"to",receiver},{"kind","window"},{"audio","off"},{"preset","balanced"},{"interactive",interactive}});
    const QJsonObject identity{{"session",session},{"source",source}};send(receiver,"Accept",identity);
    const QString fp=QString("AA:").repeated(31)+"AA";
    const QString sdp="v=0\r\na=group:BUNDLE 0\r\nm=video 9 UDP/TLS/RTP/SAVPF 96\r\na=rtcp-mux\r\na=fingerprint:sha-256 "+fp+"\r\n";
    auto data=identity;data["sdp"]=sdp;data["fingerprint"]=fp;send(sender,"SdpOffer",data);send(receiver,"SdpAnswer",data);
    send(sender,"Ready",identity);send(receiver,"Ready",identity);send(sender,"ControlGeometry",geometry());
  }
  void send(const QString &from,const QString &type,const QJsonObject &data){if(!broker.dispatch(from,generation,message(type,data),100))qFatal("Broker setup failed: %s",qPrintable(type));}
  void grant(){send(sender,"GrantControl",{{"session",session},{"source",source},{"lease",lease}});}
};
}
class StreamingControlTests:public QObject {
  Q_OBJECT
private Q_SLOTS:
  void cleanup(){controlOwnsInput=false;viewerOwnsInput=false;ordinaryInputLocal=true;}
  void mapping_data(){QTest::addColumn<QString>("caseName");for(const auto *v:{"center","top-left","bottom-right","letterbox","right-boundary","nan"})QTest::newRow(v)<<QString(v);}
  void mapping(){QFETCH(QString,caseName);QPointF point(410,245);std::optional<QPoint> expected=QPoint(-800,530);
    if(caseName=="top-left"){point={10,20};expected=QPoint(-1600,80);}if(caseName=="bottom-right"){point={809.9,469.9};expected=QPoint(-1,979);}
    if(caseName=="letterbox"){point={200,19};expected={};}if(caseName=="right-boundary"){point={810,20};expected={};}
    if(caseName=="nan"){point={qQNaN(),20};expected={};}
    QCOMPARE(mapControlPoint({10,20,800,450},{-1600,80,1600,900},point),expected);
  }
  void grantGuards_data(){QTest::addColumn<QString>("caseName");for(const auto *v:{"unavailable","not-idle","ordinary-remote","viewer","bad-lease","native-geometry","mapping-invalid","double-grant"})QTest::newRow(v)<<QString(v);}
  void grantGuards(){QFETCH(QString,caseName);OwnerFixture f;QString token=lease;
    if(caseName=="unavailable")f.native->supported=false;if(caseName=="not-idle")f.native->free=false;
    if(caseName=="ordinary-remote")ordinaryInputLocal=false;if(caseName=="viewer")viewerOwnsInput=true;
    if(caseName=="bad-lease")token="bad";if(caseName=="native-geometry")f.native->valid=false;
    if(caseName=="mapping-invalid"){auto g=geometry();g["frame"]=6;g["valid"]=false;f.owner.geometry(g);}
    if(caseName=="double-grant")f.grant();
    QVERIFY(!f.owner.grant(token,1));
  }
  void rejected_data(){QTest::addColumn<QString>("caseName");for(const auto *v:{"view-only","session","source","lease","epoch","geometry","future-frame","replay","duplicate-down","unknown-up","out-of-bounds","fractional-sequence","extra-field","unknown-key"})QTest::newRow(v)<<QString(v);}
  void rejected(){QFETCH(QString,caseName);OwnerFixture f;auto e=input();if(caseName!="view-only")f.grant();
    if(caseName=="session")e["session"]=QString(32,'1');if(caseName=="source")e["source"]=QString(32,'1');
    if(caseName=="lease")e["lease"]=QString(32,'1');if(caseName=="epoch")e["epoch"]=1;if(caseName=="geometry")e["geometry"]=2;
    if(caseName=="future-frame")e["frame"]=6;if(caseName=="fractional-sequence")e["sequence"]=1.5;
    if(caseName=="extra-field")e["target"]="arbitrary";if(caseName=="unknown-key")e["code"]=256;
    if(caseName=="unknown-up")e["down"]=false;
    if(caseName=="replay" || caseName=="duplicate-down"){if(caseName=="replay")e=input("move");if(!f.owner.input(e,1))qFatal("Initial input failed");if(caseName=="duplicate-down")e["sequence"]=2;}
    if(caseName=="out-of-bounds"){e=input("move");e["x"]=0;}
    const auto before=f.native->events.size();
    QVERIFY(!f.owner.input(e,2));
    QCOMPARE(f.native->events.size(),before);
  }
  void release_data(){QTest::addColumn<QString>("caseName");for(const auto *v:{"explicit","timeout","physical","synchronous-physical","geometry-move","mapping-lost","native-target-lost","key-up","button-up"})QTest::newRow(v)<<QString(v);}
  void release(){QFETCH(QString,caseName);OwnerFixture f;f.grant();auto e=input(caseName=="button-up"?"button":"key");
    if(!f.owner.input(e,1))qFatal("Held input setup failed");
    if(caseName=="explicit")f.owner.revoke();if(caseName=="timeout")f.owner.poll(1500);
    if(caseName=="physical"){f.native->physical=true;f.owner.poll(2);}if(caseName=="synchronous-physical")f.native->physicalPriority();
    if(caseName=="native-target-lost"){f.native->valid=false;f.owner.poll(2);}
    if(caseName=="geometry-move" || caseName=="mapping-lost"){auto g=geometry();g["frame"]=6;if(caseName=="geometry-move"){g["geometry"]=2;g["x"]=-1500;}else g["valid"]=false;f.owner.geometry(g);}
    if(caseName=="key-up" || caseName=="button-up"){e["down"]=false;e["sequence"]=2;f.owner.input(e,2);}
    QCOMPARE(f.native->events.last()["down"],QJsonValue(false));
    QCOMPARE(f.native->events.last()["code"],e["code"]);
    QCOMPARE(f.native->events.last()["kind"],e["kind"]);
  }
  void delivery_data(){QTest::addColumn<QString>("kind");for(const auto *v:{"key","button","move","wheel"})QTest::newRow(v)<<QString(v);}
  void delivery(){QFETCH(QString,kind);OwnerFixture f;f.grant();const auto e=input(kind);f.owner.input(e,1);
    QJsonObject actual,expected;QStringList keys{"kind"};
    if(kind=="key" || kind=="button")keys<<"code"<<"down";
    if(kind!="key")keys<<"x"<<"y";
    if(kind=="wheel")keys<<"dx"<<"dy";
    for(const auto &key:keys){actual[key]=f.native->events.value(0)[key];expected[key]=e[key];}
    QCOMPARE(actual,expected);
  }
  void targetReplacement(){OwnerFixture f;f.grant();QVERIFY(!f.owner.target({{"session",session},{"source",source},{"handle","different"}}));}
  void unlimitedMotion(){OwnerFixture f;f.owner.motionRate(0);f.grant();for(int i=1;i<=10;++i)f.owner.input(input("move",i),1);QCOMPARE(f.native->events.size(),10);}
  void heartbeatExtension(){OwnerFixture f;f.grant();f.owner.heartbeat(1000);QVERIFY(f.owner.input(input(),2000));}
  void motionTail(){OwnerFixture f;f.grant();f.owner.input(input("move"),0);auto e=input("move",2);e["x"]=-600;f.owner.input(e,1);f.owner.poll(4);QCOMPARE(f.native->events.last()["x"],QJsonValue(-600));}
  void motionBound(){OwnerFixture f;f.grant();for(int i=1;i<=20;++i)f.owner.input(input("move",i),1);QCOMPARE(f.native->events.size(),1);}
  void gateLifetime(){OwnerFixture f;f.grant();const bool during=streamingOwnsInput();f.owner.revoke();QCOMPARE(QString::number(during)+QString::number(streamingOwnsInput()),QString("10"));}
  void broker_data(){QTest::addColumn<QString>("caseName");for(const auto *v:{"grant","view-only","receiver-grant","input-before-grant","sender-input","stale-source","replay","revoked","disconnect"})QTest::newRow(v)<<QString(v);}
  void broker(){QFETCH(QString,caseName);BrokerFixture f(caseName!="view-only");QJsonObject grant{{"session",session},{"source",source},{"lease",lease}};
    if(caseName=="grant" || caseName=="view-only" || caseName=="receiver-grant") {
      QCOMPARE(f.broker.dispatch(caseName=="receiver-grant"?f.receiver:f.sender,f.generation,message("GrantControl",grant),101),caseName=="grant");return;
    }
    if(caseName!="input-before-grant")f.grant();auto e=input();
    if(caseName=="stale-source")e["source"]=QString(32,'1');
    if(caseName=="replay")f.send(f.receiver,"ControlInput",e);
    if(caseName=="revoked")f.send(f.sender,"RevokeControl",{{"session",session},{"source",source}});
    if(caseName=="disconnect")f.broker.detach(f.sender);
    QVERIFY(!f.broker.dispatch(caseName=="sender-input"?f.sender:f.receiver,f.generation,message("ControlInput",e),102));
  }
  void surface_data(){QTest::addColumn<QString>("caseName");for(const auto *v:{"view-only","mapped-click","letterbox","stale-pixels","key-up","focus-release"})QTest::newRow(v)<<QString(v);}
  void surface(){QFETCH(QString,caseName);deskflow::gui::VideoSurface surface;surface.resize(800,600);
    VideoFrame frame;frame.session=session;frame.source=source;frame.sequence=5;frame.geometryGeneration=1;frame.coordinateMappingValid=true;frame.physicalGeometry={-1600,80,1600,900};
    frame.pixels=QImage(1600,900,QImage::Format_RGB32);frame.pixels.fill(QColor("#19314c"));surface.setPresentedFrame(frame);
    surface.setControlEnabled(caseName!="view-only");QSignalSpy events(&surface,&deskflow::gui::VideoSurface::inputRequested);
    QSignalSpy revoke(&surface,&deskflow::gui::VideoSurface::revokeRequested);
    if(caseName=="stale-pixels"){auto image=frame.pixels;image.fill(Qt::red);surface.setFrame(image);}
    if(caseName=="key-up")QTest::keyRelease(&surface,Qt::Key_A);
    else if(caseName=="focus-release"){QFocusEvent e(QEvent::FocusOut);QApplication::sendEvent(&surface,&e);}
    else QTest::mousePress(&surface,Qt::LeftButton,Qt::NoModifier,caseName=="letterbox"?QPoint(400,10):QPoint(400,300));
    if(caseName=="mapped-click")QCOMPARE(events.first().first().toJsonObject()["x"],QJsonValue(-800));
    else if(caseName=="key-up")QCOMPARE(events.first().first().toJsonObject()["down"],QJsonValue(false));
    else if(caseName=="focus-release")QCOMPARE(revoke.size(),1);
    else QCOMPARE(events.size(),0);
  }
  void controlIndicator(){
    deskflow::streaming::SessionClient client;deskflow::gui::SenderController controller(&client);
    // Isolated QWidget boundary fixture. No core/native input session is created.
    VideoFrame frame;frame.session=session;frame.source=source;frame.sequence=5;frame.geometryGeneration=1;frame.coordinateMappingValid=true;frame.physicalGeometry={-1600,80,1600,900};
    frame.pixels=QImage(1600,900,QImage::Format_RGB32);frame.pixels.fill(QColor("#19314c"));
    {QPainter painter(&frame.pixels);painter.setPen(Qt::white);QFont font=painter.font();font.setPixelSize(54);painter.setFont(font);painter.drawText(frame.pixels.rect(),Qt::AlignCenter,"Generated source fixture\n1600 x 900; mapped origin -1600, 80");}
    const QString local(64,'d'),remote(64,'e');
    Q_EMIT client.received(message("Identity",{{"id",local},{"address","127.0.0.1"}}));
    if(!QTest::qWaitFor([&]{return controller.inventory()["id"]==local;},1000))qFatal("Component identity delivery failed");
    Q_EMIT client.received(message("Offer",{{"session",session},{"source",source},{"kind","window"},{"audio","off"},{"to",local},{"from",remote},{"interactive",true}}));
    deskflow::gui::StreamViewer viewer(&controller,{{"session",session},{"source",source},{"kind","window"},{"audio","off"},
      {"senderName","Owned component fixture"},{"title","Generated source - input mapping UI"}});
    viewer.show();
    Q_EMIT client.received(message("State",{{"session",session},{"source",source},{"sender",remote},{"receiver",local},{"state","streaming"}}));
    if(!QTest::qWaitFor([&]{return controller.inventory()["state"]=="streaming";},1000))qFatal("Component state delivery failed");
    Q_EMIT controller.presentedFrame(frame);
    Q_EMIT client.received(message("GrantControl",{{"session",session},{"source",source},{"lease",lease}}));
    if(!QTest::qWaitFor([&]{return controller.status().contains("interactive control granted");},1000))qFatal("Component grant delivery failed");
    QCOMPARE(viewer.findChild<QLabel *>("receiverMode")->text(),QString("Interactive control granted - Escape or focus loss releases control"));
    const auto proof=qEnvironmentVariable("STREAMING_CONTROL_PROOF");
    if(!proof.isEmpty()){QDir().mkpath(proof);if(!viewer.grab().save(proof+"/control-widget-consented.png"))qFatal("Cannot capture component proof");}
  }
};
QTEST_MAIN(StreamingControlTests)
#include "StreamingControlTests.moc"
