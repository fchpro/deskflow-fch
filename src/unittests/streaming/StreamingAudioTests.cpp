// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/Audio.h"
#include "streaming/GstCapturePipeline.h"
#include <QTest>
#include <cmath>
#include <chrono>
using namespace deskflow::streaming;
namespace {
const QString session(32, 'a'), source(32, 'b');
AudioBlock tone()
{
  AudioBlock block{QByteArray(480 * 8, Qt::Uninitialized), session, source, 1, 0, 10 * GST_MSECOND, 0};
  auto *pcm = reinterpret_cast<float *>(block.samples.data());
  for (int i = 0; i < 480; ++i) pcm[2 * i] = pcm[2 * i + 1] = float(0.02 * std::sin(i * 440.0 * 2 * 3.141592653589793 / 48000));
  return block;
}
void live(AudioInput &input)
{
  auto *producer = gst_element_factory_make("audiotestsrc", nullptr);
  g_object_set(producer, "is-live", TRUE, "samplesperbuffer", 480, "volume", 0.02, nullptr);
  if (!input.startSource(producer, session, source)) qFatal("Real audio producer failed: %s", qPrintable(input.error()));
}
AudioBlock captured(AudioInput &input)
{
  std::optional<AudioBlock> block;
  if (!QTest::qWaitFor([&] { block = input.takeAudio(); return block.has_value() || !input.error().isEmpty(); }, 3000) || !block)
    qFatal("Real audio pull failed: %s", qPrintable(input.error()));
  return std::move(*block);
}
GstAppSink *output(AudioOutput &output)
{
  auto *sink = gst_element_factory_make("appsink", nullptr);
  g_object_set(sink, "max-buffers", 4U, "drop", TRUE, "wait-on-eos", FALSE, nullptr);
  gst_object_ref(sink);
  if (!output.startSink(sink, session, source, 1)) qFatal("Real PCM output failed");
  return GST_APP_SINK(sink);
}
QByteArray rendered(GstAppSink *sink)
{
  auto *sample = gst_app_sink_try_pull_sample(sink, 3 * GST_SECOND);
  if (!sample) qFatal("No rendered PCM reached real appsink");
  GstMapInfo map;
  gst_buffer_map(gst_sample_get_buffer(sample), &map, GST_MAP_READ);
  QByteArray result(reinterpret_cast<const char *>(map.data), qsizetype(map.size));
  gst_buffer_unmap(gst_sample_get_buffer(sample), &map);
  gst_sample_unref(sample);
  return result;
}
}
class StreamingAudioTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void initTestCase() { QString error; if (!GstCapturePipeline::initialize(error)) qFatal("GStreamer initialization failed"); }
  void livePcm() {
    AudioInput input; live(input); auto block = captured(input);
    input.stop();
    QCOMPARE(block.samples.size(), qsizetype(480 * 8));
    double energy = 0; const auto *pcm = reinterpret_cast<const float *>(block.samples.constData());
    for (int i = 0; i < block.samples.size() / 4; ++i) energy += pcm[i] * pcm[i];
    QVERIFY(energy > 0.01);
    QCOMPARE(block.durationNs, qint64(10 * GST_MSECOND));
    QCOMPARE(block.session, session);
    QCOMPARE(block.source, source);
    QCOMPARE(block.timelineEpoch, quint64(1));
    QVERIFY(block.captureTimeNs > block.mediaTimeNs);
    qInfo() << "Real live PCM bytes" << block.samples.size() << "energy" << energy << "PTS" << block.mediaTimeNs
            << "captureNs" << block.captureTimeNs << "durationNs" << block.durationNs;
  }
  void offStopsCapture() {
    AudioInput input; live(input); captured(input);
    if (!input.start({}, session, source)) qFatal("OFF transition failed");
    QVERIFY(!input.active() && !input.takeAudio());
  }
  void renderPcm() {
    AudioOutput out; auto *sink = output(out); auto block = tone();
    if (!out.push(block)) qFatal("PCM push failed");
    const auto pcm = rendered(sink); gst_object_unref(sink);
    QCOMPARE(pcm, block.samples);
  }
  void muteAndVolume_data() {
    QTest::addColumn<double>("volume"); QTest::addColumn<bool>("mute");
    QTest::newRow("half") << 0.5 << false; QTest::newRow("mute") << 1.0 << true;
  }
  void muteAndVolume() {
    QFETCH(double, volume); QFETCH(bool, mute);
    AudioOutput out; auto *sink = output(out); auto block = tone();
    if (!out.setVolume(volume, mute) || !out.push(block)) qFatal("Volume setup failed");
    const auto pcm = rendered(sink); gst_object_unref(sink);
    const auto *actual = reinterpret_cast<const float *>(pcm.constData());
    const auto *original = reinterpret_cast<const float *>(block.samples.constData());
    double difference = 0;
    for (int i = 0; i < block.samples.size() / 4; ++i) difference += std::abs(actual[i] - original[i] * (mute ? 0 : volume));
    QVERIFY(difference < 0.00001);
  }
  void rejectsStaleInput_data() {
    QTest::addColumn<QString>("fault");
    for (const char *name : {"session", "source", "epoch", "size", "duration", "time", "order"})
      QTest::newRow(name) << QString(name);
  }
  void rejectsStaleInput() {
    QFETCH(QString, fault);
    AudioOutput out; auto *sink = output(out); auto block = tone();
    if (fault == "session") block.session = QString(32, 'c');
    if (fault == "source") block.source = QString(32, 'c');
    if (fault == "epoch") block.timelineEpoch = 2;
    if (fault == "size") block.samples.resize(48000 * 8);
    if (fault == "duration") ++block.durationNs;
    if (fault == "time") block.mediaTimeNs = -1;
    if (fault == "order" && !out.push(block)) qFatal("Initial ordered push failed");
    const bool accepted = out.push(block); gst_object_unref(sink);
    QVERIFY(!accepted);
  }
  void seekFlushesAndChangesEpoch() {
    AudioOutput out; auto *sink = output(out); auto block = tone();
    block.mediaTimeNs = 2 * GST_SECOND;
    if (!out.push(block)) qFatal("Future queued PCM failed");
    if (!out.reset(2, 5 * GST_SECOND)) qFatal("Timeline flush failed");
    block.timelineEpoch = 2; block.mediaTimeNs = 5 * GST_SECOND;
    block.samples.fill(0);
    if (!out.push(block)) qFatal("New epoch PCM failed");
    const auto pcm = rendered(sink); gst_object_unref(sink);
    QCOMPARE(pcm, block.samples);
    QCOMPARE(out.mediaOriginNs(), qint64(5 * GST_SECOND));
  }
  void pauseRejectsAndResumeRenders() {
    AudioOutput out; auto *sink = output(out);
    if (!out.pause(true)) qFatal("Pause failed");
    const bool pausedAccepted = out.push(tone());
    if (!out.pause(false)) qFatal("Resume failed");
    bool resumedAccepted = false;
    (void)QTest::qWaitFor([&] { if (!resumedAccepted) resumedAccepted = out.push(tone()); return resumedAccepted; }, 3000);
    qInfo() << "Pause accepted" << pausedAccepted << "resume accepted" << resumedAccepted << "output error" << out.error();
    gst_object_unref(sink);
    QVERIFY(!pausedAccepted && resumedAccepted);
  }
  void blocksFeedback() {
    AudioOutput out; auto *sink = output(out);
    AudioInput input;
    const bool accepted = input.startSource(gst_element_factory_make("audiotestsrc", nullptr), session, source);
    gst_object_unref(sink);
    QVERIFY(!accepted);
  }
  void staleProcessCannotCapture() {
    AudioInput input;
    const bool accepted = input.start({"application", {}, quint32(QCoreApplication::applicationPid()), 1}, session, source);
    QVERIFY(!accepted);
  }
  void endpointMustBeExplicit() {
    AudioInput input;
    QVERIFY(!input.start({"system", "nonexistent-endpoint"}, session, source));
  }
  void filePcmContract() {
    FileAudioBlock file{tone().samples, session, source, 7, GST_SECOND, 10 * GST_MSECOND};
    const auto block = fileAudioBlock(file);
    QVERIFY(block.samples == file.samples && block.session == file.session && block.source == file.source &&
            block.timelineEpoch == file.timelineEpoch && block.mediaTimeNs == file.mediaTimeNs && block.durationNs == file.durationNs);
  }
  void commonClock() {
    const auto native = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto delta = audioHostTimeNs() - native;
    qInfo() << "GStreamer minus native monotonic clock ns" << delta;
    QVERIFY(std::abs(delta) < GST_MSECOND);
  }
  void pausedClockIsFrozen() {
    AudioOutput out; auto *sink = output(out);
    if (!out.pause(true)) qFatal("Could not pause output clock");
    const auto frozen = out.runningTimeNs();
    QElapsedTimer observation; observation.start();
    // Observe a 20 ms clock interval; this is the measured quantity, not an action-settling sleep.
    (void)QTest::qWaitFor([&] { return observation.elapsed() >= 20; }, 100);
    const auto after = out.runningTimeNs(); gst_object_unref(sink);
    QCOMPARE(after, frozen);
  }
  void outputBackpressure() {
    AudioOutput out; auto *sink = output(out); auto block = tone();
    int accepted = 0;
    for (int i = 0; i < 30; ++i) {
      block.mediaTimeNs = GST_SECOND + i * 10 * GST_MSECOND;
      if (out.push(block)) ++accepted;
    }
    gst_object_unref(sink);
    QVERIFY(accepted < 30 && out.queuedBytes() <= 38400);
  }
  void asynchronousDeviceFailure() {
    AudioOutput out; auto *sink = output(out);
    GError *failure = g_error_new_literal(GST_RESOURCE_ERROR, GST_RESOURCE_ERROR_NOT_FOUND, "Controlled device removed");
    gst_element_post_message(GST_ELEMENT(sink), gst_message_new_error(GST_OBJECT(sink), failure, "owned sink error"));
    g_error_free(failure); gst_object_unref(sink);
    QCOMPARE(out.error(), QString("Controlled device removed"));
    QVERIFY(!out.active());
  }
};
QTEST_GUILESS_MAIN(StreamingAudioTests)
#include "StreamingAudioTests.moc"
