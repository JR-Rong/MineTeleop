# Target Host Duplicate Result Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host validation archives that contain duplicate `target_host_validation_result.name` values.

**Architecture:** Keep validation inside `TargetHostValidationArchive.consistency_failed`. Command result names identify validation commands; duplicate names make the archive ambiguous even when summary counts and `command_names` are self-consistent.

**Tech Stack:** Python dataclasses, JSONL parsing, `unittest`, existing `scripts/target_host_validation_report.py` CLI.

---

### Task 1: Duplicate Result Name Detection

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Write the failing test**

Add `test_target_host_validation_report_rejects_duplicate_result_names`. The JSONL archive should contain two successful `target_host_validation_result` records with the same non-empty `name` and a self-consistent summary that lists the duplicate name twice.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_result_names
```

Expected before implementation: FAIL because the report currently exits 0 with empty `consistency_failed`.

- [x] **Step 3: Write minimal implementation**

Track seen non-empty result names while validating result records and add a `result_duplicate` consistency failure for repeated names.

- [x] **Step 4: Run focused tests**

Run the new focused test and adjacent result/summary consistency tests. Expected: PASS.

- [x] **Step 5: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-result-validation.md
python3 scripts/check.py
```

Expected: all commands exit 0.

Evidence:
- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_result_names` failed before implementation because the report exited 0 with `consistency_failed=[]`.
- GREEN: focused duplicate-result-name test passed.
- Adjacent result/summary checks passed: invalid result scalars, invalid artifact fields, duplicate summary, and command-name mismatch.
- `python3 -m unittest tests.test_deployment_templates` passed 116 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py` exited 0.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-result-validation.md` exited 0.
- `python3 scripts/check.py` passed 438 tests.
