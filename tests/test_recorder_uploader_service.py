import hashlib
import json
import subprocess
import sys
import tempfile
import threading
import unittest
from contextlib import contextmanager
from dataclasses import replace
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

from mine_teleop.control import ControlCommand
from mine_teleop.config import load_vehicle_config
from mine_teleop.recording import SegmentMetadata
from mine_teleop.safety import SafetyState
from mine_teleop.signaling_service import SignalingHttpService
from mine_teleop.upload import UploadCredentialService, UploadTriggerPolicy
from mine_teleop.vehicle_adapter import MockVehicleAdapter
from mine_teleop.vehicle_control_service import VehicleControlService
from mine_teleop.vehicle_recorder_uploader import UploadApiClient, VehicleRecorderUploader


class VehicleRecorderUploaderTests(unittest.TestCase):
    def test_records_segment_uploads_to_archive_and_marks_queue_uploaded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                metadata = _metadata(segment_id="20260624T101500Z_front_000001")

                segment = recorder.record_segment(metadata, payload=b"fake-h264")
                result = recorder.process_once(now_ms=1_000)

                self.assertTrue(segment.video_path.exists())
                self.assertTrue(segment.metadata_path.exists())
                self.assertEqual(result.action, "uploaded")
                self.assertEqual(
                    recorder.queue.items[0].metadata_object_path,
                    "vehicles/vehicle-001/sessions/session-001/metadata/20260624T101500Z_front_000001.json",
                )
                self.assertEqual(recorder.queue.items[0].status, "uploaded")
                self.assertTrue((root / "archive" / result.object_path).exists())
                self.assertEqual(
                    result.metadata_object_path,
                    "vehicles/vehicle-001/sessions/session-001/metadata/20260624T101500Z_front_000001.json",
                )
                self.assertTrue((root / "archive" / result.metadata_object_path).exists())

            audit_events = [json.loads(line)["event"] for line in (root / "audit.jsonl").read_text().splitlines()]
            self.assertEqual(audit_events.count("upload_credential_issued"), 2)
            self.assertEqual(audit_events.count("upload_success"), 2)

    def test_recorded_segment_persists_upload_integrity_checksums(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                payload = b"fake-h264-checksum"
                segment = recorder.record_segment(
                    _metadata(segment_id="seg-integrity"),
                    payload=payload,
                )

                sidecar = json.loads(segment.metadata_path.read_text(encoding="utf-8"))
                queue_payload = json.loads((root / "upload-queue.json").read_text(encoding="utf-8"))
                reloaded = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )

            video_sha256 = hashlib.sha256(payload).hexdigest()
            metadata_sha256 = hashlib.sha256(segment.metadata_path.read_bytes()).hexdigest()
            self.assertEqual(sidecar["video_sha256"], video_sha256)
            self.assertEqual(queue_payload["items"][0]["video_sha256"], video_sha256)
            self.assertEqual(queue_payload["items"][0]["metadata_sha256"], metadata_sha256)
            self.assertEqual(reloaded.queue.items[0].video_sha256, video_sha256)
            self.assertEqual(reloaded.queue.items[0].metadata_sha256, metadata_sha256)

    def test_upload_success_marks_source_and_archived_sidecar_uploaded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                segment = recorder.record_segment(_metadata(segment_id="seg-upload-state"), payload=b"fake-h264")

                result = recorder.process_once(now_ms=1_000)

                source_sidecar = json.loads(segment.metadata_path.read_text(encoding="utf-8"))
                archived_sidecar_path = root / "archive" / result.metadata_object_path
                archived_sidecar = json.loads(archived_sidecar_path.read_text(encoding="utf-8"))
                metadata_sha256 = hashlib.sha256(segment.metadata_path.read_bytes()).hexdigest()

                self.assertEqual(source_sidecar["upload_state"], "uploaded")
                self.assertEqual(archived_sidecar["upload_state"], "uploaded")
                self.assertEqual(recorder.queue.items[0].metadata_sha256, metadata_sha256)

    def test_scan_pending_segments_recovers_missing_upload_queue_from_sidecars(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                payload = b"recover-from-sidecar"
                segment = recorder.writer.write_segment(
                    _metadata(segment_id="seg-scan-recover"),
                    payload=payload,
                )

                enqueued = recorder.scan_pending_segments()
                duplicate = recorder.scan_pending_segments()
                uploaded = recorder.process_once(now_ms=1_000)

                self.assertEqual(enqueued, 1)
                self.assertEqual(duplicate, 0)
                self.assertEqual(len(recorder.queue.items), 1)
                self.assertEqual(recorder.queue.items[0].video_path, str(segment.video_path))
                self.assertEqual(recorder.queue.items[0].metadata_path, str(segment.metadata_path))
                self.assertEqual(
                    recorder.queue.items[0].video_sha256,
                    hashlib.sha256(payload).hexdigest(),
                )
                self.assertTrue(recorder.queue.items[0].metadata_sha256)
                self.assertEqual(uploaded.action, "uploaded")

    def test_process_once_scans_pending_sidecars_when_queue_is_empty(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                segment = recorder.writer.write_segment(
                    _metadata(segment_id="seg-process-scan"),
                    payload=b"process-once-scan",
                )

                result = recorder.process_once(now_ms=1_000)
                source_sidecar = json.loads(segment.metadata_path.read_text(encoding="utf-8"))

                self.assertEqual(result.action, "uploaded")
                self.assertEqual(len(recorder.queue.items), 1)
                self.assertEqual(recorder.queue.items[0].status, "uploaded")
                self.assertEqual(source_sidecar["upload_state"], "uploaded")

    def test_refreshes_expiring_credentials_before_uploading_same_object_path(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                segment = recorder.record_segment(_metadata(segment_id="seg-refresh"), payload=b"fake-h264")
                stale = recorder.upload_api.issue_credential(segment.metadata, kind="video")
                recorder.queue.items[0].upload_url = stale["upload_url"]
                recorder.queue.items[0].expires_at_ms = 1_100
                recorder.queue._save()

                result = recorder.process_once(now_ms=1_000)

                self.assertEqual(result.action, "credential_refreshed")
                self.assertEqual(recorder.queue.items[0].object_path, stale["object_path"])
                self.assertNotEqual(recorder.queue.items[0].upload_url, stale["upload_url"])
                uploaded = recorder.process_once(now_ms=1_001)
                self.assertEqual(uploaded.action, "uploaded")

    def test_upload_failure_marks_retry_wait_without_affecting_control_service(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                    retry_initial_seconds=10,
                )
                segment = recorder.record_segment(_metadata(segment_id="seg-fail"), payload=b"fake-h264")
                segment.video_path.unlink()

                config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
                adapter = MockVehicleAdapter()
                control = VehicleControlService.from_config(
                    config,
                    session_id="session-001",
                    adapter=adapter,
                    telemetry_interval_ms=50,
                )
                control.start(now_ms=0)
                control.receive_command(
                    ControlCommand(
                        vehicle_id="vehicle-001",
                        session_id="session-001",
                        seq=1,
                        ts_ms=0,
                        gear="D",
                        steering=0.0,
                        throttle=0.2,
                        brake=0.0,
                    ),
                    now_ms=0,
                )

                result = recorder.process_once(now_ms=1_000)
                control.tick(1_000)

                self.assertEqual(result.action, "failed")
                self.assertEqual(recorder.queue.items[0].status, "retry_wait")
                self.assertEqual(recorder.queue.items[0].last_error, "source_file_missing")
                self.assertEqual(control.safety.state, SafetyState.TIMEOUT_BRAKE)

            audit_events = [json.loads(line)["event"] for line in (root / "audit.jsonl").read_text().splitlines()]
            self.assertIn("upload_failed", audit_events)

    def test_upload_backend_failure_marks_retry_wait_instead_of_leaving_uploading(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                    retry_initial_seconds=10,
                )
                recorder.record_segment(_metadata(segment_id="seg-upload-backend-fail"), payload=b"fake-h264")
                recorder.archive = _FailingArchiveUploader("upload_target_unavailable")

                result = recorder.process_once(now_ms=1_000)

                self.assertEqual(result.action, "failed")
                self.assertEqual(result.error, "upload_target_unavailable")
                self.assertEqual(recorder.queue.items[0].status, "retry_wait")
                self.assertEqual(recorder.queue.items[0].last_error, "upload_target_unavailable")
                self.assertEqual(recorder.queue.items[0].next_retry_at_ms, 11_000)
                self.assertEqual(recorder.process_once(now_ms=10_999).action, "wait")

            audit_events = [json.loads(line)["event"] for line in (root / "audit.jsonl").read_text().splitlines()]
            self.assertIn("upload_failed", audit_events)

    def test_upload_completion_api_failure_marks_retry_wait_instead_of_leaving_uploading(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=_FailingCompletionUploadApi(base_url, "upload_api_unavailable"),
                    refresh_margin_seconds=300,
                    retry_initial_seconds=10,
                )
                recorder.record_segment(_metadata(segment_id="seg-upload-complete-fail"), payload=b"fake-h264")

                try:
                    result = recorder.process_once(now_ms=1_000)
                except OSError as exc:
                    self.fail(f"process_once should convert completion failures to retry_wait, raised {exc}")

                self.assertEqual(result.action, "failed")
                self.assertEqual(result.error, "upload_api_unavailable")
                self.assertEqual(recorder.queue.items[0].status, "retry_wait")
                self.assertEqual(recorder.queue.items[0].last_error, "upload_api_unavailable")
                self.assertEqual(recorder.queue.items[0].next_retry_at_ms, 11_000)
                self.assertEqual(recorder.process_once(now_ms=10_999).action, "wait")

            audit_events = [json.loads(line)["event"] for line in (root / "audit.jsonl").read_text().splitlines()]
            self.assertIn("upload_failed", audit_events)

    def test_upload_bandwidth_limit_defers_next_segment_without_marking_it_uploading(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                    max_upload_bandwidth_mbps=1,
                )
                payload = b"x" * 125_000
                recorder.record_segment(_metadata(segment_id="seg-rate-1"), payload=payload)
                recorder.record_segment(_metadata(segment_id="seg-rate-2"), payload=payload)

                first = recorder.process_once(now_ms=1_000)
                limited = recorder.process_once(now_ms=1_499)
                uploaded_bytes = Path(recorder.queue.items[0].video_path).stat().st_size
                uploaded_bytes += Path(recorder.queue.items[0].metadata_path).stat().st_size
                expected_upload_interval_ms = (uploaded_bytes * 8 + 999) // 1000
                second = recorder.process_once(now_ms=1_000 + expected_upload_interval_ms)

                self.assertEqual(first.action, "uploaded")
                self.assertEqual(limited.action, "rate_limited")
                self.assertEqual(limited.segment_id, "seg-rate-2")
                self.assertEqual(limited.retry_after_ms, 1_000 + expected_upload_interval_ms - 1_499)
                self.assertEqual(second.action, "uploaded")
                self.assertEqual(recorder.queue.items[1].status, "uploaded")

    def test_from_config_applies_upload_retry_refresh_and_bandwidth_settings(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader.from_config(
                    config,
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                )

                self.assertEqual(
                    recorder.queue.refresh_margin_ms,
                    config.upload.presigned_url_refresh_margin_seconds * 1000,
                )
                self.assertEqual(recorder.queue.retry_initial_ms, config.upload.retry_initial_seconds * 1000)
                self.assertEqual(recorder.queue.retry_max_ms, config.upload.retry_max_seconds * 1000)
                self.assertIsNotNone(recorder.bandwidth_limiter)
                self.assertEqual(recorder.bandwidth_limiter.max_mbps, config.upload.max_bandwidth_mbps)

    def test_from_config_with_s3_backend_puts_video_and_metadata_to_upload_urls(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            config = replace(config, upload=replace(config.upload, backend="s3"))
            with _running_put_capture() as (put_base_url, uploads):
                signaling = SignalingHttpService(
                    audit_log_path=root / "audit.jsonl",
                    upload_credentials=UploadCredentialService(public_base_url=put_base_url),
                )
                with signaling.running() as base_url:
                    recorder = VehicleRecorderUploader.from_config(
                        config,
                        recording_root=root / "recordings",
                        queue_state_path=root / "upload-queue.json",
                        archive_root=root / "archive",
                        upload_api=UploadApiClient(base_url),
                    )
                    recorder.upload_trigger_policy = UploadTriggerPolicy(trigger_segments=1)
                    segment = recorder.record_segment(_metadata(segment_id="seg-s3-put"), payload=b"direct-put-video")

                    result = recorder.process_once(now_ms=1_000)

            video_upload_path = (
                "/upload-target/000001/vehicles/vehicle-001/sessions/session-001/cameras/front/seg-s3-put.mp4"
            )
            metadata_upload_path = "/upload-target/000002/vehicles/vehicle-001/sessions/session-001/metadata/seg-s3-put.json"
            source_sidecar = json.loads(segment.metadata_path.read_text(encoding="utf-8"))
            uploaded_sidecar = json.loads(uploads[metadata_upload_path].decode("utf-8"))

            self.assertEqual(result.action, "uploaded")
            self.assertEqual(uploads[video_upload_path], b"direct-put-video")
            self.assertEqual(uploaded_sidecar["segment_id"], "seg-s3-put")
            self.assertEqual(uploaded_sidecar["upload_state"], "uploaded")
            self.assertEqual(source_sidecar["upload_state"], "uploaded")
            self.assertFalse((root / "archive").exists())

    def test_from_config_respects_disabled_upload_without_blocking_recording(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config_path = root / "vehicle-upload-disabled.yaml"
            config_path.write_text(
                Path("configs/vehicle-agent.dev.yaml").read_text(encoding="utf-8").replace(
                    "upload:\n  enabled: true",
                    "upload:\n  enabled: false",
                ),
                encoding="utf-8",
            )
            config = load_vehicle_config(config_path)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader.from_config(
                    config,
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                )

                segment = recorder.record_segment(_metadata(segment_id="seg-upload-disabled"), payload=b"fake-h264")
                result = recorder.process_once(now_ms=1_000)

                self.assertTrue(segment.video_path.exists())
                self.assertTrue(segment.metadata_path.exists())
                self.assertEqual(recorder.queue.items, [])
                self.assertEqual(result.action, "disabled")

            audit_path = root / "audit.jsonl"
            audit_events = (
                [json.loads(line)["event"] for line in audit_path.read_text().splitlines()]
                if audit_path.exists()
                else []
            )
            self.assertNotIn("upload_credential_issued", audit_events)
            self.assertNotIn("upload_success", audit_events)

    def test_from_config_waits_for_configured_upload_trigger_segment_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader.from_config(
                    config,
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                )

                recorder.record_segment(_metadata(segment_id="seg-trigger-001"), payload=b"fake-h264")
                waiting = recorder.process_once(now_ms=1_000)
                for index in range(2, config.upload.trigger_segments + 1):
                    recorder.record_segment(_metadata(segment_id=f"seg-trigger-{index:03d}"), payload=b"fake-h264")
                uploaded = recorder.process_once(now_ms=1_001)

                self.assertEqual(waiting.action, "wait")
                self.assertEqual(recorder.queue.items[0].status, "uploaded")
                self.assertEqual(uploaded.action, "uploaded")
                self.assertEqual(uploaded.segment_id, "seg-trigger-001")

    def test_upload_trigger_interval_dispatches_oldest_pending_segment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                    upload_trigger_policy=UploadTriggerPolicy(
                        trigger_segments=20,
                        trigger_interval_ms=1_000,
                    ),
                )
                recorder.record_segment(
                    _metadata(segment_id="seg-trigger-interval"),
                    payload=b"fake-h264",
                    now_ms=0,
                )

                waiting = recorder.process_once(now_ms=999)
                uploaded = recorder.process_once(now_ms=1_000)

                self.assertEqual(waiting.action, "wait")
                self.assertEqual(uploaded.action, "uploaded")
                self.assertEqual(uploaded.segment_id, "seg-trigger-interval")

    def test_network_idle_dispatches_upload_when_configured(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader(
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                    upload_trigger_policy=UploadTriggerPolicy(
                        trigger_segments=20,
                        network_idle_enabled=True,
                    ),
                )
                recorder.record_segment(_metadata(segment_id="seg-network-idle"), payload=b"fake-h264")

                waiting = recorder.process_once(now_ms=1_000, network_idle=False)
                uploaded = recorder.process_once(now_ms=1_001, network_idle=True)

                self.assertEqual(waiting.action, "wait")
                self.assertEqual(uploaded.action, "uploaded")
                self.assertEqual(uploaded.segment_id, "seg-network-idle")

    def test_runtime_upload_updates_apply_allowed_bandwidth_and_pause_state(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader.from_config(
                    config,
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                )

                self.assertTrue(callable(getattr(recorder, "apply_runtime_update", None)))
                bandwidth = recorder.apply_runtime_update("upload.max_bandwidth_mbps", 2.5)
                paused = recorder.apply_runtime_update("upload.paused", True)
                resumed = recorder.apply_runtime_update("upload.paused", False)

            self.assertTrue(bandwidth.allowed)
            self.assertEqual(bandwidth.reason, "runtime_update_allowed")
            self.assertIsNotNone(recorder.bandwidth_limiter)
            self.assertEqual(recorder.bandwidth_limiter.max_mbps, 2.5)
            self.assertTrue(paused.allowed)
            self.assertTrue(resumed.allowed)
            self.assertFalse(recorder.queue.paused)
            self.assertIsNone(recorder.queue.pause_reason)

    def test_runtime_upload_updates_reject_invalid_or_dangerous_fields_without_state_change(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader.from_config(
                    config,
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                )

                self.assertTrue(callable(getattr(recorder, "apply_runtime_update", None)))
                original_limit = recorder.bandwidth_limiter.max_mbps
                invalid = recorder.apply_runtime_update("upload.max_bandwidth_mbps", True)
                dangerous = recorder.apply_runtime_update("control.control_timeout_ms", 1200)

            self.assertFalse(invalid.allowed)
            self.assertEqual(invalid.reason, "runtime_update_rejected_invalid_value")
            self.assertFalse(dangerous.allowed)
            self.assertTrue(dangerous.restart_required)
            self.assertEqual(recorder.bandwidth_limiter.max_mbps, original_limit)
            self.assertFalse(recorder.queue.paused)

    def test_backlog_status_reports_upload_lag_policy_before_queue_grows_unbounded(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            config = load_vehicle_config(Path("configs/vehicle-agent.dev.yaml"))
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                recorder = VehicleRecorderUploader.from_config(
                    config,
                    recording_root=root / "recordings",
                    queue_state_path=root / "upload-queue.json",
                    archive_root=root / "archive",
                    upload_api=UploadApiClient(base_url),
                )
                recorder.record_segment(_metadata(segment_id="seg-backlog"), payload=b"fake-h264")

                status = recorder.backlog_status()

            self.assertEqual(status.action, "alert_and_preserve_uploaded_only")
            self.assertGreater(status.pending_bytes, 0)
            self.assertAlmostEqual(status.net_growth_gb_per_hour, config.capacity.net_growth_gb_per_hour)
            self.assertIn("upload bandwidth below recording production", status.reason)

    def test_vehicle_uploader_cli_demo_records_and_uploads_one_segment(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            result = subprocess.run(
                [
                    sys.executable,
                    "vehicle-uploader/vehicle_uploader.py",
                    "--work-dir",
                    str(root),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("uploaded segment_id=demo_front_000001", result.stdout)
            self.assertTrue((root / "archive").exists())

    def test_vehicle_uploader_cli_service_mode_processes_existing_pending_sidecars(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recording_root = root / "recordings"
            config_path = _write_config_with_recording_root(root, recording_root)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                seed = VehicleRecorderUploader(
                    recording_root=recording_root,
                    queue_state_path=root / "seed-upload-queue.json",
                    archive_root=root / "seed-archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                seed.writer.write_segment(_metadata(segment_id="seg-cli-service"), payload=b"cli-service-h264")

                result = subprocess.run(
                    [
                        sys.executable,
                        "vehicle-uploader/vehicle_uploader.py",
                        "--service-mode",
                        "--process-once",
                        "--config",
                        str(config_path),
                        "--work-dir",
                        str(root / "uploader"),
                        "--upload-api-base-url",
                        base_url,
                        "--network-idle",
                    ],
                    cwd=Path.cwd(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=10,
                )

            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("uploaded segment_id=seg-cli-service", result.stdout)
            self.assertEqual(list(recording_root.rglob("demo_front_000001.*")), [])
            self.assertTrue((root / "uploader" / "archive").exists())

    def test_vehicle_uploader_cli_service_mode_can_emit_json_smoke_result(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            recording_root = root / "recordings"
            config_path = _write_config_with_recording_root(root, recording_root)
            signaling = SignalingHttpService(audit_log_path=root / "audit.jsonl")
            with signaling.running() as base_url:
                seed = VehicleRecorderUploader(
                    recording_root=recording_root,
                    queue_state_path=root / "seed-upload-queue.json",
                    archive_root=root / "seed-archive",
                    upload_api=UploadApiClient(base_url),
                    refresh_margin_seconds=300,
                )
                seed.writer.write_segment(_metadata(segment_id="seg-cli-json"), payload=b"cli-json-h264")

                result = subprocess.run(
                    [
                        sys.executable,
                        "vehicle-uploader/vehicle_uploader.py",
                        "--service-mode",
                        "--process-once",
                        "--config",
                        str(config_path),
                        "--work-dir",
                        str(root / "uploader"),
                        "--upload-api-base-url",
                        base_url,
                        "--network-idle",
                        "--json",
                    ],
                    cwd=Path.cwd(),
                    text=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                    timeout=10,
                )

            self.assertEqual(result.returncode, 0, result.stderr)
            records = [json.loads(line) for line in result.stdout.splitlines()]
            self.assertEqual(len(records), 1)
            self.assertEqual(records[0]["event"], "vehicle_uploader_process_once")
            self.assertTrue(records[0]["passed"])
            self.assertEqual(records[0]["action"], "uploaded")
            self.assertEqual(records[0]["segment_id"], "seg-cli-json")
            self.assertTrue(records[0]["object_path"])
            self.assertTrue(records[0]["metadata_object_path"])


def _write_config_with_recording_root(root: Path, recording_root: Path) -> Path:
    raw = Path("configs/vehicle-agent.dev.yaml").read_text(encoding="utf-8")
    raw = raw.replace("  root_dir: .local/recordings", f"  root_dir: {recording_root}")
    config_path = root / "vehicle-agent.yaml"
    config_path.write_text(raw, encoding="utf-8")
    return config_path


@contextmanager
def _running_put_capture():
    uploads = {}

    class Handler(BaseHTTPRequestHandler):
        def do_PUT(self):
            length = int(self.headers.get("Content-Length", "0"))
            uploads[self.path] = self.rfile.read(length)
            self.send_response(200)
            self.end_headers()

        def log_message(self, format, *args):
            return

    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        host, port = server.server_address
        yield f"http://{host}:{port}", uploads
    finally:
        server.shutdown()
        thread.join(timeout=5)
        server.server_close()


def _metadata(segment_id):
    return SegmentMetadata(
        vehicle_id="vehicle-001",
        session_id="session-001",
        camera_id="front",
        segment_id=segment_id,
        started_at="2026-06-24T10:15:00Z",
        ended_at="2026-06-24T10:16:00Z",
        codec="h264",
        encoder="vaapi",
        width=1920,
        height=1080,
        fps=30,
        upload_state="pending",
    )


class _FailingArchiveUploader:
    def __init__(self, error):
        self.error = error

    def upload(self, **kwargs):
        raise OSError(self.error)


class _FailingCompletionUploadApi(UploadApiClient):
    def __init__(self, base_url, error):
        super().__init__(base_url)
        self.error = error

    def mark_uploaded(self, segment_id, object_path, bytes_uploaded):
        raise OSError(self.error)


if __name__ == "__main__":
    unittest.main()
