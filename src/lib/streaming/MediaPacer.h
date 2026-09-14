// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <algorithm>
#include <cstdint>
#include <deque>
#include <utility>

namespace deskflow::streaming {
// Caller serializes this state with the transport mutex. Deadlines use the same
// monotonic nanosecond clock at admission and inspection.
class MediaPacer
{
public:
  bool allows(uint64_t size, uint32_t bitrate) const
  {
    const auto capacity = (std::max)(uint64_t(1500), uint64_t(bitrate / 80));
    return packets.size() < 512 && bytes <= capacity && size <= capacity - bytes;
  }
  bool admit(uint64_t size, uint32_t bitrate, int64_t now)
  {
    if (!allows(size, bitrate))
      return false;
    packets.emplace_back(size, now);
    bytes += size;
    return true;
  }
  void depart()
  {
    if (packets.empty())
      return;
    bytes -= packets.front().first;
    packets.pop_front();
  }
  bool expired(int64_t now) const
  {
    return !packets.empty() && now - packets.front().second > 100000000;
  }
  void clear()
  {
    packets.clear();
    bytes = 0;
  }
  uint64_t queuedBytes() const { return bytes; }

private:
  uint64_t bytes = 0;
  std::deque<std::pair<uint64_t, int64_t>> packets;
};
} // namespace deskflow::streaming
