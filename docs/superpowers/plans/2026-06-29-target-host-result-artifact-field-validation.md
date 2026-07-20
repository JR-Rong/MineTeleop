# Target Host Result Artifact Field Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject malformed target-host result records whose command or artifact path fields are missing or not strings.

**Architecture:** Extend the existing `TargetHostValidationArchive.consistency_failed` result-record validation. Keep the report schema stable by reusing `result_invalid` consistency entries for malformed fields.

**Tech Stack:** Python stdlib JSONL parsing, `unittest`, existing `scripts/target_host_validation_report.py` CLI.

---

### Task 1: Result Artifact Field Validation

**Files:**
- Modify: `tests/test_deployment_templates.py`
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Write the failing test**

Add `test_target_host_validation_report_rejects_invalid_result_artifact_fields` with one otherwise-valid `target_host_validation_result` whose `command` is empty, `stdout_path` is empty, and `stderr_path` is not a string. Keep the summary self-consistent and `passed: true` so the current false positive is visible.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_invalid_result_artifact_fields
```

Expected before implementation: FAIL because the report exits 0 with empty `consistency_failed`.

- [x] **Step 3: Write minimal implementation**

In `TargetHostValidationArchive.consistency_failed`, reject non-string or empty `command`, `stdout_path`, and `stderr_path` fields with `result_invalid` consistency entries.

- [x] **Step 4: Run focused tests**

Run the new focused test plus neighboring result/schema tests. Expected: PASS.

- [x] **Step 5: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-result-artifact-field-validation.md
python3 scripts/check.py
```

Expected: all commands exit 0.

## Evidence

- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_invalid_result_artifact_fields`
  failed because the report returned 0 with empty `consistency_failed`.
- GREEN: the same focused test passed after validating result artifact fields.
- Neighbor tests: three target-host result/schema tests passed.
- Deployment template suite: `python3 -m unittest tests.test_deployment_templates`
  passed 114 tests.
- Hygiene: `python3 -m py_compile ...` and `git diff --check ...` passed.
- Full suite: `python3 scripts/check.py` passed 436 tests.
