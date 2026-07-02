import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from urllib.parse import parse_qs, urlparse

from mine_teleop.config import (
    ConfigError,
    RuntimeConfigUpdatePolicy,
    effective_vehicle_config_log_payload,
    load_driver_config,
    load_vehicle_config,
)
from mine_teleop.upload import upload_credential_service_from_config


class ConfigContractTests(unittest.TestCase):
    def test_vehicle_config_exposes_cloud_ice_estop_and_time_sync_contract(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))

        self.assertEqual(config.cloud.signaling_url, "ws://127.0.0.1:8765/signaling")
        self.assertEqual(config.cloud.auth_url, "http://127.0.0.1:8765/auth")
        self.assertEqual(config.ice.stun_servers, ["stun:127.0.0.1:3478"])
        self.assertEqual(config.ice.turn_servers, [])
        self.assertTrue(config.control.estop_latch)
        self.assertTrue(config.control.estop_reset_requires_local_confirmation)
        self.assertEqual(config.control.time_sync_minimum, "ntp")
        self.assertEqual(config.upload.trigger_segments, 20)
        self.assertTrue(config.upload.trigger_network_idle)
        self.assertTrue(config.upload.direct_file_upload)

    def test_vehicle_config_exposes_target_hardware_and_field_safety_contract(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        payload = effective_vehicle_config_log_payload(config)

        self.assertEqual(config.hardware.can.interface, "can0")
        self.assertEqual(config.hardware.can.bitrate, 500000)
        self.assertEqual(config.hardware.can.probe_timeout_seconds, 3)
        self.assertEqual(config.hardware.encoding.vaapi_render_device, "/dev/dri/renderD128")
        self.assertEqual(config.hardware.encoding.dri_card_device, "/dev/dri/card1")
        self.assertTrue(config.hardware.encoding.require_hardware_encoder)
        self.assertIn("vaapih264enc", config.hardware.encoding.gstreamer_hardware_plugins)
        self.assertEqual(config.hardware.network.interface, "wwan0")
        self.assertEqual(config.field_safety.commissioning_mode, "bench")
        self.assertEqual(config.field_safety.max_speed_kph, 40.0)
        self.assertTrue(config.field_safety.require_can_feedback_before_control)
        self.assertTrue(config.field_safety.require_local_estop_reset)
        self.assertTrue(config.field_safety.require_time_sync)
        self.assertEqual(payload["hardware"]["can"]["interface"], "can0")
        self.assertEqual(payload["hardware"]["encoding"]["vaapi_render_device"], "/dev/dri/renderD128")
        self.assertEqual(payload["field_safety"]["commissioning_mode"], "bench")

    def test_vehicle_config_can_load_toml_config_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle-agent.toml"
            path.write_text(
                """
[vehicle]
id = "vehicle-toml"
name = "mine-truck-toml"

[cloud]
signaling_url = "ws://127.0.0.1:8765/signaling"
auth_url = "http://127.0.0.1:8765/auth"

[ice]
stun_servers = ["stun:127.0.0.1:3478"]
turn_servers = []

[control]
rate_hz = 20
max_command_gap_ms = 200
degraded_timeout_ms = 300
control_timeout_ms = 800

[control.timeout_action]
throttle = 0.0

[[control.timeout_action.deceleration_profile]]
after_ms = 0
brake = 0.3

[[control.timeout_action.deceleration_profile]]
after_ms = 500
brake = 0.6

[control.estop]
latch = true
reset_requires_local_confirmation = true

[control.time_sync]
minimum = "ntp"

[media.realtime_profiles.realtime_720p]
codec = "h264"
encoder = "vaapi"
width = 1280
height = 720
fps = 30
bitrate_kbps = 3000
keyframe_interval_frames = 30
low_latency = true

[media.record_profiles.record_source_h264]
codec = "h264"
encoder = "vaapi"
width = "source"
height = "source"
fps = "source"
bitrate_kbps = 8000
segment_seconds = 60

[[cameras]]
id = "front"
enabled = true
device = "testsrc"
capture_width = 1920
capture_height = 1080
capture_fps = 30
realtime_profile = "realtime_720p"
record_profile = "record_source_h264"

[recording]
root_dir = ".local/recordings"
retention_target_hours = 8
min_free_gb = 5
delete_uploaded_when_below_free_gb = 2
delete_unuploaded_when_below_free_gb = false
upload_lag_policy = "alert_and_preserve_uploaded_only"

[upload]
enabled = true
backend = "local_archive"
max_bandwidth_mbps = 5
trigger_segments = 20
trigger_network_idle = true
direct_file_upload = true
presigned_url_refresh_margin_seconds = 300
retry_initial_seconds = 10
retry_max_seconds = 600

[vehicle_adapter]
type = "mock"
""",
                encoding="utf-8",
            )

            try:
                config = load_vehicle_config(path)
            except Exception as exc:  # pragma: no cover - exercised during RED before TOML support exists.
                self.fail(f"TOML vehicle config should load through load_vehicle_config: {exc}")

        self.assertEqual(config.vehicle_id, "vehicle-toml")
        self.assertEqual(config.cloud.signaling_url, "ws://127.0.0.1:8765/signaling")
        self.assertEqual(config.control.deceleration_profile, [(0, 0.3), (500, 0.6)])
        self.assertEqual(config.realtime_profiles["realtime_720p"].bitrate_kbps, 3000)
        self.assertEqual(config.record_profiles["record_source_h264"].width, "source")
        self.assertEqual(config.cameras[0].camera_id, "front")
        self.assertEqual(config.upload.trigger_segments, 20)
        self.assertEqual(config.vehicle_adapter_type, "mock")

    def test_driver_config_can_load_toml_config_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "driver-console.toml"
            path.write_text(
                """
[driver]
id = "driver-console-toml"

[cloud]
auth_url = "http://127.0.0.1:8765/auth"
signaling_url = "ws://127.0.0.1:8765/signaling"

[ui]
default_layout = "grid_4"
show_debug_overlay = true

[control]
rate_hz = 20
estop_hold_ms = 500

[control.keyboard]
steering_left = "A"
steering_right = "D"
throttle = "W"
brake = "S"
estop = "E"
""",
                encoding="utf-8",
            )

            try:
                config = load_driver_config(path)
            except Exception as exc:  # pragma: no cover - exercised during RED before TOML support exists.
                self.fail(f"TOML driver config should load through load_driver_config: {exc}")

        self.assertEqual(config.driver_id, "driver-console-toml")
        self.assertEqual(config.cloud.signaling_url, "ws://127.0.0.1:8765/signaling")
        self.assertEqual(config.ui.default_layout, "grid_4")
        self.assertEqual(config.control.rate_hz, 20)
        self.assertEqual(config.control.keyboard.estop, "E")

    def test_cloud_config_rejects_public_plaintext_urls(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"

            signaling_yaml = _valid_vehicle_yaml("  turn_servers: []").replace(
                "ws://127.0.0.1:8765/signaling",
                "ws://teleop.example.com/signaling",
            )
            path.write_text(signaling_yaml, encoding="utf-8")
            with self.assertRaisesRegex(ConfigError, "cloud.signaling_url must use TLS for non-loopback hosts"):
                load_vehicle_config(path)

            auth_yaml = _valid_vehicle_yaml("  turn_servers: []").replace(
                "http://127.0.0.1:8765/auth",
                "http://teleop.example.com/auth",
            )
            path.write_text(auth_yaml, encoding="utf-8")
            with self.assertRaisesRegex(ConfigError, "cloud.auth_url must use TLS for non-loopback hosts"):
                load_vehicle_config(path)

    def test_control_deceleration_profile_rejects_non_numeric_brake_values(self):
        cases = [
            ("brake: true", "boolean"),
            ("brake: emergency", "unknown string"),
        ]
        for replacement, label in cases:
            with self.subTest(label=label):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(
                        _valid_vehicle_yaml("  turn_servers: []").replace("brake: 0.6", replacement),
                        encoding="utf-8",
                    )

                    with self.assertRaisesRegex(
                        ConfigError,
                        r"deceleration_profile\[1\]\.brake must be a number in \[0, 1\] or vehicle_defined_max_safe",
                    ):
                        load_vehicle_config(path)

    def test_public_vehicle_config_requires_device_identity(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(_public_vehicle_yaml(), encoding="utf-8")

            with self.assertRaisesRegex(
                ConfigError,
                "cloud.device_cert and cloud.device_key are required for non-loopback vehicle cloud",
            ):
                load_vehicle_config(path)

    def test_public_vehicle_config_rejects_non_string_device_identity_paths(self):
        cases = [
            ("device_cert", "cloud.device_cert must be a non-empty string"),
            ("device_key", "cloud.device_key must be a non-empty string"),
        ]
        for invalid_field, expected_error in cases:
            with self.subTest(expected_error=expected_error):
                with tempfile.TemporaryDirectory() as tmp:
                    cert_path = Path(tmp) / "vehicle.crt"
                    key_path = Path(tmp) / "vehicle.key"
                    cert_path.write_text("dev cert", encoding="utf-8")
                    key_path.write_text("dev key", encoding="utf-8")
                    cert_value = "true" if invalid_field == "device_cert" else str(cert_path)
                    key_value = "true" if invalid_field == "device_key" else str(key_path)
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(
                        _public_vehicle_yaml(
                            f"  device_cert: {cert_value}\n"
                            f"  device_key: {key_value}\n",
                        ),
                        encoding="utf-8",
                    )

                    with self.assertRaisesRegex(ConfigError, expected_error):
                        load_vehicle_config(path)

    def test_public_vehicle_config_accepts_existing_device_identity_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            cert_path = Path(tmp) / "vehicle.crt"
            key_path = Path(tmp) / "vehicle.key"
            cert_path.write_text("dev cert", encoding="utf-8")
            key_path.write_text("dev key", encoding="utf-8")
            config_path = Path(tmp) / "vehicle.yaml"
            config_path.write_text(
                _public_vehicle_yaml(
                    f"  device_cert: {cert_path}\n"
                    f"  device_key: {key_path}\n",
                ),
                encoding="utf-8",
            )

            config = load_vehicle_config(config_path)

        self.assertEqual(config.cloud.signaling_url, "wss://teleop.example.com/signaling")
        self.assertEqual(config.cloud.auth_url, "https://teleop.example.com/auth")
        self.assertEqual(config.cloud.device_cert, str(cert_path))
        self.assertEqual(config.cloud.device_key, str(key_path))

    def test_effective_vehicle_config_log_payload_redacts_sensitive_paths_and_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            cert_path = Path(tmp) / "vehicle.crt"
            key_path = Path(tmp) / "vehicle.key"
            cert_path.write_text("dev cert", encoding="utf-8")
            key_path.write_text("dev key", encoding="utf-8")
            config_path = Path(tmp) / "vehicle.yaml"
            config_path.write_text(
                _public_vehicle_yaml(
                    cloud_extra=(
                        f"  device_cert: {cert_path}\n"
                        f"  device_key: {key_path}\n"
                    ),
                    ice_turn_servers="""
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: vehicle-001
      credential: super-secret-turn
""",
                ),
                encoding="utf-8",
            )
            config = load_vehicle_config(config_path)

        payload = effective_vehicle_config_log_payload(config)
        serialized = json.dumps(payload, sort_keys=True)

        self.assertEqual(payload["event"], "effective_vehicle_config")
        self.assertEqual(payload["vehicle_id"], "vehicle-001")
        self.assertEqual(payload["cloud"]["device_cert"], "configured")
        self.assertEqual(payload["cloud"]["device_key"], "configured")
        self.assertEqual(payload["ice"]["turn_servers"][0]["credential"], "configured")
        self.assertEqual(payload["media"]["realtime_profiles"]["realtime_720p"]["bitrate_kbps"], 3000)
        self.assertEqual(payload["cameras"][0]["id"], "front")
        self.assertNotIn(str(cert_path), serialized)
        self.assertNotIn(str(key_path), serialized)
        self.assertNotIn("super-secret-turn", serialized)

    def test_turn_rest_static_auth_secret_is_redacted_and_requires_positive_ttl(self):
        with tempfile.TemporaryDirectory() as tmp:
            cert_path = Path(tmp) / "vehicle.crt"
            key_path = Path(tmp) / "vehicle.key"
            cert_path.write_text("dev cert", encoding="utf-8")
            key_path.write_text("dev key", encoding="utf-8")
            config_path = Path(tmp) / "vehicle.yaml"
            config_path.write_text(
                _public_vehicle_yaml(
                    cloud_extra=(
                        f"  device_cert: {cert_path}\n"
                        f"  device_key: {key_path}\n"
                    ),
                    ice_turn_servers="""
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: mine-teleop
      credential_mode: turn_rest
      static_auth_secret: shared-turn-secret
      credential_ttl_seconds: 600
""",
                ),
                encoding="utf-8",
            )
            config = load_vehicle_config(config_path)
            invalid_path = Path(tmp) / "invalid.yaml"
            invalid_path.write_text(
                _public_vehicle_yaml(
                    cloud_extra=(
                        f"  device_cert: {cert_path}\n"
                        f"  device_key: {key_path}\n"
                    ),
                    ice_turn_servers="""
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: mine-teleop
      credential_mode: turn_rest
      static_auth_secret: shared-turn-secret
      credential_ttl_seconds: 0
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "credential_ttl_seconds must be positive"):
                load_vehicle_config(invalid_path)

        turn = config.ice.turn_servers[0]
        payload = effective_vehicle_config_log_payload(config)
        serialized = json.dumps(payload, sort_keys=True)
        self.assertEqual(turn.credential_mode, "turn_rest")
        self.assertEqual(turn.static_auth_secret, "shared-turn-secret")
        self.assertEqual(turn.credential_ttl_seconds, 600)
        self.assertEqual(payload["ice"]["turn_servers"][0]["static_auth_secret"], "configured")
        self.assertEqual(payload["ice"]["turn_servers"][0]["credential_mode"], "turn_rest")
        self.assertNotIn("shared-turn-secret", serialized)

    def test_turn_credentials_reject_non_string_secret_values(self):
        cases = [
            (
                """
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: vehicle-001
      credential: true
""",
                "ice.turn_servers\\[0\\].credential must be a non-empty string",
            ),
            (
                """
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: vehicle-001
      credential_file: true
""",
                "ice.turn_servers\\[0\\].credential_file must be a non-empty string",
            ),
            (
                """
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: mine-teleop
      credential_mode: turn_rest
      static_auth_secret: true
      credential_ttl_seconds: 600
""",
                "ice.turn_servers\\[0\\].static_auth_secret must be a non-empty string",
            ),
            (
                """
  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: mine-teleop
      credential_mode: turn_rest
      static_auth_secret_file: true
      credential_ttl_seconds: 600
""",
                "ice.turn_servers\\[0\\].static_auth_secret_file must be a non-empty string",
            ),
        ]
        for ice_turn_servers, expected_error in cases:
            with self.subTest(expected_error=expected_error):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(_valid_vehicle_yaml(ice_turn_servers), encoding="utf-8")

                    with self.assertRaisesRegex(ConfigError, expected_error):
                        load_vehicle_config(path)

    def test_config_rejects_turn_server_without_udp_transport(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    """
  turn_servers:
    - url: turn:turn.example.com:3478?transport=tcp
      username: vehicle-001
      credential: secret
"""
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "TURN URL must use udp transport"):
                load_vehicle_config(path)

    def test_config_rejects_missing_estop_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(_valid_vehicle_yaml("  turn_servers: []", include_estop=False), encoding="utf-8")

            with self.assertRaisesRegex(ConfigError, "control.estop must be configured"):
                load_vehicle_config(path)

    def test_config_rejects_missing_time_sync_policy(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(_valid_vehicle_yaml("  turn_servers: []", include_time_sync=False), encoding="utf-8")

            with self.assertRaisesRegex(ConfigError, "control.time_sync.minimum is required"):
                load_vehicle_config(path)

    def test_driver_config_exposes_cloud_ui_and_keyboard_contract(self):
        config = load_driver_config(Path("configs/driver-console.dev.yaml"))

        self.assertEqual(config.driver_id, "driver-console-001")
        self.assertEqual(config.cloud.auth_url, "http://127.0.0.1:8765/auth")
        self.assertEqual(config.cloud.signaling_url, "ws://127.0.0.1:8765/signaling")
        self.assertEqual(config.ui.default_layout, "grid_4")
        self.assertTrue(config.ui.show_debug_overlay)
        self.assertEqual(config.control.rate_hz, 20)
        self.assertEqual(config.control.estop_hold_ms, 500)
        self.assertEqual(config.control.keyboard.throttle, "W")
        self.assertEqual(config.control.keyboard.brake, "S")
        self.assertEqual(config.control.keyboard.estop, "E")

    def test_driver_config_rejects_duplicate_keyboard_bindings(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "driver.yaml"
            path.write_text(
                """
driver:
  id: driver-console-001
cloud:
  auth_url: http://127.0.0.1:8765/auth
  signaling_url: ws://127.0.0.1:8765/signaling
ui:
  default_layout: grid_4
  show_debug_overlay: true
control:
  rate_hz: 20
  estop_hold_ms: 500
  keyboard:
    steering_left: A
    steering_right: D
    throttle: W
    brake: W
    estop: E
""",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "keyboard bindings must be unique"):
                load_driver_config(path)

    def test_driver_config_rejects_string_for_debug_overlay_boolean(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "driver.yaml"
            path.write_text(
                """
driver:
  id: driver-console-001
cloud:
  auth_url: http://127.0.0.1:8765/auth
  signaling_url: ws://127.0.0.1:8765/signaling
ui:
  default_layout: grid_4
  show_debug_overlay: "false"
control:
  rate_hz: 20
  estop_hold_ms: 500
  keyboard:
    steering_left: A
    steering_right: D
    throttle: W
    brake: S
    estop: E
""",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "ui.show_debug_overlay must be a boolean"):
                load_driver_config(path)

    def test_vehicle_config_rejects_non_positive_upload_trigger_segments(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra="  trigger_segments: 0\n",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "upload.trigger_segments must be a positive integer"):
                load_vehicle_config(path)

    def test_vehicle_config_rejects_boolean_for_integer_upload_trigger_segments(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra="  trigger_segments: true\n",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "upload.trigger_segments must be a positive integer"):
                load_vehicle_config(path)

    def test_vehicle_config_rejects_boolean_for_numeric_media_profile_values(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml("  turn_servers: []").replace(
                    "fps: 30",
                    "fps: true",
                    1,
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "realtime_720p.fps must be a positive finite number"):
                load_vehicle_config(path)

    def test_vehicle_config_rejects_string_for_camera_enabled_boolean(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml("  turn_servers: []").replace(
                    "enabled: true",
                    'enabled: "false"',
                    1,
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "cameras\\[0\\].enabled must be a boolean"):
                load_vehicle_config(path)

    def test_vehicle_config_rejects_string_for_destructive_recording_boolean(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml("  turn_servers: []").replace(
                    "recording:\n  retention_target_hours: 8",
                    'recording:\n  retention_target_hours: 8\n  delete_unuploaded_when_below_free_gb: "false"',
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                ConfigError,
                "recording.delete_unuploaded_when_below_free_gb must be a boolean",
            ):
                load_vehicle_config(path)

    def test_vehicle_config_rejects_invalid_recording_capacity_numbers(self):
        cases = [
            (
                'recording:\n  retention_target_hours: "8"',
                "recording.retention_target_hours must be a non-negative finite number",
            ),
            (
                "recording:\n  retention_target_hours: -1",
                "recording.retention_target_hours must be a non-negative finite number",
            ),
            (
                "recording:\n  retention_target_hours: 8\n  min_free_gb: true",
                "recording.min_free_gb must be a non-negative finite number",
            ),
            (
                "recording:\n  retention_target_hours: 8\n  delete_uploaded_when_below_free_gb: -1",
                "recording.delete_uploaded_when_below_free_gb must be a non-negative finite number",
            ),
        ]
        for replacement, expected_error in cases:
            with self.subTest(replacement=replacement):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(
                        _valid_vehicle_yaml("  turn_servers: []").replace(
                            "recording:\n  retention_target_hours: 8\n  upload_lag_policy: alert_and_preserve_uploaded_only",
                            replacement,
                        ),
                        encoding="utf-8",
                    )

                    with self.assertRaisesRegex(ConfigError, expected_error):
                        load_vehicle_config(path)

    def test_vehicle_config_rejects_non_string_recording_paths_and_policies(self):
        cases = [
            (
                "recording:\n  root_dir: true\n  retention_target_hours: 8",
                "recording.root_dir must be a non-empty string",
            ),
            (
                "recording:\n  retention_target_hours: 8\n  upload_lag_policy: true",
                "recording.upload_lag_policy must be a non-empty string",
            ),
        ]
        for replacement, expected_error in cases:
            with self.subTest(replacement=replacement):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(
                        _valid_vehicle_yaml("  turn_servers: []").replace(
                            "recording:\n  retention_target_hours: 8\n  upload_lag_policy: alert_and_preserve_uploaded_only",
                            replacement,
                        ),
                        encoding="utf-8",
                    )

                    with self.assertRaisesRegex(ConfigError, expected_error):
                        load_vehicle_config(path)

    def test_vehicle_config_rejects_string_for_upload_boolean(self):
        cases = [
            ('  direct_file_upload: "false"\n', "upload.direct_file_upload must be a boolean"),
            ('  trigger_network_idle: "true"\n', "upload.trigger_network_idle must be a boolean"),
        ]
        for upload_extra, expected_error in cases:
            with self.subTest(upload_extra=upload_extra):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(
                        _valid_vehicle_yaml(
                            "  turn_servers: []",
                            upload_extra=upload_extra,
                        ),
                        encoding="utf-8",
                    )

                    with self.assertRaisesRegex(ConfigError, expected_error):
                        load_vehicle_config(path)

    def test_vehicle_config_rejects_unsupported_packaged_upload_mode(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra="  direct_file_upload: false\n",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "upload.direct_file_upload=false is not supported"):
                load_vehicle_config(path)

    def test_vehicle_config_rejects_non_positive_upload_bandwidth(self):
        cases = [
            ("max_bandwidth_mbps: 0", "upload.max_bandwidth_mbps must be a positive finite number"),
            ("max_bandwidth_mbps: -1", "upload.max_bandwidth_mbps must be a positive finite number"),
            ("max_bandwidth_mbps: true", "upload.max_bandwidth_mbps must be a positive finite number"),
        ]
        for replacement, expected_error in cases:
            with self.subTest(replacement=replacement):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(
                        _valid_vehicle_yaml("  turn_servers: []").replace(
                            "max_bandwidth_mbps: 5",
                            replacement,
                        ),
                        encoding="utf-8",
                    )

                    with self.assertRaisesRegex(ConfigError, expected_error):
                        load_vehicle_config(path)

    def test_vehicle_config_rejects_non_string_upload_backend(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra="  backend: true\n",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "upload.backend must be a non-empty string"):
                load_vehicle_config(path)

    def test_vehicle_config_rejects_invalid_upload_timing_values(self):
        cases = [
            (
                "  presigned_url_refresh_margin_seconds: 0\n",
                "upload.presigned_url_refresh_margin_seconds must be a positive integer",
            ),
            (
                "  retry_initial_seconds: 0\n",
                "upload.retry_initial_seconds must be a positive integer",
            ),
            (
                "  retry_initial_seconds: 120\n  retry_max_seconds: 60\n",
                "upload.retry_initial_seconds must be less than or equal to upload.retry_max_seconds",
            ),
        ]
        for upload_extra, expected_error in cases:
            with self.subTest(upload_extra=upload_extra):
                with tempfile.TemporaryDirectory() as tmp:
                    path = Path(tmp) / "vehicle.yaml"
                    path.write_text(
                        _valid_vehicle_yaml(
                            "  turn_servers: []",
                            upload_extra=upload_extra,
                        ),
                        encoding="utf-8",
                    )

                    with self.assertRaisesRegex(ConfigError, expected_error):
                        load_vehicle_config(path)

    def test_s3_upload_config_exposes_target_and_redacts_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            secret_path = Path(tmp) / "s3-secret.txt"
            secret_path.write_text("dev-secret-key\n", encoding="utf-8")
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra=f"""
  backend: s3
  s3:
    endpoint_url: https://s3.us-west-2.amazonaws.com
    bucket: mine-teleop-recordings
    region: us-west-2
    access_key_id: AKIDEXAMPLE
    secret_access_key_file: {secret_path}
    session_token: dev-session-token
""",
                ),
                encoding="utf-8",
            )

            config = load_vehicle_config(path)
            payload = effective_vehicle_config_log_payload(config)
            serialized = json.dumps(payload, sort_keys=True)

        self.assertEqual(config.upload.backend, "s3")
        self.assertEqual(config.upload.s3.endpoint_url, "https://s3.us-west-2.amazonaws.com")
        self.assertEqual(config.upload.s3.bucket, "mine-teleop-recordings")
        self.assertEqual(config.upload.s3.region, "us-west-2")
        self.assertEqual(config.upload.s3.access_key_id, "AKIDEXAMPLE")
        self.assertEqual(config.upload.s3.secret_access_key_file, str(secret_path))
        self.assertEqual(payload["upload"]["s3"]["endpoint_url"], "https://s3.us-west-2.amazonaws.com")
        self.assertEqual(payload["upload"]["s3"]["bucket"], "mine-teleop-recordings")
        self.assertEqual(payload["upload"]["s3"]["access_key_id"], "configured")
        self.assertEqual(payload["upload"]["s3"]["secret_access_key_file"], "configured")
        self.assertEqual(payload["upload"]["s3"]["session_token"], "configured")
        self.assertNotIn(str(secret_path), serialized)
        self.assertNotIn("dev-secret-key", serialized)
        self.assertNotIn("dev-session-token", serialized)

    def test_s3_upload_backend_requires_s3_target_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra="  backend: s3\n",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "upload.s3 is required for s3 backend"):
                load_vehicle_config(path)

    def test_upload_credential_service_from_s3_config_uses_configured_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            secret_path = Path(tmp) / "s3-secret.txt"
            secret_path.write_text("dev-secret-key\n", encoding="utf-8")
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra=f"""
  backend: s3
  s3:
    endpoint_url: https://s3.us-west-2.amazonaws.com
    bucket: mine-teleop-recordings
    region: us-west-2
    access_key_id: AKIDEXAMPLE
    secret_access_key_file: {secret_path}
""",
                ),
                encoding="utf-8",
            )

            config = load_vehicle_config(path)
            service = upload_credential_service_from_config(config.upload, ttl_seconds=600)

        credential = service.issue(
            {
                "vehicle_id": "vehicle-001",
                "session_id": "session-001",
                "camera_id": "front",
                "segment_id": "seg-1",
                "kind": "video",
            },
            now_ms=0,
        )
        parsed = urlparse(credential.upload_url)
        query = {key: values[0] for key, values in parse_qs(parsed.query).items()}

        self.assertEqual(parsed.scheme, "https")
        self.assertEqual(parsed.netloc, "s3.us-west-2.amazonaws.com")
        self.assertEqual(
            parsed.path,
            "/mine-teleop-recordings/vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
        )
        self.assertEqual(query["X-Amz-Credential"], "AKIDEXAMPLE/19700101/us-west-2/s3/aws4_request")
        self.assertEqual(query["X-Amz-Expires"], "600")

    def test_upload_presign_report_cli_emits_redacted_s3_acceptance_jsonl(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            secret_path = root / "s3-secret.txt"
            token_path = root / "s3-session-token.txt"
            config_path = root / "vehicle.yaml"
            secret_path.write_text("dev-secret-key\n", encoding="utf-8")
            token_path.write_text("dev-session-token\n", encoding="utf-8")
            config_path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    upload_extra=f"""
  backend: s3
  s3:
    endpoint_url: https://s3.us-west-2.amazonaws.com
    bucket: mine-teleop-recordings
    region: us-west-2
    access_key_id: AKIDEXAMPLE
    secret_access_key_file: {secret_path}
    session_token_file: {token_path}
""",
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/upload_presign_report.py",
                    "--vehicle-config",
                    str(config_path),
                    "--vehicle-id",
                    "vehicle-001",
                    "--session-id",
                    "session-001",
                    "--camera-id",
                    "front",
                    "--segment-id",
                    "seg-1",
                    "--ttl-seconds",
                    "600",
                    "--now-ms",
                    "0",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("dev-secret-key", result.stdout)
        self.assertNotIn("dev-session-token", result.stdout)
        self.assertNotIn("AKIDEXAMPLE", result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual([record["event"] for record in records], [
            "s3_upload_presign_report",
            "s3_upload_presign_credential",
            "s3_upload_presign_credential",
        ])
        self.assertTrue(records[0]["passed"])
        self.assertEqual(records[0]["credential_count"], 2)
        self.assertEqual(records[0]["endpoint_host"], "s3.us-west-2.amazonaws.com")
        self.assertEqual(records[0]["bucket"], "mine-teleop-recordings")
        self.assertEqual(records[0]["region"], "us-west-2")
        self.assertEqual(records[1]["kind"], "video")
        self.assertEqual(
            records[1]["object_path"],
            "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
        )
        self.assertEqual(records[1]["credential_scope"], "configured/19700101/us-west-2/s3/aws4_request")
        self.assertTrue(records[1]["signature_present"])
        self.assertTrue(records[1]["session_token_present"])
        self.assertEqual(records[2]["kind"], "metadata")
        self.assertEqual(
            records[2]["object_path"],
            "vehicles/vehicle-001/sessions/session-001/metadata/seg-1.json",
        )

    def test_runtime_config_update_policy_allows_only_non_dangerous_fields(self):
        policy = RuntimeConfigUpdatePolicy.default()

        allowed = [
            policy.evaluate("logging.level", "debug"),
            policy.evaluate("media.realtime_profiles.realtime_720p.bitrate_kbps", 1800),
            policy.evaluate("upload.max_bandwidth_mbps", 3.5),
            policy.evaluate("upload.paused", True),
        ]
        rejected = [
            policy.evaluate("vehicle.id", "vehicle-002"),
            policy.evaluate("cloud.device_cert", "/etc/mine-teleop/new.crt"),
            policy.evaluate("vehicle_adapter.type", "can"),
            policy.evaluate("cameras[0].device", "/dev/video2"),
            policy.evaluate("control.control_timeout_ms", 1200),
        ]

        self.assertTrue(all(decision.allowed for decision in allowed))
        self.assertEqual(allowed[1].reason, "runtime_update_allowed")
        self.assertFalse(any(decision.allowed for decision in rejected))
        self.assertTrue(all(decision.restart_required for decision in rejected))
        self.assertEqual(rejected[0].reason, "runtime_update_rejected_dangerous_field")
        self.assertEqual(rejected[-1].path, "control.control_timeout_ms")

    def test_runtime_config_update_policy_rejects_invalid_runtime_values(self):
        policy = RuntimeConfigUpdatePolicy.default()

        bitrate = policy.evaluate("media.realtime_profiles.realtime_720p.bitrate_kbps", 0)
        boolean_bitrate = policy.evaluate("media.realtime_profiles.realtime_720p.bitrate_kbps", True)
        upload_limit = policy.evaluate("upload.max_bandwidth_mbps", -1)
        boolean_upload_limit = policy.evaluate("upload.max_bandwidth_mbps", True)
        infinite_upload_limit = policy.evaluate("upload.max_bandwidth_mbps", float("inf"))
        paused = policy.evaluate("upload.paused", "yes")

        self.assertFalse(bitrate.allowed)
        self.assertEqual(bitrate.reason, "runtime_update_rejected_invalid_value")
        self.assertFalse(boolean_bitrate.allowed)
        self.assertEqual(boolean_bitrate.reason, "runtime_update_rejected_invalid_value")
        self.assertFalse(upload_limit.allowed)
        self.assertEqual(upload_limit.reason, "runtime_update_rejected_invalid_value")
        self.assertFalse(boolean_upload_limit.allowed)
        self.assertEqual(boolean_upload_limit.reason, "runtime_update_rejected_invalid_value")
        self.assertFalse(infinite_upload_limit.allowed)
        self.assertEqual(infinite_upload_limit.reason, "runtime_update_rejected_invalid_value")
        self.assertFalse(paused.allowed)
        self.assertEqual(paused.reason, "runtime_update_rejected_invalid_value")

    def test_real_vehicle_adapter_config_requires_interface_contract(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    vehicle_adapter_yaml="""
vehicle_adapter:
  type: can
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "vehicle_adapter.contract is required"):
                load_vehicle_config(path)

    def test_real_vehicle_adapter_config_requires_control_timeout_calibration(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    vehicle_adapter_yaml="""
vehicle_adapter:
  type: can
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: required
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "control.timeout_calibration is required"):
                load_vehicle_config(path)

    def test_can_adapter_requires_chassis_control_integration_before_enablement(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml="""
vehicle_adapter:
  type: can
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: telemetry_feedback
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "vehicle_adapter.integration.chassis_control is required for can"):
                load_vehicle_config(path)

    def test_control_timeout_calibration_caps_real_vehicle_timeout(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 700
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml="""
vehicle_adapter:
  type: can
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: required
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "control.control_timeout_ms exceeds calibrated maximum"):
                load_vehicle_config(path)

    def test_real_vehicle_adapter_contract_exposes_units_heartbeat_safety_and_feedback(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml=f"""
vehicle_adapter:
  type: can
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: required
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
  integration:
    chassis_control:
      source_root: {chassis_root}
      header_path: {chassis_root / "chassis_control.h"}
      can_common_header_path: {chassis_root / "include" / "can" / "can_common.h"}
      cmake_target: chassis_control
      library_output_name: libchassis_control.so
      can_interface: can0
      abi: cplusplus
      requires_cpp_bridge: true
""",
                ),
                encoding="utf-8",
            )

            config = load_vehicle_config(path)

        self.assertEqual(config.vehicle_adapter_type, "can")
        self.assertEqual(config.vehicle_adapter_contract.steering_unit, "normalized")
        self.assertEqual(config.vehicle_adapter_contract.brake_semantics, "normalized_service_brake")
        self.assertEqual(config.vehicle_adapter_contract.gear_values, ["P", "R", "N", "D"])
        self.assertEqual(config.vehicle_adapter_contract.heartbeat_period_ms, 50)
        self.assertTrue(config.vehicle_adapter_contract.safe_stop_supported)
        self.assertTrue(config.vehicle_adapter_contract.estop_supported)
        self.assertEqual(config.vehicle_adapter_contract.command_ack, "required")
        self.assertIn("brake_feedback", config.vehicle_adapter_contract.telemetry_fields)
        self.assertEqual(config.vehicle_adapter_contract.integration.chassis_control.abi, "cplusplus")
        self.assertEqual(config.vehicle_adapter_contract.integration.chassis_control.can_interface, "can0")
        self.assertEqual(config.control.timeout_calibration.max_control_timeout_ms, 900)
        self.assertEqual(config.control.timeout_calibration.evidence, "bench-brake-test-2026-06-24")

    def test_real_vehicle_adapter_contract_requires_reverse_gear_value(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml="""
vehicle_adapter:
  type: can
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: required
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "vehicle_adapter.contract.gear_values must include P, R, N, and D"):
                load_vehicle_config(path)

    def test_real_vehicle_adapter_rejects_mismatched_configured_can_interfaces(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            path = root / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml=f"""
hardware:
  can:
    interface: can9
vehicle_adapter:
  type: can
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: telemetry_feedback
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
  integration:
    chassis_control:
      source_root: {chassis_root}
      header_path: {chassis_root / "chassis_control.h"}
      can_common_header_path: {chassis_root / "include" / "can" / "can_common.h"}
      cmake_target: chassis_control
      library_output_name: libchassis_control.so
      can_interface: can0
      abi: cplusplus
      requires_cpp_bridge: true
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                ConfigError,
                "hardware.can.interface must match vehicle_adapter.integration.chassis_control.can_interface",
            ):
                load_vehicle_config(path)

    def test_dynamic_library_adapter_records_chassis_control_and_minepilot_sources(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            (chassis_root / "include" / "can").mkdir(parents=True)
            minepilot_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            (minepilot_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "include" / "can" / "can_common.h").write_text("struct CanFrame {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_message.h").write_text("struct CanMessage {};\n", encoding="utf-8")
            (minepilot_root / "can_db.h").write_text("#define CAN_DB_ADU_TX_VEH_SPD_FRAME_ID 0x18ff\n", encoding="utf-8")
            (minepilot_root / "can_receiver.h").write_text("struct DecodedCanData {};\n", encoding="utf-8")
            (minepilot_root / "can_sender.h").write_text("class MinePilotCanSender {};\n", encoding="utf-8")
            (minepilot_root / "can_db.cpp").write_text("// generated CAN db source\n", encoding="utf-8")
            (minepilot_root / "can_receiver.cpp").write_text("// CAN receiver source\n", encoding="utf-8")
            (minepilot_root / "can_sender.cpp").write_text("// CAN sender source\n", encoding="utf-8")
            path = root / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml=f"""
vehicle_adapter:
  type: dynamic_library
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: telemetry_feedback
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
  integration:
    chassis_control:
      source_root: {chassis_root}
      header_path: {chassis_root / "chassis_control.h"}
      can_common_header_path: {chassis_root / "include" / "can" / "can_common.h"}
      cmake_target: chassis_control
      library_output_name: libchassis_control.so
      can_interface: can0
      abi: cplusplus
      requires_cpp_bridge: true
    minepilot:
      source_root: {minepilot_root}
      can_common_header_path: {minepilot_root / "include" / "can" / "can_common.h"}
      can_message_header_path: {minepilot_root / "include" / "can" / "can_message.h"}
      can_db_header_path: {minepilot_root / "can_db.h"}
      can_receiver_header_path: {minepilot_root / "can_receiver.h"}
      can_sender_header_path: {minepilot_root / "can_sender.h"}
      can_db_source_path: {minepilot_root / "can_db.cpp"}
      can_receiver_source_path: {minepilot_root / "can_receiver.cpp"}
      can_sender_source_path: {minepilot_root / "can_sender.cpp"}
""",
                ),
                encoding="utf-8",
            )

            config = load_vehicle_config(path)

        integration = config.vehicle_adapter_contract.integration
        self.assertEqual(config.vehicle_adapter_type, "dynamic_library")
        self.assertEqual(integration.chassis_control.cmake_target, "chassis_control")
        self.assertEqual(integration.chassis_control.abi, "cplusplus")
        self.assertTrue(integration.chassis_control.requires_cpp_bridge)
        self.assertEqual(integration.chassis_control.can_interface, "can0")
        self.assertEqual(integration.minepilot.can_common_header_path, str(minepilot_root / "include" / "can" / "can_common.h"))
        self.assertEqual(integration.minepilot.can_message_header_path, str(minepilot_root / "include" / "can" / "can_message.h"))
        self.assertEqual(integration.minepilot.can_db_header_path, str(minepilot_root / "can_db.h"))
        self.assertEqual(integration.minepilot.can_receiver_header_path, str(minepilot_root / "can_receiver.h"))
        self.assertEqual(integration.minepilot.can_sender_header_path, str(minepilot_root / "can_sender.h"))
        self.assertEqual(integration.minepilot.can_db_source_path, str(minepilot_root / "can_db.cpp"))
        self.assertEqual(integration.minepilot.can_receiver_source_path, str(minepilot_root / "can_receiver.cpp"))
        self.assertEqual(integration.minepilot.can_sender_source_path, str(minepilot_root / "can_sender.cpp"))

    def test_chassis_vehicle_config_template_cli_generates_loadable_c_shim_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            bridge_library = root / "libmine_teleop_chassis_bridge.so"
            chassis_library = minepilot_root / "libchassis_control.so"
            (chassis_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "src").mkdir()
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_common.h").write_text("struct CanFrame {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_message.h").write_text("struct CanMessage {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can_db.h").write_text("#define CAN_DB_ADU_TX_VEH_SPD_FRAME_ID 0x18ff\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("struct DecodedCanData {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("class MinePilotCanSender {};\n", encoding="utf-8")
            (minepilot_root / "src" / "can_db.cpp").write_text("// generated CAN db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// CAN receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// CAN sender source\n", encoding="utf-8")
            bridge_library.write_text("fake shared library path for config validation\n", encoding="utf-8")
            chassis_library.write_text("fake chassis shared library path for config validation\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/render_chassis_vehicle_config.py",
                    "--base-config",
                    "configs/vehicle-agent.dev.yaml",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--bridge-library",
                    str(bridge_library),
                    "--chassis-control-library",
                    str(chassis_library),
                    "--can-interface",
                    "can42",
                    "--max-control-timeout-ms",
                    "900",
                    "--calibration-evidence",
                    "bench-brake-test-2026-06-24",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            generated_path = root / "vehicle-agent.chassis.yaml"
            generated_path.write_text(result.stdout, encoding="utf-8")

            self.assertEqual(result.returncode, 0, result.stderr)
            config = load_vehicle_config(generated_path)

        integration = config.vehicle_adapter_contract.integration
        self.assertEqual(config.vehicle_adapter_type, "can")
        self.assertEqual(config.control.timeout_calibration.max_control_timeout_ms, 900)
        self.assertEqual(config.control.timeout_calibration.evidence, "bench-brake-test-2026-06-24")
        self.assertEqual(config.vehicle_adapter_contract.command_ack, "telemetry_feedback")
        self.assertEqual(integration.chassis_control.abi, "c_shim")
        self.assertFalse(integration.chassis_control.requires_cpp_bridge)
        self.assertEqual(integration.chassis_control.bridge_library_path, str(bridge_library))
        self.assertEqual(integration.chassis_control.library_path, str(chassis_library))
        self.assertEqual(integration.chassis_control.can_interface, "can42")
        self.assertEqual(config.hardware.can.interface, "can42")
        self.assertEqual(integration.minepilot.can_db_source_path, str(minepilot_root / "src" / "can_db.cpp"))
        self.assertEqual(integration.minepilot.can_sender_source_path, str(minepilot_root / "src" / "can_sender.cpp"))

    def test_chassis_vehicle_config_template_cli_rejects_missing_bridge_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            missing_bridge = root / "missing" / "libmine_teleop_chassis_bridge.so"
            chassis_library = root / "libchassis_control.so"
            chassis_library.write_text("fake chassis shared library path for config validation\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/render_chassis_vehicle_config.py",
                    "--bridge-library",
                    str(missing_bridge),
                    "--chassis-control-library",
                    str(chassis_library),
                    "--max-control-timeout-ms",
                    "900",
                    "--calibration-evidence",
                    "bench-brake-test",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("bridge library does not exist", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_chassis_vehicle_config_template_cli_rejects_missing_chassis_control_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bridge_library = root / "libmine_teleop_chassis_bridge.so"
            missing_chassis_library = root / "missing" / "libchassis_control.so"
            bridge_library.write_text("fake bridge shared library path for config validation\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/render_chassis_vehicle_config.py",
                    "--bridge-library",
                    str(bridge_library),
                    "--chassis-control-library",
                    str(missing_chassis_library),
                    "--max-control-timeout-ms",
                    "900",
                    "--calibration-evidence",
                    "bench-brake-test",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("chassis control library does not exist", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_chassis_vehicle_config_template_cli_requires_chassis_control_library(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            bridge_library = root / "libmine_teleop_chassis_bridge.so"
            (chassis_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "src").mkdir(parents=True)
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_common.h").write_text("struct CanFrame {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_message.h").write_text("struct CanMessage {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can_db.h").write_text("#define CAN_DB_ADU_TX_VEH_SPD_FRAME_ID 0x18ff\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("struct DecodedCanData {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("class MinePilotCanSender {};\n", encoding="utf-8")
            (minepilot_root / "src" / "can_db.cpp").write_text("// CAN db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// CAN receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// CAN sender source\n", encoding="utf-8")
            bridge_library.write_text("fake bridge shared library path for config validation\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/render_chassis_vehicle_config.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--bridge-library",
                    str(bridge_library),
                    "--max-control-timeout-ms",
                    "900",
                    "--calibration-evidence",
                    "bench-brake-test",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("the following arguments are required: --chassis-control-library", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_chassis_vehicle_config_template_cli_rejects_timeout_calibration_below_control_timeout(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bridge_library = root / "libmine_teleop_chassis_bridge.so"
            chassis_library = root / "libchassis_control.so"
            bridge_library.write_text("fake bridge shared library path for config validation\n", encoding="utf-8")
            chassis_library.write_text("fake chassis shared library path for config validation\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/render_chassis_vehicle_config.py",
                    "--bridge-library",
                    str(bridge_library),
                    "--chassis-control-library",
                    str(chassis_library),
                    "--max-control-timeout-ms",
                    "700",
                    "--calibration-evidence",
                    "bench-brake-test",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("control.control_timeout_ms exceeds calibrated maximum", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_chassis_vehicle_config_template_cli_rejects_missing_minepilot_can_sender_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            bridge_library = root / "libmine_teleop_chassis_bridge.so"
            chassis_library = root / "libchassis_control.so"
            (chassis_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "src").mkdir(parents=True)
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_common.h").write_text("struct CanFrame {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_message.h").write_text("struct CanMessage {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can_db.h").write_text("#define CAN_DB_ADU_TX_VEH_SPD_FRAME_ID 0x18ff\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("struct DecodedCanData {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("class MinePilotCanSender {};\n", encoding="utf-8")
            (minepilot_root / "src" / "can_db.cpp").write_text("// CAN db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// CAN receiver source\n", encoding="utf-8")
            bridge_library.write_text("fake bridge shared library path for config validation\n", encoding="utf-8")
            chassis_library.write_text("fake chassis shared library path for config validation\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/render_chassis_vehicle_config.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--bridge-library",
                    str(bridge_library),
                    "--chassis-control-library",
                    str(chassis_library),
                    "--max-control-timeout-ms",
                    "900",
                    "--calibration-evidence",
                    "bench-brake-test",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2)
        self.assertIn("MinePilot CAN sender source does not exist", result.stderr)
        self.assertEqual(result.stdout, "")

    def test_dynamic_library_adapter_requires_minepilot_can_source_paths(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            (chassis_root / "include" / "can").mkdir(parents=True)
            minepilot_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            (minepilot_root / "include" / "can").mkdir(parents=True)
            (minepilot_root / "include" / "can" / "can_common.h").write_text("struct CanFrame {};\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_message.h").write_text("struct CanMessage {};\n", encoding="utf-8")
            (minepilot_root / "can_db.h").write_text("#define CAN_DB_ADU_TX_VEH_SPD_FRAME_ID 0x18ff\n", encoding="utf-8")
            (minepilot_root / "can_receiver.h").write_text("struct DecodedCanData {};\n", encoding="utf-8")
            (minepilot_root / "can_sender.h").write_text("class MinePilotCanSender {};\n", encoding="utf-8")
            path = root / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml=f"""
vehicle_adapter:
  type: dynamic_library
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: telemetry_feedback
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
  integration:
    chassis_control:
      source_root: {chassis_root}
      header_path: {chassis_root / "chassis_control.h"}
      can_common_header_path: {chassis_root / "include" / "can" / "can_common.h"}
      cmake_target: chassis_control
      library_output_name: libchassis_control.so
      can_interface: can0
      abi: cplusplus
      requires_cpp_bridge: true
    minepilot:
      source_root: {minepilot_root}
      can_common_header_path: {minepilot_root / "include" / "can" / "can_common.h"}
      can_message_header_path: {minepilot_root / "include" / "can" / "can_message.h"}
      can_db_header_path: {minepilot_root / "can_db.h"}
      can_receiver_header_path: {minepilot_root / "can_receiver.h"}
      can_sender_header_path: {minepilot_root / "can_sender.h"}
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(
                ConfigError,
                "vehicle_adapter.integration.minepilot.can_db_source_path is required",
            ):
                load_vehicle_config(path)

    def test_chassis_control_cpp_abi_requires_cpp_bridge_boundary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
            (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
            path = root / "vehicle.yaml"
            path.write_text(
                _valid_vehicle_yaml(
                    "  turn_servers: []",
                    control_extra="""
  timeout_calibration:
    max_control_timeout_ms: 900
    evidence: bench-brake-test-2026-06-24
""",
                    vehicle_adapter_yaml=f"""
vehicle_adapter:
  type: dynamic_library
  contract:
    steering_unit: normalized
    throttle_unit: normalized
    brake_unit: normalized
    brake_semantics: normalized_service_brake
    gear_values: [P, R, N, D]
    heartbeat_period_ms: 50
    safe_stop_supported: true
    estop_supported: true
    command_ack: telemetry_feedback
    telemetry_fields:
      - speed_mps
      - gear
      - steering_feedback
      - throttle_feedback
      - brake_feedback
      - estop
  integration:
    chassis_control:
      source_root: {chassis_root}
      header_path: {chassis_root / "chassis_control.h"}
      can_common_header_path: {chassis_root / "include" / "can" / "can_common.h"}
      cmake_target: chassis_control
      library_output_name: libchassis_control.so
      can_interface: can0
      abi: cplusplus
      requires_cpp_bridge: false
""",
                ),
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "vehicle_adapter.integration.chassis_control.requires_cpp_bridge must be true"):
                load_vehicle_config(path)


def _valid_vehicle_yaml(
    ice_turn_servers,
    include_estop=True,
    include_time_sync=True,
    control_extra="",
    upload_extra="",
    vehicle_adapter_yaml="""
vehicle_adapter:
  type: mock
""",
):
    estop = ""
    if include_estop:
        estop = """
  estop:
    latch: true
    reset_requires_local_confirmation: true
"""
    time_sync = ""
    if include_time_sync:
        time_sync = """
  time_sync:
    minimum: ntp
"""
    return f"""
vehicle:
  id: vehicle-001
cloud:
  signaling_url: ws://127.0.0.1:8765/signaling
  auth_url: http://127.0.0.1:8765/auth
ice:
  stun_servers:
    - stun:127.0.0.1:3478
{ice_turn_servers}
control:
  rate_hz: 20
  max_command_gap_ms: 200
  degraded_timeout_ms: 300
  control_timeout_ms: 800
{control_extra.rstrip()}
{estop}{time_sync}  timeout_action:
    deceleration_profile:
      - {{after_ms: 0, brake: 0.3}}
      - {{after_ms: 500, brake: 0.6}}
media:
  realtime_profiles:
    realtime_720p: {{codec: h264, encoder: vaapi, width: 1280, height: 720, fps: 30, bitrate_kbps: 3000}}
  record_profiles:
    record_source_h264: {{codec: h264, encoder: vaapi, width: source, height: source, fps: source, bitrate_kbps: 8000, segment_seconds: 60}}
cameras:
  - {{id: front, enabled: true, device: testsrc, capture_width: 1920, capture_height: 1080, capture_fps: 30, realtime_profile: realtime_720p, record_profile: record_source_h264}}
recording:
  retention_target_hours: 8
  upload_lag_policy: alert_and_preserve_uploaded_only
upload:
  max_bandwidth_mbps: 5
{upload_extra.rstrip()}
{vehicle_adapter_yaml.strip()}
"""


def _public_vehicle_yaml(cloud_extra: str = "", ice_turn_servers: str = "  turn_servers: []"):
    return _valid_vehicle_yaml(ice_turn_servers).replace(
        """cloud:
  signaling_url: ws://127.0.0.1:8765/signaling
  auth_url: http://127.0.0.1:8765/auth
""",
        f"""cloud:
  signaling_url: wss://teleop.example.com/signaling
  auth_url: https://teleop.example.com/auth
{cloud_extra}""",
    )


if __name__ == "__main__":
    unittest.main()
