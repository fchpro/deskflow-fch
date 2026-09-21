# Automatic stream acceptance and Mac screenshot clipboard

- Mode: blitz. User explicitly requested both changes on 2026-09-21.
- Windows handles source changes/builds here; Mac agent handles native diagnosis/install through copyable prompts.
- User explicitly assigned Git syncing and builds on both platforms to this agent for this conversation. Commit/push coordinated source from Windows; Mac preserves its own changes before integrating and publishing native changes.

## Automatic acceptance — both installed; Mac runtime blocker reported

- Product decision: starting a stream to an authenticated connected peer opens its viewer and accepts automatically. Desktop control still needs a separate grant.
- App launcher opts into automatic viewer acceptance. Standalone embedded viewers retain explicit acceptance unless requested by their caller.
- Audio: select the enumerated native default output on Windows/macOS. If no verified default exists, request an output selection; never silently mute or pick another device.
- Protocol/TLS/IPC/broker role checks and Stop remain unchanged. Acceptance is bound to the current visible viewer session/source.
- MainWindow enables automatic acceptance explicitly; embedded launchers/viewers retain manual mode by default. Worker suppresses duplicate Accept and sends Stop if acceptance acknowledgement is still pending.
- Verification: build-validation passed. StreamingAutoAcceptTests passed 6 Qt cases including broker acceptance/Stop, repeat acceptance, default/missing-output UI. Real offscreen viewer inspected: no Accept controls; Stop visible. Hidden final quick check passed 52/54 in 23.39 s. Only documented MSWindowsForegroundWatcherTests and StreamingAudioTests failures remain; protected tests unchanged. Full/native Mac validation not run here.
- Installed on Windows at the existing dist/2026-09-16 path on 2026-09-21; previous full package retained as dist/2026-09-16-before-auto-accept. GUI reopened visibly; both input TCP24800 and streaming TCP24801 reconnected to Mac192.168.137.42. Settings, certificates, firewall and shortcuts preserved.
- Evidence and copyable Mac prompt/patch: temp/proof-of-work/stream-auto-accept/. PASTE-ON-MAC.txt contains instructions plus exact source patch (no Git mutations needed).
- Mac agent report relayed by user: signed build installed; TLS reconnected with settings preserved; acceptance7/7, quick39/47, full unrun. macOS lock-state checks reportedly block streaming. Exact failures/source changes not received; cross-device acceptance remains unverified. Native files cited in report are inaccessible from Windows.
- Windows read-only follow-up confirms input24800 and streaming24801 remain established to Mac192.168.137.42. Local MacSessionEnvironment::verify rejects a missing CGSSessionScreenIsLocked key; native evidence is needed to distinguish runtime failure from test-only failure. Do not bypass the lock contract.
- Detailed report received: macOS26.5.1 build25F80 arm64, Qt6.10.3/GStreamer1.28.7. Native session query has on-console=true but no CGSSessionScreenIsLocked. Production worker tests refuse media startup; matching installed binaries predict the same refusal, but installed-app stream failure was not directly reproduced. This distinction supersedes the earlier broad blocked-runtime wording.
- Mac quick failures: I18NTests, StreamingRecoveryTests, StreamingViewerQuickTests, StreamingSenderTests, OSXKeyStateTests, StreamingTransportTests, StreamingAudioTests and StreamingFileTests. Different causes; not all attributable to lock checks. Full suite unrun.
- Separate sender SIGSEGV: MacCapture destructor emits statusChanged while worker members are being destroyed. Added SenderWorker destructor disconnect plus a new focused lifetime test; native rerun pending.
- Windows follow-up: all app/test consumers built; lifetime regression4/4 Qt results; quick53/55 in29.04s with only existing foreground/audio-expectation failures. Package audit passed29 plugins/502 dependency edges/102 PE files. Installed at existing dist/2026-09-16; immediately previous package preserved in .ltemp/package-before-capture-lifetime. Evidence .ltemp/proof-of-work/mac-followup/. No protected test edits; full/native Mac suite unrun here.
- Next: integrate shared source through Git and reproduce the remaining native lock/clipboard failures without bypassing the lock policy. Return native changes through Git for Windows integration.
- Windows source pushed to origin/fch as f98547bf0cc82bb3c8c1a84dd3d733c9da6dfcee. Working input/signaling reconnected after installation. Mac follow-up prompt is .ltemp/MAC-NEXT-STEPS.txt; it requests safe integration, native crash rerun, bounded lock validation and actual screenshot-path clipboard evidence. No new lock contract has been approved or implemented.

## Mac-to-Windows screenshot clipboard — investigating

- User confirms Control–Command–Shift–3/4 (clipboard screenshot), not screenshot-file capture.
- Windows-to-Mac works; Mac-to-Windows fails to paste.
- Windows receive limit is 128 MiB. Current source has older BMP/DIB fixes already present in the installed Windows revision.
- Mac report: actual OSXClipboard converted owned PNG/TIFF320x180 to valid172840-byte 24bpp BI_RGB DIB; original clipboard restored. These probes did not cover screenshot hotkeys, 32bpp extended DIB or Windows delivery/paste.
- Supplied existing clipboard patch matches local file blobs exactly (cpp c6d815f4246c37d2905b95a18568475f9179084f, header1f7a175946a14960d60adbcfb073b671177e45dc). No new clipboard fix to apply. Installed Mac contains it; real screenshot failure cause remains unestablished.
- Next: correlate Mac diagnostics with Windows clipboard formats/decoder behavior and implement a focused regression.

## Verification plan

- Build changed app/test targets; focused auto-accept integration and rendered viewer.
- Run documented hidden quick check against rebuilt consumers once final changes are ready; retain known failures accurately.
- Native macOS build and screenshot transfer need the Mac agent; Windows cannot verify native pasteboard APIs.
