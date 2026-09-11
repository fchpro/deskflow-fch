# Fork Customizations

Personal customizations on top of upstream Deskflow. Keep this list current; read before upstream merges.

## 1. Game/app exclusion — pause input sharing while an excluded app is foreground (Windows server only)

**Goal**: while an excluded app (fps games) owns the foreground window, Deskflow's hooks are disabled so input stays fully local; resumes instantly when focus leaves the app. Measured event→callback latency: ~0.4–1.5 ms (target <20 ms).

**Root cause of the 2026-08 "still shares while bf6 is in front" bug** (from `deskflow.log`): `bf6.exe` pause followed 13 ms later by an `explorer.exe` resume — the foreground flickers to explorer during the fullscreen mode switch, and the game regaining focus fires no further `EVENT_SYSTEM_FOREGROUND`. A single event hook can therefore end in the wrong state. Fixed with redundant layers (all active simultaneously):

| Layer | Where | Mechanism |
|---|---|---|
| 1 | `MSWindowsForegroundWatcher` | WinEvent hooks: `EVENT_SYSTEM_FOREGROUND..EVENT_SYSTEM_MINIMIZEEND` (foreground, alt-tab switch, minimize) + `EVENT_OBJECT_FOCUS`; every event re-evaluates `GetForegroundWindow()` (event hwnd ignored). |
| 2 | `MSWindowsForegroundWatcher` | Poll timer (`kPollIntervalMs` = 100 ms, message-only window `WM_TIMER`) re-evaluates the foreground independent of events. |
| 3 | `MSWindowsForegroundWatcher` | Toolhelp snapshot every `kPidRefreshMs` = 1000 ms lists pids of running excluded exes; a foreground pid in the set is excluded with no `OpenProcess` (anti-cheat denies it). Pid→name cache cleared on every refresh (pid reuse). |
| 4 | `ExclusionDecider` (pure) | Pause immediate; resume only after the foreground stays non-excluded for `kResumeDelayMs` = 300 ms; an unresolvable foreground never changes state and resets the resume timer. |
| 5 | `MSWindowsHook` | `setExcludedPids()` (max 64) — in `kHOOK_WATCH_JUMP_ZONE` the low-level mouse hook passes events through untouched when the foreground pid is excluded (`isForegroundExcluded()`), regardless of the screen thread state. |
| 6 | `MSWindowsScreen::onMouseMove` | Motion on the primary screen is not reported to the server (`isExcludedAppForeground()`: watcher state or live pid check) → the server can never jump screens. |
| 7 | `MSWindowsScreen::handleFixes` (1 s) | Watchdog: `checkForegroundNow()` + re-asserts `kHOOK_DISABLE` (logs a warning) if hooks are active while paused. |

**Files**:
- `src/lib/platform/MSWindowsForegroundWatcher.{h,cpp}` — new. Layers 1–4 above plus `ExclusionDecider`. Resolves pid → exe base name (`QueryFullProcessImageNameW`, toolhelp fallback). Case-insensitive match; entries without extension match the exe stem. Logger injected (no project deps). Log lines carry the trigger source: `(foreground event)`, `(window event)`, `(poll)`, `(startup)`, `(manual)`.
- `src/lib/platform/MSWindowsHook.{h,cpp}` — modified. Layer 5 (`setExcludedPids`, `isPidExcluded`, `isForegroundExcluded`, `getMode`).
- `src/lib/platform/MSWindowsScreen.{h,cpp}` — modified. Primary screen creates the watcher when the list is non-empty; callback `handleExcludedAppChange` sets `m_hook.setMode(kHOOK_DISABLE)` while excluded and on-screen, restores `kHOOK_WATCH_JUMP_ZONE` on resume. `enable()`/`enter()` respect `m_excludedAppActive`. Layers 6–7.
- `src/lib/common/Settings.h` — modified. New key `server/excludedApps` (`Settings::Server::ExcludedApps`, QStringList) + validKeys entry.
- `src/lib/platform/CMakeLists.txt`, `src/unittests/platform/CMakeLists.txt` — new sources/tests registered.
- `src/unittests/platform/MSWindowsForegroundWatcherTests.{h,cpp}` — pure helpers, `findExcludedPids`, `ExclusionDecider` (incl. the 13 ms flicker regression), live watcher startup detection.
- `src/unittests/platform/MSWindowsHookTests.{h,cpp}` — excluded pid list / foreground guard.

**GUI (fork additions, `src/lib/gui`)**:
- `core/ForegroundAppMonitor.{h,cpp}` — 500 ms poll of the foreground exe/title in the GUI process; feeds the main window label `lblForegroundApp` ("Foreground: bf6.exe (excluded, sharing paused)", bold when excluded). Windows only; hidden elsewhere.
- `core/ProcessList.{h,cpp}` — running process enumeration (toolhelp + `EnumWindows` titles) and pure `dedupeByExe` / `filter` / `sorted` / `displayText` helpers.
- `dialogs/ExcludedAppsDialog.{h,cpp}` — opened by the `Excluded Apps` button next to `Configure Server`. Left: current list + Remove. Right: running processes ("exe - window title", titled first) with a search box; Add / double-click adds the exe name. OK writes `server/excludedApps`, flushes the conf, restarts the core if running.
- Tests: `src/unittests/gui/core/ProcessListTests`, `src/unittests/gui/ExcludedAppsDialogTests` (structure, search, add/remove, monitor matching).

**Configuration** (edit as the app list grows):
`%APPDATA%\Deskflow\Deskflow.conf`:
```ini
[server]
excludedApps=bf6.exe
```
List is read once at core start (restart core after editing). Caveat: the stock upstream build may strip this unknown key from the conf when it saves settings; re-add after switching to the custom build.

**Known process names**: Battlefield 6 = `bf6.exe` (verified live). Valorant deferred — verify its process name before adding.

**Notes**:
- Only the jump-zone watching is disabled; the low-level hooks stay installed but pass everything through (`kHOOK_DISABLE`), cost is negligible.
- Pause only applies while the cursor is on the primary (Windows) screen.
- macOS client is untouched; protocol unchanged.
- **Stock GUI strips the key**: the upstream/stock Deskflow GUI removes `excludedApps` from the conf (unknown key cleanup). Never run the stock GUI while using this feature. The stock service is stopped and disabled; run the custom build (desktop shortcut "Deskflow FCH") in Desktop process mode (`processMode=1`).

## 2. Pause/resume toast (Windows server only)

Small silent popup (bottom-right of work area, ~1 s, no sound, never steals focus) whenever pause/resume triggers: "Deskflow paused — bf6.exe" / "Deskflow resumed — bf6.exe".

**Files**:
- `src/lib/platform/MSWindowsPauseToast.{h,cpp}` — new. Plain Win32 topmost `WS_EX_NOACTIVATE` popup, GDI-drawn, `WM_TIMER` auto-close. No project deps (logger-free), compiles standalone.
- `src/lib/platform/MSWindowsScreen.cpp` — `handleExcludedAppChange` shows the toast on every state change while the server is enabled.
- `src/unittests/platform/MSWindowsPauseToastTests.{h,cpp}` — visibility, bottom-right position, no-activate styles, auto-close, text replacement.

**Limitation**: the toast cannot render over an exclusive-fullscreen game; it is visible on the desktop and over borderless/windowed apps. Non-ASCII characters in toast literals must use `\uXXXX` escapes (MSVC source-charset mojibake otherwise).

## 3. Mouse send-rate limiter (server, all platforms)

**Problem**: a 1000 Hz mouse produces 1000 tiny TCP messages/s while the cursor is on the Mac; over the Windows Mobile Hotspot link this caused seconds of input lag. Lowering the mouse polling rate in G HUB fixed it, but that also affects games.

**Solution**: coalesce mouse deltas server-side before they are sent to clients. Local Windows input is untouched (the hook still runs at full rate).

**Files**:
- `src/lib/server/MouseMoveCoalescer.h` - new, header-only, dependency-free. Accumulates `dx,dy`; releases at most once per interval; `timeUntilFlushUs()` for the tail timer.
- `src/lib/server/Server.{h,cpp}` - `onMouseMoveSecondary` routes deltas through the coalescer; `applyMouseMoveSecondary` holds the original clamp/switch/send logic; one-shot `m_mouseFlushTimer` flushes the tail of a motion; pending motion is flushed before button/wheel events and dropped on `switchScreen`.
- `src/lib/common/Settings.{h,cpp}` - new key `server/mouseSendRateHz` (default `250`, `0` = upstream behaviour: every hook event sent).
- `src/unittests/server/MouseMoveCoalescerTests.{h,cpp}` - unit tests (rate limit 1000 Hz to 250 Hz, accumulation, flush, reset).

**Configuration** (`%APPDATA%\Deskflow\Deskflow.conf`, restart core after editing):
```ini
[server]
mouseSendRateHz=250
```

## 4. Clipboard image sharing — larger default size limit

**Problem**: image clipboard sync silently failed. Bitmaps travel as uncompressed 32bpp DIBs, so any screenshot (1685x1116 = 7.5 MB, 2560x1440 = 14.7 MB) exceeded the upstream default limit of 3 MiB. Log showed `WARNING: not sending clipboard data, exceeds limit: 3072 KB`.

**Solution**: raise the default `server/clipboardSize` from 3 to 128 MiB (`src/lib/common/Settings.cpp`, `ServerConfigDialog.ui` default). No protocol change; stock clients stay compatible. The limit is applied on the server (send + receive) and pushed to clients as their send limit.

**Regression test**: `SettingsTests::defaultClipboardSizeFitsScreenshot` (default limit must exceed a 2560x1440 32bpp DIB).

**Configuration** (`%APPDATA%\Deskflow\Deskflow.conf`, restart core after editing):
```ini
[server]
clipboardSize=128
```

**Limitation**: a client enforces its *receive* limit from its own local `server/clipboardSize` setting (default 3 MiB on stock builds since July 2026). For PC -> Mac images, set `clipboardSize=128` under `[server]` in the Mac's `~/Library/Deskflow/Deskflow.conf` and restart Deskflow there. Mac -> PC needs no Mac change.

## 5. Left Ctrl / Windows swap for one client (Windows server only)

**Behavior**: left Ctrl sends Mac Command; left Windows sends Mac Control. Right Ctrl/Windows, local Windows input, and other clients retain their original mappings.

**Configuration** (`%APPDATA%\Deskflow\Deskflow.conf`, restart GUI/core after editing):
```ini
[server]
leftCtrlSuperSwapScreen=Fakhreddines-MacBook-Pro.local
```
Empty/unset disables the swap. Use the client's canonical screen name. Keep the client's existing Ctrl/Super modifier mapping at its default; an additional client-side swap would remap the output again. Renaming the Mac requires updating this setting. No Mac build or protocol change is needed.

**Implementation**:
- `server/LeftModifierSwap.h` maps key IDs and shortcut masks per recipient, including broadcast recipients and the modifier mask sent on screen entry. The primary is always excluded.
- `MSWindowsKeyState::getModifierSides()` reads Deskflow's tracked physical keys, because suppressed Windows-key events cannot be recovered reliably from `GetAsyncKeyState`.
- `KeyState::sendKeyEvent` captures Ctrl/Super sides into `IKeyState::KeyInfo`; copies retain the snapshot. The server uses the event's snapshot rather than later keyboard state. Mixed left/right modifiers and AltGr-suppressed masks are preserved.
- `PlatformScreen` forwards the side query for screen entry. The setting is enabled only by Windows servers; other platforms retain existing behavior.
- New tests: `LeftModifierSwapTests` covers mapping/scope, all 16 left/right modifier combinations, event copies, and Windows down/up/repeat capture after physical state changes. Run from the documented test environment: `ctest --test-dir build/src/unittests -R LeftModifierSwapTests --output-on-failure`.

**Manual verification**: on the Mac, left Ctrl+C/V should act as Command+C/V and left Windows should act as Control. Confirm right Ctrl remains Control and local Windows shortcuts remain unchanged.
