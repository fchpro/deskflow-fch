# PROJECT.md — deskflow-fch

## Overview

Personal fork of [Deskflow](https://github.com/deskflow/deskflow), an open-source keyboard/mouse sharing app (share one keyboard and mouse across multiple computers over the network, client/server model). This fork exists to apply personal customizations while staying updatable from upstream releases. Not intended for redistribution.

## Tech Stack

- C++20
- CMake (>= 3.24)
- Qt 6 (GUI)
- vcpkg optional on Windows (`VCPKG_QT` option)
- Google Test (unit tests in `src/unittests`)
- Platform backends: Windows, macOS, Linux (X11/Wayland)

Planned: none yet.

## Project Structure

- `CMakeLists.txt` — root build config, version (currently 1.26.0 fallback, git-derived when available)
- `src/apps/deskflow-core` — core CLI app (client/server engine)
- `src/apps/deskflow-daemon` — background daemon
- `src/apps/deskflow-gui` — Qt GUI app
- `src/lib/` — libraries: `arch` (OS abstraction), `base`, `client`, `server`, `net`, `platform` (per-OS input/screen), `deskflow` (core logic), `gui`, `common`, `io`, `mt`
- `src/unittests/` — Google Test unit tests mirroring lib layout
- `cmake/` — CMake modules
- `deploy/` — packaging/installer resources
- `docs/` — upstream docs (`docs/dev/build.md` = build instructions)
- `translations/` — Qt translation files
- `docs/project/` — fork-specific reference docs (create as needed)
- `docs/tasks/` — active task files

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

## Unfinished Tasks and Worklists

None

## Quick Check

Quick check = run all unit tests against the last build (~5 s). From a VS x64 dev prompt (`VsDevCmd.bat -arch=x64`) with `C:\Qt\6.10.3\msvc2022_64\bin` and `build\vcpkg_installed\x64-windows-release\bin` prepended to PATH:

```
cd build\src\unittests && ctest --output-on-failure
```

Full build (fullcheck equivalent, minutes): `cmake --build build -j12` from the same environment. Note: the default `all` target also runs ctest; without Qt on PATH tests exit 0xc0000135.

**Local toolchain**: MSVC 2022 Community, Qt 6.10.3 at `C:\Qt\6.10.3\msvc2022_64` (installed via aqtinstall), vcpkg at `C:\Work\tools\vcpkg` (openssl via manifest), Ninja via pip (`%APPDATA%\Python\Python313\Scripts\ninja.exe`). Configure command used: `cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=C:\Qt\6.10.3\msvc2022_64 -DCMAKE_TOOLCHAIN_FILE=C:\Work\tools\vcpkg\scripts\buildsystems\vcpkg.cmake -DVCPKG_TARGET_TRIPLET=x64-windows-release -DBUILD_INSTALLER=OFF`.

## Master Tests

Unit tests (Qt Test): 28 test binaries (26 upstream + 2 fork) under `src/unittests`, run via `ctest` from `build\src\unittests` (see Quick Check for the required PATH). No external master-test project.

## Architecture and Workflow Notes

- Client/server architecture: one machine runs the server (keyboard/mouse owner), others run clients; `src/lib/net` handles transport, `src/lib/platform` handles per-OS input injection/capture.
- Version is derived from git describe; fallback version is hardcoded in root `CMakeLists.txt`.
- Fork policy: never commit personal customizations to `master`; that branch must stay clean for upstream syncs.
- Mouse send-rate limiter (server): `MouseMoveCoalescer`, setting `server/mouseSendRateHz` (default 250, 0 = off); see `docs/project/customizations.md`.
- Game/app exclusion feature (Windows server only): see `docs/project/customizations.md` — new settings key `server/excludedApps`, watcher class `MSWindowsForegroundWatcher`, hook pause via `kHOOK_DISABLE`.
- All git mutations (branching, merging, pulling upstream) are performed by the user, not the LLM.
