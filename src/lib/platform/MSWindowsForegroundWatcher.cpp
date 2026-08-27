/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "platform/MSWindowsForegroundWatcher.h"

#include <TlHelp32.h>

#include <algorithm>

namespace {
constexpr UINT kPollTimerId = 1;
constexpr wchar_t kTimerWindowClass[] = L"DeskflowForegroundWatcher";

unsigned long long nowMs()
{
  return GetTickCount64();
}
} // namespace

//
// ExclusionDecider
//

ExclusionDecider::Transition ExclusionDecider::update(Foreground foreground, unsigned long long now)
{
  switch (foreground) {
  case Foreground::Unknown:
    // never trust an unresolved foreground: keep the current state and do
    // not let it count towards resuming
    m_resumePending = false;
    return Transition::None;

  case Foreground::Excluded:
    m_resumePending = false;
    if (!m_paused) {
      m_paused = true;
      return Transition::Pause;
    }
    return Transition::None;

  case Foreground::NotExcluded:
    if (!m_paused) {
      return Transition::None;
    }
    if (!m_resumePending) {
      m_resumePending = true;
      m_notExcludedSinceMs = now;
    }
    if (now - m_notExcludedSinceMs >= m_resumeDelayMs) {
      m_paused = false;
      m_resumePending = false;
      return Transition::Resume;
    }
    return Transition::None;
  }
  return Transition::None;
}

//
// MSWindowsForegroundWatcher
//

MSWindowsForegroundWatcher *MSWindowsForegroundWatcher::s_instance = nullptr;

MSWindowsForegroundWatcher::MSWindowsForegroundWatcher(
    const std::vector<std::wstring> &excludedApps, ExclusionCallback callback, Logger logger,
    PidsCallback pidsCallback
)
    : m_callback(std::move(callback)),
      m_pidsCallback(std::move(pidsCallback)),
      m_logger(std::move(logger))
{
  m_excludedApps.reserve(excludedApps.size());
  for (const auto &app : excludedApps) {
    if (!app.empty()) {
      m_excludedApps.push_back(toLower(app));
    }
  }

  s_instance = this;

  // layer 1: WinEvent hooks.  out-of-context: delivered via this thread's
  // message pump.  the system range covers FOREGROUND, MENU*, CAPTURE*,
  // MOVESIZE*, DIALOG*, SWITCHSTART/END (alt-tab) and MINIMIZESTART/END;
  // all cheap, every one re-evaluates GetForegroundWindow().
  m_hookSystem = SetWinEventHook(
      EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_MINIMIZEEND, nullptr, &MSWindowsForegroundWatcher::winEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
  );
  m_hookFocus = SetWinEventHook(
      EVENT_OBJECT_FOCUS, EVENT_OBJECT_FOCUS, nullptr, &MSWindowsForegroundWatcher::winEventProc, 0, 0,
      WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS
  );
  if (m_hookSystem == nullptr || m_hookFocus == nullptr) {
    log("failed to install foreground app watcher event hooks (polling still active)");
  }

  // layer 2: poll timer on a message-only window
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = &MSWindowsForegroundWatcher::timerWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kTimerWindowClass;
  RegisterClassExW(&wc);
  m_timerWindow = CreateWindowExW(
      0, kTimerWindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr
  );
  if (m_timerWindow == nullptr || SetTimer(m_timerWindow, kPollTimerId, kPollIntervalMs, nullptr) == 0) {
    log("failed to install foreground app poll timer");
  }

  log("foreground app watcher installed, excluded apps: " + std::to_string(m_excludedApps.size()) + ", poll " +
      std::to_string(kPollIntervalMs) + " ms, resume delay " + std::to_string(kResumeDelayMs) + " ms");

  // layer 3: snapshot of running excluded pids
  refreshExcludedPids();
  evaluate("startup");
}

MSWindowsForegroundWatcher::~MSWindowsForegroundWatcher()
{
  if (m_timerWindow != nullptr) {
    KillTimer(m_timerWindow, kPollTimerId);
    DestroyWindow(m_timerWindow);
    m_timerWindow = nullptr;
  }
  UnregisterClassW(kTimerWindowClass, GetModuleHandleW(nullptr));
  if (m_hookSystem != nullptr) {
    UnhookWinEvent(m_hookSystem);
    m_hookSystem = nullptr;
  }
  if (m_hookFocus != nullptr) {
    UnhookWinEvent(m_hookFocus);
    m_hookFocus = nullptr;
  }
  s_instance = nullptr;
}

void MSWindowsForegroundWatcher::checkForegroundNow()
{
  evaluate("manual");
}

bool MSWindowsForegroundWatcher::isForegroundPidExcluded() const
{
  const HWND hwnd = GetForegroundWindow();
  if (hwnd == nullptr) {
    return false;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid != 0 && m_excludedPids.count(pid) != 0;
}

std::vector<DWORD> MSWindowsForegroundWatcher::excludedPids() const
{
  return {m_excludedPids.begin(), m_excludedPids.end()};
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

std::vector<DWORD> MSWindowsForegroundWatcher::findExcludedPids(const std::vector<std::wstring> &excludedApps)
{
  std::vector<DWORD> pids;
  if (excludedApps.empty()) {
    return pids;
  }
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snapshot == INVALID_HANDLE_VALUE) {
    return pids;
  }
  PROCESSENTRY32W entry = {};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snapshot, &entry)) {
    do {
      if (isExcluded(entry.szExeFile, excludedApps)) {
        pids.push_back(entry.th32ProcessID);
      }
    } while (Process32NextW(snapshot, &entry));
  }
  CloseHandle(snapshot);
  return pids;
}

std::string MSWindowsForegroundWatcher::narrow(const std::wstring &text)
{
  std::string result;
  result.reserve(text.size());
  for (const auto ch : text) {
    result.push_back(ch < 128 ? static_cast<char>(ch) : '?');
  }
  return result;
}

void CALLBACK MSWindowsForegroundWatcher::winEventProc(HWINEVENTHOOK, DWORD event, HWND, LONG, LONG, DWORD, DWORD)
{
  if (s_instance == nullptr) {
    return;
  }
  // the event's hwnd is deliberately ignored: GetForegroundWindow() is the
  // truth, whatever window the event was about.
  s_instance->evaluate(event == EVENT_SYSTEM_FOREGROUND ? "foreground event" : "window event");
}

LRESULT CALLBACK MSWindowsForegroundWatcher::timerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_TIMER && wParam == kPollTimerId && s_instance != nullptr) {
    if (nowMs() - s_instance->m_lastPidRefreshMs >= kPidRefreshMs) {
      s_instance->refreshExcludedPids();
    }
    s_instance->evaluate("poll");
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void MSWindowsForegroundWatcher::refreshExcludedPids()
{
  m_lastPidRefreshMs = nowMs();
  const auto pids = findExcludedPids(m_excludedApps);
  std::unordered_set<DWORD> fresh(pids.begin(), pids.end());
  // pid reuse: a cached name is only valid for the lifetime of a snapshot
  m_pidNameCache.clear();
  if (fresh == m_excludedPids) {
    return;
  }
  m_excludedPids = std::move(fresh);
  log("excluded app processes running: " + std::to_string(m_excludedPids.size()));
  if (m_pidsCallback) {
    m_pidsCallback(pids);
  }
}

ExclusionDecider::Foreground MSWindowsForegroundWatcher::classifyForeground(std::wstring &exeName)
{
  const HWND hwnd = GetForegroundWindow();
  if (hwnd == nullptr) {
    return ExclusionDecider::Foreground::Unknown;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0) {
    return ExclusionDecider::Foreground::Unknown;
  }

  if (m_excludedPids.count(pid) != 0) {
    // known excluded pid: no OpenProcess needed (anti-cheat denies it)
    if (auto cached = m_pidNameCache.find(pid); cached != m_pidNameCache.end()) {
      exeName = cached->second;
    } else {
      const auto name = processImageBaseName(pid);
      exeName = name.empty() ? (m_lastExeName.empty() ? L"excluded process" : m_lastExeName) : name;
      if (!name.empty()) {
        m_pidNameCache.emplace(pid, name);
      }
    }
    return ExclusionDecider::Foreground::Excluded;
  }

  auto cached = m_pidNameCache.find(pid);
  if (cached == m_pidNameCache.end()) {
    const auto name = processImageBaseName(pid);
    if (name.empty()) {
      return ExclusionDecider::Foreground::Unknown;
    }
    cached = m_pidNameCache.emplace(pid, name).first;
  }
  exeName = cached->second;
  if (isExcluded(exeName, m_excludedApps)) {
    // the snapshot missed it (started since the last refresh)
    m_excludedPids.insert(pid);
    if (m_pidsCallback) {
      m_pidsCallback(excludedPids());
    }
    return ExclusionDecider::Foreground::Excluded;
  }
  return ExclusionDecider::Foreground::NotExcluded;
}

void MSWindowsForegroundWatcher::evaluate(const char *source)
{
  std::wstring exeName;
  const auto foreground = classifyForeground(exeName);
  if (foreground == ExclusionDecider::Foreground::Excluded) {
    m_lastExeName = exeName;
  }
  const auto transition = m_decider.update(foreground, nowMs());
  if (transition == ExclusionDecider::Transition::None) {
    return;
  }

  const bool excluded = (transition == ExclusionDecider::Transition::Pause);
  // on resume, report the app we were paused for (the toast reads better)
  const auto reported = excluded ? exeName : m_lastExeName;
  log("foreground app \"" + narrow(exeName) + "\" (" + source + ") " +
      (excluded ? "is excluded, pausing input sharing" : "is not excluded, resuming input sharing"));

  if (m_callback) {
    m_callback(excluded, reported);
  }
}

void MSWindowsForegroundWatcher::log(const std::string &message) const
{
  if (m_logger) {
    m_logger(message);
  }
}
