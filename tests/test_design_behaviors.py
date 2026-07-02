import json
import base64
import hashlib
import hmac
import inspect
import os
import socket
import subprocess
import sys
import tempfile
import time
import unittest
import urllib.error
import urllib.request
from dataclasses import replace
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import mine_teleop.media as media_module
from mine_teleop.closed_loop import run_mock_closed_loop
from mine_teleop.config import ConfigError, IceConfig, TurnServerConfig, load_vehicle_config
from mine_teleop.control import ControlCommand, ControlReceiver, LatestControlCommandMailbox, ReceiveResult
from mine_teleop.driver_console import (
    ControlCommandGenerator,
    DriverInputMerger,
    DriverConsoleStatusSnapshot,
    DriverToolbarSnapshot,
    DriverVideoDashboard,
    DriverOperationEvent,
    DriverOperationLog,
    EstopInputGuard,
    InputState,
    SoftwareControlState,
)
from mine_teleop.media import (
    ControlDataChannelConfig,
    EncoderChoice,
    EncoderSelector,
    FFmpegVaapiProbePlan,
    GStreamerPluginProbePlan,
    GStreamerPipelineBuilder,
    HardwareEncodingValidationPlan,
    HardwareEncodingValidationReport,
    H264SdpCompatibilityChecker,
    FileReplaySource,
    MediaFaultRecoveryDecision,
    MediaFaultRecoveryExecutor,
    MediaFaultRecoveryPolicy,
    MediaPipelineWatchdog,
    MediaSupervisor,
    RealtimeConnectionRecoveryDecision,
    RealtimeConnectionRecoveryExecutor,
    RealtimeConnectionRecoveryPolicy,
    RealtimeBitrateAdaptationPolicy,
    RealtimeNetworkSample,
    RealtimeProfileAdaptationPolicy,
    RealtimeProfileVariant,
    TestPatternSource,
    V4L2CameraSource,
)
from mine_teleop.netem import TcNetemPlan, WeakNetworkBaseline, WeakNetworkProfile
from mine_teleop.observability import (
    AuditEvent,
    AuditLog,
    ComponentLog,
    ComponentLogEvent,
    ControlAcceptanceMetricsRecorder,
    OperationsMetricsBuilder,
    RecordingAcceptanceMetricsRecorder,
    TelemetryPublisher,
    UploadAcceptanceMetricsRecorder,
    VideoAcceptanceMetricsRecorder,
)
from mine_teleop.preflight import VehiclePreflightChecker
from mine_teleop.recording import SegmentMetadata, SegmentWriter
from mine_teleop.safety import SafetyState, SafetyStateMachine
from mine_teleop.signaling import SessionError, SessionManager
from mine_teleop.signaling_service import (
    CoturnUsageLogParser,
    CoturnUsageSample,
    DeviceCredentialStore,
    DriverCredentialStore,
    DriverPasswordCredential,
    DriverTokenStore,
    SignalingHttpService,
)
from mine_teleop.time_sync import TimeSyncMonitor, TimeSyncStatus
from mine_teleop.upload import (
    HttpPutUploader,
    LocalArchiveUploader,
    NetworkQualitySample,
    S3PresignConfig,
    S3PresignedPutSigner,
    UploadBandwidthLimiter,
    UploadCredentialService,
    UploadNetworkQualityPolicy,
    UploadQueue,
    UploadTriggerPolicy,
)
from mine_teleop.vehicle_control_service import VehicleControlService
from mine_teleop.vehicle_adapter import MockVehicleAdapter, VehicleAdapterError, VehicleAdapterStatus


def _json_get(url):
    with urllib.request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def _json_post(url, payload):
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def _json_post_expect_error(url, payload, expected_status):
    request = urllib.request.Request(
        url,
        data=json.dumps(payload).encode("utf-8"),
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    try:
        urllib.request.urlopen(request, timeout=5)
    except urllib.error.HTTPError as exc:
        if exc.code != expected_status:
            raise
        return json.loads(exc.read().decode("utf-8"))
    raise AssertionError(f"expected HTTP {expected_status}")


def _json_get_expect_error(url, expected_status):
    try:
        urllib.request.urlopen(url, timeout=5)
    except urllib.error.HTTPError as exc:
        if exc.code != expected_status:
            raise
        return json.loads(exc.read().decode("utf-8"))
    raise AssertionError(f"expected HTTP {expected_status}")


def _wait_for_port_file(path, timeout_seconds=5):
    deadline = time.time() + timeout_seconds
    while time.time() < deadline:
        if path.exists():
            return int(path.read_text(encoding="utf-8").strip())
        time.sleep(0.05)
    raise AssertionError(f"timed out waiting for {path}")


class _FakeGStreamerElement:
    def __init__(self):
        self.properties = []

    def set_property(self, name, value):
        self.properties.append((name, value))


class _FakeGStreamerPipeline:
    def __init__(self, elements):
        self.elements = dict(elements)

    def get_by_name(self, name):
        return self.elements.get(name)


class _FakeMediaPipelineController:
    def __init__(self):
        self.calls = []

    def restart_camera_pipeline(self, camera_id):
        self.calls.append(("restart_camera_pipeline", camera_id))

    def switch_camera_encoder(self, camera_id, encoder):
        self.calls.append(("switch_camera_encoder", camera_id, encoder))

    def switch_realtime_profile(self, camera_id, profile_name):
        self.calls.append(("switch_realtime_profile", camera_id, profile_name))


class _FakeRealtimeConnectionController:
    def __init__(self):
        self.calls = []

    def reconnect_signaling(self, retry_delay_ms):
        self.calls.append(("reconnect_signaling", retry_delay_ms))

    def restart_ice(self, camera_id):
        self.calls.append(("restart_ice", camera_id))

    def rebuild_media_session(self, camera_id):
        self.calls.append(("rebuild_media_session", camera_id))


class _WebSocketClient:
    def __init__(self, sock):
        self.sock = sock

    def __enter__(self):
        return self.sock

    def __exit__(self, exc_type, exc, tb):
        self.sock.close()


def _websocket_connect(base_url, path):
    parsed = urlparse(base_url)
    sock = socket.create_connection((parsed.hostname, parsed.port), timeout=5)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {parsed.hostname}:{parsed.port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request.encode("ascii"))
    response = _recv_http_headers(sock)
    if b" 101 " not in response.splitlines()[0]:
        raise AssertionError(response.decode("utf-8", errors="replace"))
    return _WebSocketClient(sock)


def _recv_http_headers(sock):
    raw = b""
    while b"\r\n\r\n" not in raw:
        chunk = sock.recv(4096)
        if not chunk:
            break
        raw += chunk
    return raw


def _raw_http_request(base_url, request):
    parsed = urlparse(base_url)
    sock = socket.create_connection((parsed.hostname, parsed.port), timeout=5)
    try:
        sock.sendall(request.encode("ascii"))
        raw = b""
        while b"\r\n\r\n" not in raw:
            chunk = sock.recv(4096)
            if not chunk:
                break
            raw += chunk
        headers, separator, body = raw.partition(b"\r\n\r\n")
        content_length = 0
        for line in headers.splitlines()[1:]:
            name, _, value = line.partition(b":")
            if name.lower() == b"content-length":
                content_length = int(value.strip())
                break
        while len(body) < content_length:
            body += sock.recv(content_length - len(body))
        return headers + separator + body
    finally:
        sock.close()


def _websocket_send_json(sock, payload):
    body = json.dumps(payload, sort_keys=True).encode("utf-8")
    mask = b"test"
    header = bytearray([0x81])
    if len(body) < 126:
        header.append(0x80 | len(body))
    elif len(body) < 65536:
        header.extend([0x80 | 126, (len(body) >> 8) & 0xFF, len(body) & 0xFF])
    else:
        raise AssertionError("test websocket payload too large")
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(body))
    sock.sendall(bytes(header) + mask + masked)


def _websocket_send_fragmented_json(sock, payload):
    body = json.dumps(payload, sort_keys=True).encode("utf-8")
    mask = b"frag"
    header = bytearray([0x01])
    if len(body) < 126:
        header.append(0x80 | len(body))
    elif len(body) < 65536:
        header.extend([0x80 | 126, (len(body) >> 8) & 0xFF, len(body) & 0xFF])
    else:
        raise AssertionError("test websocket payload too large")
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(body))
    sock.sendall(bytes(header) + mask + masked)


def _websocket_send_unmasked_json(sock, payload):
    body = json.dumps(payload, sort_keys=True).encode("utf-8")
    header = bytearray([0x81])
    if len(body) < 126:
        header.append(len(body))
    elif len(body) < 65536:
        header.extend([126, (len(body) >> 8) & 0xFF, len(body) & 0xFF])
    else:
        raise AssertionError("test websocket payload too large")
    sock.sendall(bytes(header) + body)


def _websocket_send_unmasked_close(sock):
    sock.sendall(b"\x88\x00")


def _websocket_send_oversized_close(sock):
    body = b"x" * 126
    mask = b"clos"
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(body))
    sock.sendall(b"\x88\xfe\x00\x7e" + mask + masked)


def _websocket_recv_json(sock):
    first = sock.recv(2)
    if len(first) != 2:
        raise AssertionError("missing websocket response")
    opcode = first[0] & 0x0F
    length = first[1] & 0x7F
    if length == 126:
        length = int.from_bytes(_recv_exact(sock, 2), "big")
    elif length == 127:
        length = int.from_bytes(_recv_exact(sock, 8), "big")
    if opcode != 1:
        raise AssertionError(f"expected text frame, got opcode {opcode}")
    return json.loads(_recv_exact(sock, length).decode("utf-8"))


def _recv_exact(sock, length):
    data = b""
    while len(data) < length:
        chunk = sock.recv(length - len(data))
        if not chunk:
            raise AssertionError("socket closed")
        data += chunk
    return data


class ConfigValidationTests(unittest.TestCase):
    def test_vehicle_dev_config_loads_and_derives_capacity_plan(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))

        self.assertEqual(config.vehicle_id, "vehicle-001")
        self.assertEqual(config.control.rate_hz, 20)
        self.assertEqual(len(config.enabled_cameras), 1)
        self.assertEqual(config.enabled_cameras[0].camera_id, "front")
        self.assertEqual(config.realtime_profiles["realtime_720p"].keyframe_interval_frames, 30)
        self.assertEqual(config.realtime_profiles["realtime_480p15"].fps, 15)
        self.assertEqual(config.realtime_profiles["realtime_480p15"].height, 480)
        self.assertEqual(config.upload.retry_initial_seconds, 10)
        self.assertEqual(config.upload.retry_max_seconds, 600)
        self.assertTrue(config.capacity.recording_mbps > config.upload.max_bandwidth_mbps)
        self.assertEqual(config.capacity.status, "upload_lag_policy_required_and_configured")

    def test_vehicle_preflight_reports_device_and_permission_readiness(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            camera_device = root / "video0"
            render_device = root / "renderD128"
            missing_device = root / "missing-render"
            recording_root = root / "recordings"
            camera_device.write_text("", encoding="utf-8")
            render_device.write_text("", encoding="utf-8")
            recording_root.mkdir()
            config_path = root / "vehicle.yaml"
            config_path.write_text(
                f"""
vehicle:
  id: vehicle-001
cloud:
  signaling_url: ws://127.0.0.1:8765/signaling
  auth_url: http://127.0.0.1:8765/auth
ice:
  stun_servers:
    - stun:127.0.0.1:3478
  turn_servers: []
control:
  rate_hz: 20
  max_command_gap_ms: 200
  degraded_timeout_ms: 300
  control_timeout_ms: 800
  estop:
    latch: true
    reset_requires_local_confirmation: true
  time_sync:
    minimum: ntp
  timeout_action:
    deceleration_profile:
      - {{after_ms: 0, brake: 0.3}}
      - {{after_ms: 500, brake: 0.6}}
media:
  realtime_profiles:
    realtime_720p: {{codec: h264, encoder: vaapi, width: 1280, height: 720, fps: 30, bitrate_kbps: 3000}}
  record_profiles:
    record_source_h264: {{codec: h264, encoder: vaapi, width: source, height: source, fps: source, bitrate_kbps: 8000, segment_seconds: 60}}
cameras:
  - {{id: front, enabled: true, device: {camera_device}, capture_width: 1920, capture_height: 1080, capture_fps: 30, realtime_profile: realtime_720p, record_profile: record_source_h264}}
recording:
  root_dir: {recording_root}
  retention_target_hours: 8
  upload_lag_policy: alert_and_preserve_uploaded_only
upload:
  max_bandwidth_mbps: 5
vehicle_adapter:
  type: mock
""",
                encoding="utf-8",
            )
            config = load_vehicle_config(config_path)

            report = VehiclePreflightChecker(
                config,
                hardware_devices=[render_device, missing_device],
            ).run()

        checks = {check.name: check for check in report.checks}
        self.assertTrue(report.ready is False)
        self.assertEqual(checks["camera.front.device"].status, "ready")
        self.assertEqual(checks["recording.root_dir"].status, "ready")
        self.assertEqual(checks[f"hardware.{render_device}"].status, "ready")
        self.assertEqual(checks[f"hardware.{missing_device}"].status, "missing")
        self.assertIn(str(missing_device), checks[f"hardware.{missing_device}"].message)

    def test_config_rejects_duplicate_camera_ids(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                """
vehicle:
  id: vehicle-001
cloud:
  signaling_url: ws://127.0.0.1:8765/signaling
  auth_url: http://127.0.0.1:8765/auth
ice:
  stun_servers:
    - stun:127.0.0.1:3478
  turn_servers: []
control:
  rate_hz: 20
  max_command_gap_ms: 200
  degraded_timeout_ms: 300
  control_timeout_ms: 800
  estop:
    latch: true
    reset_requires_local_confirmation: true
  time_sync:
    minimum: ntp
  timeout_action:
    deceleration_profile:
      - after_ms: 0
        brake: 0.3
      - after_ms: 500
        brake: 0.6
media:
  realtime_profiles:
    realtime_720p: {codec: h264, encoder: vaapi, width: 1280, height: 720, fps: 30, bitrate_kbps: 3000}
  record_profiles:
    record_source_h264: {codec: h264, encoder: vaapi, width: source, height: source, fps: source, bitrate_kbps: 8000, segment_seconds: 60}
cameras:
  - {id: front, enabled: true, device: testsrc, capture_width: 1920, capture_height: 1080, capture_fps: 30, realtime_profile: realtime_720p, record_profile: record_source_h264}
  - {id: front, enabled: true, device: testsrc2, capture_width: 1920, capture_height: 1080, capture_fps: 30, realtime_profile: realtime_720p, record_profile: record_source_h264}
recording:
  retention_target_hours: 8
  upload_lag_policy: alert_and_preserve_uploaded_only
upload:
  max_bandwidth_mbps: 5
vehicle_adapter:
  type: mock
""",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "duplicate camera id"):
                load_vehicle_config(path)

    def test_config_rejects_control_thresholds_that_do_not_increase(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "vehicle.yaml"
            path.write_text(
                """
vehicle: {id: vehicle-001}
cloud:
  signaling_url: ws://127.0.0.1:8765/signaling
  auth_url: http://127.0.0.1:8765/auth
ice:
  stun_servers:
    - stun:127.0.0.1:3478
  turn_servers: []
control:
  rate_hz: 20
  max_command_gap_ms: 400
  degraded_timeout_ms: 300
  control_timeout_ms: 800
  estop:
    latch: true
    reset_requires_local_confirmation: true
  time_sync:
    minimum: ntp
  timeout_action:
    deceleration_profile:
      - {after_ms: 0, brake: 0.3}
      - {after_ms: 500, brake: 0.6}
media:
  realtime_profiles:
    realtime_720p: {codec: h264, encoder: vaapi, width: 1280, height: 720, fps: 30, bitrate_kbps: 3000}
  record_profiles:
    record_source_h264: {codec: h264, encoder: vaapi, width: source, height: source, fps: source, bitrate_kbps: 8000, segment_seconds: 60}
cameras:
  - {id: front, enabled: true, device: testsrc, capture_width: 1920, capture_height: 1080, capture_fps: 30, realtime_profile: realtime_720p, record_profile: record_source_h264}
recording: {retention_target_hours: 8, upload_lag_policy: alert_and_preserve_uploaded_only}
upload: {max_bandwidth_mbps: 5}
vehicle_adapter: {type: mock}
""",
                encoding="utf-8",
            )

            with self.assertRaisesRegex(ConfigError, "max_command_gap_ms < degraded_timeout_ms < control_timeout_ms"):
                load_vehicle_config(path)


class ControlAndSafetyTests(unittest.TestCase):
    def test_control_command_validates_ranges_and_required_identity(self):
        command = ControlCommand.from_dict(
            {
                "type": "control_command",
                "protocol_version": 1,
                "vehicle_id": "vehicle-001",
                "session_id": "session-001",
                "seq": 1,
                "ts_ms": 1000,
                "gear": "D",
                "steering": 0.25,
                "throttle": 0.5,
                "brake": 0.0,
                "estop": False,
            }
        )

        self.assertEqual(command.seq, 1)
        self.assertEqual(command.gear, "D")

        bad = dict(command.to_dict())
        bad["throttle"] = 1.2
        with self.assertRaisesRegex(ValueError, "throttle"):
            ControlCommand.from_dict(bad)

    def test_control_command_rejects_non_boolean_estop(self):
        payload = {
            "type": "control_command",
            "protocol_version": 1,
            "vehicle_id": "vehicle-001",
            "session_id": "session-001",
            "seq": 1,
            "ts_ms": 1000,
            "gear": "D",
            "steering": 0.0,
            "throttle": 0.0,
            "brake": 0.0,
            "estop": "false",
        }

        with self.assertRaisesRegex(ValueError, "estop must be a boolean"):
            ControlCommand.from_dict(payload)

    def test_control_command_rejects_non_json_numeric_fields(self):
        payload = {
            "type": "control_command",
            "protocol_version": 1,
            "vehicle_id": "vehicle-001",
            "session_id": "session-001",
            "seq": 1,
            "ts_ms": 1000,
            "gear": "D",
            "steering": 0.0,
            "throttle": 0.0,
            "brake": 0.0,
            "estop": False,
        }
        cases = [
            ("protocol_version", True, "protocol_version must be an integer"),
            ("seq", "1", "seq must be a non-negative integer"),
            ("ts_ms", True, "ts_ms must be an integer"),
            ("steering", "0.0", "steering must be a number"),
            ("throttle", True, "throttle must be a number"),
            ("brake", "0.0", "brake must be a number"),
        ]
        for field, value, message in cases:
            with self.subTest(field=field):
                bad = dict(payload)
                bad[field] = value
                with self.assertRaisesRegex(ValueError, message):
                    ControlCommand.from_dict(bad)

    def test_control_command_rejects_missing_required_json_fields(self):
        payload = {
            "type": "control_command",
            "protocol_version": 1,
            "vehicle_id": "vehicle-001",
            "session_id": "session-001",
            "seq": 1,
            "ts_ms": 1000,
            "gear": "D",
            "steering": 0.0,
            "throttle": 0.0,
            "brake": 0.0,
            "estop": False,
        }
        for field in ("protocol_version", "vehicle_id", "session_id", "seq", "ts_ms", "gear", "steering", "throttle", "brake"):
            with self.subTest(field=field):
                bad = dict(payload)
                del bad[field]
                with self.assertRaisesRegex(ValueError, f"{field} is required"):
                    ControlCommand.from_dict(bad)

    def test_control_command_validate_rejects_directly_constructed_wrong_scalar_types(self):
        base = ControlCommand(
            vehicle_id="vehicle-001",
            session_id="session-001",
            seq=1,
            ts_ms=1000,
            gear="D",
            steering=0.0,
            throttle=0.0,
            brake=0.0,
            estop=False,
        )
        cases = [
            ("protocol_version", True, "protocol_version must be an integer"),
            ("vehicle_id", True, "vehicle_id must be a string"),
            ("session_id", 123, "session_id must be a string"),
            ("seq", True, "seq must be a non-negative integer"),
            ("ts_ms", True, "ts_ms must be an integer"),
            ("steering", True, "steering must be a number"),
            ("throttle", True, "throttle must be a number"),
            ("brake", True, "brake must be a number"),
            ("estop", "false", "estop must be a boolean"),
            ("authority_token", 123, "authority_token must be a string"),
        ]
        for field, value, message in cases:
            with self.subTest(field=field):
                with self.assertRaisesRegex(ValueError, message):
                    base.replace(**{field: value})

    def test_control_command_rejects_non_string_identity_fields(self):
        payload = {
            "type": "control_command",
            "protocol_version": 1,
            "vehicle_id": "vehicle-001",
            "session_id": "session-001",
            "seq": 1,
            "ts_ms": 1000,
            "gear": "D",
            "steering": 0.0,
            "throttle": 0.0,
            "brake": 0.0,
            "estop": False,
            "authority_token": "session-token",
        }
        cases = [
            ("vehicle_id", True, "vehicle_id must be a string"),
            ("session_id", 123, "session_id must be a string"),
            ("gear", True, "gear must be a string"),
            ("authority_token", 123, "authority_token must be a string"),
        ]
        for field, value, message in cases:
            with self.subTest(field=field):
                bad = dict(payload)
                bad[field] = value
                with self.assertRaisesRegex(ValueError, message):
                    ControlCommand.from_dict(bad)

    def test_receiver_rejects_old_seq_wrong_session_and_large_local_gap(self):
        receiver = ControlReceiver(
            vehicle_id="vehicle-001",
            session_id="session-001",
            max_command_gap_ms=200,
        )
        first = ControlCommand(
            vehicle_id="vehicle-001",
            session_id="session-001",
            seq=10,
            ts_ms=1000,
            gear="D",
            steering=0.0,
            throttle=0.1,
            brake=0.0,
        )
        self.assertTrue(receiver.accept(first, receive_time_ms=1000).accepted)

        old = first.replace(seq=9, ts_ms=1050)
        self.assertEqual(receiver.accept(old, receive_time_ms=1050).reason, "old_seq")

        wrong_session = first.replace(seq=11, session_id="other", ts_ms=1100)
        self.assertEqual(receiver.accept(wrong_session, receive_time_ms=1100).reason, "wrong_session")

        late = first.replace(seq=12, ts_ms=1500)
        self.assertEqual(receiver.accept(late, receive_time_ms=1251).reason, "command_gap_exceeded")

        late_estop = first.replace(seq=12, ts_ms=1500, estop=True)
        accepted_estop = receiver.accept(late_estop, receive_time_ms=1251)
        self.assertTrue(accepted_estop.accepted)
        self.assertEqual(accepted_estop.reason, "accepted")

    def test_receiver_rejects_invalid_timing_config_and_reversed_receive_time(self):
        with self.assertRaisesRegex(ValueError, "max_command_gap_ms"):
            ControlReceiver(
                vehicle_id="vehicle-001",
                session_id="session-001",
                max_command_gap_ms=True,
            )
        with self.assertRaisesRegex(ValueError, "timestamp_warning_skew_ms"):
            ControlReceiver(
                vehicle_id="vehicle-001",
                session_id="session-001",
                max_command_gap_ms=200,
                timestamp_warning_skew_ms=True,
            )

        receiver = ControlReceiver(
            vehicle_id="vehicle-001",
            session_id="session-001",
            max_command_gap_ms=200,
        )
        command = ControlCommand(
            vehicle_id="vehicle-001",
            session_id="session-001",
            seq=1,
            ts_ms=1000,
            gear="D",
            steering=0.0,
            throttle=0.1,
            brake=0.0,
        )

        with self.assertRaisesRegex(ValueError, "receive_time_ms"):
            receiver.accept(command, receive_time_ms=True)
        self.assertTrue(receiver.accept(command, receive_time_ms=1000).accepted)
        reversed_time = receiver.accept(command.replace(seq=2, ts_ms=1100), receive_time_ms=900)
        self.assertFalse(reversed_time.accepted)
        self.assertEqual(reversed_time.reason, "receive_time_reversed")

    def test_receiver_accepts_clock_skewed_command_but_flags_timestamp_warning(self):
        receiver = ControlReceiver(
            vehicle_id="vehicle-001",
            session_id="session-001",
            max_command_gap_ms=200,
            timestamp_warning_skew_ms=5_000,
        )
        command = ControlCommand(
            vehicle_id="vehicle-001",
            session_id="session-001",
            seq=1,
            ts_ms=-60_000,
            gear="D",
            steering=0.0,
            throttle=0.1,
            brake=0.0,
        )

        result = receiver.accept(command, receive_time_ms=1_000)

        self.assertTrue(result.accepted)
        self.assertEqual(result.reason, "accepted")
        self.assertIn("driver_timestamp_skew", result.warnings)

    def test_safety_state_machine_degrades_times_out_and_latches_estop(self):
        machine = SafetyStateMachine(
            degraded_timeout_ms=300,
            control_timeout_ms=800,
            deceleration_profile=[(0, 0.3), (500, 0.6), (1500, "vehicle_defined_max_safe")],
        )
        machine.mark_ready(now_ms=0)
        command = ControlCommand(
            vehicle_id="vehicle-001",
            session_id="session-001",
            seq=1,
            ts_ms=0,
            gear="D",
            steering=0.0,
            throttle=0.2,
            brake=0.0,
        )
        machine.on_valid_command(command, now_ms=0)
        self.assertEqual(machine.state, SafetyState.CONTROL_ACTIVE)

        machine.tick(now_ms=300)
        self.assertEqual(machine.state, SafetyState.DEGRADED)
        self.assertEqual(machine.current_output(now_ms=300).throttle, 0.0)

        machine.tick(now_ms=800)
        self.assertEqual(machine.state, SafetyState.TIMEOUT_BRAKE)
        self.assertEqual(machine.current_output(now_ms=800).brake, 0.3)

        estop = command.replace(seq=2, estop=True)
        machine.on_valid_command(estop, now_ms=900)
        self.assertEqual(machine.state, SafetyState.ESTOP)
        self.assertFalse(machine.reset_estop(local_confirmed=False, authorized_by="operator", now_ms=1000))
        self.assertEqual(machine.state, SafetyState.ESTOP)
        self.assertTrue(machine.reset_estop(local_confirmed=True, authorized_by="safety-officer", now_ms=1100))
        self.assertEqual(machine.state, SafetyState.STANDBY)


class MediaRecordingUploadTests(unittest.TestCase):
    def test_encoder_selector_prefers_vaapi_then_falls_back_to_x264(self):
        selector = EncoderSelector(available_backends={"x264"})

        choice = selector.select("vaapi")

        self.assertEqual(choice.backend, "x264")
        self.assertEqual(choice.reason, "requested_unavailable_fallback")

    def test_media_supervisor_keeps_other_cameras_running_when_one_source_fails(self):
        front = TestPatternSource(camera_id="front", width=1280, height=720, fps=30)
        rear = TestPatternSource(camera_id="rear", width=1280, height=720, fps=30, fail_after_frames=1)
        supervisor = MediaSupervisor({"front": front, "rear": rear})

        first = supervisor.poll_once(now_ms=0)
        second = supervisor.poll_once(now_ms=33)

        self.assertEqual(first["front"].status, "ok")
        self.assertEqual(first["rear"].status, "ok")
        self.assertEqual(second["front"].status, "ok")
        self.assertEqual(second["rear"].status, "error")
        self.assertIn("simulated camera failure", second["rear"].message)

    def test_file_replay_source_reads_recorded_frames_in_order(self):
        with tempfile.TemporaryDirectory() as tmp:
            replay_path = Path(tmp) / "front.jsonl"
            replay_path.write_text(
                "\n".join(
                    [
                        json.dumps({"seq": 10, "width": 640, "height": 480, "timestamp_ms": 1000, "pattern": "dusty-front"}),
                        json.dumps({"seq": 11, "width": 640, "height": 480, "timestamp_ms": 1033, "pattern": "shadow-front"}),
                    ]
                ),
                encoding="utf-8",
            )
            source = FileReplaySource(camera_id="front", replay_path=replay_path)

            first = source.read_frame(now_ms=5_000)
            second = source.read_frame(now_ms=5_033)

        self.assertEqual(first.camera_id, "front")
        self.assertEqual(first.seq, 10)
        self.assertEqual(first.width, 640)
        self.assertEqual(first.height, 480)
        self.assertEqual(first.timestamp_ms, 1000)
        self.assertEqual(first.pattern, "dusty-front")
        self.assertEqual(second.seq, 11)
        self.assertEqual(second.pattern, "shadow-front")

    def test_file_replay_source_reports_exhausted_replay(self):
        with tempfile.TemporaryDirectory() as tmp:
            replay_path = Path(tmp) / "front.jsonl"
            replay_path.write_text(
                json.dumps({"seq": 1, "width": 320, "height": 240, "timestamp_ms": 0, "pattern": "single"}),
                encoding="utf-8",
            )
            source = FileReplaySource(camera_id="front", replay_path=replay_path)

            source.read_frame(now_ms=0)
            with self.assertRaisesRegex(RuntimeError, "file replay exhausted"):
                source.read_frame(now_ms=33)

    def test_file_replay_source_rejects_invalid_frame_metadata(self):
        invalid_records = [
            (
                {"seq": True, "width": 320, "height": 240, "timestamp_ms": 0, "pattern": "single"},
                "seq must be a non-negative integer",
            ),
            (
                {"seq": 1, "width": 0, "height": 240, "timestamp_ms": 0, "pattern": "single"},
                "width must be a positive integer",
            ),
            (
                {"seq": 1, "width": 320, "height": 240, "timestamp_ms": -1, "pattern": "single"},
                "timestamp_ms must be a non-negative integer",
            ),
            (
                {"seq": 1, "width": 320, "height": 240, "timestamp_ms": 0, "pattern": True},
                "pattern must be a non-empty string",
            ),
        ]

        for record, expected_error in invalid_records:
            with self.subTest(record=record), tempfile.TemporaryDirectory() as tmp:
                replay_path = Path(tmp) / "front.jsonl"
                replay_path.write_text(json.dumps(record), encoding="utf-8")

                with self.assertRaisesRegex(ValueError, expected_error):
                    FileReplaySource(camera_id="front", replay_path=replay_path)

    def test_v4l2_camera_source_builds_gstreamer_fragment_for_existing_device_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            device_path = Path(tmp) / "video0"
            device_path.write_text("", encoding="utf-8")
            source = V4L2CameraSource(
                camera_id="front",
                device_path=device_path,
                width=1280,
                height=720,
                fps=30,
            )

        self.assertEqual(source.camera_id, "front")
        self.assertEqual(source.device_path, str(device_path))
        self.assertEqual(
            source.gst_source_fragment(),
            f"v4l2src device={device_path} ! video/x-raw,width=1280,height=720,framerate=30/1",
        )

    def test_v4l2_camera_source_rejects_missing_device_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            missing_path = Path(tmp) / "video99"

            with self.assertRaisesRegex(FileNotFoundError, "V4L2 device not found"):
                V4L2CameraSource(
                    camera_id="front",
                    device_path=missing_path,
                    width=1280,
                    height=720,
                    fps=30,
                )

    def test_media_pipeline_watchdog_reports_stalled_camera_once(self):
        watchdog = MediaPipelineWatchdog(component="vehicle-media-agent", timeout_ms=200)

        watchdog.heartbeat("front", now_ms=1000)
        ok = watchdog.assess(
            "front",
            now_ms=1150,
            vehicle_id="vehicle-001",
            session_id="session-001",
        )
        stalled = watchdog.assess(
            "front",
            now_ms=1251,
            vehicle_id="vehicle-001",
            session_id="session-001",
        )
        repeated = watchdog.assess(
            "front",
            now_ms=1300,
            vehicle_id="vehicle-001",
            session_id="session-001",
        )

        self.assertEqual(ok.status, "ok")
        self.assertIsNone(ok.log_event)
        self.assertEqual(stalled.status, "stalled")
        self.assertEqual(stalled.age_ms, 251)
        self.assertEqual(stalled.log_event.event, "media_pipeline_stalled")
        self.assertEqual(stalled.log_event.camera_id, "front")
        self.assertEqual(stalled.log_event.error_code, "media_watchdog_timeout")
        self.assertIsNone(repeated.log_event)

    def test_media_pipeline_watchdog_rejects_invalid_timing_inputs(self):
        with self.assertRaisesRegex(ValueError, "timeout_ms"):
            MediaPipelineWatchdog(component="vehicle-media-agent", timeout_ms=True)

        watchdog = MediaPipelineWatchdog(component="vehicle-media-agent", timeout_ms=200)
        with self.assertRaisesRegex(ValueError, "heartbeat now_ms"):
            watchdog.heartbeat("front", now_ms=-1)
        with self.assertRaisesRegex(ValueError, "assess now_ms"):
            watchdog.assess(
                "front",
                now_ms=True,
                vehicle_id="vehicle-001",
                session_id="session-001",
            )

        watchdog.heartbeat("front", now_ms=1000)
        with self.assertRaisesRegex(ValueError, "must not be earlier than last heartbeat"):
            watchdog.assess(
                "front",
                now_ms=900,
                vehicle_id="vehicle-001",
                session_id="session-001",
            )

    def test_media_fault_recovery_restarts_only_stalled_camera_pipeline(self):
        policy = MediaFaultRecoveryPolicy(component="vehicle-media-agent")

        decision = policy.camera_pipeline_stalled(
            camera_id="front",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1300,
            reason="media_watchdog_timeout",
        )

        self.assertEqual(decision.action, "restart_camera_pipeline")
        self.assertEqual(decision.affected_camera_id, "front")
        self.assertFalse(decision.stop_vehicle_control)
        self.assertEqual(decision.status_update["state"], "reconnecting")
        self.assertTrue(decision.status_update["reconnecting"])
        self.assertEqual(decision.status_update["fault"], "media_watchdog_timeout")
        self.assertEqual(decision.log_event.event, "media_pipeline_restart_requested")
        self.assertEqual(decision.log_event.camera_id, "front")
        self.assertEqual(decision.log_event.error_code, "media_watchdog_timeout")

    def test_media_fault_recovery_executor_restarts_stalled_camera_pipeline(self):
        policy = MediaFaultRecoveryPolicy(component="vehicle-media-agent")
        decision = policy.camera_pipeline_stalled(
            camera_id="front",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1300,
            reason="media_watchdog_timeout",
        )
        controller = _FakeMediaPipelineController()
        executor = MediaFaultRecoveryExecutor(controller)

        execution = executor.execute(decision)

        self.assertEqual(controller.calls, [("restart_camera_pipeline", "front")])
        self.assertEqual(execution.action, "restart_camera_pipeline")
        self.assertEqual(execution.status_update["state"], "reconnecting")
        self.assertFalse(execution.stop_vehicle_control)

    def test_media_fault_recovery_falls_back_to_cpu_encoder_and_reports_degraded_status(self):
        policy = MediaFaultRecoveryPolicy(component="vehicle-media-agent")

        decision = policy.encoder_failed(
            requested_encoder="vaapi",
            fallback_encoder="x264",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1400,
            camera_id="front",
            reason="hardware_encoder_unavailable",
        )

        self.assertEqual(decision.action, "fallback_encoder")
        self.assertEqual(decision.encoder.backend, "x264")
        self.assertEqual(decision.encoder.reason, "hardware_encoder_unavailable_fallback")
        self.assertEqual(decision.status_update["state"], "degraded")
        self.assertEqual(decision.status_update["encoder"], "x264")
        self.assertTrue(decision.status_update["low_bitrate"])
        self.assertEqual(decision.log_event.event, "media_encoder_fallback")
        self.assertEqual(decision.log_event.message, "vaapi encoder failed; falling back to x264")
        self.assertEqual(decision.log_event.error_code, "hardware_encoder_unavailable")

    def test_media_fault_recovery_executor_switches_to_fallback_encoder(self):
        policy = MediaFaultRecoveryPolicy(component="vehicle-media-agent")
        decision = policy.encoder_failed(
            requested_encoder="vaapi",
            fallback_encoder="x264",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1400,
            camera_id="front",
            reason="hardware_encoder_unavailable",
        )
        controller = _FakeMediaPipelineController()
        executor = MediaFaultRecoveryExecutor(controller)

        execution = executor.execute(decision)

        self.assertEqual(controller.calls, [("switch_camera_encoder", "front", "x264")])
        self.assertEqual(execution.action, "fallback_encoder")
        self.assertEqual(execution.status_update["encoder"], "x264")

    def test_media_fault_recovery_executor_rejects_unknown_action(self):
        controller = _FakeMediaPipelineController()
        executor = MediaFaultRecoveryExecutor(controller)
        decision = MediaFaultRecoveryDecision(
            action="unknown",
            affected_camera_id="front",
            status_update={},
            log_event=ComponentLogEvent(
                ts_ms=1,
                level="warning",
                component="vehicle-media-agent",
                vehicle_id="vehicle-001",
                session_id="session-001",
                camera_id="front",
                event="unknown",
                message="unknown",
            ),
        )

        with self.assertRaisesRegex(RuntimeError, "unknown media fault recovery action"):
            executor.execute(decision)

        self.assertEqual(controller.calls, [])

    def test_realtime_connection_recovery_reconnects_signaling_without_stopping_control(self):
        policy = RealtimeConnectionRecoveryPolicy(component="vehicle-media-agent", signaling_backoff_ms=750)

        decision = policy.signaling_disconnected(
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1500,
            reason="websocket_closed",
        )

        self.assertEqual(decision.action, "reconnect_signaling")
        self.assertEqual(decision.retry_delay_ms, 750)
        self.assertFalse(decision.stop_vehicle_control)
        self.assertEqual(decision.status_update["signaling"], "reconnecting")
        self.assertEqual(decision.status_update["fault"], "websocket_closed")
        self.assertEqual(decision.log_event.event, "signaling_reconnect_requested")
        self.assertEqual(decision.log_event.error_code, "websocket_closed")

    def test_realtime_connection_recovery_executor_reconnects_signaling(self):
        policy = RealtimeConnectionRecoveryPolicy(component="vehicle-media-agent", signaling_backoff_ms=750)
        decision = policy.signaling_disconnected(
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1500,
            reason="websocket_closed",
        )
        controller = _FakeRealtimeConnectionController()
        executor = RealtimeConnectionRecoveryExecutor(controller)

        execution = executor.execute(decision)

        self.assertEqual(controller.calls, [("reconnect_signaling", 750)])
        self.assertEqual(execution.action, "reconnect_signaling")
        self.assertEqual(execution.status_update["signaling"], "reconnecting")
        self.assertFalse(execution.stop_vehicle_control)

    def test_realtime_connection_recovery_uses_ice_restart_before_rebuilding_session(self):
        policy = RealtimeConnectionRecoveryPolicy(component="vehicle-media-agent", max_ice_restart_attempts=2)

        restart = policy.media_disconnected(
            camera_id="front",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1600,
            reason="ice_disconnected",
            ice_restart_attempts=1,
        )
        rebuild = policy.media_disconnected(
            camera_id="front",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1700,
            reason="ice_failed",
            ice_restart_attempts=2,
        )

        self.assertEqual(restart.action, "ice_restart")
        self.assertEqual(restart.status_update["media"], "ice_restarting")
        self.assertEqual(restart.log_event.event, "media_ice_restart_requested")
        self.assertEqual(rebuild.action, "rebuild_session")
        self.assertEqual(rebuild.status_update["media"], "rebuilding_session")
        self.assertEqual(rebuild.log_event.event, "media_session_rebuild_requested")

    def test_realtime_connection_recovery_rejects_invalid_retry_settings_and_attempts(self):
        invalid_settings = [
            ({"signaling_backoff_ms": True}, "signaling_backoff_ms"),
            ({"max_ice_restart_attempts": True}, "max_ice_restart_attempts"),
        ]
        for kwargs, message in invalid_settings:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ValueError, message):
                    RealtimeConnectionRecoveryPolicy(component="vehicle-media-agent", **kwargs)

        policy = RealtimeConnectionRecoveryPolicy(component="vehicle-media-agent", max_ice_restart_attempts=2)
        invalid_attempts = [True, -1]
        for attempts in invalid_attempts:
            with self.subTest(attempts=attempts):
                with self.assertRaisesRegex(ValueError, "ice_restart_attempts"):
                    policy.media_disconnected(
                        camera_id="front",
                        vehicle_id="vehicle-001",
                        session_id="session-001",
                        now_ms=1600,
                        reason="ice_disconnected",
                        ice_restart_attempts=attempts,
                    )

    def test_realtime_connection_recovery_executor_restarts_ice_before_rebuild(self):
        policy = RealtimeConnectionRecoveryPolicy(component="vehicle-media-agent", max_ice_restart_attempts=2)
        decision = policy.media_disconnected(
            camera_id="front",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1600,
            reason="ice_disconnected",
            ice_restart_attempts=1,
        )
        controller = _FakeRealtimeConnectionController()
        executor = RealtimeConnectionRecoveryExecutor(controller)

        execution = executor.execute(decision)

        self.assertEqual(controller.calls, [("restart_ice", "front")])
        self.assertEqual(execution.action, "ice_restart")
        self.assertEqual(execution.status_update["media"], "ice_restarting")

    def test_realtime_connection_recovery_executor_rebuilds_media_session_after_ice_limit(self):
        policy = RealtimeConnectionRecoveryPolicy(component="vehicle-media-agent", max_ice_restart_attempts=2)
        decision = policy.media_disconnected(
            camera_id="front",
            vehicle_id="vehicle-001",
            session_id="session-001",
            now_ms=1700,
            reason="ice_failed",
            ice_restart_attempts=2,
        )
        controller = _FakeRealtimeConnectionController()
        executor = RealtimeConnectionRecoveryExecutor(controller)

        execution = executor.execute(decision)

        self.assertEqual(controller.calls, [("rebuild_media_session", "front")])
        self.assertEqual(execution.action, "rebuild_session")
        self.assertEqual(execution.status_update["media"], "rebuilding_session")

    def test_realtime_connection_recovery_executor_rejects_unknown_action(self):
        controller = _FakeRealtimeConnectionController()
        executor = RealtimeConnectionRecoveryExecutor(controller)
        decision = RealtimeConnectionRecoveryDecision(
            action="unknown",
            status_update={},
            log_event=ComponentLogEvent(
                ts_ms=1,
                level="warning",
                component="vehicle-media-agent",
                vehicle_id="vehicle-001",
                session_id="session-001",
                event="unknown",
                message="unknown",
            ),
        )

        with self.assertRaisesRegex(RuntimeError, "unknown realtime connection recovery action"):
            executor.execute(decision)

        self.assertEqual(controller.calls, [])

    def test_gstreamer_pipeline_builder_uses_low_latency_vaapi_and_x264_fallback(self):
        builder = GStreamerPipelineBuilder()
        self.assertIn("encoder_name", inspect.signature(builder.realtime_h264_pipeline).parameters)
        vaapi = builder.realtime_h264_pipeline(
            source_device="/dev/video0",
            width=1280,
            height=720,
            fps=30,
            bitrate_kbps=3000,
            keyframe_interval_frames=45,
            encoder_name="front_realtime_encoder",
            encoder=EncoderChoice("vaapi", "requested_available"),
        )
        x264 = builder.realtime_h264_pipeline(
            source_device="testsrc",
            width=1280,
            height=720,
            fps=30,
            bitrate_kbps=2500,
            keyframe_interval_frames=45,
            encoder=EncoderChoice("x264", "requested_unavailable_fallback"),
        )

        self.assertIn("v4l2src device=/dev/video0", vaapi)
        self.assertIn("queue max-size-buffers=2 leaky=downstream", vaapi)
        self.assertIn("video/x-raw,width=1280,height=720,framerate=30/1", vaapi)
        self.assertIn("vaapih264enc", vaapi)
        self.assertIn("name=front_realtime_encoder", vaapi)
        self.assertIn("rate-control=cbr", vaapi)
        self.assertIn("keyframe-period=45", vaapi)
        self.assertIn("webrtcbin name=webrtc", vaapi)
        self.assertIn("videotestsrc is-live=true", x264)
        self.assertIn("x264enc tune=zerolatency speed-preset=ultrafast", x264)
        self.assertIn("key-int-max=45", x264)

    def test_gstreamer_pipeline_builder_uses_separate_recording_branch_with_splitmuxsink(self):
        builder = GStreamerPipelineBuilder()

        pipeline = builder.recording_h264_pipeline(
            source_device="/dev/video0",
            capture_width=1920,
            capture_height=1080,
            capture_fps=30,
            bitrate_kbps=8000,
            segment_seconds=60,
            output_pattern="/recordings/front/%05d.mp4",
            encoder=EncoderChoice("vaapi", "requested_available"),
        )

        self.assertIn("v4l2src device=/dev/video0", pipeline)
        self.assertIn("queue max-size-buffers=0 max-size-time=0", pipeline)
        self.assertIn("video/x-raw,width=1920,height=1080,framerate=30/1", pipeline)
        self.assertIn("vaapih264enc", pipeline)
        self.assertIn("bitrate=8000", pipeline)
        self.assertIn("splitmuxsink", pipeline)
        self.assertIn("max-size-time=60000000000", pipeline)
        self.assertIn("location=/recordings/front/%05d.mp4", pipeline)
        self.assertNotIn("webrtcbin", pipeline)
        self.assertNotIn("rtph264pay", pipeline)
        self.assertNotIn("leaky=downstream", pipeline)

    def test_realtime_bitrate_policy_lowers_and_recovers_with_network_quality(self):
        policy = RealtimeBitrateAdaptationPolicy(
            target_bitrate_kbps=3000,
            min_bitrate_kbps=900,
            decrease_ratio=0.5,
            increase_ratio=1.25,
            max_rtt_ms=150,
            max_loss_percent=2.0,
        )

        degraded = policy.evaluate(
            current_bitrate_kbps=3000,
            sample=RealtimeNetworkSample(rtt_ms=180, packet_loss_percent=1.0),
        )
        floor = policy.evaluate(
            current_bitrate_kbps=1000,
            sample=RealtimeNetworkSample(rtt_ms=90, packet_loss_percent=5.0),
        )
        recovered = policy.evaluate(
            current_bitrate_kbps=1200,
            sample=RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=0.1),
        )

        self.assertEqual(degraded.bitrate_kbps, 1500)
        self.assertEqual(degraded.reason, "network_congested")
        self.assertEqual(floor.bitrate_kbps, 900)
        self.assertEqual(recovered.bitrate_kbps, 1500)
        self.assertEqual(recovered.reason, "network_recovered")

    def test_realtime_bitrate_policy_rejects_invalid_settings_and_samples(self):
        settings = {
            "target_bitrate_kbps": 3000,
            "min_bitrate_kbps": 900,
            "decrease_ratio": 0.5,
            "increase_ratio": 1.25,
            "max_rtt_ms": 150,
            "max_loss_percent": 2.0,
        }
        invalid_settings = [
            ("min_bitrate_kbps", True, "target bitrate"),
            ("increase_ratio", float("inf"), "increase_ratio"),
            ("max_rtt_ms", True, "network quality thresholds"),
            ("max_loss_percent", float("nan"), "network quality thresholds"),
        ]
        for field, value, message in invalid_settings:
            with self.subTest(field=field):
                kwargs = dict(settings)
                kwargs[field] = value
                with self.assertRaisesRegex(ValueError, message):
                    RealtimeBitrateAdaptationPolicy(**kwargs)

        policy = RealtimeBitrateAdaptationPolicy(**settings)
        valid_sample = RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=0.1)
        invalid_inputs = [
            (0, valid_sample, "current bitrate"),
            (True, valid_sample, "current bitrate"),
            (1200, RealtimeNetworkSample(rtt_ms=True, packet_loss_percent=0.1), "rtt_ms"),
            (1200, RealtimeNetworkSample(rtt_ms=-1, packet_loss_percent=0.1), "rtt_ms"),
            (1200, RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=True), "packet_loss_percent"),
            (1200, RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=float("nan")), "packet_loss_percent"),
        ]
        for current_bitrate_kbps, sample, message in invalid_inputs:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ValueError, message):
                    policy.evaluate(current_bitrate_kbps=current_bitrate_kbps, sample=sample)

    def test_realtime_profile_policy_switches_to_lower_fps_resolution_and_recovers(self):
        profile_720p = RealtimeProfileVariant(
            name="realtime_720p",
            width=1280,
            height=720,
            fps=30,
            bitrate_kbps=3000,
        )
        profile_480p = RealtimeProfileVariant(
            name="realtime_480p15",
            width=854,
            height=480,
            fps=15,
            bitrate_kbps=1200,
        )
        policy = RealtimeProfileAdaptationPolicy(
            profiles=[profile_720p, profile_480p],
            max_rtt_ms=150,
            max_loss_percent=2.0,
        )

        degraded = policy.evaluate(
            current_profile_name="realtime_720p",
            sample=RealtimeNetworkSample(rtt_ms=180, packet_loss_percent=1.0),
        )
        recovered = policy.evaluate(
            current_profile_name="realtime_480p15",
            sample=RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=0.1),
        )
        already_best = policy.evaluate(
            current_profile_name="realtime_720p",
            sample=RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=0.1),
        )

        self.assertEqual(degraded.profile_name, "realtime_480p15")
        self.assertEqual(degraded.width, 854)
        self.assertEqual(degraded.height, 480)
        self.assertEqual(degraded.fps, 15)
        self.assertEqual(degraded.bitrate_kbps, 1200)
        self.assertEqual(degraded.reason, "network_congested_profile_downshift")
        self.assertTrue(degraded.restart_required)
        self.assertEqual(recovered.profile_name, "realtime_720p")
        self.assertEqual(recovered.reason, "network_recovered_profile_upshift")
        self.assertFalse(already_best.restart_required)
        self.assertEqual(already_best.reason, "network_quality_ok")

    def test_realtime_profile_policy_rejects_invalid_profiles_and_unknown_current_profile(self):
        profile_720p = RealtimeProfileVariant(
            name="realtime_720p",
            width=1280,
            height=720,
            fps=30,
            bitrate_kbps=3000,
        )

        with self.assertRaisesRegex(ValueError, "positive width"):
            RealtimeProfileVariant(name="bad", width=0, height=720, fps=30, bitrate_kbps=3000)
        with self.assertRaisesRegex(ValueError, "duplicate realtime profile"):
            RealtimeProfileAdaptationPolicy(
                profiles=[profile_720p, profile_720p],
                max_rtt_ms=150,
                max_loss_percent=2.0,
            )
        policy = RealtimeProfileAdaptationPolicy(
            profiles=[profile_720p],
            max_rtt_ms=150,
            max_loss_percent=2.0,
        )

        with self.assertRaisesRegex(ValueError, "unknown realtime profile"):
            policy.evaluate(
                current_profile_name="missing",
                sample=RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=0.1),
            )

    def test_realtime_profile_policy_rejects_invalid_thresholds_and_samples(self):
        profile_720p = RealtimeProfileVariant(
            name="realtime_720p",
            width=1280,
            height=720,
            fps=30,
            bitrate_kbps=3000,
        )
        settings = {
            "profiles": [profile_720p],
            "max_rtt_ms": 150,
            "max_loss_percent": 2.0,
        }
        invalid_settings = [
            ("max_rtt_ms", True, "network quality thresholds"),
            ("max_loss_percent", True, "network quality thresholds"),
            ("max_loss_percent", float("nan"), "network quality thresholds"),
        ]
        for field, value, message in invalid_settings:
            with self.subTest(field=field):
                kwargs = dict(settings)
                kwargs[field] = value
                with self.assertRaisesRegex(ValueError, message):
                    RealtimeProfileAdaptationPolicy(**kwargs)

        policy = RealtimeProfileAdaptationPolicy(**settings)
        invalid_samples = [
            (RealtimeNetworkSample(rtt_ms=True, packet_loss_percent=0.1), "rtt_ms"),
            (RealtimeNetworkSample(rtt_ms=-1, packet_loss_percent=0.1), "rtt_ms"),
            (RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=True), "packet_loss_percent"),
            (RealtimeNetworkSample(rtt_ms=60, packet_loss_percent=float("nan")), "packet_loss_percent"),
        ]
        for sample, message in invalid_samples:
            with self.subTest(message=message):
                with self.assertRaisesRegex(ValueError, message):
                    policy.evaluate(current_profile_name="realtime_720p", sample=sample)

    def test_realtime_media_runtime_switches_profile_only_after_pipeline_hook_succeeds(self):
        profile_720p = RealtimeProfileVariant(
            name="realtime_720p",
            width=1280,
            height=720,
            fps=30,
            bitrate_kbps=3000,
        )
        profile_480p = RealtimeProfileVariant(
            name="realtime_480p15",
            width=854,
            height=480,
            fps=15,
            bitrate_kbps=1200,
        )
        policy = RealtimeProfileAdaptationPolicy(
            profiles=[profile_720p, profile_480p],
            max_rtt_ms=150,
            max_loss_percent=2.0,
        )
        controller = _FakeMediaPipelineController()
        runtime = media_module.RealtimeMediaRuntime(
            profile_bitrates={
                "realtime_720p": profile_720p.bitrate_kbps,
                "realtime_480p15": profile_480p.bitrate_kbps,
            },
            profile_variants_by_camera={
                "front": {
                    "realtime_720p": profile_720p,
                    "realtime_480p15": profile_480p,
                },
            },
            active_profile_by_camera={"front": "realtime_720p"},
            profile_switcher=controller.switch_realtime_profile,
        )

        decision = policy.evaluate(
            current_profile_name="realtime_720p",
            sample=RealtimeNetworkSample(rtt_ms=180, packet_loss_percent=1.0),
        )
        result = runtime.apply_realtime_profile_decision("front", decision)

        self.assertTrue(result.changed)
        self.assertTrue(result.restart_required)
        self.assertEqual(result.profile_name, "realtime_480p15")
        self.assertEqual(result.fps, 15)
        self.assertEqual(
            controller.calls,
            [("switch_realtime_profile", "front", "realtime_480p15")],
        )
        self.assertEqual(runtime.active_profile_by_camera["front"], "realtime_480p15")

        def failing_switcher(camera_id, profile_name):
            raise RuntimeError(f"cannot switch {camera_id} to {profile_name}")

        failing_runtime = media_module.RealtimeMediaRuntime(
            profile_bitrates={
                "realtime_720p": profile_720p.bitrate_kbps,
                "realtime_480p15": profile_480p.bitrate_kbps,
            },
            profile_variants_by_camera={
                "front": {
                    "realtime_720p": profile_720p,
                    "realtime_480p15": profile_480p,
                },
            },
            active_profile_by_camera={"front": "realtime_720p"},
            profile_switcher=failing_switcher,
        )

        with self.assertRaisesRegex(RuntimeError, "cannot switch front"):
            failing_runtime.apply_realtime_profile_decision("front", decision)
        self.assertEqual(failing_runtime.active_profile_by_camera["front"], "realtime_720p")

    def test_realtime_media_runtime_applies_allowed_bitrate_update_to_encoder_property(self):
        applied_updates = []
        self.assertTrue(hasattr(media_module, "RealtimeMediaRuntime"))
        runtime = media_module.RealtimeMediaRuntime(
            profile_bitrates={"realtime_720p": 3000},
            encoder_name_by_profile={"realtime_720p": "front_realtime_encoder"},
            property_setter=applied_updates.append,
        )

        decision = runtime.apply_runtime_update(
            "media.realtime_profiles.realtime_720p.bitrate_kbps",
            1800,
        )

        self.assertTrue(decision.allowed)
        self.assertEqual(decision.reason, "runtime_update_allowed")
        self.assertEqual(runtime.profile_bitrates["realtime_720p"], 1800)
        self.assertEqual(applied_updates[0].element_name, "front_realtime_encoder")
        self.assertEqual(applied_updates[0].property_name, "bitrate")
        self.assertEqual(applied_updates[0].value, 1800)

    def test_realtime_media_runtime_binds_bitrate_update_to_gstreamer_pipeline_element(self):
        self.assertTrue(hasattr(media_module, "GStreamerPipelinePropertySetter"))
        encoder = _FakeGStreamerElement()
        pipeline = _FakeGStreamerPipeline({"front_realtime_encoder": encoder})
        runtime = media_module.RealtimeMediaRuntime(
            profile_bitrates={"realtime_720p": 3000},
            encoder_name_by_profile={"realtime_720p": "front_realtime_encoder"},
            property_setter=media_module.GStreamerPipelinePropertySetter(pipeline),
        )

        decision = runtime.apply_runtime_update(
            "media.realtime_profiles.realtime_720p.bitrate_kbps",
            1800,
        )

        self.assertTrue(decision.allowed)
        self.assertEqual(encoder.properties, [("bitrate", 1800)])
        self.assertEqual(runtime.profile_bitrates["realtime_720p"], 1800)

    def test_realtime_media_runtime_rejects_missing_gstreamer_encoder_without_state_change(self):
        self.assertTrue(hasattr(media_module, "GStreamerPipelinePropertySetter"))
        runtime = media_module.RealtimeMediaRuntime(
            profile_bitrates={"realtime_720p": 3000},
            encoder_name_by_profile={"realtime_720p": "front_realtime_encoder"},
            property_setter=media_module.GStreamerPipelinePropertySetter(_FakeGStreamerPipeline({})),
        )

        with self.assertRaisesRegex(RuntimeError, "front_realtime_encoder"):
            runtime.apply_runtime_update(
                "media.realtime_profiles.realtime_720p.bitrate_kbps",
                1800,
            )

        self.assertEqual(runtime.profile_bitrates["realtime_720p"], 3000)
        self.assertEqual(runtime.applied_updates, [])

    def test_realtime_media_runtime_rejects_invalid_or_unknown_bitrate_update(self):
        applied_updates = []
        self.assertTrue(hasattr(media_module, "RealtimeMediaRuntime"))
        runtime = media_module.RealtimeMediaRuntime(
            profile_bitrates={"realtime_720p": 3000},
            encoder_name_by_profile={"realtime_720p": "front_realtime_encoder"},
            property_setter=applied_updates.append,
        )

        invalid = runtime.apply_runtime_update(
            "media.realtime_profiles.realtime_720p.bitrate_kbps",
            True,
        )
        unknown = runtime.apply_runtime_update(
            "media.realtime_profiles.rear_720p.bitrate_kbps",
            1800,
        )

        self.assertFalse(invalid.allowed)
        self.assertEqual(invalid.reason, "runtime_update_rejected_invalid_value")
        self.assertFalse(unknown.allowed)
        self.assertEqual(unknown.reason, "runtime_update_unknown_realtime_profile")
        self.assertEqual(runtime.profile_bitrates["realtime_720p"], 3000)
        self.assertEqual(applied_updates, [])

    def test_control_datachannel_config_is_unordered_and_unreliable(self):
        config = ControlDataChannelConfig.low_latency_default(label="control")

        self.assertFalse(config.ordered)
        self.assertEqual(config.max_retransmits, 0)
        self.assertEqual(config.to_dict()["protocol"], "mine-teleop-control-v1")

    def test_control_datachannel_config_exports_webrtc_init_options(self):
        config = ControlDataChannelConfig.low_latency_default(label="control")

        self.assertEqual(
            config.to_webrtc_init(),
            {
                "ordered": False,
                "maxRetransmits": 0,
                "protocol": "mine-teleop-control-v1",
            },
        )
        self.assertNotIn("max_retransmits", config.to_webrtc_init())

    def test_control_datachannel_docs_name_webrtc_init_export(self):
        readme = Path("README.md").read_text(encoding="utf-8")
        architecture = Path("docs/02-system-architecture.md").read_text(encoding="utf-8")
        docs = readme + "\n" + architecture

        self.assertIn("to_webrtc_init()", docs)
        self.assertIn("maxRetransmits", docs)

    def test_h264_sdp_checker_rejects_encoder_profile_not_offered_by_driver(self):
        checker = H264SdpCompatibilityChecker(driver_supported_profile_level_ids={"42e01f", "64001f"})
        constrained_baseline_only = H264SdpCompatibilityChecker(driver_supported_profile_level_ids={"42e01f"})
        driver_sdp = "\r\n".join(
            [
                "v=0",
                "m=video 9 UDP/TLS/RTP/SAVPF 96",
                "a=rtpmap:96 H264/90000",
                "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f;level-asymmetry-allowed=1",
            ]
        )

        compatible = checker.assess(encoder_profile_level_id="42e01f", remote_sdp=driver_sdp)
        high_profile = checker.assess(encoder_profile_level_id="64001f", remote_sdp=driver_sdp)
        unsupported_by_decoder = constrained_baseline_only.assess(encoder_profile_level_id="64001f", remote_sdp=driver_sdp)

        self.assertTrue(compatible.compatible)
        self.assertEqual(compatible.selected_profile_level_id, "42e01f")
        self.assertFalse(high_profile.compatible)
        self.assertIn("not offered by remote SDP", high_profile.reason)
        self.assertFalse(unsupported_by_decoder.compatible)
        self.assertIn("not supported by driver decoder", unsupported_by_decoder.reason)

    def test_h264_sdp_checker_ignores_non_h264_fmtp_profile_level_id(self):
        checker = H264SdpCompatibilityChecker(driver_supported_profile_level_ids={"42e01f"})
        driver_sdp = "\r\n".join(
            [
                "v=0",
                "m=video 9 UDP/TLS/RTP/SAVPF 96 97",
                "a=rtpmap:96 VP8/90000",
                "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f",
                "a=rtpmap:97 H264/90000",
            ]
        )

        result = checker.assess(encoder_profile_level_id="42e01f", remote_sdp=driver_sdp)

        self.assertFalse(result.compatible)
        self.assertEqual(result.reason, "remote SDP did not offer H264 profile-level-id")

    def test_h264_sdp_checker_accepts_any_matching_payload_with_supported_packetization(self):
        checker = H264SdpCompatibilityChecker(driver_supported_profile_level_ids={"42e01f"})
        driver_sdp = "\r\n".join(
            [
                "v=0",
                "m=video 9 UDP/TLS/RTP/SAVPF 96 97",
                "a=rtpmap:96 H264/90000",
                "a=fmtp:96 packetization-mode=1;profile-level-id=42e01f",
                "a=rtpmap:97 H264/90000",
                "a=fmtp:97 packetization-mode=0;profile-level-id=42e01f",
            ]
        )

        result = checker.assess(encoder_profile_level_id="42e01f", remote_sdp=driver_sdp)

        self.assertTrue(result.compatible)
        self.assertEqual(result.selected_profile_level_id, "42e01f")

    def test_ffmpeg_vaapi_probe_plan_builds_four_lane_docker_command(self):
        plan = FFmpegVaapiProbePlan(
            render_device="/dev/dri/renderD128",
            card_device="/dev/dri/card1",
            output_dir="/tmp/mine-teleop-vaapi",
            lanes=4,
            width=1280,
            height=720,
            fps=30,
            duration_seconds=5,
            bitrate="4M",
        )
        command = plan.docker_command()

        self.assertIn("--device /dev/dri/renderD128", command)
        self.assertIn("--device /dev/dri/card1", command)
        self.assertEqual(command.count("-c:v h264_vaapi"), 4)
        self.assertIn("testsrc2=size=1280x720:rate=30", command)
        self.assertIn("ffprobe -hide_banner", command)

    def test_gstreamer_plugin_probe_plan_checks_hardware_encoders_and_cpu_fallback(self):
        plan = GStreamerPluginProbePlan.default()

        self.assertEqual(
            plan.command,
            "gst-inspect-1.0 vaapih264enc qsvh264enc vah264enc nvh264enc x264enc",
        )
        self.assertIn("vaapih264enc", plan.hardware_plugins)
        self.assertIn("x264enc", plan.fallback_plugins)

    def test_hardware_encoding_validation_plan_covers_four_camera_vaapi_load_scenarios(self):
        plan = HardwareEncodingValidationPlan.four_camera_default(output_dir="/tmp/mine-teleop-vaapi")
        scenario_names = [scenario.name for scenario in plan.scenarios]

        self.assertEqual(
            scenario_names,
            [
                "four-camera-realtime-720p30",
                "four-camera-recording-source",
                "four-camera-realtime-plus-recording",
            ],
        )
        self.assertEqual(len(plan.scenarios[0].lanes), 4)
        self.assertEqual(len(plan.scenarios[1].lanes), 4)
        self.assertEqual(len(plan.scenarios[2].lanes), 8)
        combined_command = plan.scenarios[2].docker_command()
        self.assertEqual(combined_command.count("-c:v h264_vaapi"), 8)
        self.assertIn("pids=", combined_command)
        self.assertIn('& pids="$pids $!"', combined_command)
        self.assertIn('for pid in $pids; do wait "$pid"; done', combined_command)
        self.assertIn("testsrc2=size=1280x720:rate=30", combined_command)
        self.assertIn("testsrc2=size=1920x1080:rate=30", combined_command)
        self.assertIn("/out/four-camera-realtime-plus-recording-front-realtime-720p30.mp4", combined_command)
        self.assertIn("gst-inspect-1.0 vaapih264enc", plan.gstreamer_plugin_probe.command)
        self.assertIn("gpu_percent", plan.metrics_fields)
        self.assertIn("disk_write_mb_s", plan.metrics_fields)

    def test_hardware_encoding_validation_report_accepts_expected_ffprobe_outputs(self):
        scenario = HardwareEncodingValidationPlan.four_camera_default().scenarios[0]
        ffprobe_outputs = {
            lane.lane_id: "\n".join(
                [
                    "codec_name=h264",
                    f"width={lane.width}",
                    f"height={lane.height}",
                    "avg_frame_rate=30/1",
                    "bit_rate=3000000",
                ]
            )
            for lane in scenario.lanes
        }

        report = HardwareEncodingValidationReport.from_ffprobe_outputs(
            scenario,
            ffprobe_outputs,
            metrics={
                "cpu_percent": 42.5,
                "gpu_percent": 71.0,
                "memory_mb": 1536.0,
                "disk_write_mb_s": 24.0,
                "temperature_c": 62.0,
                "dropped_frames": 0,
            },
        )

        self.assertTrue(report.passed)
        self.assertEqual(report.scenario_name, "four-camera-realtime-720p30")
        self.assertEqual(len(report.lanes), 4)
        self.assertEqual(report.lanes[0].bitrate_kbps, 3000)
        records = [json.loads(line) for line in report.to_jsonl()]
        self.assertEqual(records[0]["event"], "hardware_encoding_validation")
        self.assertTrue(records[0]["passed"])
        self.assertEqual(records[0]["lane_count"], 4)
        self.assertEqual(records[-1]["event"], "hardware_encoding_metrics")
        self.assertEqual(records[-1]["metrics"]["gpu_percent"], 71.0)

    def test_hardware_encoding_validation_report_flags_codec_resolution_and_fps_mismatches(self):
        scenario = HardwareEncodingValidationPlan.four_camera_default().scenarios[0]
        first_lane = scenario.lanes[0]

        report = HardwareEncodingValidationReport.from_ffprobe_outputs(
            scenario,
            {
                first_lane.lane_id: "\n".join(
                    [
                        "codec_name=hevc",
                        "width=640",
                        f"height={first_lane.height}",
                        "avg_frame_rate=15/1",
                        "bit_rate=0",
                    ]
                )
            },
            metrics={"cpu_percent": 10.0},
        )

        self.assertFalse(report.passed)
        self.assertIn("missing ffprobe output", " ".join(report.failures))
        self.assertIn("expected h264", " ".join(report.failures))
        self.assertIn("expected 1280x720", " ".join(report.failures))
        self.assertIn("fps 15.00 below expected 30", " ".join(report.failures))
        self.assertIn("bit_rate must be positive", " ".join(report.failures))

    def test_segment_writer_creates_video_and_sidecar_metadata_atomically(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            writer = SegmentWriter(root)
            metadata = SegmentMetadata(
                vehicle_id="vehicle-001",
                session_id="session-001",
                camera_id="front",
                segment_id="20260624T101500Z_front_000001",
                started_at="2026-06-24T10:15:00Z",
                ended_at="2026-06-24T10:16:00Z",
                codec="h264",
                encoder="vaapi",
                width=1920,
                height=1080,
                fps=30,
                upload_state="pending",
            )

            result = writer.write_segment(metadata, payload=b"fake-h264")

            self.assertTrue(result.video_path.exists())
            self.assertTrue(result.metadata_path.exists())
            self.assertFalse(result.video_path.with_suffix(".mp4.tmp").exists())
            sidecar = json.loads(result.metadata_path.read_text(encoding="utf-8"))
            self.assertEqual(sidecar["file_size_bytes"], len(b"fake-h264"))
            self.assertEqual(sidecar["upload_state"], "pending")

    def test_upload_queue_persists_and_refreshes_expiring_presigned_urls(self):
        with tempfile.TemporaryDirectory() as tmp:
            state_path = Path(tmp) / "upload-queue.json"
            queue = UploadQueue(state_path, refresh_margin_seconds=300)
            queue.enqueue(
                segment_id="seg-1",
                video_path="/recordings/front/seg-1.mp4",
                metadata_path="/recordings/front/seg-1.json",
                object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                upload_url="https://storage.example/upload/seg-1",
                expires_at_ms=1_000_000,
            )

            reloaded = UploadQueue(state_path, refresh_margin_seconds=300)
            action = reloaded.next_action(now_ms=900_000)

            self.assertEqual(action.action, "credential_refresh")
            self.assertEqual(action.item.segment_id, "seg-1")

    def test_upload_queue_records_failures_with_exponential_retry_backoff(self):
        with tempfile.TemporaryDirectory() as tmp:
            state_path = Path(tmp) / "upload-queue.json"
            queue = UploadQueue(
                state_path,
                refresh_margin_seconds=300,
                retry_initial_seconds=10,
                retry_max_seconds=60,
            )
            queue.enqueue(
                segment_id="seg-1",
                video_path="/recordings/front/seg-1.mp4",
                metadata_path="/recordings/front/seg-1.json",
                object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                upload_url=None,
                expires_at_ms=None,
            )

            first = queue.next_action(now_ms=1000)
            queue.mark_failed(first.item.segment_id, "network_timeout", now_ms=1000)
            self.assertEqual(queue.next_action(now_ms=10_999).action, "wait")
            retry = queue.next_action(now_ms=11_000)
            self.assertEqual(retry.action, "upload")
            queue.mark_failed(retry.item.segment_id, "server_500", now_ms=11_000)

            reloaded = UploadQueue(state_path, refresh_margin_seconds=300)
            self.assertEqual(reloaded.items[0].retry_count, 2)
            self.assertEqual(reloaded.items[0].last_error, "server_500")
            self.assertEqual(reloaded.next_action(now_ms=30_999).action, "wait")
            self.assertEqual(reloaded.next_action(now_ms=31_000).action, "upload")

    def test_upload_queue_rejects_invalid_failure_reason_before_persisting(self):
        cases = [
            (True, "upload queue failure reason must be a non-empty string"),
            ("", "upload queue failure reason must be a non-empty string"),
        ]
        for reason, expected_error in cases:
            with self.subTest(reason=reason):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    queue = UploadQueue(state_path, refresh_margin_seconds=300)
                    queue.enqueue(
                        segment_id="seg-1",
                        video_path="/recordings/front/seg-1.mp4",
                        metadata_path="/recordings/front/seg-1.json",
                        object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                        upload_url=None,
                        expires_at_ms=None,
                    )
                    before = state_path.read_text(encoding="utf-8")

                    with self.assertRaisesRegex(ValueError, expected_error):
                        queue.mark_failed("seg-1", reason, now_ms=1000)

                    self.assertEqual(state_path.read_text(encoding="utf-8"), before)

    def test_upload_queue_rejects_invalid_enqueue_item_before_persisting(self):
        base_kwargs = {
            "segment_id": "seg-1",
            "video_path": "/recordings/front/seg-1.mp4",
            "metadata_path": "/recordings/front/seg-1.json",
            "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
            "upload_url": None,
            "expires_at_ms": None,
        }
        cases = [
            (
                "segment_id",
                True,
                "upload queue items[0].segment_id must be a non-empty string",
            ),
            (
                "upload_url",
                42,
                "upload queue items[0].upload_url must be a string or null",
            ),
            (
                "expires_at_ms",
                True,
                "upload queue items[0].expires_at_ms must be a non-negative integer or null",
            ),
            (
                "metadata_upload_url",
                42,
                "upload queue items[0].metadata_upload_url must be a string or null",
            ),
            (
                "enqueued_at_ms",
                -1,
                "upload queue items[0].enqueued_at_ms must be a non-negative integer or null",
            ),
        ]
        for field, value, expected_error in cases:
            with self.subTest(field=field, value=value):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    queue = UploadQueue(state_path, refresh_margin_seconds=300)
                    kwargs = {**base_kwargs, field: value}

                    with self.assertRaises(ValueError) as caught:
                        queue.enqueue(**kwargs)

                    self.assertEqual(str(caught.exception), expected_error)
                    self.assertFalse(state_path.exists())
                    self.assertEqual(queue.items, [])

    def test_upload_queue_rejects_invalid_timing_settings(self):
        cases = [
            (
                {"refresh_margin_seconds": 0},
                "refresh_margin_seconds must be positive",
            ),
            (
                {"refresh_margin_seconds": True},
                "refresh_margin_seconds must be positive",
            ),
            (
                {"refresh_margin_seconds": "300"},
                "refresh_margin_seconds must be positive",
            ),
            (
                {"retry_initial_seconds": 0},
                "retry_initial_seconds must be positive",
            ),
            (
                {"retry_initial_seconds": True},
                "retry_initial_seconds must be positive",
            ),
            (
                {"retry_initial_seconds": "10"},
                "retry_initial_seconds must be positive",
            ),
            (
                {"retry_max_seconds": 0},
                "retry_max_seconds must be positive",
            ),
            (
                {"retry_max_seconds": True},
                "retry_max_seconds must be positive",
            ),
            (
                {"retry_max_seconds": "600"},
                "retry_max_seconds must be positive",
            ),
            (
                {"retry_initial_seconds": 120, "retry_max_seconds": 60},
                "retry_initial_seconds must be less than or equal to retry_max_seconds",
            ),
        ]
        for kwargs, expected_error in cases:
            with self.subTest(kwargs=kwargs):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    params = {"refresh_margin_seconds": 300, **kwargs}
                    with self.assertRaisesRegex(ValueError, expected_error):
                        UploadQueue(state_path, **params)

    def test_upload_queue_rejects_invalid_persisted_pause_state(self):
        cases = [
            (
                {"paused": "false", "pause_reason": None, "items": []},
                "upload queue paused must be a boolean",
            ),
            (
                {"paused": True, "pause_reason": False, "items": []},
                "upload queue pause_reason must be a string or null",
            ),
        ]
        for payload, expected_error in cases:
            with self.subTest(payload=payload):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    state_path.write_text(json.dumps(payload), encoding="utf-8")

                    with self.assertRaisesRegex(ValueError, expected_error):
                        UploadQueue(state_path, refresh_margin_seconds=300)

    def test_upload_queue_rejects_invalid_persisted_items_shape(self):
        cases = [
            (
                {"paused": False, "pause_reason": None, "items": "not-a-list"},
                "upload queue items must be a list",
            ),
            (
                {"paused": False, "pause_reason": None, "items": ["not-an-object"]},
                r"upload queue items\[0\] must be an object",
            ),
        ]
        for payload, expected_error in cases:
            with self.subTest(payload=payload):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    state_path.write_text(json.dumps(payload), encoding="utf-8")

                    with self.assertRaisesRegex(ValueError, expected_error):
                        UploadQueue(state_path, refresh_margin_seconds=300)

    def test_upload_queue_rejects_invalid_persisted_item_status(self):
        base_item = {
            "segment_id": "seg-1",
            "video_path": "/recordings/front/seg-1.mp4",
            "metadata_path": "/recordings/front/seg-1.json",
            "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
            "upload_url": None,
            "expires_at_ms": None,
        }
        cases = [
            True,
            "stuck",
        ]
        for status in cases:
            with self.subTest(status=status):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    payload = {
                        "paused": False,
                        "pause_reason": None,
                        "items": [{**base_item, "status": status}],
                    }
                    state_path.write_text(json.dumps(payload), encoding="utf-8")

                    with self.assertRaisesRegex(
                        ValueError,
                        r"upload queue items\[0\]\.status must be one of credential_refresh, failed, pending, retry_wait, uploaded, uploading",
                    ):
                        UploadQueue(state_path, refresh_margin_seconds=300)

    def test_upload_queue_rejects_invalid_persisted_item_scalar_fields(self):
        base_item = {
            "segment_id": "seg-1",
            "video_path": "/recordings/front/seg-1.mp4",
            "metadata_path": "/recordings/front/seg-1.json",
            "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
            "upload_url": None,
            "expires_at_ms": None,
        }
        cases = [
            (
                "segment_id",
                True,
                "upload queue items[0].segment_id must be a non-empty string",
            ),
            (
                "upload_url",
                42,
                "upload queue items[0].upload_url must be a string or null",
            ),
            (
                "expires_at_ms",
                "soon",
                "upload queue items[0].expires_at_ms must be a non-negative integer or null",
            ),
            (
                "retry_count",
                -1,
                "upload queue items[0].retry_count must be a non-negative integer",
            ),
            (
                "retry_count",
                True,
                "upload queue items[0].retry_count must be a non-negative integer",
            ),
        ]
        for field, value, expected_error in cases:
            with self.subTest(field=field, value=value):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    payload = {
                        "paused": False,
                        "pause_reason": None,
                        "items": [{**base_item, field: value}],
                    }
                    state_path.write_text(json.dumps(payload), encoding="utf-8")

                    with self.assertRaises(ValueError) as caught:
                        UploadQueue(state_path, refresh_margin_seconds=300)
                    self.assertEqual(str(caught.exception), expected_error)

    def test_upload_queue_rejects_invalid_pause_reason_before_persisting(self):
        cases = [
            (True, "upload queue pause reason must be a non-empty string"),
            ("", "upload queue pause reason must be a non-empty string"),
        ]
        for reason, expected_error in cases:
            with self.subTest(reason=reason):
                with tempfile.TemporaryDirectory() as tmp:
                    state_path = Path(tmp) / "upload-queue.json"
                    queue = UploadQueue(state_path, refresh_margin_seconds=300)

                    with self.assertRaisesRegex(ValueError, expected_error):
                        queue.pause(reason)

                    self.assertFalse(state_path.exists())

    def test_upload_queue_pause_preserves_pending_item_until_resume(self):
        with tempfile.TemporaryDirectory() as tmp:
            state_path = Path(tmp) / "upload-queue.json"
            queue = UploadQueue(state_path, refresh_margin_seconds=300)
            queue.enqueue(
                segment_id="seg-1",
                video_path="/recordings/front/seg-1.mp4",
                metadata_path="/recordings/front/seg-1.json",
                object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                upload_url=None,
                expires_at_ms=None,
            )

            queue.pause("network_quality_poor")
            paused = queue.next_action(now_ms=1000)
            reloaded = UploadQueue(state_path, refresh_margin_seconds=300)

            self.assertEqual(paused.action, "paused")
            self.assertEqual(paused.item.status, "pending")
            self.assertTrue(reloaded.paused)
            self.assertEqual(reloaded.pause_reason, "network_quality_poor")
            self.assertEqual(reloaded.next_action(now_ms=1000).action, "paused")

            reloaded.resume()
            upload = reloaded.next_action(now_ms=1000)

            self.assertEqual(upload.action, "upload")
            self.assertEqual(upload.item.segment_id, "seg-1")

    def test_upload_network_quality_policy_pauses_queue_when_5g_degrades(self):
        with tempfile.TemporaryDirectory() as tmp:
            state_path = Path(tmp) / "upload-queue.json"
            queue = UploadQueue(state_path, refresh_margin_seconds=300)
            queue.enqueue(
                segment_id="seg-1",
                video_path="/recordings/front/seg-1.mp4",
                metadata_path="/recordings/front/seg-1.json",
                object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                upload_url=None,
                expires_at_ms=None,
            )
            policy = UploadNetworkQualityPolicy(
                max_rtt_ms=150,
                max_jitter_ms=50,
                max_loss_percent=2.0,
                min_uplink_mbps=3.0,
            )

            degraded = policy.evaluate(
                NetworkQualitySample(
                    connected=True,
                    rtt_ms=180,
                    jitter_ms=20,
                    packet_loss_percent=1.0,
                    uplink_mbps=5.0,
                )
            )
            healthy = policy.evaluate(
                NetworkQualitySample(
                    connected=True,
                    rtt_ms=80,
                    jitter_ms=10,
                    packet_loss_percent=0.2,
                    uplink_mbps=6.0,
                )
            )
            if degraded.pause:
                queue.pause(degraded.reason)

            self.assertTrue(degraded.pause)
            self.assertEqual(degraded.reason, "network_rtt_exceeded")
            self.assertFalse(healthy.pause)
            self.assertEqual(healthy.reason, "network_quality_ok")
            self.assertEqual(queue.next_action(now_ms=1000).action, "paused")
            self.assertEqual(queue.pause_reason, "network_rtt_exceeded")

    def test_upload_network_quality_policy_rejects_invalid_thresholds(self):
        cases = [
            {"max_rtt_ms": True, "max_jitter_ms": 50, "max_loss_percent": 2.0, "min_uplink_mbps": 3.0},
            {"max_rtt_ms": 150, "max_jitter_ms": True, "max_loss_percent": 2.0, "min_uplink_mbps": 3.0},
            {"max_rtt_ms": 150, "max_jitter_ms": 50, "max_loss_percent": True, "min_uplink_mbps": 3.0},
            {"max_rtt_ms": 150, "max_jitter_ms": 50, "max_loss_percent": 2.0, "min_uplink_mbps": True},
        ]
        for kwargs in cases:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(
                    ValueError,
                    "network quality thresholds must be non-negative and bandwidth must be positive",
                ):
                    UploadNetworkQualityPolicy(**kwargs)

    def test_upload_network_quality_policy_rejects_invalid_samples(self):
        policy = UploadNetworkQualityPolicy(
            max_rtt_ms=150,
            max_jitter_ms=50,
            max_loss_percent=2.0,
            min_uplink_mbps=3.0,
        )
        cases = [
            (
                NetworkQualitySample(
                    connected="yes",
                    rtt_ms=80,
                    jitter_ms=10,
                    packet_loss_percent=0.2,
                    uplink_mbps=6.0,
                ),
                "network quality sample connected must be a boolean",
            ),
            (
                NetworkQualitySample(
                    connected=True,
                    rtt_ms=True,
                    jitter_ms=10,
                    packet_loss_percent=0.2,
                    uplink_mbps=6.0,
                ),
                "network quality sample rtt_ms must be a non-negative integer",
            ),
            (
                NetworkQualitySample(
                    connected=True,
                    rtt_ms=80,
                    jitter_ms=-1,
                    packet_loss_percent=0.2,
                    uplink_mbps=6.0,
                ),
                "network quality sample jitter_ms must be a non-negative integer",
            ),
            (
                NetworkQualitySample(
                    connected=True,
                    rtt_ms=80,
                    jitter_ms=10,
                    packet_loss_percent=-0.1,
                    uplink_mbps=6.0,
                ),
                "network quality sample packet_loss_percent must be non-negative",
            ),
            (
                NetworkQualitySample(
                    connected=True,
                    rtt_ms=80,
                    jitter_ms=10,
                    packet_loss_percent=0.2,
                    uplink_mbps=-1.0,
                ),
                "network quality sample uplink_mbps must be non-negative",
            ),
        ]
        for sample, expected_error in cases:
            with self.subTest(sample=sample):
                with self.assertRaisesRegex(ValueError, expected_error):
                    policy.evaluate(sample)

    def test_upload_trigger_policy_dispatches_by_count_bytes_time_or_network_idle(self):
        policy = UploadTriggerPolicy(
            trigger_segments=3,
            trigger_bytes=10_000,
            trigger_interval_ms=60_000,
            network_idle_enabled=True,
        )

        self.assertEqual(
            policy.evaluate(pending_segments=2, pending_bytes=9_000, oldest_pending_age_ms=30_000).reason,
            "waiting_for_trigger",
        )
        self.assertEqual(
            policy.evaluate(pending_segments=3, pending_bytes=9_000, oldest_pending_age_ms=30_000).reason,
            "segment_count",
        )
        self.assertEqual(
            policy.evaluate(pending_segments=2, pending_bytes=10_000, oldest_pending_age_ms=30_000).reason,
            "accumulated_bytes",
        )
        self.assertEqual(
            policy.evaluate(pending_segments=2, pending_bytes=9_000, oldest_pending_age_ms=60_000).reason,
            "time_window",
        )
        self.assertEqual(
            policy.evaluate(pending_segments=1, pending_bytes=1, oldest_pending_age_ms=1, network_idle=True).reason,
            "network_idle",
        )

    def test_upload_trigger_policy_rejects_invalid_settings(self):
        cases = [
            (
                {"trigger_segments": True},
                "trigger_segments must be a positive integer",
            ),
            (
                {"trigger_segments": 0},
                "trigger_segments must be a positive integer",
            ),
            (
                {"trigger_bytes": True},
                "trigger_bytes must be a positive integer or null",
            ),
            (
                {"trigger_bytes": 0},
                "trigger_bytes must be a positive integer or null",
            ),
            (
                {"trigger_interval_ms": True},
                "trigger_interval_ms must be a positive integer or null",
            ),
            (
                {"trigger_interval_ms": 0},
                "trigger_interval_ms must be a positive integer or null",
            ),
            (
                {"network_idle_enabled": "yes"},
                "network_idle_enabled must be a boolean",
            ),
        ]
        for kwargs, expected_error in cases:
            with self.subTest(kwargs=kwargs):
                params = {"trigger_segments": 1, **kwargs}
                with self.assertRaisesRegex(ValueError, expected_error):
                    UploadTriggerPolicy(**params)

    def test_upload_trigger_policy_rejects_invalid_evaluation_inputs(self):
        policy = UploadTriggerPolicy(
            trigger_segments=3,
            trigger_bytes=10_000,
            trigger_interval_ms=60_000,
            network_idle_enabled=True,
        )
        cases = [
            (
                {"pending_segments": True, "pending_bytes": 1, "oldest_pending_age_ms": 1},
                "pending_segments must be a non-negative integer",
            ),
            (
                {"pending_segments": -1, "pending_bytes": 1, "oldest_pending_age_ms": 1},
                "pending_segments must be a non-negative integer",
            ),
            (
                {"pending_segments": 1, "pending_bytes": True, "oldest_pending_age_ms": 1},
                "pending_bytes must be a non-negative integer",
            ),
            (
                {"pending_segments": 1, "pending_bytes": -1, "oldest_pending_age_ms": 1},
                "pending_bytes must be a non-negative integer",
            ),
            (
                {"pending_segments": 1, "pending_bytes": 1, "oldest_pending_age_ms": True},
                "oldest_pending_age_ms must be a non-negative integer",
            ),
            (
                {"pending_segments": 1, "pending_bytes": 1, "oldest_pending_age_ms": -1},
                "oldest_pending_age_ms must be a non-negative integer",
            ),
            (
                {"pending_segments": 1, "pending_bytes": 1, "oldest_pending_age_ms": 1, "network_idle": "yes"},
                "network_idle must be a boolean",
            ),
        ]
        for kwargs, expected_error in cases:
            with self.subTest(kwargs=kwargs):
                with self.assertRaisesRegex(ValueError, expected_error):
                    policy.evaluate(**kwargs)

    def test_upload_bandwidth_limiter_delays_next_file_from_previous_uploaded_bytes(self):
        limiter = UploadBandwidthLimiter(max_mbps=1)

        self.assertEqual(limiter.retry_after_ms(now_ms=1_000), 0)

        limiter.record_upload(bytes_uploaded=125_000, finished_at_ms=1_000)

        self.assertEqual(limiter.retry_after_ms(now_ms=1_499), 501)
        self.assertEqual(limiter.retry_after_ms(now_ms=2_000), 0)

    def test_upload_bandwidth_limiter_rejects_boolean_limit(self):
        for max_mbps in (True, "5"):
            with self.subTest(max_mbps=max_mbps):
                with self.assertRaisesRegex(ValueError, "max_mbps must be positive"):
                    UploadBandwidthLimiter(max_mbps=max_mbps)

    def test_http_put_uploader_rejects_invalid_timeout_types(self):
        for timeout_seconds in (True, "30"):
            with self.subTest(timeout_seconds=timeout_seconds):
                with self.assertRaisesRegex(ValueError, "timeout_seconds must be positive"):
                    HttpPutUploader(timeout_seconds=timeout_seconds)

    def test_upload_bandwidth_limiter_rejects_invalid_upload_samples(self):
        cases = [
            (
                {"bytes_uploaded": True, "finished_at_ms": 1_000},
                "bytes_uploaded must be a non-negative integer",
            ),
            (
                {"bytes_uploaded": -1, "finished_at_ms": 1_000},
                "bytes_uploaded must be non-negative",
            ),
            (
                {"bytes_uploaded": 125_000, "finished_at_ms": True},
                "finished_at_ms must be a non-negative integer",
            ),
            (
                {"bytes_uploaded": 125_000, "finished_at_ms": -1},
                "finished_at_ms must be non-negative",
            ),
        ]
        for kwargs, expected_error in cases:
            with self.subTest(kwargs=kwargs):
                limiter = UploadBandwidthLimiter(max_mbps=1)
                limiter.record_upload(bytes_uploaded=125_000, finished_at_ms=1_000)
                before = limiter.retry_after_ms(now_ms=1_499)

                with self.assertRaisesRegex(ValueError, expected_error):
                    limiter.record_upload(**kwargs)

                self.assertEqual(limiter.retry_after_ms(now_ms=1_499), before)

    def test_upload_credential_service_refreshes_url_without_changing_object_path(self):
        service = UploadCredentialService(public_base_url="http://upload.local", ttl_seconds=900)
        request = {
            "vehicle_id": "vehicle-001",
            "session_id": "session-001",
            "camera_id": "front",
            "segment_id": "seg-1",
            "kind": "video",
        }

        first = service.issue(request, now_ms=1_000)
        second = service.issue(request, now_ms=2_000)

        self.assertEqual(first.object_path, second.object_path)
        self.assertNotEqual(first.upload_url, second.upload_url)
        self.assertEqual(first.expires_at_ms, 901_000)

    def test_upload_credential_service_rejects_invalid_ttl(self):
        for ttl_seconds in (0, -1, True):
            with self.subTest(ttl_seconds=ttl_seconds):
                with self.assertRaisesRegex(ValueError, "ttl_seconds must be a positive integer"):
                    UploadCredentialService(ttl_seconds=ttl_seconds)

    def test_upload_credential_service_rejects_non_integer_uploaded_bytes(self):
        service = UploadCredentialService(public_base_url="http://upload.local", ttl_seconds=900)
        for bytes_uploaded in (True, "64000000"):
            with self.subTest(bytes_uploaded=bytes_uploaded):
                with self.assertRaisesRegex(ValueError, "bytes_uploaded must be a non-negative integer"):
                    service.mark_uploaded(
                        segment_id="seg-1",
                        object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                        bytes_uploaded=bytes_uploaded,
                    )

    def test_upload_credential_service_rejects_invalid_failure_error(self):
        service = UploadCredentialService(public_base_url="http://upload.local", ttl_seconds=900)
        cases = [
            (True, "upload failure error must be a non-empty string"),
            (42, "upload failure error must be a non-empty string"),
            ("", "upload failure error is required"),
            ("   ", "upload failure error is required"),
        ]
        for error, expected_error in cases:
            with self.subTest(error=error):
                with self.assertRaisesRegex(ValueError, expected_error):
                    service.mark_failed(
                        segment_id="seg-bad-error",
                        object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-bad-error.mp4",
                        error=error,
                    )
                self.assertEqual(service.records, {})

    def test_upload_credential_service_rejects_invalid_record_identity_fields(self):
        valid_segment = "seg-bad-identity"
        valid_object_path = "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-bad-identity.mp4"
        cases = [
            (
                "mark_uploaded",
                {"segment_id": True, "object_path": valid_object_path, "bytes_uploaded": 1},
                "segment_id must be a non-empty string",
            ),
            (
                "mark_uploaded",
                {"segment_id": valid_segment, "object_path": "", "bytes_uploaded": 1},
                "object_path must be a non-empty string",
            ),
            (
                "mark_failed",
                {"segment_id": 42, "object_path": valid_object_path, "error": "network_timeout"},
                "segment_id must be a non-empty string",
            ),
            (
                "mark_failed",
                {"segment_id": valid_segment, "object_path": True, "error": "network_timeout"},
                "object_path must be a non-empty string",
            ),
        ]
        for method_name, kwargs, expected_error in cases:
            with self.subTest(method_name=method_name, kwargs=kwargs):
                service = UploadCredentialService(public_base_url="http://upload.local", ttl_seconds=900)
                with self.assertRaisesRegex(ValueError, expected_error):
                    getattr(service, method_name)(**kwargs)
                self.assertEqual(service.records, {})

    def test_upload_credential_service_can_issue_s3_compatible_presigned_put_url(self):
        config = S3PresignConfig(
            endpoint_url="https://s3.us-west-2.amazonaws.com",
            bucket="mine-teleop-recordings",
            region="us-west-2",
            access_key_id="AKIDEXAMPLE",
            secret_access_key="wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY",
            session_token="dev-session-token",
        )
        service = UploadCredentialService(
            ttl_seconds=900,
            s3_signer=S3PresignedPutSigner(config),
        )
        request = {
            "vehicle_id": "vehicle-001",
            "session_id": "session-001",
            "camera_id": "front",
            "segment_id": "seg-1",
            "kind": "video",
        }

        credential = service.issue(request, now_ms=0)
        parsed = urlparse(credential.upload_url)
        query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
        changed_secret = UploadCredentialService(
            ttl_seconds=900,
            s3_signer=S3PresignedPutSigner(
                S3PresignConfig(
                    endpoint_url="https://s3.us-west-2.amazonaws.com",
                    bucket="mine-teleop-recordings",
                    region="us-west-2",
                    access_key_id="AKIDEXAMPLE",
                    secret_access_key="DIFFERENTSECRET",
                    session_token="dev-session-token",
                )
            ),
        ).issue(request, now_ms=0)
        changed_query = {
            key: values[0] for key, values in parse_qs(urlparse(changed_secret.upload_url).query).items()
        }

        self.assertEqual(parsed.scheme, "https")
        self.assertEqual(parsed.netloc, "s3.us-west-2.amazonaws.com")
        self.assertEqual(
            parsed.path,
            "/mine-teleop-recordings/vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
        )
        self.assertEqual(query["X-Amz-Algorithm"], "AWS4-HMAC-SHA256")
        self.assertEqual(query["X-Amz-Credential"], "AKIDEXAMPLE/19700101/us-west-2/s3/aws4_request")
        self.assertEqual(query["X-Amz-Date"], "19700101T000000Z")
        self.assertEqual(query["X-Amz-Expires"], "900")
        self.assertEqual(query["X-Amz-SignedHeaders"], "host")
        self.assertEqual(query["X-Amz-Security-Token"], "dev-session-token")
        self.assertEqual(query["X-Amz-Content-Sha256"], "UNSIGNED-PAYLOAD")
        self.assertRegex(query["X-Amz-Signature"], r"^[0-9a-f]{64}$")
        self.assertNotEqual(query["X-Amz-Signature"], changed_query["X-Amz-Signature"])
        self.assertEqual(
            credential.object_path,
            "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
        )
        self.assertEqual(credential.expires_at_ms, 900_000)

    def test_s3_presigner_rejects_missing_required_signing_fields(self):
        config = S3PresignConfig(
            endpoint_url="https://s3.us-west-2.amazonaws.com",
            bucket="mine-teleop-recordings",
            region="us-west-2",
            access_key_id="AKIDEXAMPLE",
            secret_access_key="SECRET",
        )
        cases = [
            ("region", "region is required"),
            ("access_key_id", "access_key_id is required"),
            ("secret_access_key", "secret_access_key is required"),
        ]

        for field_name, expected_error in cases:
            with self.subTest(field_name=field_name):
                signer = S3PresignedPutSigner(replace(config, **{field_name: ""}))
                with self.assertRaisesRegex(ValueError, expected_error):
                    signer.presign_put(
                        object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                        expires_seconds=900,
                        now_ms=0,
                    )

    def test_s3_presigner_rejects_non_string_signing_fields(self):
        config = S3PresignConfig(
            endpoint_url="https://s3.us-west-2.amazonaws.com",
            bucket="mine-teleop-recordings",
            region="us-west-2",
            access_key_id="AKIDEXAMPLE",
            secret_access_key="SECRET",
        )
        cases = [
            ("endpoint_url", True, "endpoint_url must be a non-empty string"),
            ("bucket", True, "bucket must be a non-empty string"),
            ("region", True, "region must be a non-empty string"),
            ("access_key_id", True, "access_key_id must be a non-empty string"),
            ("secret_access_key", True, "secret_access_key must be a non-empty string"),
            ("session_token", True, "session_token must be a string"),
        ]

        for field_name, value, expected_error in cases:
            with self.subTest(field_name=field_name):
                signer = S3PresignedPutSigner(replace(config, **{field_name: value}))
                with self.assertRaisesRegex(ValueError, expected_error):
                    signer.presign_put(
                        object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                        expires_seconds=900,
                        now_ms=0,
                    )

    def test_s3_presigner_rejects_invalid_expires_seconds(self):
        signer = S3PresignedPutSigner(
            S3PresignConfig(
                endpoint_url="https://s3.us-west-2.amazonaws.com",
                bucket="mine-teleop-recordings",
                region="us-west-2",
                access_key_id="AKIDEXAMPLE",
                secret_access_key="SECRET",
            )
        )
        for expires_seconds in (0, -1, True, 1.5):
            with self.subTest(expires_seconds=expires_seconds):
                with self.assertRaisesRegex(ValueError, "expires_seconds must be a positive integer"):
                    signer.presign_put(
                        object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg-1.mp4",
                        expires_seconds=expires_seconds,
                        now_ms=0,
                    )

    def test_s3_presigner_rejects_unsafe_object_path_segments(self):
        signer = S3PresignedPutSigner(
            S3PresignConfig(
                endpoint_url="https://s3.us-west-2.amazonaws.com",
                bucket="mine-teleop-recordings",
                region="us-west-2",
                access_key_id="AKIDEXAMPLE",
                secret_access_key="SECRET",
            )
        )
        cases = [
            "../escape.mp4",
            "vehicles/vehicle-001/sessions/session-001/cameras/../seg-1.mp4",
            "vehicles//sessions/session-001/cameras/front/seg-1.mp4",
            r"vehicles\vehicle-001\sessions\session-001\cameras\front\seg-1.mp4",
        ]
        for object_path in cases:
            with self.subTest(object_path=object_path):
                with self.assertRaisesRegex(ValueError, "object_path must contain safe relative path segments"):
                    signer.presign_put(
                        object_path=object_path,
                        expires_seconds=900,
                        now_ms=0,
                    )

    def test_local_archive_uploader_copies_video_and_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src_video = root / "seg.mp4"
            src_meta = root / "seg.json"
            src_video.write_bytes(b"video")
            src_meta.write_text("{}", encoding="utf-8")
            uploader = LocalArchiveUploader(root / "archive")

            result = uploader.upload(
                segment_id="seg",
                video_path=src_video,
                metadata_path=src_meta,
                object_path="vehicles/vehicle-001/sessions/session-001/cameras/front/seg.mp4",
            )

            self.assertEqual(result.status, "uploaded")
            self.assertTrue((root / "archive" / "vehicles/vehicle-001/sessions/session-001/cameras/front/seg.mp4").exists())
            self.assertTrue((root / "archive" / "vehicles/vehicle-001/sessions/session-001/cameras/front/seg.json").exists())

    def test_local_archive_uploader_rejects_object_paths_outside_archive_root(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            src_video = root / "seg.mp4"
            src_meta = root / "seg.json"
            src_video.write_bytes(b"video")
            src_meta.write_text("{}", encoding="utf-8")
            uploader = LocalArchiveUploader(root / "archive")

            with self.assertRaisesRegex(ValueError, "object_path must stay under archive root"):
                uploader.upload(
                    segment_id="seg",
                    video_path=src_video,
                    metadata_path=src_meta,
                    object_path="../escape.mp4",
                )

            self.assertFalse((root / "escape.mp4").exists())


class SignalingDriverAndClosedLoopTests(unittest.TestCase):
    def test_session_manager_enforces_one_driver_per_vehicle(self):
        manager = SessionManager()
        manager.vehicle_online("vehicle-001")

        session = manager.request_session(vehicle_id="vehicle-001", driver_id="driver-a")

        self.assertEqual(session.state, "SESSION_ACTIVE")
        self.assertTrue(session.control_token.startswith("control-token-"))
        with self.assertRaisesRegex(SessionError, "control authority already granted"):
            manager.request_session(vehicle_id="vehicle-001", driver_id="driver-b")

        manager.end_session(session.session_id)
        new_session = manager.request_session(vehicle_id="vehicle-001", driver_id="driver-b")
        self.assertEqual(new_session.driver_id, "driver-b")

    def test_session_manager_rejects_non_string_session_identity_fields(self):
        manager = SessionManager()

        with self.assertRaisesRegex(ValueError, "vehicle_id must be a non-empty string"):
            manager.vehicle_online(True)
        with self.assertRaisesRegex(ValueError, "vehicle_id must be a non-empty string"):
            manager.vehicle_online("")
        with self.assertRaisesRegex(ValueError, "vehicle_id must be a non-empty string"):
            manager.vehicle_offline(True)
        with self.assertRaisesRegex(ValueError, "vehicle_id must be a non-empty string"):
            manager.request_session(True, "driver-a")

        manager.vehicle_online("vehicle-001")
        session = manager.request_session("vehicle-001", "driver-a")

        with self.assertRaisesRegex(ValueError, "driver_id must be a non-empty string"):
            manager.request_session("vehicle-001", True)
        with self.assertRaisesRegex(ValueError, "driver_id must be a non-empty string"):
            manager.request_session("vehicle-001", "")
        with self.assertRaisesRegex(ValueError, "session_id must be a non-empty string"):
            manager.end_session(True)
        with self.assertRaisesRegex(ValueError, "session_id must be a non-empty string"):
            manager.require_participant("", "driver-a")
        with self.assertRaisesRegex(ValueError, "sender must be a non-empty string"):
            manager.require_participant(session.session_id, True)

    def test_control_receiver_requires_current_session_control_token_when_configured(self):
        receiver = ControlReceiver(
            vehicle_id="vehicle-001",
            session_id="session-001",
            max_command_gap_ms=200,
            control_token="control-token-session-001",
        )
        command = ControlCommand(
            vehicle_id="vehicle-001",
            session_id="session-001",
            seq=1,
            ts_ms=0,
            gear="D",
            steering=0.0,
            throttle=0.1,
            brake=0.0,
        )

        missing = receiver.accept(command, receive_time_ms=0)
        wrong = receiver.accept(command.replace(authority_token="wrong-token"), receive_time_ms=50)
        accepted = receiver.accept(command.replace(authority_token="control-token-session-001"), receive_time_ms=100)

        self.assertFalse(missing.accepted)
        self.assertEqual(missing.reason, "control_token_invalid")
        self.assertFalse(wrong.accepted)
        self.assertEqual(wrong.reason, "control_token_invalid")
        self.assertTrue(accepted.accepted)

    def test_driver_generator_sends_complete_state_at_20hz(self):
        generator = ControlCommandGenerator(
            vehicle_id="vehicle-001",
            session_id="session-001",
            rate_hz=20,
            control_token="control-token-session-001",
        )
        idle = InputState()
        throttle = InputState(throttle_pressed=True, gear="D")

        first = generator.next_command(idle, now_ms=1000)
        self.assertIsNone(generator.next_command(idle, now_ms=1020))
        second = generator.next_command(throttle, now_ms=1050)

        self.assertEqual(first.seq, 1)
        self.assertEqual(second.seq, 2)
        self.assertEqual(second.throttle, 1.0)
        self.assertEqual(second.brake, 0.0)
        self.assertEqual(second.gear, "D")
        self.assertEqual(second.authority_token, "control-token-session-001")

    def test_driver_generator_rejects_unknown_gear_before_sending(self):
        generator = ControlCommandGenerator(vehicle_id="vehicle-001", session_id="session-001", rate_hz=20)

        with self.assertRaisesRegex(ValueError, "gear must be one of"):
            generator.next_command(InputState(gear="crawler"), now_ms=1000)

    def test_driver_generator_rejects_invalid_rate_hz_before_command_loop(self):
        for rate_hz in (0, -20, True):
            with self.subTest(rate_hz=rate_hz):
                with self.assertRaisesRegex(ValueError, "rate_hz must be a positive integer"):
                    ControlCommandGenerator(vehicle_id="vehicle-001", session_id="session-001", rate_hz=rate_hz)

    def test_driver_generator_sends_safe_heartbeat_when_window_loses_focus(self):
        generator = ControlCommandGenerator(vehicle_id="vehicle-001", session_id="session-001", rate_hz=20)
        unfocused_throttle = InputState(
            throttle_pressed=True,
            steering_left=True,
            gear="D",
            window_focused=False,
        )

        first = generator.next_command(unfocused_throttle, now_ms=1000)
        self.assertIsNone(generator.next_command(unfocused_throttle, now_ms=1020))
        second = generator.next_command(unfocused_throttle, now_ms=1050)

        self.assertEqual(first.seq, 1)
        self.assertEqual(second.seq, 2)
        self.assertEqual(second.gear, "D")
        self.assertEqual(second.throttle, 0.0)
        self.assertEqual(second.brake, 0.0)
        self.assertEqual(second.steering, 0.0)

    def test_driver_input_merger_prefers_software_controls_and_keeps_estop_highest_priority(self):
        generator = ControlCommandGenerator(vehicle_id="vehicle-001", session_id="session-001", rate_hz=20)
        merged = DriverInputMerger.merge(
            keyboard=InputState(
                steering_left=True,
                throttle_pressed=True,
                estop_pressed=True,
                gear="D",
            ),
            software=SoftwareControlState(
                steering=0.4,
                throttle=0.6,
                brake=0.25,
                gear="R",
                estop_pressed=False,
            ),
        )

        command = generator.next_command(merged, now_ms=1000)

        self.assertEqual(command.steering, 0.4)
        self.assertEqual(command.throttle, 0.0)
        self.assertEqual(command.brake, 0.25)
        self.assertEqual(command.gear, "R")
        self.assertTrue(command.estop)

    def test_driver_generator_sends_estop_immediately_and_repeats_after_release(self):
        generator = ControlCommandGenerator(vehicle_id="vehicle-001", session_id="session-001", rate_hz=20)

        idle = generator.next_command(InputState(), now_ms=1000)
        urgent = generator.next_command(InputState(estop_pressed=True), now_ms=1010)
        self.assertIsNone(generator.next_command(InputState(), now_ms=1020))
        repeat = generator.next_command(InputState(), now_ms=1060)

        self.assertFalse(idle.estop)
        self.assertIsNotNone(urgent)
        self.assertEqual(urgent.seq, 2)
        self.assertTrue(urgent.estop)
        self.assertEqual(urgent.ts_ms, 1010)
        self.assertTrue(repeat.estop)

    def test_estop_input_guard_requires_long_press_before_generator_sends_estop(self):
        guard = EstopInputGuard(required_hold_ms=500)
        generator = ControlCommandGenerator(vehicle_id="vehicle-001", session_id="session-001", rate_hz=20)

        idle = generator.next_command(InputState(estop_pressed=guard.update(raw_pressed=False, now_ms=1000)), now_ms=1000)
        short_press = guard.update(raw_pressed=True, now_ms=1010)
        too_early = guard.update(raw_pressed=True, now_ms=1490)
        urgent_ready = guard.update(raw_pressed=True, now_ms=1510)
        urgent = generator.next_command(InputState(estop_pressed=urgent_ready), now_ms=1510)
        released = guard.update(raw_pressed=False, now_ms=1520)
        repeat = generator.next_command(InputState(estop_pressed=released), now_ms=1560)

        self.assertFalse(idle.estop)
        self.assertFalse(short_press)
        self.assertFalse(too_early)
        self.assertIsNotNone(urgent)
        self.assertTrue(urgent.estop)
        self.assertEqual(urgent.ts_ms, 1510)
        self.assertTrue(repeat.estop)

    def test_estop_input_guard_rejects_invalid_hold_duration(self):
        for required_hold_ms in (True, "500", 0):
            with self.subTest(required_hold_ms=required_hold_ms):
                with self.assertRaisesRegex(ValueError, "required_hold_ms must be a positive integer"):
                    EstopInputGuard(required_hold_ms=required_hold_ms)

    def test_driver_video_dashboard_isolates_camera_status_and_persists_layout(self):
        dashboard = DriverVideoDashboard(camera_ids=["front", "rear"], layout="grid_4")

        dashboard.update_camera_status(
            "front",
            state="connected",
            fps=30,
            bitrate_kbps=3000,
            latency_ms=95,
        )
        dashboard.update_camera_status(
            "rear",
            state="reconnecting",
            fps=0,
            bitrate_kbps=0,
            latency_ms=None,
            reconnecting=True,
        )
        dashboard.mark_decode_failed("rear", "h264 decoder reset")
        dashboard.focus_camera("front")
        restored = DriverVideoDashboard.from_dict(dashboard.to_dict())

        front = restored.camera_status["front"]
        rear = restored.camera_status["rear"]
        self.assertEqual(restored.layout, "single")
        self.assertEqual(restored.focused_camera_id, "front")
        self.assertEqual(front.state, "connected")
        self.assertEqual(front.fps, 30)
        self.assertEqual(front.bitrate_kbps, 3000)
        self.assertFalse(front.decode_failed)
        self.assertEqual(rear.state, "decode_failed")
        self.assertTrue(rear.reconnecting)
        self.assertTrue(rear.decode_failed)
        self.assertIn("decoder", rear.message)

    def test_driver_video_dashboard_saves_layout_without_runtime_status(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "driver-layout.json"
            dashboard = DriverVideoDashboard(camera_ids=["front", "rear"], layout="grid_4")
            dashboard.update_camera_status(
                "front",
                state="connected",
                fps=30,
                bitrate_kbps=3000,
                latency_ms=95,
            )
            dashboard.focus_camera("front")

            dashboard.save_layout(path)
            restored = DriverVideoDashboard.load_layout(path)
            saved = json.loads(path.read_text(encoding="utf-8"))

        self.assertEqual(
            saved,
            {"camera_ids": ["front", "rear"], "focused_camera_id": "front", "layout": "single"},
        )
        self.assertEqual(restored.layout, "single")
        self.assertEqual(restored.focused_camera_id, "front")
        self.assertEqual(restored.camera_status["front"].state, "disconnected")
        self.assertEqual(restored.camera_status["front"].fps, 0)
        self.assertEqual(restored.camera_status["front"].bitrate_kbps, 0)

    def test_driver_video_dashboard_reports_visible_cameras_for_each_layout(self):
        dashboard = DriverVideoDashboard(camera_ids=["front", "rear", "left", "right"], layout="grid_4")

        self.assertEqual(dashboard.visible_camera_ids(), ["front", "rear", "left", "right"])
        self.assertEqual(dashboard.to_dict()["visible_camera_ids"], ["front", "rear", "left", "right"])

        dashboard.set_layout("grid_2")
        self.assertEqual(dashboard.visible_camera_ids(), ["front", "rear"])

        dashboard.set_layout("grid_1")
        self.assertEqual(dashboard.visible_camera_ids(), ["front"])

        dashboard.focus_camera("left")
        self.assertEqual(dashboard.layout, "single")
        self.assertEqual(dashboard.visible_camera_ids(), ["left"])
        self.assertEqual(dashboard.to_dict()["visible_camera_ids"], ["left"])

    def test_driver_video_dashboard_docs_name_visible_camera_contract(self):
        design = Path("docs/04-driver-console-design.md").read_text(encoding="utf-8")
        readme = Path("README.md").read_text(encoding="utf-8")
        docs = design + "\n" + readme

        self.assertIn("visible_camera_ids", docs)
        self.assertIn("1/2/4", docs)

    def test_driver_console_status_snapshot_maps_telemetry_to_ui_bars(self):
        dashboard = DriverVideoDashboard(camera_ids=["front", "rear"], layout="grid_4")
        dashboard.update_camera_status(
            "front",
            state="connected",
            fps=30,
            bitrate_kbps=3000,
            latency_ms=95,
        )
        dashboard.update_camera_status(
            "rear",
            state="reconnecting",
            fps=0,
            bitrate_kbps=0,
            latency_ms=None,
            reconnecting=True,
        )
        telemetry = {
            "session_id": "session-001",
            "speed_mps": 2.5,
            "gear": "D",
            "steering_feedback": 0.1,
            "throttle_feedback": 0.2,
            "brake_feedback": 0.0,
            "estop": False,
            "fault_flags": ["video.rear.websocket_closed"],
            "mock_telemetry": True,
            "link": {"control_rtt_ms": 67, "signaling_connected": False},
            "vehicle_adapter": {
                "adapter_type": "dynamic_library",
                "opened": True,
                "healthy": False,
                "can_interface": "can0",
                "library_path": "/tmp/libmine_teleop_chassis_bridge.so",
                "last_error": "mine_teleop_chassis_open failed with code -3",
            },
        }

        snapshot = DriverConsoleStatusSnapshot.from_telemetry(
            telemetry=telemetry,
            dashboard=dashboard,
            control_authority_state="active",
            packet_loss_percent=1.5,
        ).to_dict()

        self.assertEqual(snapshot["side_bar"]["speed_mps"], 2.5)
        self.assertEqual(snapshot["side_bar"]["gear"], "D")
        self.assertEqual(snapshot["side_bar"]["steering_feedback"], 0.1)
        self.assertEqual(snapshot["side_bar"]["throttle_feedback"], 0.2)
        self.assertEqual(snapshot["side_bar"]["brake_feedback"], 0.0)
        self.assertFalse(snapshot["side_bar"]["estop"])
        self.assertEqual(snapshot["side_bar"]["fault_flags"], ["video.rear.websocket_closed"])
        self.assertTrue(snapshot["side_bar"]["mock_telemetry"])
        self.assertEqual(snapshot["side_bar"]["telemetry_source_label"], "MOCK TELEMETRY")
        self.assertEqual(snapshot["side_bar"]["control_connection_state"], "signaling_disconnected")
        self.assertEqual(
            snapshot["side_bar"]["vehicle_adapter"],
            {
                "adapter_type": "dynamic_library",
                "opened": True,
                "healthy": False,
                "can_interface": "can0",
                "last_error": "mine_teleop_chassis_open failed with code -3",
            },
        )
        self.assertEqual(
            snapshot["side_bar"]["video_connection_state_by_camera"],
            {"front": "connected", "rear": "reconnecting"},
        )
        self.assertEqual(snapshot["bottom_bar"]["rtt_ms"], 67)
        self.assertEqual(snapshot["bottom_bar"]["packet_loss_percent"], 1.5)
        self.assertEqual(snapshot["bottom_bar"]["bitrate_kbps_by_camera"], {"front": 3000, "rear": 0})
        self.assertEqual(snapshot["bottom_bar"]["fps_by_camera"], {"front": 30, "rear": 0})
        self.assertIn("latency_ms_by_camera", snapshot["bottom_bar"])
        self.assertEqual(snapshot["bottom_bar"]["latency_ms_by_camera"], {"front": 95, "rear": None})
        self.assertEqual(snapshot["bottom_bar"]["session_id"], "session-001")
        self.assertEqual(snapshot["bottom_bar"]["control_authority_state"], "active")

    def test_driver_toolbar_snapshot_exposes_required_actions_and_persistent_estop(self):
        logged_out = DriverToolbarSnapshot.from_state(
            logged_in=False,
            connected=False,
            session_active=False,
        ).to_dict()
        active = DriverToolbarSnapshot.from_state(
            logged_in=True,
            connected=True,
            session_active=True,
        ).to_dict()

        self.assertEqual(
            set(logged_out["actions"]),
            {"login", "logout", "connect", "disconnect", "start_session", "end_session", "estop", "settings"},
        )
        self.assertTrue(logged_out["actions"]["login"]["enabled"])
        self.assertFalse(logged_out["actions"]["logout"]["enabled"])
        self.assertFalse(logged_out["actions"]["connect"]["enabled"])
        self.assertTrue(logged_out["actions"]["estop"]["visible"])
        self.assertFalse(logged_out["actions"]["estop"]["enabled"])
        self.assertTrue(logged_out["actions"]["settings"]["enabled"])
        self.assertFalse(active["actions"]["login"]["enabled"])
        self.assertTrue(active["actions"]["logout"]["enabled"])
        self.assertFalse(active["actions"]["connect"]["enabled"])
        self.assertTrue(active["actions"]["disconnect"]["enabled"])
        self.assertFalse(active["actions"]["start_session"]["enabled"])
        self.assertTrue(active["actions"]["end_session"]["enabled"])
        self.assertTrue(active["actions"]["estop"]["visible"])
        self.assertTrue(active["actions"]["estop"]["enabled"])

    def test_driver_operation_log_persists_required_local_events_as_json_lines(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "driver-ops.jsonl"
            log = DriverOperationLog(path)

            log.append(
                DriverOperationEvent(
                    ts_ms=1000,
                    event="login_user",
                    driver_id="driver-console-001",
                    vehicle_id="",
                    session_id="",
                    ui_version="dev-ui",
                    config_version="dev-config",
                    details={"result": "success"},
                )
            )
            log.append(
                DriverOperationEvent(
                    ts_ms=1050,
                    event="control_authority_acquired",
                    driver_id="driver-console-001",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    ui_version="dev-ui",
                    config_version="dev-config",
                    details={"authority": "active"},
                )
            )
            log.append(
                DriverOperationEvent(
                    ts_ms=1100,
                    event="estop_sent",
                    driver_id="driver-console-001",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    ui_version="dev-ui",
                    config_version="dev-config",
                    details={"redundant_send": 1},
                )
            )

            records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

        self.assertEqual([record["event"] for record in records], ["login_user", "control_authority_acquired", "estop_sent"])
        for record in records:
            self.assertIn("ts_ms", record)
            self.assertEqual(record["driver_id"], "driver-console-001")
            self.assertIn("vehicle_id", record)
            self.assertIn("session_id", record)
            self.assertEqual(record["ui_version"], "dev-ui")
            self.assertEqual(record["config_version"], "dev-config")
            self.assertIsInstance(record["details"], dict)

    def test_driver_operation_log_rotates_json_lines_by_size_with_numbered_backups(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            path = Path(tmpdir) / "driver-ops.jsonl"
            try:
                log = DriverOperationLog(path, max_bytes=1, backup_count=2)
            except TypeError as exc:
                self.fail(f"DriverOperationLog should accept rotation options: {exc}")

            log.append(
                DriverOperationEvent(
                    ts_ms=1000,
                    event="login_user",
                    driver_id="driver-console-001",
                    ui_version="dev-ui",
                    config_version="dev-config",
                )
            )
            log.append(
                DriverOperationEvent(
                    ts_ms=1050,
                    event="connection_opened",
                    driver_id="driver-console-001",
                    vehicle_id="vehicle-001",
                    ui_version="dev-ui",
                    config_version="dev-config",
                )
            )
            log.append(
                DriverOperationEvent(
                    ts_ms=1100,
                    event="connection_reconnected",
                    driver_id="driver-console-001",
                    vehicle_id="vehicle-001",
                    ui_version="dev-ui",
                    config_version="dev-config",
                    details={"attempt": 1},
                )
            )

            current = json.loads(path.read_text(encoding="utf-8"))
            first_backup = json.loads(path.with_name("driver-ops.jsonl.1").read_text(encoding="utf-8"))
            second_backup = json.loads(path.with_name("driver-ops.jsonl.2").read_text(encoding="utf-8"))

        self.assertEqual(current["event"], "connection_reconnected")
        self.assertEqual(first_backup["event"], "connection_opened")
        self.assertEqual(second_backup["event"], "login_user")

    def test_mock_closed_loop_reaches_20hz_then_times_out_and_preserves_estop_latch(self):
        result = run_mock_closed_loop(
            duration_ms=1500,
            disconnect_at_ms=500,
            estop_at_ms=300,
            step_ms=50,
        )

        self.assertEqual(result.commands_applied_before_disconnect, 10)
        self.assertIn(SafetyState.CONTROL_ACTIVE, result.safety_states)
        self.assertIn(SafetyState.ESTOP, result.safety_states)
        self.assertNotIn(SafetyState.STANDBY, result.safety_states_after_estop)
        self.assertEqual(result.final_state, SafetyState.ESTOP)


class VehicleControlServiceTests(unittest.TestCase):
    def test_service_applies_20hz_commands_emits_telemetry_and_times_out_after_disconnect(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        adapter = MockVehicleAdapter()
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=100,
        )
        service.start(now_ms=0)

        for now_ms in range(0, 1501, 50):
            if now_ms < 500:
                seq = now_ms // 50 + 1
                service.receive_command(
                    ControlCommand(
                        vehicle_id="vehicle-001",
                        session_id="session-001",
                        seq=seq,
                        ts_ms=now_ms,
                        gear="D",
                        steering=0.0,
                        throttle=0.25,
                        brake=0.0,
                    ),
                    now_ms=now_ms,
                )
            service.tick(now_ms)

        telemetry_states = [item["safety_state"] for item in service.telemetry_history]
        self.assertEqual(len(adapter.applied_commands), 10)
        self.assertIn("CONTROL_ACTIVE", telemetry_states)
        self.assertIn("DEGRADED", telemetry_states)
        self.assertIn("TIMEOUT_BRAKE", telemetry_states)
        self.assertEqual(service.safety.state, SafetyState.TIMEOUT_BRAKE)
        self.assertGreater(len(adapter.safe_outputs), 0)
        self.assertEqual(adapter.safe_outputs[-1].throttle, 0.0)
        timeout_telemetry = [item for item in service.telemetry_history if item["safety_state"] == "TIMEOUT_BRAKE"][-1]
        self.assertEqual(timeout_telemetry["throttle_feedback"], 0.0)
        self.assertEqual(timeout_telemetry["brake_feedback"], adapter.safe_outputs[-1].brake)
        self.assertEqual(
            timeout_telemetry["vehicle_adapter"],
            {
                "adapter_type": "mock",
                "opened": True,
                "healthy": True,
                "applied_command_count": len(adapter.applied_commands),
                "safe_stop_count": len(adapter.safe_outputs),
            },
        )

    def test_service_keeps_local_control_active_when_cloud_signaling_drops(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        adapter = MockVehicleAdapter()
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )
        service.start(now_ms=0)
        service.receive_command(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=1,
                ts_ms=0,
                gear="D",
                steering=0.0,
                throttle=0.2,
                brake=0.0,
            ),
            now_ms=0,
        )
        service.tick(0)

        service.set_signaling_connected(False, now_ms=100)
        service.tick(100)

        self.assertEqual(service.safety.state, SafetyState.CONTROL_ACTIVE)
        self.assertEqual(adapter.safe_outputs, [])
        self.assertFalse(service.telemetry_history[-1]["link"]["signaling_connected"])

    def test_service_from_config_creates_mock_adapter_but_refuses_unimplemented_real_adapter(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            telemetry_interval_ms=50,
        )
        can_config = replace(config, vehicle_adapter_type="can")

        self.assertIsInstance(service.adapter, MockVehicleAdapter)
        with self.assertRaisesRegex(VehicleAdapterError, "vehicle_adapter.type can is not implemented"):
            VehicleControlService.from_config(
                can_config,
                session_id="session-001",
                telemetry_interval_ms=50,
            )

    def test_service_counts_applied_commands_from_adapter_status_contract(self):
        class StatusOnlyAdapter:
            def __init__(self):
                self.opened = False
                self.applied_count = 0

            def open(self):
                self.opened = True

            def apply_control(self, command):
                self.applied_count += 1

            def apply_safe_stop(self, output):
                pass

            def get_status(self):
                return VehicleAdapterStatus(
                    adapter_type="dynamic_library",
                    opened=self.opened,
                    healthy=True,
                    can_interface="can0",
                    library_path="/opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so",
                    applied_command_count=self.applied_count,
                )

        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        adapter = StatusOnlyAdapter()
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )

        service.start(now_ms=0)
        service.receive_command(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=1,
                ts_ms=0,
                gear="D",
                steering=0.0,
                throttle=0.2,
                brake=0.0,
            ),
            now_ms=0,
        )

        self.assertEqual(service.applied_command_count, 1)

    def test_service_latches_estop_and_does_not_apply_followup_drive_command(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        adapter = MockVehicleAdapter()
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )
        service.start(now_ms=0)
        service.receive_command(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=1,
                ts_ms=0,
                gear="D",
                steering=0.0,
                throttle=0.0,
                brake=0.0,
                estop=True,
            ),
            now_ms=0,
        )
        service.tick(0)
        service.receive_command(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=2,
                ts_ms=50,
                gear="D",
                steering=0.0,
                throttle=0.5,
                brake=0.0,
            ),
            now_ms=50,
        )
        service.tick(50)

        self.assertEqual(service.safety.state, SafetyState.ESTOP)
        self.assertEqual(adapter.applied_commands, [])
        self.assertTrue(adapter.safe_outputs[-1].estop)

    def test_service_reset_estop_requires_local_confirmation_and_audits_actor(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            audit_path = Path(tmp) / "control-audit.jsonl"
            adapter = MockVehicleAdapter()
            service = VehicleControlService.from_config(
                config,
                session_id="session-001",
                adapter=adapter,
                telemetry_interval_ms=50,
                audit_log=AuditLog(audit_path),
            )
            service.start(now_ms=0)
            service.receive_command(
                ControlCommand(
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    seq=1,
                    ts_ms=0,
                    gear="D",
                    steering=0.0,
                    throttle=0.0,
                    brake=0.0,
                    estop=True,
                ),
                now_ms=0,
            )

            rejected = service.reset_estop(local_confirmed=False, authorized_by="driver-ui", now_ms=100)
            accepted = service.reset_estop(local_confirmed=True, authorized_by="safety-officer", now_ms=150)
            service.tick(now_ms=150)
            records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]

        self.assertFalse(rejected)
        self.assertTrue(accepted)
        self.assertEqual(service.safety.state, SafetyState.STANDBY)
        self.assertFalse(adapter.safe_outputs[-1].estop)
        self.assertEqual(service.telemetry_history[-1]["safety_state"], "STANDBY")
        self.assertFalse(service.telemetry_history[-1]["estop"])
        reset = next(record for record in records if record["event"] == "estop_reset")
        self.assertEqual(reset["actor"], "safety-officer")
        self.assertTrue(reset["details"]["local_confirmed"])
        self.assertEqual(reset["details"]["latched_at_ms"], 0)

    def test_service_rejects_control_command_without_current_session_token(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        adapter = MockVehicleAdapter()
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            control_token="control-token-session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )
        service.start(now_ms=0)

        rejected = service.receive_command(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=1,
                ts_ms=0,
                gear="D",
                steering=0.0,
                throttle=0.5,
                brake=0.0,
            ),
            now_ms=0,
        )
        accepted = service.receive_command(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=1,
                ts_ms=50,
                gear="D",
                steering=0.0,
                throttle=0.5,
                brake=0.0,
                authority_token="control-token-session-001",
            ),
            now_ms=50,
        )

        self.assertFalse(rejected.accepted)
        self.assertEqual(rejected.reason, "control_token_invalid")
        self.assertTrue(accepted.accepted)
        self.assertEqual(len(adapter.applied_commands), 1)

    def test_service_audits_driver_timestamp_skew_warning_without_rejecting_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            audit_path = Path(tmp) / "control-audit.jsonl"
            adapter = MockVehicleAdapter()
            service = VehicleControlService.from_config(
                config,
                session_id="session-001",
                adapter=adapter,
                telemetry_interval_ms=50,
                audit_log=AuditLog(audit_path),
            )
            service.start(now_ms=0)

            result = service.receive_command(
                ControlCommand(
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    seq=1,
                    ts_ms=-60_000,
                    gear="D",
                    steering=0.0,
                    throttle=0.5,
                    brake=0.0,
                ),
                now_ms=1_000,
            )
            self.assertTrue(audit_path.exists())
            records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]

        self.assertTrue(result.accepted)
        self.assertEqual(result.warnings, ("driver_timestamp_skew",))
        self.assertEqual(len(adapter.applied_commands), 1)
        warning = next(record for record in records if record["event"] == "control_timestamp_warning")
        self.assertEqual(warning["actor"], "vehicle-control-agent")
        self.assertEqual(warning["details"]["warning"], "driver_timestamp_skew")
        self.assertEqual(warning["details"]["seq"], 1)
        self.assertEqual(warning["details"]["command_ts_ms"], -60_000)
        self.assertEqual(warning["details"]["receive_time_ms"], 1_000)

    def test_service_audits_control_timeout_and_estop_latch_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            audit_path = Path(tmp) / "control-audit.jsonl"
            adapter = MockVehicleAdapter()
            service = VehicleControlService.from_config(
                config,
                session_id="session-001",
                adapter=adapter,
                telemetry_interval_ms=50,
                audit_log=AuditLog(audit_path),
            )
            service.start(now_ms=0)
            service.receive_command(
                ControlCommand(
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    seq=1,
                    ts_ms=0,
                    gear="D",
                    steering=0.0,
                    throttle=0.5,
                    brake=0.0,
                ),
                now_ms=0,
            )

            service.tick(900)
            service.tick(950)
            service.receive_command(
                ControlCommand(
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    seq=2,
                    ts_ms=960,
                    gear="D",
                    steering=0.0,
                    throttle=0.0,
                    brake=0.0,
                    estop=True,
                ),
                now_ms=960,
            )

            records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]

        events = [record["event"] for record in records]
        self.assertEqual(events.count("control_timeout"), 1)
        self.assertEqual(events.count("estop_latched"), 1)
        timeout = next(record for record in records if record["event"] == "control_timeout")
        estop = next(record for record in records if record["event"] == "estop_latched")
        self.assertEqual(timeout["vehicle_id"], "vehicle-001")
        self.assertEqual(timeout["session_id"], "session-001")
        self.assertEqual(timeout["details"]["state"], "TIMEOUT_BRAKE")
        self.assertEqual(estop["details"]["seq"], 2)

    def test_service_drains_only_latest_command_from_bounded_media_ipc(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        adapter = MockVehicleAdapter()
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )
        service.start(now_ms=0)
        mailbox = LatestControlCommandMailbox()

        for seq in (1, 2, 3):
            mailbox.publish(
                ControlCommand(
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    seq=seq,
                    ts_ms=seq * 50,
                    gear="D",
                    steering=0.0,
                    throttle=seq / 10,
                    brake=0.0,
                )
            )

        result = service.drain_command_mailbox(mailbox, now_ms=150)

        self.assertEqual(mailbox.dropped_count, 2)
        self.assertEqual(mailbox.pending_count, 0)
        self.assertTrue(result.accepted)
        self.assertEqual(result.command.seq, 3)
        self.assertEqual(len(adapter.applied_commands), 1)
        self.assertEqual(adapter.applied_commands[0].seq, 3)


class SignalingHttpServiceTests(unittest.TestCase):
    def test_http_driver_login_can_use_configured_password_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(
                audit_log_path=audit_path,
                driver_credentials=DriverCredentialStore.from_passwords({"driver-a": "driver-secret"}),
            )
            with service.running() as base_url:
                wrong = _json_post_expect_error(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                    expected_status=401,
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "driver-secret"},
                )

            self.assertEqual(wrong["error"], "invalid driver credentials")
            self.assertEqual(login["token_type"], "bearer")
            self.assertTrue(login["token"].startswith("driver-token-"))
            self.assertNotIn("driver-a", login["token"])

    def test_http_vehicle_online_can_use_configured_device_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(
                audit_log_path=audit_path,
                device_credentials=DeviceCredentialStore.from_tokens({"vehicle-002": "vehicle-002-secret"}),
            )
            with service.running() as base_url:
                wrong = _json_post_expect_error(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "dev-device-secret"},
                    expected_status=401,
                )
                online = _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "vehicle-002-secret"},
                )

            self.assertEqual(wrong["error"], "invalid device token")
            self.assertEqual(online["vehicle_id"], "vehicle-002")
            self.assertEqual(online["state"], "online")

    def test_driver_tokens_expire_after_configured_ttl(self):
        now_ms = [1000]
        tokens = DriverTokenStore(token_ttl_ms=50, clock_ms=lambda: now_ms[0])

        token = tokens.login("driver-a", "dev-password").token
        tokens.validate("driver-a", token)
        now_ms[0] = 1049
        tokens.validate("driver-a", token)
        now_ms[0] = 1050

        with self.assertRaisesRegex(PermissionError, "driver token expired"):
            tokens.validate("driver-a", token)

    def test_identity_stores_reject_non_string_ids_tokens_and_boolean_ttl(self):
        with self.assertRaisesRegex(ValueError, "token_ttl_ms"):
            DriverTokenStore(token_ttl_ms=True)

        tokens = DriverTokenStore(clock_ms=lambda: 1000)
        with self.assertRaisesRegex(ValueError, "driver_id must be a non-empty string"):
            tokens.login(True, "dev-password")
        token = tokens.login("driver-a", "dev-password").token
        with self.assertRaisesRegex(ValueError, "driver_id must be a non-empty string"):
            tokens.validate(True, token)
        with self.assertRaisesRegex(ValueError, "driver token must be a string"):
            tokens.validate("driver-a", True)

        devices = DeviceCredentialStore()
        with self.assertRaisesRegex(ValueError, "vehicle_id must be a non-empty string"):
            devices.register(True, "dev-device-secret")
        with self.assertRaisesRegex(ValueError, "device_token must be a non-empty string"):
            devices.register("vehicle-001", True)
        devices.register("vehicle-001", "dev-device-secret")
        with self.assertRaisesRegex(ValueError, "vehicle_id must be a non-empty string"):
            devices.validate(True, "dev-device-secret")
        with self.assertRaisesRegex(ValueError, "device_token must be a string"):
            devices.validate("vehicle-001", True)

        malformed_credentials = DriverCredentialStore(
            {
                "driver-a": DriverPasswordCredential(
                    algorithm="pbkdf2_sha256",
                    iterations="10",
                    salt="not-valid-base64",
                    digest="not-valid-base64",
                )
            }
        )
        with self.assertRaisesRegex(PermissionError, "invalid driver credentials"):
            malformed_credentials.validate("driver-a", "driver-secret")

    def test_http_identity_endpoints_reject_non_string_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.devices.register("True", "dev-device-secret")
            service.sessions.vehicle_online("True")
            service.sessions.vehicle_online("vehicle-001")
            bool_driver_token = service.tokens.login("True", "dev-password").token
            with service.running() as base_url:
                cases = [
                    (
                        "/auth/driver_login",
                        {"driver_id": True, "password": "dev-password"},
                        "driver_id must be a string",
                    ),
                    (
                        "/auth/driver_login",
                        {"driver_id": "driver-a", "password": True},
                        "password must be a string",
                    ),
                    (
                        "/vehicles/online",
                        {"vehicle_id": True, "device_token": "dev-device-secret"},
                        "vehicle_id must be a string",
                    ),
                    (
                        "/sessions",
                        {"vehicle_id": True, "driver_id": "True", "token": bool_driver_token},
                        "vehicle_id must be a string",
                    ),
                    (
                        "/sessions",
                        {"vehicle_id": "vehicle-001", "driver_id": True, "token": bool_driver_token},
                        "driver_id must be a string",
                    ),
                    (
                        "/sessions",
                        {"vehicle_id": "vehicle-001", "driver_id": "True", "token": True},
                        "token must be a string",
                    ),
                ]
                for path, payload, message in cases:
                    with self.subTest(path=path, message=message):
                        rejected = _json_post_expect_error(f"{base_url}{path}", payload, expected_status=400)
                        self.assertEqual(rejected["error"], message)

    def test_http_service_handles_login_session_signaling_and_audit_log(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                self.assertEqual(_json_get(f"{base_url}/health")["status"], "ok")

                vehicle = _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                self.assertEqual(vehicle["state"], "online")

                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                self.assertEqual(login["token_type"], "bearer")
                self.assertTrue(login["token"].startswith("driver-token-"))
                self.assertGreater(login["expires_at_ms"], 0)

                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                self.assertEqual(session["state"], "SESSION_ACTIVE")
                self.assertTrue(session["control_token"].startswith("control-token-"))
                session_id = session["session_id"]

                rejected_unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-c", "token": "bad-token"},
                    expected_status=401,
                )
                self.assertEqual(rejected_unauthenticated["error"], "invalid driver token")

                login_b = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-b", "password": "dev-password"},
                )
                rejected = _json_post_expect_error(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-b", "token": login_b["token"]},
                    expected_status=409,
                )
                self.assertEqual(rejected["error"], "control authority already granted")

                unauthorized_offer = _json_post_expect_error(
                    f"{base_url}/signaling/{session_id}/messages",
                    {
                        "sender": "driver-b",
                        "token": login_b["token"],
                        "recipient": "vehicle-001",
                        "type": "webrtc_offer",
                        "payload": {"sdp": "v=0"},
                    },
                    expected_status=403,
                )
                self.assertEqual(unauthorized_offer["error"], "sender is not current session participant")

                unauthorized_poll = _json_get_expect_error(
                    f"{base_url}/signaling/{session_id}/messages?recipient=driver-b&token={login_b['token']}",
                    expected_status=403,
                )
                self.assertEqual(unauthorized_poll["error"], "sender is not current session participant")

                unauthorized_ws = _json_get_expect_error(
                    f"{base_url}/signaling/{session_id}/ws?participant=driver-b&token={login_b['token']}",
                    expected_status=403,
                )
                self.assertEqual(unauthorized_ws["error"], "sender is not current session participant")

                unauthenticated_offer = _json_post_expect_error(
                    f"{base_url}/signaling/{session_id}/messages",
                    {
                        "sender": "driver-a",
                        "recipient": "vehicle-001",
                        "type": "webrtc_offer",
                        "payload": {"sdp": "v=0"},
                    },
                    expected_status=401,
                )
                self.assertEqual(unauthenticated_offer["error"], "invalid driver token")

                unauthenticated_poll = _json_get_expect_error(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001",
                    expected_status=401,
                )
                self.assertEqual(unauthenticated_poll["error"], "invalid device token")

                unauthenticated_ws = _json_get_expect_error(
                    f"{base_url}/signaling/{session_id}/ws?participant=driver-a",
                    expected_status=401,
                )
                self.assertEqual(unauthenticated_ws["error"], "invalid driver token")

                offer = _json_post(
                    f"{base_url}/signaling/{session_id}/messages",
                    {
                        "sender": "driver-a",
                        "token": login["token"],
                        "recipient": "vehicle-001",
                        "type": "webrtc_offer",
                        "payload": {"sdp": "v=0"},
                    },
                )
                self.assertEqual(offer["queued"], 1)

                messages = _json_get(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                )
                self.assertEqual(messages["messages"][0]["type"], "webrtc_offer")
                self.assertEqual(messages["messages"][0]["payload"]["sdp"], "v=0")
                self.assertEqual(
                    _json_get(
                        f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                    )["messages"],
                    [],
                )

                unauthenticated_end = _json_post_expect_error(
                    f"{base_url}/sessions/{session_id}/end",
                    {"actor": "driver-a"},
                    expected_status=401,
                )
                self.assertEqual(unauthenticated_end["error"], "invalid driver token")

                ended = _json_post(
                    f"{base_url}/sessions/{session_id}/end",
                    {"actor": "driver-a", "token": login["token"]},
                )
                self.assertEqual(ended["state"], "ENDED")

            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            audit_events = [record["event"] for record in audit_records]
            self.assertTrue(all(record["ts_ms"] > 0 for record in audit_records))
            self.assertIn("vehicle_online", audit_events)
            self.assertIn("driver_login", audit_events)
            self.assertIn("session_started", audit_events)
            self.assertIn("control_authority_granted", audit_events)
            self.assertIn("webrtc_offer", audit_events)
            self.assertIn("control_authority_revoked", audit_events)
            self.assertIn("session_ended", audit_events)
            granted = next(record for record in audit_records if record["event"] == "control_authority_granted")
            revoked = next(record for record in audit_records if record["event"] == "control_authority_revoked")
            self.assertEqual(granted["actor"], "driver-a")
            self.assertEqual(granted["vehicle_id"], "vehicle-001")
            self.assertEqual(granted["session_id"], session_id)
            self.assertEqual(revoked["actor"], "driver-a")
            self.assertEqual(revoked["details"]["reason"], "session_end")

    def test_http_service_rejects_non_string_session_end_actor(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "True", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "True", "token": login["token"]},
                )

                rejected = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/end",
                    {"actor": True, "token": login["token"]},
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "actor must be a string")
            self.assertEqual(service.sessions.sessions[session["session_id"]].state, "SESSION_ACTIVE")

    def test_http_service_rejects_non_string_session_end_device_token(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.devices.register(vehicle_id="vehicle-002", device_token="True")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "True"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-002", "driver_id": "driver-a", "token": login["token"]},
                )

                rejected = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/end",
                    {"actor": "vehicle-002", "device_token": True},
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "device_token must be a string")
            self.assertEqual(service.sessions.sessions[session["session_id"]].state, "SESSION_ACTIVE")

    def test_http_service_can_revoke_control_authority_without_session_end_endpoint(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login_a = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login_a["token"]},
                )

                unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/control_authority/revoke",
                    {"actor": "driver-a", "reason": "operator_takeover"},
                    expected_status=401,
                )
                revoked = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/control_authority/revoke",
                    {"actor": "driver-a", "token": login_a["token"], "reason": "operator_takeover"},
                )
                login_b = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-b", "password": "dev-password"},
                )
                replacement = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-b", "token": login_b["token"]},
                )

            self.assertEqual(unauthenticated["error"], "invalid driver token")
            self.assertEqual(revoked["state"], "ENDED")
            self.assertEqual(replacement["state"], "SESSION_ACTIVE")
            self.assertEqual(replacement["driver_id"], "driver-b")
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            revoked_record = next(
                record
                for record in audit_records
                if record["event"] == "control_authority_revoked"
                and record["session_id"] == session["session_id"]
                and record["details"]["reason"] == "operator_takeover"
            )
            self.assertEqual(revoked_record["actor"], "driver-a")
            self.assertEqual(revoked_record["vehicle_id"], "vehicle-001")

    def test_http_service_rejects_expired_driver_token_when_starting_session(self):
        now_ms = [1000]
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(
                audit_log_path=audit_path,
                driver_token_ttl_ms=50,
                clock_ms=lambda: now_ms[0],
            )
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                now_ms[0] = 1051

                rejected = _json_post_expect_error(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                    expected_status=401,
                )

            self.assertEqual(rejected["error"], "driver token expired")

    def test_http_service_audits_abnormal_disconnect(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/abnormal_disconnect",
                    {"actor": "driver-a", "reason": "websocket_closed", "detected_by": "signaling-server"},
                    expected_status=401,
                )
                recorded = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/abnormal_disconnect",
                    {
                        "actor": "driver-a",
                        "token": login["token"],
                        "reason": "websocket_closed",
                        "detected_by": "signaling-server",
                    },
                )

            self.assertEqual(unauthenticated["error"], "invalid driver token")
            self.assertEqual(recorded["event"], "abnormal_disconnect")
            self.assertEqual(recorded["session_id"], session["session_id"])
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            disconnect = next(record for record in audit_records if record["event"] == "abnormal_disconnect")
            self.assertEqual(disconnect["vehicle_id"], "vehicle-001")
            self.assertEqual(disconnect["session_id"], session["session_id"])
            self.assertEqual(disconnect["actor"], "driver-a")
            self.assertEqual(disconnect["details"]["reason"], "websocket_closed")
            self.assertEqual(disconnect["details"]["detected_by"], "signaling-server")

    def test_http_service_records_realtime_diagnostics_with_current_participant_credential(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/diagnostics",
                    {
                        "actor": "driver-a",
                        "component": "driver-console",
                        "rtt_ms": 84,
                        "packet_loss_percent": 1.5,
                        "jitter_ms": 12,
                        "video_latency_ms": 110,
                        "control_send_hz": 20.0,
                    },
                    expected_status=401,
                )
                recorded = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/diagnostics",
                    {
                        "actor": "driver-a",
                        "token": login["token"],
                        "component": "driver-console",
                        "rtt_ms": 84,
                        "packet_loss_percent": 1.5,
                        "jitter_ms": 12,
                        "video_latency_ms": 110,
                        "control_send_hz": 20.0,
                    },
                )

            self.assertEqual(unauthenticated["error"], "invalid driver token")
            self.assertEqual(recorded["event"], "realtime_diagnostics")
            self.assertEqual(recorded["session_id"], session["session_id"])
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            diagnostics = next(record for record in audit_records if record["event"] == "realtime_diagnostics")
            self.assertEqual(diagnostics["vehicle_id"], "vehicle-001")
            self.assertEqual(diagnostics["session_id"], session["session_id"])
            self.assertEqual(diagnostics["actor"], "driver-a")
            self.assertEqual(diagnostics["details"]["component"], "driver-console")
            self.assertEqual(diagnostics["details"]["rtt_ms"], 84)
            self.assertEqual(diagnostics["details"]["packet_loss_percent"], 1.5)
            self.assertEqual(diagnostics["details"]["jitter_ms"], 12)
            self.assertEqual(diagnostics["details"]["video_latency_ms"], 110)
            self.assertEqual(diagnostics["details"]["control_send_hz"], 20.0)

    def test_http_service_audits_control_timeout_with_current_participant_credential(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/control_timeout",
                    {
                        "actor": "vehicle-001",
                        "last_valid_receive_ms": 1000,
                        "timeout_entered_ms": 1800,
                        "control_timeout_ms": 800,
                    },
                    expected_status=401,
                )
                recorded = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/control_timeout",
                    {
                        "actor": "vehicle-001",
                        "device_token": "dev-device-secret",
                        "last_valid_receive_ms": 1000,
                        "timeout_entered_ms": 1800,
                        "control_timeout_ms": 800,
                    },
                )

            self.assertEqual(unauthenticated["error"], "invalid device token")
            self.assertEqual(recorded["event"], "control_timeout")
            self.assertEqual(recorded["session_id"], session["session_id"])
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            timeout = next(record for record in audit_records if record["event"] == "control_timeout")
            self.assertEqual(timeout["vehicle_id"], "vehicle-001")
            self.assertEqual(timeout["session_id"], session["session_id"])
            self.assertEqual(timeout["actor"], "vehicle-001")
            self.assertEqual(timeout["details"]["last_valid_receive_ms"], 1000)
            self.assertEqual(timeout["details"]["timeout_entered_ms"], 1800)
            self.assertEqual(timeout["details"]["control_timeout_ms"], 800)

    def test_http_service_audits_estop_with_current_participant_credential(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/estop",
                    {"actor": "driver-a", "reason": "driver_estop", "seq": 42},
                    expected_status=401,
                )
                recorded = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/estop",
                    {
                        "actor": "driver-a",
                        "token": login["token"],
                        "reason": "driver_estop",
                        "seq": 42,
                    },
                )

            self.assertEqual(unauthenticated["error"], "invalid driver token")
            self.assertEqual(recorded["event"], "estop")
            self.assertEqual(recorded["session_id"], session["session_id"])
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            estop = next(record for record in audit_records if record["event"] == "estop")
            self.assertEqual(estop["vehicle_id"], "vehicle-001")
            self.assertEqual(estop["session_id"], session["session_id"])
            self.assertEqual(estop["actor"], "driver-a")
            self.assertEqual(estop["details"]["reason"], "driver_estop")
            self.assertEqual(estop["details"]["seq"], 42)

    def test_service_audit_methods_reject_direct_wrong_scalar_types(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.sessions.vehicle_online("vehicle-001")
            session = service.sessions.request_session("vehicle-001", "driver-a")
            token = service.tokens.login("driver-a", "dev-password").token
            cases = [
                (
                    lambda: service.record_realtime_diagnostics(
                        session.session_id,
                        "driver-a",
                        "driver-console",
                        rtt_ms=True,
                        packet_loss_percent=1.5,
                        jitter_ms=12,
                        video_latency_ms=110,
                        control_send_hz=20.0,
                        token=token,
                    ),
                    "rtt_ms must be a non-negative integer",
                ),
                (
                    lambda: service.record_realtime_diagnostics(
                        session.session_id,
                        "driver-a",
                        "driver-console",
                        rtt_ms=84,
                        packet_loss_percent="1.5",
                        jitter_ms=12,
                        video_latency_ms=110,
                        control_send_hz=20.0,
                        token=token,
                    ),
                    "packet_loss_percent must be a non-negative number",
                ),
                (
                    lambda: service.record_control_timeout(
                        session.session_id,
                        "vehicle-001",
                        last_valid_receive_ms="1000",
                        timeout_entered_ms=1800,
                        control_timeout_ms=800,
                        device_token="dev-device-secret",
                    ),
                    "last_valid_receive_ms must be a non-negative integer",
                ),
                (
                    lambda: service.record_estop(
                        session.session_id,
                        "driver-a",
                        reason="driver_estop",
                        seq="42",
                        token=token,
                    ),
                    "seq must be a non-negative integer",
                ),
                (
                    lambda: service.record_turn_relay_usage(
                        session.session_id,
                        "vehicle-001",
                        bytes_sent=True,
                        bytes_received=30000,
                        duration_ms=1000,
                        device_token="dev-device-secret",
                    ),
                    "bytes_sent must be a non-negative integer",
                ),
                (
                    lambda: service.record_trusted_turn_relay_usage(
                        CoturnUsageSample(
                            session_id=session.session_id,
                            actor="vehicle-001",
                            bytes_sent=120000,
                            bytes_received=30000,
                            duration_ms="1000",
                            source="coturn",
                        )
                    ),
                    "duration_ms must be a positive integer",
                ),
            ]

            for call, expected_error in cases:
                with self.subTest(expected_error=expected_error):
                    with self.assertRaisesRegex(ValueError, expected_error):
                        call()

            self.assertFalse(audit_path.exists())

    def test_http_audit_endpoints_reject_non_json_numeric_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]
                cases = [
                    (
                        f"/sessions/{session_id}/diagnostics",
                        {
                            "actor": "driver-a",
                            "token": login["token"],
                            "component": "driver-console",
                            "rtt_ms": True,
                            "packet_loss_percent": 1.5,
                            "jitter_ms": 12,
                            "video_latency_ms": 110,
                            "control_send_hz": 20.0,
                        },
                        "rtt_ms must be a non-negative integer",
                    ),
                    (
                        f"/sessions/{session_id}/diagnostics",
                        {
                            "actor": "driver-a",
                            "token": login["token"],
                            "component": "driver-console",
                            "rtt_ms": 84,
                            "packet_loss_percent": "1.5",
                            "jitter_ms": 12,
                            "video_latency_ms": 110,
                            "control_send_hz": 20.0,
                        },
                        "packet_loss_percent must be a non-negative number",
                    ),
                    (
                        f"/sessions/{session_id}/control_timeout",
                        {
                            "actor": "vehicle-001",
                            "device_token": "dev-device-secret",
                            "last_valid_receive_ms": True,
                            "timeout_entered_ms": 1800,
                            "control_timeout_ms": 800,
                        },
                        "last_valid_receive_ms must be a non-negative integer",
                    ),
                    (
                        f"/sessions/{session_id}/estop",
                        {
                            "actor": "driver-a",
                            "token": login["token"],
                            "reason": "driver_estop",
                            "seq": "42",
                        },
                        "seq must be a non-negative integer",
                    ),
                    (
                        f"/sessions/{session_id}/turn_usage",
                        {
                            "actor": "vehicle-001",
                            "device_token": "dev-device-secret",
                            "bytes_sent": "120000",
                            "bytes_received": 30000,
                            "duration_ms": 1000,
                        },
                        "bytes_sent must be a non-negative integer",
                    ),
                ]
                for path, payload, message in cases:
                    with self.subTest(path=path, message=message):
                        rejected = _json_post_expect_error(f"{base_url}{path}", payload, expected_status=400)
                        self.assertEqual(rejected["error"], message)

    def test_http_audit_endpoints_reject_non_string_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.devices.register("True", "dev-device-secret")
            service.devices.register("vehicle-002", "True")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "True"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                bool_actor_login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "True", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                bool_actor_session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-002", "driver_id": "True", "token": bool_actor_login["token"]},
                )
                cases = [
                    (
                        "/vehicles/offline",
                        {"vehicle_id": True, "device_token": "dev-device-secret", "reason": "heartbeat_timeout"},
                        "vehicle_id must be a string",
                    ),
                    (
                        f"/sessions/{bool_actor_session['session_id']}/abnormal_disconnect",
                        {
                            "actor": True,
                            "token": bool_actor_login["token"],
                            "reason": "websocket_closed",
                            "detected_by": "signaling-server",
                        },
                        "actor must be a string",
                    ),
                    (
                        f"/sessions/{session['session_id']}/abnormal_disconnect",
                        {
                            "actor": "driver-a",
                            "token": login["token"],
                            "reason": True,
                            "detected_by": "signaling-server",
                        },
                        "reason must be a string",
                    ),
                    (
                        f"/sessions/{bool_actor_session['session_id']}/abnormal_disconnect",
                        {
                            "actor": "vehicle-002",
                            "device_token": True,
                            "reason": "websocket_closed",
                            "detected_by": "vehicle-agent",
                        },
                        "device_token must be a string",
                    ),
                    (
                        f"/sessions/{bool_actor_session['session_id']}/diagnostics",
                        {
                            "actor": "vehicle-002",
                            "device_token": True,
                            "component": "vehicle-agent",
                            "rtt_ms": 84,
                            "packet_loss_percent": 1.5,
                            "jitter_ms": 12,
                            "video_latency_ms": 110,
                            "control_send_hz": 20.0,
                        },
                        "device_token must be a string",
                    ),
                    (
                        f"/sessions/{bool_actor_session['session_id']}/control_timeout",
                        {
                            "actor": "vehicle-002",
                            "device_token": True,
                            "last_valid_receive_ms": 1000,
                            "timeout_entered_ms": 1800,
                            "control_timeout_ms": 800,
                        },
                        "device_token must be a string",
                    ),
                    (
                        f"/sessions/{bool_actor_session['session_id']}/estop",
                        {
                            "actor": "vehicle-002",
                            "device_token": True,
                            "reason": "vehicle_estop",
                            "seq": 42,
                        },
                        "device_token must be a string",
                    ),
                    (
                        f"/sessions/{bool_actor_session['session_id']}/turn_relay",
                        {
                            "actor": "vehicle-002",
                            "device_token": True,
                            "turn_url": "turn:turn.example.com:3478?transport=udp",
                            "relay_candidate": "relay 203.0.113.10:49152 udp",
                            "selected_pair": "203.0.113.10:49152/10.0.0.5:53000",
                        },
                        "device_token must be a string",
                    ),
                    (
                        f"/sessions/{bool_actor_session['session_id']}/turn_usage",
                        {
                            "actor": "vehicle-002",
                            "device_token": True,
                            "bytes_sent": 120000,
                            "bytes_received": 30000,
                            "duration_ms": 1000,
                        },
                        "device_token must be a string",
                    ),
                    (
                        f"/sessions/{session['session_id']}/diagnostics",
                        {
                            "actor": "driver-a",
                            "token": login["token"],
                            "component": True,
                            "rtt_ms": 84,
                            "packet_loss_percent": 1.5,
                            "jitter_ms": 12,
                            "video_latency_ms": 110,
                            "control_send_hz": 20.0,
                        },
                        "component must be a string",
                    ),
                    (
                        f"/sessions/{session['session_id']}/estop",
                        {
                            "actor": "driver-a",
                            "token": login["token"],
                            "reason": True,
                            "seq": 42,
                        },
                        "reason must be a string",
                    ),
                    (
                        f"/sessions/{session['session_id']}/turn_relay",
                        {
                            "actor": "driver-a",
                            "token": login["token"],
                            "turn_url": "turn:turn.example.com:3478?transport=udp",
                            "relay_candidate": "relay 203.0.113.10:49152 udp",
                            "selected_pair": True,
                        },
                        "selected_pair must be a string",
                    ),
                ]
                for path, payload, message in cases:
                    with self.subTest(path=path, message=message):
                        rejected = _json_post_expect_error(f"{base_url}{path}", payload, expected_status=400)
                        self.assertEqual(rejected["error"], message)

    def test_http_service_audits_vehicle_offline_and_failed_active_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                offline = _json_post(
                    f"{base_url}/vehicles/offline",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret", "reason": "heartbeat_timeout"},
                )

            self.assertEqual(offline["vehicle_id"], "vehicle-001")
            self.assertEqual(offline["state"], "offline")
            self.assertEqual(offline["failed_sessions"], [session["session_id"]])
            self.assertEqual(service.sessions.sessions[session["session_id"]].state, "FAILED")
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            offline_event = next(record for record in audit_records if record["event"] == "vehicle_offline")
            failed_event = next(record for record in audit_records if record["event"] == "session_failed")
            self.assertEqual(offline_event["vehicle_id"], "vehicle-001")
            self.assertEqual(offline_event["details"]["reason"], "heartbeat_timeout")
            self.assertEqual(offline_event["details"]["failed_sessions"], [session["session_id"]])
            self.assertEqual(failed_event["session_id"], session["session_id"])
            self.assertEqual(failed_event["details"]["reason"], "vehicle_offline")

    def test_http_service_audits_turn_relay_enabled(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/turn_relay",
                    {
                        "actor": "vehicle-001",
                        "turn_url": "turn:turn.example.com:3478?transport=udp",
                        "relay_candidate": "relay 203.0.113.10:49152 udp",
                        "selected_pair": "vehicle-relay-to-driver-srflx",
                    },
                    expected_status=401,
                )
                missing_diagnostics = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/turn_relay",
                    {
                        "actor": "vehicle-001",
                        "device_token": "dev-device-secret",
                        "turn_url": "turn:turn.example.com:3478?transport=udp",
                    },
                    expected_status=400,
                )
                missing_selected_pair = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/turn_relay",
                    {
                        "actor": "vehicle-001",
                        "device_token": "dev-device-secret",
                        "turn_url": "turn:turn.example.com:3478?transport=udp",
                        "relay_candidate": "relay 203.0.113.10:49152 udp",
                    },
                    expected_status=400,
                )
                recorded = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/turn_relay",
                    {
                        "actor": "vehicle-001",
                        "device_token": "dev-device-secret",
                        "turn_url": "turn:turn.example.com:3478?transport=udp",
                        "relay_candidate": "relay 203.0.113.10:49152 udp",
                        "selected_pair": "vehicle-relay-to-driver-srflx",
                    },
                )

            self.assertEqual(unauthenticated["error"], "invalid device token")
            self.assertEqual(missing_diagnostics["error"], "relay_candidate is required")
            self.assertEqual(missing_selected_pair["error"], "selected_pair is required")
            self.assertEqual(recorded["event"], "turn_relay_enabled")
            self.assertEqual(recorded["session_id"], session["session_id"])
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            relay = next(record for record in audit_records if record["event"] == "turn_relay_enabled")
            self.assertEqual(relay["vehicle_id"], "vehicle-001")
            self.assertEqual(relay["actor"], "vehicle-001")
            self.assertEqual(relay["details"]["turn_url"], "turn:turn.example.com:3478?transport=udp")
            self.assertEqual(relay["details"]["relay_candidate"], "relay 203.0.113.10:49152 udp")
            self.assertEqual(relay["details"]["selected_pair"], "vehicle-relay-to-driver-srflx")

    def test_http_service_records_turn_relay_usage_by_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                unauthorized = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/turn_usage",
                    {
                        "actor": "driver-b",
                        "token": login["token"],
                        "bytes_sent": 1000,
                        "bytes_received": 1000,
                        "duration_ms": 1000,
                    },
                    expected_status=403,
                )
                unauthenticated = _json_post_expect_error(
                    f"{base_url}/sessions/{session['session_id']}/turn_usage",
                    {
                        "actor": "vehicle-001",
                        "bytes_sent": 1000,
                        "bytes_received": 1000,
                        "duration_ms": 1000,
                    },
                    expected_status=401,
                )
                first = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/turn_usage",
                    {
                        "actor": "vehicle-001",
                        "device_token": "dev-device-secret",
                        "bytes_sent": 120000,
                        "bytes_received": 30000,
                        "duration_ms": 1000,
                    },
                )
                second = _json_post(
                    f"{base_url}/sessions/{session['session_id']}/turn_usage",
                    {
                        "actor": "driver-a",
                        "token": login["token"],
                        "bytes_sent": 50000,
                        "bytes_received": 50000,
                        "duration_ms": 2000,
                    },
                )

            self.assertEqual(unauthorized["error"], "sender is not current session participant")
            self.assertEqual(unauthenticated["error"], "invalid device token")
            self.assertEqual(first["relay_bytes_total"], 150000)
            self.assertEqual(first["last_bitrate_kbps"], 1200.0)
            self.assertEqual(second["relay_bytes_total"], 250000)
            self.assertEqual(second["last_bitrate_kbps"], 400.0)
            self.assertEqual(second["sample_count"], 2)
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            usage = [record for record in audit_records if record["event"] == "turn_relay_usage"]
            self.assertEqual(len(usage), 2)
            self.assertEqual(usage[-1]["vehicle_id"], "vehicle-001")
            self.assertEqual(usage[-1]["session_id"], session["session_id"])
            self.assertEqual(usage[-1]["details"]["relay_bytes_total"], 250000)
            self.assertEqual(usage[-1]["details"]["last_bitrate_kbps"], 400.0)

    def test_http_service_issues_configured_ice_servers_to_current_session_participants(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(
                audit_log_path=audit_path,
                ice_config=IceConfig(
                    stun_servers=["stun:stun.example.com:3478"],
                    turn_servers=[
                        TurnServerConfig(
                            url="turn:turn.example.com:3478?transport=udp",
                            username="session-turn-user",
                            credential="session-turn-secret",
                        )
                    ],
                ),
            )
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                login_b = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-b", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                unauthenticated = _json_get_expect_error(
                    f"{base_url}/sessions/{session_id}/ice_servers?actor=driver-a",
                    expected_status=401,
                )
                unauthorized = _json_get_expect_error(
                    f"{base_url}/sessions/{session_id}/ice_servers?actor=driver-b&token={login_b['token']}",
                    expected_status=403,
                )
                driver_payload = _json_get(
                    f"{base_url}/sessions/{session_id}/ice_servers?actor=driver-a&token={login['token']}"
                )
                vehicle_payload = _json_get(
                    f"{base_url}/sessions/{session_id}/ice_servers?actor=vehicle-001&device_token=dev-device-secret"
                )

            self.assertEqual(unauthenticated["error"], "invalid driver token")
            self.assertEqual(unauthorized["error"], "sender is not current session participant")
            self.assertEqual(driver_payload, vehicle_payload)
            self.assertEqual(driver_payload["session_id"], session_id)
            self.assertEqual(driver_payload["ice_servers"][0], {"urls": ["stun:stun.example.com:3478"]})
            self.assertEqual(
                driver_payload["ice_servers"][1],
                {
                    "urls": ["turn:turn.example.com:3478?transport=udp"],
                    "username": "session-turn-user",
                    "credential": "session-turn-secret",
                    "credentialType": "password",
                },
            )
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            issued = [record for record in audit_records if record["event"] == "ice_servers_issued"]
            self.assertEqual(len(issued), 2)
            self.assertEqual(issued[-1]["vehicle_id"], "vehicle-001")
            self.assertEqual(issued[-1]["session_id"], session_id)
            self.assertEqual(issued[-1]["details"]["ice_server_count"], 2)
            self.assertEqual(issued[-1]["details"]["turn_server_count"], 1)

    def test_http_service_issues_temporary_turn_rest_credentials_without_auditing_secret(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            now_ms = [1_000_000]
            service = SignalingHttpService(
                audit_log_path=audit_path,
                clock_ms=lambda: now_ms[0],
                ice_config=IceConfig(
                    stun_servers=["stun:stun.example.com:3478"],
                    turn_servers=[
                        TurnServerConfig(
                            url="turn:turn.example.com:3478?transport=udp",
                            username="mine-teleop",
                            credential_mode="turn_rest",
                            static_auth_secret="shared-turn-secret",
                            credential_ttl_seconds=600,
                        )
                    ],
                ),
            )
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )

                payload = _json_get(
                    f"{base_url}/sessions/{session['session_id']}/ice_servers?actor=driver-a&token={login['token']}"
                )

            turn = payload["ice_servers"][1]
            expected_username = f"1600:mine-teleop:{session['session_id']}:driver-a"
            expected_credential = base64.b64encode(
                hmac.new(b"shared-turn-secret", expected_username.encode("utf-8"), hashlib.sha1).digest()
            ).decode("ascii")
            self.assertEqual(turn["username"], expected_username)
            self.assertEqual(turn["credential"], expected_credential)
            self.assertEqual(turn["credentialType"], "password")
            self.assertEqual(turn["expires_at_ms"], 1_600_000)
            audit_text = audit_path.read_text(encoding="utf-8")
            self.assertIn("ice_servers_issued", audit_text)
            self.assertNotIn("shared-turn-secret", audit_text)
            self.assertNotIn(expected_credential, audit_text)

    def test_coturn_usage_log_parser_extracts_usage_sample_from_session_username(self):
        parser = CoturnUsageLogParser()

        sample = parser.parse_line(
            "2026-06-28T12:00:00Z session 000000000000000001: usage: "
            "realm=<mine-teleop>, username=<session-000001:vehicle-001>, "
            "rp=4, rb=30000, sp=5, sb=120000, duration_ms=1000"
        )
        ignored = parser.parse_line("2026-06-28T12:00:01Z session 1: allocation timeout")

        self.assertIsNone(ignored)
        self.assertEqual(sample.session_id, "session-000001")
        self.assertEqual(sample.actor, "vehicle-001")
        self.assertEqual(sample.bytes_received, 30000)
        self.assertEqual(sample.bytes_sent, 120000)
        self.assertEqual(sample.duration_ms, 1000)
        self.assertEqual(sample.source, "coturn")

    def test_coturn_usage_log_parser_extracts_rest_username_session_and_actor(self):
        parser = CoturnUsageLogParser()

        sample = parser.parse_line(
            "2026-06-28T12:00:00Z session 000000000000000001: usage: "
            "realm=<mine-teleop>, username=<1600:mine-teleop:session-000001:driver-a>, "
            "rp=4, rb=30000, sp=5, sb=120000, duration_ms=1000"
        )

        self.assertEqual(sample.session_id, "session-000001")
        self.assertEqual(sample.actor, "driver-a")

    def test_http_service_records_trusted_coturn_usage_sample(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.sessions.vehicle_online("vehicle-001")
            session = service.sessions.request_session("vehicle-001", "driver-a")
            sample = CoturnUsageLogParser().parse_line(
                f"2026-06-28T12:00:00Z session 000000000000000001: usage: "
                f"realm=<mine-teleop>, username=<{session.session_id}:vehicle-001>, "
                f"rp=4, rb=30000, sp=5, sb=120000, duration_ms=1000"
            )

            recorded = service.record_trusted_turn_relay_usage(sample)

            self.assertEqual(recorded["relay_bytes_total"], 150000)
            self.assertEqual(recorded["sample_count"], 1)
            self.assertEqual(recorded["last_bitrate_kbps"], 1200.0)
            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            usage = next(record for record in audit_records if record["event"] == "turn_relay_usage")
            self.assertEqual(usage["vehicle_id"], "vehicle-001")
            self.assertEqual(usage["session_id"], session.session_id)
            self.assertEqual(usage["actor"], "coturn")
            self.assertEqual(usage["details"]["source"], "coturn")
            self.assertEqual(usage["details"]["source_actor"], "vehicle-001")

    def test_coturn_usage_report_cli_emits_redacted_jsonl_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            log_path = Path(tmp) / "coturn.log"
            log_path.write_text(
                "\n".join(
                    [
                        "2026-06-28T12:00:00Z session 000000000000000001: usage: "
                        "realm=<mine-teleop>, username=<session-000001:vehicle-001>, "
                        "rp=4, rb=30000, sp=5, sb=120000, duration_ms=1000",
                        "2026-06-28T12:00:01Z session 000000000000000002: allocation timeout",
                        "2026-06-28T12:00:02Z session 000000000000000003: usage: "
                        "realm=<mine-teleop>, username=<1600:mine-teleop:session-000001:driver-a>, "
                        "rp=2, rb=10000, sp=3, sb=50000, duration_ms=2000",
                    ]
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/coturn_usage_report.py",
                    "--log",
                    str(log_path),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("1600:mine-teleop:session-000001:driver-a", result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(
            [record["event"] for record in records],
            ["coturn_usage_report", "coturn_usage_sample", "coturn_usage_sample"],
        )
        summary = records[0]
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["total_lines"], 3)
        self.assertEqual(summary["parsed_samples"], 2)
        self.assertEqual(summary["ignored_lines"], 1)
        self.assertEqual(summary["session_count"], 1)
        self.assertEqual(summary["relay_bytes_total"], 210000)
        self.assertEqual(summary["duration_ms_total"], 3000)
        self.assertEqual(summary["average_bitrate_kbps"], 560.0)
        self.assertEqual(records[1]["session_id"], "session-000001")
        self.assertEqual(records[1]["actor"], "vehicle-001")
        self.assertEqual(records[1]["relay_bytes"], 150000)
        self.assertEqual(records[2]["actor"], "driver-a")
        self.assertEqual(records[2]["relay_bytes"], 60000)

    def test_websocket_signaling_accepts_offer_and_queues_for_recipient(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                with _websocket_connect(
                    base_url,
                    f"/signaling/{session_id}/ws?participant=driver-a&token={login['token']}",
                ) as sock:
                    _websocket_send_json(
                        sock,
                        {
                            "sender": "driver-a",
                            "recipient": "vehicle-001",
                            "type": "webrtc_offer",
                            "payload": {"sdp": "v=0"},
                        },
                    )
                    ack = _websocket_recv_json(sock)

                self.assertEqual(ack["queued"], 1)
                messages = _json_get(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                )
                self.assertEqual(messages["messages"][0]["type"], "webrtc_offer")
                self.assertEqual(messages["messages"][0]["payload"]["sdp"], "v=0")

    def test_websocket_signaling_rejects_non_object_json_without_queueing(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                with _websocket_connect(
                    base_url,
                    f"/signaling/{session_id}/ws?participant=driver-a&token={login['token']}",
                ) as sock:
                    _websocket_send_json(sock, ["not", "a", "message"])
                    rejected = _websocket_recv_json(sock)

                messages = _json_get(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                )

            self.assertEqual(rejected["error"], "websocket message must be a JSON object")
            self.assertEqual(messages["messages"], [])

    def test_websocket_signaling_rejects_unmasked_client_frame_without_queueing(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                with _websocket_connect(
                    base_url,
                    f"/signaling/{session_id}/ws?participant=driver-a&token={login['token']}",
                ) as sock:
                    _websocket_send_unmasked_json(
                        sock,
                        {
                            "sender": "driver-a",
                            "recipient": "vehicle-001",
                            "type": "webrtc_offer",
                            "payload": {"sdp": "v=0"},
                        },
                    )
                    rejected = _websocket_recv_json(sock)

                messages = _json_get(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                )

            self.assertEqual(rejected["error"], "client websocket frames must be masked")
            self.assertEqual(messages["messages"], [])

    def test_websocket_signaling_rejects_fragmented_text_frame_without_queueing(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                with _websocket_connect(
                    base_url,
                    f"/signaling/{session_id}/ws?participant=driver-a&token={login['token']}",
                ) as sock:
                    _websocket_send_fragmented_json(
                        sock,
                        {
                            "sender": "driver-a",
                            "recipient": "vehicle-001",
                            "type": "webrtc_offer",
                            "payload": {"sdp": "v=0"},
                        },
                    )
                    rejected = _websocket_recv_json(sock)

                messages = _json_get(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                )

            self.assertEqual(rejected["error"], "fragmented websocket messages are not supported")
            self.assertEqual(messages["messages"], [])

    def test_websocket_signaling_rejects_unmasked_close_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                with _websocket_connect(
                    base_url,
                    f"/signaling/{session_id}/ws?participant=driver-a&token={login['token']}",
                ) as sock:
                    _websocket_send_unmasked_close(sock)
                    rejected = _websocket_recv_json(sock)

            self.assertEqual(rejected["error"], "client websocket frames must be masked")

    def test_websocket_signaling_rejects_oversized_close_frame(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                with _websocket_connect(
                    base_url,
                    f"/signaling/{session_id}/ws?participant=driver-a&token={login['token']}",
                ) as sock:
                    _websocket_send_oversized_close(sock)
                    rejected = _websocket_recv_json(sock)

            self.assertEqual(rejected["error"], "websocket control frames must be no longer than 125 bytes")

    def test_websocket_upgrade_requires_standard_handshake_headers(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                parsed = urlparse(base_url)
                path = f"/signaling/{session['session_id']}/ws?participant=driver-a&token={login['token']}"
                valid_key = base64.b64encode(os.urandom(16)).decode("ascii")
                cases = [
                    (
                        "missing_upgrade",
                        [
                            f"GET {path} HTTP/1.1",
                            f"Host: {parsed.hostname}:{parsed.port}",
                            "Connection: Upgrade",
                            f"Sec-WebSocket-Key: {valid_key}",
                            "Sec-WebSocket-Version: 13",
                        ],
                        "WebSocket Upgrade header is required",
                    ),
                    (
                        "missing_connection_upgrade",
                        [
                            f"GET {path} HTTP/1.1",
                            f"Host: {parsed.hostname}:{parsed.port}",
                            "Upgrade: websocket",
                            f"Sec-WebSocket-Key: {valid_key}",
                            "Sec-WebSocket-Version: 13",
                        ],
                        "Connection: Upgrade header is required",
                    ),
                    (
                        "wrong_version",
                        [
                            f"GET {path} HTTP/1.1",
                            f"Host: {parsed.hostname}:{parsed.port}",
                            "Upgrade: websocket",
                            "Connection: Upgrade",
                            f"Sec-WebSocket-Key: {valid_key}",
                            "Sec-WebSocket-Version: 12",
                        ],
                        "Sec-WebSocket-Version must be 13",
                    ),
                    (
                        "invalid_key",
                        [
                            f"GET {path} HTTP/1.1",
                            f"Host: {parsed.hostname}:{parsed.port}",
                            "Upgrade: websocket",
                            "Connection: Upgrade",
                            "Sec-WebSocket-Key: not-base64",
                            "Sec-WebSocket-Version: 13",
                        ],
                        "Sec-WebSocket-Key must be a base64-encoded 16-byte value",
                    ),
                ]

                for name, lines, message in cases:
                    with self.subTest(name=name):
                        response = _raw_http_request(base_url, "\r\n".join(lines) + "\r\n\r\n")
                        self.assertIn(b" 400 ", response.splitlines()[0])
                        self.assertIn(message.encode("utf-8"), response)

    def test_signaling_message_rejects_non_string_fields_and_non_object_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.sessions.vehicle_online("vehicle-001")
            driver_bool_token = service.tokens.login("True", "dev-password").token
            session_with_bool_driver = service.sessions.request_session("vehicle-001", "True")
            service.sessions.vehicle_online("True")
            driver_token = service.tokens.login("driver-a", "dev-password").token
            session_with_bool_vehicle = service.sessions.request_session("True", "driver-a")

            cases = [
                (
                    session_with_bool_driver.session_id,
                    {
                        "sender": True,
                        "recipient": "vehicle-001",
                        "type": "webrtc_offer",
                        "token": driver_bool_token,
                        "payload": {"sdp": "v=0"},
                    },
                    "sender must be a string",
                ),
                (
                    session_with_bool_vehicle.session_id,
                    {
                        "sender": "driver-a",
                        "recipient": True,
                        "type": "webrtc_offer",
                        "token": driver_token,
                        "payload": {"sdp": "v=0"},
                    },
                    "recipient must be a string",
                ),
                (
                    session_with_bool_vehicle.session_id,
                    {
                        "sender": "driver-a",
                        "recipient": "True",
                        "type": True,
                        "token": driver_token,
                        "payload": {"sdp": "v=0"},
                    },
                    "type must be a string",
                ),
                (
                    session_with_bool_vehicle.session_id,
                    {
                        "sender": "driver-a",
                        "recipient": "True",
                        "type": "webrtc_offer",
                        "token": driver_token,
                        "payload": [["sdp", "v=0"]],
                    },
                    "payload must be a JSON object",
                ),
            ]

            for session_id, payload, message in cases:
                with self.subTest(message=message):
                    with self.assertRaisesRegex(ValueError, message):
                        service.enqueue_signaling_payload(session_id, payload)

    def test_signaling_rejects_non_participant_recipient(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                rejected = _json_post_expect_error(
                    f"{base_url}/signaling/{session_id}/messages",
                    {
                        "sender": "driver-a",
                        "recipient": "driver-b",
                        "type": "webrtc_offer",
                        "token": login["token"],
                        "payload": {"sdp": "v=0"},
                    },
                    expected_status=403,
                )
                messages = _json_get(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                )

            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(rejected["error"], "recipient is not current session participant")
            self.assertEqual(messages["messages"], [])
            self.assertNotIn("webrtc_offer", [record["event"] for record in audit_records])

    def test_signaling_rejects_client_forged_control_authority_revoked_message(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
                session_id = session["session_id"]

                rejected = _json_post_expect_error(
                    f"{base_url}/signaling/{session_id}/messages",
                    {
                        "sender": "driver-a",
                        "recipient": "vehicle-001",
                        "type": "control_authority_revoked",
                        "token": login["token"],
                        "payload": {"reason": "operator_takeover"},
                    },
                    expected_status=400,
                )
                messages = _json_get(
                    f"{base_url}/signaling/{session_id}/messages?recipient=vehicle-001&device_token=dev-device-secret"
                )

            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(rejected["error"], "unsupported signaling message type")
            self.assertEqual(messages["messages"], [])
            self.assertEqual(service.sessions.sessions[session_id].state, "SESSION_ACTIVE")
            self.assertNotIn("control_authority_revoked", [record["event"] for record in audit_records])

    def test_cloud_service_docs_distinguish_lifecycle_apis_from_client_signaling_queue(self):
        docs = Path("docs/05-cloud-services-design.md").read_text(encoding="utf-8")

        self.assertIn("受控 HTTP API 生命周期事件", docs)
        self.assertIn("客户端可入队信令消息", docs)
        self.assertIn("不能通过普通信令消息伪造 `session_accept`、`session_reject`", docs)

    def test_http_signaling_rejects_non_string_device_token_before_auth(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.devices.register(vehicle_id="vehicle-002", device_token="True")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "True"},
                )
                login = _json_post(
                    f"{base_url}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                session = _json_post(
                    f"{base_url}/sessions",
                    {"vehicle_id": "vehicle-002", "driver_id": "driver-a", "token": login["token"]},
                )

                rejected = _json_post_expect_error(
                    f"{base_url}/signaling/{session['session_id']}/messages",
                    {
                        "sender": "vehicle-002",
                        "recipient": "driver-a",
                        "type": "webrtc_answer",
                        "device_token": True,
                        "payload": {"sdp": "v=0"},
                    },
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "device_token must be a string")
            self.assertEqual(service.messages.pop_for(session["session_id"], "driver-a"), [])

    def test_device_tokens_are_registered_per_vehicle_for_online_and_upload_api(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.devices.register(vehicle_id="vehicle-002", device_token="vehicle-002-secret")

            with service.running() as base_url:
                wrong_online = _json_post_expect_error(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "dev-device-secret"},
                    expected_status=401,
                )
                self.assertEqual(wrong_online["error"], "invalid device token")

                online = _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "vehicle-002-secret"},
                )
                self.assertEqual(online["vehicle_id"], "vehicle-002")

                wrong_upload = _json_post_expect_error(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-002",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "seg-vehicle-002",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "video",
                    },
                    expected_status=401,
                )
                self.assertEqual(wrong_upload["error"], "invalid device token")

                issued = _json_post(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "vehicle-002-secret",
                        "vehicle_id": "vehicle-002",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "seg-vehicle-002",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "video",
                    },
                )
                self.assertEqual(
                    issued["object_path"],
                    "vehicles/vehicle-002/sessions/session-001/cameras/front/seg-vehicle-002.mp4",
                )

    def test_http_service_rejects_non_string_device_token_before_device_auth(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.devices.register(vehicle_id="vehicle-002", device_token="True")
            with service.running() as base_url:
                rejected = _json_post_expect_error(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": True},
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "device_token must be a string")
            self.assertNotIn("vehicle-002", service.sessions.online_vehicles)

    def test_upload_complete_rejects_token_for_different_object_path_vehicle(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            service.devices.register(vehicle_id="vehicle-002", device_token="vehicle-002-secret")
            with service.running() as base_url:
                rejected = _json_post_expect_error(
                    f"{base_url}/uploads/complete",
                    {
                        "device_token": "vehicle-002-secret",
                        "vehicle_id": "vehicle-002",
                        "segment_id": "seg-vehicle-001",
                        "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-vehicle-001.mp4",
                        "bytes_uploaded": 64000000,
                    },
                    expected_status=401,
                )

            self.assertEqual(rejected["error"], "invalid device token")

    def test_upload_complete_rejects_non_standard_object_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                rejected = _json_post_expect_error(
                    f"{base_url}/uploads/complete",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "segment_id": "seg-outside-prefix",
                        "object_path": "../outside/seg-outside-prefix.mp4",
                        "bytes_uploaded": 64000000,
                    },
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "invalid upload object_path")

    def test_upload_complete_rejects_negative_uploaded_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                rejected = _json_post_expect_error(
                    f"{base_url}/uploads/complete",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "segment_id": "seg-negative-bytes",
                        "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-negative-bytes.mp4",
                        "bytes_uploaded": -1,
                    },
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "bytes_uploaded must be non-negative")

    def test_upload_complete_rejects_non_integer_uploaded_bytes(self):
        cases = [True, "64000000"]
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                for bytes_uploaded in cases:
                    with self.subTest(bytes_uploaded=bytes_uploaded):
                        rejected = _json_post_expect_error(
                            f"{base_url}/uploads/complete",
                            {
                                "device_token": "dev-device-secret",
                                "vehicle_id": "vehicle-001",
                                "segment_id": "seg-non-integer-bytes",
                                "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-non-integer-bytes.mp4",
                                "bytes_uploaded": bytes_uploaded,
                            },
                            expected_status=400,
                        )
                        self.assertEqual(rejected["error"], "bytes_uploaded must be a non-negative integer")

    def test_upload_complete_and_failed_reject_non_string_fields(self):
        cases = [
            (
                "/uploads/complete",
                {
                    "device_token": "dev-device-secret",
                    "vehicle_id": "vehicle-001",
                    "segment_id": True,
                    "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-bool-id.mp4",
                    "bytes_uploaded": 64000000,
                },
                "segment_id must be a string",
            ),
            (
                "/uploads/failed",
                {
                    "device_token": "dev-device-secret",
                    "vehicle_id": "vehicle-001",
                    "segment_id": "seg-bool-error",
                    "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-bool-error.mp4",
                    "error": True,
                },
                "error must be a string",
            ),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                for endpoint, payload, message in cases:
                    with self.subTest(endpoint=endpoint, message=message):
                        rejected = _json_post_expect_error(
                            f"{base_url}{endpoint}",
                            payload,
                            expected_status=400,
                        )
                        self.assertEqual(rejected["error"], message)

    def test_upload_complete_and_failed_reject_unsafe_object_path_segments(self):
        cases = [
            (
                "/uploads/complete",
                {
                    "device_token": "dev-device-secret",
                    "vehicle_id": "vehicle-001",
                    "segment_id": "seg-unsafe-camera",
                    "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/../seg-unsafe-camera.mp4",
                    "bytes_uploaded": 64000000,
                },
            ),
            (
                "/uploads/failed",
                {
                    "device_token": "dev-device-secret",
                    "vehicle_id": "vehicle-001",
                    "segment_id": "seg-unsafe-session",
                    "object_path": "vehicles/vehicle-001/sessions/../metadata/seg-unsafe-session.json",
                    "error": "network_timeout",
                },
            ),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                for endpoint, payload in cases:
                    with self.subTest(endpoint=endpoint, object_path=payload["object_path"]):
                        rejected = _json_post_expect_error(
                            f"{base_url}{endpoint}",
                            payload,
                            expected_status=400,
                        )
                        self.assertEqual(rejected["error"], "invalid upload object_path")

    def test_upload_complete_and_failed_require_segment_id_to_match_object_path(self):
        cases = [
            (
                "/uploads/complete",
                {
                    "device_token": "dev-device-secret",
                    "vehicle_id": "vehicle-001",
                    "segment_id": "seg-payload",
                    "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-object.mp4",
                    "bytes_uploaded": 64000000,
                },
            ),
            (
                "/uploads/failed",
                {
                    "device_token": "dev-device-secret",
                    "vehicle_id": "vehicle-001",
                    "segment_id": "seg-payload",
                    "object_path": "vehicles/vehicle-001/sessions/session-001/metadata/seg-object.json",
                    "error": "network_timeout",
                },
            ),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                for endpoint, payload in cases:
                    with self.subTest(endpoint=endpoint):
                        rejected = _json_post_expect_error(
                            f"{base_url}{endpoint}",
                            payload,
                            expected_status=400,
                        )
                        self.assertEqual(rejected["error"], "segment_id must match upload object_path")

    def test_upload_failed_requires_failure_reason(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                rejected = _json_post_expect_error(
                    f"{base_url}/uploads/failed",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "segment_id": "seg-missing-error",
                        "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/seg-missing-error.mp4",
                        "error": "",
                    },
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "upload failure error is required")

    def test_http_service_issues_refreshes_and_records_upload_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                unauthenticated = _json_post_expect_error(
                    f"{base_url}/uploads/credentials",
                    {
                        "vehicle_id": "vehicle-001",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "20260624T101500Z_front_000001",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "video",
                    },
                    expected_status=401,
                )
                self.assertEqual(unauthenticated["error"], "invalid device token")

                issued = _json_post(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "20260624T101500Z_front_000001",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "video",
                    },
                )
                self.assertEqual(
                    issued["object_path"],
                    "vehicles/vehicle-001/sessions/session-001/cameras/front/20260624T101500Z_front_000001.mp4",
                )
                self.assertIn("/upload-target/", issued["upload_url"])
                self.assertGreater(issued["expires_at_ms"], issued["issued_at_ms"])

                refreshed = _json_post(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "20260624T101500Z_front_000001",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "video",
                    },
                )
                self.assertEqual(refreshed["object_path"], issued["object_path"])
                self.assertNotEqual(refreshed["upload_url"], issued["upload_url"])

                metadata = _json_post(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "20260624T101500Z_front_000001",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "metadata",
                    },
                )
                self.assertEqual(
                    metadata["object_path"],
                    "vehicles/vehicle-001/sessions/session-001/metadata/20260624T101500Z_front_000001.json",
                )

                uploaded = _json_post(
                    f"{base_url}/uploads/complete",
                    {
                        "device_token": "dev-device-secret",
                        "segment_id": "20260624T101500Z_front_000001",
                        "object_path": issued["object_path"],
                        "bytes_uploaded": 64000000,
                    },
                )
                self.assertEqual(uploaded["status"], "uploaded")

                failed = _json_post(
                    f"{base_url}/uploads/failed",
                    {
                        "device_token": "dev-device-secret",
                        "segment_id": "20260624T101600Z_front_000002",
                        "object_path": "vehicles/vehicle-001/sessions/session-001/cameras/front/20260624T101600Z_front_000002.mp4",
                        "error": "network_timeout",
                    },
                )
                self.assertEqual(failed["status"], "failed")
                self.assertEqual(failed["error"], "network_timeout")

            audit_records = [json.loads(line) for line in audit_path.read_text(encoding="utf-8").splitlines()]
            audit_events = [record["event"] for record in audit_records]
            self.assertIn("upload_credential_issued", audit_events)
            self.assertIn("upload_success", audit_events)
            self.assertIn("upload_failed", audit_events)
            success = next(record for record in audit_records if record["event"] == "upload_success")
            failure = next(record for record in audit_records if record["event"] == "upload_failed")
            self.assertEqual(success["vehicle_id"], "vehicle-001")
            self.assertEqual(success["session_id"], "session-001")
            self.assertEqual(success["actor"], "vehicle-001")
            self.assertEqual(failure["vehicle_id"], "vehicle-001")
            self.assertEqual(failure["session_id"], "session-001")
            self.assertEqual(failure["actor"], "vehicle-001")

    def test_http_service_can_issue_configured_s3_upload_credentials(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            uploads = UploadCredentialService(
                ttl_seconds=600,
                s3_signer=S3PresignedPutSigner(
                    S3PresignConfig(
                        endpoint_url="https://s3.us-west-2.amazonaws.com",
                        bucket="mine-teleop-recordings",
                        region="us-west-2",
                        access_key_id="AKIDEXAMPLE",
                        secret_access_key="SECRET",
                    )
                ),
            )
            service = SignalingHttpService(audit_log_path=audit_path, upload_credentials=uploads)
            with service.running() as base_url:
                issued = _json_post(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "20260624T101500Z_front_000001",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "video",
                    },
                )

            parsed = urlparse(issued["upload_url"])
            query = {key: values[0] for key, values in parse_qs(parsed.query).items()}
            self.assertEqual(parsed.scheme, "https")
            self.assertEqual(parsed.netloc, "s3.us-west-2.amazonaws.com")
            self.assertEqual(
                parsed.path,
                "/mine-teleop-recordings/vehicles/vehicle-001/sessions/session-001/cameras/front/20260624T101500Z_front_000001.mp4",
            )
            self.assertTrue(query["X-Amz-Credential"].startswith("AKIDEXAMPLE/"))
            self.assertTrue(query["X-Amz-Credential"].endswith("/us-west-2/s3/aws4_request"))
            self.assertEqual(query["X-Amz-Expires"], "600")
            self.assertEqual(issued["expires_at_ms"] - issued["issued_at_ms"], 600_000)

    def test_upload_credentials_reject_unsafe_object_path_segments(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                rejected = _json_post_expect_error(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "session_id": "session-001",
                        "camera_id": "../outside",
                        "segment_id": "seg-unsafe",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": "video",
                    },
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "camera_id must be a safe object path segment")

    def test_upload_credentials_reject_non_string_object_path_segments(self):
        cases = [
            ("session_id", True, "session_id must be a safe object path segment"),
            ("camera_id", None, "camera_id must be a safe object path segment"),
            ("segment_id", None, "segment_id must be a safe object path segment"),
        ]
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                for field_name, field_value, expected_error in cases:
                    with self.subTest(field_name=field_name, field_value=field_value):
                        payload = {
                            "device_token": "dev-device-secret",
                            "vehicle_id": "vehicle-001",
                            "session_id": "session-001",
                            "camera_id": "front",
                            "segment_id": "seg-unsafe",
                            "started_at": "2026-06-24T10:15:00Z",
                            "kind": "video",
                        }
                        payload[field_name] = field_value
                        rejected = _json_post_expect_error(
                            f"{base_url}/uploads/credentials",
                            payload,
                            expected_status=400,
                        )
                        self.assertEqual(rejected["error"], expected_error)

    def test_upload_credentials_reject_non_string_kind(self):
        with tempfile.TemporaryDirectory() as tmp:
            audit_path = Path(tmp) / "audit.jsonl"
            service = SignalingHttpService(audit_log_path=audit_path)
            with service.running() as base_url:
                rejected = _json_post_expect_error(
                    f"{base_url}/uploads/credentials",
                    {
                        "device_token": "dev-device-secret",
                        "vehicle_id": "vehicle-001",
                        "session_id": "session-001",
                        "camera_id": "front",
                        "segment_id": "seg-non-string-kind",
                        "started_at": "2026-06-24T10:15:00Z",
                        "kind": True,
                    },
                    expected_status=400,
                )

            self.assertEqual(rejected["error"], "kind must be a non-empty string")


class ObservabilityTests(unittest.TestCase):
    def test_telemetry_publisher_marks_mock_telemetry_and_camera_statuses(self):
        adapter = MockVehicleAdapter()
        adapter.apply_control(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=1,
                ts_ms=1000,
                gear="D",
                steering=0.1,
                throttle=0.2,
                brake=0.0,
            )
        )
        publisher = TelemetryPublisher(vehicle_id="vehicle-001", session_id="session-001", source="mock")
        payload = publisher.build(
            telemetry=adapter.read_telemetry(),
            safety_state=SafetyState.CONTROL_ACTIVE,
            control_rtt_ms=55,
            video_status={
                "front": {"fps": 30, "bitrate_kbps": 3000, "latency_ms": 95, "state": "connected"},
                "rear": {"state": "reconnecting"},
            },
            system={"cpu_percent": 12.5, "disk_free_gb": 80.0},
            now_ms=2000,
        )

        self.assertEqual(payload["type"], "telemetry")
        self.assertTrue(payload["mock_telemetry"])
        self.assertEqual(payload["safety_state"], "CONTROL_ACTIVE")
        self.assertEqual(payload["video"]["front"]["bitrate_kbps"], 3000)
        self.assertEqual(payload["video"]["front"]["latency_ms"], 95)
        self.assertFalse(payload["video"]["front"]["low_bitrate"])
        self.assertEqual(payload["video"]["rear"]["fps"], 0)
        self.assertEqual(payload["video"]["rear"]["bitrate_kbps"], 0)
        self.assertIsNone(payload["video"]["rear"]["latency_ms"])
        self.assertTrue(payload["video"]["rear"]["reconnecting"])
        self.assertEqual(payload["system"]["disk_free_gb"], 80.0)

    def test_telemetry_publisher_reports_system_and_video_fault_flags(self):
        adapter = MockVehicleAdapter()
        publisher = TelemetryPublisher(vehicle_id="vehicle-001", session_id="session-001", source="mock")

        payload = publisher.build(
            telemetry=adapter.read_telemetry(),
            safety_state=SafetyState.CONTROL_ACTIVE,
            control_rtt_ms=55,
            video_status={
                "front": {
                    "state": "degraded",
                    "fault": "hardware_encoder_unavailable",
                    "encoder": "x264",
                    "low_bitrate": True,
                },
                "rear": {"state": "connected"},
            },
            system={"fault_flags": ["disk_watermark_low"]},
            now_ms=2000,
        )

        self.assertEqual(payload["video"]["front"]["fault"], "hardware_encoder_unavailable")
        self.assertEqual(payload["video"]["front"]["encoder"], "x264")
        self.assertEqual(
            payload["fault_flags"],
            ["disk_watermark_low", "video.front.hardware_encoder_unavailable"],
        )

    def test_audit_log_persists_control_session_and_estop_events_as_json_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "audit.jsonl"
            log = AuditLog(path)
            log.append(
                AuditEvent(
                    ts_ms=1000,
                    event="session_started",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    actor="driver-a",
                )
            )
            log.append(
                AuditEvent(
                    ts_ms=1200,
                    event="estop_latched",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    actor="driver-a",
                    details={"seq": 24},
                )
            )

            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 2)
            self.assertEqual(json.loads(lines[0])["event"], "session_started")
            self.assertEqual(json.loads(lines[1])["details"]["seq"], 24)

    def test_audit_log_rotates_json_lines_by_size_with_numbered_backups(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "audit.jsonl"
            try:
                log = AuditLog(path, max_bytes=1, backup_count=2)
            except TypeError as exc:
                self.fail(f"AuditLog should accept rotation options: {exc}")

            log.append(
                AuditEvent(
                    ts_ms=1000,
                    event="session_started",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    actor="driver-a",
                )
            )
            log.append(
                AuditEvent(
                    ts_ms=1100,
                    event="control_authority_granted",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    actor="driver-a",
                )
            )
            log.append(
                AuditEvent(
                    ts_ms=1200,
                    event="estop",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    actor="driver-a",
                    details={"seq": 24},
                )
            )

            current = json.loads(path.read_text(encoding="utf-8"))
            first_backup = json.loads(path.with_name("audit.jsonl.1").read_text(encoding="utf-8"))
            second_backup = json.loads(path.with_name("audit.jsonl.2").read_text(encoding="utf-8"))

        self.assertEqual(current["event"], "estop")
        self.assertEqual(first_backup["event"], "control_authority_granted")
        self.assertEqual(second_backup["event"], "session_started")

    def test_component_log_persists_operations_log_fields_as_json_lines(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "component.log"
            log = ComponentLog(path)

            log.append(
                ComponentLogEvent(
                    ts_ms=2000,
                    level="warning",
                    component="vehicle-media-agent",
                    vehicle_id="vehicle-001",
                    session_id="session-001",
                    camera_id="front",
                    event="camera_reconnecting",
                    message="front camera pipeline is reconnecting",
                    error_code="camera_timeout",
                )
            )

            record = json.loads(path.read_text(encoding="utf-8"))
            self.assertEqual(record["ts"], "1970-01-01T00:00:02.000Z")
            self.assertEqual(record["level"], "WARNING")
            self.assertEqual(record["component"], "vehicle-media-agent")
            self.assertEqual(record["vehicle_id"], "vehicle-001")
            self.assertEqual(record["session_id"], "session-001")
            self.assertEqual(record["camera_id"], "front")
            self.assertEqual(record["event"], "camera_reconnecting")
            self.assertEqual(record["message"], "front camera pipeline is reconnecting")
            self.assertEqual(record["error_code"], "camera_timeout")

    def test_component_log_applies_runtime_logging_level_update(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "component.log"
            try:
                log = ComponentLog(path, min_level="warning")
            except TypeError as exc:
                self.fail(f"ComponentLog should accept a minimum log level: {exc}")

            log.append(ComponentLogEvent(ts_ms=1000, level="info", component="vehicle-agent", event="ignored_info"))
            self.assertFalse(path.exists())

            warning = ComponentLogEvent(ts_ms=1100, level="warning", component="vehicle-agent", event="kept_warning")
            log.append(warning)

            decision = log.apply_runtime_update("logging.level", "debug")
            log.append(ComponentLogEvent(ts_ms=1200, level="debug", component="vehicle-agent", event="kept_debug"))

            records = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]

        self.assertTrue(decision.allowed)
        self.assertEqual(decision.reason, "runtime_update_allowed")
        self.assertEqual([record["event"] for record in records], ["kept_warning", "kept_debug"])

    def test_component_log_rotates_json_lines_by_size_with_numbered_backups(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "component.log"
            log = ComponentLog(path, max_bytes=1, backup_count=2)

            log.append(ComponentLogEvent(ts_ms=1000, level="info", component="vehicle-agent", event="first"))
            log.append(ComponentLogEvent(ts_ms=2000, level="info", component="vehicle-agent", event="second"))
            log.append(ComponentLogEvent(ts_ms=3000, level="info", component="vehicle-agent", event="third"))

            current = json.loads(path.read_text(encoding="utf-8"))
            first_backup = json.loads(path.with_name("component.log.1").read_text(encoding="utf-8"))
            second_backup = json.loads(path.with_name("component.log.2").read_text(encoding="utf-8"))
            self.assertEqual(current["event"], "third")
            self.assertEqual(first_backup["event"], "second")
            self.assertEqual(second_backup["event"], "first")

    def test_operations_metrics_snapshot_contains_vehicle_cloud_and_driver_acceptance_fields(self):
        snapshot = OperationsMetricsBuilder().build(
            vehicle_system={
                "cpu_percent": 31.5,
                "gpu_percent": 42.0,
                "memory_percent": 64.0,
                "disk_free_gb": 128.0,
                "disk_write_mbps": 18.2,
                "network_5g_state": "connected",
            },
            video_status={
                "front": {"fps": 30, "bitrate_kbps": 3000, "decode_fps": 29},
                "rear": {"fps": 28, "bitrate_kbps": 2800, "decode_fps": 27},
            },
            control={"command_hz": 20, "timeout_count": 1},
            cloud={
                "signaling_connections": 2,
                "turn_relay_bytes": 2048,
                "active_sessions": 1,
                "upload_success_count": 9,
                "upload_failure_count": 1,
                "upload_failure_reasons": {"network_timeout": 1},
            },
            vehicle_adapter={
                "adapter_type": "dynamic_library",
                "opened": True,
                "healthy": False,
                "can_interface": "can0",
                "library_path": "/tmp/libmine_teleop_chassis_bridge.so",
                "last_error": "mine_teleop_chassis_open failed with code -3",
                "applied_command_count": 12,
                "safe_stop_count": 2,
            },
            driver={
                "control_send_hz": 20.0,
                "ui_jank_ms": 12,
                "rtt_ms": 84,
                "packet_loss_rate": 0.02,
            },
        )

        self.assertEqual(snapshot["vehicle"]["cpu_percent"], 31.5)
        self.assertEqual(snapshot["vehicle"]["gpu_percent"], 42.0)
        self.assertEqual(snapshot["vehicle"]["memory_percent"], 64.0)
        self.assertEqual(snapshot["vehicle"]["disk_free_gb"], 128.0)
        self.assertEqual(snapshot["vehicle"]["disk_write_mbps"], 18.2)
        self.assertEqual(snapshot["vehicle"]["network_5g_state"], "connected")
        self.assertEqual(snapshot["vehicle"]["encoder_fps_by_camera"], {"front": 30, "rear": 28})
        self.assertEqual(
            snapshot["vehicle"]["realtime_bitrate_kbps_by_camera"],
            {"front": 3000, "rear": 2800},
        )
        self.assertEqual(snapshot["vehicle"]["control_command_hz"], 20)
        self.assertEqual(snapshot["vehicle"]["control_timeout_count"], 1)
        self.assertEqual(
            snapshot["vehicle_adapter"],
            {
                "adapter_type": "dynamic_library",
                "opened": True,
                "healthy": False,
                "can_interface": "can0",
                "library_path": "/tmp/libmine_teleop_chassis_bridge.so",
                "last_error": "mine_teleop_chassis_open failed with code -3",
                "applied_command_count": 12,
                "safe_stop_count": 2,
            },
        )
        self.assertEqual(snapshot["cloud"]["signaling_connections"], 2)
        self.assertEqual(snapshot["cloud"]["turn_relay_bytes"], 2048)
        self.assertEqual(snapshot["cloud"]["active_sessions"], 1)
        self.assertEqual(snapshot["cloud"]["upload_success_rate"], 0.9)
        self.assertEqual(snapshot["cloud"]["upload_failure_reasons"], {"network_timeout": 1})
        self.assertEqual(snapshot["driver"]["video_decode_fps_by_camera"], {"front": 29, "rear": 27})
        self.assertEqual(snapshot["driver"]["control_send_hz"], 20.0)
        self.assertEqual(snapshot["driver"]["ui_jank_ms"], 12)
        self.assertEqual(snapshot["driver"]["rtt_ms"], 84)
        self.assertEqual(snapshot["driver"]["packet_loss_rate"], 0.02)

    def test_control_acceptance_metrics_report_records_frequency_rtt_rejections_and_brake_samples(self):
        recorder = ControlAcceptanceMetricsRecorder()
        for seq, ts_ms in enumerate((0, 50, 100, 150), start=1):
            recorder.record_driver_send(seq=seq, ts_ms=ts_ms)
        recorder.record_receive(ReceiveResult(True, "accepted"), receive_time_ms=0)
        recorder.record_receive(ReceiveResult(True, "accepted"), receive_time_ms=50)
        recorder.record_receive(ReceiveResult(False, "old_seq"), receive_time_ms=90)
        recorder.record_receive(ReceiveResult(False, "command_gap_exceeded"), receive_time_ms=400)
        recorder.record_rtt(55)
        recorder.record_rtt(65)
        recorder.record_timeout(last_valid_receive_ms=50, timeout_entered_ms=850)
        recorder.record_brake_sample(
            now_ms=850,
            stage="timeout_brake_stage_0",
            speed_mps=1.2,
            brake_feedback=0.3,
            distance_since_last_valid_m=0.9,
        )
        recorder.record_brake_sample(
            now_ms=1350,
            stage="timeout_brake_stage_1",
            speed_mps=0.4,
            brake_feedback=0.6,
            distance_since_last_valid_m=1.4,
        )

        report = recorder.to_report()

        self.assertEqual(report["control_send_hz"], 20.0)
        self.assertEqual(report["control_receive_hz"], 20.0)
        self.assertEqual(report["control_rtt_ms_avg"], 60.0)
        self.assertEqual(report["command_out_of_order_count"], 1)
        self.assertEqual(report["command_expired_count"], 1)
        self.assertEqual(report["timeout_trigger_ms"], 850)
        self.assertEqual(report["coast_time_before_timeout_ms"], 800)
        self.assertEqual(report["coast_distance_before_timeout_m"], 0.9)
        self.assertEqual(report["brake_stage_samples"][0]["brake_feedback"], 0.3)
        self.assertEqual(report["stopping_distance_m"], 1.4)

    def test_video_acceptance_metrics_report_records_latency_drop_failures_and_reconnects(self):
        recorder = VideoAcceptanceMetricsRecorder()

        recorder.record_sample(
            camera_id="front",
            fps=30,
            bitrate_kbps=3000,
            end_to_end_latency_ms=95,
            decoded_frames=100,
            dropped_frames=2,
        )
        recorder.record_sample(
            camera_id="front",
            fps=28,
            bitrate_kbps=2800,
            end_to_end_latency_ms=105,
            decoded_frames=97,
            dropped_frames=1,
        )
        recorder.record_decode_failure("front")
        recorder.record_reconnect("front")

        report = recorder.to_report()

        front = report["cameras"]["front"]
        self.assertEqual(front["fps_avg"], 29.0)
        self.assertEqual(front["bitrate_kbps_avg"], 2900.0)
        self.assertEqual(front["end_to_end_latency_ms_avg"], 100.0)
        self.assertEqual(front["decoded_frames"], 197)
        self.assertEqual(front["dropped_frames"], 3)
        self.assertEqual(front["dropped_frame_rate"], 0.015)
        self.assertEqual(front["decode_failure_count"], 1)
        self.assertEqual(front["reconnect_count"], 1)

    def test_recording_acceptance_metrics_report_records_segment_metadata_size_fps_latency_and_disk_growth(self):
        recorder = RecordingAcceptanceMetricsRecorder()

        recorder.record_segment(
            camera_id="front",
            segment_id="seg-001",
            segment_complete=True,
            metadata_complete=True,
            file_size_bytes=64_000_000,
            encoding_fps=30,
            write_latency_ms=35,
            disk_used_bytes_after=1_000_000_000,
        )
        recorder.record_segment(
            camera_id="front",
            segment_id="seg-002",
            segment_complete=False,
            metadata_complete=False,
            file_size_bytes=60_000_000,
            encoding_fps=28,
            write_latency_ms=45,
            disk_used_bytes_after=1_068_000_000,
        )

        report = recorder.to_report()

        front = report["cameras"]["front"]
        self.assertEqual(front["segment_count"], 2)
        self.assertEqual(front["complete_segment_count"], 1)
        self.assertEqual(front["metadata_complete_count"], 1)
        self.assertFalse(front["all_segments_complete"])
        self.assertFalse(front["all_metadata_complete"])
        self.assertEqual(front["file_size_bytes_total"], 124_000_000)
        self.assertEqual(front["file_size_bytes_avg"], 62_000_000.0)
        self.assertEqual(front["encoding_fps_avg"], 29.0)
        self.assertEqual(front["write_latency_ms_avg"], 40.0)
        self.assertEqual(front["disk_growth_bytes"], 68_000_000)

    def test_upload_acceptance_metrics_report_records_speed_retries_failures_and_realtime_impact(self):
        recorder = UploadAcceptanceMetricsRecorder()

        recorder.record_realtime_baseline(camera_id="front", fps=30, bitrate_kbps=3000)
        recorder.record_realtime_during_upload(camera_id="front", fps=27, bitrate_kbps=2400)
        recorder.record_upload(
            segment_id="seg-001",
            bytes_uploaded=120_000_000,
            started_ms=1_000,
            finished_ms=11_000,
            retry_count=2,
            status="failed",
            failure_reason="network_timeout",
        )
        recorder.record_upload(
            segment_id="seg-002",
            bytes_uploaded=60_000_000,
            started_ms=12_000,
            finished_ms=17_000,
            retry_count=0,
            status="uploaded",
        )

        report = recorder.to_report()

        self.assertEqual(report["upload_count"], 2)
        self.assertEqual(report["uploaded_count"], 1)
        self.assertEqual(report["failed_count"], 1)
        self.assertEqual(report["bytes_uploaded_total"], 180_000_000)
        self.assertEqual(report["upload_speed_mbps_avg"], 96.0)
        self.assertEqual(report["retry_count_total"], 2)
        self.assertEqual(report["failure_reasons"], {"network_timeout": 1})
        self.assertEqual(
            report["realtime_impact"]["front"],
            {
                "baseline_fps_avg": 30.0,
                "during_upload_fps_avg": 27.0,
                "fps_delta": -3.0,
                "baseline_bitrate_kbps_avg": 3000.0,
                "during_upload_bitrate_kbps_avg": 2400.0,
                "bitrate_kbps_delta": -600.0,
            },
        )


class TimeSyncTests(unittest.TestCase):
    def test_chrony_tracking_output_parses_synchronization_and_offset_estimate(self):
        status = TimeSyncStatus.from_chronyc_tracking(
            """
Reference ID    : 0A000001 (time.local)
Stratum         : 3
System time     : 0.000234567 seconds fast of NTP time
Leap status     : Normal
""",
            source="chrony",
        )

        self.assertEqual(status.source, "chrony")
        self.assertTrue(status.synchronized)
        self.assertEqual(status.stratum, 3)
        self.assertAlmostEqual(status.offset_ms, 0.234567, places=6)

    def test_time_sync_monitor_builds_startup_log_event_and_rejects_unsynchronized_clock(self):
        monitor = TimeSyncMonitor(minimum="ntp", max_offset_ms=50)
        unsynchronized = TimeSyncStatus(
            source="chrony",
            synchronized=False,
            offset_ms=-120.0,
            stratum=None,
        )

        assessment = monitor.assess(
            unsynchronized,
            component="vehicle-agent",
            vehicle_id="vehicle-001",
            session_id="",
            now_ms=2_000,
        )

        self.assertFalse(assessment.acceptable)
        self.assertEqual(assessment.log_event.level, "error")
        self.assertEqual(assessment.log_event.event, "time_sync_status")
        self.assertEqual(assessment.log_event.error_code, "time_not_synchronized")
        self.assertIn("minimum=ntp", assessment.log_event.message)
        self.assertIn("offset_ms=-120.000", assessment.log_event.message)


class WeakNetworkTests(unittest.TestCase):
    def test_weak_network_baseline_matches_documented_latency_loss_and_bandwidth_values(self):
        baseline = WeakNetworkBaseline.default()

        self.assertEqual(baseline.delay_ms, (50, 100, 200))
        self.assertEqual(baseline.jitter_ms, (20, 50))
        self.assertEqual(baseline.loss_percent, (1, 3, 5))
        self.assertEqual(baseline.bandwidth_mbps, (5, 10, 20))

    def test_weak_network_baseline_generates_full_documented_matrix(self):
        profiles = WeakNetworkBaseline.default().profiles()

        self.assertEqual(len(profiles), 54)
        self.assertEqual(profiles[0].name, "weak-50ms-jitter20-loss1-bandwidth5")
        self.assertTrue(
            any(profile.name == "weak-200ms-jitter50-loss5-bandwidth20" for profile in profiles)
        )

    def test_tc_netem_plan_generates_dry_run_apply_and_clear_commands(self):
        profile = WeakNetworkProfile(
            name="weak-100ms-jitter20-loss3-bandwidth10",
            delay_ms=100,
            jitter_ms=20,
            loss_percent=3,
            bandwidth_mbps=10,
        )

        plan = TcNetemPlan(interface="wwan0", profile=profile)

        self.assertEqual(
            plan.apply_command,
            "sudo tc qdisc add dev wwan0 root netem delay 100ms 20ms loss 3% rate 10mbit",
        )
        self.assertEqual(plan.clear_command, "sudo tc qdisc del dev wwan0 root")
        self.assertIn("dry-run", plan.warning)
        self.assertIn("confirm", plan.warning)


class CommandLineEntryPointTests(unittest.TestCase):
    def test_unified_cli_lists_and_dispatches_packaged_entrypoints(self):
        list_result = subprocess.run(
            [sys.executable, "-m", "mine_teleop.cli", "--list"],
            text=True,
            capture_output=True,
            check=True,
        )

        self.assertIn("vehicle-agent", list_result.stdout)
        self.assertIn("target-host-validation-plan", list_result.stdout)

        plan_result = subprocess.run(
            [sys.executable, "-m", "mine_teleop.cli", "target-host-validation-plan", "--format", "jsonl"],
            text=True,
            capture_output=True,
            check=True,
        )
        records = [json.loads(line) for line in plan_result.stdout.splitlines() if line.strip()]
        self.assertEqual(records[0]["event"], "target_host_validation_plan")
        self.assertIn("vehicle.preflight", records[0]["command_names"])

        agent_result = subprocess.run(
            [
                sys.executable,
                "-m",
                "mine_teleop.cli",
                "vehicle-agent",
                "--duration-ms",
                "100",
                "--disconnect-at-ms",
                "50",
            ],
            text=True,
            capture_output=True,
            check=True,
        )
        self.assertIn("final_state=", agent_result.stdout)

    def test_development_entrypoints_run_from_their_script_paths(self):
        scripts = [
            Path("vehicle-agent/vehicle_agent.py"),
            Path("vehicle-media-agent/vehicle_media_agent.py"),
            Path("driver-console/driver_console.py"),
            Path("signaling-server/signaling_server.py"),
        ]

        for script in scripts:
            with self.subTest(script=str(script)):
                result = subprocess.run(
                    [sys.executable, str(script)],
                    cwd=Path.cwd(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_vehicle_agent_prints_redacted_effective_config_on_startup(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "vehicle-agent.yaml"
            config_path.write_text(
                Path("configs/vehicle-agent.dev.yaml")
                .read_text(encoding="utf-8")
                .replace(
                    "  turn_servers: []",
                    """  turn_servers:
    - url: turn:turn.example.com:3478?transport=udp
      username: vehicle-001
      credential: super-secret-turn
""",
                ),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-agent/vehicle_agent.py",
                    "--config",
                    str(config_path),
                    "--duration-ms",
                    "0",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("super-secret-turn", result.stdout)
        first_record = json.loads(result.stdout.splitlines()[0])
        self.assertEqual(first_record["event"], "effective_vehicle_config")
        self.assertEqual(first_record["ice"]["turn_servers"][0]["credential"], "configured")

    def test_signaling_server_prints_redacted_effective_config_when_vehicle_config_is_loaded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            port_file = root / "port.txt"
            audit_log = root / "audit.jsonl"
            secret_path = root / "s3-secret.txt"
            config_path = root / "vehicle-agent.yaml"
            secret_path.write_text("dev-secret-key\n", encoding="utf-8")
            config_path.write_text(
                Path("configs/vehicle-agent.dev.yaml")
                .read_text(encoding="utf-8")
                .replace(
                    "  backend: local_archive\n",
                    f"""  backend: s3
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
            process = subprocess.Popen(
                [
                    sys.executable,
                    "signaling-server/signaling_server.py",
                    "--serve",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    "0",
                    "--port-file",
                    str(port_file),
                    "--audit-log",
                    str(audit_log),
                    "--vehicle-config",
                    str(config_path),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                _wait_for_port_file(port_file)
                first_line = process.stdout.readline()
            finally:
                process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)

        self.assertNotIn("dev-secret-key", first_line)
        self.assertNotIn("dev-session-token", first_line)
        self.assertNotIn(str(secret_path), first_line)
        first_record = json.loads(first_line)
        self.assertEqual(first_record["event"], "effective_vehicle_config")
        self.assertEqual(first_record["upload"]["s3"]["access_key_id"], "configured")
        self.assertEqual(first_record["upload"]["s3"]["secret_access_key_file"], "configured")
        self.assertEqual(first_record["upload"]["s3"]["session_token"], "configured")

    def test_acceptance_metrics_report_cli_emits_all_metric_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples = [
                {
                    "event": "video_sample",
                    "camera_id": "front",
                    "fps": 30,
                    "bitrate_kbps": 3000,
                    "end_to_end_latency_ms": 120,
                    "decoded_frames": 300,
                    "dropped_frames": 3,
                },
                {"event": "video_decode_failure", "camera_id": "front"},
                {"event": "video_reconnect", "camera_id": "front"},
                {"event": "control_driver_send", "seq": 1, "ts_ms": 0},
                {"event": "control_driver_send", "seq": 2, "ts_ms": 50},
                {"event": "control_receive", "accepted": True, "reason": "accepted", "receive_time_ms": 0},
                {"event": "control_receive", "accepted": True, "reason": "accepted", "receive_time_ms": 50},
                {"event": "control_receive", "accepted": False, "reason": "old_seq", "receive_time_ms": 75},
                {"event": "control_rtt", "rtt_ms": 60},
                {"event": "control_timeout", "last_valid_receive_ms": 50, "timeout_entered_ms": 850},
                {
                    "event": "control_brake_sample",
                    "now_ms": 850,
                    "stage": "timeout_brake_stage_0",
                    "speed_mps": 1.2,
                    "brake_feedback": 0.3,
                    "distance_since_last_valid_m": 0.9,
                },
                {
                    "event": "recording_segment",
                    "camera_id": "front",
                    "segment_id": "front-000001",
                    "segment_complete": True,
                    "metadata_complete": True,
                    "file_size_bytes": 1000,
                    "encoding_fps": 30,
                    "write_latency_ms": 12,
                    "disk_used_bytes_after": 2000,
                },
                {
                    "event": "recording_segment",
                    "camera_id": "front",
                    "segment_id": "front-000002",
                    "segment_complete": True,
                    "metadata_complete": True,
                    "file_size_bytes": 1200,
                    "encoding_fps": 29,
                    "write_latency_ms": 14,
                    "disk_used_bytes_after": 3200,
                },
                {"event": "upload_realtime_baseline", "camera_id": "front", "fps": 30, "bitrate_kbps": 3000},
                {"event": "upload_realtime_during_upload", "camera_id": "front", "fps": 28, "bitrate_kbps": 2600},
                {
                    "event": "upload_sample",
                    "segment_id": "front-000001",
                    "bytes_uploaded": 1_000_000,
                    "started_ms": 0,
                    "finished_ms": 1000,
                    "retry_count": 1,
                    "status": "uploaded",
                },
            ]
            samples_path.write_text(
                "\n".join(json.dumps(sample, sort_keys=True) for sample in samples),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(
            [record["event"] for record in records],
            [
                "acceptance_metrics_report",
                "video_acceptance_metrics",
                "control_acceptance_metrics",
                "recording_acceptance_metrics",
                "upload_acceptance_metrics",
            ],
        )
        summary = records[0]
        self.assertEqual(summary["scenario"], "weak-100ms-turn-relay")
        self.assertEqual(summary["sample_count"], len(samples))
        self.assertEqual(summary["report_count"], 4)
        self.assertEqual(records[1]["report"]["cameras"]["front"]["decode_failure_count"], 1)
        self.assertEqual(records[2]["report"]["command_out_of_order_count"], 1)
        self.assertEqual(records[2]["report"]["coast_distance_before_timeout_m"], 0.9)
        self.assertEqual(records[3]["report"]["cameras"]["front"]["disk_growth_bytes"], 1200)
        self.assertEqual(records[4]["report"]["retry_count_total"], 1)

    def test_acceptance_metrics_report_cli_marks_explicit_recording_and_upload_failures(self):
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples = [
                {
                    "event": "recording_segment",
                    "camera_id": "front",
                    "segment_id": "front-000001",
                    "segment_complete": False,
                    "metadata_complete": False,
                    "file_size_bytes": 1000,
                    "encoding_fps": 30,
                    "write_latency_ms": 12,
                    "disk_used_bytes_after": 2000,
                },
                {"event": "upload_realtime_baseline", "camera_id": "front", "fps": 30, "bitrate_kbps": 3000},
                {"event": "upload_realtime_during_upload", "camera_id": "front", "fps": 28, "bitrate_kbps": 2600},
                {
                    "event": "upload_sample",
                    "segment_id": "front-000001",
                    "bytes_uploaded": 1_000_000,
                    "started_ms": 0,
                    "finished_ms": 1000,
                    "retry_count": 1,
                    "status": "failed",
                    "failure_reason": "network_timeout",
                },
            ]
            samples_path.write_text(
                "\n".join(json.dumps(sample, sort_keys=True) for sample in samples),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "target-host-acceptance",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        summary, recording, upload = records
        self.assertFalse(summary["passed"])
        self.assertEqual(summary["failures"], recording["failures"] + upload["failures"])
        self.assertFalse(recording["passed"])
        self.assertIn("front recording segment incomplete", recording["failures"])
        self.assertIn("front recording metadata incomplete", recording["failures"])
        self.assertFalse(upload["passed"])
        self.assertEqual(upload["failures"], ["upload samples failed: network_timeout=1"])

    def test_acceptance_metrics_report_cli_rejects_non_boolean_control_receive_accepted(self):
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(
                json.dumps(
                    {
                        "event": "control_receive",
                        "accepted": "false",
                        "reason": "old_seq",
                        "receive_time_ms": 75,
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("control_receive.accepted must be a boolean", result.stderr)

    def test_acceptance_metrics_report_cli_rejects_non_boolean_recording_segment_flags(self):
        invalid_samples = [
            (
                {
                    "event": "recording_segment",
                    "camera_id": "front",
                    "segment_id": "front-000001",
                    "segment_complete": "false",
                    "metadata_complete": True,
                    "file_size_bytes": 1000,
                    "encoding_fps": 30,
                    "write_latency_ms": 12,
                    "disk_used_bytes_after": 2000,
                },
                "recording_segment.segment_complete must be a boolean",
            ),
            (
                {
                    "event": "recording_segment",
                    "camera_id": "front",
                    "segment_id": "front-000001",
                    "segment_complete": True,
                    "metadata_complete": "false",
                    "file_size_bytes": 1000,
                    "encoding_fps": 30,
                    "write_latency_ms": 12,
                    "disk_used_bytes_after": 2000,
                },
                "recording_segment.metadata_complete must be a boolean",
            ),
        ]

        for sample, expected_error in invalid_samples:
            with self.subTest(expected_error=expected_error), tempfile.TemporaryDirectory() as tmp:
                samples_path = Path(tmp) / "acceptance-samples.jsonl"
                samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

                result = subprocess.run(
                    [
                        sys.executable,
                        "scripts/acceptance_metrics_report.py",
                        "--samples",
                        str(samples_path),
                        "--scenario",
                        "weak-100ms-turn-relay",
                    ],
                    cwd=Path.cwd(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected_error, result.stderr)

    def test_acceptance_metrics_report_cli_rejects_implicitly_typed_sample_fields(self):
        invalid_samples = [
            (
                {
                    "event": "video_sample",
                    "camera_id": "front",
                    "fps": "30",
                    "bitrate_kbps": 3000,
                    "end_to_end_latency_ms": 120,
                    "decoded_frames": 300,
                    "dropped_frames": 3,
                },
                "video_sample.fps must be an integer",
            ),
            (
                {
                    "event": "upload_sample",
                    "segment_id": "front-000001",
                    "bytes_uploaded": 1_000_000,
                    "started_ms": 0,
                    "finished_ms": 1000,
                    "retry_count": 1,
                    "status": True,
                },
                "upload_sample.status must be a non-empty string",
            ),
        ]

        for sample, expected_error in invalid_samples:
            with self.subTest(expected_error=expected_error), tempfile.TemporaryDirectory() as tmp:
                samples_path = Path(tmp) / "acceptance-samples.jsonl"
                samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

                result = subprocess.run(
                    [
                        sys.executable,
                        "scripts/acceptance_metrics_report.py",
                        "--samples",
                        str(samples_path),
                        "--scenario",
                        "weak-100ms-turn-relay",
                    ],
                    cwd=Path.cwd(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected_error, result.stderr)

    def test_acceptance_metrics_report_cli_rejects_negative_sample_metrics(self):
        invalid_samples = [
            (
                {
                    "event": "control_rtt",
                    "rtt_ms": -1,
                },
                "control_rtt.rtt_ms must be a non-negative integer",
            ),
            (
                {
                    "event": "upload_sample",
                    "segment_id": "front-000001",
                    "bytes_uploaded": -1,
                    "started_ms": 0,
                    "finished_ms": 1000,
                    "retry_count": 1,
                    "status": "uploaded",
                },
                "upload_sample.bytes_uploaded must be a non-negative integer",
            ),
        ]

        for sample, expected_error in invalid_samples:
            with self.subTest(expected_error=expected_error), tempfile.TemporaryDirectory() as tmp:
                samples_path = Path(tmp) / "acceptance-samples.jsonl"
                samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

                result = subprocess.run(
                    [
                        sys.executable,
                        "scripts/acceptance_metrics_report.py",
                        "--samples",
                        str(samples_path),
                        "--scenario",
                        "weak-100ms-turn-relay",
                    ],
                    cwd=Path.cwd(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(expected_error, result.stderr)

    def test_acceptance_metrics_report_cli_rejects_reversed_control_timeout_sample(self):
        sample = {
            "event": "control_timeout",
            "last_valid_receive_ms": 850,
            "timeout_entered_ms": 849,
        }
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "control_timeout.timeout_entered_ms must not be earlier than last_valid_receive_ms",
            result.stderr,
        )

    def test_acceptance_metrics_report_cli_rejects_reversed_upload_sample_times(self):
        sample = {
            "event": "upload_sample",
            "segment_id": "front-000001",
            "bytes_uploaded": 1_000_000,
            "started_ms": 1000,
            "finished_ms": 999,
            "retry_count": 0,
            "status": "uploaded",
        }
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "upload_sample.finished_ms must not be earlier than started_ms",
            result.stderr,
        )

    def test_acceptance_metrics_report_cli_rejects_unknown_sample_event(self):
        samples = [
            {
                "event": "video_sample",
                "camera_id": "front",
                "fps": 30,
                "bitrate_kbps": 3000,
                "end_to_end_latency_ms": 120,
                "decoded_frames": 300,
                "dropped_frames": 3,
            },
            {
                "event": "upload_smaple",
                "segment_id": "front-000001",
            },
        ]
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(
                "\n".join(json.dumps(sample, sort_keys=True) for sample in samples),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("unknown acceptance sample event upload_smaple", result.stderr)

    def test_acceptance_metrics_report_cli_rejects_unknown_upload_sample_status(self):
        sample = {
            "event": "upload_sample",
            "segment_id": "front-000001",
            "bytes_uploaded": 1_000_000,
            "started_ms": 0,
            "finished_ms": 1000,
            "retry_count": 0,
            "status": "uploded",
        }
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("upload_sample.status must be uploaded or failed", result.stderr)

    def test_acceptance_metrics_report_cli_requires_failure_reason_for_failed_upload_sample(self):
        sample = {
            "event": "upload_sample",
            "segment_id": "front-000001",
            "bytes_uploaded": 1_000_000,
            "started_ms": 0,
            "finished_ms": 1000,
            "retry_count": 1,
            "status": "failed",
        }
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "upload_sample.failure_reason is required when status is failed",
            result.stderr,
        )

    def test_acceptance_metrics_report_cli_requires_reason_for_rejected_control_receive(self):
        sample = {
            "event": "control_receive",
            "accepted": False,
            "receive_time_ms": 75,
        }
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "control_receive.reason is required when accepted is false",
            result.stderr,
        )

    def test_acceptance_metrics_report_cli_requires_upload_realtime_baseline_and_during_pair(self):
        sample = {
            "event": "upload_realtime_during_upload",
            "camera_id": "front",
            "fps": 28,
            "bitrate_kbps": 2600,
        }
        with tempfile.TemporaryDirectory() as tmp:
            samples_path = Path(tmp) / "acceptance-samples.jsonl"
            samples_path.write_text(json.dumps(sample, sort_keys=True) + "\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/acceptance_metrics_report.py",
                    "--samples",
                    str(samples_path),
                    "--scenario",
                    "weak-100ms-turn-relay",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "upload realtime impact for camera front requires baseline and during_upload samples",
            result.stderr,
        )

    def test_driver_console_uses_driver_config(self):
        result = subprocess.run(
            [
                sys.executable,
                "driver-console/driver_console.py",
                "--config",
                "configs/driver-console.dev.yaml",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("driver=driver-console-001", result.stdout)
        command_lines = [line for line in result.stdout.splitlines() if line.startswith("{")]
        commands = [json.loads(line) for line in command_lines]
        self.assertEqual([command["seq"] for command in commands], [1, 2, 3])
        self.assertEqual(commands[0]["vehicle_id"], "vehicle-001")
        self.assertFalse(commands[0]["estop"])

    def test_driver_console_writes_local_operation_log_when_requested(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "driver-ops.jsonl"
            result = subprocess.run(
                [
                    sys.executable,
                    "driver-console/driver_console.py",
                    "--config",
                    "configs/driver-console.dev.yaml",
                    "--operation-log",
                    str(log_path),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            records = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines()]

        events = [record["event"] for record in records]
        self.assertIn("login_user", events)
        self.assertIn("connection_opened", events)
        self.assertIn("connection_reconnected", events)
        self.assertIn("session_started", events)
        self.assertIn("control_authority_acquired", events)
        self.assertIn("control_command_sent", events)
        self.assertIn("connection_closed", events)
        self.assertIn("logout_user", events)
        self.assertEqual(records[0]["driver_id"], "driver-console-001")
        self.assertEqual(records[0]["ui_version"], "grid_4")
        self.assertEqual(records[0]["config_version"], "configs/driver-console.dev.yaml")

    def test_driver_console_can_rotate_local_operation_log_when_requested(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            log_path = Path(tmpdir) / "driver-ops.jsonl"
            result = subprocess.run(
                [
                    sys.executable,
                    "driver-console/driver_console.py",
                    "--config",
                    "configs/driver-console.dev.yaml",
                    "--operation-log",
                    str(log_path),
                    "--operation-log-max-bytes",
                    "1",
                    "--operation-log-backup-count",
                    "1",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            current = json.loads(log_path.read_text(encoding="utf-8"))
            backup = json.loads(log_path.with_name("driver-ops.jsonl.1").read_text(encoding="utf-8"))

        self.assertEqual(current["event"], "logout_user")
        self.assertEqual(backup["event"], "connection_closed")

    def test_vehicle_media_agent_prints_realtime_pipeline_from_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "vehicle-agent.yaml"
            config_path.write_text(
                Path("configs/vehicle-agent.dev.yaml")
                .read_text(encoding="utf-8")
                .replace("keyframe_interval_frames: 30", "keyframe_interval_frames: 45")
                .replace("  - id: rear\n    enabled: false", "  - id: rear\n    enabled: true"),
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-media-agent/vehicle_media_agent.py",
                    "--config",
                    str(config_path),
                    "--mode",
                    "pipeline",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("camera=front", result.stdout)
        self.assertIn("camera=rear", result.stdout)
        self.assertGreaterEqual(result.stdout.count("webrtcbin name=webrtc"), 2)
        self.assertIn("webrtcbin name=webrtc", result.stdout)
        self.assertIn("vaapih264enc", result.stdout)
        self.assertIn("keyframe-period=45", result.stdout)

    def test_vehicle_agent_run_loop_reports_timeout_and_telemetry_count(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-agent/vehicle_agent.py",
                "--run-loop",
                "--duration-ms",
                "1500",
                "--disconnect-at-ms",
                "500",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("time_sync_status", result.stdout)
        self.assertIn("minimum=ntp", result.stdout)
        self.assertIn("final_state=TIMEOUT_BRAKE", result.stdout)
        self.assertIn("accepted_commands=10", result.stdout)
        self.assertIn("telemetry_count=", result.stdout)

    def test_vehicle_agent_preflight_cli_reports_missing_hardware_with_nonzero_exit(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            camera_device = root / "video0"
            render_device = root / "renderD128"
            missing_device = root / "missing-render"
            recording_root = root / "recordings"
            camera_device.write_text("", encoding="utf-8")
            render_device.write_text("", encoding="utf-8")
            recording_root.mkdir()
            config_path = root / "vehicle.yaml"
            config_path.write_text(
                Path("configs/vehicle-agent.dev.yaml")
                .read_text(encoding="utf-8")
                .replace("device: testsrc", f"device: {camera_device}", 1)
                .replace("root_dir: .local/recordings", f"root_dir: {recording_root}"),
                encoding="utf-8",
            )
            result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-agent/vehicle_agent.py",
                    "--config",
                    str(config_path),
                    "--preflight",
                    "--hardware-device",
                    str(render_device),
                    "--hardware-device",
                    str(missing_device),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertTrue(result.stdout.strip(), result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        preflight = next(record for record in records if record["event"] == "vehicle_preflight")
        self.assertFalse(preflight["ready"])
        checks = {record["name"]: record for record in records if "name" in record}
        self.assertEqual(checks["camera.front.device"]["status"], "ready")
        self.assertEqual(checks["recording.root_dir"]["status"], "ready")
        self.assertEqual(checks[f"hardware.{render_device}"]["status"], "ready")
        self.assertEqual(checks[f"hardware.{missing_device}"]["status"], "missing")

    def test_vehicle_agent_preflight_uses_configured_hardware_devices_by_default(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            camera_device = root / "video0"
            render_device = root / "renderD129"
            card_device = root / "card2"
            recording_root = root / "recordings"
            camera_device.write_text("", encoding="utf-8")
            render_device.write_text("", encoding="utf-8")
            card_device.write_text("", encoding="utf-8")
            recording_root.mkdir()
            config_path = root / "vehicle.yaml"
            content = Path("configs/vehicle-agent.dev.yaml").read_text(encoding="utf-8")
            content = content.replace("device: testsrc", f"device: {camera_device}", 1)
            content = content.replace("root_dir: .local/recordings", f"root_dir: {recording_root}")
            content = content.replace("/dev/dri/renderD128", str(render_device))
            content = content.replace("/dev/dri/card1", str(card_device))
            config_path.write_text(content, encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-agent/vehicle_agent.py",
                    "--config",
                    str(config_path),
                    "--preflight",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records if "name" in record}
        self.assertEqual(checks[f"hardware.{render_device}"]["status"], "ready")
        self.assertEqual(checks[f"hardware.{card_device}"]["status"], "ready")

    def test_vehicle_agent_adapter_status_cli_opens_configured_adapter_and_prints_json(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-agent/vehicle_agent.py",
                "--config",
                "configs/vehicle-agent.dev.yaml",
                "--adapter-status",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        status = next(record for record in records if record["event"] == "vehicle_adapter_status")
        self.assertEqual(status["vehicle_id"], "vehicle-001")
        self.assertTrue(status["ready"])
        self.assertEqual(status["status"]["adapter_type"], "mock")
        self.assertTrue(status["status"]["opened"])
        self.assertTrue(status["status"]["healthy"])
        self.assertEqual(status["status"]["applied_command_count"], 0)
        self.assertEqual(status["status"]["safe_stop_count"], 0)

    def test_vehicle_agent_adapter_status_cli_reports_feedback_poll_capability(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-agent/vehicle_agent.py",
                "--config",
                "configs/vehicle-agent.dev.yaml",
                "--adapter-status",
                "--poll-feedback",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        feedback = next(record for record in records if record["event"] == "vehicle_adapter_feedback_poll")
        self.assertEqual(feedback["vehicle_id"], "vehicle-001")
        self.assertFalse(feedback["attempted"])
        self.assertFalse(feedback["received"])
        self.assertEqual(feedback["reason"], "adapter_feedback_poll_not_supported")

    def test_vehicle_agent_adapter_status_cli_can_require_feedback_poll(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-agent/vehicle_agent.py",
                "--config",
                "configs/vehicle-agent.dev.yaml",
                "--adapter-status",
                "--require-feedback",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 2, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        status = next(record for record in records if record["event"] == "vehicle_adapter_status")
        feedback = next(record for record in records if record["event"] == "vehicle_adapter_feedback_poll")
        self.assertTrue(status["ready"])
        self.assertFalse(feedback["received"])
        self.assertEqual(feedback["reason"], "adapter_feedback_poll_not_supported")

    def test_vehicle_agent_default_demo_refuses_real_adapter_config(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            bridge_library = root / "libmine_teleop_chassis_bridge.so"
            chassis_library = minepilot_root / "libchassis_control.so"
            rendered_config = root / "vehicle-agent.chassis.yaml"
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
            render = subprocess.run(
                [
                    sys.executable,
                    "scripts/render_chassis_vehicle_config.py",
                    "--output",
                    str(rendered_config),
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
                    "bench-brake-test-2026-06-24",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(render.returncode, 0, render.stderr)

            result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-agent/vehicle_agent.py",
                    "--config",
                    str(rendered_config),
                    "--duration-ms",
                    "0",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        mode_error = next(record for record in records if record["event"] == "vehicle_agent_mode_error")
        self.assertEqual(mode_error["mode"], "mock_demo")
        self.assertEqual(mode_error["vehicle_adapter_type"], "can")
        self.assertEqual(mode_error["reason"], "mock_demo_requires_mock_adapter")

    def test_vehicle_agent_demo_honors_disconnect_argument(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-agent/vehicle_agent.py",
                "--duration-ms",
                "1500",
                "--disconnect-at-ms",
                "1000",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("accepted_commands=20", result.stdout)

    def test_vehicle_media_agent_prints_four_lane_vaapi_probe_by_default(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-media-agent/vehicle_media_agent.py",
                "--config",
                "configs/vehicle-agent.dev.yaml",
                "--mode",
                "vaapi-probe",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("-c:v h264_vaapi"), 4)
        self.assertIn("--device /dev/dri/renderD128", result.stdout)

    def test_vehicle_media_agent_prints_gstreamer_plugin_probe(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-media-agent/vehicle_media_agent.py",
                "--config",
                "configs/vehicle-agent.dev.yaml",
                "--mode",
                "gst-probe",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "gst-inspect-1.0 vaapih264enc qsvh264enc vah264enc nvh264enc x264enc",
            result.stdout,
        )

    def test_vehicle_media_agent_honors_configured_hardware_encoding_probe_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "vehicle-agent.yaml"
            content = Path("configs/vehicle-agent.dev.yaml").read_text(encoding="utf-8")
            content = content.replace("/dev/dri/renderD128", "/dev/dri/renderD129")
            content = content.replace("/dev/dri/card1", "/dev/dri/card2")
            content = content.replace("/tmp/mine-teleop-vaapi", "/tmp/custom-vaapi")
            content = content.replace("      - vaapih264enc", "      - customh264enc")
            content = content.replace("      - x264enc", "      - openh264enc")
            config_path.write_text(content, encoding="utf-8")

            vaapi_result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-media-agent/vehicle_media_agent.py",
                    "--config",
                    str(config_path),
                    "--mode",
                    "vaapi-probe",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            gst_result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-media-agent/vehicle_media_agent.py",
                    "--config",
                    str(config_path),
                    "--mode",
                    "gst-probe",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(vaapi_result.returncode, 0, vaapi_result.stderr)
        self.assertIn("--device /dev/dri/renderD129", vaapi_result.stdout)
        self.assertIn("--device /dev/dri/card2", vaapi_result.stdout)
        self.assertIn("-v /tmp/custom-vaapi:/out", vaapi_result.stdout)
        self.assertEqual(gst_result.returncode, 0, gst_result.stderr)
        self.assertIn("gst-inspect-1.0 customh264enc qsvh264enc vah264enc nvh264enc openh264enc", gst_result.stdout)

    def test_vehicle_media_agent_prints_hardware_encoding_validation_probes(self):
        result = subprocess.run(
            [
                sys.executable,
                "vehicle-media-agent/vehicle_media_agent.py",
                "--config",
                "configs/vehicle-agent.dev.yaml",
                "--mode",
                "hardware-probes",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gst_plugin_probe=", result.stdout)
        self.assertIn("scenario=four-camera-realtime-720p30", result.stdout)
        self.assertIn("scenario=four-camera-recording-source", result.stdout)
        self.assertIn("scenario=four-camera-realtime-plus-recording", result.stdout)
        self.assertIn(
            "metrics=cpu_percent,gpu_percent,memory_mb,disk_write_mb_s,temperature_c,encoded_fps,bitrate_kbps,dropped_frames",
            result.stdout,
        )

    def test_vehicle_media_agent_prints_hardware_encoding_validation_report(self):
        scenario = HardwareEncodingValidationPlan.four_camera_default().scenarios[0]
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            args = [
                sys.executable,
                "vehicle-media-agent/vehicle_media_agent.py",
                "--config",
                "configs/vehicle-agent.dev.yaml",
                "--mode",
                "hardware-report",
                "--scenario",
                scenario.name,
            ]
            for lane in scenario.lanes:
                output_path = root / f"{lane.lane_id}.txt"
                output_path.write_text(
                    "\n".join(
                        [
                            "codec_name=h264",
                            f"width={lane.width}",
                            f"height={lane.height}",
                            "avg_frame_rate=30/1",
                            "bit_rate=3000000",
                        ]
                    ),
                    encoding="utf-8",
                )
                args.extend(["--ffprobe-output", f"{lane.lane_id}={output_path}"])
            metrics_path = root / "metrics.json"
            metrics_path.write_text(
                json.dumps({"cpu_percent": 42.5, "gpu_percent": 71.0, "dropped_frames": 0}),
                encoding="utf-8",
            )
            args.extend(["--metrics-json", str(metrics_path)])

            result = subprocess.run(
                args,
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(records[0]["event"], "hardware_encoding_validation")
        self.assertTrue(records[0]["passed"])
        self.assertEqual(records[0]["lane_count"], 4)

    def test_signaling_server_cli_serves_health_endpoint(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            port_file = root / "port.txt"
            audit_log = root / "audit.jsonl"
            process = subprocess.Popen(
                [
                    sys.executable,
                    "signaling-server/signaling_server.py",
                    "--serve",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    "0",
                    "--port-file",
                    str(port_file),
                    "--audit-log",
                    str(audit_log),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                port = _wait_for_port_file(port_file)
                self.assertEqual(_json_get(f"http://127.0.0.1:{port}/health")["status"], "ok")
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

    def test_signaling_server_cli_can_rotate_audit_log_when_requested(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            port_file = root / "port.txt"
            audit_log = root / "audit.jsonl"
            process = subprocess.Popen(
                [
                    sys.executable,
                    "signaling-server/signaling_server.py",
                    "--serve",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    "0",
                    "--port-file",
                    str(port_file),
                    "--audit-log",
                    str(audit_log),
                    "--audit-log-max-bytes",
                    "1",
                    "--audit-log-backup-count",
                    "1",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.PIPE,
            )
            try:
                port = _wait_for_port_file(port_file)
                _json_post(
                    f"http://127.0.0.1:{port}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                login = _json_post(
                    f"http://127.0.0.1:{port}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                )
                _json_post(
                    f"http://127.0.0.1:{port}/sessions",
                    {"vehicle_id": "vehicle-001", "driver_id": "driver-a", "token": login["token"]},
                )
            finally:
                process.terminate()
                try:
                    process.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate(timeout=5)

            current = json.loads(audit_log.read_text(encoding="utf-8"))
            backup = json.loads(audit_log.with_name("audit.jsonl.1").read_text(encoding="utf-8"))

        self.assertEqual(current["event"], "control_authority_granted")
        self.assertEqual(backup["event"], "session_started")

    def test_signaling_server_cli_loads_driver_credentials_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            port_file = root / "port.txt"
            audit_log = root / "audit.jsonl"
            credentials_path = root / "driver-credentials.json"
            credential = DriverCredentialStore.hash_password("driver-secret", iterations=10)
            credentials_path.write_text(
                json.dumps({"drivers": {"driver-a": credential.__dict__}}, sort_keys=True),
                encoding="utf-8",
            )
            process = subprocess.Popen(
                [
                    sys.executable,
                    "signaling-server/signaling_server.py",
                    "--serve",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    "0",
                    "--port-file",
                    str(port_file),
                    "--audit-log",
                    str(audit_log),
                    "--driver-credentials",
                    str(credentials_path),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                port = _wait_for_port_file(port_file)
                wrong = _json_post_expect_error(
                    f"http://127.0.0.1:{port}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "dev-password"},
                    expected_status=401,
                )
                login = _json_post(
                    f"http://127.0.0.1:{port}/auth/driver_login",
                    {"driver_id": "driver-a", "password": "driver-secret"},
                )
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

        self.assertEqual(wrong["error"], "invalid driver credentials")
        self.assertEqual(login["token_type"], "bearer")
        self.assertTrue(login["token"].startswith("driver-token-"))
        self.assertNotIn("driver-a", login["token"])

    def test_signaling_server_cli_loads_device_credentials_file(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            port_file = root / "port.txt"
            audit_log = root / "audit.jsonl"
            credentials_path = root / "device-credentials.json"
            credentials_path.write_text(
                json.dumps({"vehicles": {"vehicle-002": "vehicle-002-secret"}}, sort_keys=True),
                encoding="utf-8",
            )
            process = subprocess.Popen(
                [
                    sys.executable,
                    "signaling-server/signaling_server.py",
                    "--serve",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    "0",
                    "--port-file",
                    str(port_file),
                    "--audit-log",
                    str(audit_log),
                    "--device-credentials",
                    str(credentials_path),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            try:
                port = _wait_for_port_file(port_file)
                wrong = _json_post_expect_error(
                    f"http://127.0.0.1:{port}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "dev-device-secret"},
                    expected_status=401,
                )
                online = _json_post(
                    f"http://127.0.0.1:{port}/vehicles/online",
                    {"vehicle_id": "vehicle-002", "device_token": "vehicle-002-secret"},
                )
            finally:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)

        self.assertEqual(wrong["error"], "invalid device token")
        self.assertEqual(online["vehicle_id"], "vehicle-002")
        self.assertEqual(online["state"], "online")

    def test_signaling_server_cli_requires_tls_for_non_loopback_bind(self):
        process = subprocess.Popen(
            [
                sys.executable,
                "signaling-server/signaling_server.py",
                "--serve",
                "--host",
                "0.0.0.0",
                "--port",
                "0",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            stdout, stderr = process.communicate(timeout=2)
        except subprocess.TimeoutExpired:
            process.terminate()
            process.wait(timeout=5)
            self.fail("signaling server should reject non-loopback plaintext bind before serving")

        self.assertNotEqual(process.returncode, 0, stdout)
        self.assertIn("--tls-cert and --tls-key are required for non-loopback hosts", stderr)

    def test_netem_plan_cli_prints_dry_run_commands_without_executing_tc(self):
        result = subprocess.run(
            [
                sys.executable,
                "scripts/netem_plan.py",
                "--interface",
                "wwan0",
                "--delay-ms",
                "100",
                "--jitter-ms",
                "20",
                "--loss-percent",
                "3",
                "--bandwidth-mbps",
                "10",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("dry-run", result.stdout)
        self.assertIn("sudo tc qdisc add dev wwan0 root netem delay 100ms 20ms loss 3% rate 10mbit", result.stdout)
        self.assertIn("sudo tc qdisc del dev wwan0 root", result.stdout)

    def test_netem_plan_cli_prints_full_matrix_without_executing_tc(self):
        result = subprocess.run(
            [
                sys.executable,
                "scripts/netem_plan.py",
                "--interface",
                "wwan0",
                "--matrix",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.count("apply="), 54)
        self.assertIn("profile=weak-200ms-jitter50-loss5-bandwidth20", result.stdout)


if __name__ == "__main__":
    unittest.main()
