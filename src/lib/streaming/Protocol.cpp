// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "Protocol.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QUuid>
#include <QtEndian>

namespace deskflow::streaming {
bool FrameReader::feed(const QByteArray &bytes, const std::function<void(const QJsonObject &)> &receive)
{
  if (m_failed || bytes.size() > limit + 4 || m_buffer.size() + bytes.size() > 2 * (limit + 4))
    return m_failed = true, false;
  m_buffer += bytes;
  while (m_buffer.size() >= 4) {
    const auto length = qFromBigEndian<quint32>(m_buffer.constData());
    if (length == 0 || length > limit)
      return m_failed = true, false;
    if (m_buffer.size() < length + 4)
      break;
    const auto body = m_buffer.mid(4, length);
    m_buffer.remove(0, length + 4);
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(body, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject() ||
        document.toJson(QJsonDocument::Compact) != body || !validMessage(document.object()))
      return m_failed = true, false;
    receive(document.object());
  }
  return true;
}
QByteArray FrameReader::encode(const QJsonObject &value)
{
  const auto body = QJsonDocument(value).toJson(QJsonDocument::Compact);
  if (body.isEmpty() || body.size() > limit || !validMessage(value))
    return {};
  QByteArray output(4, '\0');
  qToBigEndian<quint32>(static_cast<quint32>(body.size()), output.data());
  return output + body;
}
bool fields(const QJsonObject &object, const QStringList &required)
{
  auto keys = object.keys();
  auto expected = required;
  expected.sort();
  return keys == expected;
}
bool identifier(const QString &value, int length)
{
  if (value.size() != length)
    return false;
  for (const auto c : value) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
      return false;
  }
  return true;
}
QString randomId()
{
  return QUuid::createUuid().toString(QUuid::Id128);
}
QJsonObject message(const QString &type, const QJsonObject &data)
{
  return {{"v", 1}, {"type", type}, {"data", data}};
}
bool validMessage(const QJsonObject &value)
{
  static const QStringList types = {
      "Challenge", "Proof",     "Authenticated",   "Capabilities", "Roster",        "Offer", "Accept",  "Decline",
      "SdpOffer",  "SdpAnswer", "IceCandidate",    "Ready",        "Heartbeat",     "Stop",  "Stopped", "Error",
      "Pause",     "Resume",    "PlaybackCommand", "PlaybackState", "GrantControl", "RevokeControl", "State", "Identity",
      "ControlTarget", "ControlGeometry", "ControlInput", "ControlAvailability", "ViewerFocus"
  };
  return fields(value, {"v", "type", "data"}) && value["v"] == 1 && types.contains(value["type"].toString()) &&
         value["data"].isObject();
}
} // namespace deskflow::streaming
