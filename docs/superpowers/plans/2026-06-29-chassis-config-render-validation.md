# Chassis Config Render Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ensure the ChassisControl vehicle config renderer only emits configs that satisfy the runtime vehicle adapter contract.

**Architecture:** Keep validation at the script boundary so generated `dynamic_library` configs cannot point at a missing C shim. Reuse the existing config loader contract instead of adding a second permissive interpretation of `bridge_library_path`.

**Tech Stack:** Python standard library, `unittest`, existing `mine_teleop.config` loader, existing `scripts/check.py`.

---

### Task 1: Reject Missing Bridge Library Paths

**Files:**
- Modify: `tests/test_config_contract.py`
- Modify: `scripts/render_chassis_vehicle_config.py`
- Verify: `python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_bridge_library`

- [x] **Step 1: Write the failing test**

```python
def test_render_chassis_vehicle_config_rejects_missing_bridge_library(self):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        missing_bridge = root / "missing" / "libmine_teleop_chassis_bridge.so"
        chassis_library = root / "libchassis_control.so"
        chassis_library.write_text("", encoding="utf-8")

        result = subprocess.run(
            [
                sys.executable,
                "scripts/render_chassis_vehicle_config.py",
                "--bridge-library",
                str(missing_bridge),
                "--chassis-control-library",
                str(chassis_library),
                "--max-control-timeout-ms",
                "900",
                "--calibration-evidence",
                "bench-brake-test",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    self.assertEqual(result.returncode, 2)
    self.assertIn("bridge library does not exist", result.stderr)
    self.assertEqual(result.stdout, "")
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_bridge_library
```

Expected: FAIL because the script currently returns 0 and writes YAML even when `--bridge-library` does not exist.

- [x] **Step 2b: Write and run the companion missing chassis library test**

```python
def test_chassis_vehicle_config_template_cli_rejects_missing_chassis_control_library(self):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        bridge_library = root / "libmine_teleop_chassis_bridge.so"
        missing_chassis_library = root / "missing" / "libchassis_control.so"
        bridge_library.write_text("fake bridge shared library path for config validation\n", encoding="utf-8")

        result = subprocess.run(
            [
                sys.executable,
                "scripts/render_chassis_vehicle_config.py",
                "--bridge-library",
                str(bridge_library),
                "--chassis-control-library",
                str(missing_chassis_library),
                "--max-control-timeout-ms",
                "900",
                "--calibration-evidence",
                "bench-brake-test",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    self.assertEqual(result.returncode, 2)
    self.assertIn("chassis control library does not exist", result.stderr)
    self.assertEqual(result.stdout, "")
```

Run:

```bash
python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_bridge_library tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_chassis_control_library
```

Expected: FAIL because both missing dynamic library paths are still accepted.

- [x] **Step 3: Write minimal implementation**

Add a small path validation helper in `scripts/render_chassis_vehicle_config.py` before rendering:

```python
def _existing_file_arg(parser: argparse.ArgumentParser, value: str, label: str) -> Path:
    path = Path(value)
    if not path.is_file():
        parser.error(f"{label} does not exist: {path}")
    return path
```

Use it for `args.bridge_library` and for `args.chassis_control_library` when the optional chassis library argument is provided. `argparse.ArgumentParser.error()` handles the `stderr` message and exit code 2.

- [x] **Step 4: Run test to verify it passes**

Run:

```bash
python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_bridge_library tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_chassis_control_library
```

Expected: PASS.

- [x] **Step 5: Verify existing positive path still works**

Run:

```bash
python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_generates_loadable_c_shim_config
```

Expected: PASS with fixture-created bridge and chassis libraries.

- [x] **Step 6: Run repo validation**

Run:

```bash
python3 scripts/check.py
git diff --check -- scripts/render_chassis_vehicle_config.py tests/test_config_contract.py docs/superpowers/plans/2026-06-29-chassis-config-render-validation.md
```

Expected: all tests pass and diff check has no output.

### Task 2: Reject Missing Generated Chassis/MinePilot Paths

**Files:**
- Modify: `tests/test_config_contract.py`
- Modify: `scripts/render_chassis_vehicle_config.py`
- Verify: `python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_minepilot_can_sender_source`

- [x] **Step 1: Write the failing test**

```python
def test_chassis_vehicle_config_template_cli_rejects_missing_minepilot_can_sender_source(self):
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        chassis_root = root / "ChassisControl"
        minepilot_root = root / "MinePilot"
        bridge_library = root / "libmine_teleop_chassis_bridge.so"
        chassis_library = root / "libchassis_control.so"
        (chassis_root / "include" / "can").mkdir(parents=True)
        (minepilot_root / "include" / "can").mkdir(parents=True)
        (minepilot_root / "src").mkdir(parents=True)
        (chassis_root / "chassis_control.h").write_text("bool Initialize();\n", encoding="utf-8")
        (chassis_root / "include" / "can" / "can_common.h").write_text("int can_open();\n", encoding="utf-8")
        (minepilot_root / "include" / "can" / "can_common.h").write_text("struct CanFrame {};\n", encoding="utf-8")
        (minepilot_root / "include" / "can" / "can_message.h").write_text("struct CanMessage {};\n", encoding="utf-8")
        (minepilot_root / "include" / "can_db.h").write_text("#define CAN_DB_ADU_TX_VEH_SPD_FRAME_ID 0x18ff\n", encoding="utf-8")
        (minepilot_root / "include" / "can_receiver.h").write_text("struct DecodedCanData {};\n", encoding="utf-8")
        (minepilot_root / "include" / "can_sender.h").write_text("class MinePilotCanSender {};\n", encoding="utf-8")
        (minepilot_root / "src" / "can_db.cpp").write_text("// CAN db source\n", encoding="utf-8")
        (minepilot_root / "src" / "can_receiver.cpp").write_text("// CAN receiver source\n", encoding="utf-8")
        bridge_library.write_text("fake bridge shared library path for config validation\n", encoding="utf-8")
        chassis_library.write_text("fake chassis shared library path for config validation\n", encoding="utf-8")

        result = subprocess.run(
            [
                sys.executable,
                "scripts/render_chassis_vehicle_config.py",
                "--chassis-control-root",
                str(chassis_root),
                "--minepilot-root",
                str(minepilot_root),
                "--bridge-library",
                str(bridge_library),
                "--chassis-control-library",
                str(chassis_library),
                "--max-control-timeout-ms",
                "900",
                "--calibration-evidence",
                "bench-brake-test",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

    self.assertEqual(result.returncode, 2)
    self.assertIn("MinePilot CAN sender source does not exist", result.stderr)
    self.assertEqual(result.stdout, "")
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_minepilot_can_sender_source
```

Expected: FAIL because the script currently returns 0 and writes YAML with a missing `src/can_sender.cpp` path.

- [x] **Step 3: Write minimal implementation**

Add path checks for every generated ChassisControl and MinePilot contract path before `_vehicle_adapter_config()` renders the YAML. Reuse a small helper:

```python
def _required_file(parser: argparse.ArgumentParser, path: Path, label: str) -> Path:
    if not path.is_file():
        parser.error(f"{label} does not exist: {path}")
    return path
```

- [x] **Step 4: Run test to verify it passes**

Run:

```bash
python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_minepilot_can_sender_source
```

Expected: PASS.

- [x] **Step 5: Run focused render contract tests**

Run:

```bash
python3 -m unittest tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_generates_loadable_c_shim_config tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_bridge_library tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_chassis_control_library tests.test_config_contract.ConfigContractTests.test_chassis_vehicle_config_template_cli_rejects_missing_minepilot_can_sender_source
```

Expected: PASS.

- [x] **Step 6: Run repo validation**

Run:

```bash
python3 scripts/check.py
git diff --check -- scripts/render_chassis_vehicle_config.py tests/test_config_contract.py docs/superpowers/plans/2026-06-29-chassis-config-render-validation.md
```

Expected: all tests pass and diff check has no output.
