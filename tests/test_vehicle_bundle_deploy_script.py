import os
import subprocess
import unittest
from pathlib import Path


SCRIPT = Path("scripts/deploy_vehicle_bundle.sh")
LIVE_CONTROL_SCRIPT = Path("scripts/start_live_control_plane_tunnel.sh")
LIVE_MEDIA_SCRIPT = Path("scripts/run_vehicle_live_media.sh")
LIVE_CONTROL_ONE_COMMAND_SCRIPT = Path("scripts/start_live_control_one_command.sh")
LIVE_VEHICLE_ONE_COMMAND_SCRIPT = Path("scripts/start_live_vehicle_one_command.sh")
TIMESYNC_SCRIPT = Path("scripts/setup_vehicle_timesync.sh")
BUILD_BUNDLE_SCRIPT = Path("scripts/build_ubuntu_bundle.py")


class VehicleBundleDeployScriptTests(unittest.TestCase):
    def test_script_exists_is_executable_and_has_valid_shell_syntax(self):
        self.assertTrue(SCRIPT.is_file(), f"{SCRIPT} should exist")
        self.assertTrue(os.access(SCRIPT, os.X_OK), f"{SCRIPT} should be executable")

        result = subprocess.run(["bash", "-n", str(SCRIPT)], text=True, capture_output=True)

        self.assertEqual(result.returncode, 0, result.stderr)

    def test_live_run_scripts_are_executable_and_have_valid_shell_syntax(self):
        for script in (
            LIVE_CONTROL_SCRIPT,
            LIVE_MEDIA_SCRIPT,
            LIVE_CONTROL_ONE_COMMAND_SCRIPT,
            LIVE_VEHICLE_ONE_COMMAND_SCRIPT,
            TIMESYNC_SCRIPT,
        ):
            self.assertTrue(script.is_file(), f"{script} should exist")
            self.assertTrue(os.access(script, os.X_OK), f"{script} should be executable")
            result = subprocess.run(["bash", "-n", str(script)], text=True, capture_output=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_one_command_control_script_wraps_control_plane_and_tunnel_without_real_password(self):
        text = LIVE_CONTROL_ONE_COMMAND_SCRIPT.read_text(encoding="utf-8")

        self.assertIn('VEHICLE_SSH_HOST="60.205.213.254"', text)
        self.assertIn('VEHICLE_SSH_PORT="6000"', text)
        self.assertIn('VEHICLE_SSH_USER="user"', text)
        self.assertIn('VEHICLE_SSH_PASSWORD="CHANGE_ME"', text)
        self.assertIn("scripts/start_live_control_plane_tunnel.sh", text)
        self.assertIn("MINE_TELEOP_VEHICLE_SSH_HOST", text)
        self.assertIn("MINE_TELEOP_OPEN_BROWSER", text)
        self.assertNotIn("cz666666", text)

    def test_one_command_vehicle_script_pins_camera_mapping_and_adaptive_controls(self):
        text = LIVE_VEHICLE_ONE_COMMAND_SCRIPT.read_text(encoding="utf-8")

        self.assertIn('CAMERA_DEVICES="front=/dev/video0 rear=/dev/video2"', text)
        self.assertIn('MVS_CAMERAS="hikrobot=mvs:0"', text)
        self.assertIn('PYLON_CAMERAS="basler=pylon:0"', text)
        self.assertIn('CAMERA_CONTROL_PROFILE="adaptive"', text)
        self.assertIn('CAMERA_ADAPTIVE_BRIGHTNESS="0"', text)
        self.assertIn('CAMERA_ADAPTIVE_CONTRAST="32"', text)
        self.assertIn("MINE_TELEOP_CAMERA_GAIN_AUTOMATIC", text)
        self.assertIn("MINE_TELEOP_CAMERA_WHITE_BALANCE_AUTO", text)
        self.assertIn("scripts/run_vehicle_live_media.sh", text)
        self.assertIn("MINE_TELEOP_CAMERA_DEVICES", text)
        self.assertIn("MINE_TELEOP_CAMERA_CONTROL_PROFILE", text)

    def test_one_command_vehicle_script_defaults_to_480p15_low_bandwidth_media(self):
        text = LIVE_VEHICLE_ONE_COMMAND_SCRIPT.read_text(encoding="utf-8")

        self.assertIn('REALTIME_PROFILE="realtime_480p15"', text)
        self.assertIn('CAPTURE_WIDTH="640"', text)
        self.assertIn('CAPTURE_HEIGHT="480"', text)
        self.assertIn('CAPTURE_FPS="15"', text)
        self.assertIn('MVS_CAPTURE_WIDTH="640"', text)
        self.assertIn('MVS_CAPTURE_HEIGHT="512"', text)
        self.assertIn('PYLON_CAPTURE_WIDTH="640"', text)
        self.assertIn('PYLON_CAPTURE_HEIGHT="512"', text)
        self.assertIn('MVS_JPEG_QUALITY="65"', text)
        self.assertIn("MINE_TELEOP_REALTIME_PROFILE", text)
        self.assertIn("MINE_TELEOP_MVS_JPEG_QUALITY", text)

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

    def test_ubuntu_bundle_includes_live_vehicle_one_command_script(self):
        text = BUILD_BUNDLE_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("start_live_vehicle_one_command.sh", text)
        self.assertIn("/workspace/output/scripts/start_live_vehicle_one_command.sh", text)

    def test_ubuntu_bundle_includes_pylon_bridge_source(self):
        text = BUILD_BUNDLE_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("pylon_camera_bridge.cpp", text)
        self.assertIn("/workspace/output/scripts/pylon_camera_bridge.cpp", text)

    def test_ubuntu_bundle_collects_runtime_dispatched_mine_teleop_modules(self):
        text = BUILD_BUNDLE_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("--collect-submodules mine_teleop", text)

    def test_ubuntu_bundle_hidden_imports_runtime_dispatched_vehicle_modules(self):
        text = BUILD_BUNDLE_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("--hidden-import mine_teleop.vehicle_media_runtime", text)
        self.assertIn("--hidden-import mine_teleop.vehicle_teleop_runtime", text)
        self.assertIn("--hidden-import platform", text)
        self.assertIn("--hidden-import copy", text)

    def test_live_media_script_exposes_adaptive_and_low_light_camera_controls(self):
        text = LIVE_MEDIA_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("MINE_TELEOP_CAMERA_CONTROL_PROFILE", text)
        self.assertIn("adaptive", text)
        self.assertIn("apply_adaptive_camera_controls", text)
        self.assertIn("white_balance_temperature_auto", text)
        self.assertIn("power_line_frequency", text)
        self.assertIn("MINE_TELEOP_CAMERA_LOW_LIGHT", text)
        self.assertIn("apply_low_light_camera_controls", text)
        self.assertIn("brightness", text)
        self.assertIn("contrast", text)
        self.assertIn("gain", text)
        self.assertIn("gamma", text)
        self.assertIn("backlight_compensation", text)
        self.assertIn("exposure_dynamic_framerate", text)

    def test_live_media_script_enables_all_detected_camera_devices_with_mjpeg_codec(self):
        text = LIVE_MEDIA_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("MINE_TELEOP_CAMERA_DEVICES", text)
        self.assertIn("MINE_TELEOP_ENABLE_MVS_CAMERA", text)
        self.assertIn("MINE_TELEOP_MVS_SDK_DIR", text)
        self.assertIn("find_mvs_camera_devices()", text)
        self.assertIn("mvs-camera-bridge --sdk-root", text)
        self.assertIn("hikrobot=mvs:0", text)
        self.assertIn("is_mvs_camera_device", text)
        self.assertIn("MINE_TELEOP_ENABLE_PYLON_CAMERA", text)
        self.assertIn("MINE_TELEOP_PYLON_ROOT", text)
        self.assertIn("find_pylon_camera_devices()", text)
        self.assertIn("ensure_pylon_camera_bridge()", text)
        self.assertIn("Basler/pylon camera mapping is configured but pylon bridge is unavailable", text)
        self.assertIn('"$PYLON_BRIDGE_BIN" --list --json', text)
        self.assertIn("basler=pylon:0", text)
        self.assertIn("is_pylon_camera_device", text)
        self.assertIn("find_camera_devices()", text)
        self.assertIn("camera_device_pairs", text)
        self.assertIn("write_live_config \"${camera_device_pairs[@]}\"", text)
        self.assertIn('FRAMES="${MINE_TELEOP_MEDIA_FRAMES:-300}"', text)
        self.assertIn('FRAME_CODEC="${MINE_TELEOP_FRAME_CODEC:-mjpeg}"', text)
        self.assertIn('REALTIME_PROFILE="${MINE_TELEOP_REALTIME_PROFILE:-realtime_720p}"', text)
        self.assertIn("MINE_TELEOP_MVS_JPEG_QUALITY", text)
        self.assertIn("f\"    realtime_profile: {realtime_profile}\"", text)
        self.assertIn('--frame-codec "$FRAME_CODEC"', text)

    def test_live_media_config_generation_preserves_hardware_section_indentation(self):
        text = LIVE_MEDIA_SCRIPT.read_text(encoding="utf-8")

        self.assertIn("suffix.rstrip()", text)
        self.assertNotIn("suffix.lstrip()", text)

    def test_live_control_tunnel_exposes_signaling_for_vehicle_control_feedback(self):
        text = LIVE_CONTROL_SCRIPT.read_text(encoding="utf-8")

        self.assertIn('SIGNALING_LOCAL_PORT="${MINE_TELEOP_SIGNALING_LOCAL_PORT:-8765}"', text)
        self.assertIn('SIGNALING_REMOTE_PORT="${MINE_TELEOP_SIGNALING_REMOTE_PORT:-18765}"', text)
        self.assertIn('-R ${remote_signaling_port}:127.0.0.1:${signaling_local_port}', text)
        self.assertIn('VEHICLE_SIDE_SIGNALING_HTTP_URL=http://127.0.0.1:${SIGNALING_REMOTE_PORT}', text)
        self.assertIn('--teleop-log-controls', text)

    def test_dry_run_targets_configured_vehicle_ssh_tunnel_without_remote_docker(self):
        result = subprocess.run(
            ["bash", str(SCRIPT), "--dry-run", "--host", "203.0.113.9", "--user", "fielduser", "--port", "6000"],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("fielduser@203.0.113.9", result.stdout)
        self.assertIn("ssh -p 6000", result.stdout)
        self.assertIn("scp -P 6000", result.stdout)
        self.assertIn("dist/mine-teleop-ubuntu-x86_64.tar.gz", result.stdout)
        self.assertIn("tar -xzf", result.stdout)
        self.assertIn("bin/mine-teleop --list", result.stdout)
        self.assertIn("bin/ffmpeg -hide_banner -hwaccels", result.stdout)
        self.assertNotIn(" docker ", result.stdout)
        self.assertNotIn("sudo docker", result.stdout)

    def test_deploy_replaces_existing_scripts_directory_from_bundle(self):
        text = SCRIPT.read_text(encoding="utf-8")

        self.assertIn('"$REMOTE_DIR/scripts"', text)

    def test_deploy_removes_extracting_directory_after_move(self):
        text = SCRIPT.read_text(encoding="utf-8")

        self.assertIn('rm -rf "$REMOTE_DIR/.extracting"', text)
        self.assertNotIn('rmdir "$REMOTE_DIR/.extracting"', text)

    def test_dry_run_requires_host_and_user_when_not_dry(self):
        result = subprocess.run(
            ["bash", str(SCRIPT)],
            text=True,
            capture_output=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required", result.stderr)

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
