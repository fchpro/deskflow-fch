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
