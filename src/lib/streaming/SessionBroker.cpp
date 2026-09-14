// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "SessionBroker.h"
#include "Control.h"
#include <QRegularExpression>

namespace deskflow::streaming {
namespace {
bool choice(const QJsonValue &value, const QStringList &choices)
{
  return value.isString() && choices.contains(value.toString());
}
bool list(const QJsonValue &value, const QStringList &choices)
{
  if (!value.isArray() || value.toArray().size() > choices.size())
    return false;
  QStringList seen;
  for (const auto &entry : value.toArray()) {
    if (!choice(entry, choices) || seen.contains(entry.toString()))
      return false;
    seen.append(entry.toString());
  }
  return true;
}
bool supports(const Peer &sender, const Peer &receiver, const QJsonObject &offer)
{
  auto required = offer;
  required.remove("title"); required.remove("playback");
  const auto title = offer["title"].toString();
  return (!offer.contains("title") || (offer["title"].isString() && title.size() <= 255 &&
          !title.contains(QRegularExpression("[\\x00-\\x1f\\x7f]")))) &&
         (!offer.contains("playback") || (offer["playback"].isBool() &&
          (!offer["playback"].toBool() || offer["kind"] == "file"))) &&
         fields(required, {"session", "to", "source", "kind", "audio", "preset", "interactive"}) &&
         identifier(offer["session"].toString()) && identifier(offer["source"].toString()) &&
         sender.capabilities["sources"].toArray().contains(offer["kind"]) &&
         receiver.capabilities["receive"].toBool() && sender.capabilities["audio"].toArray().contains(offer["audio"]) &&
         receiver.capabilities["audio"].toArray().contains(offer["audio"]) &&
         choice(offer["preset"], {"low", "balanced", "smooth"}) && offer["interactive"].isBool() &&
         (!offer["interactive"].toBool() ||
          (sender.capabilities["control"].toBool() && receiver.capabilities["control"].toBool()));
}
bool validSdp(const QJsonObject &data, bool audioAllowed)
{
  if (!fields(data, {"session", "source", "sdp", "fingerprint"}))
    return false;
  const auto sdp = data["sdp"].toString();
  const auto fingerprint = data["fingerprint"].toString();
  const QRegularExpression hash("^[0-9A-F]{2}(:[0-9A-F]{2}){31}$");
  if (sdp.size() > 200000 || !sdp.startsWith("v=0\r\n") || !hash.match(fingerprint).hasMatch() ||
      !sdp.contains("a=group:BUNDLE ") || !sdp.contains("a=rtcp-mux\r\n") || sdp.contains("a=candidate:") ||
      sdp.contains("a=ice-lite"))
    return false;
  int fingerprints = 0, media = 0;
  for (const auto &line : sdp.split("\r\n")) {
    if (line.startsWith("a=fingerprint:")) {
      if (line != "a=fingerprint:sha-256 " + fingerprint)
        return false;
      ++fingerprints;
    }
    if (line.startsWith("m=")) {
      const auto parts = line.split(' ');
      if (parts[0] == "m=audio" && !audioAllowed)
        return false;
      if (parts.size() < 4 || (parts[0] != "m=audio" && parts[0] != "m=video") || parts[2] != "UDP/TLS/RTP/SAVPF")
        return false;
      ++media;
    }
  }
  return fingerprints > 0 && media > 0;
}
} // namespace
bool SessionBroker::validCapabilities(const QJsonObject &value)
{
  return fields(value, {"sources", "receive", "audio", "control"}) &&
         list(value["sources"], {"screen", "window", "file"}) && value["receive"].isBool() &&
         list(value["audio"], {"off", "system", "application", "file"}) && value["control"].isBool();
}
bool SessionBroker::attach(const Peer &peer)
{
  if (!identifier(peer.id, 64) || !identifier(peer.generation) || peer.name.isEmpty() || peer.name.size() > 255 ||
      peer.address.isNull() || !validCapabilities(peer.capabilities) || m_peers.size() >= 64 ||
      m_peers.contains(peer.id))
    return false;
  m_peers.insert(peer.id, peer);
  roster();
  return true;
}
void SessionBroker::detach(const QString &id)
{
  for (const auto &session : m_sessions.values()) {
    if (session.sender == id || session.receiver == id)
      stop(session.id, "peerDisconnected");
  }
  m_peers.remove(id);
  roster();
}
void SessionBroker::roster()
{
  QJsonArray entries;
  for (const auto &peer : m_peers)
    entries.append(
        QJsonObject{
            {"id", peer.id}, {"name", peer.name}, {"generation", peer.generation}, {"capabilities", peer.capabilities}, {"address", peer.address.toString()}
        }
    );
  for (const auto &peer : m_peers)
    Q_EMIT deliver(peer.id, message("Roster", {{"peers", entries}}));
}
bool SessionBroker::fail(const QString &peer, const QString &reason)
{
  Q_EMIT deliver(peer, message("Error", {{"reason", reason}}));
  return false;
}
void SessionBroker::state(Session &session, const QString &next, qint64 deadline)
{
  session.state = next;
  session.deadline = deadline;
  const auto event = message(
      "State", {{"session", session.id},
                {"source", session.source},
                {"state", next},
                {"sender", session.sender},
                {"receiver", session.receiver}}
  );
  Q_EMIT deliver(session.sender, event);
  Q_EMIT deliver(session.receiver, event);
}
void SessionBroker::stop(const QString &id, const QString &reason)
{
  auto it = m_sessions.find(id);
  if (it == m_sessions.end())
    return;
  const auto session = it.value();
  Q_EMIT controlRevoked(id);
  m_sessions.erase(it);
  const auto event = message("Stopped", {{"session", id}, {"source", session.source}, {"reason", reason}});
  Q_EMIT deliver(session.sender, event);
  Q_EMIT deliver(session.receiver, event);
}
bool SessionBroker::dispatch(const QString &origin, const QString &generation, const QJsonObject &frame, qint64 now)
{
  if (!m_peers.contains(origin) || m_peers[origin].generation != generation || !validMessage(frame))
    return false;
  const auto type = frame["type"].toString();
  const auto data = frame["data"].toObject();
  if (type == "Capabilities") {
    if (!validCapabilities(data))
      return fail(origin, "malformedCapabilities");
    for (const auto &session : m_sessions.values()) {
      if (session.sender == origin || session.receiver == origin)
        stop(session.id, "capabilitiesChanged");
    }
    m_peers[origin].capabilities = data;
    roster();
    return true;
  }
  if (type == "Offer") {
    const auto target = data["to"].toString();
    if (target == origin || !m_peers.contains(target) || !supports(m_peers[origin], m_peers[target], data))
      return fail(origin, "incompatibleOffer");
    for (const auto &session : m_sessions) {
      if (session.sender == origin || session.receiver == origin || session.sender == target ||
          session.receiver == target)
        return fail(origin, "busy");
    }
    const auto id = data["session"].toString();
    if (m_usedSessions.contains(id) || m_usedSessions.size() >= 65536)
      return fail(origin, "duplicateSession");
    m_usedSessions.insert(id);
    Session session;
    session.id = id;
    session.sender = origin;
    session.receiver = target;
    session.source = data["source"].toString();
    session.senderGeneration = generation;
    session.receiverGeneration = m_peers[target].generation;
    session.offer = data;
    m_sessions.insert(id, session);
    auto forwarded = data;
    forwarded.insert("from", origin);
    forwarded.insert("generation", generation);
    Q_EMIT deliver(target, message("Offer", forwarded));
    state(m_sessions[id], "awaitingConsent", now + 30000);
    return true;
  }
  const auto id = data["session"].toString();
  auto it = m_sessions.find(id);
  if (it == m_sessions.end())
    return fail(origin, "staleSession");
  auto &session = it.value();
  const bool sender = session.sender == origin;
  if ((!sender && session.receiver != origin) || data["source"] != session.source ||
      generation != (sender ? session.senderGeneration : session.receiverGeneration))
    return fail(origin, "sessionIdentityMismatch");
  // Viewer focus is owned only by the receiving core's private IPC endpoint.
  // Reject network delivery as a control error without ending valid media.
  if (type == "ViewerFocus") return fail(origin,"invalidControlFocus");
  if (type == "Stop" || type == "Decline") {
    if (!fields(data, {"session", "source"}) || (type == "Decline" && (sender || session.state != "awaitingConsent")))
      return fail(origin, "invalidStop");
    stop(id, type == "Decline" ? "declined" : "stopped");
    return true;
  }
  if (type == "Accept") {
    if (sender || session.state != "awaitingConsent" || now >= session.deadline || !fields(data, {"session", "source"}))
      return fail(origin, "invalidConsent");
    session.senderHeartbeat = session.receiverHeartbeat = now;
    state(session, "negotiating", now + 10000);
    Q_EMIT deliver(session.sender, message("Accept", data));
    return true;
  }
  if (type == "SdpOffer" || type == "SdpAnswer") {
    if (!validSdp(data, session.offer["audio"] != "off") ||
        (type == "SdpOffer" ? (!sender || session.state != "negotiating")
                            : (sender || session.state != "awaitingAnswer")))
      return fail(origin, "invalidSdp");
    state(session, type == "SdpOffer" ? "awaitingAnswer" : "starting", now + 10000);
  } else if (type == "IceCandidate") {
    if ((!fields(data, {"session", "source", "candidate"}) && !fields(data, {"session", "source", "candidate", "mline"})) || session.state == "awaitingConsent" ||
        (data.contains("mline") && (!data["mline"].isDouble() || data["mline"].toDouble() != data["mline"].toInt() || data["mline"].toInt() < 0 || data["mline"].toInt() > (session.offer["audio"] == "off" ? 0 : 1))))
      return fail(origin, "invalidCandidate");
    const auto tokens = data["candidate"].toString().split(' ');
    bool portOk = false;
    const auto port = tokens.size() > 5 ? tokens[5].toUInt(&portOk) : 0;
    if (tokens.size() < 8 || tokens.size() > 18 || tokens.size() % 2 != 0 || !tokens[0].startsWith("candidate:") ||
        tokens[0].size() > 64 || tokens[1] != "1" || tokens[2].toUpper() != "UDP" || !portOk || port < 24802 ||
        port > 24831 || tokens[6] != "typ" || tokens[7] != "host" || QHostAddress(tokens[4]) != m_peers[origin].address)
      return fail(origin, "candidateOutsidePeer");
    QSet<QString> extensions;
    for (int i = 8; i < tokens.size(); i += 2) {
      const auto &key = tokens[i];
      const auto &value = tokens[i + 1];
      if (extensions.contains(key) || !QStringList{"generation", "network-id", "network-cost", "ufrag"}.contains(key) ||
          !QRegularExpression("^[a-zA-Z0-9+/]{1,256}$").match(value).hasMatch())
        return fail(origin, "invalidCandidateExtension");
      extensions.insert(key);
    }
  } else if (type == "Ready") {
    if (!fields(data, {"session", "source"}) || session.state != "starting")
      return fail(origin, "invalidReady");
    (sender ? session.senderReady : session.receiverReady) = true;
    if (session.senderReady && session.receiverReady)
      state(session, "streaming", 0);
  } else if (type == "Heartbeat") {
    if (!fields(data, {"session", "source"}) || session.state == "awaitingConsent")
      return fail(origin, "invalidHeartbeat");
    (sender ? session.senderHeartbeat : session.receiverHeartbeat) = now;
    if(!sender && !session.controlLease.isEmpty())Q_EMIT deliver(session.sender,frame);
    return true;
  } else if (type == "Pause" || type == "Resume") {
    if (!sender || !fields(data, {"session", "source"}) || session.state != (type == "Pause" ? "streaming" : "paused"))
      return fail(origin, "invalidPlaybackState");
    state(session, type == "Pause" ? "paused" : "streaming", 0);
  } else if (type == "PlaybackState") {
    const auto integer = [](const QJsonValue &value, double maximum) {
      return value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= maximum &&
             value.toDouble() == double(value.toInteger(-1));
    };
    if (!sender || session.offer["kind"] != "file" ||
        !QStringList{"starting", "streaming", "paused"}.contains(session.state) ||
        !fields(data, {"session", "source", "positionMs", "durationMs", "epoch", "paused", "seekable"}) ||
        !integer(data["durationMs"], 604800000) || !integer(data["positionMs"], data["durationMs"].toDouble()) ||
        !integer(data["epoch"], 9007199254740991.0) || !data["paused"].isBool() || !data["seekable"].isBool() ||
        (!session.playback.isEmpty() && data["epoch"].toInteger() < session.playback["epoch"].toInteger()))
      return fail(origin, "invalidPlaybackState");
    session.playback = data;
  } else if (type == "PlaybackCommand") {
    if (sender || session.offer["kind"] != "file" || !session.offer["playback"].toBool() ||
        !QStringList{"streaming", "paused"}.contains(session.state) || session.playback.isEmpty() ||
        !fields(data, {"session", "source", "action", "positionMs", "epoch"}) ||
        data["epoch"] != session.playback["epoch"] ||
        !choice(data["action"], {"pause", "resume", "seek"}) || !data["positionMs"].isDouble() ||
        data["positionMs"].toDouble() != double(data["positionMs"].toInteger(-1)) ||
        data["positionMs"].toDouble() < 0 ||
        (data["action"] == "seek" ? (!session.playback["seekable"].toBool() ||
          data["positionMs"].toDouble() >= session.playback["durationMs"].toDouble()) : data["positionMs"] != 0) ||
        now - session.lastPlaybackCommand < 100)
      return fail(origin, "invalidPlaybackCommand");
    session.lastPlaybackCommand = now;
  } else if (type == "ControlGeometry") {
    if (!sender || session.offer["kind"]=="file" || !QStringList{"starting","streaming"}.contains(session.state) ||
        !validControlGeometry(data) || (!session.controlGeometry.isEmpty() &&
        data["frame"].toInteger()<=session.controlGeometry["frame"].toInteger())) return fail(origin,"invalidControlGeometry");
    if (!session.controlLease.isEmpty() && (data["geometry"]!=session.controlGeometry["geometry"] ||
        data["epoch"]!=session.controlGeometry["epoch"] || !data["valid"].toBool())) {
      session.controlLease.clear(); Q_EMIT controlRevoked(id);
      const auto revoked=message("RevokeControl",{{"session",id},{"source",session.source}});
      Q_EMIT deliver(session.sender,revoked);Q_EMIT deliver(session.receiver,revoked);
    }
    session.controlGeometry=data;
  } else if (type == "GrantControl") {
    if (!sender || session.state!="streaming" || session.offer["kind"]=="file" || !session.offer["interactive"].toBool() ||
        !session.controlGeometry["valid"].toBool() || !session.controlLease.isEmpty() ||
        !fields(data,{"session","source","lease"}) || !identifier(data["lease"].toString())) return fail(origin,"invalidControlGrant");
    session.controlLease=data["lease"].toString();session.controlSequence=0;
    Q_EMIT deliver(session.sender,frame);
  } else if (type == "RevokeControl") {
    if(!fields(data,{"session","source"}))return fail(origin,"invalidControlRevoke");
    session.controlLease.clear();Q_EMIT controlRevoked(id);
    Q_EMIT deliver(origin,frame);
  } else if (type == "ControlInput") {
    if(sender || session.state!="streaming" || session.controlLease.isEmpty() || !validControlInput(data) ||
       data["lease"]!=session.controlLease || data["geometry"]!=session.controlGeometry["geometry"] ||
       data["epoch"]!=session.controlGeometry["epoch"] || data["frame"].toInteger()>session.controlGeometry["frame"].toInteger() ||
       data["sequence"].toInteger()<=session.controlSequence)return fail(origin,"invalidControlInput");
    session.controlSequence=data["sequence"].toInteger();
  } else {
    // Only explicitly validated session commands reach the opposite peer.
    return fail(origin, "unsupportedCommand");
  }
  Q_EMIT deliver(sender ? session.receiver : session.sender, frame);
  return true;
}
void SessionBroker::expire(qint64 now)
{
  for (const auto &session : m_sessions.values()) {
    if ((session.deadline && now >= session.deadline) ||
        (session.state != "awaitingConsent" &&
         (now - session.senderHeartbeat >= 3000 || now - session.receiverHeartbeat >= 3000)))
      stop(session.id, "timeout");
  }
}
} // namespace deskflow::streaming
