from __future__ import annotations

import ipaddress
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Dict, List
from urllib.parse import parse_qs, urlparse

import yaml

from .capacity import mbps_to_gb_per_hour, recording_mbps_for


class ConfigError(ValueError):
    """Raised when a runtime configuration violates the design contract."""


@dataclass(frozen=True)
class RuntimeConfigUpdateDecision:
    path: str
    allowed: bool
    reason: str
    restart_required: bool


class RuntimeConfigUpdatePolicy:
    def __init__(self, allowed_exact: set[str], allowed_prefixes: tuple[str, ...], dangerous_prefixes: tuple[str, ...]) -> None:
        self.allowed_exact = allowed_exact
        self.allowed_prefixes = allowed_prefixes
        self.dangerous_prefixes = dangerous_prefixes

    @classmethod
    def default(cls) -> "RuntimeConfigUpdatePolicy":
        return cls(
            allowed_exact={
                "logging.level",
                "upload.max_bandwidth_mbps",
                "upload.paused",
            },
            allowed_prefixes=("media.realtime_profiles.",),
            dangerous_prefixes=(
                "vehicle.",
                "cloud.device_cert",
                "cloud.device_key",
                "vehicle_adapter.",
                "cameras",
                "control.",
            ),
        )

    def evaluate(self, path: str, value: Any) -> RuntimeConfigUpdateDecision:
        if path in self.allowed_exact or self._matches_allowed_prefix(path):
            if not self._valid_runtime_value(path, value):
                return RuntimeConfigUpdateDecision(path, False, "runtime_update_rejected_invalid_value", False)
            return RuntimeConfigUpdateDecision(path, True, "runtime_update_allowed", False)
        if self._matches_dangerous_prefix(path):
            return RuntimeConfigUpdateDecision(path, False, "runtime_update_rejected_dangerous_field", True)
        return RuntimeConfigUpdateDecision(path, False, "runtime_update_rejected_unknown_field", True)

    def _matches_allowed_prefix(self, path: str) -> bool:
        if not path.endswith(".bitrate_kbps"):
            return False
        return any(path.startswith(prefix) for prefix in self.allowed_prefixes)

    def _matches_dangerous_prefix(self, path: str) -> bool:
        return any(path == prefix.rstrip(".") or path.startswith(prefix) for prefix in self.dangerous_prefixes)

    def _valid_runtime_value(self, path: str, value: Any) -> bool:
        if path == "logging.level":
            return str(value).lower() in {"debug", "info", "warning", "error"}
        if path == "upload.paused":
            return isinstance(value, bool)
        if path == "upload.max_bandwidth_mbps":
            return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value)) and value > 0
        if path.endswith(".bitrate_kbps"):
            return isinstance(value, int) and not isinstance(value, bool) and value > 0
        return False


@dataclass(frozen=True)
class ControlTimeoutCalibration:
    max_control_timeout_ms: int
    evidence: str


@dataclass(frozen=True)
class ControlConfig:
    rate_hz: int
    max_command_gap_ms: int
    degraded_timeout_ms: int
    control_timeout_ms: int
    deceleration_profile: List[tuple]
    estop_latch: bool
    estop_reset_requires_local_confirmation: bool
    time_sync_minimum: str
    timeout_calibration: ControlTimeoutCalibration | None = None


@dataclass(frozen=True)
class CloudConfig:
    signaling_url: str
    auth_url: str
    device_cert: str | None = None
    device_key: str | None = None


@dataclass(frozen=True)
class TurnServerConfig:
    url: str
    username: str
    credential: str | None = None
    credential_file: str | None = None
    credential_mode: str = "password"
    static_auth_secret: str | None = None
    static_auth_secret_file: str | None = None
    credential_ttl_seconds: int | None = None


@dataclass(frozen=True)
class IceConfig:
    stun_servers: List[str]
    turn_servers: List[TurnServerConfig]


@dataclass(frozen=True)
class DriverUiConfig:
    default_layout: str
    show_debug_overlay: bool


@dataclass(frozen=True)
class DriverKeyboardConfig:
    steering_left: str
    steering_right: str
    throttle: str
    brake: str
    estop: str


@dataclass(frozen=True)
class DriverGamepadConfig:
    enabled: bool
    steering_axis: int
    throttle_axis: int
    brake_axis: int
    axis_deadzone: float
    throttle_inverted: bool
    brake_inverted: bool
    estop_button: int


@dataclass(frozen=True)
class DriverControlConfig:
    rate_hz: int
    estop_hold_ms: int
    keyboard: DriverKeyboardConfig
    gamepad: DriverGamepadConfig


@dataclass(frozen=True)
class DriverConfig:
    driver_id: str
    cloud: CloudConfig
    ui: DriverUiConfig
    control: DriverControlConfig


@dataclass(frozen=True)
class MediaProfile:
    name: str
    codec: str
    encoder: str
    width: Any
    height: Any
    fps: Any
    bitrate_kbps: int
    keyframe_interval_frames: int | None = None
    segment_seconds: int | None = None


@dataclass(frozen=True)
class CameraConfig:
    camera_id: str
    enabled: bool
    device: str
    capture_width: int
    capture_height: int
    capture_fps: int
    realtime_profile: str
    record_profile: str


@dataclass(frozen=True)
class RecordingConfig:
    root_dir: str
    retention_target_hours: float
    upload_lag_policy: str | None
    min_free_gb: float
    delete_uploaded_when_below_free_gb: float
    delete_unuploaded_when_below_free_gb: bool


@dataclass(frozen=True)
class UploadConfig:
    enabled: bool
    backend: str
    max_bandwidth_mbps: float
    trigger_segments: int
    trigger_bytes_mb: float | None
    trigger_interval_seconds: int | None
    trigger_network_idle: bool
    direct_file_upload: bool
    presigned_url_refresh_margin_seconds: int
    retry_initial_seconds: int
    retry_max_seconds: int
    s3: "UploadS3Config | None" = None


@dataclass(frozen=True)
class UploadS3Config:
    endpoint_url: str
    bucket: str
    region: str
    access_key_id: str
    secret_access_key: str | None = None
    secret_access_key_file: str | None = None
    session_token: str | None = None
    session_token_file: str | None = None


@dataclass(frozen=True)
class VehicleAdapterContract:
    steering_unit: str
    throttle_unit: str
    brake_unit: str
    brake_semantics: str
    gear_values: List[str]
    heartbeat_period_ms: int
    safe_stop_supported: bool
    estop_supported: bool
    command_ack: str
    telemetry_fields: List[str]
    integration: "VehicleAdapterIntegrationConfig | None" = None


@dataclass(frozen=True)
class ChassisControlIntegrationConfig:
    source_root: str
    header_path: str
    can_common_header_path: str
    cmake_target: str
    library_output_name: str
    can_interface: str
    abi: str
    requires_cpp_bridge: bool
    library_path: str | None = None
    bridge_library_path: str | None = None


@dataclass(frozen=True)
class MinePilotCanIntegrationConfig:
    source_root: str
    can_common_header_path: str
    can_message_header_path: str
    can_db_header_path: str
    can_receiver_header_path: str
    can_sender_header_path: str
    can_db_source_path: str
    can_receiver_source_path: str
    can_sender_source_path: str


@dataclass(frozen=True)
class CanHardwareConfig:
    interface: str
    bitrate: int
    probe_timeout_seconds: int
    restart_ms: int | None = None


@dataclass(frozen=True)
class EncodingHardwareConfig:
    vaapi_render_device: str
    dri_card_device: str
    require_hardware_encoder: bool
    gstreamer_hardware_plugins: List[str]
    gstreamer_fallback_plugins: List[str]
    ffmpeg_probe_output_dir: str
    ffmpeg_binary: str
    ffprobe_binary: str
    vainfo_binary: str
    libva_drivers_path: str
    validation_duration_seconds: int


@dataclass(frozen=True)
class NetworkHardwareConfig:
    interface: str


@dataclass(frozen=True)
class HardwareConfig:
    can: CanHardwareConfig
    encoding: EncodingHardwareConfig
    network: NetworkHardwareConfig

    @property
    def preflight_devices(self) -> List[str]:
        return [self.encoding.vaapi_render_device, self.encoding.dri_card_device]


@dataclass(frozen=True)
class FieldSafetyConfig:
    commissioning_mode: str
    max_speed_kph: float
    require_can_feedback_before_control: bool
    require_local_estop_reset: bool
    require_time_sync: bool


@dataclass(frozen=True)
class VehicleAdapterIntegrationConfig:
    chassis_control: ChassisControlIntegrationConfig
    minepilot: MinePilotCanIntegrationConfig | None = None


@dataclass(frozen=True)
class CapacityPlan:
    recording_mbps: float
    upload_mbps: float
    status: str
    recording_gb_per_hour: float
    upload_gb_per_hour: float
    net_growth_gb_per_hour: float
    required_retention_gb: float

    def recalculate(
        self,
        cameras: List[CameraConfig],
        record_profiles: Dict[str, MediaProfile],
        recording: RecordingConfig,
        upload: UploadConfig,
    ) -> "CapacityPlan":
        return _derive_capacity(cameras, record_profiles, recording, upload)


@dataclass(frozen=True)
class VehicleConfig:
    vehicle_id: str
    cloud: CloudConfig
    ice: IceConfig
    control: ControlConfig
    realtime_profiles: Dict[str, MediaProfile]
    record_profiles: Dict[str, MediaProfile]
    cameras: List[CameraConfig]
    hardware: HardwareConfig
    field_safety: FieldSafetyConfig
    recording: RecordingConfig
    upload: UploadConfig
    vehicle_adapter_type: str
    vehicle_adapter_contract: VehicleAdapterContract | None
    capacity: CapacityPlan

    @property
    def enabled_cameras(self) -> List[CameraConfig]:
        return [camera for camera in self.cameras if camera.enabled]


def effective_vehicle_config_log_payload(config: VehicleConfig) -> Dict[str, Any]:
    return {
        "event": "effective_vehicle_config",
        "vehicle_id": config.vehicle_id,
        "cloud": {
            "signaling_url": config.cloud.signaling_url,
            "auth_url": config.cloud.auth_url,
            "device_cert": _configured(config.cloud.device_cert),
            "device_key": _configured(config.cloud.device_key),
        },
        "ice": {
            "stun_servers": list(config.ice.stun_servers),
            "turn_servers": [
                {
                    "url": turn.url,
                    "username": turn.username,
                    "credential": _configured(turn.credential),
                    "credential_file": _configured(turn.credential_file),
                    "credential_mode": turn.credential_mode,
                    "static_auth_secret": _configured(turn.static_auth_secret),
                    "static_auth_secret_file": _configured(turn.static_auth_secret_file),
                    "credential_ttl_seconds": turn.credential_ttl_seconds,
                }
                for turn in config.ice.turn_servers
            ],
        },
        "control": {
            "rate_hz": config.control.rate_hz,
            "max_command_gap_ms": config.control.max_command_gap_ms,
            "degraded_timeout_ms": config.control.degraded_timeout_ms,
            "control_timeout_ms": config.control.control_timeout_ms,
            "time_sync_minimum": config.control.time_sync_minimum,
            "estop_latch": config.control.estop_latch,
            "estop_reset_requires_local_confirmation": config.control.estop_reset_requires_local_confirmation,
        },
        "media": {
            "realtime_profiles": _profiles_payload(config.realtime_profiles),
            "record_profiles": _profiles_payload(config.record_profiles),
        },
        "cameras": [
            {
                "id": camera.camera_id,
                "enabled": camera.enabled,
                "device": camera.device,
                "capture_width": camera.capture_width,
                "capture_height": camera.capture_height,
                "capture_fps": camera.capture_fps,
                "realtime_profile": camera.realtime_profile,
                "record_profile": camera.record_profile,
            }
            for camera in config.cameras
        ],
        "hardware": {
            "can": {
                "interface": config.hardware.can.interface,
                "bitrate": config.hardware.can.bitrate,
                "restart_ms": config.hardware.can.restart_ms,
                "probe_timeout_seconds": config.hardware.can.probe_timeout_seconds,
            },
            "encoding": {
                "vaapi_render_device": config.hardware.encoding.vaapi_render_device,
                "dri_card_device": config.hardware.encoding.dri_card_device,
                "require_hardware_encoder": config.hardware.encoding.require_hardware_encoder,
                "gstreamer_hardware_plugins": list(config.hardware.encoding.gstreamer_hardware_plugins),
                "gstreamer_fallback_plugins": list(config.hardware.encoding.gstreamer_fallback_plugins),
                "ffmpeg_probe_output_dir": config.hardware.encoding.ffmpeg_probe_output_dir,
                "ffmpeg_binary": config.hardware.encoding.ffmpeg_binary,
                "ffprobe_binary": config.hardware.encoding.ffprobe_binary,
                "vainfo_binary": config.hardware.encoding.vainfo_binary,
                "libva_drivers_path": config.hardware.encoding.libva_drivers_path,
                "validation_duration_seconds": config.hardware.encoding.validation_duration_seconds,
            },
            "network": {
                "interface": config.hardware.network.interface,
            },
        },
        "field_safety": {
            "commissioning_mode": config.field_safety.commissioning_mode,
            "max_speed_kph": config.field_safety.max_speed_kph,
            "require_can_feedback_before_control": config.field_safety.require_can_feedback_before_control,
            "require_local_estop_reset": config.field_safety.require_local_estop_reset,
            "require_time_sync": config.field_safety.require_time_sync,
        },
        "recording": {
            "retention_target_hours": config.recording.retention_target_hours,
            "upload_lag_policy": config.recording.upload_lag_policy,
            "min_free_gb": config.recording.min_free_gb,
            "delete_uploaded_when_below_free_gb": config.recording.delete_uploaded_when_below_free_gb,
            "delete_unuploaded_when_below_free_gb": config.recording.delete_unuploaded_when_below_free_gb,
        },
        "upload": {
            "enabled": config.upload.enabled,
            "backend": config.upload.backend,
            "max_bandwidth_mbps": config.upload.max_bandwidth_mbps,
            "trigger_segments": config.upload.trigger_segments,
            "trigger_bytes_mb": config.upload.trigger_bytes_mb,
            "trigger_interval_seconds": config.upload.trigger_interval_seconds,
            "trigger_network_idle": config.upload.trigger_network_idle,
            "direct_file_upload": config.upload.direct_file_upload,
            "presigned_url_refresh_margin_seconds": config.upload.presigned_url_refresh_margin_seconds,
            "retry_initial_seconds": config.upload.retry_initial_seconds,
            "retry_max_seconds": config.upload.retry_max_seconds,
            "s3": _upload_s3_payload(config.upload.s3),
        },
        "vehicle_adapter": {
            "type": config.vehicle_adapter_type,
            "contract_configured": config.vehicle_adapter_contract is not None,
        },
        "capacity": {
            "recording_mbps": config.capacity.recording_mbps,
            "upload_mbps": config.capacity.upload_mbps,
            "status": config.capacity.status,
            "recording_gb_per_hour": config.capacity.recording_gb_per_hour,
            "upload_gb_per_hour": config.capacity.upload_gb_per_hour,
            "net_growth_gb_per_hour": config.capacity.net_growth_gb_per_hour,
            "required_retention_gb": config.capacity.required_retention_gb,
        },
    }


def _profiles_payload(profiles: Dict[str, MediaProfile]) -> Dict[str, Dict[str, Any]]:
    return {
        name: {
            "codec": profile.codec,
            "encoder": profile.encoder,
            "width": profile.width,
            "height": profile.height,
            "fps": profile.fps,
            "bitrate_kbps": profile.bitrate_kbps,
            "keyframe_interval_frames": profile.keyframe_interval_frames,
            "segment_seconds": profile.segment_seconds,
        }
        for name, profile in profiles.items()
    }


def _upload_s3_payload(config: UploadS3Config | None) -> Dict[str, Any] | None:
    if config is None:
        return None
    return {
        "endpoint_url": config.endpoint_url,
        "bucket": config.bucket,
        "region": config.region,
        "access_key_id": _configured(config.access_key_id),
        "secret_access_key": _configured(config.secret_access_key),
        "secret_access_key_file": _configured(config.secret_access_key_file),
        "session_token": _configured(config.session_token),
        "session_token_file": _configured(config.session_token_file),
    }


def _configured(value: Any) -> str:
    return "configured" if value else "not_configured"


def load_vehicle_config(path: Path | str) -> VehicleConfig:
    data = _load_config_mapping(Path(path))
    if not isinstance(data, dict):
        raise ConfigError("vehicle config must be a mapping")
    return _parse_vehicle_config(data)


def load_driver_config(path: Path | str) -> DriverConfig:
    data = _load_config_mapping(Path(path))
    if not isinstance(data, dict):
        raise ConfigError("driver config must be a mapping")
    return _parse_driver_config(data)


def _load_config_mapping(path: Path) -> Dict[str, Any]:
    if path.suffix.lower() == ".toml":
        return _load_toml_mapping(path)
    with path.open("r", encoding="utf-8") as handle:
        return yaml.safe_load(handle) or {}


def _load_toml_mapping(path: Path) -> Dict[str, Any]:
    toml_loader = _stdlib_or_installed_toml_loader()
    if toml_loader is not None:
        with path.open("rb") as handle:
            data = toml_loader(handle)
        if not isinstance(data, dict):
            raise ConfigError("TOML config must be a mapping")
        return data
    return _parse_basic_toml(path.read_text(encoding="utf-8"))


def _stdlib_or_installed_toml_loader():
    try:
        import tomllib  # type: ignore

        return tomllib.load
    except ModuleNotFoundError:
        try:
            import tomli  # type: ignore

            return tomli.load
        except ModuleNotFoundError:
            return None


def _parse_basic_toml(text: str) -> Dict[str, Any]:
    data: Dict[str, Any] = {}
    current: Dict[str, Any] = data
    for line_number, raw_line in enumerate(text.splitlines(), start=1):
        line = _strip_toml_comment(raw_line).strip()
        if not line:
            continue
        if line.startswith("[[") and line.endswith("]]"):
            current = _toml_array_table(data, line[2:-2].strip(), line_number)
            continue
        if line.startswith("[") and line.endswith("]"):
            current = _toml_table(data, line[1:-1].strip(), line_number)
            continue
        if "=" not in line:
            raise ConfigError(f"TOML line {line_number} must be a table header or key/value pair")
        key, raw_value = line.split("=", 1)
        _toml_set(current, key.strip(), _parse_toml_value(raw_value.strip(), line_number), line_number)
    return data


def _strip_toml_comment(line: str) -> str:
    in_string = False
    quote_char = ""
    escaped = False
    for index, char in enumerate(line):
        if in_string:
            if escaped:
                escaped = False
                continue
            if char == "\\" and quote_char == '"':
                escaped = True
                continue
            if char == quote_char:
                in_string = False
            continue
        if char in {'"', "'"}:
            in_string = True
            quote_char = char
            continue
        if char == "#":
            return line[:index]
    return line


def _toml_table(data: Dict[str, Any], path: str, line_number: int) -> Dict[str, Any]:
    current = data
    for part in _toml_path_parts(path, line_number):
        next_value = current.setdefault(part, {})
        if not isinstance(next_value, dict):
            raise ConfigError(f"TOML table {path} conflicts with existing value on line {line_number}")
        current = next_value
    return current


def _toml_array_table(data: Dict[str, Any], path: str, line_number: int) -> Dict[str, Any]:
    parts = _toml_path_parts(path, line_number)
    current = data
    for part in parts[:-1]:
        next_value = current.setdefault(part, {})
        if not isinstance(next_value, dict):
            raise ConfigError(f"TOML table {path} conflicts with existing value on line {line_number}")
        current = next_value
    items = current.setdefault(parts[-1], [])
    if not isinstance(items, list):
        raise ConfigError(f"TOML array table {path} conflicts with existing value on line {line_number}")
    item: Dict[str, Any] = {}
    items.append(item)
    return item


def _toml_set(current: Dict[str, Any], key: str, value: Any, line_number: int) -> None:
    parts = _toml_path_parts(key, line_number)
    target = current
    for part in parts[:-1]:
        next_value = target.setdefault(part, {})
        if not isinstance(next_value, dict):
            raise ConfigError(f"TOML key {key} conflicts with existing value on line {line_number}")
        target = next_value
    if parts[-1] in target:
        raise ConfigError(f"TOML key {key} is duplicated on line {line_number}")
    target[parts[-1]] = value


def _toml_path_parts(path: str, line_number: int) -> List[str]:
    parts = [part.strip() for part in path.split(".")]
    if any(not part for part in parts):
        raise ConfigError(f"TOML dotted path is invalid on line {line_number}")
    return parts


def _parse_toml_value(value: str, line_number: int) -> Any:
    if value.startswith('"') and value.endswith('"'):
        return _parse_toml_double_quoted(value, line_number)
    if value.startswith("'") and value.endswith("'"):
        return value[1:-1]
    if value == "true":
        return True
    if value == "false":
        return False
    if value.startswith("[") and value.endswith("]"):
        return [_parse_toml_value(part.strip(), line_number) for part in _split_toml_array(value[1:-1]) if part.strip()]
    try:
        if any(char in value for char in (".", "e", "E")):
            return float(value)
        return int(value, 10)
    except ValueError as exc:
        raise ConfigError(f"TOML value {value!r} is not supported on line {line_number}") from exc


def _parse_toml_double_quoted(value: str, line_number: int) -> str:
    try:
        import json

        parsed = json.loads(value)
    except ValueError as exc:
        raise ConfigError(f"TOML string is invalid on line {line_number}") from exc
    if not isinstance(parsed, str):
        raise ConfigError(f"TOML string is invalid on line {line_number}")
    return parsed


def _split_toml_array(value: str) -> List[str]:
    parts: List[str] = []
    start = 0
    in_string = False
    quote_char = ""
    escaped = False
    for index, char in enumerate(value):
        if in_string:
            if escaped:
                escaped = False
                continue
            if char == "\\" and quote_char == '"':
                escaped = True
                continue
            if char == quote_char:
                in_string = False
            continue
        if char in {'"', "'"}:
            in_string = True
            quote_char = char
            continue
        if char == ",":
            parts.append(value[start:index])
            start = index + 1
    parts.append(value[start:])
    return parts


def _parse_vehicle_config(data: Dict[str, Any]) -> VehicleConfig:
    vehicle = _mapping(data, "vehicle")
    vehicle_id = _required_str(vehicle, "id", "vehicle.id")

    cloud = _parse_cloud(_mapping(data, "cloud"), require_vehicle_identity=True)
    ice = _parse_ice(_mapping(data, "ice"))
    control = _parse_control(_mapping(data, "control"))
    media = _mapping(data, "media")
    realtime_profiles = _parse_profiles(_mapping(media, "realtime_profiles"))
    record_profiles = _parse_profiles(_mapping(media, "record_profiles"), record=True)
    cameras = _parse_cameras(data.get("cameras", []), realtime_profiles, record_profiles)
    hardware = _parse_hardware(data.get("hardware", {}))
    field_safety = _parse_field_safety(data.get("field_safety", {}), control)
    recording = _parse_recording(_mapping(data, "recording"))
    upload = _parse_upload(_mapping(data, "upload"))
    adapter = _mapping(data, "vehicle_adapter")
    adapter_type, adapter_contract = _parse_vehicle_adapter(
        adapter,
        require_integration=control.timeout_calibration is not None,
    )
    if adapter_type != "mock" and control.timeout_calibration is None:
        raise ConfigError("control.timeout_calibration is required for real vehicle adapters")
    if (
        adapter_contract is not None
        and adapter_contract.integration is not None
        and hardware.can.interface != adapter_contract.integration.chassis_control.can_interface
    ):
        raise ConfigError(
            "hardware.can.interface must match vehicle_adapter.integration.chassis_control.can_interface"
        )

    capacity = _derive_capacity(cameras, record_profiles, recording, upload)

    return VehicleConfig(
        vehicle_id=vehicle_id,
        cloud=cloud,
        ice=ice,
        control=control,
        realtime_profiles=realtime_profiles,
        record_profiles=record_profiles,
        cameras=cameras,
        hardware=hardware,
        field_safety=field_safety,
        recording=recording,
        upload=upload,
        vehicle_adapter_type=adapter_type,
        vehicle_adapter_contract=adapter_contract,
        capacity=capacity,
    )


def _parse_driver_config(data: Dict[str, Any]) -> DriverConfig:
    driver = _mapping(data, "driver")
    driver_id = _required_str(driver, "id", "driver.id")
    cloud = _parse_cloud(_mapping(data, "cloud"))
    ui = _parse_driver_ui(_mapping(data, "ui"))
    control = _parse_driver_control(_mapping(data, "control"))
    return DriverConfig(driver_id=driver_id, cloud=cloud, ui=ui, control=control)


def _parse_driver_ui(raw: Dict[str, Any]) -> DriverUiConfig:
    layout = _required_str(raw, "default_layout", "ui.default_layout")
    if layout not in {"single", "grid_1", "grid_2", "grid_4"}:
        raise ConfigError("ui.default_layout must be single, grid_1, grid_2, or grid_4")
    return DriverUiConfig(
        default_layout=layout,
        show_debug_overlay=_optional_bool(raw, "show_debug_overlay", "ui.show_debug_overlay", default=False),
    )


def _parse_driver_control(raw: Dict[str, Any]) -> DriverControlConfig:
    rate_hz = _positive_int(raw, "rate_hz", "control.rate_hz")
    estop_hold_ms = _positive_int(raw, "estop_hold_ms", "control.estop_hold_ms")
    keyboard = _mapping(raw, "keyboard")
    parsed_keyboard = DriverKeyboardConfig(
        steering_left=_required_key_name(keyboard, "steering_left", "control.keyboard.steering_left"),
        steering_right=_required_key_name(keyboard, "steering_right", "control.keyboard.steering_right"),
        throttle=_required_key_name(keyboard, "throttle", "control.keyboard.throttle"),
        brake=_required_key_name(keyboard, "brake", "control.keyboard.brake"),
        estop=_required_key_name(keyboard, "estop", "control.keyboard.estop"),
    )
    bindings = [
        parsed_keyboard.steering_left,
        parsed_keyboard.steering_right,
        parsed_keyboard.throttle,
        parsed_keyboard.brake,
        parsed_keyboard.estop,
    ]
    if len(set(bindings)) != len(bindings):
        raise ConfigError("keyboard bindings must be unique")
    return DriverControlConfig(
        rate_hz=rate_hz,
        estop_hold_ms=estop_hold_ms,
        keyboard=parsed_keyboard,
        gamepad=_parse_driver_gamepad(raw.get("gamepad")),
    )


def _parse_driver_gamepad(raw: Any) -> DriverGamepadConfig:
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise ConfigError("control.gamepad must be configured as a mapping")
    axis_deadzone = _non_negative_finite_number(raw.get("axis_deadzone", 0.05), "control.gamepad.axis_deadzone")
    if axis_deadzone >= 1.0:
        raise ConfigError("control.gamepad.axis_deadzone must be in [0, 1)")
    return DriverGamepadConfig(
        enabled=_optional_bool(raw, "enabled", "control.gamepad.enabled", default=False),
        steering_axis=_positive_or_zero_int(
            {"steering_axis": raw.get("steering_axis", 0)},
            "steering_axis",
            "control.gamepad.steering_axis",
        ),
        throttle_axis=_positive_or_zero_int(
            {"throttle_axis": raw.get("throttle_axis", 2)},
            "throttle_axis",
            "control.gamepad.throttle_axis",
        ),
        brake_axis=_positive_or_zero_int(
            {"brake_axis": raw.get("brake_axis", 5)},
            "brake_axis",
            "control.gamepad.brake_axis",
        ),
        axis_deadzone=axis_deadzone,
        throttle_inverted=_optional_bool(raw, "throttle_inverted", "control.gamepad.throttle_inverted", default=True),
        brake_inverted=_optional_bool(raw, "brake_inverted", "control.gamepad.brake_inverted", default=True),
        estop_button=_positive_or_zero_int(
            {"estop_button": raw.get("estop_button", 0)},
            "estop_button",
            "control.gamepad.estop_button",
        ),
    )


def _parse_control(raw: Dict[str, Any]) -> ControlConfig:
    rate_hz = _positive_int(raw, "rate_hz", "control.rate_hz")
    max_gap = _positive_int(raw, "max_command_gap_ms", "control.max_command_gap_ms")
    degraded = _positive_int(raw, "degraded_timeout_ms", "control.degraded_timeout_ms")
    timeout = _positive_int(raw, "control_timeout_ms", "control.control_timeout_ms")
    period_ms = 1000.0 / rate_hz

    if not (max_gap < degraded < timeout):
        raise ConfigError("max_command_gap_ms < degraded_timeout_ms < control_timeout_ms is required")
    if max_gap <= period_ms:
        raise ConfigError("max_command_gap_ms must be greater than the control command period")

    timeout_action = _mapping(raw, "timeout_action")
    profile = timeout_action.get("deceleration_profile")
    if not isinstance(profile, list) or len(profile) < 2:
        raise ConfigError("deceleration_profile must contain at least two braking stages")

    parsed_profile = []
    previous_after = -1
    for index, stage in enumerate(profile):
        if not isinstance(stage, dict):
            raise ConfigError("deceleration_profile stages must be mappings")
        after_ms = _positive_or_zero_int(stage, "after_ms", f"deceleration_profile[{index}].after_ms")
        brake = stage.get("brake")
        if brake is None:
            raise ConfigError(f"deceleration_profile[{index}].brake is required")
        parsed_brake = _parse_deceleration_brake(brake, f"deceleration_profile[{index}].brake")
        if after_ms <= previous_after:
            raise ConfigError("deceleration_profile after_ms values must strictly increase")
        parsed_profile.append((after_ms, parsed_brake))
        previous_after = after_ms

    first_brake = parsed_profile[0][1]
    if first_brake == "vehicle_defined_max_safe" or float(first_brake) >= 1.0:
        raise ConfigError("deceleration_profile must not start with full braking")

    estop = raw.get("estop")
    if not isinstance(estop, dict):
        raise ConfigError("control.estop must be configured")
    estop_latch = _required_bool(estop, "latch", "control.estop.latch")
    if not estop_latch:
        raise ConfigError("control.estop.latch must be true")
    reset_requires_local_confirmation = _required_bool(
        estop,
        "reset_requires_local_confirmation",
        "control.estop.reset_requires_local_confirmation",
    )
    if not reset_requires_local_confirmation:
        raise ConfigError("control.estop.reset_requires_local_confirmation must be true")

    time_sync = raw.get("time_sync")
    if not isinstance(time_sync, dict):
        raise ConfigError("control.time_sync.minimum is required")
    time_sync_minimum = _required_str(time_sync, "minimum", "control.time_sync.minimum")
    if time_sync_minimum not in {"ntp", "ptp"}:
        raise ConfigError("control.time_sync.minimum must be ntp or ptp")

    timeout_calibration = None
    if "timeout_calibration" in raw:
        calibration = _mapping(raw, "timeout_calibration")
        max_control_timeout_ms = _positive_int(
            calibration,
            "max_control_timeout_ms",
            "control.timeout_calibration.max_control_timeout_ms",
        )
        if timeout > max_control_timeout_ms:
            raise ConfigError("control.control_timeout_ms exceeds calibrated maximum")
        timeout_calibration = ControlTimeoutCalibration(
            max_control_timeout_ms=max_control_timeout_ms,
            evidence=_required_str(calibration, "evidence", "control.timeout_calibration.evidence"),
        )

    return ControlConfig(
        rate_hz=rate_hz,
        max_command_gap_ms=max_gap,
        degraded_timeout_ms=degraded,
        control_timeout_ms=timeout,
        deceleration_profile=parsed_profile,
        estop_latch=estop_latch,
        estop_reset_requires_local_confirmation=reset_requires_local_confirmation,
        time_sync_minimum=time_sync_minimum,
        timeout_calibration=timeout_calibration,
    )


def _parse_cloud(raw: Dict[str, Any], require_vehicle_identity: bool = False) -> CloudConfig:
    signaling_url = _required_url(raw, "signaling_url", "cloud.signaling_url", {"ws", "wss"})
    auth_url = _required_url(raw, "auth_url", "cloud.auth_url", {"http", "https"})
    _require_tls_for_non_loopback(signaling_url, "cloud.signaling_url", {"wss"})
    _require_tls_for_non_loopback(auth_url, "cloud.auth_url", {"https"})
    device_cert = _optional_non_empty_string(raw, "device_cert", "cloud.device_cert")
    device_key = _optional_non_empty_string(raw, "device_key", "cloud.device_key")
    if (device_cert is None) != (device_key is None):
        raise ConfigError("cloud.device_cert and cloud.device_key must be configured together")
    if require_vehicle_identity and device_cert is None and _has_non_loopback_host(signaling_url, auth_url):
        raise ConfigError("cloud.device_cert and cloud.device_key are required for non-loopback vehicle cloud")
    if device_cert is not None and not Path(device_cert).exists():
        raise ConfigError("cloud.device_cert must exist when configured")
    if device_key is not None and not Path(device_key).exists():
        raise ConfigError("cloud.device_key must exist when configured")
    return CloudConfig(
        signaling_url=signaling_url,
        auth_url=auth_url,
        device_cert=device_cert,
        device_key=device_key,
    )


def _parse_ice(raw: Dict[str, Any]) -> IceConfig:
    stun_servers = raw.get("stun_servers", [])
    if not isinstance(stun_servers, list) or not stun_servers:
        raise ConfigError("ice.stun_servers must contain at least one STUN URL")
    parsed_stun = []
    for index, value in enumerate(stun_servers):
        parsed_stun.append(_validate_ice_url(value, f"ice.stun_servers[{index}]", {"stun", "stuns"}))

    turn_servers = raw.get("turn_servers", [])
    if not isinstance(turn_servers, list):
        raise ConfigError("ice.turn_servers must be a list")
    parsed_turn = []
    for index, value in enumerate(turn_servers):
        if not isinstance(value, dict):
            raise ConfigError(f"ice.turn_servers[{index}] must be a mapping")
        url = _validate_ice_url(value.get("url"), f"ice.turn_servers[{index}].url", {"turn", "turns"})
        query = parse_qs(urlparse(url).query)
        transport = query.get("transport", ["udp"])[0].lower()
        if transport != "udp":
            raise ConfigError("TURN URL must use udp transport")
        username = _required_str(value, "username", f"ice.turn_servers[{index}].username")
        credential_mode = _optional_non_empty_string(
            value,
            "credential_mode",
            f"ice.turn_servers[{index}].credential_mode",
            default="password",
        )
        if credential_mode not in {"password", "turn_rest"}:
            raise ConfigError(f"ice.turn_servers[{index}].credential_mode must be password or turn_rest")
        credential = _optional_non_empty_string(value, "credential", f"ice.turn_servers[{index}].credential")
        credential_file = _optional_non_empty_string(
            value,
            "credential_file",
            f"ice.turn_servers[{index}].credential_file",
        )
        static_auth_secret = _optional_non_empty_string(
            value,
            "static_auth_secret",
            f"ice.turn_servers[{index}].static_auth_secret",
        )
        static_auth_secret_file = _optional_non_empty_string(
            value,
            "static_auth_secret_file",
            f"ice.turn_servers[{index}].static_auth_secret_file",
        )
        credential_ttl_seconds = value.get("credential_ttl_seconds")
        if credential_mode == "password" and credential is None and credential_file is None:
            raise ConfigError(f"ice.turn_servers[{index}] must configure credential or credential_file")
        if credential_mode == "turn_rest":
            if static_auth_secret is None and static_auth_secret_file is None:
                raise ConfigError(f"ice.turn_servers[{index}] must configure static_auth_secret or static_auth_secret_file")
            if not isinstance(credential_ttl_seconds, int) or isinstance(credential_ttl_seconds, bool) or credential_ttl_seconds <= 0:
                raise ConfigError(f"ice.turn_servers[{index}].credential_ttl_seconds must be positive")
        if credential_file is not None and not Path(str(credential_file)).exists():
            raise ConfigError(f"ice.turn_servers[{index}].credential_file must exist when configured")
        if static_auth_secret_file is not None and not Path(str(static_auth_secret_file)).exists():
            raise ConfigError(f"ice.turn_servers[{index}].static_auth_secret_file must exist when configured")
        parsed_turn.append(
            TurnServerConfig(
                url=url,
                username=username,
                credential=credential,
                credential_file=credential_file,
                credential_mode=credential_mode,
                static_auth_secret=static_auth_secret,
                static_auth_secret_file=static_auth_secret_file,
                credential_ttl_seconds=credential_ttl_seconds if credential_mode == "turn_rest" else None,
            )
        )
    return IceConfig(stun_servers=parsed_stun, turn_servers=parsed_turn)


def _parse_profiles(raw: Dict[str, Any], record: bool = False) -> Dict[str, MediaProfile]:
    profiles: Dict[str, MediaProfile] = {}
    for name, profile in raw.items():
        if not isinstance(profile, dict):
            raise ConfigError(f"media profile {name} must be a mapping")
        bitrate = _positive_int(profile, "bitrate_kbps", f"{name}.bitrate_kbps")
        fps = profile.get("fps")
        if not record or fps != "source":
            _positive_number(fps, f"{name}.fps")
        width = profile.get("width")
        height = profile.get("height")
        if not record or width != "source":
            _positive_number(width, f"{name}.width")
        if not record or height != "source":
            _positive_number(height, f"{name}.height")
        segment_seconds = None
        keyframe_interval_frames = None
        if not record:
            keyframe_interval_frames = _positive_int(
                {"keyframe_interval_frames": profile.get("keyframe_interval_frames", 30)},
                "keyframe_interval_frames",
                f"{name}.keyframe_interval_frames",
            )
        if record:
            segment_seconds = _positive_int(profile, "segment_seconds", f"{name}.segment_seconds")
        profiles[name] = MediaProfile(
            name=name,
            codec=_required_str(profile, "codec", f"{name}.codec"),
            encoder=_required_str(profile, "encoder", f"{name}.encoder"),
            width=width,
            height=height,
            fps=fps,
            bitrate_kbps=bitrate,
            keyframe_interval_frames=keyframe_interval_frames,
            segment_seconds=segment_seconds,
        )
    if not profiles:
        raise ConfigError("at least one media profile is required")
    return profiles


def _parse_cameras(
    raw: Any,
    realtime_profiles: Dict[str, MediaProfile],
    record_profiles: Dict[str, MediaProfile],
) -> List[CameraConfig]:
    if not isinstance(raw, list):
        raise ConfigError("cameras must be a list")
    cameras: List[CameraConfig] = []
    seen = set()
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise ConfigError(f"cameras[{index}] must be a mapping")
        camera_id = _required_str(item, "id", f"cameras[{index}].id")
        if camera_id in seen:
            raise ConfigError(f"duplicate camera id: {camera_id}")
        seen.add(camera_id)
        realtime_profile = _required_str(item, "realtime_profile", f"cameras[{index}].realtime_profile")
        record_profile = _required_str(item, "record_profile", f"cameras[{index}].record_profile")
        if realtime_profile not in realtime_profiles:
            raise ConfigError(f"unknown realtime_profile {realtime_profile}")
        if record_profile not in record_profiles:
            raise ConfigError(f"unknown record_profile {record_profile}")
        cameras.append(
            CameraConfig(
                camera_id=camera_id,
                enabled=_optional_bool(item, "enabled", f"cameras[{index}].enabled", default=True),
                device=_required_str(item, "device", f"cameras[{index}].device"),
                capture_width=_positive_int(item, "capture_width", f"cameras[{index}].capture_width"),
                capture_height=_positive_int(item, "capture_height", f"cameras[{index}].capture_height"),
                capture_fps=_positive_int(item, "capture_fps", f"cameras[{index}].capture_fps"),
                realtime_profile=realtime_profile,
                record_profile=record_profile,
            )
        )
    if not any(camera.enabled for camera in cameras):
        raise ConfigError("at least one enabled camera is required")
    return cameras


def _parse_hardware(raw: Any) -> HardwareConfig:
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise ConfigError("hardware must be configured as a mapping")
    can = raw.get("can", {})
    if can is None:
        can = {}
    if not isinstance(can, dict):
        raise ConfigError("hardware.can must be configured as a mapping")
    encoding = raw.get("encoding", {})
    if encoding is None:
        encoding = {}
    if not isinstance(encoding, dict):
        raise ConfigError("hardware.encoding must be configured as a mapping")
    network = raw.get("network", {})
    if network is None:
        network = {}
    if not isinstance(network, dict):
        raise ConfigError("hardware.network must be configured as a mapping")

    restart_ms = can.get("restart_ms")
    if restart_ms is not None:
        restart_ms = _positive_or_zero_int(can, "restart_ms", "hardware.can.restart_ms")
    return HardwareConfig(
        can=CanHardwareConfig(
            interface=_optional_non_empty_string(can, "interface", "hardware.can.interface", default="can0"),
            bitrate=_positive_int(
                {"bitrate": can.get("bitrate", 500000)},
                "bitrate",
                "hardware.can.bitrate",
            ),
            probe_timeout_seconds=_positive_int(
                {"probe_timeout_seconds": can.get("probe_timeout_seconds", 3)},
                "probe_timeout_seconds",
                "hardware.can.probe_timeout_seconds",
            ),
            restart_ms=restart_ms,
        ),
        encoding=EncodingHardwareConfig(
            vaapi_render_device=_optional_non_empty_string(
                encoding,
                "vaapi_render_device",
                "hardware.encoding.vaapi_render_device",
                default="/dev/dri/renderD128",
            ),
            dri_card_device=_optional_non_empty_string(
                encoding,
                "dri_card_device",
                "hardware.encoding.dri_card_device",
                default="/dev/dri/card1",
            ),
            require_hardware_encoder=_optional_bool(
                encoding,
                "require_hardware_encoder",
                "hardware.encoding.require_hardware_encoder",
                default=True,
            ),
            gstreamer_hardware_plugins=_optional_str_list(
                encoding,
                "gstreamer_hardware_plugins",
                "hardware.encoding.gstreamer_hardware_plugins",
                ("vaapih264enc", "qsvh264enc", "vah264enc", "nvh264enc"),
            ),
            gstreamer_fallback_plugins=_optional_str_list(
                encoding,
                "gstreamer_fallback_plugins",
                "hardware.encoding.gstreamer_fallback_plugins",
                ("x264enc",),
            ),
            ffmpeg_probe_output_dir=_optional_non_empty_string(
                encoding,
                "ffmpeg_probe_output_dir",
                "hardware.encoding.ffmpeg_probe_output_dir",
                default="/tmp/mine-teleop-vaapi",
            ),
            ffmpeg_binary=_optional_non_empty_string(
                encoding,
                "ffmpeg_binary",
                "hardware.encoding.ffmpeg_binary",
                default="ffmpeg",
            ),
            ffprobe_binary=_optional_non_empty_string(
                encoding,
                "ffprobe_binary",
                "hardware.encoding.ffprobe_binary",
                default="ffprobe",
            ),
            vainfo_binary=_optional_non_empty_string(
                encoding,
                "vainfo_binary",
                "hardware.encoding.vainfo_binary",
                default="vainfo",
            ),
            libva_drivers_path=_optional_non_empty_string(
                encoding,
                "libva_drivers_path",
                "hardware.encoding.libva_drivers_path",
                default="/usr/lib/x86_64-linux-gnu/dri",
            ),
            validation_duration_seconds=_positive_int(
                {"validation_duration_seconds": encoding.get("validation_duration_seconds", 5)},
                "validation_duration_seconds",
                "hardware.encoding.validation_duration_seconds",
            ),
        ),
        network=NetworkHardwareConfig(
            interface=_optional_non_empty_string(network, "interface", "hardware.network.interface", default="wwan0"),
        ),
    )


def _parse_field_safety(raw: Any, control: ControlConfig) -> FieldSafetyConfig:
    if raw is None:
        raw = {}
    if not isinstance(raw, dict):
        raise ConfigError("field_safety must be configured as a mapping")
    commissioning_mode = _optional_non_empty_string(
        raw,
        "commissioning_mode",
        "field_safety.commissioning_mode",
        default="bench",
    )
    if commissioning_mode not in {"bench", "closed_site", "production"}:
        raise ConfigError("field_safety.commissioning_mode must be bench, closed_site, or production")
    require_local_estop_reset = _optional_bool(
        raw,
        "require_local_estop_reset",
        "field_safety.require_local_estop_reset",
        default=True,
    )
    if require_local_estop_reset and not control.estop_reset_requires_local_confirmation:
        raise ConfigError("field_safety.require_local_estop_reset requires control.estop.reset_requires_local_confirmation")
    require_time_sync = _optional_bool(
        raw,
        "require_time_sync",
        "field_safety.require_time_sync",
        default=True,
    )
    if require_time_sync and control.time_sync_minimum not in {"ntp", "ptp"}:
        raise ConfigError("field_safety.require_time_sync requires control.time_sync.minimum")
    return FieldSafetyConfig(
        commissioning_mode=commissioning_mode,
        max_speed_kph=_positive_finite_number(raw.get("max_speed_kph", 40), "field_safety.max_speed_kph"),
        require_can_feedback_before_control=_optional_bool(
            raw,
            "require_can_feedback_before_control",
            "field_safety.require_can_feedback_before_control",
            default=True,
        ),
        require_local_estop_reset=require_local_estop_reset,
        require_time_sync=require_time_sync,
    )


def _parse_recording(raw: Dict[str, Any]) -> RecordingConfig:
    return RecordingConfig(
        root_dir=_optional_non_empty_string(
            raw,
            "root_dir",
            "recording.root_dir",
            default="/var/lib/mine-teleop/recordings",
        ),
        retention_target_hours=_non_negative_finite_number(
            raw.get("retention_target_hours", 0),
            "recording.retention_target_hours",
        ),
        upload_lag_policy=_optional_non_empty_string(raw, "upload_lag_policy", "recording.upload_lag_policy"),
        min_free_gb=_non_negative_finite_number(raw.get("min_free_gb", 0), "recording.min_free_gb"),
        delete_uploaded_when_below_free_gb=_non_negative_finite_number(
            raw.get("delete_uploaded_when_below_free_gb", 0),
            "recording.delete_uploaded_when_below_free_gb",
        ),
        delete_unuploaded_when_below_free_gb=_optional_bool(
            raw,
            "delete_unuploaded_when_below_free_gb",
            "recording.delete_unuploaded_when_below_free_gb",
            default=False,
        ),
    )


def _parse_upload(raw: Dict[str, Any]) -> UploadConfig:
    trigger_bytes_mb = raw.get("trigger_bytes_mb")
    trigger_interval_seconds = raw.get("trigger_interval_seconds")
    backend = _optional_non_empty_string(raw, "backend", "upload.backend", default="local_archive")
    if backend not in {"local_archive", "s3"}:
        raise ConfigError("upload.backend must be local_archive or s3")
    max_bandwidth_mbps = _positive_finite_number(
        raw.get("max_bandwidth_mbps"),
        "upload.max_bandwidth_mbps",
    )
    if trigger_bytes_mb is not None:
        _positive_number(trigger_bytes_mb, "upload.trigger_bytes_mb")
    if trigger_interval_seconds is not None:
        _positive_int(raw, "trigger_interval_seconds", "upload.trigger_interval_seconds")
    refresh_margin_seconds = (
        _positive_int(raw, "presigned_url_refresh_margin_seconds", "upload.presigned_url_refresh_margin_seconds")
        if "presigned_url_refresh_margin_seconds" in raw
        else 300
    )
    retry_initial_seconds = (
        _positive_int(raw, "retry_initial_seconds", "upload.retry_initial_seconds")
        if "retry_initial_seconds" in raw
        else 10
    )
    retry_max_seconds = (
        _positive_int(raw, "retry_max_seconds", "upload.retry_max_seconds")
        if "retry_max_seconds" in raw
        else 600
    )
    if retry_initial_seconds > retry_max_seconds:
        raise ConfigError("upload.retry_initial_seconds must be less than or equal to upload.retry_max_seconds")
    direct_file_upload = _optional_bool(raw, "direct_file_upload", "upload.direct_file_upload", default=True)
    if not direct_file_upload:
        raise ConfigError("upload.direct_file_upload=false is not supported; packaged uploads are not implemented")
    return UploadConfig(
        enabled=_optional_bool(raw, "enabled", "upload.enabled", default=True),
        backend=backend,
        max_bandwidth_mbps=max_bandwidth_mbps,
        trigger_segments=_positive_int(raw, "trigger_segments", "upload.trigger_segments")
        if "trigger_segments" in raw
        else 1,
        trigger_bytes_mb=float(trigger_bytes_mb) if trigger_bytes_mb is not None else None,
        trigger_interval_seconds=int(trigger_interval_seconds) if trigger_interval_seconds is not None else None,
        trigger_network_idle=_optional_bool(
            raw,
            "trigger_network_idle",
            "upload.trigger_network_idle",
            default=False,
        ),
        direct_file_upload=direct_file_upload,
        presigned_url_refresh_margin_seconds=refresh_margin_seconds,
        retry_initial_seconds=retry_initial_seconds,
        retry_max_seconds=retry_max_seconds,
        s3=_parse_upload_s3(raw, backend),
    )


def _parse_upload_s3(raw: Dict[str, Any], backend: str) -> UploadS3Config | None:
    if backend != "s3":
        return None
    s3 = raw.get("s3")
    if not isinstance(s3, dict):
        raise ConfigError("upload.s3 is required for s3 backend")
    endpoint_url = _required_url(s3, "endpoint_url", "upload.s3.endpoint_url", {"http", "https"})
    _require_tls_for_non_loopback(endpoint_url, "upload.s3.endpoint_url", {"https"})
    secret_access_key = _optional_non_empty_str(s3, "secret_access_key", "upload.s3.secret_access_key")
    secret_access_key_file = _optional_existing_file(
        s3,
        "secret_access_key_file",
        "upload.s3.secret_access_key_file",
    )
    if (secret_access_key is None) == (secret_access_key_file is None):
        raise ConfigError(
            "exactly one of upload.s3.secret_access_key or upload.s3.secret_access_key_file is required"
        )
    session_token = _optional_non_empty_str(s3, "session_token", "upload.s3.session_token")
    session_token_file = _optional_existing_file(
        s3,
        "session_token_file",
        "upload.s3.session_token_file",
    )
    if session_token is not None and session_token_file is not None:
        raise ConfigError("configure only one of upload.s3.session_token or upload.s3.session_token_file")
    return UploadS3Config(
        endpoint_url=endpoint_url,
        bucket=_required_safe_bucket(s3, "bucket", "upload.s3.bucket"),
        region=_required_str(s3, "region", "upload.s3.region"),
        access_key_id=_required_str(s3, "access_key_id", "upload.s3.access_key_id"),
        secret_access_key=secret_access_key,
        secret_access_key_file=secret_access_key_file,
        session_token=session_token,
        session_token_file=session_token_file,
    )


def _parse_vehicle_adapter(
    raw: Dict[str, Any],
    *,
    require_integration: bool = True,
) -> tuple[str, VehicleAdapterContract | None]:
    adapter_type = _required_str(raw, "type", "vehicle_adapter.type")
    if adapter_type not in {"mock", "can", "dynamic_library"}:
        raise ConfigError("vehicle_adapter.type must be mock, can, or dynamic_library")
    if adapter_type == "mock":
        return adapter_type, None

    contract = raw.get("contract")
    if not isinstance(contract, dict):
        raise ConfigError("vehicle_adapter.contract is required before enabling a real adapter")
    telemetry_fields = _required_str_list(
        contract,
        "telemetry_fields",
        "vehicle_adapter.contract.telemetry_fields",
    )
    required_telemetry = {
        "speed_mps",
        "gear",
        "steering_feedback",
        "throttle_feedback",
        "brake_feedback",
        "estop",
    }
    missing_telemetry = sorted(required_telemetry - set(telemetry_fields))
    if missing_telemetry:
        raise ConfigError(
            "vehicle_adapter.contract.telemetry_fields must include " + ", ".join(missing_telemetry)
        )
    gear_values = _required_str_list(contract, "gear_values", "vehicle_adapter.contract.gear_values")
    if not {"P", "R", "N", "D"}.issubset(set(gear_values)):
        raise ConfigError("vehicle_adapter.contract.gear_values must include P, R, N, and D")
    safe_stop_supported = _required_bool(
        contract,
        "safe_stop_supported",
        "vehicle_adapter.contract.safe_stop_supported",
    )
    if not safe_stop_supported:
        raise ConfigError("vehicle_adapter.contract.safe_stop_supported must be true")
    estop_supported = _required_bool(
        contract,
        "estop_supported",
        "vehicle_adapter.contract.estop_supported",
    )
    if not estop_supported:
        raise ConfigError("vehicle_adapter.contract.estop_supported must be true")
    command_ack = _required_str(contract, "command_ack", "vehicle_adapter.contract.command_ack")
    if command_ack not in {"required", "telemetry_feedback"}:
        raise ConfigError("vehicle_adapter.contract.command_ack must be required or telemetry_feedback")
    integration = _parse_vehicle_adapter_integration(adapter_type, raw, require_integration=require_integration)
    return (
        adapter_type,
        VehicleAdapterContract(
            steering_unit=_required_str(contract, "steering_unit", "vehicle_adapter.contract.steering_unit"),
            throttle_unit=_required_str(contract, "throttle_unit", "vehicle_adapter.contract.throttle_unit"),
            brake_unit=_required_str(contract, "brake_unit", "vehicle_adapter.contract.brake_unit"),
            brake_semantics=_required_str(
                contract,
                "brake_semantics",
                "vehicle_adapter.contract.brake_semantics",
            ),
            gear_values=gear_values,
            heartbeat_period_ms=_positive_int(
                contract,
                "heartbeat_period_ms",
                "vehicle_adapter.contract.heartbeat_period_ms",
            ),
            safe_stop_supported=safe_stop_supported,
            estop_supported=estop_supported,
            command_ack=command_ack,
            telemetry_fields=telemetry_fields,
            integration=integration,
        ),
    )


def _parse_vehicle_adapter_integration(
    adapter_type: str,
    raw: Dict[str, Any],
    *,
    require_integration: bool,
) -> VehicleAdapterIntegrationConfig | None:
    integration = raw.get("integration")
    if integration is None:
        if require_integration and adapter_type in {"can", "dynamic_library"}:
            raise ConfigError(f"vehicle_adapter.integration.chassis_control is required for {adapter_type}")
        return None
    if not isinstance(integration, dict):
        raise ConfigError("vehicle_adapter.integration must be configured as a mapping")

    chassis = _mapping(integration, "chassis_control")
    abi = _required_str(chassis, "abi", "vehicle_adapter.integration.chassis_control.abi")
    if abi not in {"cplusplus", "c_shim"}:
        raise ConfigError("vehicle_adapter.integration.chassis_control.abi must be cplusplus or c_shim")
    requires_cpp_bridge = _required_bool(
        chassis,
        "requires_cpp_bridge",
        "vehicle_adapter.integration.chassis_control.requires_cpp_bridge",
    )
    if abi == "cplusplus" and not requires_cpp_bridge:
        raise ConfigError("vehicle_adapter.integration.chassis_control.requires_cpp_bridge must be true for cplusplus ABI")
    bridge_library_path = None
    if "bridge_library_path" in chassis:
        bridge_library_path = _required_existing_file(
            chassis,
            "bridge_library_path",
            "vehicle_adapter.integration.chassis_control.bridge_library_path",
        )
    if abi == "c_shim" and bridge_library_path is None:
        raise ConfigError("vehicle_adapter.integration.chassis_control.bridge_library_path is required for c_shim ABI")
    library_path = None
    if "library_path" in chassis:
        library_path = _required_existing_file(
            chassis,
            "library_path",
            "vehicle_adapter.integration.chassis_control.library_path",
        )

    minepilot = None
    if "minepilot" in integration:
        minepilot_raw = _mapping(integration, "minepilot")
        minepilot = MinePilotCanIntegrationConfig(
            source_root=_required_existing_dir(
                minepilot_raw,
                "source_root",
                "vehicle_adapter.integration.minepilot.source_root",
            ),
            can_common_header_path=_required_existing_file(
                minepilot_raw,
                "can_common_header_path",
                "vehicle_adapter.integration.minepilot.can_common_header_path",
            ),
            can_message_header_path=_required_existing_file(
                minepilot_raw,
                "can_message_header_path",
                "vehicle_adapter.integration.minepilot.can_message_header_path",
            ),
            can_db_header_path=_required_existing_file(
                minepilot_raw,
                "can_db_header_path",
                "vehicle_adapter.integration.minepilot.can_db_header_path",
            ),
            can_receiver_header_path=_required_existing_file(
                minepilot_raw,
                "can_receiver_header_path",
                "vehicle_adapter.integration.minepilot.can_receiver_header_path",
            ),
            can_sender_header_path=_required_existing_file(
                minepilot_raw,
                "can_sender_header_path",
                "vehicle_adapter.integration.minepilot.can_sender_header_path",
            ),
            can_db_source_path=_required_existing_file(
                minepilot_raw,
                "can_db_source_path",
                "vehicle_adapter.integration.minepilot.can_db_source_path",
            ),
            can_receiver_source_path=_required_existing_file(
                minepilot_raw,
                "can_receiver_source_path",
                "vehicle_adapter.integration.minepilot.can_receiver_source_path",
            ),
            can_sender_source_path=_required_existing_file(
                minepilot_raw,
                "can_sender_source_path",
                "vehicle_adapter.integration.minepilot.can_sender_source_path",
            ),
        )

    return VehicleAdapterIntegrationConfig(
        chassis_control=ChassisControlIntegrationConfig(
            source_root=_required_existing_dir(
                chassis,
                "source_root",
                "vehicle_adapter.integration.chassis_control.source_root",
            ),
            header_path=_required_existing_file(
                chassis,
                "header_path",
                "vehicle_adapter.integration.chassis_control.header_path",
            ),
            can_common_header_path=_required_existing_file(
                chassis,
                "can_common_header_path",
                "vehicle_adapter.integration.chassis_control.can_common_header_path",
            ),
            cmake_target=_required_str(
                chassis,
                "cmake_target",
                "vehicle_adapter.integration.chassis_control.cmake_target",
            ),
            library_output_name=_required_str(
                chassis,
                "library_output_name",
                "vehicle_adapter.integration.chassis_control.library_output_name",
            ),
            can_interface=_required_str(
                chassis,
                "can_interface",
                "vehicle_adapter.integration.chassis_control.can_interface",
            ),
            abi=abi,
            requires_cpp_bridge=requires_cpp_bridge,
            library_path=library_path,
            bridge_library_path=bridge_library_path,
        ),
        minepilot=minepilot,
    )


def _derive_capacity(
    cameras: List[CameraConfig],
    record_profiles: Dict[str, MediaProfile],
    recording: RecordingConfig,
    upload: UploadConfig,
) -> CapacityPlan:
    recording_mbps = recording_mbps_for(cameras, record_profiles)
    recording_gb_per_hour = mbps_to_gb_per_hour(recording_mbps)
    upload_gb_per_hour = mbps_to_gb_per_hour(upload.max_bandwidth_mbps)
    net_growth_gb_per_hour = max(0.0, recording_gb_per_hour - upload_gb_per_hour)
    required_retention_gb = recording_gb_per_hour * recording.retention_target_hours
    if upload.max_bandwidth_mbps < recording_mbps:
        if not recording.upload_lag_policy:
            raise ConfigError("upload_lag_policy is required when upload bandwidth is below recording production rate")
        status = "upload_lag_policy_required_and_configured"
    else:
        status = "upload_capacity_sufficient"
    return CapacityPlan(
        recording_mbps=recording_mbps,
        upload_mbps=upload.max_bandwidth_mbps,
        status=status,
        recording_gb_per_hour=recording_gb_per_hour,
        upload_gb_per_hour=upload_gb_per_hour,
        net_growth_gb_per_hour=net_growth_gb_per_hour,
        required_retention_gb=required_retention_gb,
    )


def _mapping(raw: Dict[str, Any], key: str) -> Dict[str, Any]:
    value = raw.get(key)
    if not isinstance(value, dict):
        raise ConfigError(f"{key} must be configured as a mapping")
    return value


def _required_str(raw: Dict[str, Any], key: str, label: str) -> str:
    value = raw.get(key)
    if not isinstance(value, str) or not value:
        raise ConfigError(f"{label} is required")
    return value


def _required_existing_dir(raw: Dict[str, Any], key: str, label: str) -> str:
    value = Path(_required_str(raw, key, label))
    if not value.is_dir():
        raise ConfigError(f"{label} must exist")
    return str(value)


def _required_existing_file(raw: Dict[str, Any], key: str, label: str) -> str:
    value = Path(_required_str(raw, key, label))
    if not value.is_file():
        raise ConfigError(f"{label} must exist")
    return str(value)


def _optional_existing_file(raw: Dict[str, Any], key: str, label: str) -> str | None:
    if key not in raw or raw.get(key) is None:
        return None
    value = Path(_required_str(raw, key, label))
    if not value.is_file():
        raise ConfigError(f"{label} must exist")
    return str(value)


def _optional_non_empty_str(raw: Dict[str, Any], key: str, label: str) -> str | None:
    if key not in raw or raw.get(key) is None:
        return None
    return _required_str(raw, key, label)


def _optional_non_empty_string(
    raw: Dict[str, Any],
    key: str,
    label: str,
    default: str | None = None,
) -> str | None:
    if key not in raw or raw.get(key) is None:
        return default
    value = raw.get(key)
    if not isinstance(value, str) or not value:
        raise ConfigError(f"{label} must be a non-empty string")
    return value


def _required_safe_bucket(raw: Dict[str, Any], key: str, label: str) -> str:
    value = _required_str(raw, key, label)
    if "/" in value or "\\" in value:
        raise ConfigError(f"{label} must not contain path separators")
    return value


def _required_str_list(raw: Dict[str, Any], key: str, label: str) -> List[str]:
    value = raw.get(key)
    if not isinstance(value, list) or not value:
        raise ConfigError(f"{label} must be a non-empty list")
    parsed = []
    for index, item in enumerate(value):
        if not isinstance(item, str) or not item:
            raise ConfigError(f"{label}[{index}] must be a non-empty string")
        parsed.append(item)
    return parsed


def _optional_str_list(
    raw: Dict[str, Any],
    key: str,
    label: str,
    default: tuple[str, ...],
) -> List[str]:
    if key not in raw or raw.get(key) is None:
        return list(default)
    return _required_str_list(raw, key, label)


def _required_key_name(raw: Dict[str, Any], key: str, label: str) -> str:
    value = _required_str(raw, key, label)
    normalized = value.strip().upper()
    if not normalized:
        raise ConfigError(f"{label} is required")
    return normalized


def _required_bool(raw: Dict[str, Any], key: str, label: str) -> bool:
    value = raw.get(key)
    if not isinstance(value, bool):
        raise ConfigError(f"{label} must be true or false")
    return value


def _optional_bool(raw: Dict[str, Any], key: str, label: str, default: bool = False) -> bool:
    value = raw.get(key, default)
    if not isinstance(value, bool):
        raise ConfigError(f"{label} must be a boolean")
    return value


def _required_url(raw: Dict[str, Any], key: str, label: str, schemes: set[str]) -> str:
    value = _required_str(raw, key, label)
    return _validate_url(value, label, schemes)


def _validate_ice_url(value: Any, label: str, schemes: set[str]) -> str:
    if not isinstance(value, str) or not value:
        raise ConfigError(f"{label} is required")
    return _validate_url(value, label, schemes)


def _validate_url(value: str, label: str, schemes: set[str]) -> str:
    parsed = urlparse(value)
    if parsed.scheme not in schemes:
        expected = ", ".join(sorted(schemes))
        raise ConfigError(f"{label} must use one of: {expected}")
    if not parsed.netloc and not parsed.path:
        raise ConfigError(f"{label} must include a host")
    return value


def _require_tls_for_non_loopback(value: str, label: str, tls_schemes: set[str]) -> None:
    parsed = urlparse(value)
    if parsed.scheme in tls_schemes:
        return
    host = parsed.hostname
    if host is not None and _is_loopback_host(host):
        return
    raise ConfigError(f"{label} must use TLS for non-loopback hosts")


def _has_non_loopback_host(*values: str) -> bool:
    for value in values:
        host = urlparse(value).hostname
        if host is None or not _is_loopback_host(host):
            return True
    return False


def _is_loopback_host(host: str) -> bool:
    if host.lower() == "localhost":
        return True
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


def _positive_int(raw: Dict[str, Any], key: str, label: str) -> int:
    value = raw.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
        raise ConfigError(f"{label} must be a positive integer")
    return value


def _positive_or_zero_int(raw: Dict[str, Any], key: str, label: str) -> int:
    value = raw.get(key)
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ConfigError(f"{label} must be a non-negative integer")
    return value


def _positive_number(value: Any, label: str) -> None:
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(float(value)) or value <= 0:
        raise ConfigError(f"{label} must be a positive finite number")


def _positive_finite_number(value: Any, label: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(float(value)) or value <= 0:
        raise ConfigError(f"{label} must be a positive finite number")
    return float(value)


def _non_negative_finite_number(value: Any, label: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(float(value)) or value < 0:
        raise ConfigError(f"{label} must be a non-negative finite number")
    return float(value)


def _parse_deceleration_brake(value: Any, label: str) -> float | str:
    if value == "vehicle_defined_max_safe":
        return value
    if (
        not isinstance(value, (int, float))
        or isinstance(value, bool)
        or not math.isfinite(float(value))
        or not 0.0 <= float(value) <= 1.0
    ):
        raise ConfigError(f"{label} must be a number in [0, 1] or vehicle_defined_max_safe")
    return float(value)
