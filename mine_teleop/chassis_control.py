from __future__ import annotations

import math
from dataclasses import dataclass
from typing import Any, Callable, Iterable, Tuple

from .config import VehicleAdapterContract
from .control import ControlCommand
from .safety import ControlOutput


GEAR_TO_CHASSIS_VALUE = {
    "N": 1,
    "R": 2,
    "D": 3,
    "P": 4,
}


@dataclass(frozen=True)
class ChassisControlVehicleStateIntent:
    cur_velocity: float
    target_velocity: Tuple[float, float]
    target_acceleration: Tuple[float, float]
    target_gear: int
    target_steering_angle: Tuple[float, ...]


@dataclass(frozen=True)
class ChassisControlCommandIntent:
    actions: Tuple[str, ...]
    can_interface: str
    vehicle_state: ChassisControlVehicleStateIntent | None = None
    estop: bool = False


@dataclass(frozen=True)
class ChassisControlFeedbackSnapshot:
    shake_hand_status: int = 0
    epb_status: Tuple[int, int, int, int] = (0, 0, 0, 0)
    gear_status: int = 0
    mcu_mode: Tuple[int, int, int, int, int, int, int, int] = (0, 0, 0, 0, 0, 0, 0, 0)
    eps_mode: Tuple[int, int, int, int] = (0, 0, 0, 0)
    eps_angle: Tuple[float | None, float | None, float | None, float | None] = (None, None, None, None)
    ehb_mode: Tuple[int, int, int, int, int, int, int, int] = (0, 0, 0, 0, 0, 0, 0, 0)
    vehicle_speed: float = 0.0
    vehicle_speed_valid: bool = False


class ChassisControlFeedbackPump:
    def __init__(self, reader: Callable[[], Any], adapter: Any) -> None:
        self.reader = reader
        self.adapter = adapter
        self.last_snapshot: ChassisControlFeedbackSnapshot | None = None

    def poll_once(self) -> bool:
        decoded_can_data = self.reader()
        if decoded_can_data is None:
            return False
        snapshot = feedback_snapshot_from_decoded_can_data(decoded_can_data)
        self.adapter.update_feedback(snapshot)
        self.last_snapshot = snapshot
        return True


@dataclass(frozen=True)
class ChassisControlIntegrationPlan:
    cmake_target: str
    library_output_name: str
    can_interface: str
    abi: str
    requires_cpp_bridge: bool
    required_calls: Tuple[str, ...]
    feedback_sources: Tuple[str, ...]


class ChassisControlCommandMapper:
    def __init__(
        self,
        can_interface: str,
        wheel_count: int = 8,
        max_speed_mps: float = 2.0,
        max_accel_mps2: float = 1.0,
        max_decel_mps2: float = 3.0,
        max_steering_rad: float = 0.5,
    ) -> None:
        if not isinstance(can_interface, str) or can_interface == "":
            raise ValueError("can_interface must be a non-empty string")
        if isinstance(wheel_count, bool) or not isinstance(wheel_count, int) or wheel_count <= 0:
            raise ValueError("wheel_count must be a positive integer")
        for label, value in {
            "max_speed_mps": max_speed_mps,
            "max_accel_mps2": max_accel_mps2,
            "max_decel_mps2": max_decel_mps2,
            "max_steering_rad": max_steering_rad,
        }.items():
            if _invalid_positive_finite_number(value):
                raise ValueError(f"{label} must be a positive finite number")
        self.can_interface = can_interface
        self.wheel_count = wheel_count
        self.max_speed_mps = max_speed_mps
        self.max_accel_mps2 = max_accel_mps2
        self.max_decel_mps2 = max_decel_mps2
        self.max_steering_rad = max_steering_rad

    def map_control(self, command: ControlCommand) -> ChassisControlCommandIntent:
        command.validate()
        return self._map_motion(
            gear=command.gear,
            steering=command.steering,
            throttle=command.throttle,
            brake=command.brake,
            estop=command.estop,
        )

    def map_safe_stop(self, output: ControlOutput) -> ChassisControlCommandIntent:
        return self._map_motion(
            gear=output.gear,
            steering=output.steering,
            throttle=output.throttle,
            brake=output.brake,
            estop=output.estop,
        )

    def _map_motion(
        self,
        gear: str,
        steering: float,
        throttle: float,
        brake: float,
        estop: bool,
    ) -> ChassisControlCommandIntent:
        if estop:
            return ChassisControlCommandIntent(
                actions=("EmergencyStop",),
                can_interface=self.can_interface,
                estop=True,
            )
        if gear not in GEAR_TO_CHASSIS_VALUE:
            raise ValueError(f"unsupported gear {gear}")
        safe_throttle = _clamp(throttle, 0.0, 1.0)
        safe_brake = _clamp(brake, 0.0, 1.0)
        if gear in {"P", "N"}:
            target_speed = 0.0
            target_accel = -safe_brake * self.max_decel_mps2
        else:
            # Braking must also lower the commanded target velocity, otherwise the
            # chassis receives a "drive fast" target together with a hard decel.
            target_speed = safe_throttle * (1.0 - safe_brake) * self.max_speed_mps
            target_accel = safe_throttle * self.max_accel_mps2 - safe_brake * self.max_decel_mps2
        steering_angle = _clamp(steering, -1.0, 1.0) * self.max_steering_rad
        vehicle_state = ChassisControlVehicleStateIntent(
            cur_velocity=0.0,
            target_velocity=(target_speed, 0.0),
            target_acceleration=(target_accel, 0.0),
            target_gear=GEAR_TO_CHASSIS_VALUE[gear],
            target_steering_angle=tuple(steering_angle for _ in range(self.wheel_count)),
        )
        return ChassisControlCommandIntent(
            actions=("UpdateVehicleState", "RunArmingStateMachine", "SendCanMessage"),
            can_interface=self.can_interface,
            vehicle_state=vehicle_state,
        )


def build_chassis_control_integration_plan(contract: VehicleAdapterContract) -> ChassisControlIntegrationPlan:
    if contract.integration is None:
        raise ValueError("vehicle_adapter.integration is required")
    chassis = contract.integration.chassis_control
    feedback_sources = []
    if contract.integration.minepilot is not None:
        feedback_sources.extend(
            [
                contract.integration.minepilot.can_common_header_path,
                contract.integration.minepilot.can_message_header_path,
                contract.integration.minepilot.can_db_header_path,
                contract.integration.minepilot.can_receiver_header_path,
                contract.integration.minepilot.can_sender_header_path,
                contract.integration.minepilot.can_db_source_path,
                contract.integration.minepilot.can_receiver_source_path,
                contract.integration.minepilot.can_sender_source_path,
            ]
        )
    return ChassisControlIntegrationPlan(
        cmake_target=chassis.cmake_target,
        library_output_name=chassis.library_output_name,
        can_interface=chassis.can_interface,
        abi=chassis.abi,
        requires_cpp_bridge=chassis.requires_cpp_bridge,
        required_calls=(
            "Initialize",
            "UpdateVehicleState",
            "RunArmingStateMachine",
            "SendCanMessage",
            "EmergencyStopWheels",
        ),
        feedback_sources=tuple(feedback_sources),
    )


def feedback_snapshot_from_decoded_can_data(decoded_can_data: Any) -> ChassisControlFeedbackSnapshot:
    return ChassisControlFeedbackSnapshot(
        shake_hand_status=_int_field(
            decoded_can_data,
            "wvcu_veh_shake_hand_sts_frame",
            ("wvcu_shake_hand_sts", "wvcu_veh_shake_hand_sts", "shake_hand_status"),
        ),
        epb_status=(
            _int_field(decoded_can_data, "wvcu_prk_1_sts_frame", ("prk_1_sts01_mode",)),
            _int_field(decoded_can_data, "wvcu_prk_1_sts_frame", ("prk_1_sts02_mode",)),
            _int_field(decoded_can_data, "wvcu_prk_1_sts_frame", ("prk_1_sts03_mode",)),
            _int_field(decoded_can_data, "wvcu_prk_1_sts_frame", ("prk_1_sts04_mode",)),
        ),
        gear_status=_int_field(decoded_can_data, "wvcu_vcu_sts_frame", ("wvcu_gear_sts_now",)),
        mcu_mode=tuple(
            _int_field(
                decoded_can_data,
                f"wvcu_mot{index}_sts02_frame",
                (f"mot_{index}_sts02_mot_work_mode",),
            )
            for index in range(1, 9)
        ),
        eps_mode=tuple(
            _int_field(
                decoded_can_data,
                f"wvcu_str{index}_sts_frame",
                (f"str_{index}_mode_sts",),
            )
            for index in range(1, 5)
        ),
        eps_angle=tuple(
            _optional_float_field(
                decoded_can_data,
                f"wvcu_str{index}_sts_frame",
                (f"str_{index}_ang_sts",),
            )
            for index in range(1, 5)
        ),
        ehb_mode=(
            _int_field(decoded_can_data, "wvcu_brk1_sts_frame", ("brk_1_sts01_mode",)),
            _int_field(decoded_can_data, "wvcu_brk1_sts_frame", ("brk_1_sts02_mode",)),
            _int_field(decoded_can_data, "wvcu_brk3_sts_frame", ("brk_3_sts03_mode",)),
            _int_field(decoded_can_data, "wvcu_brk3_sts_frame", ("brk_3_sts04_mode",)),
            _int_field(decoded_can_data, "wvcu_brk5_sts_frame", ("brk_5_sts05_mode",)),
            _int_field(decoded_can_data, "wvcu_brk5_sts_frame", ("brk_5_sts06_mode",)),
            _int_field(decoded_can_data, "wvcu_brk7_sts_frame", ("brk_7_sts07_mode",)),
            _int_field(decoded_can_data, "wvcu_brk7_sts_frame", ("brk_7_sts08_mode",)),
        ),
        vehicle_speed=_float_field(decoded_can_data, "wvcu_veh_spd_sts_now_frame", ("wvcu_veh_spd_now",)),
        vehicle_speed_valid=bool(_get_value(decoded_can_data, "has_wvcu_veh_spd_sts_now_frame", False)),
    )


def _clamp(value: float, minimum: float, maximum: float) -> float:
    return max(minimum, min(maximum, float(value)))


def _invalid_positive_finite_number(value: object) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return True
    return not math.isfinite(float(value)) or value <= 0


def _int_field(decoded_can_data: Any, frame_name: str, field_names: Iterable[str]) -> int:
    return _coerce_int(_frame_field(decoded_can_data, frame_name, field_names, default=0))


def _float_field(decoded_can_data: Any, frame_name: str, field_names: Iterable[str]) -> float:
    return _coerce_float(_frame_field(decoded_can_data, frame_name, field_names, default=0.0))


def _optional_float_field(decoded_can_data: Any, frame_name: str, field_names: Iterable[str]) -> float | None:
    value = _frame_field(decoded_can_data, frame_name, field_names, default=None)
    if value is None:
        return None
    return _coerce_float(value)


def _frame_field(decoded_can_data: Any, frame_name: str, field_names: Iterable[str], default: Any) -> Any:
    frame = _get_value(decoded_can_data, frame_name, None)
    if frame is not None:
        value = _first_value(frame, field_names, None)
        if value is not None:
            return value
    return _first_value(decoded_can_data, field_names, default)


def _first_value(source: Any, names: Iterable[str], default: Any) -> Any:
    for name in names:
        value = _get_value(source, name, None)
        if value is not None:
            return value
    return default


def _get_value(source: Any, name: str, default: Any) -> Any:
    if isinstance(source, dict):
        return source.get(name, default)
    return getattr(source, name, default)


def _coerce_int(value: Any) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _coerce_float(value: Any) -> float:
    try:
        return float(value)
    except (TypeError, ValueError):
        return 0.0
