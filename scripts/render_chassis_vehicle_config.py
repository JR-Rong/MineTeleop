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
    parser.add_argument("--recording-root", help="Recording root directory to write into recording.root_dir.")
    parser.add_argument("--network-interface", help="Network interface to write into hardware.network.interface.")
    parser.add_argument("--ffmpeg-binary", help="Bundle ffmpeg path to write into hardware.encoding.ffmpeg_binary.")
    parser.add_argument("--ffprobe-binary", help="Bundle ffprobe path to write into hardware.encoding.ffprobe_binary.")
    parser.add_argument("--vainfo-binary", help="Bundle vainfo path to write into hardware.encoding.vainfo_binary.")
    parser.add_argument("--libva-drivers-path", help="Bundle VAAPI driver directory to write into hardware.encoding.libva_drivers_path.")
    parser.add_argument(
        "--camera-device",
        action="append",
        default=[],
        metavar="CAMERA_ID=DEVICE",
        help="Enable and bind an existing camera id to a device path. May be repeated.",
    )
    parser.add_argument(
        "--camera-capture-size",
        metavar="WIDTHxHEIGHT",
        help="Capture size to apply to cameras provided via --camera-device.",
    )
    parser.add_argument(
        "--camera-capture-fps",
        type=int,
        help="Capture FPS to apply to cameras provided via --camera-device.",
    )
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
    _inject_runtime_overrides(
        data,
        parser=parser,
        recording_root=args.recording_root,
        network_interface=args.network_interface,
        ffmpeg_binary=args.ffmpeg_binary,
        ffprobe_binary=args.ffprobe_binary,
        vainfo_binary=args.vainfo_binary,
        libva_drivers_path=args.libva_drivers_path,
        camera_devices=args.camera_device,
        camera_capture_size=args.camera_capture_size,
        camera_capture_fps=args.camera_capture_fps,
    )
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


def _inject_runtime_overrides(
    data: dict[str, Any],
    *,
    parser: argparse.ArgumentParser,
    recording_root: str | None,
    network_interface: str | None,
    ffmpeg_binary: str | None,
    ffprobe_binary: str | None,
    vainfo_binary: str | None,
    libva_drivers_path: str | None,
    camera_devices: list[str],
    camera_capture_size: str | None,
    camera_capture_fps: int | None,
) -> None:
    if recording_root:
        recording = data.setdefault("recording", {})
        if not isinstance(recording, dict):
            parser.error("base config recording section must be a mapping")
        recording["root_dir"] = recording_root

    if any((network_interface, ffmpeg_binary, ffprobe_binary, vainfo_binary, libva_drivers_path)):
        hardware = data.setdefault("hardware", {})
        if not isinstance(hardware, dict):
            parser.error("base config hardware section must be a mapping")
        if network_interface:
            network = hardware.setdefault("network", {})
            if not isinstance(network, dict):
                parser.error("base config hardware.network section must be a mapping")
            network["interface"] = network_interface
        if any((ffmpeg_binary, ffprobe_binary, vainfo_binary, libva_drivers_path)):
            encoding = hardware.setdefault("encoding", {})
            if not isinstance(encoding, dict):
                parser.error("base config hardware.encoding section must be a mapping")
            for key, value in (
                ("ffmpeg_binary", ffmpeg_binary),
                ("ffprobe_binary", ffprobe_binary),
                ("vainfo_binary", vainfo_binary),
                ("libva_drivers_path", libva_drivers_path),
            ):
                if value:
                    encoding[key] = value

    if camera_devices:
        size = _parse_capture_size(parser, camera_capture_size) if camera_capture_size else None
        if camera_capture_fps is not None and camera_capture_fps <= 0:
            parser.error("--camera-capture-fps must be positive")
        cameras = data.get("cameras")
        if not isinstance(cameras, list):
            parser.error("base config cameras section must be a list")
        by_id = {camera.get("id"): camera for camera in cameras if isinstance(camera, dict)}
        for raw in camera_devices:
            if "=" not in raw:
                parser.error("--camera-device must use CAMERA_ID=DEVICE")
            camera_id, device = raw.split("=", 1)
            if not camera_id or not device:
                parser.error("--camera-device must use non-empty CAMERA_ID=DEVICE")
            camera = by_id.get(camera_id)
            if camera is None:
                parser.error(f"camera id does not exist in base config: {camera_id}")
            camera["enabled"] = True
            camera["device"] = device
            if size is not None:
                camera["capture_width"] = size[0]
                camera["capture_height"] = size[1]
            if camera_capture_fps is not None:
                camera["capture_fps"] = camera_capture_fps


def _parse_capture_size(parser: argparse.ArgumentParser, value: str) -> tuple[int, int]:
    if "x" not in value:
        parser.error("--camera-capture-size must use WIDTHxHEIGHT")
    raw_width, raw_height = value.split("x", 1)
    try:
        width = int(raw_width)
        height = int(raw_height)
    except ValueError:
        parser.error("--camera-capture-size must use integer WIDTHxHEIGHT")
    if width <= 0 or height <= 0:
        parser.error("--camera-capture-size dimensions must be positive")
    return width, height


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
