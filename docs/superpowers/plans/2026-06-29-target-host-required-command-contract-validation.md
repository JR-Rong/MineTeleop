# Target Host Required Command Contract Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host validation archives that downgrade plan-required commands to optional results.

**Architecture:** Keep validation in `TargetHostValidationArchive.consistency_failed`, next to existing result scalar checks. Add a fixed command requirement map for the default target-host plan and reject known command names whose `required` flag differs from the generated plan contract.

**Tech Stack:** Python standard library, `unittest`, JSONL target-host validation archives.

---

## Task 1: Required Command Contract

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Write the failing test**
  - Add a target-host archive where `vehicle.preflight` is marked `required=false`, includes valid stdout evidence, and has a self-consistent summary.
  - Expected: report exits 2 with `result_required_mismatch`.

- [x] **Step 2: Run test to verify it fails**
  - Run: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_required_command_downgrade`
  - Expected: FAIL with `AssertionError: 0 != 2`.
  - RED result: failed with `AssertionError: 0 != 2`, proving the downgraded required command was accepted.

- [x] **Step 3: Write minimal implementation**
  - Add a known target-host command requirement map.
  - In result consistency validation, compare known command names to the expected boolean after validating `required` is a bool.

- [x] **Step 4: Run test to verify it passes**
  - Run the focused test again.
  - Expected: PASS.
  - GREEN result: focused test passed.

- [x] **Step 5: Run adjacent and full validation**
  - Run adjacent result/summary/preflight tests.
  - Run `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`.
  - Run py_compile, diff check, and `python3 scripts/check.py`.
  - Adjacent result/summary/preflight/plan tests: 10 passed after correcting one local test name in the command.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 128 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-required-command-contract-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-acceptance-metrics-report-validation.md docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-metrics-validation.md`: PASS.
  - `python3 scripts/check.py`: 459 passed.

## Review Lanes

- Reuse: Reuse existing `consistency_failed` result validation shape.
- Quality: Only enforce known target-host plan command names so custom test plans remain possible.
- Efficiency: Adds one dictionary lookup per result record.

## Post-Implementation Review

- Reuse: Added the required/optional contract next to existing result scalar validation and reused the `consistency_failed` report surface.
- Quality: Known default target-host commands can no longer be downgraded from required to optional in hand-edited archives.
- Efficiency: Validation adds only a constant-time lookup for each result record.
- Regression risk: Existing custom artifact shell plan and optional sample-report commands still pass adjacent and full validation.
