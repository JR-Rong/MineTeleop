from __future__ import annotations

import hashlib
import json
import urllib.request
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict

from .capacity import UploadBacklogMonitor, UploadBacklogStatus, scan_segment_candidates
from .config import RuntimeConfigUpdateDecision, RuntimeConfigUpdatePolicy, VehicleConfig
from .recording import SegmentMetadata, SegmentWriteResult, SegmentWriter, update_segment_upload_state
from .upload import HttpPutUploader, LocalArchiveUploader, UploadBandwidthLimiter, UploadQueue, UploadTriggerPolicy


@dataclass(frozen=True)
class RecorderUploaderResult:
    action: str
    segment_id: str
    object_path: str = ""
    metadata_object_path: str = ""
    error: str = ""
    retry_after_ms: int = 0


class UploadApiClient:
    def __init__(self, base_url: str, device_token: str = "dev-device-secret") -> None:
        self.base_url = base_url.rstrip("/")
        self.device_token = device_token

    def issue_credential(self, metadata: SegmentMetadata, kind: str) -> Dict[str, Any]:
        return self._post(
            "/uploads/credentials",
            {
                "device_token": self.device_token,
                "vehicle_id": metadata.vehicle_id,
                "session_id": metadata.session_id,
                "camera_id": metadata.camera_id,
                "segment_id": metadata.segment_id,
                "started_at": metadata.started_at,
                "kind": kind,
            },
        )

    def mark_uploaded(self, segment_id: str, object_path: str, bytes_uploaded: int) -> Dict[str, Any]:
        return self._post(
            "/uploads/complete",
            {
                "device_token": self.device_token,
                "segment_id": segment_id,
                "object_path": object_path,
                "bytes_uploaded": bytes_uploaded,
            },
        )

    def mark_failed(self, segment_id: str, object_path: str, error: str) -> Dict[str, Any]:
        return self._post(
            "/uploads/failed",
            {
                "device_token": self.device_token,
                "segment_id": segment_id,
                "object_path": object_path,
                "error": error,
            },
        )

    def _post(self, path: str, payload: Dict[str, Any]) -> Dict[str, Any]:
        request = urllib.request.Request(
            self.base_url + path,
            data=json.dumps(payload).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        with urllib.request.urlopen(request, timeout=5) as response:
            return json.loads(response.read().decode("utf-8"))


class VehicleRecorderUploader:
    def __init__(
        self,
        recording_root: Path | str,
        queue_state_path: Path | str,
        archive_root: Path | str,
        upload_api: UploadApiClient,
        refresh_margin_seconds: int,
        retry_initial_seconds: int = 10,
        retry_max_seconds: int = 600,
        max_upload_bandwidth_mbps: float | None = None,
        upload_trigger_policy: UploadTriggerPolicy | None = None,
        upload_enabled: bool = True,
        backlog_monitor: UploadBacklogMonitor | None = None,
        file_uploader: object | None = None,
    ) -> None:
        self.writer = SegmentWriter(recording_root)
        self.queue = UploadQueue(
            queue_state_path,
            refresh_margin_seconds=refresh_margin_seconds,
            retry_initial_seconds=retry_initial_seconds,
            retry_max_seconds=retry_max_seconds,
        )
        self.archive = file_uploader or LocalArchiveUploader(archive_root)
        self.upload_api = upload_api
        self.bandwidth_limiter = (
            UploadBandwidthLimiter(max_upload_bandwidth_mbps)
            if max_upload_bandwidth_mbps is not None
            else None
        )
        self.upload_trigger_policy = upload_trigger_policy or UploadTriggerPolicy()
        self.upload_enabled = upload_enabled
        self._metadata_by_segment: Dict[str, SegmentMetadata] = {}
        self.backlog_monitor = backlog_monitor

    @classmethod
    def from_config(
        cls,
        config: VehicleConfig,
        recording_root: Path | str,
        queue_state_path: Path | str,
        archive_root: Path | str,
        upload_api: UploadApiClient,
    ) -> "VehicleRecorderUploader":
        return cls(
            recording_root=recording_root,
            queue_state_path=queue_state_path,
            archive_root=archive_root,
            upload_api=upload_api,
            refresh_margin_seconds=config.upload.presigned_url_refresh_margin_seconds,
            retry_initial_seconds=config.upload.retry_initial_seconds,
            retry_max_seconds=config.upload.retry_max_seconds,
            max_upload_bandwidth_mbps=config.upload.max_bandwidth_mbps,
            upload_trigger_policy=UploadTriggerPolicy(
                trigger_segments=config.upload.trigger_segments,
                trigger_bytes=(
                    int(config.upload.trigger_bytes_mb * 1_000_000)
                    if config.upload.trigger_bytes_mb is not None
                    else None
                ),
                trigger_interval_ms=(
                    config.upload.trigger_interval_seconds * 1000
                    if config.upload.trigger_interval_seconds is not None
                    else None
                ),
                network_idle_enabled=config.upload.trigger_network_idle,
            ),
            upload_enabled=config.upload.enabled,
            backlog_monitor=UploadBacklogMonitor(
                recording_gb_per_hour=config.capacity.recording_gb_per_hour,
                upload_gb_per_hour=config.capacity.upload_gb_per_hour,
                required_retention_gb=config.capacity.required_retention_gb,
                upload_lag_policy=config.recording.upload_lag_policy,
            ),
            file_uploader=HttpPutUploader() if config.upload.backend == "s3" else None,
        )

    def record_segment(
        self,
        metadata: SegmentMetadata,
        payload: bytes,
        now_ms: int | None = None,
    ) -> SegmentWriteResult:
        segment = self.writer.write_segment(metadata, payload)
        if not self.upload_enabled:
            self._metadata_by_segment[segment.metadata.segment_id] = segment.metadata
            return segment
        video_credential = self.upload_api.issue_credential(segment.metadata, kind="video")
        metadata_credential = self.upload_api.issue_credential(segment.metadata, kind="metadata")
        self.queue.enqueue(
            segment_id=segment.metadata.segment_id,
            video_path=str(segment.video_path),
            metadata_path=str(segment.metadata_path),
            object_path=video_credential["object_path"],
            upload_url=video_credential["upload_url"],
            expires_at_ms=int(video_credential["expires_at_ms"]),
            metadata_object_path=metadata_credential["object_path"],
            metadata_upload_url=metadata_credential["upload_url"],
            metadata_expires_at_ms=int(metadata_credential["expires_at_ms"]),
            video_sha256=segment.metadata.video_sha256,
            metadata_sha256=_sha256_file(segment.metadata_path),
            enqueued_at_ms=now_ms,
        )
        self._metadata_by_segment[segment.metadata.segment_id] = segment.metadata
        return segment

    def scan_pending_segments(self) -> int:
        if not self.upload_enabled:
            return 0
        queued_segment_ids = {item.segment_id for item in self.queue.items}
        enqueued = 0
        for candidate in scan_segment_candidates(self.writer.root_dir):
            if candidate.upload_state != "pending" or candidate.segment_id in queued_segment_ids:
                continue
            if not candidate.video_path.exists():
                continue
            metadata = SegmentMetadata(**json.loads(candidate.metadata_path.read_text(encoding="utf-8")))
            video_credential = self.upload_api.issue_credential(metadata, kind="video")
            metadata_credential = self.upload_api.issue_credential(metadata, kind="metadata")
            self.queue.enqueue(
                segment_id=metadata.segment_id,
                video_path=str(candidate.video_path),
                metadata_path=str(candidate.metadata_path),
                object_path=video_credential["object_path"],
                upload_url=video_credential["upload_url"],
                expires_at_ms=int(video_credential["expires_at_ms"]),
                metadata_object_path=metadata_credential["object_path"],
                metadata_upload_url=metadata_credential["upload_url"],
                metadata_expires_at_ms=int(metadata_credential["expires_at_ms"]),
                video_sha256=metadata.video_sha256 or _sha256_file(candidate.video_path),
                metadata_sha256=_sha256_file(candidate.metadata_path),
            )
            queued_segment_ids.add(metadata.segment_id)
            self._metadata_by_segment[metadata.segment_id] = metadata
            enqueued += 1
        return enqueued

    def process_once(self, now_ms: int, network_idle: bool = False) -> RecorderUploaderResult:
        if not self.upload_enabled:
            return RecorderUploaderResult(action="disabled", segment_id="")
        if self.queue.paused:
            try:
                action = self.queue.next_action(now_ms)
            except IndexError:
                return RecorderUploaderResult(action="idle", segment_id="")
        else:
            if self._first_actionable_item() is None:
                self.scan_pending_segments()
            first_item = self._first_actionable_item()
            if first_item is None:
                return RecorderUploaderResult(action="idle", segment_id="")
            if (
                first_item.status == "retry_wait"
                and first_item.next_retry_at_ms is not None
                and now_ms < first_item.next_retry_at_ms
            ):
                action = self.queue.next_action(now_ms)
            else:
                decision = self._upload_trigger_decision(now_ms, network_idle=network_idle)
                if not decision.dispatch:
                    return RecorderUploaderResult(
                        action="wait",
                        segment_id=first_item.segment_id,
                        object_path=first_item.object_path,
                        metadata_object_path=first_item.metadata_object_path,
                        error=decision.reason,
                    )
                action = self.queue.next_action(now_ms)

        item = action.item
        if action.action == "wait":
            return RecorderUploaderResult(
                action="wait",
                segment_id=item.segment_id,
                object_path=item.object_path,
                metadata_object_path=item.metadata_object_path,
            )
        if action.action == "credential_refresh":
            metadata = self._metadata_for(item)
            video_credential = self.upload_api.issue_credential(metadata, kind="video")
            metadata_credential = self.upload_api.issue_credential(metadata, kind="metadata")
            item.object_path = video_credential["object_path"]
            item.upload_url = video_credential["upload_url"]
            item.expires_at_ms = int(video_credential["expires_at_ms"])
            item.metadata_object_path = metadata_credential["object_path"]
            item.metadata_upload_url = metadata_credential["upload_url"]
            item.metadata_expires_at_ms = int(metadata_credential["expires_at_ms"])
            item.status = "pending"
            self.queue._save()
            return RecorderUploaderResult(
                action="credential_refreshed",
                segment_id=item.segment_id,
                object_path=item.object_path,
                metadata_object_path=item.metadata_object_path,
            )

        video_path = Path(item.video_path)
        metadata_path = Path(item.metadata_path)
        metadata_object_path = item.metadata_object_path or str(Path(item.object_path).with_suffix(".json"))
        if self.bandwidth_limiter is not None:
            retry_after_ms = self.bandwidth_limiter.retry_after_ms(now_ms)
            if retry_after_ms > 0:
                item.status = "pending"
                self.queue._save()
                return RecorderUploaderResult(
                    action="rate_limited",
                    segment_id=item.segment_id,
                    object_path=item.object_path,
                    metadata_object_path=metadata_object_path,
                    retry_after_ms=retry_after_ms,
                )
        if not video_path.exists() or not metadata_path.exists():
            return self._mark_upload_failed(item, metadata_object_path, "source_file_missing", now_ms)

        try:
            archive_result = self.archive.upload(
                segment_id=item.segment_id,
                video_path=video_path,
                metadata_path=metadata_path,
                object_path=item.object_path,
                metadata_object_path=metadata_object_path,
                upload_url=item.upload_url,
                metadata_upload_url=item.metadata_upload_url,
            )
        except Exception as exc:
            error = str(exc) or exc.__class__.__name__
            return self._mark_upload_failed(item, metadata_object_path, error, now_ms)
        update_segment_upload_state(metadata_path, "uploaded")
        if archive_result.metadata_object_path is not None:
            update_segment_upload_state(archive_result.metadata_object_path, "uploaded")
        item.metadata_sha256 = _sha256_file(metadata_path)
        self.queue.mark_uploaded(item.segment_id)
        bytes_uploaded = video_path.stat().st_size
        metadata_bytes_uploaded = metadata_path.stat().st_size
        self.upload_api.mark_uploaded(item.segment_id, item.object_path, bytes_uploaded)
        if item.metadata_object_path:
            self.upload_api.mark_uploaded(item.segment_id, item.metadata_object_path, metadata_bytes_uploaded)
        if self.bandwidth_limiter is not None:
            self.bandwidth_limiter.record_upload(bytes_uploaded + metadata_bytes_uploaded, finished_at_ms=now_ms)
        return RecorderUploaderResult(
            action="uploaded",
            segment_id=item.segment_id,
            object_path=item.object_path,
            metadata_object_path=metadata_object_path,
        )

    def backlog_status(self) -> UploadBacklogStatus:
        pending_bytes = 0
        for item in self.queue.items:
            if item.status == "uploaded":
                continue
            for path_value in (item.video_path, item.metadata_path):
                path = Path(path_value)
                if path.exists():
                    pending_bytes += path.stat().st_size
        if self.backlog_monitor is None:
            return UploadBacklogStatus("ok", pending_bytes, pending_bytes / 1_000_000_000, 0.0)
        return self.backlog_monitor.evaluate(pending_bytes)

    def apply_runtime_update(
        self,
        path: str,
        value: Any,
        policy: RuntimeConfigUpdatePolicy | None = None,
    ) -> RuntimeConfigUpdateDecision:
        decision = (policy or RuntimeConfigUpdatePolicy.default()).evaluate(path, value)
        if not decision.allowed:
            return decision
        if path == "upload.max_bandwidth_mbps":
            self.bandwidth_limiter = UploadBandwidthLimiter(float(value))
        elif path == "upload.paused":
            if value:
                self.queue.pause("runtime_config_update")
            else:
                self.queue.resume()
        else:
            return RuntimeConfigUpdateDecision(path, False, "runtime_update_not_applicable_to_uploader", False)
        return decision

    def _metadata_for(self, item) -> SegmentMetadata:
        metadata = self._metadata_by_segment.get(item.segment_id)
        if metadata is not None:
            return metadata
        raw = json.loads(Path(item.metadata_path).read_text(encoding="utf-8"))
        metadata = SegmentMetadata(**raw)
        self._metadata_by_segment[item.segment_id] = metadata
        return metadata

    def _mark_upload_failed(
        self,
        item,
        metadata_object_path: str,
        error: str,
        now_ms: int,
    ) -> RecorderUploaderResult:
        self.queue.mark_failed(item.segment_id, error, now_ms)
        self.upload_api.mark_failed(item.segment_id, item.object_path, error)
        if item.metadata_object_path:
            self.upload_api.mark_failed(item.segment_id, item.metadata_object_path, error)
        return RecorderUploaderResult(
            action="failed",
            segment_id=item.segment_id,
            object_path=item.object_path,
            metadata_object_path=metadata_object_path,
            error=error,
        )

    def _first_actionable_item(self):
        for item in self.queue.items:
            if item.status in {"pending", "retry_wait", "credential_refresh"}:
                return item
        return None

    def _upload_trigger_decision(self, now_ms: int, network_idle: bool = False):
        pending_items = [
            item
            for item in self.queue.items
            if item.status in {"pending", "retry_wait", "credential_refresh"}
        ]
        pending_bytes = 0
        for item in pending_items:
            for path_value in (item.video_path, item.metadata_path):
                path = Path(path_value)
                if path.exists():
                    pending_bytes += path.stat().st_size
        enqueued_at_values = [
            item.enqueued_at_ms
            for item in pending_items
            if item.enqueued_at_ms is not None
        ]
        oldest_pending_age_ms = (
            max(0, now_ms - min(enqueued_at_values))
            if enqueued_at_values
            else 0
        )
        return self.upload_trigger_policy.evaluate(
            pending_segments=len(pending_items),
            pending_bytes=pending_bytes,
            oldest_pending_age_ms=oldest_pending_age_ms,
            network_idle=network_idle,
        )


def _sha256_file(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()
