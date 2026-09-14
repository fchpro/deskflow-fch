// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Capture.h"
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
namespace deskflow::streaming {
// One pipeline per capture, with an appsink bounded to one raw frame.
// All control/pull methods run on the owning media worker. GStreamer owns streaming threads.
class GstCapturePipeline {
public:
  ~GstCapturePipeline();
  bool start(GstElement *source, int fps, QString &error); // takes source ownership even on failure
  void stop();
  std::optional<VideoFrame> pull(QString &error);
  static bool initialize(QString &error);
private:
  GstElement *m_pipeline = nullptr;
  GstAppSink *m_sink = nullptr;
};
} // namespace deskflow::streaming
