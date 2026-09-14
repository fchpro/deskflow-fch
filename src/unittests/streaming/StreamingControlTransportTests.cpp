// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "ControlTlsFixture.h"
#include "base/Log.h"
using namespace control_fixture;
class NativeBoundary final:public NativeControl {
public:
  QList<QJsonObject> calls;
  bool available()const override{return true;}
  bool bind(const QJsonObject &)override{return true;}
  bool verify(const QRect &,const std::optional<QPoint> &,bool)override{return true;}
  bool idle()const override{return true;}
  bool inject(const QJsonObject &e)override{calls.append(e);return true;}
  bool physicalInput()override{return false;}
};
class StreamingControlTransportTests:public QObject {
  Q_OBJECT
  Log m_log;
private Q_SLOTS:
  void viewerFocus(){
    auto native=std::make_unique<NativeBoundary>();auto *sink=native.get();ChannelFixture f(std::move(native),true,false);f.target();f.grant();
    auto focus=f.identity();focus["focused"]=true;f.local.send(message("ViewerFocus",focus));
    QVERIFY(QTest::qWaitFor([]{return viewerOwnsInput.load();},3000));
    f.local.send(message("ControlInput",f.input()));
    QVERIFY(QTest::qWaitFor([&]{return ChannelFixture::has(f.remoteMessages,"ControlInput");},3000));
    QCOMPARE(sink->calls.size(),0);
    focus["focused"]=false;f.local.send(message("ViewerFocus",focus));
    QVERIFY(QTest::qWaitFor([&]{return ChannelFixture::has(f.remoteMessages,"RevokeControl");},3000));
    QVERIFY(!viewerOwnsInput.load());
    f.local.send(message("ControlInput",f.input()));
    QVERIFY(QTest::qWaitFor([&]{return ChannelFixture::has(f.localMessages,"Error");},3000));
  }
  void staleViewerFocus(){
    ChannelFixture f(std::make_unique<NativeBoundary>(),false,false);
    auto focus=f.identity();focus["focused"]=true;focus["source"]=randomId();f.local.send(message("ViewerFocus",focus));
    f.local.send(message("RevokeControl",f.identity()));
    if(!QTest::qWaitFor([&]{return ChannelFixture::has(f.remoteMessages,"RevokeControl");},3000))qFatal("Ordered IPC barrier failed");
    QVERIFY(!viewerOwnsInput.load());
  }
  void modifierMapping_data(){QTest::addColumn<QString>("recipient");QTest::addColumn<int>("key");QTest::addColumn<int>("mapped");
    QTest::newRow("left-control")<<QString("Client")<<162<<91;QTest::newRow("left-super")<<QString("Client")<<91<<162;
    QTest::newRow("right-control")<<QString("Client")<<163<<163;QTest::newRow("other-peer")<<QString("Other")<<162<<162;QTest::newRow("disabled")<<QString()<<162<<162;}
  void modifierMapping(){QFETCH(QString,recipient);QFETCH(int,key);QFETCH(int,mapped);
    ChannelFixture f(std::make_unique<NativeBoundary>(),true,false);f.service->configureControl({},recipient,250);f.target();f.grant();
    auto focus=f.identity();focus["focused"]=true;f.local.send(message("ViewerFocus",focus));
    if(!QTest::qWaitFor([]{return viewerOwnsInput.load();},3000))qFatal("Viewer focus ownership failed");
    auto event=f.input();event["code"]=key;f.local.send(message("ControlInput",event));
    if(!QTest::qWaitFor([&]{return ChannelFixture::has(f.remoteMessages,"ControlInput");},3000))qFatal("Mapped event delivery failed");
    for(const auto &frame:f.remoteMessages)if(frame["type"]=="ControlInput"){
#ifdef Q_OS_WIN
      QCOMPARE(frame["data"].toObject()["code"],QJsonValue(mapped));
#else
      QCOMPARE(frame["data"].toObject()["code"],QJsonValue(key));
#endif
      return;
    }
  }
  void route_data(){QTest::addColumn<QString>("scenario");for(const auto *v:{"authorized","view-only","stale-source","replayed","revoke","disconnect"})QTest::newRow(v)<<QString(v);}
  void route(){QFETCH(QString,scenario);auto native=std::make_unique<NativeBoundary>();auto *sink=native.get();ChannelFixture f(std::move(native),scenario!="view-only");f.target();
    if(scenario!="view-only")f.grant();
    auto event=f.input();if(scenario=="stale-source")event["source"]=randomId();
    f.remote->send(message("ControlInput",event));
    if(scenario=="view-only" || scenario=="stale-source"){
      QVERIFY(QTest::qWaitFor([&]{return ChannelFixture::has(f.remoteMessages,"Error");},3000));
      QCOMPARE(sink->calls.size(),0);return;
    }
    QVERIFY(QTest::qWaitFor([&]{return sink->calls.size()==1;},3000));
    QCOMPARE(sink->calls.first()["code"],event["code"]);
    if(scenario=="authorized")return;
    if(scenario=="replayed"){
      f.remote->send(message("ControlInput",event));
      QVERIFY(QTest::qWaitFor([&]{return ChannelFixture::has(f.remoteMessages,"Error");},3000));
      QCOMPARE(sink->calls.size(),1);return;
    }
    if(scenario=="revoke")f.local.send(message("RevokeControl",f.identity()));else f.remote->close();
    QVERIFY(QTest::qWaitFor([&]{return sink->calls.size()==2;},3000));
    QCOMPARE(sink->calls.last()["down"],QJsonValue(false));
  }
};
QTEST_GUILESS_MAIN(StreamingControlTransportTests)
#include "StreamingControlTransportTests.moc"
