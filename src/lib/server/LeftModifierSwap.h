/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "deskflow/KeyTypes.h"
#include <string>
#include <string_view>
#include <utility>

// Remap each recipient separately; the primary screen is never eligible.
class LeftModifierSwap
{
public:
  explicit LeftModifierSwap(std::string target = {}) : m_target(std::move(target)) {}

  std::pair<KeyID, KeyModifierMask> map(
      std::string_view destination, bool primary, KeyID key, KeyModifierMask mask, KeyModifierSides sides
  ) const
  {
    if (primary || m_target.empty() || destination != m_target)
      return {key, mask};

    if (key == kKeyControl_L)
      key = kKeySuper_L;
    else if (key == kKeySuper_L)
      key = kKeyControl_L;

    auto result = mask & ~(KeyModifierControl | KeyModifierSuper);
    if (mask & KeyModifierControl) {
      if (sides.left & KeyModifierControl)
        result |= KeyModifierSuper;
      if (!(sides.left & KeyModifierControl) || (sides.right & KeyModifierControl))
        result |= KeyModifierControl;
    }
    if (mask & KeyModifierSuper) {
      if (sides.left & KeyModifierSuper)
        result |= KeyModifierControl;
      if (!(sides.left & KeyModifierSuper) || (sides.right & KeyModifierSuper))
        result |= KeyModifierSuper;
    }
    return {key, result};
  }

private:
  std::string m_target;
};
