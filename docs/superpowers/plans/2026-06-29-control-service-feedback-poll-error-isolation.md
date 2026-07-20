# Control Service Feedback Poll Error Isolation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Keep the control service telemetry loop running when optional adapter feedback polling fails, while still exposing the adapter error in telemetry status.

**Architecture:** `VehicleControlService.tick()` polls adapter feedback immediately before building telemetry. Keep that ordering, but isolate only feedback poll/cache-update failures inside `_poll_adapter_feedback()` so control output and telemetry evidence remain available. Do not catch command application, safe-stop, or telemetry read errors in this change.

**Tech Stack:** Python standard library, `unittest`, existing `DynamicLibraryVehicleAdapter` and fake bridge test helper.

---

### Task 1: Feedback Poll Error Isolation

**Files:**
- Modify: `tests/test_chassis_control_integration.py`
- Modify: `mine_teleop/vehicle_control_service.py`

- [x] **Step 1: Write the failing test**

Add `test_control_service_reports_feedback_poll_error_in_telemetry_without_stopping_tick` near the existing control service feedback tests. Extend `_FakeBridgeLibrary` with `poll_result` so the real `DynamicLibraryVehicleAdapter.poll_feedback()` sets its `last_error` and raises `VehicleAdapterError`.

```python
    def test_control_service_reports_feedback_poll_error_in_telemetry_without_stopping_tick(self):
        fake = _FakeBridgeLibrary(poll_result=-7)
        adapter = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0),
            library_loader=lambda path: fake,
        )
        config = replace(
            load_vehicle_config(Path("configs/vehicle-agent.dev.yaml")),
            vehicle_adapter_type="dynamic_library",
            vehicle_adapter_contract=_contract_with_chassis_abi(
                "c_shim",
                bridge_library_path="/tmp/libmine_teleop_chassis_bridge.so",
            ),
        )
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )

        service.start(now_ms=0)
        try:
            service.tick(now_ms=0)
        except VehicleAdapterError as exc:
            self.fail(f"feedback poll errors should be reported in telemetry, not stop tick: {exc}")

        self.assertEqual(fake.calls, [("open", b"can0"), ("poll_feedback",), ("read_telemetry",)])
        telemetry = service.telemetry_history[-1]
        self.assertEqual(telemetry["source"], "dynamic_library")
        self.assertFalse(telemetry["vehicle_adapter"]["healthy"])
        self.assertEqual(
            telemetry["vehicle_adapter"]["last_error"],
            "mine_teleop_chassis_poll_feedback failed with code -7",
        )
```

Update `_FakeBridgeLibrary`:

```python
class _FakeBridgeLibrary:
    def __init__(self, open_result=0, poll_result=0):
        self.calls = []
        self.open_result = open_result
        self.poll_result = poll_result
        ...

    def _poll_feedback(self, feedback_ptr):
        ...
        self.calls.append(("poll_feedback",))
        return self.poll_result
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_reports_feedback_poll_error_in_telemetry_without_stopping_tick
```

Expected: FAIL because `VehicleControlService.tick()` currently lets the feedback poll `VehicleAdapterError` escape.

- [x] **Step 3: Write minimal implementation**

Wrap feedback polling and feedback-cache update in `_poll_adapter_feedback()`:

```python
        try:
            snapshot = poll_feedback()
        except Exception:
            return
        update_feedback = getattr(self.adapter, "update_feedback", None)
        if snapshot is not None and callable(update_feedback):
            try:
                update_feedback(snapshot)
            except Exception:
                return
```

- [x] **Step 4: Run test to verify it passes**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_reports_feedback_poll_error_in_telemetry_without_stopping_tick
```

Expected: PASS.

- [x] **Step 5: Run adjacent validation**

Run:

```bash
python3 -m unittest \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_polls_adapter_feedback_before_telemetry_read \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_forwards_polled_feedback_snapshot_before_telemetry_read \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_reports_feedback_poll_error_in_telemetry_without_stopping_tick \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_dynamic_library_adapter_polls_minepilot_can_feedback_from_c_shim \
  tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_dynamic_library_service_telemetry_is_not_marked_as_mock
```

Expected: all tests pass.

- [x] **Step 6: Run full validation for touched surface**

Run:

```bash
python3 -m unittest tests.test_chassis_control_integration
python3 -m unittest tests.test_design_behaviors.VehicleControlServiceTests
python3 -m py_compile mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py
git diff --check -- mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-control-service-feedback-poll-error-isolation.md
python3 scripts/check.py
```

Expected: all commands pass.

## Execution Results

- RED: `python3 -m unittest tests.test_chassis_control_integration.ChassisControlIntegrationTests.test_control_service_reports_feedback_poll_error_in_telemetry_without_stopping_tick` failed with `AssertionError: feedback poll errors should be reported in telemetry, not stop tick: mine_teleop_chassis_poll_feedback failed with code -7`.
- GREEN: the same focused test passed after isolating feedback poll/update failures in `_poll_adapter_feedback()`.
- Adjacent validation: 5 feedback/dynamic-library service tests passed.
- Related validation: `python3 -m unittest tests.test_chassis_control_integration` passed, 25 tests.
- Service validation: `python3 -m unittest tests.test_design_behaviors.VehicleControlServiceTests` passed, 10 tests.
- Compile validation: `python3 -m py_compile mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py` passed.
- Diff validation: `git diff --check -- mine_teleop/vehicle_control_service.py tests/test_chassis_control_integration.py docs/superpowers/plans/2026-06-29-control-service-feedback-poll-error-isolation.md` passed.
- Full validation: `python3 scripts/check.py` passed, 452 tests in 55.539s.

## Review

- Reuse: Reused the existing adapter `last_error` and telemetry `vehicle_adapter` status contract instead of adding a second error channel.
- Quality: The exception boundary is narrow: feedback poll/cache refresh is isolated, while control command, safe-stop, and telemetry-read failures still surface.
- Efficiency: One focused service-level regression covers the real dynamic-library adapter path and existing fake bridge helper.
