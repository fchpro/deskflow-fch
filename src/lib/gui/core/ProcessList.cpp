/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ProcessList.h"

#include <QHash>

#include <algorithm>

#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <TlHelp32.h>
#endif

namespace deskflow::gui {

#ifdef Q_OS_WIN
namespace {
struct TitleScan
{
  QHash<quint32, QString> titles;
};

BOOL CALLBACK collectWindowTitle(HWND hwnd, LPARAM param)
{
  auto *scan = reinterpret_cast<TitleScan *>(param);
  if (!IsWindowVisible(hwnd) || GetWindow(hwnd, GW_OWNER) != nullptr) {
    return TRUE;
  }
  const int length = GetWindowTextLengthW(hwnd);
  if (length <= 0) {
    return TRUE;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0 || scan->titles.contains(pid)) {
    return TRUE;
  }
  std::wstring text(static_cast<size_t>(length) + 1, L'\0');
  const int copied = GetWindowTextW(hwnd, text.data(), length + 1);
  if (copied > 0) {
    scan->titles.insert(pid, QString::fromWCharArray(text.c_str(), copied));
  }
  return TRUE;
}
} // namespace
#endif

QList<ProcessInfo> ProcessList::running()
{
  QList<ProcessInfo> list;
#ifdef Q_OS_WIN
  TitleScan scan;
  EnumWindows(&collectWindowTitle, reinterpret_cast<LPARAM>(&scan));

  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return list;
  }
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      ProcessInfo info;
      info.exe = QString::fromWCharArray(entry.szExeFile);
      info.pid = entry.th32ProcessID;
      info.title = scan.titles.value(info.pid);
      if (!info.exe.isEmpty()) {
        list.append(info);
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
#endif
  return list;
}

QList<ProcessInfo> ProcessList::dedupeByExe(const QList<ProcessInfo> &list)
{
  QList<ProcessInfo> result;
  QHash<QString, int> indexByExe;
  for (const auto &info : list) {
    const auto key = info.exe.toLower();
    if (auto it = indexByExe.find(key); it != indexByExe.end()) {
      auto &kept = result[it.value()];
      if (kept.title.isEmpty() && !info.title.isEmpty()) {
        kept = info;
      }
      continue;
    }
    indexByExe.insert(key, result.size());
    result.append(info);
  }
  return result;
}

QList<ProcessInfo> ProcessList::filter(const QList<ProcessInfo> &list, const QString &query)
{
  const auto needle = query.trimmed();
  if (needle.isEmpty()) {
    return list;
  }
  QList<ProcessInfo> result;
  for (const auto &info : list) {
    if (info.exe.contains(needle, Qt::CaseInsensitive) || info.title.contains(needle, Qt::CaseInsensitive)) {
      result.append(info);
    }
  }
  return result;
}

QList<ProcessInfo> ProcessList::sorted(const QList<ProcessInfo> &list)
{
  QList<ProcessInfo> result = list;
  std::stable_sort(result.begin(), result.end(), [](const ProcessInfo &a, const ProcessInfo &b) {
    const bool aTitled = !a.title.isEmpty();
    const bool bTitled = !b.title.isEmpty();
    if (aTitled != bTitled) {
      return aTitled;
    }
    return a.exe.compare(b.exe, Qt::CaseInsensitive) < 0;
  });
  return result;
}

QString ProcessList::displayText(const ProcessInfo &info)
{
  if (info.title.isEmpty()) {
    return info.exe;
  }
  return QStringLiteral("%1  -  %2").arg(info.exe, info.title);
}

} // namespace deskflow::gui
