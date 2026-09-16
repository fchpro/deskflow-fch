# PROJECT.md — deskflow-fch

## Overview

Personal fork of [Deskflow](https://github.com/deskflow/deskflow), an open-source keyboard/mouse sharing app (share one keyboard and mouse across multiple computers over the network, client/server model). This fork exists to apply personal customizations while staying updatable from upstream releases. Not intended for redistribution.

## Tech Stack

- C++20
- CMake (>= 3.24)
- Qt 6 (GUI)
- vcpkg optional on Windows (`VCPKG_QT` option)
- Qt Test (unit tests in `src/unittests`)
- Platform backends: Windows, macOS, Linux (X11/Wayland)

- GStreamer 1.28.7 SDK (optional `BUILD_STREAMING=ON`; native capture/audio and VP8/Opus transport with sender/receiver GUI and file playback implemented; cross-device acceptance pending). Windows SDK installed at `C:\Work\tools\gstreamer\1.28.7`.
- Native capture: WinRT WGC/D3D11 on Windows; ScreenCaptureKit and Linux portal/PipeWire/XComposite source implementations await remote builds and acceptance. See `docs/project/streaming.md`.

## Project Structure

- `CMakeLists.txt` — root build config, version (currently 1.26.0 fallback, git-derived when available)
- `src/apps/deskflow-core` — core CLI app (client/server engine)
- `src/apps/deskflow-daemon` — background daemon
- `src/apps/deskflow-gui` — Qt GUI app
- `src/lib/` — libraries: `arch` (OS abstraction), `base`, `client`, `server`, `net`, `platform` (per-OS input/screen), `deskflow` (core logic), `gui`, `common`, `io`, `mt`
- `src/lib/streaming/` — authenticated session broker/TLS/private IPC, core-owned desktop-control leases/native input, capture adapters, bounded frames/FileSource playback, audio and VP8/Opus transport
- `src/lib/gui/streaming/` — sender launcher, receiving viewer, authorized file controls and one consent-gated media worker for both roles
- `src/unittests/` — Qt Test unit/integration tests mirroring lib layout
- `tools/check_headless.py` — runs unchanged CTest on an unselected Windows desktop so native test windows stay hidden
- `tools/capture_probe.py` / `tools/CaptureProbe.cpp` — controlled native capture fixture on an unselected desktop; capture rejection is reported without switching desktops
- `tools/control_probe.py` / `tools/ControlProbe.cpp` — owned control fixture; default inactive-desktop rejection without injection, optional separately authorized native input through real TLS/private IPC
- `cmake/` — CMake modules
- `deploy/` — packaging/installer resources
- `docs/` — upstream docs (`docs/dev/build.md` = build instructions)
- `translations/` — Qt translation files
- `docs/project/` — fork-specific reference docs (create as needed)
- `docs/tasks/` — active task files
- `docs/worklists/` — staged multi-item worklists; finished folders move under `docs/worklists/done/`

## Fork & Update Workflow

- `master` — mirrors upstream Deskflow; never customized directly.
- `fch` — custom branch; all personal edits live here.
- Remote `origin` = `fchpro/deskflow-fch` (personal GitHub fork).
- Update flow when upstream releases a new version:
  1. User fetches/pulls upstream into `master` (git mutations are user-run).
  2. Rebase or merge `master` into the custom branch (user-run).
  3. Resolve conflicts, rebuild, re-verify customizations.
- Keep customizations small and isolated to ease upstream merges; document each one in `docs/project/customizations.md`.

## Detailed Documentation Guide

| Doc | Topic | When to read |
|---|---|---|
| `docs/dev/build.md` | Upstream build instructions | Before building |
| `docs/Configuration.md` | App configuration | When changing config behavior |
| `docs/dev/protocol_reference.md` | Network protocol | When touching client/server/net |
| `docs/project/customizations.md` | List of personal customizations | Before any edit or upstream update |
| `docs/project/streaming-packaging.md` | Private media runtime, native package prerequisites, notices and validation matrix | Before packaging or diagnosing installed streaming |
| `docs/project/streaming.md` | Streaming/media architecture, control ownership and native permission contracts, dependencies, validation and platform limits | Before streaming implementation or validation |

## Unfinished Tasks and Worklists

- `docs/tasks/left-modifier-swap-verification.md` — Mac-side visual verification after enabling the left Ctrl/Windows swap.
- `docs/worklists/2026-09-13-1250/worklist-2026-09-13-1250.md` — complete screen/window/video streaming feature; phase 2 sequential execution authorized.

## Quick Check

Quick check = run fast registered tests against the last build (~24 s locally; extended media/network scenarios excluded by label). From a VS x64 dev prompt (`VsDevCmd.bat -arch=x64`) with `C:\Qt\6.10.3\msvc2022_64\bin` and `build\vcpkg_installed\x64-windows-release\bin` prepended to PATH (also `C:\Work\tools\gstreamer\1.28.7\bin` when `BUILD_STREAMING=ON`):

```
cd build\src\unittests && ctest -LE streaming-extended --output-on-failure
```

Full validation (minutes): `cmake --build build --target build-validation -j12` then the hidden exhaustive command below. `build-validation` builds every registered C++ test consumer and application without launching tests. The default `all` target also runs CTest and can create native test windows; without Qt on PATH tests exit 0xc0000135.

- Headless Windows quick check: `python tools/check_headless.py --log temp/proof-of-work/quick-check.txt` with the same Qt/vcpkg PATH. Runs the quick selection on a separate unselected Windows desktop; never switches the user's desktop. `QT_QPA_PLATFORM=offscreen` alone cannot hide native Win32 test windows.
- Known headless limitation: unchanged `MSWindowsForegroundWatcherTests::watcherStartupDetectsForeground` requires an active foreground desktop and fails on an unselected desktop. A fully passing check currently needs authorized native test windows; the hidden runner reports this failure without skipping or altering that test.
- Current item-10 protected-test blocker: StreamingAudioTests::asynchronousDeviceFailure expects a raw backend diagnostic; sanitized runtime output intentionally fails that old expectation. Exact protected assertion update awaits approval. Hidden checks therefore currently fail this test and the foreground watcher; overload also has a separately retained intermittent full-suite failure. See active worklist item10.
- Focused streaming integration: `ctest --test-dir build/src/unittests -R Streaming --output-on-failure` (real TLS loopback and private IPC; no visible windows).
- Exhaustive automated test command (`test:all` equivalent): `ctest --test-dir build/src/unittests --output-on-failure`; includes extended media/network scenarios beyond the quick-check budget. Use `python tools/check_headless.py --full --log temp/proof-of-work/full-check.txt` for exhaustive hidden Windows execution. Capture frame-pipeline tests use real GStreamer raw-video production. File playback tests generate owned fixtures with the test-only FFmpeg CLI on PATH and exercise real installed GStreamer decoders; no user media or visible windows. The explicitly approved controlled Windows-window probe captured changing native pixels; full source/lifecycle/platform acceptance remains incomplete. Build changed targets first; a cached test result does not validate changed source.
- Build while current app binaries are running: configure `-DCMAKE_RUNTIME_OUTPUT_DIRECTORY=<absolute-alternate-output>` then build normally. This cache option defaults to `<build>/bin` and is currently reset to `build/bin` (2026-09-16). `build/bin-streaming-validation` holds older validation outputs.
- Installed app the user runs: desktop and taskbar-pinned shortcuts `Deskflow FCH` -> `dist/<YYYY-MM-DD>/deskflow.exe` (currently `dist/2026-09-16`, gitignored). Update flow: build `deskflow deskflow-core deskflow-daemon`, then `cmake --install build --prefix C:/Work/projects/deskflow-fch/dist/<new-date>` (fresh directory; bundles Qt, CRT and the private GStreamer runtime), repoint the shortcut. Running from `build/bin` needs GStreamer `bin` on PATH; the shortcut has none.

**Local toolchain**: MSVC 2022 Community, Qt 6.10.3 at `C:\Qt\6.10.3\msvc2022_64` (installed via aqtinstall), vcpkg at `C:\Work\tools\vcpkg` (openssl via manifest), Ninja via pip (`%APPDATA%\Python\Python313\Scripts\ninja.exe`). Configure command used: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.3\msvc2022_64 -DCMAKE_TOOLCHAIN_FILE=C:\Work\tools\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release -DBUILD_INSTALLER=OFF`.

## Master Tests

Unit tests (Qt Test): 56 C++ test binaries plus one Python package suite / 60 CTest registrations with `BUILD_STREAMING=ON` (25 upstream + 31 fork C++ binaries) under `src/unittests`; includes StreamingControlBoundaryTests, StreamingControlTests, StreamingControlLifecycleTests, StreamingControlHookTests and the extended StreamingControlTransportTests. Run via `ctest` from `build\src\unittests` (see Quick Check for the required PATH). No external master-test project. Previously completed streaming tests remain protected master tests.

## Architecture and Workflow Notes

- Client/server architecture: one machine runs the server (keyboard/mouse owner), others run clients; `src/lib/net` handles transport, `src/lib/platform` handles per-OS input injection/capture.
- Version is derived from git describe; fallback version is hardcoded in root `CMakeLists.txt`.
- Fork policy: never commit personal customizations to `master`; that branch must stay clean for upstream syncs.
- Mouse send-rate limiter (server): `MouseMoveCoalescer`, setting `server/mouseSendRateHz` (default 250, 0 = off); see `docs/project/customizations.md`.
- Clipboard image sharing: default `server/clipboardSize` raised 3 -> 128 MiB (bitmaps are uncompressed DIBs; screenshots exceeded 3 MiB). Clients enforce receive limit from their own local setting; see `docs/project/customizations.md` section 4.
- Game/app exclusion feature (Windows server only): see `docs/project/customizations.md` — settings key `server/excludedApps`, watcher `MSWindowsForegroundWatcher` (events + 100 ms poll + pid snapshot + 300 ms resume debounce), hook-level pid guard, motion drop in `MSWindowsScreen`, 1 s watchdog. GUI: foreground label + `Excluded Apps` dialog (process picker with search) in the main window.
- Left Ctrl/Windows swap (Windows server): `server/leftCtrlSuperSwapScreen` selects one canonical client name; local/right-side keys remain unchanged. Per-event physical modifier snapshots preserve shortcuts without changing the protocol. See `docs/project/customizations.md` section 5.
- All git mutations (branching, merging, pulling upstream) are performed by the user, not the LLM.

- Streaming ownership: core worker owns the input-exporter-bound TLS 1.3 broker/private IPC and one source-bound desktop-control lease. GUI cannot inject OS input. Grants require explicit interactive offer permission, source-local grant, valid matching presented-frame geometry and idle ordinary input ownership. Native/ordinary input gates suppress echo and release held state on revoke; failed native releases retain arbitration and retry. Input protocol 1.8 remains unchanged. File playback permission is separate. macOS/X11 backends are source implementations awaiting their builds/native acceptance; standard Wayland physical-observer limitations require the documented product decision. Windows active-session service-core delegation is implemented with exact-logon native pipes but privileged runtime remains unaccepted; session zero remains rejected. Streaming-only login/power monitoring covers file/receiver/capture paths. macOS lock-state admission is unresolved and may refuse streaming; see the documented product decision.

### Capture development commands

- Configure media: add `-DBUILD_STREAMING=ON`, put `C:\Work\tools\gstreamer\1.28.7\bin` on PATH and set `PKG_CONFIG_PATH=C:\Work\tools\gstreamer\1.28.7\lib\pkgconfig`. The seven GStreamer modules must resolve >=1.28.7; libnice is also required; configuration fails if missing. OFF leaves native platform code buildable but disables GStreamer graphs.
- Build capture validation: `cmake --build build --target StreamingCaptureTests StreamingCapturePipelineTests StreamingCaptureProbe -j12` (Windows; PipelineTests requires ON).
- Focused capture tests: `ctest --test-dir build/src/unittests -R StreamingCapture --output-on-failure` with the same Qt/vcpkg/GStreamer PATH. No native windows are created by these unit/pipeline tests.
- Native controlled capture probe: `python tools/capture_probe.py`; creates its fixture only on an unselected desktop and records real capture/refusal logs under the item-03 proof folder. It cannot prove captured pixels when the OS denies inactive-desktop capture. It is separate from quick check/test:all because interactive capture acceptance needs explicit permission.
- Direct WinRT WGC replaces the initial GStreamer WGC-source choice: upstream 1.28.7 internally attempts DXGI if WGC construction fails. The native adapter has no source/API substitution path. Captured frames expose `coordinateMappingValid`; interactive input must remain disabled when false. Full details and remote-platform limitations: `docs/project/streaming.md`.
- Local file playback validation: `cmake --build build --target StreamingFileTests -j12`, then `ctest --test-dir build/src/unittests -R StreamingFileTests --output-on-failure`. Included in quick/exhaustive CTest; requires FFmpeg CLI for generated fixtures plus the documented Qt/vcpkg/GStreamer PATH. FileSource state/epoch/PCM integration API and tested format list are in `docs/project/streaming.md`.

- Audio validation: `cmake --build build --target StreamingAudioTests StreamingAudioProbe -j12` then `ctest --test-dir build/src/unittests -R StreamingAudioTests --output-on-failure`. PCM tests join quick/exhaustive CTest. The opt-in Windows StreamingAudioProbe emits an owned quiet tone and is separate from automated quick/test:all. Audio platform APIs, clock/queue contracts, exact command and unverified native/cross-device limits: `docs/project/streaming.md`. Linux BUILD_STREAMING additionally requires libpipewire-0.3 >=1.0 and a GStreamer PipeWire plugin with target-object/stream-properties; macOS adds CoreAudio/ScreenCaptureKit audio.

- Media transport validation: `cmake --build build --target StreamingMediaTests -j12`, then `ctest --test-dir build/src/unittests -R StreamingMediaTests --output-on-failure`. The extended registered suite exercises two separate endpoint processes using real broker-validated SDP/ICE and DTLS-SRTP, synthetic changing frames, actual FileSource/PCM, seek/resize, delayed consumption, encrypted-UDP loss and bandwidth emulation. It requires the documented SDK PATH and FFmpeg only for owned test-file generation. `StreamingMediaQuickTests` selects negotiation/pacing/epoch-zero and ordinary audio/video cases for quick check. No windows or audible output are created. `STREAMING_MEDIA_PROOF=<absolute-directory>` saves real decoded output PNGs; Qt Test `-o <absolute-log>,txt` captures full endpoint telemetry. Detailed limits and integration contracts: `docs/project/streaming.md`.

- Streaming policy validation: `cmake --build build --target StreamingPacerTests StreamingContextTests -j12`; quick/exhaustive CTest includes actual pacer admission/departure/expiry/reset guards and authenticated listener/client address publication policy.

- Sender UI validation: `cmake --build build --target StreamingSenderTests deskflow -j12`, then `ctest --test-dir build/src/unittests -R StreamingSenderTests --output-on-failure`. Included in quick/exhaustive checks; requires test-only FFmpeg for an owned 160x96 file and a separate headless DTLS receiver process. Set QT_QPA_PLATFORM=offscreen and QT_QPA_FONTDIR=C:\Windows\Fonts for isolated QWidget rendering on Windows; STREAMING_SENDER_PROOF selects a local screenshot output directory. Tests render the actual embedded StreamLauncher component without constructing MainWindow or accessing user settings/core singleton sockets. Full MainWindow/cross-device capture acceptance remains manual. See streaming.md for file preroll readiness and sender APIs.

- Receiver validation: `cmake --build build --target StreamingViewerTests deskflow -j12`; quick registration `StreamingViewerQuickTests` covers viewer policies and real separate-process file sender/receiver workflows. Exhaustive `StreamingViewerTests` also runs Windows native output/owned-process loopback and requires an enumerated **Steam Streaming Speakers** virtual endpoint; it fails explicitly if absent and never selects another device. Both need test-only FFmpeg and the same Qt/GStreamer PATH. `STREAMING_VIEWER_PROOF=<absolute-directory>` saves real receiver widgets and decoded source references; use offscreen plus the Windows font directory. The tests use isolated private IPC and widgets without MainWindow/settings/current core. Cross-device playback, physical audibility and global input-hook shortcut isolation remain manual acceptance.
- Receiver epoch rule: send Ready on first decoded video before waiting for audio. Preserve one next-due and one latest video frame; acknowledge GUI delivery. Flush both tracks on newer epochs and hold the output clock until the first new PCM is available. Receiver scheduling explicitly adds 40 ms to audio PTS and subtracts the same margin from video presentation time; legacy AudioOutput callers default to zero. This margin contributes to the documented latency/buffering budgets and does not establish physical A/V acceptance. Bind video source identity through encoding and authenticated identity/Opus priming/padding metadata through decoding before appsink can drop buffers; local jitter timestamps can repeat and cannot uniquely identify PCM. Invalid overlapping PCM ends the stream rather than being retried as backpressure.
- Paused seek ordering: keyframe requests target the selected frame's transport running time so a queued older raw frame cannot consume the request. Sender revalidates received playback-command epochs against its current FileSource and rejects overlapping seeks recoverably; broker validation alone cannot close that queued-command race.

- Control validation: `cmake --build build --target StreamingControlBoundaryTests StreamingControlTests StreamingControlLifecycleTests StreamingControlHookTests StreamingControlTransportTests StreamingControlProbe deskflow deskflow-core -j12` (Windows ON). Quick includes policy/lifecycle/actual low-level-hook dispatch boundary tests; exhaustive also includes real exporter/TLS/private-IPC control routing with an explicitly substituted native API boundary. `python tools/control_probe.py` verifies inactive-desktop refusal; `python tools/control_probe.py --verify-core-guard` also refuses if any Deskflow GUI/core/daemon is running. Neither injects input. The headed 20-second owned fixture requires separate approval and user closure of current Deskflow; see streaming.md. Existing running binaries are never stopped or replaced by validation.
- macOS build (Homebrew Qt, `BUILD_STREAMING=OFF`, no GStreamer installed) compiles the streaming control/capture `.mm` sources. OBJC/OBJCXX languages are enabled in the root `CMakeLists.txt`; enabling them in a subdirectory breaks generation and recompiles `.m` files as ObjC++. C++-only declarations in headers shared with `.m` files need `#ifdef __cplusplus`. Install steps: see memory/mac install notes (macdeployqt, core `install_name_tool`, Developer ID signing).
- Linux X11 control adds pkg-config dependencies `xcb-xinput`, `xcb-xtest`, `xcb-keysyms`, `xcb-randr` when `BUILD_X11_SUPPORT=ON`. Native control uses the exact captured XID/process birth/monitor and current source layout. macOS adds ApplicationServices/Carbon and exact sample ScreenCaptureKit geometry; missing metadata disables mapping. These sources have no local Windows-native validation claim.

- Recovery validation: `cmake --build build --target StreamingRecoveryTests StreamingWindowsIpcTests StreamingEnvironmentTests -j12`; `ctest --test-dir build/src/unittests -R "Streaming(Recovery|WindowsIpc|Environment)Tests" --output-on-failure`. Recovery requires BUILD_STREAMING=ON; Windows IPC and message-only session-notification checks also build OFF. All join quick/exhaustive checks. Session/lease-bound queued commands, initial capture state, backend-error privacy, early-epoch PCM and controller-exit detachment have dedicated regressions. Real isolated launcher screenshots use `STREAMING_RECOVERY_SCREENSHOT=<absolute-png>` with offscreen/font paths. Remote/native acceptance and the macOS lock API decision remain explicit limitations in streaming.md.

- Extended recovery/resources: `cmake --build build --target StreamingRecoveryResourceTests -j12`; `ctest --test-dir build/src/unittests -R StreamingRecoveryResourceTests --output-on-failure`. Windows ON only; real owned FileSource/PCM, private IPC/broker and separate receiver with repeated sessions in one controller. Requires FFmpeg and StreamingSenderTests (build dependency); hidden exhaustive includes it and quick excludes it by streaming-extended label.

- Streaming packaging: Windows packages pin Qt6.10.3/GStreamer1.28.7 and deliberately select SDK OpenSSL3.5.0 for the process after import/export checks. Private plugins/scanner and temporary registries override development discovery. CMake development markers are explicit and build-bound. Packaging rejects stale plugins, forwarded exports and CRT versions older than the compiler; local CRT14.44.35211 covers compiler14.43. See streaming-packaging.md for clean staging, notices and commands. macOS/Linux packaging source integration remains uncompiled here; full UI/clean-machine/device/signing acceptance remains blocked.

- Windows package outputs: local portable7Z and MSI build successfully with compatible extracted CRT and owned WiX4.0.6 toolchain. MSI payload inspection is read-only; service installation and full clean-host UI/device acceptance remain manual. Exact local WiX commands are in docs/dev/build.md.

- FileSource timestamps are media stream time derived from each GStreamer sample segment; raw decoder PTS can contain MP4 reorder/edit-list offsets. `StreamingFileTimelineTests` checks both decoded tracks at startup and seek in quick/exhaustive CTest.

- Post-first-frame recovery: authenticated newer video identities without decoded progress for3s terminate both peers with an actionable restart message. Static/paused identities remain valid; initial timeout and total-video-blackhole liveness are separate. `StreamingDecodeProgressPolicyTests` joins quick; `StreamingDecodeProgressTests` joins exhaustive with real selective encrypted-video loss, bilateral Stop/cleanup and same-instance reuse. Current quick/exhaustive selections contain53/60 registrations.
