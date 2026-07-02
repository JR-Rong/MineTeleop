#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import math
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.control import ReceiveResult  # noqa: E402
from mine_teleop.observability import (  # noqa: E402
    ControlAcceptanceMetricsRecorder,
    RecordingAcceptanceMetricsRecorder,
    UploadAcceptanceMetricsRecorder,
    VideoAcceptanceMetricsRecorder,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Emit JSONL acceptance metric reports from field sample JSONL.")
    parser.add_argument("--samples", required=True, help="Path to JSONL sample records, or '-' for stdin.")
    parser.add_argument("--scenario", required=True, help="Scenario name such as direct, turn-relay, or weak-net case.")
    args = parser.parse_args()

    samples = _read_samples(args.samples)
    reports = _build_reports(samples)
    failures = _summary_failures(reports)
    summary = {
        "event": "acceptance_metrics_report",
        "scenario": args.scenario,
        "sample_count": len(samples),
        "report_count": len(reports),
        "passed": not failures,
        "failures": failures,
    }
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    for event, report, passed, report_failures in reports:
        print(
            json.dumps(
                {
                    "event": event,
                    "scenario": args.scenario,
                    "passed": passed,
                    "failures": report_failures,
                    "report": report,
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
    return 0 if reports else 2


def _read_samples(path: str) -> list[dict[str, Any]]:
    text = sys.stdin.read() if path == "-" else Path(path).read_text(encoding="utf-8")
    samples: list[dict[str, Any]] = []
    for line_number, line in enumerate(text.splitlines(), start=1):
        if not line.strip():
            continue
        value = json.loads(line)
        if not isinstance(value, dict):
            raise ValueError(f"sample line {line_number} must be a JSON object")
        samples.append(value)
    return samples


def _build_reports(samples: list[dict[str, Any]]) -> list[tuple[str, dict[str, Any], bool, list[str]]]:
    video = VideoAcceptanceMetricsRecorder()
    control = ControlAcceptanceMetricsRecorder()
    recording = RecordingAcceptanceMetricsRecorder()
    upload = UploadAcceptanceMetricsRecorder()
    seen = {"video": False, "control": False, "recording": False, "upload": False}
    upload_realtime_samples: dict[str, set[str]] = {}

    for sample in samples:
        event = _required_string(sample, "event", "event")
        if event == "video_sample":
            seen["video"] = True
            video.record_sample(
                camera_id=_required_string(sample, "camera_id", "video_sample.camera_id"),
                fps=_required_non_negative_int(sample, "fps", "video_sample.fps"),
                bitrate_kbps=_required_non_negative_int(sample, "bitrate_kbps", "video_sample.bitrate_kbps"),
                end_to_end_latency_ms=_required_non_negative_int(
                    sample, "end_to_end_latency_ms", "video_sample.end_to_end_latency_ms"
                ),
                decoded_frames=_required_non_negative_int(sample, "decoded_frames", "video_sample.decoded_frames"),
                dropped_frames=_required_non_negative_int(sample, "dropped_frames", "video_sample.dropped_frames"),
            )
        elif event == "video_decode_failure":
            seen["video"] = True
            video.record_decode_failure(_required_string(sample, "camera_id", "video_decode_failure.camera_id"))
        elif event == "video_reconnect":
            seen["video"] = True
            video.record_reconnect(_required_string(sample, "camera_id", "video_reconnect.camera_id"))
        elif event == "control_driver_send":
            seen["control"] = True
            control.record_driver_send(
                seq=_required_non_negative_int(sample, "seq", "control_driver_send.seq"),
                ts_ms=_required_non_negative_int(sample, "ts_ms", "control_driver_send.ts_ms"),
            )
        elif event == "control_receive":
            seen["control"] = True
            accepted = _required_bool(sample, "accepted", "control_receive.accepted")
            reason = _optional_string(sample, "reason", "control_receive.reason")
            if not accepted and reason == "":
                raise ValueError("control_receive.reason is required when accepted is false")
            control.record_receive(
                ReceiveResult(
                    accepted,
                    reason,
                ),
                receive_time_ms=_required_non_negative_int(
                    sample, "receive_time_ms", "control_receive.receive_time_ms"
                ),
            )
        elif event == "control_rtt":
            seen["control"] = True
            control.record_rtt(_required_non_negative_int(sample, "rtt_ms", "control_rtt.rtt_ms"))
        elif event == "control_timeout":
            seen["control"] = True
            last_valid_receive_ms = _required_non_negative_int(
                sample, "last_valid_receive_ms", "control_timeout.last_valid_receive_ms"
            )
            timeout_entered_ms = _required_non_negative_int(
                sample, "timeout_entered_ms", "control_timeout.timeout_entered_ms"
            )
            if timeout_entered_ms < last_valid_receive_ms:
                raise ValueError("control_timeout.timeout_entered_ms must not be earlier than last_valid_receive_ms")
            control.record_timeout(
                last_valid_receive_ms=last_valid_receive_ms,
                timeout_entered_ms=timeout_entered_ms,
            )
        elif event == "control_brake_sample":
            seen["control"] = True
            control.record_brake_sample(
                now_ms=_required_non_negative_int(sample, "now_ms", "control_brake_sample.now_ms"),
                stage=_required_string(sample, "stage", "control_brake_sample.stage"),
                speed_mps=_required_non_negative_number(sample, "speed_mps", "control_brake_sample.speed_mps"),
                brake_feedback=_required_non_negative_number(
                    sample, "brake_feedback", "control_brake_sample.brake_feedback"
                ),
                distance_since_last_valid_m=_required_non_negative_number(
                    sample,
                    "distance_since_last_valid_m",
                    "control_brake_sample.distance_since_last_valid_m",
                ),
            )
        elif event == "recording_segment":
            seen["recording"] = True
            recording.record_segment(
                camera_id=_required_string(sample, "camera_id", "recording_segment.camera_id"),
                segment_id=_required_string(sample, "segment_id", "recording_segment.segment_id"),
                segment_complete=_required_bool(
                    sample, "segment_complete", "recording_segment.segment_complete"
                ),
                metadata_complete=_required_bool(
                    sample, "metadata_complete", "recording_segment.metadata_complete"
                ),
                file_size_bytes=_required_non_negative_int(
                    sample, "file_size_bytes", "recording_segment.file_size_bytes"
                ),
                encoding_fps=_required_non_negative_int(sample, "encoding_fps", "recording_segment.encoding_fps"),
                write_latency_ms=_required_non_negative_int(
                    sample, "write_latency_ms", "recording_segment.write_latency_ms"
                ),
                disk_used_bytes_after=_required_non_negative_int(
                    sample, "disk_used_bytes_after", "recording_segment.disk_used_bytes_after"
                ),
            )
        elif event == "upload_realtime_baseline":
            seen["upload"] = True
            camera_id = _required_string(sample, "camera_id", "upload_realtime_baseline.camera_id")
            upload_realtime_samples.setdefault(camera_id, set()).add("baseline")
            upload.record_realtime_baseline(
                camera_id=camera_id,
                fps=_required_non_negative_int(sample, "fps", "upload_realtime_baseline.fps"),
                bitrate_kbps=_required_non_negative_int(
                    sample, "bitrate_kbps", "upload_realtime_baseline.bitrate_kbps"
                ),
            )
        elif event == "upload_realtime_during_upload":
            seen["upload"] = True
            camera_id = _required_string(sample, "camera_id", "upload_realtime_during_upload.camera_id")
            upload_realtime_samples.setdefault(camera_id, set()).add("during_upload")
            upload.record_realtime_during_upload(
                camera_id=camera_id,
                fps=_required_non_negative_int(sample, "fps", "upload_realtime_during_upload.fps"),
                bitrate_kbps=_required_non_negative_int(
                    sample, "bitrate_kbps", "upload_realtime_during_upload.bitrate_kbps"
                ),
            )
        elif event == "upload_sample":
            seen["upload"] = True
            started_ms = _required_non_negative_int(sample, "started_ms", "upload_sample.started_ms")
            finished_ms = _required_non_negative_int(sample, "finished_ms", "upload_sample.finished_ms")
            if finished_ms < started_ms:
                raise ValueError("upload_sample.finished_ms must not be earlier than started_ms")
            status = _required_upload_status(sample, "status", "upload_sample.status")
            failure_reason = _optional_string(sample, "failure_reason", "upload_sample.failure_reason")
            if status == "failed" and failure_reason == "":
                raise ValueError("upload_sample.failure_reason is required when status is failed")
            upload.record_upload(
                segment_id=_required_string(sample, "segment_id", "upload_sample.segment_id"),
                bytes_uploaded=_required_non_negative_int(sample, "bytes_uploaded", "upload_sample.bytes_uploaded"),
                started_ms=started_ms,
                finished_ms=finished_ms,
                retry_count=_required_non_negative_int(sample, "retry_count", "upload_sample.retry_count"),
                status=status,
                failure_reason=failure_reason,
            )
        else:
            raise ValueError(f"unknown acceptance sample event {event}")

    for camera_id, kinds in upload_realtime_samples.items():
        if kinds != {"baseline", "during_upload"}:
            raise ValueError(
                f"upload realtime impact for camera {camera_id} requires baseline and during_upload samples"
            )

    reports: list[tuple[str, dict[str, Any], bool, list[str]]] = []
    if seen["video"]:
        _append_report(reports, "video_acceptance_metrics", video.to_report())
    if seen["control"]:
        _append_report(reports, "control_acceptance_metrics", control.to_report())
    if seen["recording"]:
        _append_report(reports, "recording_acceptance_metrics", recording.to_report())
    if seen["upload"]:
        _append_report(reports, "upload_acceptance_metrics", upload.to_report())
    return reports


def _append_report(
    reports: list[tuple[str, dict[str, Any], bool, list[str]]],
    event: str,
    report: dict[str, Any],
) -> None:
    failures = _report_failures(event, report)
    reports.append((event, report, not failures, failures))


def _summary_failures(reports: list[tuple[str, dict[str, Any], bool, list[str]]]) -> list[str]:
    if not reports:
        return ["no acceptance metric reports generated"]
    failures: list[str] = []
    for _event, _report, _passed, report_failures in reports:
        failures.extend(report_failures)
    return failures


def _report_failures(event: str, report: dict[str, Any]) -> list[str]:
    if event == "recording_acceptance_metrics":
        return _recording_report_failures(report)
    if event == "upload_acceptance_metrics":
        return _upload_report_failures(report)
    return []


def _recording_report_failures(report: dict[str, Any]) -> list[str]:
    cameras = report.get("cameras", {})
    if not isinstance(cameras, dict) or not cameras:
        return ["recording report has no cameras"]
    failures: list[str] = []
    for camera_id, camera in sorted(cameras.items()):
        if not isinstance(camera, dict):
            failures.append(f"{camera_id} recording report invalid")
            continue
        if camera.get("all_segments_complete") is not True:
            failures.append(f"{camera_id} recording segment incomplete")
        if camera.get("all_metadata_complete") is not True:
            failures.append(f"{camera_id} recording metadata incomplete")
    return failures


def _upload_report_failures(report: dict[str, Any]) -> list[str]:
    failed_count = int(report.get("failed_count", 0) or 0)
    if failed_count <= 0:
        return []
    reasons = report.get("failure_reasons", {})
    if isinstance(reasons, dict) and reasons:
        return [f"upload samples failed: {_count_mapping_summary(reasons)}"]
    return [f"upload samples failed: {failed_count}"]


def _count_mapping_summary(values: dict[Any, Any]) -> str:
    return ", ".join(f"{key}={values[key]}" for key in sorted(values))


def _required_bool(sample: dict[str, Any], field: str, label: str) -> bool:
    value = sample.get(field)
    if not isinstance(value, bool):
        raise ValueError(f"{label} must be a boolean")
    return value


def _required_int(sample: dict[str, Any], field: str, label: str) -> int:
    value = sample.get(field)
    if isinstance(value, bool) or not isinstance(value, int):
        raise ValueError(f"{label} must be an integer")
    return value


def _required_non_negative_int(sample: dict[str, Any], field: str, label: str) -> int:
    value = _required_int(sample, field, label)
    if value < 0:
        raise ValueError(f"{label} must be a non-negative integer")
    return value


def _required_number(sample: dict[str, Any], field: str, label: str) -> float:
    value = sample.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        raise ValueError(f"{label} must be a finite number")
    return float(value)


def _required_non_negative_number(sample: dict[str, Any], field: str, label: str) -> float:
    value = _required_number(sample, field, label)
    if value < 0:
        raise ValueError(f"{label} must be a non-negative number")
    return value


def _required_string(sample: dict[str, Any], field: str, label: str) -> str:
    value = sample.get(field)
    if not isinstance(value, str) or value == "":
        raise ValueError(f"{label} must be a non-empty string")
    return value


def _optional_string(sample: dict[str, Any], field: str, label: str) -> str:
    if field not in sample:
        return ""
    value = sample[field]
    if not isinstance(value, str):
        raise ValueError(f"{label} must be a string")
    return value


def _required_upload_status(sample: dict[str, Any], field: str, label: str) -> str:
    value = _required_string(sample, field, label)
    if value not in {"uploaded", "failed"}:
        raise ValueError(f"{label} must be uploaded or failed")
    return value


if __name__ == "__main__":
    raise SystemExit(main())
