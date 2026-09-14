// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "GstCapturePipeline.h"
#include "GstRuntime.h"
#include <gst/video/video.h>
#include <chrono>
#include <mutex>
namespace deskflow::streaming {
bool GstCapturePipeline::initialize(QString &error)
{
  static std::mutex initialization;
  const std::lock_guard lock(initialization);
  if (gst_is_initialized()) return true;
  if (!configureGstRuntime(error)) return false;
  GError *failure = nullptr;
  if (!gst_init_check(nullptr, nullptr, &failure)) {
    error = QString::fromUtf8(failure ? failure->message : "GStreamer initialization failed");
    g_clear_error(&failure);
    return false;
  }
  return true;
}
GstCapturePipeline::~GstCapturePipeline() { stop(); }
void GstCapturePipeline::stop()
{
  if (m_pipeline) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
  }
  m_pipeline = nullptr;
  m_sink = nullptr;
}
bool GstCapturePipeline::start(GstElement *source, int fps, QString &error)
{
  stop();
  auto *convert = gst_element_factory_make("videoconvert", nullptr);
  auto *sink = gst_element_factory_make("appsink", nullptr);
  auto *filter = gst_element_factory_make("capsfilter", nullptr);
  if (!source || !convert || !sink || !filter) {
    for (auto *element : {source, convert, sink, filter})
      if (element) gst_object_unref(element);
    error = "Required capture/conversion/appsink plugin is missing";
    return false;
  }
  m_pipeline = gst_pipeline_new(nullptr);
  auto *rate = gst_caps_new_simple("video/x-raw", "framerate", GST_TYPE_FRACTION, fps, 1, nullptr);
  g_object_set(filter, "caps", rate, nullptr);
  gst_caps_unref(rate);
  auto *caps = gst_caps_from_string("video/x-raw,format=BGRA,pixel-aspect-ratio=1/1");
  g_object_set(sink, "caps", caps, "max-buffers", 1U, "drop", TRUE, "sync", FALSE,
               "wait-on-eos", FALSE, "enable-last-sample", FALSE, nullptr);
  gst_caps_unref(caps);
  gst_bin_add_many(GST_BIN(m_pipeline), source, filter, convert, sink, nullptr);
  if (!gst_element_link_many(source, filter, convert, sink, nullptr)) {
    error = "Capture pipeline cannot negotiate system-memory video";
    stop();
    return false;
  }
  m_sink = GST_APP_SINK(sink);
  if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
    error = "Capture backend refused to start";
    stop();
    return false;
  }
  return true;
}
std::optional<VideoFrame> GstCapturePipeline::pull(QString &error)
{
  if (!m_pipeline) return {};
  auto *bus = gst_element_get_bus(m_pipeline);
  auto *message = gst_bus_pop_filtered(bus, GstMessageType(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
  gst_object_unref(bus);
  if (message) {
    if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError *failure = nullptr;
      gchar *debug = nullptr;
      gst_message_parse_error(message, &failure, &debug);
      error = "Capture backend failed. Check source permission and installed capture components.";
      g_clear_error(&failure);
      g_free(debug);
    } else error = "Capture source ended";
    gst_message_unref(message);
    return {};
  }
  auto *sample = gst_app_sink_try_pull_sample(m_sink, 0);
  if (!sample) return {};
  GstVideoInfo info;
  GstVideoFrame mapped;
  std::optional<VideoFrame> result;
  auto *buffer = gst_sample_get_buffer(sample);
  if (!GST_BUFFER_PTS_IS_VALID(buffer)) {
    error = "Capture source did not provide a media timestamp";
    gst_sample_unref(sample);
    return {};
  }
  if (gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
      GST_VIDEO_INFO_FORMAT(&info) == GST_VIDEO_FORMAT_BGRA &&
      gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ)) {
    VideoFrame frame;
    frame.pixels = QImage(static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0)),
                         GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                         GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0), QImage::Format_ARGB32).copy();
    frame.mediaTimeNs = qint64(GST_BUFFER_PTS(buffer));
    // GstSystemClock and GstBuffer PTS use a monotonic clock; timestamp at source when provided.
    frame.captureTimeNs = qint64(gst_element_get_base_time(m_pipeline)) + frame.mediaTimeNs;
    gst_video_frame_unmap(&mapped);
    result = std::move(frame);
  } else error = "Capture returned an unsupported raw pixel format";
  gst_sample_unref(sample);
  return result;
}
} // namespace deskflow::streaming
