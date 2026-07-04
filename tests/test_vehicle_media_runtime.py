from __future__ import annotations

import json
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
from mine_teleop.vehicle_media_runtime import EncodedFrame, VehicleMediaRuntime


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


if __name__ == "__main__":
    unittest.main()
