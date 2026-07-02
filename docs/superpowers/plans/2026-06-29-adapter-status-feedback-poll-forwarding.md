# Adapter Status Feedback Poll Forwarding Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Forward non-empty `--adapter-status --poll-feedback` snapshots into adapter `update_feedback()` before returning the JSON payload.

**Architecture:** `vehicle-agent/vehicle_agent.py` already builds `vehicle_adapter_feedback_poll` JSON from `poll_feedback()`. Reuse that helper and add the same generic forwarding behavior as `VehicleControlService`: when `poll_feedback()` returns a snapshot and the adapter exposes `update_feedback()`, refresh the adapter feedback cache before reporting success.

**Tech Stack:** Python 3, `unittest`, `runpy` module loading, local fake adapter.

---

### Task 1: RED Test

**Files:**
- Modify: `tests/test_chassis_control_integration.py`

- [x] **Step 1: Add feedback poll payload forwarding regression**

Add `test_adapter_status_feedback_poll_payload_forwards_snapshot_to_update_feedback`, loading `vehicle-agent/vehicle_agent.py` with `runpy.run_path()` and calling `_adapter_feedback_poll_payload()` with a fake adapter.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_adapter_status_feedback_poll_payload_forwards_snapshot_to_update_feedback
```

Expected: FAIL because `_adapter_feedback_poll_payload()` currently ignores the returned snapshot after serializing it.

Observed: FAIL because the fake adapter call sequence only contained `poll_feedback`, with no `update_feedback(snapshot)`.

### Task 2: GREEN Implementation

**Files:**
- Modify: `vehicle-agent/vehicle_agent.py`

- [x] **Step 1: Forward non-empty snapshots**

```python
    update_feedback = getattr(adapter, "update_feedback", None)
    if callable(update_feedback):
        update_feedback(snapshot)
```

Add this after the `snapshot is None` branch and before returning the successful payload.

- [x] **Step 2: Run focused validation**

Run the same one-test command.

Expected: PASS.

Observed: PASS.

### Task 3: Regression Validation

**Files:**
- Test: `tests/test_chassis_control_integration.py`
- Test: `vehicle-agent/vehicle_agent.py`

- [x] **Step 1: Run adjacent feedback tests**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_adapter_status_feedback_poll_payload_forwards_snapshot_to_update_feedback tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_forwards_polled_feedback_snapshot_before_telemetry_read tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_dynamic_library_adapter_polls_minepilot_can_feedback_from_c_shim tests.test_design_behaviors.CommandLineEntryPointTests.test_vehicle_agent_adapter_status_cli_reports_feedback_poll_capability tests.test_design_behaviors.CommandLineEntryPointTests.test_vehicle_agent_adapter_status_cli_can_require_feedback_poll
```

Expected: PASS.

Observed: PASS, 5 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration
python3 -m unittest tests.test_design_behaviors.CommandLineEntryPointTests
python3 -m py_compile vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py
git diff --check -- vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-adapter-status-feedback-poll-forwarding.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_chassis_control_integration`: PASS, 23 tests.
- `python3 -m unittest tests.test_design_behaviors.CommandLineEntryPointTests`: PASS, 38 tests.
- `python3 -m py_compile vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py`: PASS.
- `git diff --check -- vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-adapter-status-feedback-poll-forwarding.md`: PASS.
- `python3 scripts/check.py`: PASS, 450 tests.

### Self-Review

- Spec coverage: Covers the target-host feedback poll path used by `vehicle.adapter.feedback_poll`.
- Placeholder scan: No placeholders.
- Type consistency: Uses the same snapshot/update_feedback contract as the service-level feedback polling path.
