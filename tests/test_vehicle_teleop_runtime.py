from __future__ import annotations

import json
import tempfile
import unittest
from pathlib import Path
from urllib import error, request

from mine_teleop.driver_console_runtime import DriverConsoleRuntime
from mine_teleop.safety import SafetyState
from mine_teleop.signaling_service import SignalingHttpService
from mine_teleop.vehicle_teleop_runtime import VehicleTeleopRuntime


def _json_post(url: str, payload: dict) -> dict:
    body = json.dumps(payload).encode("utf-8")
    req = request.Request(url, data=body, method="POST", headers={"Content-Type": "application/json"})
    with request.urlopen(req, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


def _json_get(url: str) -> dict:
    with request.urlopen(url, timeout=5) as response:
        return json.loads(response.read().decode("utf-8"))


class VehicleTeleopRuntimeTests(unittest.TestCase):
    def test_vehicle_executes_relayed_driver_control_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )

                driver = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                )
                driver.connect(now_ms=0)

                vehicle = VehicleTeleopRuntime.from_config(
                    "configs/vehicle-agent.dev.yaml",
                    signaling_http_url=signaling_url,
                    device_token="dev-device-secret",
                )
                vehicle.register_online()
                self.assertTrue(vehicle.discover_session(now_ms=0))
                self.assertEqual(vehicle.session_id, driver.session_id)

                # Driver presses "forward + steer right" and relays it.
                sent = driver.send_control(
                    {"gear": "D", "throttle": 0.5, "steering": 0.25, "brake": 0.0},
                    now_ms=0,
                )
                self.assertIsNotNone(sent)

                result = vehicle.poll_and_execute(now_ms=10)

        self.assertEqual(result["received_control_commands"], 1)
        self.assertEqual(result["applied_control_commands"], 1)
        self.assertEqual(result["safety_state"], SafetyState.CONTROL_ACTIVE.value)
        self.assertEqual(vehicle.service.applied_command_count, 1)
        applied = vehicle.service.adapter.applied_commands[-1]
        self.assertEqual(applied.gear, "D")
        self.assertAlmostEqual(applied.throttle, 0.5)
        self.assertAlmostEqual(applied.steering, 0.25)
        self.assertEqual(applied.brake, 0.0)

    def test_vehicle_executes_estop_from_driver(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                driver = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                )
                driver.connect(now_ms=0)
                vehicle = VehicleTeleopRuntime.from_config(
                    "configs/vehicle-agent.dev.yaml",
                    signaling_http_url=signaling_url,
                    device_token="dev-device-secret",
                )
                vehicle.register_online()
                vehicle.discover_session(now_ms=0)

                driver.send_control({"gear": "N", "estop": True, "brake": 1.0}, now_ms=0)
                result = vehicle.poll_and_execute(now_ms=10)

        self.assertGreaterEqual(result["applied_control_commands"], 1)
        self.assertEqual(vehicle.service.safety.state, SafetyState.ESTOP)

    def test_vehicle_reports_control_receive_latency_and_logs(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )
                driver = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                )
                driver.connect(now_ms=0)
                vehicle = VehicleTeleopRuntime.from_config(
                    "configs/vehicle-agent.dev.yaml",
                    signaling_http_url=signaling_url,
                    device_token="dev-device-secret",
                )
                vehicle.register_online()
                vehicle.discover_session(now_ms=0)

                sent = driver.send_control(
                    {"gear": "D", "throttle": 0.25, "steering": -0.5, "brake": 0.0},
                    now_ms=1_000,
                )
                self.assertIsNotNone(sent)
                result = vehicle.poll_and_execute(now_ms=1_055)
                summary = vehicle.summary()

        self.assertEqual(result["control_latency_ms"], 55)
        self.assertEqual(len(result["control_receive_logs"]), 1)
        record = result["control_receive_logs"][0]
        self.assertEqual(record["seq"], sent.seq)
        self.assertEqual(record["receive_time_ms"], 1055)
        self.assertEqual(record["control_latency_ms"], 55)
        self.assertEqual(record["gear"], "D")
        self.assertAlmostEqual(record["steering"], -0.5)
        self.assertAlmostEqual(record["throttle"], 0.25)
        self.assertAlmostEqual(record["brake"], 0.0)
        self.assertEqual(summary["control_latency_ms_avg"], 55.0)
        self.assertEqual(summary["control_receive_logs"][-1]["seq"], sent.seq)

    def test_session_discovery_endpoint_returns_active_session(self):
        with tempfile.TemporaryDirectory() as tmp:
            service = SignalingHttpService(audit_log_path=Path(tmp) / "audit.jsonl")
            with service.running() as signaling_url:
                _json_post(
                    f"{signaling_url}/vehicles/online",
                    {"vehicle_id": "vehicle-001", "device_token": "dev-device-secret"},
                )

                none_yet = _json_get(
                    f"{signaling_url}/vehicles/vehicle-001/session?device_token=dev-device-secret"
                )
                self.assertEqual(none_yet["state"], "none")
                self.assertEqual(none_yet["session_id"], "")

                driver = DriverConsoleRuntime.from_config(
                    "configs/driver-console.dev.yaml",
                    signaling_http_url=signaling_url,
                    vehicle_id="vehicle-001",
                    password="dev-password",
                )
                driver.connect(now_ms=0)

                active = _json_get(
                    f"{signaling_url}/vehicles/vehicle-001/session?device_token=dev-device-secret"
                )
                self.assertEqual(active["session_id"], driver.session_id)
                self.assertEqual(active["state"], "SESSION_ACTIVE")

                with self.assertRaises(error.HTTPError) as ctx:
                    _json_get(f"{signaling_url}/vehicles/vehicle-001/session?device_token=wrong")
                self.assertEqual(ctx.exception.code, 401)


if __name__ == "__main__":
    unittest.main()
