import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import yaml

from mine_teleop.deployment_validation import TargetHostValidationPlan, ValidationCommand


_CHASSIS_CONTROL_LIBRARY_PATH = "/Volumes/SystemDisk/Workspace/MinePilot/libchassis_control.so"
_BRIDGE_LIBRARY_PATH = "/opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so"
_TARGET_HOST_SUMMARY_METADATA = {
    "acceptance_scenario": "target-host-acceptance",
    "vehicle_config_path": "/etc/mine-teleop/vehicle-agent.yaml",
    "can_interface": "can0",
    "chassis_control_root": "/Volumes/SystemDisk/Workspace/ChassisControl",
    "minepilot_root": "/Volumes/SystemDisk/Workspace/MinePilot",
    "chassis_control_branch": "UI_Test",
    "minepilot_branch": "merge_ui_test",
    "bridge_build_dir": "build/chassis-control-bridge",
    "uploader_work_dir": "/var/lib/mine-teleop/uploader",
    "bridge_library_path": _BRIDGE_LIBRARY_PATH,
    "chassis_control_library_path": _CHASSIS_CONTROL_LIBRARY_PATH,
    "minepilot_can_probe_build_dir": "/tmp/mine-teleop-minepilot-can-probe",
    "can_probe_timeout_seconds": 3,
    "chassis_control_commit": "a" * 40,
    "minepilot_commit": "a" * 40,
    "chassis_control_dirty": False,
    "minepilot_dirty": False,
    "chassis_control_changed_paths": 0,
    "minepilot_changed_paths": 0,
}
_MINEPILOT_CAN_SENDER_EXECUTABLE = "/tmp/mine-teleop-minepilot-can-probe/can_sender_main"
_FEEDBACK_SNAPSHOT = {
    "shake_hand_status": 1,
    "epb_status": [0, 0, 0, 0],
    "gear_status": 3,
    "mcu_mode": [0, 0, 0, 0, 0, 0, 0, 0],
    "eps_mode": [0, 0, 0, 0],
    "eps_angle": [0.0, None, None, None],
    "ehb_mode": [0, 0, 0, 0, 0, 0, 0, 0],
    "vehicle_speed": 0.0,
    "vehicle_speed_valid": True,
}
_BRIDGE_READY_CHECK_NAMES = (
    "chassis_control.root",
    "minepilot.root",
    "chassis_control.branch",
    "minepilot.branch",
    "chassis_control.commit",
    "minepilot.commit",
    "chassis_control.dirty",
    "minepilot.dirty",
    "chassis_control.header",
    "chassis_control.can_common_header",
    "minepilot.chassis_control_header",
    "minepilot.can_common_header",
    "minepilot.can_message_header",
    "minepilot.can_db_header",
    "minepilot.can_receiver_header",
    "minepilot.can_sender_header",
    "minepilot.can_db_source",
    "minepilot.can_receiver_source",
    "minepilot.can_sender_source",
    "chassis_control.library",
    "chassis_control.symbols",
    "cmake.configure",
    "cmake.build",
)


def _bridge_ready_message(check_name):
    if check_name == "chassis_control.branch":
        return "/tmp/ChassisControl is on expected branch UI_Test"
    if check_name == "minepilot.branch":
        return "/tmp/MinePilot is on expected branch merge_ui_test"
    if check_name in {"chassis_control.commit", "minepilot.commit"}:
        return f"/tmp/{check_name} HEAD commit={'a' * 40}"
    if check_name in {"chassis_control.dirty", "minepilot.dirty"}:
        return f"/tmp/{check_name} dirty=false changed_paths=0"
    return f"{check_name} ready"


def _bridge_ready_path(check_name):
    if check_name == "chassis_control.root":
        return "/Volumes/SystemDisk/Workspace/ChassisControl"
    if check_name == "minepilot.root":
        return "/Volumes/SystemDisk/Workspace/MinePilot"
    if check_name in {"chassis_control.library", "chassis_control.symbols"}:
        return _CHASSIS_CONTROL_LIBRARY_PATH
    if check_name in {"cmake.configure", "cmake.build"}:
        return "build/chassis-control-bridge"
    return "/tmp/bridge"


class SystemdTemplateTests(unittest.TestCase):
    def test_vehicle_and_cloud_services_have_restart_env_and_file_logging(self):
        services = {
            "mine-teleop-vehicle-agent.service": "vehicle-agent/vehicle_agent.py --run-loop",
            "mine-teleop-vehicle-media-agent.service": "vehicle-media-agent/vehicle_media_agent.py",
            "mine-teleop-vehicle-uploader.service": "vehicle-uploader/vehicle_uploader.py",
            "mine-teleop-signaling-server.service": "signaling-server/signaling_server.py --serve",
        }

        for filename, expected_command in services.items():
            with self.subTest(filename=filename):
                content = (Path("deployments/systemd") / filename).read_text(encoding="utf-8")

                self.assertIn("WantedBy=multi-user.target", content)
                self.assertIn("Restart=on-failure", content)
                self.assertIn("WorkingDirectory=/opt/mine-teleop", content)
                self.assertIn("EnvironmentFile=/etc/mine-teleop/mine-teleop.env", content)
                self.assertIn("LogsDirectory=mine-teleop", content)
                self.assertIn("StandardOutput=append:/var/log/mine-teleop/", content)
                self.assertIn("StandardError=append:/var/log/mine-teleop/", content)
                self.assertIn(expected_command, content)

    def test_vehicle_agent_systemd_runs_preflight_before_long_lived_control_loop(self):
        service = Path("deployments/systemd/mine-teleop-vehicle-agent.service").read_text(encoding="utf-8")

        self.assertIn(
            "ExecStartPre=/usr/bin/python3 /opt/mine-teleop/vehicle-agent/vehicle_agent.py "
            "--config /etc/mine-teleop/vehicle-agent.yaml --preflight "
            "--hardware-device /dev/dri/renderD128 --hardware-device /dev/dri/card1",
            service,
        )
        self.assertIn(
            "ExecStartPre=/usr/bin/python3 /opt/mine-teleop/vehicle-agent/vehicle_agent.py "
            "--config /etc/mine-teleop/vehicle-agent.yaml --adapter-status",
            service,
        )
        self.assertIn(
            "ExecStart=/usr/bin/python3 /opt/mine-teleop/vehicle-agent/vehicle_agent.py "
            "--run-loop --config /etc/mine-teleop/vehicle-agent.yaml",
            service,
        )

    def test_systemd_services_use_deployed_vehicle_config_and_audit_paths(self):
        media = Path("deployments/systemd/mine-teleop-vehicle-media-agent.service").read_text(encoding="utf-8")
        uploader = Path("deployments/systemd/mine-teleop-vehicle-uploader.service").read_text(encoding="utf-8")
        signaling = Path("deployments/systemd/mine-teleop-signaling-server.service").read_text(encoding="utf-8")

        self.assertIn(
            "ExecStart=/usr/bin/python3 /opt/mine-teleop/vehicle-media-agent/vehicle_media_agent.py "
            "--config /etc/mine-teleop/vehicle-agent.yaml",
            media,
        )
        self.assertIn(
            "ExecStart=/usr/bin/python3 /opt/mine-teleop/vehicle-uploader/vehicle_uploader.py "
            "--service-mode --config /etc/mine-teleop/vehicle-agent.yaml "
            "--work-dir /var/lib/mine-teleop/uploader",
            uploader,
        )
        self.assertIn("--service-mode", uploader)
        self.assertIn("--work-dir /var/lib/mine-teleop/uploader", uploader)
        self.assertIn("StateDirectory=mine-teleop/uploader", uploader)
        self.assertIn("Nice=10", uploader)
        self.assertIn("IOSchedulingClass=idle", uploader)
        self.assertIn("CPUWeight=50", uploader)
        self.assertIn("IOWeight=50", uploader)
        self.assertIn("--host 127.0.0.1", signaling)
        self.assertIn("--vehicle-config /etc/mine-teleop/vehicle-agent.yaml", signaling)
        self.assertIn("--audit-log /var/log/mine-teleop/signaling-audit.jsonl", signaling)

    def test_turn_server_systemd_and_coturn_template_are_udp_first_and_secret_based(self):
        service = Path("deployments/systemd/mine-teleop-turn-server.service").read_text(encoding="utf-8")
        config = Path("deployments/turnserver/turnserver.conf.template").read_text(encoding="utf-8")

        self.assertIn("ExecStart=/usr/bin/turnserver -c /etc/mine-teleop/turnserver.conf", service)
        self.assertIn("EnvironmentFile=/etc/mine-teleop/mine-teleop.env", service)
        self.assertIn("Restart=on-failure", service)
        self.assertIn("LogsDirectory=mine-teleop", service)
        self.assertIn("StandardOutput=append:/var/log/mine-teleop/turn-server.log", service)
        self.assertIn("StandardError=append:/var/log/mine-teleop/turn-server.err.log", service)

        self.assertIn("listening-port=3478", config)
        self.assertIn("tls-listening-port=5349", config)
        self.assertIn("fingerprint", config)
        self.assertIn("lt-cred-mech", config)
        self.assertIn("use-auth-secret", config)
        self.assertIn("static-auth-secret=${MINE_TELEOP_TURN_STATIC_AUTH_SECRET}", config)
        self.assertIn("realm=${MINE_TELEOP_TURN_REALM}", config)
        self.assertIn("cert=/etc/mine-teleop/tls/turn.crt", config)
        self.assertIn("pkey=/etc/mine-teleop/tls/turn.key", config)
        self.assertIn("no-tcp-relay", config)
        self.assertIn("total-quota=", config)
        self.assertIn("bps-capacity=", config)

    def test_open_questions_record_current_chassis_control_integration_target(self):
        content = Path("docs/13-open-questions.md").read_text(encoding="utf-8")

        self.assertIn("已确认的首个接入路径", content)
        self.assertIn("ChassisControl", content)
        self.assertIn("UI_Test", content)
        self.assertIn("MinePilot", content)
        self.assertIn("merge_ui_test", content)
        self.assertIn("can_db", content)
        self.assertIn("can_receiver", content)
        self.assertIn("can_sender", content)
        self.assertIn("仍待确认", content)
        self.assertIn("控制指令单位和范围", content)
        self.assertNotIn("最终是 CAN、动态库、串口、以太网，还是其他接口。", content)


class ContainerTemplateTests(unittest.TestCase):
    def test_control_console_container_runs_driver_console_program_without_compose(self):
        dockerfile = Path("deployments/container/Dockerfile.control").read_text(encoding="utf-8")
        runner = Path("scripts/run_driver_console_docker.sh")

        self.assertIn("COPY driver-console/ driver-console/", dockerfile)
        self.assertIn("COPY mine_teleop/ mine_teleop/", dockerfile)
        self.assertIn("EXPOSE 8080", dockerfile)
        self.assertIn('CMD ["python3", "-m", "mine_teleop.control_console_container"]', dockerfile)
        self.assertTrue(runner.is_file())
        self.assertTrue(os.access(runner, os.X_OK))
        runner_text = runner.read_text(encoding="utf-8")
        self.assertIn("docker build", runner_text)
        self.assertIn("docker run --rm", runner_text)
        self.assertIn("MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL", runner_text)
        self.assertIn("MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID", runner_text)
        self.assertNotIn("driver-console/driver_console.py --serve", runner_text)
        self.assertNotIn("docker compose", runner_text)

    def test_control_plane_runner_starts_foreground_local_docker_program_without_service_install(self):
        runner = Path("scripts/run_control_plane_docker.sh")
        dockerfile = Path("deployments/container/Dockerfile.control").read_text(encoding="utf-8")

        self.assertTrue(runner.is_file())
        self.assertTrue(os.access(runner, os.X_OK))
        runner_text = runner.read_text(encoding="utf-8")
        self.assertIn('CMD ["python3", "-m", "mine_teleop.control_console_container"]', dockerfile)
        self.assertIn("docker build", runner_text)
        self.assertIn("deployments/container/Dockerfile.control", runner_text)
        self.assertIn("signaling-server/signaling_server.py --serve", runner_text)
        self.assertIn("--host 0.0.0.0", runner_text)
        self.assertIn("--allow-insecure-nonloopback-dev", runner_text)
        self.assertIn("-e MINE_TELEOP_ALLOW_WILDCARD_BIND=\"1\"", runner_text)
        self.assertIn("/vehicles/online", runner_text)
        self.assertIn("dev-device-secret", runner_text)
        self.assertIn("-e MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL=\"http://127.0.0.1:8765\"", runner_text)
        self.assertIn("-e MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID=\"$vehicle_id\"", runner_text)
        self.assertIn("-e MINE_TELEOP_DRIVER_CONSOLE_PASSWORD=\"$password\"", runner_text)
        self.assertIn("-e MINE_TELEOP_DRIVER_CONSOLE_HOST=\"0.0.0.0\"", runner_text)
        self.assertIn("-e MINE_TELEOP_DRIVER_CONSOLE_PORT=\"8080\"", runner_text)
        self.assertIn("-e MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG=", runner_text)
        self.assertIn("--network \"container:$server_name\"", runner_text)
        self.assertIn("-p \"127.0.0.1:${console_port}:8080\"", runner_text)
        self.assertIn("-p \"127.0.0.1:${signaling_port}:8765\"", runner_text)
        self.assertIn("DRIVER_CONSOLE_URL=http://127.0.0.1:${console_port}", runner_text)
        self.assertIn("SIGNALING_HTTP_URL=http://127.0.0.1:${signaling_port}", runner_text)
        self.assertNotIn("driver-console/driver_console.py --serve", runner_text)
        self.assertIn("trap cleanup EXIT", runner_text)
        self.assertIn("trap stop_control_plane INT TERM", runner_text)
        self.assertNotIn("docker compose", runner_text)
        self.assertNotIn("--restart", runner_text)
        self.assertNotIn("systemctl", runner_text)

    def test_control_plane_browser_smoke_exercises_operator_page_with_real_browser(self):
        browser_smoke = Path("scripts/control_plane_browser_smoke.py")

        self.assertTrue(browser_smoke.is_file())
        self.assertTrue(os.access(browser_smoke, os.X_OK))
        smoke_text = browser_smoke.read_text(encoding="utf-8")
        self.assertIn("--driver-console-url", smoke_text)
        self.assertIn("--signaling-container", smoke_text)
        self.assertIn("--chrome-binary", smoke_text)
        self.assertIn("--remote-debugging-port", smoke_text)
        self.assertIn("docker exec", smoke_text)
        self.assertIn("connectConsole()", smoke_text)
        self.assertIn("sendControl({gear:'D'", smoke_text)
        self.assertIn("canvas.captureStream", smoke_text)
        self.assertIn("ondatachannel", smoke_text)
        self.assertIn("SESSION_ACTIVE", smoke_text)
        self.assertIn("control_command_sent", smoke_text)
        self.assertIn("vehicle_peer_offer_forwarded_via_signaling", smoke_text)
        self.assertIn("webrtc_answer_received_via_signaling", smoke_text)
        self.assertIn("local_ice_candidate_received_via_signaling", smoke_text)
        self.assertIn("remote_ice_candidate_forwarded_via_signaling", smoke_text)
        self.assertIn("loopback_webrtc_media_received", smoke_text)
        self.assertIn("datachannel_control_command_received", smoke_text)
        self.assertIn("operator_session_state_text", smoke_text)
        self.assertIn("operator_control_authority_text", smoke_text)
        self.assertIn("operator_camera_summary_text", smoke_text)
        self.assertIn("operator_datachannel_state_text", smoke_text)
        self.assertIn("operator_connect_form_vehicle_id", smoke_text)
        self.assertIn("document.getElementById('connect-vehicle-id').value", smoke_text)
        self.assertIn("document.getElementById('connect-password').value", smoke_text)
        self.assertIn("document.getElementById('operator-session-state')", smoke_text)
        self.assertIn("browser_control_plane_smoke", smoke_text)

    def test_control_plane_browser_smoke_runner_starts_docker_stack_and_cleans_up(self):
        runner = Path("scripts/run_control_plane_browser_smoke.sh")

        self.assertTrue(runner.is_file())
        self.assertTrue(os.access(runner, os.X_OK))
        runner_text = runner.read_text(encoding="utf-8")
        self.assertIn("scripts/run_control_plane_docker.sh", runner_text)
        self.assertIn("scripts/control_plane_browser_smoke.py", runner_text)
        self.assertIn("--signaling-container \"$server_name\"", runner_text)
        self.assertIn("MINE_TELEOP_CONTROL_PLANE_SERVER", runner_text)
        self.assertIn("MINE_TELEOP_CONTROL_PLANE_CONSOLE", runner_text)
        self.assertIn("BROWSER_SMOKE_ARTIFACT_DIR=", runner_text)
        self.assertIn("trap cleanup EXIT", runner_text)
        self.assertNotIn("systemctl", runner_text)
        self.assertNotIn("--restart", runner_text)

    def test_control_plane_smoke_runs_real_driver_console_http_program(self):
        smoke_runner = Path("scripts/run_control_plane_docker_smoke.sh").read_text(encoding="utf-8")

        self.assertIn("MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL=http://127.0.0.1:8765", smoke_runner)
        self.assertIn("MINE_TELEOP_DRIVER_CONSOLE_HOST=127.0.0.1", smoke_runner)
        self.assertIn("MINE_TELEOP_DRIVER_CONSOLE_PORT=8080", smoke_runner)
        self.assertNotIn("driver-console/driver_console.py --serve", smoke_runner)
        self.assertIn("--driver-console-url http://127.0.0.1:8080", smoke_runner)
        self.assertIn("--network \"container:$server_name\"", smoke_runner)
        smoke_script = Path("scripts/control_plane_smoke.py").read_text(encoding="utf-8")
        self.assertIn("frame_sequence", smoke_script)
        self.assertIn("decoded_frame_count_by_camera", smoke_script)
        self.assertIn("control_console_received_image", smoke_script)
        self.assertIn("for expected_sequence in (1, 2):", smoke_script)
        self.assertIn("/api/webrtc/answer", smoke_script)
        self.assertIn("webrtc_answer_forwarded", smoke_script)
        self.assertIn("remote_ice_candidate_received", smoke_script)
        self.assertIn("local_ice_candidate_forwarded", smoke_script)
        self.assertIn("/api/control/gamepad", smoke_script)
        self.assertIn("gamepad_count=2", smoke_script)
        self.assertIn('"gamepad_control_commands": gamepad_count', smoke_script)
        self.assertIn("vehicle_received_steering", smoke_script)
        self.assertIn("vehicle_received_acceleration", smoke_script)
        self.assertIn("vehicle_received_deceleration", smoke_script)

    def test_ubuntu_bundle_builder_dry_run_targets_single_linux_executable_and_dynamic_libs(self):
        with tempfile.TemporaryDirectory() as tmp:
            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/build_ubuntu_bundle.py",
                    "--dry-run",
                    "--output-dir",
                    str(Path(tmp) / "bundle"),
                    "--docker-workspace",
                    str(Path(tmp) / "workspace"),
                ],
                text=True,
                capture_output=True,
                check=True,
            )

        self.assertIn("docker create", result.stdout)
        self.assertIn("docker cp", result.stdout)
        self.assertIn("--platform linux/amd64", result.stdout)
        self.assertIn("pyinstaller", result.stdout)
        self.assertIn("--onefile", result.stdout)
        self.assertIn("mine_teleop/cli.py", result.stdout)
        self.assertIn("mine-teleop.real", result.stdout)
        self.assertIn("LD_LIBRARY_PATH", result.stdout)
        self.assertIn("libmine_teleop_chassis_bridge.so", result.stdout)
        self.assertIn("libchassis_control.so", result.stdout)
        self.assertIn("apt-get -o Acquire::Retries=5", result.stdout)
        self.assertIn("apt_get install -y --no-install-recommends", result.stdout)
        self.assertIn("ffmpeg", result.stdout)
        self.assertIn("ffprobe", result.stdout)
        self.assertIn("vainfo", result.stdout)
        self.assertIn("LIBVA_DRIVERS_PATH", result.stdout)
        self.assertIn("iHD_drv_video.so", result.stdout)

    def test_ubuntu_bundle_docs_cover_software_usage_and_architecture(self):
        docs = {
            "docs/15-ubuntu-bundle-software.md": ("软件说明", "bin/mine-teleop"),
            "docs/16-ubuntu-bundle-usage.md": ("使用说明", "target-host-validation-plan"),
            "docs/17-ubuntu-bundle-architecture.md": ("架构说明", "libmine_teleop_chassis_bridge.so"),
        }
        for path, expected_terms in docs.items():
            with self.subTest(path=path):
                content = Path(path).read_text(encoding="utf-8")
                for term in expected_terms:
                    self.assertIn(term, content)
                self.assertIn("libchassis_control.so", content)

    def test_target_host_validation_plan_covers_preflight_bridge_media_and_network_commands(self):
        plan = TargetHostValidationPlan.default(
            vehicle_config_path="/etc/mine-teleop/vehicle-agent.yaml",
            hardware_devices=("/dev/dri/renderD128", "/dev/dri/card1"),
            can_interface="can0",
            network_interface="wwan0",
        )

        commands = {command.name: command for command in plan.commands}

        self.assertIn("gpu.pci.summary", commands)
        self.assertIn("gpu_pci_summary", commands["gpu.pci.summary"].command)
        self.assertIn("gpu.dri.nodes", commands)
        self.assertIn("gpu_dri_nodes", commands["gpu.dri.nodes"].command)
        self.assertIn("gpu.vaapi.vainfo", commands)
        self.assertIn("gpu_vaapi_vainfo", commands["gpu.vaapi.vainfo"].command)
        self.assertIn("vehicle.preflight", commands)
        self.assertIn("--hardware-device /dev/dri/renderD128", commands["vehicle.preflight"].command)
        self.assertIn("--hardware-device /dev/dri/card1", commands["vehicle.preflight"].command)
        self.assertIn("vehicle.adapter.status", commands)
        self.assertTrue(commands["vehicle.adapter.status"].required)
        self.assertIn(
            "vehicle-agent/vehicle_agent.py --config /etc/mine-teleop/vehicle-agent.yaml --adapter-status",
            commands["vehicle.adapter.status"].command,
        )
        self.assertIn("vehicle.adapter.feedback_poll", commands)
        self.assertTrue(commands["vehicle.adapter.feedback_poll"].required)
        self.assertIn(
            "vehicle-agent/vehicle_agent.py --config /etc/mine-teleop/vehicle-agent.yaml "
            "--adapter-status --poll-feedback --require-feedback",
            commands["vehicle.adapter.feedback_poll"].command,
        )
        self.assertIn("vehicle.uploader.process_once", commands)
        self.assertTrue(commands["vehicle.uploader.process_once"].required)
        self.assertIn(
            "vehicle-uploader/vehicle_uploader.py --service-mode --process-once "
            "--config /etc/mine-teleop/vehicle-agent.yaml --work-dir /var/lib/mine-teleop/uploader --json",
            commands["vehicle.uploader.process_once"].command,
        )
        self.assertIn("chassis.bridge.check", commands)
        self.assertIn("scripts/chassis_bridge_check.py", commands["chassis.bridge.check"].command)
        self.assertRegex(commands["chassis.bridge.check"].command, r"(?:^| )--build(?: |$)")
        self.assertIn("--chassis-control-branch UI_Test", commands["chassis.bridge.check"].command)
        self.assertIn("--minepilot-branch merge_ui_test", commands["chassis.bridge.check"].command)
        self.assertIn(
            f"--chassis-control-library {_CHASSIS_CONTROL_LIBRARY_PATH}",
            commands["chassis.bridge.check"].command,
        )
        self.assertIn("media.hardware.probes", commands)
        self.assertIn(
            "vehicle-media-agent/vehicle_media_agent.py --config /etc/mine-teleop/vehicle-agent.yaml --mode hardware-probes",
            commands["media.hardware.probes"].command,
        )
        self.assertIn("network.weak.matrix", commands)
        self.assertIn("scripts/netem_plan.py --interface wwan0 --matrix", commands["network.weak.matrix"].command)
        self.assertIn("can.interface.show", commands)
        self.assertIn("ip -details link show can0", commands["can.interface.show"].command)
        self.assertIn("can_interface_state", commands["can.interface.show"].command)
        self.assertIn("minepilot.can.sources", commands)
        self.assertIn("src/can_db.cpp", commands["minepilot.can.sources"].command)
        self.assertIn("include/can/can_common.h", commands["minepilot.can.sources"].command)
        self.assertIn("include/can/can_message.h", commands["minepilot.can.sources"].command)
        self.assertIn("include/can_receiver.h", commands["minepilot.can.sources"].command)
        self.assertIn("src/can_receiver.cpp", commands["minepilot.can.sources"].command)
        self.assertIn("include/can_sender.h", commands["minepilot.can.sources"].command)
        self.assertIn("src/can_sender.cpp", commands["minepilot.can.sources"].command)
        self.assertIn("minepilot_can_sources", commands["minepilot.can.sources"].command)
        self.assertIn("minepilot.can.socket.probe", commands)
        self.assertIn("script/check_can.sh can0", commands["minepilot.can.socket.probe"].command)
        self.assertIn("minepilot_can_socket_probe", commands["minepilot.can.socket.probe"].command)
        self.assertIn("minepilot.can.sender.build", commands)
        self.assertIn("-DBUILD_TESTING=ON", commands["minepilot.can.sender.build"].command)
        self.assertIn("--target can_sender_main", commands["minepilot.can.sender.build"].command)
        self.assertIn("minepilot_can_sender_build", commands["minepilot.can.sender.build"].command)
        self.assertIn("minepilot.can.sender.smoke", commands)
        self.assertIn("probe_output=$(timeout 3s", commands["minepilot.can.sender.smoke"].command)
        self.assertIn("timeout 3s", commands["minepilot.can.sender.smoke"].command)
        self.assertIn("can_sender_main can0", commands["minepilot.can.sender.smoke"].command)
        self.assertIn('"$status" -eq 0', commands["minepilot.can.sender.smoke"].command)
        self.assertIn('"$status" -eq 124', commands["minepilot.can.sender.smoke"].command)
        self.assertIn("MINE_TELEOP_CAN_SMOKE_OUTPUT", commands["minepilot.can.sender.smoke"].command)
        self.assertIn("minepilot_can_sender_smoke", commands["minepilot.can.sender.smoke"].command)
        self.assertIn("startup_banner_seen", commands["minepilot.can.sender.smoke"].command)
        self.assertEqual(commands["media.hardware.report.template"].required, False)
        self.assertIn("acceptance.metrics.report", commands)
        self.assertEqual(commands["acceptance.metrics.report"].required, False)
        self.assertIn("scripts/acceptance_metrics_report.py", commands["acceptance.metrics.report"].command)
        self.assertIn("--samples /tmp/mine-teleop-acceptance-samples.jsonl", commands["acceptance.metrics.report"].command)
        self.assertIn("--scenario target-host-acceptance", commands["acceptance.metrics.report"].command)

        records = [json.loads(line) for line in plan.to_jsonl()]
        self.assertEqual(records[0]["event"], "target_host_validation_plan")
        self.assertEqual(records[0]["command_count"], len(plan.commands))
        self.assertEqual(records[0]["optional_count"], sum(1 for command in plan.commands if not command.required))
        self.assertEqual(records[0]["command_names"], [command.name for command in plan.commands])
        self.assertEqual(records[0]["command_requirements"], {command.name: command.required for command in plan.commands})
        self.assertEqual(records[0]["vehicle_config_path"], "/etc/mine-teleop/vehicle-agent.yaml")
        self.assertEqual(records[0]["hardware_devices"], ["/dev/dri/renderD128", "/dev/dri/card1"])
        self.assertEqual(records[0]["can_interface"], "can0")
        self.assertEqual(records[0]["network_interface"], "wwan0")
        self.assertEqual(records[0]["chassis_control_root"], "/Volumes/SystemDisk/Workspace/ChassisControl")
        self.assertEqual(records[0]["minepilot_root"], "/Volumes/SystemDisk/Workspace/MinePilot")
        self.assertEqual(records[0]["uploader_work_dir"], "/var/lib/mine-teleop/uploader")
        self.assertEqual(records[0]["bridge_library_path"], _BRIDGE_LIBRARY_PATH)
        self.assertEqual(records[0]["chassis_control_library_path"], _CHASSIS_CONTROL_LIBRARY_PATH)
        self.assertEqual(records[0]["chassis_control_branch"], "UI_Test")
        self.assertEqual(records[0]["minepilot_branch"], "merge_ui_test")
        self.assertEqual(records[1]["event"], "target_host_validation_command")

    def test_target_host_validation_plan_uses_configured_vaapi_render_device(self):
        plan = TargetHostValidationPlan.default(
            hardware_devices=("/dev/dri/card2", "/dev/dri/renderD129"),
        )

        commands = {command.name: command for command in plan.commands}

        self.assertIn(
            "LIBVA_DRIVERS_PATH=/usr/lib/x86_64-linux-gnu/dri vainfo --display drm --device /dev/dri/renderD129",
            commands["gpu.vaapi.vainfo"].command,
        )
        self.assertIn("gpu_vaapi_vainfo", commands["gpu.vaapi.vainfo"].command)
        self.assertIn('"/dev/dri/renderD129"', commands["gpu.vaapi.vainfo"].command)
        self.assertIn("--hardware-device /dev/dri/card2", commands["vehicle.preflight"].command)
        self.assertIn("--hardware-device /dev/dri/renderD129", commands["vehicle.preflight"].command)

    def test_target_host_validation_plan_cli_uses_vehicle_config_hardware_defaults(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = Path(tmp) / "vehicle-agent.yaml"
            data = yaml.safe_load(Path("configs/vehicle-agent.dev.yaml").read_text(encoding="utf-8"))
            data["hardware"]["can"]["interface"] = "can42"
            data["hardware"]["can"]["probe_timeout_seconds"] = 7
            data["hardware"]["encoding"]["vaapi_render_device"] = "/dev/dri/renderD129"
            data["hardware"]["encoding"]["dri_card_device"] = "/dev/dri/card2"
            data["hardware"]["encoding"]["ffmpeg_binary"] = "/opt/mine-teleop/bin/ffmpeg"
            data["hardware"]["encoding"]["ffprobe_binary"] = "/opt/mine-teleop/bin/ffprobe"
            data["hardware"]["encoding"]["vainfo_binary"] = "/opt/mine-teleop/bin/vainfo"
            data["hardware"]["encoding"]["libva_drivers_path"] = "/opt/mine-teleop/lib/dri"
            data["hardware"]["network"]["interface"] = "wwan1"
            config_path.write_text(yaml.safe_dump(data, sort_keys=False), encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/target_host_validation_plan.py",
                    "--vehicle-config",
                    str(config_path),
                    "--format",
                    "jsonl",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        summary = records[0]
        commands = {record["name"]: record for record in records[1:]}

        self.assertEqual(summary["can_interface"], "can42")
        self.assertEqual(summary["hardware_devices"], ["/dev/dri/renderD129", "/dev/dri/card2"])
        self.assertEqual(summary["network_interface"], "wwan1")
        self.assertEqual(summary["can_probe_timeout_seconds"], 7)
        self.assertEqual(summary["ffmpeg_binary"], "/opt/mine-teleop/bin/ffmpeg")
        self.assertEqual(summary["ffprobe_binary"], "/opt/mine-teleop/bin/ffprobe")
        self.assertEqual(summary["vainfo_binary"], "/opt/mine-teleop/bin/vainfo")
        self.assertEqual(summary["libva_drivers_path"], "/opt/mine-teleop/lib/dri")
        self.assertIn("ip -details link show can42", commands["can.interface.show"]["command"])
        self.assertIn("script/check_can.sh can42", commands["minepilot.can.socket.probe"]["command"])
        self.assertIn("timeout 7s", commands["minepilot.can.sender.smoke"]["command"])
        self.assertIn("can_sender_main can42", commands["minepilot.can.sender.smoke"]["command"])
        self.assertIn(
            "LIBVA_DRIVERS_PATH=/opt/mine-teleop/lib/dri /opt/mine-teleop/bin/vainfo --display drm --device /dev/dri/renderD129",
            commands["gpu.vaapi.vainfo"]["command"],
        )
        self.assertIn("--hardware-device /dev/dri/renderD129", commands["vehicle.preflight"]["command"])
        self.assertIn("--hardware-device /dev/dri/card2", commands["vehicle.preflight"]["command"])
        self.assertIn("scripts/netem_plan.py --interface wwan1 --matrix", commands["network.weak.matrix"]["command"])

    def test_target_host_validation_plan_cli_prints_shell_script(self):
        result = subprocess.run(
            [
                sys.executable,
                "scripts/target_host_validation_plan.py",
                "--vehicle-config",
                "/etc/mine-teleop/vehicle-agent.yaml",
                "--hardware-device",
                "/dev/dri/renderD128",
                "--hardware-device",
                "/dev/dri/card1",
                "--can-interface",
                "can0",
                "--network-interface",
                "wwan0",
                "--format",
                "shell",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("set -euo pipefail", result.stdout)
        self.assertIn("python3 vehicle-agent/vehicle_agent.py --config /etc/mine-teleop/vehicle-agent.yaml --preflight", result.stdout)
        self.assertIn("python3 vehicle-agent/vehicle_agent.py --config /etc/mine-teleop/vehicle-agent.yaml --adapter-status", result.stdout)
        self.assertIn(
            "python3 vehicle-agent/vehicle_agent.py --config /etc/mine-teleop/vehicle-agent.yaml "
            "--adapter-status --poll-feedback --require-feedback",
            result.stdout,
        )
        self.assertIn(
            "python3 vehicle-uploader/vehicle_uploader.py --service-mode --process-once "
            "--config /etc/mine-teleop/vehicle-agent.yaml --work-dir /var/lib/mine-teleop/uploader --json",
            result.stdout,
        )
        self.assertIn("python3 scripts/chassis_bridge_check.py", result.stdout)
        self.assertIn(
            "python3 vehicle-media-agent/vehicle_media_agent.py "
            "--config /etc/mine-teleop/vehicle-agent.yaml --mode hardware-probes",
            result.stdout,
        )
        self.assertIn("python3 scripts/netem_plan.py --interface wwan0 --matrix", result.stdout)
        self.assertIn("MinePilot/script/check_can.sh can0", result.stdout)
        self.assertIn("can_sender_main can0", result.stdout)
        self.assertIn("--chassis-control-branch UI_Test", result.stdout)
        self.assertIn("--minepilot-branch merge_ui_test", result.stdout)
        self.assertIn("python3 scripts/acceptance_metrics_report.py --samples /tmp/mine-teleop-acceptance-samples.jsonl --scenario target-host-acceptance", result.stdout)

    def test_target_host_validation_plan_cli_can_emit_bundle_entrypoint_shell_script(self):
        result = subprocess.run(
            [
                sys.executable,
                "scripts/target_host_validation_plan.py",
                "--vehicle-config",
                "/etc/mine-teleop/vehicle-agent.yaml",
                "--hardware-device",
                "/dev/dri/renderD128",
                "--hardware-device",
                "/dev/dri/card1",
                "--can-interface",
                "can1",
                "--network-interface",
                "wwan0",
                "--mine-teleop-binary",
                "/opt/mine-teleop/bin/mine-teleop",
                "--ffmpeg-binary",
                "/opt/mine-teleop/bin/ffmpeg",
                "--ffprobe-binary",
                "/opt/mine-teleop/bin/ffprobe",
                "--vainfo-binary",
                "/opt/mine-teleop/bin/vainfo",
                "--libva-drivers-path",
                "/opt/mine-teleop/lib/dri",
                "--format",
                "shell",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn(
            "/opt/mine-teleop/bin/mine-teleop vehicle-agent "
            "--config /etc/mine-teleop/vehicle-agent.yaml --preflight",
            result.stdout,
        )
        self.assertIn(
            "/opt/mine-teleop/bin/mine-teleop vehicle-agent "
            "--config /etc/mine-teleop/vehicle-agent.yaml --adapter-status",
            result.stdout,
        )
        self.assertIn(
            "/opt/mine-teleop/bin/mine-teleop vehicle-agent "
            "--config /etc/mine-teleop/vehicle-agent.yaml --adapter-status --poll-feedback --require-feedback",
            result.stdout,
        )
        self.assertIn(
            "/opt/mine-teleop/bin/mine-teleop vehicle-uploader --service-mode --process-once "
            "--config /etc/mine-teleop/vehicle-agent.yaml --work-dir /var/lib/mine-teleop/uploader --json",
            result.stdout,
        )
        self.assertIn(
            "/opt/mine-teleop/bin/mine-teleop chassis-bridge-check "
            "--chassis-control-root /Volumes/SystemDisk/Workspace/ChassisControl",
            result.stdout,
        )
        self.assertIn("--skip-cmake", result.stdout)
        self.assertIn("/opt/mine-teleop/bin/mine-teleop vehicle-media-agent --config /etc/mine-teleop/vehicle-agent.yaml --mode hardware-probes", result.stdout)
        self.assertIn("/opt/mine-teleop/bin/mine-teleop netem-plan --interface wwan0 --matrix", result.stdout)
        self.assertIn("/opt/mine-teleop/bin/mine-teleop acceptance-metrics-report --samples /tmp/mine-teleop-acceptance-samples.jsonl --scenario target-host-acceptance", result.stdout)
        self.assertIn("LIBVA_DRIVERS_PATH=/opt/mine-teleop/lib/dri /opt/mine-teleop/bin/vainfo --display drm", result.stdout)
        self.assertNotIn("python3 vehicle-agent/vehicle_agent.py", result.stdout)
        self.assertNotIn("python3 scripts/chassis_bridge_check.py", result.stdout)

    def test_target_host_validation_plan_cli_can_emit_artifact_archiving_shell_script(self):
        result = subprocess.run(
            [
                sys.executable,
                "scripts/target_host_validation_plan.py",
                "--format",
                "shell",
                "--artifact-dir",
                "/tmp/mine-teleop-target-validation",
            ],
            cwd=Path.cwd(),
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("artifact_dir=/tmp/mine-teleop-target-validation", result.stdout)
        self.assertIn("target_host_validation_results.jsonl", result.stdout)
        self.assertIn("run_validation_command gpu.pci.summary required", result.stdout)
        self.assertIn("lspci -nnk | grep -EA4", result.stdout)
        self.assertIn("stdout_path", result.stdout)
        self.assertIn("stderr_path", result.stdout)
        self.assertIn("required_failures", result.stdout)
        self.assertIn("command_names", result.stdout)
        self.assertIn("command_requirements", result.stdout)
        self.assertIn("vehicle_config_path", result.stdout)
        self.assertIn("chassis_control_branch", result.stdout)
        self.assertIn("minepilot_branch", result.stdout)
        self.assertIn("--chassis-control-branch UI_Test", result.stdout)
        self.assertIn("--minepilot-branch merge_ui_test", result.stdout)
        self.assertIn("target_host_validation_archive.jsonl", result.stdout)
        self.assertIn("scripts/target_host_validation_report.py", result.stdout)
        self.assertIn("--verify-artifacts", result.stdout)
        self.assertIn('exit "$report_status"', result.stdout)

    def test_target_host_validation_artifact_script_records_summary_after_all_commands(self):
        plan = TargetHostValidationPlan(
            commands=(
                ValidationCommand("required_ok", "printf required-ok", True, "required success"),
                ValidationCommand("optional_bad", "printf optional-bad >&2; exit 7", False, "optional failure"),
                ValidationCommand("required_ok_2", "printf required-ok-2", True, "second required success"),
            )
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = root / "target_validation.sh"
            artifact_dir = root / "artifacts"
            script.write_text(plan.to_shell_script(artifact_dir=str(artifact_dir)), encoding="utf-8")
            script.chmod(0o755)

            result = subprocess.run(
                [str(script)],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            records = [
                json.loads(line)
                for line in (artifact_dir / "target_host_validation_results.jsonl").read_text(encoding="utf-8").splitlines()
            ]
            first_stdout_exists = Path(records[0]["stdout_path"]).exists()
            optional_stderr_exists = Path(records[1]["stderr_path"]).exists()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([record["event"] for record in records[:-1]], ["target_host_validation_result"] * 3)
        summary = records[-1]
        self.assertEqual(summary["event"], "target_host_validation_summary")
        self.assertTrue(summary["passed"])
        self.assertEqual(summary["command_count"], 3)
        self.assertEqual(summary["required_count"], 2)
        self.assertEqual(summary["optional_count"], 1)
        self.assertEqual(summary["command_names"], ["required_ok", "optional_bad", "required_ok_2"])
        self.assertEqual(
            summary["command_requirements"],
            {"optional_bad": False, "required_ok": True, "required_ok_2": True},
        )
        self.assertEqual(summary["vehicle_config_path"], "/etc/mine-teleop/vehicle-agent.yaml")
        self.assertEqual(summary["can_interface"], "can0")
        self.assertEqual(summary["chassis_control_root"], "/Volumes/SystemDisk/Workspace/ChassisControl")
        self.assertEqual(summary["minepilot_root"], "/Volumes/SystemDisk/Workspace/MinePilot")
        self.assertEqual(summary["chassis_control_branch"], "UI_Test")
        self.assertEqual(summary["minepilot_branch"], "merge_ui_test")
        self.assertEqual(summary["chassis_control_library_path"], _CHASSIS_CONTROL_LIBRARY_PATH)
        self.assertEqual(summary["required_failures"], 0)
        self.assertEqual(summary["optional_failures"], 1)
        self.assertEqual(records[1]["returncode"], 7)
        self.assertTrue(first_stdout_exists)
        self.assertTrue(optional_stderr_exists)

    def test_target_host_validation_artifact_summary_records_external_checkout_revisions(self):
        chassis_commit = "1" * 40
        minepilot_commit = "2" * 40
        stdout_records = [
            {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES)},
        ]
        for check_name in _BRIDGE_READY_CHECK_NAMES:
            message = _bridge_ready_message(check_name)
            if check_name == "chassis_control.commit":
                message = f"/tmp/ChassisControl HEAD commit={chassis_commit}"
            if check_name == "minepilot.commit":
                message = f"/tmp/MinePilot HEAD commit={minepilot_commit}"
            if check_name == "chassis_control.dirty":
                message = "/tmp/ChassisControl dirty=true changed_paths=14"
            if check_name == "minepilot.dirty":
                message = "/tmp/MinePilot dirty=false changed_paths=0"
            stdout_records.append(
                {
                    "name": check_name,
                    "path": _bridge_ready_path(check_name),
                    "status": "ready",
                    "message": message,
                }
            )
        stdout_payload = "\n".join(json.dumps(record, sort_keys=True) for record in stdout_records)
        plan = TargetHostValidationPlan(
            commands=(
                ValidationCommand(
                    "chassis.bridge.check",
                    f"printf '%s\\n' {shlex.quote(stdout_payload)}",
                    True,
                    "bridge check success",
                ),
            )
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = root / "target_validation.sh"
            artifact_dir = root / "artifacts"
            script.write_text(plan.to_shell_script(artifact_dir=str(artifact_dir)), encoding="utf-8")
            script.chmod(0o755)

            result = subprocess.run(
                [str(script)],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            records = [
                json.loads(line)
                for line in (artifact_dir / "target_host_validation_results.jsonl").read_text(encoding="utf-8").splitlines()
            ]

        self.assertEqual(result.returncode, 0, result.stderr)
        summary = records[-1]
        self.assertEqual(summary["event"], "target_host_validation_summary")
        self.assertEqual(summary["chassis_control_commit"], chassis_commit)
        self.assertEqual(summary["minepilot_commit"], minepilot_commit)
        self.assertEqual(summary["chassis_control_dirty"], True)
        self.assertEqual(summary["minepilot_dirty"], False)
        self.assertEqual(summary["chassis_control_changed_paths"], 14)
        self.assertEqual(summary["minepilot_changed_paths"], 0)
        self.assertEqual(summary["bridge_library_path"], _BRIDGE_LIBRARY_PATH)
        self.assertEqual(summary["chassis_control_library_path"], _CHASSIS_CONTROL_LIBRARY_PATH)

    def test_target_host_validation_artifact_script_fails_when_feedback_report_lacks_received_evidence(self):
        feedback_json = json.dumps(
            {
                "event": "vehicle_adapter_feedback_poll",
                "vehicle_id": "vehicle-001",
                "attempted": True,
                "received": False,
                "reason": "no_feedback_frame",
            },
            sort_keys=True,
        )
        plan = TargetHostValidationPlan(
            commands=(
                ValidationCommand(
                    "vehicle.adapter.feedback_poll",
                    f"printf '%s\\n' {shlex.quote(feedback_json)}",
                    True,
                    "feedback command returned zero without decoded CAN feedback",
                ),
            )
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            script = root / "target_validation.sh"
            artifact_dir = root / "artifacts"
            script.write_text(plan.to_shell_script(artifact_dir=str(artifact_dir)), encoding="utf-8")
            script.chmod(0o755)

            result = subprocess.run(
                [str(script)],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            archive_report_path = artifact_dir / "target_host_validation_archive.jsonl"
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertTrue(archive_report_path.exists())
            archive_report = [
                json.loads(line)
                for line in archive_report_path.read_text(encoding="utf-8").splitlines()
            ]

        self.assertFalse(archive_report[0]["passed"])
        self.assertEqual(
            archive_report[0]["evidence_failed"],
            [
                {
                    "name": "vehicle.adapter.feedback_poll",
                    "reason": "vehicle_adapter_feedback_poll_not_received",
                    "stdout_path": str(artifact_dir / "vehicle.adapter.feedback_poll.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_cli_summarizes_failed_archive(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "gpu.vaapi.vainfo",
                    "required": True,
                    "returncode": 0,
                    "command": "vainfo",
                    "stdout_path": "/tmp/vainfo.stdout.log",
                    "stderr_path": "/tmp/vainfo.stderr.log",
                },
                {
                    "event": "target_host_validation_result",
                    "name": "minepilot.can.sender.smoke",
                    "required": True,
                    "returncode": 2,
                    "command": "can sender smoke",
                    "stdout_path": "/tmp/can.stdout.log",
                    "stderr_path": "/tmp/can.stderr.log",
                },
                {
                    "event": "target_host_validation_result",
                    "name": "network.weak.matrix",
                    "required": False,
                    "returncode": 1,
                    "command": "netem matrix",
                    "stdout_path": "/tmp/netem.stdout.log",
                    "stderr_path": "/tmp/netem.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 2,
                    "optional_count": 1,
                    "required_failures": 1,
                    "optional_failures": 1,
                    "command_names": [
                        "gpu.vaapi.vainfo",
                        "minepilot.can.sender.smoke",
                        "network.weak.matrix",
                    ],
                    "passed": False,
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(report), 1)
        self.assertEqual(report[0]["event"], "target_host_validation_archive")
        self.assertFalse(report[0]["passed"])
        self.assertEqual(report[0]["result_count"], 3)
        self.assertEqual(report[0]["required_failed"], ["minepilot.can.sender.smoke"])
        self.assertEqual(report[0]["optional_failed"], ["network.weak.matrix"])
        self.assertEqual(report[0]["summary"]["required_failures"], 1)

    def test_target_host_validation_report_requires_weak_network_matrix_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "network.weak.matrix.stdout.log"
            stderr_path = root / "network.weak.matrix.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text("", encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "network.weak.matrix",
                    "required": False,
                    "returncode": 0,
                    "command": "python3 scripts/netem_plan.py --interface wwan0 --matrix",
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
                    "command_names": ["network.weak.matrix"],
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
                    "name": "network.weak.matrix",
                    "reason": "weak_network_matrix_missing_warning",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_unexpected_weak_network_profile(self):
        matrix = subprocess.run(
            [
                sys.executable,
                "scripts/netem_plan.py",
                "--interface",
                "wwan0",
                "--matrix",
            ],
            cwd=Path.cwd(),
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "network.weak.matrix.stdout.log"
            stderr_path = root / "network.weak.matrix.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(matrix.stdout + "profile=weak-stale-extra\n", encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "network.weak.matrix",
                    "required": False,
                    "returncode": 0,
                    "command": "python3 scripts/netem_plan.py --interface wwan0 --matrix",
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
                    "command_names": ["network.weak.matrix"],
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
                    "name": "network.weak.matrix",
                    "reason": "weak_network_matrix_unexpected_profiles",
                    "stdout_path": str(stdout_path),
                    "unexpected": "weak-stale-extra",
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_weak_network_profile(self):
        matrix = subprocess.run(
            [
                sys.executable,
                "scripts/netem_plan.py",
                "--interface",
                "wwan0",
                "--matrix",
            ],
            cwd=Path.cwd(),
            check=True,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "network.weak.matrix.stdout.log"
            stderr_path = root / "network.weak.matrix.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(matrix.stdout + "profile=weak-50ms-jitter20-loss1-bandwidth5\n", encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "network.weak.matrix",
                    "required": False,
                    "returncode": 0,
                    "command": "python3 scripts/netem_plan.py --interface wwan0 --matrix",
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
                    "command_names": ["network.weak.matrix"],
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
                    "name": "network.weak.matrix",
                    "profile": "weak-50ms-jitter20-loss1-bandwidth5",
                    "reason": "weak_network_matrix_duplicate_profiles",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_chassis_bridge_build_ready_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES) - 1},
                *(
                    {
                        "name": check_name,
                        "path": "/tmp/bridge",
                        "status": "ready",
                        "message": f"{check_name} ready",
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                    if check_name != "cmake.build"
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "missing": "cmake.build",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_missing_ready_checks",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_chassis_bridge_check_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES)},
                {
                    "name": "cmake.build",
                    "path": _bridge_ready_path("cmake.build"),
                    "status": "failed",
                    "message": "cmake build failed before a later duplicate ready record",
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "check": "cmake.build",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_duplicate_check",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_chassis_bridge_ready_summary_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES)},
                {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES)},
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_duplicate_ready_summary",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_chassis_bridge_summary_check_count_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES) - 1},
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": len(_BRIDGE_READY_CHECK_NAMES),
                    "expected": len(_BRIDGE_READY_CHECK_NAMES) - 1,
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_count_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_invalid_chassis_bridge_summary_check_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {"event": "chassis_bridge_check", "ready": True, "check_count": float(len(_BRIDGE_READY_CHECK_NAMES))},
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_invalid_summary",
                    "stdout_path": str(stdout_path),
                    "summary": str(float(len(_BRIDGE_READY_CHECK_NAMES))),
                }
            ],
        )

    def test_target_host_validation_report_rejects_chassis_bridge_build_dir_summary_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": (
                            "/tmp/other-bridge-build"
                            if check_name == "cmake.build"
                            else _bridge_ready_path(check_name)
                        ),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": "/tmp/other-bridge-build",
                    "check": "cmake.build",
                    "expected": "build/chassis-control-bridge",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_build_dir_summary_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_chassis_bridge_library_outside_checkout_roots(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": (
                            "/tmp/unrelated/libchassis_control.so"
                            if check_name == "chassis_control.library"
                            else _bridge_ready_path(check_name)
                        ),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": "/tmp/unrelated/libchassis_control.so",
                    "check": "chassis_control.library",
                    "expected": "/Volumes/SystemDisk/Workspace/ChassisControl,/Volumes/SystemDisk/Workspace/MinePilot",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_library_root_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_chassis_bridge_library_summary_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata["chassis_control_library_path"] = "/tmp/other/libchassis_control.so"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": _CHASSIS_CONTROL_LIBRARY_PATH,
                    "check": "chassis_control.library",
                    "expected": "/tmp/other/libchassis_control.so",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_library_summary_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_chassis_bridge_symbols_path_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            bridge_records = [
                {"event": "chassis_bridge_check", "ready": True, "check_count": len(_BRIDGE_READY_CHECK_NAMES)},
                *[
                    {
                        "name": check_name,
                        "path": (
                            "/Volumes/SystemDisk/Workspace/ChassisControl/build/lib/libchassis_control.so"
                            if check_name == "chassis_control.symbols"
                            else _bridge_ready_path(check_name)
                        ),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ],
            ]
            stdout_path.write_text(
                "\n".join(json.dumps(record, sort_keys=True) for record in bridge_records) + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": "/Volumes/SystemDisk/Workspace/ChassisControl/build/lib/libchassis_control.so",
                    "check": "chassis_control.symbols",
                    "expected": _CHASSIS_CONTROL_LIBRARY_PATH,
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_symbols_library_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_docs_describe_bridge_build_dir_binding(self):
        testing_doc = Path("docs/11-testing-and-validation.md").read_text(encoding="utf-8")
        operations_doc = Path("docs/12-operations-and-troubleshooting.md").read_text(encoding="utf-8")

        for content in (testing_doc, operations_doc):
            with self.subTest(document=content.splitlines()[0]):
                self.assertIn("bridge_build_dir", content)
                self.assertIn("cmake.configure", content)
                self.assertIn("cmake.build", content)
                self.assertIn("check_count", content)
                self.assertIn("summary `bridge_build_dir`", content)
                self.assertIn("`chassis_control.symbols`", content)
        self.assertIn("`--skip-cmake` 不能和 `--build` 混用", operations_doc)
        self.assertIn("`chassis_control.library`", operations_doc)
        self.assertIn("必须位于 ChassisControl 或 MinePilot checkout", operations_doc)

    def test_target_host_validation_docs_describe_external_checkout_revision_summary(self):
        testing_doc = Path("docs/11-testing-and-validation.md").read_text(encoding="utf-8")
        operations_doc = Path("docs/12-operations-and-troubleshooting.md").read_text(encoding="utf-8")

        for content in (testing_doc, operations_doc):
            with self.subTest(document=content.splitlines()[0]):
                self.assertIn("chassis_control_commit", content)
                self.assertIn("minepilot_commit", content)
                self.assertIn("chassis_control_dirty", content)
                self.assertIn("minepilot_dirty", content)
                self.assertIn("changed_paths", content)
                self.assertIn("chassis_control_library_path", content)
                self.assertIn("commit/dirty", content)
                self.assertIn("revision summary", content)
                self.assertIn("revision summary 缺失", content)
                self.assertIn("revision summary 无效", content)

    def test_target_host_validation_docs_reject_extra_hardware_probe_scenarios(self):
        testing_doc = Path("docs/11-testing-and-validation.md").read_text(encoding="utf-8")
        operations_doc = Path("docs/12-operations-and-troubleshooting.md").read_text(encoding="utf-8")

        for content in (testing_doc, operations_doc):
            with self.subTest(document=content.splitlines()[0]):
                self.assertIn("media.hardware.probes", content)
                self.assertIn("额外场景", content)

    def test_target_host_validation_docs_reject_extra_weak_network_profiles(self):
        testing_doc = Path("docs/11-testing-and-validation.md").read_text(encoding="utf-8")
        operations_doc = Path("docs/12-operations-and-troubleshooting.md").read_text(encoding="utf-8")

        for content in (testing_doc, operations_doc):
            with self.subTest(document=content.splitlines()[0]):
                self.assertIn("network.weak.matrix", content)
                self.assertIn("额外 profile", content)

    def test_target_host_validation_docs_reject_failed_acceptance_metric_reports(self):
        testing_doc = Path("docs/11-testing-and-validation.md").read_text(encoding="utf-8")
        operations_doc = Path("docs/12-operations-and-troubleshooting.md").read_text(encoding="utf-8")

        for content in (testing_doc, operations_doc):
            with self.subTest(document=content.splitlines()[0]):
                self.assertIn("acceptance.metrics.report", content)
                self.assertIn("显式失败", content)

    def test_target_host_validation_docs_require_acceptance_scenario_summary(self):
        testing_doc = Path("docs/11-testing-and-validation.md").read_text(encoding="utf-8")
        operations_doc = Path("docs/12-operations-and-troubleshooting.md").read_text(encoding="utf-8")

        for content in (testing_doc, operations_doc):
            with self.subTest(document=content.splitlines()[0]):
                self.assertIn("acceptance_scenario", content)
                self.assertIn("summary", content)

    def test_target_host_validation_report_requires_chassis_bridge_dirty_state_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            missing_dirty_checks = {"chassis_control.dirty", "minepilot.dirty"}
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES) - len(missing_dirty_checks),
                },
                *(
                    {
                        "name": check_name,
                        "path": "/tmp/bridge",
                        "status": "ready",
                        "message": f"{check_name} ready",
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                    if check_name not in missing_dirty_checks
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "missing": "chassis_control.dirty,minepilot.dirty",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_missing_ready_checks",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_chassis_bridge_commit_and_dirty_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": (
                            _bridge_ready_message(check_name)
                            if check_name in {"chassis_control.branch", "minepilot.branch"}
                            else f"{check_name} ready"
                        ),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "check": "chassis_control.commit",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_missing_commit_metadata",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_chassis_bridge_revision_summary_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata.update(
                {
                    "chassis_control_commit": "b" * 40,
                    "minepilot_commit": "a" * 40,
                    "chassis_control_dirty": False,
                    "minepilot_dirty": False,
                    "chassis_control_changed_paths": 0,
                    "minepilot_changed_paths": 0,
                }
            )
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": "a" * 40,
                    "check": "chassis_control.commit",
                    "expected": "b" * 40,
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_revision_summary_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_chassis_bridge_revision_summary_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata.pop("chassis_control_commit")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "check": "chassis_control.commit",
                    "field": "chassis_control_commit",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_revision_summary_missing",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_invalid_chassis_bridge_revision_commit_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata["chassis_control_commit"] = "not-a-commit"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "check": "chassis_control.commit",
                    "field": "chassis_control_commit",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_revision_summary_invalid",
                    "stdout_path": str(stdout_path),
                    "summary": "not-a-commit",
                }
            ],
        )

    def test_target_host_validation_report_rejects_invalid_chassis_bridge_dirty_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata["minepilot_dirty"] = "false"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "check": "minepilot.dirty",
                    "field": "minepilot_dirty",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_revision_summary_invalid",
                    "stdout_path": str(stdout_path),
                    "summary": "false",
                }
            ],
        )

    def test_target_host_validation_report_rejects_invalid_chassis_bridge_changed_paths_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata["minepilot_changed_paths"] = True
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "check": "minepilot.dirty",
                    "field": "minepilot_changed_paths",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_revision_summary_invalid",
                    "stdout_path": str(stdout_path),
                    "summary": "True",
                }
            ],
        )

    def test_target_host_validation_report_rejects_chassis_bridge_dirty_summary_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata.update(
                {
                    "chassis_control_commit": "a" * 40,
                    "minepilot_commit": "a" * 40,
                    "chassis_control_dirty": False,
                    "minepilot_dirty": True,
                    "chassis_control_changed_paths": 0,
                    "minepilot_changed_paths": 0,
                }
            )
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": "false",
                    "check": "minepilot.dirty",
                    "expected": "true",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_revision_summary_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_chassis_bridge_branch_summary_match(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": _bridge_ready_path(check_name),
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata["chassis_control_branch"] = "main"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": "UI_Test",
                    "check": "chassis_control.branch",
                    "expected": "main",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_branch_summary_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_chassis_bridge_root_summary_match(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "chassis.bridge.check.stdout.log"
            stderr_path = root / "chassis.bridge.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "event": "chassis_bridge_check",
                    "ready": True,
                    "check_count": len(_BRIDGE_READY_CHECK_NAMES),
                },
                *(
                    {
                        "name": check_name,
                        "path": "/tmp/wrong-checkout",
                        "status": "ready",
                        "message": _bridge_ready_message(check_name),
                    }
                    for check_name in _BRIDGE_READY_CHECK_NAMES
                ),
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "chassis.bridge.check",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 scripts/chassis_bridge_check.py --build",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["chassis.bridge.check"],
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
                    "actual": "/tmp/wrong-checkout",
                    "check": "chassis_control.root",
                    "expected": "/Volumes/SystemDisk/Workspace/ChassisControl",
                    "name": "chassis.bridge.check",
                    "reason": "chassis_bridge_check_root_summary_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_preflight_ready_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.preflight.stdout.log"
            stderr_path = root / "vehicle.preflight.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text("", encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.preflight",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 vehicle-agent/vehicle_agent.py --preflight",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.preflight"],
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
                    "event": "vehicle_preflight",
                    "name": "vehicle.preflight",
                    "reason": "vehicle_preflight_missing_ready_summary",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_required_command_downgrade(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.preflight.stdout.log"
            stderr_path = root / "vehicle.preflight.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_preflight",
                        "ready": True,
                        "check_count": 3,
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.preflight",
                    "required": False,
                    "returncode": 0,
                    "command": "python3 vehicle-agent/vehicle_agent.py --preflight",
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
                    "command_names": ["vehicle.preflight"],
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
            report[0]["consistency_failed"],
            [
                {
                    "actual": False,
                    "expected": True,
                    "field": "results[0].required",
                    "name": "vehicle.preflight",
                    "reason": "result_required_mismatch",
                }
            ],
        )

    def test_target_host_validation_report_rejects_missing_command_from_summary_requirements(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.preflight.stdout.log"
            stderr_path = root / "vehicle.preflight.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_preflight",
                        "ready": True,
                        "check_count": 3,
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.preflight",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 vehicle-agent/vehicle_agent.py --preflight",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.preflight"],
                    "command_requirements": {
                        "vehicle.preflight": True,
                        "chassis.bridge.check": True,
                    },
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
            report[0]["consistency_failed"],
            [
                {
                    "field": "command_requirements",
                    "missing": "chassis.bridge.check",
                    "reason": "summary_command_missing",
                }
            ],
        )

    def test_target_host_validation_report_rejects_result_missing_from_summary_requirements(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "custom.required.check.stdout.log"
            stderr_path = root / "custom.required.check.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text("custom ok\n", encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "custom.required.check",
                    "required": False,
                    "returncode": 0,
                    "command": "printf custom-ok",
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
                    "command_names": ["custom.required.check"],
                    "command_requirements": {},
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
            report[0]["consistency_failed"],
            [
                {
                    "field": "command_requirements",
                    "missing": "custom.required.check",
                    "reason": "summary_command_requirement_missing",
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_preflight_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.preflight.stdout.log"
            stderr_path = root / "vehicle.preflight.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {"event": "vehicle_preflight", "ready": True, "check_count": 3},
                {"event": "vehicle_preflight", "ready": False, "check_count": 3},
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.preflight",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 vehicle-agent/vehicle_agent.py --preflight",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.preflight"],
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
                    "event": "vehicle_preflight",
                    "name": "vehicle.preflight",
                    "reason": "vehicle_preflight_duplicate_evidence",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_gpu_structured_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = ["gpu.pci.summary", "gpu.dri.nodes", "gpu.vaapi.vainfo"]
            records = []
            for command_name in command_names:
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text("", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "gpu_pci_summary",
                    "name": "gpu.pci.summary",
                    "reason": "gpu_pci_summary_missing_evidence",
                    "stdout_path": str(root / "gpu.pci.summary.stdout.log"),
                },
                {
                    "event": "gpu_dri_nodes",
                    "name": "gpu.dri.nodes",
                    "reason": "gpu_dri_nodes_missing_evidence",
                    "stdout_path": str(root / "gpu.dri.nodes.stdout.log"),
                },
                {
                    "event": "gpu_vaapi_vainfo",
                    "name": "gpu.vaapi.vainfo",
                    "reason": "gpu_vaapi_vainfo_missing_evidence",
                    "stdout_path": str(root / "gpu.vaapi.vainfo.stdout.log"),
                },
            ],
        )

    def test_target_host_validation_report_accepts_gpu_structured_evidence(self):
        evidence = {
            "gpu.pci.summary": {
                "event": "gpu_pci_summary",
                "passed": True,
            },
            "gpu.dri.nodes": {
                "event": "gpu_dri_nodes",
                "passed": True,
                "path": "/dev/dri",
            },
            "gpu.vaapi.vainfo": {
                "device": "/dev/dri/renderD128",
                "event": "gpu_vaapi_vainfo",
                "passed": True,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(
                    json.dumps(stdout_record, sort_keys=True) + "\n",
                    encoding="utf-8",
                )
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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

        self.assertEqual(result.returncode, 0, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(report[0]["passed"])
        self.assertEqual(report[0]["evidence_failed"], [])

    def test_target_host_validation_report_rejects_duplicate_gpu_evidence(self):
        evidence = {
            "gpu.pci.summary": [
                {
                    "event": "gpu_pci_summary",
                    "passed": True,
                }
            ],
            "gpu.dri.nodes": [
                {
                    "event": "gpu_dri_nodes",
                    "passed": True,
                    "path": "/dev/dri",
                },
                {
                    "event": "gpu_dri_nodes",
                    "passed": True,
                    "path": "/tmp/other-dri",
                },
            ],
            "gpu.vaapi.vainfo": [
                {
                    "device": "/dev/dri/renderD128",
                    "event": "gpu_vaapi_vainfo",
                    "passed": True,
                }
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_records in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(
                    "".join(json.dumps(stdout_record, sort_keys=True) + "\n" for stdout_record in stdout_records),
                    encoding="utf-8",
                )
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "gpu_dri_nodes",
                    "name": "gpu.dri.nodes",
                    "reason": "gpu_dri_nodes_duplicate_evidence",
                    "stdout_path": str(root / "gpu.dri.nodes.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_requires_hardware_probe_plan_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "media.hardware.probes.stdout.log"
            stderr_path = root / "media.hardware.probes.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text("", encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "media.hardware.probes",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 vehicle-media-agent/vehicle_media_agent.py --mode hardware-probes",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["media.hardware.probes"],
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
                    "name": "media.hardware.probes",
                    "reason": "hardware_probe_plan_missing_gst_plugin_probe",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_unexpected_hardware_probe_scenario(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "media.hardware.probes.stdout.log"
            stderr_path = root / "media.hardware.probes.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    [
                        "gst_plugin_probe=gst-inspect-1.0 vaapih264enc qsvh264enc vah264enc nvh264enc x264enc",
                        "scenario=four-camera-realtime-720p30",
                        "sudo docker run realtime",
                        "scenario=four-camera-recording-source",
                        "sudo docker run recording",
                        "scenario=four-camera-realtime-plus-recording",
                        "sudo docker run combined",
                        "scenario=stale-hardware-run",
                        "sudo docker run stale",
                        "metrics=cpu_percent,gpu_percent,memory_mb,disk_write_mb_s,temperature_c,encoded_fps,bitrate_kbps,dropped_frames",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "media.hardware.probes",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 vehicle-media-agent/vehicle_media_agent.py --mode hardware-probes",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["media.hardware.probes"],
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
                    "name": "media.hardware.probes",
                    "reason": "hardware_probe_plan_unexpected_scenarios",
                    "stdout_path": str(stdout_path),
                    "unexpected": "stale-hardware-run",
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_hardware_probe_scenario(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "media.hardware.probes.stdout.log"
            stderr_path = root / "media.hardware.probes.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    [
                        "gst_plugin_probe=gst-inspect-1.0 vaapih264enc qsvh264enc vah264enc nvh264enc x264enc",
                        "scenario=four-camera-realtime-720p30",
                        "sudo docker run realtime",
                        "scenario=four-camera-recording-source",
                        "sudo docker run recording",
                        "scenario=four-camera-realtime-plus-recording",
                        "sudo docker run combined",
                        "scenario=four-camera-realtime-720p30",
                        "sudo docker run duplicate-realtime",
                        "metrics=cpu_percent,gpu_percent,memory_mb,disk_write_mb_s,temperature_c,encoded_fps,bitrate_kbps,dropped_frames",
                    ]
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "media.hardware.probes",
                    "required": True,
                    "returncode": 0,
                    "command": "python3 vehicle-media-agent/vehicle_media_agent.py --mode hardware-probes",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["media.hardware.probes"],
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
                    "name": "media.hardware.probes",
                    "reason": "hardware_probe_plan_duplicate_scenarios",
                    "scenario": "four-camera-realtime-720p30",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_can_interface_and_source_structured_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = ["can.interface.show", "minepilot.can.sources"]
            records = []
            for command_name in command_names:
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text("", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 2,
                    "required_count": 2,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "can_interface_state",
                    "name": "can.interface.show",
                    "reason": "can_interface_state_missing_evidence",
                    "stdout_path": str(root / "can.interface.show.stdout.log"),
                },
                {
                    "event": "minepilot_can_sources",
                    "name": "minepilot.can.sources",
                    "reason": "minepilot_can_sources_missing_evidence",
                    "stdout_path": str(root / "minepilot.can.sources.stdout.log"),
                },
            ],
        )

    def test_target_host_validation_report_accepts_can_interface_and_source_structured_evidence(self):
        evidence = {
            "can.interface.show": {
                "event": "can_interface_state",
                "interface": "can0",
                "passed": True,
            },
            "minepilot.can.sources": {
                "event": "minepilot_can_sources",
                "files": [
                    "include/can/can_common.h",
                    "include/can/can_message.h",
                    "include/can_db.h",
                    "src/can_db.cpp",
                    "include/can_receiver.h",
                    "src/can_receiver.cpp",
                    "include/can_sender.h",
                    "src/can_sender.cpp",
                ],
                "passed": True,
                "root": "/Volumes/SystemDisk/Workspace/MinePilot",
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 2,
                    "required_count": 2,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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

        self.assertEqual(result.returncode, 0, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(report[0]["passed"])
        self.assertEqual(report[0]["evidence_failed"], [])

    def test_target_host_validation_report_rejects_duplicate_can_interface_evidence(self):
        source_files = [
            "include/can/can_common.h",
            "include/can/can_message.h",
            "include/can_db.h",
            "src/can_db.cpp",
            "include/can_receiver.h",
            "src/can_receiver.cpp",
            "include/can_sender.h",
            "src/can_sender.cpp",
        ]
        evidence = {
            "can.interface.show": [
                {
                    "event": "can_interface_state",
                    "interface": "can0",
                    "passed": True,
                },
                {
                    "event": "can_interface_state",
                    "interface": "can1",
                    "passed": True,
                },
            ],
            "minepilot.can.sources": [
                {
                    "event": "minepilot_can_sources",
                    "files": source_files,
                    "passed": True,
                    "root": "/Volumes/SystemDisk/Workspace/MinePilot",
                }
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_records in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(
                    "".join(json.dumps(stdout_record, sort_keys=True) + "\n" for stdout_record in stdout_records),
                    encoding="utf-8",
                )
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 2,
                    "required_count": 2,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "can_interface_state",
                    "name": "can.interface.show",
                    "reason": "can_interface_state_duplicate_evidence",
                    "stdout_path": str(root / "can.interface.show.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_minepilot_can_sources_evidence(self):
        source_files = [
            "include/can/can_common.h",
            "include/can/can_message.h",
            "include/can_db.h",
            "src/can_db.cpp",
            "include/can_receiver.h",
            "src/can_receiver.cpp",
            "include/can_sender.h",
            "src/can_sender.cpp",
        ]
        evidence = {
            "can.interface.show": [
                {
                    "event": "can_interface_state",
                    "interface": "can0",
                    "passed": True,
                }
            ],
            "minepilot.can.sources": [
                {
                    "event": "minepilot_can_sources",
                    "files": source_files,
                    "passed": True,
                    "root": "/Volumes/SystemDisk/Workspace/MinePilot",
                },
                {
                    "event": "minepilot_can_sources",
                    "files": source_files,
                    "passed": True,
                    "root": "/tmp/other-minepilot",
                },
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_records in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(
                    "".join(json.dumps(stdout_record, sort_keys=True) + "\n" for stdout_record in stdout_records),
                    encoding="utf-8",
                )
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 2,
                    "required_count": 2,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "minepilot_can_sources",
                    "name": "minepilot.can.sources",
                    "reason": "minepilot_can_sources_duplicate_evidence",
                    "stdout_path": str(root / "minepilot.can.sources.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_can_sources_root_mismatch(self):
        evidence = {
            "can.interface.show": {
                "event": "can_interface_state",
                "interface": "can0",
                "passed": True,
            },
            "minepilot.can.sources": {
                "event": "minepilot_can_sources",
                "files": [
                    "include/can/can_common.h",
                    "include/can/can_message.h",
                    "include/can_db.h",
                    "src/can_db.cpp",
                    "include/can_receiver.h",
                    "src/can_receiver.cpp",
                    "include/can_sender.h",
                    "src/can_sender.cpp",
                ],
                "passed": True,
                "root": "/tmp/other-minepilot",
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 2,
                    "required_count": 2,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "actual": "/tmp/other-minepilot",
                    "event": "minepilot_can_sources",
                    "expected": "/Volumes/SystemDisk/Workspace/MinePilot",
                    "name": "minepilot.can.sources",
                    "reason": "minepilot_can_sources_root_mismatch",
                    "stdout_path": str(root / "minepilot.can.sources.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_requires_minepilot_can_probe_structured_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = [
                "minepilot.can.socket.probe",
                "minepilot.can.sender.build",
                "minepilot.can.sender.smoke",
            ]
            records = []
            for command_name in command_names:
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text("", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "minepilot_can_socket_probe",
                    "name": "minepilot.can.socket.probe",
                    "reason": "minepilot_can_probe_missing_evidence",
                    "stdout_path": str(root / "minepilot.can.socket.probe.stdout.log"),
                },
                {
                    "event": "minepilot_can_sender_build",
                    "name": "minepilot.can.sender.build",
                    "reason": "minepilot_can_probe_missing_evidence",
                    "stdout_path": str(root / "minepilot.can.sender.build.stdout.log"),
                },
                {
                    "event": "minepilot_can_sender_smoke",
                    "name": "minepilot.can.sender.smoke",
                    "reason": "minepilot_can_probe_missing_evidence",
                    "stdout_path": str(root / "minepilot.can.sender.smoke.stdout.log"),
                },
            ],
        )

    def test_target_host_validation_report_accepts_minepilot_can_probe_structured_evidence(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_sender_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                "interface": "can0",
                "passed": True,
                "startup_banner_seen": True,
                "timeout_seconds": 3,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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

        self.assertEqual(result.returncode, 0, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(report[0]["passed"])
        self.assertEqual(report[0]["evidence_failed"], [])

    def test_target_host_validation_report_rejects_duplicate_minepilot_can_probe_evidence(self):
        evidence = {
            "minepilot.can.socket.probe": [
                {
                    "event": "minepilot_can_socket_probe",
                    "interface": "can0",
                    "passed": True,
                    "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
                }
            ],
            "minepilot.can.sender.build": [
                {
                    "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                    "event": "minepilot_can_sender_build",
                    "passed": True,
                    "target": "can_sender_main",
                }
            ],
            "minepilot.can.sender.smoke": [
                {
                    "accepted_exit_code": True,
                    "event": "minepilot_can_sender_smoke",
                    "exit_code": 124,
                    "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                    "interface": "can0",
                    "passed": True,
                    "startup_banner_seen": True,
                    "timeout_seconds": 3,
                },
                {
                    "accepted_exit_code": True,
                    "event": "minepilot_can_sender_smoke",
                    "exit_code": 124,
                    "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                    "interface": "can0",
                    "passed": True,
                    "timeout_seconds": 3,
                },
            ],
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_records in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(
                    "".join(json.dumps(stdout_record, sort_keys=True) + "\n" for stdout_record in stdout_records),
                    encoding="utf-8",
                )
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "minepilot_can_sender_smoke",
                    "name": "minepilot.can.sender.smoke",
                    "reason": "minepilot_can_probe_duplicate_evidence",
                    "stdout_path": str(root / "minepilot.can.sender.smoke.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_sender_smoke_missing_startup_banner(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_sender_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                "interface": "can0",
                "passed": True,
                "timeout_seconds": 3,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "event": "minepilot_can_sender_smoke",
                    "name": "minepilot.can.sender.smoke",
                    "reason": "minepilot_can_sender_smoke_startup_banner_missing",
                    "stdout_path": str(root / "minepilot.can.sender.smoke.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_socket_probe_script_mismatch(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/tmp/other-minepilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_sender_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                "interface": "can0",
                "passed": True,
                "startup_banner_seen": True,
                "timeout_seconds": 3,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "actual": "/tmp/other-minepilot/script/check_can.sh",
                    "event": "minepilot_can_socket_probe",
                    "expected": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
                    "name": "minepilot.can.socket.probe",
                    "reason": "minepilot_can_socket_probe_script_mismatch",
                    "stdout_path": str(root / "minepilot.can.socket.probe.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_sender_build_dir_mismatch(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/other-mine-teleop-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_sender_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                "interface": "can0",
                "passed": True,
                "startup_banner_seen": True,
                "timeout_seconds": 3,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "actual": "/tmp/other-mine-teleop-can-probe",
                    "event": "minepilot_can_sender_build",
                    "expected": "/tmp/mine-teleop-minepilot-can-probe",
                    "name": "minepilot.can.sender.build",
                    "reason": "minepilot_can_sender_build_dir_mismatch",
                    "stdout_path": str(root / "minepilot.can.sender.build.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_sender_smoke_interface_mismatch(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_sender_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                "interface": "can1",
                "passed": True,
                "startup_banner_seen": True,
                "timeout_seconds": 3,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "actual": "can1",
                    "event": "minepilot_can_sender_smoke",
                    "expected": "can0",
                    "name": "minepilot.can.sender.smoke",
                    "reason": "minepilot_can_probe_interface_mismatch",
                    "stdout_path": str(root / "minepilot.can.sender.smoke.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_sender_smoke_timeout_mismatch(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_sender_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                "interface": "can0",
                "passed": True,
                "startup_banner_seen": True,
                "timeout_seconds": 9,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "actual": "9",
                    "event": "minepilot_can_sender_smoke",
                    "expected": "3",
                    "name": "minepilot.can.sender.smoke",
                    "reason": "minepilot_can_sender_smoke_timeout_mismatch",
                    "stdout_path": str(root / "minepilot.can.sender.smoke.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_sender_smoke_executable_mismatch(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_sender_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": "/tmp/other-mine-teleop-can-probe/can_sender_main",
                "interface": "can0",
                "passed": True,
                "startup_banner_seen": True,
                "timeout_seconds": 3,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "actual": "/tmp/other-mine-teleop-can-probe/can_sender_main",
                    "event": "minepilot_can_sender_smoke",
                    "expected": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                    "name": "minepilot.can.sender.smoke",
                    "reason": "minepilot_can_sender_smoke_executable_mismatch",
                    "stdout_path": str(root / "minepilot.can.sender.smoke.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_rejects_minepilot_sender_build_target_mismatch(self):
        evidence = {
            "minepilot.can.socket.probe": {
                "event": "minepilot_can_socket_probe",
                "interface": "can0",
                "passed": True,
                "script": "/Volumes/SystemDisk/Workspace/MinePilot/script/check_can.sh",
            },
            "minepilot.can.sender.build": {
                "build_dir": "/tmp/mine-teleop-minepilot-can-probe",
                "event": "minepilot_can_sender_build",
                "passed": True,
                "target": "can_receiver_main",
            },
            "minepilot.can.sender.smoke": {
                "accepted_exit_code": True,
                "event": "minepilot_can_sender_smoke",
                "exit_code": 124,
                "executable": _MINEPILOT_CAN_SENDER_EXECUTABLE,
                "interface": "can0",
                "passed": True,
                "startup_banner_seen": True,
                "timeout_seconds": 3,
            },
        }
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            results_path = root / "target_host_validation_results.jsonl"
            command_names = list(evidence)
            records = []
            for command_name, stdout_record in evidence.items():
                stdout_path = root / f"{command_name}.stdout.log"
                stderr_path = root / f"{command_name}.stderr.log"
                stdout_path.write_text(json.dumps(stdout_record, sort_keys=True) + "\n", encoding="utf-8")
                stderr_path.write_text("", encoding="utf-8")
                records.append(
                    {
                        "event": "target_host_validation_result",
                        "name": command_name,
                        "required": True,
                        "returncode": 0,
                        "command": command_name,
                        "stdout_path": str(stdout_path),
                        "stderr_path": str(stderr_path),
                    }
                )
            records.append(
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 3,
                    "required_count": 3,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": command_names,
                    "passed": True,
                }
            )
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
                    "actual": "can_receiver_main",
                    "event": "minepilot_can_sender_build",
                    "expected": "can_sender_main",
                    "name": "minepilot.can.sender.build",
                    "reason": "minepilot_can_sender_build_target_mismatch",
                    "stdout_path": str(root / "minepilot.can.sender.build.stdout.log"),
                }
            ],
        )

    def test_target_host_validation_report_fails_incomplete_archive_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "gpu.vaapi.vainfo",
                    "required": True,
                    "returncode": 0,
                    "command": "vainfo",
                    "stdout_path": "/tmp/vainfo.stdout.log",
                    "stderr_path": "/tmp/vainfo.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 2,
                    "required_count": 2,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
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
            report[0]["consistency_failed"],
            [
                {
                    "actual": 1,
                    "field": "command_count",
                    "reason": "summary_mismatch",
                    "summary": 2,
                },
                {
                    "actual": 1,
                    "field": "required_count",
                    "reason": "summary_mismatch",
                    "summary": 2,
                },
                {
                    "actual": ["gpu.vaapi.vainfo"],
                    "field": "command_names",
                    "reason": "summary_missing",
                    "summary": None,
                },
            ],
        )

    def test_target_host_validation_report_rejects_boolean_summary_counts(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "custom.required.check",
                    "required": True,
                    "returncode": 0,
                    "command": "true",
                    "stdout_path": "/tmp/custom.required.stdout.log",
                    "stderr_path": "/tmp/custom.required.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": True,
                    "required_count": True,
                    "optional_count": False,
                    "required_failures": False,
                    "optional_failures": False,
                    "command_names": ["custom.required.check"],
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["consistency_failed"],
            [
                {"actual": 1, "field": "command_count", "reason": "summary_mismatch", "summary": True},
                {"actual": 1, "field": "required_count", "reason": "summary_mismatch", "summary": True},
                {"actual": 0, "field": "optional_count", "reason": "summary_mismatch", "summary": False},
                {"actual": 0, "field": "required_failures", "reason": "summary_mismatch", "summary": False},
                {"actual": 0, "field": "optional_failures", "reason": "summary_mismatch", "summary": False},
            ],
        )

    def test_target_host_validation_report_rejects_invalid_result_record_scalars(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "minepilot.can.sender.smoke",
                    "required": 1,
                    "returncode": "2",
                    "command": "can sender smoke",
                    "stdout_path": "/tmp/can.stdout.log",
                    "stderr_path": "/tmp/can.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 0,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["minepilot.can.sender.smoke"],
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["consistency_failed"],
            [
                {
                    "field": "results[0].required",
                    "name": "minepilot.can.sender.smoke",
                    "reason": "result_invalid",
                    "value": 1,
                },
                {
                    "field": "results[0].returncode",
                    "name": "minepilot.can.sender.smoke",
                    "reason": "result_invalid",
                    "value": "2",
                },
            ],
        )

    def test_target_host_validation_report_rejects_invalid_result_artifact_fields(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "custom.required.check",
                    "required": True,
                    "returncode": 0,
                    "command": "",
                    "stdout_path": "",
                    "stderr_path": None,
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["custom.required.check"],
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["consistency_failed"],
            [
                {
                    "field": "results[0].command",
                    "name": "custom.required.check",
                    "reason": "result_invalid",
                    "value": "",
                },
                {
                    "field": "results[0].stdout_path",
                    "name": "custom.required.check",
                    "reason": "result_invalid",
                    "value": "",
                },
                {
                    "field": "results[0].stderr_path",
                    "name": "custom.required.check",
                    "reason": "result_invalid",
                    "value": None,
                },
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_summary_records(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            first_summary = {
                "event": "target_host_validation_summary",
                **_TARGET_HOST_SUMMARY_METADATA,
                "command_count": 1,
                "required_count": 1,
                "optional_count": 0,
                "required_failures": 1,
                "optional_failures": 0,
                "command_names": ["custom.required.check"],
                "passed": False,
            }
            second_summary = dict(first_summary)
            second_summary["required_failures"] = 0
            second_summary["passed"] = True
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "custom.required.check",
                    "required": True,
                    "returncode": 0,
                    "command": "true",
                    "stdout_path": "/tmp/custom.stdout.log",
                    "stderr_path": "/tmp/custom.stderr.log",
                },
                first_summary,
                second_summary,
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["consistency_failed"],
            [{"actual": 2, "field": "summary", "reason": "summary_duplicate"}],
        )

    def test_target_host_validation_report_rejects_duplicate_result_names(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            first_stdout = root / "custom.required.check.first.stdout.log"
            first_stderr = root / "custom.required.check.first.stderr.log"
            second_stdout = root / "custom.required.check.second.stdout.log"
            second_stderr = root / "custom.required.check.second.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            first_stdout.write_text("", encoding="utf-8")
            first_stderr.write_text("", encoding="utf-8")
            second_stdout.write_text("", encoding="utf-8")
            second_stderr.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "custom.required.check",
                    "required": True,
                    "returncode": 0,
                    "command": "true",
                    "stdout_path": str(first_stdout),
                    "stderr_path": str(first_stderr),
                },
                {
                    "event": "target_host_validation_result",
                    "name": "custom.required.check",
                    "required": True,
                    "returncode": 0,
                    "command": "true",
                    "stdout_path": str(second_stdout),
                    "stderr_path": str(second_stderr),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 2,
                    "required_count": 2,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["custom.required.check", "custom.required.check"],
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["consistency_failed"],
            [
                {
                    "field": "results[1].name",
                    "name": "custom.required.check",
                    "reason": "result_duplicate",
                }
            ],
        )

    def test_target_host_validation_report_fails_when_summary_command_names_do_not_match_results(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "gpu.vaapi.vainfo",
                    "required": True,
                    "returncode": 0,
                    "command": "vainfo",
                    "stdout_path": "/tmp/vainfo.stdout.log",
                    "stderr_path": "/tmp/vainfo.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.preflight"],
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
            report[0]["consistency_failed"],
            [
                {
                    "actual": ["gpu.vaapi.vainfo"],
                    "field": "command_names",
                    "reason": "summary_mismatch",
                    "summary": ["vehicle.preflight"],
                }
            ],
        )

    def test_target_host_validation_report_fails_when_summary_command_names_are_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "gpu.vaapi.vainfo",
                    "required": True,
                    "returncode": 0,
                    "command": "vainfo",
                    "stdout_path": "/tmp/vainfo.stdout.log",
                    "stderr_path": "/tmp/vainfo.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
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
            report[0]["consistency_failed"],
            [
                {
                    "actual": ["gpu.vaapi.vainfo"],
                    "field": "command_names",
                    "reason": "summary_missing",
                    "summary": None,
                }
            ],
        )

    def test_target_host_validation_report_fails_when_summary_metadata_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "gpu.vaapi.vainfo",
                    "required": True,
                    "returncode": 0,
                    "command": "vainfo",
                    "stdout_path": "/tmp/vainfo.stdout.log",
                    "stderr_path": "/tmp/vainfo.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["gpu.vaapi.vainfo"],
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
            report[0]["consistency_failed"],
            [
                {"field": "acceptance_scenario", "reason": "summary_missing", "summary": None},
                {"field": "vehicle_config_path", "reason": "summary_missing", "summary": None},
                {"field": "can_interface", "reason": "summary_missing", "summary": None},
                {"field": "chassis_control_root", "reason": "summary_missing", "summary": None},
                {"field": "minepilot_root", "reason": "summary_missing", "summary": None},
                {"field": "chassis_control_branch", "reason": "summary_missing", "summary": None},
                {"field": "minepilot_branch", "reason": "summary_missing", "summary": None},
                {"field": "bridge_build_dir", "reason": "summary_missing", "summary": None},
                {"field": "uploader_work_dir", "reason": "summary_missing", "summary": None},
                {"field": "minepilot_can_probe_build_dir", "reason": "summary_missing", "summary": None},
                {"field": "can_probe_timeout_seconds", "reason": "summary_missing", "summary": None},
            ],
        )

    def test_target_host_validation_report_rejects_invalid_can_probe_summary_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            summary_metadata = dict(_TARGET_HOST_SUMMARY_METADATA)
            summary_metadata["minepilot_can_probe_build_dir"] = ""
            summary_metadata["can_probe_timeout_seconds"] = True
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "gpu.vaapi.vainfo",
                    "required": True,
                    "returncode": 0,
                    "command": "vainfo",
                    "stdout_path": "/tmp/vainfo.stdout.log",
                    "stderr_path": "/tmp/vainfo.stderr.log",
                },
                {
                    "event": "target_host_validation_summary",
                    **summary_metadata,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["gpu.vaapi.vainfo"],
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
            report[0]["consistency_failed"],
            [
                {"field": "minepilot_can_probe_build_dir", "reason": "summary_invalid", "summary": ""},
                {"field": "can_probe_timeout_seconds", "reason": "summary_invalid", "summary": True},
            ],
        )

    def test_target_host_validation_report_rejects_invalid_summary_passed_field(self):
        with tempfile.TemporaryDirectory() as tmp:
            results_path = Path(tmp) / "target_host_validation_results.jsonl"
            records = [
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 0,
                    "required_count": 0,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": [],
                    "passed": "true",
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["consistency_failed"],
            [{"field": "passed", "reason": "summary_invalid", "summary": "true"}],
        )

    def test_target_host_validation_report_cli_can_verify_archived_stdout_stderr_files(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "gpu.vaapi.vainfo.stdout.log"
            stderr_path = root / "gpu.vaapi.vainfo.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text("vainfo ok\n", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "gpu.vaapi.vainfo",
                    "required": True,
                    "returncode": 0,
                    "command": "vainfo",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["gpu.vaapi.vainfo"],
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
        self.assertEqual(len(report), 1, result.stderr)
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["missing_artifacts"],
            [
                {
                    "name": "gpu.vaapi.vainfo",
                    "path": str(stderr_path),
                    "stream": "stderr",
                }
            ],
        )

    def test_target_host_validation_report_requires_feedback_poll_received_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.feedback_poll.stdout.log"
            stderr_path = root / "vehicle.adapter.feedback_poll.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_feedback_poll",
                        "vehicle_id": "vehicle-001",
                        "attempted": True,
                        "received": False,
                        "reason": "no_feedback_frame",
                        "snapshot": None,
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.feedback_poll",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status --poll-feedback --require-feedback",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.feedback_poll"],
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
                    "name": "vehicle.adapter.feedback_poll",
                    "reason": "vehicle_adapter_feedback_poll_not_received",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_feedback_poll_stdout_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.feedback_poll.stdout.log"
            stderr_path = root / "vehicle.adapter.feedback_poll.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.feedback_poll",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status --poll-feedback --require-feedback",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.feedback_poll"],
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
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(report[0]["missing_artifacts"], [])
        self.assertEqual(
            report[0]["evidence_failed"],
            [
                {
                    "name": "vehicle.adapter.feedback_poll",
                    "reason": "vehicle_adapter_feedback_poll_not_received",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_feedback_poll_decoded_snapshot_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.feedback_poll.stdout.log"
            stderr_path = root / "vehicle.adapter.feedback_poll.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_feedback_poll",
                        "vehicle_id": "vehicle-001",
                        "attempted": True,
                        "received": True,
                        "reason": "feedback_frame_received",
                        "snapshot": {
                            "steering_angle": 0.0,
                            "speed_mps": 0.0,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.feedback_poll",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status --poll-feedback --require-feedback",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.feedback_poll"],
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
                    "missing": (
                        "ehb_mode,epb_status,eps_angle,eps_mode,gear_status,mcu_mode,"
                        "shake_hand_status,vehicle_speed,vehicle_speed_valid"
                    ),
                    "name": "vehicle.adapter.feedback_poll",
                    "reason": "vehicle_adapter_feedback_poll_snapshot_missing_fields",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_accepts_feedback_poll_received_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.feedback_poll.stdout.log"
            stderr_path = root / "vehicle.adapter.feedback_poll.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps(record, sort_keys=True)
                    for record in (
                        {
                            "event": "vehicle_adapter_status",
                            "vehicle_id": "vehicle-001",
                            "ready": True,
                            "status": {
                                "adapter_type": "dynamic_library",
                                "opened": True,
                                "healthy": True,
                                "can_interface": "can0",
                                "library_path": _BRIDGE_LIBRARY_PATH,
                            },
                        },
                        {
                            "event": "vehicle_adapter_feedback_poll",
                            "vehicle_id": "vehicle-001",
                            "attempted": True,
                            "received": True,
                            "reason": "feedback_received",
                            "snapshot": _FEEDBACK_SNAPSHOT,
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
                    "name": "vehicle.adapter.feedback_poll",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status --poll-feedback --require-feedback",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.feedback_poll"],
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

        self.assertEqual(result.returncode, 0, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(report[0]["passed"])
        self.assertEqual(report[0]["evidence_failed"], [])

    def test_target_host_validation_report_rejects_duplicate_feedback_poll_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.feedback_poll.stdout.log"
            stderr_path = root / "vehicle.adapter.feedback_poll.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps(record, sort_keys=True)
                    for record in (
                        {
                            "event": "vehicle_adapter_status",
                            "vehicle_id": "vehicle-001",
                            "ready": True,
                            "status": {
                                "adapter_type": "dynamic_library",
                                "opened": True,
                                "healthy": True,
                                "can_interface": "can0",
                                "library_path": _BRIDGE_LIBRARY_PATH,
                            },
                        },
                        {
                            "event": "vehicle_adapter_feedback_poll",
                            "vehicle_id": "vehicle-001",
                            "attempted": True,
                            "received": False,
                            "reason": "no_feedback_frame",
                            "snapshot": None,
                        },
                        {
                            "event": "vehicle_adapter_feedback_poll",
                            "vehicle_id": "vehicle-001",
                            "attempted": True,
                            "received": True,
                            "reason": "feedback_received",
                            "snapshot": _FEEDBACK_SNAPSHOT,
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
                    "name": "vehicle.adapter.feedback_poll",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status --poll-feedback --require-feedback",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.feedback_poll"],
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

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["evidence_failed"],
            [
                {
                    "actual": "2",
                    "name": "vehicle.adapter.feedback_poll",
                    "reason": "vehicle_adapter_feedback_poll_duplicate",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_adapter_status_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.feedback_poll.stdout.log"
            stderr_path = root / "vehicle.adapter.feedback_poll.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps(record, sort_keys=True)
                    for record in (
                        {
                            "event": "vehicle_adapter_status",
                            "vehicle_id": "vehicle-001",
                            "ready": True,
                            "status": {
                                "adapter_type": "dynamic_library",
                                "opened": True,
                                "healthy": True,
                                "can_interface": "can0",
                                "library_path": _BRIDGE_LIBRARY_PATH,
                            },
                        },
                        {
                            "event": "vehicle_adapter_status",
                            "vehicle_id": "vehicle-001",
                            "ready": True,
                            "status": {
                                "adapter_type": "mock",
                                "opened": True,
                                "healthy": True,
                            },
                        },
                        {
                            "event": "vehicle_adapter_feedback_poll",
                            "vehicle_id": "vehicle-001",
                            "attempted": True,
                            "received": True,
                            "reason": "feedback_received",
                            "snapshot": _FEEDBACK_SNAPSHOT,
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
                    "name": "vehicle.adapter.feedback_poll",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status --poll-feedback --require-feedback",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.feedback_poll"],
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

        self.assertEqual(result.returncode, 2, result.stdout)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(report[0]["passed"])
        self.assertEqual(
            report[0]["evidence_failed"],
            [
                {
                    "actual": "2",
                    "name": "vehicle.adapter.feedback_poll",
                    "reason": "vehicle_adapter_status_duplicate",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_feedback_poll_adapter_status_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.feedback_poll.stdout.log"
            stderr_path = root / "vehicle.adapter.feedback_poll.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_feedback_poll",
                        "vehicle_id": "vehicle-001",
                        "attempted": True,
                        "received": True,
                        "reason": "feedback_received",
                        "snapshot": _FEEDBACK_SNAPSHOT,
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.feedback_poll",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status --poll-feedback --require-feedback",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.feedback_poll"],
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
                    "name": "vehicle.adapter.feedback_poll",
                    "reason": "vehicle_adapter_status_missing",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_adapter_status_opened_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": False,
                        "status": {
                            "adapter_type": "dynamic_library",
                            "opened": False,
                            "healthy": True,
                            "can_interface": "can0",
                            "library_path": _BRIDGE_LIBRARY_PATH,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_not_opened",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_adapter_status_ready_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": False,
                        "status": {
                            "adapter_type": "dynamic_library",
                            "opened": True,
                            "healthy": False,
                            "can_interface": "can0",
                            "library_path": _BRIDGE_LIBRARY_PATH,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_not_healthy",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_adapter_status_bridge_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "dynamic_library",
                            "opened": True,
                            "healthy": True,
                            "can_interface": "can0",
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "missing": "library_path",
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_missing_bridge_metadata",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_empty_can_adapter_interface_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "can",
                            "opened": True,
                            "healthy": True,
                            "can_interface": "",
                            "library_path": _BRIDGE_LIBRARY_PATH,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "missing": "can_interface",
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_missing_bridge_metadata",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_can_adapter_without_bridge_library_metadata(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "can",
                            "opened": True,
                            "healthy": True,
                            "can_interface": "can0",
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "missing": "library_path",
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_missing_bridge_metadata",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_mock_adapter_status_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "mock",
                            "opened": True,
                            "healthy": True,
                            "applied_command_count": 0,
                            "safe_stop_count": 0,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_not_real_adapter",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_accepts_adapter_status_ready_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "dynamic_library",
                            "opened": True,
                            "healthy": True,
                            "can_interface": "can0",
                            "library_path": _BRIDGE_LIBRARY_PATH,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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

        self.assertEqual(result.returncode, 0, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(report[0]["passed"])
        self.assertEqual(report[0]["evidence_failed"], [])

    def test_target_host_validation_report_accepts_distinct_bridge_and_chassis_libraries(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "dynamic_library",
                            "opened": True,
                            "healthy": True,
                            "can_interface": "can0",
                            "library_path": _BRIDGE_LIBRARY_PATH,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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

        self.assertEqual(result.returncode, 0, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(report[0]["passed"])
        self.assertEqual(report[0]["evidence_failed"], [])

    def test_target_host_validation_report_rejects_adapter_status_library_path_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "dynamic_library",
                            "opened": True,
                            "healthy": True,
                            "can_interface": "can0",
                            "library_path": "/tmp/other/libmine_teleop_chassis_bridge.so",
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "actual": "/tmp/other/libmine_teleop_chassis_bridge.so",
                    "expected": _BRIDGE_LIBRARY_PATH,
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_library_path_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_adapter_status_can_interface_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.adapter.status.stdout.log"
            stderr_path = root / "vehicle.adapter.status.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "vehicle_adapter_status",
                        "vehicle_id": "vehicle-001",
                        "ready": True,
                        "status": {
                            "adapter_type": "dynamic_library",
                            "opened": True,
                            "healthy": True,
                            "can_interface": "can1",
                            "library_path": _BRIDGE_LIBRARY_PATH,
                        },
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.adapter.status",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle-agent --adapter-status",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.adapter.status"],
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
                    "actual": "can1",
                    "expected": "can0",
                    "name": "vehicle.adapter.status",
                    "reason": "vehicle_adapter_status_can_interface_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_uploader_process_once_json_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.uploader.process_once.stdout.log"
            stderr_path = root / "vehicle.uploader.process_once.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text("uploaded segment_id=seg-001\n", encoding="utf-8")
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.uploader.process_once",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle uploader process once",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.uploader.process_once"],
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
                    "event": "vehicle_uploader_process_once",
                    "name": "vehicle.uploader.process_once",
                    "reason": "vehicle_uploader_process_once_missing_evidence",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_failed_uploader_process_once(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.uploader.process_once.stdout.log"
            stderr_path = root / "vehicle.uploader.process_once.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "action": "failed",
                        "error": "source_file_missing",
                        "event": "vehicle_uploader_process_once",
                        "passed": False,
                        "segment_id": "seg-001",
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.uploader.process_once",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle uploader process once",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.uploader.process_once"],
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
                    "event": "vehicle_uploader_process_once",
                    "name": "vehicle.uploader.process_once",
                    "reason": "vehicle_uploader_process_once_failed",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_unknown_uploader_process_once_action(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.uploader.process_once.stdout.log"
            stderr_path = root / "vehicle.uploader.process_once.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "action": "pretended_success",
                        "event": "vehicle_uploader_process_once",
                        "passed": True,
                        "segment_id": "seg-001",
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.uploader.process_once",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle uploader process once",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.uploader.process_once"],
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
                    "event": "vehicle_uploader_process_once",
                    "name": "vehicle.uploader.process_once",
                    "reason": "vehicle_uploader_process_once_invalid_evidence",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_uploader_process_once_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "vehicle.uploader.process_once.stdout.log"
            stderr_path = root / "vehicle.uploader.process_once.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_records = [
                {
                    "action": "uploaded",
                    "event": "vehicle_uploader_process_once",
                    "passed": True,
                    "segment_id": "seg-001",
                },
                {
                    "action": "failed",
                    "error": "source_file_missing",
                    "event": "vehicle_uploader_process_once",
                    "passed": False,
                    "segment_id": "seg-001",
                },
            ]
            stdout_path.write_text(
                "".join(json.dumps(record, sort_keys=True) + "\n" for record in stdout_records),
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "vehicle.uploader.process_once",
                    "required": True,
                    "returncode": 0,
                    "command": "vehicle uploader process once",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "command_count": 1,
                    "required_count": 1,
                    "optional_count": 0,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["vehicle.uploader.process_once"],
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
                    "event": "vehicle_uploader_process_once",
                    "name": "vehicle.uploader.process_once",
                    "reason": "vehicle_uploader_process_once_duplicate_evidence",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_all_acceptance_metric_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "acceptance.metrics.report.stdout.log"
            stderr_path = root / "acceptance.metrics.report.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                json.dumps(
                    {
                        "event": "acceptance_metrics_report",
                        "scenario": "target-host-acceptance",
                    },
                    sort_keys=True,
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "acceptance.metrics.report",
                    "required": False,
                    "returncode": 0,
                    "command": "acceptance metrics report",
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
                    "command_names": ["acceptance.metrics.report"],
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
                    "name": "acceptance.metrics.report",
                    "reason": "acceptance_metrics_missing_reports",
                    "missing": "video_acceptance_metrics,control_acceptance_metrics,recording_acceptance_metrics,upload_acceptance_metrics",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_acceptance_metrics_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "acceptance.metrics.report.stdout.log"
            stderr_path = root / "acceptance.metrics.report.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps({"event": event, "scenario": "target-host-acceptance"}, sort_keys=True)
                    for event in (
                        "video_acceptance_metrics",
                        "control_acceptance_metrics",
                        "recording_acceptance_metrics",
                        "upload_acceptance_metrics",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "acceptance.metrics.report",
                    "required": False,
                    "returncode": 0,
                    "command": "acceptance metrics report",
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
                    "command_names": ["acceptance.metrics.report"],
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
                    "name": "acceptance.metrics.report",
                    "reason": "acceptance_metrics_missing_reports",
                    "missing": "acceptance_metrics_report",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_acceptance_metrics_scenario_match(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "acceptance.metrics.report.stdout.log"
            stderr_path = root / "acceptance.metrics.report.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps({"event": event, "scenario": "weak-100ms-turn-relay"}, sort_keys=True)
                    for event in (
                        "acceptance_metrics_report",
                        "video_acceptance_metrics",
                        "control_acceptance_metrics",
                        "recording_acceptance_metrics",
                        "upload_acceptance_metrics",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "acceptance.metrics.report",
                    "required": False,
                    "returncode": 0,
                    "command": "acceptance metrics report",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "acceptance_scenario": "target-host-acceptance",
                    "command_count": 1,
                    "required_count": 0,
                    "optional_count": 1,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["acceptance.metrics.report"],
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
                    "actual": "weak-100ms-turn-relay",
                    "event": "acceptance_metrics_report",
                    "expected": "target-host-acceptance",
                    "name": "acceptance.metrics.report",
                    "reason": "acceptance_metrics_scenario_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_acceptance_metric_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "acceptance.metrics.report.stdout.log"
            stderr_path = root / "acceptance.metrics.report.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps(record, sort_keys=True)
                    for record in (
                        {
                            "event": "acceptance_metrics_report",
                            "scenario": "target-host-acceptance",
                            "passed": True,
                        },
                        {
                            "event": "video_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                            "passed": True,
                        },
                        {
                            "event": "video_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                            "passed": True,
                        },
                        {
                            "event": "control_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                            "passed": True,
                        },
                        {
                            "event": "recording_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                            "passed": True,
                        },
                        {
                            "event": "upload_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                            "passed": True,
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
                    "name": "acceptance.metrics.report",
                    "required": False,
                    "returncode": 0,
                    "command": "acceptance metrics report",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "acceptance_scenario": "target-host-acceptance",
                    "command_count": 1,
                    "required_count": 0,
                    "optional_count": 1,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["acceptance.metrics.report"],
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
                    "event": "video_acceptance_metrics",
                    "name": "acceptance.metrics.report",
                    "reason": "acceptance_metrics_report_duplicate",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_acceptance_scenario_summary(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "acceptance.metrics.report.stdout.log"
            stderr_path = root / "acceptance.metrics.report.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps({"event": event, "scenario": "target-host-acceptance"}, sort_keys=True)
                    for event in (
                        "acceptance_metrics_report",
                        "video_acceptance_metrics",
                        "control_acceptance_metrics",
                        "recording_acceptance_metrics",
                        "upload_acceptance_metrics",
                    )
                )
                + "\n",
                encoding="utf-8",
            )
            stderr_path.write_text("", encoding="utf-8")
            records = [
                {
                    "event": "target_host_validation_result",
                    "name": "acceptance.metrics.report",
                    "required": False,
                    "returncode": 0,
                    "command": "acceptance metrics report",
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
                    "command_names": ["acceptance.metrics.report"],
                    "passed": True,
                },
            ]
            records[1].pop("acceptance_scenario", None)
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
        self.assertIn(
            {
                "field": "acceptance_scenario",
                "reason": "summary_missing",
                "summary": None,
            },
            report[0]["consistency_failed"],
        )

    def test_target_host_validation_report_rejects_failed_acceptance_metric_report(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "acceptance.metrics.report.stdout.log"
            stderr_path = root / "acceptance.metrics.report.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text(
                "\n".join(
                    json.dumps(record, sort_keys=True)
                    for record in (
                        {
                            "event": "acceptance_metrics_report",
                            "scenario": "target-host-acceptance",
                            "sample_count": 4,
                        },
                        {
                            "event": "video_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                            "passed": False,
                        },
                        {
                            "event": "control_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                        },
                        {
                            "event": "recording_acceptance_metrics",
                            "scenario": "target-host-acceptance",
                        },
                        {
                            "event": "upload_acceptance_metrics",
                            "scenario": "target-host-acceptance",
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
                    "name": "acceptance.metrics.report",
                    "required": False,
                    "returncode": 0,
                    "command": "acceptance metrics report",
                    "stdout_path": str(stdout_path),
                    "stderr_path": str(stderr_path),
                },
                {
                    "event": "target_host_validation_summary",
                    **_TARGET_HOST_SUMMARY_METADATA,
                    "acceptance_scenario": "target-host-acceptance",
                    "command_count": 1,
                    "required_count": 0,
                    "optional_count": 1,
                    "required_failures": 0,
                    "optional_failures": 0,
                    "command_names": ["acceptance.metrics.report"],
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
                    "event": "video_acceptance_metrics",
                    "name": "acceptance.metrics.report",
                    "reason": "acceptance_metrics_report_failed",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_requires_hardware_encoding_summary_lane_and_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "media.hardware.report.template.stdout.log"
            stderr_path = root / "media.hardware.report.template.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            stdout_path.write_text("hardware report command completed\n", encoding="utf-8")
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
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_missing_reports",
                    "missing": "hardware_encoding_validation,hardware_encoding_lane,hardware_encoding_metrics",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_accepts_hardware_encoding_summary_lane_and_metrics(self):
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
                            "lane_count": 4,
                            "failures": [],
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
                            "event": "hardware_encoding_lane",
                            "scenario": "four-camera-realtime-720p30",
                            "lane_id": "rear-realtime-720p30",
                            "codec_name": "h264",
                            "width": 1280,
                            "height": 720,
                            "fps": 30.0,
                            "bitrate_kbps": 3000,
                            "passed": True,
                            "failures": [],
                        },
                        {
                            "event": "hardware_encoding_lane",
                            "scenario": "four-camera-realtime-720p30",
                            "lane_id": "left-realtime-720p30",
                            "codec_name": "h264",
                            "width": 1280,
                            "height": 720,
                            "fps": 30.0,
                            "bitrate_kbps": 3000,
                            "passed": True,
                            "failures": [],
                        },
                        {
                            "event": "hardware_encoding_lane",
                            "scenario": "four-camera-realtime-720p30",
                            "lane_id": "right-realtime-720p30",
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

        self.assertEqual(result.returncode, 0, result.stderr)
        report = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertTrue(report[0]["passed"])
        self.assertEqual(report[0]["evidence_failed"], [])

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

    def test_target_host_validation_report_rejects_duplicate_hardware_encoding_lane_ids(self):
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
                            "lane_count": 2,
                            "failures": [],
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
                    "lane_id": "front-realtime-720p30",
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_lane_duplicate",
                    "scenario": "four-camera-realtime-720p30",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_duplicate_hardware_encoding_metrics(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            stdout_path = root / "media.hardware.report.template.stdout.log"
            stderr_path = root / "media.hardware.report.template.stderr.log"
            results_path = root / "target_host_validation_results.jsonl"
            metrics = {
                "bitrate_kbps": 3000,
                "cpu_percent": 42.5,
                "disk_write_mb_s": 24.0,
                "dropped_frames": 0,
                "encoded_fps": 30.0,
                "gpu_percent": 71.0,
                "memory_mb": 1536.0,
                "temperature_c": 62.0,
            }
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
                            "metrics": metrics,
                        },
                        {
                            "event": "hardware_encoding_metrics",
                            "scenario": "four-camera-realtime-720p30",
                            "metrics": metrics,
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
                    "reason": "hardware_encoding_metrics_duplicate",
                    "scenario": "four-camera-realtime-720p30",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_hardware_encoding_metrics_scenario_mismatch(self):
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
                            "scenario": "stale-hardware-run",
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
                    "actual": "stale-hardware-run",
                    "expected": "four-camera-realtime-720p30",
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_metrics_scenario_mismatch",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_hardware_encoding_metrics_missing_fields(self):
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
                            "metrics": {"gpu_percent": 71.0, "dropped_frames": 0},
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
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_metrics_missing_fields",
                    "scenario": "four-camera-realtime-720p30",
                    "missing": "cpu_percent,memory_mb,disk_write_mb_s,temperature_c,encoded_fps,bitrate_kbps",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_hardware_encoding_lane_count_mismatch(self):
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
                            "lane_count": 4,
                            "failures": [],
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
                            "metrics": {"gpu_percent": 71.0, "dropped_frames": 0},
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
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_lane_count_mismatch",
                    "scenario": "four-camera-realtime-720p30",
                    "expected": "4",
                    "actual": "1",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_failed_hardware_encoding_summary(self):
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
                            "passed": False,
                            "lane_count": 4,
                            "failures": ["front-realtime-720p30: fps 10.00 below expected 30"],
                        },
                        {
                            "event": "hardware_encoding_lane",
                            "scenario": "four-camera-realtime-720p30",
                            "lane_id": "front-realtime-720p30",
                            "codec_name": "h264",
                            "width": 1280,
                            "height": 720,
                            "fps": 10.0,
                            "bitrate_kbps": 3000,
                            "passed": False,
                            "failures": ["front-realtime-720p30: fps 10.00 below expected 30"],
                        },
                        {
                            "event": "hardware_encoding_metrics",
                            "scenario": "four-camera-realtime-720p30",
                            "metrics": {"gpu_percent": 71.0, "dropped_frames": 120},
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
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_validation_failed",
                    "scenario": "four-camera-realtime-720p30",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_target_host_validation_report_rejects_failed_hardware_encoding_lane(self):
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
                            "event": "hardware_encoding_lane",
                            "scenario": "four-camera-realtime-720p30",
                            "lane_id": "front-realtime-720p30",
                            "codec_name": "h264",
                            "width": 1280,
                            "height": 720,
                            "fps": 10.0,
                            "bitrate_kbps": 3000,
                            "passed": False,
                            "failures": ["front-realtime-720p30: fps 10.00 below expected 30"],
                        },
                        {
                            "event": "hardware_encoding_metrics",
                            "scenario": "four-camera-realtime-720p30",
                            "metrics": {"gpu_percent": 71.0, "dropped_frames": 120},
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
                    "name": "media.hardware.report.template",
                    "reason": "hardware_encoding_lane_failed",
                    "lane_id": "front-realtime-720p30",
                    "stdout_path": str(stdout_path),
                }
            ],
        )

    def test_chassis_bridge_check_cli_reports_ready_fake_roots_and_cmake_configure(self):
        if shutil.which("cmake") is None:
            self.skipTest("cmake is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            _write_fake_chassis_control_symbol_tools(bin_dir)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "include" / "can" / "can_common.h").write_text("// CAN common header\n", encoding="utf-8")
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            (minepilot_root / "include" / "can").mkdir()
            (minepilot_root / "include" / "can" / "can_common.h").write_text("// MinePilot CAN common\n", encoding="utf-8")
            (minepilot_root / "include" / "can" / "can_message.h").write_text("// MinePilot CAN message\n", encoding="utf-8")
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")
            build_dir = root / "build"
            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(build_dir),
                ],
                cwd=Path.cwd(),
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(records[0]["event"], "chassis_bridge_check")
        self.assertTrue(records[0]["ready"])
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["chassis_control.header"]["status"], "ready")
        self.assertEqual(checks["chassis_control.can_common_header"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_common_header"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_message_header"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_db_header"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_receiver_header"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_sender_header"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_db_source"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_receiver_source"]["status"], "ready")
        self.assertEqual(checks["minepilot.can_sender_source"]["status"], "ready")
        self.assertEqual(checks["cmake.configure"]["status"], "ready")

    def test_chassis_bridge_check_cli_pins_auto_selected_library_for_cmake_configure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            _write_fake_chassis_control_symbol_tools(bin_dir)
            fake_cmake = bin_dir / "cmake"
            cmake_log = root / "cmake.log"
            fake_cmake.write_text(
                """#!/usr/bin/env python3
import os
import pathlib
import sys

log = pathlib.Path(os.environ["FAKE_CMAKE_LOG"])
existing = log.read_text(encoding="utf-8") if log.exists() else ""
log.write_text(existing + " ".join(sys.argv[1:]) + "\\n", encoding="utf-8")
build_dir = pathlib.Path(sys.argv[sys.argv.index("-B") + 1])
build_dir.mkdir(parents=True, exist_ok=True)
(build_dir / "CMakeCache.txt").write_text("configured\\n", encoding="utf-8")
sys.exit(0)
""",
                encoding="utf-8",
            )
            fake_cmake.chmod(0o755)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "include" / "can" / "can_common.h").write_text("// CAN common header\n", encoding="utf-8")
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            selected_library = minepilot_root / "libchassis_control.dylib"
            selected_library.write_text("", encoding="utf-8")
            build_dir = root / "build"
            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
            env["FAKE_CMAKE_LOG"] = str(cmake_log)

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(build_dir),
                ],
                cwd=Path.cwd(),
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            cmake_log_text = cmake_log.read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["chassis_control.library"]["path"], str(selected_library))
        self.assertIn(f"-DCHASSIS_CONTROL_LIBRARY={selected_library}", cmake_log_text)

    def test_chassis_bridge_check_cli_fails_when_selected_library_missing_required_symbol(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            _write_fake_chassis_control_symbol_tools(bin_dir, missing_symbols=("EmergencyStopWheels()",))
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "include" / "can" / "can_common.h").write_text("// CAN common header\n", encoding="utf-8")
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            selected_library = minepilot_root / "libchassis_control.dylib"
            selected_library.write_text("", encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--skip-cmake",
                ],
                cwd=Path.cwd(),
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["chassis_control.symbols"]["path"], str(selected_library))
        self.assertEqual(checks["chassis_control.symbols"]["status"], "missing_symbol")
        self.assertIn("EmergencyStopWheels()", checks["chassis_control.symbols"]["message"])

    def test_chassis_bridge_check_cli_runs_optional_cmake_build_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            _write_fake_chassis_control_symbol_tools(bin_dir)
            fake_cmake = bin_dir / "cmake"
            cmake_log = root / "cmake.log"
            fake_cmake.write_text(
                """#!/usr/bin/env python3
import os
import pathlib
import sys

pathlib.Path(os.environ["FAKE_CMAKE_LOG"]).write_text(
    pathlib.Path(os.environ["FAKE_CMAKE_LOG"]).read_text(encoding="utf-8") + " ".join(sys.argv[1:]) + "\\n"
    if pathlib.Path(os.environ["FAKE_CMAKE_LOG"]).exists()
    else " ".join(sys.argv[1:]) + "\\n",
    encoding="utf-8",
)
if "--build" not in sys.argv:
    build_dir = pathlib.Path(sys.argv[sys.argv.index("-B") + 1])
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / "CMakeCache.txt").write_text("configured\\n", encoding="utf-8")
sys.exit(0)
""",
                encoding="utf-8",
            )
            fake_cmake.chmod(0o755)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "include" / "can" / "can_common.h").write_text("// CAN common header\n", encoding="utf-8")
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")
            build_dir = root / "build"
            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
            env["FAKE_CMAKE_LOG"] = str(cmake_log)

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(build_dir),
                    "--build",
                ],
                cwd=Path.cwd(),
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            cmake_log_text = cmake_log.read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["cmake.configure"]["status"], "ready")
        self.assertEqual(checks["cmake.build"]["status"], "ready")
        self.assertIn("--build", cmake_log_text)
        self.assertIn("--target mine_teleop_chassis_bridge", cmake_log_text)

    def test_chassis_bridge_check_cli_fails_when_build_is_requested_but_cmake_is_skipped(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "include" / "can" / "can_common.h").write_text("// CAN common header\n", encoding="utf-8")
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--skip-cmake",
                    "--build",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stdout)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["cmake.configure"]["status"], "skipped")
        self.assertEqual(checks["cmake.build"]["status"], "failed")
        self.assertIn("build was requested", checks["cmake.build"]["message"])

    def test_chassis_bridge_check_cli_reports_meaningful_cmake_build_failure(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            _write_fake_chassis_control_symbol_tools(bin_dir)
            fake_cmake = bin_dir / "cmake"
            fake_cmake.write_text(
                """#!/usr/bin/env python3
import pathlib
import sys

if "--build" not in sys.argv:
    build_dir = pathlib.Path(sys.argv[sys.argv.index("-B") + 1])
    build_dir.mkdir(parents=True, exist_ok=True)
    (build_dir / "CMakeCache.txt").write_text("configured\\n", encoding="utf-8")
    sys.exit(0)
print('can_common.h:16:6: error: "Unsupported platform"')
print("make: *** [mine_teleop_chassis_bridge] Error 2")
sys.exit(1)
""",
                encoding="utf-8",
            )
            fake_cmake.chmod(0o755)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "include" / "can" / "can_common.h").write_text("// CAN common header\n", encoding="utf-8")
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--build",
                ],
                cwd=Path.cwd(),
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["cmake.build"]["status"], "failed")
        self.assertIn("Unsupported platform", checks["cmake.build"]["message"])
        self.assertIn("target Ubuntu or Docker linux/amd64", checks["cmake.build"]["message"])

    def test_chassis_bridge_check_cli_can_print_linux_docker_build_command(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            repo_root = root / "mine-teleop"
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            repo_root.mkdir()
            chassis_root.mkdir()
            minepilot_root.mkdir()
            selected_library = minepilot_root / "libchassis_control.so"

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--chassis-control-library",
                    str(selected_library),
                    "--build-dir",
                    "build/chassis-control-bridge",
                    "--docker-command",
                    "--host-repo-root",
                    str(repo_root),
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(records), 1)
        record = records[0]
        self.assertEqual(record["event"], "chassis_bridge_docker_command")
        self.assertEqual(record["platform"], "linux/amd64")
        self.assertIn("docker build --platform linux/amd64 -t minepilot-build-env", record["build_image_command"])
        self.assertIn(f"-v {repo_root}:/workspace/mine-teleop", record["run_command"])
        self.assertIn(f"-v {chassis_root}:/workspace/ChassisControl", record["run_command"])
        self.assertIn(f"-v {minepilot_root}:/workspace/MinePilot", record["run_command"])
        self.assertIn("-DCHASSIS_CONTROL_ROOT=/workspace/ChassisControl", record["run_command"])
        self.assertIn("-DMINEPILOT_ROOT=/workspace/MinePilot", record["run_command"])
        self.assertIn("-DCHASSIS_CONTROL_LIBRARY=/workspace/MinePilot/libchassis_control.so", record["run_command"])
        self.assertIn("cmake --build /workspace/mine-teleop/build/chassis-control-bridge", record["run_command"])
        self.assertIn("--target mine_teleop_chassis_bridge", record["run_command"])

    def test_chassis_bridge_check_cli_fails_when_minepilot_sender_header_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--skip-cmake",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(records[0]["ready"])
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["minepilot.can_sender_header"]["status"], "missing")

    def test_chassis_bridge_check_cli_fails_when_chassis_can_common_header_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--skip-cmake",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(records[0]["ready"])
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["chassis_control.can_common_header"]["status"], "missing")

    def test_chassis_bridge_check_cli_fails_when_minepilot_can_sender_source_is_missing(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--skip-cmake",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        self.assertFalse(records[0]["ready"])
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["minepilot.can_sender_source"]["status"], "missing")

    def test_chassis_bridge_check_cli_reports_wrong_external_branches(self):
        if shutil.which("git") is None:
            self.skipTest("git is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            minepilot_root.mkdir()
            subprocess.run(["git", "init", "-b", "old_chassis"], cwd=chassis_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run(["git", "init", "-b", "old_minepilot"], cwd=minepilot_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--skip-cmake",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 2, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["chassis_control.branch"]["status"], "mismatch")
        self.assertIn("expected UI_Test", checks["chassis_control.branch"]["message"])
        self.assertEqual(checks["minepilot.branch"]["status"], "mismatch")
        self.assertIn("expected merge_ui_test", checks["minepilot.branch"]["message"])

    def test_chassis_bridge_check_cli_reports_external_checkout_commit_and_dirty_state(self):
        if shutil.which("git") is None:
            self.skipTest("git is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            _write_fake_chassis_control_symbol_tools(bin_dir)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            minepilot_root.mkdir()
            subprocess.run(["git", "init", "-b", "UI_Test"], cwd=chassis_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            subprocess.run(["git", "init", "-b", "merge_ui_test"], cwd=minepilot_root, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            (chassis_root / "chassis_control.h").write_text("// ChassisControl API header\n", encoding="utf-8")
            (chassis_root / "include" / "can").mkdir(parents=True)
            (chassis_root / "include" / "can" / "can_common.h").write_text("// CAN common header\n", encoding="utf-8")
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")
            for repo in (chassis_root, minepilot_root):
                subprocess.run(["git", "add", "."], cwd=repo, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                subprocess.run(
                    [
                        "git",
                        "-c",
                        "user.name=Mine Teleop Test",
                        "-c",
                        "user.email=mine-teleop@example.invalid",
                        "commit",
                        "-m",
                        "fixture",
                    ],
                    cwd=repo,
                    check=True,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE,
                )
            (minepilot_root / "include" / "can_sender.h").write_text("// sender dirty\n", encoding="utf-8")
            env = os.environ.copy()
            env["PATH"] = str(bin_dir) + os.pathsep + env.get("PATH", "")
            chassis_commit = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=chassis_root,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            ).stdout.strip()
            minepilot_commit = subprocess.run(
                ["git", "rev-parse", "HEAD"],
                cwd=minepilot_root,
                check=True,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            ).stdout.strip()

            result = subprocess.run(
                [
                    sys.executable,
                    "scripts/chassis_bridge_check.py",
                    "--chassis-control-root",
                    str(chassis_root),
                    "--minepilot-root",
                    str(minepilot_root),
                    "--build-dir",
                    str(root / "build"),
                    "--skip-cmake",
                ],
                cwd=Path.cwd(),
                env=env,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertEqual(result.returncode, 0, result.stderr)
        records = [json.loads(line) for line in result.stdout.splitlines()]
        checks = {record["name"]: record for record in records[1:]}
        self.assertEqual(checks["chassis_control.commit"]["status"], "ready")
        self.assertIn(chassis_commit, checks["chassis_control.commit"]["message"])
        self.assertEqual(checks["minepilot.commit"]["status"], "ready")
        self.assertIn(minepilot_commit, checks["minepilot.commit"]["message"])
        self.assertEqual(checks["chassis_control.dirty"]["status"], "ready")
        self.assertIn("dirty=false", checks["chassis_control.dirty"]["message"])
        self.assertEqual(checks["minepilot.dirty"]["status"], "ready")
        self.assertIn("dirty=true", checks["minepilot.dirty"]["message"])

    def test_chassis_control_bridge_template_exports_stable_c_shim_symbols(self):
        bridge = Path("deployments/chassis-control-bridge/chassis_control_bridge.cpp").read_text(encoding="utf-8")
        header = Path("deployments/chassis-control-bridge/mine_teleop_chassis_bridge.h").read_text(encoding="utf-8")
        cmake = Path("deployments/chassis-control-bridge/CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn('#include "mine_teleop_chassis_bridge.h"', bridge)
        self.assertIn("#ifndef MINE_TELEOP_CHASSIS_BRIDGE_H", header)
        self.assertIn("struct MineTeleopChassisTelemetry", header)
        self.assertIn("struct MineTeleopChassisFeedback", header)
        self.assertIn('extern "C"', bridge)
        for symbol in [
            "mine_teleop_chassis_open",
            "mine_teleop_chassis_apply_state",
            "mine_teleop_chassis_emergency_stop",
            "mine_teleop_chassis_update_feedback",
            "mine_teleop_chassis_poll_feedback",
            "mine_teleop_chassis_read_telemetry",
            "mine_teleop_chassis_close",
        ]:
            self.assertIn(symbol, bridge)
            self.assertIn(symbol, header)
        self.assertIn('#include "can_receiver.h"', bridge)
        self.assertIn("can_receive(g_can_handle, &rx_frame, 0)", bridge)
        self.assertIn("ChassisControl", cmake)
        self.assertIn("MinePilot", cmake)
        self.assertIn("chassis_control", cmake)
        self.assertIn("MINEPILOT_CAN_COMMON_HEADER", cmake)
        self.assertIn("MINEPILOT_CAN_MESSAGE_HEADER", cmake)
        self.assertIn("MINEPILOT_CAN_DB_SOURCE", cmake)
        self.assertIn("MINEPILOT_CAN_RECEIVER_SOURCE", cmake)
        self.assertIn("MINEPILOT_CAN_SENDER_SOURCE", cmake)
        self.assertIn("mine_teleop_chassis_bridge.h", cmake)
        self.assertIn("${MINEPILOT_CAN_RECEIVER_SOURCE}", cmake)
        self.assertIn("${MINEPILOT_CAN_DB_SOURCE}", cmake)
        self.assertIn("can_sender.h", cmake)

    def test_chassis_control_configuration_docs_list_stable_c_shim_symbols(self):
        header = Path("deployments/chassis-control-bridge/mine_teleop_chassis_bridge.h").read_text(encoding="utf-8")
        docs = Path("docs/08-configuration.md").read_text(encoding="utf-8")

        for symbol in [
            "mine_teleop_chassis_open",
            "mine_teleop_chassis_apply_state",
            "mine_teleop_chassis_emergency_stop",
            "mine_teleop_chassis_update_feedback",
            "mine_teleop_chassis_poll_feedback",
            "mine_teleop_chassis_read_telemetry",
            "mine_teleop_chassis_close",
        ]:
            with self.subTest(symbol=symbol):
                self.assertIn(f"{symbol}(", header)
                self.assertIn(f"`{symbol}`", docs)

    def test_chassis_control_bridge_cmake_finds_minepilot_root_library(self):
        if shutil.which("cmake") is None:
            self.skipTest("cmake is not installed")
        cache = ""
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_sender.cpp").write_text("// sender source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")
            build_dir = root / "build"

            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    "deployments/chassis-control-bridge",
                    "-B",
                    str(build_dir),
                    f"-DCHASSIS_CONTROL_ROOT={chassis_root}",
                    f"-DMINEPILOT_ROOT={minepilot_root}",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            if result.returncode == 0:
                cache = (build_dir / "CMakeCache.txt").read_text(encoding="utf-8")

        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"CHASSIS_CONTROL_API_ROOT:PATH={minepilot_root}", cache)

    def test_chassis_control_bridge_cmake_requires_minepilot_can_sources(self):
        if shutil.which("cmake") is None:
            self.skipTest("cmake is not installed")
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            chassis_root = root / "ChassisControl"
            minepilot_root = root / "MinePilot"
            chassis_root.mkdir()
            minepilot_root.mkdir()
            (minepilot_root / "chassis_control.h").write_text("// MinePilot ABI header\n", encoding="utf-8")
            (minepilot_root / "include").mkdir()
            _write_minepilot_low_level_can_headers(minepilot_root)
            (minepilot_root / "include" / "can_db.h").write_text("// db\n", encoding="utf-8")
            (minepilot_root / "include" / "can_receiver.h").write_text("// receiver\n", encoding="utf-8")
            (minepilot_root / "include" / "can_sender.h").write_text("// sender\n", encoding="utf-8")
            (minepilot_root / "src").mkdir()
            (minepilot_root / "src" / "can_db.cpp").write_text("// db source\n", encoding="utf-8")
            (minepilot_root / "src" / "can_receiver.cpp").write_text("// receiver source\n", encoding="utf-8")
            (minepilot_root / "libchassis_control.dylib").write_text("", encoding="utf-8")

            result = subprocess.run(
                [
                    "cmake",
                    "-S",
                    "deployments/chassis-control-bridge",
                    "-B",
                    str(root / "build"),
                    f"-DCHASSIS_CONTROL_ROOT={chassis_root}",
                    f"-DMINEPILOT_ROOT={minepilot_root}",
                ],
                cwd=Path.cwd(),
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MinePilot CAN integration source not found", result.stdout + result.stderr)
        self.assertIn("can_sender.cpp", result.stdout + result.stderr)

    def test_vehicle_container_template_pins_media_stack_and_mounts_devices_recordings_and_limits(self):
        dockerfile = Path("deployments/container/Dockerfile.media").read_text(encoding="utf-8")
        compose = yaml.safe_load(Path("deployments/container/docker-compose.vehicle.yml").read_text(encoding="utf-8"))

        self.assertIn("FROM ubuntu:22.04", dockerfile)
        self.assertIn("ffmpeg", dockerfile)
        self.assertIn("vainfo", dockerfile)
        self.assertIn("intel-media-va-driver", dockerfile)
        self.assertIn("gstreamer1.0-tools", dockerfile)
        self.assertIn("/var/lib/mine-teleop/recordings", dockerfile)
        self.assertIn("/var/lib/mine-teleop/uploader", dockerfile)
        self.assertIn("chown -R mineteleop:mineteleop /var/lib/mine-teleop", dockerfile)
        self.assertIn("USER mineteleop", dockerfile)

        media = compose["services"]["vehicle-media-agent"]
        devices = set(media["devices"])
        self.assertIn("/dev/dri/renderD128:/dev/dri/renderD128", devices)
        self.assertIn("/dev/dri/card1:/dev/dri/card1", devices)
        self.assertIn("/dev/video0:/dev/video0", devices)
        self.assertIn("/dev/video1:/dev/video1", devices)
        self.assertIn("mine-teleop-recordings:/var/lib/mine-teleop/recordings", media["volumes"])
        self.assertEqual(media["cpus"], "2.0")
        self.assertEqual(media["mem_limit"], "2g")
        self.assertIn("vehicle-media-agent/vehicle_media_agent.py", " ".join(media["command"]))

        agent = compose["services"]["vehicle-agent"]
        uploader = compose["services"]["vehicle-uploader"]
        self.assertIn("mine-teleop-recordings:/var/lib/mine-teleop/recordings", uploader["volumes"])
        self.assertEqual(uploader["cpus"], "1.0")
        self.assertEqual(uploader["mem_limit"], "1g")
        self.assertLess(uploader["cpu_shares"], agent["cpu_shares"])
        self.assertLess(uploader["cpu_shares"], media["cpu_shares"])
        self.assertIn("--service-mode", uploader["command"])

    def test_signaling_tls_docs_include_required_identity_credentials(self):
        docs = Path("docs/12-operations-and-troubleshooting.md").read_text(encoding="utf-8")
        section = docs.split("### Signaling TLS", 1)[1].split("### Identity Credentials", 1)[0]

        self.assertIn("--tls-cert /etc/mine-teleop/tls/signaling.crt", section)
        self.assertIn("--tls-key /etc/mine-teleop/tls/signaling.key", section)
        self.assertIn("--driver-credentials /etc/mine-teleop/driver-credentials.json", section)
        self.assertIn("--device-credentials /etc/mine-teleop/device-credentials.json", section)

    def test_vehicle_container_template_uses_deployed_vehicle_config(self):
        compose = yaml.safe_load(Path("deployments/container/docker-compose.vehicle.yml").read_text(encoding="utf-8"))

        for service_name in ("vehicle-agent", "vehicle-media-agent", "vehicle-uploader"):
            with self.subTest(service=service_name):
                service = compose["services"][service_name]
                command = service["command"]
                self.assertIn("--config", command)
                self.assertIn("/etc/mine-teleop/vehicle-agent.yaml", command)
                self.assertNotIn("configs/vehicle-agent.dev.yaml", command)
                self.assertIn("/etc/mine-teleop:/etc/mine-teleop:ro", service["volumes"])

    def test_turn_container_template_exposes_udp_turn_and_isolated_resources(self):
        compose = yaml.safe_load(Path("deployments/container/docker-compose.vehicle.yml").read_text(encoding="utf-8"))

        turn = compose["services"]["turn-server"]
        self.assertEqual(turn["image"], "coturn/coturn:4.6.2")
        self.assertIn("3478:3478/udp", turn["ports"])
        self.assertIn("5349:5349/udp", turn["ports"])
        self.assertIn("49152-49200:49152-49200/udp", turn["ports"])
        self.assertIn("../turnserver/turnserver.conf.template:/etc/mine-teleop/turnserver.conf:ro", turn["volumes"])
        self.assertIn("../tls:/etc/mine-teleop/tls:ro", turn["volumes"])
        self.assertEqual(turn["cpus"], "1.0")
        self.assertEqual(turn["mem_limit"], "512m")
        self.assertEqual(turn["restart"], "on-failure")

def _write_minepilot_low_level_can_headers(minepilot_root: Path) -> None:
    can_dir = minepilot_root / "include" / "can"
    can_dir.mkdir()
    (can_dir / "can_common.h").write_text("// MinePilot CAN common\n", encoding="utf-8")
    (can_dir / "can_message.h").write_text("// MinePilot CAN message\n", encoding="utf-8")


def _write_fake_chassis_control_symbol_tools(bin_dir: Path, *, missing_symbols: tuple[str, ...] = ()) -> None:
    symbols = [
        "Initialize(std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char> > const&)",
        "UpdateVehicleState(VCU_feedback_t const&)",
        "RunArmingStateMachine()",
        "ResetArmingStateMachine()",
        "ResetDisarmSequence()",
        "SendCanMessage(unsigned int, unsigned char const*, unsigned char)",
        "EmergencyStopWheels()",
    ]
    exported = [symbol for symbol in symbols if symbol not in missing_symbols]
    nm = bin_dir / "nm"
    nm.write_text(
        "#!/usr/bin/env python3\n"
        "import sys\n"
        "symbols = " + repr(exported) + "\n"
        "for index, symbol in enumerate(symbols, start=1):\n"
        "    print(f'{index:016x} T {symbol}')\n",
        encoding="utf-8",
    )
    nm.chmod(0o755)
    cxxfilt = bin_dir / "c++filt"
    cxxfilt.write_text(
        "#!/usr/bin/env python3\n"
        "import sys\n"
        "sys.stdout.write(sys.stdin.read())\n",
        encoding="utf-8",
    )
    cxxfilt.chmod(0o755)


if __name__ == "__main__":
    unittest.main()
