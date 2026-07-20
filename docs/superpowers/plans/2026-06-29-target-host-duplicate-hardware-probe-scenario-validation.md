# Target Host Duplicate Hardware Probe Scenario Validation Implementation Plan

**Goal:** Reject target-host `media.hardware.probes` archives where a required hardware probe scenario appears more than once.

**Architecture:** The validator already checks missing and unexpected scenario names. Add duplicate detection before the set-based checks so a report cannot hide repeated probe commands behind a complete scenario set.

## Steps

- [x] **Step 1: Add RED archive test**
  - Build a `media.hardware.probes` stdout with all three required scenarios, but repeat `four-camera-realtime-720p30`.
  - Expected failure reason: `hardware_probe_plan_duplicate_scenarios`.
  - Confirm the focused test currently fails because the archive is accepted.
  - RED result: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_hardware_probe_scenario` failed with `AssertionError: 0 != 2`.

- [x] **Step 2: Add duplicate scenario validation**
  - Extract non-empty `scenario=` names in order.
  - Count each scenario and fail when any count is greater than one.
  - GREEN result: focused test passed.

- [x] **Step 3: Validate**
  - Run the focused test.
  - Run adjacent hardware probe plan archive tests.
  - Run the full `ContainerTemplateTests` surface.
  - Run py_compile, diff check, and `scripts/check.py`.
  - Focused test: PASS.
  - Adjacent hardware probe plan archive tests: 5 passed.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 124 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-probe-scenario-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-lane-validation.md`: PASS.
  - `python3 scripts/check.py`: 455 passed.

## Review Lanes

- Reuse: Keep the same `_hardware_probe_plan_failure` payload style.
- Quality: Preserve existing missing/unexpected/metrics checks and only add duplicate detection.
- Efficiency: Reuse parsed stdout lines; no extra subprocesses or file reads.

## Post-Implementation Review

- Reuse: Used the existing hardware probe failure shape and added a small reusable duplicate-value helper.
- Quality: Duplicate detection runs before set-based missing/unexpected checks, so the error reason is specific.
- Efficiency: Reuses already-read stdout lines and performs only in-memory counting.
- Regression risk: Adjacent missing/extra scenario and plan coverage tests still pass.
