// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "FileSource.h"
#include <gst/app/gstappsrc.h>
#include <memory>

namespace deskflow::streaming {
class NativeAudioCapture;
struct AudioBlock {
  QByteArray samples; // owned interleaved 48 kHz stereo F32LE; maximum 100 ms
  QString session, source;
  quint64 timelineEpoch = 0;
  qint64 mediaTimeNs = 0, durationNs = 0, captureTimeNs = 0;
};
AudioBlock fileAudioBlock(const FileAudioBlock &block);
bool validAudioBlock(const AudioBlock &block);
qint64 audioHostTimeNs(); // GStreamer monotonic system clock; same QPC epoch as Windows WGC
inline qint64 audioPlayoutPosition(qint64 runningTimeNs, qint64 marginNs) {
  return qMax(qint64(0), runningTimeNs - marginNs);
}

// Explicit selection; application includes all audio in the selected process tree, not one tab/window.
struct AudioSelection {
  QString scope = "off"; // off, system, application
  QString deviceId; // mandatory system endpoint on Windows/Linux; empty for macOS system-wide SCK audio
  quint32 processId = 0;
  quint64 processBirth = 0; // Windows process creation FILETIME; never trust PID alone
};
struct AudioEndpoint { QString id, name; };
QVector<AudioEndpoint> audioOutputEndpoints(QString &error);
quint64 audioProcessBirth(quint32 pid);

// All operations on the GUI media worker. Source and output lifetimes are mutually exclusive
// in this process, preventing receiver playback from entering a simultaneous sender loopback.
// Caller must already have receiver consent and keep draining takeAudio(). OFF calls stop().
class AudioInput {
public:
  AudioInput();
  AudioInput(const AudioInput &) = delete;
  AudioInput &operator=(const AudioInput &) = delete;
  ~AudioInput();
  bool start(const AudioSelection &, const QString &session, const QString &source, quint64 epoch = 1);
  void stop();
  std::optional<AudioBlock> takeAudio();
  QString error() const { return m_error; }
  bool active() const { return m_pipeline != nullptr; }
  // Native adapters transfer ownership of a real raw-PCM source; not a fallback factory.
  bool startSource(GstElement *ownedSource, const QString &session, const QString &source, quint64 epoch = 1);
private:
  void fail(const QString &);
  GstElement *m_pipeline = nullptr;
  GstAppSink *m_sink = nullptr;
  QString m_session, m_source, m_error;
  quint64 m_epoch = 0;
  void *m_process = nullptr;
  std::unique_ptr<NativeAudioCapture> m_native;
};

// Maps sender media PTS into one receiver graph. Pass the same running-time mapping to video.
// reset() flushes queued audio on seek; identity and epoch must match every push.
// No backend/default-device substitution: native output requires an enumerated endpoint.
class AudioOutput {
public:
  AudioOutput() = default;
  AudioOutput(const AudioOutput &) = delete;
  AudioOutput &operator=(const AudioOutput &) = delete;
  ~AudioOutput();
  bool start(const QString &deviceId, const QString &session, const QString &source, quint64 epoch,
             qint64 mediaOriginNs = 0, qint64 playoutMarginNs = 0);
  bool startSink(GstElement *ownedSink, const QString &session, const QString &source, quint64 epoch,
                 qint64 mediaOriginNs = 0, qint64 playoutMarginNs = 0);
  bool push(const AudioBlock &);
  bool reset(quint64 epoch, qint64 mediaOriginNs);
  bool pause(bool paused);
  bool setVolume(double volume, bool muted);
  void stop();
  QString error(); // also consumes fatal asynchronous sink errors and stops output
  bool active() const { return m_pipeline != nullptr; }
  qint64 runningTimeNs() const;
  qint64 presentationTimeNs() const { return audioPlayoutPosition(runningTimeNs(), m_playoutMargin); }
  qint64 playoutMarginNs() const { return m_playoutMargin; }
  qint64 mediaOriginNs() const;
  quint64 queuedBytes() const;
  qint64 nextMediaTimeNs() const { return m_lastEnd; }
private:
  void fail(const QString &);
  GstElement *m_pipeline = nullptr, *m_volume = nullptr;
  GstAppSrc *m_source = nullptr;
  QString m_session, m_identity, m_error;
  quint64 m_epoch = 0;
  qint64 m_origin = 0, m_lastEnd = -1, m_pausedTime = 0, m_playoutMargin = 0;
  bool m_paused = false;
};
} // namespace deskflow::streaming
