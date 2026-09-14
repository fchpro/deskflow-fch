// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/SessionBroker.h"
#include "common/StreamingInputGate.h"
#include <QTest>
#include <QSignalSpy>
using namespace deskflow::streaming;
class StreamingControlBoundaryTests:public QObject {
  Q_OBJECT
private Q_SLOTS:
  void cleanup(){controlOwnsInput=false;viewerOwnsInput=false;ordinaryInputLocal=true;}
  void privateFocus_data(){QTest::addColumn<QString>("aspect");for(const char *v:{"rejected","reason","recipient","type","count","ownership","media"})QTest::newRow(v)<<QString(v);}
  void privateFocus(){QFETCH(QString,aspect);SessionBroker broker;
    const QString sender(64,'d'),receiver(64,'e'),generation(32,'f'),session(32,'a'),source(32,'b');
    const QJsonObject caps{{"sources",QJsonArray{"window"}},{"receive",true},{"audio",QJsonArray{"off"}},{"control",true}};
    if(!broker.attach({sender,"Sender",generation,QHostAddress::LocalHost,caps}) || !broker.attach({receiver,"Receiver",generation,QHostAddress::LocalHost,caps}) ||
      !broker.dispatch(sender,generation,message("Offer",{{"session",session},{"source",source},{"to",receiver},{"kind","window"},{"audio","off"},{"preset","low"},{"interactive",false}}),0))qFatal("Owned broker fixture failed");
    QSignalSpy delivery(&broker,&SessionBroker::deliver);const QJsonObject identity{{"session",session},{"source",source}};
    auto focus=identity;focus["focused"]=true;const bool accepted=broker.dispatch(receiver,generation,message("ViewerFocus",focus),1);
    const auto event=delivery.isEmpty()?QJsonObject{}:delivery.last()[1].toJsonObject();
    if(aspect=="rejected")QVERIFY(!accepted);
    if(aspect=="reason")QCOMPARE(event["data"].toObject()["reason"],QJsonValue("invalidControlFocus"));
    if(aspect=="recipient")QCOMPARE(delivery.isEmpty()?QString():delivery.last()[0].toString(),receiver);
    if(aspect=="type")QCOMPARE(event["type"],QJsonValue("Error"));
    if(aspect=="count")QCOMPARE(delivery.size(),1);
    if(aspect=="ownership")QVERIFY(!streamingOwnsInput());
    if(aspect=="media")QVERIFY(broker.dispatch(receiver,generation,message("Accept",identity),2));
  }
};
QTEST_GUILESS_MAIN(StreamingControlBoundaryTests)
#include "StreamingControlBoundaryTests.moc"
