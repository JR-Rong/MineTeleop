# Target Host Command Requirements Summary Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host validation archives that omit commands promised by the generated plan summary.

**Architecture:** Keep the validation in `TargetHostValidationArchive.consistency_failed`. Have `TargetHostValidationPlan` emit a `command_requirements` map in plan JSONL and artifact summaries, then make the report compare that map against actual result records when the map is present.

**Tech Stack:** Python standard library, `unittest`, JSONL target-host validation archives.

---

## Task 1: Summary-Carried Command Requirements

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`
- Modify: `docs/11-testing-and-validation.md`
- Modify: `docs/12-operations-and-troubleshooting.md`

- [x] **Step 1: Write the failing test**
  - Add a target-host archive where result records contain only `vehicle.preflight`, while summary `command_requirements` also declares `chassis.bridge.check` as required.
  - Expected: report exits 2 with `summary_command_missing`.

- [x] **Step 2: Run test to verify it fails**
  - Run: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_missing_command_from_summary_requirements`
  - Expected: FAIL with `AssertionError: 0 != 2`.
  - RED result: failed with `AssertionError: 0 != 2`, proving a truncated self-consistent archive was accepted.

- [x] **Step 3: Write minimal implementation**
  - Add `command_requirements` to generated plan metadata.
  - In archive consistency validation, when `command_requirements` exists, reject missing result records and required/optional mismatches.

- [x] **Step 4: Run focused and adjacent validation**
  - Focused command-requirements test: passed.
  - Adjacent plan JSONL, artifact shell, artifact summary, required downgrade, and command_names tests: 6 passed.

- [x] **Step 5: Run full validation**
  - Run `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`.
  - Run py_compile, diff check, and `python3 scripts/check.py`.
  - `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 129 passed.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/11-testing-and-validation.md docs/12-operations-and-troubleshooting.md docs/superpowers/plans/2026-06-29-target-host-command-requirements-summary-validation.md`: PASS.
  - `python3 scripts/check.py`: 460 passed.

## Review Lanes

- Reuse: Reuse existing summary/result consistency validation and JSONL report surface.
- Quality: Keep `command_requirements` optional so minimal local archive tests remain compatible, while generated target-host artifacts carry the full plan contract.
- Efficiency: Adds one dictionary construction and one pass over summary requirements per report.

## Post-Implementation Review

- Reuse: Added summary-carried command coverage checks in the existing `consistency_failed` lane instead of creating a second report path.
- Quality: Generated target-host artifacts now preserve both actual result order (`command_names`) and planned required/optional expectations (`command_requirements`).
- Efficiency: The report builds a single actual-name-to-required map and compares it with the summary requirement map once.
- Regression risk: Minimal hand-built archive tests without `command_requirements` remain supported; generated plan JSONL and artifact summaries now assert the new field explicitly.
