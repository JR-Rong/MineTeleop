# Adapter Status Feedback Update Error Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve `vehicle_adapter_feedback_poll` JSON evidence when `poll_feedback()` receives a decoded CAN snapshot but `update_feedback(snapshot)` fails.

**Architecture:** `vehicle-agent/vehicle_agent.py` owns the adapter-status smoke helper that emits feedback-poll JSON. Keep the behavior local to `_adapter_feedback_poll_payload()`: after a non-empty snapshot is received, call `update_feedback(snapshot)` when available, but convert update failures into a structured failed feedback-poll payload instead of raising to the outer smoke handler.

**Tech Stack:** Python standard library, `unittest`, existing Mine Teleop vehicle agent helpers.

---

### Task 1: Feedback Update Failure Evidence

**Files:**
- Modify: `tests/test_chassis_control_integration.py`
- Modify: `vehicle-agent/vehicle_agent.py`

- [x] **Step 1: Write the failing test**

Add `test_adapter_status_feedback_poll_payload_reports_update_error` next to the existing feedback-poll payload test. Reuse `_PollingFeedbackAdapter`, extended with an optional `update_error`, and assert that the helper returns a failed payload instead of raising.

```python
    def test_adapter_status_feedback_poll_payload_reports_update_error(self):
        snapshot = ChassisControlFeedbackSnapshot(
            shake_hand_status=5,
            epb_status=(2, 3, 4, 5),
            gear_status=3,
            mcu_mode=(11, 12, 13, 14, 15, 16, 17, 18),
            eps_mode=(21, 22, 23, 24),
            eps_angle=(0.1, -0.2, 0.3, -0.4),
            ehb_mode=(31, 32, 33, 34, 35, 36, 37, 38),
            vehicle_speed=1.75,
            vehicle_speed_valid=True,
        )
        adapter = _PollingFeedbackAdapter(
            snapshot,
            update_error=RuntimeError("feedback cache rejected snapshot"),
        )
        vehicle_agent = runpy.run_path("vehicle-agent/vehicle_agent.py")

        try:
            payload = vehicle_agent["_adapter_feedback_poll_payload"](adapter)
        except RuntimeError as exc:
            self.fail(f"feedback poll should report update errors as payload: {exc}")

        self.assertFalse(payload["received"])
        self.assertEqual(payload["reason"], "adapter_feedback_update_error")
        self.assertEqual(payload["error"], "feedback cache rejected snapshot")
        self.assertEqual(payload["snapshot"]["shake_hand_status"], 5)
        self.assertEqual(
            adapter.calls,
            [
                ("poll_feedback",),
                ("update_feedback", snapshot),
            ],
        )
        self.assertEqual(adapter.snapshots, [])
```

Update `_PollingFeedbackAdapter`:

```python
class _PollingFeedbackAdapter:
    def __init__(self, snapshot, update_error=None):
        self.snapshot = snapshot
        self.update_error = update_error
        self.snapshots = []
        self.calls = []
        self.opened = False

    def update_feedback(self, snapshot):
        self.calls.append(("update_feedback", snapshot))
        if self.update_error is not None:
            raise self.update_error
        self.snapshots.append(snapshot)
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_adapter_status_feedback_poll_payload_reports_update_error
```

Expected: FAIL because `_adapter_feedback_poll_payload()` currently lets `update_feedback()` exceptions escape.

- [x] **Step 3: Write minimal implementation**

Wrap only the optional cache update in `_adapter_feedback_poll_payload()`:

```python
    update_feedback = getattr(adapter, "update_feedback", None)
    if callable(update_feedback):
        try:
            update_feedback(snapshot)
        except Exception as exc:
            return {
                "attempted": True,
                "received": False,
                "reason": "adapter_feedback_update_error",
                "error": str(exc),
                "snapshot": asdict(snapshot),
            }
```

- [x] **Step 4: Run test to verify it passes**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_adapter_status_feedback_poll_payload_reports_update_error
```

Expected: PASS.

- [x] **Step 5: Run adjacent validation**

Run:

```bash
python3 -m unittest \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_adapter_status_feedback_poll_payload_forwards_snapshot_to_update_feedback \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_adapter_status_feedback_poll_payload_reports_update_error \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_forwards_polled_feedback_snapshot_before_telemetry_read \
  tests.test_design_behaviors.CommandLineEntryPointTests.test_vehicle_agent_adapter_status_cli_reports_feedback_poll_capability \
  tests.test_design_behaviors.CommandLineEntryPointTests.test_vehicle_agent_adapter_status_cli_can_require_feedback_poll
```

Expected: all tests pass.

- [x] **Step 6: Run full validation for touched surface**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration
python3 -m unittest tests.test_design_behaviors.CommandLineEntryPointTests
python3 -m py_compile vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py
git diff --check -- vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-adapter-status-feedback-update-error.md
python3 scripts/check.py
```

Expected: all commands pass.

## Execution Results

- RED: `python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_adapter_status_feedback_poll_payload_reports_update_error` failed with `AssertionError: feedback poll should report update errors as payload: feedback cache rejected snapshot`.
- GREEN: the same focused test passed after wrapping `update_feedback(snapshot)`.
- Adjacent validation: 5 feedback/CLI tests passed.
- Related validation: `python3 -m unittest tests.test_chassis_control_integration` passed, 24 tests.
- CLI validation: `python3 -m unittest tests.test_design_behaviors.CommandLineEntryPointTests` passed, 38 tests.
- Compile validation: `python3 -m py_compile vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py` passed.
- Diff validation: `git diff --check -- vehicle-agent/vehicle_agent.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-adapter-status-feedback-update-error.md` passed.
- Full validation: `python3 scripts/check.py` passed, 451 tests in 56.898s.

## Review

- Reuse: Reused the existing feedback-poll helper, `asdict(snapshot)` serialization, and `_PollingFeedbackAdapter` test double.
- Quality: Kept the new branch narrow; update failures now produce structured evidence without changing success-path semantics.
- Efficiency: Added one focused regression and reused existing adjacent/full checks; no new runtime dependency or broader scan required.
