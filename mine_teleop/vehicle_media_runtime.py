from __future__ import annotations

import base64
import collections
import http.client
import json
import os
import select
import subprocess
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
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


class _PipedFfmpegStream:
    """A long-lived ffmpeg process whose stdout is framed incrementally.

    stderr is drained on a daemon thread so a chatty ffmpeg cannot fill the
    ~64KB pipe buffer and deadlock stdout frame production (H3).
    """

    def __init__(self, process: subprocess.Popen[bytes]) -> None:
        self.process = process
        self.buffer = b""
        self._stderr_chunks: collections.deque[bytes] = collections.deque(maxlen=64)
        self._stderr_thread = threading.Thread(target=self._drain_stderr, daemon=True)
        self._stderr_thread.start()

    def _drain_stderr(self) -> None:
        stderr = self.process.stderr
        if stderr is None:
            return
        try:
            for chunk in iter(lambda: stderr.read(4096), b""):
                self._stderr_chunks.append(chunk)
        except (ValueError, OSError):
            return

    def stderr_text(self) -> str:
        return b"".join(self._stderr_chunks).decode("utf-8", errors="replace").strip()

    def alive(self) -> bool:
        return self.process.poll() is None

    def close(self) -> None:
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=2)
        self._stderr_thread.join(timeout=1)
        if self.process.stdout is not None:
            try:
                self.process.stdout.close()
            except OSError:
                pass
        if self.process.stderr is not None:
            try:
                self.process.stderr.close()
            except OSError:
                pass


class _PersistentFfmpegEncoder:
    """Shared machinery for persistent (streamed) ffmpeg encoders."""

    codec = ""

    def __init__(self, config: VehicleConfig, *, ffmpeg_binary: str = "ffmpeg", frame_timeout_seconds: float = 2.0) -> None:
        self.config = config
        self.ffmpeg_binary = ffmpeg_binary
        self.frame_timeout_seconds = frame_timeout_seconds
        self._streams: dict[str, _PipedFfmpegStream] = {}
        self._streams_lock = threading.Lock()

    def encode(self, camera: CameraConfig, *, seq: int, now_ms: int) -> EncodedFrame:
        profile = self.config.realtime_profiles[camera.realtime_profile]
        width = int(profile.width)
        height = int(profile.height)
        fps = int(profile.fps)
        stream = self._stream_for(camera, width=width, height=height, fps=fps)
        payload = self._read_unit(camera.camera_id, stream)
        return EncodedFrame(
            camera_id=camera.camera_id,
            seq=seq,
            codec=self.codec,
            payload=payload,
            captured_at_ms=now_ms,
            encoded_at_ms=_now_ms(),
            width=width,
            height=height,
            fps=fps,
            bitrate_kbps=profile.bitrate_kbps,
        )

    def close(self) -> None:
        with self._streams_lock:
            streams = list(self._streams.values())
            self._streams.clear()
        for stream in streams:
            stream.close()

    def _stream_for(self, camera: CameraConfig, *, width: int, height: int, fps: int) -> _PipedFfmpegStream:
        with self._streams_lock:
            stream = self._streams.get(camera.camera_id)
            if stream is not None and stream.alive():
                return stream
            if stream is not None:
                stream.close()
            command = self._command(camera, width=width, height=height, fps=fps)
            process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if process.stdout is None:
                raise RuntimeError("ffmpeg stream stdout is unavailable")
            stream = _PipedFfmpegStream(process)
            self._streams[camera.camera_id] = stream
            return stream

    def _read_unit(self, camera_id: str, stream: _PipedFfmpegStream) -> bytes:
        if stream.process.stdout is None:
            raise RuntimeError(f"ffmpeg stream stdout closed for {camera_id}")
        fd = stream.process.stdout.fileno()
        deadline = time.monotonic() + self.frame_timeout_seconds
        while True:
            unit = self._extract_unit(stream)
            if unit is not None:
                return unit
            if stream.process.poll() is not None:
                raise RuntimeError(f"ffmpeg stream exited for {camera_id}: {stream.stderr_text()}")
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise RuntimeError(f"timed out waiting for a frame from {camera_id}: {stream.stderr_text()}")
            readable, _, _ = select.select([fd], [], [], remaining)
            if not readable:
                raise RuntimeError(f"timed out waiting for a frame from {camera_id}: {stream.stderr_text()}")
            chunk = os.read(fd, 262144)
            if not chunk:
                raise RuntimeError(f"ffmpeg stream ended for {camera_id}: {stream.stderr_text()}")
            stream.buffer += chunk
            self._trim_buffer(stream)

    def _extract_unit(self, stream: _PipedFfmpegStream) -> bytes | None:
        raise NotImplementedError

    def _trim_buffer(self, stream: _PipedFfmpegStream) -> None:
        pass

    def _command(self, camera: CameraConfig, *, width: int, height: int, fps: int) -> list[str]:
        raise NotImplementedError


def _is_mvs_camera_device(device: str) -> bool:
    normalized = device.strip().lower()
    return normalized == "mvs" or normalized.startswith("mvs:") or normalized.startswith("hikrobot:")


def _is_pylon_camera_device(device: str) -> bool:
    normalized = device.strip().lower()
    return normalized == "pylon" or normalized.startswith("pylon:") or normalized.startswith("basler:")


def _mvs_bridge_command(camera: CameraConfig) -> list[str]:
    command = _mvs_bridge_executable()
    command.extend(["--sdk-root", os.environ.get("MINE_TELEOP_MVS_SDK_DIR", "/opt/MVS")])
    selector = camera.device.split(":", 1)[1] if ":" in camera.device else "0"
    selector = selector.strip() or "0"
    if selector.startswith("index="):
        command.extend(["--device-index", selector.split("=", 1)[1]])
    elif selector.startswith("serial="):
        command.extend(["--serial", selector.split("=", 1)[1]])
    elif selector.startswith("model="):
        command.extend(["--model", selector.split("=", 1)[1]])
    elif selector.isdigit():
        command.extend(["--device-index", selector])
    else:
        command.extend(["--serial", selector])
    command.extend(
        [
            "--width",
            str(camera.capture_width),
            "--height",
            str(camera.capture_height),
            "--fps",
            str(camera.capture_fps),
            "--frames",
            "0",
            "--jpeg-quality",
            os.environ.get("MINE_TELEOP_MVS_JPEG_QUALITY", "80") or "80",
        ]
    )
    return command


def _mvs_bridge_executable() -> list[str]:
    override = os.environ.get("MINE_TELEOP_MVS_BRIDGE_BIN")
    if override:
        return [override]
    executable = Path(sys.executable)
    if executable.name.startswith("mine-teleop"):
        return [str(executable), "mvs-camera-bridge"]
    return [sys.executable, "-m", "mine_teleop.mvs_camera_bridge"]


def _pylon_bridge_command(camera: CameraConfig) -> list[str]:
    command = _pylon_bridge_executable()
    selector = camera.device.split(":", 1)[1] if ":" in camera.device else "0"
    selector = selector.strip() or "0"
    if selector.startswith("index="):
        command.extend(["--device-index", selector.split("=", 1)[1]])
    elif selector.startswith("serial="):
        command.extend(["--serial", selector.split("=", 1)[1]])
    elif selector.startswith("model="):
        command.extend(["--model", selector.split("=", 1)[1]])
    elif selector.isdigit():
        command.extend(["--device-index", selector])
    else:
        command.extend(["--serial", selector])
    command.extend(
        [
            "--width",
            str(camera.capture_width),
            "--height",
            str(camera.capture_height),
            "--fps",
            str(camera.capture_fps),
            "--frames",
            "0",
        ]
    )
    return command


def _pylon_bridge_executable() -> list[str]:
    override = os.environ.get("MINE_TELEOP_PYLON_BRIDGE_BIN")
    if override:
        return [override]
    executable = Path(sys.executable)
    if executable.name.startswith("mine-teleop"):
        return [str(executable), "pylon-camera-bridge"]
    return [sys.executable, "-m", "mine_teleop.cli", "pylon-camera-bridge"]


class MjpegFrameEncoder(_PersistentFfmpegEncoder):
    codec = "mjpeg"

    def _command(self, camera: CameraConfig, *, width: int, height: int, fps: int) -> list[str]:
        if _is_mvs_camera_device(camera.device):
            return _mvs_bridge_command(camera)
        if _is_pylon_camera_device(camera.device):
            return _pylon_bridge_command(camera)
        command = [self.ffmpeg_binary, "-hide_banner", "-loglevel", "error"]
        if camera.device == "testsrc":
            command.extend(["-f", "lavfi", "-i", f"testsrc2=size={width}x{height}:rate={fps}"])
        else:
            command.extend(
                [
                    "-f",
                    "v4l2",
                    "-input_format",
                    "mjpeg",
                    "-video_size",
                    f"{camera.capture_width}x{camera.capture_height}",
                    "-framerate",
                    str(camera.capture_fps),
                    "-i",
                    camera.device,
                    "-vf",
                    f"fps={fps},scale={width}:{height}",
                ]
            )
        command.extend(["-q:v", "5", "-f", "image2pipe", "-vcodec", "mjpeg", "pipe:1"])
        return command

    def _extract_unit(self, stream: _PipedFfmpegStream) -> bytes | None:
        start = stream.buffer.find(b"\xff\xd8")
        if start < 0:
            stream.buffer = b""
            return None
        if start > 0:
            stream.buffer = stream.buffer[start:]
        end = stream.buffer.find(b"\xff\xd9", 2)
        if end < 0:
            return None
        end += 2
        frame = stream.buffer[:end]
        stream.buffer = stream.buffer[end:]
        return frame

    def _trim_buffer(self, stream: _PipedFfmpegStream) -> None:
        if len(stream.buffer) > 16 * 1024 * 1024:
            start = stream.buffer.rfind(b"\xff\xd8")
            if start <= 0:
                # A single in-progress image already exceeds the cap: fail
                # cleanly instead of truncating forever.
                raise RuntimeError("MJPEG frame exceeded 16MiB without a complete image")
            stream.buffer = stream.buffer[start:]


class FfmpegH264FrameEncoder(_PersistentFfmpegEncoder):
    """Persistent all-intra H.264 encoder.

    Every frame is an IDR (``-g 1``) with repeated SPS/PPS and an Access Unit
    Delimiter, so each emitted access unit is independently decodable by the
    driver console (which decodes one posted payload at a time). This avoids the
    old per-frame ``ffmpeg`` spawn that reopened the camera every frame (H2).
    """

    codec = "h264"
    _AUD = b"\x00\x00\x00\x01\x09"

    def _command(self, camera: CameraConfig, *, width: int, height: int, fps: int) -> list[str]:
        if _is_mvs_camera_device(camera.device) or _is_pylon_camera_device(camera.device):
            raise RuntimeError("SDK camera bridges require --frame-codec mjpeg")
        command = [self.ffmpeg_binary, "-hide_banner", "-loglevel", "error"]
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
                    f"fps={fps},scale={width}:{height}",
                ]
            )
        profile = self.config.realtime_profiles[camera.realtime_profile]
        command.extend(
            [
                "-c:v",
                "libx264",
                "-preset",
                "ultrafast",
                "-tune",
                "zerolatency",
                "-g",
                "1",
                "-bf",
                "0",
                "-x264-params",
                "repeat-headers=1:keyint=1",
                "-b:v",
                f"{profile.bitrate_kbps}k",
                "-pix_fmt",
                "yuv420p",
                "-bsf:v",
                "h264_metadata=aud=insert",
                "-f",
                "h264",
                "pipe:1",
            ]
        )
        return command

    def _extract_unit(self, stream: _PipedFfmpegStream) -> bytes | None:
        start = stream.buffer.find(self._AUD)
        if start < 0:
            # Keep only a tail that might contain a partial start code.
            if len(stream.buffer) > 4:
                stream.buffer = stream.buffer[-4:]
            return None
        nxt = stream.buffer.find(self._AUD, start + len(self._AUD))
        if nxt < 0:
            if start > 0:
                stream.buffer = stream.buffer[start:]
            return None
        unit = stream.buffer[start:nxt]
        stream.buffer = stream.buffer[nxt:]
        return unit

    def _trim_buffer(self, stream: _PipedFfmpegStream) -> None:
        if len(stream.buffer) > 32 * 1024 * 1024:
            start = stream.buffer.rfind(self._AUD)
            if start <= 0:
                raise RuntimeError("H.264 access unit exceeded 32MiB without a delimiter")
            stream.buffer = stream.buffer[start:]


class DriverConsoleFrameSink:
    """Posts encoded frames over per-sender keep-alive connections.

    Each sender thread gets its own connection, so cameras can POST in parallel
    without paying a fresh TCP handshake for every frame.
    """

    def __init__(self, driver_console_url: str, *, clock_sync_interval_ms: int = 5000) -> None:
        self.driver_console_url = driver_console_url.rstrip("/")
        parsed = urlparse(self.driver_console_url)
        if parsed.scheme not in {"http", "https"} or not parsed.hostname:
            raise ValueError("driver_console_url must be an http(s) URL")
        self._scheme = parsed.scheme
        self._host = parsed.hostname
        self._port = parsed.port or (443 if parsed.scheme == "https" else 80)
        self._base_path = parsed.path.rstrip("/")
        self._lock = threading.Lock()
        self._connections: dict[int, http.client.HTTPConnection] = {}
        self.clock_offset_ms = 0
        self.clock_sync_interval_ms = clock_sync_interval_ms
        self._last_clock_sync_ms: int | None = None

    def _new_connection(self) -> http.client.HTTPConnection:
        if self._scheme == "https":
            return http.client.HTTPSConnection(self._host, self._port, timeout=10)
        return http.client.HTTPConnection(self._host, self._port, timeout=10)

    def _request(self, method: str, path: str, body: bytes | None = None) -> dict[str, Any]:
        headers = {"Content-Type": "application/json"} if body is not None else {}
        thread_id = threading.get_ident()
        for attempt in range(2):
            conn = self._connection_for_thread(thread_id)
            try:
                conn.request(method, f"{self._base_path}{path}", body=body, headers=headers)
                response = conn.getresponse()
                data = response.read()
                if response.status < 200 or response.status >= 300:
                    raise RuntimeError(f"{method} {path} failed with status {response.status}: {data[:256]!r}")
                parsed = json.loads(data.decode("utf-8"))
                if not isinstance(parsed, dict):
                    raise RuntimeError("driver console response must be a JSON object")
                return parsed
            except (http.client.HTTPException, ConnectionError, OSError):
                # Stale/broken keep-alive socket: drop only this sender's
                # connection and retry once.
                self._close_thread_connection(thread_id)
                if attempt == 1:
                    raise
        raise RuntimeError("unreachable")

    def _connection_for_thread(self, thread_id: int) -> http.client.HTTPConnection:
        with self._lock:
            conn = self._connections.get(thread_id)
            if conn is None:
                conn = self._new_connection()
                self._connections[thread_id] = conn
            return conn

    def _maybe_sync_clock(self) -> None:
        now = _now_ms()
        if self._last_clock_sync_ms is not None and now - self._last_clock_sync_ms < self.clock_sync_interval_ms:
            return
        try:
            t0 = _now_ms()
            payload = self._request("GET", "/api/time")
            t3 = _now_ms()
        except Exception:
            return
        console_now = payload.get("now_ms")
        if isinstance(console_now, bool) or not isinstance(console_now, int):
            return
        # NTP-style single-sample offset to add to vehicle timestamps to express
        # them in the console clock domain.
        self.clock_offset_ms = console_now - (t0 + t3) // 2
        self._last_clock_sync_ms = now

    def send_frame(self, frame: EncodedFrame, *, sent_at_ms: int) -> dict[str, Any]:
        self._maybe_sync_clock()
        payload = frame.to_post_payload(sent_at_ms=sent_at_ms)
        payload["clock_offset_ms"] = self.clock_offset_ms
        return self._request("POST", "/api/media/frame", json.dumps(payload).encode("utf-8"))

    def _close_thread_connection(self, thread_id: int) -> None:
        with self._lock:
            conn = self._connections.pop(thread_id, None)
        if conn is not None:
            try:
                conn.close()
            except Exception:
                pass

    def close(self) -> None:
        with self._lock:
            connections = list(self._connections.values())
            self._connections.clear()
        for conn in connections:
            try:
                conn.close()
            except Exception:
                pass


class _LatestFrameMailbox:
    """Single-slot mailbox that always holds the freshest frame; stale frames
    are dropped so a slow network never backs up capture (real-time teleop wants
    the newest frame, not every frame)."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._latest: EncodedFrame | None = None
        self.published = 0
        self.dropped = 0

    def publish(self, frame: EncodedFrame) -> None:
        with self._lock:
            if self._latest is not None:
                self.dropped += 1
            self._latest = frame
            self.published += 1

    def pop(self) -> EncodedFrame | None:
        with self._lock:
            frame = self._latest
            self._latest = None
            return frame


class _RealtimeFrameQueue:
    """Bounded FIFO for real-time frames.

    It preserves send order for fresh frames while dropping the oldest frames
    when the network falls behind, avoiding unbounded latency growth.
    """

    def __init__(self, *, max_frames: int = 3) -> None:
        if isinstance(max_frames, bool) or max_frames <= 0:
            raise ValueError("max_frames must be positive")
        self._lock = threading.Lock()
        self._frames: collections.deque[EncodedFrame] = collections.deque()
        self.max_frames = max_frames
        self.published = 0
        self.dropped = 0

    def publish(self, frame: EncodedFrame) -> None:
        with self._lock:
            while len(self._frames) >= self.max_frames:
                self._frames.popleft()
                self.dropped += 1
            self._frames.append(frame)
            self.published += 1

    def pop(self) -> EncodedFrame | None:
        with self._lock:
            if not self._frames:
                return None
            return self._frames.popleft()


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
        self._uploader: VehicleRecorderUploader | None = None

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
                    {"camera_id": camera.camera_id, "seq": seq, "sent_at_ms": sent_at_ms, "response": response}
                )
            if seq < frame_count and frame_interval_ms > 0:
                sleeper(frame_interval_ms / 1000.0)
        return self._summary("vehicle_media_teleop_summary", cameras, sent_results, dropped=0)

    def stream_frames(
        self,
        *,
        duration_ms: int | None = None,
        frame_count: int | None = None,
        capture_interval_ms: int = 0,
        frame_queue_depth: int | None = None,
        now_fn: Any = None,
        sleep_fn: Any = None,
    ) -> dict[str, Any]:
        """Capture and send concurrently per camera.

        Each camera has its own capture producer and sender consumer connected
        by a single-slot mailbox, so slow capture/encode or network work on one
        camera does not block the other cameras. Stale frames are still dropped.
        """
        if duration_ms is None and frame_count is None:
            raise ValueError("stream_frames requires duration_ms or frame_count")
        if duration_ms is not None and (isinstance(duration_ms, bool) or duration_ms < 0):
            raise ValueError("duration_ms must be non-negative")
        if frame_count is not None and (isinstance(frame_count, bool) or frame_count <= 0):
            raise ValueError("frame_count must be positive")
        cameras = list(self.config.enabled_cameras)
        if not cameras:
            raise RuntimeError("vehicle media runtime requires at least one enabled camera")
        if frame_queue_depth is None:
            frame_queue_depth = _frame_queue_depth_from_env()
        if isinstance(frame_queue_depth, bool) or frame_queue_depth <= 0:
            raise ValueError("frame_queue_depth must be positive")
        clock = now_fn or _now_ms
        sleeper = sleep_fn or time.sleep
        mailboxes = {camera.camera_id: _RealtimeFrameQueue(max_frames=frame_queue_depth) for camera in cameras}
        sent_results: list[dict[str, Any]] = []
        sent_lock = threading.Lock()
        capture_errors: list[dict[str, str]] = []
        capture_errors_lock = threading.Lock()
        stop_event = threading.Event()
        start_ms = int(clock())

        def capture(camera: CameraConfig) -> None:
            seq = 0
            while not stop_event.is_set():
                if frame_count is not None and seq >= frame_count:
                    return
                if duration_ms is not None and int(clock()) - start_ms >= duration_ms:
                    return
                seq += 1
                try:
                    frame = self.encoder.encode(camera, seq=seq, now_ms=int(clock()))
                except Exception as exc:
                    with capture_errors_lock:
                        capture_errors.append({"camera_id": camera.camera_id, "error": str(exc)})
                    stop_event.set()
                    return
                self.last_frame = frame
                mailboxes[camera.camera_id].publish(frame)
                if capture_interval_ms > 0:
                    sleeper(capture_interval_ms / 1000.0)

        def sender(camera: CameraConfig) -> None:
            mailbox = mailboxes[camera.camera_id]
            while True:
                frame = mailbox.pop()
                if frame is None:
                    if stop_event.is_set():
                        return
                    sleeper(0.002)
                    continue
                sent_at_ms = int(clock())
                try:
                    response = self.frame_sink.send_frame(frame, sent_at_ms=sent_at_ms)
                except Exception as exc:  # keep streaming; report per-frame errors
                    response = {"error": str(exc)}
                with sent_lock:
                    sent_results.append(
                        {"camera_id": camera.camera_id, "seq": frame.seq, "sent_at_ms": sent_at_ms, "response": response}
                    )

        sender_threads = [threading.Thread(target=sender, args=(camera,), daemon=True) for camera in cameras]
        capture_threads = [threading.Thread(target=capture, args=(camera,), daemon=True) for camera in cameras]
        for thread in sender_threads:
            thread.start()
        for thread in capture_threads:
            thread.start()
        try:
            for thread in capture_threads:
                thread.join()
        finally:
            stop_event.set()
            for thread in sender_threads:
                thread.join(timeout=15)
        captured = sum(box.published for box in mailboxes.values())
        dropped = sum(box.dropped for box in mailboxes.values())
        summary = self._summary("vehicle_media_stream_summary", cameras, sent_results, dropped=dropped)
        summary["captured_frames"] = captured
        summary["capture_errors"] = len(capture_errors)
        summary["frame_queue_depth"] = frame_queue_depth
        if capture_errors:
            summary["capture_error_details"] = capture_errors
            summary["passed"] = False
        elapsed_ms = max(1, int(clock()) - start_ms)
        summary["duration_ms"] = elapsed_ms
        summary["achieved_fps"] = round(len(sent_results) * 1000.0 / elapsed_ms, 2)
        return summary

    def _summary(
        self, event: str, cameras: list[CameraConfig], sent_results: list[dict[str, Any]], *, dropped: int
    ) -> dict[str, Any]:
        latencies = [
            int(result["response"]["end_to_end_latency_ms"])
            for result in sent_results
            if isinstance(result.get("response"), dict)
            and isinstance(result["response"].get("end_to_end_latency_ms"), int)
            and not isinstance(result["response"]["end_to_end_latency_ms"], bool)
        ]
        errors = sum(1 for result in sent_results if isinstance(result.get("response"), dict) and "error" in result["response"])
        return {
            "event": event,
            "passed": bool(sent_results) and errors == 0,
            "vehicle_id": self.config.vehicle_id,
            "camera_ids": [camera.camera_id for camera in cameras],
            "sent_frames": len(sent_results),
            "dropped_frames": dropped,
            "send_errors": errors,
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
        if self._uploader is None:
            self._uploader = VehicleRecorderUploader.from_config(
                self.config,
                recording_root=recording_root,
                queue_state_path=queue_state_path,
                archive_root=archive_root,
                upload_api=upload_api,
            )
            self._uploader.upload_trigger_policy = UploadTriggerPolicy(trigger_segments=1)
        uploader = self._uploader
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

    def close(self) -> None:
        close = getattr(self.encoder, "close", None)
        if callable(close):
            close()
        close_sink = getattr(self.frame_sink, "close", None)
        if callable(close_sink):
            close_sink()


def _average(values: list[int]) -> float:
    if not values:
        return 0.0
    return sum(values) / len(values)


def _now_ms() -> int:
    return int(time.time() * 1000)


def _frame_queue_depth_from_env() -> int:
    raw = os.environ.get("MINE_TELEOP_FRAME_QUEUE_DEPTH", "3")
    try:
        value = int(raw)
    except ValueError:
        raise ValueError("MINE_TELEOP_FRAME_QUEUE_DEPTH must be an integer") from None
    if value <= 0:
        raise ValueError("MINE_TELEOP_FRAME_QUEUE_DEPTH must be positive")
    return value


def _iso_ms(value_ms: int) -> str:
    return datetime.fromtimestamp(value_ms / 1000.0, tz=timezone.utc).isoformat()


def _utc_segment_prefix(value_ms: int) -> str:
    return datetime.fromtimestamp(value_ms / 1000.0, tz=timezone.utc).strftime("%Y%m%dT%H%M%SZ")
