// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <cstdint>
#include <optional>
#include <utility>
namespace deskflow::streaming {
// Caller serializes packet/decoded updates and uses one monotonic nanosecond clock.
class DecodeProgress {
public:
  void received(uint64_t epoch,uint64_t sequence,int64_t now) {
    const auto identity=std::pair(epoch,sequence);
    if(!m_received||identity>*m_received)m_received=identity;
    if(m_decoded&&identity>*m_decoded&&m_pendingSince<0)m_pendingSince=now;
  }
  void decoded(uint64_t epoch,uint64_t sequence,int64_t now) {
    const auto identity=std::pair(epoch,sequence);
    if(!m_decoded||identity>*m_decoded){
      m_decoded=identity;
      m_pendingSince=m_received&&*m_received>identity?now:-1;
    }
  }
  bool stalled(int64_t now) const {return m_pendingSince>=0&&now-m_pendingSince>3000000000LL;}
  void clear(){m_received.reset();m_decoded.reset();m_pendingSince=-1;}
private:
  std::optional<std::pair<uint64_t,uint64_t>> m_received,m_decoded;
  int64_t m_pendingSince=-1;
};
}
