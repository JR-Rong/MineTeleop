# Target Host Duplicate Hardware Metrics Validation Implementation Plan

**Goal:** Reject target-host `media.hardware.report.template` evidence where `hardware_encoding_metrics` appears more than once for the validated hardware scenario.

**Architecture:** The hardware report validator already rejects duplicate validation summaries and duplicate lane ids. Add the same single-record guard for scenario-matched metrics before accepting complete metric fields.

## Steps

- [x] **Step 1: Add RED hardware metrics duplicate test**
  - Generate a hardware report with one validation summary, one lane, and two complete `hardware_encoding_metrics` records for the same scenario.
  - Expected failure reason: `hardware_encoding_metrics_duplicate`.
  - Confirm the focused test currently fails because the archive is accepted.
  - RED result: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_hardware_encoding_metrics` failed with `AssertionError: 0 != 2`.

- [x] **Step 2: Add duplicate metrics validation**
  - Reuse the existing scenario filter in `_hardware_metrics_failure`.
  - Fail when more than one matching metrics record remains.
  - GREEN result: focused test passed.

- [x] **Step 3: Validate**
  - Run the focused test.
  - Run adjacent hardware report evidence tests.
  - Run the full `ContainerTemplateTests` surface.
  - Run py_compile, diff check, and `scripts/check.py`.
  - Focused test: PASS.
  - Adjacent hardware report evidence tests: 10 passed.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 127 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-acceptance-metrics-report-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-metrics-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-lane-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-probe-scenario-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-weak-network-profile-validation.md`: PASS.
  - `python3 scripts/check.py`: 458 passed.

## Review Lanes

- Reuse: Keep the check inside `_hardware_metrics_failure` with the existing hardware failure payload style.
- Quality: Preserve scenario mismatch and missing metric field behavior.
- Efficiency: Count already-filtered JSONL records only once.

## Post-Implementation Review

- Reuse: Kept the duplicate check inside the existing hardware metrics validator and reused the hardware report failure shape.
- Quality: Enforces the documented single `hardware_encoding_metrics` record before accepting complete metrics.
- Efficiency: Adds one length check over already-parsed metrics records.
- Regression risk: Accept, duplicate summary, duplicate lane, scenario mismatch, missing fields, lane count, and failed report tests still pass.
