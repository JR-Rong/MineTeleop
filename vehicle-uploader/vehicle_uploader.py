#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.config import load_vehicle_config
from mine_teleop.recording import SegmentMetadata
from mine_teleop.signaling_service import SignalingHttpService
from mine_teleop.upload import UploadTriggerPolicy
from mine_teleop.vehicle_recorder_uploader import UploadApiClient, VehicleRecorderUploader


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a local recorder/uploader development demo.")
    parser.add_argument("--config", default="configs/vehicle-agent.dev.yaml")
    parser.add_argument("--work-dir", default=".local/uploader-demo")
    parser.add_argument("--service-mode", action="store_true", help="Process configured recording sidecars instead of creating demo data.")
    parser.add_argument("--process-once", action="store_true", help="Run one uploader iteration and exit; intended for service smoke tests.")
    parser.add_argument("--poll-interval-seconds", type=float, default=5.0, help="Delay between service-mode uploader iterations.")
    parser.add_argument("--upload-api-base-url", default="", help="Override the HTTP base URL for Upload API calls.")
    parser.add_argument("--device-token", default="dev-device-secret")
    parser.add_argument("--queue-state-path", default="")
    parser.add_argument("--archive-root", default="")
    parser.add_argument("--network-idle", action="store_true", help="Tell the upload trigger policy that the network is idle.")
    parser.add_argument("--json", action="store_true", help="Print process results as JSONL evidence records.")
    args = parser.parse_args()
    if args.poll_interval_seconds <= 0:
        parser.error("--poll-interval-seconds must be positive")
    if args.process_once and not args.service_mode:
        parser.error("--process-once requires --service-mode")
    if args.service_mode:
        return _run_service(args)
    return _run_demo(args)


def _run_demo(args: argparse.Namespace) -> int:
    root = Path(args.work_dir)
    config = load_vehicle_config(args.config)
    signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
    with signaling.running() as base_url:
        uploader = VehicleRecorderUploader.from_config(
            config,
            recording_root=root / "recordings",
            queue_state_path=root / "upload-queue.json",
            archive_root=root / "archive",
            upload_api=UploadApiClient(base_url),
        )
        uploader.upload_trigger_policy = UploadTriggerPolicy(trigger_segments=1)
        metadata = SegmentMetadata(
            vehicle_id="vehicle-001",
            session_id="session-001",
            camera_id="front",
            segment_id="demo_front_000001",
            started_at="2026-06-24T10:15:00Z",
            ended_at="2026-06-24T10:16:00Z",
            codec="h264",
            encoder="vaapi",
            width=1920,
            height=1080,
            fps=30,
            upload_state="pending",
        )
        uploader.record_segment(metadata, payload=b"demo-h264")
        result = uploader.process_once(now_ms=1_000)
        print(_format_result(result, json_output=args.json))
    return 0


def _run_service(args: argparse.Namespace) -> int:
    root = Path(args.work_dir)
    config = load_vehicle_config(args.config)
    upload_api_base_url = args.upload_api_base_url or _origin_from_http_url(config.cloud.auth_url)
    uploader = VehicleRecorderUploader.from_config(
        config,
        recording_root=Path(config.recording.root_dir),
        queue_state_path=Path(args.queue_state_path) if args.queue_state_path else root / "upload-queue.json",
        archive_root=Path(args.archive_root) if args.archive_root else root / "archive",
        upload_api=UploadApiClient(upload_api_base_url, device_token=args.device_token),
    )
    while True:
        result = uploader.process_once(now_ms=int(time.time() * 1000), network_idle=args.network_idle)
        print(_format_result(result, json_output=args.json), flush=True)
        if args.process_once:
            return 0
        time.sleep(args.poll_interval_seconds)


def _origin_from_http_url(value: str) -> str:
    parsed = urlparse(value)
    if parsed.scheme not in {"http", "https"} or not parsed.netloc:
        raise ValueError("cloud.auth_url must include an http(s) scheme and host")
    return f"{parsed.scheme}://{parsed.netloc}"


def _format_result(result, *, json_output: bool = False) -> str:
    if json_output:
        record = {
            "action": result.action,
            "event": "vehicle_uploader_process_once",
            "passed": result.action != "failed",
            "segment_id": result.segment_id,
        }
        if result.object_path:
            record["object_path"] = result.object_path
        if result.metadata_object_path:
            record["metadata_object_path"] = result.metadata_object_path
        if result.error:
            record["error"] = result.error
        if result.retry_after_ms:
            record["retry_after_ms"] = result.retry_after_ms
        return json.dumps(record, sort_keys=True)
    parts = [result.action, f"segment_id={result.segment_id}"]
    if result.object_path:
        parts.append(f"object_path={result.object_path}")
    if result.metadata_object_path:
        parts.append(f"metadata_object_path={result.metadata_object_path}")
    if result.error:
        parts.append(f"error={result.error}")
    if result.retry_after_ms:
        parts.append(f"retry_after_ms={result.retry_after_ms}")
    return " ".join(parts)


if __name__ == "__main__":
    raise SystemExit(main())
