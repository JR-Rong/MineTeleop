from __future__ import annotations

import json
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List


@dataclass(frozen=True)
class RetentionAction:
    action: str
    projected_free_bytes: int
    deleted_segment_ids: List[str]
    reason: str = ""


@dataclass(frozen=True)
class SegmentCandidate:
    segment_id: str
    video_path: Path
    metadata_path: Path
    upload_state: str
    started_at: str


@dataclass(frozen=True)
class UploadBacklogStatus:
    action: str
    pending_bytes: int
    pending_gb: float
    net_growth_gb_per_hour: float
    reason: str = ""


class UploadBacklogMonitor:
    def __init__(
        self,
        recording_gb_per_hour: float,
        upload_gb_per_hour: float,
        required_retention_gb: float,
        upload_lag_policy: str | None,
    ) -> None:
        self.recording_gb_per_hour = recording_gb_per_hour
        self.upload_gb_per_hour = upload_gb_per_hour
        self.required_retention_gb = required_retention_gb
        self.upload_lag_policy = upload_lag_policy

    def evaluate(self, pending_bytes: int) -> UploadBacklogStatus:
        pending_gb = pending_bytes / 1_000_000_000
        net_growth = max(0.0, self.recording_gb_per_hour - self.upload_gb_per_hour)
        if net_growth <= 0:
            return UploadBacklogStatus("ok", pending_bytes, pending_gb, net_growth)
        if not self.upload_lag_policy:
            return UploadBacklogStatus(
                "alert_missing_upload_lag_policy",
                pending_bytes,
                pending_gb,
                net_growth,
                "upload bandwidth below recording production but no lag policy is configured",
            )
        if self.required_retention_gb > 0 and pending_gb >= self.required_retention_gb:
            return UploadBacklogStatus(
                "pause_recording_and_alert",
                pending_bytes,
                pending_gb,
                net_growth,
                "pending backlog has reached configured retention capacity",
            )
        if pending_bytes > 0:
            return UploadBacklogStatus(
                self.upload_lag_policy,
                pending_bytes,
                pending_gb,
                net_growth,
                "upload bandwidth below recording production; applying configured lag policy",
            )
        return UploadBacklogStatus(
            "alert_upload_capacity_below_recording_rate",
            pending_bytes,
            pending_gb,
            net_growth,
            "upload bandwidth below recording production before backlog accumulates",
        )


class DiskWatermarkPolicy:
    def __init__(
        self,
        root_dir: Path | str,
        min_free_bytes: int,
        delete_uploaded_when_below_free_bytes: int,
        delete_unuploaded_when_below_free_bytes: bool,
    ) -> None:
        self.root_dir = Path(root_dir)
        self.min_free_bytes = min_free_bytes
        self.delete_uploaded_when_below_free_bytes = delete_uploaded_when_below_free_bytes
        self.delete_unuploaded_when_below_free_bytes = delete_unuploaded_when_below_free_bytes

    def enforce(self, current_free_bytes: int) -> RetentionAction:
        if current_free_bytes >= self.min_free_bytes:
            return RetentionAction("ok", current_free_bytes, [])
        projected = current_free_bytes
        deleted: List[str] = []
        deleted_unuploaded = False
        for segment in self._segments():
            if segment.upload_state != "uploaded" and not self.delete_unuploaded_when_below_free_bytes:
                continue
            if segment.upload_state != "uploaded":
                deleted_unuploaded = True
            projected += self._delete_segment(segment)
            deleted.append(segment.segment_id)
            if projected >= self.delete_uploaded_when_below_free_bytes:
                return self._deleted_action(projected, deleted, deleted_unuploaded)
        if deleted and projected >= self.delete_uploaded_when_below_free_bytes:
            return self._deleted_action(projected, deleted, deleted_unuploaded)
        if deleted_unuploaded:
            reason = "explicit unuploaded deletion policy applied; disk watermark still below threshold"
        else:
            reason = "unuploaded segments preserved; disk watermark still below threshold"
        return RetentionAction("pause_recording_and_alert", projected, deleted, reason)

    def _deleted_action(self, projected: int, deleted: List[str], deleted_unuploaded: bool) -> RetentionAction:
        if deleted_unuploaded:
            return RetentionAction(
                "deleted_unuploaded_segments",
                projected,
                deleted,
                "explicit unuploaded deletion policy applied",
            )
        return RetentionAction("deleted_uploaded_segments", projected, deleted)

    def _segments(self) -> List[SegmentCandidate]:
        return scan_segment_candidates(self.root_dir)

    def _delete_segment(self, segment: SegmentCandidate) -> int:
        reclaimed = 0
        for path in (segment.video_path, segment.metadata_path):
            if path.exists():
                reclaimed += path.stat().st_size
                path.unlink()
        return reclaimed


def mbps_to_gb_per_hour(mbps: float) -> float:
    return mbps * 3600 / 8 / 1000


def recording_mbps_for(cameras: Iterable[object], record_profiles: Dict[str, object]) -> float:
    total = 0.0
    for camera in cameras:
        if getattr(camera, "enabled"):
            profile = record_profiles[getattr(camera, "record_profile")]
            total += getattr(profile, "bitrate_kbps") / 1000.0
    return total


def scan_segment_candidates(root_dir: Path | str) -> List[SegmentCandidate]:
    candidates = []
    for metadata_path in Path(root_dir).rglob("*.json"):
        # A single corrupt/partial sidecar (e.g. power loss mid-write) must not
        # abort the whole scan, which gates retention/backlog enforcement.
        try:
            raw = json.loads(metadata_path.read_text(encoding="utf-8"))
            segment_id = str(raw["segment_id"])
        except (OSError, ValueError, KeyError, TypeError):
            continue
        video_path = metadata_path.with_suffix(".mp4")
        candidates.append(
            SegmentCandidate(
                segment_id=segment_id,
                video_path=video_path,
                metadata_path=metadata_path,
                upload_state=str(raw.get("upload_state", "pending")),
                started_at=str(raw.get("started_at", "")),
            )
        )
    return sorted(candidates, key=lambda item: (item.started_at, item.segment_id))
