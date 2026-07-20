# Control Service Feedback Poll Forwarding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Forward non-empty adapter `poll_feedback()` snapshots into `update_feedback()` before telemetry is read.

**Architecture:** `VehicleControlService.tick()` already polls adapter feedback before building telemetry. Keep that timing, but preserve the returned snapshot and forward it only when the adapter exposes `update_feedback()`, so generic CAN/MinePilot readers can refresh the ChassisControl feedback cache without changing adapters that only poll internally.

**Tech Stack:** Python 3, `unittest`, local fake adapter.

---

### Task 1: RED Test

**Files:**
- Modify: `tests/test_chassis_control_integration.py`

- [x] **Step 1: Add service forwarding regression**

Add `test_control_service_forwards_polled_feedback_snapshot_before_telemetry_read`, using a fake adapter whose `poll_feedback()` returns `ChassisControlFeedbackSnapshot` and whose `update_feedback()` records snapshots.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_forwards_polled_feedback_snapshot_before_telemetry_read
```

Expected: FAIL because `_poll_adapter_feedback()` currently ignores the returned snapshot.

Observed: FAIL because the call sequence was `open -> poll_feedback -> read_telemetry`, with no `update_feedback(snapshot)` before telemetry.

### Task 2: GREEN Implementation

**Files:**
- Modify: `mine_teleop/vehicle_control_service.py`

- [x] **Step 1: Forward non-empty snapshots**

```python
    def _poll_adapter_feedback(self) -> None:
        poll_feedback = getattr(self.adapter, "poll_feedback", None)
        if not callable(poll_feedback):
            return
        snapshot = poll_feedback()
        update_feedback = getattr(self.adapter, "update_feedback", None)
        if snapshot is not None and callable(update_feedback):
            update_feedback(snapshot)
```

- [x] **Step 2: Run focused validation**

Run the same one-test command.

Expected: PASS.

Observed: PASS.

### Task 3: Regression Validation

**Files:**
- Test: `tests/test_chassis_control_integration.py`
- Test: `mine_teleop/vehicle_control_service.py`

- [x] **Step 1: Run adjacent integration tests**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_polls_adapter_feedback_before_telemetry_read tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_forwards_polled_feedback_snapshot_before_telemetry_read tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_dynamic_library_adapter_polls_minepilot_can_feedback_from_c_shim tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_dynamic_library_adapter_forwards_decoded_can_feedback_to_c_shim
```

Expected: PASS.

Observed: PASS, 4 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration
python3 -m py_compile mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py
git diff --check -- mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-control-service-feedback-poll-forwarding.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_chassis_control_integration`: PASS, 22 tests.
- `python3 -m py_compile mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py`: PASS.
- `git diff --check -- mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-control-service-feedback-poll-forwarding.md`: PASS.
- `python3 scripts/check.py`: PASS, 449 tests.

### Self-Review

- Spec coverage: Covers the runtime feedback-cache forwarding requirement from `docs/08-configuration.md`.
- Placeholder scan: No placeholders.
- Type consistency: Uses existing `ChassisControlFeedbackSnapshot` and adapter `update_feedback()` contract.
