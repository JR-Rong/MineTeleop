from __future__ import annotations

import base64
import json
import subprocess
import time
import urllib.request
from dataclasses import asdict, dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Protocol
from urllib.parse import urlparse

from .config import CameraConfig, VehicleConfig
from .recording import SegmentMetadata
from .upload import UploadTriggerPolicy
from .vehicle_recorder_uploader import UploadApiClient, VehicleRecorderUploader


@dataclass(frozen=True)
class EncodedFrame:
    camera_id: str
    seq: int
    codec: str
    payload: bytes
    captured_at_ms: int
    encoded_at_ms: int
    width: int
    height: int
    fps: int
    bitrate_kbps: int

    def to_post_payload(self, *, sent_at_ms: int) -> dict[str, Any]:
        return {
            "camera_id": self.camera_id,
            "codec": self.codec,
            "payload_base64": base64.b64encode(self.payload).decode("ascii"),
            "captured_at_ms": self.captured_at_ms,
            "encoded_at_ms": self.encoded_at_ms,
            "sent_at_ms": sent_at_ms,
            "seq": self.seq,
            "width": self.width,
            "height": self.height,
            "fps": self.fps,
            "bitrate_kbps": self.bitrate_kbps,
        }


class FrameEncoder(Protocol):
    def encode(self, camera: CameraConfig, *, seq: int, now_ms: int) -> EncodedFrame:
        ...


class FrameSink(Protocol):
    def send_frame(self, frame: EncodedFrame, *, sent_at_ms: int) -> dict[str, Any]:
        ...


class FfmpegH264FrameEncoder:
    def __init__(self, config: VehicleConfig, *, ffmpeg_binary: str = "ffmpeg") -> None:
        self.config = config
        self.ffmpeg_binary = ffmpeg_binary

    def encode(self, camera: CameraConfig, *, seq: int, now_ms: int) -> EncodedFrame:
        profile = self.config.realtime_profiles[camera.realtime_profile]
        width = int(profile.width)
        height = int(profile.height)
        fps = int(profile.fps)
        command = self._command(camera, width=width, height=height, fps=fps, bitrate_kbps=profile.bitrate_kbps)
        result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if result.returncode != 0:
            stderr = result.stderr.decode("utf-8", errors="replace").strip()
            raise RuntimeError(f"ffmpeg H.264 frame encode failed for {camera.camera_id}: {stderr}")
        if not result.stdout:
            raise RuntimeError(f"ffmpeg H.264 frame encode produced no payload for {camera.camera_id}")
        return EncodedFrame(
            camera_id=camera.camera_id,
            seq=seq,
            codec="h264",
            payload=result.stdout,
            captured_at_ms=now_ms,
            encoded_at_ms=_now_ms(),
            width=width,
            height=height,
            fps=fps,
            bitrate_kbps=profile.bitrate_kbps,
        )

    def _command(self, camera: CameraConfig, *, width: int, height: int, fps: int, bitrate_kbps: int) -> list[str]:
        command = [self.ffmpeg_binary, "-hide_banner", "-loglevel", "error", "-y"]
        if camera.device == "testsrc":
            command.extend(["-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate={fps}"])
        else:
            command.extend(
                [
                    "-f",
                    "v4l2",
                    "-video_size",
                    f"{camera.capture_width}x{camera.capture_height}",
                    "-framerate",
                    str(camera.capture_fps),
                    "-i",
                    camera.device,
                    "-vf",
                    f"scale={width}:{height}",
                ]
            )
        command.extend(
            [
                "-frames:v",
                "1",
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-tune",
                "zerolatency",
                "-b:v",
                f"{bitrate_kbps}k",
                "-pix_fmt",
                "yuv420p",
                "-f",
                "h264",
                "pipe:1",
            ]
        )
        return command


class DriverConsoleFrameSink:
    def __init__(self, driver_console_url: str) -> None:
        self.driver_console_url = driver_console_url.rstrip("/")
        parsed = urlparse(self.driver_console_url)
        if parsed.scheme not in {"http", "https"} or not parsed.netloc:
            raise ValueError("driver_console_url must be an http(s) URL")

    def send_frame(self, frame: EncodedFrame, *, sent_at_ms: int) -> dict[str, Any]:
        payload = frame.to_post_payload(sent_at_ms=sent_at_ms)
        body = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            f"{self.driver_console_url}/api/media/frame",
            data=body,
            method="POST",
            headers={"Content-Type": "application/json"},
        )
        with urllib.request.urlopen(req, timeout=10) as response:
            data = json.loads(response.read().decode("utf-8"))
        if not isinstance(data, dict):
            raise RuntimeError("driver console frame ingest response must be an object")
        return data


class VehicleMediaRuntime:
    def __init__(
        self,
        config: VehicleConfig,
        *,
        frame_sink: FrameSink,
        encoder: FrameEncoder | None = None,
    ) -> None:
        self.config = config
        self.frame_sink = frame_sink
        self.encoder = encoder or FfmpegH264FrameEncoder(config, ffmpeg_binary=config.hardware.encoding.ffmpeg_binary)
        self.last_frame: EncodedFrame | None = None

    def send_frames(
        self,
        *,
        frame_count: int,
        frame_interval_ms: int = 33,
        now_fn: Any = None,
        sleep_fn: Any = None,
    ) -> dict[str, Any]:
        if isinstance(frame_count, bool) or frame_count <= 0:
            raise ValueError("frame_count must be positive")
        if isinstance(frame_interval_ms, bool) or frame_interval_ms < 0:
            raise ValueError("frame_interval_ms must be non-negative")
        cameras = list(self.config.enabled_cameras)
        if not cameras:
            raise RuntimeError("vehicle media runtime requires at least one enabled camera")
        clock = now_fn or _now_ms
        sleeper = sleep_fn or time.sleep
        sent_results: list[dict[str, Any]] = []
        for seq in range(1, frame_count + 1):
            for camera in cameras:
                frame = self.encoder.encode(camera, seq=seq, now_ms=int(clock()))
                self.last_frame = frame
                sent_at_ms = int(clock())
                response = self.frame_sink.send_frame(frame, sent_at_ms=sent_at_ms)
                sent_results.append(
                    {
                        "camera_id": camera.camera_id,
                        "seq": seq,
                        "sent_at_ms": sent_at_ms,
                        "response": response,
                    }
                )
            if seq < frame_count and frame_interval_ms > 0:
                sleeper(frame_interval_ms / 1000.0)
        latencies = [
            int(result["response"]["end_to_end_latency_ms"])
            for result in sent_results
            if isinstance(result.get("response"), dict)
            and "end_to_end_latency_ms" in result["response"]
            and not isinstance(result["response"]["end_to_end_latency_ms"], bool)
        ]
        return {
            "event": "vehicle_media_teleop_summary",
            "passed": len(sent_results) == frame_count * len(cameras),
            "vehicle_id": self.config.vehicle_id,
            "camera_ids": [camera.camera_id for camera in cameras],
            "sent_frames": len(sent_results),
            "latency": {
                "end_to_end_latency_ms_avg": _average(latencies),
                "end_to_end_latency_ms_max": max(latencies) if latencies else 0,
            },
            "frames": sent_results,
        }

    def record_and_upload_once(
        self,
        frame: EncodedFrame,
        *,
        recording_root: str | Path,
        queue_state_path: str | Path,
        archive_root: str | Path,
        upload_api: UploadApiClient,
        now_ms: int | None = None,
    ) -> dict[str, Any]:
        timestamp = _now_ms() if now_ms is None else now_ms
        uploader = VehicleRecorderUploader.from_config(
            self.config,
            recording_root=recording_root,
            queue_state_path=queue_state_path,
            archive_root=archive_root,
            upload_api=upload_api,
        )
        uploader.upload_trigger_policy = UploadTriggerPolicy(trigger_segments=1)
        segment_id = f"{_utc_segment_prefix(timestamp)}_{frame.camera_id}_{frame.seq:06d}"
        metadata = SegmentMetadata(
            vehicle_id=self.config.vehicle_id,
            session_id="teleop-session",
            camera_id=frame.camera_id,
            segment_id=segment_id,
            started_at=_iso_ms(frame.captured_at_ms),
            ended_at=_iso_ms(frame.encoded_at_ms),
            codec=frame.codec,
            encoder="ffmpeg-libx264",
            width=frame.width,
            height=frame.height,
            fps=frame.fps,
            upload_state="pending",
        )
        uploader.record_segment(metadata, payload=frame.payload, now_ms=timestamp)
        result = uploader.process_once(now_ms=timestamp, network_idle=True)
        return asdict(result)


def _average(values: list[int]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _iso_ms(value_ms: int) -> str:
    return datetime.fromtimestamp(value_ms / 1000.0, tz=timezone.utc).isoformat()


def _utc_segment_prefix(value_ms: int) -> str:
    return datetime.fromtimestamp(value_ms / 1000.0, tz=timezone.utc).strftime("%Y%m%dT%H%M%SZ")
