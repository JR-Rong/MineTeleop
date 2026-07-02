#!/usr/bin/env python3
from __future__ import annotations

import argparse
from pathlib import Path
from typing import Any

import yaml


TELEMETRY_FIELDS = [
    "speed_mps",
    "gear",
    "steering_feedback",
    "throttle_feedback",
    "brake_feedback",
    "estop",
]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Render a vehicle-agent config for the ChassisControl/MinePilot C shim adapter."
    )
    parser.add_argument("--base-config", default="configs/vehicle-agent.dev.yaml")
    parser.add_argument("--output", help="Write rendered YAML to this path instead of stdout.")
    parser.add_argument("--adapter-type", choices=["can", "dynamic_library"], default="can")
    parser.add_argument("--chassis-control-root", default="/Volumes/SystemDisk/Workspace/ChassisControl")
    parser.add_argument("--minepilot-root", default="/Volumes/SystemDisk/Workspace/MinePilot")
    parser.add_argument("--bridge-library", default="/opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so")
    parser.add_argument(
        "--chassis-control-library",
        required=True,
        help="Actual libchassis_control path linked by the C shim.",
    )
    parser.add_argument("--can-interface", default="can0")
    parser.add_argument("--cmake-target", default="chassis_control")
    parser.add_argument("--library-output-name", default="libchassis_control.so")
    parser.add_argument("--max-control-timeout-ms", type=int, required=True)
    parser.add_argument("--calibration-evidence", required=True)
    args = parser.parse_args()
    chassis_control_root = Path(args.chassis_control_root)
    minepilot_root = Path(args.minepilot_root)
    bridge_library = _existing_file_arg(parser, args.bridge_library, "bridge library")
    chassis_control_library = _existing_file_arg(parser, args.chassis_control_library, "chassis control library")

    data = _load_yaml_mapping(Path(args.base_config))
    _inject_timeout_calibration(
        data,
        max_control_timeout_ms=args.max_control_timeout_ms,
        evidence=args.calibration_evidence,
    )
    _inject_can_hardware(data, interface=args.can_interface)
    data["vehicle_adapter"] = _vehicle_adapter_config(
        parser=parser,
        adapter_type=args.adapter_type,
        chassis_control_root=chassis_control_root,
        minepilot_root=minepilot_root,
        bridge_library=bridge_library,
        chassis_control_library=chassis_control_library,
        can_interface=args.can_interface,
        cmake_target=args.cmake_target,
        library_output_name=args.library_output_name,
    )

    rendered = yaml.safe_dump(data, allow_unicode=True, sort_keys=False)
    if args.output:
        Path(args.output).write_text(rendered, encoding="utf-8")
    else:
        print(rendered, end="")
    return 0


def _existing_file_arg(parser: argparse.ArgumentParser, value: str, label: str) -> Path:
    path = Path(value)
    if not path.is_file():
        parser.error(f"{label} does not exist: {path}")
    return path


def _required_file(parser: argparse.ArgumentParser, path: Path, label: str) -> Path:
    if not path.is_file():
        parser.error(f"{label} does not exist: {path}")
    return path


def _load_yaml_mapping(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as handle:
        data = yaml.safe_load(handle) or {}
    if not isinstance(data, dict):
        raise ValueError(f"{path} must contain a YAML mapping")
    return data


def _inject_timeout_calibration(
    data: dict[str, Any],
    *,
    max_control_timeout_ms: int,
    evidence: str,
) -> None:
    if max_control_timeout_ms <= 0:
        raise ValueError("--max-control-timeout-ms must be positive")
    if not evidence:
        raise ValueError("--calibration-evidence is required")
    control = data.setdefault("control", {})
    if not isinstance(control, dict):
        raise ValueError("base config control section must be a mapping")
    control_timeout_ms = control.get("control_timeout_ms")
    if (
        isinstance(control_timeout_ms, int)
        and not isinstance(control_timeout_ms, bool)
        and control_timeout_ms > max_control_timeout_ms
    ):
        raise ValueError("control.control_timeout_ms exceeds calibrated maximum")
    control["timeout_calibration"] = {
        "max_control_timeout_ms": max_control_timeout_ms,
        "evidence": evidence,
    }


def _inject_can_hardware(data: dict[str, Any], *, interface: str) -> None:
    hardware = data.setdefault("hardware", {})
    if not isinstance(hardware, dict):
        raise ValueError("base config hardware section must be a mapping")
    can = hardware.setdefault("can", {})
    if not isinstance(can, dict):
        raise ValueError("base config hardware.can section must be a mapping")
    can["interface"] = interface


def _vehicle_adapter_config(
    *,
    parser: argparse.ArgumentParser,
    adapter_type: str,
    chassis_control_root: Path,
    minepilot_root: Path,
    bridge_library: Path,
    chassis_control_library: Path,
    can_interface: str,
    cmake_target: str,
    library_output_name: str,
) -> dict[str, Any]:
    chassis_header = _required_file(parser, chassis_control_root / "chassis_control.h", "ChassisControl header")
    chassis_can_common = _required_file(
        parser,
        chassis_control_root / "include" / "can" / "can_common.h",
        "ChassisControl CAN common header",
    )
    minepilot_can_common = _required_file(
        parser,
        minepilot_root / "include" / "can" / "can_common.h",
        "MinePilot CAN common header",
    )
    minepilot_can_message = _required_file(
        parser,
        minepilot_root / "include" / "can" / "can_message.h",
        "MinePilot CAN message header",
    )
    minepilot_can_db_header = _required_file(parser, minepilot_root / "include" / "can_db.h", "MinePilot CAN DB header")
    minepilot_can_receiver_header = _required_file(
        parser,
        minepilot_root / "include" / "can_receiver.h",
        "MinePilot CAN receiver header",
    )
    minepilot_can_sender_header = _required_file(
        parser,
        minepilot_root / "include" / "can_sender.h",
        "MinePilot CAN sender header",
    )
    minepilot_can_db_source = _required_file(parser, minepilot_root / "src" / "can_db.cpp", "MinePilot CAN DB source")
    minepilot_can_receiver_source = _required_file(
        parser,
        minepilot_root / "src" / "can_receiver.cpp",
        "MinePilot CAN receiver source",
    )
    minepilot_can_sender_source = _required_file(
        parser,
        minepilot_root / "src" / "can_sender.cpp",
        "MinePilot CAN sender source",
    )
    chassis_control = {
        "source_root": _path(chassis_control_root),
        "header_path": _path(chassis_header),
        "can_common_header_path": _path(chassis_can_common),
        "cmake_target": cmake_target,
        "library_output_name": library_output_name,
        "can_interface": can_interface,
        "abi": "c_shim",
        "requires_cpp_bridge": False,
        "bridge_library_path": _path(bridge_library),
        "library_path": _path(chassis_control_library),
    }
    return {
        "type": adapter_type,
        "contract": {
            "steering_unit": "normalized",
            "throttle_unit": "normalized",
            "brake_unit": "normalized",
            "brake_semantics": "normalized_service_brake",
            "gear_values": ["P", "R", "N", "D"],
            "heartbeat_period_ms": 50,
            "safe_stop_supported": True,
            "estop_supported": True,
            "command_ack": "telemetry_feedback",
            "telemetry_fields": list(TELEMETRY_FIELDS),
        },
        "integration": {
            "chassis_control": chassis_control,
            "minepilot": {
                "source_root": _path(minepilot_root),
                "can_common_header_path": _path(minepilot_can_common),
                "can_message_header_path": _path(minepilot_can_message),
                "can_db_header_path": _path(minepilot_can_db_header),
                "can_receiver_header_path": _path(minepilot_can_receiver_header),
                "can_sender_header_path": _path(minepilot_can_sender_header),
                "can_db_source_path": _path(minepilot_can_db_source),
                "can_receiver_source_path": _path(minepilot_can_receiver_source),
                "can_sender_source_path": _path(minepilot_can_sender_source),
            },
        },
    }


def _path(path: Path) -> str:
    return str(path)


if __name__ == "__main__":
    raise SystemExit(main())
