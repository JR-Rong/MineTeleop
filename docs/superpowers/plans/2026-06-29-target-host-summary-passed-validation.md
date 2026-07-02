# Target Host Summary Passed Validation

## Goal

Make the target-host validation report reject malformed archive summaries whose
overall `passed` field is missing or not a boolean.

## Acceptance

- RED: a focused report test proves a summary with `"passed": "true"` is
  currently accepted.
- GREEN: the report returns exit code 2 and records a `summary_invalid`
  consistency failure for the malformed `passed` field.
- Existing target-host summary consistency tests still pass.
- Full `python3 scripts/check.py` passes.

## Scope

- In scope: target-host archive summary consistency validation and focused tests.
- Out of scope: changing generated target-host shell command behavior, CAN
  runtime probes, ChassisControl/MinePilot source checks, or production adapter
  control paths.

## Plan

1. [x] Add a failing test for a non-boolean summary `passed` value.
2. [x] Run the focused test and confirm it fails for the expected reason.
3. [x] Add minimal consistency validation for `passed`.
4. [x] Run the focused target-host tests.
5. [x] Run the full local validation suite and diff hygiene checks.

## Evidence

- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_invalid_summary_passed_field`
  returned 0 before the fix was expected but failed the assertion with report
  `passed=true` and empty `consistency_failed`.
- GREEN: the same focused test passed after the fix.
- Neighbor tests: five target-host summary consistency tests passed.
- Deployment template suite: `python3 -m unittest tests.test_deployment_templates`
  passed 111 tests.
- Full suite: `python3 scripts/check.py` passed 433 tests.
- Hygiene: `python3 -m py_compile ...` and `git diff --check ...` passed.
- External checkout smoke: ChassisControl `UI_Test` and MinePilot
  `merge_ui_test` roots are present; branch/source/library checks are ready.
- Platform note: host CMake build fails on macOS because MinePilot CAN sources
  reject unsupported platforms; Docker is installed, but the current Docker
  file sharing exposes `/Volumes/SystemDisk/Workspace/...` mounts as empty
  directories, so a Linux container build could not be completed here.
