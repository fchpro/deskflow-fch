// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#include <cstdint>

// The relay hook suppresses motion, so each reported position is relative to
// the parked cursor, not the preceding suppressed event. Queued events can
// arrive before PRE_WARP updates the saved position.
constexpr int32_t windowsMouseDelta(int32_t position, int32_t previous, int32_t center, bool onScreen)
{
  return position - (onScreen ? previous : center);
}
