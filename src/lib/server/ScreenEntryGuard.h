/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/DirectionTypes.h"

#include <cstdint>

/**
 * @brief Switching hysteresis for the edge a client screen was entered from.
 *
 * Pure logic, no dependencies. After a motion-driven switch onto a client
 * screen the cursor sits exactly on the entry edge (the server gives clients
 * no jump-zone margin). A small physical reversal after a fast flick would
 * otherwise switch straight back. While armed, leaving through the entry edge
 * is held until the accumulated push beyond that edge exceeds the threshold.
 * Moving back inside resets the accumulated push; moving at least the
 * threshold inward disarms the guard. Other edges are never affected.
 */
class ScreenEntryGuard
{
public:
  //! Arms the guard for leaving in @p backDir; @p thresholdPx <= 0 disables it.
  void arm(Direction backDir, int32_t thresholdPx)
  {
    reset();
    if (thresholdPx <= 0 || backDir == Direction::NoDirection) {
      return;
    }
    m_backDir = backDir;
    m_threshold = thresholdPx;
  }

  void reset()
  {
    m_backDir = Direction::NoDirection;
    m_threshold = 0;
    m_push = 0;
  }

  bool armed() const
  {
    return m_backDir != Direction::NoDirection;
  }

  Direction backDirection() const
  {
    return m_backDir;
  }

  int32_t accumulatedPush() const
  {
    return m_push;
  }

  /**
   * @brief Decides whether leaving the active screen must be held.
   * @param dir direction the cursor is leaving in
   * @param pushPx how far beyond the edge this motion went (> 0)
   * @return true to clamp at the edge without switching; false to let the
   *   normal switch logic run (the guard disarms when it releases)
   */
  bool holdLeave(Direction dir, int32_t pushPx)
  {
    if (!armed() || dir != m_backDir) {
      return false;
    }
    m_push += pushPx > 0 ? pushPx : 0;
    if (m_push > m_threshold) {
      reset();
      return false;
    }
    return true;
  }

  /**
   * @brief Records in-screen motion.
   * @param inwardPx distance of the cursor from the entry edge (0 = on it)
   */
  void observeInside(int32_t inwardPx)
  {
    if (!armed() || inwardPx <= 0) {
      return;
    }
    if (inwardPx >= m_threshold) {
      reset();
      return;
    }
    m_push = 0;
  }

  /**
   * @brief Signed distance of (x, y) inside @p edge of a screen rectangle.
   *
   * 0 = on the edge pixel, positive = inside, negative = beyond the edge.
   */
  static int32_t distanceInsideEdge(
      Direction edge, int32_t x, int32_t y, int32_t ax, int32_t ay, int32_t aw, int32_t ah
  )
  {
    switch (edge) {
      using enum Direction;
    case Left:
      return x - ax;
    case Right:
      return ax + aw - 1 - x;
    case Top:
      return y - ay;
    case Bottom:
      return ay + ah - 1 - y;
    default:
      return 0;
    }
  }

  static Direction opposite(Direction dir)
  {
    switch (dir) {
      using enum Direction;
    case Left:
      return Right;
    case Right:
      return Left;
    case Top:
      return Bottom;
    case Bottom:
      return Top;
    default:
      return NoDirection;
    }
  }

private:
  Direction m_backDir = Direction::NoDirection;
  int32_t m_threshold = 0;
  int32_t m_push = 0;
};
