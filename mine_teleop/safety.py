from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import List, Tuple

from .control import ControlCommand


class SafetyState(str, Enum):
    INIT = "INIT"
    STANDBY = "STANDBY"
    CONTROL_ACTIVE = "CONTROL_ACTIVE"
    DEGRADED = "DEGRADED"
    TIMEOUT_BRAKE = "TIMEOUT_BRAKE"
    ESTOP = "ESTOP"
    FAULT = "FAULT"


@dataclass(frozen=True)
class ControlOutput:
    gear: str
    steering: float
    throttle: float
    brake: float
    estop: bool = False


class SafetyStateMachine:
    def __init__(
        self,
        degraded_timeout_ms: int,
        control_timeout_ms: int,
        deceleration_profile: List[Tuple[int, float | str]],
    ) -> None:
        self.degraded_timeout_ms = degraded_timeout_ms
        self.control_timeout_ms = control_timeout_ms
        # Sort by elapsed threshold so _brake_for_timeout (which keeps the last
        # matching stage) is correct even if a caller passes an unordered profile.
        self.deceleration_profile = sorted(deceleration_profile, key=lambda stage: stage[0])
        self.state = SafetyState.INIT
        self.last_valid_command: ControlCommand | None = None
        self.last_valid_receive_ms: int | None = None
        self.timeout_entered_ms: int | None = None
        self.estop_latched_at_ms: int | None = None
        self.estop_reset_by: str | None = None

    def mark_ready(self, now_ms: int) -> None:
        if self.state == SafetyState.INIT:
            self.state = SafetyState.STANDBY

    def on_valid_command(self, command: ControlCommand, now_ms: int) -> None:
        if command.estop:
            self.last_valid_command = command
            self.last_valid_receive_ms = now_ms
            self.state = SafetyState.ESTOP
            self.estop_latched_at_ms = now_ms
            return
        if self.state in {SafetyState.ESTOP, SafetyState.FAULT}:
            return
        self.last_valid_command = command
        self.last_valid_receive_ms = now_ms
        self.timeout_entered_ms = None
        self.state = SafetyState.CONTROL_ACTIVE

    def tick(self, now_ms: int) -> None:
        if self.state in {SafetyState.INIT, SafetyState.STANDBY, SafetyState.ESTOP, SafetyState.FAULT}:
            return
        if self.last_valid_receive_ms is None:
            return
        elapsed = now_ms - self.last_valid_receive_ms
        if elapsed >= self.control_timeout_ms:
            if self.state != SafetyState.TIMEOUT_BRAKE:
                self.timeout_entered_ms = now_ms
            self.state = SafetyState.TIMEOUT_BRAKE
        elif elapsed >= self.degraded_timeout_ms:
            self.state = SafetyState.DEGRADED

    def current_output(self, now_ms: int) -> ControlOutput:
        base = self.last_valid_command
        gear = base.gear if base else "N"
        steering = base.steering if base else 0.0
        if self.state == SafetyState.CONTROL_ACTIVE and base:
            return ControlOutput(gear=gear, steering=steering, throttle=base.throttle, brake=base.brake)
        if self.state == SafetyState.DEGRADED:
            return ControlOutput(gear=gear, steering=steering, throttle=0.0, brake=base.brake if base else 0.0)
        if self.state == SafetyState.TIMEOUT_BRAKE:
            return ControlOutput(gear=gear, steering=0.0, throttle=0.0, brake=self._brake_for_timeout(now_ms))
        if self.state == SafetyState.ESTOP:
            return ControlOutput(gear=gear, steering=0.0, throttle=0.0, brake=1.0, estop=True)
        if self.state == SafetyState.FAULT:
            # A fault must fail safe with full braking, never coast.
            return ControlOutput(gear=gear, steering=0.0, throttle=0.0, brake=1.0)
        return ControlOutput(gear="N", steering=0.0, throttle=0.0, brake=0.0)

    def reset_estop(self, local_confirmed: bool, authorized_by: str, now_ms: int) -> bool:
        if self.state != SafetyState.ESTOP:
            return False
        if not local_confirmed or not authorized_by:
            return False
        self.estop_reset_by = authorized_by
        self.last_valid_command = None
        self.last_valid_receive_ms = None
        self.timeout_entered_ms = None
        self.state = SafetyState.STANDBY
        return True

    def _brake_for_timeout(self, now_ms: int) -> float:
        entered = self.timeout_entered_ms if self.timeout_entered_ms is not None else now_ms
        elapsed = now_ms - entered
        chosen: float | str = 0.0
        for after_ms, brake in self.deceleration_profile:
            if elapsed >= after_ms:
                chosen = brake
        if chosen == "vehicle_defined_max_safe":
            return 1.0
        return float(chosen)
