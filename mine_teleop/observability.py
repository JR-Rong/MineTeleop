from __future__ import annotations

import json
from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Dict, List

from .config import RuntimeConfigUpdateDecision, RuntimeConfigUpdatePolicy
from .log_rotation import backup_path, rotate_file_if_needed
from .safety import SafetyState
from .vehicle_adapter import MockTelemetry


class TelemetryPublisher:
    def __init__(self, vehicle_id: str, session_id: str, source: str) -> None:
        self.vehicle_id = vehicle_id
        self.session_id = session_id
        self.source = source

    def build(
        self,
        telemetry: MockTelemetry,
        safety_state: SafetyState,
        control_rtt_ms: int,
        video_status: Dict[str, Dict[str, Any]],
        system: Dict[str, Any],
        now_ms: int,
    ) -> Dict[str, Any]:
        return {
            "type": "telemetry",
            "protocol_version": 1,
            "vehicle_id": self.vehicle_id,
            "session_id": self.session_id,
            "ts_ms": now_ms,
            "source": self.source,
            "mock_telemetry": self.source == "mock",
            "speed_mps": telemetry.speed_mps,
            "gear": telemetry.gear,
            "steering_feedback": telemetry.steering_feedback,
            "throttle_feedback": telemetry.throttle_feedback,
            "brake_feedback": telemetry.brake_feedback,
            "estop": telemetry.estop,
            "safety_state": safety_state.value,
            "fault_flags": self._fault_flags(system, video_status),
            "link": {
                "control_rtt_ms": control_rtt_ms,
            },
            "video": self._normalize_video_status(video_status),
            "system": system,
        }

    def _normalize_video_status(self, video_status: Dict[str, Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
        normalized = {}
        for camera_id, status in video_status.items():
            state = str(status.get("state", "unknown"))
            normalized[camera_id] = {
                "state": state,
                "fps": status.get("fps", 0),
                "bitrate_kbps": status.get("bitrate_kbps", 0),
                "latency_ms": status.get("latency_ms"),
                "low_bitrate": bool(status.get("low_bitrate", False)),
                "reconnecting": state == "reconnecting" or bool(status.get("reconnecting", False)),
            }
            if "fault" in status:
                normalized[camera_id]["fault"] = status["fault"]
            if "encoder" in status:
                normalized[camera_id]["encoder"] = status["encoder"]
        return normalized

    def _fault_flags(self, system: Dict[str, Any], video_status: Dict[str, Dict[str, Any]]) -> List[str]:
        flags = [str(flag) for flag in system.get("fault_flags", [])]
        for camera_id, status in video_status.items():
            fault = status.get("fault")
            if fault:
                flags.append(f"video.{camera_id}.{fault}")
        return flags


class OperationsMetricsBuilder:
    def build(
        self,
        vehicle_system: Dict[str, Any],
        video_status: Dict[str, Dict[str, Any]],
        control: Dict[str, Any],
        cloud: Dict[str, Any],
        driver: Dict[str, Any] | None = None,
        vehicle_adapter: Dict[str, Any] | None = None,
    ) -> Dict[str, Any]:
        driver = driver or {}
        upload_success_count = int(cloud.get("upload_success_count", 0))
        upload_failure_count = int(cloud.get("upload_failure_count", 0))
        upload_total = upload_success_count + upload_failure_count
        upload_success_rate = upload_success_count / upload_total if upload_total else 0.0
        return {
            "vehicle": {
                "cpu_percent": vehicle_system.get("cpu_percent", 0.0),
                "gpu_percent": vehicle_system.get("gpu_percent", 0.0),
                "memory_percent": vehicle_system.get("memory_percent", 0.0),
                "disk_free_gb": vehicle_system.get("disk_free_gb", 0.0),
                "disk_write_mbps": vehicle_system.get("disk_write_mbps", 0.0),
                "network_5g_state": vehicle_system.get("network_5g_state", "unknown"),
                "encoder_fps_by_camera": {
                    camera_id: status.get("fps", 0) for camera_id, status in video_status.items()
                },
                "realtime_bitrate_kbps_by_camera": {
                    camera_id: status.get("bitrate_kbps", 0) for camera_id, status in video_status.items()
                },
                "control_command_hz": control.get("command_hz", 0),
                "control_timeout_count": control.get("timeout_count", 0),
            },
            "vehicle_adapter": self._vehicle_adapter_status(vehicle_adapter or {}),
            "cloud": {
                "signaling_connections": cloud.get("signaling_connections", 0),
                "turn_relay_bytes": cloud.get("turn_relay_bytes", 0),
                "active_sessions": cloud.get("active_sessions", 0),
                "upload_success_rate": upload_success_rate,
                "upload_failure_reasons": dict(cloud.get("upload_failure_reasons", {})),
            },
            "driver": {
                "video_decode_fps_by_camera": {
                    camera_id: status.get("decode_fps", 0) for camera_id, status in video_status.items()
                },
                "control_send_hz": driver.get("control_send_hz", 0),
                "ui_jank_ms": driver.get("ui_jank_ms", 0),
                "rtt_ms": driver.get("rtt_ms", 0),
                "packet_loss_rate": driver.get("packet_loss_rate", 0.0),
            },
        }

    def _vehicle_adapter_status(self, status: Dict[str, Any]) -> Dict[str, Any]:
        return {
            "adapter_type": status.get("adapter_type", "unknown"),
            "opened": bool(status.get("opened", False)),
            "healthy": bool(status.get("healthy", False)),
            "can_interface": status.get("can_interface"),
            "library_path": status.get("library_path"),
            "last_error": status.get("last_error"),
            "applied_command_count": int(status.get("applied_command_count", 0) or 0),
            "safe_stop_count": int(status.get("safe_stop_count", 0) or 0),
        }


class RecordingAcceptanceMetricsRecorder:
    def __init__(self) -> None:
        self._cameras: Dict[str, Dict[str, Any]] = {}

    def record_segment(
        self,
        camera_id: str,
        segment_id: str,
        segment_complete: bool,
        metadata_complete: bool,
        file_size_bytes: int,
        encoding_fps: int,
        write_latency_ms: int,
        disk_used_bytes_after: int,
    ) -> None:
        camera = self._camera(camera_id)
        camera["segment_ids"].append(segment_id)
        camera["segment_complete"].append(segment_complete)
        camera["metadata_complete"].append(metadata_complete)
        camera["file_size_bytes"].append(file_size_bytes)
        camera["encoding_fps"].append(encoding_fps)
        camera["write_latency_ms"].append(write_latency_ms)
        camera["disk_used_bytes_after"].append(disk_used_bytes_after)

    def to_report(self) -> Dict[str, Any]:
        cameras = {}
        for camera_id, camera in self._cameras.items():
            segment_count = len(camera["segment_ids"])
            complete_segment_count = sum(1 for value in camera["segment_complete"] if value)
            metadata_complete_count = sum(1 for value in camera["metadata_complete"] if value)
            file_sizes = camera["file_size_bytes"]
            disk_samples = camera["disk_used_bytes_after"]
            cameras[camera_id] = {
                "segment_count": segment_count,
                "complete_segment_count": complete_segment_count,
                "metadata_complete_count": metadata_complete_count,
                "all_segments_complete": segment_count > 0 and complete_segment_count == segment_count,
                "all_metadata_complete": segment_count > 0 and metadata_complete_count == segment_count,
                "file_size_bytes_total": sum(file_sizes),
                "file_size_bytes_avg": _average(file_sizes),
                "encoding_fps_avg": _average(camera["encoding_fps"]),
                "write_latency_ms_avg": _average(camera["write_latency_ms"]),
                "disk_growth_bytes": self._disk_growth_bytes(disk_samples),
            }
        return {"cameras": cameras}

    def _camera(self, camera_id: str) -> Dict[str, Any]:
        if camera_id not in self._cameras:
            self._cameras[camera_id] = {
                "segment_ids": [],
                "segment_complete": [],
                "metadata_complete": [],
                "file_size_bytes": [],
                "encoding_fps": [],
                "write_latency_ms": [],
                "disk_used_bytes_after": [],
            }
        return self._cameras[camera_id]

    def _disk_growth_bytes(self, disk_samples: List[int]) -> int:
        if len(disk_samples) < 2:
            return 0
        return disk_samples[-1] - disk_samples[0]


class UploadAcceptanceMetricsRecorder:
    def __init__(self) -> None:
        self._uploads: List[Dict[str, Any]] = []
        self._realtime: Dict[str, Dict[str, List[int]]] = {}

    def record_upload(
        self,
        segment_id: str,
        bytes_uploaded: int,
        started_ms: int,
        finished_ms: int,
        retry_count: int,
        status: str,
        failure_reason: str = "",
    ) -> None:
        self._uploads.append(
            {
                "segment_id": segment_id,
                "bytes_uploaded": bytes_uploaded,
                "started_ms": started_ms,
                "finished_ms": finished_ms,
                "retry_count": retry_count,
                "status": status,
                "failure_reason": failure_reason,
            }
        )

    def record_realtime_baseline(self, camera_id: str, fps: int, bitrate_kbps: int) -> None:
        camera = self._realtime_camera(camera_id)
        camera["baseline_fps"].append(fps)
        camera["baseline_bitrate_kbps"].append(bitrate_kbps)

    def record_realtime_during_upload(self, camera_id: str, fps: int, bitrate_kbps: int) -> None:
        camera = self._realtime_camera(camera_id)
        camera["during_upload_fps"].append(fps)
        camera["during_upload_bitrate_kbps"].append(bitrate_kbps)

    def to_report(self) -> Dict[str, Any]:
        uploaded_count = sum(1 for upload in self._uploads if upload["status"] == "uploaded")
        failed_count = sum(1 for upload in self._uploads if upload["status"] == "failed")
        bytes_uploaded_total = sum(int(upload["bytes_uploaded"]) for upload in self._uploads)
        duration_ms_total = sum(self._duration_ms(upload) for upload in self._uploads)
        return {
            "upload_count": len(self._uploads),
            "uploaded_count": uploaded_count,
            "failed_count": failed_count,
            "bytes_uploaded_total": bytes_uploaded_total,
            "upload_speed_mbps_avg": self._upload_speed_mbps(bytes_uploaded_total, duration_ms_total),
            "retry_count_total": sum(int(upload["retry_count"]) for upload in self._uploads),
            "failure_reasons": self._failure_reasons(),
            "realtime_impact": self._realtime_impact(),
        }

    def _realtime_camera(self, camera_id: str) -> Dict[str, List[int]]:
        if camera_id not in self._realtime:
            self._realtime[camera_id] = {
                "baseline_fps": [],
                "baseline_bitrate_kbps": [],
                "during_upload_fps": [],
                "during_upload_bitrate_kbps": [],
            }
        return self._realtime[camera_id]

    def _duration_ms(self, upload: Dict[str, Any]) -> int:
        return max(0, int(upload["finished_ms"]) - int(upload["started_ms"]))

    def _upload_speed_mbps(self, bytes_uploaded: int, duration_ms: int) -> float:
        if duration_ms <= 0:
            return 0.0
        return round(bytes_uploaded * 8 / duration_ms / 1000, 3)

    def _failure_reasons(self) -> Dict[str, int]:
        reasons: Dict[str, int] = {}
        for upload in self._uploads:
            reason = str(upload.get("failure_reason", ""))
            if not reason:
                continue
            reasons[reason] = reasons.get(reason, 0) + 1
        return reasons

    def _realtime_impact(self) -> Dict[str, Dict[str, float]]:
        impact = {}
        for camera_id, camera in self._realtime.items():
            baseline_fps = _average(camera["baseline_fps"])
            during_fps = _average(camera["during_upload_fps"])
            baseline_bitrate = _average(camera["baseline_bitrate_kbps"])
            during_bitrate = _average(camera["during_upload_bitrate_kbps"])
            impact[camera_id] = {
                "baseline_fps_avg": baseline_fps,
                "during_upload_fps_avg": during_fps,
                "fps_delta": during_fps - baseline_fps,
                "baseline_bitrate_kbps_avg": baseline_bitrate,
                "during_upload_bitrate_kbps_avg": during_bitrate,
                "bitrate_kbps_delta": during_bitrate - baseline_bitrate,
            }
        return impact


class VideoAcceptanceMetricsRecorder:
    def __init__(self) -> None:
        self._cameras: Dict[str, Dict[str, Any]] = {}

    def record_sample(
        self,
        camera_id: str,
        fps: int,
        bitrate_kbps: int,
        end_to_end_latency_ms: int,
        decoded_frames: int,
        dropped_frames: int,
    ) -> None:
        camera = self._camera(camera_id)
        camera["fps_samples"].append(fps)
        camera["bitrate_kbps_samples"].append(bitrate_kbps)
        camera["end_to_end_latency_ms_samples"].append(end_to_end_latency_ms)
        camera["decoded_frames"] += decoded_frames
        camera["dropped_frames"] += dropped_frames

    def record_decode_failure(self, camera_id: str) -> None:
        self._camera(camera_id)["decode_failure_count"] += 1

    def record_reconnect(self, camera_id: str) -> None:
        self._camera(camera_id)["reconnect_count"] += 1

    def to_report(self) -> Dict[str, Any]:
        cameras = {}
        for camera_id, camera in self._cameras.items():
            decoded_frames = int(camera["decoded_frames"])
            dropped_frames = int(camera["dropped_frames"])
            total_frames = decoded_frames + dropped_frames
            dropped_frame_rate = dropped_frames / total_frames if total_frames else 0.0
            cameras[camera_id] = {
                "fps_avg": _average(camera["fps_samples"]),
                "bitrate_kbps_avg": _average(camera["bitrate_kbps_samples"]),
                "end_to_end_latency_ms_avg": _average(camera["end_to_end_latency_ms_samples"]),
                "decoded_frames": decoded_frames,
                "dropped_frames": dropped_frames,
                "dropped_frame_rate": round(dropped_frame_rate, 3),
                "decode_failure_count": int(camera["decode_failure_count"]),
                "reconnect_count": int(camera["reconnect_count"]),
            }
        return {"cameras": cameras}

    def _camera(self, camera_id: str) -> Dict[str, Any]:
        if camera_id not in self._cameras:
            self._cameras[camera_id] = {
                "fps_samples": [],
                "bitrate_kbps_samples": [],
                "end_to_end_latency_ms_samples": [],
                "decoded_frames": 0,
                "dropped_frames": 0,
                "decode_failure_count": 0,
                "reconnect_count": 0,
            }
        return self._cameras[camera_id]


class ControlAcceptanceMetricsRecorder:
    def __init__(self) -> None:
        self.driver_send_times_ms: List[int] = []
        self.received_times_ms: List[int] = []
        self.rtt_samples_ms: List[int] = []
        self.rejection_counts: Dict[str, int] = {}
        self.timeout_trigger_ms: int | None = None
        self.last_valid_receive_ms: int | None = None
        self.brake_stage_samples: List[Dict[str, Any]] = []

    def record_driver_send(self, seq: int, ts_ms: int) -> None:
        self.driver_send_times_ms.append(ts_ms)

    def record_receive(self, result: Any, receive_time_ms: int) -> None:
        if getattr(result, "accepted", False):
            self.received_times_ms.append(receive_time_ms)
            return
        reason = str(getattr(result, "reason", "unknown"))
        self.rejection_counts[reason] = self.rejection_counts.get(reason, 0) + 1

    def record_rtt(self, rtt_ms: int) -> None:
        self.rtt_samples_ms.append(rtt_ms)

    def record_timeout(self, last_valid_receive_ms: int, timeout_entered_ms: int) -> None:
        self.last_valid_receive_ms = last_valid_receive_ms
        self.timeout_trigger_ms = timeout_entered_ms

    def record_brake_sample(
        self,
        now_ms: int,
        stage: str,
        speed_mps: float,
        brake_feedback: float,
        distance_since_last_valid_m: float,
    ) -> None:
        self.brake_stage_samples.append(
            {
                "now_ms": now_ms,
                "stage": stage,
                "speed_mps": speed_mps,
                "brake_feedback": brake_feedback,
                "distance_since_last_valid_m": distance_since_last_valid_m,
            }
        )

    def to_report(self) -> Dict[str, Any]:
        return {
            "control_send_hz": _frequency_hz(self.driver_send_times_ms),
            "control_receive_hz": _frequency_hz(self.received_times_ms),
            "control_rtt_ms_avg": _average(self.rtt_samples_ms),
            "command_out_of_order_count": self.rejection_counts.get("old_seq", 0),
            "command_expired_count": self.rejection_counts.get("command_gap_exceeded", 0),
            "rejection_counts": dict(self.rejection_counts),
            "timeout_trigger_ms": self.timeout_trigger_ms,
            "coast_time_before_timeout_ms": self._coast_time_before_timeout_ms(),
            "coast_distance_before_timeout_m": self._coast_distance_before_timeout_m(),
            "brake_stage_samples": list(self.brake_stage_samples),
            "stopping_distance_m": self._stopping_distance_m(),
        }

    def _coast_time_before_timeout_ms(self) -> int | None:
        if self.timeout_trigger_ms is None or self.last_valid_receive_ms is None:
            return None
        return self.timeout_trigger_ms - self.last_valid_receive_ms

    def _coast_distance_before_timeout_m(self) -> float | None:
        if not self.brake_stage_samples:
            return None
        return self.brake_stage_samples[0]["distance_since_last_valid_m"]

    def _stopping_distance_m(self) -> float | None:
        if not self.brake_stage_samples:
            return None
        return self.brake_stage_samples[-1]["distance_since_last_valid_m"]


@dataclass(frozen=True)
class ComponentLogEvent:
    ts_ms: int
    level: str
    component: str
    vehicle_id: str = ""
    session_id: str = ""
    camera_id: str = ""
    event: str = ""
    message: str = ""
    error_code: str = ""

    def to_record(self) -> Dict[str, str]:
        ts = datetime.fromtimestamp(self.ts_ms / 1000, tz=timezone.utc)
        return {
            "ts": ts.isoformat(timespec="milliseconds").replace("+00:00", "Z"),
            "level": self.level.upper(),
            "component": self.component,
            "vehicle_id": self.vehicle_id,
            "session_id": self.session_id,
            "camera_id": self.camera_id,
            "event": self.event,
            "message": self.message,
            "error_code": self.error_code,
        }


class ComponentLog:
    def __init__(
        self,
        path: Path | str,
        max_bytes: int | None = None,
        backup_count: int = 0,
        min_level: str = "info",
    ) -> None:
        self.path = Path(path)
        self.max_bytes = max_bytes
        self.backup_count = backup_count
        self.min_level = _normalize_log_level(min_level)

    def append(self, event: ComponentLogEvent) -> None:
        if _log_level_value(event.level) < _log_level_value(self.min_level):
            return
        self.path.parent.mkdir(parents=True, exist_ok=True)
        line = json.dumps(event.to_record(), ensure_ascii=False, sort_keys=True) + "\n"
        self._rotate_if_needed(len(line.encode("utf-8")))
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(line)

    def apply_runtime_update(
        self,
        path: str,
        value: Any,
        policy: RuntimeConfigUpdatePolicy | None = None,
    ) -> RuntimeConfigUpdateDecision:
        decision = (policy or RuntimeConfigUpdatePolicy.default()).evaluate(path, value)
        if not decision.allowed:
            return decision
        if path != "logging.level":
            return RuntimeConfigUpdateDecision(path, False, "runtime_update_not_applicable_to_component_log", False)
        self.min_level = _normalize_log_level(str(value))
        return decision

    def _rotate_if_needed(self, incoming_bytes: int) -> None:
        rotate_file_if_needed(self.path, incoming_bytes, self.max_bytes, self.backup_count)

    def _backup_path(self, index: int) -> Path:
        return backup_path(self.path, index)


@dataclass(frozen=True)
class AuditEvent:
    ts_ms: int
    event: str
    vehicle_id: str
    session_id: str
    actor: str
    details: Dict[str, Any] = field(default_factory=dict)


class AuditLog:
    def __init__(self, path: Path | str, max_bytes: int | None = None, backup_count: int = 0) -> None:
        self.path = Path(path)
        self.max_bytes = max_bytes
        self.backup_count = backup_count

    def append(self, event: AuditEvent) -> None:
        self.path.parent.mkdir(parents=True, exist_ok=True)
        line = json.dumps(asdict(event), ensure_ascii=False, sort_keys=True) + "\n"
        rotate_file_if_needed(self.path, len(line.encode("utf-8")), self.max_bytes, self.backup_count)
        with self.path.open("a", encoding="utf-8") as handle:
            handle.write(line)


def _frequency_hz(times_ms: List[int]) -> float:
    if len(times_ms) < 2:
        return 0.0
    duration_ms = times_ms[-1] - times_ms[0]
    if duration_ms <= 0:
        return 0.0
    return round((len(times_ms) - 1) * 1000 / duration_ms, 3)


def _average(values: List[int]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


_LOG_LEVELS = {
    "debug": 10,
    "info": 20,
    "warning": 30,
    "error": 40,
}


def _normalize_log_level(level: str) -> str:
    normalized = str(level).lower()
    if normalized not in _LOG_LEVELS:
        raise ValueError("log level must be debug, info, warning, or error")
    return normalized


def _log_level_value(level: str) -> int:
    return _LOG_LEVELS.get(str(level).lower(), _LOG_LEVELS["info"])
