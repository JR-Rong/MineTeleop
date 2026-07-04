from __future__ import annotations

import runpy
import sys
from dataclasses import dataclass
from pathlib import Path

from mine_teleop import capacity as _capacity  # noqa: F401
from mine_teleop import chassis_bridge_check as _chassis_bridge_check  # noqa: F401
from mine_teleop import chassis_control as _chassis_control  # noqa: F401
from mine_teleop import closed_loop as _closed_loop  # noqa: F401
from mine_teleop import config as _config  # noqa: F401
from mine_teleop import control as _control  # noqa: F401
from mine_teleop import deployment_validation as _deployment_validation  # noqa: F401
from mine_teleop import driver_console as _driver_console  # noqa: F401
from mine_teleop import driver_console_runtime as _driver_console_runtime  # noqa: F401
from mine_teleop import log_rotation as _log_rotation  # noqa: F401
from mine_teleop import media as _media  # noqa: F401
from mine_teleop import netem as _netem  # noqa: F401
from mine_teleop import observability as _observability  # noqa: F401
from mine_teleop import preflight as _preflight  # noqa: F401
from mine_teleop import recording as _recording  # noqa: F401
from mine_teleop import safety as _safety  # noqa: F401
from mine_teleop import signaling as _signaling  # noqa: F401
from mine_teleop import signaling_service as _signaling_service  # noqa: F401
from mine_teleop import time_sync as _time_sync  # noqa: F401
from mine_teleop import upload as _upload  # noqa: F401
from mine_teleop import vehicle_adapter as _vehicle_adapter  # noqa: F401
from mine_teleop import vehicle_control_service as _vehicle_control_service  # noqa: F401
from mine_teleop import vehicle_media_runtime as _vehicle_media_runtime  # noqa: F401
from mine_teleop import vehicle_recorder_uploader as _vehicle_recorder_uploader  # noqa: F401


@dataclass(frozen=True)
class Entrypoint:
    script: str
    description: str


ENTRYPOINTS: dict[str, Entrypoint] = {
    "vehicle-agent": Entrypoint("vehicle-agent/vehicle_agent.py", "Run vehicle control preflight, adapter smoke, or dev loop."),
    "vehicle-media-agent": Entrypoint("vehicle-media-agent/vehicle_media_agent.py", "Print media pipelines and hardware probe plans."),
    "vehicle-uploader": Entrypoint("vehicle-uploader/vehicle_uploader.py", "Run recorder/uploader demo or service smoke."),
    "driver-console": Entrypoint("driver-console/driver_console.py", "Run driver-console command demo or HTTP control program."),
    "signaling-server": Entrypoint("signaling-server/signaling_server.py", "Run local signaling and upload API service."),
    "acceptance-metrics-report": Entrypoint("scripts/acceptance_metrics_report.py", "Summarize field acceptance JSONL samples."),
    "chassis-bridge-check": Entrypoint("scripts/chassis_bridge_check.py", "Validate/build the ChassisControl C shim bridge."),
    "coturn-usage-report": Entrypoint("scripts/coturn_usage_report.py", "Parse coturn usage logs into JSONL evidence."),
    "netem-plan": Entrypoint("scripts/netem_plan.py", "Print weak-network tc netem dry-run commands."),
    "render-chassis-vehicle-config": Entrypoint(
        "scripts/render_chassis_vehicle_config.py",
        "Render vehicle-agent config for ChassisControl/MinePilot bridge.",
    ),
    "target-host-validation-plan": Entrypoint(
        "scripts/target_host_validation_plan.py",
        "Generate target Ubuntu/CAN host validation commands.",
    ),
    "target-host-validation-report": Entrypoint(
        "scripts/target_host_validation_report.py",
        "Verify archived target-host validation results.",
    ),
    "upload-presign-report": Entrypoint("scripts/upload_presign_report.py", "Emit redacted S3 presign smoke evidence."),
}


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    if not args or args[0] in {"-h", "--help", "help"}:
        _print_help()
        return 0
    if args[0] == "--list":
        for name in sorted(ENTRYPOINTS):
            print(name)
        return 0

    command = args[0]
    entrypoint = ENTRYPOINTS.get(command)
    if entrypoint is None:
        print(f"unknown command: {command}", file=sys.stderr)
        print("run 'mine-teleop --list' to see available commands", file=sys.stderr)
        return 2
    return _run_entrypoint(entrypoint, args[1:])


def _print_help() -> None:
    print("Usage: mine-teleop <command> [args...]")
    print()
    print("Commands:")
    for name in sorted(ENTRYPOINTS):
        print(f"  {name:32} {ENTRYPOINTS[name].description}")


def _run_entrypoint(entrypoint: Entrypoint, args: list[str]) -> int:
    script = _resource_root() / entrypoint.script
    if not script.is_file():
        print(f"entrypoint script not found: {script}", file=sys.stderr)
        return 2
    old_argv = sys.argv
    sys.argv = [str(script), *args]
    try:
        runpy.run_path(str(script), run_name="__main__")
    except SystemExit as exc:
        code = exc.code
        if code is None:
            return 0
        if isinstance(code, int):
            return code
        print(code, file=sys.stderr)
        return 1
    finally:
        sys.argv = old_argv
    return 0


def _resource_root() -> Path:
    bundled_root = getattr(sys, "_MEIPASS", None)
    if bundled_root:
        return Path(str(bundled_root))
    return Path(__file__).resolve().parents[1]


if __name__ == "__main__":
    raise SystemExit(main())
