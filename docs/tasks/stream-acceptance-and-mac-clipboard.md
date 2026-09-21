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
- Windows-to-Mac works. User subsequently confirmed Mac screenshots paste into Windows Paint and Claude; ChatGPT paste remains unverified after correction.
- Windows receive limit is 128 MiB. Current source has older BMP/DIB fixes already present in the installed Windows revision.
- Mac report: actual OSXClipboard converted owned PNG/TIFF320x180 to valid172840-byte 24bpp BI_RGB DIB; original clipboard restored. These probes did not cover screenshot hotkeys, 32bpp extended DIB or Windows delivery/paste.
- Supplied existing clipboard patch matches local file blobs exactly (cpp c6d815f4246c37d2905b95a18568475f9179084f, header1f7a175946a14960d60adbcfb073b671177e45dc). No new clipboard fix to apply. Installed Mac contains it; real screenshot failure cause remains unestablished.
- Next: correlate Mac diagnostics with Windows clipboard formats/decoder behavior and implement a focused regression.

## Verification plan

- Build changed app/test targets; focused auto-accept integration and rendered viewer.
- Run documented hidden quick check against rebuilt consumers once final changes are ready; retain known failures accurately.
- Native macOS build and screenshot transfer need the Mac agent; Windows cannot verify native pasteboard APIs.

## Mac coordinated follow-up, 2026-09-21

- Integrated Windows source plus preserved Mac fixes/tests pushed as `e126d7fad7ca96a023d21c25c2d9638507143255`; signed installed core identifies `e126d7fa`.
- All consumers built. Native lifetime4/4, launcherStructure3/3, acceptance7/7 pass. Quick40/48 in91.21s; same eight failing suites, but sender failure is now decoded-frame workflow rather than teardown crash. Exact failures and package/installation evidence: `docs/project/macos-validation.md`.
- Lock/unlock and real sleep/wake observed. Private key absent/true/absent while public on-console stays true. Session-switch remains pending. Concrete build-specific proposal and limitations recorded in `item-10-macos-lock-decision.md`; no lock-policy change made.
- Real system screenshot clipboard cases include transparent shadow9,397,372-byte and opaque4,256,124-byte top-down32-bit V5 DIBs. Both convert/decode. Installed client logged9,397,384-byte sends on clipboard0/1; focus-return transition and Windows receive/paste still unverified. Existing BMP patch not reapplied. Windows receive/format/SetClipboardData evidence is the next missing boundary.
- App/settings/certificate/trust backed up; settings/trust restored byte-identically. Signed bundle and private runtime verified. Installed app reconnects ordinary TLS after sleep; source discovery reports missing Screen Recording permission and streaming peer label remains disconnected after wake. No decoded frames or operational streaming claimed.

## Windows correlation after Mac report

- Checkout already at Mac report commit3c60cacc8 when inspected; fetch confirmed remote. Separate uncommitted Windows mouse-boundary work was preserved without staging or modifying it.
- Windows log at2026-09-21T11:28:58.193 and11:28:59.581 rejects Mac clipboard0/1 as mis-sequenced, matching the Mac's11:28:56 enqueue report after11:26:57 reconnection. Server::onClipboardChanged returns before reading/converting bitmap data when the sequence is older than the clipboard owner sequence. This establishes rejection for that probe, not the cause of every original paste failure.
- Current Windows clipboard contains Deskflow-owned text/HTML only; it cannot supply the overwritten screenshot's DIB metadata. Read-only native enumeration saved under .ltemp/proof-of-work/windows-clipboard/current-metadata.txt; matching rejection logs in mac-export-rejection.txt.
- Next reproducible transfer: enter the Mac with the Windows-controlled pointer after reconnection, capture the owned test window to the Mac clipboard, return the pointer to Windows, and retain that image until Windows formats/decoder/paste are inspected. Do not copy the Mac report or restore the original clipboard before that inspection.
- User approval requested for the documented25F80-only private lock adapter with startup calibration; no approval received or lock-policy edit made at this point. Session-switch validation remains required before activation.

## Windows native bitmap publication, 2026-09-21

- Actual Mac screenshots arrive as complete124-byte V5/32-bit/BI_BITFIELDS DIBs. Deskflow published only CF_DIB; Windows advertised CF_DIBV5 but GetClipboardData returned null. WPF GetImage raised CLIPBRD_E_BAD_DATA and WinForms returned null.
- Adding the exact same bytes explicitly as CF_DIBV5 made both native decoders return the correct dimensions. A verified unchanged V5-only diagnostic still failed the user's ChatGPT paste test; this native fix is not yet sufficient evidence of a ChatGPT fix. Subsequent tests were confounded by replacement screenshots/text; clipboard sequence tracking detected replacements.
- MSWindowsClipboard now publishes an independent CF_DIBV5 copy of complete V5 converter output while retaining CF_DIB. Header/masks/alpha/colour data are unchanged. Existing repaired40-byte and ordinary bitmaps retain their publication path. No Mac source/build change is needed.
- New MSWindowsClipboardNativeBitmapTests exercises real Windows clipboard publication/retrieval of CF_DIBV5, CF_DIB and CF_BITMAP, exact V5 bytes and decoded RGB pixels. Covers healthy Mac V5, ordinary Windows INFOHEADER and repaired legacy Mac header. Existing protected tests unchanged.
- build-validation passed; quick55/57 in28.36s. Only documented foreground-window and raw-audio-message expectation failures remain. Full suite unrun. Local diagnostic/build/check/package output: .ltemp/proof-of-work/clipboard-v5/.
- Installed Windows update at dist/2026-09-16; prior package retained at .ltemp/package-before-clipboard-v5. Settings unchanged; input24800 and signaling24801 reconnected to the Mac. Pre-check screenshot restored after clipboard tests. Package audit29 plugins/502 edges/102 PE files passed.
- Next: verify a newly transferred Mac screenshot in ChatGPT. Do not mark ChatGPT compatibility fixed from native-decoder results alone. No Mac rebuild is needed for this Windows-only change.

## Standard CF_DIB representation follow-up

- User reports ChatGPT still rejects the installed V5-only correction. Windows clipboard inspection confirms the new core publishes readable CF_DIBV5 and both WPF/WinForms decode it, so native V5 retrieval alone does not establish ChatGPT compatibility.
- Installed app package declares Electron42.3.0. Its Chromium148.0.7778.180 bitmap reader uses CF_DIB and adds12 mask bytes after biSize for BI_BITFIELDS; V5 already embeds those masks. This is a concrete layout incompatibility, not yet proof of the entire app failure path. Sources: https://raw.githubusercontent.com/electron/electron/v42.3.0/DEPS and https://raw.githubusercontent.com/chromium/chromium/148.0.7778.180/ui/base/clipboard/clipboard_win.cc .
- Temporary standard-DIB test was replaced during the test (sequence1584 ->1597; original124-byte header restored). A new capture or peer clipboard republication can replace diagnostics; do not interpret that test as verification of the corrected format.
- Permanent Windows publication now gives standard sRGB BGRA V5 screenshots a40-byte BI_RGB CF_DIB with byte-identical pixel rows. CF_DIBV5 retains the complete original header, masks, alpha and colour data. Other profiles/masks retain their existing representation. No protocol or Mac changes.
- Native regression now also checks exact standard CF_DIB header/pixels, top-down/bottom-up rows, preserved partial alpha in V5 and decoded RGB pixels.
- Build passed; final quick/package/install and ChatGPT acceptance pending.
