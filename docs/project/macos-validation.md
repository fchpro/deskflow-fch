# macOS local validation, 2026-09-21

## Installed result

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
