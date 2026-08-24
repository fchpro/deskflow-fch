/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsForegroundWatcher.h"

#include <TlHelp32.h>

#include <algorithm>

MSWindowsForegroundWatcher *MSWindowsForegroundWatcher::s_instance = nullptr;

MSWindowsForegroundWatcher::MSWindowsForegroundWatcher(
    const std::vector<std::wstring> &excludedApps, ExclusionCallback callback, Logger logger
)
    : m_callback(std::move(callback)),
      m_logger(std::move(logger))
{
  m_excludedApps.reserve(excludedApps.size());
  for (const auto &app : excludedApps) {
    if (!app.empty()) {
      m_excludedApps.push_back(toLower(app));
    }
  }

  s_instance = this;

  // out-of-context: the callback is delivered via this thread's message pump,
  // so this must be constructed on a thread that pumps messages.
  m_hook = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, &MSWindowsForegroundWatcher::winEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT
  );
  if (m_hook == nullptr) {
    log("failed to install foreground app watcher");
  } else {
    log("foreground app watcher installed, excluded apps: " + std::to_string(m_excludedApps.size()));
  }

  checkForegroundNow();
}

MSWindowsForegroundWatcher::~MSWindowsForegroundWatcher()
{
  if (m_hook != nullptr) {
    UnhookWinEvent(m_hook);
    m_hook = nullptr;
  }
  s_instance = nullptr;
}

void MSWindowsForegroundWatcher::checkForegroundNow()
{
  handleForegroundWindow(GetForegroundWindow());
}

std::wstring MSWindowsForegroundWatcher::toLower(const std::wstring &text)
{
  std::wstring result = text;
  std::transform(result.begin(), result.end(), result.begin(), ::towlower);
  return result;
}

std::wstring MSWindowsForegroundWatcher::exeBaseName(const std::wstring &path)
{
  const auto pos = path.find_last_of(L"\\/");
  return (pos == std::wstring::npos) ? path : path.substr(pos + 1);
}

bool MSWindowsForegroundWatcher::isExcluded(const std::wstring &exeName, const std::vector<std::wstring> &excludedApps)
{
  const auto name = toLower(exeBaseName(exeName));
  const auto dot = name.find_last_of(L'.');
  const auto stem = (dot == std::wstring::npos) ? name : name.substr(0, dot);

  for (const auto &entry : excludedApps) {
    const auto app = toLower(entry);
    if (app == name) {
      return true;
    }
    // an entry with no extension matches the exe stem
    if (app.find(L'.') == std::wstring::npos && app == stem) {
      return true;
    }
  }
  return false;
}

std::wstring MSWindowsForegroundWatcher::processImageBaseName(DWORD pid)
{
  if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid); process != nullptr) {
    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(process, 0, path, &size) != 0;
    CloseHandle(process);
    if (ok) {
      return exeBaseName(path);
    }
  }

  // protected processes (e.g. anti-cheat) deny OpenProcess; a toolhelp
  // snapshot still exposes the exe name.
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return {};
  }
  std::wstring result;
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (entry.th32ProcessID == pid) {
        result = entry.szExeFile;
        break;
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return result;
}

void CALLBACK MSWindowsForegroundWatcher::winEventProc(
    HWINEVENTHOOK, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD
)
{
  if (event != EVENT_SYSTEM_FOREGROUND || idObject != OBJID_WINDOW || idChild != CHILDID_SELF) {
    return;
  }
  if (s_instance != nullptr) {
    s_instance->handleForegroundWindow(hwnd);
  }
}

void MSWindowsForegroundWatcher::handleForegroundWindow(HWND hwnd)
{
  if (hwnd == nullptr) {
    return;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0) {
    return;
  }
  const auto exeName = processImageBaseName(pid);
  if (exeName.empty()) {
    return;
  }
  const bool excluded = isExcluded(exeName, m_excludedApps);
  if (excluded == m_lastExcluded) {
    return;
  }
  m_lastExcluded = excluded;

  std::string narrowName;
  narrowName.reserve(exeName.size());
  for (const auto ch : exeName) {
    narrowName.push_back(ch < 128 ? static_cast<char>(ch) : '?');
  }
  log("foreground app \"" + narrowName + (excluded ? "\" is excluded, pausing input sharing" : "\" is not excluded, resuming input sharing"));

  if (m_callback) {
    m_callback(excluded, exeName);
  }
}

void MSWindowsForegroundWatcher::log(const std::string &message) const
{
  if (m_logger) {
    m_logger(message);
  }
}
