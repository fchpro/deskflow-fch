# macOS local validation, 2026-09-21

## Earlier automatic-acceptance installation

Superseded by the coordinated native follow-up below.

- `/Applications/Deskflow.app`: automatic stream acceptance enabled in MainWindow; embedded launchers/viewers retain manual defaults.
- Existing Developer ID team `5KGS79Q5Y7`; hardened runtime; strict/deep signature verification passed. Not notarized during this task.
- Ordinary input connection re-established to `192.168.137.1` with TLS 1.3. Streaming launcher reports `Connected. Choose a source and compatible destination.` Physical Windows keyboard/mouse delivery was not exercised.
- Settings, certificate and both existing trust files remained byte-identical to the pre-install backup.
- Backup bundle: `/Applications/Deskflow-pre-auto-accept-20260921.app`; second bundle and settings copy: `temp/backups/auto-accept-20260921/`.
- Source patch applied without Git mutations. Existing uncommitted documentation preserved. No protected tests changed.

## Toolchain and commands

- Qt: `/Users/fchpro/Qt/6.10.3/macos`.
- GStreamer: `/Users/fchpro/Library/Frameworks/GStreamer.framework/Versions/1.0`, version 1.28.7; libnice 0.1.24.
- CMake/Ninja from existing toolchain. AppleClang 17; arm64 Release; C++20. OpenSSL static libraries resolve from the GStreamer SDK, version 3.5.0.

```sh
PKG_CONFIG_PATH=/Users/fchpro/Library/Frameworks/GStreamer.framework/Versions/1.0/lib/pkgconfig cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/Users/fchpro/Qt/6.10.3/macos -DBUILD_STREAMING=ON -DBUILD_TESTS=ON -DBUILD_INSTALLER=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build build --target build-validation -j8
QT_QPA_PLATFORM=offscreen build/src/unittests/gui/StreamingAutoAcceptTests
QT_QPA_PLATFORM=offscreen ctest --test-dir build/src/unittests -LE streaming-extended --output-on-failure
```

The unchanged OSXClipboardTests use the general clipboard. Preserve all clipboard item representations before running CTest and restore/byte-verify them afterwards. This run used `temp/clipboard-guard.swift` and three owned backups under `temp/backups/auto-accept-20260921/`. Do not run clipboard probes concurrently with that suite.

Exhaustive command: `QT_QPA_PLATFORM=offscreen ctest --test-dir build/src/unittests --output-on-failure`. Not run for this task; known native/media blockers remain. The current Mac quick selection exceeds the 30-second target when these failures occur. It is not a passing or budget-compliant Mac check.

## Actual verification

- `build-validation`: all registered application/test consumers compiled successfully, including MacAudio.mm.
- `StreamingAutoAcceptTests`: 7 Qt results passed, no failures/skips. Covers real isolated private IPC/broker acceptance after viewer display, hidden Accept, Stop, no desktop-control grant, duplicate acceptance, later-session reset, default/missing audio selection and stale display-only offer rejection.
- Native CoreAudio check independently queried `kAudioHardwarePropertyDefaultOutputDevice` and its UID. The enumerated default matched `MacBook Pro Speakers`, `BuiltInSpeakerDevice`.
- Viewer screenshot rendered and inspected. It contains no Accept button; it also reports `This macOS login does not expose a verifiable lock state for streaming.` No decoded video is claimed.
- Quick selection: 39/47 passed in 170.26 seconds. Failures below; none bypassed or edited.

| Suite | Observed failure |
|---|---|
| I18NTests | Language/native-name and language-selection expectations |
| StreamingRecoveryTests | Real receiver transport setup abort |
| StreamingViewerQuickTests | Three real-receiver cases never obtain decoded frames |
| StreamingSenderTests | Terminated after 90.98 seconds; captured stack shows worker/capture destruction during launcherStructure; repeated SIGSEGV diagnostics in capture status callback |
| OSXKeyStateTests | Native key-release assertions |
| StreamingTransportTests | TLS negotiation expectation and encrypted fixture handshake abort |
| StreamingAudioTests | Native clock tolerance and protected raw backend-message expectation |
| StreamingFileTests | Owned media fixture generation abort |

Prior Mac logs already recorded recovery/viewer/TLS/audio/file failures. This task did not establish a pre-change baseline for every failing case. The sender teardown stall and I18N/native-key failures must not be claimed as caused or fixed by automatic acceptance.

## Packaging

- Fresh stage: `cmake --install build --prefix /Users/fchpro/projects/deskflow-fch/temp/auto-accept-install`.
- `deploy/mac/streaming-install.cmake.in` now stores plugins under `Contents/PlugIns/gstreamer` and creates the runtime symlink `Contents/MacOS/gstreamer-1.0`. This removes the previously manual workaround for codesign rejecting a dotted physical directory.
- Existing workflow: macdeployqt, private SDK dependency collection/relocation, sign nested Mach-O code with existing Developer ID and hardened runtime, then sign and verify the enclosing app.
- Strict/deep signatures and dependency closure passed for 88 Mach-O files. Twelve media factories loaded using a new private registry and the packaged scanner. No external non-system dependency required by the audited bundle.
- Installed only after staging/signature/runtime checks. GUI detected and shut down the older core by its IPC version, then launched the rebuilt core. Installed UI and live socket establish reconnection.

## Clipboard diagnosis

- Preserved existing `OSXClipboardBMPConverter` extended-DIB-header/pixel-offset fix from commit `62a8ee749`; no clipboard source changed in this task.
- Owned 320×180 red PNG and TIFF were put on the native pasteboard, read through the actual OSXClipboard and BMP converter, then decoded and checked for dimensions/red pixels. Both passed; DIB output was 172840 bytes with 40-byte header, BI_RGB and 24-bit pixels.
- User clipboard was restored and every saved representation byte-verified after each probe and the quick check.
- These probes did not reproduce the reported failure. They do not prove screenshot hotkey handling, every screenshot pixel format, transport to Windows or Windows paste behavior. No new cause or fix is asserted.
- Existing source fix exported for Windows retention: `temp/existing-mac-clipboard-fix.patch`. This is the already committed Mac converter fix, not a newly diagnosed fix. Current Windows converter also contains its existing malformed-Mac-DIB normalization; unchanged here.

## Evidence and remaining work

- Evidence: `temp/proof-of-work/auto-accept-20260921/`.
- `focused.txt`, `viewer.png`: isolated automatic acceptance and actual rendered viewer.
- `installed-ui.txt`, `installed.png`, `live-app.txt`, `settings-verified.txt`: installed UI/TLS connection and preserved settings/trust.
- `build.txt`, `package.txt`, `sign.txt`, `dependencies.txt`, `runtime.txt`: build and package checks.
- `quick-check.txt.gz`: full captured quick output; `quick-check.compact.txt`: captured output with repeated SIGSEGV/timing diagnostics condensed. A sender teardown loop generated 3.5 GiB of repeated output; the duplicate CTest temporary log was removed after retaining compressed evidence.
- `clipboard.txt`, `clipboard-tiff.txt`, `clipboard-quick-guard.txt`: owned-image checks and restoration.
- `sender-hang.txt`: actual sampled process stack before termination.
- Existing macOS session admission requires a lock-state key absent on this login. No privacy bypass or assumed-unlocked fallback was introduced. The existing worklist's macOS lock decision remains unresolved.
- Incoming Windows-origin automatic viewer opening, real video/audio and Mac-to-Windows screenshot pasting remain unverified. This session has no Windows desktop/input tool. Isolated automatic acceptance passes but must not be described as cross-device media acceptance.


## Changed files

- `src/lib/gui/MainWindow.cpp`: opt into automatic acceptance.
- `src/lib/gui/streaming/StreamDialog.{cpp,h}`, `StreamViewer.{cpp,h}`: optional automatic mode, viewer-first acceptance, audio-default selection and UI text.
- `src/lib/gui/streaming/SenderWorker.{cpp,h}`, `ReceiverWorker.cpp`: native-default inventory, duplicate-Accept guard and immediate Stop semantics.
- `src/lib/streaming/Audio.{cpp,h}`, `MacAudio.mm`: explicit native default-output identity.
- `src/unittests/gui/CMakeLists.txt`, `StreamingAutoAcceptTests.cpp`: register supplied focused suite plus native CoreAudio verification.
- `deploy/mac/streaming-install.cmake.in`: permanent plugin-directory/signing correction.
- `PROJECT.md`, `docs/project/customizations.md`, `streaming.md`, `streaming-packaging.md`, `macos-validation.md`: behavior, commands, installation results and limitations.
- Export only: `temp/existing-mac-clipboard-fix.patch`. No clipboard source edits.

## Coordinated native follow-up, 2026-09-21

### Source and installation

- User explicitly authorized Git/build/install on this Mac for this task. Reviewed local edits committed as `53932863f`; merged `origin/fch` through `6f7e16f0d`, including Windows fix `f98547bf0`, without resetting or overwriting work. Integrated source pushed as `e126d7fad7ca96a023d21c25c2d9638507143255`.
- Duplicate acceptance changes merged once. Preserved native CoreAudio default-output test and permanent plugin-directory packaging fix. No protected test assertions rewritten.
- `cmake --build build --target build-validation -j8` passed with existing Qt6.10.3/GStreamer1.28.7 and `BUILD_STREAMING=ON`.
- Installed `/Applications/Deskflow.app`; core reports `v1.26.0.9999 (e126d7fa)`. Signed with existing Developer ID/hardened runtime. Strict/deep signature check passed. Audited 88 Mach-O files and 642 dependency edges with zero external non-system/missing dependencies. Twelve private media factories loaded with a fresh registry.
- Backups: `/Applications/Deskflow-pre-native-20260921.app` and `.ltemp/native-20260921/application-backup/Deskflow.app`. Settings/certificate/trust under `.ltemp/native-20260921/settings-before` compare byte-identically after restoration/install. Temporary Debug/file logging restored to Info/file-disabled.
- Installed GUI observed TLS1.3 reconnection before and after sleep. Existing missing `en` layout/cursor warnings remain. Physical input correctness was not independently verified.
- Installed source selector directly reports `Enable Screen Recording for Deskflow in System Settings`; no sources enumerated. After wake the streaming label reports `peerDisconnected` despite ordinary input TLS being connected. No permission grant, private adapter, helper or entitlement added. No operational streaming/decoded-frame claim.

### Executed validation

| Check | Native result |
|---|---|
| StreamingCaptureLifetimeTests | 4 passed / 0 failed / 0 skipped; 2 ms |
| StreamingSenderTests launcherStructure | 3 passed / 0 failed / 0 skipped; 297 ms; actual Mac capture adapter teardown no longer crashes |
| StreamingAutoAcceptTests | 7 passed / 0 failed / 0 skipped; 335 ms; CoreAudio default matched |
| Quick CTest selection | 40/48 passed; 91.21 s; fails the 30-second budget |

Quick failures retained separately in `.ltemp/native-20260921/quick-failures.txt`, with complete output in `quick-check.txt`:

- I18NTests: four language/native-name selection expectations.
- StreamingRecoveryTests: receiver transport setup abort.
- StreamingViewerQuickTests: three workflows obtain no decoded frames.
- StreamingSenderTests: `realFileWorkflow` does not obtain ten decoded frames. `launcherStructure` now passes; no teardown SIGSEGV.
- OSXKeyStateTests: two native key-release assertions.
- StreamingTransportTests: TLS negotiation expectation and encrypted fixture handshake abort.
- StreamingAudioTests: common-clock tolerance and protected raw-backend-error expectation.
- StreamingFileTests: generated media fixture abort.

These remain failures, not skipped/passing coverage. Exhaustive suite unrun. Quick clipboard guard restored and byte-verified all saved representations. Rendered acceptance screenshot inspected: no Accept button; explicit unverifiable-lock error; no video.

### Real screenshot clipboard evidence

- Used an owned native 800×500 window with opaque blue contents, rounded corners and native shadow. `/usr/sbin/screencapture -x -c -l <owned-window-id>` and `-R <owned-interior-rectangle>` exercised the system screenshot-to-clipboard path. No pre-encoded PNG/TIFF was substituted.
- A separate automated Control–Command–Shift–4 attempt did not change the clipboard; it is not claimed as a successful hotkey reproduction. Command-path screenshots succeeded. Source and decoded images inspected.
- Actual `OSXClipboard::synchronize/open/has/get/close` and `OSXClipboardBMPConverter::fromIClipboard` ran against the screenshot pasteboard. Offered formats included public.png, public.tiff, com.microsoft.bmp, HEIC/AVIF and other OS image conversions; complete type list in `clipboard-screenshot.txt`. BMP fetch status was 0.

| Case | OS BMP bytes | Export DIB bytes | Dimensions | Alpha range |
|---|---:|---:|---|---|
| Window with shadow | 9,397,386 | 9,397,372 | 1824 × -1288 | 0–255 |
| Opaque interior | 4,256,138 | 4,256,124 | 1400 × -760 | 255–255 |

Both have 124-byte BITMAPV5HEADER, 32 bpp, BI_BITFIELDS=3, masks `00ff0000/0000ff00/000000ff/ff000000`, BMP pixel offset138 and DIB pixel offset124. Rewrapped BMP byte sizes match original BMP sizes; both decode successfully with correct owned contents. Negative height is top-down storage.

- Installed client, with temporary Debug logging, recorded at 11:28:56 local time: `sending clipboard 0 seqnum=0`, `sent clipboard size=9397384`, then the same for clipboard1. This is exactly the 9,397,372-byte bitmap plus 12 bytes of clipboard framing. Source inspection shows this log is emitted after enqueueing all ClipboardSending chunks; it is not a wire-delivery acknowledgement. No limit warning. Export happened while inactive; no focus-return event occurred during this capture, so that particular transition remains unverified.
- Mac saved receive setting is128 MiB. Windows coordinator reports128 MiB. Source negotiates the server send limit in KiB and checks bytes against limit×1024. The installed Info/Debug logs do not expose the numeric negotiated value; it was not read from live memory. This sample was demonstrably admitted and sent.
- Original clipboard was restored and byte-verified after command captures, quick checks and the send probe. No clipboard content or credentials committed. No new converter fix: the existing source works for these screenshot cases.
- Missing Windows evidence: corresponding receive/assemble log for clipboard0/1 and9,397,384-byte payload; advertised CF_DIB/CF_DIBV5/CF_BITMAP formats; Win32 SetClipboardData/GetLastError outcomes; normalization result with original124-byte/top-down/32-bit BI_BITFIELDS metadata; destination application's paste outcome. A local decode and Mac send log do not establish cross-device paste.

### Artifact boundaries

All new probes/logs/fixtures/backups are under `.ltemp/native-20260921`, verified excluded by `git check-ignore` through local `.git/info/exclude`. Nothing there is staged. Lock evidence and proposed policy are in the item-10 lock decision document. Initial dependency-audit parser failure is retained separately; it mistakenly counted universal-architecture headers and dylib IDs as dependencies. Corrected audit selects arm64 and excludes LC_ID_DYLIB; no package mutation was used to silence it.
