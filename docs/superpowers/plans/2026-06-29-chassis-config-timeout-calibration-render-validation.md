# Chassis Config Timeout Calibration Render Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `render_chassis_vehicle_config.py` reject generated real-adapter configs whose timeout calibration cap is lower than the configured `control_timeout_ms`.

**Architecture:** Keep the validation at the render-script boundary, next to the existing timeout calibration injection. The base config still remains the source of `control.control_timeout_ms`; the script checks the injected `max_control_timeout_ms` before emitting YAML so it cannot produce a config that the loader later rejects.

**Tech Stack:** Python standard library, PyYAML, `unittest`, existing vehicle config loader tests.

---

## Task 1: Render-Time Timeout Calibration Guard

**Files:**
- Modify: `scripts/render_chassis_vehicle_config.py`
- Modify: `tests/test_config_contract.py`
- Modify: `docs/superpowers/plans/2026-06-29-chassis-config-timeout-calibration-render-validation.md`

- [x] **Step 1: Write the failing test**
  - Add a test that runs `scripts/render_chassis_vehicle_config.py` against `configs/vehicle-agent.dev.yaml` with `--max-control-timeout-ms 700`.
  - Expected: the command exits non-zero and stderr says `control.control_timeout_ms exceeds calibrated maximum`.

- [x] **Step 2: Run test to verify it fails**
  - Run: `python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_timeout_calibration_below_control_timeout`
  - Expected: FAIL with `AssertionError: 0 != 0`, proving the script emitted an invalid config.
  - RED result: failed with `AssertionError: 0 == 0`, proving the script returned 0 for a calibration cap below the base `control_timeout_ms`.

- [x] **Step 3: Write minimal implementation**
  - In `_inject_timeout_calibration`, after confirming the control section is a mapping, compare integer `control_timeout_ms` with `max_control_timeout_ms`.
  - Raise `ValueError("control.control_timeout_ms exceeds calibrated maximum")` when the cap is too low.

- [x] **Step 4: Run focused and adjacent validation**
  - Run the focused timeout-calibration render test again.
  - Run adjacent chassis config render tests.
  - Focused timeout-calibration render test: passed.
  - Adjacent render/config calibration tests: 6 passed.

- [x] **Step 5: Run full validation**
  - Run `python3 -m unittest tests.test_config_contract.ConfigContractTests`.
  - Run py_compile, diff check, and `python3 scripts/check.py`.
  - `python3 -m unittest tests.test_config_contract.ConfigContractTests`: 49 passed.
  - `python3 -m py_compile scripts/render_chassis_vehicle_config.py tests/test_config_contract.py`: PASS.
  - `git diff --check -- scripts/render_chassis_vehicle_config.py tests/test_config_contract.py docs/superpowers/plans/2026-06-29-chassis-config-timeout-calibration-render-validation.md`: PASS.
  - `python3 scripts/check.py`: 462 passed.

## Review Lanes

- Reuse: Reuse the existing config-loader error message and render-script timeout injection location.
- Quality: Fail before writing invalid target-host YAML.
- Efficiency: Adds one integer comparison during render.

## Post-Implementation Review

- Reuse: The render script now uses the same `control.control_timeout_ms exceeds calibrated maximum` contract as the config loader.
- Quality: The script no longer emits a real-adapter YAML that is known to violate timeout calibration before target-host validation.
- Efficiency: The check is a single local comparison against the already loaded base config.
- Regression risk: Existing successful C shim rendering and missing-file failures still pass adjacent tests.
