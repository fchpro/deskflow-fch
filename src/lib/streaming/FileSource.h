// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Capture.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QTimer>
#include <gst/app/gstappsink.h>
#include <atomic>

namespace deskflow::streaming {
enum class FilePlaybackState { Stopped, Opening, Paused, Playing, Seeking, Ended, Error };
struct FileMetadata {
  QString name;
  qint64 durationNs = 0;
  bool seekable = false;
  bool hasAudio = false;
};
struct FileAudioBlock {
  QByteArray samples; // owned interleaved F32LE, 48 kHz stereo
  QString session;
  QString source;
  quint64 timelineEpoch = 0;
  qint64 mediaTimeNs = 0;
  qint64 durationNs = 0;
};
// GUI media-worker API; all public calls on its owning Qt thread. No native windows/audio output.
// Open only after receiver consent. Caller forwards epochs/flushes to transport and stops on terminal state.
class FileSource : public QObject {
  Q_OBJECT
public:
  explicit FileSource(QObject *parent = nullptr);
  ~FileSource() override;
  bool open(const QString &path, const QString &session, const QString &source, bool audio);
  bool command(const QString &session, const QString &source, const QString &action, qint64 positionNs = 0);
  void stop();
  FilePlaybackState state() const { return m_state; }
  QString error() const { return m_error; }
  FileMetadata metadata() const { return m_metadata; }
  qint64 positionNs() const { return m_position; }
  quint64 timelineEpoch() const { return m_epoch; }
  std::optional<VideoFrame> takeFrame();
  std::optional<VideoFrame> takePreroll(); // one real paused frame for consented transport negotiation
  std::optional<FileAudioBlock> takeAudio();
Q_SIGNALS:
  void stateChanged();
  void timelineReset(quint64 epoch); // synchronous invalidation before a flushing seek
private:
  static void padAdded(GstElement *, GstPad *, gpointer);
  static void noMorePads(GstElement *, gpointer);
  static gint selectDecoder(GstElement *, GstPad *, GstCaps *, GstElementFactory *, gpointer);
  static void unknownType(GstElement *, GstPad *, GstCaps *, gpointer);
  void poll();
  void release();
  void fail(const QString &reason);
  void setState(FilePlaybackState state);
  std::optional<VideoFrame> readFrame(GstSample *sample);
  std::optional<VideoFrame> m_preroll;
  GstElement *m_pipeline = nullptr;
  GstElement *m_videoConvert = nullptr;
  GstElement *m_audioConvert = nullptr;
  GstAppSink *m_videoSink = nullptr;
  GstAppSink *m_audioSink = nullptr;
  std::atomic_bool m_hasVideo{false}, m_hasAudio{false}, m_discovered{false}, m_linkError{false};
  bool m_audioEnabled = false, m_resumeAfterSeek = false;
  FilePlaybackState m_state = FilePlaybackState::Stopped;
  QString m_session, m_source, m_error;
  FileMetadata m_metadata;
  qint64 m_position = 0;
  quint64 m_epoch = 0, m_sequence = 0;
  QTimer m_timer;
  QElapsedTimer m_operation;
};
} // namespace deskflow::streaming
