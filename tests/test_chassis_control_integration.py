import unittest
import runpy
from dataclasses import replace
from pathlib import Path

from mine_teleop.chassis_control import (
    build_chassis_control_integration_plan,
    ChassisControlCommandMapper,
    ChassisControlFeedbackPump,
    ChassisControlFeedbackSnapshot,
)
from mine_teleop.config import (
    ChassisControlIntegrationConfig,
    MinePilotCanIntegrationConfig,
    VehicleAdapterContract,
    VehicleAdapterIntegrationConfig,
    load_vehicle_config,
)
from mine_teleop.control import ControlCommand
from mine_teleop.safety import ControlOutput
from mine_teleop.vehicle_adapter import (
    DynamicLibraryVehicleAdapter,
    MockTelemetry,
    VehicleAdapterError,
    VehicleAdapterStatus,
    create_vehicle_adapter,
)
from mine_teleop.vehicle_control_service import VehicleControlService


class ChassisControlIntegrationTests(unittest.TestCase):
    def test_control_command_maps_to_chassis_control_vehicle_state_intent(self):
        mapper = ChassisControlCommandMapper(
            can_interface="can0",
            wheel_count=8,
            max_speed_mps=4.0,
            max_accel_mps2=1.5,
            max_decel_mps2=3.0,
            max_steering_rad=0.6,
        )

        intent = mapper.map_control(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=7,
                ts_ms=1000,
                gear="D",
                steering=0.5,
                throttle=0.25,
                brake=0.0,
            )
        )

        self.assertEqual(intent.actions, ("UpdateVehicleState", "RunArmingStateMachine", "SendCanMessage"))
        self.assertEqual(intent.can_interface, "can0")
        self.assertFalse(intent.estop)
        self.assertEqual(intent.vehicle_state.target_gear, 3)
        self.assertEqual(intent.vehicle_state.target_velocity, (1.0, 0.0))
        self.assertEqual(intent.vehicle_state.target_acceleration, (0.375, 0.0))
        self.assertEqual(intent.vehicle_state.target_steering_angle, (0.3,) * 8)

    def test_reverse_gear_uses_chassis_control_reverse_value_without_negative_velocity(self):
        mapper = ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0)

        intent = mapper.map_control(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=8,
                ts_ms=1020,
                gear="R",
                steering=-1.0,
                throttle=0.5,
                brake=0.0,
            )
        )

        self.assertEqual(intent.vehicle_state.target_gear, 2)
        self.assertEqual(intent.vehicle_state.target_velocity, (1.0, 0.0))
        self.assertEqual(intent.vehicle_state.target_steering_angle, (-0.5,) * 4)

    def test_command_mapper_rejects_invalid_chassis_control_settings(self):
        invalid_settings = [
            ({"can_interface": True}, "can_interface must be a non-empty string"),
            ({"can_interface": ""}, "can_interface must be a non-empty string"),
            ({"wheel_count": True}, "wheel_count must be a positive integer"),
            ({"wheel_count": 0}, "wheel_count must be a positive integer"),
            ({"max_speed_mps": float("nan")}, "max_speed_mps must be a positive finite number"),
            ({"max_accel_mps2": float("inf")}, "max_accel_mps2 must be a positive finite number"),
            ({"max_decel_mps2": True}, "max_decel_mps2 must be a positive finite number"),
            ({"max_steering_rad": 0.0}, "max_steering_rad must be a positive finite number"),
        ]

        for kwargs, message in invalid_settings:
            settings = {"can_interface": "can0"}
            settings.update(kwargs)
            with self.subTest(settings=kwargs):
                with self.assertRaisesRegex(ValueError, message):
                    ChassisControlCommandMapper(**settings)

    def test_estop_safe_output_maps_to_emergency_stop_action(self):
        mapper = ChassisControlCommandMapper(can_interface="can0")

        intent = mapper.map_safe_stop(
            ControlOutput(gear="D", steering=0.2, throttle=0.2, brake=1.0, estop=True)
        )

        self.assertEqual(intent.actions, ("EmergencyStop",))
        self.assertTrue(intent.estop)
        self.assertIsNone(intent.vehicle_state)

    def test_dynamic_library_factory_reports_cpp_bridge_requirement(self):
        contract = VehicleAdapterContract(
            steering_unit="normalized",
            throttle_unit="normalized",
            brake_unit="normalized",
            brake_semantics="normalized_service_brake",
            gear_values=["P", "R", "N", "D"],
            heartbeat_period_ms=50,
            safe_stop_supported=True,
            estop_supported=True,
            command_ack="telemetry_feedback",
            telemetry_fields=[
                "speed_mps",
                "gear",
                "steering_feedback",
                "throttle_feedback",
                "brake_feedback",
                "estop",
            ],
            integration=VehicleAdapterIntegrationConfig(
                chassis_control=ChassisControlIntegrationConfig(
                    source_root="/Volumes/SystemDisk/Workspace/ChassisControl",
                    header_path="/Volumes/SystemDisk/Workspace/ChassisControl/chassis_control.h",
                    can_common_header_path="/Volumes/SystemDisk/Workspace/ChassisControl/include/can/can_common.h",
                    cmake_target="chassis_control",
                    library_output_name="libchassis_control.so",
                    can_interface="can0",
                    abi="cplusplus",
                    requires_cpp_bridge=True,
                ),
            ),
        )

        with self.assertRaisesRegex(VehicleAdapterError, "requires a C\\+\\+ bridge/C shim"):
            create_vehicle_adapter("dynamic_library", contract)

    def test_dynamic_library_factory_creates_c_shim_adapter_without_opening_can(self):
        contract = _contract_with_chassis_abi("c_shim", bridge_library_path="/tmp/libmine_teleop_chassis_bridge.so")

        adapter = create_vehicle_adapter("dynamic_library", contract)

        self.assertIsInstance(adapter, DynamicLibraryVehicleAdapter)

    def test_can_factory_uses_c_shim_bridge_when_configured_without_opening_can(self):
        contract = _contract_with_chassis_abi("c_shim", bridge_library_path="/tmp/libmine_teleop_chassis_bridge.so")

        adapter = create_vehicle_adapter("can", contract)

        self.assertIsInstance(adapter, DynamicLibraryVehicleAdapter)
        self.assertEqual(adapter.can_interface, "can0")
        self.assertEqual(adapter.get_status().adapter_type, "can")

    def test_dynamic_library_adapter_calls_c_shim_for_control_estop_and_telemetry(self):
        fake = _FakeBridgeLibrary()
        adapter = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0),
            library_loader=lambda path: fake,
        )

        adapter.open()
        adapter.apply_control(
            ControlCommand(
                vehicle_id="vehicle-001",
                session_id="session-001",
                seq=1,
                ts_ms=1000,
                gear="D",
                steering=0.5,
                throttle=0.5,
                brake=0.0,
            )
        )
        adapter.apply_safe_stop(ControlOutput(gear="D", steering=0.0, throttle=0.0, brake=1.0, estop=True))
        telemetry = adapter.read_telemetry()
        status = adapter.get_status()
        adapter.close()

        self.assertEqual(fake.calls[0], ("open", b"can0"))
        self.assertEqual(fake.calls[1][0], "apply_state")
        self.assertEqual(fake.calls[1][1:4], (3, 1.0, 0.5))
        self.assertEqual(fake.calls[1][4], (0.25, 0.25, 0.25, 0.25))
        self.assertEqual(fake.calls[2], ("emergency_stop",))
        self.assertEqual(telemetry.speed_mps, 1.25)
        self.assertEqual(telemetry.gear, "D")
        self.assertTrue(telemetry.estop)
        self.assertEqual(status.applied_command_count, 1)
        self.assertEqual(status.safe_stop_count, 1)
        self.assertEqual(fake.calls[-1], ("close",))

    def test_mock_vehicle_adapter_reports_open_and_command_counts(self):
        mock = create_vehicle_adapter("mock")

        self.assertEqual(
            mock.get_status(),
            VehicleAdapterStatus(
                adapter_type="mock",
                opened=False,
                healthy=True,
                applied_command_count=0,
                safe_stop_count=0,
            ),
        )

        mock.open()
        mock.apply_safe_stop(ControlOutput(gear="D", steering=0.0, throttle=0.0, brake=1.0, estop=False))

        self.assertEqual(
            mock.get_status(),
            VehicleAdapterStatus(
                adapter_type="mock",
                opened=True,
                healthy=True,
                applied_command_count=0,
                safe_stop_count=1,
            ),
        )

    def test_dynamic_library_adapter_reports_bridge_status_and_errors(self):
        failing_bridge = _FakeBridgeLibrary(open_result=-3)
        dynamic = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4),
            library_loader=lambda path: failing_bridge,
        )

        self.assertEqual(
            dynamic.get_status(),
            VehicleAdapterStatus(
                adapter_type="dynamic_library",
                opened=False,
                healthy=True,
                can_interface="can0",
                library_path="/tmp/libmine_teleop_chassis_bridge.so",
            ),
        )

        with self.assertRaisesRegex(VehicleAdapterError, "mine_teleop_chassis_open failed with code -3"):
            dynamic.open()

        self.assertEqual(
            dynamic.get_status(),
            VehicleAdapterStatus(
                adapter_type="dynamic_library",
                opened=False,
                healthy=False,
                can_interface="can0",
                library_path="/tmp/libmine_teleop_chassis_bridge.so",
                last_error="mine_teleop_chassis_open failed with code -3",
            ),
        )

    def test_dynamic_library_adapter_records_library_load_failure_in_status(self):
        def fail_to_load(path):
            raise OSError(f"{path}: cannot open shared object file")

        dynamic = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4),
            library_loader=fail_to_load,
        )

        with self.assertRaisesRegex(VehicleAdapterError, "failed to load dynamic library"):
            dynamic.open()

        self.assertEqual(
            dynamic.get_status(),
            VehicleAdapterStatus(
                adapter_type="dynamic_library",
                opened=False,
                healthy=False,
                can_interface="can0",
                library_path="/tmp/libmine_teleop_chassis_bridge.so",
                last_error=(
                    "failed to load dynamic library /tmp/libmine_teleop_chassis_bridge.so: "
                    "/tmp/libmine_teleop_chassis_bridge.so: cannot open shared object file"
                ),
            ),
        )

    def test_dynamic_library_service_telemetry_is_not_marked_as_mock(self):
        fake = _FakeBridgeLibrary()
        adapter = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0),
            library_loader=lambda path: fake,
        )
        config = replace(
            load_vehicle_config(Path("configs/vehicle-agent.dev.yaml")),
            vehicle_adapter_type="dynamic_library",
            vehicle_adapter_contract=_contract_with_chassis_abi(
                "c_shim",
                bridge_library_path="/tmp/libmine_teleop_chassis_bridge.so",
            ),
        )
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )

        service.start(now_ms=0)
        service.tick(now_ms=0)

        self.assertFalse(service.telemetry_history[-1]["mock_telemetry"])
        self.assertEqual(service.telemetry_history[-1]["source"], "dynamic_library")
        self.assertEqual(
            service.telemetry_history[-1]["vehicle_adapter"],
            {
                "adapter_type": "dynamic_library",
                "opened": True,
                "healthy": True,
                "can_interface": "can0",
                "library_path": "/tmp/libmine_teleop_chassis_bridge.so",
                "applied_command_count": 0,
                "safe_stop_count": 0,
            },
        )

    def test_dynamic_library_adapter_forwards_decoded_can_feedback_to_c_shim(self):
        fake = _FakeBridgeLibrary()
        adapter = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0),
            library_loader=lambda path: fake,
        )

        adapter.update_feedback(
            ChassisControlFeedbackSnapshot(
                shake_hand_status=5,
                epb_status=(2, 2, 2, 2),
                gear_status=3,
                mcu_mode=(1, 1, 1, 1, 1, 1, 1, 1),
                eps_mode=(1, 1, 1, 1),
                eps_angle=(0.1, -0.2, 0.3, -0.4),
                ehb_mode=(1, 1, 1, 1, 1, 1, 1, 1),
                vehicle_speed=1.75,
                vehicle_speed_valid=True,
            )
        )

        self.assertEqual(fake.calls[-1][0], "update_feedback")
        self.assertEqual(fake.calls[-1][1:6], (5, (2, 2, 2, 2), 3, (0.1, -0.2, 0.3, -0.4), 1.75))

    def test_dynamic_library_adapter_polls_minepilot_can_feedback_from_c_shim(self):
        fake = _FakeBridgeLibrary()
        adapter = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0),
            library_loader=lambda path: fake,
        )

        snapshot = adapter.poll_feedback()

        self.assertEqual(fake.calls[-1][0], "poll_feedback")
        self.assertEqual(
            snapshot,
            ChassisControlFeedbackSnapshot(
                shake_hand_status=5,
                epb_status=(2, 3, 4, 5),
                gear_status=3,
                mcu_mode=(11, 12, 13, 14, 15, 16, 17, 18),
                eps_mode=(21, 22, 23, 24),
                eps_angle=(0.1, -0.2, 0.3, -0.4),
                ehb_mode=(31, 32, 33, 34, 35, 36, 37, 38),
                vehicle_speed=1.75,
                vehicle_speed_valid=True,
            ),
        )

    def test_control_service_polls_adapter_feedback_before_telemetry_read(self):
        fake = _FakeBridgeLibrary()
        adapter = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0),
            library_loader=lambda path: fake,
        )
        config = replace(
            load_vehicle_config(Path("configs/vehicle-agent.dev.yaml")),
            vehicle_adapter_type="dynamic_library",
            vehicle_adapter_contract=_contract_with_chassis_abi(
                "c_shim",
                bridge_library_path="/tmp/libmine_teleop_chassis_bridge.so",
            ),
        )
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )

        service.start(now_ms=0)
        service.tick(now_ms=0)

        poll_index = fake.calls.index(("poll_feedback",))
        read_index = fake.calls.index(("read_telemetry",))
        self.assertLess(poll_index, read_index)

    def test_control_service_forwards_polled_feedback_snapshot_before_telemetry_read(self):
        snapshot = ChassisControlFeedbackSnapshot(
            shake_hand_status=5,
            epb_status=(2, 3, 4, 5),
            gear_status=3,
            mcu_mode=(11, 12, 13, 14, 15, 16, 17, 18),
            eps_mode=(21, 22, 23, 24),
            eps_angle=(0.1, -0.2, 0.3, -0.4),
            ehb_mode=(31, 32, 33, 34, 35, 36, 37, 38),
            vehicle_speed=1.75,
            vehicle_speed_valid=True,
        )
        adapter = _PollingFeedbackAdapter(snapshot)
        config = replace(
            load_vehicle_config(Path("configs/vehicle-agent.dev.yaml")),
            vehicle_adapter_type="dynamic_library",
            vehicle_adapter_contract=_contract_with_chassis_abi(
                "c_shim",
                bridge_library_path="/tmp/libmine_teleop_chassis_bridge.so",
            ),
        )
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )

        service.start(now_ms=0)
        service.tick(now_ms=0)

        self.assertEqual(
            adapter.calls,
            [
                ("open",),
                ("poll_feedback",),
                ("update_feedback", snapshot),
                ("read_telemetry",),
            ],
        )
        self.assertEqual(adapter.snapshots, [snapshot])

    def test_control_service_reports_feedback_poll_error_in_telemetry_without_stopping_tick(self):
        fake = _FakeBridgeLibrary(poll_result=-7)
        adapter = DynamicLibraryVehicleAdapter(
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
            can_interface="can0",
            mapper=ChassisControlCommandMapper(can_interface="can0", wheel_count=4, max_speed_mps=2.0),
            library_loader=lambda path: fake,
        )
        config = replace(
            load_vehicle_config(Path("configs/vehicle-agent.dev.yaml")),
            vehicle_adapter_type="dynamic_library",
            vehicle_adapter_contract=_contract_with_chassis_abi(
                "c_shim",
                bridge_library_path="/tmp/libmine_teleop_chassis_bridge.so",
            ),
        )
        service = VehicleControlService.from_config(
            config,
            session_id="session-001",
            adapter=adapter,
            telemetry_interval_ms=50,
        )

        service.start(now_ms=0)
        try:
            service.tick(now_ms=0)
        except VehicleAdapterError as exc:
            self.fail(f"feedback poll errors should be reported in telemetry, not stop tick: {exc}")

        self.assertEqual(fake.calls, [("open", b"can0"), ("poll_feedback",), ("read_telemetry",)])
        telemetry = service.telemetry_history[-1]
        self.assertEqual(telemetry["source"], "dynamic_library")
        self.assertFalse(telemetry["vehicle_adapter"]["healthy"])
        self.assertEqual(
            telemetry["vehicle_adapter"]["last_error"],
            "mine_teleop_chassis_poll_feedback failed with code -7",
        )

    def test_adapter_status_feedback_poll_payload_forwards_snapshot_to_update_feedback(self):
        snapshot = ChassisControlFeedbackSnapshot(
            shake_hand_status=5,
            epb_status=(2, 3, 4, 5),
            gear_status=3,
            mcu_mode=(11, 12, 13, 14, 15, 16, 17, 18),
            eps_mode=(21, 22, 23, 24),
            eps_angle=(0.1, -0.2, 0.3, -0.4),
            ehb_mode=(31, 32, 33, 34, 35, 36, 37, 38),
            vehicle_speed=1.75,
            vehicle_speed_valid=True,
        )
        adapter = _PollingFeedbackAdapter(snapshot)
        vehicle_agent = runpy.run_path("vehicle-agent/vehicle_agent.py")

        payload = vehicle_agent["_adapter_feedback_poll_payload"](adapter)

        self.assertTrue(payload["received"])
        self.assertEqual(payload["snapshot"]["shake_hand_status"], 5)
        self.assertEqual(
            adapter.calls,
            [
                ("poll_feedback",),
                ("update_feedback", snapshot),
            ],
        )
        self.assertEqual(adapter.snapshots, [snapshot])

    def test_adapter_status_feedback_poll_payload_reports_update_error(self):
        snapshot = ChassisControlFeedbackSnapshot(
            shake_hand_status=5,
            epb_status=(2, 3, 4, 5),
            gear_status=3,
            mcu_mode=(11, 12, 13, 14, 15, 16, 17, 18),
            eps_mode=(21, 22, 23, 24),
            eps_angle=(0.1, -0.2, 0.3, -0.4),
            ehb_mode=(31, 32, 33, 34, 35, 36, 37, 38),
            vehicle_speed=1.75,
            vehicle_speed_valid=True,
        )
        adapter = _PollingFeedbackAdapter(
            snapshot,
            update_error=RuntimeError("feedback cache rejected snapshot"),
        )
        vehicle_agent = runpy.run_path("vehicle-agent/vehicle_agent.py")

        try:
            payload = vehicle_agent["_adapter_feedback_poll_payload"](adapter)
        except RuntimeError as exc:
            self.fail(f"feedback poll should report update errors as payload: {exc}")

        self.assertFalse(payload["received"])
        self.assertEqual(payload["reason"], "adapter_feedback_update_error")
        self.assertEqual(payload["error"], "feedback cache rejected snapshot")
        self.assertEqual(payload["snapshot"]["shake_hand_status"], 5)
        self.assertEqual(
            adapter.calls,
            [
                ("poll_feedback",),
                ("update_feedback", snapshot),
            ],
        )
        self.assertEqual(adapter.snapshots, [])

    def test_feedback_pump_maps_minepilot_decoded_can_data_to_adapter_snapshot(self):
        decoded = {
            "wvcu_veh_shake_hand_sts_frame": {"wvcu_shake_hand_sts": 5},
            "wvcu_prk_1_sts_frame": {
                "prk_1_sts01_mode": 2,
                "prk_1_sts02_mode": 3,
                "prk_1_sts03_mode": 4,
                "prk_1_sts04_mode": 5,
            },
            "wvcu_vcu_sts_frame": {"wvcu_gear_sts_now": 3},
            "wvcu_mot1_sts02_frame": {"mot_1_sts02_mot_work_mode": 11},
            "wvcu_mot2_sts02_frame": {"mot_2_sts02_mot_work_mode": 12},
            "wvcu_mot3_sts02_frame": {"mot_3_sts02_mot_work_mode": 13},
            "wvcu_mot4_sts02_frame": {"mot_4_sts02_mot_work_mode": 14},
            "wvcu_mot5_sts02_frame": {"mot_5_sts02_mot_work_mode": 15},
            "wvcu_mot6_sts02_frame": {"mot_6_sts02_mot_work_mode": 16},
            "wvcu_mot7_sts02_frame": {"mot_7_sts02_mot_work_mode": 17},
            "wvcu_mot8_sts02_frame": {"mot_8_sts02_mot_work_mode": 18},
            "wvcu_str1_sts_frame": {"str_1_mode_sts": 21, "str_1_ang_sts": 1.5},
            "wvcu_str2_sts_frame": {"str_2_mode_sts": 22, "str_2_ang_sts": -2.5},
            "wvcu_str3_sts_frame": {"str_3_mode_sts": 23, "str_3_ang_sts": 3.5},
            "wvcu_str4_sts_frame": {"str_4_mode_sts": 24, "str_4_ang_sts": -4.5},
            "wvcu_brk1_sts_frame": {"brk_1_sts01_mode": 31, "brk_1_sts02_mode": 32},
            "wvcu_brk3_sts_frame": {"brk_3_sts03_mode": 33, "brk_3_sts04_mode": 34},
            "wvcu_brk5_sts_frame": {"brk_5_sts05_mode": 35, "brk_5_sts06_mode": 36},
            "wvcu_brk7_sts_frame": {"brk_7_sts07_mode": 37, "brk_7_sts08_mode": 38},
            "wvcu_veh_spd_sts_now_frame": {"wvcu_veh_spd_now": 1.75},
            "has_wvcu_veh_spd_sts_now_frame": True,
        }
        adapter = _FakeFeedbackAdapter()
        pump = ChassisControlFeedbackPump(reader=lambda: decoded, adapter=adapter)

        self.assertTrue(pump.poll_once())

        self.assertEqual(
            adapter.snapshots,
            [
                ChassisControlFeedbackSnapshot(
                    shake_hand_status=5,
                    epb_status=(2, 3, 4, 5),
                    gear_status=3,
                    mcu_mode=(11, 12, 13, 14, 15, 16, 17, 18),
                    eps_mode=(21, 22, 23, 24),
                    eps_angle=(1.5, -2.5, 3.5, -4.5),
                    ehb_mode=(31, 32, 33, 34, 35, 36, 37, 38),
                    vehicle_speed=1.75,
                    vehicle_speed_valid=True,
                )
            ],
        )

    def test_feedback_pump_skips_empty_reader_result(self):
        adapter = _FakeFeedbackAdapter()
        pump = ChassisControlFeedbackPump(reader=lambda: None, adapter=adapter)

        self.assertFalse(pump.poll_once())
        self.assertEqual(adapter.snapshots, [])

    def test_integration_plan_tracks_minepilot_can_db_receiver_and_sender_sources(self):
        contract = VehicleAdapterContract(
            steering_unit="normalized",
            throttle_unit="normalized",
            brake_unit="normalized",
            brake_semantics="normalized_service_brake",
            gear_values=["P", "R", "N", "D"],
            heartbeat_period_ms=50,
            safe_stop_supported=True,
            estop_supported=True,
            command_ack="telemetry_feedback",
            telemetry_fields=["speed_mps", "gear", "steering_feedback", "brake_feedback", "estop"],
            integration=VehicleAdapterIntegrationConfig(
                chassis_control=ChassisControlIntegrationConfig(
                    source_root="/Volumes/SystemDisk/Workspace/ChassisControl",
                    header_path="/Volumes/SystemDisk/Workspace/ChassisControl/chassis_control.h",
                    can_common_header_path="/Volumes/SystemDisk/Workspace/ChassisControl/include/can/can_common.h",
                    cmake_target="chassis_control",
                    library_output_name="libchassis_control.so",
                    can_interface="can0",
                    abi="cplusplus",
                    requires_cpp_bridge=True,
                ),
                minepilot=MinePilotCanIntegrationConfig(
                    source_root="/Volumes/SystemDisk/Workspace/MinePilot",
                    can_common_header_path="/Volumes/SystemDisk/Workspace/MinePilot/include/can/can_common.h",
                    can_message_header_path="/Volumes/SystemDisk/Workspace/MinePilot/include/can/can_message.h",
                    can_db_header_path="/Volumes/SystemDisk/Workspace/MinePilot/include/can_db.h",
                    can_receiver_header_path="/Volumes/SystemDisk/Workspace/MinePilot/include/can_receiver.h",
                    can_sender_header_path="/Volumes/SystemDisk/Workspace/MinePilot/include/can_sender.h",
                    can_db_source_path="/Volumes/SystemDisk/Workspace/MinePilot/src/can_db.cpp",
                    can_receiver_source_path="/Volumes/SystemDisk/Workspace/MinePilot/src/can_receiver.cpp",
                    can_sender_source_path="/Volumes/SystemDisk/Workspace/MinePilot/src/can_sender.cpp",
                ),
            ),
        )

        plan = build_chassis_control_integration_plan(contract)

        self.assertIn("SendCanMessage", plan.required_calls)
        self.assertIn("EmergencyStopWheels", plan.required_calls)
        self.assertNotIn("EmergencyStop", plan.required_calls)
        self.assertEqual(
            plan.feedback_sources,
            (
                "/Volumes/SystemDisk/Workspace/MinePilot/include/can/can_common.h",
                "/Volumes/SystemDisk/Workspace/MinePilot/include/can/can_message.h",
                "/Volumes/SystemDisk/Workspace/MinePilot/include/can_db.h",
                "/Volumes/SystemDisk/Workspace/MinePilot/include/can_receiver.h",
                "/Volumes/SystemDisk/Workspace/MinePilot/include/can_sender.h",
                "/Volumes/SystemDisk/Workspace/MinePilot/src/can_db.cpp",
                "/Volumes/SystemDisk/Workspace/MinePilot/src/can_receiver.cpp",
                "/Volumes/SystemDisk/Workspace/MinePilot/src/can_sender.cpp",
            ),
        )

    def test_bridge_cmake_compiles_minepilot_can_sender_with_receiver_and_db(self):
        cmake = Path("deployments/chassis-control-bridge/CMakeLists.txt").read_text()
        bridge_sources = cmake.split("add_library(mine_teleop_chassis_bridge SHARED", 1)[1].split(")", 1)[0]

        self.assertIn('"${MINEPILOT_CAN_DB_SOURCE}"', bridge_sources)
        self.assertIn('"${MINEPILOT_CAN_RECEIVER_SOURCE}"', bridge_sources)
        self.assertIn('"${MINEPILOT_CAN_SENDER_SOURCE}"', bridge_sources)

    def test_bridge_feedback_poll_uses_nonblocking_linked_can_receive(self):
        bridge = Path("deployments/chassis-control-bridge/chassis_control_bridge.cpp").read_text()
        poll_body = bridge.split("extern \"C\" int mine_teleop_chassis_poll_feedback", 1)[1].split(
            "extern \"C\" int mine_teleop_chassis_read_telemetry", 1
        )[0]

        self.assertIn("can_receive(g_can_handle, &rx_frame, 0)", poll_body)
        self.assertNotIn("read_can_message", poll_body)

    def test_bridge_emergency_stop_uses_exported_wheel_stop_after_open_guard(self):
        bridge = Path("deployments/chassis-control-bridge/chassis_control_bridge.cpp").read_text()
        stop_body = bridge.split("extern \"C\" int mine_teleop_chassis_emergency_stop", 1)[1].split(
            "extern \"C\" int mine_teleop_chassis_update_feedback", 1
        )[0]

        self.assertIn("g_can_interface.empty()", stop_body)
        self.assertIn("g_can_handle == nullptr", stop_body)
        self.assertIn("EmergencyStopWheels()", stop_body)
        self.assertNotIn("EmergencyStop()", stop_body)
        self.assertLess(stop_body.index("g_can_handle == nullptr"), stop_body.index("EmergencyStopWheels()"))


if __name__ == "__main__":
    unittest.main()


def _contract_with_chassis_abi(abi, bridge_library_path=None):
    return VehicleAdapterContract(
        steering_unit="normalized",
        throttle_unit="normalized",
        brake_unit="normalized",
        brake_semantics="normalized_service_brake",
        gear_values=["P", "R", "N", "D"],
        heartbeat_period_ms=50,
        safe_stop_supported=True,
        estop_supported=True,
        command_ack="telemetry_feedback",
        telemetry_fields=[
            "speed_mps",
            "gear",
            "steering_feedback",
            "throttle_feedback",
            "brake_feedback",
            "estop",
        ],
        integration=VehicleAdapterIntegrationConfig(
            chassis_control=ChassisControlIntegrationConfig(
                source_root="/Volumes/SystemDisk/Workspace/ChassisControl",
                header_path="/Volumes/SystemDisk/Workspace/ChassisControl/chassis_control.h",
                can_common_header_path="/Volumes/SystemDisk/Workspace/ChassisControl/include/can/can_common.h",
                cmake_target="chassis_control",
                library_output_name="libchassis_control.so",
                can_interface="can0",
                abi=abi,
                requires_cpp_bridge=abi == "cplusplus",
                bridge_library_path=bridge_library_path,
            ),
        ),
    )


class _FakeBridgeFunction:
    def __init__(self, callback):
        self.callback = callback
        self.argtypes = None
        self.restype = None

    def __call__(self, *args):
        return self.callback(*args)


class _FakeBridgeLibrary:
    def __init__(self, open_result=0, poll_result=0):
        self.calls = []
        self.open_result = open_result
        self.poll_result = poll_result
        self.mine_teleop_chassis_open = _FakeBridgeFunction(self._open)
        self.mine_teleop_chassis_apply_state = _FakeBridgeFunction(self._apply_state)
        self.mine_teleop_chassis_emergency_stop = _FakeBridgeFunction(self._emergency_stop)
        self.mine_teleop_chassis_update_feedback = _FakeBridgeFunction(self._update_feedback)
        self.mine_teleop_chassis_poll_feedback = _FakeBridgeFunction(self._poll_feedback)
        self.mine_teleop_chassis_read_telemetry = _FakeBridgeFunction(self._read_telemetry)
        self.mine_teleop_chassis_close = _FakeBridgeFunction(self._close)

    def _open(self, can_interface):
        self.calls.append(("open", can_interface))
        return self.open_result

    def _apply_state(self, target_gear, target_vx, target_ax, steering_values, steering_count):
        self.calls.append(
            (
                "apply_state",
                target_gear,
                target_vx,
                target_ax,
                tuple(steering_values[index] for index in range(steering_count)),
            )
        )
        return 0

    def _emergency_stop(self):
        self.calls.append(("emergency_stop",))
        return 0

    def _update_feedback(self, feedback_ptr):
        feedback = feedback_ptr._obj
        self.calls.append(
            (
                "update_feedback",
                feedback.shake_hand_status,
                tuple(feedback.epb_status),
                feedback.gear_status,
                tuple(feedback.eps_angle),
                feedback.vehicle_speed,
            )
        )
        return 0

    def _poll_feedback(self, feedback_ptr):
        feedback = feedback_ptr._obj
        feedback.shake_hand_status = 5
        for index, value in enumerate((2, 3, 4, 5)):
            feedback.epb_status[index] = value
        feedback.gear_status = 3
        for index, value in enumerate((11, 12, 13, 14, 15, 16, 17, 18)):
            feedback.mcu_mode[index] = value
        for index, value in enumerate((21, 22, 23, 24)):
            feedback.eps_mode[index] = value
        for index, value in enumerate((0.1, -0.2, 0.3, -0.4)):
            feedback.eps_angle[index] = value
        for index, value in enumerate((31, 32, 33, 34, 35, 36, 37, 38)):
            feedback.ehb_mode[index] = value
        feedback.vehicle_speed = 1.75
        feedback.vehicle_speed_valid = 1
        self.calls.append(("poll_feedback",))
        return self.poll_result

    def _read_telemetry(self, telemetry_ptr):
        telemetry = telemetry_ptr._obj
        telemetry.speed_mps = 1.25
        telemetry.gear = 3
        telemetry.steering_feedback = 0.1
        telemetry.throttle_feedback = 0.2
        telemetry.brake_feedback = 1.0
        telemetry.estop = 1
        self.calls.append(("read_telemetry",))
        return 0

    def _close(self):
        self.calls.append(("close",))
        return 0


class _FakeFeedbackAdapter:
    def __init__(self):
        self.snapshots = []

    def update_feedback(self, snapshot):
        self.snapshots.append(snapshot)


class _PollingFeedbackAdapter:
    def __init__(self, snapshot, update_error=None):
        self.snapshot = snapshot
        self.update_error = update_error
        self.snapshots = []
        self.calls = []
        self.opened = False

    def open(self):
        self.opened = True
        self.calls.append(("open",))

    def poll_feedback(self):
        self.calls.append(("poll_feedback",))
        return self.snapshot

    def update_feedback(self, snapshot):
        self.calls.append(("update_feedback", snapshot))
        if self.update_error is not None:
            raise self.update_error
        self.snapshots.append(snapshot)

    def read_telemetry(self):
        self.calls.append(("read_telemetry",))
        return MockTelemetry(
            speed_mps=1.75,
            gear="D",
            steering_feedback=0.1,
            throttle_feedback=0.2,
            brake_feedback=0.0,
            estop=False,
        )

    def get_status(self):
        return VehicleAdapterStatus(
            adapter_type="dynamic_library",
            opened=self.opened,
            healthy=True,
            can_interface="can0",
            library_path="/tmp/libmine_teleop_chassis_bridge.so",
        )
