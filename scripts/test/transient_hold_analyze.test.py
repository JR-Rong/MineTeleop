import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("transient_hold_analyze.py")


class TransientHoldAnalyzeTest(unittest.TestCase):
    def run_script(self, lines):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "vehicle-runtime.log"
            log.write_text("\n".join(lines) + "\n", encoding="utf-8")
            output = subprocess.run([sys.executable, str(SCRIPT), str(log)], check=True, capture_output=True, text=True)
            return json.loads(output.stdout)

    def test_summarizes_holds_escalations_and_hard_stops(self):
        events = [
            {"event": "vehicle_transient_hold_entered", "causes": ["control_gap"]},
            {"event": "vehicle_transient_hold_resumed", "last": {"duration_ms": 120, "outcome": "resumed"}},
            {"event": "vehicle_transient_hold_entered", "causes": ["late_commands"]},
            {"event": "vehicle_transient_hold_escalated", "last": {"duration_ms": 700, "outcome": "escalated",
                                                                  "escalation_reason": "hold_duration_exceeded"}},
            {"event": "control_hard_stop_transition"},
        ]
        lines = [json.dumps(event) for event in events] + ["not json", ""]
        summary = self.run_script(lines)
        self.assertEqual(summary["holds_entered"], 2)
        self.assertEqual(summary["resumed"], 1)
        self.assertEqual(summary["escalated"], 1)
        self.assertEqual(summary["escalation_reasons"], {"hold_duration_exceeded": 1})
        self.assertEqual(summary["hard_stops"], 1)
        self.assertEqual(summary["resumed_duration_ms"]["max"], 120)
        self.assertEqual(summary["ignored_lines"], 1)

    def run_files(self, *contents):
        with tempfile.TemporaryDirectory() as directory:
            paths = []
            for index, lines in enumerate(contents):
                path = Path(directory) / f"runtime-{index}.log"
                path.write_text("\n".join(lines) + "\n", encoding="utf-8")
                paths.append(str(path))
            output = subprocess.run([sys.executable, str(SCRIPT), *paths], check=True, capture_output=True, text=True)
            return json.loads(output.stdout)

    def test_merges_files_and_reports_percentiles_causes_and_unknown_reasons(self):
        def resumed(duration):
            return json.dumps({"event": "vehicle_transient_hold_resumed", "last": {"duration_ms": duration, "outcome": "resumed"}})
        first = [json.dumps({"event": "vehicle_transient_hold_entered", "causes": ["control_gap", "driver_video_stale"]})]
        first += [resumed(duration) for duration in (100, 200, 300)]
        second = [resumed(400), resumed(700),
                  json.dumps({"event": "vehicle_transient_hold_escalated", "last": {"duration_ms": 700, "outcome": "escalated"}}),
                  "123", "null", "[1, 2]", json.dumps({"event": "something_else"})]
        summary = self.run_files(first, second)
        self.assertEqual(summary["holds_entered"], 1)
        self.assertEqual(summary["resumed"], 5)
        self.assertEqual(summary["causes"], {"control_gap": 1, "driver_video_stale": 1})
        self.assertEqual(summary["escalation_reasons"], {"unknown": 1})
        self.assertEqual(summary["resumed_duration_ms"], {"count": 5, "p50": 300, "p95": 700, "max": 700})
        self.assertEqual(summary["ignored_lines"], 0)

    def test_empty_logs_report_null_statistics(self):
        summary = self.run_files(["not json at all"])
        self.assertEqual(summary["holds_entered"], 0)
        self.assertEqual(summary["resumed_duration_ms"], {"count": 0, "p50": None, "p95": None, "max": None})
        self.assertEqual(summary["ignored_lines"], 1)


if __name__ == "__main__":
    unittest.main()
