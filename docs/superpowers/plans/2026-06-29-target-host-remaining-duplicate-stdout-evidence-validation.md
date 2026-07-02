# Target Host Remaining Duplicate Stdout Evidence Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject duplicate structured stdout evidence for the remaining target-host validators that still accepted the first matching JSONL event.

**Architecture:** `vehicle.preflight`, GPU probe commands, and `vehicle.uploader.process_once` each parse stdout JSONL and currently accept the first matching event. Tighten each validator to require exactly one matching event before applying its existing validity checks.

**Tech Stack:** Python 3, `unittest`, JSONL target-host validation archives.

---

### Task 1: RED Tests

**Files:**
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Add duplicate evidence tests**

Add tests for:
- `test_target_host_validation_report_rejects_duplicate_preflight_evidence`
- `test_target_host_validation_report_rejects_duplicate_gpu_evidence`
- `test_target_host_validation_report_rejects_duplicate_uploader_process_once_evidence`

- [x] **Step 2: Run tests to verify they fail**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_preflight_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_gpu_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_uploader_process_once_evidence
```

Expected: FAIL because each current validator accepts the first matching stdout event.

Observed: FAIL for all 3 tests with `AssertionError: 0 != 2`, proving each validator accepted duplicate stdout evidence before the fix.

### Task 2: GREEN Implementation

**Files:**
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Reject duplicate `vehicle_preflight` records**

Use `_preflight_failure("vehicle_preflight_duplicate_evidence", stdout_path)` and set `actual` to the duplicate count.

- [x] **Step 2: Reject duplicate GPU event records**

Use `_gpu_failure(name, event_name, f"{event_name}_duplicate_evidence", stdout_path)` and set `actual` to the duplicate count.

- [x] **Step 3: Reject duplicate `vehicle_uploader_process_once` records**

Use `_uploader_process_once_failure("vehicle_uploader_process_once_duplicate_evidence", stdout_path)` and set `actual` to the duplicate count.

- [x] **Step 4: Run focused validation**

Run the same three-test command from Task 1.

Expected: PASS.

Observed: PASS, 3 tests.

### Task 3: Regression Validation

**Files:**
- Test: `tests/test_deployment_templates.py`
- Test: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Run adjacent report tests**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_requires_preflight_ready_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_preflight_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_requires_gpu_structured_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_accepts_gpu_structured_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_gpu_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_requires_uploader_process_once_json_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_failed_uploader_process_once tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_unknown_uploader_process_once_action tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_uploader_process_once_evidence
```

Expected: PASS.

Observed: PASS, 9 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-remaining-duplicate-stdout-evidence-validation.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_deployment_templates`: PASS, 126 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py`: PASS.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-remaining-duplicate-stdout-evidence-validation.md`: PASS.
- `python3 scripts/check.py`: PASS, 448 tests.
- `rg -n "for stdout_record in _stdout_json_records" mine_teleop/deployment_validation.py`: only list comprehensions remain.

### Self-Review

- Spec coverage: Covers the structured-evidence requirements for preflight, GPU probes, and uploader process-once target-host validation.
- Placeholder scan: No placeholders.
- Type consistency: Failure payloads reuse existing validator-specific failure shapes and stringified duplicate counts.
