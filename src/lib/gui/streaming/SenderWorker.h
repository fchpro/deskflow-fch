// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "streaming/Capture.h"
#include "streaming/SessionEnvironment.h"
#include "ReceiverTimeline.h"
#include <QJsonObject>
#include <QTimer>
#ifdef DESKFLOW_CAPTURE_GSTREAMER
#include "streaming/MediaTransport.h"
#include <deque>
#endif

namespace deskflow::gui {
class SenderWorker : public QObject {
  Q_OBJECT
  friend class StreamingSenderTests;
  friend class StreamingViewerTests;
  friend class StreamingRecoveryTests;
  friend class StreamingCaptureLifetimeTests;
public:
  ~SenderWorker() override;
  void refresh();
  void connection(bool connected);
  void receive(const QJsonObject &frame);
  void start(const QJsonObject &selection, const QString &requestedSession = {});
  bool sessionMatches(const QString &session) const { return !session.isEmpty() && session==m_session; }
  bool leaseMatches(const QString &lease) const { return !lease.isEmpty() && lease==m_controlLease; }
  void stop(const QString &reason = tr("Stopped"), bool notify = true);
  void acknowledgePreview() { m_previewPending = false; }
  void accept(const QString &endpoint);
  void playbackCommand(const QString &action, qint64 positionMs = 0);
  void volume(double gain, bool muted);
  void grantControl();
  void revokeControl();
  void viewerFocus(bool focused);
  void controlInput(const QJsonObject &event, const streaming::VideoFrame &presented);
  void acknowledgeViewer() { m_viewerPending = false; }
Q_SIGNALS:
  void outgoing(const QJsonObject &frame);
  void inventory(const QJsonObject &value);
  void status(const QString &text, bool active);
  void preview(const QImage &image);
  void incoming(const QJsonObject &offer);
  void viewerFrame(const QImage &image);
  void presentedFrame(const streaming::VideoFrame &frame);
  void controlChanged(bool granted);
  void playback(const QJsonObject &state);
private:
  void publish();
  void observeCapture();
  void begin();
  void poll();
  void capabilities();
  QJsonObject m_inventory, m_selection;
  QString m_session, m_source, m_state;
  bool m_stopping = false, m_previewPending = false;
  bool m_startingCapture = false;
  bool m_receiving = false, m_viewerPending = false;
  bool m_acceptSent = false;
  QJsonObject m_playback;
  QString m_controlLease;
  qint64 m_controlSequence=0;
  bool m_targetSent=false;
  std::unique_ptr<streaming::CaptureDevice> m_capture;
  std::unique_ptr<streaming::SessionEnvironment> m_environment;
  QVector<streaming::CaptureSource> m_sources;
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  std::unique_ptr<streaming::FileSource> m_file;
  std::unique_ptr<streaming::AudioInput> m_audio;
  std::unique_ptr<streaming::MediaTransport> m_media;
  QTimer *m_timer = nullptr;
  qint64 m_anchor = 0;
  bool m_fileStarted = false, m_prerollSent = false;
  quint64 m_audioEpoch = 0, m_audioDropped = 0;
  std::deque<streaming::AudioBlock> m_pendingAudio;
  std::optional<streaming::AudioBlock> m_futureAudio;
  qint64 m_futureAudioSince=0;
  void admitReceivedAudio(streaming::AudioBlock block);
  void releaseFutureAudio();
  void queueAudio(streaming::AudioBlock block);
  void drainAudio(const std::function<bool(const streaming::AudioBlock &)> &submit);
  void receivePoll();
  void reportPlayback();
  std::unique_ptr<streaming::AudioOutput> m_output;
  ReceiverVideoQueue m_videoQueue;
  ReceiverClock m_videoClock;
  quint64 m_receiveEpoch = 0;
  quint64 m_presentedFrames = 0;
  qint64 m_presentedPts = -1;
  bool m_receiverReady = false, m_clockSet = false, m_paused = false, m_audioEpochPending = false;
  qint64 m_origin = 0, m_lastReport = 0;
  double m_gain = 1.0;
  bool m_muted = false;
#endif
};
} // namespace deskflow::gui
