# Target Host Result Record Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject malformed `target_host_validation_result` records before they can hide required command failures.

**Architecture:** Keep validation inside `TargetHostValidationArchive.consistency_failed`, alongside existing summary consistency checks. Add a narrow helper that verifies each result record has a string command name, boolean `required`, and integer non-boolean `returncode`.

**Tech Stack:** Python stdlib JSONL parsing, `unittest`, existing `scripts/target_host_validation_report.py` CLI.

---

### Task 1: Result Record Scalar Validation

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Write the failing test**

Add `test_target_host_validation_report_rejects_invalid_result_record_scalars` to `tests/test_deployment_templates.py`. The archive should contain one `target_host_validation_result` with `required: 1` and `returncode: "2"`, plus a self-consistent summary that otherwise reports no required or optional commands. The expected report should exit 2 and list result field consistency failures.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_invalid_result_record_scalars
```

Expected before the implementation: FAIL because the report exits 0 with an empty `consistency_failed` list.

- [x] **Step 3: Write minimal implementation**

Add result record scalar validation to `TargetHostValidationArchive.consistency_failed`, rejecting non-string names, non-boolean `required`, and boolean/non-integer `returncode` values.

- [x] **Step 4: Run test to verify it passes**

Run the focused test again. Expected: PASS.

- [x] **Step 5: Run focused and full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-result-record-validation.md
python3 scripts/check.py
```

Expected: all commands exit 0.

## Evidence

- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_invalid_result_record_scalars`
  failed because the report returned 0 with empty `consistency_failed`.
- GREEN: the same focused test passed after result record scalar validation.
- Neighbor tests: four target-host report consistency/failure summary tests passed.
- Deployment template suite: `python3 -m unittest tests.test_deployment_templates`
  passed 113 tests.
- Hygiene: `python3 -m py_compile ...` and `git diff --check ...` passed.
- Full suite: `python3 scripts/check.py` passed 435 tests.
