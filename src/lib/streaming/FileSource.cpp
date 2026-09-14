// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "FileSource.h"
#include "GstCapturePipeline.h"
#include <QFile>
#include <QFileInfo>
#include <gst/video/video.h>

namespace deskflow::streaming {
FileSource::FileSource(QObject *parent) : QObject(parent)
{
  m_timer.setInterval(5);
  connect(&m_timer, &QTimer::timeout, this, &FileSource::poll);
}
FileSource::~FileSource() { release(); }
void FileSource::setState(FilePlaybackState state)
{
  m_state = state;
  Q_EMIT stateChanged();
}
void FileSource::release()
{
  m_preroll.reset();
  m_timer.stop();
  if (m_pipeline) {
    gst_element_set_state(m_pipeline, GST_STATE_NULL);
    gst_object_unref(m_pipeline);
  }
  m_pipeline = nullptr;
  m_videoSink = m_audioSink = nullptr;
  m_videoConvert = m_audioConvert = nullptr;
}
void FileSource::stop()
{
  release();
  m_session.clear();
  m_source.clear();
  m_position = 0;
  m_metadata = {};
  m_error.clear();
  setState(FilePlaybackState::Stopped);
}
void FileSource::fail(const QString &reason)
{
  release();
  m_error = reason;
  setState(FilePlaybackState::Error);
}
void FileSource::noMorePads(GstElement *, gpointer data)
{
  static_cast<FileSource *>(data)->m_discovered = true;
}
gint FileSource::selectDecoder(GstElement *, GstPad *, GstCaps *, GstElementFactory *factory, gpointer)
{
  // No adaptive/network demuxers or URI handlers may be instantiated, even for disguised local files.
  const QString name = QString::fromUtf8(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory)));
  const QStringList allowed{"matroskademux", "qtdemux", "vp8dec", "vp9dec", "avdec_h264", "h264parse",
                            "aacparse", "avdec_aac", "opusdec", "vorbisdec", "opusparse", "vorbisparse"};
  return allowed.contains(name) ? 0 : 2; // GST_AUTOPLUG_SELECT_TRY / SKIP
}
void FileSource::unknownType(GstElement *, GstPad *, GstCaps *caps, gpointer data)
{
  auto &self = *static_cast<FileSource *>(data);
  const auto *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  if (g_str_has_prefix(name, "video/") || (self.m_audioEnabled && g_str_has_prefix(name, "audio/")))
    self.m_linkError = true;
}
void FileSource::padAdded(GstElement *, GstPad *pad, gpointer data)
{
  auto &self = *static_cast<FileSource *>(data);
  auto *caps = gst_pad_get_current_caps(pad);
  if (!caps) { self.m_linkError = true; return; }
  const auto *name = gst_structure_get_name(gst_caps_get_structure(caps, 0));
  GstElement *target = nullptr;
  if (g_str_has_prefix(name, "video/x-raw") && !self.m_hasVideo.exchange(true))
    target = self.m_videoConvert;
  else if (g_str_has_prefix(name, "audio/x-raw") && !self.m_hasAudio.exchange(true) && self.m_audioEnabled)
    target = self.m_audioConvert;
  if (target) {
    auto *sink = gst_element_get_static_pad(target, "sink");
    if (gst_pad_link(pad, sink) != GST_PAD_LINK_OK) self.m_linkError = true;
    gst_object_unref(sink);
  } else {
    // Explicitly discard unselected tracks; never play a second audio/video track locally.
    auto *discard = gst_element_factory_make("fakesink", nullptr);
    if (!discard) self.m_linkError = true;
    else {
      g_object_set(discard, "sync", FALSE, "async", FALSE, nullptr);
      gst_bin_add(GST_BIN(self.m_pipeline), discard);
      auto *sink = gst_element_get_static_pad(discard, "sink");
      if (gst_pad_link(pad, sink) != GST_PAD_LINK_OK) self.m_linkError = true;
      gst_object_unref(sink);
      gst_element_sync_state_with_parent(discard);
    }
  }
  gst_caps_unref(caps);
}
bool FileSource::open(const QString &path, const QString &session, const QString &source, bool audio)
{
  stop();
  if (!validCaptureGeneration(session) || !validCaptureGeneration(source)) {
    fail("Invalid accepted session/source identity"); return false;
  }
  const QFileInfo file(path);
  if (!file.isAbsolute() || path.startsWith("//") || path.startsWith("\\\\")) {
    fail("Select an absolute local file path; network URLs and shares are unsupported"); return false;
  }
  if (!file.exists() || !file.isFile()) {
    fail("Video file is missing or is not a regular file; select an existing video"); return false;
  }
  QFile readable(file.absoluteFilePath());
  if (!readable.open(QIODevice::ReadOnly)) {
    fail("Video file is unreadable; check file permissions and availability"); return false;
  }
  // Restrict containers before decodebin; it cannot create a URI source or follow playlists/redirects.
  const auto header = readable.read(16);
  if (!header.startsWith(QByteArray::fromHex("1a45dfa3")) && header.mid(4, 4) != "ftyp") {
    fail("Unsupported video container; select WebM/Matroska or MP4/MOV with an installed decoder"); return false;
  }
  QString initError;
  if (!GstCapturePipeline::initialize(initError)) { fail(initError); return false; }
  m_session = session; m_source = source; m_audioEnabled = audio;
  m_epoch = 0; m_sequence = 0;
  m_hasVideo = false; m_hasAudio = false; m_discovered = false; m_linkError = false;
  m_metadata.name = file.fileName();
  GError *failure = nullptr;
  m_pipeline = gst_parse_launch(
      "filesrc name=input ! decodebin name=decoder "
      "queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 name=vqueue ! videoconvert ! "
      "video/x-raw,format=BGRA,pixel-aspect-ratio=1/1 ! appsink name=video sync=true async=false "
      "max-buffers=1 drop=true wait-on-eos=false enable-last-sample=false "
      "queue max-size-buffers=4 max-size-bytes=0 max-size-time=0 name=aqueue ! audioconvert ! audioresample ! "
      "audio/x-raw,format=F32LE,rate=48000,channels=2,layout=interleaved ! "
      "appsink name=audio sync=true async=false max-buffers=4 drop=true wait-on-eos=false enable-last-sample=false",
      &failure);
  if (failure || !m_pipeline) {
    g_clear_error(&failure); fail("Required file decoder/conversion/appsink plugin is missing"); return false;
  }
  auto *input = gst_bin_get_by_name(GST_BIN(m_pipeline), "input");
  const auto encodedPath = file.absoluteFilePath().toUtf8();
  g_object_set(input, "location", encodedPath.constData(), nullptr);
  gst_object_unref(input);
  auto *decoder = gst_bin_get_by_name(GST_BIN(m_pipeline), "decoder");
  g_signal_connect(decoder, "pad-added", G_CALLBACK(padAdded), this);
  g_signal_connect(decoder, "no-more-pads", G_CALLBACK(noMorePads), this);
  g_signal_connect(decoder, "autoplug-select", G_CALLBACK(selectDecoder), this);
  g_signal_connect(decoder, "unknown-type", G_CALLBACK(unknownType), this);
  gst_object_unref(decoder);
  m_videoConvert = gst_bin_get_by_name(GST_BIN(m_pipeline), "vqueue");
  m_audioConvert = gst_bin_get_by_name(GST_BIN(m_pipeline), "aqueue");
  m_videoSink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(m_pipeline), "video"));
  m_audioSink = GST_APP_SINK(gst_bin_get_by_name(GST_BIN(m_pipeline), "audio"));
  // The pipeline retains these objects until NULL has joined all streaming callbacks.
  for (auto *element : {m_videoConvert, m_audioConvert, GST_ELEMENT(m_videoSink), GST_ELEMENT(m_audioSink)})
    gst_object_unref(element);
  m_operation.start();
  setState(FilePlaybackState::Opening);
  if (gst_element_set_state(m_pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
    fail("Video decoder could not open the file; check its format and codec installation"); return false;
  }
  m_timer.start();
  return true;
}
void FileSource::poll()
{
  if (!m_pipeline) return;
  auto *bus = gst_element_get_bus(m_pipeline);
  auto *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  gst_object_unref(bus);
  if (message) {
    GError *failure = nullptr;
    gchar *debug = nullptr;
    gst_message_parse_error(message, &failure, &debug);
    // Avoid disclosing a private path from a backend diagnostic to remote status/log consumers.
    const bool resource = failure && failure->domain == GST_RESOURCE_ERROR;
    gst_message_unref(message); g_clear_error(&failure); g_free(debug);
    fail(resource ? "File read failed; check permissions and that the selected file remains available"
                  : "Video decode failed; file may be corrupt or its codec unavailable");
    return;
  }
  if (m_linkError || (m_discovered && !m_hasVideo)) {
    fail("File contains no decodable video track or its raw format cannot be linked"); return;
  }
  if (m_state == FilePlaybackState::Opening || m_state == FilePlaybackState::Seeking) {
    GstState current, pending;
    const auto result = gst_element_get_state(m_pipeline, &current, &pending, 0);
    // async=false sinks avoid an absent-audio preroll deadlock. A real video preroll is the readiness signal.
    auto *sample = gst_app_sink_try_pull_preroll(m_videoSink, 0);
    if (m_discovered && sample && result != GST_STATE_CHANGE_ASYNC) {
      m_preroll = readFrame(sample);
      if (!m_preroll) return;
      gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &m_metadata.durationNs);
      m_metadata.hasAudio = m_hasAudio;
      auto *query = gst_query_new_seeking(GST_FORMAT_TIME);
      gboolean seekable = FALSE;
      if (gst_element_query(m_pipeline, query)) gst_query_parse_seeking(query, nullptr, &seekable, nullptr, nullptr);
      gst_query_unref(query);
      m_metadata.seekable = seekable && m_metadata.durationNs > 0;
      if (m_resumeAfterSeek && m_state == FilePlaybackState::Seeking) {
        gst_element_set_state(m_pipeline, GST_STATE_PLAYING);
        setState(FilePlaybackState::Playing);
      } else setState(FilePlaybackState::Paused);
    } else {
      if (sample) gst_sample_unref(sample);
      if (m_operation.elapsed() > 3000) fail("File decode/seek timed out; check the file and installed codecs");
      return;
    }
  }
  if (!m_pipeline) return;
  gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &m_position);
  if (m_state == FilePlaybackState::Playing && gst_app_sink_is_eos(m_videoSink) &&
      (!m_audioEnabled || !m_hasAudio || gst_app_sink_is_eos(m_audioSink))) {
    m_position = m_metadata.durationNs;
    release();
    setState(FilePlaybackState::Ended);
  }
}
bool FileSource::command(const QString &session, const QString &source, const QString &action, qint64 position)
{
  if (session != m_session || source != m_source || session.isEmpty()) return false;
  if (action == "stop") { stop(); return true; }
  if (!m_pipeline) return false;
  if (action == "pause" && m_state == FilePlaybackState::Playing) {
    if (gst_element_set_state(m_pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) return false;
    gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &m_position);
    setState(FilePlaybackState::Paused); return true;
  }
  if (action == "resume" && m_state == FilePlaybackState::Paused) {
    if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) return false;
    setState(FilePlaybackState::Playing); return true;
  }
  if (action == "seek" && (m_state == FilePlaybackState::Paused || m_state == FilePlaybackState::Playing) &&
      m_metadata.seekable && position >= 0 && position < m_metadata.durationNs) {
    m_resumeAfterSeek = m_state == FilePlaybackState::Playing;
    gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
    ++m_epoch;
    setState(FilePlaybackState::Seeking);
    Q_EMIT timelineReset(m_epoch);
    // Send one seek through the selected linked video path to the common demuxer.
    // A pipeline-wide event also visits the unlinked audio branch for silent/OFF files.
    if (!gst_element_seek_simple(GST_ELEMENT(m_videoSink), GST_FORMAT_TIME,
                                GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_ACCURATE), position)) {
      fail("File does not support this seek; reopen the selected video"); return false;
    }
    m_position = position;
    m_operation.restart();
    return true;
  }
  return false;
}
std::optional<VideoFrame> FileSource::takeFrame()
{
  if (!m_pipeline || m_state != FilePlaybackState::Playing) return {};
  auto *sample = gst_app_sink_try_pull_sample(m_videoSink, 0);
  if (!sample) return {};
  return readFrame(sample);
}
std::optional<VideoFrame> FileSource::takePreroll()
{
  if (m_state != FilePlaybackState::Paused) return {};
  auto frame = std::move(m_preroll); m_preroll.reset(); return frame;
}
std::optional<VideoFrame> FileSource::readFrame(GstSample *sample)
{
  auto *buffer = gst_sample_get_buffer(sample);
  GstVideoInfo info;
  GstVideoFrame mapped;
  std::optional<VideoFrame> result;
  // Decoder/edit-list offsets belong to the segment, not the file media timeline.
  const auto mediaTime = gst_segment_to_stream_time(gst_sample_get_segment(sample), GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
  if (GST_CLOCK_TIME_IS_VALID(mediaTime) && gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) &&
      gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ)) {
    VideoFrame frame;
    frame.pixels = QImage(static_cast<const uchar *>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0)),
                         GST_VIDEO_INFO_WIDTH(&info), GST_VIDEO_INFO_HEIGHT(&info),
                         GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0), QImage::Format_ARGB32).copy();
    frame.session = m_session; frame.source = m_source; frame.sequence = ++m_sequence;
    frame.timelineEpoch = m_epoch;
    frame.mediaTimeNs = qint64(mediaTime);
    frame.captureTimeNs = qint64(gst_util_get_timestamp());
    gst_video_frame_unmap(&mapped);
    result = std::move(frame);
  }
  gst_sample_unref(sample);
  if (!result) fail("Decoded video has no valid pixels or presentation timestamp");
  return result;
}
std::optional<FileAudioBlock> FileSource::takeAudio()
{
  if (!m_pipeline || !m_audioEnabled || m_state != FilePlaybackState::Playing) return {};
  auto *sample = gst_app_sink_try_pull_sample(m_audioSink, 0);
  if (!sample) return {};
  auto *buffer = gst_sample_get_buffer(sample);
  GstMapInfo mapped;
  std::optional<FileAudioBlock> result;
  const auto mediaTime = gst_segment_to_stream_time(gst_sample_get_segment(sample), GST_FORMAT_TIME, GST_BUFFER_PTS(buffer));
  if (GST_CLOCK_TIME_IS_VALID(mediaTime) && gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
    FileAudioBlock block;
    block.samples = QByteArray(reinterpret_cast<const char *>(mapped.data), qsizetype(mapped.size));
    block.session = m_session; block.source = m_source; block.timelineEpoch = m_epoch;
    block.mediaTimeNs = qint64(mediaTime);
    block.durationNs = qint64(mapped.size / 8) * GST_SECOND / 48000;
    gst_buffer_unmap(buffer, &mapped);
    result = std::move(block);
  }
  gst_sample_unref(sample);
  if (!result) fail("Decoded audio has no valid samples or presentation timestamp");
  return result;
}
} // namespace deskflow::streaming
