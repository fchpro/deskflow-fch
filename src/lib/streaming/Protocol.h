// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <functional>

namespace deskflow::streaming {
// Canonical compact JSON rejects duplicate keys and ambiguous encodings. All
// channels use the same bounded incremental framing, never newline IPC.
class FrameReader
{
public:
  static constexpr qsizetype limit = 256 * 1024;
  bool feed(const QByteArray &bytes, const std::function<void(const QJsonObject &)> &receive);
  static QByteArray encode(const QJsonObject &message);

private:
  QByteArray m_buffer;
  bool m_failed = false;
};
bool fields(const QJsonObject &object, const QStringList &required);
bool identifier(const QString &value, int length = 32);
QString randomId();
QJsonObject message(const QString &type, const QJsonObject &data = {});
bool validMessage(const QJsonObject &message);
} // namespace deskflow::streaming
