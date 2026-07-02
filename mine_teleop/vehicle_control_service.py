from __future__ import annotations

from dataclasses import asdict
from typing import Any, Dict, List

from .config import VehicleConfig
from .control import ControlCommand, ControlReceiver, LatestControlCommandMailbox, ReceiveResult
from .observability import AuditEvent, AuditLog, TelemetryPublisher
from .safety import SafetyState, SafetyStateMachine
from .vehicle_adapter import create_vehicle_adapter


class VehicleControlService:
    def __init__(
        self,
        vehicle_id: str,
        session_id: str,
        receiver: ControlReceiver,
        safety: SafetyStateMachine,
        adapter: Any,
        telemetry_publisher: TelemetryPublisher,
        telemetry_interval_ms: int,
        audit_log: AuditLog | None = None,
    ) -> None:
        self.vehicle_id = vehicle_id
        self.session_id = session_id
        self.receiver = receiver
        self.safety = safety
        self.adapter = adapter
        self.telemetry_publisher = telemetry_publisher
        self.telemetry_interval_ms = telemetry_interval_ms
        self.telemetry_history: List[Dict[str, Any]] = []
        self.receive_history: List[ReceiveResult] = []
        self.signaling_connected = True
        self.audit_log = audit_log
        self._timeout_audited = False
        self._estop_audited = False
        self._last_telemetry_ms: int | None = None

    @property
    def applied_command_count(self) -> int:
        return int(self.adapter.get_status().applied_command_count)

    @classmethod
    def from_config(
        cls,
        config: VehicleConfig,
        session_id: str,
        control_token: str = "",
        adapter: Any | None = None,
        telemetry_interval_ms: int = 100,
        audit_log: AuditLog | None = None,
    ) -> "VehicleControlService":
        receiver = ControlReceiver(
            vehicle_id=config.vehicle_id,
            session_id=session_id,
            max_command_gap_ms=config.control.max_command_gap_ms,
            control_token=control_token,
        )
        safety = SafetyStateMachine(
            degraded_timeout_ms=config.control.degraded_timeout_ms,
            control_timeout_ms=config.control.control_timeout_ms,
            deceleration_profile=config.control.deceleration_profile,
        )
        return cls(
            vehicle_id=config.vehicle_id,
            session_id=session_id,
            receiver=receiver,
            safety=safety,
            adapter=adapter or create_vehicle_adapter(config.vehicle_adapter_type, config.vehicle_adapter_contract),
            telemetry_publisher=TelemetryPublisher(config.vehicle_id, session_id, source=config.vehicle_adapter_type),
            telemetry_interval_ms=telemetry_interval_ms,
            audit_log=audit_log,
        )

    def start(self, now_ms: int) -> None:
        self.adapter.open()
        self.safety.mark_ready(now_ms)

    def receive_command(self, command: ControlCommand, now_ms: int) -> ReceiveResult:
        result = self.receiver.accept(command, receive_time_ms=now_ms)
        self.receive_history.append(result)
        if not result.accepted or result.command is None:
            return result

        self._audit_control_warnings(result, now_ms)
        self.safety.on_valid_command(result.command, now_ms)
        if result.command.estop and self.safety.state == SafetyState.ESTOP:
            self._audit_estop_latched(result.command, now_ms)
        if self.safety.state == SafetyState.CONTROL_ACTIVE:
            self.adapter.apply_control(result.command)
        else:
            self.adapter.apply_safe_stop(self.safety.current_output(now_ms))
        return result

    def drain_command_mailbox(self, mailbox: LatestControlCommandMailbox, now_ms: int) -> ReceiveResult | None:
        command = mailbox.pop_latest()
        if command is None:
            return None
        return self.receive_command(command, now_ms)

    def set_signaling_connected(self, connected: bool, now_ms: int) -> None:
        self.signaling_connected = connected

    def reset_estop(self, local_confirmed: bool, authorized_by: str, now_ms: int) -> bool:
        latched_at_ms = self.safety.estop_latched_at_ms
        reset = self.safety.reset_estop(
            local_confirmed=local_confirmed,
            authorized_by=authorized_by,
            now_ms=now_ms,
        )
        if not reset:
            return False
        self._estop_audited = False
        self.adapter.apply_safe_stop(self.safety.current_output(now_ms))
        self._audit_estop_reset(
            authorized_by=authorized_by,
            local_confirmed=local_confirmed,
            latched_at_ms=latched_at_ms,
            now_ms=now_ms,
        )
        return True

    def tick(self, now_ms: int) -> None:
        self.safety.tick(now_ms)
        if self.safety.state == SafetyState.TIMEOUT_BRAKE:
            self._audit_control_timeout(now_ms)
        if self.safety.state in {SafetyState.DEGRADED, SafetyState.TIMEOUT_BRAKE, SafetyState.ESTOP}:
            self.adapter.apply_safe_stop(self.safety.current_output(now_ms))
        if self._should_emit_telemetry(now_ms):
            self._poll_adapter_feedback()
            self.telemetry_history.append(self._build_telemetry(now_ms))
            self._last_telemetry_ms = now_ms

    def _should_emit_telemetry(self, now_ms: int) -> bool:
        return self._last_telemetry_ms is None or now_ms - self._last_telemetry_ms >= self.telemetry_interval_ms

    def _poll_adapter_feedback(self) -> None:
        poll_feedback = getattr(self.adapter, "poll_feedback", None)
        if not callable(poll_feedback):
            return
        try:
            snapshot = poll_feedback()
        except Exception:
            return
        update_feedback = getattr(self.adapter, "update_feedback", None)
        if snapshot is not None and callable(update_feedback):
            try:
                update_feedback(snapshot)
            except Exception:
                return

    def _build_telemetry(self, now_ms: int) -> Dict[str, Any]:
        payload = self.telemetry_publisher.build(
            telemetry=self.adapter.read_telemetry(),
            safety_state=self.safety.state,
            control_rtt_ms=0,
            video_status={},
            system={
                "cpu_percent": 0.0,
                "memory_percent": 0.0,
                "disk_free_gb": 0.0,
            },
            now_ms=now_ms,
        ) | {
            "link": {
                "control_rtt_ms": 0,
                "signaling_connected": self.signaling_connected,
            }
        }
        payload["vehicle_adapter"] = _compact_status(self.adapter.get_status())
        return payload

    def _audit_control_timeout(self, now_ms: int) -> None:
        if self.audit_log is None or self._timeout_audited:
            return
        self._timeout_audited = True
        self.audit_log.append(
            AuditEvent(
                ts_ms=now_ms,
                event="control_timeout",
                vehicle_id=self.vehicle_id,
                session_id=self.session_id,
                actor="vehicle-control-agent",
                details={
                    "state": self.safety.state.value,
                    "last_valid_receive_ms": self.safety.last_valid_receive_ms,
                    "control_timeout_ms": self.safety.control_timeout_ms,
                },
            )
        )

    def _audit_estop_latched(self, command: ControlCommand, now_ms: int) -> None:
        if self.audit_log is None or self._estop_audited:
            return
        self._estop_audited = True
        self.audit_log.append(
            AuditEvent(
                ts_ms=now_ms,
                event="estop_latched",
                vehicle_id=self.vehicle_id,
                session_id=self.session_id,
                actor="vehicle-control-agent",
                details={"seq": command.seq, "command_ts_ms": command.ts_ms},
            )
        )

    def _audit_control_warnings(self, result: ReceiveResult, now_ms: int) -> None:
        if self.audit_log is None or result.command is None:
            return
        for warning in result.warnings:
            event_name = "control_timestamp_warning" if warning == "driver_timestamp_skew" else "control_command_warning"
            self.audit_log.append(
                AuditEvent(
                    ts_ms=now_ms,
                    event=event_name,
                    vehicle_id=self.vehicle_id,
                    session_id=self.session_id,
                    actor="vehicle-control-agent",
                    details={
                        "warning": warning,
                        "seq": result.command.seq,
                        "command_ts_ms": result.command.ts_ms,
                        "receive_time_ms": now_ms,
                    },
                )
            )

    def _audit_estop_reset(
        self,
        authorized_by: str,
        local_confirmed: bool,
        latched_at_ms: int | None,
        now_ms: int,
    ) -> None:
        if self.audit_log is None:
            return
        self.audit_log.append(
            AuditEvent(
                ts_ms=now_ms,
                event="estop_reset",
                vehicle_id=self.vehicle_id,
                session_id=self.session_id,
                actor=authorized_by,
                details={
                    "local_confirmed": local_confirmed,
                    "latched_at_ms": latched_at_ms,
                    "reset_at_ms": now_ms,
                },
            )
        )


def _compact_status(status: Any) -> Dict[str, Any]:
    return {key: value for key, value in asdict(status).items() if value is not None}
