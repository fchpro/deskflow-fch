// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include "streaming/Capture.h"
namespace deskflow::gui {
// Two 20-ms codec packets of scheduling margin, included in both output and video time.
inline constexpr qint64 receiverPlayoutMarginNs = 40000000;
class ReceiverVideoQueue {
public:
  void clear() { m_next.reset(); m_latest.reset(); m_epoch.reset(); }
  void push(streaming::VideoFrame frame) {
    if (m_epoch && frame.timelineEpoch < *m_epoch) return;
    if (!m_epoch || frame.timelineEpoch > *m_epoch) { clear(); m_epoch=frame.timelineEpoch; }
    if (!m_next) m_next=std::move(frame); else m_latest=std::move(frame);
  }
  std::optional<streaming::VideoFrame> take(qint64 positionNs,bool outstanding) {
    if (!m_next || outstanding || m_next->mediaTimeNs>positionNs+5000000) return {};
    auto result=std::move(m_next); m_next=std::move(m_latest); m_latest.reset(); return result;
  }
  qint64 nextPts() const { return m_next ? m_next->mediaTimeNs : -1; }
private:
  std::optional<streaming::VideoFrame> m_next,m_latest;
  std::optional<quint64> m_epoch;
};
// Used only when no native output graph supplies the shared media clock.
class ReceiverClock {
public:
  void reset(qint64 origin,qint64 now) { m_origin=origin; m_anchor=now; m_pause=now; m_paused=false; }
  void pause(bool paused,qint64 now) {
    if (paused==m_paused) return;
    if (paused) m_pause=now; else m_anchor+=now-m_pause;
    m_paused=paused;
  }
  qint64 position(qint64 now) const { return m_origin+(m_paused ? m_pause : now)-m_anchor; }
private:
  qint64 m_origin=0,m_anchor=0,m_pause=0;
  bool m_paused=false;
};
}
