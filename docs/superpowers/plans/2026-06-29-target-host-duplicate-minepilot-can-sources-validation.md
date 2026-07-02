# Target Host Duplicate MinePilot CAN Sources Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host archives where `minepilot.can.sources` stdout contains duplicate `minepilot_can_sources` records.

**Architecture:** The archive report already validates the MinePilot CAN source file list and checkout root. Keep that validation, but require exactly one matching sources record so a valid source list cannot hide a later contradictory checkout root or file list.

**Tech Stack:** Python 3, `unittest`, JSONL target-host validation archives.

---

### Task 1: RED Test

**Files:**
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Add duplicate sources evidence test**

```python
    def test_target_host_validation_report_rejects_duplicate_minepilot_can_sources_evidence(self):
        source_files = [
            "include/can/can_common.h",
            "include/can/can_message.h",
            "include/can_db.h",
            "src/can_db.cpp",
            "include/can_receiver.h",
            "src/can_receiver.cpp",
            "include/can_sender.h",
            "src/can_sender.cpp",
        ]
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_minepilot_can_sources_evidence
```

Expected: FAIL because the current report accepts the first matching sources record.

Observed: FAIL with `AssertionError: 0 != 2`, proving the report accepted duplicate sources evidence before the fix.

### Task 2: GREEN Implementation

**Files:**
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Require a single sources record**

```python
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == _MINEPILOT_CAN_SOURCES_EVENT
    ]
    if len(matching_records) > 1:
        failure = _minepilot_can_sources_failure("minepilot_can_sources_duplicate_evidence", stdout_path)
        failure["actual"] = str(len(matching_records))
        return failure
```

- [x] **Step 2: Run focused validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_minepilot_can_sources_evidence
```

Expected: PASS.

Observed: PASS.

### Task 3: Regression Validation

**Files:**
- Test: `tests/test_deployment_templates.py`
- Test: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Run adjacent sources tests**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_requires_can_interface_and_source_structured_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_accepts_can_interface_and_source_structured_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_minepilot_can_sources_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_can_sources_root_mismatch
```

Expected: PASS.

Observed: PASS, 4 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-minepilot-can-sources-validation.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_deployment_templates`: PASS, 122 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py`: PASS.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-minepilot-can-sources-validation.md`: PASS.
- `python3 scripts/check.py`: PASS, 444 tests.

### Self-Review

- Spec coverage: Covers the MinePilot CAN source root/file-list evidence requirement from `docs/11-testing-and-validation.md` and `docs/12-operations-and-troubleshooting.md`.
- Placeholder scan: No placeholders.
- Type consistency: Failure payload reuses the MinePilot CAN sources evidence shape and stringifies duplicate counts.
