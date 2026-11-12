/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Fakhri Chahed
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include <QTest>

class ScreenEntryGuardTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void smallPushAtEntryEdge_held();
  void cumulativePushOverThreshold_releases();
  void inwardMove_resetsAccumulatedPush();
  void inwardThreshold_disarms();
  void otherDirections_unaffected();
  void zeroThreshold_neverHolds();
  void reset_disarms();
  void distanceInsideEdge_allEdges();
  void macLeftOfPc_reversalAfterFlick();
};
