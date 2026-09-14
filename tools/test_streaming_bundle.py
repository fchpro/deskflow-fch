"""Windows streaming package integration tests; requires a built ON application and SDK."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument("--config", required=True)
args, remaining = parser.parse_known_args()
config = json.loads(Path(args.config).read_text())
root = Path(config["source"])
spec = importlib.util.spec_from_file_location("bundle_streaming", root / "deploy/windows/bundle_streaming.py")
bundler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bundler)


class StreamingBundleTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="deskflow-package-test-")
        self.stage = Path(self.temporary.name)
        shutil.copy2(config["application"], self.stage / "deskflow.exe")

    def tearDown(self):
        self.temporary.cleanup()

    def package(self):
        bundler.bundle(config["sdk"], self.stage, config["qt"], config["crt"], config["minimum_crt"])

    def test_real_relocated_pipeline(self):
        self.package()
        shutil.copy2(config["pipeline_test"], self.stage / "StreamingCapturePipelineTests.exe")
        shutil.copy2(Path(config["qt"]) / "bin/Qt6Test.dll", self.stage / "Qt6Test.dll")
        env = os.environ.copy()
        env["PATH"] = str(self.stage) + ";" + os.environ["SystemRoot"] + "/System32"
        env["GST_PLUGIN_PATH_1_0"] = str(Path(config["sdk"]) / "lib/gstreamer-1.0")
        env["GST_PLUGIN_SYSTEM_PATH_1_0"] = env["GST_PLUGIN_PATH_1_0"]
        log = self.stage / "pipeline.txt"
        result = subprocess.run([str(self.stage / "StreamingCapturePipelineTests.exe"), "-o", str(log) + ",txt"],
                                env=env, capture_output=True, text=True, timeout=15)
        print("COMMAND: relocated StreamingCapturePipelineTests with package/System32 PATH", flush=True)
        print(log.read_text() if log.exists() else result.stderr, "EXIT:", result.returncode, flush=True)
        self.assertEqual(result.returncode, 0)

    def test_sdk_openssl_selected(self):
        shutil.copy2(Path(config["application"]).parent / "libssl-3-x64.dll", self.stage / "libssl-3-x64.dll")
        self.package()
        self.assertEqual(bundler.digest(self.stage / "libssl-3-x64.dll"),
                         bundler.digest(Path(config["sdk"]) / "bin/libssl-3-x64.dll"))

    def test_clean_qt_platform(self):
        (self.stage / "plugins/platforms").mkdir(parents=True)
        shutil.copy2(Path(config["qt"]) / "plugins/platforms/qoffscreen.dll", self.stage / "plugins/platforms/qoffscreen.dll")
        self.package()
        (self.stage / "settings").mkdir()
        (self.stage / "settings/Deskflow.conf").write_text(" ")
        env = {k: v for k, v in os.environ.items() if not k.startswith("QT_")}
        env["PATH"] = str(self.stage) + ";" + os.environ["SystemRoot"] + "/System32"
        env["QT_QPA_PLATFORM"] = "offscreen"
        env["QT_DEBUG_PLUGINS"] = "1"
        env["QT_LOGGING_TO_CONSOLE"] = "1"
        env["QT_FORCE_STDERR_LOGGING"] = "1"
        env["XDG_STATE_HOME"] = str(self.stage / "state")
        result = subprocess.run([str(self.stage / "deskflow.exe"), "--help"], env=env,
                                capture_output=True, text=True, timeout=15)
        print(result.stdout, result.stderr, "EXIT:", result.returncode, flush=True)
        self.assertEqual(result.returncode, 0)
        self.assertIn('"' + str(self.stage / "plugins/platforms/qoffscreen.dll").replace("\\", "/") + '" loaded library',
                      result.stderr.replace("\\", "/"))

    def test_sdk_crypto_selected(self):
        shutil.copy2(Path(config["application"]).parent / "libcrypto-3-x64.dll", self.stage / "libcrypto-3-x64.dll")
        self.package()
        self.assertEqual(bundler.digest(self.stage / "libcrypto-3-x64.dll"),
                         bundler.digest(Path(config["sdk"]) / "bin/libcrypto-3-x64.dll"))

    def test_unlisted_plugin_rejected(self):
        (self.stage / "gstreamer-1.0").mkdir()
        shutil.copy2(Path(config["sdk"]) / "lib/gstreamer-1.0/gstcoreelements.dll",
                     self.stage / "gstreamer-1.0/unlisted.dll")
        with self.assertRaisesRegex(RuntimeError, "Unlisted existing plugins"):
            self.package()

    def test_forwarded_exports_rejected(self):
        with self.assertRaisesRegex(RuntimeError, "Forwarded exports need an explicit dependency audit"):
            bundler.pe_exports(Path(os.environ["SystemRoot"]) / "System32/kernel32.dll")

    def test_old_crt_rejected(self):
        old_crt = self.stage / "old-crt"
        shutil.copytree(config["crt"], old_crt)
        binary = old_crt / "vcruntime140.dll"
        with bundler.pefile.PE(str(binary)) as pe:
            pe.VS_FIXEDFILEINFO[0].FileVersionMS = (14 << 16) | 42
            changed = pe.write()
        binary.write_bytes(changed)
        with self.assertRaisesRegex(RuntimeError, "MSVC runtime is older than the compiler"):
            bundler.bundle(config["sdk"], self.stage, config["qt"], old_crt, config["minimum_crt"])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0], *remaining])
