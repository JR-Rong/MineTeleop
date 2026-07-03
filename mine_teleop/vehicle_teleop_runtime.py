from __future__ import annotations

import json
import time
from pathlib import Path
from typing import Any
from urllib import request
from urllib.parse import urlencode, urlparse, urlunparse

from .config import VehicleConfig, load_vehicle_config
from .control import ControlCommand, ReceiveResult
from .vehicle_control_service import VehicleControlService


class VehicleTeleopRuntime:
    """Vehicle-end counterpart to the driver console.

    It registers the vehicle online with the signaling service, discovers the
    session created by a connecting driver, polls the signaling relay for
    ``control_command`` messages addressed to the vehicle, and feeds each one
    into a :class:`VehicleControlService` so the vehicle actually executes the
    remote driver's steering/throttle/brake (and e-stop) commands.
    """

    def __init__(
        self,
        config: VehicleConfig,
        *,
        signaling_http_url: str,
        device_token: str,
        telemetry_interval_ms: int = 100,
    ) -> None:
        self.config = config
        self.signaling_http_url = _normalize_signaling_http_url(signaling_http_url)
        self.vehicle_id = config.vehicle_id
        self.device_token = device_token
        self.telemetry_interval_ms = telemetry_interval_ms
        self.session_id = ""
        self.service: VehicleControlService | None = None
        self.processed_control_commands = 0
        self._last_applied_command: ControlCommand | None = None
        self._last_result: ReceiveResult | None = None

    @classmethod
    def from_config(
        cls,
        path: str | Path,
        *,
        signaling_http_url: str | None = None,
        device_token: str,
        telemetry_interval_ms: int = 100,
    ) -> "VehicleTeleopRuntime":
        config = load_vehicle_config(Path(path))
        return cls(
            config,
            signaling_http_url=signaling_http_url or config.cloud.signaling_url,
            device_token=device_token,
            telemetry_interval_ms=telemetry_interval_ms,
        )

    def register_online(self) -> dict[str, Any]:
        return _json_post(
            f"{self.signaling_http_url}/vehicles/online",
            {"vehicle_id": self.vehicle_id, "device_token": self.device_token},
        )

    def register_offline(self) -> dict[str, Any]:
        return _json_post(
            f"{self.signaling_http_url}/vehicles/offline",
            {"vehicle_id": self.vehicle_id, "device_token": self.device_token},
        )

    def discover_session(self, *, now_ms: int | None = None) -> bool:
        query = urlencode({"device_token": self.device_token})
        payload = _json_get(f"{self.signaling_http_url}/vehicles/{self.vehicle_id}/session?{query}")
        session_id = str(payload.get("session_id", ""))
        if not session_id:
            return False
        if session_id != self.session_id:
            self._start_session(session_id, _now_ms() if now_ms is None else now_ms)
        return True

    def _start_session(self, session_id: str, now_ms: int) -> None:
        self.session_id = session_id
        self.service = VehicleControlService.from_config(
            self.config,
            session_id=session_id,
            telemetry_interval_ms=self.telemetry_interval_ms,
        )
        self.service.start(now_ms=now_ms)

    def poll_and_execute(self, *, now_ms: int | None = None) -> dict[str, Any]:
        if self.service is None or not self.session_id:
            raise RuntimeError("vehicle teleop runtime has no active session")
        timestamp = _now_ms() if now_ms is None else now_ms
        query = urlencode({"recipient": self.vehicle_id, "device_token": self.device_token})
        payload = _json_get(f"{self.signaling_http_url}/signaling/{self.session_id}/messages?{query}")
        messages = payload.get("messages", [])
        if not isinstance(messages, list):
            raise RuntimeError("signaling messages response must contain a messages list")
        received = 0
        applied = 0
        for message in messages:
            if not isinstance(message, dict) or message.get("type") != "control_command":
                continue
            received += 1
            command = ControlCommand.from_dict(message.get("payload", {}))
            result = self.service.receive_command(command, now_ms=timestamp)
            self._last_result = result
            if result.accepted and result.command is not None:
                applied += 1
                self._last_applied_command = result.command
        # Always advance the safety state machine so command-timeout braking and
        # telemetry continue even when no new command arrived this cycle.
        self.service.tick(timestamp)
        self.processed_control_commands += applied
        return {
            "received_control_commands": received,
            "applied_control_commands": applied,
            "safety_state": self.service.safety.state.value,
        }

    def run(
        self,
        *,
        duration_ms: int,
        poll_interval_ms: int = 50,
        session_wait_ms: int = 5_000,
        now_fn: Any = None,
    ) -> dict[str, Any]:
        clock = now_fn or _now_ms
        self.register_online()
        start_ms = clock()
        deadline_session = start_ms + session_wait_ms
        while not self.discover_session(now_ms=clock()):
            if clock() >= deadline_session:
                return {
                    "event": "vehicle_teleop_run",
                    "vehicle_id": self.vehicle_id,
                    "session_discovered": False,
                    "reason": "no_active_session",
                }
            time.sleep(poll_interval_ms / 1000.0)
        deadline = clock() + duration_ms
        while clock() < deadline:
            self.poll_and_execute(now_ms=clock())
            time.sleep(poll_interval_ms / 1000.0)
        summary = self.summary()
        summary["session_discovered"] = True
        return summary

    def summary(self) -> dict[str, Any]:
        if self.service is None:
            return {
                "event": "vehicle_teleop_run",
                "vehicle_id": self.vehicle_id,
                "session_id": self.session_id,
                "safety_state": "INIT",
                "applied_command_count": 0,
                "processed_control_commands": 0,
                "last_command": None,
            }
        return {
            "event": "vehicle_teleop_run",
            "vehicle_id": self.vehicle_id,
            "session_id": self.session_id,
            "safety_state": self.service.safety.state.value,
            "applied_command_count": self.service.applied_command_count,
            "processed_control_commands": self.processed_control_commands,
            "telemetry_count": len(self.service.telemetry_history),
            "last_command": self._last_applied_command.to_dict() if self._last_applied_command else None,
        }


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
