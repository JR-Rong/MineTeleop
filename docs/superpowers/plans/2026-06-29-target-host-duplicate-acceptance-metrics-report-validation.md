# Target Host Duplicate Acceptance Metrics Report Validation Implementation Plan

**Goal:** Reject target-host `acceptance.metrics.report` evidence where one of the required acceptance metric report events appears more than once.

**Architecture:** The validator already requires the documented acceptance metric event set and compares each event's scenario to the target-host summary. Add duplicate event detection before failed-report and scenario checks so repeated report evidence cannot satisfy set-based presence checks.

## Steps

- [x] **Step 1: Add RED acceptance metrics report test**
  - Generate all documented acceptance metric report events with a duplicate `video_acceptance_metrics` event.
  - Expected failure reason: `acceptance_metrics_report_duplicate`.
  - Confirm the focused test currently fails because the archive is accepted.
  - RED result: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_acceptance_metric_report` failed with `AssertionError: 0 != 2`.

- [x] **Step 2: Add duplicate acceptance metric validation**
  - Extract required acceptance metric events in order.
  - Reuse `_duplicate_values` and fail on the first duplicate event.
  - GREEN result: focused test passed.

- [x] **Step 3: Validate**
  - Run the focused test.
  - Run adjacent acceptance metrics report tests.
  - Run the full `ContainerTemplateTests` surface.
  - Run py_compile, diff check, and `scripts/check.py`.
  - Focused test: PASS.
  - Adjacent acceptance metrics report tests: 5 passed.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 126 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-acceptance-metrics-report-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-lane-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-probe-scenario-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-weak-network-profile-validation.md`: PASS.
  - `python3 scripts/check.py`: 457 passed.

## Review Lanes

- Reuse: Reuse `_duplicate_values` and the existing acceptance metrics evidence failure payload style.
- Quality: Preserve missing-report, failed-report, and scenario mismatch behavior.
- Efficiency: Count only the already-parsed JSONL stdout records.

## Post-Implementation Review

- Reuse: Reused the duplicate-value helper already shared by target-host evidence validators.
- Quality: Duplicate required metric events now produce a precise failure before failed-report and scenario checks.
- Efficiency: Adds only one in-memory pass over parsed stdout records.
- Regression risk: Existing missing-report, scenario mismatch, and failed-report tests still pass.
