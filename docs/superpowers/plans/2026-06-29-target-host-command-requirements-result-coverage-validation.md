# Target Host Command Requirements Result Coverage Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host validation archives whose `command_requirements` summary omits actual result commands.

**Architecture:** Keep the check inside `TargetHostValidationArchive.consistency_failed`, next to the existing command name and summary-carried requirement checks. Treat `command_requirements` as optional for legacy/minimal hand-built archives, but when present it must cover every valid result command name and preserve each result's required/optional expectation.

**Tech Stack:** Python standard library, `unittest`, JSONL target-host validation archives.

---

## Task 1: Result-to-Requirement Coverage

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`
- Modify: `docs/superpowers/plans/2026-06-29-target-host-command-requirements-result-coverage-validation.md`

- [x] **Step 1: Write the failing test**
  - Add a target-host archive where result records contain a custom command, `command_names` matches the result list, and summary `command_requirements` is present but omits that command.
  - Expected: report exits 2 with `summary_command_requirement_missing`.

- [x] **Step 2: Run test to verify it fails**
  - Run: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_result_missing_from_summary_requirements`
  - Expected: FAIL with `AssertionError: 0 != 2`.
  - RED result: failed with `AssertionError: 0 != 2`, proving `command_requirements` could be a subset of actual results.

- [x] **Step 3: Write minimal implementation**
  - After validating `command_requirements` is a dict, compute result command names with valid boolean `required` fields.
  - Reject any actual result command missing from the requirement map.

- [x] **Step 4: Run focused and adjacent validation**
  - Run the focused test again.
  - Run adjacent command-requirements and summary command-name tests.
  - Focused result-coverage test: passed.
  - Adjacent requirements, downgrade, command names, artifact summary, and plan JSONL tests: 6 passed.

- [x] **Step 5: Run full validation**
  - Run `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`.
  - Run py_compile, diff check, and `python3 scripts/check.py`.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 130 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-command-requirements-result-coverage-validation.md`: PASS.
  - `python3 scripts/check.py`: 461 passed.

## Review Lanes

- Reuse: Reuse the existing `consistency_failed` summary/result validation surface.
- Quality: Keep legacy summary compatibility by enforcing this only when `command_requirements` is present.
- Efficiency: Reuse the existing actual requirement map built during summary requirement validation.

## Post-Implementation Review

- Reuse: Added the reverse coverage check inside the existing summary-carried requirement validation.
- Quality: A generated or self-describing archive can no longer omit a result command from `command_requirements` to bypass custom required/optional expectations.
- Efficiency: The implementation reuses the already built `actual_requirements` dict and performs one membership pass.
- Regression risk: Archives without `command_requirements` retain the existing compatibility path; generated target-host artifacts include the field and are now checked both ways.
