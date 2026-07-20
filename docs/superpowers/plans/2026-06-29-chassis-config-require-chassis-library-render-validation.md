# Chassis Config Required Chassis Library Render Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `render_chassis_vehicle_config.py` require the actual linked `libchassis_control` path so generated target-host configs always record the lower-level ChassisControl library behind the C shim.

**Architecture:** Keep this guard at the render CLI boundary. The bridge library remains the Python-loaded C shim; the chassis-control library is the dynamic library that the shim links and that target-host evidence must preserve for report correlation.

**Tech Stack:** Python standard library, PyYAML, `unittest`, existing vehicle config loader tests.

---

## Task 1: Require ChassisControl Library During Render

**Files:**
- Modify: `scripts/render_chassis_vehicle_config.py`
- Modify: `tests/test_config_contract.py`
- Modify: `docs/superpowers/plans/2026-06-29-chassis-config-require-chassis-library-render-validation.md`

- [x] **Step 1: Write the failing test**
  - Add a test that runs `scripts/render_chassis_vehicle_config.py` with valid fake ChassisControl/MinePilot roots and bridge library but omits `--chassis-control-library`.
  - Expected: argparse exits with code 2 and reports `--chassis-control-library` as required.

- [x] **Step 2: Run test to verify it fails**
  - Run: `python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_requires_chassis_control_library`
  - Expected: FAIL because the current script accepts the missing argument and emits YAML without `library_path`.
  - RED result: failed with `AssertionError: 0 != 2`, proving the script accepted the missing library argument.

- [x] **Step 3: Write minimal implementation**
  - Mark `--chassis-control-library` as required.
  - Validate it with the existing `_existing_file_arg`.
  - Always write `integration.chassis_control.library_path` into the generated adapter config.

- [x] **Step 4: Run focused and adjacent validation**
  - Run the focused required-library test again.
  - Run adjacent chassis config render tests that cover successful render, missing bridge/library files, timeout calibration, and MinePilot CAN sender source validation.
  - Focused required-library render test: passed.
  - Adjacent render/config tests: 6 passed.

- [x] **Step 5: Run full validation**
  - Run `python3 -m unittest tests.test_config_contract.ConfigContractTests`.
  - Run py_compile, diff check, and `python3 scripts/check.py`.
  - `python3 -m unittest tests.test_config_contract.ConfigContractTests`: 50 passed.
  - First `python3 scripts/check.py`: failed because `test_vehicle_agent_default_demo_refuses_real_adapter_config` still used the old render CLI contract without `--chassis-control-library`.
  - Updated that adjacent test fixture with a fake `libchassis_control.so` and the required render argument.
  - `python3 -m unittest tests.test_design_behaviors.CommandLineEntryPointTests.test_vehicle_agent_default_demo_refuses_real_adapter_config`: passed.
  - `python3 -m py_compile scripts/render_chassis_vehicle_config.py tests/test_config_contract.py tests/test_design_behaviors.py`: PASS.
  - `git diff --check -- scripts/render_chassis_vehicle_config.py tests/test_config_contract.py tests/test_design_behaviors.py docs/superpowers/plans/2026-06-29-chassis-config-require-chassis-library-render-validation.md`: PASS.
  - Final `python3 scripts/check.py`: 463 passed.
  - `python3 scripts/chassis_bridge_check.py --chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl --minepilot-root /Volumes/SystemDisk/Workspace/MinePilot --chassis-control-library /Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so --build-dir build/chassis-control-bridge`: 22 checks ready, including ChassisControl `UI_Test`, MinePilot `merge_ui_test`, MinePilot CAN headers/sources, selected library symbols, and CMake configure.

## External Checkout Investigation

- ChassisControl checkout: `/Volumes/SystemDisk/Workspace/ChassisControl`, branch `UI_Test`, HEAD `886b20611d867b09e6d4a9589656fcf465086aec`, dirty with 14 changed paths.
- MinePilot checkout: `/Volumes/SystemDisk/Workspace/MinePilot`, branch `merge_ui_test`, HEAD `12d18cd995c7f7e9ecb3e19a857dc7ec7c3e6867`, dirty with 4 changed paths.
- MinePilot provides the required low-level CAN integration files: `include/can/can_common.h`, `include/can/can_message.h`, `include/can_db.h`, `include/can_receiver.h`, `include/can_sender.h`, `src/can_db.cpp`, `src/can_receiver.cpp`, and `src/can_sender.cpp`.
- `/Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so` exists and exports the required ChassisControl symbols checked by `scripts/chassis_bridge_check.py`.

## Review Lanes

- Reuse: Reuse existing argparse and path validation helpers.
- Quality: Fail before producing target-host YAML that cannot be tied back to the linked ChassisControl library.
- Efficiency: Adds only CLI required-argument validation and removes optional branching.

## Post-Implementation Review

- Reuse: The script keeps the existing `_existing_file_arg` validation path and the surrounding config-rendering pattern.
- Quality: Generated real-adapter YAML now always includes `integration.chassis_control.library_path`, aligning with target-host report correlation.
- Efficiency: No extra filesystem traversal was added beyond validating the already supplied dynamic library path.
- Regression risk: The broader CLI test fixture needed the same required argument; after updating it, the full local suite passed.
