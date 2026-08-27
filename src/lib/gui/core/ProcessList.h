/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QList>
#include <QString>

namespace deskflow::gui {

struct ProcessInfo
{
  QString exe;   //!< exe base name, e.g. "bf6.exe"
  QString title; //!< a visible top-level window title, empty when none
  quint32 pid = 0;

  bool operator==(const ProcessInfo &other) const = default;
};

//! Running-process enumeration and pure list helpers (used by the excluded
//! apps dialog).  `running()` is Windows-only; the helpers are portable.
class ProcessList
{
public:
  //! One entry per running process (Windows); empty on other platforms
  static QList<ProcessInfo> running();

  //! Keep one entry per exe (case-insensitive), preferring one with a title
  static QList<ProcessInfo> dedupeByExe(const QList<ProcessInfo> &list);

  //! Case-insensitive substring match on exe or title; empty query keeps all
  static QList<ProcessInfo> filter(const QList<ProcessInfo> &list, const QString &query);

  //! Entries with a window title first, then alphabetical by exe
  static QList<ProcessInfo> sorted(const QList<ProcessInfo> &list);

  //! "bf6.exe  -  Battlefield 6" or just the exe when there is no title
  static QString displayText(const ProcessInfo &info);
};

} // namespace deskflow::gui
