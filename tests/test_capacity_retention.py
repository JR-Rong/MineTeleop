import json
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path

from mine_teleop.capacity import DiskWatermarkPolicy
from mine_teleop.config import load_vehicle_config


class CapacityPlanningTests(unittest.TestCase):
    def test_dev_config_calculates_hourly_growth_and_required_retention(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))

        self.assertAlmostEqual(config.capacity.recording_mbps, 8.0)
        self.assertAlmostEqual(config.capacity.recording_gb_per_hour, 3.6)
        self.assertAlmostEqual(config.capacity.upload_gb_per_hour, 2.25)
        self.assertAlmostEqual(config.capacity.net_growth_gb_per_hour, 1.35)
        self.assertAlmostEqual(config.capacity.required_retention_gb, 28.8)
        self.assertEqual(config.capacity.status, "upload_lag_policy_required_and_configured")
        self.assertEqual(config.recording.delete_uploaded_when_below_free_gb, 2.0)
        self.assertFalse(config.recording.delete_unuploaded_when_below_free_gb)

    def test_four_camera_plan_matches_documented_fourteen_gb_per_hour_rule(self):
        config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
        template = config.cameras[0]
        cameras = [replace(template, camera_id=f"cam-{index}", enabled=True) for index in range(4)]

        plan = config.capacity.recalculate(cameras, config.record_profiles, config.recording, config.upload)

        self.assertAlmostEqual(plan.recording_mbps, 32.0)
        self.assertAlmostEqual(plan.recording_gb_per_hour, 14.4)


class DiskWatermarkPolicyTests(unittest.TestCase):
    def test_deletes_only_uploaded_segments_until_soft_watermark_is_recovered(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            uploaded_old = _segment(
                root,
                "old-uploaded",
                upload_state="uploaded",
                size=120,
                started_at="2026-06-24T10:00:00Z",
            )
            uploaded_new = _segment(
                root,
                "new-uploaded",
                upload_state="uploaded",
                size=120,
                started_at="2026-06-24T10:01:00Z",
            )
            pending = _segment(
                root,
                "pending",
                upload_state="pending",
                size=500,
                started_at="2026-06-24T10:02:00Z",
            )
            policy = DiskWatermarkPolicy(
                root_dir=root,
                min_free_bytes=1_000,
                delete_uploaded_when_below_free_bytes=800,
                delete_unuploaded_when_below_free_bytes=False,
            )

            result = policy.enforce(current_free_bytes=100)

            self.assertEqual(result.action, "deleted_uploaded_segments")
            self.assertEqual(result.deleted_segment_ids, ["old-uploaded", "new-uploaded"])
            self.assertFalse(uploaded_old.video_path.exists())
            self.assertFalse(uploaded_new.metadata_path.exists())
            self.assertTrue(pending.video_path.exists())
            self.assertGreaterEqual(result.projected_free_bytes, 800)

    def test_pauses_recording_and_alerts_when_uploaded_deletion_cannot_recover_watermark(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pending = _segment(
                root,
                "pending",
                upload_state="pending",
                size=500,
                started_at="2026-06-24T10:00:00Z",
            )
            policy = DiskWatermarkPolicy(
                root_dir=root,
                min_free_bytes=1_000,
                delete_uploaded_when_below_free_bytes=800,
                delete_unuploaded_when_below_free_bytes=False,
            )

            result = policy.enforce(current_free_bytes=400)

            self.assertEqual(result.action, "pause_recording_and_alert")
            self.assertEqual(result.deleted_segment_ids, [])
            self.assertTrue(pending.video_path.exists())
            self.assertIn("unuploaded segments preserved", result.reason)

    def test_explicit_unuploaded_deletion_policy_reports_destructive_action(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            pending = _segment(
                root,
                "pending",
                upload_state="pending",
                size=900,
                started_at="2026-06-24T10:00:00Z",
            )
            policy = DiskWatermarkPolicy(
                root_dir=root,
                min_free_bytes=1_000,
                delete_uploaded_when_below_free_bytes=800,
                delete_unuploaded_when_below_free_bytes=True,
            )

            result = policy.enforce(current_free_bytes=100)

            self.assertEqual(result.action, "deleted_unuploaded_segments")
            self.assertEqual(result.deleted_segment_ids, ["pending"])
            self.assertFalse(pending.video_path.exists())
            self.assertFalse(pending.metadata_path.exists())
            self.assertIn("explicit unuploaded deletion policy", result.reason)

    def test_missing_video_file_is_not_counted_as_reclaimed_space(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            uploaded = _segment(
                root,
                "metadata-only",
                upload_state="uploaded",
                size=10_000,
                started_at="2026-06-24T10:00:00Z",
            )
            uploaded.video_path.unlink()
            policy = DiskWatermarkPolicy(
                root_dir=root,
                min_free_bytes=1_000,
                delete_uploaded_when_below_free_bytes=800,
                delete_unuploaded_when_below_free_bytes=False,
            )

            result = policy.enforce(current_free_bytes=100)

            self.assertEqual(result.action, "pause_recording_and_alert")
            self.assertLess(result.projected_free_bytes, 800)


class _Segment:
    def __init__(self, video_path, metadata_path):
        self.video_path = video_path
        self.metadata_path = metadata_path


def _segment(root, segment_id, upload_state, size, started_at):
    segment_dir = root / "vehicle-001" / "session-001" / "front"
    segment_dir.mkdir(parents=True, exist_ok=True)
    video_path = segment_dir / f"{segment_id}.mp4"
    metadata_path = segment_dir / f"{segment_id}.json"
    video_path.write_bytes(b"x" * size)
    metadata_path.write_text(
        json.dumps(
            {
                "vehicle_id": "vehicle-001",
                "session_id": "session-001",
                "camera_id": "front",
                "segment_id": segment_id,
                "started_at": started_at,
                "ended_at": started_at.replace(":00Z", ":30Z"),
                "codec": "h264",
                "encoder": "vaapi",
                "width": 1920,
                "height": 1080,
                "fps": 30,
                "file_size_bytes": size,
                "upload_state": upload_state,
            },
            sort_keys=True,
        ),
        encoding="utf-8",
    )
    return _Segment(video_path, metadata_path)


if __name__ == "__main__":
    unittest.main()
