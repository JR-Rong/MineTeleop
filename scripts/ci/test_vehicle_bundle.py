"""Exercise archive admission before the real Linux package-runtime checks.

These tests never start Docker or execute bundled binaries. The fake Docker
boundary deliberately fails instead of reporting a fictitious runtime pass.
"""
import hashlib
import io
import os
from pathlib import Path
import subprocess
import tarfile
import tempfile
import unittest


CHECKER = Path(__file__).resolve().parents[1] / "test" / "check_cpp_ubuntu_bundle.sh"
PACKAGE = "mine-teleop-vehicle-ubuntu22.04-x64-fixture"
PACKAGE_FILES = (
    "bin/mine-teleop", "bin/mine-teleop-run", "bin/mine-teleop-recorder",
    "bin/mine-teleop-aravis-camera", "lib/vendor/chassis/libmine_teleop_chassis_bridge.so",
    "lib/gstreamer-1.0/libgstvideorate.so",
    "lib/vendor/chassis/libchassis_control.so", "config/vehicle-agent.yaml",
    "config/mine-teleop-field-root.crt", "deployments/udev/99-mine-teleop-basler.rules",
    "scripts/setup_basler_usb_access.sh", "BUILD-INFO.txt", "README.txt",
)


class VehicleBundleAdmissionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.archive = self.root / (PACKAGE + ".tar.gz")
        self.marker = self.root / "docker-boundary.log"
        tools = self.root / "tools"
        tools.mkdir()
        fake_docker = tools / "docker"
        fake_docker.write_text('#!/bin/sh\nprintf "%s\\n" "$*" >> "$BUNDLE_TEST_DOCKER_MARKER"\nexit 77\n')
        fake_docker.chmod(0o755)
        self.environment = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ.get("PATH", ""),
                                BUNDLE_TEST_DOCKER_MARKER=str(self.marker))

    def package(self, omit=None):
        with tarfile.open(self.archive, "w:gz") as archive:
            for relative in PACKAGE_FILES:
                if relative == omit:
                    continue
                member = tarfile.TarInfo(PACKAGE + "/" + relative)
                member.size = 1
                member.mode = 0o755 if relative.startswith("bin/") else 0o644
                archive.addfile(member, io.BytesIO(b"x"))
        checksum = hashlib.sha256(self.archive.read_bytes()).hexdigest()
        Path(str(self.archive) + ".sha256").write_text(checksum + "  " + self.archive.name + "\n")

    def check(self):
        return subprocess.run(["bash", str(CHECKER), str(self.archive)], env=self.environment,
                              capture_output=True, text=True, timeout=10)

    def test_missing_recorder_is_rejected_before_any_runtime_check(self):
        self.package(omit="bin/mine-teleop-recorder")
        result = self.check()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("required bundle path is missing: " + PACKAGE + "/bin/mine-teleop-recorder", result.stderr)
        self.assertFalse(self.marker.exists(), "missing helper reached Docker execution")

    def test_present_recorder_reaches_only_the_isolated_runtime_boundary(self):
        self.package()
        result = self.check()
        self.assertEqual(result.returncode, 77, result.stderr)
        invocation = self.marker.read_text()
        self.assertIn("create --platform linux/amd64 --network none", invocation)
        self.assertNotIn("ubuntu_bundle_check=passed", result.stdout)

    def test_missing_videorate_is_rejected_before_any_runtime_check(self):
        relative = "lib/gstreamer-1.0/libgstvideorate.so"
        self.package(omit=relative)
        result = self.check()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("required bundle path is missing: " + PACKAGE + "/" + relative, result.stderr)
        self.assertFalse(self.marker.exists(), "missing videorate reached Docker execution")

    def test_missing_checksum_is_rejected_before_any_runtime_check(self):
        self.package()
        Path(str(self.archive) + ".sha256").unlink()
        result = self.check()
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertIn("bundle checksum is missing", result.stderr)
        self.assertFalse(self.marker.exists(), "unverified archive reached Docker execution")


if __name__ == "__main__":
    unittest.main()
