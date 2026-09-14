// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Audio.h"
#include <QHostAddress>
#include <QJsonObject>
#include <QObject>
#include <memory>

namespace deskflow::gui { class StreamingRecoveryTests; }
namespace deskflow::streaming {
struct MediaPreset { int width, height, fps, bitrate, maximum; };
std::optional<MediaPreset> mediaPreset(const QString &name);
struct MediaConfiguration {
  QString session, source, preset = "balanced";
  QHostAddress localAddress, peerAddress; // from authenticated core/broker state only
  bool sender = false, audio = false;
};
struct MediaStatistics {
  quint64 videoSubmitted = 0, videoDecoded = 0, audioDecoded = 0, rawDropped = 0;
  quint64 sentPackets = 0, receivedPackets = 0, sentBytes = 0, receivedBytes = 0;
  quint64 audioPackets = 0, queuedVideo = 0, queuedAudioBytes = 0, pacerBytes = 0;
  quint64 boundedJitterBuffers = 0, keyframeRequests = 0, startupRetries = 0, staticRepeats = 0;
  quint32 estimatedBitrate = 0;
  bool connected = false, congestionControl = false;
};
// Construct/use/destroy on one dedicated Qt media worker. GStreamer owns codec/ICE
// streaming threads. Signals contain bounded signaling only; poll the latest pixels.
// Start ONLY after broker State=negotiating (explicit receiver consent).
// One transport per process; source/output adapters must stop before destroying it.
class MediaTransport : public QObject {
  Q_OBJECT
public:
  explicit MediaTransport(QObject *parent = nullptr);
  ~MediaTransport();
  bool start(const MediaConfiguration &);
  void stop();
  bool receive(const QJsonObject &authenticatedBrokerMessage);
  bool pushVideo(const VideoFrame &);
  bool pushAudio(const AudioBlock &);
  std::optional<VideoFrame> takeVideo();
  std::optional<AudioBlock> takeAudio();
  MediaStatistics statistics() const;
  QString error() const;
  bool active() const;
  void poll(); // worker timer: bus/errors/negotiation timeout/congestion/telemetry
Q_SIGNALS:
  void signaling(const QJsonObject &);
  void failed(const QString &);
  void connectedChanged(bool);
private:
  friend class deskflow::gui::StreamingRecoveryTests;
  bool consumeBusError(GstBus *bus);
  struct Impl;
  std::unique_ptr<Impl> d;
};
} // namespace deskflow::streaming
