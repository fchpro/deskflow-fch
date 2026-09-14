// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "Capture.h"
namespace deskflow::streaming {
// Direct WGC only. No DXGI duplication/default-monitor path exists.
class WindowsWgcSource {
public:
  WindowsWgcSource();
  ~WindowsWgcSource();
  bool start(quint64 window, quint64 monitor, int fps, QString &error);
  void stop();
  std::optional<VideoFrame> pull(QString &error);
private:
  struct State;
  std::shared_ptr<State> m_state;
};
}
