from __future__ import annotations

import math
import threading
from dataclasses import dataclass, replace
from typing import Any, Dict, Tuple


ALLOWED_GEARS = {"P", "R", "N", "D"}


@dataclass(frozen=True)
class ControlCommand:
    vehicle_id: str
    session_id: str
    seq: int
    ts_ms: int
    gear: str
    steering: float
    throttle: float
    brake: float
    estop: bool = False
    authority_token: str = ""
    protocol_version: int = 1
    message_type: str = "control_command"

    @classmethod
    def from_dict(cls, payload: Dict[str, Any]) -> "ControlCommand":
        if payload.get("type") != "control_command":
            raise ValueError("type must be control_command")
        command = cls(
            protocol_version=_required_int(payload, "protocol_version"),
            vehicle_id=_required_string(payload, "vehicle_id"),
            session_id=_required_string(payload, "session_id"),
            seq=_required_non_negative_int(payload, "seq"),
            ts_ms=_required_int(payload, "ts_ms"),
            gear=_required_string(payload, "gear"),
            steering=_required_number(payload, "steering"),
            throttle=_required_number(payload, "throttle"),
            brake=_required_number(payload, "brake"),
            estop=_optional_bool(payload, "estop", default=False),
            authority_token=_optional_string(payload, "authority_token", default=""),
        )
        command.validate()
        return command

    def validate(self) -> None:
        _validate_int_field(self.protocol_version, "protocol_version")
        if self.protocol_version != 1:
            raise ValueError("unsupported protocol_version")
        _validate_string_field(self.vehicle_id, "vehicle_id")
        if not self.vehicle_id:
            raise ValueError("vehicle_id is required")
        _validate_string_field(self.session_id, "session_id")
        if not self.session_id:
            raise ValueError("session_id is required")
        _validate_non_negative_int_field(self.seq, "seq")
        if self.seq < 0:
            raise ValueError("seq must be non-negative")
        _validate_int_field(self.ts_ms, "ts_ms")
        _validate_string_field(self.gear, "gear")
        if self.gear not in ALLOWED_GEARS:
            raise ValueError(f"gear must be one of {sorted(ALLOWED_GEARS)}")
        _validate_number_field(self.steering, "steering")
        _validate_number_field(self.throttle, "throttle")
        _validate_number_field(self.brake, "brake")
        if not isinstance(self.estop, bool):
            raise ValueError("estop must be a boolean")
        _validate_string_field(self.authority_token, "authority_token")
        _range(self.steering, -1.0, 1.0, "steering")
        _range(self.throttle, 0.0, 1.0, "throttle")
        _range(self.brake, 0.0, 1.0, "brake")

    def to_dict(self) -> Dict[str, Any]:
        return {
            "type": self.message_type,
            "protocol_version": self.protocol_version,
            "vehicle_id": self.vehicle_id,
            "session_id": self.session_id,
            "seq": self.seq,
            "ts_ms": self.ts_ms,
            "gear": self.gear,
            "steering": self.steering,
            "throttle": self.throttle,
            "brake": self.brake,
            "estop": self.estop,
            "authority_token": self.authority_token,
        }

    def replace(self, **changes: Any) -> "ControlCommand":
        command = replace(self, **changes)
        command.validate()
        return command


@dataclass(frozen=True)
class ReceiveResult:
    accepted: bool
    reason: str
    command: ControlCommand | None = None
    warnings: Tuple[str, ...] = ()


class LatestControlCommandMailbox:
    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._latest: ControlCommand | None = None
        self.dropped_count = 0

    def publish(self, command: ControlCommand) -> None:
        command.validate()
        with self._lock:
            if self._latest is not None:
                self.dropped_count += 1
            self._latest = command

    def pop_latest(self) -> ControlCommand | None:
        with self._lock:
            command = self._latest
            self._latest = None
            return command

    @property
    def pending_count(self) -> int:
        with self._lock:
            return 1 if self._latest is not None else 0


class ControlReceiver:
    def __init__(
        self,
        vehicle_id: str,
        session_id: str,
        max_command_gap_ms: int,
        protocol_version: int = 1,
        control_authority: bool = True,
        control_token: str = "",
        timestamp_warning_skew_ms: int = 5_000,
    ) -> None:
        if _invalid_int(max_command_gap_ms, minimum=1):
            raise ValueError("max_command_gap_ms must be a positive integer")
        if _invalid_int(timestamp_warning_skew_ms, minimum=0):
            raise ValueError("timestamp_warning_skew_ms must be a non-negative integer")
        self.vehicle_id = vehicle_id
        self.session_id = session_id
        self.max_command_gap_ms = max_command_gap_ms
        self.protocol_version = protocol_version
        self.control_authority = control_authority
        self.control_token = control_token
        self.timestamp_warning_skew_ms = timestamp_warning_skew_ms
        self.last_seq: int | None = None
        self.last_valid_receive_ms: int | None = None

    def accept(self, command: ControlCommand, receive_time_ms: int) -> ReceiveResult:
        if _invalid_int(receive_time_ms, minimum=0):
            raise ValueError("receive_time_ms must be a non-negative integer")
        try:
            command.validate()
        except ValueError as exc:
            return ReceiveResult(False, f"invalid_command:{exc}")
        if command.protocol_version != self.protocol_version:
            return ReceiveResult(False, "wrong_protocol_version")
        if command.vehicle_id != self.vehicle_id:
            return ReceiveResult(False, "wrong_vehicle")
        if command.session_id != self.session_id:
            return ReceiveResult(False, "wrong_session")
        if not self.control_authority:
            return ReceiveResult(False, "control_authority_missing")
        if self.control_token and command.authority_token != self.control_token:
            return ReceiveResult(False, "control_token_invalid")
        if self.last_seq is not None and command.seq <= self.last_seq:
            return ReceiveResult(False, "old_seq")
        if self.last_valid_receive_ms is not None and receive_time_ms < self.last_valid_receive_ms:
            return ReceiveResult(False, "receive_time_reversed")
        if (
            self.last_valid_receive_ms is not None
            and receive_time_ms - self.last_valid_receive_ms > self.max_command_gap_ms
            and not command.estop
        ):
            return ReceiveResult(False, "command_gap_exceeded")

        self.last_seq = command.seq
        self.last_valid_receive_ms = receive_time_ms
        warnings = []
        if abs(receive_time_ms - command.ts_ms) > self.timestamp_warning_skew_ms:
            warnings.append("driver_timestamp_skew")
        return ReceiveResult(True, "accepted", command, tuple(warnings))


def _range(value: float, minimum: float, maximum: float, label: str) -> None:
    if not math.isfinite(value):
        raise ValueError(f"{label} must be a finite number")
    if value < minimum or value > maximum:
        raise ValueError(f"{label} must be in [{minimum}, {maximum}]")


def _invalid_int(value: object, minimum: int) -> bool:
    return isinstance(value, bool) or not isinstance(value, int) or value < minimum


def _validate_int_field(value: object, key: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be an integer")


def _validate_non_negative_int_field(value: object, key: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be a non-negative integer")


def _validate_number_field(value: object, key: str) -> None:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{key} must be a number")


def _validate_string_field(value: object, key: str) -> None:
    if not isinstance(value, str):
        raise ValueError(f"{key} must be a string")


def _required_int(payload: Dict[str, Any], key: str) -> int:
    value = _required_value(payload, key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be an integer")
    return value


def _required_non_negative_int(payload: Dict[str, Any], key: str) -> int:
    value = _required_value(payload, key)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{key} must be a non-negative integer")
    if value < 0:
        raise ValueError(f"{key} must be non-negative")
    return value


def _required_number(payload: Dict[str, Any], key: str) -> float:
    value = _required_value(payload, key)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{key} must be a number")
    return float(value)


def _required_string(payload: Dict[str, Any], key: str) -> str:
    value = _required_value(payload, key)
    if not isinstance(value, str):
        raise ValueError(f"{key} must be a string")
    return value


def _required_value(payload: Dict[str, Any], key: str) -> Any:
    if key not in payload:
        raise ValueError(f"{key} is required")
    return payload[key]


def _optional_string(payload: Dict[str, Any], key: str, default: str) -> str:
    value = payload.get(key, default)
    if not isinstance(value, str):
        raise ValueError(f"{key} must be a string")
    return value


def _optional_bool(payload: Dict[str, Any], key: str, default: bool) -> bool:
    value = payload.get(key, default)
    if not isinstance(value, bool):
        raise ValueError(f"{key} must be a boolean")
    return value
