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
from mine_teleop.driver_console_runtime import DriverConsoleHttpApp, DriverConsoleRuntime, JsonlControlCommandSink


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a local driver console command generation demo.")
    parser.add_argument("--config", default="configs/driver-console.dev.yaml")
    parser.add_argument("--operation-log", help="Append local driver operation events as JSONL.")
    parser.add_argument("--operation-log-max-bytes", type=int, help="Rotate the operation log before this size.")
    parser.add_argument("--operation-log-backup-count", type=int, default=0, help="Number of rotated operation logs to keep.")
    parser.add_argument("--serve", action="store_true", help="Run the Docker-friendly HTTP driver console program.")
    parser.add_argument("--host", default="127.0.0.1", help="HTTP host for --serve.")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port for --serve; use 0 to pick a free port.")
    parser.add_argument("--port-file", default="", help="Write the selected HTTP port for --serve.")
    parser.add_argument("--signaling-http-url", help="HTTP base URL for the signaling service.")
    parser.add_argument("--vehicle-id", default="vehicle-001", help="Vehicle id to request when connecting.")
    parser.add_argument("--password", default="dev-password", help="Driver login password.")
    parser.add_argument("--control-output", help="Write generated control commands as JSONL instead of signaling relay.")
    args = parser.parse_args()

    config = load_driver_config(args.config)
    operation_log = (
        DriverOperationLog(
            args.operation_log,
            max_bytes=args.operation_log_max_bytes,
            backup_count=args.operation_log_backup_count,
        )
        if args.operation_log
        else None
    )
    if args.serve:
        sink = JsonlControlCommandSink(args.control_output) if args.control_output else None
        runtime = DriverConsoleRuntime(
            config,
            signaling_http_url=args.signaling_http_url or config.cloud.signaling_url,
            vehicle_id=args.vehicle_id,
            password=args.password,
            control_sink=sink,
            operation_log=operation_log,
            config_version=args.config,
        )
        app = DriverConsoleHttpApp(runtime)
        server = app.make_server(args.host, args.port)
        actual_host, actual_port = server.server_address
        if args.port_file:
            Path(args.port_file).write_text(str(actual_port), encoding="utf-8")
        print(
            json.dumps(
                {
                    "event": "driver_console_startup",
                    "host": actual_host,
                    "port": actual_port,
                    "status": "serving",
                },
                ensure_ascii=False,
                sort_keys=True,
            ),
            flush=True,
        )
        try:
            server.serve_forever()
        except KeyboardInterrupt:
            pass
        finally:
            server.server_close()
        return 0

    vehicle_id = "vehicle-001"
    session_id = "session-001"

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
