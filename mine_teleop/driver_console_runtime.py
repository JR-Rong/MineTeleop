from __future__ import annotations

import base64
import html
import json
import shutil
import subprocess
import threading
import time
from collections import deque
from contextlib import contextmanager
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Iterator, Protocol
from urllib import request
from urllib.parse import urlencode, urlparse, urlunparse

from .config import DriverConfig, load_driver_config
from .control import ControlCommand
from .driver_console import (
    CameraDisplayStatus,
    ControlCommandGenerator,
    DriverConsoleStatusSnapshot,
    DriverInputMerger,
    DriverOperationEvent,
    DriverOperationLog,
    DriverToolbarSnapshot,
    DriverVideoDashboard,
    InputState,
    SoftwareControlState,
)


# Cap the console request body so a malicious/buggy client cannot force an
# unbounded allocation. Generous enough for a base64 720p intra frame.
MAX_CONSOLE_BODY_BYTES = 8 * 1024 * 1024


class ControlCommandSink(Protocol):
    def send(self, command: ControlCommand) -> None:
        ...


class RecordingControlCommandSink:
    def __init__(self) -> None:
        self.commands: list[ControlCommand] = []

    def send(self, command: ControlCommand) -> None:
        command.validate()
        self.commands.append(command)


class JsonlControlCommandSink:
    def __init__(self, path: str | Path) -> None:
        self.path = Path(path)

    def send(self, command: ControlCommand) -> None:
        command.validate()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(command.to_dict(), ensure_ascii=False, sort_keys=True) + "\n")


class SignalingControlCommandSink:
    def __init__(
        self,
        signaling_http_url: str,
        driver_id: str,
        vehicle_id: str,
        token: str,
    ) -> None:
        self.signaling_http_url = signaling_http_url.rstrip("/")
        self.driver_id = driver_id
        self.vehicle_id = vehicle_id
        self.token = token

    def send(self, command: ControlCommand) -> None:
        command.validate()
        _json_post(
            f"{self.signaling_http_url}/signaling/{command.session_id}/messages",
            {
                "sender": self.driver_id,
                "token": self.token,
                "recipient": self.vehicle_id,
                "type": "control_command",
                "payload": command.to_dict(),
            },
        )


class DriverConsoleRuntime:
    def __init__(
        self,
        config: DriverConfig,
        *,
        signaling_http_url: str,
        vehicle_id: str,
        password: str,
        control_sink: ControlCommandSink | None = None,
        camera_ids: tuple[str, ...] = (),
        operation_log: DriverOperationLog | None = None,
        config_version: str = "",
        frame_dir: str | Path = "/tmp/mine-teleop-driver-console/frames",
    ) -> None:
        self.config = config
        self.signaling_http_url = _normalize_signaling_http_url(signaling_http_url)
        self.vehicle_id = vehicle_id
        self.password = password
        self.driver_id = config.driver_id
        self.control_sink = control_sink
        self.dashboard = DriverVideoDashboard(camera_ids=camera_ids, layout=config.ui.default_layout)
        self.operation_log = operation_log
        self.config_version = config_version
        self.frame_dir = Path(frame_dir)
        self.latest_frame_by_camera: dict[str, Path] = {}
        self.latest_frame_content_type_by_camera: dict[str, str] = {}
        self.latest_frame_timing_by_camera: dict[str, dict[str, Any]] = {}
        self.decoded_frame_count_by_camera: dict[str, int] = {}
        self.frame_receive_times_by_camera: dict[str, deque[int]] = {}
        self.token = ""
        self.session_id = ""
        self.control_token = ""
        self._last_signaling_messages: list[dict[str, Any]] = []
        self._generator: ControlCommandGenerator | None = None
        self._last_command: ControlCommand | None = None
        self._latest_telemetry: dict[str, Any] = {
            "session_id": "",
            "gear": "N",
            "link": {"signaling_connected": False, "control_rtt_ms": 0},
        }

    @classmethod
    def from_config(
        cls,
        path: str | Path,
        *,
        signaling_http_url: str | None = None,
        vehicle_id: str,
        password: str,
        control_sink: ControlCommandSink | None = None,
        operation_log: DriverOperationLog | None = None,
        frame_dir: str | Path = "/tmp/mine-teleop-driver-console/frames",
    ) -> "DriverConsoleRuntime":
        config_path = Path(path)
        config = load_driver_config(config_path)
        return cls(
            config,
            signaling_http_url=signaling_http_url or config.cloud.signaling_url,
            vehicle_id=vehicle_id,
            password=password,
            control_sink=control_sink,
            operation_log=operation_log,
            config_version=str(path),
            frame_dir=frame_dir,
        )

    def connect(
        self,
        *,
        vehicle_id: str | None = None,
        password: str | None = None,
        now_ms: int | None = None,
    ) -> None:
        timestamp = _now_ms() if now_ms is None else now_ms
        if vehicle_id is not None:
            self.vehicle_id = _require_non_empty_string(vehicle_id, "vehicle_id")
        if password is not None:
            self.password = _require_non_empty_string(password, "password")
        login = _json_post(
            f"{self.signaling_http_url}/auth/driver_login",
            {"driver_id": self.driver_id, "password": self.password},
        )
        self.token = str(login["token"])
        session = _json_post(
            f"{self.signaling_http_url}/sessions",
            {"vehicle_id": self.vehicle_id, "driver_id": self.driver_id, "token": self.token},
        )
        self.session_id = str(session["session_id"])
        self.control_token = str(session["control_token"])
        self._generator = ControlCommandGenerator(
            self.vehicle_id,
            self.session_id,
            rate_hz=self.config.control.rate_hz,
            control_token=self.control_token,
        )
        if self.control_sink is None:
            self.control_sink = SignalingControlCommandSink(
                self.signaling_http_url,
                driver_id=self.driver_id,
                vehicle_id=self.vehicle_id,
                token=self.token,
            )
        self._latest_telemetry = {
            "session_id": self.session_id,
            "gear": "N",
            "link": {"signaling_connected": True, "control_rtt_ms": 0},
        }
        self._append_operation(timestamp, "login_user", {"result": "success"})
        self._append_operation(timestamp, "connection_opened", {"signaling_url": self.signaling_http_url})
        self._append_operation(timestamp, "session_started", {"vehicle_id": self.vehicle_id})
        self._append_operation(
            timestamp,
            "control_authority_acquired",
            {"rate_hz": self.config.control.rate_hz},
        )

    def poll_signaling_messages_once(self) -> list[dict[str, Any]]:
        self._require_connected()
        query = urlencode({"recipient": self.driver_id, "token": self.token})
        payload = _json_get(f"{self.signaling_http_url}/signaling/{self.session_id}/messages?{query}")
        messages = payload.get("messages", [])
        if not isinstance(messages, list):
            raise RuntimeError("signaling messages response must contain a messages list")
        self._last_signaling_messages = [dict(message) for message in messages if isinstance(message, dict)]
        for message in messages:
            self._handle_signaling_message(message)
        return list(self._last_signaling_messages)

    def poll_signaling_once(self) -> int:
        messages = self.poll_signaling_messages_once()
        return len(messages)

    def send_webrtc_answer(self, answer: dict[str, Any]) -> dict[str, Any]:
        payload = _require_session_description(answer, expected_type="answer")
        return self._send_signaling_message("webrtc_answer", payload)

    def send_webrtc_ice_candidate(self, candidate: dict[str, Any]) -> dict[str, Any]:
        if not isinstance(candidate, dict) or not candidate:
            raise ValueError("ICE candidate payload must be a non-empty object")
        return self._send_signaling_message("ice_candidate", candidate)

    def send_control(self, state: dict[str, Any], *, now_ms: int | None = None) -> ControlCommand | None:
        self._require_connected()
        timestamp = _now_ms() if now_ms is None else now_ms
        keyboard = InputState(gear=str(state.get("gear", "N")), window_focused=bool(state.get("window_focused", True)))
        software = SoftwareControlState(
            steering=_optional_axis(state.get("steering")),
            throttle=_optional_axis(state.get("throttle")),
            brake=_optional_axis(state.get("brake")),
            gear=str(state.get("gear", "N")),
            estop_pressed=bool(state.get("estop", False)),
        )
        return self._send_input_state(DriverInputMerger.merge(keyboard, software), now_ms=timestamp)

    def send_keyboard_control(
        self,
        keys: list[str],
        *,
        gear: str = "N",
        window_focused: bool = True,
        now_ms: int | None = None,
    ) -> ControlCommand | None:
        self._require_connected()
        timestamp = _now_ms() if now_ms is None else now_ms
        pressed = {str(key).upper() for key in keys}
        keyboard = self.config.control.keyboard
        return self._send_input_state(
            InputState(
                steering_left=keyboard.steering_left.upper() in pressed,
                steering_right=keyboard.steering_right.upper() in pressed,
                throttle_pressed=keyboard.throttle.upper() in pressed,
                brake_pressed=keyboard.brake.upper() in pressed,
                estop_pressed=keyboard.estop.upper() in pressed,
                window_focused=window_focused,
                gear=str(gear).upper(),
            ),
            now_ms=timestamp,
        )

    def send_gamepad_control(
        self,
        *,
        steering_axis: float,
        throttle_axis: float,
        brake_axis: float,
        gear: str = "N",
        estop: bool = False,
        window_focused: bool = True,
        now_ms: int | None = None,
    ) -> ControlCommand | None:
        self._require_connected()
        if not self.config.control.gamepad.enabled:
            raise RuntimeError("gamepad control is disabled by driver configuration")
        timestamp = _now_ms() if now_ms is None else now_ms
        return self._send_input_state(
            InputState(
                steering_axis=float(steering_axis),
                throttle_axis=float(throttle_axis),
                brake_axis=float(brake_axis),
                estop_pressed=bool(estop),
                window_focused=window_focused,
                gear=str(gear).upper(),
            ),
            now_ms=timestamp,
        )

    def _send_input_state(self, input_state: InputState, *, now_ms: int) -> ControlCommand | None:
        if self._generator is None or self.control_sink is None:
            raise RuntimeError("driver console runtime is not ready to send control")
        command = self._generator.next_command(input_state, now_ms=now_ms)
        if command is None:
            return None
        self.control_sink.send(command)
        self._last_command = command
        self._latest_telemetry = self._latest_telemetry | {
            "gear": command.gear,
            "steering_feedback": command.steering,
            "throttle_feedback": command.throttle,
            "brake_feedback": command.brake,
            "estop": command.estop,
            "link": {"signaling_connected": True, "control_rtt_ms": 0},
        }
        self._append_operation(now_ms, "control_command_sent", {"seq": command.seq, "estop": command.estop})
        if command.estop:
            self._append_operation(now_ms, "estop_sent", {"seq": command.seq})
        return command

    def _send_signaling_message(self, message_type: str, payload: dict[str, Any]) -> dict[str, Any]:
        self._require_connected()
        return _json_post(
            f"{self.signaling_http_url}/signaling/{self.session_id}/messages",
            {
                "sender": self.driver_id,
                "token": self.token,
                "recipient": self.vehicle_id,
                "type": message_type,
                "payload": payload,
            },
        )

    def snapshot(self) -> dict[str, Any]:
        logged_in = bool(self.token)
        session_active = bool(self.session_id)
        telemetry = dict(self._latest_telemetry)
        telemetry["session_id"] = self.session_id
        status = DriverConsoleStatusSnapshot.from_telemetry(
            telemetry=telemetry,
            dashboard=self.dashboard,
            control_authority_state="active" if session_active else "inactive",
            packet_loss_percent=0.0,
        ).to_dict()
        return {
            "driver_id": self.driver_id,
            "vehicle_id": self.vehicle_id,
            "session": {
                "session_id": self.session_id,
                "state": "SESSION_ACTIVE" if session_active else "DISCONNECTED",
                "control_token_present": bool(self.control_token),
            },
            "dashboard": self.dashboard.to_dict(),
            "toolbar": DriverToolbarSnapshot.from_state(
                logged_in=logged_in,
                connected=logged_in,
                session_active=session_active,
            ).to_dict(),
            "status": status,
            "decoded_frame_count_by_camera": dict(self.decoded_frame_count_by_camera),
            "latest_frame_timing_by_camera": dict(self.latest_frame_timing_by_camera),
            "last_command": self._last_command.to_dict() if self._last_command else None,
        }

    def ingest_encoded_frame(
        self,
        camera_id: str,
        codec: str,
        payload: bytes,
        *,
        captured_at_ms: int | None = None,
        encoded_at_ms: int | None = None,
        sent_at_ms: int | None = None,
        received_at_ms: int | None = None,
        clock_offset_ms: int = 0,
    ) -> dict[str, Any]:
        if not camera_id:
            raise ValueError("camera_id is required")
        codec_lower = codec.lower()
        if codec_lower not in {"h264", "mjpeg", "jpeg"}:
            raise ValueError("only h264 and mjpeg encoded frames are supported")
        if not payload:
            raise ValueError("encoded frame payload is empty")
        ffmpeg = shutil.which("ffmpeg") if codec_lower == "h264" else None
        if codec_lower == "h264" and not ffmpeg:
            raise RuntimeError("ffmpeg is required to decode control-console H.264 frames")
        receive_time_ms = _now_ms() if received_at_ms is None else _optional_non_negative_int(received_at_ms, "received_at_ms")
        captured_at_ms = _optional_non_negative_int(captured_at_ms, "captured_at_ms")
        encoded_at_ms = _optional_non_negative_int(encoded_at_ms, "encoded_at_ms")
        sent_at_ms = _optional_non_negative_int(sent_at_ms, "sent_at_ms")
        self._ensure_camera(camera_id)
        self.frame_dir.mkdir(parents=True, exist_ok=True)
        if codec_lower == "h264":
            encoded_path = self.frame_dir / f"{camera_id}.h264"
            frame_path = self.frame_dir / f"{camera_id}.png"
            encoded_path.write_bytes(payload)
            subprocess.run(
                [
                    ffmpeg,
                    "-hide_banner",
                    "-loglevel",
                    "error",
                    "-y",
                    "-f",
                    "h264",
                    "-i",
                    str(encoded_path),
                    "-frames:v",
                    "1",
                    str(frame_path),
                ],
                check=True,
            )
            decoded_at_ms = _now_ms()
            decode_latency_ms = max(0, decoded_at_ms - receive_time_ms)
            content_type = "image/png"
        else:
            if not (payload.startswith(b"\xff\xd8") and payload.endswith(b"\xff\xd9")):
                raise ValueError("mjpeg frame payload must contain a complete JPEG image")
            frame_path = self.frame_dir / f"{camera_id}.jpg"
            frame_path.write_bytes(payload)
            decoded_at_ms = receive_time_ms
            decode_latency_ms = 0
            content_type = "image/jpeg"
        size_bytes = frame_path.stat().st_size
        if size_bytes <= 0:
            raise RuntimeError("decoded frame is empty")
        current = self.dashboard.camera_status[camera_id]
        # Express the vehicle-side capture/send timestamps in the console clock
        # domain before computing cross-machine latency (H1). encode/decode
        # latency are intra-host and need no correction.
        captured_console_ms = None if captured_at_ms is None else captured_at_ms + clock_offset_ms
        sent_console_ms = None if sent_at_ms is None else sent_at_ms + clock_offset_ms
        latency = _non_negative_delta(decoded_at_ms, captured_console_ms)
        received_fps = self._record_frame_receive(camera_id, receive_time_ms)
        display_fps = current.fps
        if received_fps > 0:
            display_fps = max(1, int(round(received_fps)))
        self.dashboard.update_camera_status(
            camera_id,
            state="connected",
            fps=display_fps,
            bitrate_kbps=current.bitrate_kbps,
            latency_ms=latency if latency is not None else current.latency_ms,
            message="decoded_frame_received",
        )
        self.latest_frame_by_camera[camera_id] = frame_path
        self.latest_frame_content_type_by_camera[camera_id] = content_type
        frame_sequence = self.decoded_frame_count_by_camera.get(camera_id, 0) + 1
        self.decoded_frame_count_by_camera[camera_id] = frame_sequence
        timing = {
            "captured_at_ms": captured_at_ms,
            "encoded_at_ms": encoded_at_ms,
            "sent_at_ms": sent_at_ms,
            "received_at_ms": receive_time_ms,
            "decoded_at_ms": decoded_at_ms,
            "encode_latency_ms": _non_negative_delta(encoded_at_ms, captured_at_ms),
            "transport_latency_ms": _non_negative_delta(receive_time_ms, sent_console_ms),
            "decode_latency_ms": decode_latency_ms,
            "end_to_end_latency_ms": latency,
            "received_fps": round(received_fps, 2),
        }
        timing_snapshot = {
            "camera_id": camera_id,
            "frame_sequence": frame_sequence,
            "frame_size_bytes": size_bytes,
            "codec": codec_lower,
        } | {key: value for key, value in timing.items() if value is not None}
        self.latest_frame_timing_by_camera[camera_id] = timing_snapshot
        return {
            "camera_id": camera_id,
            "codec": codec_lower,
            "frame_received": True,
            "frame_sequence": frame_sequence,
            "frame_path": str(frame_path),
            "frame_size_bytes": size_bytes,
        } | {key: value for key, value in timing.items() if value is not None}

    def read_decoded_frame(self, camera_id: str) -> bytes:
        frame_path = self.latest_frame_by_camera.get(camera_id)
        if frame_path is None or not frame_path.is_file():
            raise FileNotFoundError(f"no decoded frame for camera {camera_id}")
        return frame_path.read_bytes()

    def read_decoded_frame_content_type(self, camera_id: str) -> str:
        return self.latest_frame_content_type_by_camera.get(camera_id, "image/png")

    def _handle_signaling_message(self, message: dict[str, Any]) -> None:
        if message.get("type") != "webrtc_offer":
            return
        payload = message.get("payload", {})
        if not isinstance(payload, dict):
            raise RuntimeError("webrtc_offer payload must be an object")
        for track in payload.get("media_tracks", []):
            if not isinstance(track, dict):
                continue
            camera_id = str(track.get("camera_id", ""))
            if not camera_id:
                continue
            self._ensure_camera(camera_id)
            self.dashboard.update_camera_status(
                camera_id,
                state="connected",
                fps=int(track.get("fps", 0)),
                bitrate_kbps=int(track.get("bitrate_kbps", 0)),
                latency_ms=track.get("latency_ms"),
                message="webrtc_offer_received",
            )

    def _ensure_camera(self, camera_id: str) -> None:
        if camera_id not in self.dashboard.camera_status:
            self.dashboard.camera_status[camera_id] = CameraDisplayStatus(camera_id=camera_id)

    def _record_frame_receive(self, camera_id: str, received_at_ms: int) -> float:
        samples = self.frame_receive_times_by_camera.setdefault(camera_id, deque())
        samples.append(received_at_ms)
        cutoff_ms = received_at_ms - 5000
        while samples and samples[0] < cutoff_ms:
            samples.popleft()
        while len(samples) > 120:
            samples.popleft()
        if len(samples) < 2:
            return 0.0
        elapsed_ms = max(1, samples[-1] - samples[0])
        return (len(samples) - 1) * 1000.0 / elapsed_ms

    def _require_connected(self) -> None:
        if not self.session_id or not self.token:
            raise RuntimeError("driver console runtime is not connected")

    def _append_operation(self, ts_ms: int, event: str, details: dict[str, Any]) -> None:
        if self.operation_log is None:
            return
        self.operation_log.append(
            DriverOperationEvent(
                ts_ms=ts_ms,
                event=event,
                driver_id=self.driver_id,
                vehicle_id=self.vehicle_id,
                session_id=self.session_id,
                ui_version=self.config.ui.default_layout,
                config_version=self.config_version,
                details=details,
            )
        )


class DriverConsoleHttpApp:
    def __init__(self, runtime: DriverConsoleRuntime) -> None:
        self.runtime = runtime

    def make_server(self, host: str = "127.0.0.1", port: int = 8080) -> ThreadingHTTPServer:
        return ThreadingHTTPServer((host, port), self._make_handler())

    @contextmanager
    def running(self, host: str = "127.0.0.1", port: int = 0) -> Iterator[str]:
        server = self.make_server(host, port)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            actual_host, actual_port = server.server_address
            yield f"http://{actual_host}:{actual_port}"
        finally:
            server.shutdown()
            thread.join(timeout=5)
            server.server_close()

    def _make_handler(self):
        app = self

        class Handler(BaseHTTPRequestHandler):
            # HTTP/1.1 so the vehicle media sender can reuse one keep-alive
            # connection across frames instead of a fresh TCP handshake per frame.
            protocol_version = "HTTP/1.1"

            def do_GET(self) -> None:
                path = urlparse(self.path).path
                if path == "/health":
                    self._json_response(200, {"status": "ok"})
                    return
                if path == "/api/time":
                    self._json_response(200, {"now_ms": _now_ms()})
                    return
                if path == "/api/status":
                    self._json_response(200, app.runtime.snapshot())
                    return
                if path == "/":
                    body = _control_console_html(app.runtime).encode("utf-8")
                    self.send_response(200)
                    self.send_header("Content-Type", "text/html; charset=utf-8")
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                if path.startswith("/api/frame/"):
                    segment = path[len("/api/frame/") :]
                    camera_id = segment
                    for ext in (".png", ".jpg", ".jpeg"):
                        if segment.endswith(ext):
                            camera_id = segment[: -len(ext)]
                            break
                    try:
                        body = app.runtime.read_decoded_frame(camera_id)
                    except FileNotFoundError as exc:
                        self._json_response(404, {"error": str(exc)})
                        return
                    self.send_response(200)
                    self.send_header("Content-Type", app.runtime.read_decoded_frame_content_type(camera_id))
                    self.send_header("Content-Length", str(len(body)))
                    self.end_headers()
                    self.wfile.write(body)
                    return
                self._json_response(404, {"error": "not found"})

            def do_POST(self) -> None:
                try:
                    payload = self._read_json()
                    if self.path == "/api/connect":
                        app.runtime.connect(
                            vehicle_id=_optional_payload_string(payload, "vehicle_id"),
                            password=_optional_payload_string(payload, "password"),
                        )
                        self._json_response(200, app.runtime.snapshot())
                        return
                    if self.path == "/api/poll-signaling":
                        messages = app.runtime.poll_signaling_messages_once()
                        self._json_response(
                            200,
                            {
                                "received_messages": len(messages),
                                "messages": messages,
                                "snapshot": app.runtime.snapshot(),
                            },
                        )
                        return
                    if self.path == "/api/webrtc/answer":
                        result = app.runtime.send_webrtc_answer(payload)
                        self._json_response(200, result)
                        return
                    if self.path == "/api/webrtc/ice-candidate":
                        candidate = payload.get("candidate", payload)
                        if not isinstance(candidate, dict):
                            raise ValueError("candidate must be an object")
                        result = app.runtime.send_webrtc_ice_candidate(candidate)
                        self._json_response(200, result)
                        return
                    if self.path == "/api/control":
                        state = dict(payload)
                        now_ms = state.pop("now_ms", None)
                        command = app.runtime.send_control(
                            state,
                            now_ms=int(now_ms) if now_ms is not None else None,
                        )
                        self._json_response(
                            200,
                            {
                                "sent": command is not None,
                                "command": command.to_dict() if command else None,
                                "snapshot": app.runtime.snapshot(),
                            },
                        )
                        return
                    if self.path == "/api/control/keyboard":
                        keys = payload.get("keys", [])
                        if not isinstance(keys, list):
                            raise ValueError("keys must be a list")
                        now_ms = payload.get("now_ms")
                        command = app.runtime.send_keyboard_control(
                            [str(key) for key in keys],
                            gear=str(payload.get("gear", "N")),
                            window_focused=bool(payload.get("window_focused", True)),
                            now_ms=int(now_ms) if now_ms is not None else None,
                        )
                        self._json_response(
                            200,
                            {
                                "sent": command is not None,
                                "command": command.to_dict() if command else None,
                                "snapshot": app.runtime.snapshot(),
                            },
                        )
                        return
                    if self.path == "/api/control/gamepad":
                        now_ms = payload.get("now_ms")
                        command = app.runtime.send_gamepad_control(
                            steering_axis=float(payload.get("steering_axis", 0.0)),
                            throttle_axis=float(payload.get("throttle_axis", 0.0)),
                            brake_axis=float(payload.get("brake_axis", 0.0)),
                            gear=str(payload.get("gear", "N")),
                            estop=bool(payload.get("estop", False)),
                            window_focused=bool(payload.get("window_focused", True)),
                            now_ms=int(now_ms) if now_ms is not None else None,
                        )
                        self._json_response(
                            200,
                            {
                                "sent": command is not None,
                                "command": command.to_dict() if command else None,
                                "snapshot": app.runtime.snapshot(),
                            },
                        )
                        return
                    if self.path == "/api/media/frame":
                        camera_id = str(payload.get("camera_id", ""))
                        codec = str(payload.get("codec", ""))
                        payload_base64 = payload.get("payload_base64", "")
                        if not isinstance(payload_base64, str):
                            raise ValueError("payload_base64 must be a string")
                        frame = app.runtime.ingest_encoded_frame(
                            camera_id,
                            codec,
                            base64.b64decode(payload_base64.encode("ascii"), validate=True),
                            captured_at_ms=_optional_payload_int(payload, "captured_at_ms"),
                            encoded_at_ms=_optional_payload_int(payload, "encoded_at_ms"),
                            sent_at_ms=_optional_payload_int(payload, "sent_at_ms"),
                            clock_offset_ms=_optional_signed_int(payload, "clock_offset_ms"),
                        )
                        self._json_response(200, frame)
                        return
                    self._json_response(404, {"error": "not found"})
                except Exception as exc:
                    self._json_response(400, {"error": str(exc)})

            def log_message(self, format: str, *args: Any) -> None:
                return

            def _read_json(self) -> dict[str, Any]:
                raw_length = self.headers.get("Content-Length", "0")
                try:
                    length = int(raw_length)
                except ValueError:
                    raise ValueError("invalid Content-Length header")
                if length < 0:
                    raise ValueError("invalid Content-Length header")
                if length > MAX_CONSOLE_BODY_BYTES:
                    raise ValueError("request body too large")
                raw = self.rfile.read(length).decode("utf-8") if length else "{}"
                data = json.loads(raw)
                if not isinstance(data, dict):
                    raise ValueError("JSON body must be an object")
                return data

            def _json_response(self, status: int, payload: dict[str, Any]) -> None:
                body = json.dumps(payload, ensure_ascii=False, sort_keys=True).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        return Handler

def _control_console_html(runtime: DriverConsoleRuntime) -> str:
    gamepad = runtime.config.control.gamepad
    gamepad_mapping = {
        "enabled": gamepad.enabled,
        "steering_axis": gamepad.steering_axis,
        "throttle_axis": gamepad.throttle_axis,
        "brake_axis": gamepad.brake_axis,
        "axis_deadzone": gamepad.axis_deadzone,
        "throttle_inverted": gamepad.throttle_inverted,
        "brake_inverted": gamepad.brake_inverted,
        "estop_button": gamepad.estop_button,
    }
    html_text = _CONTROL_CONSOLE_HTML.replace(
        "__GAMEPAD_MAPPING_JSON__",
        json.dumps(gamepad_mapping, ensure_ascii=False, sort_keys=True),
    )
    return (
        html_text.replace("__DEFAULT_VEHICLE_ID__", html.escape(runtime.vehicle_id, quote=True))
        .replace("__DEFAULT_PASSWORD__", html.escape(runtime.password, quote=True))
    )


_CONTROL_CONSOLE_HTML = """<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Mine Teleop Driver Console</title>
  <style>
    :root { color-scheme: dark; font-family: Arial, sans-serif; background: #151819; color: #f3f5f2; }
    body { margin: 0; height: 100vh; overflow: hidden; display: grid; grid-template-rows: auto minmax(0, 1fr) auto; }
    header, footer { display: flex; gap: 8px; align-items: center; padding: 8px 10px; background: #202624; border-bottom: 1px solid #39413d; }
    header { flex-wrap: wrap; }
    footer { border-top: 1px solid #39413d; border-bottom: 0; font-size: 12px; color: #c6cec8; min-height: 18px; }
    main { min-height: 0; display: grid; grid-template-columns: minmax(0, 1fr) 280px; gap: 8px; padding: 8px; overflow: hidden; }
    .grid { height: 100%; min-height: 0; display: grid; grid-template-columns: repeat(12, minmax(0, 1fr)); grid-template-rows: repeat(10, minmax(0, 1fr)); gap: 8px; }
    .camera { border: 1px solid #404b46; background: #0b0d0d; min-height: 0; position: relative; overflow: hidden; }
    .camera.dragging { outline: 2px solid #b8d8c3; outline-offset: -2px; }
    .camera-frame { position: absolute; inset: 0; display: grid; place-items: center; min-height: 0; color: #9fa9a3; font-size: 14px; --camera-brightness: 1; --camera-contrast: 1; }
    .camera-frame img, .camera-frame video { width: 100%; height: 100%; max-width: 100%; max-height: 100%; object-fit: contain; filter: brightness(var(--camera-brightness, 1)) contrast(var(--camera-contrast, 1)); transition: filter 180ms ease; }
    .camera-title { position: absolute; left: 0; right: 0; top: 0; z-index: 2; padding: 6px 8px 3px; background: linear-gradient(to bottom, rgba(31, 37, 35, 0.88), rgba(31, 37, 35, 0.35)); font-size: 12px; pointer-events: none; }
    .camera-meta { position: absolute; left: 0; right: 0; bottom: 0; z-index: 2; background: linear-gradient(to top, rgba(15, 18, 17, 0.92), rgba(15, 18, 17, 0.72)); padding: 5px 8px 6px; display: grid; gap: 3px; font-size: 10px; color: #c6cec8; max-height: 48%; overflow: auto; }
    .camera-meta .latency-total { color: #f3f5f2; font-weight: 700; }
    .timing-grid { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 3px 8px; }
    .timing-grid span { overflow-wrap: anywhere; }
    .camera-manual-remove { position: absolute; top: 8px; left: 8px; width: 28px; height: 28px; padding: 0; display: none; z-index: 3; background: rgba(56, 34, 34, 0.92); border-color: #8f5a5a; }
    body.layout-editing .camera.manual .camera-manual-remove { display: block; }
    .camera-layout-controls { position: absolute; top: 8px; right: 8px; display: none; grid-template-columns: repeat(4, 28px); gap: 4px; z-index: 3; }
    body.layout-editing .camera-layout-controls { display: grid; }
    .camera-layout-controls button { width: 28px; height: 28px; padding: 0; font-size: 11px; background: rgba(39, 48, 44, 0.92); }
    .camera-resize-grip { position: absolute; right: 0; bottom: 0; width: 26px; height: 26px; display: none; cursor: nwse-resize; z-index: 2; }
    .camera-resize-grip::after { content: ""; position: absolute; right: 6px; bottom: 6px; width: 10px; height: 10px; border-right: 2px solid #b8d8c3; border-bottom: 2px solid #b8d8c3; }
    body.layout-editing .camera-resize-grip { display: block; }
    aside { min-height: 0; max-height: 100%; overflow: auto; display: grid; gap: 8px; align-content: start; }
    .operator-panel { display: grid; grid-template-columns: repeat(2, minmax(0, 1fr)); gap: 6px; }
    .status-tile { border: 1px solid #3d4843; background: #111514; padding: 7px; min-height: 48px; display: grid; gap: 3px; }
    .status-tile span { color: #9fa9a3; font-size: 11px; text-transform: uppercase; }
    .status-tile strong { font-size: 14px; line-height: 1.2; overflow-wrap: anywhere; }
    .status-tile small { color: #c6cec8; overflow-wrap: anywhere; }
    button, select, input { height: 30px; background: #27302c; color: #f3f5f2; border: 1px solid #526058; border-radius: 6px; padding: 0 8px; }
    button { cursor: pointer; }
    button.estop { background: #8f1f1f; border-color: #c34242; font-weight: 700; }
    label { display: grid; gap: 4px; color: #c6cec8; font-size: 12px; }
    pre { margin: 0; padding: 8px; background: #101312; border: 1px solid #303934; overflow: auto; max-height: 150px; font-size: 11px; }
    @media (max-width: 900px) {
      body { height: auto; min-height: 100vh; overflow: auto; }
      main { grid-template-columns: 1fr; }
      .grid { min-height: 720px; grid-template-rows: repeat(10, minmax(52px, 1fr)); }
    }
  </style>
</head>
<body>
  <header>
    <strong>Mine Teleop Driver Console</strong>
    <label>Vehicle <input id="connect-vehicle-id" value="__DEFAULT_VEHICLE_ID__"></label>
    <label>Password <input id="connect-password" type="password" value="__DEFAULT_PASSWORD__"></label>
    <button onclick="connectConsole()">Connect</button>
    <button onclick="pollSignaling()">Poll Video</button>
    <button id="layout-edit-button" onclick="toggleCameraLayoutEdit()">Edit Layout</button>
    <button onclick="applySurroundLayout()">Surround</button>
    <button onclick="resetCameraLayout()">Reset Layout</button>
    <label>Camera <input id="manual-camera-id" placeholder="front"></label>
    <button onclick="addManualCameraFromForm()">Add Camera</button>
    <button class="estop" onclick="sendControl({estop:true, brake:1, gear:'N'})">ESTOP</button>
  </header>
  <main>
    <section id="cameras" class="grid"></section>
    <aside>
      <section class="operator-panel" aria-label="operator status">
        <div class="status-tile"><span>Session</span><strong id="operator-session-state">DISCONNECTED</strong><small id="operator-session-id">-</small></div>
        <div class="status-tile"><span>Authority</span><strong id="operator-control-authority">inactive</strong><small id="operator-signaling-state">disconnected</small></div>
        <div class="status-tile"><span>Cameras</span><strong id="operator-camera-summary">0/0 connected</strong><small id="operator-webrtc-state">idle</small></div>
        <div class="status-tile"><span>Control</span><strong id="operator-command-summary">no command</strong><small id="operator-datachannel-state">closed</small></div>
      </section>
      <label>Gear <select id="gear"><option>N</option><option>D</option><option>R</option><option>P</option></select></label>
      <label>Steering <input id="steering" type="range" min="-1" max="1" step="0.05" value="0"></label>
      <label>Throttle <input id="throttle" type="range" min="0" max="1" step="0.05" value="0"></label>
      <label>Brake <input id="brake" type="range" min="0" max="1" step="0.05" value="0"></label>
      <button onclick="sendCurrentControl()">Send Control</button>
      <pre id="status">{}</pre>
    </aside>
  </main>
  <footer id="footer">disconnected</footer>
  <script>
    let keyboardControlEnabled = false;
    let lastGamepadSeenMs = 0;
    let peerConnection = null;
    let controlDataChannel = null;
    let remoteCameraIds = [];
    let signalingPollEnabled = false;
    const pendingRemoteIceCandidates = [];
    const pressedKeys = new Set();
    const remoteStreamsByCamera = new Map();
    const gamepadMapping = __GAMEPAD_MAPPING_JSON__;
    const operatorPanelState = {dataChannel: 'closed', webrtc: 'idle'};
    const knownCameraIds = new Set();
    const lastRenderedFrameSequenceByCamera = {};
    const CAMERA_LAYOUT_STORAGE_KEY = 'mineTeleopCameraLayoutV3';
    const CAMERA_MANUAL_STORAGE_KEY = 'mineTeleopManualCameraIdsV1';
    const CAMERA_GRID_COLUMNS = 12;
    const CAMERA_GRID_ROWS = 10;
    const manualCameraIds = new Set(loadManualCameraIds());
    let cameraLayoutEditEnabled = false;
    let cameraLayoutByCamera = loadCameraLayout();
    let cameraLayoutDrag = null;
    let lastSnapshot = null;
    const brightnessCanvas = document.createElement('canvas');
    const brightnessContext = brightnessCanvas.getContext('2d', {willReadFrequently: true});
    async function postJson(path, payload) {
      const res = await fetch(path, {method: 'POST', headers: {'Content-Type': 'application/json'}, body: JSON.stringify(payload)});
      const data = await res.json();
      if (!res.ok) throw new Error(data.error || res.statusText);
      return data;
    }
    async function refresh() {
      const data = await (await fetch('/api/status')).json();
      render(data);
    }
    function render(data) {
      lastSnapshot = data;
      document.getElementById('status').textContent = JSON.stringify(data, null, 2);
      document.getElementById('footer').textContent = `${data.session.state} ${data.session.session_id || ''}`;
      renderOperatorStatus(data);
      const cameras = data.dashboard.cameras || {};
      const timings = data.latest_frame_timing_by_camera || {};
      renderCameraGrid(cameras, timings);
      restoreRemoteVideos();
    }
    function renderCameraGrid(cameras, timings) {
      const grid = document.getElementById('cameras');
      const cameraIds = mergedCameraIds(cameras);
      for (const knownId of Array.from(knownCameraIds)) {
        if (!cameraIds.includes(knownId)) {
          document.getElementById(`camera-card-${knownId}`)?.remove();
          knownCameraIds.delete(knownId);
          delete lastRenderedFrameSequenceByCamera[knownId];
          delete cameraLayoutByCamera[knownId];
        }
      }
      ensureCameraLayout(cameraIds);
      for (const id of cameraIds) {
        if (!knownCameraIds.has(id)) createCameraCard(id);
        syncCameraCardManualState(id);
        applyCameraLayout(id);
        updateCameraCard(id, cameras[id] || emptyManualCamera(id), timings[id] || {});
      }
    }
    function createCameraCard(id) {
      const grid = document.getElementById('cameras');
      const article = document.createElement('article');
      article.className = 'camera';
      article.id = `camera-card-${id}`;
      article.dataset.cameraId = id;
      article.innerHTML = `<div class="camera-frame" id="camera-body-${id}"><span id="camera-placeholder-${id}">waiting</span><img id="camera-img-${id}" alt="${id}" hidden></div><strong class="camera-title" id="camera-title-${id}"></strong><button type="button" class="camera-manual-remove" title="Remove manual camera" aria-label="Remove manual camera" data-manual-remove="true">x</button>${renderCameraLayoutControls()}<div class="camera-resize-grip" data-layout-resize="true" title="Resize"></div>${renderFrameTiming({camera_id: id}, {})}`;
      article.addEventListener('pointerdown', startCameraLayoutDrag);
      grid.appendChild(article);
      knownCameraIds.add(id);
    }
    function syncCameraCardManualState(id) {
      const article = document.getElementById(`camera-card-${id}`);
      if (!article) return;
      const isManual = manualCameraIds.has(id);
      article.classList.toggle('manual', isManual);
      const removeButton = article.querySelector('[data-manual-remove]');
      if (removeButton) removeButton.hidden = !isManual;
    }
    function updateCameraCard(id, cam, timing) {
      setText(`camera-title-${id}`, `${id} ${formatFps(timing.received_fps ?? cam.fps)}fps ${cam.bitrate_kbps}kbps`);
      setText(`camera-e2e-${id}`, `E2E ${formatMs(timing.end_to_end_latency_ms ?? cam.latency_ms)}`);
      setText(`camera-capture-${id}`, `capture ${formatTime(timing.captured_at_ms)}`);
      setText(`camera-encode-${id}`, `encode ${formatTime(timing.encoded_at_ms)} / ${formatMs(timing.encode_latency_ms)}`);
      setText(`camera-send-${id}`, `send ${formatTime(timing.sent_at_ms)}`);
      setText(`camera-receive-${id}`, `receive ${formatTime(timing.received_at_ms)} / ${formatMs(timing.transport_latency_ms)}`);
      setText(`camera-decode-${id}`, `decode ${formatTime(timing.decoded_at_ms)} / ${formatMs(timing.decode_latency_ms)}`);
      setText(`camera-seq-${id}`, `seq ${timing.frame_sequence ?? '-'}`);
      if (remoteStreamsByCamera.has(id)) return;
      const sequence = timing.frame_sequence;
      const img = document.getElementById(`camera-img-${id}`);
      const placeholder = document.getElementById(`camera-placeholder-${id}`);
      if (!img || !placeholder) return;
      if (!img.dataset.brightnessBound) {
        img.addEventListener('load', () => calibrateCameraBrightness(id, img));
        img.dataset.brightnessBound = '1';
      }
      if (Number.isFinite(Number(sequence)) && sequence > 0) {
        if (lastRenderedFrameSequenceByCamera[id] !== sequence) {
          img.src = `/api/frame/${id}.png?seq=${sequence}`;
          lastRenderedFrameSequenceByCamera[id] = sequence;
        }
        img.hidden = false;
        placeholder.hidden = true;
      } else {
        img.hidden = true;
        placeholder.hidden = false;
        placeholder.textContent = cam.state || 'waiting';
      }
    }
    function renderFrameTiming(timing, cam) {
      const total = timing.end_to_end_latency_ms ?? cam.latency_ms;
      return `<div class="camera-meta"><span id="camera-e2e-${timing.camera_id || ''}" class="latency-total">E2E ${formatMs(total)}</span><div class="timing-grid">
        <span id="camera-capture-${timing.camera_id || ''}">capture ${formatTime(timing.captured_at_ms)}</span>
        <span id="camera-encode-${timing.camera_id || ''}">encode ${formatTime(timing.encoded_at_ms)} / ${formatMs(timing.encode_latency_ms)}</span>
        <span id="camera-send-${timing.camera_id || ''}">send ${formatTime(timing.sent_at_ms)}</span>
        <span id="camera-receive-${timing.camera_id || ''}">receive ${formatTime(timing.received_at_ms)} / ${formatMs(timing.transport_latency_ms)}</span>
        <span id="camera-decode-${timing.camera_id || ''}">decode ${formatTime(timing.decoded_at_ms)} / ${formatMs(timing.decode_latency_ms)}</span>
        <span id="camera-seq-${timing.camera_id || ''}">seq ${timing.frame_sequence ?? '-'}</span>
      </div></div>`;
    }
    function renderCameraLayoutControls() {
      return `<div class="camera-layout-controls" aria-label="camera layout controls">
        <button type="button" title="Move up" data-layout-action="up">U</button>
        <button type="button" title="Move down" data-layout-action="down">D</button>
        <button type="button" title="Move left" data-layout-action="left">L</button>
        <button type="button" title="Move right" data-layout-action="right">R</button>
        <button type="button" title="Wider" data-layout-action="wider">W+</button>
        <button type="button" title="Narrower" data-layout-action="narrower">W-</button>
        <button type="button" title="Taller" data-layout-action="taller">H+</button>
        <button type="button" title="Shorter" data-layout-action="shorter">H-</button>
      </div>`;
    }
    function loadCameraLayout() {
      try {
        const raw = window.localStorage.getItem(CAMERA_LAYOUT_STORAGE_KEY);
        return raw ? JSON.parse(raw) : {};
      } catch (_err) {
        return {};
      }
    }
    function saveCameraLayout() {
      window.localStorage.setItem(CAMERA_LAYOUT_STORAGE_KEY, JSON.stringify(cameraLayoutByCamera));
    }
    function loadManualCameraIds() {
      try {
        const raw = window.localStorage.getItem(CAMERA_MANUAL_STORAGE_KEY);
        const parsed = raw ? JSON.parse(raw) : [];
        return Array.isArray(parsed) ? parsed.map(normalizeCameraId).filter(Boolean) : [];
      } catch (_err) {
        return [];
      }
    }
    function saveManualCameraIds() {
      window.localStorage.setItem(CAMERA_MANUAL_STORAGE_KEY, JSON.stringify(Array.from(manualCameraIds)));
    }
    function normalizeCameraId(value) {
      return String(value || '').trim().replace(/\\s+/g, '-').replace(/[^A-Za-z0-9:_-]/g, '-').replace(/-+/g, '-').replace(/^-|-$/g, '').slice(0, 64);
    }
    function mergedCameraIds(cameras) {
      const ids = Object.keys(cameras);
      for (const id of manualCameraIds) {
        if (!ids.includes(id)) ids.push(id);
      }
      return ids;
    }
    function emptyManualCamera(id) {
      return {camera_id: id, state: 'waiting', fps: 0, bitrate_kbps: 0, latency_ms: null};
    }
    function rerenderLastSnapshot() {
      render(lastSnapshot || {session: {state: 'DISCONNECTED'}, status: {}, dashboard: {cameras: {}}, latest_frame_timing_by_camera: {}});
    }
    function addManualCameraFromForm() {
      const input = document.getElementById('manual-camera-id');
      const id = normalizeCameraId(input.value);
      if (!id) {
        input.focus();
        return;
      }
      addManualCameraSlot(id);
      input.value = '';
      input.focus();
    }
    function addManualCameraSlot(id) {
      id = normalizeCameraId(id);
      if (!id) return;
      manualCameraIds.add(id);
      saveManualCameraIds();
      applyDefaultLayoutForIds(mergedCameraIds(lastSnapshot?.dashboard?.cameras || {}));
      rerenderLastSnapshot();
    }
    function removeManualCameraSlot(id) {
      id = normalizeCameraId(id);
      if (!id) return;
      manualCameraIds.delete(id);
      saveManualCameraIds();
      if (!((lastSnapshot?.dashboard?.cameras || {})[id])) {
        delete cameraLayoutByCamera[id];
        saveCameraLayout();
      }
      applyDefaultLayoutForIds(mergedCameraIds(lastSnapshot?.dashboard?.cameras || {}));
      rerenderLastSnapshot();
    }
    function ensureCameraLayout(cameraIds) {
      let changed = false;
      for (const id of cameraIds) {
        const normalized = normalizeCameraLayout(cameraLayoutByCamera[id] || defaultCameraLayout(id, cameraIds));
        if (JSON.stringify(cameraLayoutByCamera[id]) !== JSON.stringify(normalized)) {
          cameraLayoutByCamera[id] = normalized;
          changed = true;
        }
      }
      if (changed) saveCameraLayout();
    }
    function defaultCameraLayout(id, cameraIds) {
      if (cameraIds.length === 3) return defaultThreeCameraLayout(id, cameraIds);
      if (cameraIds.length >= 4) return defaultSurroundCameraLayout(id, cameraIds);
      const normalizedId = String(id).toLowerCase();
      const named = {
        front: {col: 1, row: 1, colSpan: 8, rowSpan: 10},
        rear: {col: 9, row: 1, colSpan: 4, rowSpan: 5},
        left: {col: 1, row: 1, colSpan: 3, rowSpan: 5},
        right: {col: 10, row: 1, colSpan: 3, rowSpan: 5},
        top: {col: 4, row: 1, colSpan: 6, rowSpan: 3},
        bottom: {col: 4, row: 8, colSpan: 6, rowSpan: 3},
      };
      if (named[normalizedId]) return named[normalizedId];
      const index = Math.max(0, cameraIds.indexOf(id));
      const fallback = [
        {col: 1, row: 1, colSpan: 8, rowSpan: 10},
        {col: 9, row: 1, colSpan: 4, rowSpan: 5},
        {col: 9, row: 6, colSpan: 4, rowSpan: 5},
        {col: 1, row: 6, colSpan: 4, rowSpan: 5},
        {col: 5, row: 6, colSpan: 4, rowSpan: 5},
      ];
      return fallback[index % fallback.length];
    }
    function defaultSurroundCameraLayout(id, cameraIds) {
      const normalizedId = String(id).toLowerCase();
      const named = {
        front: {col: 4, row: 3, colSpan: 6, rowSpan: 6},
        rear: {col: 4, row: 1, colSpan: 6, rowSpan: 2},
        hikrobot: {col: 4, row: 9, colSpan: 6, rowSpan: 2},
        left: {col: 1, row: 3, colSpan: 3, rowSpan: 6},
        right: {col: 10, row: 3, colSpan: 3, rowSpan: 6},
        top: {col: 4, row: 1, colSpan: 6, rowSpan: 2},
        bottom: {col: 4, row: 9, colSpan: 6, rowSpan: 2},
      };
      if (named[normalizedId]) return named[normalizedId];
      const index = Math.max(0, cameraIds.indexOf(id));
      const fallback = [
        {col: 4, row: 3, colSpan: 6, rowSpan: 6},
        {col: 4, row: 1, colSpan: 6, rowSpan: 2},
        {col: 4, row: 9, colSpan: 6, rowSpan: 2},
        {col: 1, row: 3, colSpan: 3, rowSpan: 6},
        {col: 10, row: 3, colSpan: 3, rowSpan: 6},
        {col: 1, row: 1, colSpan: 3, rowSpan: 2},
        {col: 10, row: 1, colSpan: 3, rowSpan: 2},
      ];
      return fallback[index % fallback.length];
    }
    function defaultThreeCameraLayout(id, cameraIds) {
      const normalizedId = String(id).toLowerCase();
      const named = {
        front: {col: 1, row: 1, colSpan: 8, rowSpan: 10},
        rear: {col: 9, row: 1, colSpan: 4, rowSpan: 5},
        hikrobot: {col: 9, row: 6, colSpan: 4, rowSpan: 5},
      };
      if (named[normalizedId]) return named[normalizedId];
      const index = Math.max(0, cameraIds.indexOf(id));
      const fallback = [
        {col: 1, row: 1, colSpan: 8, rowSpan: 10},
        {col: 9, row: 1, colSpan: 4, rowSpan: 5},
        {col: 9, row: 6, colSpan: 4, rowSpan: 5},
      ];
      return fallback[index % fallback.length];
    }
    function normalizeCameraLayout(layout) {
      const colSpan = clamp(Math.round(Number(layout.colSpan || 3)), 2, CAMERA_GRID_COLUMNS);
      const rowSpan = clamp(Math.round(Number(layout.rowSpan || 3)), 2, CAMERA_GRID_ROWS);
      return {
        col: clamp(Math.round(Number(layout.col || 1)), 1, CAMERA_GRID_COLUMNS - colSpan + 1),
        row: clamp(Math.round(Number(layout.row || 1)), 1, CAMERA_GRID_ROWS - rowSpan + 1),
        colSpan,
        rowSpan,
      };
    }
    function applyCameraLayout(id) {
      const article = document.getElementById(`camera-card-${id}`);
      if (!article) return;
      const layout = normalizeCameraLayout(cameraLayoutByCamera[id] || defaultCameraLayout(id, Array.from(knownCameraIds)));
      article.style.gridColumn = `${layout.col} / span ${layout.colSpan}`;
      article.style.gridRow = `${layout.row} / span ${layout.rowSpan}`;
      article.dataset.layout = `${layout.col},${layout.row},${layout.colSpan},${layout.rowSpan}`;
    }
    function toggleCameraLayoutEdit() {
      cameraLayoutEditEnabled = !cameraLayoutEditEnabled;
      document.body.classList.toggle('layout-editing', cameraLayoutEditEnabled);
      setText('layout-edit-button', cameraLayoutEditEnabled ? 'Done Layout' : 'Edit Layout');
    }
    function applySurroundLayout() {
      applyDefaultLayoutForIds(Array.from(knownCameraIds));
    }
    function applyDefaultLayoutForIds(ids) {
      cameraLayoutByCamera = {};
      for (const id of ids) {
        cameraLayoutByCamera[id] = normalizeCameraLayout(defaultCameraLayout(id, ids));
        applyCameraLayout(id);
      }
      saveCameraLayout();
    }
    function resetCameraLayout() {
      window.localStorage.removeItem(CAMERA_LAYOUT_STORAGE_KEY);
      cameraLayoutByCamera = {};
      applyDefaultLayoutForIds(Array.from(knownCameraIds));
    }
    function nudgeCameraLayout(id, action) {
      const deltas = {
        up: {row: -1},
        down: {row: 1},
        left: {col: -1},
        right: {col: 1},
        wider: {colSpan: 1},
        narrower: {colSpan: -1},
        taller: {rowSpan: 1},
        shorter: {rowSpan: -1},
      };
      const delta = deltas[action];
      if (!delta) return;
      const layout = normalizeCameraLayout(cameraLayoutByCamera[id] || defaultCameraLayout(id, Array.from(knownCameraIds)));
      cameraLayoutByCamera[id] = normalizeCameraLayout({
        col: layout.col + (delta.col || 0),
        row: layout.row + (delta.row || 0),
        colSpan: layout.colSpan + (delta.colSpan || 0),
        rowSpan: layout.rowSpan + (delta.rowSpan || 0),
      });
      applyCameraLayout(id);
      saveCameraLayout();
    }
    function startCameraLayoutDrag(event) {
      if (!cameraLayoutEditEnabled) return;
      if (event.target.closest('button')) return;
      const article = event.currentTarget;
      const id = article.dataset.cameraId;
      if (!id) return;
      cameraLayoutDrag = {
        id,
        resizing: Boolean(event.target.closest('[data-layout-resize]')),
        startX: event.clientX,
        startY: event.clientY,
        startLayout: normalizeCameraLayout(cameraLayoutByCamera[id] || defaultCameraLayout(id, Array.from(knownCameraIds))),
      };
      article.classList.add('dragging');
      article.setPointerCapture(event.pointerId);
      event.preventDefault();
    }
    function updateCameraLayoutDrag(event) {
      if (!cameraLayoutDrag) return;
      const grid = document.getElementById('cameras');
      const rect = grid.getBoundingClientRect();
      const cellWidth = rect.width / CAMERA_GRID_COLUMNS;
      const cellHeight = rect.height / CAMERA_GRID_ROWS;
      const colDelta = Math.round((event.clientX - cameraLayoutDrag.startX) / cellWidth);
      const rowDelta = Math.round((event.clientY - cameraLayoutDrag.startY) / cellHeight);
      const start = cameraLayoutDrag.startLayout;
      cameraLayoutByCamera[cameraLayoutDrag.id] = normalizeCameraLayout(cameraLayoutDrag.resizing ? {
        col: start.col,
        row: start.row,
        colSpan: start.colSpan + colDelta,
        rowSpan: start.rowSpan + rowDelta,
      } : {
        col: start.col + colDelta,
        row: start.row + rowDelta,
        colSpan: start.colSpan,
        rowSpan: start.rowSpan,
      });
      applyCameraLayout(cameraLayoutDrag.id);
    }
    function finishCameraLayoutDrag() {
      if (!cameraLayoutDrag) return;
      document.getElementById(`camera-card-${cameraLayoutDrag.id}`)?.classList.remove('dragging');
      saveCameraLayout();
      cameraLayoutDrag = null;
    }
    function sampleMediaLuminance(media) {
      if (!brightnessContext || media.hidden) return null;
      const width = 32;
      const height = 18;
      brightnessCanvas.width = width;
      brightnessCanvas.height = height;
      try {
        brightnessContext.drawImage(media, 0, 0, width, height);
        const pixels = brightnessContext.getImageData(0, 0, width, height).data;
        let total = 0;
        let count = 0;
        for (let i = 0; i < pixels.length; i += 16) {
          total += 0.2126 * pixels[i] + 0.7152 * pixels[i + 1] + 0.0722 * pixels[i + 2];
          count += 1;
        }
        return count ? total / count : null;
      } catch (_err) {
        return null;
      }
    }
    function calibrateCameraBrightness(cameraId, media) {
      const luminance = sampleMediaLuminance(media);
      if (!Number.isFinite(Number(luminance))) return;
      const body = document.getElementById(`camera-body-${cameraId}`);
      if (!body) return;
      const brightness = clamp(118 / Math.max(28, Number(luminance)), 0.65, 1.85);
      const contrast = clamp(1 + Math.abs(128 - Number(luminance)) / 520, 1, 1.25);
      body.style.setProperty('--camera-brightness', brightness.toFixed(2));
      body.style.setProperty('--camera-contrast', contrast.toFixed(2));
      body.dataset.luminance = String(Math.round(Number(luminance)));
    }
    function calibrateVisibleCameraBrightness() {
      for (const id of knownCameraIds) {
        const video = document.getElementById(`webrtc-video-${id}`);
        const img = document.getElementById(`camera-img-${id}`);
        const media = video || (img && !img.hidden ? img : null);
        if (media) calibrateCameraBrightness(id, media);
      }
    }
    function formatMs(value) {
      return Number.isFinite(Number(value)) ? `${Math.round(Number(value))}ms` : '-';
    }
    function formatFps(value) {
      if (!Number.isFinite(Number(value))) return '0';
      const fps = Number(value);
      return fps >= 10 ? String(Math.round(fps)) : fps.toFixed(1).replace(/\\.0$/, '');
    }
    function formatTime(value) {
      if (!Number.isFinite(Number(value))) return '-';
      const date = new Date(Number(value));
      return date.toLocaleTimeString([], {hour12: false, hour: '2-digit', minute: '2-digit', second: '2-digit'}) + `.${String(date.getMilliseconds()).padStart(3, '0')}`;
    }
    function renderOperatorStatus(data) {
      const session = data.session || {};
      const status = data.status || {};
      const sideBar = status.side_bar || {};
      const bottomBar = status.bottom_bar || {};
      const dashboard = data.dashboard || {};
      const cameras = dashboard.cameras || {};
      const cameraList = Object.keys(cameras).map((id) => cameras[id]);
      const connectedCameras = cameraList.filter((camera) => camera.state === 'connected');
      const visible = mergedCameraIds(cameras);
      const command = data.last_command || null;
      setText('operator-session-state', session.state || 'DISCONNECTED');
      setText('operator-session-id', session.session_id || '-');
      setText('operator-control-authority', bottomBar.control_authority_state || 'inactive');
      setText('operator-signaling-state', sideBar.control_connection_state || 'disconnected');
      setText('operator-camera-summary', `${connectedCameras.length}/${cameraList.length} connected`);
      setText('operator-webrtc-state', `${operatorPanelState.webrtc}; visible ${visible.join(',') || '-'}`);
      setText('operator-command-summary', summarizeCommand(command));
      setText('operator-datachannel-state', operatorPanelState.dataChannel);
    }
    function summarizeCommand(command) {
      if (!command) return 'no command';
      const steering = Number(command.steering || 0).toFixed(2);
      const throttle = Number(command.throttle || 0).toFixed(2);
      const brake = Number(command.brake || 0).toFixed(2);
      return `seq ${command.seq} ${command.gear} str ${steering} thr ${throttle} brk ${brake}`;
    }
    function setText(id, value) {
      const node = document.getElementById(id);
      if (node) node.textContent = String(value);
    }
    function updateWebRtcState(state) {
      operatorPanelState.webrtc = state;
      setText('operator-webrtc-state', state);
    }
    function updateDataChannelState(state) {
      operatorPanelState.dataChannel = state;
      setText('operator-datachannel-state', state);
    }
    function connectPayloadFromForm() {
      return {
        vehicle_id: document.getElementById('connect-vehicle-id').value.trim(),
        password: document.getElementById('connect-password').value
      };
    }
    async function connectConsole() {
      keyboardControlEnabled = true;
      signalingPollEnabled = true;
      render(await postJson('/api/connect', connectPayloadFromForm()));
      await pollSignaling();
    }
    async function pollSignaling() {
      if (!signalingPollEnabled) return;
      const data = await postJson('/api/poll-signaling', {});
      await handleSignalingMessages(data.messages || []);
      render(data.snapshot);
    }
    async function sendCurrentControl() {
      await sendControl({
        gear: document.getElementById('gear').value,
        steering: Number(document.getElementById('steering').value),
        throttle: Number(document.getElementById('throttle').value),
        brake: Number(document.getElementById('brake').value)
      });
    }
    async function sendControl(payload) {
      const data = await postJson('/api/control', payload);
      if (data.sent) sendCommandOverDataChannel(data.command);
      render(data.snapshot);
    }
    async function handleSignalingMessages(messages) {
      for (const message of messages) {
        if (message.type === 'webrtc_offer') {
          await startWebRtcFromOffer(message.payload || {});
        } else if (message.type === 'ice_candidate') {
          await addRemoteIceCandidate(message.payload || {});
        }
      }
    }
    async function addRemoteIceCandidate(candidate) {
      if (!candidate || !candidate.candidate) return;
      if (!peerConnection || !peerConnection.remoteDescription) {
        pendingRemoteIceCandidates.push(candidate);
        return;
      }
      await peerConnection.addIceCandidate(new RTCIceCandidate(candidate));
    }
    async function flushPendingRemoteIceCandidates() {
      while (pendingRemoteIceCandidates.length) {
        await addRemoteIceCandidate(pendingRemoteIceCandidates.shift());
      }
    }
    async function startWebRtcFromOffer(offer) {
      if (!offer.sdp) return;
      if (peerConnection) peerConnection.close();
      remoteCameraIds = (offer.media_tracks || []).map((track) => track.camera_id).filter(Boolean);
      peerConnection = new RTCPeerConnection({});
      updateWebRtcState('negotiating');
      controlDataChannel = peerConnection.createDataChannel('control', {
        ordered: false,
        maxRetransmits: 0,
        protocol: 'mine-teleop-control-v1'
      });
      updateDataChannelState('connecting');
      peerConnection.onconnectionstatechange = () => updateWebRtcState(peerConnection.connectionState || 'unknown');
      controlDataChannel.onopen = () => updateDataChannelState('open');
      controlDataChannel.onclose = () => updateDataChannelState('closed');
      controlDataChannel.onerror = () => updateDataChannelState('error');
      peerConnection.ontrack = (event) => {
        const cameraId = remoteCameraIds.shift() || 'front';
        const stream = event.streams[0] || new MediaStream([event.track]);
        remoteStreamsByCamera.set(cameraId, stream);
        attachRemoteStreamToCamera(cameraId, stream);
      };
      peerConnection.onicecandidate = async (event) => {
        if (!event.candidate) return;
        await postJson('/api/webrtc/ice-candidate', {
          candidate: event.candidate.toJSON ? event.candidate.toJSON() : event.candidate
        });
      };
      await peerConnection.setRemoteDescription({type: offer.type || 'offer', sdp: offer.sdp});
      await flushPendingRemoteIceCandidates();
      const answer = await peerConnection.createAnswer();
      await peerConnection.setLocalDescription(answer);
      await postJson('/api/webrtc/answer', {
        type: peerConnection.localDescription.type,
        sdp: peerConnection.localDescription.sdp
      });
    }
    function attachRemoteStreamToCamera(cameraId, stream) {
      const body = document.getElementById(`camera-body-${cameraId}`);
      if (!body) return;
      let video = document.getElementById(`webrtc-video-${cameraId}`);
      if (!video) {
        body.innerHTML = `<video id="webrtc-video-${cameraId}" autoplay playsinline muted></video>`;
        video = document.getElementById(`webrtc-video-${cameraId}`);
      }
      video.srcObject = stream;
      video.onloadeddata = () => calibrateCameraBrightness(cameraId, video);
    }
    function restoreRemoteVideos() {
      for (const [cameraId, stream] of remoteStreamsByCamera.entries()) {
        attachRemoteStreamToCamera(cameraId, stream);
      }
    }
    function sendCommandOverDataChannel(command) {
      if (!command || !controlDataChannel || controlDataChannel.readyState !== 'open') return false;
      controlDataChannel.send(JSON.stringify(command));
      updateDataChannelState(`sent seq ${command.seq}`);
      return true;
    }
    async function sendKeyboardControl() {
      if (!keyboardControlEnabled) return;
      if (Date.now() - lastGamepadSeenMs < 250) return;
      try {
        const data = await postJson('/api/control/keyboard', {
          keys: Array.from(pressedKeys),
          gear: document.getElementById('gear').value,
          now_ms: Date.now()
        });
        if (data.sent) {
          sendCommandOverDataChannel(data.command);
          render(data.snapshot);
        }
      } catch (err) {
        document.getElementById('footer').textContent = err.message;
      }
    }
    function clamp(value, min, max) {
      return Math.min(max, Math.max(min, value));
    }
    function applyDeadzone(value) {
      return Math.abs(value) < gamepadMapping.axis_deadzone ? 0 : value;
    }
    function readAxis(gamepad, axisIndex, fallback) {
      if (axisIndex < 0 || axisIndex >= gamepad.axes.length) return fallback;
      const value = Number(gamepad.axes[axisIndex]);
      return Number.isFinite(value) ? value : fallback;
    }
    function pedalAxisToUnit(value, inverted) {
      const normalized = inverted ? (1 - value) / 2 : (value + 1) / 2;
      return clamp(applyDeadzone(normalized), 0, 1);
    }
    async function sendGamepadControl() {
      if (!keyboardControlEnabled || !gamepadMapping.enabled || !('getGamepads' in navigator)) return;
      const gamepads = Array.from(navigator.getGamepads()).filter(Boolean);
      const gamepad = gamepads[0];
      if (!gamepad) return;
      lastGamepadSeenMs = Date.now();
      const estopButton = gamepad.buttons[gamepadMapping.estop_button];
      try {
        const data = await postJson('/api/control/gamepad', {
          steering_axis: clamp(applyDeadzone(readAxis(gamepad, gamepadMapping.steering_axis, 0)), -1, 1),
          throttle_axis: pedalAxisToUnit(readAxis(gamepad, gamepadMapping.throttle_axis, 1), gamepadMapping.throttle_inverted),
          brake_axis: pedalAxisToUnit(readAxis(gamepad, gamepadMapping.brake_axis, 1), gamepadMapping.brake_inverted),
          estop: Boolean(estopButton && estopButton.pressed),
          gear: document.getElementById('gear').value,
          now_ms: Date.now()
        });
        if (data.sent) {
          sendCommandOverDataChannel(data.command);
          render(data.snapshot);
        }
      } catch (err) {
        document.getElementById('footer').textContent = err.message;
      }
    }
    document.addEventListener('keydown', (event) => {
      pressedKeys.add(event.key.toUpperCase());
    });
    document.addEventListener('keyup', (event) => {
      pressedKeys.delete(event.key.toUpperCase());
    });
    document.addEventListener('click', (event) => {
      const removeButton = event.target.closest('[data-manual-remove]');
      if (removeButton) {
        const article = removeButton.closest('.camera');
        if (article && article.dataset.cameraId) removeManualCameraSlot(article.dataset.cameraId);
        return;
      }
      const button = event.target.closest('[data-layout-action]');
      if (!button) return;
      const article = button.closest('.camera');
      if (!article || !article.dataset.cameraId) return;
      nudgeCameraLayout(article.dataset.cameraId, button.dataset.layoutAction);
    });
    document.addEventListener('pointermove', updateCameraLayoutDrag);
    document.addEventListener('pointerup', finishCameraLayoutDrag);
    document.addEventListener('pointercancel', finishCameraLayoutDrag);
    setInterval(sendGamepadControl, 50);
    setInterval(sendKeyboardControl, 50);
    setInterval(pollSignaling, 1000);
    setInterval(calibrateVisibleCameraBrightness, 2000);
    refresh();
  </script>
</body>
</html>
"""


def _normalize_signaling_http_url(url: str) -> str:
    parsed = urlparse(url)
    scheme = parsed.scheme
    if scheme == "ws":
        scheme = "http"
    elif scheme == "wss":
        scheme = "https"
    path = parsed.path
    if path.rstrip("/") == "/signaling":
        path = ""
    return urlunparse((scheme, parsed.netloc, path.rstrip("/"), "", "", "")).rstrip("/")


def _require_session_description(payload: dict[str, Any], *, expected_type: str) -> dict[str, str]:
    if not isinstance(payload, dict):
        raise ValueError("WebRTC session description must be an object")
    description_type = payload.get("type")
    sdp = payload.get("sdp")
    if description_type != expected_type:
        raise ValueError(f"WebRTC session description type must be {expected_type}")
    if not isinstance(sdp, str) or not sdp:
        raise ValueError("WebRTC session description sdp is required")
    return {"type": expected_type, "sdp": sdp}


def _optional_axis(value: Any) -> float | None:
    if value is None:
        return None
    return float(value)


def _optional_payload_string(payload: dict[str, Any], key: str) -> str | None:
    if key not in payload:
        return None
    value = payload[key]
    if value is None:
        return None
    return _require_non_empty_string(value, key)


def _optional_payload_int(payload: dict[str, Any], key: str) -> int | None:
    if key not in payload:
        return None
    value = payload[key]
    if value is None:
        return None
    return _optional_non_negative_int(value, key)


def _optional_signed_int(payload: dict[str, Any], key: str) -> int:
    value = payload.get(key)
    if value is None:
        return 0
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be an integer")
    return value


def _require_non_empty_string(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a string")
    if not value:
        raise ValueError(f"{field} must be a non-empty string")
    return value


def _optional_non_negative_int(value: Any, field: str) -> int | None:
    if value is None:
        return None
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field} must be a non-negative integer")
    if value < 0:
        raise ValueError(f"{field} must be non-negative")
    return value


def _non_negative_delta(later_ms: int | None, earlier_ms: int | None) -> int | None:
    if later_ms is None or earlier_ms is None:
        return None
    return max(0, later_ms - earlier_ms)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _json_get(url: str) -> dict[str, Any]:
    with request.urlopen(url, timeout=5) as response:
        return _decode_response(response.read())


def _json_post(url: str, payload: dict[str, Any]) -> dict[str, Any]:
    body = json.dumps(payload).encode("utf-8")
    req = request.Request(url, data=body, method="POST", headers={"Content-Type": "application/json"})
    with request.urlopen(req, timeout=5) as response:
        return _decode_response(response.read())


def _decode_response(body: bytes) -> dict[str, Any]:
    data = json.loads(body.decode("utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError("expected JSON object response")
    return data
