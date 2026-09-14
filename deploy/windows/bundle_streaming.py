"""Complete a Windows install with the pinned, private GStreamer runtime.

Packaging-only dependency: pefile. No SDK tools or Python are needed at runtime.
Existing SDK/vcpkg OpenSSL copies are deliberately unified to SDK 3.5.0, then
every ordinary/delay import into a bundled DLL is checked against its exports.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import sys

import pefile

PLUGINS = """app coreelements audioconvert audioresample volume videoconvertscale
playback typefindfunctions matroska isomp4 vpx libav videoparsersbad audioparsers
opus opusparse vorbis ogg rtp rtpmanager rsrtp nice dtls srtp sctp webrtc wasapi2
videotestsrc audiotestsrc""".split()


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def pe_imports(path):
    with pefile.PE(str(path), fast_load=True, max_symbol_exports=65536) as pe:
        pe.parse_data_directories(directories=[1, 13])
        return [(entry.dll.decode().lower(), [imp.name or imp.ordinal for imp in entry.imports])
                for category in ("DIRECTORY_ENTRY_IMPORT", "DIRECTORY_ENTRY_DELAY_IMPORT")
                for entry in getattr(pe, category, [])]


def pe_exports(path):
    with pefile.PE(str(path), fast_load=True, max_symbol_exports=65536) as pe:
        pe.parse_data_directories(directories=[0])
        if hasattr(pe, "DIRECTORY_ENTRY_EXPORT") and any(s.forwarder for s in pe.DIRECTORY_ENTRY_EXPORT.symbols):
            raise RuntimeError(f"Forwarded exports need an explicit dependency audit: {path.name}")
        return {value for entry in getattr(pe, "DIRECTORY_ENTRY_EXPORT", object()).symbols
                for value in (entry.name, entry.ordinal)} if hasattr(pe, "DIRECTORY_ENTRY_EXPORT") else set()


def bundle(sdk, destination, qt, crt, minimum_crt):
    sdk, destination, qt, crt = [Path(p).resolve() for p in (sdk, destination, qt, crt)]
    with pefile.PE(str(crt / "vcruntime140.dll")) as pe:
        version = pe.VS_FIXEDFILEINFO[0].FileVersionMS
    actual_crt = (version >> 16, version & 65535)
    required_crt = tuple(int(value) for value in minimum_crt.split("."))
    if actual_crt < required_crt:
        raise RuntimeError(f"MSVC runtime is older than the compiler: {actual_crt} < {required_crt}")
    if (sdk / "lib/pkgconfig/gstreamer-1.0.pc").read_text().split("Version: ")[1].splitlines()[0] != "1.28.7":
        raise RuntimeError("Packaging requires GStreamer SDK exactly 1.28.7")
    if 'OPENSSL_VERSION_TEXT "OpenSSL 3.5.0 ' not in (sdk / "include/openssl/opensslv.h").read_text():
        raise RuntimeError("Packaging requires the SDK OpenSSL 3.5.0")
    if not (destination / "deskflow.exe").is_file():
        raise RuntimeError("Install Deskflow before bundling its streaming runtime")
    expected_plugins = {f"gst{name}.dll" for name in PLUGINS}
    unexpected = [p.name for p in (destination / "gstreamer-1.0").glob("*") if p.name not in expected_plugins]
    if unexpected:
        raise RuntimeError(f"Unlisted existing plugins; use a fresh staging directory: {unexpected}")
    records = []

    def copy(source, target):
        if not source.is_file():
            raise RuntimeError(f"Required runtime file missing: {source}")
        target.parent.mkdir(parents=True, exist_ok=True)
        previous = digest(target) if target.exists() else None
        shutil.copy2(source, target)
        records.append({"path": target.relative_to(destination).as_posix(), "source": str(source),
                        "sha256": digest(target), "replaced_sha256": previous})

    for plugin in PLUGINS:
        copy(sdk / f"lib/gstreamer-1.0/gst{plugin}.dll", destination / f"gstreamer-1.0/gst{plugin}.dll")
    copy(sdk / "libexec/gstreamer-1.0/gst-plugin-scanner.exe", destination / "gst-plugin-scanner.exe")
    for runtime_file in crt.glob("*.dll"):
        copy(runtime_file, destination / runtime_file.name)
    for name in ("libcrypto-3-x64.dll", "libssl-3-x64.dll"):
        print(f"Selected SDK OpenSSL 3.5.0: {name}", flush=True)
        copy(sdk / "bin" / name, destination / name)

    system = Path(os.environ["SystemRoot"]) / "System32"
    search = [sdk / "bin", qt / "bin", crt]
    queue = list(destination.rglob("*.dll")) + list(destination.rglob("*.exe"))
    visited = set()
    audit = []
    exports = {}
    while queue:
        binary = queue.pop()
        if binary in visited:
            continue
        visited.add(binary)
        for name, symbols in pe_imports(binary):
            if name.startswith(("api-ms-", "ext-ms-")):
                continue  # OS API-set contracts have no redistributable file.
            target = destination / name
            if not target.exists():
                source = next((folder / name for folder in search if (folder / name).is_file()), None)
                if source:
                    copy(source, target)
                    queue.append(target)
                elif (system / name).is_file():
                    continue
                else:
                    raise RuntimeError(f"Unresolved dependency {name} from {binary.name}")
            if target not in exports:
                exports[target] = pe_exports(target)
            missing = [s.decode() if isinstance(s, bytes) else s for s in symbols if s not in exports[target]]
            if missing:
                raise RuntimeError(f"Unresolved imports {binary.name} -> {name}: {missing}")
            audit.append({"binary": binary.relative_to(destination).as_posix(), "dependency": name,
                          "symbols_checked": len(symbols)})

    shutil.copytree(sdk / "share/licenses", destination / "licenses/gstreamer", dirs_exist_ok=True)
    copy(sdk / "share/versions.txt", destination / "licenses/gstreamer-versions.txt")
    shutil.copytree(qt / "sbom", destination / "licenses/qt-sbom", dirs_exist_ok=True)
    shutil.copytree(Path(__file__).parents[1] / "licenses/qt-6.10.3", destination / "licenses/qt-6.10.3", dirs_exist_ok=True)
    shutil.copytree(Path(__file__).parents[1] / "licenses/msvc-14.44.35211", destination / "licenses/msvc-14.44.35211", dirs_exist_ok=True)
    copy(Path(__file__).parents[2] / "docs/project/streaming.md", destination / "docs/streaming.md")
    copy(Path(__file__).parents[2] / "docs/project/streaming-packaging.md", destination / "docs/streaming-packaging.md")
    (destination / "streaming-runtime.version").write_bytes(b"1.28.7\n")
    manifest = {"gstreamer": "1.28.7", "openssl": "3.5.0", "msvc_runtime": actual_crt,
                "minimum_msvc_runtime": required_crt, "plugins": PLUGINS,
                "files": records, "import_audit": audit,
                "runtime_files": [{"path": p.relative_to(destination).as_posix(), "sha256": digest(p)}
                                  for p in sorted(destination.rglob("*")) if p.is_file() and p.suffix in (".dll", ".exe")]}
    (destination / "streaming-runtime.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"Bundled {len(PLUGINS)} plugins; checked {len(audit)} dependency edges across {len(visited)} PE files", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("sdk", "destination", "qt", "crt", "minimum-crt"):
        parser.add_argument("--" + name, required=True)
    args = parser.parse_args()
    try:
        bundle(args.sdk, args.destination, args.qt, args.crt, args.minimum_crt)
    except (OSError, RuntimeError, pefile.PEFormatError) as error:
        print(f"Streaming packaging failed: {error}", file=sys.stderr)
        sys.exit(1)
