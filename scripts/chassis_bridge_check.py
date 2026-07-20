#!/usr/bin/env python3
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.chassis_bridge_check import (  # noqa: E402
    DEFAULT_CHASSIS_CONTROL_BRANCH,
    DEFAULT_CHASSIS_CONTROL_ROOT,
    DEFAULT_CONTAINER_WORKSPACE,
    DEFAULT_DOCKER_IMAGE,
    DEFAULT_DOCKER_PLATFORM,
    DEFAULT_MINEPILOT_BRANCH,
    DEFAULT_MINEPILOT_ROOT,
    ChassisBridgeChecker,
    build_chassis_bridge_docker_command_plan,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Validate ChassisControl/MinePilot bridge prerequisites.")
    parser.add_argument("--chassis-control-root", default=str(DEFAULT_CHASSIS_CONTROL_ROOT))
    parser.add_argument("--minepilot-root", default=str(DEFAULT_MINEPILOT_ROOT))
    parser.add_argument("--build-dir", default="build/chassis-control-bridge-check")
    parser.add_argument("--chassis-control-library", help="Explicit libchassis_control path to pass to CMake.")
    parser.add_argument("--chassis-control-branch", default=DEFAULT_CHASSIS_CONTROL_BRANCH)
    parser.add_argument("--minepilot-branch", default=DEFAULT_MINEPILOT_BRANCH)
    parser.add_argument("--skip-cmake", action="store_true", help="Only run file and library path checks.")
    parser.add_argument("--build", action="store_true", help="Run cmake --build after configure succeeds.")
    parser.add_argument("--build-target", default="mine_teleop_chassis_bridge")
    parser.add_argument(
        "--docker-command",
        action="store_true",
        help="Print linux/amd64 Docker commands for building the bridge instead of running checks.",
    )
    parser.add_argument("--host-repo-root", default=str(Path(__file__).resolve().parents[1]))
    parser.add_argument("--docker-image", default=DEFAULT_DOCKER_IMAGE)
    parser.add_argument("--docker-platform", default=DEFAULT_DOCKER_PLATFORM)
    parser.add_argument("--container-workspace", default=DEFAULT_CONTAINER_WORKSPACE)
    args = parser.parse_args()

    if args.docker_command:
        plan = build_chassis_bridge_docker_command_plan(
            host_repo_root=args.host_repo_root,
            chassis_control_root=args.chassis_control_root,
            minepilot_root=args.minepilot_root,
            chassis_control_library=args.chassis_control_library,
            build_dir=args.build_dir,
            build_target=args.build_target,
            image=args.docker_image,
            platform=args.docker_platform,
            container_workspace=args.container_workspace,
        )
        for line in plan.to_jsonl():
            print(line)
        return 0

    report = ChassisBridgeChecker(
        chassis_control_root=args.chassis_control_root,
        minepilot_root=args.minepilot_root,
        build_dir=args.build_dir,
        chassis_control_library=args.chassis_control_library,
        chassis_control_branch=args.chassis_control_branch,
        minepilot_branch=args.minepilot_branch,
        run_cmake=not args.skip_cmake,
        run_build=args.build,
        build_target=args.build_target,
    ).run()
    for line in report.to_jsonl():
        print(line)
    return 0 if report.ready else 2


if __name__ == "__main__":
    raise SystemExit(main())
