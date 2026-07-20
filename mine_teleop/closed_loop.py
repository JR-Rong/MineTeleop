from __future__ import annotations

from dataclasses import dataclass
from typing import List

from .control import ControlReceiver
from .driver_console import ControlCommandGenerator, InputState
from .safety import SafetyState, SafetyStateMachine
from .vehicle_adapter import MockVehicleAdapter


@dataclass(frozen=True)
class MockClosedLoopResult:
    commands_applied_before_disconnect: int
    safety_states: List[SafetyState]
    safety_states_after_estop: List[SafetyState]
    final_state: SafetyState


def run_mock_closed_loop(
    duration_ms: int,
    disconnect_at_ms: int | None = None,
    estop_at_ms: int | None = None,
    step_ms: int = 50,
) -> MockClosedLoopResult:
    generator = ControlCommandGenerator("vehicle-001", "session-001", rate_hz=20)
    receiver = ControlReceiver("vehicle-001", "session-001", max_command_gap_ms=200)
    safety = SafetyStateMachine(
        degraded_timeout_ms=300,
        control_timeout_ms=800,
        deceleration_profile=[(0, 0.3), (500, 0.6), (1500, "vehicle_defined_max_safe")],
    )
    adapter = MockVehicleAdapter()
    safety.mark_ready(now_ms=0)
    accepted_before_disconnect = 0
    states: List[SafetyState] = []
    states_after_estop: List[SafetyState] = []
    estop_seen = False

    for now_ms in range(0, duration_ms + 1, step_ms):
        connected = disconnect_at_ms is None or now_ms < disconnect_at_ms
        if connected:
            input_state = InputState(
                throttle_pressed=True,
                gear="D",
                estop_pressed=estop_at_ms is not None and now_ms == estop_at_ms,
            )
            command = generator.next_command(input_state, now_ms=now_ms)
            if command is not None:
                result = receiver.accept(command, receive_time_ms=now_ms)
                if result.accepted and result.command is not None:
                    accepted_before_disconnect += 1
                    safety.on_valid_command(result.command, now_ms=now_ms)
                    adapter.apply_control(result.command)

        safety.tick(now_ms)
        if safety.state in {SafetyState.DEGRADED, SafetyState.TIMEOUT_BRAKE, SafetyState.ESTOP}:
            adapter.apply_safe_stop(safety.current_output(now_ms))
        states.append(safety.state)
        if safety.state == SafetyState.ESTOP:
            estop_seen = True
        if estop_seen:
            states_after_estop.append(safety.state)

    return MockClosedLoopResult(
        commands_applied_before_disconnect=accepted_before_disconnect,
        safety_states=states,
        safety_states_after_estop=states_after_estop,
        final_state=states[-1],
    )
