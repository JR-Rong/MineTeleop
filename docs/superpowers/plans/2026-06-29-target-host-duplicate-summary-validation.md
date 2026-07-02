# Target Host Duplicate Summary Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host validation archives that contain more than one `target_host_validation_summary` record.

**Architecture:** Preserve the existing JSONL parser and report shape while tracking how many summary records were seen. Surface duplicate summaries through `TargetHostValidationArchive.consistency_failed` so the report exits 2 instead of silently trusting the last summary.

**Tech Stack:** Python dataclasses, JSONL parsing, `unittest`, existing `scripts/target_host_validation_report.py` CLI.

---

### Task 1: Duplicate Summary Detection

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Write the failing test**

Add `test_target_host_validation_report_rejects_duplicate_summary_records`. The JSONL archive should contain one successful required result and two `target_host_validation_summary` records. The second summary is internally self-consistent and `passed: true`, proving that the current parser trusts the last summary.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_summary_records
```

Expected before implementation: FAIL because the report exits 0 with empty `consistency_failed`.

- [x] **Step 3: Write minimal implementation**

Add a `summary_count` field to `TargetHostValidationArchive`, increment it in `from_jsonl`, and add a `summary_duplicate` consistency failure when `summary_count > 1`.

- [x] **Step 4: Run focused tests**

Run the new focused test and adjacent summary consistency tests. Expected: PASS.

- [x] **Step 5: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-summary-validation.md
python3 scripts/check.py
```

Expected: all commands exit 0.

Evidence:
- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_summary_records` failed before implementation because the report exited 0 with `consistency_failed=[]`.
- GREEN: focused duplicate-summary test passed.
- Adjacent summary checks passed: command-name mismatch, missing command names, missing metadata, and invalid `passed` field.
- `python3 -m unittest tests.test_deployment_templates` passed 115 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py` exited 0.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-summary-validation.md` exited 0.
- `python3 scripts/check.py` passed 437 tests.
