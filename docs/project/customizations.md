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
`%APPDATA%\Deskflow\Deskflow-fch.conf` (or beside the selected portable/custom settings file):
```ini
[server]
excludedApps=bf6.exe
```
List is read once at core start (restart core after editing). The fork stores exclusions separately from stock settings. A legacy `server/excludedApps` entry in `Deskflow.conf` migrates on load; the legacy entry is removed only after a successful save. An existing fork policy, including an explicitly empty list, wins over stale legacy settings.

**Known process names**: Battlefield 6 = `bf6.exe` (verified live). Valorant deferred — verify its process name before adding.

**Notes**:
- Only the jump-zone watching is disabled; the low-level hooks stay installed but pass everything through (`kHOOK_DISABLE`), cost is negligible.
- Pause only applies while the cursor is on the primary (Windows) screen.
- macOS client is untouched; protocol unchanged.
- **Stock GUI strips legacy keys**: stock Deskflow removes fork-only keys from its own configuration. The separate exclusion file survives that cleanup, but stock binaries do not implement game exclusion. The stock service stays stopped and disabled; run the custom build (desktop shortcut "Deskflow FCH" -> `dist/<date>/deskflow.exe`, see PROJECT.md) in Desktop process mode (`processMode=1`).

### Lost BF6 exclusion policy (2026-09-22)

- Active `Deskflow.conf` lacked `server/excludedApps`; prior backups contained `bf6.exe`. Logs showed watcher startup through September21 11:55 but no watcher on later core starts. The deleting process was not identified.
- `Settings` now reads/writes exclusions in sibling `Deskflow-fch.conf`; GUI and core share this policy. Migration preserves explicit empty selections and avoids reimporting stale legacy values. Other settings retain their existing storage.
- New `ExcludedAppsPersistenceTests` uses isolated portable child processes to verify legacy migration, survival after main-config cleanup, GUI-save/core-restart persistence, explicit empty lists and switching config directories. Included in quick/exhaustive CTest; no existing protected tests changed.
- Built all validation targets; persistence/watcher/hook suites passed. Hidden quick56/58 in27.76s retained the known foreground-desktop and audio failures. Installed at `dist/2026-09-16`; backup `.ltemp/package-before-bf6-exclusion`. Restored BF6 policy; actual BF6 gameplay was not exercised.

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

### Windows boundary bounce (2026-09-21)

- Symptom: repeated Windows/Mac switches milliseconds apart while moving left into the Mac; captured in the user's clip and `deskflow.log` at 11:36:45–50.
- Cause addressed: the Windows relay hook suppresses motion. Multiple queued hook positions share the parked center; subtracting the preceding suppressed position can reverse the delta or drop repeated motion before PRE_WARP is dispatched.
- `MSWindowsScreen::onMouseMove` uses `MSWindowsMouseMotion.h` to subtract the center for remote motion and the previous position for local motion. Existing warp/mark filtering, bogus-motion rejection and coalescing remain.
- `MSWindowsMouseMotionTests`: queued leftward input at a simulated Mac right boundary with 250 Hz/unlimited sending; repeated input, reversal, both axes and local motion. Run `ctest --test-dir build/src/unittests -R MSWindowsMouseMotionTests --output-on-failure` with the documented DLL PATH.
- Windows-only change; no Mac update or protocol change. Installed at dist/2026-09-16; prior package retained in .ltemp/package-before-mouse-boundary. Core SHA256 matches rebuilt output; Mac TLS reconnected. Physical intermittent acceptance remains unverified.
- Validation: build-validation passed; mouse motion/coalescer/control-hook suites passed; hidden quick54/56 in32.56s with the same two documented failing suites (foreground watcher/audio). Requested full suite subsequently ran:60/63 in132.77s; failures MSWindowsForegroundWatcherTests, StreamingMediaTests and StreamingAudioTests. Build-validation then succeeded (up to date), fresh packaging passed29 plugins/502 dependency edges/102 PE files, and the app was reinstalled at the same path. Pre-reinstall package: .ltemp/package-before-full-suite-install. Native cross-device motion replay remains unrun. Evidence: .ltemp/proof-of-work/mouse-boundary/.

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

### Windows native V5 clipboard publication

- Complete Mac V5 bitmaps are explicitly published as CF_DIBV5 as well as CF_DIB. Windows advertises synthesized V5 for a CF_DIB-only Mac BITFIELDS image but native retrieval can fail; WPF reports CLIPBRD_E_BAD_DATA.
- Duplicate the converted handle before transferring ownership. Preserve the complete original in CF_DIBV5. Standard sRGB BGRA V5 screenshots also get a40-byte BI_RGB CF_DIB with unchanged pixel rows for legacy readers (Chromium counts V5 masks twice). Other masks/profiles and repaired legacy DIBs retain their existing representation. No Mac update/protocol change.
- MSWindowsClipboardNativeBitmapTests covers real native format retrieval and decoded RGB pixels for Mac V5, ordinary Windows and legacy repaired DIBs. ChatGPT paste acceptance remains pending; Paint/Claude transfer was user-confirmed.

## 5. Left Ctrl / Windows swap for one client (Windows server only)

**Behavior**: left Ctrl sends Mac Command; left Windows sends Mac Control. Right Ctrl/Windows, local Windows input, and other clients retain their original mappings.

**Configuration** (`%APPDATA%\Deskflow\Deskflow-fch.conf`, restart GUI/core after editing):
```ini
[server]
leftCtrlSuperSwapScreen=Fakhreddines-MacBook-Pro.local
```
Legacy values migrate from sibling `Deskflow.conf`. The fork file survives stock-config cleanup; an existing value (including an explicit empty target) wins over stale legacy settings. Empty/unset disables the swap. Use the client's canonical screen name. Keep the client's existing Ctrl/Super modifier mapping at its default; an additional client-side swap would remap the output again. Renaming the Mac requires updating this setting. No Mac build or protocol change is needed.

**Implementation**:
- `server/LeftModifierSwap.h` maps key IDs and shortcut masks per recipient, including broadcast recipients and the modifier mask sent on screen entry. The primary is always excluded.
- `MSWindowsKeyState::getModifierSides()` reads Deskflow's tracked physical keys, because suppressed Windows-key events cannot be recovered reliably from `GetAsyncKeyState`.
- `KeyState::sendKeyEvent` captures Ctrl/Super sides into `IKeyState::KeyInfo`; copies retain the snapshot. The server uses the event's snapshot rather than later keyboard state. Mixed left/right modifiers and AltGr-suppressed masks are preserved.
- `PlatformScreen` forwards the side query for screen entry. The setting is enabled only by Windows servers; other platforms retain existing behavior.
- New tests: `LeftModifierSwapTests` covers mapping/scope, all 16 left/right modifier combinations, event copies, and Windows down/up/repeat capture after physical state changes. Run from the documented test environment: `ctest --test-dir build/src/unittests -R LeftModifierSwapTests --output-on-failure`.

### Lost swap setting (2026-09-22)

- Mapping code was intact; the active configuration lacked its target. The deleting process was not identified.
- `Settings` now persists the target beside exclusions in `Deskflow-fch.conf`. Restored `Fakhreddines-MacBook-Pro.local`; installed at `dist/2026-09-16`, backup `.ltemp/package-before-modifier-restore`. Core logs confirm enabled mapping and a reconnected secure client.
- `LeftModifierSwapPersistenceTests` adds six isolated process scenarios: legacy migration plus main-config cleanup, absent/default target, stale legacy precedence, save/restart, explicit disable/restart and config-directory isolation. Loaded settings feed the real key mapper to check swap and scope. Existing mapping/event-side coverage remains unchanged.
- Build-validation and four targeted suites passed. Hidden quick57/59 in27.80s retains the known foreground-watcher/audio failures. Physical Mac shortcuts remain unverified. Logs: `.ltemp/proof-of-work/left-modifier-restore/`.
- Run `ctest --test-dir build/src/unittests -R "LeftModifierSwap|ExcludedAppsPersistence|^SettingsTests$" --output-on-failure` with the documented DLL PATH. New suite joins quick/exhaustive checks.

**Manual verification**: on the Mac, left Ctrl+C/V should act as Command+C/V and left Windows should act as Control. Confirm right Ctrl remains Control and local Windows shortcuts remain unchanged.
# Streaming session foundations

- `src/lib/streaming/`: bounded session broker, input-exporter-bound TLS 1.3 signaling and credential-checked private GUI IPC. `CoreProcess::streamingSession()` is the GUI integration API.
- Input protocol 1.8 remains unchanged. Broker port is input port + 1. PeerAuth is mandatory. Sender/receiver/control owners publish actual capabilities. Windows active-session service-core delegation uses exact-logon native IPC; privileged runtime acceptance remains unrun and session-zero capture/input remains prohibited.
- Windows deployment explicitly includes Qt OpenSSL TLS backend. Alternate executable build output is configurable while the current app is running.
- Detailed security/API/state contracts and validation limitations: `docs/project/streaming.md`.

# Native capture foundations

- `src/lib/streaming/Capture*`, `WindowsCapture*`, `WindowsWgcSource*`, `MacCapture.mm`, `PortalCapture.cpp`, `X11Capture.cpp`: source enumeration, exact-source native capture and bounded owned frames; worker-thread API consumed by the sender orchestration.
- `BUILD_STREAMING=ON` resolves GStreamer >=1.28.7 and builds its raw-frame pipeline. Windows direct WGC avoids the upstream GStreamer source's internal DXGI substitution. The approved SDK is at `C:/Work/tools/gstreamer/1.28.7`.
- Native capture is wired into the consent-gated shared sender/receiver worker; full capture acceptance remains pending. The separately approved controlled Windows-window probe produced blind-checked native captured pixels; display/lifecycle/performance acceptance remains incomplete. macOS/Linux source implementations remain uncompiled/unvalidated. Geometry/input mapping flags must gate later control. Commands and full limitations: `docs/project/streaming.md`.


# Local video-file playback

- `src/lib/streaming/FileSource.{h,cpp}` owns asynchronous local decoding, playback controls, metadata and shared-clock bounded BGRA/48-kHz stereo PCM output. `VideoFrame.timelineEpoch` distinguishes seek resets from source/geometry changes.
- Explicit filesrc plus decode-factory allowlist prevents URI/adaptive playback. Tests generate owned WebM/MP4/MOV/Matroska media; no product FFmpeg dependency. Sender and authorized receiver playback controls use FileSource. Seek events route through the selected linked video branch to the common demuxer so disabled/unlinked audio cannot reject valid file seeking.
- Commands, verified formats, test prerequisites, ownership and downstream seek flush obligations: `docs/project/streaming.md`.

# Media transport

- `src/lib/streaming/MediaTransport.{h,cpp}`: worker-owned VP8/Opus encoder, direct-interface libnice/WebRTC DTLS-SRTP, explicitly negotiated TWCC/GCC, encrypted RTP geometry/epoch metadata and bounded decoded output.
- Core private IPC adds verified local `Identity`; roster entries expose authenticated interface addresses. ICE signals carry media-line indexes. Input protocol 1.8 is unchanged.
- File/audio epoch zero is valid; exact session/source/epoch checks remain enforced. Quantized 30-FPS file timestamps use phase-based pacing.
- Sender/receiver orchestration and core-owned desktop-control implementation are present. Local transport and virtual-output evidence do not establish cross-device/native-control acceptance or physical audibility; see `docs/project/streaming.md`.

## 7. Streaming input ownership

- New direction: authenticated receiver commands control a specifically captured sender source only after interactive offer permission and explicit source-local grant. One core lease; GUI media worker never calls native injection.
- Ordinary client/server input and Windows/macOS/X11 hook dispatch honor the core ownership gates. Viewer window focus isolates local shortcuts even in view-only mode. Release ordinary ownership only after the existing native leave/key-release operation finishes.
- Preserve `server/leftCtrlSuperSwapScreen`: Windows-origin left Ctrl/Super maps once for the authenticated configured recipient name; right modifiers/other peers stay unchanged. Preserve configurable mouse coalescing (250 Hz default; 0 disables).
- Windows exclusions are passed from existing settings to the new native owner and checked on grant and each input verification. Physical input revokes the lease; marked synthetic input never echoes into existing Deskflow routing.
- Cleanup retains failed native releases and blocks new/ordinary ownership until retry succeeds; core logs persistent release refusal. Full native/cross-host coexistence acceptance remains blocked by the scoped host/approval prerequisites.

# Stream launcher and sender controls

- MainWindow embeds StreamLauncher: Stream opens source/destination/audio/quality selection; running status and Stop remain in the main window when the dialog closes.
- SenderController routes authenticated SessionClient state through a dedicated SenderWorker; capture/file/audio/media never run on the GUI or input thread.
- FileSource retains one real paused preroll frame. Receiver Ready after first decoded video unlocks initial file playback; no file audio is required before Ready.
- Offscreen component and real IPC/broker/DTLS file tests avoid the running app/user settings. Windows capture clears cached WinRT factories before apartment shutdown to prevent repeated-discovery use-after-free. Detailed APIs/limits: docs/project/streaming.md.

# Receiving viewer and file controls

- StreamViewer exposes sender/source identity, consent, exact audio endpoint, aspect-preserving video, fullscreen/Escape, Stop, private volume/mute and permission-aware file timeline.
- One SenderController/worker owns either direction and publishes capabilities once. Retired viewers clear pixels and cannot control later sessions.
- File playback permission is explicit and distinct from desktop input control. The core validates sender/receiver identity, session/source, timeline epoch, bounds and command rate.
- New epoch PCM starts the receiver clock after paused video preroll with an explicit synchronized 40-ms playout margin; authenticated Opus clipping and per-decoded-buffer metadata preserve sample alignment despite repeated local jitter PTS. Source filenames are sanitized without transmitting paths.
- Seek keyframe requests target the selected frame's transport running time; queued older frames cannot consume them. Sender rechecks command epoch after broker delivery and rejects overlapping seeks without ending the session.
- Headless real receiver and owned-process virtual-output tests preserve existing tests. Full MainWindow and cross-device physical-output/input-hook acceptance remain unverified.


## 8. Streaming recovery and privacy

- Session/lease-bound GUI queues, retained pending Start identity, early broker Stop and controller-exit IPC detach prevent stale control and retired errors crossing sessions.
- Capture has an explicit initial Starting state. Backend diagnostics are sanitized. Receiver holds one future-epoch audio block with a three-second video deadline.
- Streaming-only login/power monitor stops file/receiver/capture paths without changing ordinary lock-screen input behavior. Windows native IPC supports exact-login active-session service delegation in source; privileged runtime acceptance remains pending.
- New focused recovery, Windows IPC and environment tests and platform limitations: `docs/project/streaming.md`. macOS lock admission may be unavailable pending the explicit API/product decision; no fallback to assumed unlocked state.

- Item-10 current verification: explicit ON/OFF app/core/daemon builds and new recovery/IPC/environment/resource suites pass. Hidden full and quick remain blocked by the unchanged foreground condition and protected raw-audio-error expectation pending exact approval. Overload partial-startup recurrence remains unresolved; concurrent ordinary customization/clipboard tests pass under real media load. Exact evidence and native/platform blockers are recorded in active worklist item10.

## Streaming package integration

`BUILD_STREAMING=ON` packages the private Windows media runtime and validates dependency imports. Native macOS plugin relocation/signing and Linux package-owner dependencies have source integration but require their target hosts for build/runtime acceptance. Use `cmake --build build --target build-validation -j12` before the hidden quick/full test runner. Setup, licenses, exact command matrix and clean-runtime limitations: [streaming-packaging.md](streaming-packaging.md).

- FileSource converts decoded video/PCM sample PTS through the GStreamer segment into media stream time. This fixes MP4 reorder/edit-list offsets in playback and diagnostic frame timestamps; initial and seek positions have real H264/AAC integration coverage.

- Streaming recovery ends observed authenticated-source progress without decoded frames after3s through bilateral Stop. Static/paused sources remain valid; full video blackhole liveness and real-device/reference performance remain acceptance limitations. See streaming.md and active worklist item12.


## Paired-computer automatic stream acceptance

- MainWindow opts in; reusable launchers/viewers default to manual mode. Viewer opens before Accept; audio uses the native default output or waits for selection. Stop remains available and desktop control still needs a separate grant.
- Windows-tested patch applied on Mac without changing existing clipboard converters. CoreAudio default UID and isolated broker/IPC acceptance tests pass. Installed app reconnects over TLS 1.3; native media and cross-device acceptance remain limited by the macOS session-state check. See `macos-validation.md`.
