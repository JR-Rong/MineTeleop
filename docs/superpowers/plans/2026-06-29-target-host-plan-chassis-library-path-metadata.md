# Target Host Plan Chassis Library Path Metadata Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make target-host validation plan and artifact summary metadata consistently expose `chassis_control_library_path`.

**Architecture:** Keep the existing `--chassis-control-library` CLI and bridge-check command unchanged. Normalize the metadata emitted by `TargetHostValidationPlan` so plan JSONL and artifact summaries both carry the same field name that the report verifier already uses.

**Tech Stack:** Python standard library, `unittest`, existing deployment validation helpers.

---

## Task 1: Chassis Library Path Metadata

**Files:**
- Modify: `mine_teleop/deployment_validation.py`
- Modify: `tests/test_deployment_templates.py`
- Modify: `docs/superpowers/plans/2026-06-29-target-host-plan-chassis-library-path-metadata.md`

- [x] **Step 1: Write failing tests**
  - Add an assertion that `TargetHostValidationPlan.default().to_jsonl()` first record includes `chassis_control_library_path`.
  - Add an assertion that artifact shell summaries include `chassis_control_library_path` even before bridge stdout enrichment.

- [x] **Step 2: Run tests to verify they fail**
  - Run focused deployment template tests.
  - Expected: FAIL with missing `chassis_control_library_path`.
  - RED result: failed with `KeyError: 'chassis_control_library_path'` for both plan JSONL and artifact shell summary.

- [x] **Step 3: Implement minimal metadata normalization**
  - Store `chassis_control_library_path` in the plan context.
  - Remove or avoid the older unqualified `chassis_control_library` metadata key in emitted metadata.
  - Keep command construction using the existing local variable and CLI flag.

- [x] **Step 4: Run focused and adjacent validation**
  - Run the focused deployment template tests again.
  - Run adjacent target-host plan/report tests.
  - Focused GREEN result: 2 tests passed.
  - Adjacent `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`: 130 passed.

- [x] **Step 5: Run full validation**
  - Run `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests`.
  - Run py_compile, diff check, and `python3 scripts/check.py`.
  - `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-plan-chassis-library-path-metadata.md`: PASS.
  - `python3 scripts/check.py`: 463 passed.

## Review Lanes

- Reuse: Use the existing `_summary_metadata()` path instead of introducing a second metadata formatter.
- Quality: Keep one reader-facing field name aligned with docs and report validation.
- Efficiency: No new command execution, filesystem reads, or target-host steps.

## Post-Implementation Review

- Reuse: `TargetHostValidationPlan._summary_metadata()` now carries the same `chassis_control_library_path` field used by the report verifier.
- Quality: Plan JSONL and artifact summaries no longer diverge from the documented summary field name.
- Efficiency: The change is metadata-only and does not add any target-host command, filesystem probe, or external checkout operation.
- Regression risk: Existing target-host plan, shell, and report tests passed as a group.
