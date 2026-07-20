from __future__ import annotations

import hashlib
import json
import os
from dataclasses import asdict, dataclass, fields, replace
from pathlib import Path


@dataclass(frozen=True)
class SegmentMetadata:
    vehicle_id: str
    session_id: str
    camera_id: str
    segment_id: str
    started_at: str
    ended_at: str
    codec: str
    encoder: str
    width: int
    height: int
    fps: int
    upload_state: str
    file_size_bytes: int = 0
    video_sha256: str = ""

    @classmethod
    def from_dict(cls, raw: dict) -> "SegmentMetadata":
        # Tolerate forward-compatible metadata: ignore unknown keys written by a
        # newer schema instead of raising TypeError. Missing required fields still
        # surface as an error.
        known = {field.name for field in fields(cls)}
        return cls(**{key: value for key, value in raw.items() if key in known})


@dataclass(frozen=True)
class SegmentWriteResult:
    video_path: Path
    metadata_path: Path
    metadata: SegmentMetadata


class SegmentWriter:
    def __init__(self, root_dir: Path | str) -> None:
        self.root_dir = Path(root_dir)

    def write_segment(self, metadata: SegmentMetadata, payload: bytes) -> SegmentWriteResult:
        segment_dir = self.root_dir / metadata.vehicle_id / metadata.session_id / metadata.camera_id
        segment_dir.mkdir(parents=True, exist_ok=True)
        video_path = segment_dir / f"{metadata.segment_id}.mp4"
        metadata_path = segment_dir / f"{metadata.segment_id}.json"
        tmp_video = video_path.with_suffix(".mp4.tmp")
        tmp_metadata = metadata_path.with_suffix(".json.tmp")

        tmp_video.write_bytes(payload)
        os.replace(tmp_video, video_path)

        final_metadata = replace(
            metadata,
            file_size_bytes=video_path.stat().st_size,
            video_sha256=hashlib.sha256(payload).hexdigest(),
        )
        tmp_metadata.write_text(
            json.dumps(asdict(final_metadata), ensure_ascii=False, indent=2, sort_keys=True),
            encoding="utf-8",
        )
        os.replace(tmp_metadata, metadata_path)
        return SegmentWriteResult(video_path=video_path, metadata_path=metadata_path, metadata=final_metadata)


def update_segment_upload_state(metadata_path: Path | str, upload_state: str) -> SegmentMetadata:
    path = Path(metadata_path)
    raw = json.loads(path.read_text(encoding="utf-8"))
    raw["upload_state"] = upload_state
    metadata = SegmentMetadata.from_dict(raw)
    tmp_metadata = path.with_suffix(path.suffix + ".tmp")
    tmp_metadata.write_text(
        json.dumps(asdict(metadata), ensure_ascii=False, indent=2, sort_keys=True),
        encoding="utf-8",
    )
    os.replace(tmp_metadata, path)
    return metadata
