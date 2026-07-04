#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from dataclasses import asdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.closed_loop import run_mock_closed_loop
from mine_teleop.config import effective_vehicle_config_log_payload, load_vehicle_config
from mine_teleop.control import ControlCommand
from mine_teleop.preflight import VehiclePreflightChecker
from mine_teleop.time_sync import TimeSyncMonitor, TimeSyncStatus
from mine_teleop.vehicle_adapter import create_vehicle_adapter
from mine_teleop.vehicle_control_service import VehicleControlService
from mine_teleop.vehicle_teleop_runtime import VehicleTeleopRuntime


def main() -> int:
    parser = argparse.ArgumentParser(description="Run the Mine Teleop mock vehicle agent loop.")
    parser.add_argument("--config", default="configs/vehicle-agent.dev.yaml")
    parser.add_argument("--duration-ms", type=int, default=1500)
    parser.add_argument("--run-loop", action="store_true", help="Run the long-lived control service simulation.")
    parser.add_argument("--disconnect-at-ms", type=int, default=500)
    parser.add_argument("--preflight", action="store_true", help="Run startup device and path checks, then exit.")
    parser.add_argument(
        "--adapter-status",
        action="store_true",
        help="Open the configured vehicle adapter, print its status as JSON, then exit.",
    )
    parser.add_argument(
        "--poll-feedback",
        action="store_true",
        help="With --adapter-status, poll one decoded CAN feedback frame and print the result as JSON.",
    )
    parser.add_argument(
        "--require-feedback",
        action="store_true",
        help="With --adapter-status, return nonzero unless decoded CAN feedback polling succeeds.",
    )
    parser.add_argument(
        "--hardware-device",
        action="append",
        default=[],
        help="Additional hardware device path to check during --preflight.",
    )
    parser.add_argument(
        "--teleop",
        action="store_true",
        help="Run the live teleop loop: consume relayed driver control commands and execute them.",
    )
    parser.add_argument(
        "--signaling-http-url",
        default="",
        help="Signaling base URL for --teleop (defaults to the vehicle config cloud.signaling_url).",
    )
    parser.add_argument("--device-token", default="dev-device-secret", help="Vehicle device token for --teleop.")
    parser.add_argument("--teleop-duration-ms", type=int, default=5000, help="How long to run the --teleop loop.")
    parser.add_argument("--teleop-poll-interval-ms", type=int, default=50, help="--teleop signaling poll cadence.")
    parser.add_argument(
        "--teleop-session-wait-ms",
        type=int,
        default=5000,
        help="How long --teleop waits for an active driver session before giving up.",
    )
    parser.add_argument(
        "--teleop-log-controls",
        action="store_true",
        help="With --teleop, print one JSONL record for each accepted control command.",
    )
    args = parser.parse_args()

    config = load_vehicle_config(args.config)
    print(json.dumps(effective_vehicle_config_log_payload(config), ensure_ascii=False, sort_keys=True))
    if args.teleop:
        return _run_teleop(config, args)
    if args.adapter_status:
        return _run_adapter_status_smoke(
            config,
            poll_feedback=args.poll_feedback or args.require_feedback,
            require_feedback=args.require_feedback,
        )

    if args.preflight:
        report = VehiclePreflightChecker(config, hardware_devices=args.hardware_device).run()
        print(
            json.dumps(
                {
                    "event": "vehicle_preflight",
                    "vehicle_id": config.vehicle_id,
                    "ready": report.ready,
                    "check_count": len(report.checks),
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
        for check in report.checks:
            print(json.dumps(check.__dict__, ensure_ascii=False, sort_keys=True))
        return 0 if report.ready else 2

    if args.run_loop:
        time_sync = TimeSyncMonitor(config.control.time_sync_minimum).assess(
            TimeSyncStatus(source="dev-simulated", synchronized=True, offset_ms=0.0, stratum=1),
            component="vehicle-agent",
            vehicle_id=config.vehicle_id,
            session_id="session-001",
            now_ms=0,
        )
        print(json.dumps(time_sync.log_event.to_record(), ensure_ascii=False, sort_keys=True))
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            telemetry_interval_ms=100,
        )
        service.start(now_ms=0)
        for now_ms in range(0, args.duration_ms + 1, 50):
            if now_ms < args.disconnect_at_ms:
                seq = now_ms // 50 + 1
                service.receive_command(
                    ControlCommand(
                        vehicle_id=config.vehicle_id,
                        session_id="session-001",
                        seq=seq,
                        ts_ms=now_ms,
                        gear="D",
                        steering=0.0,
                        throttle=0.25,
                        brake=0.0,
                    ),
                    now_ms=now_ms,
                )
            service.tick(now_ms)
        print(
            f"vehicle={config.vehicle_id} final_state={service.safety.state.value} "
            f"accepted_commands={service.applied_command_count} "
            f"telemetry_count={len(service.telemetry_history)}"
        )
        return 0

    if config.vehicle_adapter_type != "mock":
        print(
            json.dumps(
                {
                    "event": "vehicle_agent_mode_error",
                    "vehicle_id": config.vehicle_id,
                    "mode": "mock_demo",
                    "reason": "mock_demo_requires_mock_adapter",
                    "vehicle_adapter_type": config.vehicle_adapter_type,
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
        return 2

    result = run_mock_closed_loop(duration_ms=args.duration_ms, disconnect_at_ms=args.disconnect_at_ms, estop_at_ms=300)
    print(
        f"vehicle={config.vehicle_id} final_state={result.final_state.value} "
        f"accepted_commands={result.commands_applied_before_disconnect}"
    )
    return 0


def _run_teleop(config, args) -> int:
    runtime = VehicleTeleopRuntime(
        config,
        signaling_http_url=args.signaling_http_url or config.cloud.signaling_url,
        device_token=args.device_token,
    )
    summary = runtime.run(
        duration_ms=args.teleop_duration_ms,
        poll_interval_ms=args.teleop_poll_interval_ms,
        session_wait_ms=args.teleop_session_wait_ms,
        control_log_callback=(
            lambda record: print(json.dumps(record, ensure_ascii=False, sort_keys=True), flush=True)
            if args.teleop_log_controls
            else None
        ),
    )
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    return 0 if summary.get("session_discovered") else 2


def _run_adapter_status_smoke(config, *, poll_feedback: bool = False, require_feedback: bool = False) -> int:
    adapter = None
    exit_code = 2
    try:
        adapter = create_vehicle_adapter(config.vehicle_adapter_type, config.vehicle_adapter_contract)
        adapter.open()
        status = adapter.get_status()
        ready = bool(status.opened and status.healthy)
        print(
            json.dumps(
                {
                    "event": "vehicle_adapter_status",
                    "vehicle_id": config.vehicle_id,
                    "ready": ready,
                    "status": asdict(status),
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
        feedback_ready = True
        if poll_feedback:
            feedback_payload = _adapter_feedback_poll_payload(adapter)
            print(
                json.dumps(
                    {
                        "event": "vehicle_adapter_feedback_poll",
                        "vehicle_id": config.vehicle_id,
                    }
                    | feedback_payload,
                    ensure_ascii=False,
                    sort_keys=True,
                )
            )
            feedback_ready = bool(feedback_payload["received"]) or not require_feedback
        exit_code = 0 if ready and feedback_ready else 2
    except Exception as exc:
        status_payload = _adapter_status_payload(adapter, config.vehicle_adapter_type, str(exc))
        print(
            json.dumps(
                {
                    "event": "vehicle_adapter_status",
                    "vehicle_id": config.vehicle_id,
                    "ready": False,
                    "status": status_payload,
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
        exit_code = 2
    finally:
        if adapter is not None:
            try:
                adapter.close()
            except Exception as exc:
                print(
                    json.dumps(
                        {
                            "event": "vehicle_adapter_close_error",
                            "vehicle_id": config.vehicle_id,
                            "error": str(exc),
                        },
                        ensure_ascii=False,
                        sort_keys=True,
                    )
                )
                exit_code = 2
    return exit_code


def _adapter_feedback_poll_payload(adapter) -> dict:
    poll_feedback = getattr(adapter, "poll_feedback", None)
    if not callable(poll_feedback):
        return {
            "attempted": False,
            "received": False,
            "reason": "adapter_feedback_poll_not_supported",
            "snapshot": None,
        }
    try:
        snapshot = poll_feedback()
    except Exception as exc:
        return {
            "attempted": True,
            "received": False,
            "reason": "adapter_feedback_poll_error",
            "error": str(exc),
            "snapshot": None,
        }
    if snapshot is None:
        return {
            "attempted": True,
            "received": False,
            "reason": "no_feedback_frame",
            "snapshot": None,
        }
    update_feedback = getattr(adapter, "update_feedback", None)
    if callable(update_feedback):
        try:
            update_feedback(snapshot)
        except Exception as exc:
            return {
                "attempted": True,
                "received": False,
                "reason": "adapter_feedback_update_error",
                "error": str(exc),
                "snapshot": asdict(snapshot),
            }
    return {
        "attempted": True,
        "received": True,
        "reason": "feedback_frame_received",
        "snapshot": asdict(snapshot),
    }


def _adapter_status_payload(adapter, adapter_type: str, error: str) -> dict:
    if adapter is None:
        return {
            "adapter_type": adapter_type,
            "opened": False,
            "healthy": False,
            "can_interface": None,
            "library_path": None,
            "applied_command_count": 0,
            "safe_stop_count": 0,
            "last_error": error,
        }
    payload = asdict(adapter.get_status())
    payload["healthy"] = False
    if payload.get("last_error") is None:
        payload["last_error"] = error
    return payload


if __name__ == "__main__":
    raise SystemExit(main())
