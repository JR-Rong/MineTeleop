from __future__ import annotations

import json
from dataclasses import dataclass, field, replace
from pathlib import Path
from typing import Any, Iterable

from .control import ALLOWED_GEARS, ControlCommand
from .log_rotation import rotate_file_if_needed


@dataclass(frozen=True)
class DriverOperationEvent:
    ts_ms: int
    event: str
    driver_id: str
    vehicle_id: str = ""
    session_id: str = ""
    ui_version: str = ""
    config_version: str = ""
    details: dict[str, Any] = field(default_factory=dict)

    def to_dict(self) -> dict[str, Any]:
        return {
            "ts_ms": self.ts_ms,
            "event": self.event,
            "driver_id": self.driver_id,
            "vehicle_id": self.vehicle_id,
            "session_id": self.session_id,
            "ui_version": self.ui_version,
            "config_version": self.config_version,
            "details": dict(self.details),
        }


class DriverOperationLog:
    def __init__(self, path: str | Path, max_bytes: int | None = None, backup_count: int = 0) -> None:
        self.path = Path(path)
        self.max_bytes = max_bytes
        self.backup_count = backup_count

    def append(self, event: DriverOperationEvent) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        line = json.dumps(event.to_dict(), sort_keys=True) + "\n"
        rotate_file_if_needed(self.path, len(line.encode("utf-8")), self.max_bytes, self.backup_count)
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(line)


@dataclass(frozen=True)
class CameraDisplayStatus:
    camera_id: str
    state: str = "disconnected"
    fps: int = 0
    bitrate_kbps: int = 0
    latency_ms: int | None = None
    low_bitrate: bool = False
    reconnecting: bool = False
    decode_failed: bool = False
    message: str = ""

    def to_dict(self) -> dict[str, Any]:
        return {
            "camera_id": self.camera_id,
            "state": self.state,
            "fps": self.fps,
            "bitrate_kbps": self.bitrate_kbps,
            "latency_ms": self.latency_ms,
            "low_bitrate": self.low_bitrate,
            "reconnecting": self.reconnecting,
            "decode_failed": self.decode_failed,
            "message": self.message,
        }

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "CameraDisplayStatus":
        return cls(
            camera_id=str(data["camera_id"]),
            state=str(data.get("state", "disconnected")),
            fps=int(data.get("fps", 0)),
            bitrate_kbps=int(data.get("bitrate_kbps", 0)),
            latency_ms=data.get("latency_ms"),
            low_bitrate=bool(data.get("low_bitrate", False)),
            reconnecting=bool(data.get("reconnecting", False)),
            decode_failed=bool(data.get("decode_failed", False)),
            message=str(data.get("message", "")),
        )


class DriverVideoDashboard:
    _VALID_LAYOUTS = {"single", "grid_1", "grid_2", "grid_4"}

    def __init__(self, camera_ids: Iterable[str], layout: str) -> None:
        self._validate_layout(layout)
        self.layout = layout
        self.focused_camera_id: str | None = None
        self.camera_status = {camera_id: CameraDisplayStatus(camera_id=camera_id) for camera_id in camera_ids}

    def update_camera_status(
        self,
        camera_id: str,
        state: str,
        fps: int,
        bitrate_kbps: int,
        latency_ms: int | None,
        low_bitrate: bool = False,
        reconnecting: bool = False,
        message: str = "",
    ) -> None:
        current = self._require_camera(camera_id)
        self.camera_status[camera_id] = replace(
            current,
            state=state,
            fps=fps,
            bitrate_kbps=bitrate_kbps,
            latency_ms=latency_ms,
            low_bitrate=low_bitrate,
            reconnecting=reconnecting or state == "reconnecting",
            message=message,
        )

    def mark_decode_failed(self, camera_id: str, message: str) -> None:
        current = self._require_camera(camera_id)
        self.camera_status[camera_id] = replace(
            current,
            state="decode_failed",
            decode_failed=True,
            reconnecting=True,
            message=message,
        )

    def focus_camera(self, camera_id: str) -> None:
        self._require_camera(camera_id)
        self.layout = "single"
        self.focused_camera_id = camera_id

    def set_layout(self, layout: str) -> None:
        self._validate_layout(layout)
        self.layout = layout
        if layout != "single":
            self.focused_camera_id = None

    def visible_camera_ids(self) -> list[str]:
        camera_ids = list(self.camera_status.keys())
        if self.layout == "single":
            if self.focused_camera_id is not None:
                self._require_camera(self.focused_camera_id)
                return [self.focused_camera_id]
            return camera_ids[:1]
        visible_count = {"grid_1": 1, "grid_2": 2, "grid_4": 4}[self.layout]
        return camera_ids[:visible_count]

    def to_dict(self) -> dict[str, Any]:
        return {
            "layout": self.layout,
            "focused_camera_id": self.focused_camera_id,
            "visible_camera_ids": self.visible_camera_ids(),
            "cameras": {camera_id: status.to_dict() for camera_id, status in self.camera_status.items()},
        }

    def save_layout(self, path: str | Path) -> None:
        payload = {
            "camera_ids": list(self.camera_status.keys()),
            "focused_camera_id": self.focused_camera_id,
            "layout": self.layout,
        }
        target = Path(path)
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(json.dumps(payload, sort_keys=True) + "\n", encoding="utf-8")

    @classmethod
    def load_layout(cls, path: str | Path) -> "DriverVideoDashboard":
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        camera_ids = data.get("camera_ids", [])
        if not isinstance(camera_ids, list) or not all(
            isinstance(camera_id, str) and camera_id for camera_id in camera_ids
        ):
            raise ValueError("camera_ids must be a list of non-empty strings")
        dashboard = cls(camera_ids=camera_ids, layout=str(data["layout"]))
        focused_camera_id = data.get("focused_camera_id")
        if focused_camera_id is not None:
            if not isinstance(focused_camera_id, str):
                raise ValueError("focused_camera_id must be a string")
            dashboard.focus_camera(focused_camera_id)
        return dashboard

    @classmethod
    def from_dict(cls, data: dict[str, Any]) -> "DriverVideoDashboard":
        cameras = data.get("cameras", {})
        dashboard = cls(camera_ids=cameras.keys(), layout=str(data["layout"]))
        dashboard.focused_camera_id = data.get("focused_camera_id")
        dashboard.camera_status = {
            camera_id: CameraDisplayStatus.from_dict(status) for camera_id, status in cameras.items()
        }
        return dashboard

    def _require_camera(self, camera_id: str) -> CameraDisplayStatus:
        if camera_id not in self.camera_status:
            raise KeyError(f"unknown camera: {camera_id}")
        return self.camera_status[camera_id]

    def _validate_layout(self, layout: str) -> None:
        if layout not in self._VALID_LAYOUTS:
            raise ValueError("layout must be single, grid_1, grid_2, or grid_4")


@dataclass(frozen=True)
class DriverConsoleStatusSnapshot:
    side_bar: dict[str, Any]
    bottom_bar: dict[str, Any]

    @classmethod
    def from_telemetry(
        cls,
        telemetry: dict[str, Any],
        dashboard: DriverVideoDashboard,
        control_authority_state: str,
        packet_loss_percent: float,
    ) -> "DriverConsoleStatusSnapshot":
        link = dict(telemetry.get("link", {}))
        video_states = {
            camera_id: status.state for camera_id, status in dashboard.camera_status.items()
        }
        bitrate_by_camera = {
            camera_id: status.bitrate_kbps for camera_id, status in dashboard.camera_status.items()
        }
        fps_by_camera = {
            camera_id: status.fps for camera_id, status in dashboard.camera_status.items()
        }
        latency_by_camera = {
            camera_id: status.latency_ms for camera_id, status in dashboard.camera_status.items()
        }
        mock_telemetry = bool(telemetry.get("mock_telemetry", False))
        return cls(
            side_bar={
                "speed_mps": telemetry.get("speed_mps", 0.0),
                "gear": telemetry.get("gear", "N"),
                "steering_feedback": telemetry.get("steering_feedback", 0.0),
                "throttle_feedback": telemetry.get("throttle_feedback", 0.0),
                "brake_feedback": telemetry.get("brake_feedback", 0.0),
                "estop": bool(telemetry.get("estop", False)),
                "fault_flags": [str(flag) for flag in telemetry.get("fault_flags", [])],
                "mock_telemetry": mock_telemetry,
                "telemetry_source_label": "MOCK TELEMETRY" if mock_telemetry else "LIVE TELEMETRY",
                "control_connection_state": _control_connection_state(link),
                "vehicle_adapter": _vehicle_adapter_status(telemetry.get("vehicle_adapter", {})),
                "video_connection_state_by_camera": video_states,
            },
            bottom_bar={
                "rtt_ms": int(link.get("control_rtt_ms", 0)),
                "packet_loss_percent": packet_loss_percent,
                "bitrate_kbps_by_camera": bitrate_by_camera,
                "fps_by_camera": fps_by_camera,
                "latency_ms_by_camera": latency_by_camera,
                "session_id": str(telemetry.get("session_id", "")),
                "control_authority_state": control_authority_state,
            },
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "side_bar": dict(self.side_bar),
            "bottom_bar": dict(self.bottom_bar),
        }


@dataclass(frozen=True)
class DriverToolbarAction:
    enabled: bool
    visible: bool = True

    def to_dict(self) -> dict[str, Any]:
        return {
            "enabled": self.enabled,
            "visible": self.visible,
        }


@dataclass(frozen=True)
class DriverToolbarSnapshot:
    actions: dict[str, DriverToolbarAction]

    @classmethod
    def from_state(
        cls,
        logged_in: bool,
        connected: bool,
        session_active: bool,
    ) -> "DriverToolbarSnapshot":
        return cls(
            actions={
                "login": DriverToolbarAction(enabled=not logged_in),
                "logout": DriverToolbarAction(enabled=logged_in),
                "connect": DriverToolbarAction(enabled=logged_in and not connected),
                "disconnect": DriverToolbarAction(enabled=connected),
                "start_session": DriverToolbarAction(enabled=connected and not session_active),
                "end_session": DriverToolbarAction(enabled=session_active),
                "estop": DriverToolbarAction(enabled=session_active, visible=True),
                "settings": DriverToolbarAction(enabled=True),
            }
        )

    def to_dict(self) -> dict[str, Any]:
        return {
            "actions": {name: action.to_dict() for name, action in self.actions.items()}
        }


class EstopInputGuard:
    def __init__(self, required_hold_ms: int) -> None:
        if isinstance(required_hold_ms, bool) or not isinstance(required_hold_ms, int) or required_hold_ms <= 0:
            raise ValueError("required_hold_ms must be a positive integer")
        self.required_hold_ms = required_hold_ms
        self._pressed_since_ms: int | None = None
        self._armed = False

    def update(self, raw_pressed: bool, now_ms: int) -> bool:
        if not raw_pressed:
            self._pressed_since_ms = None
            self._armed = False
            return False
        if self._pressed_since_ms is None:
            self._pressed_since_ms = now_ms
        if now_ms - self._pressed_since_ms >= self.required_hold_ms:
            self._armed = True
        return self._armed


@dataclass(frozen=True)
class InputState:
    steering_left: bool = False
    steering_right: bool = False
    throttle_pressed: bool = False
    brake_pressed: bool = False
    estop_pressed: bool = False
    window_focused: bool = True
    gear: str = "N"
    steering_axis: float | None = None
    throttle_axis: float | None = None
    brake_axis: float | None = None


@dataclass(frozen=True)
class SoftwareControlState:
    steering: float | None = None
    throttle: float | None = None
    brake: float | None = None
    gear: str | None = None
    estop_pressed: bool = False


class DriverInputMerger:
    @staticmethod
    def merge(keyboard: InputState, software: SoftwareControlState) -> InputState:
        return InputState(
            steering_left=keyboard.steering_left if software.steering is None else False,
            steering_right=keyboard.steering_right if software.steering is None else False,
            throttle_pressed=keyboard.throttle_pressed if software.throttle is None else False,
            brake_pressed=keyboard.brake_pressed if software.brake is None else False,
            estop_pressed=keyboard.estop_pressed or software.estop_pressed,
            window_focused=keyboard.window_focused,
            gear=software.gear or keyboard.gear,
            steering_axis=software.steering,
            throttle_axis=software.throttle,
            brake_axis=software.brake,
        )


class ControlCommandGenerator:
    def __init__(
        self,
        vehicle_id: str,
        session_id: str,
        rate_hz: int,
        control_token: str = "",
        estop_redundant_sends: int = 3,
    ) -> None:
        if isinstance(rate_hz, bool) or not isinstance(rate_hz, int) or rate_hz <= 0:
            raise ValueError("rate_hz must be a positive integer")
        self.vehicle_id = vehicle_id
        self.session_id = session_id
        self.control_token = control_token
        self.period_ms = int(1000 / rate_hz)
        self._last_sent_ms: int | None = None
        self._seq = 0
        self._estop_was_pressed = False
        self._estop_repeat_remaining = 0
        self.estop_redundant_sends = max(1, estop_redundant_sends)

    def next_command(self, input_state: InputState, now_ms: int) -> ControlCommand | None:
        new_estop_press = input_state.estop_pressed and not self._estop_was_pressed
        if self._last_sent_ms is not None and now_ms - self._last_sent_ms < self.period_ms and not new_estop_press:
            self._estop_was_pressed = input_state.estop_pressed
            return None
        if input_state.gear not in ALLOWED_GEARS:
            raise ValueError(f"gear must be one of {sorted(ALLOWED_GEARS)}")
        if new_estop_press:
            self._estop_repeat_remaining = self.estop_redundant_sends
        self._last_sent_ms = now_ms
        self._seq += 1
        focused = input_state.window_focused
        steering = 0.0
        if focused and input_state.steering_axis is not None:
            steering = _clamp(input_state.steering_axis, -1.0, 1.0)
        elif focused and input_state.steering_left and not input_state.steering_right:
            steering = -1.0
        elif focused and input_state.steering_right and not input_state.steering_left:
            steering = 1.0
        if focused and input_state.brake_axis is not None:
            brake = _clamp(input_state.brake_axis, 0.0, 1.0)
        else:
            brake = 1.0 if focused and input_state.brake_pressed else 0.0
        if brake or not focused:
            throttle = 0.0
        elif input_state.throttle_axis is not None:
            throttle = _clamp(input_state.throttle_axis, 0.0, 1.0)
        else:
            throttle = 1.0 if input_state.throttle_pressed else 0.0
        estop = input_state.estop_pressed or self._estop_repeat_remaining > 0
        if self._estop_repeat_remaining > 0:
            self._estop_repeat_remaining -= 1
        self._estop_was_pressed = input_state.estop_pressed
        return ControlCommand(
            vehicle_id=self.vehicle_id,
            session_id=self.session_id,
            seq=self._seq,
            ts_ms=now_ms,
            gear=input_state.gear,
            steering=steering,
            throttle=throttle,
            brake=brake,
            estop=estop,
            authority_token=self.control_token,
        )


def _clamp(value: float, low: float, high: float) -> float:
    return max(low, min(high, float(value)))


def _control_connection_state(link: dict[str, Any]) -> str:
    if link.get("signaling_connected") is False:
        return "signaling_disconnected"
    return "connected"


def _vehicle_adapter_status(status: Any) -> dict[str, Any]:
    if not isinstance(status, dict):
        status = {}
    return {
        "adapter_type": str(status.get("adapter_type", "unknown")),
        "opened": bool(status.get("opened", False)),
        "healthy": bool(status.get("healthy", False)),
        "can_interface": status.get("can_interface"),
        "last_error": status.get("last_error"),
    }
