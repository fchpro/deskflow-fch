# Fork Customizations

Personal customizations on top of upstream Deskflow. Keep this list current; read before upstream merges.

## 1. Game/app exclusion — pause input sharing while an excluded app is foreground (Windows server only)

**Goal**: while an excluded app (fps games) owns the foreground window, Deskflow's hooks are disabled so input stays fully local; resumes instantly when focus leaves the app. Measured event→callback latency: ~0.4–1.5 ms (target <20 ms).

**Files**:
- `src/lib/platform/MSWindowsForegroundWatcher.{h,cpp}` — new. `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)` (out-of-context, delivered on the screen thread's message pump). Resolves foreground pid → exe base name (`QueryFullProcessImageNameW`, toolhelp-snapshot fallback for anti-cheat-protected processes like bf6). Case-insensitive match; entries without extension match the exe stem. Logger injected (no project deps) so the file compiles standalone.
- `src/lib/platform/MSWindowsScreen.{h,cpp}` — modified. Primary screen creates the watcher when the list is non-empty; callback `handleExcludedAppChange` sets `m_hook.setMode(kHOOK_DISABLE)` while excluded and on-screen, restores `kHOOK_WATCH_JUMP_ZONE` on resume. `enable()`/`enter()` respect `m_excludedAppActive`.
- `src/lib/common/Settings.h` — modified. New key `server/excludedApps` (`Settings::Server::ExcludedApps`, QStringList) + validKeys entry.
- `src/lib/platform/CMakeLists.txt`, `src/unittests/platform/CMakeLists.txt` — new sources/test registered.
- `src/unittests/platform/MSWindowsForegroundWatcherTests.{h,cpp}` — new Qt Test for the pure helpers.

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
