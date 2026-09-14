// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "streaming/GstCapturePipeline.h"
#include <QDir>
#include <QTest>
using namespace deskflow::streaming;
namespace {
VideoFrame realFrame()
{
  QString error;
  if (!GstCapturePipeline::initialize(error)) qFatal("GStreamer setup failed: %s", qPrintable(error));
  auto *source = gst_element_factory_make("videotestsrc", nullptr);
  if (!source) qFatal("The real GStreamer test-source plugin is missing");
  g_object_set(source, "is-live", TRUE, "pattern", 4, nullptr); // real red raw-video producer
  GstCapturePipeline pipeline;
  if (!pipeline.start(source, 30, error)) qFatal("Pipeline setup failed: %s", qPrintable(error));
  std::optional<VideoFrame> frame;
  if (!QTest::qWaitFor([&] { frame = pipeline.pull(error); return frame.has_value() || !error.isEmpty(); }, 3000) ||
      !frame || !error.isEmpty()) qFatal("No real captured test-source frame: %s", qPrintable(error));
  pipeline.stop();
  return std::move(*frame);
}
}
class StreamingCapturePipelineTests : public QObject {
  Q_OBJECT
private Q_SLOTS:
  void decodedPixelsSurvivePipelineStop()
  {
    const auto frame = realFrame();
    QCOMPARE(frame.pixels.pixelColor(20, 20), QColor(Qt::red));
    const auto proof = qEnvironmentVariable("DESKFLOW_CAPTURE_PIXEL_PROOF");
    if (!proof.isEmpty()) {
      if (!frame.pixels.save(proof)) qFatal("Could not save decoded pixel proof");
      qInfo().noquote() << "Real GStreamer red test-source frame saved:" << proof
                       << "capture-ns" << frame.captureTimeNs << "media-ns" << frame.mediaTimeNs;
    }
  }
  void nativeTimestampIsPreserved()
  {
    const auto frame = realFrame();
    QVERIFY(frame.captureTimeNs > 0 && frame.mediaTimeNs >= 0);
  }
};
QTEST_GUILESS_MAIN(StreamingCapturePipelineTests)
#include "StreamingCapturePipelineTests.moc"
