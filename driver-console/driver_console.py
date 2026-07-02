#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.config import load_driver_config
from mine_teleop.driver_console import (
    ControlCommandGenerator,
    DriverOperationEvent,
    DriverOperationLog,
    EstopInputGuard,
    InputState,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a local driver console command generation demo.")
    parser.add_argument("--config", default="configs/driver-console.dev.yaml")
    parser.add_argument("--operation-log", help="Append local driver operation events as JSONL.")
    parser.add_argument("--operation-log-max-bytes", type=int, help="Rotate the operation log before this size.")
    parser.add_argument("--operation-log-backup-count", type=int, default=0, help="Number of rotated operation logs to keep.")
    args = parser.parse_args()

    config = load_driver_config(args.config)
    vehicle_id = "vehicle-001"
    session_id = "session-001"
    operation_log = (
        DriverOperationLog(
            args.operation_log,
            max_bytes=args.operation_log_max_bytes,
            backup_count=args.operation_log_backup_count,
        )
        if args.operation_log
        else None
    )

    def append_operation(ts_ms: int, event: str, details: dict) -> None:
        if operation_log is None:
            return
        operation_log.append(
            DriverOperationEvent(
                ts_ms=ts_ms,
                event=event,
                driver_id=config.driver_id,
                vehicle_id=vehicle_id,
                session_id=session_id,
                ui_version=config.ui.default_layout,
                config_version=args.config,
                details=details,
            )
        )

    append_operation(0, "login_user", {"result": "success"})
    append_operation(0, "connection_opened", {"signaling_url": config.cloud.signaling_url})
    append_operation(0, "connection_reconnected", {"signaling_url": config.cloud.signaling_url, "attempt": 1})
    append_operation(0, "session_started", {"vehicle_id": vehicle_id})
    append_operation(
        0,
        "control_authority_acquired",
        {"rate_hz": config.control.rate_hz, "estop_hold_ms": config.control.estop_hold_ms},
    )

    generator = ControlCommandGenerator(vehicle_id, session_id, rate_hz=config.control.rate_hz)
    estop_guard = EstopInputGuard(required_hold_ms=config.control.estop_hold_ms)
    print(f"driver={config.driver_id} layout={config.ui.default_layout}")
    for now_ms in (0, generator.period_ms, generator.period_ms * 2):
        estop_pressed = estop_guard.update(raw_pressed=False, now_ms=now_ms)
        command = generator.next_command(InputState(throttle_pressed=True, gear="D", estop_pressed=estop_pressed), now_ms)
        if command is not None:
            print(json.dumps(command.to_dict(), ensure_ascii=False, sort_keys=True))
            append_operation(now_ms, "control_command_sent", {"seq": command.seq, "estop": command.estop})
            if command.estop:
                append_operation(now_ms, "estop_sent", {"seq": command.seq})
    append_operation(generator.period_ms * 3, "control_authority_released", {"reason": "demo_complete"})
    append_operation(generator.period_ms * 3, "session_ended", {"reason": "demo_complete"})
    append_operation(generator.period_ms * 3, "connection_closed", {"reason": "demo_complete"})
    append_operation(generator.period_ms * 3, "logout_user", {"reason": "demo_complete"})
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
