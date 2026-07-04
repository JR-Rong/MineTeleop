import base64
import json
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path
from urllib import request

from mine_teleop.control_console_container import ContainerDriverConsoleSettings
from mine_teleop.driver_console_runtime import DriverConsoleHttpApp, DriverConsoleRuntime, RecordingControlCommandSink
from mine_teleop.signaling_service import SignalingHttpService


def _json_get(url: str) -> dict:
    with request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def _json_post(url: str, payload: dict) -> dict:
    body = json.dumps(payload).encode("utf-8")
    req = request.Request(url, data=body, method="POST", headers={"Content-Type": "application/json"})
    with request.urlopen(req, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


class DriverConsoleRuntimeTests(unittest.TestCase):
    def test_container_entrypoint_reads_deployment_environment(self):
        settings = ContainerDriverConsoleSettings.from_env(
            {
                "MINE_TELEOP_DRIVER_CONSOLE_CONFIG": "/etc/mine-teleop/driver-console.yaml",
                "MINE_TELEOP_DRIVER_CONSOLE_HOST": "0.0.0.0",
                "MINE_TELEOP_DRIVER_CONSOLE_PORT": "18080",
                "MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL": "https://teleop.example.com",
                "MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID": "vehicle-field-007",
                "MINE_TELEOP_DRIVER_CONSOLE_PASSWORD": "field-secret",
                "MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG": "/var/log/mine-teleop/driver-ops.jsonl",
                "MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG_MAX_BYTES": "10485760",
                "MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG_BACKUP_COUNT": "5",
                "MINE_TELEOP_DRIVER_CONSOLE_FRAME_DIR": "/var/lib/mine-teleop/frames",
                "MINE_TELEOP_DRIVER_CONSOLE_CONTROL_OUTPUT": "/var/log/mine-teleop/control.jsonl",
            }
        )

        self.assertEqual(settings.config_path, "/etc/mine-teleop/driver-console.yaml")
        self.assertEqual(settings.host, "0.0.0.0")
        self.assertEqual(settings.port, 18080)
        self.assertEqual(settings.signaling_http_url, "https://teleop.example.com")
        self.assertEqual(settings.vehicle_id, "vehicle-field-007")
        self.assertEqual(settings.password, "field-secret")
        self.assertEqual(settings.operation_log, "/var/log/mine-teleop/driver-ops.jsonl")
        self.assertEqual(settings.operation_log_max_bytes, 10485760)
        self.assertEqual(settings.operation_log_backup_count, 5)
        self.assertEqual(settings.frame_dir, "/var/lib/mine-teleop/frames")
        self.assertEqual(settings.control_output, "/var/log/mine-teleop/control.jsonl")

    def test_container_entrypoint_defaults_to_docker_control_program_contract(self):
        settings = ContainerDriverConsoleSettings.from_env({})

        self.assertEqual(settings.config_path, "configs/driver-console.dev.yaml")
        self.assertEqual(settings.host, "0.0.0.0")
        self.assertEqual(settings.port, 8080)
        self.assertEqual(settings.signaling_http_url, "")
        self.assertEqual(settings.vehicle_id, "vehicle-001")
        self.assertEqual(settings.password, "dev-password")
        self.assertEqual(settings.operation_log, "/tmp/mine-teleop-driver-console/operation-log.jsonl")
        self.assertEqual(settings.operation_log_max_bytes, 10485760)
        self.assertEqual(settings.operation_log_backup_count, 5)
        self.assertEqual(settings.frame_dir, "/tmp/mine-teleop-driver-console/frames")
        self.assertEqual(settings.control_output, "")

    def test_runtime_connects_receives_media_offer_and_exposes_operator_snapshot(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                sink = RecordingControlCommandSink()
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=base_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=sink,
                )

                runtime.connect()
                _json_post(
                    f"{base_url}/signaling/{runtime.session_id}/messages",
                    {
                        "sender": "vehicle-001",
                        "device_token": "dev-device-secret",
                        "recipient": "driver-console-001",
                        "type": "webrtc_offer",
                        "payload": {
                            "media_tracks": [
                                {
                                    "camera_id": "front",
                                    "codec": "h264",
                                    "width": 1280,
                                    "height": 720,
                                    "fps": 30,
                                    "bitrate_kbps": 2500,
                                }
                            ]
                        },
                    },
                )

                received = runtime.poll_signaling_once()
                snapshot = runtime.snapshot()

        self.assertEqual(received, 1)
        self.assertEqual(snapshot["session"]["state"], "SESSION_ACTIVE")
        self.assertEqual(snapshot["dashboard"]["cameras"]["front"]["state"], "connected")
        self.assertEqual(snapshot["dashboard"]["cameras"]["front"]["fps"], 30)
        self.assertEqual(snapshot["toolbar"]["actions"]["estop"]["enabled"], True)
        self.assertEqual(snapshot["status"]["bottom_bar"]["control_authority_state"], "active")

    def test_runtime_generates_control_commands_for_vehicle_transport(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                sink = RecordingControlCommandSink()
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=base_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=sink,
                )
                runtime.connect()

                first = runtime.send_control(
                    {"gear": "D", "throttle": 0.5, "steering": 0.25, "brake": 0.0},
                    now_ms=0,
                )
                too_soon = runtime.send_control(
                    {"gear": "D", "throttle": 0.75, "steering": 0.0, "brake": 0.0},
                    now_ms=20,
                )
                second = runtime.send_control(
                    {"gear": "D", "throttle": 0.75, "steering": 0.0, "brake": 0.0},
                    now_ms=50,
                )

        self.assertIsNotNone(first)
        self.assertIsNone(too_soon)
        self.assertIsNotNone(second)
        self.assertEqual(len(sink.commands), 2)
        self.assertEqual([command.seq for command in sink.commands], [1, 2])
        self.assertEqual(sink.commands[0].vehicle_id, "vehicle-001")
        self.assertEqual(sink.commands[0].session_id, runtime.session_id)
        self.assertEqual(sink.commands[0].authority_token, runtime.control_token)
        self.assertEqual(sink.commands[0].steering, 0.25)
        self.assertEqual(sink.commands[0].throttle, 0.5)
        self.assertEqual(sink.commands[1].throttle, 0.75)

    def test_runtime_default_transport_relays_control_command_to_vehicle_recipient(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=base_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                )
                runtime.connect()

                runtime.send_control({"gear": "D", "throttle": 0.5}, now_ms=0)
                messages = _json_get(
                    f"{base_url}/signaling/{runtime.session_id}/messages"
                    "?recipient=vehicle-001&device_token=dev-device-secret"
                )

        self.assertEqual(len(messages["messages"]), 1)
        message = messages["messages"][0]
        self.assertEqual(message["type"], "control_command")
        self.assertEqual(message["sender"], "driver-console-001")
        self.assertEqual(message["recipient"], "vehicle-001")
        self.assertEqual(message["payload"]["type"], "control_command")
        self.assertEqual(message["payload"]["vehicle_id"], "vehicle-001")
        self.assertEqual(message["payload"]["authority_token"], runtime.control_token)

    def test_runtime_relays_webrtc_answer_to_vehicle_recipient(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=base_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                )
                runtime.connect()

                result = runtime.send_webrtc_answer(
                    {"type": "answer", "sdp": "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102\r\n"}
                )
                messages = _json_get(
                    f"{base_url}/signaling/{runtime.session_id}/messages"
                    "?recipient=vehicle-001&device_token=dev-device-secret"
                )

        self.assertEqual(result["queued"], 1)
        self.assertEqual(len(messages["messages"]), 1)
        message = messages["messages"][0]
        self.assertEqual(message["type"], "webrtc_answer")
        self.assertEqual(message["sender"], "driver-console-001")
        self.assertEqual(message["recipient"], "vehicle-001")
        self.assertEqual(message["payload"]["type"], "answer")
        self.assertIn("m=video", message["payload"]["sdp"])

    def test_runtime_relays_webrtc_ice_candidate_to_vehicle_recipient(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as base_url:
                _json_post(
                    f"{base_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=base_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                )
                runtime.connect()

                result = runtime.send_webrtc_ice_candidate(
                    {
                        "candidate": "candidate:1 1 udp 2122260223 127.0.0.1 5000 typ host",
                        "sdpMid": "0",
                        "sdpMLineIndex": 0,
                    }
                )
                messages = _json_get(
                    f"{base_url}/signaling/{runtime.session_id}/messages"
                    "?recipient=vehicle-001&device_token=dev-device-secret"
                )

        self.assertEqual(result["queued"], 1)
        self.assertEqual(len(messages["messages"]), 1)
        message = messages["messages"][0]
        self.assertEqual(message["type"], "ice_candidate")
        self.assertEqual(message["sender"], "driver-console-001")
        self.assertEqual(message["recipient"], "vehicle-001")
        self.assertIn("candidate:1", message["payload"]["candidate"])


class DriverConsoleHttpAppTests(unittest.TestCase):
    def test_http_app_serves_operator_status_and_control_api(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                sink = RecordingControlCommandSink()
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=sink,
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    page = request.urlopen(f"{console_url}/", timeout=5).read().decode("utf-8")
                    connected = _json_post(f"{console_url}/api/connect", {})
                    _json_post(
                        f"{signaling_url}/signaling/{runtime.session_id}/messages",
                        {
                            "sender": "vehicle-001",
                            "device_token": "dev-device-secret",
                            "recipient": "driver-console-001",
                            "type": "webrtc_offer",
                            "payload": {
                                "media_tracks": [
                                    {
                                        "camera_id": "front",
                                        "codec": "h264",
                                        "width": 320,
                                        "height": 180,
                                        "fps": 15,
                                        "bitrate_kbps": 900,
                                    }
                                ]
                            },
                        },
                    )
                    polled = _json_post(f"{console_url}/api/poll-signaling", {})
                    control = _json_post(
                        f"{console_url}/api/control",
                        {"gear": "D", "throttle": 0.4, "steering": 0.1, "now_ms": 0},
                    )
                    status = _json_get(f"{console_url}/api/status")

        self.assertIn("Mine Teleop Driver Console", page)
        self.assertEqual(connected["session"]["state"], "SESSION_ACTIVE")
        self.assertEqual(polled["received_messages"], 1)
        self.assertEqual(control["command"]["seq"], 1)
        self.assertEqual(status["dashboard"]["cameras"]["front"]["state"], "connected")
        self.assertEqual(status["dashboard"]["cameras"]["front"]["bitrate_kbps"], 900)
        self.assertEqual(len(sink.commands), 1)

    def test_http_connect_accepts_operator_vehicle_and_password_payload(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-not-selected",
                    password="wrong-default-password",
                    control_sink=RecordingControlCommandSink(),
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    connected = _json_post(
                        f"{console_url}/api/connect",
                        {"vehicle_id": "vehicle-001", "password": "dev-password"},
                    )

        self.assertEqual(connected["session"]["state"], "SESSION_ACTIVE")
        self.assertEqual(connected["vehicle_id"], "vehicle-001")
        self.assertEqual(runtime.vehicle_id, "vehicle-001")
        self.assertEqual(runtime.password, "dev-password")

    def test_http_connect_rejects_non_string_operator_payload_fields(self):
        runtime = DriverConsoleRuntime.from_config(
            "configs/driver-console.dev.yaml",
            signaling_http_url="http://127.0.0.1:8765",
            vehicle_id="vehicle-001",
            password="dev-password",
            control_sink=RecordingControlCommandSink(),
        )
        app = DriverConsoleHttpApp(runtime)
        with app.running("127.0.0.1", 0) as console_url:
            body = json.dumps({"vehicle_id": 7, "password": "dev-password"}).encode("utf-8")
            req = request.Request(
                f"{console_url}/api/connect",
                data=body,
                method="POST",
                headers={"Content-Type": "application/json"},
            )
            with self.assertRaises(Exception) as caught:
                request.urlopen(req, timeout=5)

        self.assertIn("HTTP Error 400", str(caught.exception))

    def test_http_page_exposes_operator_connection_form(self):
        runtime = DriverConsoleRuntime.from_config(
            "configs/driver-console.dev.yaml",
            signaling_http_url="http://127.0.0.1:8765",
            vehicle_id="vehicle-001",
            password="dev-password",
            control_sink=RecordingControlCommandSink(),
        )
        app = DriverConsoleHttpApp(runtime)
        with app.running("127.0.0.1", 0) as console_url:
            page = request.urlopen(f"{console_url}/", timeout=5).read().decode("utf-8")

        self.assertIn('id="connect-vehicle-id"', page)
        self.assertIn('id="connect-password"', page)
        self.assertIn("function connectPayloadFromForm()", page)
        self.assertIn("postJson('/api/connect', connectPayloadFromForm())", page)

    def test_http_page_wires_keyboard_events_to_20hz_control_loop(self):
        runtime = DriverConsoleRuntime.from_config(
            "configs/driver-console.dev.yaml",
            signaling_http_url="http://127.0.0.1:8765",
            vehicle_id="vehicle-001",
            password="dev-password",
            control_sink=RecordingControlCommandSink(),
        )
        app = DriverConsoleHttpApp(runtime)
        with app.running("127.0.0.1", 0) as console_url:
            page = request.urlopen(f"{console_url}/", timeout=5).read().decode("utf-8")

        self.assertIn("addEventListener('keydown'", page)
        self.assertIn("addEventListener('keyup'", page)
        self.assertIn("setInterval(sendKeyboardControl, 50)", page)
        self.assertIn("/api/control/keyboard", page)

    def test_http_page_wires_browser_webrtc_media_and_control_datachannel(self):
        runtime = DriverConsoleRuntime.from_config(
            "configs/driver-console.dev.yaml",
            signaling_http_url="http://127.0.0.1:8765",
            vehicle_id="vehicle-001",
            password="dev-password",
            control_sink=RecordingControlCommandSink(),
        )
        app = DriverConsoleHttpApp(runtime)
        with app.running("127.0.0.1", 0) as console_url:
            page = request.urlopen(f"{console_url}/", timeout=5).read().decode("utf-8")

        self.assertIn("new RTCPeerConnection", page)
        self.assertIn("createDataChannel('control'", page)
        self.assertIn("ordered: false", page)
        self.assertIn("maxRetransmits: 0", page)
        self.assertIn("setRemoteDescription", page)
        self.assertIn("createAnswer", page)
        self.assertIn("/api/webrtc/answer", page)
        self.assertIn("/api/webrtc/ice-candidate", page)
        self.assertIn("addIceCandidate", page)
        self.assertIn("setInterval(pollSignaling, 1000)", page)
        self.assertIn("ontrack", page)
        self.assertIn("controlDataChannel.send", page)

    def test_http_page_renders_operator_status_panel_without_relying_on_raw_json(self):
        runtime = DriverConsoleRuntime.from_config(
            "configs/driver-console.dev.yaml",
            signaling_http_url="http://127.0.0.1:8765",
            vehicle_id="vehicle-001",
            password="dev-password",
            control_sink=RecordingControlCommandSink(),
        )
        app = DriverConsoleHttpApp(runtime)
        with app.running("127.0.0.1", 0) as console_url:
            page = request.urlopen(f"{console_url}/", timeout=5).read().decode("utf-8")

        self.assertIn('id="operator-session-state"', page)
        self.assertIn('id="operator-session-id"', page)
        self.assertIn('id="operator-control-authority"', page)
        self.assertIn('id="operator-signaling-state"', page)
        self.assertIn('id="operator-camera-summary"', page)
        self.assertIn('id="operator-command-summary"', page)
        self.assertIn('id="operator-webrtc-state"', page)
        self.assertIn('id="operator-datachannel-state"', page)
        self.assertIn("function renderOperatorStatus(data)", page)
        self.assertIn("renderOperatorStatus(data);", page)
        self.assertIn("function updateDataChannelState(state)", page)
        self.assertIn("operatorPanelState", page)

    def test_http_page_wires_gamepad_api_to_20hz_control_loop(self):
        runtime = DriverConsoleRuntime.from_config(
            "configs/driver-console.dev.yaml",
            signaling_http_url="http://127.0.0.1:8765",
            vehicle_id="vehicle-001",
            password="dev-password",
            control_sink=RecordingControlCommandSink(),
        )
        app = DriverConsoleHttpApp(runtime)
        with app.running("127.0.0.1", 0) as console_url:
            page = request.urlopen(f"{console_url}/", timeout=5).read().decode("utf-8")

        self.assertIn("navigator.getGamepads", page)
        self.assertIn("gamepadMapping", page)
        self.assertIn("setInterval(sendGamepadControl, 50)", page)
        self.assertIn("/api/control/gamepad", page)

    def test_http_app_maps_keyboard_state_to_20hz_control_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                sink = RecordingControlCommandSink()
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=sink,
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    _json_post(f"{console_url}/api/connect", {})
                    first = _json_post(
                        f"{console_url}/api/control/keyboard",
                        {"keys": ["ArrowUp", "ArrowRight"], "gear": "D", "now_ms": 0},
                    )
                    too_soon = _json_post(
                        f"{console_url}/api/control/keyboard",
                        {"keys": ["ArrowUp", "ArrowRight"], "gear": "D", "now_ms": 20},
                    )
                    brake = _json_post(
                        f"{console_url}/api/control/keyboard",
                        {"keys": ["ArrowUp", "ArrowDown"], "gear": "D", "now_ms": 50},
                    )

        self.assertTrue(first["sent"])
        self.assertFalse(too_soon["sent"])
        self.assertTrue(brake["sent"])
        self.assertEqual(len(sink.commands), 2)
        self.assertEqual(sink.commands[0].throttle, 1.0)
        self.assertEqual(sink.commands[0].steering, 1.0)
        self.assertEqual(sink.commands[0].brake, 0.0)
        self.assertEqual(sink.commands[1].throttle, 0.0)
        self.assertEqual(sink.commands[1].brake, 1.0)
        self.assertEqual([command.seq for command in sink.commands], [1, 2])

    def test_http_poll_signaling_returns_offer_payload_for_browser_webrtc(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=RecordingControlCommandSink(),
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    _json_post(f"{console_url}/api/connect", {})
                    _json_post(
                        f"{signaling_url}/signaling/{runtime.session_id}/messages",
                        {
                            "sender": "vehicle-001",
                            "device_token": "dev-device-secret",
                            "recipient": "driver-console-001",
                            "type": "webrtc_offer",
                            "payload": {
                                "type": "offer",
                                "sdp": "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102\r\n",
                                "media_tracks": [
                                    {
                                        "camera_id": "front",
                                        "codec": "h264",
                                        "width": 320,
                                        "height": 180,
                                        "fps": 15,
                                        "bitrate_kbps": 900,
                                    }
                                ],
                            },
                        },
                    )
                    polled = _json_post(f"{console_url}/api/poll-signaling", {})

        self.assertEqual(polled["received_messages"], 1)
        self.assertEqual(len(polled["messages"]), 1)
        self.assertEqual(polled["messages"][0]["type"], "webrtc_offer")
        self.assertEqual(polled["messages"][0]["payload"]["type"], "offer")
        self.assertIn("m=video", polled["messages"][0]["payload"]["sdp"])
        self.assertEqual(polled["snapshot"]["dashboard"]["cameras"]["front"]["state"], "connected")

    def test_http_app_relays_browser_webrtc_answer_to_vehicle(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=RecordingControlCommandSink(),
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    _json_post(f"{console_url}/api/connect", {})
                    answer = _json_post(
                        f"{console_url}/api/webrtc/answer",
                        {"type": "answer", "sdp": "v=0\r\nm=video 9 UDP/TLS/RTP/SAVPF 102\r\n"},
                    )
                    messages = _json_get(
                        f"{signaling_url}/signaling/{runtime.session_id}/messages"
                        "?recipient=vehicle-001&device_token=dev-device-secret"
                    )

        self.assertEqual(answer["queued"], 1)
        self.assertEqual(messages["messages"][0]["type"], "webrtc_answer")
        self.assertEqual(messages["messages"][0]["payload"]["type"], "answer")

    def test_http_poll_signaling_returns_remote_ice_candidate_for_browser_webrtc(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=RecordingControlCommandSink(),
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    _json_post(f"{console_url}/api/connect", {})
                    _json_post(
                        f"{signaling_url}/signaling/{runtime.session_id}/messages",
                        {
                            "sender": "vehicle-001",
                            "device_token": "dev-device-secret",
                            "recipient": "driver-console-001",
                            "type": "ice_candidate",
                            "payload": {
                                "candidate": "candidate:2 1 udp 2122260223 127.0.0.1 5001 typ host",
                                "sdpMid": "0",
                                "sdpMLineIndex": 0,
                            },
                        },
                    )
                    polled = _json_post(f"{console_url}/api/poll-signaling", {})

        self.assertEqual(polled["received_messages"], 1)
        self.assertEqual(polled["messages"][0]["type"], "ice_candidate")
        self.assertIn("candidate:2", polled["messages"][0]["payload"]["candidate"])

    def test_http_app_relays_browser_webrtc_ice_candidate_to_vehicle(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=RecordingControlCommandSink(),
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    _json_post(f"{console_url}/api/connect", {})
                    candidate = _json_post(
                        f"{console_url}/api/webrtc/ice-candidate",
                        {
                            "candidate": {
                                "candidate": "candidate:3 1 udp 2122260223 127.0.0.1 5002 typ host",
                                "sdpMid": "0",
                                "sdpMLineIndex": 0,
                            }
                        },
                    )
                    messages = _json_get(
                        f"{signaling_url}/signaling/{runtime.session_id}/messages"
                        "?recipient=vehicle-001&device_token=dev-device-secret"
                    )

        self.assertEqual(candidate["queued"], 1)
        self.assertEqual(messages["messages"][0]["type"], "ice_candidate")
        self.assertIn("candidate:3", messages["messages"][0]["payload"]["candidate"])

    def test_http_app_maps_gamepad_axes_to_20hz_control_commands(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                sink = RecordingControlCommandSink()
                runtime = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                    control_sink=sink,
                )
                app = DriverConsoleHttpApp(runtime)
                with app.running("127.0.0.1", 0) as console_url:
                    _json_post(f"{console_url}/api/connect", {})
                    first = _json_post(
                        f"{console_url}/api/control/gamepad",
                        {
                            "steering_axis": -0.35,
                            "throttle_axis": 0.65,
                            "brake_axis": 0.0,
                            "gear": "D",
                            "now_ms": 0,
                        },
                    )
                    too_soon = _json_post(
                        f"{console_url}/api/control/gamepad",
                        {
                            "steering_axis": 0.25,
                            "throttle_axis": 0.8,
                            "brake_axis": 0.0,
                            "gear": "D",
                            "now_ms": 20,
                        },
                    )
                    brake = _json_post(
                        f"{console_url}/api/control/gamepad",
                        {
                            "steering_axis": 0.2,
                            "throttle_axis": 0.9,
                            "brake_axis": 0.4,
                            "gear": "D",
                            "now_ms": 50,
                        },
                    )

        self.assertTrue(first["sent"])
        self.assertFalse(too_soon["sent"])
        self.assertTrue(brake["sent"])
        self.assertEqual(len(sink.commands), 2)
        self.assertEqual(sink.commands[0].steering, -0.35)
        self.assertEqual(sink.commands[0].throttle, 0.65)
        self.assertEqual(sink.commands[0].brake, 0.0)
        self.assertEqual(sink.commands[1].steering, 0.2)
        self.assertEqual(sink.commands[1].throttle, 0.0)
        self.assertEqual(sink.commands[1].brake, 0.4)
        self.assertEqual([command.seq for command in sink.commands], [1, 2])

    def test_http_app_decodes_h264_frame_and_serves_png(self):
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            self.skipTest("ffmpeg is required for H.264 decode smoke")
        with tempfile.TemporaryDirectory() as tmp:
            frame_dir = Path(tmp) / "frames"
            encoded_path = Path(tmp) / "front.h264"
            subprocess.run(
                [
                    ffmpeg,
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-y",
                    "-f",
                    "lavfi",
                    "-i",
                    "testsrc2=size=160x90:rate=1",
                    "-frames:v",
                    "1",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "ultrafast",
                    "-tune",
                    "zerolatency",
                    "-f",
                    "h264",
                    str(encoded_path),
                ],
                check=True,
            )
            runtime = DriverConsoleRuntime.from_config(
                "configs/driver-console.dev.yaml",
                signaling_http_url="http://127.0.0.1:8765",
                vehicle_id="vehicle-001",
                password="dev-password",
                control_sink=RecordingControlCommandSink(),
                frame_dir=frame_dir,
            )
            app = DriverConsoleHttpApp(runtime)
            with app.running("127.0.0.1", 0) as console_url:
                decoded = _json_post(
                    f"{console_url}/api/media/frame",
                    {
                        "camera_id": "front",
                        "codec": "h264",
                        "payload_base64": base64.b64encode(encoded_path.read_bytes()).decode("ascii"),
                    },
                )
                decoded_again = _json_post(
                    f"{console_url}/api/media/frame",
                    {
                        "camera_id": "front",
                        "codec": "h264",
                        "payload_base64": base64.b64encode(encoded_path.read_bytes()).decode("ascii"),
                    },
                )
                png = request.urlopen(f"{console_url}/api/frame/front.png", timeout=5).read()
                cache_busted_png = request.urlopen(f"{console_url}/api/frame/front.png?ts=1", timeout=5).read()
                status = _json_get(f"{console_url}/api/status")

        self.assertTrue(decoded["frame_received"])
        self.assertEqual(decoded["frame_sequence"], 1)
        self.assertEqual(decoded_again["frame_sequence"], 2)
        self.assertGreater(decoded["frame_size_bytes"], 0)
        self.assertTrue(png.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertTrue(cache_busted_png.startswith(b"\x89PNG\r\n\x1a\n"))
        self.assertEqual(status["dashboard"]["cameras"]["front"]["state"], "connected")
        self.assertEqual(status["dashboard"]["cameras"]["front"]["message"], "decoded_frame_received")
        self.assertEqual(status["decoded_frame_count_by_camera"]["front"], 2)

    def test_http_app_records_media_frame_latency(self):
        ffmpeg = shutil.which("ffmpeg")
        if not ffmpeg:
            self.skipTest("ffmpeg is required for H.264 decode smoke")
        with tempfile.TemporaryDirectory() as tmp:
            frame_dir = Path(tmp) / "frames"
            encoded_path = Path(tmp) / "front.h264"
            subprocess.run(
                [
                    ffmpeg,
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-y",
                    "-f",
                    "lavfi",
                    "-i",
                    "testsrc2=size=160x90:rate=1",
                    "-frames:v",
                    "1",
                    "-c:v",
                    "libx264",
                    "-preset",
                    "ultrafast",
                    "-tune",
                    "zerolatency",
                    "-f",
                    "h264",
                    str(encoded_path),
                ],
                check=True,
            )
            runtime = DriverConsoleRuntime.from_config(
                "configs/driver-console.dev.yaml",
                signaling_http_url="http://127.0.0.1:8765",
                vehicle_id="vehicle-001",
                password="dev-password",
                control_sink=RecordingControlCommandSink(),
                frame_dir=frame_dir,
            )
            captured_at_ms = int(time.time() * 1000) - 120
            encoded_at_ms = captured_at_ms + 20
            sent_at_ms = captured_at_ms + 40
            app = DriverConsoleHttpApp(runtime)
            with app.running("127.0.0.1", 0) as console_url:
                decoded = _json_post(
                    f"{console_url}/api/media/frame",
                    {
                        "camera_id": "front",
                        "codec": "h264",
                        "payload_base64": base64.b64encode(encoded_path.read_bytes()).decode("ascii"),
                        "captured_at_ms": captured_at_ms,
                        "encoded_at_ms": encoded_at_ms,
                        "sent_at_ms": sent_at_ms,
                    },
                )
                status = _json_get(f"{console_url}/api/status")

        self.assertTrue(decoded["frame_received"])
        self.assertGreaterEqual(decoded["end_to_end_latency_ms"], 0)
        self.assertGreaterEqual(decoded["transport_latency_ms"], 0)
        self.assertGreaterEqual(decoded["decode_latency_ms"], 0)
        self.assertEqual(decoded["encode_latency_ms"], 20)
        self.assertEqual(status["dashboard"]["cameras"]["front"]["latency_ms"], decoded["end_to_end_latency_ms"])

    def test_driver_console_cli_can_serve_http_control_program(self):
        with tempfile.TemporaryDirectory() as tmp:
            port_file = Path(tmp) / "driver-console.port"
            control_output = Path(tmp) / "control.jsonl"
            proc = subprocess.Popen(
                [
                    sys.executable,
                    "driver-console/driver_console.py",
                    "--config",
                    "configs/driver-console.dev.yaml",
                    "--serve",
                    "--host",
                    "127.0.0.1",
                    "--port",
                    "0",
                    "--port-file",
                    str(port_file),
                    "--signaling-http-url",
                    "http://127.0.0.1:8765",
                    "--vehicle-id",
                    "vehicle-001",
                    "--password",
                    "dev-password",
                    "--control-output",
                    str(control_output),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            try:
                deadline = time.time() + 5
                while time.time() < deadline and not port_file.exists():
                    time.sleep(0.05)
                self.assertTrue(port_file.exists(), "driver-console did not write a port file")
                port = int(port_file.read_text(encoding="utf-8"))
                health = _json_get(f"http://127.0.0.1:{port}/health")
            finally:
                proc.terminate()
                try:
                    proc.communicate(timeout=5)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    proc.communicate(timeout=5)

        self.assertEqual(health, {"status": "ok"})


if __name__ == "__main__":
    unittest.main()
