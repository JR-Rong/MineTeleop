import os
import subprocess
import unittest
from pathlib import Path


SCRIPT = Path("scripts/deploy_vehicle_bundle.sh")


class VehicleBundleDeployScriptTests(unittest.TestCase):
    def test_script_exists_is_executable_and_has_valid_shell_syntax(self):
        self.assertTrue(SCRIPT.is_file(), f"{SCRIPT} should exist")
        self.assertTrue(os.access(SCRIPT, os.X_OK), f"{SCRIPT} should be executable")

        result = subprocess.run(["bash", "-n", str(SCRIPT)], text=True, capture_output=True)

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_dry_run_targets_default_vehicle_ssh_tunnel_without_remote_docker(self):
        result = subprocess.run(
            ["bash", str(SCRIPT), "--dry-run"],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("user@60.205.213.254", result.stdout)
        self.assertIn("ssh -p 6000", result.stdout)
        self.assertIn("scp -P 6000", result.stdout)
        self.assertIn("dist/mine-teleop-ubuntu-x86_64.tar.gz", result.stdout)
        self.assertIn("tar -xzf", result.stdout)
        self.assertIn("bin/mine-teleop --list", result.stdout)
        self.assertIn("bin/ffmpeg -hide_banner -hwaccels", result.stdout)
        self.assertNotIn(" docker ", result.stdout)
        self.assertNotIn("sudo docker", result.stdout)

    def test_dry_run_can_send_media_frames_to_driver_console_url(self):
        result = subprocess.run(
            [
                "bash",
                str(SCRIPT),
                "--dry-run",
                "--driver-console-url",
                "http://control.example:8080",
                "--media-frames",
                "3",
                "--frame-interval-ms",
                "40",
            ],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("vehicle-media-agent", result.stdout)
        self.assertIn("--mode teleop", result.stdout)
        self.assertIn("--driver-console-url \"http://control.example:8080\"", result.stdout)
        self.assertIn("--frames \"3\"", result.stdout)
        self.assertIn("--frame-interval-ms \"40\"", result.stdout)
        self.assertIn("--ffmpeg-binary \"/home/user/mine-teleop/bin/ffmpeg\"", result.stdout)

    def test_dry_run_can_run_control_receiver_with_jsonl_logs(self):
        result = subprocess.run(
            [
                "bash",
                str(SCRIPT),
                "--dry-run",
                "--signaling-http-url",
                "http://control.example:8765",
                "--run-control-teleop",
                "--teleop-duration-ms",
                "20000",
            ],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("vehicle-agent", result.stdout)
        self.assertIn("--teleop", result.stdout)
        self.assertIn("--signaling-http-url \"http://control.example:8765\"", result.stdout)
        self.assertIn("--teleop-duration-ms \"20000\"", result.stdout)
        self.assertIn("--teleop-log-controls", result.stdout)

    def test_run_control_teleop_requires_signaling_url(self):
        result = subprocess.run(
            ["bash", str(SCRIPT), "--dry-run", "--run-control-teleop"],
            text=True,
            capture_output=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("--run-control-teleop requires --signaling-http-url", result.stderr)


if __name__ == "__main__":
    unittest.main()
