// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SenderWorker.h"
#include "streaming/Protocol.h"
#include <QJsonArray>
#include <cmath>

namespace deskflow::gui {
using namespace streaming;
void SenderWorker::accept(const QString &endpoint)
{
  if (!m_receiving || m_session.isEmpty() || m_state != "awaitingConsent") return;
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (m_selection["audio"] != "off") {
    QString error; bool found = false;
    for (const auto &device : audioOutputEndpoints(error)) found |= device.id == endpoint;
    if (!found) { Q_EMIT status(tr("Select an available audio output endpoint before accepting."), true); return; }
  }
#endif
  m_selection["device"] = endpoint;
  Q_EMIT outgoing(message("Accept", {{"session", m_session}, {"source", m_source}}));
}
void SenderWorker::volume(double gain, bool muted)
{
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (!m_receiving || !std::isfinite(gain) || gain < 0 || gain > 1) return;
  m_gain = gain; m_muted = muted;
  if (m_output && m_output->active() && !m_output->setVolume(gain, muted)) stop(tr("Audio volume change failed."));
#endif
}
void SenderWorker::playbackCommand(const QString &action, qint64 positionMs)
{
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (m_session.isEmpty() || m_selection["kind"] != "file" ||
      !QStringList{"pause", "resume", "seek"}.contains(action)) return;
  if (m_receiving) {
    if (!m_selection["playback"].toBool() || m_playback.isEmpty()) return;
    Q_EMIT outgoing(message("PlaybackCommand", {{"session", m_session}, {"source", m_source},
      {"action", action}, {"positionMs", positionMs}, {"epoch", m_playback["epoch"]}}));
    return;
  }
  if (!m_file || !m_fileStarted || positionMs < 0 || positionMs > m_file->metadata().durationNs / GST_MSECOND) return;
  if (m_file->state() == FilePlaybackState::Seeking) return;
  if (action == "seek" && positionMs * GST_MSECOND >= m_file->metadata().durationNs) return;
  if (!m_file->command(m_session, m_source, action, positionMs * GST_MSECOND)) {
    stop(tr("File playback failed: %1").arg(m_file->error())); return;
  }
  m_lastReport = 0; reportPlayback();
#endif
}
#ifdef DESKFLOW_CAPTURE_GSTREAMER
void SenderWorker::reportPlayback()
{
  if (!m_file || !m_fileStarted || m_file->state() == FilePlaybackState::Seeking) return;
  const auto now = audioHostTimeNs();
  if (m_lastReport && now - m_lastReport < 200 * GST_MSECOND) return;
  m_lastReport = now;
  const auto metadata = m_file->metadata();
  const auto frame = message("PlaybackState", {{"session", m_session}, {"source", m_source},
    {"positionMs", qBound(qint64(0), m_file->positionNs() / GST_MSECOND, metadata.durationNs / GST_MSECOND)},
    {"durationMs", metadata.durationNs / GST_MSECOND}, {"epoch", qint64(m_file->timelineEpoch())},
    {"paused", m_file->state() == FilePlaybackState::Paused}, {"seekable", metadata.seekable}});
  m_playback = frame["data"].toObject(); m_playback["allowed"] = true;
  Q_EMIT playback(m_playback); Q_EMIT outgoing(frame);
}
void SenderWorker::receivePoll()
{
  if(m_futureAudio && audioHostTimeNs()-m_futureAudioSince>=3*GST_SECOND){
    stop(tr("New video timeline did not arrive. Start a new stream."));return;
  }
  if (auto video = m_media->takeVideo()) {
    // File opening audio is paused at the sender until this exact signal.
    if (!m_receiverReady) {
      m_receiverReady = true;
      Q_EMIT outgoing(message("Ready", {{"session", m_session}, {"source", m_source}}));
    }
    if (!m_clockSet || video->timelineEpoch > m_receiveEpoch) {
      m_videoQueue.clear(); m_pendingAudio.clear();
      const bool reset = m_clockSet;
      m_receiveEpoch = video->timelineEpoch; m_origin = video->mediaTimeNs;
      const auto now=audioHostTimeNs(); m_videoClock.reset(m_origin,now); m_videoClock.pause(m_paused,now); m_clockSet = true;
      if (reset && m_output && m_output->active()) {
        m_audioEpochPending = true;
        if (!m_output->pause(true) || !m_output->reset(m_receiveEpoch, m_origin)) {
          stop(tr("Audio timeline reset failed: %1").arg(m_output->error())); return;
        }
      }
    }
    if (video->timelineEpoch == m_receiveEpoch) {
      // Keep the next due frame until its shared audio deadline. Replacing it
      // with each newer future frame starves presentation on a delayed device clock.
      m_videoQueue.push(std::move(*video));
    }
  }
  if (m_output && m_clockSet) {
    releaseFutureAudio();
    if (m_pendingAudio.empty() && !m_futureAudio) if (auto audio = m_media->takeAudio()) {
      admitReceivedAudio(*audio);
    }
    if(m_session.isEmpty() || m_futureAudio)return;
    if (!m_pendingAudio.empty() && !m_output->active()) {
      if (!m_output->start(m_selection["device"].toString(), m_session, m_source, m_receiveEpoch, m_origin, receiverPlayoutMarginNs) ||
          !m_output->setVolume(m_gain, m_muted) || (m_paused && !m_output->pause(true))) {
        stop(tr("Audio output failed: %1").arg(m_output->error())); return;
      }
    }
    if (!m_pendingAudio.empty() && m_output->active() && m_audioEpochPending && !m_paused) {
      // A new epoch starts when PCM can actually be scheduled. Starting on the
      // paused video preroll puts all later samples behind the native sink clock.
      if (!m_output->pause(false)) { stop(m_output->error()); return; }
      m_audioEpochPending = false;
    }
    if (!m_pendingAudio.empty() && m_output->active() &&
        m_pendingAudio.front().mediaTimeNs < m_output->nextMediaTimeNs()) {
      stop(tr("Decoded audio overlaps previously scheduled samples.")); return;
    }
    if (!m_paused && m_output->active()) drainAudio([this](const AudioBlock &block) { return m_output->push(block); });
    if (!m_output->error().isEmpty()) { stop(tr("Audio output failed: %1").arg(m_output->error())); return; }
  }
  const auto position = m_output && m_output->active() ? m_output->mediaOriginNs() + m_output->presentationTimeNs()
    : m_videoClock.position(audioHostTimeNs());
  const auto now=audioHostTimeNs();
  if (now-m_lastReport>=GST_SECOND) {
    m_lastReport=now;
    qInfo()<<"Receiver presentation decoded"<<m_media->statistics().videoDecoded<<"clock PTS"<<position
      <<"presented frames"<<m_presentedFrames<<"presented PTS"<<m_presentedPts
      <<"pending PTS"<<m_videoQueue.nextPts()
      <<"audio queued bytes"<<(m_output ? m_output->queuedBytes() : 0)
      <<"playout margin ns"<<(m_output ? m_output->playoutMarginNs() : 0)
      <<"audio next PTS"<<(m_output ? m_output->nextMediaTimeNs() : -1)
      <<"pending audio PTS"<<(m_pendingAudio.empty() ? -1 : m_pendingAudio.front().mediaTimeNs);
  }
  if (auto present=m_videoQueue.take(position,m_viewerPending)) {
    m_viewerPending = true;
    ++m_presentedFrames; m_presentedPts = present->mediaTimeNs;
    Q_EMIT presentedFrame(*present);
    Q_EMIT viewerFrame(present->pixels);
  }
}
void SenderWorker::admitReceivedAudio(AudioBlock block){
  if(block.timelineEpoch>m_receiveEpoch){
    if(m_futureAudio && block.timelineEpoch<=m_futureAudio->timelineEpoch)return;
    m_futureAudio=std::move(block);m_futureAudioSince=audioHostTimeNs();
    m_pendingAudio.clear();m_videoQueue.clear();m_audioEpochPending=true;
    if(m_output && m_output->active() && !m_output->pause(true))stop(m_output->error());
    return;
  }
  if(!m_futureAudio && block.timelineEpoch==m_receiveEpoch && block.mediaTimeNs>=m_origin && m_pendingAudio.empty())
    m_pendingAudio.push_back(std::move(block));
}
void SenderWorker::releaseFutureAudio(){
  if(!m_futureAudio || m_futureAudio->timelineEpoch>m_receiveEpoch)return;
  auto block=std::move(*m_futureAudio);m_futureAudio.reset();
  if(block.timelineEpoch==m_receiveEpoch && block.mediaTimeNs>=m_origin)m_pendingAudio.push_back(std::move(block));
}
#endif
} // namespace deskflow::gui
