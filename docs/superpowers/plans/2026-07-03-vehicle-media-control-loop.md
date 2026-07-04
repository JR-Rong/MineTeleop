# Vehicle Media Control Loop Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a runnable Mine Teleop mock field loop where the vehicle side captures/encodes/sends H.264 frames, records/uploads encoded video segments, receives driver controls, and reports image/control latency.

**Architecture:** Keep CAN out of scope and use the existing Python reference stack. Add a vehicle media runtime that uses the configured camera profile, ffmpeg, and the existing driver-console HTTP frame ingest endpoint; extend the control and media paths with structured timing fields.

**Tech Stack:** Python 3.9+, stdlib HTTP clients, ffmpeg/libx264 or VAAPI when available, existing `SignalingHttpService`, `DriverConsoleHttpApp`, `VehicleTeleopRuntime`, and `VehicleRecorderUploader`.

---

### Task 1: Control Console Media Timing

**Files:**
- Modify: `mine_teleop/driver_console_runtime.py`
- Modify: `tests/test_driver_console_runtime.py`

- [ ] **Step 1: Write failing test**

Add a test that posts an H.264 frame with `captured_at_ms`, `encoded_at_ms`, and `sent_at_ms`, then asserts `/api/media/frame` returns `end_to_end_latency_ms`, `transport_latency_ms`, and `decode_latency_ms`, and that `/api/status` exposes the camera `latency_ms`.

- [ ] **Step 2: Run failing test**

Run: `.venv/bin/python -m unittest tests.test_driver_console_runtime.DriverConsoleHttpAppTests.test_http_app_records_media_frame_latency -v`

Expected: FAIL because the response does not yet include media latency fields.

- [ ] **Step 3: Implement minimal runtime support**

Thread optional media timestamps through `DriverConsoleHttpApp.do_POST("/api/media/frame")` into `DriverConsoleRuntime.ingest_encoded_frame()`, compute latencies using receive/decode time, and update the dashboard camera latency.

- [ ] **Step 4: Run passing test**

Run: `.venv/bin/python -m unittest tests.test_driver_console_runtime.DriverConsoleHttpAppTests.test_http_app_records_media_frame_latency -v`

Expected: PASS.

### Task 2: Vehicle Media Runtime

**Files:**
- Create: `mine_teleop/vehicle_media_runtime.py`
- Modify: `vehicle-media-agent/vehicle_media_agent.py`
- Modify: `mine_teleop/cli.py`
- Create: `tests/test_vehicle_media_runtime.py`

- [ ] **Step 1: Write failing tests**

Add tests for a fake H.264 encoder and fake HTTP frame sink proving `VehicleMediaRuntime.send_frames()` sends configured camera frames with timing metadata and returns per-camera latency summary. Add a CLI smoke test that `vehicle-media-agent --mode teleop --frames 1` posts a frame to a running `DriverConsoleHttpApp`.

- [ ] **Step 2: Run failing tests**

Run: `.venv/bin/python -m unittest tests.test_vehicle_media_runtime -v`

Expected: FAIL because `mine_teleop.vehicle_media_runtime` and the `teleop` CLI mode do not exist.

- [ ] **Step 3: Implement media runtime**

Create `FfmpegH264FrameEncoder`, `DriverConsoleFrameSink`, `VehicleMediaRuntime`, and JSON-serializable send results. Use test source via ffmpeg `testsrc2`; use V4L2 input for real device paths. Default to x264 for frame-smoke compatibility, while preserving configured dimensions, fps, bitrate, camera id, and timing fields.

- [ ] **Step 4: Wire CLI mode**

Add `vehicle-media-agent --mode teleop`, `--driver-console-url`, `--frames`, `--frame-interval-ms`, `--ffmpeg-binary`, and JSONL output. Keep existing pipeline/probe modes unchanged.

- [ ] **Step 5: Run passing tests**

Run: `.venv/bin/python -m unittest tests.test_vehicle_media_runtime -v`

Expected: PASS.

### Task 3: Control Receive Logs And Latency

**Files:**
- Modify: `mine_teleop/vehicle_teleop_runtime.py`
- Modify: `vehicle-agent/vehicle_agent.py`
- Modify: `tests/test_vehicle_teleop_runtime.py`

- [ ] **Step 1: Write failing test**

Extend the vehicle teleop test to assert `poll_and_execute()` returns `control_latency_ms` and command log records containing seq, steering, throttle, brake, receive time, and latency.

- [ ] **Step 2: Run failing test**

Run: `.venv/bin/python -m unittest tests.test_vehicle_teleop_runtime.VehicleTeleopRuntimeTests.test_vehicle_reports_control_receive_latency_and_logs -v`

Expected: FAIL because the runtime summary does not expose command latency/log records.

- [ ] **Step 3: Implement control receive timing**

Compute `now_ms - command.ts_ms` for each accepted command, store recent control receive records, include them in poll results and summary, and add a vehicle-agent flag that emits JSONL control receive logs during `--teleop`.

- [ ] **Step 4: Run passing test**

Run: `.venv/bin/python -m unittest tests.test_vehicle_teleop_runtime -v`

Expected: PASS.

### Task 4: Recording Upload Smoke

**Files:**
- Modify: `vehicle-media-agent/vehicle_media_agent.py`
- Modify: `mine_teleop/vehicle_media_runtime.py`
- Modify: `tests/test_vehicle_media_runtime.py`

- [ ] **Step 1: Write failing test**

Add a test that runs media teleop with `--record-upload-once`, a local `SignalingHttpService`, and an archive work dir, then asserts one segment sidecar is written and upload result is `uploaded`.

- [ ] **Step 2: Run failing test**

Run: `.venv/bin/python -m unittest tests.test_vehicle_media_runtime.VehicleMediaRuntimeCliTests.test_vehicle_media_agent_records_and_uploads_one_encoded_segment -v`

Expected: FAIL because media teleop does not yet call `VehicleRecorderUploader`.

- [ ] **Step 3: Implement one-shot recording upload**

When requested, write the last encoded frame payload as a minimal H.264 `.mp4` payload using existing `SegmentWriter` semantics, issue upload credentials through the existing Upload API, and process one upload iteration.

- [ ] **Step 4: Run passing test**

Run: `.venv/bin/python -m unittest tests.test_vehicle_media_runtime -v`

Expected: PASS.

### Task 5: Verification

**Files:**
- No production files unless verification exposes a bug.

- [ ] **Step 1: Run targeted tests**

Run: `.venv/bin/python -m unittest tests.test_vehicle_media_runtime tests.test_driver_console_runtime tests.test_vehicle_teleop_runtime tests.test_control_plane_smoke -v`

Expected: PASS, except any documented pre-existing baseline failure must be called out.

- [ ] **Step 2: Run control-plane smoke**

Run: `.venv/bin/python scripts/control_plane_smoke.py --artifact-dir .local/control-smoke`

Expected: JSON summary with `passed=true`, image received, controls forwarded, and frame latency fields populated.

- [ ] **Step 3: Check SSH reachability**

Run: `ssh -o BatchMode=yes -o ConnectTimeout=8 -p 6000 user@60.205.213.254 true`

Expected: Either a successful login or a clear authentication/network blocker to report.
