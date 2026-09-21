// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SenderWorker.h"
#include "SenderController.h"
#include "streaming/Protocol.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QRegularExpression>
#include <QPointer>
#include <QScopedValueRollback>

namespace deskflow::gui {
using namespace streaming;
SenderWorker::~SenderWorker()
{
  // Native capture destructors can emit statusChanged from stop(). Member
  // teardown must not re-enter this worker through its capture observer.
  if (m_capture) disconnect(m_capture.get(), nullptr, this, nullptr);
}
void SenderWorker::publish() {
  if(m_session.isEmpty()){m_inventory.remove("session");m_inventory.remove("source");m_inventory.remove("state");}
  else {m_inventory["session"]=m_session;m_inventory["source"]=m_source;m_inventory["state"]=m_state;}
  m_inventory["lease"]=m_controlLease;
  m_inventory["interactive"]=!m_session.isEmpty() && m_selection["interactive"].toBool();
  m_inventory["receiving"]=m_receiving;Q_EMIT inventory(m_inventory);
}
void SenderWorker::connection(bool connected)
{
  if (!connected) {
    stop(m_inventory["connected"].toBool() ? tr("Streaming connection lost. Capture stopped.")
      : tr("Start Deskflow in desktop mode with mutually trusted compatible peers."), false);
    m_inventory.remove("id"); m_inventory.remove("address");
    m_inventory["peers"] = QJsonArray{};
  }
  m_inventory["connected"] = connected;
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  m_inventory["built"] = true;
#else
  m_inventory["built"] = false;
#endif
  publish();
  if (connected) {
    Q_EMIT status(tr("Connected. Choose a source and compatible destination."), false);
    capabilities();
  }
}
void SenderWorker::capabilities()
{
  if (!m_inventory["connected"].toBool() || !m_session.isEmpty()) return;
  QJsonArray kinds, audio{"off"};
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  kinds.append("file"); audio.append("file");
  if (m_inventory["systemAudio"].toBool()) audio.append("system");
  for (const auto &source : m_inventory["sources"].toArray()) {
    const auto value = source.toObject();
    if (value["available"].toBool() && !kinds.contains(value["kind"])) kinds.append(value["kind"]);
    if (value["applicationAudio"].toBool() && !audio.contains("application")) audio.append("application");
  }
#endif
  // One owner publishes both roles. Audio scope describes PCM reception as well as capture.
  const bool receiving = m_inventory["built"].toBool();
  if (receiving) audio = QJsonArray{"off", "file", "system", "application"};
  Q_EMIT outgoing(message("Capabilities", {{"sources", kinds}, {"receive", receiving}, {"audio", audio}, {"control", receiving && m_inventory["control"].toBool()}}));
}
void SenderWorker::refresh()
{
  if (m_inventory["busy"].toBool()) return;
  QJsonArray sources, endpoints;
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (!m_capture) {
    m_capture = createCaptureDevice();
    observeCapture();
  }
  m_sources = m_capture->sources();
  for (const auto &source : m_sources) {
    sources.append(QJsonObject{{"id", source.id}, {"kind", source.kind}, {"title", source.title},
      {"available", source.status.state == CaptureState::Available || source.status.state == CaptureState::PermissionRequired},
      {"reason", source.status.reason}, {"applicationAudio", source.audioProcessId != 0 && source.audioProcessBirth != 0},
      {"width", source.physicalGeometry.width()}, {"height", source.physicalGeometry.height()}});
  }
  QString error;
  for (const auto &endpoint : audioOutputEndpoints(error))
    endpoints.append(QJsonObject{{"id", endpoint.id}, {"name", endpoint.name}});
  m_inventory["discoveryError"] = m_capture->status().reason;
  m_inventory["audioError"] = error;
#ifdef Q_OS_MACOS
  m_inventory["endpointRequired"] = false;
  m_inventory["systemAudio"] = true;
#else
  m_inventory["endpointRequired"] = true;
  m_inventory["systemAudio"] = !endpoints.isEmpty();
#endif
#endif
  m_inventory["sources"] = sources; m_inventory["endpoints"] = endpoints;
  publish(); capabilities();
}
void SenderWorker::observeCapture()
{
  connect(m_capture.get(), &CaptureDevice::statusChanged, this, [this] {
    const auto state = m_capture->status();
    if (!m_startingCapture && !m_receiving && m_selection["kind"] != "file" &&
        !m_session.isEmpty() && m_state != "awaitingConsent" && state.state != CaptureState::Available &&
        state.state != CaptureState::Starting && state.state != CaptureState::PermissionRequired)
      stop(state.reason.isEmpty() ? tr("Capture ended. Select the source and start again.") : state.reason);
  });
}
void SenderWorker::start(const QJsonObject &selection, const QString &requestedSession)
{
  const auto error = senderSelectionError(m_inventory, selection);
  if (!error.isEmpty()) { Q_EMIT status(error, !m_session.isEmpty()); return; }
  m_selection = selection;
  m_receiving = false;
  m_session = requestedSession.isEmpty() ? randomId() : requestedSession;
  m_source = selection["kind"] == "file" ? randomId() : selection["source"].toString();
  m_state = "awaitingConsent";
  m_inventory["busy"] = true; publish();
  Q_EMIT status(tr("Connecting to receiver. Capture starts when the receiver is ready."), true);
  QString title = selection["kind"] == "file" ? QFileInfo(selection["path"].toString()).fileName() : QString{};
  for (const auto &source : m_sources) if (source.id == m_source) title = source.title;
  title.remove(QRegularExpression("[\\x00-\\x1f\\x7f]"));
  Q_EMIT outgoing(message("Offer", {{"session", m_session}, {"source", m_source}, {"to", selection["to"]},
    {"kind", selection["kind"]}, {"audio", selection["audio"]}, {"preset", selection["preset"]}, {"interactive", selection["interactive"].toBool()},
    {"title", title.left(255)}, {"playback", selection["kind"] == "file" && selection["playback"].toBool()}}));
}
void SenderWorker::receive(const QJsonObject &frame)
{
  const auto type = frame["type"].toString(); const auto data = frame["data"].toObject();
  if(type=="ControlAvailability") { m_inventory["control"]=data["available"].toBool();publish();capabilities();return; }
  if (type == "Identity") {
    if (!m_session.isEmpty() && (m_inventory["id"] != data["id"] || m_inventory["address"] != data["address"]))
      stop(tr("Authenticated local interface changed."));
    m_inventory["id"] = data["id"]; m_inventory["address"] = data["address"]; publish(); return;
  }
  if (type == "Roster") {
    m_inventory["peers"] = data["peers"]; publish(); return;
  }
  if (type == "Error") {
    if(data["reason"].toString().startsWith("invalidControl")) {
      m_controlLease.clear(); publish(); Q_EMIT controlChanged(false);
      Q_EMIT status(tr("Control rejected. Restore the selected source and release local input before granting again."),!m_session.isEmpty());return;
    }
    if (data["reason"] == "invalidPlaybackCommand") {
      Q_EMIT status(tr("Playback command rejected. Wait for the current timeline before retrying."), !m_session.isEmpty()); return;
    }
    stop(tr("Streaming request failed: %1").arg(data["reason"].toString())); return;
  }
  if (type == "Offer" && m_inventory["built"].toBool() && m_session.isEmpty() && data["to"] == m_inventory["id"]) {
    m_session = data["session"].toString(); m_source = data["source"].toString();
    m_selection = data; m_selection["to"] = data["from"]; m_receiving = true;
    m_state = "awaitingConsent"; m_inventory["busy"] = true; publish();
    QJsonObject offer = data;
    for (const auto &entry : m_inventory["peers"].toArray())
      if (entry.toObject()["id"] == data["from"]) offer["senderName"] = entry.toObject()["name"];
#ifdef DESKFLOW_CAPTURE_GSTREAMER
    m_gain = 1.0; m_muted = false;
    QString error; QJsonArray endpoints;
    for (const auto &endpoint : audioOutputEndpoints(error))
      endpoints.append(QJsonObject{{"id", endpoint.id}, {"name", endpoint.name}, {"default", endpoint.isDefault}});
    offer["endpoints"] = endpoints; offer["audioError"] = error;
#endif
    Q_EMIT status(tr("Incoming stream. Preparing the receiving viewer."), true);
    Q_EMIT incoming(offer); return;
  }
  if (type == "State" && data["receiver"] == m_inventory["id"] && m_session.isEmpty()) {
    m_inventory["busy"] = true; publish(); return;
  }
  if (type == "Stopped" && m_session.isEmpty()) { m_inventory["busy"] = false; publish(); return; }
  if (m_session.isEmpty() || data["session"] != m_session || data["source"] != m_source) return;
  if(type=="GrantControl") {m_controlLease=data["lease"].toString();m_controlSequence=0;publish();Q_EMIT controlChanged(true);Q_EMIT status(m_receiving?tr("Receiving - interactive control granted. Escape releases control."):tr("Streaming - receiver controls this source. Local input or Revoke releases control."),true);return;}
  if(type=="RevokeControl") {m_controlLease.clear();publish();Q_EMIT controlChanged(false);Q_EMIT status(m_receiving?tr("Receiving - viewing only."):tr("Streaming - receiver is viewing only."),true);return;}
  if (type == "Stopped") { stop(tr("Stream ended: %1").arg(data["reason"].toString()), false); return; }
  if (type == "State") {
    if (data[m_receiving ? "receiver" : "sender"] != m_inventory["id"] ||
        data[m_receiving ? "sender" : "receiver"] != m_selection["to"]) {
      stop(tr("Authenticated session identity changed.")); return;
    }
    const auto previous = m_state;
    m_state = data["state"].toString();
    m_inventory["session"] = m_session; m_inventory["state"] = m_state; publish();
    if (m_state == "negotiating" && previous == "awaitingConsent") begin();
    else if (m_state == "streaming") Q_EMIT status(m_receiving ? tr("Receiving — viewing only. Stop ends the session on both computers.")
      : tr("Streaming — viewing only. Stop ends capture on both computers."), true);
    return;
  }
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (type == "PlaybackCommand" && !m_receiving) {
    // Broker validation can precede a queued local seek on this worker.
    if (!m_file || data["epoch"].toInteger(-1) != qint64(m_file->timelineEpoch())) {
      m_lastReport = 0; reportPlayback(); return;
    }
    playbackCommand(data["action"].toString(), data["positionMs"].toInteger()); return;
  }
  if (type == "PlaybackState" && m_receiving) {
    m_playback = data; m_playback["allowed"] = m_selection["playback"].toBool();
    const bool paused = data["paused"].toBool();
    if (paused != m_paused) {
      m_videoClock.pause(paused,audioHostTimeNs());
      m_paused = paused;
      if (m_output && m_output->active() && !m_output->pause(paused || m_audioEpochPending)) { stop(m_output->error()); return; }
    }
    Q_EMIT playback(m_playback); return;
  }
  if (m_media && QStringList{"SdpOffer", "SdpAnswer", "IceCandidate"}.contains(type) && !m_media->receive(frame))
    stop(tr("Media negotiation failed: %1").arg(m_media->error()));
#endif
}
void SenderWorker::begin()
{
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  QHostAddress peerAddress;
  for (const auto &entry : m_inventory["peers"].toArray())
    if (entry.toObject()["id"] == m_selection["to"]) peerAddress = QHostAddress(entry.toObject()["address"].toString());
  const auto preset = mediaPreset(m_selection["preset"].toString());
  if (!preset || peerAddress.isNull()) { stop(tr("The authenticated destination disappeared.")); return; }
  if (!m_environment) m_environment=createSessionEnvironment();
  if (const auto failure=m_environment->start();!failure.isEmpty()) { stop(failure);return; }
  m_targetSent=false;
  m_anchor = audioHostTimeNs();
  m_fileStarted = false; m_prerollSent = false;
  m_receiverReady = false; m_clockSet = false; m_paused = false; m_audioEpochPending = false; m_videoQueue.clear();
  m_presentedFrames = 0; m_presentedPts = -1;
  m_receiveEpoch = 0; m_lastReport = 0;
  m_media = std::make_unique<MediaTransport>();
  connect(m_media.get(), &MediaTransport::signaling, this, &SenderWorker::outgoing);
  // A queued stop avoids deleting the emitting transport while its stack is active.
  connect(m_media.get(), &MediaTransport::failed, this, [this, transport=QPointer<MediaTransport>(m_media.get())](const QString &error) {
    if (!transport || transport != m_media.get()) return;
    stop(tr("Media stopped: %1").arg(error));
  }, Qt::QueuedConnection);
  connect(m_media.get(), &MediaTransport::connectedChanged, this, [this](bool connected) {
    if (connected && !m_receiving && !m_session.isEmpty())
      Q_EMIT outgoing(message("Ready", {{"session", m_session}, {"source", m_source}}));
  });
  if (!m_media->start({m_session, m_source, m_selection["preset"].toString(),
      QHostAddress(m_inventory["address"].toString()), peerAddress, !m_receiving, m_selection["audio"] != "off"})) {
    stop(tr("Cannot start media: %1").arg(m_media->error())); return;
  }
  if (m_receiving) {
    if (m_selection["audio"] != "off") m_output = std::make_unique<AudioOutput>();
  } else if (m_selection["kind"] == "file") {
    m_file = std::make_unique<FileSource>();
    connect(m_file.get(), &FileSource::timelineReset, this, [this] {
      m_pendingAudio.clear(); m_prerollSent = false;
    });
    if (!m_file->open(m_selection["path"].toString(), m_session, m_source, m_selection["audio"] == "file")) {
      stop(tr("Cannot open selected video: %1").arg(m_file->error())); return;
    }
  } else {
    bool started = false;
    {
      // Native start first cancels its previous selection. Those synchronous
      // cleanup notifications do not revoke the newly accepted session.
      QScopedValueRollback starting(m_startingCapture, true);
      started = m_capture && m_capture->start(m_source, m_session, preset->fps);
    }
    if (!started) {
      stop(m_capture ? m_capture->status().reason : tr("Capture source is unavailable.")); return;
    }
    if (m_selection["audio"] != "off") {
      AudioSelection audio; audio.scope = m_selection["audio"].toString(); audio.deviceId = m_selection["device"].toString();
      for (const auto &source : m_sources) if (source.id == m_source) {
        audio.processId = source.audioProcessId; audio.processBirth = source.audioProcessBirth;
      }
      m_audio = std::make_unique<AudioInput>();
      if (!m_audio->start(audio, m_session, m_source, 0)) { stop(m_audio->error()); return; }
    }
  }
  if (!m_timer) {
    m_timer = new QTimer(this); m_timer->setInterval(10);
    connect(m_timer, &QTimer::timeout, this, &SenderWorker::poll);
  }
  m_timer->start();
  Q_EMIT status(m_receiving ? tr("Accepted. Connecting encrypted video…") : tr("Receiver accepted. Starting encrypted media and source preview…"), true);
#endif
}
void SenderWorker::stop(const QString &reason, bool notify)
{
  if (m_stopping) return;
  m_stopping = true;
  m_controlLease.clear();Q_EMIT controlChanged(false);
  const auto session = m_session, source = m_source;
  const bool decline = m_receiving && m_state == "awaitingConsent" && !m_acceptSent;
  m_session.clear(); m_state.clear(); m_acceptSent = false;
  // Release the core-owned input lease before any native/codec teardown can wait.
  if (notify && !session.isEmpty()) Q_EMIT outgoing(message(decline ? "Decline" : "Stop", {{"session", session}, {"source", source}}));
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (m_timer) m_timer->stop();
  m_environment.reset();
  if (m_capture) m_capture->stop();
  if (m_audio) m_audio->stop();
  if (m_file) m_file->stop();
  if (m_media) { disconnect(m_media.get(), nullptr, this, nullptr); m_media->stop(); }
  m_output.reset(); m_videoQueue.clear();
  m_audio.reset(); m_file.reset(); m_media.reset(); m_pendingAudio.clear();m_futureAudio.reset();m_futureAudioSince=0;
#endif
  m_receiving = false;
  m_inventory["busy"] = false; m_inventory.remove("session"); m_inventory.remove("state"); publish();
  Q_EMIT preview(QImage{});
  Q_EMIT viewerFrame(QImage{});
  m_playback = {}; Q_EMIT playback(m_playback);
  Q_EMIT status(reason, false);
  m_stopping = false;
}
#ifdef DESKFLOW_CAPTURE_GSTREAMER
void SenderWorker::queueAudio(AudioBlock block)
{
  if (!validAudioBlock(block)) return;
  if (block.timelineEpoch != m_audioEpoch) { m_pendingAudio.clear(); m_audioEpoch = block.timelineEpoch; }
  // Retain one source block (<=100 ms). poll submits <=20 ms at a time into
  // transport's independent 60-ms queue before reading another source block.
  if (!m_pendingAudio.empty()) { ++m_audioDropped; return; }
  m_pendingAudio.push_back(std::move(block));
}
void SenderWorker::drainAudio(const std::function<bool(const AudioBlock &)> &submit)
{
  while (!m_pendingAudio.empty()) {
    auto &block = m_pendingAudio.front(); AudioBlock part = block;
    part.samples = block.samples.first(qMin(qsizetype(7680), block.samples.size()));
    part.durationNs = part.samples.size() / 8 * GST_SECOND / 48000;
    if (!submit(part)) break;
    block.samples.remove(0, part.samples.size());
    block.mediaTimeNs += part.durationNs;
    if (block.captureTimeNs != 0) block.captureTimeNs += part.durationNs;
    block.durationNs -= part.durationNs;
    if (block.samples.isEmpty()) m_pendingAudio.pop_front();
  }
}
#endif
void SenderWorker::poll()
{
#ifdef DESKFLOW_CAPTURE_GSTREAMER
  if (!m_media || m_session.isEmpty()) return;
  if (m_environment) if (const auto failure=m_environment->poll();!failure.isEmpty()) { stop(failure);return; }
  if (m_receiving) { receivePoll(); return; }
  std::optional<VideoFrame> video;
  if (m_file) {
    if (m_file->state() == FilePlaybackState::Error) { stop(tr("Video file failed: %1").arg(m_file->error())); return; }
    if (m_file->state() == FilePlaybackState::Ended) { stop(tr("Video file ended.")); return; }
    if (!m_prerollSent && m_file->state() == FilePlaybackState::Paused) {
      video = m_file->takePreroll(); m_prerollSent = video.has_value();
    }
    if (!m_fileStarted && m_prerollSent && m_state == "streaming") {
      m_fileStarted = m_file->command(m_session, m_source, "resume");
      if (!m_fileStarted) { stop(tr("Cannot start file playback.")); return; }
    }
    if (!video) video = m_file->takeFrame();
    if (m_pendingAudio.empty()) if (auto block = m_file->takeAudio()) queueAudio(fileAudioBlock(*block));
  } else if (m_capture) {
    video = m_capture->takeFrame();
    const auto state = m_capture->status().state;
    if (state != CaptureState::Available && state != CaptureState::Starting && state != CaptureState::PermissionRequired) {
      stop(m_capture->status().reason); return;
    }
    if (video) { video->mediaTimeNs = video->captureTimeNs - m_anchor; video->timelineEpoch = 0; }
    if (m_audio) {
      if (m_pendingAudio.empty()) if (auto block = m_audio->takeAudio()) {
        block->mediaTimeNs = block->captureTimeNs - m_anchor; queueAudio(*block);
      }
      if (!m_audio->error().isEmpty()) { stop(m_audio->error()); return; }
    }
  }
  if (video) {
    if(m_capture && !m_file && m_inventory["control"].toBool()) {
      if(!m_targetSent) { const auto target=m_capture->controlTarget();if(!target.isEmpty()){Q_EMIT outgoing(message("ControlTarget",target));m_targetSent=true;} }
      if(m_targetSent) Q_EMIT outgoing(message("ControlGeometry",{{"session",video->session},{"source",video->source},
        {"frame",qint64(video->sequence)},{"epoch",qint64(video->timelineEpoch)},{"geometry",qint64(video->geometryGeneration)},
        {"x",video->physicalGeometry.x()},{"y",video->physicalGeometry.y()},{"width",video->physicalGeometry.width()},
        {"height",video->physicalGeometry.height()},{"valid",video->coordinateMappingValid}}));
    }
    m_media->pushVideo(*video); // transport retains bounded latest video and accounts rejected frames
    if (!m_previewPending) {
      m_previewPending = true;
      Q_EMIT preview(video->pixels.scaled(640, 360, Qt::KeepAspectRatio, Qt::FastTransformation));
    }
  }
  drainAudio([this](const AudioBlock &block) { return m_media->pushAudio(block); });
  reportPlayback();
#endif
}
void SenderWorker::grantControl(){
  if(m_receiving || m_state!="streaming" || !m_selection["interactive"].toBool() || !m_controlLease.isEmpty())return;
  Q_EMIT outgoing(message("GrantControl",{{"session",m_session},{"source",m_source},{"lease",randomId()}}));
}
void SenderWorker::revokeControl(){
  m_controlLease.clear();publish();Q_EMIT controlChanged(false);
  if(!m_session.isEmpty())Q_EMIT status(m_receiving?tr("Receiving - viewing only."):tr("Streaming - receiver is viewing only."),true);
  if(!m_session.isEmpty())Q_EMIT outgoing(message("RevokeControl",{{"session",m_session},{"source",m_source}}));
}
void SenderWorker::viewerFocus(bool focused){
  if(!m_receiving || m_session.isEmpty())return;
  Q_EMIT outgoing(message("ViewerFocus",{{"session",m_session},{"source",m_source},{"focused",focused}}));
  if(!focused)revokeControl();
}
void SenderWorker::controlInput(const QJsonObject &event,const streaming::VideoFrame &presented){
  if(!m_receiving || m_controlLease.isEmpty() || presented.session!=m_session || presented.source!=m_source || !presented.coordinateMappingValid)return;
  auto data=event;data["session"]=m_session;data["source"]=m_source;data["lease"]=m_controlLease;
  data["sequence"]=++m_controlSequence;data["frame"]=qint64(presented.sequence);data["epoch"]=qint64(presented.timelineEpoch);
  data["geometry"]=qint64(presented.geometryGeneration);
  Q_EMIT outgoing(message("ControlInput",data));
}
} // namespace deskflow::gui
