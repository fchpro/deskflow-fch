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
#include <unordered_map>
#include <unordered_set>
#include <vector>

//! Pure pause/resume state machine for the excluded-app feature
/*!
Pausing is immediate.  Resuming requires the foreground to stay
non-excluded for `resumeDelayMs` (the foreground flickers to explorer.exe
for ~10 ms while a fullscreen game switches modes, and the game regaining
focus fires no further foreground event).  An unknown foreground (exe could
not be resolved) never changes the state and never starts the resume timer.
*/
class ExclusionDecider
{
public:
  enum class Foreground
  {
    Excluded,
    NotExcluded,
    Unknown
  };
  enum class Transition
  {
    None,
    Pause,
    Resume
  };

  explicit ExclusionDecider(unsigned resumeDelayMs) : m_resumeDelayMs(resumeDelayMs)
  {
  }

  Transition update(Foreground foreground, unsigned long long nowMs);

  bool paused() const
  {
    return m_paused;
  }

private:
  unsigned m_resumeDelayMs;
  bool m_paused = false;
  bool m_resumePending = false;
  unsigned long long m_notExcludedSinceMs = 0;
};

//! Watches the foreground window and reports when an excluded app owns it
/*!
Several independent detection mechanisms run at once so a missed event can
never leave input sharing active while an excluded app is in front:

1. WinEvent hooks (foreground, alt-tab switch, minimize, focus) — every
   event re-evaluates GetForegroundWindow() instead of trusting the event's
   window.
2. A thread timer polls GetForegroundWindow() every `pollIntervalMs`.
3. Every `pidRefreshMs` a toolhelp snapshot lists the pids of all running
   excluded exes; a foreground pid in that set is excluded without any
   OpenProcess call (anti-cheat protected games deny it).  The set is also
   handed to the caller (`PidsCallback`) so the low-level mouse hook can
   check it directly.
4. Resume is debounced (see ExclusionDecider), pause is immediate.

The exclusion list holds exe file names (e.g. "bf6.exe"), matched
case-insensitively; an entry without an extension matches the exe stem.
Logging is injected so this class has no project dependencies.  Must be
constructed on a thread that pumps messages.
*/
class MSWindowsForegroundWatcher
{
public:
  using ExclusionCallback = std::function<void(bool excluded, const std::wstring &exeName)>;
  using PidsCallback = std::function<void(const std::vector<DWORD> &excludedPids)>;
  using Logger = std::function<void(const std::string &message)>;

  static constexpr unsigned kPollIntervalMs = 100;
  static constexpr unsigned kResumeDelayMs = 300;
  static constexpr unsigned kPidRefreshMs = 1000;

  MSWindowsForegroundWatcher(
      const std::vector<std::wstring> &excludedApps, ExclusionCallback callback, Logger logger,
      PidsCallback pidsCallback = nullptr
  );
  ~MSWindowsForegroundWatcher();
  MSWindowsForegroundWatcher(const MSWindowsForegroundWatcher &) = delete;
  MSWindowsForegroundWatcher &operator=(const MSWindowsForegroundWatcher &) = delete;

  bool isForegroundExcluded() const
  {
    return m_decider.paused();
  }

  //! Re-evaluate the current foreground window immediately
  void checkForegroundNow();

  //! Cheap live check: is the current foreground pid one of the known
  //! excluded pids (no name resolution, safe to call per mouse event)
  bool isForegroundPidExcluded() const;

  //! Pids of running excluded exes from the last snapshot
  std::vector<DWORD> excludedPids() const;

  static std::wstring toLower(const std::wstring &text);

  //! File name without directories, e.g. "C:\a\bf6.exe" -> "bf6.exe"
  static std::wstring exeBaseName(const std::wstring &path);

  //! Case-insensitive match of an exe base name against the exclusion list
  static bool isExcluded(const std::wstring &exeName, const std::vector<std::wstring> &excludedApps);

  //! Exe base name for a pid; empty on failure.  Falls back to a toolhelp
  //! snapshot when the process denies opening (e.g. anti-cheat protected).
  static std::wstring processImageBaseName(DWORD pid);

  //! Pids of every running process whose exe matches the exclusion list
  static std::vector<DWORD> findExcludedPids(const std::vector<std::wstring> &excludedApps);

  static std::string narrow(const std::wstring &text);

private:
  static void CALLBACK
  winEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD eventThread, DWORD time);
  static LRESULT CALLBACK timerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

  void evaluate(const char *source);
  void refreshExcludedPids();
  ExclusionDecider::Foreground classifyForeground(std::wstring &exeName);
  void log(const std::string &message) const;

  std::vector<std::wstring> m_excludedApps;
  ExclusionCallback m_callback;
  PidsCallback m_pidsCallback;
  Logger m_logger;
  HWINEVENTHOOK m_hookSystem = nullptr;
  HWINEVENTHOOK m_hookFocus = nullptr;
  HWND m_timerWindow = nullptr;
  ExclusionDecider m_decider{kResumeDelayMs};
  std::unordered_set<DWORD> m_excludedPids;
  std::unordered_map<DWORD, std::wstring> m_pidNameCache;
  unsigned long long m_lastPidRefreshMs = 0;
  std::wstring m_lastExeName;

  static MSWindowsForegroundWatcher *s_instance;
};
