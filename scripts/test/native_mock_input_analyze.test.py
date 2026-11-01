import importlib.util
import pathlib
import unittest

path = pathlib.Path(__file__).with_name("native_mock_input_analyze.py")
spec = importlib.util.spec_from_file_location("native_mock_input_analyze", path)
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class InputEvidenceTest(unittest.TestCase):
    def setUp(self):
        self.metadata = {"periodic_input": [{"scenario": "forward-left", "started_utc": "2026-01-01T00:00:00Z",
                                             "ended_utc": "2026-01-01T00:00:05Z"}]}
        self.viewer = [{"event": "native_mock_input_batch", "session_id": "s", "dropped": 0,
                        "records": [{"wall_utc_ms": 1767225602000, "keys": 5, "intent_seq": 3, "gear": "D"}]}]
        self.command = {"stage": "receive_apply", "accepted": True, "session_id": "s", "intent_seq": 3,
                        "requested_gear": "D", "requested_throttle": 0, "requested_steering": 0,
                        "effective_throttle": 0, "effective_steering": 0, "transport": "dc"}

    def analyze(self):
        return module.analyze(self.metadata, self.viewer, [{"commands": [self.command]}])

    def test_matches_zero_limited_keys_to_vehicle_sequence(self):
        self.assertTrue(self.analyze()["all_windows_matched"])
        self.assertFalse(self.analyze()["all_nine_scenarios_covered"])
        self.command["intent_seq"] = 4
        self.assertFalse(self.analyze()["all_windows_matched"])

    def test_rejects_wrong_session_and_nonzero_motion(self):
        self.command["session_id"] = "other"
        self.assertFalse(self.analyze()["all_windows_matched"])
        self.command["session_id"] = "s"
        self.command["effective_steering"] = .1
        self.assertFalse(self.analyze()["all_windows_matched"])

    def test_dropped_trace_is_not_accepted(self):
        self.viewer[0]["dropped"] = 1
        with self.assertRaises(ValueError):
            self.analyze()

    def test_brake_requires_a_brake_command(self):
        self.metadata["periodic_input"][0]["scenario"] = "brake"
        self.viewer[0]["records"][0]["keys"] = 16
        self.assertFalse(self.analyze()["all_windows_matched"])
        self.command["requested_brake"] = .3
        self.assertTrue(self.analyze()["all_windows_matched"])


if __name__ == "__main__":
    unittest.main()
