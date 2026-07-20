from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import unittest
from dataclasses import dataclass, replace
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib import request

from mine_teleop.config import load_driver_config, load_vehicle_config
from mine_teleop.driver_console_runtime import DriverConsoleHttpApp, DriverConsoleRuntime, RecordingControlCommandSink
from mine_teleop.signaling_service import SignalingHttpService
from mine_teleop.vehicle_media_runtime import (
    DriverConsoleFrameSink,
    EncodedFrame,
    MjpegFrameEncoder,
    VehicleMediaRuntime,
    _LatestFrameMailbox,
    _RealtimeFrameQueue,
)


def _json_get(url: str) -> dict:
    with request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


@dataclass
class _FakeEncoder:
    calls: list[tuple[str, int, int]]

    def encode(self, camera, *, seq: int, now_ms: int) -> EncodedFrame:
        self.calls.append((camera.camera_id, seq, now_ms))
        return EncodedFrame(
            camera_id=camera.camera_id,
            seq=seq,
            codec="h264",
            payload=b"fake-h264-frame",
            captured_at_ms=now_ms,
            encoded_at_ms=now_ms + 5,
            width=320,
            height=180,
            fps=15,
            bitrate_kbps=800,
        )


@dataclass
class _FakeSink:
    posted: list[dict]

    def send_frame(self, frame: EncodedFrame, *, sent_at_ms: int) -> dict:
        payload = frame.to_post_payload(sent_at_ms=sent_at_ms)
        self.posted.append(payload)
        return {
            "frame_received": True,
            "frame_sequence": len(self.posted),
            "end_to_end_latency_ms": 12,
            "transport_latency_ms": 4,
            "decode_latency_ms": 3,
        }


class _BarrierEncoder:
    def __init__(self, parties: int) -> None:
        self.barrier = threading.Barrier(parties, timeout=2)
        self.calls: list[str] = []
        self._lock = threading.Lock()

    def encode(self, camera, *, seq: int, now_ms: int) -> EncodedFrame:
        with self._lock:
            self.calls.append(camera.camera_id)
        self.barrier.wait()
        return EncodedFrame(
            camera_id=camera.camera_id,
            seq=seq,
            codec="h264",
            payload=b"fake-h264-frame",
            captured_at_ms=now_ms,
            encoded_at_ms=now_ms + 5,
            width=320,
            height=180,
            fps=15,
            bitrate_kbps=800,
        )


class _BarrierSink:
    def __init__(self, parties: int) -> None:
        self.barrier = threading.Barrier(parties, timeout=2)
        self.posted: list[dict] = []
        self._lock = threading.Lock()

    def send_frame(self, frame: EncodedFrame, *, sent_at_ms: int) -> dict:
        payload = frame.to_post_payload(sent_at_ms=sent_at_ms)
        with self._lock:
            self.posted.append(payload)
        self.barrier.wait()
        return {
            "frame_received": True,
            "frame_sequence": len(self.posted),
            "end_to_end_latency_ms": 12,
            "transport_latency_ms": 4,
            "decode_latency_ms": 3,
        }


class VehicleMediaRuntimeTests(unittest.TestCase):
    def test_send_frames_posts_encoded_camera_frames_with_timing_metadata(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        encoder = _FakeEncoder(calls=[])
        sink = _FakeSink(posted=[])
        clock_values = iter([1_000, 1_010, 1_050, 1_060])
        runtime = VehicleMediaRuntime(config, frame_sink=sink, encoder=encoder)

        summary = runtime.send_frames(frame_count=2, now_fn=lambda: next(clock_values), sleep_fn=lambda _seconds: None)

        self.assertTrue(summary["passed"])
        self.assertEqual(summary["sent_frames"], 2)
        self.assertEqual(summary["camera_ids"], ["front"])
        self.assertEqual(summary["latency"]["end_to_end_latency_ms_avg"], 12.0)
        self.assertEqual(encoder.calls, [("front", 1, 1000), ("front", 2, 1050)])
        self.assertEqual(len(sink.posted), 2)
        self.assertEqual(sink.posted[0]["camera_id"], "front")
        self.assertEqual(sink.posted[0]["codec"], "h264")
        self.assertEqual(sink.posted[0]["captured_at_ms"], 1000)
        self.assertEqual(sink.posted[0]["encoded_at_ms"], 1005)
        self.assertEqual(sink.posted[0]["sent_at_ms"], 1010)
        self.assertTrue(sink.posted[0]["payload_base64"])

    def test_mjpeg_frame_encoder_reuses_one_ffmpeg_process_per_camera(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            launches = root / "launches.txt"
            fake_ffmpeg = root / "fake-ffmpeg.py"
            fake_ffmpeg.write_text(
                "\n".join(
                    [
                        "#!/usr/bin/env python3",
                        "import os, sys, time",
                        "with open(os.environ['MINE_TELEOP_FAKE_FFMPEG_LAUNCHES'], 'a', encoding='utf-8') as handle:",
                        "    handle.write('launch\\n')",
                        "sys.stdout.buffer.write(b'\\xff\\xd8first\\xff\\xd9')",
                        "sys.stdout.buffer.flush()",
                        "sys.stdout.buffer.write(b'\\xff\\xd8second\\xff\\xd9')",
                        "sys.stdout.buffer.flush()",
                        "time.sleep(2)",
                    ]
                ),
                encoding="utf-8",
            )
            fake_ffmpeg.chmod(0o755)
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            camera = config.enabled_cameras[0]
            old_env = os.environ.get("MINE_TELEOP_FAKE_FFMPEG_LAUNCHES")
            os.environ["MINE_TELEOP_FAKE_FFMPEG_LAUNCHES"] = str(launches)
            encoder = MjpegFrameEncoder(config, ffmpeg_binary=str(fake_ffmpeg), frame_timeout_seconds=1.0)
            try:
                first = encoder.encode(camera, seq=1, now_ms=1000)
                second = encoder.encode(camera, seq=2, now_ms=1100)
                launches_lines = launches.read_text(encoding="utf-8").splitlines()
            finally:
                encoder.close()
                if old_env is None:
                    os.environ.pop("MINE_TELEOP_FAKE_FFMPEG_LAUNCHES", None)
                else:
                    os.environ["MINE_TELEOP_FAKE_FFMPEG_LAUNCHES"] = old_env

        self.assertEqual(first.codec, "mjpeg")
        self.assertEqual(second.codec, "mjpeg")
        self.assertEqual(first.payload, b"\xff\xd8first\xff\xd9")
        self.assertEqual(second.payload, b"\xff\xd8second\xff\xd9")
        self.assertEqual(launches_lines, ["launch"])

    def test_mjpeg_frame_encoder_uses_mvs_bridge_for_hikrobot_camera(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        camera = replace(
            config.enabled_cameras[0],
            camera_id="hikrobot",
            device="mvs:0",
            capture_width=1280,
            capture_height=1024,
            capture_fps=15,
        )
        encoder = MjpegFrameEncoder(config, ffmpeg_binary="/opt/mine-teleop/bin/ffmpeg")

        command = encoder._command(camera, width=1280, height=720, fps=15)

        self.assertNotIn("v4l2", command)
        self.assertIn("mine_teleop.mvs_camera_bridge", command)
        self.assertIn("--device-index", command)
        self.assertIn("0", command)
        self.assertIn("--width", command)
        self.assertIn("1280", command)
        self.assertIn("--height", command)
        self.assertIn("1024", command)
        self.assertIn("--fps", command)
        self.assertIn("15", command)

    def test_mjpeg_frame_encoder_passes_configured_mvs_jpeg_quality(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        camera = replace(
            config.enabled_cameras[0],
            camera_id="hikrobot",
            device="mvs:0",
            capture_width=640,
            capture_height=512,
            capture_fps=15,
        )
        old_quality = os.environ.get("MINE_TELEOP_MVS_JPEG_QUALITY")
        os.environ["MINE_TELEOP_MVS_JPEG_QUALITY"] = "65"
        encoder = MjpegFrameEncoder(config, ffmpeg_binary="/opt/mine-teleop/bin/ffmpeg")
        try:
            command = encoder._command(camera, width=854, height=480, fps=15)
        finally:
            if old_quality is None:
                os.environ.pop("MINE_TELEOP_MVS_JPEG_QUALITY", None)
            else:
                os.environ["MINE_TELEOP_MVS_JPEG_QUALITY"] = old_quality

        self.assertIn("--jpeg-quality", command)
        quality_index = command.index("--jpeg-quality")
        self.assertEqual(command[quality_index + 1], "65")

    def test_mjpeg_frame_encoder_uses_pylon_bridge_for_basler_camera(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        camera = replace(
            config.enabled_cameras[0],
            camera_id="basler",
            device="pylon:0",
            capture_width=1280,
            capture_height=1024,
            capture_fps=15,
        )
        encoder = MjpegFrameEncoder(config, ffmpeg_binary="/opt/mine-teleop/bin/ffmpeg")

        command = encoder._command(camera, width=1280, height=720, fps=15)

        self.assertNotIn("v4l2", command)
        self.assertIn("pylon-camera-bridge", command)
        self.assertIn("--device-index", command)
        self.assertIn("0", command)
        self.assertIn("--width", command)
        self.assertIn("1280", command)
        self.assertIn("--height", command)
        self.assertIn("1024", command)
        self.assertIn("--fps", command)
        self.assertIn("15", command)


class VehicleMediaRuntimeCliTests(unittest.TestCase):
    def test_vehicle_media_agent_teleop_posts_one_frame_to_driver_console(self):
        with tempfile.TemporaryDirectory() as tmp:
            frame_dir = Path(tmp) / "frames"
            driver_config = load_driver_config(Path("configs/driver-console.dev.yaml"))
            runtime = DriverConsoleRuntime(
                driver_config,
                signaling_http_url="http://127.0.0.1:8765",
                vehicle_id="vehicle-001",
                password="dev-password",
                control_sink=RecordingControlCommandSink(),
                frame_dir=frame_dir,
            )
            app = DriverConsoleHttpApp(runtime)
            with app.running("127.0.0.1", 0) as console_url:
                result = subprocess.run(
                    [
                        sys.executable,
                        "vehicle-media-agent/vehicle_media_agent.py",
                        "--config",
                        "configs/vehicle-agent.dev.yaml",
                        "--mode",
                        "teleop",
                        "--driver-console-url",
                        console_url,
                        "--frames",
                        "1",
                        "--frame-interval-ms",
                        "1",
                        "--json",
                    ],
                    cwd=Path.cwd(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=60,
                )
                status = _json_get(f"{console_url}/api/status")

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(records[-1]["event"], "vehicle_media_teleop_summary")
        self.assertTrue(records[-1]["passed"])
        self.assertEqual(records[-1]["sent_frames"], 1)
        self.assertEqual(status["decoded_frame_count_by_camera"]["front"], 1)
        self.assertGreaterEqual(status["dashboard"]["cameras"]["front"]["latency_ms"], 0)

    def test_vehicle_media_agent_records_and_uploads_one_encoded_segment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            driver_config = load_driver_config(Path("configs/driver-console.dev.yaml"))
            driver_runtime = DriverConsoleRuntime(
                driver_config,
                signaling_http_url="http://127.0.0.1:8765",
                vehicle_id="vehicle-001",
                password="dev-password",
                control_sink=RecordingControlCommandSink(),
                frame_dir=root / "frames",
            )
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as upload_base_url:
                with DriverConsoleHttpApp(driver_runtime).running("127.0.0.1", 0) as console_url:
                    result = subprocess.run(
                        [
                            sys.executable,
                            "vehicle-media-agent/vehicle_media_agent.py",
                            "--config",
                            "configs/vehicle-agent.dev.yaml",
                            "--mode",
                            "teleop",
                            "--driver-console-url",
                            console_url,
                            "--frames",
                            "1",
                            "--frame-interval-ms",
                            "1",
                            "--record-upload-once",
                            "--recording-root",
                            str(root / "recordings"),
                            "--uploader-work-dir",
                            str(root / "uploader"),
                            "--upload-api-base-url",
                            upload_base_url,
                            "--json",
                        ],
                        cwd=Path.cwd(),
                        text=True,
                        stdout=subprocess.PIPE,
                        stderr=subprocess.PIPE,
                        timeout=60,
                    )

            self.assertEqual(result.returncode, 0, result.stderr)
            records = [json.loads(line) for line in result.stdout.splitlines()]
            summary = records[-1]
            self.assertTrue(summary["passed"])
            self.assertEqual(summary["recording_upload"]["action"], "uploaded")
            self.assertEqual(len(list((root / "recordings").rglob("*.json"))), 1)
            self.assertTrue((root / "uploader" / "archive").exists())


def _frame(seq: int) -> EncodedFrame:
    return EncodedFrame(
        camera_id="front",
        seq=seq,
        codec="mjpeg",
        payload=b"\xff\xd8frame\xff\xd9",
        captured_at_ms=1000 + seq,
        encoded_at_ms=1001 + seq,
        width=320,
        height=180,
        fps=15,
        bitrate_kbps=800,
    )


class LatestFrameMailboxTests(unittest.TestCase):
    def test_mailbox_keeps_only_the_freshest_frame_and_counts_drops(self):
        mailbox = _LatestFrameMailbox()
        mailbox.publish(_frame(1))
        mailbox.publish(_frame(2))
        mailbox.publish(_frame(3))
        self.assertEqual(mailbox.published, 3)
        self.assertEqual(mailbox.dropped, 2)
        popped = mailbox.pop()
        self.assertIsNotNone(popped)
        self.assertEqual(popped.seq, 3)
        self.assertIsNone(mailbox.pop())


class RealtimeFrameQueueTests(unittest.TestCase):
    def test_queue_drops_oldest_frame_but_preserves_send_order_for_fresh_frames(self):
        queue = _RealtimeFrameQueue(max_frames=3)

        for seq in range(1, 6):
            queue.publish(_frame(seq))

        self.assertEqual(queue.published, 5)
        self.assertEqual(queue.dropped, 2)
        self.assertEqual([queue.pop().seq, queue.pop().seq, queue.pop().seq], [3, 4, 5])
        self.assertIsNone(queue.pop())


class StreamFramesTests(unittest.TestCase):
    def test_stream_frames_sends_and_always_delivers_the_latest_frame(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        encoder = _FakeEncoder(calls=[])
        sink = _FakeSink(posted=[])
        runtime = VehicleMediaRuntime(config, frame_sink=sink, encoder=encoder)

        summary = runtime.stream_frames(frame_count=5, capture_interval_ms=0, sleep_fn=lambda _s: None)

        self.assertEqual(summary["event"], "vehicle_media_stream_summary")
        self.assertEqual(summary["captured_frames"], 5)
        self.assertEqual(summary["sent_frames"] + summary["dropped_frames"], 5)
        self.assertGreaterEqual(summary["sent_frames"], 1)
        self.assertEqual(summary["send_errors"], 0)
        # The freshest (last) captured frame must always be delivered.
        self.assertEqual(sink.posted[-1]["seq"], 5)

    def test_stream_frames_captures_each_camera_in_parallel(self):
        config = _two_camera_config()
        encoder = _BarrierEncoder(parties=2)
        sink = _FakeSink(posted=[])
        runtime = VehicleMediaRuntime(config, frame_sink=sink, encoder=encoder)

        summary = runtime.stream_frames(frame_count=1, capture_interval_ms=0)

        self.assertTrue(summary["passed"])
        self.assertEqual(summary["captured_frames"], 2)
        self.assertEqual(summary["sent_frames"], 2)
        self.assertEqual(summary["capture_errors"], 0)
        self.assertCountEqual(encoder.calls, ["front", "rear"])

    def test_stream_frames_sends_each_camera_in_parallel(self):
        config = _two_camera_config()
        encoder = _FakeEncoder(calls=[])
        sink = _BarrierSink(parties=2)
        runtime = VehicleMediaRuntime(config, frame_sink=sink, encoder=encoder)

        summary = runtime.stream_frames(frame_count=1, capture_interval_ms=0)

        self.assertTrue(summary["passed"])
        self.assertEqual(summary["sent_frames"], 2)
        self.assertEqual(summary["capture_errors"], 0)
        self.assertCountEqual([item["camera_id"] for item in sink.posted], ["front", "rear"])


class DriverConsoleFrameSinkTests(unittest.TestCase):
    def test_sink_reuses_connection_and_applies_clock_offset(self):
        with tempfile.TemporaryDirectory() as tmp:
            driver_config = load_driver_config(Path("configs/driver-console.dev.yaml"))
            runtime = DriverConsoleRuntime(
                driver_config,
                signaling_http_url="http://127.0.0.1:8765",
                vehicle_id="vehicle-001",
                password="dev-password",
                control_sink=RecordingControlCommandSink(),
                frame_dir=Path(tmp) / "frames",
            )
            with DriverConsoleHttpApp(runtime).running("127.0.0.1", 0) as console_url:
                sink = DriverConsoleFrameSink(console_url)
                try:
                    responses = [sink.send_frame(_frame(seq), sent_at_ms=2000 + seq) for seq in (1, 2, 3)]
                finally:
                    sink.close()
                status = _json_get(f"{console_url}/api/status")

        self.assertEqual(len(responses), 3)
        for response in responses:
            self.assertTrue(response["frame_received"])
            self.assertGreaterEqual(response["end_to_end_latency_ms"], 0)
        self.assertIsInstance(sink.clock_offset_ms, int)
        self.assertIsNotNone(sink._last_clock_sync_ms)
        self.assertEqual(status["decoded_frame_count_by_camera"]["front"], 3)

    def test_sink_allows_parallel_sender_threads_with_keepalive_per_thread(self):
        barrier = threading.Barrier(2, timeout=2)
        posted: list[str] = []
        posted_lock = threading.Lock()

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, _format, *args):
                return

            def do_GET(self):
                if self.path != "/api/time":
                    self.send_error(404)
                    return
                self._send_json({"now_ms": 2000})

            def do_POST(self):
                if self.path != "/api/media/frame":
                    self.send_error(404)
                    return
                length = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(length).decode("utf-8"))
                with posted_lock:
                    posted.append(payload["camera_id"])
                barrier.wait()
                self._send_json(
                    {
                        "frame_received": True,
                        "frame_sequence": len(posted),
                        "end_to_end_latency_ms": 12,
                        "transport_latency_ms": 4,
                        "decode_latency_ms": 3,
                    }
                )

            def _send_json(self, payload: dict) -> None:
                data = json.dumps(payload).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        sink = DriverConsoleFrameSink(f"http://127.0.0.1:{server.server_port}")
        responses: list[dict] = []
        errors: list[BaseException] = []

        def post(frame: EncodedFrame) -> None:
            try:
                responses.append(sink.send_frame(frame, sent_at_ms=3000))
            except BaseException as exc:
                errors.append(exc)

        front = _frame(1)
        rear = replace(_frame(1), camera_id="rear")
        senders = [threading.Thread(target=post, args=(frame,)) for frame in (front, rear)]
        try:
            for sender in senders:
                sender.start()
            for sender in senders:
                sender.join(timeout=5)
        finally:
            sink.close()
            server.shutdown()
            server.server_close()
            thread.join(timeout=2)

        self.assertFalse(any(sender.is_alive() for sender in senders))
        self.assertEqual(errors, [])
        self.assertEqual(len(responses), 2)
        self.assertCountEqual(posted, ["front", "rear"])


def _two_camera_config():
    config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
    cameras = [
        replace(config.cameras[0], camera_id="front", enabled=True),
        replace(config.cameras[1], camera_id="rear", enabled=True),
    ]
    return replace(config, cameras=cameras)


if __name__ == "__main__":
    unittest.main()
