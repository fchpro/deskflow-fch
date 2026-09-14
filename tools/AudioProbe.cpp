// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
// Explicit, opt-in hardware test. Emits only its own quiet two-second 440 Hz tone.
#include "streaming/Audio.h"
#include "streaming/GstCapturePipeline.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QTest>
#include <cmath>
#include <cstdio>
using namespace deskflow::streaming;
const QString session(32, 'a'), source(32, 'b');
int main(int argc, char **argv)
{
  QCoreApplication app(argc, argv);
  QString error;
  if (!GstCapturePipeline::initialize(error)) return 2;
  const auto endpoints = audioOutputEndpoints(error);
  if (endpoints.isEmpty()) { fprintf(stderr, "No output endpoints: %s\n", qPrintable(error)); return 3; }
  if (app.arguments().contains("--tone")) {
    AudioOutput out;
    if (!out.start(app.arguments().last(), session, source, 1)) {
      fprintf(stderr, "Output error: %s\n", qPrintable(out.error())); return 4;
    }
    printf("READY\n"); fflush(stdout);
    if (getchar() != 'G') return 5;
    QElapsedTimer timer; timer.start();
    int frames = 0;
    const auto origin = out.runningTimeNs() + 50 * GST_MSECOND;
    bool failed = false;
    (void)QTest::qWaitFor([&] {
      if (frames >= 200) return timer.elapsed() >= 2200;
      while (frames < 200 && out.runningTimeNs() + 50 * GST_MSECOND >= origin + frames * 10 * GST_MSECOND) {
      AudioBlock block{QByteArray(480 * 8, Qt::Uninitialized), session, source, 1,
                       qint64(origin + frames * 10 * GST_MSECOND), 10 * GST_MSECOND, 0};
      auto *pcm = reinterpret_cast<float *>(block.samples.data());
      for (int i = 0; i < 480; ++i) pcm[2 * i] = pcm[2 * i + 1] =
        float(0.02 * std::sin((frames * 480 + i) * 440.0 * 2 * 3.141592653589793 / 48000));
      if (out.push(block)) ++frames; else break;
      if (!out.error().isEmpty()) { fprintf(stderr, "Sink failure: %s\n", qPrintable(out.error())); failed = true; return true; }
      }
      return false;
    }, 3000);
    printf("OUTPUT frames=%d endpoint=%s\n", frames, qPrintable(app.arguments().last()));
    return failed || frames != 200 ? 6 : 0;
  }
  auto endpoint = endpoints.first();
  if (app.arguments().size() == 2)
    for (const auto &entry : endpoints) if (entry.id == app.arguments().last()) endpoint = entry;
  printf("Selected explicit endpoint=%s name=%s\n", qPrintable(endpoint.id), qPrintable(endpoint.name));
  QProcess child;
  child.setProcessChannelMode(QProcess::SeparateChannels);
  child.start(app.applicationFilePath(), {"--tone", endpoint.id});
  QByteArray response;
  if (!QTest::qWaitFor([&] { response += child.readAllStandardOutput(); return response.contains("READY") || child.state() == QProcess::NotRunning; }, 3000) || !response.contains("READY")) {
    fprintf(stderr, "Tone child failed: %s %s\n", response.constData(), child.readAllStandardError().constData()); return 7;
  }
  AudioInput capture;
  const quint32 pid = quint32(child.processId());
  if (!capture.start({"application", {}, pid, audioProcessBirth(pid)}, session, source)) {
    fprintf(stderr, "Process capture failed: %s\n", qPrintable(capture.error())); child.write("X"); child.waitForFinished(3000); return 8;
  }
  child.write("G");
  QByteArray pcm;
  qint64 firstPts = -1, lastPts = 0, firstCapture = -1, lastCapture = 0;
  int blocks = 0;
  (void)QTest::qWaitFor([&] {
    while (auto block = capture.takeAudio()) {
      if (firstPts < 0) { firstPts = block->mediaTimeNs; firstCapture = block->captureTimeNs; }
      lastPts = block->mediaTimeNs; lastCapture = block->captureTimeNs;
      pcm += block->samples; ++blocks;
    }
    return child.state() == QProcess::NotRunning || !capture.error().isEmpty();
  }, 4000); // actual process activation and two seconds of device playback
  child.waitForFinished(1000);
  capture.takeAudio();
  double energy = 0, sine = 0, cosine = 0;
  const auto *samples = reinterpret_cast<const float *>(pcm.constData());
  const int count = int(pcm.size() / 8);
  for (int i = 0; i < count; ++i) {
    const double value = samples[2 * i]; energy += value * value;
    sine += value * std::sin(i * 440.0 * 2 * 3.141592653589793 / 48000);
    cosine += value * std::cos(i * 440.0 * 2 * 3.141592653589793 / 48000);
  }
  const double rms = count ? std::sqrt(energy / count) : 0;
  const double peak440 = count ? 2 * std::hypot(sine, cosine) / count : 0;
  const auto proof = qEnvironmentVariable("DESKFLOW_AUDIO_PROOF");
  if (!proof.isEmpty()) { QFile file(proof); if (file.open(QIODevice::WriteOnly)) file.write(pcm); }
  printf("%s", child.readAllStandardOutput().constData());
  printf("CHILD_ERROR %s\n", child.readAllStandardError().constData());
  printf("CAPTURE blocks=%d frames=%d RMS=%.8f amplitude440Hz=%.8f firstPTS=%lld lastPTS=%lld spanNs=%lld clockMappingDeltaNs=%lld\n",
    blocks, count, rms, peak440, firstPts, lastPts, lastPts - firstPts, (lastCapture - firstCapture) - (lastPts - firstPts));
  printf("PROCESS_EXIT stopped=%d reason=%s childExit=%d\n", !capture.active(), qPrintable(capture.error()), child.exitCode());
  const bool passed = blocks > 100 && rms > 0.001 && peak440 > 0.001 && !capture.active() && child.exitCode() == 0;
  printf("%s: digital process-loopback of owned tone; physical audibility and cross-device A/V sync unverified\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 9;
}
