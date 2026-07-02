from __future__ import annotations

import secrets
import threading
from dataclasses import dataclass
from typing import Dict


class SessionError(RuntimeError):
    pass


@dataclass
class Session:
    session_id: str
    vehicle_id: str
    driver_id: str
    state: str
    control_token: str


class SessionManager:
    def __init__(self) -> None:
        self.online_vehicles: set[str] = set()
        self.sessions: Dict[str, Session] = {}
        self._counter = 0
        self._lock = threading.Lock()

    def vehicle_online(self, vehicle_id: str) -> None:
        _require_non_empty_string(vehicle_id, "vehicle_id")
        with self._lock:
            self.online_vehicles.add(vehicle_id)

    def vehicle_offline(self, vehicle_id: str) -> None:
        _require_non_empty_string(vehicle_id, "vehicle_id")
        with self._lock:
            self.online_vehicles.discard(vehicle_id)
            for session in self.sessions.values():
                if session.vehicle_id == vehicle_id and session.state == "SESSION_ACTIVE":
                    session.state = "FAILED"

    def request_session(self, vehicle_id: str, driver_id: str) -> Session:
        _require_non_empty_string(vehicle_id, "vehicle_id")
        _require_non_empty_string(driver_id, "driver_id")
        with self._lock:
            if vehicle_id not in self.online_vehicles:
                raise SessionError("vehicle is not online")
            for session in self.sessions.values():
                if session.vehicle_id == vehicle_id and session.state == "SESSION_ACTIVE":
                    raise SessionError("control authority already granted")
            self._counter += 1
            # The control_token authorizes control commands and must NOT be derivable
            # from the session_id (which is routinely logged/returned to clients).
            session = Session(
                session_id=f"session-{self._counter:06d}",
                vehicle_id=vehicle_id,
                driver_id=driver_id,
                state="SESSION_ACTIVE",
                control_token=f"control-token-{secrets.token_urlsafe(24)}",
            )
            self.sessions[session.session_id] = session
            return session

    def end_session(self, session_id: str) -> None:
        _require_non_empty_string(session_id, "session_id")
        with self._lock:
            if session_id not in self.sessions:
                raise SessionError("unknown session")
            self.sessions[session_id].state = "ENDED"

    def revoke_control_authority(self, session_id: str) -> Session:
        _require_non_empty_string(session_id, "session_id")
        with self._lock:
            if session_id not in self.sessions:
                raise SessionError("unknown session")
            session = self.sessions[session_id]
            if session.state != "SESSION_ACTIVE":
                raise SessionError("session is not active")
            session.state = "ENDED"
            return session

    def require_participant(self, session_id: str, sender: str) -> Session:
        _require_non_empty_string(session_id, "session_id")
        _require_non_empty_string(sender, "sender")
        with self._lock:
            if session_id not in self.sessions:
                raise SessionError("unknown session")
            session = self.sessions[session_id]
            if session.state != "SESSION_ACTIVE":
                raise SessionError("session is not active")
            if sender not in {session.driver_id, session.vehicle_id}:
                raise SessionError("sender is not current session participant")
            return session


def _require_non_empty_string(value: object, field_name: str) -> str:
    if not isinstance(value, str) or value == "":
        raise ValueError(f"{field_name} must be a non-empty string")
    return value
