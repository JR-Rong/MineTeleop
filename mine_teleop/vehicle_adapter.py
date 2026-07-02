from __future__ import annotations

import ctypes
import math
from dataclasses import dataclass, field
from typing import Callable, List

from .chassis_control import (
    ChassisControlCommandIntent,
    ChassisControlCommandMapper,
    ChassisControlFeedbackSnapshot,
)
from .config import VehicleAdapterContract
from .control import ControlCommand
from .safety import ControlOutput


class VehicleAdapterError(RuntimeError):
    """Raised when a configured vehicle adapter cannot be constructed safely."""


@dataclass(frozen=True)
class MockTelemetry:
    speed_mps: float
    gear: str
    steering_feedback: float
    throttle_feedback: float
    brake_feedback: float
    estop: bool


@dataclass(frozen=True)
class VehicleAdapterStatus:
    adapter_type: str
    opened: bool
    healthy: bool
    can_interface: str | None = None
    library_path: str | None = None
    applied_command_count: int = 0
    safe_stop_count: int = 0
    last_error: str | None = None


@dataclass
class MockVehicleAdapter:
    applied_commands: List[ControlCommand] = field(default_factory=list)
    safe_outputs: List[ControlOutput] = field(default_factory=list)
    _latest_output: ControlOutput | None = field(default=None, init=False, repr=False)
    _opened: bool = field(default=False, init=False, repr=False)

    def open(self) -> None:
        self._opened = True

    def close(self) -> None:
        self._opened = False

    def apply_control(self, command: ControlCommand) -> None:
        self.applied_commands.append(command)
        self._latest_output = ControlOutput(
            gear=command.gear,
            steering=command.steering,
            throttle=command.throttle,
            brake=command.brake,
            estop=command.estop,
        )

    def apply_safe_stop(self, output: ControlOutput) -> None:
        self.safe_outputs.append(output)
        self._latest_output = output

    def read_telemetry(self) -> MockTelemetry:
        if self._latest_output is None:
            return MockTelemetry(0.0, "N", 0.0, 0.0, 0.0, False)
        return MockTelemetry(
            speed_mps=self._latest_output.throttle * 2.0,
            gear=self._latest_output.gear,
            steering_feedback=self._latest_output.steering,
            throttle_feedback=self._latest_output.throttle,
            brake_feedback=self._latest_output.brake,
            estop=self._latest_output.estop,
        )

    def get_status(self) -> VehicleAdapterStatus:
        return VehicleAdapterStatus(
            adapter_type="mock",
            opened=self._opened,
            healthy=True,
            applied_command_count=len(self.applied_commands),
            safe_stop_count=len(self.safe_outputs),
        )


class _BridgeTelemetry(ctypes.Structure):
    _fields_ = [
        ("speed_mps", ctypes.c_double),
        ("gear", ctypes.c_int),
        ("steering_feedback", ctypes.c_double),
        ("throttle_feedback", ctypes.c_double),
        ("brake_feedback", ctypes.c_double),
        ("estop", ctypes.c_int),
    ]


class _BridgeFeedback(ctypes.Structure):
    _fields_ = [
        ("shake_hand_status", ctypes.c_int),
        ("epb_status", ctypes.c_int * 4),
        ("gear_status", ctypes.c_int),
        ("mcu_mode", ctypes.c_int * 8),
        ("eps_mode", ctypes.c_int * 4),
        ("eps_angle", ctypes.c_double * 4),
        ("ehb_mode", ctypes.c_int * 8),
        ("vehicle_speed", ctypes.c_double),
        ("vehicle_speed_valid", ctypes.c_int),
    ]


class DynamicLibraryVehicleAdapter:
    def __init__(
        self,
        library_path: str,
        can_interface: str,
        mapper: ChassisControlCommandMapper | None = None,
        library_loader: Callable[[str], object] = ctypes.CDLL,
        adapter_type: str = "dynamic_library",
    ) -> None:
        if not library_path:
            raise ValueError("library_path is required")
        self.library_path = library_path
        self.can_interface = can_interface
        self.adapter_type = adapter_type
        self.mapper = mapper or ChassisControlCommandMapper(can_interface=can_interface)
        self._library_loader = library_loader
        self._library: object | None = None
        self._latest_output: ControlOutput | None = None
        self._opened = False
        self._last_error: str | None = None
        self._applied_command_count = 0
        self._safe_stop_count = 0

    def open(self) -> None:
        library = self._load_library()
        open_fn = _bridge_function(
            library,
            "mine_teleop_chassis_open",
            [ctypes.c_char_p],
            ctypes.c_int,
        )
        result = open_fn(self.can_interface.encode("utf-8"))
        self._raise_if_error(result, "mine_teleop_chassis_open")
        self._opened = True
        self._last_error = None

    def close(self) -> None:
        if self._library is None:
            return
        close_fn = _bridge_function(
            self._library,
            "mine_teleop_chassis_close",
            [],
            ctypes.c_int,
        )
        result = close_fn()
        self._raise_if_error(result, "mine_teleop_chassis_close")
        self._opened = False
        self._last_error = None

    def apply_control(self, command: ControlCommand) -> None:
        self._apply_intent(self.mapper.map_control(command))
        self._applied_command_count += 1
        self._latest_output = ControlOutput(
            gear=command.gear,
            steering=command.steering,
            throttle=command.throttle,
            brake=command.brake,
            estop=command.estop,
        )

    def apply_safe_stop(self, output: ControlOutput) -> None:
        self._apply_intent(self.mapper.map_safe_stop(output))
        self._safe_stop_count += 1
        self._latest_output = output

    def update_feedback(self, snapshot: ChassisControlFeedbackSnapshot) -> None:
        library = self._load_library()
        bridge_feedback = _BridgeFeedback(
            shake_hand_status=int(snapshot.shake_hand_status),
            epb_status=(ctypes.c_int * 4)(*snapshot.epb_status),
            gear_status=int(snapshot.gear_status),
            mcu_mode=(ctypes.c_int * 8)(*snapshot.mcu_mode),
            eps_mode=(ctypes.c_int * 4)(*snapshot.eps_mode),
            eps_angle=(ctypes.c_double * 4)(*_bridge_eps_angle(snapshot.eps_angle)),
            ehb_mode=(ctypes.c_int * 8)(*snapshot.ehb_mode),
            vehicle_speed=float(snapshot.vehicle_speed),
            vehicle_speed_valid=1 if snapshot.vehicle_speed_valid else 0,
        )
        update_fn = _bridge_function(
            library,
            "mine_teleop_chassis_update_feedback",
            [ctypes.POINTER(_BridgeFeedback)],
            ctypes.c_int,
        )
        result = update_fn(ctypes.byref(bridge_feedback))
        self._raise_if_error(result, "mine_teleop_chassis_update_feedback")

    def poll_feedback(self) -> ChassisControlFeedbackSnapshot | None:
        library = self._load_library()
        bridge_feedback = _BridgeFeedback()
        poll_fn = _bridge_function(
            library,
            "mine_teleop_chassis_poll_feedback",
            [ctypes.POINTER(_BridgeFeedback)],
            ctypes.c_int,
        )
        result = poll_fn(ctypes.byref(bridge_feedback))
        if result == 1:
            return None
        self._raise_if_error(result, "mine_teleop_chassis_poll_feedback")
        return _feedback_snapshot_from_bridge(bridge_feedback)

    def read_telemetry(self) -> MockTelemetry:
        library = self._load_library()
        telemetry = _BridgeTelemetry()
        read_fn = _bridge_function(
            library,
            "mine_teleop_chassis_read_telemetry",
            [ctypes.POINTER(_BridgeTelemetry)],
            ctypes.c_int,
        )
        result = read_fn(ctypes.byref(telemetry))
        self._raise_if_error(result, "mine_teleop_chassis_read_telemetry")
        return MockTelemetry(
            speed_mps=float(telemetry.speed_mps),
            gear=_gear_from_chassis_value(int(telemetry.gear)),
            steering_feedback=float(telemetry.steering_feedback),
            throttle_feedback=float(telemetry.throttle_feedback),
            brake_feedback=float(telemetry.brake_feedback),
            estop=bool(telemetry.estop),
        )

    def get_status(self) -> VehicleAdapterStatus:
        return VehicleAdapterStatus(
            adapter_type=self.adapter_type,
            opened=self._opened,
            healthy=self._last_error is None,
            can_interface=self.can_interface,
            library_path=self.library_path,
            applied_command_count=self._applied_command_count,
            safe_stop_count=self._safe_stop_count,
            last_error=self._last_error,
        )

    def _apply_intent(self, intent: ChassisControlCommandIntent) -> None:
        library = self._load_library()
        if intent.estop:
            stop_fn = _bridge_function(
                library,
                "mine_teleop_chassis_emergency_stop",
                [],
                ctypes.c_int,
            )
            result = stop_fn()
            self._raise_if_error(result, "mine_teleop_chassis_emergency_stop")
            return
        if intent.vehicle_state is None:
            raise VehicleAdapterError("chassis control intent missing vehicle_state")
        state = intent.vehicle_state
        steering_array_type = ctypes.c_double * len(state.target_steering_angle)
        steering_array = steering_array_type(*state.target_steering_angle)
        apply_fn = _bridge_function(
            library,
            "mine_teleop_chassis_apply_state",
            [
                ctypes.c_int,
                ctypes.c_double,
                ctypes.c_double,
                ctypes.POINTER(ctypes.c_double),
                ctypes.c_int,
            ],
            ctypes.c_int,
        )
        result = apply_fn(
            state.target_gear,
            state.target_velocity[0],
            state.target_acceleration[0],
            steering_array,
            len(state.target_steering_angle),
        )
        self._raise_if_error(result, "mine_teleop_chassis_apply_state")

    def _load_library(self) -> object:
        if self._library is None:
            try:
                self._library = self._library_loader(self.library_path)
            except OSError as exc:
                self._last_error = f"failed to load dynamic library {self.library_path}: {exc}"
                raise VehicleAdapterError(self._last_error) from exc
        return self._library

    def _raise_if_error(self, result: int, function_name: str) -> None:
        if result != 0:
            self._last_error = f"{function_name} failed with code {result}"
            raise VehicleAdapterError(self._last_error)


def create_vehicle_adapter(
    adapter_type: str,
    contract: VehicleAdapterContract | None = None,
) -> MockVehicleAdapter:
    if adapter_type == "mock":
        return MockVehicleAdapter()
    if adapter_type in {"can", "dynamic_library"} and contract is not None and contract.integration is not None:
        chassis = contract.integration.chassis_control
        if chassis.abi == "cplusplus":
            raise VehicleAdapterError(
                f"vehicle_adapter.type {adapter_type} requires a C++ bridge/C shim before runtime loading; "
                "configured ChassisControl ABI is cplusplus"
            )
        if chassis.abi == "c_shim":
            if chassis.bridge_library_path is None:
                raise VehicleAdapterError(
                    "vehicle_adapter.integration.chassis_control.bridge_library_path is required for c_shim ABI"
                )
            return DynamicLibraryVehicleAdapter(
                library_path=chassis.bridge_library_path,
                can_interface=chassis.can_interface,
                adapter_type=adapter_type,
            )
    if adapter_type in {"can", "dynamic_library"}:
        raise VehicleAdapterError(
            f"vehicle_adapter.type {adapter_type} is not implemented; "
            "define the real vehicle interface contract before enabling it"
        )
    raise VehicleAdapterError(f"unsupported vehicle_adapter.type {adapter_type}")


def _bridge_function(library: object, name: str, argtypes: list[object], restype: object) -> object:
    try:
        function = getattr(library, name)
    except AttributeError as exc:
        raise VehicleAdapterError(f"dynamic library is missing required symbol {name}") from exc
    function.argtypes = argtypes
    function.restype = restype
    return function


def _bridge_eps_angle(values: tuple[float | None, float | None, float | None, float | None]) -> tuple[float, ...]:
    return tuple(math.nan if value is None else float(value) for value in values)


def _feedback_snapshot_from_bridge(feedback: _BridgeFeedback) -> ChassisControlFeedbackSnapshot:
    return ChassisControlFeedbackSnapshot(
        shake_hand_status=int(feedback.shake_hand_status),
        epb_status=tuple(int(value) for value in feedback.epb_status),
        gear_status=int(feedback.gear_status),
        mcu_mode=tuple(int(value) for value in feedback.mcu_mode),
        eps_mode=tuple(int(value) for value in feedback.eps_mode),
        eps_angle=tuple(float(value) if not math.isnan(float(value)) else None for value in feedback.eps_angle),
        ehb_mode=tuple(int(value) for value in feedback.ehb_mode),
        vehicle_speed=float(feedback.vehicle_speed),
        vehicle_speed_valid=bool(feedback.vehicle_speed_valid),
    )


def _gear_from_chassis_value(value: int) -> str:
    return {
        1: "N",
        2: "R",
        3: "D",
        4: "P",
    }.get(value, "N")
