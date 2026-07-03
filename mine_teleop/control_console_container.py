from __future__ import annotations

import json
import os
from dataclasses import dataclass
from pathlib import Path
from typing import Mapping

from .config import load_driver_config
from .driver_console import DriverOperationLog
from .driver_console_runtime import DriverConsoleHttpApp, DriverConsoleRuntime, JsonlControlCommandSink


@dataclass(frozen=True)
class ContainerDriverConsoleSettings:
    config_path: str = "configs/driver-console.dev.yaml"
    host: str = "0.0.0.0"
    port: int = 8080
    signaling_http_url: str = ""
    vehicle_id: str = "vehicle-001"
    password: str = "dev-password"
    operation_log: str = "/tmp/mine-teleop-driver-console/operation-log.jsonl"
    operation_log_max_bytes: int = 10 * 1024 * 1024
    operation_log_backup_count: int = 5
    frame_dir: str = "/tmp/mine-teleop-driver-console/frames"
    control_output: str = ""

    @classmethod
    def from_env(cls, env: Mapping[str, str] | None = None) -> "ContainerDriverConsoleSettings":
        values = os.environ if env is None else env

        return cls(
            config_path=values.get("MINE_TELEOP_DRIVER_CONSOLE_CONFIG", cls.config_path),
            host=values.get("MINE_TELEOP_DRIVER_CONSOLE_HOST", cls.host),
            port=_env_int(values, "MINE_TELEOP_DRIVER_CONSOLE_PORT", cls.port),
            signaling_http_url=values.get("MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL", cls.signaling_http_url),
            vehicle_id=values.get("MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID", cls.vehicle_id),
            password=values.get("MINE_TELEOP_DRIVER_CONSOLE_PASSWORD", cls.password),
            operation_log=values.get("MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG", cls.operation_log),
            operation_log_max_bytes=_env_int(
                values,
                "MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG_MAX_BYTES",
                cls.operation_log_max_bytes,
            ),
            operation_log_backup_count=_env_int(
                values,
                "MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG_BACKUP_COUNT",
                cls.operation_log_backup_count,
            ),
            frame_dir=values.get("MINE_TELEOP_DRIVER_CONSOLE_FRAME_DIR", cls.frame_dir),
            control_output=values.get("MINE_TELEOP_DRIVER_CONSOLE_CONTROL_OUTPUT", cls.control_output),
        )


def build_runtime(settings: ContainerDriverConsoleSettings) -> DriverConsoleRuntime:
    config = load_driver_config(Path(settings.config_path))
    operation_log = (
        DriverOperationLog(
            settings.operation_log,
            max_bytes=settings.operation_log_max_bytes,
            backup_count=settings.operation_log_backup_count,
        )
        if settings.operation_log
        else None
    )
    control_sink = JsonlControlCommandSink(settings.control_output) if settings.control_output else None
    return DriverConsoleRuntime(
        config,
        signaling_http_url=settings.signaling_http_url or config.cloud.signaling_url,
        vehicle_id=settings.vehicle_id,
        password=settings.password,
        control_sink=control_sink,
        operation_log=operation_log,
        config_version=settings.config_path,
        frame_dir=settings.frame_dir,
    )


def main() -> int:
    settings = ContainerDriverConsoleSettings.from_env()
    runtime = build_runtime(settings)
    app = DriverConsoleHttpApp(runtime)
    server = app.make_server(settings.host, settings.port)
    actual_host, actual_port = server.server_address
    print(
        json.dumps(
            {
                "event": "driver_console_container_startup",
                "host": actual_host,
                "port": actual_port,
                "status": "serving",
                "config_path": settings.config_path,
                "vehicle_id": settings.vehicle_id,
                "signaling_http_url": settings.signaling_http_url or runtime.signaling_http_url,
                "operation_log": settings.operation_log,
                "frame_dir": settings.frame_dir,
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


def _env_int(values: Mapping[str, str], key: str, default: int) -> int:
    raw = values.get(key)
    if raw is None or raw == "":
        return default
    return int(raw)


if __name__ == "__main__":
    raise SystemExit(main())
