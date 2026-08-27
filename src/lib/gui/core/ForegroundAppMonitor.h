/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

//! Polls the foreground window (GUI side) and reports its exe, title and
//! whether it is in the excluded apps list.  Windows only; a no-op elsewhere.
class ForegroundAppMonitor : public QObject
{
  Q_OBJECT

public:
  static constexpr int kPollIntervalMs = 500;

  explicit ForegroundAppMonitor(QObject *parent = nullptr);

  static bool isSupported();

  void start();
  void stop();

  //! Force a poll now (also used by tests)
  void poll();

  QString exe() const
  {
    return m_exe;
  }
  QString title() const
  {
    return m_title;
  }
  bool excluded() const
  {
    return m_excluded;
  }

  //! Pure helper: case-insensitive membership test used for the label state
  static bool isExcludedApp(const QString &exe, const QStringList &excludedApps);

Q_SIGNALS:
  void changed(const QString &exe, const QString &title, bool excluded);

private:
  QTimer m_timer;
  QString m_exe;
  QString m_title;
  bool m_excluded = false;
  bool m_reported = false;
};
