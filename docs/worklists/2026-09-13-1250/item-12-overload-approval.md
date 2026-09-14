# Protected overload test proposal — NOT APPLIED

Status: approval required. Production now terminates a post-first-frame decode stall with a distinct actionable error after3s of pending authenticated video progress. Existing3.5s observation can end before that deadline; original-batch20-04/07/12 retain2/7/3 decoded blue-only frames and failure.

## Exact requested approval

MANUAL STEP: Approve only the diff below to `src/unittests/streaming/StreamingMediaTests.cpp`. Keep every other existing master test, first-frame timeout branch, normal successful frame/color/audio/metadata check and bilateral cleanup requirement unchanged. After approval, run faithful before/after and independent insurance for each added condition.

- Overload allows a separate10s negotiation deadline followed by8s observation and2s endpoint cleanup bound. The outer real-network wait is23s (10+8+2 plus3s bounded process exit); other scenarios retain their original deadlines.
- Extend only overload observation from3.5s to8s:3s first-frame budget +3s post-first-frame decoding budget +2s documented cleanup budget. Other scenarios stay1.5s. This remains a bounded observation window, not a preset performance claim.
- Accept only the new explicit decode-stall failure when prior decoded red/blue evidence exists, the receiver exits2, the sender exits0 after propagated Stop, and the technical progress-deadline diagnostic exists.
- Preserve the original first-frame-timeout branch and successful-stream minimum frame count/both colors. Preserve authenticated signaling rejection and both inactive-resource logs.
- The new failure path is not classified as first-frame timeout. Mere low FPS, partial frames or arbitrary backend errors are not accepted.

```diff
--- a/src/unittests/streaming/StreamingMediaTests.cpp
+++ b/src/unittests/streaming/StreamingMediaTests.cpp
@@ -66,7 +66,7 @@
   bool ready=false;int sequence=0; qint64 lastVideo=-40,lastAudio=-20,connected=-1;bool red=false,blue=false,sawNewEpoch=false,sawResize=false;double energy=0;int decoded=0;
   std::vector<double> latency;quint64 lastSequence=0;
   QObject::connect(&feed,&QTimer::timeout,&app,[&]{
-    const auto ms=clock.elapsed();if(ms>9000){qWarning()<<"Endpoint deadline";app.exit(6);return;}
+    const auto ms=clock.elapsed();const bool deadline=overload?(connected<0?ms>10000:ms-connected>10000):ms>9000;if(deadline){qWarning()<<"Endpoint deadline";app.exit(6);return;}
     if(!transport.active())return;
     if(sending&&!filePath.isEmpty()){
       if(!fileStarted){fileStarted=true;if(!file.open(filePath,session,source,audio)){qWarning()<<file.error();app.exit(9);return;}}
@@ -98,7 +98,7 @@
     }
     const auto stats=transport.statistics();if(stats.connected&&connected<0)connected=ms;
     if(!ready&&stats.connected&&(sending?stats.sentPackets>0:decoded>0)){ready=true;socket.write(FrameReader::encode(message("Ready",{{"session",session},{"source",source}})));}
-    if(connected>=0&&ms-connected>=(overload?3500:1500)){
+    if(connected>=0&&ms-connected>=(overload?8000:1500)){
       qInfo()<<"Endpoint result"<<(sending?"sender":"receiver")<<"epoch2"<<sawNewEpoch<<"resized"<<sawResize<<"decoded"<<decoded<<"red"<<red<<"blue"<<blue<<"audioEnergy"<<energy<<"packets"<<stats.sentPackets<<stats.receivedPackets<<"audioPackets"<<stats.audioPackets<<"GCC"<<stats.congestionControl<<"keyframeRequests"<<stats.keyframeRequests<<"startupRetries"<<stats.startupRetries;
       if(!latency.empty()){std::sort(latency.begin(),latency.end());qInfo()<<"Same-host source handoff to decoded consumption ms n/p50/p95/max"<<latency.size()<<latency[latency.size()/2]<<latency[size_t((latency.size()-1)*0.95)]<<latency.back();}
       const bool ok=sending?(stats.sentPackets>0&&stats.startupRetries>=2&&stats.keyframeRequests>=3&&stats.congestionControl&&stats.queuedVideo<=2&&stats.queuedAudioBytes<=23040&&stats.pacerBytes<=37500&&(audio?stats.audioPackets>0:stats.audioPackets==0)):(stats.boundedJitterBuffers==quint64(audio?2:1)&&decoded>=(staticSource?1:bandwidth?5:10)&&red&&(staticSource||blue)&&(!seek||(sawNewEpoch&&sawResize))&&(audio?energy>0.1:energy==0));
@@ -217,14 +217,20 @@
   launch(sending,"sender");if(!QTest::qWaitFor([&]{return clients.size()==1;},3000))qFatal("Sender IPC failed");
   launch(receiving,"receiver");if(!QTest::qWaitFor([&]{return clients.size()==2;},3000))qFatal("Receiver IPC failed");
   if(!broker.dispatch(senderId,generation,message("Offer",{{"session",session},{"to",receiver},{"source",source},{"kind","screen"},{"audio",audio?"system":"off"},{"preset","low"},{"interactive",false}}),clock.elapsed())||!broker.dispatch(receiver,generation,message("Accept",{{"session",session},{"source",source}}),clock.elapsed()))qFatal("Consent failed");
-  const bool ended=QTest::qWaitFor([&]{return sending.state()==QProcess::NotRunning&&receiving.state()==QProcess::NotRunning;},12000);
+  const bool ended=QTest::qWaitFor([&]{return sending.state()==QProcess::NotRunning&&receiving.state()==QProcess::NotRunning;},scenario=="overload"?23000:12000);
   if(!ended){sending.kill();receiving.kill();sending.waitForFinished();receiving.waitForFinished();}
   qInfo()<<"Network emulator encrypted media packets/lost"<<mediaPackets<<lost<<"forwardedBytes"<<forwardedBytes<<"limitBps"<<(scenario=="loss"?0:rate);
   qInfo()<<"Exit codes"<<sending.exitCode()<<receiving.exitCode();
   const auto senderLog=sending.readAllStandardError(),receiverLog=receiving.readAllStandardError();
   qInfo().noquote()<<senderLog<<receiverLog;
   const bool stoppedOverload=scenario=="overload"&&receiving.exitCode()==2&&receiverLog.contains("First decoded video frame timed out")&&senderLog.contains("Peer Stop propagated active false")&&(sending.exitCode()==0||sending.exitCode()==2);
-  QVERIFY2(ended&&!rejected&&senderLog.contains("Media resources released active false")&&receiverLog.contains("Media resources released active false")&&((sending.exitCode()==0&&receiving.exitCode()==0)||stoppedOverload)&&(!network||lost>0),"Real WebRTC endpoints must exchange changing VP8 pixels and selected Opus PCM through broker-validated signaling");
+  // Overload observation covers the3s first-frame deadline,3s decode-progress deadline and2s cleanup budget.
+  const bool stoppedDecodeProgress=scenario=="overload"&&receiving.exitCode()==2&&sending.exitCode()==0&&
+    receiverLog.contains("Video stopped updating. Start a new stream to try again.")&&
+    receiverLog.contains("Authenticated source advanced without decoded progress for3s")&&
+    (receiverLog.contains("Decoded red sequence")||receiverLog.contains("Decoded blue sequence"))&&
+    senderLog.contains("Peer Stop propagated active false");
+  QVERIFY2(ended&&!rejected&&senderLog.contains("Media resources released active false")&&receiverLog.contains("Media resources released active false")&&((sending.exitCode()==0&&receiving.exitCode()==0)||stoppedOverload||stoppedDecodeProgress)&&(!network||lost>0),"Real WebRTC endpoints must exchange changing VP8 pixels and selected Opus PCM through broker-validated signaling");
  }
 };
 int main(int argc,char **argv){QCoreApplication app(argc,argv);if(app.arguments().contains("--endpoint"))return endpoint(app);StreamingMediaTests tests;return QTest::qExec(&tests,argc,argv);}
```

The proposal has not changed the protected file. A separate diagnostic copy may exercise these exact changes; its results never count as the protected test passing.

## Executed proposal-only diagnostic

`temp/proof-of-work/worklist-2026-09-13-1250/item-12/proposal-batch20-01` through `proposal-batch20-20`: fixed20-run copy batch exits0 in every run. All20 exercise the original first-frame-timeout branch; zero successful media outcomes and zero new decode-stall branches. This batch does not validate the added acceptance branch and is not protected-test acceptance. Each directory contains actual Qt and command output; executable `build/item12diagnostics/MediaProposal12.exe`, source `temp/MediaProposal12.cpp`. New recovery behavior is independently covered by the selective-loss regression and20-case insurance manifest.
