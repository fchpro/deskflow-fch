# Dependency notice sources

- `qt-6.10.3/`: unmodified license texts from `qt/qtbase` tag `v6.10.3`, `LICENSES/`.
  Source: https://github.com/qt/qtbase/tree/v6.10.3/LICENSES
- Windows package also includes the installed Qt SBOM files and the pinned GStreamer
  SDK's complete `share/licenses/` and `share/versions.txt`.
- `msvc-14.44.35211/license.rtf`: unmodified English `license.rtf` (Burn payload `u4`)
  from the Microsoft-signed redistributable14.44.35211.0 downloaded from
  https://aka.ms/vs/17/release/vc_redist.x64.exe . Runtime DLLs were extracted into an
  owned development directory without running the installer or changing system state.
- These files record notices; they do not certify redistribution compliance or replace
  any corresponding-source/relinking obligations. This fork is for personal use.
