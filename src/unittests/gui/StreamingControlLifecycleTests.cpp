// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/Control.h"
#include "streaming/ControlKeys.h"
#include "gui/streaming/StreamViewer.h"
#include "common/StreamingInputGate.h"
#include <QTest>
#include <QSignalSpy>
#include <QShortcut>
#include <QDir>
using namespace deskflow::streaming;
namespace {
struct Native:NativeControl {
  bool working=true,buttonReleaseWorks=true;QList<QJsonObject> events;std::function<void()> verifying;std::function<void(const QJsonObject &)> injecting;
  bool available()const override{return true;}bool bind(const QJsonObject &)override{return true;}
  bool verify(const QRect &,const std::optional<QPoint> &,bool)override{if(verifying)verifying();return true;}bool idle()const override{return true;}
  bool inject(const QJsonObject &event)override{events.append(event);if(injecting)injecting(event);return working && (buttonReleaseWorks || event["kind"]!="button" || event["down"].toBool());}bool physicalInput()override{return false;}
};
const QString session(32,'a'),source(32,'b'),lease(32,'c');
QJsonObject inputEvent(int sequence=1){return {{"session",session},{"source",source},{"lease",lease},{"sequence",sequence},{"frame",1},{"epoch",0},{"geometry",1},{"kind","key"},{"code",65},{"down",true}};}
struct Fixture {
  Native *native=new Native;ControlOwner owner{std::unique_ptr<NativeControl>(native)};
  Fixture(){controlOwnsInput=false;viewerOwnsInput=false;ordinaryInputLocal=true;
    if(!owner.target({{"session",session},{"source",source}}) || !owner.geometry({{"session",session},{"source",source},{"frame",1},{"epoch",0},{"geometry",1},{"x",0},{"y",0},{"width",640},{"height",400},{"valid",true}}) || !owner.grant(lease,0) || !owner.input(inputEvent(),1))qFatal("Initial held-key setup failed");}
};
struct ViewerFixture {
  SessionClient client;deskflow::gui::SenderController controller{&client};std::unique_ptr<deskflow::gui::StreamViewer> viewer;
  ViewerFixture(){const QString local(64,'d'),remote(64,'e');
    Q_EMIT client.received(message("Identity",{{"id",local},{"address","127.0.0.1"}}));
    if(!QTest::qWaitFor([&]{return controller.inventory()["id"]==local;},1000))qFatal("Viewer identity setup failed");
    Q_EMIT client.received(message("Offer",{{"session",session},{"source",source},{"kind","window"},{"audio","off"},{"to",local},{"from",remote},{"interactive",true}}));
    Q_EMIT client.received(message("State",{{"session",session},{"source",source},{"sender",remote},{"receiver",local},{"state","streaming"}}));
    if(!QTest::qWaitFor([&]{return controller.inventory()["state"]=="streaming";},1000))qFatal("Viewer state setup failed");
    viewer=std::make_unique<deskflow::gui::StreamViewer>(&controller,QJsonObject{{"session",session},{"source",source},{"kind","window"},{"audio","off"},{"senderName","Owned component fixture"},{"title","Control lifecycle"}});
    viewer->show();VideoFrame frame;frame.session=session;frame.source=source;frame.sequence=1;frame.geometryGeneration=1;frame.coordinateMappingValid=true;frame.physicalGeometry={0,0,640,400};frame.pixels=QImage(640,400,QImage::Format_RGB32);frame.pixels.fill(QColor("#19314c"));Q_EMIT controller.presentedFrame(frame);
    Q_EMIT client.received(message("GrantControl",{{"session",session},{"source",source},{"lease",lease}}));
    if(!QTest::qWaitFor([&]{return controller.status().contains("interactive control granted");},1000))qFatal("Viewer grant setup failed");
  }
};
}
class StreamingControlLifecycleTests:public QObject {
  Q_OBJECT
private Q_SLOTS:
  void cleanup(){controlOwnsInput=false;viewerOwnsInput=false;ordinaryInputLocal=true;}
  void navigationIsNotPrintable_data(){QTest::addColumn<int>("code");for(int code:{33,34,35,36,37,38,39,40,45,46})QTest::newRow(qPrintable(QString::number(code)))<<code;}
  void navigationIsNotPrintable(){QFETCH(int,code);QCOMPARE(printableControlSymbol(code),0);}
  void portableViewerKey_data(){QTest::addColumn<int>("key");QTest::addColumn<int>("code");
    QTest::newRow("semicolon")<<int(Qt::Key_Semicolon)<<186;QTest::newRow("bracket")<<int(Qt::Key_BracketLeft)<<219;QTest::newRow("backslash")<<int(Qt::Key_Backslash)<<220;
    QTest::newRow("page-up")<<int(Qt::Key_PageUp)<<33;QTest::newRow("page-down")<<int(Qt::Key_PageDown)<<34;QTest::newRow("insert")<<int(Qt::Key_Insert)<<45;
  }
  void portableViewerKey(){QFETCH(int,key);QFETCH(int,code);deskflow::gui::VideoSurface surface;VideoFrame frame;frame.pixels=QImage(640,400,QImage::Format_RGB32);frame.coordinateMappingValid=true;surface.setPresentedFrame(frame);surface.setControlEnabled(true);QSignalSpy inputs(&surface,&deskflow::gui::VideoSurface::inputRequested);
    QKeyEvent event(QEvent::KeyPress,key,Qt::NoModifier);QApplication::sendEvent(&surface,&event);QCOMPARE(inputs.isEmpty()?0:inputs.first().first().toJsonObject()["code"].toInt(),code);
  }
  void repeatFromViewer(){deskflow::gui::VideoSurface surface;VideoFrame frame;frame.pixels=QImage(640,400,QImage::Format_RGB32);frame.coordinateMappingValid=true;surface.setPresentedFrame(frame);surface.setControlEnabled(true);
    QSignalSpy inputs(&surface,&deskflow::gui::VideoSurface::inputRequested);
    QKeyEvent repeat(QEvent::KeyPress,Qt::Key_A,Qt::NoModifier,"a",true);QApplication::sendEvent(&surface,&repeat);
    QCOMPARE(inputs.size(),1);
  }
  void repeatAtCore(){Fixture f;auto repeat=inputEvent(2);repeat["kind"]="repeat";QVERIFY(f.owner.input(repeat,2));}
  void repeatGuards_data(){QTest::addColumn<bool>("held");QTest::newRow("not-held")<<false;QTest::newRow("release-is-not-repeat")<<true;}
  void repeatGuards(){QFETCH(bool,held);Fixture f;auto repeat=inputEvent(2);repeat["kind"]="repeat";if(held)repeat["down"]=false;else repeat["code"]=66;QVERIFY(!f.owner.input(repeat,2));}
  void releaseHeldButton(){Fixture f;auto button=inputEvent(2);button["kind"]="button";button["code"]=1;button["x"]=100;button["y"]=100;if(!f.owner.input(button,2))qFatal("Held button setup failed");f.owner.revoke();
    QCOMPARE(f.native->events.last(),QJsonObject({{"kind","button"},{"code",1},{"down",false},{"release",true}}));}
  void releaseButtonRetry(){Fixture f;auto button=inputEvent(2);button["kind"]="button";button["code"]=1;button["x"]=100;button["y"]=100;if(!f.owner.input(button,2))qFatal("Held button setup failed");f.native->buttonReleaseWorks=false;f.owner.revoke();f.native->buttonReleaseWorks=true;f.owner.poll(3);QCOMPARE(f.native->events.size(),5);}
  void ordinaryReleaseOrdering(){Fixture f;f.owner.revoke();ordinaryInputLocal=false;bool premature=false;
    finishOrdinaryInput([&]{premature=f.owner.grant(lease,2);});QVERIFY(!premature);QVERIFY(ordinaryInputLocal.load());}
  void releaseRetry(){Fixture f;f.native->working=false;f.owner.revoke();f.native->working=true;f.owner.poll(2);QCOMPARE(f.native->events.size(),3);}
  void releaseBlocksOwnership(){Fixture f;f.native->working=false;f.owner.revoke();QVERIFY(controlOwnsInput.load());f.native->working=true;f.owner.poll(2);}
  void revokeDuringVerification(){Fixture f;int verification=0;f.native->verifying=[&]{if(++verification==2)f.owner.revoke();};auto next=inputEvent(2);next["code"]=66;f.owner.input(next,2);QCOMPARE(f.native->events.size(),2);}
  void revokeDuringInjection_data(){QTest::addColumn<bool>("button");QTest::newRow("key")<<false;QTest::newRow("button")<<true;}
  void revokeDuringInjection(){QFETCH(bool,button);Fixture f;auto next=inputEvent(2);next["code"]=button?1:66;if(button){next["kind"]="button";next["x"]=100;next["y"]=100;}
    bool interrupted=false;f.native->injecting=[&](const QJsonObject &event){if(event["down"].toBool() && !std::exchange(interrupted,true))f.owner.revoke();};f.owner.input(next,2);
    QCOMPARE(f.native->events.last()["code"],next["code"]);
    QCOMPARE(f.native->events.last()["down"],QJsonValue(false));
  }
  void shortcutRelease_data(){QTest::addColumn<int>("key");QTest::newRow("fullscreen")<<int(Qt::Key_F11);QTest::newRow("playback")<<int(Qt::CTRL|Qt::Key_Space);}
  void shortcutRelease(){QFETCH(int,key);ViewerFixture f;QSignalSpy changed(&f.controller,&deskflow::gui::SenderController::controlChanged);
    QShortcut *shortcut=nullptr;for(auto candidate:f.viewer->findChildren<QShortcut *>())if(candidate->key()==QKeySequence(key))shortcut=candidate;
    if(!shortcut || !QMetaObject::invokeMethod(shortcut,"activated",Qt::DirectConnection))qFatal("Owned shortcut fixture missing");
    QVERIFY(QTest::qWaitFor([&]{return !changed.isEmpty() && !changed.last().first().toBool();},1000));
  }
  void revokedStatus(){ViewerFixture f;QSignalSpy changed(&f.controller,&deskflow::gui::SenderController::controlChanged);f.controller.revokeControl();
    if(!QTest::qWaitFor([&]{return !changed.isEmpty();},1000))qFatal("Revoke fixture delivery failed");
    const auto path=qEnvironmentVariable("STREAMING_CONTROL_STATUS_PROOF");if(!path.isEmpty() && !f.viewer->grab().save(path))qFatal("Control status screenshot failed");
    QVERIFY(!f.controller.status().contains("interactive control granted"));
  }
};
QTEST_MAIN(StreamingControlLifecycleTests)
#include "StreamingControlLifecycleTests.moc"
