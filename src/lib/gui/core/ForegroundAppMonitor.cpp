/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ForegroundAppMonitor.h"

#include "common/Settings.h"

#ifdef Q_OS_WIN
#include "platform/MSWindowsForegroundWatcher.h"
#endif

ForegroundAppMonitor::ForegroundAppMonitor(QObject *parent) : QObject(parent)
{
  m_timer.setInterval(kPollIntervalMs);
  connect(&m_timer, &QTimer::timeout, this, &ForegroundAppMonitor::poll);
}

bool ForegroundAppMonitor::isSupported()
{
#ifdef Q_OS_WIN
  return true;
#else
  return false;
#endif
}

void ForegroundAppMonitor::start()
{
  if (!isSupported()) {
    return;
  }
  poll();
  m_timer.start();
}

void ForegroundAppMonitor::stop()
{
  m_timer.stop();
}

bool ForegroundAppMonitor::isExcludedApp(const QString &exe, const QStringList &excludedApps)
{
  const auto name = exe.toLower();
  const auto dot = name.lastIndexOf(QLatin1Char('.'));
  const auto stem = (dot < 0) ? name : name.left(dot);
  for (const auto &entry : excludedApps) {
    const auto app = entry.trimmed().toLower();
    if (app.isEmpty()) {
      continue;
    }
    if (app == name) {
      return true;
    }
    if (!app.contains(QLatin1Char('.')) && app == stem) {
      return true;
    }
  }
  return false;
}

void ForegroundAppMonitor::poll()
{
  QString exe;
  QString title;
#ifdef Q_OS_WIN
  const HWND hwnd = GetForegroundWindow();
  if (hwnd != nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != 0) {
      exe = QString::fromStdWString(MSWindowsForegroundWatcher::processImageBaseName(pid));
    }
    const int length = GetWindowTextLengthW(hwnd);
    if (length > 0) {
      std::wstring text(static_cast<size_t>(length) + 1, L'\0');
      const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
      if (copied > 0) {
        title = QString::fromWCharArray(text.c_str(), copied);
      }
    }
  }
#endif

  const bool excluded =
      !exe.isEmpty() && isExcludedApp(exe, Settings::value(Settings::Server::ExcludedApps).toStringList());
  if (m_reported && exe == m_exe && title == m_title && excluded == m_excluded) {
    return;
  }
  m_reported = true;
  m_exe = exe;
  m_title = title;
  m_excluded = excluded;
  Q_EMIT changed(m_exe, m_title, m_excluded);
}
