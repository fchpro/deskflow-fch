/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <cstdint>
#include <optional>

/**
 * @brief Rate-limits mouse-move deltas sent to a client.
 *
 * Pure logic, no dependencies. Deltas are accumulated and released at most
 * once per interval. When the interval has not elapsed the delta stays
 * pending; the caller is expected to arm a timer of `timeUntilFlush()` to
 * release the tail via `flush()`.
 *
 * Time is passed in explicitly (microseconds, monotonic) so the class is
 * testable without a clock.
 */
class MouseMoveCoalescer
{
public:
  struct Delta
  {
    int32_t dx = 0;
    int32_t dy = 0;
  };

  explicit MouseMoveCoalescer(int64_t intervalUs) : m_intervalUs(intervalUs)
  {
  }

  bool enabled() const
  {
    return m_intervalUs > 0;
  }

  int64_t intervalUs() const
  {
    return m_intervalUs;
  }

  /// Add a delta at time `nowUs`. Returns the accumulated delta if it should
  /// be sent now, otherwise `std::nullopt` (delta kept pending).
  std::optional<Delta> add(int32_t dx, int32_t dy, int64_t nowUs)
  {
    m_pending.dx += dx;
    m_pending.dy += dy;
    if (!enabled() || nowUs - m_lastFlushUs >= m_intervalUs) {
      return flush(nowUs);
    }
    return std::nullopt;
  }

  /// Release whatever is pending (may be zero delta). Resets the interval.
  Delta flush(int64_t nowUs)
  {
    Delta out = m_pending;
    m_pending = Delta{};
    m_lastFlushUs = nowUs;
    return out;
  }

  bool hasPending() const
  {
    return m_pending.dx != 0 || m_pending.dy != 0;
  }

  /// Microseconds until the next flush is allowed, given `nowUs` (>= 0).
  int64_t timeUntilFlushUs(int64_t nowUs) const
  {
    const int64_t remaining = m_intervalUs - (nowUs - m_lastFlushUs);
    return remaining > 0 ? remaining : 0;
  }

  /// Drop pending motion (e.g. on screen switch).
  void reset()
  {
    m_pending = Delta{};
  }

private:
  int64_t m_intervalUs;
  int64_t m_lastFlushUs = INT64_MIN / 2;
  Delta m_pending;
};
