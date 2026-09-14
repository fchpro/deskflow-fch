// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/MediaPacer.h"
#include <QTest>
using namespace deskflow::streaming;
class StreamingPacerTests:public QObject {
 Q_OBJECT
private Q_SLOTS:
 void pacerLimits_data(){QTest::addColumn<QString>("scenario");for(const char *name:{"floor","bytes","packets","expiry","departure","reset","shrink","admission"})QTest::newRow(name)<<QString(name);}
 void pacerLimits(){
   QFETCH(QString,scenario);MediaPacer pacer;bool valid=false;
   if(scenario=="floor")valid=pacer.allows(1500,1000)&&!pacer.allows(1501,1000);
   if(scenario=="bytes"){pacer.admit(24900,2000000,0);valid=pacer.allows(100,2000000)&&!pacer.allows(101,2000000);}
   if(scenario=="packets"){for(int i=0;i<512;++i)if(!pacer.admit(1,2000000,0))qFatal("Packet boundary setup failed");valid=!pacer.allows(1,2000000);}
   if(scenario=="expiry"){pacer.admit(1,2000000,500);valid=!pacer.expired(100000500)&&pacer.expired(100000501);}
   if(scenario=="departure"){pacer.admit(100,2000000,0);pacer.admit(200,2000000,50000000);pacer.depart();valid=pacer.queuedBytes()==200&&!pacer.expired(100000001);}
   if(scenario=="reset"){pacer.admit(100,2000000,0);pacer.clear();valid=pacer.queuedBytes()==0&&!pacer.expired(200000000)&&pacer.allows(25000,2000000);}
   if(scenario=="shrink"){pacer.admit(25000,2000000,0);valid=!pacer.allows(1,1000000);}
   if(scenario=="admission")valid=!pacer.admit(25001,2000000,0)&&pacer.queuedBytes()==0;
   QVERIFY2(valid,qPrintable(scenario));
 }
};
QTEST_APPLESS_MAIN(StreamingPacerTests)
#include "StreamingPacerTests.moc"
