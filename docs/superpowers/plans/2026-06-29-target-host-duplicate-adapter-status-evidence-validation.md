# Target Host Duplicate Adapter Status Evidence Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host validation stdout that contains more than one `vehicle_adapter_status` evidence record.

**Architecture:** Keep validation inside the shared `_adapter_status_records_failure` helper so both `vehicle.adapter.status` and `vehicle.adapter.feedback_poll` use the same unambiguous evidence rule. The vehicle agent adapter status smoke prints one status record; duplicates make it possible for a good status to hide a later mock, unhealthy, or wrong-interface status.

**Tech Stack:** Python JSONL parsing, existing target-host archive report CLI, `unittest`.

---

### Task 1: Duplicate Adapter Status Evidence Detection

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Write the failing test**

Add `test_target_host_validation_report_rejects_duplicate_adapter_status_evidence`. The archived feedback-poll stdout should include two `vehicle_adapter_status` records followed by one valid `vehicle_adapter_feedback_poll` record. The first status is ready and real; the second status is mock, proving the current parser trusts the first ready status.

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_adapter_status_evidence
```

Expected before implementation: FAIL because the report exits 0 by accepting the first ready status.

- [x] **Step 3: Write minimal implementation**

Collect `vehicle_adapter_status` stdout records in `_adapter_status_records_failure`. If more than one record is present, return `vehicle_adapter_status_duplicate` with the duplicate count before validating record content.

- [x] **Step 4: Run focused tests**

Run the new test plus existing adapter status and feedback-poll evidence tests. Expected: PASS.

- [x] **Step 5: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-adapter-status-evidence-validation.md
python3 scripts/check.py
```

Expected: all commands exit 0.

Evidence:
- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_adapter_status_evidence` failed before implementation because the report exited 0 with `evidence_failed=[]`.
- GREEN: focused duplicate-adapter-status evidence test passed.
- Adjacent adapter status and feedback-poll checks passed: duplicate feedback evidence, accepted feedback evidence, missing adapter status, opened/ready/metadata failures, mock adapter rejection, ready adapter acceptance, library path mismatch, and CAN interface mismatch.
- `python3 -m unittest tests.test_deployment_templates` passed 118 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py` exited 0.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-adapter-status-evidence-validation.md` exited 0.
- `python3 scripts/check.py` passed 440 tests.
