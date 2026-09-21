# Streaming packaging and operation

## Current support

| Surface | Implemented | Acceptance |
|---|---|---|
| Windows x64 install / portable package | Qt6.10.3, GStreamer1.28.7 plugins/scanner and DLL closure | Isolated local install, TLS, VP8/Opus, H.264/AAC file decoding verified; clean OS/real-device acceptance pending |
| macOS app bundle | Private SDK closure and scanner; local Developer ID signed install | 2026-09-21 repaired staging: signature/dependency/media-factory/IPC checks passed; capture/TCC/cross-device acceptance pending |
| Native Linux DEB/RPM/Arch | Required factory inspection and native package-owner dependency generation | Source only; native builds/package installation unrun |
| Flatpak streaming | No supported streaming sandbox contract | Direct PipeWire/process identities/login1 requirements unresolved; existing Flatpak remains streaming OFF |

Both endpoints require this feature build. The already running `build/bin` app/core remain the
older build; validation uses `build/bin-streaming-validation`. No release, service installation,
system settings change or signing credential use occurs during local package verification.

## Windows runtime contents

- `deskflow.exe`, `deskflow-core.exe`; installed format also contains `deskflow-daemon.exe`.
- Qt deployment output, platform/TLS plugins and CRT DLLs. Portable 7Z removes the daemon and
  creates `settings/Deskflow.conf`; it cannot service UAC/login-screen input.
- 29 GStreamer plugins in `gstreamer-1.0/`; exact list in `deploy/windows/bundle_streaming.py`.
  Includes core/app/conversion, WebRTC/DTLS/SRTP/libnice/GCC, VP8/Opus, selected local-file
  demux/decoder plugins, WASAPI2 and diagnostic video/audio test sources.
- `gst-plugin-scanner.exe` beside the runtime DLLs. It is the SDK helper, not a wrapper.
- SDK OpenSSL3.5.0 selected for all consumers. vcpkg builds currently use OpenSSL3.4.1 headers;
  differing same-name DLLs are excluded from CMake's first collection stage and selected once
  by the package bundler. The bundler verifies ordinary/delay imports against packaged exports.
  Forwarded exports are currently refused and require an explicit dependency audit.
- `streaming-runtime.version` activates private discovery. Development outputs receive a
  separate CMake-generated `streaming-runtime.development` marker bound to the build tree.
  Missing/invalid package markers never enable SDK discovery merely because staging is below build.
- `streaming-runtime.json`: SDK versions, selected plugins, source/copy hashes, import edges,
  final runtime-file hashes. Portable pre-packaging reruns the audit after removing the daemon.
- `licenses/gstreamer/`, SDK `versions.txt`, Qt SBOM and 38 upstream Qt6.10.3 license texts.
  `deploy/licenses/README.md` records sources. No source archives or redistribution certification
  are supplied. The Microsoft 14.44.35211 English redistributable license is included.
- The local MSVC compiler is 14.43; the selected extracted Microsoft CRT is 14.44.35211.0.
  `MSVC_REDIST_DIR` selects an owned redistributable tree containing `x64/Microsoft.VC143.CRT`.
  Packaging rejects a runtime older than the compiler. Use an official Microsoft redistributable
  and its applicable notices when overriding this path; never collect CRT files from System32.

The plugin path, system plugin path, helper and per-process temporary registry are explicitly
selected before GStreamer initialization. Windows empty environment values remove variables;
system paths therefore use the actual private plugin directory instead of an empty value.
No install-directory write permission is needed for registry caching. Unlisted existing plugin
files cause packaging failure; use a new staging directory. FFmpeg CLI is a fixture-generation
tool only and is absent from the product package. Its decoder libraries arrive through gst-libav.

## Build and package commands

Use a VS2022 x64 developer prompt. Set process-local PATH to Qt6.10.3 `bin`, the build's
`vcpkg_installed/x64-windows-release/bin`, and GStreamer1.28.7 `bin`.
Set `PKG_CONFIG_PATH=C:\Work\tools\gstreamer\1.28.7\lib\pkgconfig`.

```bat
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:/Qt/6.10.3/msvc2022_64 -DCMAKE_TOOLCHAIN_FILE=C:/Work/tools/vcpkg/scripts/buildsystems/vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release -DBUILD_STREAMING=ON -DBUILD_INSTALLER=ON -DMSVC_REDIST_DIR=C:/Work/projects/deskflow-fch/temp/msvc-redist-14.44.35211 -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=C:/Work/projects/deskflow-fch/build/bin-streaming-validation
cmake --build build --target build-validation -j12
python tools/check_headless.py --log temp/proof-of-work/quick-check.txt
python tools/check_headless.py --full --log temp/proof-of-work/full-check.txt
cmake --install build --prefix C:/Work/projects/deskflow-fch/temp/streaming-install
cpack -G 7Z --config build/CPackConfig.cmake -B temp/streaming-package
```

Use fresh absolute install/package staging paths. Packaging requires Python3 plus `pefile`;
the program does not. `BUILD_STREAMING=OFF` retains upstream installation behavior and does
not package GStreamer. `build-validation` builds all registered test consumers and applications
without executing tests. The default build target still launches CTest and can create native
test windows; use the documented hidden runner when headless execution is required.

| Check | Command | Prerequisites / scope |
|---|---|---|
| Quick | `python tools/check_headless.py --log temp/proof-of-work/quick-check.txt` | Last rebuilt consumers; quick selection; <=30s target |
| Full validation | `cmake --build build --target build-validation -j12` then hidden `--full` command above | Compile all consumers plus exhaustive registered tests |
| Exhaustive automated tests | `python tools/check_headless.py --full --log temp/proof-of-work/full-check.txt` | Every CTest registration including runtime/package suites; original foreground/audio blockers remain |
| Runtime discovery | `ctest --test-dir build/src/unittests -R StreamingRuntime --output-on-failure` | ON; eleven relocation/privacy scenarios; quick registration selects two |
| Package integration | `python tools/test_streaming_bundle.py --config build/streaming-package-inputs.json -v` | ON; built app/pipeline test, SDK, Qt and pefile; also registered as StreamingBundleTests in exhaustive CTest |
| Owned file diagnostic | `StreamingPackageProbe.exe <absolute-owned-media> <absolute-decoded.png> <absolute-log.json>` | Build with ON/tests; copy probe into a validation-only package clone; no device capture/output/input |

Exhaustive file/sender/viewer/media tests need FFmpeg CLI to generate owned fixtures. The full
viewer suite also requires the exact **Steam Streaming Speakers** virtual endpoint; it never
chooses another. Manual/native capture, audible audio, elevated session delegation and input
probes are permission-gated acceptance tools outside automated `test:all`; exact commands are
in `streaming.md`. They must not be run implicitly on the user's active desktop.

## Native platform operation

- macOS uses pkg-config's actual `pluginsdir`, `pluginscannerdir` and `prefix`. Packaging fails
  when a required plugin/scanner or complete SDK notice directory is absent. Set
  `STREAMING_MEDIA_LICENSE_DIR` when the selected SDK stores notices elsewhere.
- macOS packaging first deploys Qt, then copies media, relocates nested dependencies, signs native
  helpers/libraries and seals/verifies the app. This remains ad-hoc development packaging.
  Developer ID, hardened runtime, nested signatures and notarization require native release
  validation. Development `get-task-allow`/library-validation exceptions are not distribution fixes.
- `NSScreenCaptureUsageDescription` explains selected display/window and enabled audio sharing.
  TCC screen recording and Accessibility authorization remain distinct. Current code uses
  ScreenCaptureKit audio, not a microphone or CoreAudio process-tap fallback. macOS lock-state
  admission remains an unresolved feature blocker; see the active worklist decision.
- Linux requires GStreamer>=1.28.7, libnice, libportal>=0.9.1, PipeWire>=1.0, its GStreamer plugin,
  a working session manager, matching portal backend and login1 session service. X11 additionally
  requires XComposite/XRandR/XFixes and control XInput/XTest/XKB dependencies.
- Linux packaging runs `gst-inspect-1.0` for required factories, resolves actual plugin files,
  then queries dpkg/rpm/pacman for the owning package. Unknown/unpackaged plugin providers cause
  configuration failure. DEB/RPM include installed provider versions; Arch records package names.
  A target repository must supply the required versions; local `/usr/local` SDKs cannot silently
  become undeclared runtime dependencies. The compositor-specific portal backend is a host
  prerequisite rather than an invented universal package name.
- Wayland interactive control still needs the documented physical-input-priority product decision.
  Flatpak does not inherit unrestricted host PipeWire nodes, `/proc` identities or login1 access
  from ScreenCast consent. Broad host/system-bus permissions were not added.

## Troubleshooting and remaining acceptance

| Symptom | Check / action |
|---|---|
| Packaged runtime missing/invalid | Restore matching package marker, plugin directory and scanner; do not add the development SDK to PATH |
| Unlisted plugin or forwarded export packaging error | Use clean staging; explicitly review changed dependency/provider before packaging |
| Missing media factory / decoder | Verify manifest and SDK version; supported file container alone does not ensure its codec is available |
| Scanner warning | Preserve `GST_DEBUG=GST_PLUGIN_LOADING:6` output; verify the packaged helper actually starts and processes plugins |
| No source/audio endpoint or permission refusal | Review the selected source's native permission/session prerequisites; do not substitute another source/device |
| Hidden quick failure | Existing foreground test needs an active desktop; protected audio expectation update awaits exact approval |

MANUAL STEP: on a clean Windows VM/host with no Qt/GStreamer/FFmpeg development installation,
unpack the final portable artifact, verify its hashes, launch the real UI and perform authorized
source capture/encoder/decoder/output checks against a compatible peer; capture actual UI/source
screenshots and device/permission/build logs. Current local evidence does not establish this.

MANUAL STEP: on named macOS and Linux X11/Wayland hosts configure/build/test/install the native
package, verify dependency ownership/relocation/signatures and run the approved source/audio/control
matrix. Provide macOS distribution credentials only for an explicitly requested signed release.
Native packaging sources are uncompiled/unexecuted here; package/source changes need native regression
and check-test validation before platform completion.

Sources: [GStreamer runtime discovery](https://gstreamer.freedesktop.org/documentation/gstreamer/running.html),
[OpenSSL compatibility](https://openssl-library.org/policies/technical/api-compat/),
[Microsoft runtime requirements](https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist),
[Qt licensing](https://doc.qt.io/qt-6/licensing.html),
[ScreenCaptureKit](https://developer.apple.com/documentation/screencapturekit),
[Apple notarization](https://developer.apple.com/documentation/security/notarizing-macos-software-before-distribution),
[Flatpak permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html),
[PipeWire access](https://docs.pipewire.org/page_access.html).

File diagnostics report segment-converted media time. H264 reorder and AAC edit-list offsets make raw decoder PTS unsuitable as file timestamps; generated reference images must use the reported media time. `StreamingFileTimelineTests` covers video/PCM startup and500ms seek with actual decoders.

## macOS local installation evidence, 2026-09-21

See PROJECT.md, Local macOS installation, for exact backup paths and validation limits.
The staged binaries include the shortened IPC names but retain the older embedded git version.
The committed packaging script copied the SDK dependency closure. Signing then required moving
plugins to `Contents/PlugIns/gstreamer` and symlinking `Contents/MacOS/gstreamer-1.0` to that directory.
The dotted physical directory was interpreted as an invalid nested bundle. The automatic-acceptance update now incorporates this correction in the packaging template.
All nested code and the bundle were signed with the existing Developer ID team and hardened runtime.
Strict/deep verification passed. Twelve media factories loaded with the packaged scanner and a new
private registry. Live GUI/core IPC worked and ordinary client connection was established.
The initial install kept TLS disabled. The subsequent authorized configuration enabled TLS and fingerprint checking; the ordinary input connection established TLSv1.3, while streaming still reported `peerUnsupportedOrInsufficientTrust`. Device capture, cross-device
streaming and notarization are not established by these checks. The removed local build directory
prevented running CTest. Historical unrun-platform statements above describe earlier Windows work.


The automatic-acceptance build on 2026-09-21 used the corrected template directly in a fresh
`temp/auto-accept-install` stage. Developer ID signing, strict/deep signature verification,
88-file Mach-O dependency closure and twelve private media factory checks passed. Installed
`/Applications/Deskflow.app` reconnected over TLS 1.3 and streaming reports a compatible peer.
See `macos-validation.md` for commands, backups, failing tests and unresolved native acceptance.
