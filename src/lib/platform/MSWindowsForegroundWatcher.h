/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>

#include <functional>
#include <string>
#include <vector>

//! Watches the foreground window and reports when an excluded app owns it
/*!
Installs an out-of-context WinEvent hook for EVENT_SYSTEM_FOREGROUND on the
calling thread.  The callback fires on that thread's message pump whenever the
foreground app transitions between excluded and not excluded.  The exclusion
list holds exe file names (e.g. "bf6.exe"), matched case-insensitively; an
entry without an extension matches the exe stem (e.g. "bf6" matches
"bf6.exe").  Logging is injected so this class has no project dependencies.
*/
class MSWindowsForegroundWatcher
{
public:
  using ExclusionCallback = std::function<void(bool excluded, const std::wstring &exeName)>;
  using Logger = std::function<void(const std::string &message)>;

  MSWindowsForegroundWatcher(const std::vector<std::wstring> &excludedApps, ExclusionCallback callback, Logger logger);
  ~MSWindowsForegroundWatcher();
  MSWindowsForegroundWatcher(const MSWindowsForegroundWatcher &) = delete;
  MSWindowsForegroundWatcher &operator=(const MSWindowsForegroundWatcher &) = delete;

  bool isForegroundExcluded() const
  {
    return m_lastExcluded;
  }

  //! Re-evaluate the current foreground window immediately
  void checkForegroundNow();

  static std::wstring toLower(const std::wstring &text);

  //! File name without directories, e.g. "C:\\a\\bf6.exe" -> "bf6.exe"
  static std::wstring exeBaseName(const std::wstring &path);

  //! Case-insensitive match of an exe base name against the exclusion list
  static bool isExcluded(const std::wstring &exeName, const std::vector<std::wstring> &excludedApps);

  //! Exe base name for a pid; empty on failure.  Falls back to a toolhelp
  //! snapshot when the process denies opening (e.g. anti-cheat protected).
  static std::wstring processImageBaseName(DWORD pid);

private:
  static void CALLBACK
  winEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD eventThread, DWORD time);
  void handleForegroundWindow(HWND hwnd);
  void log(const std::string &message) const;

  std::vector<std::wstring> m_excludedApps;
  ExclusionCallback m_callback;
  Logger m_logger;
  HWINEVENTHOOK m_hook = nullptr;
  bool m_lastExcluded = false;

  static MSWindowsForegroundWatcher *s_instance;
};
