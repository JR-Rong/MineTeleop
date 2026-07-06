from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import unittest
from dataclasses import dataclass
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


if __name__ == "__main__":
    unittest.main()

