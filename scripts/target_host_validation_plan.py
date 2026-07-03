#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.chassis_bridge_check import (  # noqa: E402
    DEFAULT_CHASSIS_CONTROL_BRANCH,
    DEFAULT_MINEPILOT_BRANCH,
)
from mine_teleop.config import ConfigError, load_vehicle_config  # noqa: E402
from mine_teleop.deployment_validation import TargetHostValidationPlan  # noqa: E402


def main() -> int:
    parser = argparse.ArgumentParser(description="Print target-host validation commands for Mine Teleop deployment.")
    parser.add_argument("--vehicle-config", default="/etc/mine-teleop/vehicle-agent.yaml")
    parser.add_argument("--hardware-device", action="append")
    parser.add_argument("--can-interface")
    parser.add_argument("--network-interface")
    parser.add_argument("--chassis-control-root", default="/Volumes/SystemDisk/Workspace/ChassisControl")
    parser.add_argument("--minepilot-root", default="/Volumes/SystemDisk/Workspace/MinePilot")
    parser.add_argument("--chassis-control-branch", default=DEFAULT_CHASSIS_CONTROL_BRANCH)
    parser.add_argument("--minepilot-branch", default=DEFAULT_MINEPILOT_BRANCH)
    parser.add_argument("--bridge-library", default="/opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so")
    parser.add_argument(
        "--chassis-control-library",
        default="/Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so",
    )
    parser.add_argument("--bridge-build-dir", default="build/chassis-control-bridge")
    parser.add_argument("--uploader-work-dir", default="/var/lib/mine-teleop/uploader")
    parser.add_argument("--minepilot-can-probe-build-dir", default="/tmp/mine-teleop-minepilot-can-probe")
    parser.add_argument("--can-probe-timeout-seconds", type=int)
    parser.add_argument("--acceptance-samples", default="/tmp/mine-teleop-acceptance-samples.jsonl")
    parser.add_argument("--acceptance-scenario", default="target-host-acceptance")
    parser.add_argument(
        "--mine-teleop-binary",
        help="Generate commands that invoke this bundled mine-teleop binary instead of source-tree Python scripts.",
    )
    parser.add_argument("--format", choices=["jsonl", "shell"], default="jsonl")
    parser.add_argument("--artifact-dir", help="When printing shell, wrap commands to archive stdout, stderr, and return codes here.")
    args = parser.parse_args()
    config_defaults = _vehicle_config_defaults(parser, args.vehicle_config)

    plan = TargetHostValidationPlan.default(
        vehicle_config_path=args.vehicle_config,
        hardware_devices=args.hardware_device or config_defaults["hardware_devices"],
        can_interface=args.can_interface or config_defaults["can_interface"],
        network_interface=args.network_interface or config_defaults["network_interface"],
        chassis_control_root=args.chassis_control_root,
        minepilot_root=args.minepilot_root,
        chassis_control_branch=args.chassis_control_branch,
        minepilot_branch=args.minepilot_branch,
        bridge_library_path=args.bridge_library,
        chassis_control_library=args.chassis_control_library,
        bridge_build_dir=args.bridge_build_dir,
        uploader_work_dir=args.uploader_work_dir,
        minepilot_can_probe_build_dir=args.minepilot_can_probe_build_dir,
        can_probe_timeout_seconds=args.can_probe_timeout_seconds or config_defaults["can_probe_timeout_seconds"],
        acceptance_samples_path=args.acceptance_samples,
        acceptance_scenario=args.acceptance_scenario,
        mine_teleop_binary=args.mine_teleop_binary,
    )
    if args.format == "shell":
        print(plan.to_shell_script(artifact_dir=args.artifact_dir), end="")
    else:
        for line in plan.to_jsonl():
            print(line)
    return 0


def _vehicle_config_defaults(parser: argparse.ArgumentParser, vehicle_config_path: str) -> dict[str, object]:
    fallback = {
        "can_interface": "can0",
        "can_probe_timeout_seconds": 3,
        "hardware_devices": ("/dev/dri/renderD128", "/dev/dri/card1"),
        "network_interface": "wwan0",
    }
    path = Path(vehicle_config_path)
    if not path.exists():
        return fallback
    try:
        config = load_vehicle_config(path)
    except ConfigError as exc:
        parser.error(f"vehicle config is invalid: {exc}")
    return {
        "can_interface": config.hardware.can.interface,
        "can_probe_timeout_seconds": config.hardware.can.probe_timeout_seconds,
        "hardware_devices": tuple(config.hardware.preflight_devices),
        "network_interface": config.hardware.network.interface,
    }


if __name__ == "__main__":
    raise SystemExit(main())
