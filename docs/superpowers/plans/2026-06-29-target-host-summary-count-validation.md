# Target Host Summary Count Validation

## Goal

Reject target-host validation summaries whose count fields use booleans instead
of real integers.

## Acceptance

- RED: a focused report test proves boolean count fields are accepted when they
  compare equal to `1` or `0`.
- GREEN: the report returns exit code 2 and records `summary_mismatch` for each
  boolean count field.
- Neighbor target-host summary consistency tests still pass.
- Full `python3 scripts/check.py` passes.

## Scope

- In scope: target-host summary count consistency validation and focused tests.
- Out of scope: changing result record schemas, command generation, artifact
  paths, or ChassisControl/MinePilot runtime behavior.

## Plan

1. [x] Add a failing test for boolean summary count fields.
2. [x] Run the focused test and confirm the false positive.
3. [x] Reject bool values in the existing count comparison.
4. [x] Run focused and neighbor tests.
5. [x] Run full local validation and hygiene checks.

## Evidence

- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_boolean_summary_counts`
  failed because the report returned 0 with empty `consistency_failed`.
- GREEN: the same focused test passed after rejecting bool count values.
- Neighbor tests: six target-host summary consistency tests passed.
- Deployment template suite: `python3 -m unittest tests.test_deployment_templates`
  passed 112 tests.
- Full suite: `python3 scripts/check.py` passed 434 tests.
- Hygiene: `python3 -m py_compile ...` and `git diff --check ...` passed.
