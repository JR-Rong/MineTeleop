# Target Host Duplicate Hardware Lane Validation Implementation Plan

**Goal:** Reject target-host hardware encoding archives where multiple `hardware_encoding_lane` records reuse the same `lane_id` within one scenario.

**Architecture:** `media.hardware.report.template` already validates that a hardware report has a summary, lane records, and metrics. Tighten the lane evidence so `lane_count` cannot be satisfied by duplicated lane IDs.

**External context checked:**

- `/Volumes/SystemDisk/Workspace/ChassisControl` is on `UI_Test` and exposes the C++ ChassisControl API used by the bridge.
- `/Volumes/SystemDisk/Workspace/MinePilot` is on `merge_ui_test` and provides `libchassis_control.so`, `can_db`, `can_receiver`, and `can_sender`.
- The current repo already contains the C shim bridge and target-host validation path, so this change stays in archive evidence validation.

## Steps

- [x] **Step 1: Add RED archive test**
  - Add a `ContainerTemplateTests` case where `lane_count=2` but both lane records use `front-realtime-720p30`.
  - Expected failure reason: `hardware_encoding_lane_duplicate`.
  - Verify the focused test currently fails with return code 0 instead of 2.
  - RED result: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_hardware_encoding_lane_ids` failed with `AssertionError: 0 != 2`.

- [x] **Step 2: Add lane duplicate validation**
  - Add a helper that counts `(scenario, lane_id)` pairs among `hardware_encoding_lane` records.
  - Call it after lane count validation and before per-lane pass/fail validation.
  - GREEN result: focused test passed.

- [x] **Step 3: Validate**
  - Run the focused new test.
  - Run adjacent hardware report archive tests.
  - Run the full `ContainerTemplateTests` surface.
  - Run py_compile, diff check, and `scripts/check.py`.
  - Focused test: PASS.
  - Adjacent hardware archive tests: 5 passed.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 123 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-lane-validation.md`: PASS.
  - `python3 scripts/check.py`: 454 passed.

## Review Lanes

- Reuse: Use the existing hardware report evidence helper pattern and failure payload style.
- Quality: Keep validation local to `deployment_validation.py` without changing report generation.
- Efficiency: One pass over JSONL records; no extra filesystem reads.

## Post-Implementation Review

- Reuse: Reused the existing `_hardware_*_failure` helper pattern and target-host archive failure payload shape.
- Quality: Kept the change local to artifact validation; no behavior changes to hardware report generation.
- Efficiency: Added a small in-memory count over parsed stdout records after the existing lane count check.
- Regression risk: Existing count mismatch, failed lane, failed summary, and metrics validations still pass in adjacent tests.
