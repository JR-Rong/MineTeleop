# Target Host Duplicate CAN Interface Evidence Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host archives where `can.interface.show` stdout contains duplicate `can_interface_state` records.

**Architecture:** The archive report already validates that the CAN interface probe stdout is structured and matches summary `can_interface`. Tighten that validation to require exactly one matching state record so contradictory interface evidence cannot be accepted.

**Tech Stack:** Python 3, `unittest`, JSONL target-host validation archives.

---

### Task 1: RED Test

**Files:**
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Add duplicate CAN interface evidence test**

```python
    def test_target_host_validation_report_rejects_duplicate_can_interface_evidence(self):
        evidence = {
            "can.interface.show": [
                {"event": "can_interface_state", "interface": "can0", "passed": True},
                {"event": "can_interface_state", "interface": "can1", "passed": True},
            ],
            "minepilot.can.sources": [
                {
                    "event": "minepilot_can_sources",
                    "files": source_files,
                    "passed": True,
                    "root": "/Volumes/SystemDisk/Workspace/MinePilot",
                }
            ],
        }
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_can_interface_evidence
```

Expected: FAIL because the current report accepts the first matching interface state record.

Observed: FAIL with `AssertionError: 0 != 2`, proving the report accepted duplicate interface evidence before the fix.

### Task 2: GREEN Implementation

**Files:**
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Require a single interface state record**

```python
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == _CAN_INTERFACE_EVENT
    ]
    if len(matching_records) > 1:
        failure = _can_interface_failure("can_interface_state_duplicate_evidence", stdout_path)
        failure["actual"] = str(len(matching_records))
        return failure
```

- [x] **Step 2: Run focused validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_can_interface_evidence
```

Expected: PASS.

Observed: PASS.

### Task 3: Regression Validation

**Files:**
- Test: `tests/test_deployment_templates.py`
- Test: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Run adjacent CAN interface tests**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_requires_can_interface_and_source_structured_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_accepts_can_interface_and_source_structured_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_can_interface_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_minepilot_can_sources_evidence
```

Expected: PASS.

Observed: PASS, 4 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-can-interface-evidence-validation.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_deployment_templates`: PASS, 123 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py`: PASS.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-can-interface-evidence-validation.md`: PASS.
- `python3 scripts/check.py`: PASS, 445 tests.

### Self-Review

- Spec coverage: Covers the target CAN interface structured-evidence requirement from `docs/11-testing-and-validation.md` and `docs/12-operations-and-troubleshooting.md`.
- Placeholder scan: No placeholders.
- Type consistency: Failure payload reuses the CAN interface evidence shape and stringifies duplicate counts.
