// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/DecodeProgress.h"
#include <QTest>
using namespace deskflow::streaming;
class StreamingDecodeProgressPolicyTests:public QObject {
 Q_OBJECT
private Q_SLOTS:
 void policy_data(){
  QTest::addColumn<QString>("scenario");QTest::addColumn<bool>("expected");
  for(const auto *name:{"initial","paused","static","decoded-progress","stale-epoch","clear-pending","clear-decoded","deadline-boundary"})QTest::newRow(name)<<QString(name)<<false;
  for(const auto *name:{"expired","next-epoch","pending-newer","newer-packets","zero"})QTest::newRow(name)<<QString(name)<<true;
 }
 void policy(){
  QFETCH(QString,scenario);QFETCH(bool,expected);DecodeProgress progress;int64_t now=4000000000LL;
  if(scenario=="initial")progress.received(1,1,100);
  else if(scenario=="zero"){progress.decoded(0,0,0);progress.received(0,1,100);}
  else if(scenario=="stale-epoch"){progress.decoded(2,1,0);progress.received(1,100,100);}
  else if(scenario=="next-epoch"){progress.decoded(1,100,0);progress.received(2,0,100);}
  else {
    progress.decoded(1,1,0);
    if(scenario=="clear-decoded"){progress.clear();progress.received(1,2,100);}
    else if(scenario=="static")progress.received(1,1,100);
    else if(scenario!="paused"){
      progress.received(1,2,100);
      if(scenario=="decoded-progress")progress.decoded(1,2,200);
      if(scenario=="pending-newer"){progress.received(1,3,150);progress.decoded(1,2,200);}
      if(scenario=="newer-packets")progress.received(1,3,2000000000LL);
      if(scenario=="clear-pending")progress.clear();
      if(scenario=="deadline-boundary")now=3000000100LL;
    }
  }
  QCOMPARE(progress.stalled(now),expected);
 }
};
QTEST_GUILESS_MAIN(StreamingDecodeProgressPolicyTests)
#include "StreamingDecodeProgressPolicyTests.moc"
