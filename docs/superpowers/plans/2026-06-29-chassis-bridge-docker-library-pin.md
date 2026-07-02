# Chassis Bridge Docker Library Pin Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve explicit `--chassis-control-library` selection in the Linux Docker bridge-build command plan.

**Architecture:** Keep the existing Docker command plan shape and mounted checkout layout. Translate a host library path under mine-teleop, ChassisControl, or MinePilot into the corresponding `/workspace/...` container path, then pass it to CMake as `-DCHASSIS_CONTROL_LIBRARY`.

**Tech Stack:** Python standard library, `unittest`, existing chassis bridge check CLI.

---

## Task 1: Docker Command Library Pin

**Files:**
- Modify: `mine_teleop/chassis_bridge_check.py`
- Modify: `scripts/chassis_bridge_check.py`
- Modify: `tests/test_deployment_templates.py`
- Modify: `docs/12-operations-and-troubleshooting.md`
- Create: `docs/superpowers/plans/2026-06-29-chassis-bridge-docker-library-pin.md`

- [x] **Step 1: Write failing test**
  - Update `test_chassis_bridge_check_cli_can_print_linux_docker_build_command` to pass `--chassis-control-library <MinePilot>/libchassis_control.so`.
  - Assert the generated Docker `run_command` contains `-DCHASSIS_CONTROL_LIBRARY=/workspace/MinePilot/libchassis_control.so`.

- [x] **Step 2: Run test to verify RED**
  - Run `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_chassis_bridge_check_cli_can_print_linux_docker_build_command`.
  - RED result: failed because the generated `run_command` only included `CHASSIS_CONTROL_ROOT` and `MINEPILOT_ROOT`.

- [x] **Step 3: Implement minimal path mapping**
  - Add an optional `chassis_control_library` parameter to `build_chassis_bridge_docker_command_plan`.
  - Map host paths under the mounted mine-teleop, ChassisControl, or MinePilot roots to their container paths.
  - Pass the mapped value as `-DCHASSIS_CONTROL_LIBRARY`.
  - Forward the CLI `--chassis-control-library` argument into the Docker command plan.

- [x] **Step 4: Run focused GREEN**
  - Run `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_chassis_bridge_check_cli_can_print_linux_docker_build_command`.
  - GREEN result: 1 test passed.

- [x] **Step 5: Run adjacent and full validation**
  - Run adjacent chassis bridge CLI/deployment template tests.
  - Run `python3 -m py_compile` for changed Python files.
  - Run `git diff --check`.
  - Run `python3 scripts/check.py`.
  - Adjacent result: 4 chassis bridge deployment-template tests passed.
  - Broader result: `ContainerTemplateTests` 130 passed; `ChassisControlIntegrationTests` 25 passed.
  - `python3 -m py_compile mine_teleop/chassis_bridge_check.py scripts/chassis_bridge_check.py tests/test_deployment_templates.py`: PASS.
  - `git diff --check -- mine_teleop/chassis_bridge_check.py scripts/chassis_bridge_check.py tests/test_deployment_templates.py docs/12-operations-and-troubleshooting.md docs/superpowers/plans/2026-06-29-chassis-bridge-docker-library-pin.md`: PASS.
  - `python3 scripts/check.py`: 463 passed.

## Review Lanes

- Reuse: Use the existing Docker command plan generator and CLI flag instead of adding a second Docker path.
- Quality: Keep the path mapping local to the bridge-check module and only affect explicit library selection.
- Efficiency: No Docker daemon call is introduced; the command plan remains a dry JSONL output.

## Post-Implementation Review

- Reuse: The existing `--chassis-control-library` CLI flag now feeds both direct checks and Docker command planning.
- Quality: The generated Docker CMake invocation now pins the same selected ChassisControl/MinePilot dynamic library as the host-side check.
- Efficiency: The change only rewrites the dry-run command string and does not add any Docker daemon, filesystem, or target CAN operation.
- Regression risk: Full local validation passed; real bridge build still requires target Ubuntu or Docker linux/amd64 because MinePilot CAN headers reject macOS.
