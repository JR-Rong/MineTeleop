# Target Host Duplicate Chassis Bridge Check Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host archives where `chassis.bridge.check` stdout contains duplicate check records for the same check name.

**Architecture:** The archive report already parses `chassis_bridge_check` summary and per-check stdout JSONL records. Keep the existing metadata and ready-status validation, but detect duplicate per-check names before building the lookup dictionary so a later ready record cannot overwrite an earlier failed or contradictory record.

**Tech Stack:** Python 3, `unittest`, JSONL target-host validation archives.

---

### Task 1: RED Test

**Files:**
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Add duplicate chassis bridge check test**

```python
    def test_target_host_validation_report_rejects_duplicate_chassis_bridge_check_records(self):
        stdout_records = [
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
            {
                "name": "cmake.build",
                "path": _bridge_ready_path("cmake.build"),
                "status": "ready",
                "message": _bridge_ready_message("cmake.build"),
            },
        ]
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_chassis_bridge_check_records
```

Expected: FAIL because the current report overwrites duplicate check records by name.

Observed: FAIL with `AssertionError: 0 != 2`, proving the report accepted duplicate check records before the fix.

### Task 2: GREEN Implementation

**Files:**
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Reject duplicate check names**

```python
    checks: dict[str, dict[str, Any]] = {}
    check_counts: dict[str, int] = {}
    for stdout_record in stdout_records:
        name = stdout_record.get("name")
        if isinstance(name, str):
            check_counts[name] = check_counts.get(name, 0) + 1
            checks[name] = stdout_record
    for check_name, count in check_counts.items():
        if count > 1:
            return {
                "actual": str(count),
                "check": check_name,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_duplicate_check",
                "stdout_path": stdout_path,
            }
```

- [x] **Step 2: Run focused validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_chassis_bridge_check_records
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
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_requires_chassis_bridge_build_ready_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_chassis_bridge_check_records tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_chassis_bridge_summary_check_count_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_invalid_chassis_bridge_summary_check_count tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_chassis_bridge_build_dir_summary_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_chassis_bridge_library_outside_checkout_roots tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_chassis_bridge_library_summary_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_chassis_bridge_symbols_path_mismatch
```

Expected: PASS.

Observed: PASS, 8 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-chassis-bridge-check-validation.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_deployment_templates`: PASS, 120 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py`: PASS.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-chassis-bridge-check-validation.md`: PASS.
- `python3 scripts/check.py`: PASS, 442 tests.

### Self-Review

- Spec coverage: Covers the bridge stdout count and splice-prevention requirement from `docs/11-testing-and-validation.md` and `docs/12-operations-and-troubleshooting.md`.
- Placeholder scan: No placeholders.
- Type consistency: Failure payload uses existing report dictionary shape and stringifies duplicate counts like nearby evidence validation failures.
