from __future__ import annotations

import json
import re
import shlex
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Any, Iterable, Mapping

from mine_teleop.chassis_bridge_check import (
    DEFAULT_CHASSIS_CONTROL_BRANCH,
    DEFAULT_CHASSIS_CONTROL_ROOT,
    DEFAULT_MINEPILOT_BRANCH,
    DEFAULT_MINEPILOT_ROOT,
)
from mine_teleop.netem import WeakNetworkBaseline


_FEEDBACK_POLL_COMMAND_NAME = "vehicle.adapter.feedback_poll"
_ADAPTER_STATUS_COMMAND_NAME = "vehicle.adapter.status"
_UPLOADER_PROCESS_ONCE_COMMAND_NAME = "vehicle.uploader.process_once"
_UPLOADER_PROCESS_ONCE_EVENT = "vehicle_uploader_process_once"
_UPLOADER_PROCESS_ONCE_ALLOWED_ACTIONS = (
    "credential_refreshed",
    "disabled",
    "idle",
    "rate_limited",
    "uploaded",
    "wait",
)
_ACCEPTANCE_METRICS_COMMAND_NAME = "acceptance.metrics.report"
_HARDWARE_REPORT_COMMAND_NAME = "media.hardware.report.template"
_HARDWARE_PROBES_COMMAND_NAME = "media.hardware.probes"
_WEAK_NETWORK_MATRIX_COMMAND_NAME = "network.weak.matrix"
_CHASSIS_BRIDGE_COMMAND_NAME = "chassis.bridge.check"
_PREFLIGHT_COMMAND_NAME = "vehicle.preflight"
_PREFLIGHT_EVENT = "vehicle_preflight"
_GPU_PCI_COMMAND_NAME = "gpu.pci.summary"
_GPU_DRI_COMMAND_NAME = "gpu.dri.nodes"
_GPU_VAAPI_COMMAND_NAME = "gpu.vaapi.vainfo"
_GPU_COMMAND_EVENTS = {
    _GPU_PCI_COMMAND_NAME: "gpu_pci_summary",
    _GPU_DRI_COMMAND_NAME: "gpu_dri_nodes",
    _GPU_VAAPI_COMMAND_NAME: "gpu_vaapi_vainfo",
}
_CAN_INTERFACE_COMMAND_NAME = "can.interface.show"
_MINEPILOT_CAN_SOURCES_COMMAND_NAME = "minepilot.can.sources"
_CAN_INTERFACE_EVENT = "can_interface_state"
_MINEPILOT_CAN_SOURCES_EVENT = "minepilot_can_sources"
_MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME = "minepilot.can.socket.probe"
_MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME = "minepilot.can.sender.build"
_MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME = "minepilot.can.sender.smoke"
_MINEPILOT_CAN_COMMAND_EVENTS = {
    _MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME: "minepilot_can_socket_probe",
    _MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME: "minepilot_can_sender_build",
    _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME: "minepilot_can_sender_smoke",
}
_MINEPILOT_CAN_SENDER_STARTUP_BANNER = "\u542f\u52a8CAN\u53d1\u9001\u7ebf\u7a0b"
_BRIDGE_ADAPTER_STATUS_METADATA_FIELDS = ("can_interface", "library_path")
_REQUIRED_CHASSIS_BRIDGE_READY_CHECKS = (
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
_CHASSIS_BRIDGE_COMMIT_CHECKS = ("chassis_control.commit", "minepilot.commit")
_CHASSIS_BRIDGE_DIRTY_CHECKS = ("chassis_control.dirty", "minepilot.dirty")
_CHASSIS_BRIDGE_BUILD_DIR_CHECKS = ("cmake.configure", "cmake.build")
_CHASSIS_BRIDGE_BRANCH_SUMMARY_FIELDS = {
    "chassis_control.branch": "chassis_control_branch",
    "minepilot.branch": "minepilot_branch",
}
_CHASSIS_BRIDGE_ROOT_SUMMARY_FIELDS = {
    "chassis_control.root": "chassis_control_root",
    "minepilot.root": "minepilot_root",
}
_CHASSIS_BRIDGE_BRANCH_METADATA_RE = re.compile(r"\bexpected branch (?P<branch>\S+)\b")
_CHASSIS_BRIDGE_COMMIT_METADATA_RE = re.compile(r"\bcommit=(?P<commit>[0-9a-fA-F]{40})\b")
_CHASSIS_BRIDGE_DIRTY_METADATA_RE = re.compile(
    r"\bdirty=(?P<dirty>true|false)\b.*\bchanged_paths=(?P<changed_paths>\d+)\b"
)
_REQUIRED_MINEPILOT_CAN_SOURCE_FILES = (
    "include/can/can_common.h",
    "include/can/can_message.h",
    "include/can_db.h",
    "src/can_db.cpp",
    "include/can_receiver.h",
    "src/can_receiver.cpp",
    "include/can_sender.h",
    "src/can_sender.cpp",
)
_REQUIRED_FEEDBACK_SNAPSHOT_FIELDS = (
    "ehb_mode",
    "epb_status",
    "eps_angle",
    "eps_mode",
    "gear_status",
    "mcu_mode",
    "shake_hand_status",
    "vehicle_speed",
    "vehicle_speed_valid",
)
_REQUIRED_ACCEPTANCE_METRIC_EVENTS = (
    "acceptance_metrics_report",
    "video_acceptance_metrics",
    "control_acceptance_metrics",
    "recording_acceptance_metrics",
    "upload_acceptance_metrics",
)
_REQUIRED_HARDWARE_REPORT_EVENTS = (
    "hardware_encoding_validation",
    "hardware_encoding_lane",
    "hardware_encoding_metrics",
)
_REQUIRED_HARDWARE_PROBE_SCENARIOS = (
    "four-camera-realtime-720p30",
    "four-camera-recording-source",
    "four-camera-realtime-plus-recording",
)
_REQUIRED_WEAK_NETWORK_PROFILE_NAMES = tuple(
    profile.name for profile in WeakNetworkBaseline.default().profiles()
)
_REQUIRED_HARDWARE_METRIC_FIELDS = (
    "cpu_percent",
    "gpu_percent",
    "memory_mb",
    "disk_write_mb_s",
    "temperature_c",
    "encoded_fps",
    "bitrate_kbps",
    "dropped_frames",
)
_REQUIRED_SUMMARY_STRING_FIELDS = (
    "acceptance_scenario",
    "vehicle_config_path",
    "can_interface",
    "chassis_control_root",
    "minepilot_root",
    "chassis_control_branch",
    "minepilot_branch",
    "bridge_build_dir",
    "uploader_work_dir",
    "minepilot_can_probe_build_dir",
)
_REQUIRED_SUMMARY_POSITIVE_INT_FIELDS = (
    "can_probe_timeout_seconds",
)
_REQUIRED_SUMMARY_BOOL_FIELDS = (
    "passed",
)
_TARGET_HOST_COMMAND_REQUIREMENTS = {
    _GPU_PCI_COMMAND_NAME: True,
    _GPU_DRI_COMMAND_NAME: True,
    _GPU_VAAPI_COMMAND_NAME: True,
    _HARDWARE_PROBES_COMMAND_NAME: True,
    _PREFLIGHT_COMMAND_NAME: True,
    _CAN_INTERFACE_COMMAND_NAME: True,
    _MINEPILOT_CAN_SOURCES_COMMAND_NAME: True,
    _MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME: True,
    _MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME: True,
    _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME: True,
    _CHASSIS_BRIDGE_COMMAND_NAME: True,
    _ADAPTER_STATUS_COMMAND_NAME: True,
    _FEEDBACK_POLL_COMMAND_NAME: True,
    _UPLOADER_PROCESS_ONCE_COMMAND_NAME: True,
    _WEAK_NETWORK_MATRIX_COMMAND_NAME: False,
    _HARDWARE_REPORT_COMMAND_NAME: False,
    _ACCEPTANCE_METRICS_COMMAND_NAME: False,
}


@dataclass(frozen=True)
class ValidationCommand:
    name: str
    command: str
    required: bool
    purpose: str


@dataclass(frozen=True)
class TargetHostValidationArchive:
    results: tuple[dict[str, Any], ...]
    summary: dict[str, Any] | None = None
    summary_count: int = 0

    @classmethod
    def from_jsonl(cls, path: str | Path) -> "TargetHostValidationArchive":
        results: list[dict[str, Any]] = []
        summary: dict[str, Any] | None = None
        summary_count = 0
        with Path(path).open("r", encoding="utf-8") as handle:
            for line_number, line in enumerate(handle, start=1):
                if not line.strip():
                    continue
                record = json.loads(line)
                if not isinstance(record, dict):
                    raise ValueError(f"line {line_number} must contain a JSON object")
                event = record.get("event")
                if event == "target_host_validation_result":
                    results.append(record)
                elif event == "target_host_validation_summary":
                    summary_count += 1
                    summary = record
        return cls(results=tuple(results), summary=summary, summary_count=summary_count)

    @property
    def required_failed(self) -> tuple[str, ...]:
        return tuple(
            str(record.get("name", ""))
            for record in self.results
            if record.get("required") is True and _validation_result_failed(record)
        )

    @property
    def optional_failed(self) -> tuple[str, ...]:
        return tuple(
            str(record.get("name", ""))
            for record in self.results
            if record.get("required") is False and _validation_result_failed(record)
        )

    @property
    def missing_artifacts(self) -> tuple[dict[str, str], ...]:
        missing: list[dict[str, str]] = []
        for record in self.results:
            name = str(record.get("name", ""))
            for field, stream in (("stdout_path", "stdout"), ("stderr_path", "stderr")):
                path = record.get(field)
                if not isinstance(path, str) or not path or not Path(path).exists():
                    missing.append({"name": name, "path": str(path or ""), "stream": stream})
        return tuple(missing)

    @property
    def evidence_failed(self) -> tuple[dict[str, str], ...]:
        failed: list[dict[str, str]] = []
        for record in self.results:
            failure = _gpu_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _preflight_evidence_failure(record)
            if failure is not None:
                failed.append(failure)
            failure = _hardware_probe_plan_evidence_failure(record)
            if failure is not None:
                failed.append(failure)
            failure = _chassis_bridge_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _can_interface_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _minepilot_can_sources_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _minepilot_can_probe_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _adapter_status_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _feedback_poll_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _uploader_process_once_evidence_failure(record)
            if failure is not None:
                failed.append(failure)
            failure = _acceptance_metrics_evidence_failure(record, self.summary)
            if failure is not None:
                failed.append(failure)
            failure = _hardware_report_evidence_failure(record)
            if failure is not None:
                failed.append(failure)
            failure = _weak_network_matrix_evidence_failure(record)
            if failure is not None:
                failed.append(failure)
        return tuple(failed)

    @property
    def consistency_failed(self) -> tuple[dict[str, Any], ...]:
        if self.summary is None:
            return ({"field": "summary", "reason": "summary_missing"},)
        actual_counts = {
            "command_count": len(self.results),
            "required_count": sum(1 for record in self.results if record.get("required") is True),
            "optional_count": sum(1 for record in self.results if record.get("required") is False),
            "required_failures": len(self.required_failed),
            "optional_failures": len(self.optional_failed),
        }
        failed: list[dict[str, Any]] = []
        if self.summary_count > 1:
            failed.append({"actual": self.summary_count, "field": "summary", "reason": "summary_duplicate"})
        seen_result_names: set[str] = set()
        for index, record in enumerate(self.results):
            name = record.get("name")
            record_name = name if isinstance(name, str) else ""
            if not isinstance(name, str) or name == "":
                failed.append(
                    {
                        "field": f"results[{index}].name",
                        "name": record_name,
                        "reason": "result_invalid",
                        "value": name,
                    }
                )
            elif name in seen_result_names:
                failed.append(
                    {
                        "field": f"results[{index}].name",
                        "name": name,
                        "reason": "result_duplicate",
                    }
                )
            else:
                seen_result_names.add(name)
            for field in ("command", "stdout_path", "stderr_path"):
                value = record.get(field)
                if not isinstance(value, str) or value == "":
                    failed.append(
                        {
                            "field": f"results[{index}].{field}",
                            "name": record_name,
                            "reason": "result_invalid",
                            "value": value,
                        }
                    )
            required = record.get("required")
            if not isinstance(required, bool):
                failed.append(
                    {
                        "field": f"results[{index}].required",
                        "name": record_name,
                        "reason": "result_invalid",
                        "value": required,
                    }
                )
            else:
                expected_required = _TARGET_HOST_COMMAND_REQUIREMENTS.get(record_name)
                if expected_required is not None and required is not expected_required:
                    failed.append(
                        {
                            "actual": required,
                            "expected": expected_required,
                            "field": f"results[{index}].required",
                            "name": record_name,
                            "reason": "result_required_mismatch",
                        }
                    )
            returncode = record.get("returncode")
            if isinstance(returncode, bool) or not isinstance(returncode, int):
                failed.append(
                    {
                        "field": f"results[{index}].returncode",
                        "name": record_name,
                        "reason": "result_invalid",
                        "value": returncode,
                    }
                )
        for field, actual in actual_counts.items():
            summary_value = self.summary.get(field)
            if isinstance(summary_value, bool) or not isinstance(summary_value, int) or summary_value != actual:
                failed.append(
                    {
                        "actual": actual,
                        "field": field,
                        "reason": "summary_mismatch",
                        "summary": summary_value,
                    }
                )
        actual_names = [str(record.get("name", "")) for record in self.results]
        summary_names = self.summary.get("command_names")
        if summary_names is None:
            failed.append(
                {
                    "actual": actual_names,
                    "field": "command_names",
                    "reason": "summary_missing",
                    "summary": None,
                }
            )
        elif (
            not isinstance(summary_names, list)
            or not all(isinstance(name, str) for name in summary_names)
            or summary_names != actual_names
        ):
            failed.append(
                {
                    "actual": actual_names,
                    "field": "command_names",
                    "reason": "summary_mismatch",
                    "summary": summary_names,
                }
            )
        command_requirements = self.summary.get("command_requirements")
        if command_requirements is not None:
            if not isinstance(command_requirements, dict):
                failed.append(
                    {
                        "field": "command_requirements",
                        "reason": "summary_invalid",
                        "summary": command_requirements,
                    }
                )
            else:
                actual_requirements = {
                    record["name"]: record["required"]
                    for record in self.results
                    if isinstance(record.get("name"), str)
                    and record.get("name") != ""
                    and isinstance(record.get("required"), bool)
                }
                missing_command_names: list[str] = []
                for name, expected_required in command_requirements.items():
                    if not isinstance(name, str) or name == "":
                        failed.append(
                            {
                                "field": "command_requirements",
                                "reason": "summary_invalid",
                                "summary": command_requirements,
                            }
                        )
                        continue
                    if not isinstance(expected_required, bool):
                        failed.append(
                            {
                                "field": f"command_requirements.{name}",
                                "reason": "summary_invalid",
                                "summary": expected_required,
                            }
                        )
                        continue
                    actual_required = actual_requirements.get(name)
                    if actual_required is None:
                        missing_command_names.append(name)
                    elif actual_required is not expected_required:
                        failed.append(
                            {
                                "actual": actual_required,
                                "expected": expected_required,
                                "field": f"command_requirements.{name}",
                                "name": name,
                                "reason": "summary_command_required_mismatch",
                            }
                        )
                if missing_command_names:
                    failed.append(
                        {
                            "field": "command_requirements",
                            "missing": ",".join(missing_command_names),
                            "reason": "summary_command_missing",
                        }
                    )
                missing_requirement_names = [
                    name for name in actual_requirements if name not in command_requirements
                ]
                if missing_requirement_names:
                    failed.append(
                        {
                            "field": "command_requirements",
                            "missing": ",".join(missing_requirement_names),
                            "reason": "summary_command_requirement_missing",
                        }
                    )
        for field in _REQUIRED_SUMMARY_STRING_FIELDS:
            summary_value = self.summary.get(field)
            if summary_value is None:
                failed.append({"field": field, "reason": "summary_missing", "summary": None})
            elif not isinstance(summary_value, str) or summary_value == "":
                failed.append({"field": field, "reason": "summary_invalid", "summary": summary_value})
        for field in _REQUIRED_SUMMARY_POSITIVE_INT_FIELDS:
            summary_value = self.summary.get(field)
            if summary_value is None:
                failed.append({"field": field, "reason": "summary_missing", "summary": None})
            elif isinstance(summary_value, bool) or not isinstance(summary_value, int) or summary_value <= 0:
                failed.append({"field": field, "reason": "summary_invalid", "summary": summary_value})
        for field in _REQUIRED_SUMMARY_BOOL_FIELDS:
            summary_value = self.summary.get(field)
            if summary_value is None:
                failed.append({"field": field, "reason": "summary_missing", "summary": None})
            elif not isinstance(summary_value, bool):
                failed.append({"field": field, "reason": "summary_invalid", "summary": summary_value})
        return tuple(failed)

    @property
    def passed(self) -> bool:
        summary_passed = self.summary.get("passed") if self.summary is not None else None
        if isinstance(summary_passed, bool):
            return (
                summary_passed
                and not self.required_failed
                and not self.evidence_failed
                and not self.consistency_failed
            )
        return not self.required_failed and not self.evidence_failed and not self.consistency_failed

    def passed_for_report(self, *, verify_artifacts: bool = False) -> bool:
        return self.passed and (not verify_artifacts or not self.missing_artifacts)

    def to_jsonl(self, *, verify_artifacts: bool = False) -> tuple[str, ...]:
        record = {
            "event": "target_host_validation_archive",
            "passed": self.passed_for_report(verify_artifacts=verify_artifacts),
            "result_count": len(self.results),
            "required_failed": list(self.required_failed),
            "optional_failed": list(self.optional_failed),
            "missing_artifacts": list(self.missing_artifacts) if verify_artifacts else [],
            "evidence_failed": list(self.evidence_failed),
            "consistency_failed": list(self.consistency_failed),
            "summary": dict(self.summary or {}),
        }
        return (json.dumps(record, ensure_ascii=False, sort_keys=True),)


@dataclass(frozen=True)
class TargetHostValidationPlan:
    commands: tuple[ValidationCommand, ...]
    context: Mapping[str, Any] | None = None

    @classmethod
    def default(
        cls,
        *,
        vehicle_config_path: str = "/etc/mine-teleop/vehicle-agent.yaml",
        hardware_devices: Iterable[str] = ("/dev/dri/renderD128", "/dev/dri/card1"),
        can_interface: str = "can0",
        network_interface: str = "wwan0",
        chassis_control_root: str = str(DEFAULT_CHASSIS_CONTROL_ROOT),
        minepilot_root: str = str(DEFAULT_MINEPILOT_ROOT),
        chassis_control_branch: str = DEFAULT_CHASSIS_CONTROL_BRANCH,
        minepilot_branch: str = DEFAULT_MINEPILOT_BRANCH,
        bridge_library_path: str = "/opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so",
        chassis_control_library: str = f"{DEFAULT_MINEPILOT_ROOT}/libchassis_control.so",
        bridge_build_dir: str = "build/chassis-control-bridge",
        uploader_work_dir: str = "/var/lib/mine-teleop/uploader",
        minepilot_can_probe_build_dir: str = "/tmp/mine-teleop-minepilot-can-probe",
        can_probe_timeout_seconds: int = 3,
        acceptance_samples_path: str = "/tmp/mine-teleop-acceptance-samples.jsonl",
        acceptance_scenario: str = "target-host-acceptance",
        mine_teleop_binary: str | None = None,
    ) -> "TargetHostValidationPlan":
        devices = tuple(hardware_devices)
        vaapi_render_device = _select_vaapi_render_device(devices)
        hardware_args = " ".join(f"--hardware-device {_quote(device)}" for device in devices)
        minepilot_can_probe_build_dir = str(minepilot_can_probe_build_dir)
        can_probe_timeout_seconds = int(can_probe_timeout_seconds)
        if can_probe_timeout_seconds <= 0:
            raise ValueError("can_probe_timeout_seconds must be positive")
        vehicle_agent_command = _entrypoint_command(
            mine_teleop_binary,
            "vehicle-agent",
            "python3 vehicle-agent/vehicle_agent.py",
        )
        vehicle_media_agent_command = _entrypoint_command(
            mine_teleop_binary,
            "vehicle-media-agent",
            "python3 vehicle-media-agent/vehicle_media_agent.py",
        )
        vehicle_uploader_command = _entrypoint_command(
            mine_teleop_binary,
            "vehicle-uploader",
            "python3 vehicle-uploader/vehicle_uploader.py",
        )
        chassis_bridge_check_command = _entrypoint_command(
            mine_teleop_binary,
            "chassis-bridge-check",
            "python3 scripts/chassis_bridge_check.py",
        )
        netem_plan_command = _entrypoint_command(
            mine_teleop_binary,
            "netem-plan",
            "python3 scripts/netem_plan.py",
        )
        acceptance_metrics_command = _entrypoint_command(
            mine_teleop_binary,
            "acceptance-metrics-report",
            "python3 scripts/acceptance_metrics_report.py",
        )
        chassis_bridge_check_mode_arg = "--skip-cmake" if mine_teleop_binary else "--build"
        minepilot_can_source_test = "test " + " -a ".join(
            f"-f {_quote(f'{minepilot_root}/{source_file}')}"
            for source_file in _REQUIRED_MINEPILOT_CAN_SOURCE_FILES
        )
        minepilot_can_sender_executable = f"{minepilot_can_probe_build_dir}/can_sender_main"
        return cls(
            commands=(
                ValidationCommand(
                    name=_GPU_PCI_COMMAND_NAME,
                    command=(
                        "lspci -nnk | grep -EA4 'VGA|3D|Display|NVIDIA|Intel|AMD' && "
                        + _jsonl_echo_command(
                            {
                                "event": _GPU_COMMAND_EVENTS[_GPU_PCI_COMMAND_NAME],
                                "passed": True,
                            }
                        )
                    ),
                    required=True,
                    purpose="Record visible GPU devices and kernel drivers.",
                ),
                ValidationCommand(
                    name=_GPU_DRI_COMMAND_NAME,
                    command=(
                        "ls -l /dev/dri && "
                        + _jsonl_echo_command(
                            {
                                "event": _GPU_COMMAND_EVENTS[_GPU_DRI_COMMAND_NAME],
                                "passed": True,
                                "path": "/dev/dri",
                            }
                        )
                    ),
                    required=True,
                    purpose="Record DRM render/card device nodes.",
                ),
                ValidationCommand(
                    name=_GPU_VAAPI_COMMAND_NAME,
                    command=(
                        f"vainfo --display drm --device {_quote(vaapi_render_device)} && "
                        + _jsonl_echo_command(
                            {
                                "device": vaapi_render_device,
                                "event": _GPU_COMMAND_EVENTS[_GPU_VAAPI_COMMAND_NAME],
                                "passed": True,
                            }
                        )
                    ),
                    required=True,
                    purpose="Verify VAAPI opens the target Intel render node.",
                ),
                ValidationCommand(
                    name=_HARDWARE_PROBES_COMMAND_NAME,
                    command=(
                        f"{vehicle_media_agent_command} --config {_quote(vehicle_config_path)} "
                        "--mode hardware-probes"
                    ),
                    required=True,
                    purpose="Print four-camera VAAPI and GStreamer hardware probe commands.",
                ),
                ValidationCommand(
                    name="vehicle.preflight",
                    command=(
                        f"{vehicle_agent_command} --config {_quote(vehicle_config_path)} "
                        f"--preflight {hardware_args}"
                    ).strip(),
                    required=True,
                    purpose="Check vehicle config, camera paths, recording root, and hardware devices.",
                ),
                ValidationCommand(
                    name=_CAN_INTERFACE_COMMAND_NAME,
                    command=(
                        f"ip -details link show {_quote(can_interface)} && "
                        + _jsonl_echo_command(
                            {
                                "event": _CAN_INTERFACE_EVENT,
                                "interface": can_interface,
                                "passed": True,
                            }
                        )
                    ),
                    required=True,
                    purpose="Record target CAN interface state before opening the chassis bridge.",
                ),
                ValidationCommand(
                    name=_MINEPILOT_CAN_SOURCES_COMMAND_NAME,
                    command=(
                        minepilot_can_source_test
                        + " && "
                        + _jsonl_echo_command(
                            {
                                "event": _MINEPILOT_CAN_SOURCES_EVENT,
                                "files": list(_REQUIRED_MINEPILOT_CAN_SOURCE_FILES),
                                "passed": True,
                                "root": minepilot_root,
                            }
                        )
                    ),
                    required=True,
                    purpose="Verify MinePilot CAN DB, receiver, and sender source files are present.",
                ),
                ValidationCommand(
                    name=_MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME,
                    command=(
                        f"bash {_quote(f'{minepilot_root}/script/check_can.sh')} {_quote(can_interface)} && "
                        + _jsonl_echo_command(
                            {
                                "event": _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME],
                                "interface": can_interface,
                                "passed": True,
                                "script": f"{minepilot_root}/script/check_can.sh",
                            }
                        )
                    ),
                    required=True,
                    purpose="Run MinePilot's CAN interface and raw socket probe on the target interface.",
                ),
                ValidationCommand(
                    name=_MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME,
                    command=(
                        f"cmake -S {_quote(minepilot_root)} -B {_quote(minepilot_can_probe_build_dir)} "
                        "-DBUILD_TESTING=ON && "
                        f"cmake --build {_quote(minepilot_can_probe_build_dir)} "
                        "--target can_sender_main -j$(nproc) && "
                        + _jsonl_echo_command(
                            {
                                "build_dir": minepilot_can_probe_build_dir,
                                "event": _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME],
                                "passed": True,
                                "target": "can_sender_main",
                            }
                        )
                    ),
                    required=True,
                    purpose="Build MinePilot can_sender_main with can_sender, can_receiver, and can_db.",
                ),
                ValidationCommand(
                    name=_MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME,
                    command=(
                        "bash -lc "
                        + _quote(
                            f"probe_output=$(timeout {can_probe_timeout_seconds}s "
                            f"{minepilot_can_sender_executable} {can_interface} 2>&1); "
                            'status=$?; '
                            'printf "%s\\n" "$probe_output"; '
                            'if [ "$status" -eq 0 ] || [ "$status" -eq 124 ]; then '
                            'MINE_TELEOP_CAN_SMOKE_STATUS="$status" '
                            'MINE_TELEOP_CAN_SMOKE_OUTPUT="$probe_output" python3 -c '
                            f"{_quote(_minepilot_can_sender_smoke_json_python(can_interface, can_probe_timeout_seconds, minepilot_can_sender_executable))}; "
                            "exit 0; fi; "
                            'exit "$status"'
                        )
                    ),
                    required=True,
                    purpose=(
                        "Open the target CAN interface and send MinePilot CAN frames briefly; "
                        "timeout 124 is accepted after the probe starts."
                    ),
                ),
                ValidationCommand(
                    name="chassis.bridge.check",
                    command=(
                        f"{chassis_bridge_check_command} "
                        f"--chassis-control-root {_quote(chassis_control_root)} "
                        f"--minepilot-root {_quote(minepilot_root)} "
                        f"--chassis-control-branch {_quote(chassis_control_branch)} "
                        f"--minepilot-branch {_quote(minepilot_branch)} "
                        f"--chassis-control-library {_quote(chassis_control_library)} "
                        f"--build-dir {_quote(bridge_build_dir)} "
                        f"{chassis_bridge_check_mode_arg}"
                    ),
                    required=True,
                    purpose="Validate ChassisControl/MinePilot headers, library selection, CMake configure, and bridge build.",
                ),
                ValidationCommand(
                    name="vehicle.adapter.status",
                    command=(
                        f"{vehicle_agent_command} --config {_quote(vehicle_config_path)} --adapter-status"
                    ),
                    required=True,
                    purpose="Open the configured VehicleAdapter and archive opened/healthy bridge status.",
                ),
                ValidationCommand(
                    name="vehicle.adapter.feedback_poll",
                    command=(
                        f"{vehicle_agent_command} --config {_quote(vehicle_config_path)} "
                        "--adapter-status --poll-feedback --require-feedback"
                    ),
                    required=True,
                    purpose="Require one decoded CAN feedback frame through the configured VehicleAdapter bridge.",
                ),
                ValidationCommand(
                    name=_UPLOADER_PROCESS_ONCE_COMMAND_NAME,
                    command=(
                        f"{vehicle_uploader_command} --service-mode --process-once "
                        f"--config {_quote(vehicle_config_path)} "
                        f"--work-dir {_quote(uploader_work_dir)} "
                        "--json"
                    ),
                    required=True,
                    purpose="Run one configured uploader service iteration and archive structured queue/upload status.",
                ),
                ValidationCommand(
                    name=_WEAK_NETWORK_MATRIX_COMMAND_NAME,
                    command=f"{netem_plan_command} --interface {_quote(network_interface)} --matrix",
                    required=False,
                    purpose="Print the documented weak-network tc matrix without applying it.",
                ),
                ValidationCommand(
                    name="media.hardware.report.template",
                    command=(
                        f"{vehicle_media_agent_command} --config {_quote(vehicle_config_path)} --mode hardware-report "
                        "--scenario four-camera-realtime-720p30 "
                        "--ffprobe-output front-realtime-720p30=/tmp/front.ffprobe.txt "
                        "--ffprobe-output rear-realtime-720p30=/tmp/rear.ffprobe.txt "
                        "--ffprobe-output left-realtime-720p30=/tmp/left.ffprobe.txt "
                        "--ffprobe-output right-realtime-720p30=/tmp/right.ffprobe.txt "
                        "--metrics-json /tmp/mine-teleop-vaapi-metrics.json"
                    ),
                    required=False,
                    purpose="Convert saved target-host ffprobe and metrics samples into JSONL acceptance evidence.",
                ),
                ValidationCommand(
                    name="acceptance.metrics.report",
                    command=(
                        f"{acceptance_metrics_command} "
                        f"--samples {_quote(acceptance_samples_path)} "
                        f"--scenario {_quote(acceptance_scenario)}"
                    ),
                    required=False,
                    purpose="Convert saved target-host acceptance samples into JSONL metrics reports.",
                ),
            ),
            context={
                "acceptance_samples_path": acceptance_samples_path,
                "acceptance_scenario": acceptance_scenario,
                "bridge_build_dir": bridge_build_dir,
                "bridge_library_path": bridge_library_path,
                "can_interface": can_interface,
                "can_probe_timeout_seconds": can_probe_timeout_seconds,
                "chassis_control_branch": chassis_control_branch,
                "chassis_control_library_path": chassis_control_library,
                "chassis_control_root": chassis_control_root,
                "hardware_devices": list(devices),
                "minepilot_branch": minepilot_branch,
                "minepilot_can_probe_build_dir": minepilot_can_probe_build_dir,
                "minepilot_root": minepilot_root,
                "mine_teleop_binary": mine_teleop_binary,
                "network_interface": network_interface,
                "uploader_work_dir": uploader_work_dir,
                "vehicle_config_path": vehicle_config_path,
            },
        )

    def to_jsonl(self) -> tuple[str, ...]:
        summary = self._summary_metadata()
        summary.update(
            {
                "event": "target_host_validation_plan",
                "command_count": len(self.commands),
                "required_count": sum(1 for command in self.commands if command.required),
                "optional_count": sum(1 for command in self.commands if not command.required),
            }
        )
        lines = [json.dumps(summary, ensure_ascii=False, sort_keys=True)]
        for command in self.commands:
            record = asdict(command)
            record["event"] = "target_host_validation_command"
            lines.append(json.dumps(record, ensure_ascii=False, sort_keys=True))
        return tuple(lines)

    def to_shell_script(self, artifact_dir: str | None = None) -> str:
        if artifact_dir is not None:
            return self._to_artifact_shell_script(artifact_dir)
        lines = [
            "#!/usr/bin/env bash",
            "set -euo pipefail",
            "",
        ]
        for command in self.commands:
            requirement = "required" if command.required else "optional"
            lines.append(f"# [{requirement}] {command.name}: {command.purpose}")
            lines.append(command.command)
            lines.append("")
        return "\n".join(lines).rstrip() + "\n"

    def _to_artifact_shell_script(self, artifact_dir: str) -> str:
        summary_metadata_json = json.dumps(
            self._summary_metadata(),
            ensure_ascii=False,
            sort_keys=True,
        )
        lines = [
            "#!/usr/bin/env bash",
            "set -euo pipefail",
            "",
            f"artifact_dir={_quote(artifact_dir)}",
            'results_path="$artifact_dir/target_host_validation_results.jsonl"',
            'mkdir -p "$artifact_dir"',
            ': > "$results_path"',
            "required_failures=0",
            "optional_failures=0",
            "",
            "run_validation_command() {",
            '  local name="$1"',
            '  local required="$2"',
            '  local command="$3"',
            '  local stdout_path="$artifact_dir/${name}.stdout.log"',
            '  local stderr_path="$artifact_dir/${name}.stderr.log"',
            '  set +e',
            '  bash -lc "$command" >"$stdout_path" 2>"$stderr_path"',
            '  local status="$?"',
            '  set -e',
            '  python3 - "$results_path" "$name" "$required" "$status" "$command" "$stdout_path" "$stderr_path" <<\'PY\'',
            "import json",
            "import sys",
            "",
            "results_path, name, required, status, command, stdout_path, stderr_path = sys.argv[1:]",
            "record = {",
            '    "event": "target_host_validation_result",',
            '    "name": name,',
            '    "required": required == "required",',
            '    "returncode": int(status),',
            '    "command": command,',
            '    "stdout_path": stdout_path,',
            '    "stderr_path": stderr_path,',
            "}",
            'with open(results_path, "a", encoding="utf-8") as handle:',
            "    handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + \"\\n\")",
            "PY",
            '  if [ "$required" = "required" ] && [ "$status" -ne 0 ]; then',
            "    required_failures=$((required_failures + 1))",
            "  fi",
            '  if [ "$required" = "optional" ] && [ "$status" -ne 0 ]; then',
            "    optional_failures=$((optional_failures + 1))",
            "  fi",
            "}",
            "",
        ]
        for command in self.commands:
            requirement = "required" if command.required else "optional"
            lines.append(f"# [{requirement}] {command.name}: {command.purpose}")
            lines.append(
                f"run_validation_command {_quote(command.name)} {requirement} {_quote(command.command)}"
            )
            lines.append("")
        lines.extend(
            [
                (
                    "python3 - \"$results_path\" \"$required_failures\" \"$optional_failures\" <<'PY'\n"
                    "import json\n"
                    "import re\n"
                    "import sys\n"
                    "\n"
                    "results_path, required_failures, optional_failures = sys.argv[1:]\n"
                    "required_failures = int(required_failures)\n"
                    "optional_failures = int(optional_failures)\n"
                    f"metadata = json.loads({summary_metadata_json!r})\n"
                    "commit_re = re.compile(r\"\\bcommit=([0-9a-fA-F]{40})\\b\")\n"
                    "dirty_re = re.compile(r\"\\bdirty=(true|false)\\b.*\\bchanged_paths=(\\d+)\\b\")\n"
                    "\n"
                    "def json_records(path):\n"
                    "    try:\n"
                    "        with open(path, \"r\", encoding=\"utf-8\") as handle:\n"
                    "            for line in handle:\n"
                    "                if not line.strip():\n"
                    "                    continue\n"
                    "                try:\n"
                    "                    record = json.loads(line)\n"
                    "                except json.JSONDecodeError:\n"
                    "                    continue\n"
                    "                if isinstance(record, dict):\n"
                    "                    yield record\n"
                    "    except OSError:\n"
                    "        return\n"
                    "\n"
                    "def attach_bridge_checkout_metadata(summary):\n"
                    "    for result in json_records(results_path):\n"
                    "        if result.get(\"event\") != \"target_host_validation_result\":\n"
                    "            continue\n"
                    "        if result.get(\"name\") != \"chassis.bridge.check\":\n"
                    "            continue\n"
                    "        stdout_path = result.get(\"stdout_path\")\n"
                    "        if not isinstance(stdout_path, str) or not stdout_path:\n"
                    "            return\n"
                    "        for bridge_record in json_records(stdout_path):\n"
                    "            name = bridge_record.get(\"name\")\n"
                    "            message = bridge_record.get(\"message\")\n"
                    "            if not isinstance(name, str) or not isinstance(message, str):\n"
                    "                continue\n"
                    "            prefix = None\n"
                    "            if name.startswith(\"chassis_control.\"):\n"
                    "                prefix = \"chassis_control\"\n"
                    "            elif name.startswith(\"minepilot.\"):\n"
                    "                prefix = \"minepilot\"\n"
                    "            if name == \"chassis_control.library\":\n"
                    "                path = bridge_record.get(\"path\")\n"
                    "                if isinstance(path, str) and path:\n"
                    "                    summary[\"chassis_control_library_path\"] = path\n"
                    "            if prefix is None:\n"
                    "                continue\n"
                    "            if name.endswith(\".commit\"):\n"
                    "                match = commit_re.search(message)\n"
                    "                if match is not None:\n"
                    "                    summary[f\"{prefix}_commit\"] = match.group(1)\n"
                    "            elif name.endswith(\".dirty\"):\n"
                    "                match = dirty_re.search(message)\n"
                    "                if match is not None:\n"
                    "                    summary[f\"{prefix}_dirty\"] = match.group(1) == \"true\"\n"
                    "                    summary[f\"{prefix}_changed_paths\"] = int(match.group(2))\n"
                    "        return\n"
                    "\n"
                    "record = dict(metadata)\n"
                    "attach_bridge_checkout_metadata(record)\n"
                    "record.update({\n"
                    "    \"event\": \"target_host_validation_summary\",\n"
                    f"    \"command_count\": {len(self.commands)},\n"
                    f"    \"required_count\": {sum(1 for command in self.commands if command.required)},\n"
                    f"    \"optional_count\": {sum(1 for command in self.commands if not command.required)},\n"
                    "    \"required_failures\": required_failures,\n"
                    "    \"optional_failures\": optional_failures,\n"
                    "    \"passed\": required_failures == 0,\n"
                    "})\n"
                    "with open(results_path, \"a\", encoding=\"utf-8\") as handle:\n"
                    "    handle.write(json.dumps(record, ensure_ascii=False, sort_keys=True) + \"\\n\")\n"
                    "PY"
                ),
                'archive_report_path="$artifact_dir/target_host_validation_archive.jsonl"',
                "set +e",
                (
                    'python3 scripts/target_host_validation_report.py '
                    '--results "$results_path" --verify-artifacts >"$archive_report_path"'
                ),
                'report_status="$?"',
                "set -e",
                'if [ "$report_status" -ne 0 ]; then',
                '  exit "$report_status"',
                "fi",
                "",
            ]
        )
        return "\n".join(lines)

    def _summary_metadata(self) -> dict[str, Any]:
        metadata = {
            "acceptance_scenario": "target-host-acceptance",
            "vehicle_config_path": "/etc/mine-teleop/vehicle-agent.yaml",
            "can_interface": "can0",
            "chassis_control_root": str(DEFAULT_CHASSIS_CONTROL_ROOT),
            "minepilot_root": str(DEFAULT_MINEPILOT_ROOT),
            "chassis_control_branch": DEFAULT_CHASSIS_CONTROL_BRANCH,
            "minepilot_branch": DEFAULT_MINEPILOT_BRANCH,
            "bridge_build_dir": "build/chassis-control-bridge",
            "bridge_library_path": "/opt/mine-teleop/lib/libmine_teleop_chassis_bridge.so",
            "chassis_control_library_path": f"{DEFAULT_MINEPILOT_ROOT}/libchassis_control.so",
            "uploader_work_dir": "/var/lib/mine-teleop/uploader",
            "minepilot_can_probe_build_dir": "/tmp/mine-teleop-minepilot-can-probe",
            "can_probe_timeout_seconds": 3,
        }
        metadata.update(dict(self.context or {}))
        metadata["command_names"] = [command.name for command in self.commands]
        metadata["command_requirements"] = {command.name: command.required for command in self.commands}
        return metadata


def _quote(value: str) -> str:
    return shlex.quote(value)


def _entrypoint_command(mine_teleop_binary: str | None, subcommand: str, source_command: str) -> str:
    if mine_teleop_binary:
        return f"{_quote(mine_teleop_binary)} {subcommand}"
    return source_command


def _jsonl_echo_command(record: Mapping[str, Any]) -> str:
    payload = json.dumps(dict(record), ensure_ascii=False, sort_keys=True)
    return f"printf '%s\\n' {_quote(payload)}"


def _minepilot_can_sender_smoke_json_python(
    can_interface: str,
    timeout_seconds: int,
    executable: str,
) -> str:
    event_json = json.dumps(_MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME])
    interface_json = json.dumps(can_interface)
    executable_json = json.dumps(executable)
    startup_banner_json = json.dumps(_MINEPILOT_CAN_SENDER_STARTUP_BANNER)
    return (
        "import json, os; "
        f"startup_banner = {startup_banner_json}; "
        "probe_output = os.environ.get('MINE_TELEOP_CAN_SMOKE_OUTPUT', ''); "
        "print(json.dumps({"
        "'accepted_exit_code': True, "
        f"'event': {event_json}, "
        "'exit_code': int(os.environ['MINE_TELEOP_CAN_SMOKE_STATUS']), "
        f"'executable': {executable_json}, "
        f"'interface': {interface_json}, "
        "'passed': True, "
        "'startup_banner_seen': startup_banner in probe_output, "
        f"'timeout_seconds': {timeout_seconds}"
        "}, sort_keys=True))"
    )


def _select_vaapi_render_device(devices: tuple[str, ...]) -> str:
    for device in devices:
        if device.rsplit("/", 1)[-1].startswith("renderD"):
            return device
    return "/dev/dri/renderD128"


def _validation_result_failed(record: dict[str, Any]) -> bool:
    try:
        return int(record.get("returncode", 1)) != 0
    except (TypeError, ValueError):
        return True


def _path_is_within_root(path: str, root: str) -> bool:
    normalized_root = root.rstrip("/")
    return path == normalized_root or path.startswith(f"{normalized_root}/")


def _adapter_status_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    if record.get("name") != _ADAPTER_STATUS_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    return _adapter_status_records_failure(
        _stdout_json_records(stdout_path),
        stdout_path,
        summary,
        _ADAPTER_STATUS_COMMAND_NAME,
    )


def _adapter_status_records_failure(
    records: tuple[dict[str, Any], ...],
    stdout_path: str,
    summary: dict[str, Any] | None,
    name: str,
) -> dict[str, str] | None:
    status_records = tuple(
        stdout_record for stdout_record in records if stdout_record.get("event") == "vehicle_adapter_status"
    )
    if len(status_records) > 1:
        failure = _adapter_status_failure("vehicle_adapter_status_duplicate", stdout_path, name)
        failure["actual"] = str(len(status_records))
        return failure
    for stdout_record in status_records:
        status = stdout_record.get("status")
        if not isinstance(status, dict):
            return _adapter_status_failure("vehicle_adapter_status_missing_status", stdout_path, name)
        if status.get("opened") is not True:
            return _adapter_status_failure("vehicle_adapter_status_not_opened", stdout_path, name)
        if status.get("healthy") is not True:
            return _adapter_status_failure("vehicle_adapter_status_not_healthy", stdout_path, name)
        if stdout_record.get("ready") is not True:
            return _adapter_status_failure("vehicle_adapter_status_not_ready", stdout_path, name)
        if status.get("adapter_type") not in {"can", "dynamic_library"}:
            return _adapter_status_failure("vehicle_adapter_status_not_real_adapter", stdout_path, name)
        metadata_failure = _adapter_status_metadata_failure(status, stdout_path, name)
        if metadata_failure is not None:
            return metadata_failure
        interface_failure = _adapter_status_can_interface_failure(status, stdout_path, summary, name)
        if interface_failure is not None:
            return interface_failure
        library_failure = _adapter_status_library_path_failure(status, stdout_path, summary, name)
        if library_failure is not None:
            return library_failure
        return None
    return _adapter_status_failure("vehicle_adapter_status_missing", stdout_path, name)


def _chassis_bridge_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    if record.get("name") != _CHASSIS_BRIDGE_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    stdout_records = _stdout_json_records(stdout_path)
    ready_summary_records = [
        stdout_record
        for stdout_record in stdout_records
        if stdout_record.get("event") == "chassis_bridge_check" and stdout_record.get("ready") is True
    ]
    if len(ready_summary_records) > 1:
        return {
            "actual": str(len(ready_summary_records)),
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_duplicate_ready_summary",
            "stdout_path": stdout_path,
        }
    summary_record = ready_summary_records[0] if ready_summary_records else None
    if summary_record is None:
        return {
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_missing_ready_summary",
            "stdout_path": stdout_path,
        }
    checks: dict[str, dict[str, Any]] = {}
    check_counts: dict[str, int] = {}
    for stdout_record in stdout_records:
        name = stdout_record.get("name")
        if isinstance(name, str):
            check_counts[name] = check_counts.get(name, 0) + 1
            checks[name] = stdout_record
    for check_name, count in check_counts.items():
        if count > 1:
            return {
                "actual": str(count),
                "check": check_name,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_duplicate_check",
                "stdout_path": stdout_path,
            }
    missing = tuple(check for check in _REQUIRED_CHASSIS_BRIDGE_READY_CHECKS if check not in checks)
    if missing:
        return {
            "missing": ",".join(missing),
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_missing_ready_checks",
            "stdout_path": stdout_path,
        }
    check_count = summary_record.get("check_count")
    if isinstance(check_count, bool) or not isinstance(check_count, int) or check_count <= 0:
        return {
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_invalid_summary",
            "stdout_path": stdout_path,
            "summary": str(check_count),
        }
    if check_count != len(checks):
        return {
            "actual": len(checks),
            "expected": check_count,
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_count_mismatch",
            "stdout_path": stdout_path,
        }
    for check_name in _REQUIRED_CHASSIS_BRIDGE_READY_CHECKS:
        status = checks[check_name].get("status")
        if status != "ready":
            return {
                "check": check_name,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_not_ready",
                "status": str(status),
                "stdout_path": stdout_path,
            }
    metadata_failure = _chassis_bridge_metadata_failure(checks, stdout_path, summary)
    if metadata_failure is not None:
        return metadata_failure
    return None


def _chassis_bridge_metadata_failure(
    checks: dict[str, dict[str, Any]],
    stdout_path: str,
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    for check_name, summary_field in _CHASSIS_BRIDGE_ROOT_SUMMARY_FIELDS.items():
        actual = checks[check_name].get("path")
        if not isinstance(actual, str) or actual == "":
            return {
                "check": check_name,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_missing_root_metadata",
                "stdout_path": stdout_path,
            }
        expected = summary.get(summary_field) if summary is not None else None
        if isinstance(expected, str) and expected and actual != expected:
            return {
                "actual": actual,
                "check": check_name,
                "expected": expected,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_root_summary_mismatch",
                "stdout_path": stdout_path,
            }
    library_path = checks["chassis_control.library"].get("path")
    if not isinstance(library_path, str) or library_path == "":
        return {
            "check": "chassis_control.library",
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_missing_library_metadata",
            "stdout_path": stdout_path,
        }
    expected_library_roots = tuple(
        root
        for root in (
            summary.get("chassis_control_root") if summary is not None else None,
            summary.get("minepilot_root") if summary is not None else None,
        )
        if isinstance(root, str) and root
    )
    if expected_library_roots and not any(_path_is_within_root(library_path, root) for root in expected_library_roots):
        return {
            "actual": library_path,
            "check": "chassis_control.library",
            "expected": ",".join(expected_library_roots),
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_library_root_mismatch",
            "stdout_path": stdout_path,
        }
    if summary is not None:
        summary_library_path = summary.get("chassis_control_library_path")
        if "chassis_control_library_path" not in summary:
            return {
                "check": "chassis_control.library",
                "field": "chassis_control_library_path",
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_library_summary_missing",
                "stdout_path": stdout_path,
            }
        if not isinstance(summary_library_path, str) or summary_library_path == "":
            return {
                "check": "chassis_control.library",
                "field": "chassis_control_library_path",
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_library_summary_invalid",
                "stdout_path": stdout_path,
                "summary": str(summary_library_path),
            }
        if library_path != summary_library_path:
            return {
                "actual": library_path,
                "check": "chassis_control.library",
                "expected": summary_library_path,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_library_summary_mismatch",
                "stdout_path": stdout_path,
            }
    symbols_path = checks["chassis_control.symbols"].get("path")
    if not isinstance(symbols_path, str) or symbols_path == "":
        return {
            "check": "chassis_control.symbols",
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_missing_symbols_metadata",
            "stdout_path": stdout_path,
        }
    if symbols_path != library_path:
        return {
            "actual": symbols_path,
            "check": "chassis_control.symbols",
            "expected": library_path,
            "name": _CHASSIS_BRIDGE_COMMAND_NAME,
            "reason": "chassis_bridge_check_symbols_library_mismatch",
            "stdout_path": stdout_path,
        }
    for check_name, summary_field in _CHASSIS_BRIDGE_BRANCH_SUMMARY_FIELDS.items():
        message = checks[check_name].get("message")
        match = _CHASSIS_BRIDGE_BRANCH_METADATA_RE.search(message) if isinstance(message, str) else None
        if match is None:
            return {
                "check": check_name,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_missing_branch_metadata",
                "stdout_path": stdout_path,
            }
        expected = summary.get(summary_field) if summary is not None else None
        actual = match.group("branch")
        if isinstance(expected, str) and expected and actual != expected:
            return {
                "actual": actual,
                "check": check_name,
                "expected": expected,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_branch_summary_mismatch",
                "stdout_path": stdout_path,
            }
    for check_name in _CHASSIS_BRIDGE_COMMIT_CHECKS:
        message = checks[check_name].get("message")
        match = _CHASSIS_BRIDGE_COMMIT_METADATA_RE.search(message) if isinstance(message, str) else None
        if match is None:
            return {
                "check": check_name,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_missing_commit_metadata",
                "stdout_path": stdout_path,
            }
        prefix = check_name.split(".", 1)[0]
        summary_field = f"{prefix}_commit"
        expected = summary.get(summary_field) if summary is not None else None
        actual = match.group("commit")
        if summary is not None and summary_field not in summary:
            return {
                "check": check_name,
                "field": summary_field,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_missing",
                "stdout_path": stdout_path,
            }
        if summary is not None and (
            not isinstance(expected, str) or _CHASSIS_BRIDGE_COMMIT_METADATA_RE.fullmatch(f"commit={expected}") is None
        ):
            return {
                "check": check_name,
                "field": summary_field,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_invalid",
                "stdout_path": stdout_path,
                "summary": str(expected),
            }
        if isinstance(expected, str) and expected and actual != expected:
            return {
                "actual": actual,
                "check": check_name,
                "expected": expected,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_mismatch",
                "stdout_path": stdout_path,
            }
    for check_name in _CHASSIS_BRIDGE_DIRTY_CHECKS:
        message = checks[check_name].get("message")
        match = _CHASSIS_BRIDGE_DIRTY_METADATA_RE.search(message) if isinstance(message, str) else None
        if match is None:
            return {
                "check": check_name,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_missing_dirty_metadata",
                "stdout_path": stdout_path,
            }
        prefix = check_name.split(".", 1)[0]
        actual_dirty = match.group("dirty") == "true"
        dirty_field = f"{prefix}_dirty"
        expected_dirty = summary.get(dirty_field) if summary is not None else None
        if summary is not None and dirty_field not in summary:
            return {
                "check": check_name,
                "field": dirty_field,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_missing",
                "stdout_path": stdout_path,
            }
        if summary is not None and not isinstance(expected_dirty, bool):
            return {
                "check": check_name,
                "field": dirty_field,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_invalid",
                "stdout_path": stdout_path,
                "summary": str(expected_dirty),
            }
        if isinstance(expected_dirty, bool) and actual_dirty != expected_dirty:
            return {
                "actual": str(actual_dirty).lower(),
                "check": check_name,
                "expected": str(expected_dirty).lower(),
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_mismatch",
                "stdout_path": stdout_path,
            }
        actual_changed_paths = int(match.group("changed_paths"))
        changed_paths_field = f"{prefix}_changed_paths"
        expected_changed_paths = summary.get(changed_paths_field) if summary is not None else None
        if summary is not None and changed_paths_field not in summary:
            return {
                "check": check_name,
                "field": changed_paths_field,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_missing",
                "stdout_path": stdout_path,
            }
        if summary is not None and (
            isinstance(expected_changed_paths, bool)
            or not isinstance(expected_changed_paths, int)
            or expected_changed_paths < 0
        ):
            return {
                "check": check_name,
                "field": changed_paths_field,
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_invalid",
                "stdout_path": stdout_path,
                "summary": str(expected_changed_paths),
            }
        if (
            isinstance(expected_changed_paths, int)
            and not isinstance(expected_changed_paths, bool)
            and actual_changed_paths != expected_changed_paths
        ):
            return {
                "actual": str(actual_changed_paths),
                "check": check_name,
                "expected": str(expected_changed_paths),
                "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                "reason": "chassis_bridge_check_revision_summary_mismatch",
                "stdout_path": stdout_path,
            }
    expected_build_dir = summary.get("bridge_build_dir") if summary is not None else None
    if isinstance(expected_build_dir, str) and expected_build_dir:
        for check_name in _CHASSIS_BRIDGE_BUILD_DIR_CHECKS:
            actual = checks[check_name].get("path")
            if actual != expected_build_dir:
                return {
                    "actual": str(actual),
                    "check": check_name,
                    "expected": expected_build_dir,
                    "name": _CHASSIS_BRIDGE_COMMAND_NAME,
                    "reason": "chassis_bridge_check_build_dir_summary_mismatch",
                    "stdout_path": stdout_path,
                }
    return None


def _preflight_evidence_failure(record: dict[str, Any]) -> dict[str, str] | None:
    if record.get("name") != _PREFLIGHT_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == _PREFLIGHT_EVENT
    ]
    if len(matching_records) > 1:
        failure = _preflight_failure("vehicle_preflight_duplicate_evidence", stdout_path)
        failure["actual"] = str(len(matching_records))
        return failure
    for stdout_record in matching_records:
        if stdout_record.get("ready") is not True:
            return _preflight_failure("vehicle_preflight_not_ready", stdout_path)
        check_count = stdout_record.get("check_count")
        if isinstance(check_count, bool) or not isinstance(check_count, int) or check_count <= 0:
            return _preflight_failure("vehicle_preflight_invalid_summary", stdout_path)
        return None
    return _preflight_failure("vehicle_preflight_missing_ready_summary", stdout_path)


def _preflight_failure(reason: str, stdout_path: str) -> dict[str, str]:
    return {
        "event": _PREFLIGHT_EVENT,
        "name": _PREFLIGHT_COMMAND_NAME,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _hardware_probe_plan_evidence_failure(record: dict[str, Any]) -> dict[str, str] | None:
    if record.get("name") != _HARDWARE_PROBES_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    lines = _stdout_text_lines(stdout_path)
    if not any(line.startswith("gst_plugin_probe=") and "gst-inspect-1.0" in line for line in lines):
        return _hardware_probe_plan_failure("hardware_probe_plan_missing_gst_plugin_probe", stdout_path)
    scenario_entries = tuple(
        line.split("=", 1)[1].strip()
        for line in lines
        if line.startswith("scenario=") and line.split("=", 1)[1].strip()
    )
    duplicate_scenarios = _duplicate_values(scenario_entries)
    if duplicate_scenarios:
        failure = _hardware_probe_plan_failure("hardware_probe_plan_duplicate_scenarios", stdout_path)
        failure["scenario"] = duplicate_scenarios[0][0]
        failure["actual"] = str(duplicate_scenarios[0][1])
        return failure
    scenario_names = set(scenario_entries)
    missing_scenarios = tuple(
        scenario for scenario in _REQUIRED_HARDWARE_PROBE_SCENARIOS if scenario not in scenario_names
    )
    if missing_scenarios:
        failure = _hardware_probe_plan_failure("hardware_probe_plan_missing_scenarios", stdout_path)
        failure["missing"] = ",".join(missing_scenarios)
        return failure
    unexpected_scenarios = tuple(
        scenario for scenario in sorted(scenario_names) if scenario not in _REQUIRED_HARDWARE_PROBE_SCENARIOS
    )
    if unexpected_scenarios:
        failure = _hardware_probe_plan_failure("hardware_probe_plan_unexpected_scenarios", stdout_path)
        failure["unexpected"] = ",".join(unexpected_scenarios)
        return failure
    metrics = _hardware_probe_plan_metrics(lines)
    missing_metrics = tuple(field for field in _REQUIRED_HARDWARE_METRIC_FIELDS if field not in metrics)
    if missing_metrics:
        failure = _hardware_probe_plan_failure("hardware_probe_plan_missing_metrics", stdout_path)
        failure["missing"] = ",".join(missing_metrics)
        return failure
    return None


def _hardware_probe_plan_metrics(lines: tuple[str, ...]) -> set[str]:
    for line in lines:
        if not line.startswith("metrics="):
            continue
        return {field.strip() for field in line.split("=", 1)[1].split(",") if field.strip()}
    return set()


def _duplicate_values(values: tuple[str, ...]) -> tuple[tuple[str, int], ...]:
    counts: dict[str, int] = {}
    for value in values:
        counts[value] = counts.get(value, 0) + 1
    seen: set[str] = set()
    duplicates: list[tuple[str, int]] = []
    for value in values:
        if counts[value] <= 1 or value in seen:
            continue
        seen.add(value)
        duplicates.append((value, counts[value]))
    return tuple(duplicates)


def _hardware_probe_plan_failure(reason: str, stdout_path: str) -> dict[str, str]:
    return {
        "name": _HARDWARE_PROBES_COMMAND_NAME,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _gpu_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    name = record.get("name")
    if not isinstance(name, str):
        return None
    event_name = _GPU_COMMAND_EVENTS.get(name)
    if event_name is None:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == event_name
    ]
    if len(matching_records) > 1:
        failure = _gpu_failure(name, event_name, f"{event_name}_duplicate_evidence", stdout_path)
        failure["actual"] = str(len(matching_records))
        return failure
    for stdout_record in matching_records:
        if stdout_record.get("passed") is not True:
            return _gpu_failure(name, event_name, f"{event_name}_invalid_evidence", stdout_path)
        if name == _GPU_DRI_COMMAND_NAME and stdout_record.get("path") != "/dev/dri":
            return _gpu_failure(name, event_name, f"{event_name}_invalid_evidence", stdout_path)
        if name == _GPU_VAAPI_COMMAND_NAME:
            expected_device = _summary_vaapi_render_device(summary)
            device = stdout_record.get("device")
            if expected_device is not None and device != expected_device:
                failure = _gpu_failure(name, event_name, "gpu_vaapi_vainfo_device_mismatch", stdout_path)
                failure["expected"] = expected_device
                failure["actual"] = str(device)
                return failure
        return None
    return _gpu_failure(name, event_name, f"{event_name}_missing_evidence", stdout_path)


def _summary_vaapi_render_device(summary: dict[str, Any] | None) -> str | None:
    if summary is None:
        return None
    devices = summary.get("hardware_devices")
    if not isinstance(devices, list) or not all(isinstance(device, str) for device in devices):
        return None
    return _select_vaapi_render_device(tuple(devices))


def _gpu_failure(name: str, event_name: str, reason: str, stdout_path: str) -> dict[str, str]:
    return {
        "event": event_name,
        "name": name,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _minepilot_can_probe_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    name = record.get("name")
    if not isinstance(name, str):
        return None
    event_name = _MINEPILOT_CAN_COMMAND_EVENTS.get(name)
    if event_name is None:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == event_name
    ]
    if len(matching_records) > 1:
        failure = _minepilot_can_probe_failure(
            name,
            event_name,
            "minepilot_can_probe_duplicate_evidence",
            stdout_path,
        )
        failure["actual"] = str(len(matching_records))
        return failure
    for stdout_record in matching_records:
        if stdout_record.get("passed") is not True:
            return _minepilot_can_probe_failure(
                name,
                event_name,
                "minepilot_can_probe_invalid_evidence",
                stdout_path,
            )
        interface_failure = _minepilot_can_probe_interface_failure(
            name,
            event_name,
            stdout_record,
            stdout_path,
            summary,
        )
        if interface_failure is not None:
            return interface_failure
        if name == _MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME:
            script_failure = _minepilot_can_socket_probe_script_evidence_failure(
                stdout_record,
                stdout_path,
                summary,
            )
            if script_failure is not None:
                return script_failure
        if name == _MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME:
            build_failure = _minepilot_can_sender_build_evidence_failure(stdout_record, stdout_path, summary)
            if build_failure is not None:
                return build_failure
        if name == _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME:
            smoke_failure = _minepilot_can_sender_smoke_evidence_failure(stdout_record, stdout_path, summary)
            if smoke_failure is not None:
                return smoke_failure
        return None
    return _minepilot_can_probe_failure(
        name,
        event_name,
        "minepilot_can_probe_missing_evidence",
        stdout_path,
    )


def _minepilot_can_sender_smoke_evidence_failure(
    record: dict[str, Any],
    stdout_path: str,
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    exit_code = record.get("exit_code")
    if record.get("accepted_exit_code") is not True or isinstance(exit_code, bool) or exit_code not in {0, 124}:
        failure = _minepilot_can_probe_failure(
            _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME,
            _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME],
            "minepilot_can_probe_invalid_evidence",
            stdout_path,
        )
        failure["exit_code"] = str(exit_code)
        return failure
    if record.get("startup_banner_seen") is not True:
        return _minepilot_can_probe_failure(
            _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME,
            _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME],
            "minepilot_can_sender_smoke_startup_banner_missing",
            stdout_path,
        )
    expected_timeout = summary.get("can_probe_timeout_seconds") if summary is not None else None
    if isinstance(expected_timeout, int) and not isinstance(expected_timeout, bool) and expected_timeout > 0:
        actual_timeout = record.get("timeout_seconds")
        if actual_timeout != expected_timeout:
            failure = _minepilot_can_probe_failure(
                _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME,
                _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME],
                "minepilot_can_sender_smoke_timeout_mismatch",
                stdout_path,
            )
            failure["actual"] = str(actual_timeout)
            failure["expected"] = str(expected_timeout)
            return failure
    expected_build_dir = summary.get("minepilot_can_probe_build_dir") if summary is not None else None
    if isinstance(expected_build_dir, str) and expected_build_dir:
        expected_executable = f"{expected_build_dir}/can_sender_main"
        actual_executable = record.get("executable")
        if actual_executable != expected_executable:
            failure = _minepilot_can_probe_failure(
                _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME,
                _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME],
                "minepilot_can_sender_smoke_executable_mismatch",
                stdout_path,
            )
            failure["actual"] = str(actual_executable)
            failure["expected"] = expected_executable
            return failure
    return None


def _minepilot_can_sender_build_evidence_failure(
    record: dict[str, Any],
    stdout_path: str,
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    target = record.get("target")
    if target != "can_sender_main":
        failure = _minepilot_can_probe_failure(
            _MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME,
            _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME],
            "minepilot_can_sender_build_target_mismatch",
            stdout_path,
        )
        failure["actual"] = str(target)
        failure["expected"] = "can_sender_main"
        return failure
    expected_build_dir = summary.get("minepilot_can_probe_build_dir") if summary is not None else None
    if not isinstance(expected_build_dir, str) or expected_build_dir == "":
        return None
    actual_build_dir = record.get("build_dir")
    if actual_build_dir == expected_build_dir:
        return None
    failure = _minepilot_can_probe_failure(
        _MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME,
        _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SENDER_BUILD_COMMAND_NAME],
        "minepilot_can_sender_build_dir_mismatch",
        stdout_path,
    )
    failure["actual"] = str(actual_build_dir)
    failure["expected"] = expected_build_dir
    return failure


def _minepilot_can_socket_probe_script_evidence_failure(
    record: dict[str, Any],
    stdout_path: str,
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    minepilot_root = summary.get("minepilot_root") if summary is not None else None
    if not isinstance(minepilot_root, str) or minepilot_root == "":
        return None
    expected_script = f"{minepilot_root}/script/check_can.sh"
    actual_script = record.get("script")
    if actual_script == expected_script:
        return None
    failure = _minepilot_can_probe_failure(
        _MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME,
        _MINEPILOT_CAN_COMMAND_EVENTS[_MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME],
        "minepilot_can_socket_probe_script_mismatch",
        stdout_path,
    )
    failure["actual"] = str(actual_script)
    failure["expected"] = expected_script
    return failure


def _minepilot_can_probe_interface_failure(
    name: str,
    event_name: str,
    record: dict[str, Any],
    stdout_path: str,
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    if name not in {_MINEPILOT_CAN_SOCKET_PROBE_COMMAND_NAME, _MINEPILOT_CAN_SENDER_SMOKE_COMMAND_NAME}:
        return None
    expected = summary.get("can_interface") if summary is not None else None
    if not isinstance(expected, str) or expected == "":
        return None
    actual = record.get("interface")
    if actual == expected:
        return None
    failure = _minepilot_can_probe_failure(
        name,
        event_name,
        "minepilot_can_probe_interface_mismatch",
        stdout_path,
    )
    failure["actual"] = str(actual)
    failure["expected"] = expected
    return failure


def _minepilot_can_probe_failure(
    name: str,
    event_name: str,
    reason: str,
    stdout_path: str,
) -> dict[str, str]:
    return {
        "event": event_name,
        "name": name,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _can_interface_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    if record.get("name") != _CAN_INTERFACE_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    expected_interface = None
    if summary is not None and isinstance(summary.get("can_interface"), str):
        expected_interface = summary["can_interface"]
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == _CAN_INTERFACE_EVENT
    ]
    if len(matching_records) > 1:
        failure = _can_interface_failure("can_interface_state_duplicate_evidence", stdout_path)
        failure["actual"] = str(len(matching_records))
        return failure
    for stdout_record in matching_records:
        interface = stdout_record.get("interface")
        if stdout_record.get("passed") is not True or not isinstance(interface, str) or interface == "":
            return _can_interface_failure("can_interface_state_invalid_evidence", stdout_path)
        if expected_interface is not None and interface != expected_interface:
            failure = _can_interface_failure("can_interface_state_interface_mismatch", stdout_path)
            failure["expected"] = expected_interface
            failure["actual"] = interface
            return failure
        return None
    return _can_interface_failure("can_interface_state_missing_evidence", stdout_path)


def _can_interface_failure(reason: str, stdout_path: str) -> dict[str, str]:
    return {
        "event": _CAN_INTERFACE_EVENT,
        "name": _CAN_INTERFACE_COMMAND_NAME,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _minepilot_can_sources_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    if record.get("name") != _MINEPILOT_CAN_SOURCES_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == _MINEPILOT_CAN_SOURCES_EVENT
    ]
    if len(matching_records) > 1:
        failure = _minepilot_can_sources_failure("minepilot_can_sources_duplicate_evidence", stdout_path)
        failure["actual"] = str(len(matching_records))
        return failure
    for stdout_record in matching_records:
        files = stdout_record.get("files")
        if stdout_record.get("passed") is not True or not isinstance(files, list):
            return _minepilot_can_sources_failure("minepilot_can_sources_invalid_evidence", stdout_path)
        file_names = {file_name for file_name in files if isinstance(file_name, str)}
        missing = tuple(
            source_file
            for source_file in _REQUIRED_MINEPILOT_CAN_SOURCE_FILES
            if source_file not in file_names
        )
        if missing:
            failure = _minepilot_can_sources_failure("minepilot_can_sources_missing_files", stdout_path)
            failure["missing"] = ",".join(missing)
            return failure
        expected_root = summary.get("minepilot_root") if summary is not None else None
        if isinstance(expected_root, str) and expected_root:
            actual_root = stdout_record.get("root")
            if actual_root != expected_root:
                failure = _minepilot_can_sources_failure("minepilot_can_sources_root_mismatch", stdout_path)
                failure["actual"] = str(actual_root)
                failure["expected"] = expected_root
                return failure
        return None
    return _minepilot_can_sources_failure("minepilot_can_sources_missing_evidence", stdout_path)


def _minepilot_can_sources_failure(reason: str, stdout_path: str) -> dict[str, str]:
    return {
        "event": _MINEPILOT_CAN_SOURCES_EVENT,
        "name": _MINEPILOT_CAN_SOURCES_COMMAND_NAME,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _adapter_status_failure(
    reason: str,
    stdout_path: str,
    name: str = _ADAPTER_STATUS_COMMAND_NAME,
) -> dict[str, str]:
    return {
        "name": name,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _adapter_status_metadata_failure(
    status: dict[str, Any],
    stdout_path: str,
    name: str = _ADAPTER_STATUS_COMMAND_NAME,
) -> dict[str, str] | None:
    adapter_type = status.get("adapter_type")
    if adapter_type in {"can", "dynamic_library"}:
        missing = tuple(
            field
            for field in _BRIDGE_ADAPTER_STATUS_METADATA_FIELDS
            if not isinstance(status.get(field), str) or status.get(field) == ""
        )
    else:
        return None
    if not missing:
        return None
    return {
        "missing": ",".join(missing),
        "name": name,
        "reason": "vehicle_adapter_status_missing_bridge_metadata",
        "stdout_path": stdout_path,
    }


def _adapter_status_can_interface_failure(
    status: dict[str, Any],
    stdout_path: str,
    summary: dict[str, Any] | None,
    name: str = _ADAPTER_STATUS_COMMAND_NAME,
) -> dict[str, str] | None:
    expected = summary.get("can_interface") if summary is not None else None
    if not isinstance(expected, str) or expected == "":
        return None
    actual = status.get("can_interface")
    if actual == expected:
        return None
    return {
        "actual": str(actual),
        "expected": expected,
        "name": name,
        "reason": "vehicle_adapter_status_can_interface_mismatch",
        "stdout_path": stdout_path,
    }


def _adapter_status_library_path_failure(
    status: dict[str, Any],
    stdout_path: str,
    summary: dict[str, Any] | None,
    name: str = _ADAPTER_STATUS_COMMAND_NAME,
) -> dict[str, str] | None:
    expected = summary.get("bridge_library_path") if summary is not None else None
    if not isinstance(expected, str) or expected == "":
        expected = summary.get("chassis_control_library_path") if summary is not None else None
    if not isinstance(expected, str) or expected == "":
        return None
    actual = status.get("library_path")
    if actual == expected:
        return None
    return {
        "actual": str(actual),
        "expected": expected,
        "name": name,
        "reason": "vehicle_adapter_status_library_path_mismatch",
        "stdout_path": stdout_path,
    }


def _feedback_poll_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    if record.get("name") != _FEEDBACK_POLL_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    path = Path(stdout_path)
    if not path.exists():
        return _feedback_poll_missing_evidence(stdout_path)
    stdout_records = _stdout_json_records(stdout_path)
    feedback_records = tuple(
        stdout_record
        for stdout_record in stdout_records
        if stdout_record.get("event") == "vehicle_adapter_feedback_poll"
    )
    if len(feedback_records) > 1:
        return {
            "actual": str(len(feedback_records)),
            "name": _FEEDBACK_POLL_COMMAND_NAME,
            "reason": "vehicle_adapter_feedback_poll_duplicate",
            "stdout_path": stdout_path,
        }
    first_event_failure: dict[str, str] | None = None
    for stdout_record in feedback_records:
        failure = _feedback_poll_record_failure(stdout_record, stdout_path)
        if failure is None:
            adapter_status_failure = _adapter_status_records_failure(
                stdout_records,
                stdout_path,
                summary,
                _FEEDBACK_POLL_COMMAND_NAME,
            )
            if adapter_status_failure is not None:
                return adapter_status_failure
            return None
        if first_event_failure is None:
            first_event_failure = failure
    if first_event_failure is not None:
        return first_event_failure
    return _feedback_poll_missing_evidence(stdout_path)


def _feedback_poll_missing_evidence(stdout_path: str) -> dict[str, str]:
    return {
        "name": _FEEDBACK_POLL_COMMAND_NAME,
        "reason": "vehicle_adapter_feedback_poll_not_received",
        "stdout_path": stdout_path,
    }


def _feedback_poll_record_failure(record: dict[str, Any], stdout_path: str) -> dict[str, str] | None:
    if record.get("attempted") is not True or record.get("received") is not True:
        return _feedback_poll_missing_evidence(stdout_path)
    snapshot = record.get("snapshot")
    if not isinstance(snapshot, dict):
        return {
            "name": _FEEDBACK_POLL_COMMAND_NAME,
            "reason": "vehicle_adapter_feedback_poll_invalid_snapshot",
            "stdout_path": stdout_path,
        }
    missing = tuple(field for field in _REQUIRED_FEEDBACK_SNAPSHOT_FIELDS if field not in snapshot)
    if not missing:
        return None
    return {
        "missing": ",".join(missing),
        "name": _FEEDBACK_POLL_COMMAND_NAME,
        "reason": "vehicle_adapter_feedback_poll_snapshot_missing_fields",
        "stdout_path": stdout_path,
    }


def _uploader_process_once_evidence_failure(record: dict[str, Any]) -> dict[str, str] | None:
    if record.get("name") != _UPLOADER_PROCESS_ONCE_COMMAND_NAME:
        return None
    if record.get("required") is not True or _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    matching_records = [
        stdout_record
        for stdout_record in _stdout_json_records(stdout_path)
        if stdout_record.get("event") == _UPLOADER_PROCESS_ONCE_EVENT
    ]
    if len(matching_records) > 1:
        failure = _uploader_process_once_failure(
            "vehicle_uploader_process_once_duplicate_evidence",
            stdout_path,
        )
        failure["actual"] = str(len(matching_records))
        return failure
    for stdout_record in matching_records:
        action = stdout_record.get("action")
        if stdout_record.get("passed") is False or action == "failed":
            return _uploader_process_once_failure(
                "vehicle_uploader_process_once_failed",
                stdout_path,
            )
        if not isinstance(action, str) or action not in _UPLOADER_PROCESS_ONCE_ALLOWED_ACTIONS:
            return _uploader_process_once_failure(
                "vehicle_uploader_process_once_invalid_evidence",
                stdout_path,
            )
        if stdout_record.get("passed") is not True:
            return _uploader_process_once_failure(
                "vehicle_uploader_process_once_invalid_evidence",
                stdout_path,
            )
        return None
    return _uploader_process_once_failure(
        "vehicle_uploader_process_once_missing_evidence",
        stdout_path,
    )


def _uploader_process_once_failure(reason: str, stdout_path: str) -> dict[str, str]:
    return {
        "event": _UPLOADER_PROCESS_ONCE_EVENT,
        "name": _UPLOADER_PROCESS_ONCE_COMMAND_NAME,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _acceptance_metrics_evidence_failure(
    record: dict[str, Any],
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    if record.get("name") != _ACCEPTANCE_METRICS_COMMAND_NAME:
        return None
    if _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    stdout_records = _stdout_json_records(stdout_path)
    seen = _event_names(stdout_records)
    missing = tuple(event for event in _REQUIRED_ACCEPTANCE_METRIC_EVENTS if event not in seen)
    if not missing:
        duplicate_failure = _acceptance_metrics_duplicate_report_failure(stdout_records, stdout_path)
        if duplicate_failure is not None:
            return duplicate_failure
        failed_report = _acceptance_metrics_failed_report(stdout_records, stdout_path)
        if failed_report is not None:
            return failed_report
        return _acceptance_metrics_scenario_failure(stdout_records, stdout_path, summary)
    return {
        "name": _ACCEPTANCE_METRICS_COMMAND_NAME,
        "reason": "acceptance_metrics_missing_reports",
        "missing": ",".join(missing),
        "stdout_path": stdout_path,
    }


def _acceptance_metrics_duplicate_report_failure(
    records: tuple[dict[str, Any], ...],
    stdout_path: str,
) -> dict[str, str] | None:
    events: list[str] = []
    for record in records:
        event = record.get("event")
        if event in _REQUIRED_ACCEPTANCE_METRIC_EVENTS:
            events.append(str(event))
    duplicates = _duplicate_values(tuple(events))
    if not duplicates:
        return None
    event, count = duplicates[0]
    return {
        "actual": str(count),
        "event": event,
        "name": _ACCEPTANCE_METRICS_COMMAND_NAME,
        "reason": "acceptance_metrics_report_duplicate",
        "stdout_path": stdout_path,
    }


def _acceptance_metrics_failed_report(
    records: tuple[dict[str, Any], ...],
    stdout_path: str,
) -> dict[str, str] | None:
    for record in records:
        event = record.get("event")
        if event not in _REQUIRED_ACCEPTANCE_METRIC_EVENTS:
            continue
        if record.get("passed") is not False:
            continue
        return {
            "event": str(event),
            "name": _ACCEPTANCE_METRICS_COMMAND_NAME,
            "reason": "acceptance_metrics_report_failed",
            "stdout_path": stdout_path,
        }
    return None


def _acceptance_metrics_scenario_failure(
    records: tuple[dict[str, Any], ...],
    stdout_path: str,
    summary: dict[str, Any] | None,
) -> dict[str, str] | None:
    expected = summary.get("acceptance_scenario") if summary is not None else None
    if not isinstance(expected, str) or expected == "":
        return None
    for record in records:
        event = record.get("event")
        if event not in _REQUIRED_ACCEPTANCE_METRIC_EVENTS:
            continue
        actual = record.get("scenario")
        if actual != expected:
            return {
                "actual": str(actual),
                "event": str(event),
                "expected": expected,
                "name": _ACCEPTANCE_METRICS_COMMAND_NAME,
                "reason": "acceptance_metrics_scenario_mismatch",
                "stdout_path": stdout_path,
            }
    return None


def _hardware_report_evidence_failure(record: dict[str, Any]) -> dict[str, str] | None:
    if record.get("name") != _HARDWARE_REPORT_COMMAND_NAME:
        return None
    if _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    stdout_records = _stdout_json_records(stdout_path)
    seen = _event_names(stdout_records)
    missing = tuple(event for event in _REQUIRED_HARDWARE_REPORT_EVENTS if event not in seen)
    if not missing:
        duplicate_failure = _hardware_summary_duplicate_failure(stdout_records, stdout_path)
        if duplicate_failure is not None:
            return duplicate_failure
        summary_failure = _hardware_summary_failure(stdout_records, stdout_path)
        if summary_failure is not None:
            return summary_failure
        lane_count_failure = _hardware_lane_count_failure(stdout_records, stdout_path)
        if lane_count_failure is not None:
            return lane_count_failure
        lane_duplicate_failure = _hardware_lane_duplicate_failure(stdout_records, stdout_path)
        if lane_duplicate_failure is not None:
            return lane_duplicate_failure
        lane_failure = _hardware_lane_failure(stdout_records, stdout_path)
        if lane_failure is not None:
            return lane_failure
        metrics_failure = _hardware_metrics_failure(stdout_records, stdout_path)
        if metrics_failure is not None:
            return metrics_failure
        return None
    return {
        "name": _HARDWARE_REPORT_COMMAND_NAME,
        "reason": "hardware_encoding_missing_reports",
        "missing": ",".join(missing),
        "stdout_path": stdout_path,
    }


def _hardware_summary_duplicate_failure(
    records: tuple[dict[str, Any], ...],
    stdout_path: str,
) -> dict[str, str] | None:
    summary_records = tuple(record for record in records if record.get("event") == "hardware_encoding_validation")
    if len(summary_records) <= 1:
        return None
    return {
        "actual": str(len(summary_records)),
        "name": _HARDWARE_REPORT_COMMAND_NAME,
        "reason": "hardware_encoding_validation_duplicate",
        "stdout_path": stdout_path,
    }


def _hardware_summary_failure(records: tuple[dict[str, Any], ...], stdout_path: str) -> dict[str, str] | None:
    for record in records:
        if record.get("event") != "hardware_encoding_validation":
            continue
        if record.get("passed") is True:
            return None
        scenario = record.get("scenario")
        return {
            "name": _HARDWARE_REPORT_COMMAND_NAME,
            "reason": "hardware_encoding_validation_failed",
            "scenario": scenario if isinstance(scenario, str) else "",
            "stdout_path": stdout_path,
        }
    return None


def _hardware_lane_failure(records: tuple[dict[str, Any], ...], stdout_path: str) -> dict[str, str] | None:
    for record in records:
        if record.get("event") != "hardware_encoding_lane":
            continue
        if record.get("passed") is True:
            continue
        lane_id = record.get("lane_id")
        return {
            "name": _HARDWARE_REPORT_COMMAND_NAME,
            "reason": "hardware_encoding_lane_failed",
            "lane_id": lane_id if isinstance(lane_id, str) else "",
            "stdout_path": stdout_path,
        }
    return None


def _hardware_lane_count_failure(records: tuple[dict[str, Any], ...], stdout_path: str) -> dict[str, str] | None:
    for record in records:
        if record.get("event") != "hardware_encoding_validation":
            continue
        scenario = record.get("scenario")
        expected = record.get("lane_count")
        if isinstance(expected, bool) or not isinstance(expected, int) or expected <= 0:
            return {
                "name": _HARDWARE_REPORT_COMMAND_NAME,
                "reason": "hardware_encoding_lane_count_mismatch",
                "scenario": scenario if isinstance(scenario, str) else "",
                "expected": str(expected),
                "actual": "0",
                "stdout_path": stdout_path,
            }
        actual = sum(
            1
            for lane_record in records
            if lane_record.get("event") == "hardware_encoding_lane"
            and lane_record.get("scenario") == scenario
        )
        if actual == expected:
            return None
        return {
            "name": _HARDWARE_REPORT_COMMAND_NAME,
            "reason": "hardware_encoding_lane_count_mismatch",
            "scenario": scenario if isinstance(scenario, str) else "",
            "expected": str(expected),
            "actual": str(actual),
            "stdout_path": stdout_path,
        }
    return None


def _hardware_lane_duplicate_failure(records: tuple[dict[str, Any], ...], stdout_path: str) -> dict[str, str] | None:
    seen: dict[tuple[str, str], int] = {}
    for record in records:
        if record.get("event") != "hardware_encoding_lane":
            continue
        scenario = record.get("scenario")
        lane_id = record.get("lane_id")
        if not isinstance(scenario, str) or not isinstance(lane_id, str):
            continue
        key = (scenario, lane_id)
        seen[key] = seen.get(key, 0) + 1

    for (scenario, lane_id), count in seen.items():
        if count > 1:
            return {
                "actual": str(count),
                "lane_id": lane_id,
                "name": _HARDWARE_REPORT_COMMAND_NAME,
                "reason": "hardware_encoding_lane_duplicate",
                "scenario": scenario,
                "stdout_path": stdout_path,
            }
    return None


def _hardware_metrics_failure(records: tuple[dict[str, Any], ...], stdout_path: str) -> dict[str, str] | None:
    metric_records = tuple(record for record in records if record.get("event") == "hardware_encoding_metrics")
    expected_scenario = _hardware_validation_scenario(records)
    if len(metric_records) > 1:
        scenario = expected_scenario
        if scenario is None:
            first_scenario = metric_records[0].get("scenario")
            scenario = first_scenario if isinstance(first_scenario, str) else ""
        return {
            "actual": str(len(metric_records)),
            "name": _HARDWARE_REPORT_COMMAND_NAME,
            "reason": "hardware_encoding_metrics_duplicate",
            "scenario": scenario,
            "stdout_path": stdout_path,
        }
    if expected_scenario is not None:
        matching_metric_records = tuple(
            record for record in metric_records if record.get("scenario") == expected_scenario
        )
        if not matching_metric_records:
            actual_scenario = ""
            for record in metric_records:
                scenario = record.get("scenario")
                if isinstance(scenario, str):
                    actual_scenario = scenario
                    break
            return {
                "actual": actual_scenario,
                "expected": expected_scenario,
                "name": _HARDWARE_REPORT_COMMAND_NAME,
                "reason": "hardware_encoding_metrics_scenario_mismatch",
                "stdout_path": stdout_path,
            }
        metric_records = matching_metric_records
    for record in metric_records:
        scenario = record.get("scenario")
        metrics = record.get("metrics")
        if not isinstance(metrics, dict):
            missing = _REQUIRED_HARDWARE_METRIC_FIELDS
        else:
            missing = tuple(field for field in _REQUIRED_HARDWARE_METRIC_FIELDS if field not in metrics)
        if not missing:
            return None
        return {
            "name": _HARDWARE_REPORT_COMMAND_NAME,
            "reason": "hardware_encoding_metrics_missing_fields",
            "scenario": scenario if isinstance(scenario, str) else "",
            "missing": ",".join(missing),
            "stdout_path": stdout_path,
        }
    return None


def _hardware_validation_scenario(records: tuple[dict[str, Any], ...]) -> str | None:
    for record in records:
        if record.get("event") != "hardware_encoding_validation":
            continue
        scenario = record.get("scenario")
        if isinstance(scenario, str) and scenario:
            return scenario
    return None


def _weak_network_matrix_evidence_failure(record: dict[str, Any]) -> dict[str, str] | None:
    if record.get("name") != _WEAK_NETWORK_MATRIX_COMMAND_NAME:
        return None
    if _validation_result_failed(record):
        return None
    stdout_path = record.get("stdout_path")
    if not isinstance(stdout_path, str) or not stdout_path:
        stdout_path = ""
    lines = _stdout_text_lines(stdout_path)
    if not any(line.startswith("dry-run only:") and "confirm" in line for line in lines):
        return _weak_network_matrix_failure("weak_network_matrix_missing_warning", stdout_path)
    profile_entries = tuple(
        line.split("=", 1)[1].strip()
        for line in lines
        if line.startswith("profile=") and line.split("=", 1)[1].strip()
    )
    duplicate_profiles = _duplicate_values(profile_entries)
    if duplicate_profiles:
        failure = _weak_network_matrix_failure("weak_network_matrix_duplicate_profiles", stdout_path)
        failure["profile"] = duplicate_profiles[0][0]
        failure["actual"] = str(duplicate_profiles[0][1])
        return failure
    profile_names = set(profile_entries)
    missing_profiles = tuple(
        profile_name
        for profile_name in _REQUIRED_WEAK_NETWORK_PROFILE_NAMES
        if profile_name not in profile_names
    )
    if missing_profiles:
        failure = _weak_network_matrix_failure("weak_network_matrix_missing_profiles", stdout_path)
        failure["missing"] = ",".join(missing_profiles)
        return failure
    unexpected_profiles = tuple(
        profile_name
        for profile_name in sorted(profile_names)
        if profile_name not in _REQUIRED_WEAK_NETWORK_PROFILE_NAMES
    )
    if unexpected_profiles:
        failure = _weak_network_matrix_failure("weak_network_matrix_unexpected_profiles", stdout_path)
        failure["unexpected"] = ",".join(unexpected_profiles)
        return failure
    apply_count = sum(
        1
        for line in lines
        if line.startswith("apply=sudo tc qdisc add dev ") and " root netem " in line
    )
    clear_count = sum(
        1
        for line in lines
        if line.startswith("clear=sudo tc qdisc del dev ") and line.endswith(" root")
    )
    expected_count = len(_REQUIRED_WEAK_NETWORK_PROFILE_NAMES)
    if apply_count != expected_count or clear_count != expected_count:
        failure = _weak_network_matrix_failure("weak_network_matrix_command_count_mismatch", stdout_path)
        failure["expected"] = str(expected_count)
        failure["actual_apply"] = str(apply_count)
        failure["actual_clear"] = str(clear_count)
        return failure
    return None


def _weak_network_matrix_failure(reason: str, stdout_path: str) -> dict[str, str]:
    return {
        "name": _WEAK_NETWORK_MATRIX_COMMAND_NAME,
        "reason": reason,
        "stdout_path": stdout_path,
    }


def _stdout_event_names(stdout_path: str) -> set[str]:
    return _event_names(_stdout_json_records(stdout_path))


def _event_names(records: tuple[dict[str, Any], ...]) -> set[str]:
    events: set[str] = set()
    for record in records:
        event = record.get("event")
        if isinstance(event, str):
            events.add(event)
    return events


def _stdout_json_records(stdout_path: str) -> tuple[dict[str, Any], ...]:
    records: list[dict[str, Any]] = []
    try:
        with Path(stdout_path).open("r", encoding="utf-8") as handle:
            for line in handle:
                if not line.strip():
                    continue
                try:
                    stdout_record = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if isinstance(stdout_record, dict):
                    records.append(stdout_record)
    except OSError:
        return ()
    return tuple(records)


def _stdout_text_lines(stdout_path: str) -> tuple[str, ...]:
    try:
        with Path(stdout_path).open("r", encoding="utf-8") as handle:
            return tuple(line.strip() for line in handle if line.strip())
    except OSError:
        return ()
