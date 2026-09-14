// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once
#include <atomic>
#include <cstdint>
namespace deskflow::streaming {
// Shared by core input dispatch and its signaling worker. Never set by media code.
inline std::atomic<bool> controlOwnsInput{false};
inline std::atomic<bool> viewerOwnsInput{false};
inline std::atomic<bool> ordinaryInputLocal{true};
inline constexpr uintptr_t controlInputMarker = 0x44534643;
inline bool streamingOwnsInput() { return controlOwnsInput.load() || viewerOwnsInput.load(); }
template<class NativeRelease> void finishOrdinaryInput(NativeRelease release) {
  release();
  ordinaryInputLocal=true;
}
}
