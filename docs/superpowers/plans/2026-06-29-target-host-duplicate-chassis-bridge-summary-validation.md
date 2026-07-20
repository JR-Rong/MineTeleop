# Target Host Duplicate Chassis Bridge Summary Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host archives where `chassis.bridge.check` stdout contains duplicate ready `chassis_bridge_check` summary records.

**Architecture:** The archive report already requires a ready `chassis_bridge_check` summary record before validating per-check evidence. Tighten that requirement from "at least one ready summary" to "exactly one ready summary" so duplicated or spliced stdout cannot be accepted.

**Tech Stack:** Python 3, `unittest`, JSONL target-host validation archives.

---

### Task 1: RED Test

**Files:**
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Add duplicate ready summary test**

```python
    def test_target_host_validation_report_rejects_duplicate_chassis_bridge_ready_summary_records(self):
        stdout_records = [
            {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES)},
            {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES)},
            *(
                {
                    "name": check_name,
                    "path": _bridge_ready_path(check_name),
                    "status": "ready",
                    "message": _bridge_ready_message(check_name),
                }
                for check_name in _BRIDGE_READY_CHECK_NAMES
            ),
        ]
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_chassis_bridge_ready_summary_records
```

Expected: FAIL because the current report accepts the first ready summary and ignores duplicates.

Observed: FAIL with `AssertionError: 0 != 2`, proving the report accepted duplicate ready summary records before the fix.

### Task 2: GREEN Implementation

**Files:**
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Require exactly one ready summary**

```python
    ready_summary_records = [
        stdout_record
        for stdout_record in stdout_records
        if stdout_record.get("event") == "chassis_bridge_check" and stdout_record.get("ready") is True
    ]
    if len(ready_summary_records) > 1:
        return {
            "actual": str(len(ready_summary_records)),
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_duplicate_ready_summary",
            "stdout_path": stdout_path,
        }
    summary_record = ready_summary_records[0] if ready_summary_records else None
```

- [x] **Step 2: Run focused validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_chassis_bridge_ready_summary_records
```

Expected: PASS.

Observed: PASS.

### Task 3: Regression Validation

**Files:**
- Test: `tests/test_deployment_templates.py`
- Test: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Run adjacent chassis bridge archive tests**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_requires_chassis_bridge_build_ready_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_chassis_bridge_ready_summary_records tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_chassis_bridge_check_records tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_chassis_bridge_summary_check_count_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_invalid_chassis_bridge_summary_check_count
```

Expected: PASS.

Observed: PASS, 5 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-chassis-bridge-summary-validation.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_deployment_templates`: PASS, 121 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py`: PASS.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-chassis-bridge-summary-validation.md`: PASS.
- `python3 scripts/check.py`: PASS, 443 tests.

### Self-Review

- Spec coverage: Covers the bridge stdout summary and splice-prevention requirement from `docs/11-testing-and-validation.md` and `docs/12-operations-and-troubleshooting.md`.
- Placeholder scan: No placeholders.
- Type consistency: Failure payload matches the existing archive evidence failure shape and stringifies duplicate counts.
