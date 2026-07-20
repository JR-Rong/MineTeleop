# Target Host Duplicate Feedback Evidence Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host validation stdout that contains more than one `vehicle_adapter_feedback_poll` evidence record.

**Architecture:** Keep validation inside `TargetHostValidationArchive.evidence_failed`. The vehicle adapter feedback command is expected to emit one feedback poll result in the same stdout as adapter status; duplicate feedback records make it possible for a later successful JSON line to hide an earlier no-feedback result.

**Tech Stack:** Python JSONL parsing, existing `TargetHostValidationArchive`, `unittest`, `scripts/target_host_validation_report.py`.

---

### Task 1: Duplicate Feedback Poll Evidence Detection

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Write the failing test**

Add `test_target_host_validation_report_rejects_duplicate_feedback_poll_evidence`. The archived stdout should include one valid `vehicle_adapter_status`, then a `vehicle_adapter_feedback_poll` record with `received=false`, followed by another `vehicle_adapter_feedback_poll` record with `received=true` and a complete snapshot.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_feedback_poll_evidence
```

Expected before implementation: FAIL because the report exits 0 by accepting the later successful feedback record.

- [x] **Step 3: Write minimal implementation**

Collect `vehicle_adapter_feedback_poll` stdout records before validating them. If more than one record is present, return a `vehicle_adapter_feedback_poll_duplicate` evidence failure with the duplicate count.

- [x] **Step 4: Run focused tests**

Run the new test plus existing feedback-poll evidence tests. Expected: PASS.

- [x] **Step 5: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-feedback-evidence-validation.md
python3 scripts/check.py
```

Expected: all commands exit 0.

Evidence:
- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_feedback_poll_evidence` failed before implementation because the report exited 0 with `evidence_failed=[]`.
- GREEN: focused duplicate-feedback-poll evidence test passed.
- Adjacent feedback-poll checks passed: accepted received evidence, no received evidence, missing stdout evidence, incomplete snapshot, and missing adapter status.
- `python3 -m unittest tests.test_deployment_templates` passed 117 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py` exited 0.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-feedback-evidence-validation.md` exited 0.
- `python3 scripts/check.py` passed 439 tests.
