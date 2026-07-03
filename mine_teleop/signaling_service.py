from __future__ import annotations

import base64
import binascii
import hashlib
import hmac
import json
import re
import secrets
import threading
import time
from contextlib import contextmanager
from dataclasses import asdict, dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, Iterator, List
from urllib.parse import parse_qs, urlparse

from .config import IceConfig
from .control import ControlCommand
from .observability import AuditEvent, AuditLog
from .signaling import SessionError, SessionManager
from .upload import UploadCredentialService


SIGNALING_MESSAGE_TYPES = {
    "webrtc_offer",
    "webrtc_answer",
    "ice_candidate",
    "session_end",
    "control_command",
}

# Cap request/frame sizes so a malicious or buggy client cannot force an
# unbounded allocation (Content-Length / WebSocket frame length are attacker
# controlled). Signaling payloads are small JSON objects.
MAX_HTTP_BODY_BYTES = 256 * 1024
MAX_WEBSOCKET_MESSAGE_BYTES = 256 * 1024


def _time_ms() -> int:
    return int(time.time() * 1000)


def _session_error_status(exc: SessionError) -> int:
    message = str(exc)
    if (
        "sender is not current session participant" in message
        or "recipient is not current session participant" in message
        or "sender is not authenticated websocket participant" in message
    ):
        return 403
    if "unknown session" in message:
        return 404
    if "control authority" in message:
        return 409
    return 400


def _required_non_negative_int(payload: Dict[str, Any], field: str) -> int:
    value = payload.get(field)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field} must be a non-negative integer")
    if value < 0:
        raise ValueError(f"{field} must be non-negative")
    return value


def _required_non_negative_number(payload: Dict[str, Any], field: str) -> float:
    value = payload.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a non-negative number")
    if value < 0:
        raise ValueError(f"{field} must be non-negative")
    return float(value)


def _required_json_string(payload: Dict[str, Any], field: str) -> str:
    value = payload.get(field)
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a string")
    return value


def _optional_json_string(payload: Dict[str, Any], field: str, default: str = "") -> str:
    value = payload.get(field, default)
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a string")
    return value


def _require_non_empty_string_value(value: Any, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise ValueError(f"{field} must be a non-empty string")
    return value


def _require_string_value(value: Any, field: str) -> str:
    if not isinstance(value, str):
        raise ValueError(f"{field} must be a string")
    return value


def _required_positive_int_value(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field} must be a positive integer")
    if value <= 0:
        raise ValueError(f"{field} must be positive")
    return value


def _required_non_negative_int_value(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{field} must be a non-negative integer")
    if value < 0:
        raise ValueError(f"{field} must be non-negative")
    return value


def _required_non_negative_number_value(value: Any, field: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise ValueError(f"{field} must be a non-negative number")
    if value < 0:
        raise ValueError(f"{field} must be non-negative")
    return float(value)


def _verify_driver_password(password: str, credential: DriverPasswordCredential) -> bool:
    if credential.algorithm != "pbkdf2_sha256":
        return False
    if isinstance(credential.iterations, bool) or not isinstance(credential.iterations, int) or credential.iterations <= 0:
        return False
    try:
        salt = base64.b64decode(credential.salt.encode("ascii"), validate=True)
        expected = base64.b64decode(credential.digest.encode("ascii"), validate=True)
    except (ValueError, UnicodeEncodeError):
        return False
    actual = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, credential.iterations)
    return hmac.compare_digest(actual, expected)


def _optional_json_object(payload: Dict[str, Any], field: str) -> Dict[str, Any]:
    value = payload.get(field, {})
    if not isinstance(value, dict):
        raise ValueError(f"{field} must be a JSON object")
    return dict(value)


@dataclass(frozen=True)
class SignalingMessage:
    session_id: str
    sender: str
    recipient: str
    type: str
    payload: Dict[str, Any]


@dataclass
class TurnUsageSummary:
    session_id: str
    vehicle_id: str
    relay_bytes_total: int = 0
    sample_count: int = 0
    last_bitrate_kbps: float = 0.0

    def to_dict(self) -> Dict[str, Any]:
        return {
            "session_id": self.session_id,
            "vehicle_id": self.vehicle_id,
            "relay_bytes_total": self.relay_bytes_total,
            "sample_count": self.sample_count,
            "last_bitrate_kbps": self.last_bitrate_kbps,
        }


@dataclass(frozen=True)
class CoturnUsageSample:
    session_id: str
    actor: str
    bytes_sent: int
    bytes_received: int
    duration_ms: int
    source: str = "coturn"


class CoturnUsageLogParser:
    def parse_line(self, line: str) -> CoturnUsageSample | None:
        if "usage:" not in line:
            return None
        fields = _parse_coturn_usage_fields(line)
        username = fields.get("username", "")
        session_id, actor = _split_coturn_username(username)
        if not session_id or not actor:
            return None
        duration_ms = _positive_int_from_fields(fields, "duration_ms")
        if duration_ms is None:
            duration_ms = _positive_int_from_fields(fields, "duration")
        if duration_ms is None:
            return None
        bytes_received = _non_negative_int_from_fields(fields, "rb")
        bytes_sent = _non_negative_int_from_fields(fields, "sb")
        if bytes_received is None or bytes_sent is None:
            return None
        return CoturnUsageSample(
            session_id=session_id,
            actor=actor,
            bytes_sent=bytes_sent,
            bytes_received=bytes_received,
            duration_ms=duration_ms,
        )


@dataclass(frozen=True)
class DriverLoginToken:
    token: str
    driver_id: str
    issued_at_ms: int
    expires_at_ms: int


@dataclass(frozen=True)
class DriverPasswordCredential:
    algorithm: str
    iterations: int
    salt: str
    digest: str


class DriverCredentialStore:
    def __init__(self, credentials: Dict[str, DriverPasswordCredential]) -> None:
        self._credentials = dict(credentials)

    @classmethod
    def from_passwords(
        cls,
        passwords_by_driver: Dict[str, str],
        *,
        iterations: int = 120_000,
    ) -> "DriverCredentialStore":
        credentials: Dict[str, DriverPasswordCredential] = {}
        for driver_id, password in passwords_by_driver.items():
            driver_id = _require_non_empty_string_value(driver_id, "driver_id")
            password = _require_non_empty_string_value(password, "password")
            credentials[driver_id] = cls.hash_password(password, iterations=iterations)
        return cls(credentials)

    @classmethod
    def from_json_file(cls, path: str | Path) -> "DriverCredentialStore":
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("driver credential file must be a JSON object")
        drivers = data.get("drivers")
        if not isinstance(drivers, dict):
            raise ValueError("driver credential file must contain a drivers object")
        credentials: Dict[str, DriverPasswordCredential] = {}
        for driver_id, raw in drivers.items():
            driver_id = _require_non_empty_string_value(driver_id, "driver_id")
            if not isinstance(raw, dict):
                raise ValueError("driver credential entry must be a JSON object")
            credentials[driver_id] = DriverPasswordCredential(
                algorithm=_require_non_empty_string_value(raw.get("algorithm"), "algorithm"),
                iterations=_required_positive_int_value(raw.get("iterations"), "iterations"),
                salt=_require_non_empty_string_value(raw.get("salt"), "salt"),
                digest=_require_non_empty_string_value(raw.get("digest"), "digest"),
            )
        return cls(credentials)

    @staticmethod
    def hash_password(password: str, *, iterations: int = 120_000) -> DriverPasswordCredential:
        password = _require_non_empty_string_value(password, "password")
        if isinstance(iterations, bool) or not isinstance(iterations, int) or iterations <= 0:
            raise ValueError("iterations must be positive")
        salt_bytes = secrets.token_bytes(16)
        digest = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt_bytes, iterations)
        return DriverPasswordCredential(
            algorithm="pbkdf2_sha256",
            iterations=iterations,
            salt=base64.b64encode(salt_bytes).decode("ascii"),
            digest=base64.b64encode(digest).decode("ascii"),
        )

    def validate(self, driver_id: str, password: str) -> None:
        driver_id = _require_non_empty_string_value(driver_id, "driver_id")
        password = _require_string_value(password, "password")
        credential = self._credentials.get(driver_id)
        if credential is None or not _verify_driver_password(password, credential):
            raise PermissionError("invalid driver credentials")


class DriverTokenStore:
    def __init__(
        self,
        token_ttl_ms: int = 30 * 60 * 1000,
        clock_ms: Callable[[], int] | None = None,
        credentials: DriverCredentialStore | None = None,
    ) -> None:
        if isinstance(token_ttl_ms, bool) or not isinstance(token_ttl_ms, int) or token_ttl_ms <= 0:
            raise ValueError("token_ttl_ms must be positive")
        self.token_ttl_ms = token_ttl_ms
        self._clock_ms = clock_ms or _time_ms
        self._tokens: Dict[str, DriverLoginToken] = {}
        self._counter = 0
        self._credentials = credentials
        self._lock = threading.Lock()

    def login(self, driver_id: str, password: str) -> DriverLoginToken:
        driver_id = _require_non_empty_string_value(driver_id, "driver_id")
        password = _require_string_value(password, "password")
        if self._credentials is not None:
            self._credentials.validate(driver_id, password)
        elif password != "dev-password":
            raise PermissionError("invalid driver credentials")
        self._counter += 1
        # Opaque random token; must not be guessable from driver_id or a counter.
        token = f"driver-token-{secrets.token_urlsafe(24)}"
        issued_at_ms = self._clock_ms()
        login_token = DriverLoginToken(
            token=token,
            driver_id=driver_id,
            issued_at_ms=issued_at_ms,
            expires_at_ms=issued_at_ms + self.token_ttl_ms,
        )
        with self._lock:
            self._tokens[token] = login_token
        return login_token

    def validate(self, driver_id: str, token: str) -> None:
        driver_id = _require_non_empty_string_value(driver_id, "driver_id")
        token = _require_string_value(token, "driver token")
        with self._lock:
            login_token = self._tokens.get(token)
        if login_token is None or login_token.driver_id != driver_id:
            raise PermissionError("invalid driver token")
        if self._clock_ms() >= login_token.expires_at_ms:
            raise PermissionError("driver token expired")


class DeviceCredentialStore:
    def __init__(self, tokens_by_vehicle: Dict[str, str] | None = None) -> None:
        self._tokens_by_vehicle: Dict[str, str] = {}
        self._lock = threading.Lock()
        if tokens_by_vehicle:
            for vehicle_id, device_token in tokens_by_vehicle.items():
                self.register(vehicle_id, device_token)

    @classmethod
    def from_tokens(cls, tokens_by_vehicle: Dict[str, str]) -> "DeviceCredentialStore":
        return cls(tokens_by_vehicle)

    @classmethod
    def from_json_file(cls, path: str | Path) -> "DeviceCredentialStore":
        data = json.loads(Path(path).read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError("device credential file must be a JSON object")
        vehicles = data.get("vehicles")
        if not isinstance(vehicles, dict):
            raise ValueError("device credential file must contain a vehicles object")
        return cls.from_tokens(vehicles)

    def register(self, vehicle_id: str, device_token: str) -> None:
        vehicle_id = _require_non_empty_string_value(vehicle_id, "vehicle_id")
        device_token = _require_non_empty_string_value(device_token, "device_token")
        with self._lock:
            self._tokens_by_vehicle[vehicle_id] = device_token

    def validate(self, vehicle_id: str, device_token: str) -> None:
        vehicle_id = _require_non_empty_string_value(vehicle_id, "vehicle_id")
        device_token = _require_string_value(device_token, "device_token")
        with self._lock:
            expected = self._tokens_by_vehicle.get(vehicle_id)
        if expected is None or not hmac.compare_digest(expected, device_token):
            raise PermissionError("invalid device token")


class SignalingMessageStore:
    def __init__(self) -> None:
        self._messages: Dict[tuple[str, str], List[SignalingMessage]] = {}
        self._lock = threading.Lock()

    def enqueue(self, message: SignalingMessage) -> int:
        key = (message.session_id, message.recipient)
        with self._lock:
            self._messages.setdefault(key, []).append(message)
            return len(self._messages[key])

    def pop_for(self, session_id: str, recipient: str) -> List[SignalingMessage]:
        with self._lock:
            return self._messages.pop((session_id, recipient), [])


class SignalingHttpService:
    def __init__(
        self,
        audit_log_path: Path | str,
        driver_token_ttl_ms: int = 30 * 60 * 1000,
        clock_ms: Callable[[], int] | None = None,
        upload_credentials: UploadCredentialService | None = None,
        ice_config: IceConfig | None = None,
        driver_credentials: DriverCredentialStore | None = None,
        device_credentials: DeviceCredentialStore | None = None,
        audit_log_max_bytes: int | None = None,
        audit_log_backup_count: int = 0,
    ) -> None:
        self._clock_ms = clock_ms or _time_ms
        self.sessions = SessionManager()
        self.tokens = DriverTokenStore(
            token_ttl_ms=driver_token_ttl_ms,
            clock_ms=self._clock_ms,
            credentials=driver_credentials,
        )
        if device_credentials is None:
            self.devices = DeviceCredentialStore()
            self.devices.register("vehicle-001", "dev-device-secret")
        else:
            self.devices = device_credentials
        self.messages = SignalingMessageStore()
        self.turn_usage_by_session: Dict[str, TurnUsageSummary] = {}
        self._turn_usage_lock = threading.Lock()
        self.audit = AuditLog(audit_log_path, max_bytes=audit_log_max_bytes, backup_count=audit_log_backup_count)
        self.uploads = upload_credentials or UploadCredentialService()
        self.ice_config = ice_config

    def make_server(self, host: str = "127.0.0.1", port: int = 0) -> ThreadingHTTPServer:
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
        service = self

        class Handler(BaseHTTPRequestHandler):
            def do_GET(self) -> None:
                parsed = urlparse(self.path)
                if parsed.path == "/health":
                    self._json_response(200, {"status": "ok"})
                    return
                parts = _path_parts(parsed.path)
                if len(parts) == 3 and parts[0] == "signaling" and parts[2] == "ws":
                    query = parse_qs(parsed.query)
                    participant = query.get("participant", [""])[0]
                    if not participant:
                        self._json_response(400, {"error": "participant is required"})
                        return
                    try:
                        session = service.sessions.require_participant(parts[1], participant)
                        service._require_signaling_credential(
                            session,
                            participant,
                            token=query.get("token", [""])[0],
                            device_token=query.get("device_token", [""])[0],
                        )
                    except PermissionError as exc:
                        self._json_response(401, {"error": str(exc)})
                        return
                    except SessionError as exc:
                        self._json_response(_session_error_status(exc), {"error": str(exc)})
                        return
                    self._websocket_loop(parts[1], participant)
                    return
                if len(parts) == 3 and parts[0] == "signaling" and parts[2] == "messages":
                    query = parse_qs(parsed.query)
                    recipient = query.get("recipient", [""])[0]
                    if not recipient:
                        self._json_response(400, {"error": "recipient is required"})
                        return
                    try:
                        session = service.sessions.require_participant(parts[1], recipient)
                        service._require_signaling_credential(
                            session,
                            recipient,
                            token=query.get("token", [""])[0],
                            device_token=query.get("device_token", [""])[0],
                        )
                    except PermissionError as exc:
                        self._json_response(401, {"error": str(exc)})
                        return
                    except SessionError as exc:
                        self._json_response(_session_error_status(exc), {"error": str(exc)})
                        return
                    messages = service.messages.pop_for(parts[1], recipient)
                    self._json_response(200, {"messages": [asdict(message) for message in messages]})
                    return
                if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "ice_servers":
                    query = parse_qs(parsed.query)
                    actor = query.get("actor", [""])[0]
                    if not actor:
                        self._json_response(400, {"error": "actor is required"})
                        return
                    try:
                        payload = service.issue_ice_servers(
                            session_id=parts[1],
                            actor=actor,
                            token=query.get("token", [""])[0],
                            device_token=query.get("device_token", [""])[0],
                        )
                    except PermissionError as exc:
                        self._json_response(401, {"error": str(exc)})
                        return
                    except SessionError as exc:
                        self._json_response(_session_error_status(exc), {"error": str(exc)})
                        return
                    except ValueError as exc:
                        self._json_response(400, {"error": str(exc)})
                        return
                    self._json_response(200, payload)
                    return
                if len(parts) == 3 and parts[0] == "vehicles" and parts[2] == "session":
                    query = parse_qs(parsed.query)
                    vehicle_id = parts[1]
                    try:
                        service.devices.validate(vehicle_id, query.get("device_token", [""])[0])
                    except PermissionError as exc:
                        self._json_response(401, {"error": str(exc)})
                        return
                    active = service.sessions.active_session_for_vehicle(vehicle_id)
                    if active is None:
                        self._json_response(200, {"vehicle_id": vehicle_id, "session_id": "", "state": "none"})
                        return
                    self._json_response(
                        200,
                        {
                            "vehicle_id": vehicle_id,
                            "session_id": active.session_id,
                            "driver_id": active.driver_id,
                            "state": active.state,
                        },
                    )
                    return
                self._json_response(404, {"error": "not found"})

            def do_POST(self) -> None:
                parsed = urlparse(self.path)
                parts = _path_parts(parsed.path)
                try:
                    payload = self._read_json()
                    if parsed.path == "/auth/driver_login":
                        driver_id = _required_json_string(payload, "driver_id")
                        token = service.tokens.login(driver_id, payload.get("password", ""))
                        service._audit(
                            "driver_login",
                            vehicle_id="",
                            session_id="",
                            actor=driver_id,
                        )
                        self._json_response(
                            200,
                            {
                                "token_type": "bearer",
                                "token": token.token,
                                "expires_at_ms": token.expires_at_ms,
                            },
                        )
                        return
                    if parsed.path == "/vehicles/online":
                        vehicle_id = _required_json_string(payload, "vehicle_id")
                        service._require_device_token(payload)
                        service.sessions.vehicle_online(vehicle_id)
                        service._audit("vehicle_online", vehicle_id=vehicle_id, session_id="", actor=vehicle_id)
                        self._json_response(200, {"vehicle_id": vehicle_id, "state": "online"})
                        return
                    if parsed.path == "/vehicles/offline":
                        vehicle_id = _required_json_string(payload, "vehicle_id")
                        service._require_device_token(payload)
                        offline = service.record_vehicle_offline(
                            vehicle_id=vehicle_id,
                            reason=_optional_json_string(payload, "reason", default="reported_offline"),
                        )
                        self._json_response(200, offline)
                        return
                    if parsed.path == "/sessions":
                        driver_id = _required_json_string(payload, "driver_id")
                        vehicle_id = _required_json_string(payload, "vehicle_id")
                        service.tokens.validate(driver_id, _optional_json_string(payload, "token"))
                        session = service.sessions.request_session(vehicle_id, driver_id)
                        service._audit(
                            "session_started",
                            vehicle_id=session.vehicle_id,
                            session_id=session.session_id,
                            actor=driver_id,
                        )
                        service._audit(
                            "control_authority_granted",
                            vehicle_id=session.vehicle_id,
                            session_id=session.session_id,
                            actor=driver_id,
                            details={"driver_id": driver_id},
                        )
                        self._json_response(200, asdict(session))
                        return
                    if parsed.path == "/uploads/credentials":
                        service._require_device_token(payload)
                        credential = service.uploads.issue(payload)
                        service._audit(
                            "upload_credential_issued",
                            vehicle_id=str(payload.get("vehicle_id", "")),
                            session_id=str(payload.get("session_id", "")),
                            actor=str(payload.get("vehicle_id", "")),
                            details={
                                "segment_id": credential.segment_id,
                                "kind": credential.kind,
                                "object_path": credential.object_path,
                            },
                        )
                        self._json_response(200, asdict(credential))
                        return
                    if parsed.path == "/uploads/complete":
                        service._require_device_token(payload)
                        segment_id = _required_json_string(payload, "segment_id")
                        object_path = _required_json_string(payload, "object_path")
                        context = _upload_audit_context_from_object_path(object_path)
                        _require_segment_matches_object_path(segment_id, context)
                        record = service.uploads.mark_uploaded(
                            segment_id=segment_id,
                            object_path=object_path,
                            bytes_uploaded=_required_non_negative_int(payload, "bytes_uploaded"),
                        )
                        service._audit(
                            "upload_success",
                            vehicle_id=context["vehicle_id"],
                            session_id=context["session_id"],
                            actor=context["vehicle_id"] or "vehicle",
                            details=asdict(record),
                        )
                        self._json_response(200, asdict(record))
                        return
                    if parsed.path == "/uploads/failed":
                        service._require_device_token(payload)
                        segment_id = _required_json_string(payload, "segment_id")
                        object_path = _required_json_string(payload, "object_path")
                        context = _upload_audit_context_from_object_path(object_path)
                        _require_segment_matches_object_path(segment_id, context)
                        record = service.uploads.mark_failed(
                            segment_id=segment_id,
                            object_path=object_path,
                            error=_required_json_string(payload, "error"),
                        )
                        service._audit(
                            "upload_failed",
                            vehicle_id=context["vehicle_id"],
                            session_id=context["session_id"],
                            actor=context["vehicle_id"] or "vehicle",
                            details=asdict(record),
                        )
                        self._json_response(200, asdict(record))
                        return
                    if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "end":
                        session_id = parts[1]
                        actor = _optional_json_string(payload, "actor")
                        session = service.sessions.require_participant(session_id, actor)
                        service._require_signaling_credential(
                            session,
                            actor,
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        service.sessions.end_session(session_id)
                        service._audit(
                            "session_ended",
                            vehicle_id=session.vehicle_id,
                            session_id=session_id,
                            actor=actor,
                        )
                        service._audit(
                            "control_authority_revoked",
                            vehicle_id=session.vehicle_id,
                            session_id=session_id,
                            actor=actor,
                            details={"reason": "session_end"},
                        )
                        self._json_response(200, asdict(service.sessions.sessions[session_id]))
                        return
                    if (
                        len(parts) == 4
                        and parts[0] == "sessions"
                        and parts[2] == "control_authority"
                        and parts[3] == "revoke"
                    ):
                        session_id = parts[1]
                        actor = _optional_json_string(payload, "actor")
                        reason = _optional_json_string(payload, "reason", default="operator_revoke") or "operator_revoke"
                        session = service.sessions.require_participant(session_id, actor)
                        service._require_signaling_credential(
                            session,
                            actor,
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        revoked = service.sessions.revoke_control_authority(session_id)
                        service._audit(
                            "control_authority_revoked",
                            vehicle_id=revoked.vehicle_id,
                            session_id=session_id,
                            actor=actor,
                            details={"reason": reason},
                        )
                        self._json_response(200, asdict(revoked))
                        return
                    if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "abnormal_disconnect":
                        recorded = service.record_abnormal_disconnect(
                            session_id=parts[1],
                            actor=_optional_json_string(payload, "actor"),
                            reason=_optional_json_string(payload, "reason"),
                            detected_by=_optional_json_string(payload, "detected_by", default="cloud"),
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        self._json_response(200, recorded)
                        return
                    if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "diagnostics":
                        recorded = service.record_realtime_diagnostics(
                            session_id=parts[1],
                            actor=_optional_json_string(payload, "actor"),
                            component=_optional_json_string(payload, "component"),
                            rtt_ms=_required_non_negative_int(payload, "rtt_ms"),
                            packet_loss_percent=_required_non_negative_number(payload, "packet_loss_percent"),
                            jitter_ms=_required_non_negative_int(payload, "jitter_ms"),
                            video_latency_ms=_required_non_negative_int(payload, "video_latency_ms"),
                            control_send_hz=_required_non_negative_number(payload, "control_send_hz"),
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        self._json_response(200, recorded)
                        return
                    if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "control_timeout":
                        recorded = service.record_control_timeout(
                            session_id=parts[1],
                            actor=_optional_json_string(payload, "actor"),
                            last_valid_receive_ms=_required_non_negative_int(payload, "last_valid_receive_ms"),
                            timeout_entered_ms=_required_non_negative_int(payload, "timeout_entered_ms"),
                            control_timeout_ms=_required_non_negative_int(payload, "control_timeout_ms"),
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        self._json_response(200, recorded)
                        return
                    if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "estop":
                        recorded = service.record_estop(
                            session_id=parts[1],
                            actor=_optional_json_string(payload, "actor"),
                            reason=_optional_json_string(payload, "reason"),
                            seq=_required_non_negative_int(payload, "seq"),
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        self._json_response(200, recorded)
                        return
                    if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "turn_relay":
                        recorded = service.record_turn_relay_enabled(
                            session_id=parts[1],
                            actor=_optional_json_string(payload, "actor"),
                            turn_url=_optional_json_string(payload, "turn_url"),
                            relay_candidate=_optional_json_string(payload, "relay_candidate"),
                            selected_pair=_optional_json_string(payload, "selected_pair"),
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        self._json_response(200, recorded)
                        return
                    if len(parts) == 3 and parts[0] == "sessions" and parts[2] == "turn_usage":
                        recorded = service.record_turn_relay_usage(
                            session_id=parts[1],
                            actor=_optional_json_string(payload, "actor"),
                            bytes_sent=_required_non_negative_int(payload, "bytes_sent"),
                            bytes_received=_required_non_negative_int(payload, "bytes_received"),
                            duration_ms=_required_non_negative_int(payload, "duration_ms"),
                            token=_optional_json_string(payload, "token"),
                            device_token=_optional_json_string(payload, "device_token"),
                        )
                        self._json_response(200, recorded)
                        return
                    if len(parts) == 3 and parts[0] == "signaling" and parts[2] == "messages":
                        session_id = parts[1]
                        self._json_response(200, service.enqueue_signaling_payload(session_id, payload))
                        return
                    self._json_response(404, {"error": "not found"})
                except PermissionError as exc:
                    self._json_response(401, {"error": str(exc)})
                except SessionError as exc:
                    self._json_response(_session_error_status(exc), {"error": str(exc)})
                except ValueError as exc:
                    self._json_response(400, {"error": str(exc)})

            def log_message(self, format: str, *args: Any) -> None:
                return

            def _read_json(self) -> Dict[str, Any]:
                raw_length = self.headers.get("Content-Length", "0")
                try:
                    length = int(raw_length)
                except (TypeError, ValueError):
                    raise ValueError("invalid Content-Length header")
                if length < 0:
                    raise ValueError("invalid Content-Length header")
                if length > MAX_HTTP_BODY_BYTES:
                    raise ValueError("request body too large")
                raw = self.rfile.read(length).decode("utf-8") if length else "{}"
                data = json.loads(raw)
                if not isinstance(data, dict):
                    raise ValueError("JSON body must be an object")
                return data

            def _json_response(self, status: int, payload: Dict[str, Any]) -> None:
                body = json.dumps(payload, ensure_ascii=False, sort_keys=True).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def _websocket_loop(self, session_id: str, participant: str) -> None:
                if self.headers.get("Upgrade", "").lower() != "websocket":
                    self._json_response(400, {"error": "WebSocket Upgrade header is required"})
                    return
                connection_tokens = {token.strip().lower() for token in self.headers.get("Connection", "").split(",")}
                if "upgrade" not in connection_tokens:
                    self._json_response(400, {"error": "Connection: Upgrade header is required"})
                    return
                if self.headers.get("Sec-WebSocket-Version", "") != "13":
                    self._json_response(400, {"error": "Sec-WebSocket-Version must be 13"})
                    return
                key = self.headers.get("Sec-WebSocket-Key", "")
                if not key:
                    self._json_response(400, {"error": "Sec-WebSocket-Key is required"})
                    return
                try:
                    decoded_key = base64.b64decode(key.encode("ascii"), validate=True)
                except (binascii.Error, UnicodeEncodeError):
                    self._json_response(400, {"error": "Sec-WebSocket-Key must be a base64-encoded 16-byte value"})
                    return
                if len(decoded_key) != 16:
                    self._json_response(400, {"error": "Sec-WebSocket-Key must be a base64-encoded 16-byte value"})
                    return
                accept = base64.b64encode(
                    hashlib.sha1((key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode("ascii")).digest()
                ).decode("ascii")
                self.send_response(101, "Switching Protocols")
                self.send_header("Upgrade", "websocket")
                self.send_header("Connection", "Upgrade")
                self.send_header("Sec-WebSocket-Accept", accept)
                self.end_headers()

                while True:
                    try:
                        payload = self._read_websocket_json()
                    except ValueError as exc:
                        self._write_websocket_json({"error": str(exc)})
                        return
                    if payload is None:
                        return
                    try:
                        response = service.enqueue_signaling_payload(
                            session_id,
                            payload,
                            authenticated_actor=participant,
                        )
                    except (PermissionError, SessionError, ValueError) as exc:
                        response = {"error": str(exc)}
                    self._write_websocket_json(response)

            def _read_websocket_json(self) -> Dict[str, Any] | None:
                while True:
                    header = self.rfile.read(2)
                    if len(header) < 2:
                        return None
                    fin = bool(header[0] & 0x80)
                    opcode = header[0] & 0x0F
                    masked = bool(header[1] & 0x80)
                    length = header[1] & 0x7F
                    if not masked:
                        raise ValueError("client websocket frames must be masked")
                    if length == 126:
                        length = int.from_bytes(self.rfile.read(2), "big")
                    elif length == 127:
                        length = int.from_bytes(self.rfile.read(8), "big")
                    if opcode in {8, 9, 10} and length > 125:
                        raise ValueError("websocket control frames must be no longer than 125 bytes")
                    if length > MAX_WEBSOCKET_MESSAGE_BYTES:
                        raise ValueError("websocket message too large")
                    mask = self.rfile.read(4) if masked else b""
                    raw = self.rfile.read(length)
                    if masked:
                        raw = bytes(byte ^ mask[index % 4] for index, byte in enumerate(raw))
                    if opcode == 8:  # close
                        return None
                    if opcode == 9:  # ping -> reply with pong and keep the connection open
                        self._write_websocket_frame(0x8A, raw)
                        continue
                    if opcode == 10:  # pong -> ignore keepalive
                        continue
                    if not fin:
                        raise ValueError("fragmented websocket messages are not supported")
                    if opcode != 1:
                        raise ValueError("only text websocket frames are supported")
                    data = json.loads(raw.decode("utf-8"))
                    if not isinstance(data, dict):
                        raise ValueError("websocket message must be a JSON object")
                    return data

            def _write_websocket_frame(self, first_byte: int, body: bytes) -> None:
                header = bytearray([first_byte])
                if len(body) < 126:
                    header.append(len(body))
                elif len(body) < 65536:
                    header.extend([126, (len(body) >> 8) & 0xFF, len(body) & 0xFF])
                else:
                    header.extend([127, *len(body).to_bytes(8, "big")])
                self.wfile.write(bytes(header) + body)
                self.wfile.flush()

            def _write_websocket_json(self, payload: Dict[str, Any]) -> None:
                body = json.dumps(payload, ensure_ascii=False, sort_keys=True).encode("utf-8")
                self._write_websocket_frame(0x81, body)

        return Handler

    def enqueue_signaling_payload(
        self,
        session_id: str,
        payload: Dict[str, Any],
        authenticated_actor: str | None = None,
    ) -> Dict[str, int]:
        message_type = _required_json_string(payload, "type")
        if message_type not in SIGNALING_MESSAGE_TYPES:
            raise ValueError("unsupported signaling message type")
        sender = _required_json_string(payload, "sender")
        session = self.sessions.require_participant(session_id, sender)
        if authenticated_actor is None:
            self._require_signaling_credential(
                session,
                sender,
                token=_optional_json_string(payload, "token"),
                device_token=_optional_json_string(payload, "device_token"),
            )
        elif sender != authenticated_actor:
            raise SessionError("sender is not authenticated websocket participant")
        recipient = _required_json_string(payload, "recipient")
        if recipient not in {session.driver_id, session.vehicle_id}:
            raise SessionError("recipient is not current session participant")
        message_payload = _optional_json_object(payload, "payload")
        if message_type == "control_command":
            command = ControlCommand.from_dict(message_payload)
            if sender != session.driver_id:
                raise SessionError("control_command sender must be current driver")
            if recipient != session.vehicle_id:
                raise SessionError("control_command recipient must be current vehicle")
            if command.vehicle_id != session.vehicle_id:
                raise SessionError("control_command vehicle_id is not current session vehicle")
            if command.session_id != session_id:
                raise SessionError("control_command session_id is not current session")
            if command.authority_token != session.control_token:
                raise SessionError("control authority token is invalid")
        message = SignalingMessage(
            session_id=session_id,
            sender=sender,
            recipient=recipient,
            type=message_type,
            payload=message_payload,
        )
        queued = self.messages.enqueue(message)
        self._audit(
            message_type,
            vehicle_id=session.vehicle_id,
            session_id=session_id,
            actor=message.sender,
            details={"recipient": message.recipient},
        )
        return {"queued": queued}

    def record_abnormal_disconnect(
        self,
        session_id: str,
        actor: str,
        reason: str,
        detected_by: str = "cloud",
        token: str = "",
        device_token: str = "",
    ) -> Dict[str, str]:
        if not reason:
            raise ValueError("abnormal disconnect reason is required")
        session = self.sessions.require_participant(session_id, actor)
        self._require_signaling_credential(session, actor, token=token, device_token=device_token)
        self._audit(
            "abnormal_disconnect",
            vehicle_id=session.vehicle_id,
            session_id=session_id,
            actor=actor,
            details={"reason": reason, "detected_by": detected_by or "cloud"},
        )
        return {"event": "abnormal_disconnect", "session_id": session_id}

    def record_realtime_diagnostics(
        self,
        session_id: str,
        actor: str,
        component: str,
        rtt_ms: int,
        packet_loss_percent: float,
        jitter_ms: int,
        video_latency_ms: int,
        control_send_hz: float,
        token: str = "",
        device_token: str = "",
    ) -> Dict[str, str]:
        if not isinstance(component, str):
            raise ValueError("component must be a string")
        if not component:
            raise ValueError("diagnostics component is required")
        rtt_ms = _required_non_negative_int_value(rtt_ms, "rtt_ms")
        packet_loss_percent = _required_non_negative_number_value(packet_loss_percent, "packet_loss_percent")
        jitter_ms = _required_non_negative_int_value(jitter_ms, "jitter_ms")
        video_latency_ms = _required_non_negative_int_value(video_latency_ms, "video_latency_ms")
        control_send_hz = _required_non_negative_number_value(control_send_hz, "control_send_hz")
        session = self.sessions.require_participant(session_id, actor)
        self._require_signaling_credential(session, actor, token=token, device_token=device_token)
        self._audit(
            "realtime_diagnostics",
            vehicle_id=session.vehicle_id,
            session_id=session_id,
            actor=actor,
            details={
                "component": component,
                "rtt_ms": rtt_ms,
                "packet_loss_percent": packet_loss_percent,
                "jitter_ms": jitter_ms,
                "video_latency_ms": video_latency_ms,
                "control_send_hz": control_send_hz,
            },
        )
        return {"event": "realtime_diagnostics", "session_id": session_id}

    def record_control_timeout(
        self,
        session_id: str,
        actor: str,
        last_valid_receive_ms: int,
        timeout_entered_ms: int,
        control_timeout_ms: int,
        token: str = "",
        device_token: str = "",
    ) -> Dict[str, str]:
        last_valid_receive_ms = _required_non_negative_int_value(last_valid_receive_ms, "last_valid_receive_ms")
        timeout_entered_ms = _required_non_negative_int_value(timeout_entered_ms, "timeout_entered_ms")
        control_timeout_ms = _required_non_negative_int_value(control_timeout_ms, "control_timeout_ms")
        if timeout_entered_ms < last_valid_receive_ms:
            raise ValueError("timeout_entered_ms must be greater than or equal to last_valid_receive_ms")
        session = self.sessions.require_participant(session_id, actor)
        self._require_signaling_credential(session, actor, token=token, device_token=device_token)
        self._audit(
            "control_timeout",
            vehicle_id=session.vehicle_id,
            session_id=session_id,
            actor=actor,
            details={
                "last_valid_receive_ms": last_valid_receive_ms,
                "timeout_entered_ms": timeout_entered_ms,
                "control_timeout_ms": control_timeout_ms,
            },
        )
        return {"event": "control_timeout", "session_id": session_id}

    def record_estop(
        self,
        session_id: str,
        actor: str,
        reason: str,
        seq: int,
        token: str = "",
        device_token: str = "",
    ) -> Dict[str, str]:
        if not isinstance(reason, str):
            raise ValueError("reason must be a string")
        if not reason:
            raise ValueError("estop reason is required")
        seq = _required_non_negative_int_value(seq, "seq")
        session = self.sessions.require_participant(session_id, actor)
        self._require_signaling_credential(session, actor, token=token, device_token=device_token)
        self._audit(
            "estop",
            vehicle_id=session.vehicle_id,
            session_id=session_id,
            actor=actor,
            details={"reason": reason, "seq": seq},
        )
        return {"event": "estop", "session_id": session_id}

    def record_turn_relay_enabled(
        self,
        session_id: str,
        actor: str,
        turn_url: str,
        relay_candidate: str,
        selected_pair: str = "",
        token: str = "",
        device_token: str = "",
    ) -> Dict[str, str]:
        if not turn_url:
            raise ValueError("turn_url is required")
        if not relay_candidate:
            raise ValueError("relay_candidate is required")
        if not selected_pair:
            raise ValueError("selected_pair is required")
        session = self.sessions.require_participant(session_id, actor)
        self._require_signaling_credential(session, actor, token=token, device_token=device_token)
        self._audit(
            "turn_relay_enabled",
            vehicle_id=session.vehicle_id,
            session_id=session_id,
            actor=actor,
            details={
                "turn_url": turn_url,
                "relay_candidate": relay_candidate,
                "selected_pair": selected_pair,
            },
        )
        return {"event": "turn_relay_enabled", "session_id": session_id}

    def record_turn_relay_usage(
        self,
        session_id: str,
        actor: str,
        bytes_sent: int,
        bytes_received: int,
        duration_ms: int,
        token: str = "",
        device_token: str = "",
    ) -> Dict[str, Any]:
        session = self.sessions.require_participant(session_id, actor)
        self._require_signaling_credential(session, actor, token=token, device_token=device_token)
        bytes_sent = _required_non_negative_int_value(bytes_sent, "bytes_sent")
        bytes_received = _required_non_negative_int_value(bytes_received, "bytes_received")
        duration_ms = _required_positive_int_value(duration_ms, "duration_ms")
        return self._record_turn_relay_usage(
            session=session,
            audit_actor=actor,
            bytes_sent=bytes_sent,
            bytes_received=bytes_received,
            duration_ms=duration_ms,
            extra_details={},
        )

    def record_trusted_turn_relay_usage(self, sample: CoturnUsageSample) -> Dict[str, Any]:
        session = self.sessions.require_participant(sample.session_id, sample.actor)
        bytes_sent = _required_non_negative_int_value(sample.bytes_sent, "bytes_sent")
        bytes_received = _required_non_negative_int_value(sample.bytes_received, "bytes_received")
        duration_ms = _required_positive_int_value(sample.duration_ms, "duration_ms")
        return self._record_turn_relay_usage(
            session=session,
            audit_actor=sample.source,
            bytes_sent=bytes_sent,
            bytes_received=bytes_received,
            duration_ms=duration_ms,
            extra_details={"source": sample.source, "source_actor": sample.actor},
        )

    def issue_ice_servers(
        self,
        session_id: str,
        actor: str,
        token: str = "",
        device_token: str = "",
    ) -> Dict[str, Any]:
        session = self.sessions.require_participant(session_id, actor)
        self._require_signaling_credential(session, actor, token=token, device_token=device_token)
        ice_servers = self._ice_server_records(session_id=session_id, actor=actor)
        turn_server_count = sum(1 for record in ice_servers if _ice_record_is_turn(record))
        self._audit(
            "ice_servers_issued",
            vehicle_id=session.vehicle_id,
            session_id=session_id,
            actor=actor,
            details={
                "ice_server_count": len(ice_servers),
                "turn_server_count": turn_server_count,
            },
        )
        return {"session_id": session_id, "ice_servers": ice_servers}

    def _ice_server_records(self, session_id: str, actor: str) -> List[Dict[str, Any]]:
        if self.ice_config is None:
            return []
        records: List[Dict[str, Any]] = []
        for stun_url in self.ice_config.stun_servers:
            records.append({"urls": [stun_url]})
        for turn in self.ice_config.turn_servers:
            username = turn.username
            credential = turn.credential
            expires_at_ms = None
            if turn.credential_mode == "turn_rest":
                if turn.credential_ttl_seconds is None:
                    raise ValueError("TURN REST credential TTL is not configured")
                expires_at_s = self._clock_ms() // 1000 + turn.credential_ttl_seconds
                expires_at_ms = expires_at_s * 1000
                username = f"{expires_at_s}:{turn.username}:{session_id}:{actor}"
                secret = turn.static_auth_secret
                if secret is None and turn.static_auth_secret_file is not None:
                    secret = Path(turn.static_auth_secret_file).read_text(encoding="utf-8").strip()
                if secret is None:
                    raise ValueError("TURN static auth secret is not configured")
                credential = base64.b64encode(
                    hmac.new(secret.encode("utf-8"), username.encode("utf-8"), hashlib.sha1).digest()
                ).decode("ascii")
            elif credential is None and turn.credential_file is not None:
                credential = Path(turn.credential_file).read_text(encoding="utf-8").strip()
            if credential is None:
                raise ValueError("TURN credential is not configured")
            record = {
                "urls": [turn.url],
                "username": username,
                "credential": credential,
                "credentialType": "password",
            }
            if expires_at_ms is not None:
                record["expires_at_ms"] = expires_at_ms
            records.append(
                record
            )
        return records

    def _record_turn_relay_usage(
        self,
        session: Any,
        audit_actor: str,
        bytes_sent: int,
        bytes_received: int,
        duration_ms: int,
        extra_details: Dict[str, Any],
    ) -> Dict[str, Any]:
        sample_bytes = bytes_sent + bytes_received
        with self._turn_usage_lock:
            summary = self.turn_usage_by_session.setdefault(
                session.session_id,
                TurnUsageSummary(session_id=session.session_id, vehicle_id=session.vehicle_id),
            )
            summary.relay_bytes_total += sample_bytes
            summary.sample_count += 1
            summary.last_bitrate_kbps = round(sample_bytes * 8 / duration_ms, 3)
            payload = summary.to_dict()
        self._audit(
            "turn_relay_usage",
            vehicle_id=session.vehicle_id,
            session_id=session.session_id,
            actor=audit_actor,
            details={
                "bytes_sent": bytes_sent,
                "bytes_received": bytes_received,
                **extra_details,
                **payload,
            },
        )
        return payload

    def record_vehicle_offline(self, vehicle_id: str, reason: str = "reported_offline") -> Dict[str, Any]:
        active_sessions = [
            session
            for session in self.sessions.sessions.values()
            if session.vehicle_id == vehicle_id and session.state == "SESSION_ACTIVE"
        ]
        self.sessions.vehicle_offline(vehicle_id)
        failed_session_ids = [session.session_id for session in active_sessions]
        self._audit(
            "vehicle_offline",
            vehicle_id=vehicle_id,
            session_id="",
            actor=vehicle_id,
            details={"reason": reason or "reported_offline", "failed_sessions": failed_session_ids},
        )
        for session in active_sessions:
            self._audit(
                "session_failed",
                vehicle_id=vehicle_id,
                session_id=session.session_id,
                actor=vehicle_id,
                details={"reason": "vehicle_offline", "offline_reason": reason or "reported_offline"},
            )
        return {"vehicle_id": vehicle_id, "state": "offline", "failed_sessions": failed_session_ids}

    def _audit(
        self,
        event: str,
        vehicle_id: str,
        session_id: str,
        actor: str,
        details: Dict[str, Any] | None = None,
    ) -> None:
        self.audit.append(
            AuditEvent(
                ts_ms=int(time.time() * 1000),
                event=event,
                vehicle_id=vehicle_id,
                session_id=session_id,
                actor=actor,
                details=details or {},
            )
        )

    def _require_device_token(self, payload: Dict[str, Any]) -> None:
        vehicle_id = _optional_json_string(payload, "vehicle_id")
        object_path = _optional_json_string(payload, "object_path") if "object_path" in payload else ""
        object_vehicle_id = _vehicle_id_from_object_path(object_path)
        if object_vehicle_id:
            if vehicle_id and vehicle_id != object_vehicle_id:
                raise PermissionError("invalid device token")
            vehicle_id = object_vehicle_id
        self.devices.validate(vehicle_id, _optional_json_string(payload, "device_token"))

    def _require_signaling_credential(
        self,
        session: Any,
        actor: str,
        token: str = "",
        device_token: str = "",
    ) -> None:
        if actor == session.driver_id:
            self.tokens.validate(actor, token)
            return
        if actor == session.vehicle_id:
            self.devices.validate(actor, device_token)
            return
        raise SessionError("sender is not current session participant")


def _path_parts(path: str) -> List[str]:
    return [part for part in path.split("/") if part]


def _parse_coturn_usage_fields(line: str) -> Dict[str, str]:
    fields: Dict[str, str] = {}
    for key, angle_value, bare_value in re.findall(r"([A-Za-z_][\w]*)=(?:<([^>]*)>|([^,\s]+))", line):
        fields[key] = angle_value if angle_value else bare_value
    return fields


def _split_coturn_username(username: str) -> tuple[str, str]:
    parts = [part.strip() for part in username.split(":")]
    if len(parts) >= 3 and parts[0].isdigit():
        return parts[-2], parts[-1]
    for separator in (":", "|"):
        if separator in username:
            session_id, actor = username.split(separator, 1)
            return session_id.strip(), actor.strip()
    return "", ""


def _ice_record_is_turn(record: Dict[str, Any]) -> bool:
    urls = record.get("urls", [])
    return any(str(url).startswith(("turn:", "turns:")) for url in urls)


def _positive_int_from_fields(fields: Dict[str, str], key: str) -> int | None:
    value = _non_negative_int_from_fields(fields, key)
    if value is None or value <= 0:
        return None
    return value


def _non_negative_int_from_fields(fields: Dict[str, str], key: str) -> int | None:
    try:
        value = int(fields[key])
    except (KeyError, TypeError, ValueError):
        return None
    if value < 0:
        return None
    return value


def _vehicle_id_from_object_path(object_path: str) -> str:
    return _upload_audit_context_from_object_path(object_path)["vehicle_id"]


def _require_segment_matches_object_path(segment_id: str, context: Dict[str, str]) -> None:
    if segment_id != context["segment_id"]:
        raise ValueError("segment_id must match upload object_path")


def _upload_audit_context_from_object_path(object_path: str) -> Dict[str, str]:
    if not object_path:
        return {"vehicle_id": "", "session_id": "", "segment_id": ""}
    parts = object_path.split("/")
    valid_video_path = (
        len(parts) == 7
        and parts[0] == "vehicles"
        and parts[2] == "sessions"
        and parts[4] == "cameras"
        and parts[6].endswith(".mp4")
    )
    valid_metadata_path = (
        len(parts) == 6
        and parts[0] == "vehicles"
        and parts[2] == "sessions"
        and parts[4] == "metadata"
        and parts[5].endswith(".json")
    )
    if not valid_video_path and not valid_metadata_path:
        raise ValueError("invalid upload object_path")
    dynamic_segments = [parts[1], parts[3]]
    if valid_video_path:
        segment_id = parts[6][:-4]
        dynamic_segments.extend([parts[5], segment_id])
    else:
        segment_id = parts[5][:-5]
        dynamic_segments.append(segment_id)
    if not all(_is_safe_upload_object_path_segment(segment) for segment in dynamic_segments):
        raise ValueError("invalid upload object_path")
    return {"vehicle_id": parts[1], "session_id": parts[3], "segment_id": segment_id}


def _is_safe_upload_object_path_segment(segment: str) -> bool:
    return bool(segment) and segment not in {".", ".."} and "/" not in segment and "\\" not in segment
