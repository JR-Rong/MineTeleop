# Target Host Duplicate MinePilot CAN Evidence Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host archives where a MinePilot CAN probe stdout contains duplicate JSONL records for the same expected probe event.

**Architecture:** The archive report already validates each required MinePilot CAN probe command by reading structured stdout JSONL. Keep that flow, but require exactly one matching stdout event per command so contradictory duplicate evidence cannot be accepted.

**Tech Stack:** Python 3, `unittest`, JSONL target-host validation archives.

---

### Task 1: RED Test

**Files:**
- Modify: `tests/test_deployment_templates.py`

- [x] **Step 1: Add the failing duplicate-evidence test**

```python
    def test_target_host_validation_report_rejects_duplicate_minepilot_can_probe_evidence(self):
        evidence = {
            "minepilot.can.socket.probe": [
                {
                    "event": "minepilot_can_socket_probe",
                    "interface": "can0",
                    "passed": True,
                    "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
                }
            ],
            "minepilot.can.sender.build": [
                {
                    "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                    "event": "minepilot_can_sender_build",
                    "passed": True,
                    "target": "can_sender_main",
                }
            ],
            "minepilot.can.sender.smoke": [
                {
                    "accepted_exit_code": True,
                    "event": "minepilot_can_sender_smoke",
                    "exit_code": 124,
                    "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                    "interface": "can0",
                    "passed": True,
                    "startup_banner_seen": True,
                    "timeout_seconds": 3,
                },
                {
                    "accepted_exit_code": True,
                    "event": "minepilot_can_sender_smoke",
                    "exit_code": 124,
                    "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                    "interface": "can0",
                    "passed": True,
                    "timeout_seconds": 3,
                },
            ],
        }
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_minepilot_can_probe_evidence
```

Expected: FAIL because the current report accepts the first matching stdout event.

Observed: FAIL with `AssertionError: 0 != 2`, proving the report accepted duplicate stdout evidence before the fix.

### Task 2: GREEN Implementation

**Files:**
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Require a single matching stdout event**

```python
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == event_name
    ]
    if len(matching_records) > 1:
        failure = _minepilot_can_probe_failure(
            name,
            event_name,
            "minepilot_can_probe_duplicate_evidence",
            stdout_path,
        )
        failure["actual"] = str(len(matching_records))
        return failure
    for stdout_record in matching_records:
        ...
```

- [x] **Step 2: Run focused validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_minepilot_can_probe_evidence
```

Expected: PASS.

Observed: PASS.

### Task 3: Regression Validation

**Files:**
- Test: `tests/test_deployment_templates.py`
- Test: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Run adjacent MinePilot CAN report tests**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_minepilot_can_probe_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_accepts_minepilot_can_probe_structured_evidence tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_sender_smoke_missing_startup_banner tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_socket_probe_script_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_sender_build_dir_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_sender_smoke_interface_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_sender_smoke_timeout_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_sender_smoke_executable_mismatch tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_minepilot_sender_build_target_mismatch
```

Expected: PASS.

Observed: PASS, 9 tests.

- [x] **Step 2: Run full validation**

Run:

```bash
python3 -m unittest tests.test_deployment_templates
python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-minepilot-can-evidence-validation.md
python3 scripts/check.py
```

Expected: all pass.

Observed:
- `python3 -m unittest tests.test_deployment_templates`: PASS, 119 tests.
- `python3 -m py_compile mine_teleop/deployment_validation.py scripts/target_host_validation_report.py tests/test_deployment_templates.py`: PASS.
- `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-minepilot-can-evidence-validation.md`: PASS.
- `python3 scripts/check.py`: PASS, 441 tests.

### Self-Review

- Spec coverage: Covers the target-host MinePilot CAN evidence requirement from `docs/11-testing-and-validation.md` and `docs/12-operations-and-troubleshooting.md`.
- Placeholder scan: No placeholders.
- Type consistency: Failure payload matches existing evidence failure dictionaries and uses stringified `actual` counts like nearby validation rules.
