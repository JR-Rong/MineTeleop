import os
import subprocess
import unittest
from pathlib import Path


SCRIPT = Path("scripts/deploy_vehicle_bundle.sh")
LIVE_CONTROL_SCRIPT = Path("scripts/start_live_control_plane_tunnel.sh")
LIVE_MEDIA_SCRIPT = Path("scripts/run_vehicle_live_media.sh")
TIMESYNC_SCRIPT = Path("scripts/setup_vehicle_timesync.sh")
BUILD_BUNDLE_SCRIPT = Path("scripts/build_ubuntu_bundle.py")


class VehicleBundleDeployScriptTests(unittest.TestCase):
    def test_script_exists_is_executable_and_has_valid_shell_syntax(self):
        self.assertTrue(SCRIPT.is_file(), f"{SCRIPT} should exist")
        self.assertTrue(os.access(SCRIPT, os.X_OK), f"{SCRIPT} should be executable")

        result = subprocess.run(["bash", "-n", str(SCRIPT)], text=True, capture_output=True)

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_live_run_scripts_are_executable_and_have_valid_shell_syntax(self):
        for script in (LIVE_CONTROL_SCRIPT, LIVE_MEDIA_SCRIPT, TIMESYNC_SCRIPT):
            self.assertTrue(script.is_file(), f"{script} should exist")
            self.assertTrue(os.access(script, os.X_OK), f"{script} should be executable")
            result = subprocess.run(["bash", "-n", str(script)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_timesync_script_configures_chrony_without_embedding_passwords(self):
        text = TIMESYNC_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("MINE_TELEOP_NTP_SERVERS", text)
        self.assertIn("MINE_TELEOP_TIMESYNC_BACKEND", text)
        self.assertIn("chrony", text)
        self.assertIn("chronyc tracking", text)
        self.assertIn("chronyc sources -v", text)
        self.assertIn("systemd-timesyncd", text)
        self.assertIn("timedatectl timesync-status", text)
        self.assertIn("timedatectl status", text)
        self.assertIn("systemctl enable --now chrony", text)
        self.assertIn("systemctl enable --now systemd-timesyncd", text)
        self.assertNotIn("cz666666", text)

    def test_ubuntu_bundle_includes_vehicle_timesync_script(self):
        text = BUILD_BUNDLE_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("setup_vehicle_timesync.sh", text)
        self.assertIn("/workspace/output/scripts/setup_vehicle_timesync.sh", text)

    def test_live_media_script_exposes_low_light_camera_controls(self):
        text = LIVE_MEDIA_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("MINE_TELEOP_CAMERA_LOW_LIGHT", text)
        self.assertIn("brightness=", text)
        self.assertIn("gain=", text)
        self.assertIn("gamma=", text)
        self.assertIn("backlight_compensation=", text)
        self.assertIn("exposure_dynamic_framerate=", text)

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
