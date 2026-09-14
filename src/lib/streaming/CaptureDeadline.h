// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <QtGlobal>
namespace deskflow::streaming {
class CaptureDeadline {
public:
  void start(qint64 nowMs) { m_started = nowMs; m_received = false; }
  void frame(qint64) { m_received = true; }
  bool expired(qint64 nowMs) const { return !m_received && nowMs - m_started > 3000; }
private:
  qint64 m_started = 0;
  bool m_received = false;
};
}
