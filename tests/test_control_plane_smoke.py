from __future__ import annotations

import unittest

from scripts import control_plane_smoke


class ControlPlaneSmokeSummaryTests(unittest.TestCase):
    def test_control_summary_reports_vehicle_steering_acceleration_and_deceleration(self):
        payloads = [
            {"seq": 1, "steering": 0.0, "throttle": 0.5, "brake": 0.0, "authority_token": "tok"},
            {"seq": 2, "steering": -0.25, "throttle": 0.6, "brake": 0.0, "authority_token": "tok"},
            {"seq": 3, "steering": 0.1, "throttle": 0.0, "brake": 0.35, "authority_token": "tok"},
        ]

        summarize = getattr(control_plane_smoke, "_summarize_control_payloads", None)
        self.assertIsNotNone(summarize)
        summary = summarize(payloads, software_count=1, gamepad_count=2)

        self.assertTrue(summary["vehicle_received_steering"])
        self.assertTrue(summary["vehicle_received_acceleration"])
        self.assertTrue(summary["vehicle_received_deceleration"])
        self.assertEqual(summary["vehicle_received_commands"], 3)
        self.assertEqual(summary["seq"], [1, 2, 3])
        self.assertEqual(summary["steering_values"], [0.0, -0.25, 0.1])
        self.assertEqual(summary["throttle_values"], [0.5, 0.6, 0.0])
        self.assertEqual(summary["brake_values"], [0.0, 0.0, 0.35])
        self.assertTrue(summary["authority_token_present"])


if __name__ == "__main__":
    unittest.main()
