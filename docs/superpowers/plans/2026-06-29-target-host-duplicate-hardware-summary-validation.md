# Target Host Duplicate Hardware Summary Validation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Reject target-host hardware encoding report archives that contain more than one `hardware_encoding_validation` summary record.

**Architecture:** Keep the check inside `TargetHostValidationArchive.evidence_failed` through `_hardware_report_evidence_failure()`. The hardware report command is expected to emit one validation summary plus lane and metrics records; duplicate summaries make it possible for a passing summary to hide a later failed summary in the same stdout.

**Tech Stack:** Python standard library, `unittest`, existing target-host validation report CLI.

---

### Task 1: Duplicate Hardware Encoding Summary

**Files:**
- Modify: `tests/test_deployment_templates.py`
- Modify: `mine_teleop/deployment_validation.py`

- [x] **Step 1: Write the failing test**

Add `test_target_host_validation_report_rejects_duplicate_hardware_encoding_validation_summary` near the existing hardware report validation tests. The stdout should include one passing `hardware_encoding_validation`, one failing duplicate summary, one valid lane, and one valid metrics record.

```python
    def test_target_host_validation_report_rejects_duplicate_hardware_encoding_validation_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "media.hardware.report.template.stdout.log"
            stderr_path = root / "media.hardware.report.template.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps(record, sort_keys=True)
                    for record in (
                        {
                            "event": "hardware_encoding_validation",
                            "scenario": "four-camera-realtime-720p30",
                            "passed": True,
                            "lane_count": 1,
                            "failures": [],
                        },
                        {
                            "event": "hardware_encoding_validation",
                            "scenario": "four-camera-realtime-720p30",
                            "passed": False,
                            "lane_count": 1,
                            "failures": ["later failed duplicate summary"],
                        },
                        {
                            "event": "hardware_encoding_lane",
                            "scenario": "four-camera-realtime-720p30",
                            "lane_id": "front-realtime-720p30",
                            "codec_name": "h264",
                            "width": 1280,
                            "height": 720,
                            "fps": 30.0,
                            "bitrate_kbps": 3000,
                            "passed": True,
                            "failures": [],
                        },
                        {
                            "event": "hardware_encoding_metrics",
                            "scenario": "four-camera-realtime-720p30",
                            "metrics": {
                                "bitrate_kbps": 3000,
                                "cpu_percent": 42.5,
                                "disk_write_mb_s": 24.0,
                                "dropped_frames": 0,
                                "encoded_fps": 30.0,
                                "gpu_percent": 71.0,
                                "memory_mb": 1536.0,
                                "temperature_c": 62.0,
                            },
                        },
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "media.hardware.report.template",
                    "required": False,
                    "returncode": 0,
                    "command": "vehicle-media-agent --mode hardware-report",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 0,
                    "optional_count": 1,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["media.hardware.report.template"],
                    "passed": True,
                },
            ]
            results_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in records),
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/target_host_validation_report.py",
                    "--results",
                    str(results_path),
                    "--verify-artifacts",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["evidence_failed"],
            [
                {
                    "actual": "2",
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_validation_duplicate",
                    "stdout_path": str(stdout_path),
                }
            ],
        )
```

- [x] **Step 2: Run test to verify it fails**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_hardware_encoding_validation_summary
```

Expected: FAIL because the report currently accepts the first passing summary and ignores the later duplicate failure.

- [x] **Step 3: Write minimal implementation**

Add a duplicate check before `_hardware_summary_failure()` trusts the first summary:

```python
def _hardware_summary_duplicate_failure(records: tuple[dict[str, Any], ...], stdout_path: str) -> dict[str, str] | None:
    summary_records = tuple(record for record in records if record.get("event") == "hardware_encoding_validation")
    if len(summary_records) <= 1:
        return None
    return {
        "actual": str(len(summary_records)),
        "name": _HARDWARE_REPORT_COMMAND_NAME,
        "reason": "hardware_encoding_validation_duplicate",
        "stdout_path": stdout_path,
    }
```

Call it from `_hardware_report_evidence_failure()` before `_hardware_summary_failure()`.

- [x] **Step 4: Run test to verify it passes**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_hardware_encoding_validation_summary
```

Expected: PASS.

- [x] **Step 5: Run adjacent validation**

Run:

```bash
python3 -m unittest \
  tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_accepts_hardware_encoding_summary_lane_and_metrics \
  tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_hardware_encoding_validation_summary \
  tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_failed_hardware_encoding_summary \
  tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_hardware_encoding_lane_count_mismatch \
  tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_hardware_encoding_metrics_missing_fields
```

Expected: all tests pass.

- [x] **Step 6: Run full validation for touched surface**

Run:

```bash
python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests
python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py
git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-summary-validation.md
python3 scripts/check.py
```

Expected: all commands pass.

## Execution Results

- RED: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests.test_target_host_validation_report_rejects_duplicate_hardware_encoding_validation_summary` failed with `AssertionError: 0 != 2`, proving the duplicate summary was accepted.
- GREEN: the same focused test passed after adding `hardware_encoding_validation_duplicate`.
- Adjacent validation: 5 hardware report archive tests passed.
- Related validation: `python3 -m unittest tests.test_deployment_templates.ContainerTemplateTests` passed, 122 tests.
- Compile validation: `python3 -m py_compile mine_teleop/deployment_validation.py tests/test_deployment_templates.py` passed.
- Diff validation: `git diff --check -- mine_teleop/deployment_validation.py tests/test_deployment_templates.py docs/superpowers/plans/2026-06-29-target-host-duplicate-hardware-summary-validation.md` passed.
- Full validation: `python3 scripts/check.py` passed, 453 tests in 55.612s.

## Review

- Reuse: Reused the existing hardware report evidence failure path and report schema.
- Quality: Added one narrow duplicate-summary guard before trusting the first summary.
- Efficiency: Kept command generation unchanged and validated through the existing deployment-template suite plus the full repo check.
