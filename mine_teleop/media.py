from __future__ import annotations

import json
import math
import shlex
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Dict, Optional, Set

from .config import RuntimeConfigUpdateDecision, RuntimeConfigUpdatePolicy
from .observability import ComponentLogEvent


def _quote(value: str) -> str:
    return shlex.quote(value)


def _background_wait_script(commands: list[str]) -> str:
    parts = ["pids="]
    for command in commands:
        parts.append(f"({command}) & pids=\"$pids $!\"")
    parts.append('for pid in $pids; do wait "$pid"; done')
    return " ; ".join(parts)


@dataclass(frozen=True)
class EncoderChoice:
    backend: str
    reason: str


class EncoderSelector:
    def __init__(self, available_backends: Set[str]) -> None:
        self.available_backends = set(available_backends)

    def select(self, requested: str) -> EncoderChoice:
        if requested in self.available_backends:
            return EncoderChoice(requested, "requested_available")
        if "x264" in self.available_backends:
            return EncoderChoice("x264", "requested_unavailable_fallback")
        raise RuntimeError(f"encoder backend {requested} unavailable and x264 fallback missing")


def _invalid_int(value: object, minimum: int) -> bool:
    return isinstance(value, bool) or not isinstance(value, int) or value < minimum


def _invalid_number(value: object, minimum: float, allow_zero: bool = True) -> bool:
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return True
    if not math.isfinite(float(value)):
        return True
    if allow_zero:
        return value < minimum
    return value <= minimum


def _validate_realtime_network_sample(sample: RealtimeNetworkSample) -> None:
    if _invalid_int(sample.rtt_ms, minimum=0):
        raise ValueError("realtime network sample rtt_ms must be a non-negative integer")
    if _invalid_number(sample.packet_loss_percent, minimum=0):
        raise ValueError("realtime network sample packet_loss_percent must be non-negative")


@dataclass(frozen=True)
class RealtimeNetworkSample:
    rtt_ms: int
    packet_loss_percent: float


@dataclass(frozen=True)
class RealtimeBitrateDecision:
    bitrate_kbps: int
    reason: str


@dataclass(frozen=True)
class RealtimeProfileVariant:
    name: str
    width: int
    height: int
    fps: int
    bitrate_kbps: int

    def __post_init__(self) -> None:
        if not self.name:
            raise ValueError("realtime profile name must be non-empty")
        for field_name in ("width", "height", "fps", "bitrate_kbps"):
            value = getattr(self, field_name)
            if not isinstance(value, int) or isinstance(value, bool) or value <= 0:
                raise ValueError(f"realtime profile {self.name} must have positive {field_name}")


@dataclass(frozen=True)
class RealtimeProfileDecision:
    profile_name: str
    width: int
    height: int
    fps: int
    bitrate_kbps: int
    reason: str
    restart_required: bool


@dataclass(frozen=True)
class RealtimeProfileSwitchResult:
    camera_id: str
    profile_name: str
    width: int
    height: int
    fps: int
    bitrate_kbps: int
    reason: str
    restart_required: bool
    changed: bool


class RealtimeBitrateAdaptationPolicy:
    def __init__(
        self,
        target_bitrate_kbps: int,
        min_bitrate_kbps: int,
        decrease_ratio: float,
        increase_ratio: float,
        max_rtt_ms: int,
        max_loss_percent: float,
    ) -> None:
        if (
            _invalid_int(min_bitrate_kbps, minimum=1)
            or _invalid_int(target_bitrate_kbps, minimum=1)
            or target_bitrate_kbps < min_bitrate_kbps
        ):
            raise ValueError("target bitrate must be greater than or equal to positive minimum bitrate")
        if _invalid_number(decrease_ratio, minimum=0, allow_zero=False) or decrease_ratio >= 1.0:
            raise ValueError("decrease_ratio must be between 0 and 1")
        if _invalid_number(increase_ratio, minimum=1, allow_zero=False):
            raise ValueError("increase_ratio must be greater than 1")
        if _invalid_int(max_rtt_ms, minimum=0) or _invalid_number(max_loss_percent, minimum=0):
            raise ValueError("network quality thresholds must be non-negative")
        self.target_bitrate_kbps = target_bitrate_kbps
        self.min_bitrate_kbps = min_bitrate_kbps
        self.decrease_ratio = decrease_ratio
        self.increase_ratio = increase_ratio
        self.max_rtt_ms = max_rtt_ms
        self.max_loss_percent = max_loss_percent

    def evaluate(self, current_bitrate_kbps: int, sample: RealtimeNetworkSample) -> RealtimeBitrateDecision:
        if _invalid_int(current_bitrate_kbps, minimum=1):
            raise ValueError("current bitrate must be a positive integer")
        _validate_realtime_network_sample(sample)
        if sample.rtt_ms > self.max_rtt_ms or sample.packet_loss_percent > self.max_loss_percent:
            next_bitrate = max(self.min_bitrate_kbps, int(current_bitrate_kbps * self.decrease_ratio))
            return RealtimeBitrateDecision(next_bitrate, "network_congested")
        if current_bitrate_kbps < self.target_bitrate_kbps:
            next_bitrate = min(self.target_bitrate_kbps, int(current_bitrate_kbps * self.increase_ratio))
            return RealtimeBitrateDecision(next_bitrate, "network_recovered")
        return RealtimeBitrateDecision(self.target_bitrate_kbps, "network_quality_ok")


class RealtimeProfileAdaptationPolicy:
    def __init__(
        self,
        profiles: list[RealtimeProfileVariant],
        max_rtt_ms: int,
        max_loss_percent: float,
    ) -> None:
        if not profiles:
            raise ValueError("at least one realtime profile is required")
        if _invalid_int(max_rtt_ms, minimum=0) or _invalid_number(max_loss_percent, minimum=0):
            raise ValueError("network quality thresholds must be non-negative")

        names: set[str] = set()
        for profile in profiles:
            if profile.name in names:
                raise ValueError(f"duplicate realtime profile {profile.name}")
            names.add(profile.name)

        self.profiles = list(profiles)
        self.profile_index_by_name = {profile.name: index for index, profile in enumerate(self.profiles)}
        self.max_rtt_ms = max_rtt_ms
        self.max_loss_percent = max_loss_percent

    def evaluate(self, current_profile_name: str, sample: RealtimeNetworkSample) -> RealtimeProfileDecision:
        if current_profile_name not in self.profile_index_by_name:
            raise ValueError(f"unknown realtime profile {current_profile_name}")
        _validate_realtime_network_sample(sample)

        current_index = self.profile_index_by_name[current_profile_name]
        if sample.rtt_ms > self.max_rtt_ms or sample.packet_loss_percent > self.max_loss_percent:
            target_index = min(len(self.profiles) - 1, current_index + 1)
            reason = (
                "network_congested_profile_downshift"
                if target_index != current_index
                else "network_congested_profile_floor"
            )
            return self._decision_for(target_index, reason, restart_required=target_index != current_index)

        if current_index > 0:
            return self._decision_for(
                current_index - 1,
                "network_recovered_profile_upshift",
                restart_required=True,
            )
        return self._decision_for(current_index, "network_quality_ok", restart_required=False)

    def _decision_for(self, index: int, reason: str, restart_required: bool) -> RealtimeProfileDecision:
        profile = self.profiles[index]
        return RealtimeProfileDecision(
            profile_name=profile.name,
            width=profile.width,
            height=profile.height,
            fps=profile.fps,
            bitrate_kbps=profile.bitrate_kbps,
            reason=reason,
            restart_required=restart_required,
        )


@dataclass(frozen=True)
class GStreamerPropertyUpdate:
    element_name: str
    property_name: str
    value: int


class GStreamerPipelinePropertySetter:
    def __init__(self, pipeline: object) -> None:
        self.pipeline = pipeline

    def __call__(self, update: GStreamerPropertyUpdate) -> None:
        get_by_name = getattr(self.pipeline, "get_by_name", None)
        if get_by_name is None:
            raise RuntimeError("gstreamer pipeline does not expose get_by_name")
        element = get_by_name(update.element_name)
        if element is None:
            raise RuntimeError(f"gstreamer element {update.element_name} not found")
        set_property = getattr(element, "set_property", None)
        if set_property is None:
            raise RuntimeError(f"gstreamer element {update.element_name} does not expose set_property")
        set_property(update.property_name, update.value)


class RealtimeMediaRuntime:
    def __init__(
        self,
        profile_bitrates: Dict[str, int],
        encoder_name_by_profile: Optional[Dict[str, str]] = None,
        property_setter: Optional[Callable[[GStreamerPropertyUpdate], None]] = None,
        profile_variants_by_camera: Optional[Dict[str, Dict[str, RealtimeProfileVariant]]] = None,
        active_profile_by_camera: Optional[Dict[str, str]] = None,
        profile_switcher: Optional[Callable[[str, str], None]] = None,
    ) -> None:
        self.profile_bitrates = dict(profile_bitrates)
        self.encoder_name_by_profile = encoder_name_by_profile or {
            profile_name: f"{profile_name}_encoder" for profile_name in profile_bitrates
        }
        self.property_setter = property_setter
        self.profile_variants_by_camera = {
            camera_id: dict(profiles) for camera_id, profiles in (profile_variants_by_camera or {}).items()
        }
        self.active_profile_by_camera = dict(active_profile_by_camera or {})
        self.profile_switcher = profile_switcher
        self.applied_updates: list[GStreamerPropertyUpdate] = []
        self.applied_profile_switches: list[RealtimeProfileSwitchResult] = []

    def apply_runtime_update(
        self,
        path: str,
        value: object,
        policy: Optional[RuntimeConfigUpdatePolicy] = None,
    ) -> RuntimeConfigUpdateDecision:
        decision = (policy or RuntimeConfigUpdatePolicy.default()).evaluate(path, value)
        if not decision.allowed:
            return decision
        prefix = "media.realtime_profiles."
        suffix = ".bitrate_kbps"
        if not path.startswith(prefix) or not path.endswith(suffix):
            return RuntimeConfigUpdateDecision(path, False, "runtime_update_not_applicable_to_media_runtime", False)
        profile_name = path[len(prefix) : -len(suffix)]
        if profile_name not in self.profile_bitrates:
            return RuntimeConfigUpdateDecision(path, False, "runtime_update_unknown_realtime_profile", False)

        bitrate_kbps = int(value)
        update = GStreamerPropertyUpdate(
            element_name=self.encoder_name_by_profile.get(profile_name, f"{profile_name}_encoder"),
            property_name="bitrate",
            value=bitrate_kbps,
        )
        if self.property_setter is not None:
            self.property_setter(update)
        self.profile_bitrates[profile_name] = bitrate_kbps
        self.applied_updates.append(update)
        return decision

    def apply_realtime_profile_decision(
        self,
        camera_id: str,
        decision: RealtimeProfileDecision,
    ) -> RealtimeProfileSwitchResult:
        profiles = self.profile_variants_by_camera.get(camera_id)
        if profiles is None:
            raise ValueError(f"unknown realtime camera {camera_id}")
        if decision.profile_name not in profiles:
            raise ValueError(f"unknown realtime profile {decision.profile_name} for camera {camera_id}")

        active_profile_name = self.active_profile_by_camera.get(camera_id)
        changed = active_profile_name != decision.profile_name
        if changed:
            if self.profile_switcher is None:
                raise RuntimeError("realtime profile switcher is not configured")
            self.profile_switcher(camera_id, decision.profile_name)
            self.active_profile_by_camera[camera_id] = decision.profile_name

        profile = profiles[decision.profile_name]
        self.profile_bitrates[decision.profile_name] = profile.bitrate_kbps
        result = RealtimeProfileSwitchResult(
            camera_id=camera_id,
            profile_name=profile.name,
            width=profile.width,
            height=profile.height,
            fps=profile.fps,
            bitrate_kbps=profile.bitrate_kbps,
            reason=decision.reason,
            restart_required=decision.restart_required and changed,
            changed=changed,
        )
        self.applied_profile_switches.append(result)
        return result


@dataclass(frozen=True)
class MediaPipelineWatchdogStatus:
    camera_id: str
    status: str
    last_heartbeat_ms: int | None
    age_ms: int | None
    log_event: ComponentLogEvent | None = None


class MediaPipelineWatchdog:
    def __init__(self, component: str, timeout_ms: int) -> None:
        if _invalid_int(timeout_ms, minimum=1):
            raise ValueError("timeout_ms must be positive")
        self.component = component
        self.timeout_ms = timeout_ms
        self._last_heartbeat_by_camera: Dict[str, int] = {}
        self._reported_stalls: Set[str] = set()

    def heartbeat(self, camera_id: str, now_ms: int) -> None:
        if _invalid_int(now_ms, minimum=0):
            raise ValueError("heartbeat now_ms must be a non-negative integer")
        self._last_heartbeat_by_camera[camera_id] = now_ms
        self._reported_stalls.discard(camera_id)

    def assess(
        self,
        camera_id: str,
        now_ms: int,
        vehicle_id: str,
        session_id: str,
    ) -> MediaPipelineWatchdogStatus:
        if _invalid_int(now_ms, minimum=0):
            raise ValueError("assess now_ms must be a non-negative integer")
        last_heartbeat_ms = self._last_heartbeat_by_camera.get(camera_id)
        if last_heartbeat_ms is None:
            return MediaPipelineWatchdogStatus(camera_id, "missing_heartbeat", None, None)
        if now_ms < last_heartbeat_ms:
            raise ValueError("assess now_ms must not be earlier than last heartbeat")
        age_ms = now_ms - last_heartbeat_ms
        if age_ms <= self.timeout_ms:
            return MediaPipelineWatchdogStatus(camera_id, "ok", last_heartbeat_ms, age_ms)
        log_event = None
        if camera_id not in self._reported_stalls:
            self._reported_stalls.add(camera_id)
            log_event = ComponentLogEvent(
                ts_ms=now_ms,
                level="warning",
                component=self.component,
                vehicle_id=vehicle_id,
                session_id=session_id,
                camera_id=camera_id,
                event="media_pipeline_stalled",
                message=f"{camera_id} media pipeline heartbeat stalled for {age_ms} ms",
                error_code="media_watchdog_timeout",
            )
        return MediaPipelineWatchdogStatus(camera_id, "stalled", last_heartbeat_ms, age_ms, log_event)


@dataclass(frozen=True)
class MediaFaultRecoveryDecision:
    action: str
    affected_camera_id: str
    status_update: Dict[str, object]
    log_event: ComponentLogEvent
    stop_vehicle_control: bool = False
    encoder: EncoderChoice | None = None


@dataclass(frozen=True)
class MediaFaultRecoveryExecution:
    action: str
    affected_camera_id: str
    status_update: Dict[str, object]
    log_event: ComponentLogEvent
    stop_vehicle_control: bool


class MediaFaultRecoveryExecutor:
    def __init__(self, pipeline_controller: object) -> None:
        self.pipeline_controller = pipeline_controller

    def execute(self, decision: MediaFaultRecoveryDecision) -> MediaFaultRecoveryExecution:
        if decision.action == "restart_camera_pipeline":
            self._call_controller("restart_camera_pipeline", decision.affected_camera_id)
        elif decision.action == "fallback_encoder":
            if decision.encoder is None:
                raise RuntimeError("fallback_encoder recovery decision is missing encoder")
            self._call_controller("switch_camera_encoder", decision.affected_camera_id, decision.encoder.backend)
        else:
            raise RuntimeError(f"unknown media fault recovery action {decision.action}")
        return MediaFaultRecoveryExecution(
            action=decision.action,
            affected_camera_id=decision.affected_camera_id,
            status_update=decision.status_update,
            log_event=decision.log_event,
            stop_vehicle_control=decision.stop_vehicle_control,
        )

    def _call_controller(self, method_name: str, *args: object) -> None:
        method = getattr(self.pipeline_controller, method_name, None)
        if method is None:
            raise RuntimeError(f"media pipeline controller does not expose {method_name}")
        method(*args)


class MediaFaultRecoveryPolicy:
    def __init__(self, component: str) -> None:
        self.component = component

    def camera_pipeline_stalled(
        self,
        camera_id: str,
        vehicle_id: str,
        session_id: str,
        now_ms: int,
        reason: str,
    ) -> MediaFaultRecoveryDecision:
        return MediaFaultRecoveryDecision(
            action="restart_camera_pipeline",
            affected_camera_id=camera_id,
            status_update={
                "state": "reconnecting",
                "fps": 0,
                "bitrate_kbps": 0,
                "latency_ms": None,
                "low_bitrate": False,
                "reconnecting": True,
                "fault": reason,
            },
            log_event=ComponentLogEvent(
                ts_ms=now_ms,
                level="warning",
                component=self.component,
                vehicle_id=vehicle_id,
                session_id=session_id,
                camera_id=camera_id,
                event="media_pipeline_restart_requested",
                message=f"{camera_id} media pipeline stalled; restart requested",
                error_code=reason,
            ),
        )

    def encoder_failed(
        self,
        requested_encoder: str,
        fallback_encoder: str,
        vehicle_id: str,
        session_id: str,
        now_ms: int,
        camera_id: str,
        reason: str,
    ) -> MediaFaultRecoveryDecision:
        encoder = EncoderChoice(fallback_encoder, f"{reason}_fallback")
        return MediaFaultRecoveryDecision(
            action="fallback_encoder",
            affected_camera_id=camera_id,
            status_update={
                "state": "degraded",
                "fps": 0,
                "bitrate_kbps": 0,
                "latency_ms": None,
                "low_bitrate": True,
                "reconnecting": True,
                "encoder": fallback_encoder,
                "fault": reason,
            },
            log_event=ComponentLogEvent(
                ts_ms=now_ms,
                level="warning",
                component=self.component,
                vehicle_id=vehicle_id,
                session_id=session_id,
                camera_id=camera_id,
                event="media_encoder_fallback",
                message=f"{requested_encoder} encoder failed; falling back to {fallback_encoder}",
                error_code=reason,
            ),
            encoder=encoder,
        )


@dataclass(frozen=True)
class RealtimeConnectionRecoveryDecision:
    action: str
    status_update: Dict[str, object]
    log_event: ComponentLogEvent
    retry_delay_ms: int = 0
    stop_vehicle_control: bool = False


@dataclass(frozen=True)
class RealtimeConnectionRecoveryExecution:
    action: str
    status_update: Dict[str, object]
    log_event: ComponentLogEvent
    retry_delay_ms: int
    stop_vehicle_control: bool


class RealtimeConnectionRecoveryExecutor:
    def __init__(self, realtime_controller: object) -> None:
        self.realtime_controller = realtime_controller

    def execute(self, decision: RealtimeConnectionRecoveryDecision) -> RealtimeConnectionRecoveryExecution:
        if decision.action == "reconnect_signaling":
            self._call_controller("reconnect_signaling", decision.retry_delay_ms)
        elif decision.action == "ice_restart":
            self._call_controller("restart_ice", self._camera_id(decision))
        elif decision.action == "rebuild_session":
            self._call_controller("rebuild_media_session", self._camera_id(decision))
        else:
            raise RuntimeError(f"unknown realtime connection recovery action {decision.action}")
        return RealtimeConnectionRecoveryExecution(
            action=decision.action,
            status_update=decision.status_update,
            log_event=decision.log_event,
            retry_delay_ms=decision.retry_delay_ms,
            stop_vehicle_control=decision.stop_vehicle_control,
        )

    def _call_controller(self, method_name: str, *args: object) -> None:
        method = getattr(self.realtime_controller, method_name, None)
        if method is None:
            raise RuntimeError(f"realtime connection controller does not expose {method_name}")
        method(*args)

    def _camera_id(self, decision: RealtimeConnectionRecoveryDecision) -> str:
        camera_id = decision.log_event.camera_id
        if not camera_id:
            raise RuntimeError(f"{decision.action} recovery decision is missing camera_id")
        return camera_id


class RealtimeConnectionRecoveryPolicy:
    def __init__(
        self,
        component: str,
        signaling_backoff_ms: int = 500,
        max_ice_restart_attempts: int = 2,
    ) -> None:
        if _invalid_int(signaling_backoff_ms, minimum=1):
            raise ValueError("signaling_backoff_ms must be positive")
        if _invalid_int(max_ice_restart_attempts, minimum=1):
            raise ValueError("max_ice_restart_attempts must be positive")
        self.component = component
        self.signaling_backoff_ms = signaling_backoff_ms
        self.max_ice_restart_attempts = max_ice_restart_attempts

    def signaling_disconnected(
        self,
        vehicle_id: str,
        session_id: str,
        now_ms: int,
        reason: str,
    ) -> RealtimeConnectionRecoveryDecision:
        return RealtimeConnectionRecoveryDecision(
            action="reconnect_signaling",
            retry_delay_ms=self.signaling_backoff_ms,
            status_update={"signaling": "reconnecting", "fault": reason},
            log_event=ComponentLogEvent(
                ts_ms=now_ms,
                level="warning",
                component=self.component,
                vehicle_id=vehicle_id,
                session_id=session_id,
                event="signaling_reconnect_requested",
                message=f"signaling disconnected; reconnect in {self.signaling_backoff_ms} ms",
                error_code=reason,
            ),
        )

    def media_disconnected(
        self,
        camera_id: str,
        vehicle_id: str,
        session_id: str,
        now_ms: int,
        reason: str,
        ice_restart_attempts: int,
    ) -> RealtimeConnectionRecoveryDecision:
        if _invalid_int(ice_restart_attempts, minimum=0):
            raise ValueError("ice_restart_attempts must be a non-negative integer")
        rebuild_session = ice_restart_attempts >= self.max_ice_restart_attempts
        action = "rebuild_session" if rebuild_session else "ice_restart"
        media_state = "rebuilding_session" if rebuild_session else "ice_restarting"
        event = "media_session_rebuild_requested" if rebuild_session else "media_ice_restart_requested"
        message = (
            f"{camera_id} media ICE restart attempts exhausted; session rebuild requested"
            if rebuild_session
            else f"{camera_id} media disconnected; ICE restart requested"
        )
        return RealtimeConnectionRecoveryDecision(
            action=action,
            status_update={"media": media_state, "fault": reason},
            log_event=ComponentLogEvent(
                ts_ms=now_ms,
                level="warning",
                component=self.component,
                vehicle_id=vehicle_id,
                session_id=session_id,
                camera_id=camera_id,
                event=event,
                message=message,
                error_code=reason,
            ),
        )


@dataclass(frozen=True)
class ControlDataChannelConfig:
    label: str
    ordered: bool
    max_retransmits: int
    protocol: str

    @classmethod
    def low_latency_default(cls, label: str) -> "ControlDataChannelConfig":
        return cls(
            label=label,
            ordered=False,
            max_retransmits=0,
            protocol="mine-teleop-control-v1",
        )

    def to_dict(self) -> Dict[str, object]:
        return {
            "label": self.label,
            "ordered": self.ordered,
            "max_retransmits": self.max_retransmits,
            "protocol": self.protocol,
        }

    def to_webrtc_init(self) -> Dict[str, object]:
        return {
            "ordered": self.ordered,
            "maxRetransmits": self.max_retransmits,
            "protocol": self.protocol,
        }


@dataclass(frozen=True)
class H264SdpCompatibilityResult:
    compatible: bool
    selected_profile_level_id: str = ""
    reason: str = ""


class H264SdpCompatibilityChecker:
    def __init__(self, driver_supported_profile_level_ids: Set[str]) -> None:
        self.driver_supported_profile_level_ids = {
            profile_level_id.lower() for profile_level_id in driver_supported_profile_level_ids
        }

    def assess(self, encoder_profile_level_id: str, remote_sdp: str) -> H264SdpCompatibilityResult:
        requested = encoder_profile_level_id.lower()
        if requested not in self.driver_supported_profile_level_ids:
            return H264SdpCompatibilityResult(
                False,
                reason=f"encoder profile-level-id {requested} not supported by driver decoder",
            )
        remote_profiles = self._remote_h264_profiles(remote_sdp)
        if not remote_profiles:
            return H264SdpCompatibilityResult(False, reason="remote SDP did not offer H264 profile-level-id")
        if requested not in remote_profiles:
            return H264SdpCompatibilityResult(
                False,
                reason=f"encoder profile-level-id {requested} not offered by remote SDP",
            )
        packetization_modes = remote_profiles[requested]
        if not packetization_modes.intersection({"", "1"}):
            packetization_mode = ", ".join(sorted(packetization_modes))
            return H264SdpCompatibilityResult(
                False,
                reason=f"remote SDP packetization-mode {packetization_mode} is not supported",
            )
        return H264SdpCompatibilityResult(True, selected_profile_level_id=requested)

    def _remote_h264_profiles(self, remote_sdp: str) -> Dict[str, Set[str]]:
        h264_payload_types: Set[str] = set()
        for raw_line in remote_sdp.splitlines():
            line = raw_line.strip()
            if not line.lower().startswith("a=rtpmap:"):
                continue
            payload_type, _, codec = line[len("a=rtpmap:") :].partition(" ")
            if codec.lower().startswith("h264/"):
                h264_payload_types.add(payload_type.strip())

        profiles: Dict[str, Set[str]] = {}
        for raw_line in remote_sdp.splitlines():
            line = raw_line.strip()
            if not line.startswith("a=fmtp:") or "profile-level-id" not in line:
                continue
            payload_type, _, params = line[len("a=fmtp:") :].partition(" ")
            if payload_type.strip() not in h264_payload_types:
                continue
            parsed = self._parse_fmtp_params(params)
            profile_level_id = parsed.get("profile-level-id", "").lower()
            if profile_level_id:
                profiles.setdefault(profile_level_id, set()).add(parsed.get("packetization-mode", ""))
        return profiles

    def _parse_fmtp_params(self, params: str) -> Dict[str, str]:
        parsed: Dict[str, str] = {}
        for part in params.split(";"):
            key, separator, value = part.strip().partition("=")
            if separator:
                parsed[key.lower()] = value.strip()
        return parsed


class GStreamerPipelineBuilder:
    def realtime_h264_pipeline(
        self,
        source_device: str,
        width: int,
        height: int,
        fps: int,
        bitrate_kbps: int,
        encoder: EncoderChoice,
        keyframe_interval_frames: int = 30,
        encoder_name: str = "realtime_encoder",
    ) -> str:
        source = "videotestsrc is-live=true" if source_device == "testsrc" else f"v4l2src device={source_device}"
        encoder_stage = self._encoder_stage(encoder, bitrate_kbps, keyframe_interval_frames, encoder_name)
        return " ! ".join(
            [
                source,
                "queue max-size-buffers=2 leaky=downstream",
                "videoconvert",
                "videoscale",
                f"video/x-raw,width={width},height={height},framerate={fps}/1",
                encoder_stage,
                "h264parse config-interval=1",
                "rtph264pay config-interval=1 pt=96",
                "application/x-rtp,media=video,encoding-name=H264,payload=96",
                "webrtcbin name=webrtc bundle-policy=max-bundle",
            ]
        )

    def recording_h264_pipeline(
        self,
        source_device: str,
        capture_width: int,
        capture_height: int,
        capture_fps: int,
        bitrate_kbps: int,
        segment_seconds: int,
        output_pattern: str,
        encoder: EncoderChoice,
    ) -> str:
        source = "videotestsrc is-live=true" if source_device == "testsrc" else f"v4l2src device={source_device}"
        encoder_stage = self._encoder_stage(encoder, bitrate_kbps)
        segment_ns = segment_seconds * 1_000_000_000
        return " ! ".join(
            [
                source,
                "queue max-size-buffers=0 max-size-time=0",
                "videoconvert",
                f"video/x-raw,width={capture_width},height={capture_height},framerate={capture_fps}/1",
                encoder_stage,
                "h264parse config-interval=-1",
                f"splitmuxsink muxer=mp4mux max-size-time={segment_ns} location={output_pattern}",
            ]
        )

    def _encoder_stage(
        self,
        encoder: EncoderChoice,
        bitrate_kbps: int,
        keyframe_interval_frames: int = 30,
        encoder_name: Optional[str] = None,
    ) -> str:
        name_property = f" name={encoder_name}" if encoder_name else ""
        if encoder.backend in {"vaapi", "qsv"}:
            return f"vaapih264enc rate-control=cbr bitrate={bitrate_kbps} keyframe-period={keyframe_interval_frames}{name_property}"
        if encoder.backend == "x264":
            return f"x264enc tune=zerolatency speed-preset=ultrafast bitrate={bitrate_kbps} key-int-max={keyframe_interval_frames}{name_property}"
        raise ValueError(f"unsupported realtime encoder backend: {encoder.backend}")


@dataclass(frozen=True)
class FFmpegVaapiProbePlan:
    render_device: str
    card_device: str
    output_dir: str
    lanes: int
    width: int
    height: int
    fps: int
    duration_seconds: int
    bitrate: str
    ffmpeg_binary: str = "ffmpeg"
    ffprobe_binary: str = "ffprobe"
    vainfo_binary: str = "vainfo"
    libva_drivers_path: str = "/usr/lib/x86_64-linux-gnu/dri"

    def host_command(self) -> str:
        if self.lanes <= 0:
            raise ValueError("VAAPI probe plan must include at least one lane")
        lane_commands = []
        for lane in range(self.lanes):
            lane_commands.append(
                f"{_quote(self.ffmpeg_binary)} -hide_banner "
                f"-vaapi_device {_quote(self.render_device)} "
                f"-f lavfi -i testsrc2=size={self.width}x{self.height}:rate={self.fps} "
                f"-t {self.duration_seconds} "
                "-vf format=nv12,hwupload "
                f"-c:v h264_vaapi -b:v {self.bitrate} "
                f"-y {_quote(f'{self.output_dir}/vaapi-h264-lane-{lane}.mp4')}"
            )
        probe = (
            f"{_quote(self.ffprobe_binary)} -hide_banner -select_streams v:0 "
            "-show_entries stream=codec_name,width,height,avg_frame_rate,bit_rate "
            f"-of default=nw=1 {_quote(f'{self.output_dir}/vaapi-h264-lane-0.mp4')}"
        )
        return " && ".join(
            [
                "set -e",
                f"mkdir -p {_quote(self.output_dir)}",
                f"export LIBVA_DRIVERS_PATH={_quote(self.libva_drivers_path)}",
                f"{_quote(self.vainfo_binary)} --display drm --device {_quote(self.render_device)}",
                _background_wait_script(lane_commands),
                probe,
            ]
        )

    def docker_command(self) -> str:
        if self.lanes <= 0:
            raise ValueError("VAAPI probe plan must include at least one lane")
        lane_commands = []
        for lane in range(self.lanes):
            lane_commands.append(
                "ffmpeg -hide_banner "
                f"-vaapi_device {self.render_device} "
                f"-f lavfi -i testsrc2=size={self.width}x{self.height}:rate={self.fps} "
                f"-t {self.duration_seconds} "
                "-vf format=nv12,hwupload "
                f"-c:v h264_vaapi -b:v {self.bitrate} "
                f"-y /out/vaapi-h264-lane-{lane}.mp4"
            )
        probe = (
            "ffprobe -hide_banner -select_streams v:0 "
            "-show_entries stream=codec_name,width,height,avg_frame_rate,bit_rate "
            "-of default=nw=1 /out/vaapi-h264-lane-0.mp4"
        )
        inner = " && ".join(
            [
                "set -e",
                "apt-get update",
                "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ffmpeg vainfo intel-media-va-driver",
                f"(vainfo --display drm --device {self.render_device} || true)",
                _background_wait_script(lane_commands),
                probe,
            ]
        )
        return (
            "sudo docker run --rm "
            f"--device {self.render_device} "
            f"--device {self.card_device} "
            f"-v {self.output_dir}:/out "
            "ubuntu:22.04 "
            f"bash -lc {inner!r}"
        )


@dataclass(frozen=True)
class GStreamerPluginProbePlan:
    hardware_plugins: tuple[str, ...]
    fallback_plugins: tuple[str, ...]

    @classmethod
    def default(cls) -> "GStreamerPluginProbePlan":
        return cls(
            hardware_plugins=("vaapih264enc", "qsvh264enc", "vah264enc", "nvh264enc"),
            fallback_plugins=("x264enc",),
        )

    @property
    def command(self) -> str:
        plugins = [*self.hardware_plugins, *self.fallback_plugins]
        return "gst-inspect-1.0 " + " ".join(plugins)


@dataclass(frozen=True)
class FFmpegVaapiProbeLane:
    lane_id: str
    width: int
    height: int
    fps: int
    bitrate: str


@dataclass(frozen=True)
class FFmpegVaapiLoadScenario:
    name: str
    render_device: str
    card_device: str
    output_dir: str
    duration_seconds: int
    lanes: tuple[FFmpegVaapiProbeLane, ...]
    ffmpeg_binary: str = "ffmpeg"
    ffprobe_binary: str = "ffprobe"
    vainfo_binary: str = "vainfo"
    libva_drivers_path: str = "/usr/lib/x86_64-linux-gnu/dri"

    def host_command(self) -> str:
        if not self.lanes:
            raise ValueError("hardware encoding scenario must include at least one lane")
        lane_commands = []
        for lane in self.lanes:
            lane_commands.append(
                f"{_quote(self.ffmpeg_binary)} -hide_banner "
                f"-vaapi_device {_quote(self.render_device)} "
                f"-f lavfi -i testsrc2=size={lane.width}x{lane.height}:rate={lane.fps} "
                f"-t {self.duration_seconds} "
                "-vf format=nv12,hwupload "
                f"-c:v h264_vaapi -b:v {lane.bitrate} "
                f"-y {_quote(f'{self.output_dir}/{self.name}-{lane.lane_id}.mp4')}"
            )
        probe = (
            f"{_quote(self.ffprobe_binary)} -hide_banner -select_streams v:0 "
            "-show_entries stream=codec_name,width,height,avg_frame_rate,bit_rate "
            f"-of default=nw=1 {_quote(f'{self.output_dir}/{self.name}-{self.lanes[0].lane_id}.mp4')}"
        )
        return " && ".join(
            [
                "set -e",
                f"mkdir -p {_quote(self.output_dir)}",
                f"export LIBVA_DRIVERS_PATH={_quote(self.libva_drivers_path)}",
                f"{_quote(self.vainfo_binary)} --display drm --device {_quote(self.render_device)}",
                _background_wait_script(lane_commands),
                probe,
            ]
        )

    def docker_command(self) -> str:
        if not self.lanes:
            raise ValueError("hardware encoding scenario must include at least one lane")
        lane_commands = []
        for lane in self.lanes:
            lane_commands.append(
                "ffmpeg -hide_banner "
                f"-vaapi_device {self.render_device} "
                f"-f lavfi -i testsrc2=size={lane.width}x{lane.height}:rate={lane.fps} "
                f"-t {self.duration_seconds} "
                "-vf format=nv12,hwupload "
                f"-c:v h264_vaapi -b:v {lane.bitrate} "
                f"-y /out/{self.name}-{lane.lane_id}.mp4"
            )
        probe = (
            "ffprobe -hide_banner -select_streams v:0 "
            "-show_entries stream=codec_name,width,height,avg_frame_rate,bit_rate "
            f"-of default=nw=1 /out/{self.name}-{self.lanes[0].lane_id}.mp4"
        )
        inner = " && ".join(
            [
                "set -e",
                "apt-get update",
                "DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends ffmpeg vainfo intel-media-va-driver",
                f"(vainfo --display drm --device {self.render_device} || true)",
                _background_wait_script(lane_commands),
                probe,
            ]
        )
        return (
            "sudo docker run --rm "
            f"--device {self.render_device} "
            f"--device {self.card_device} "
            f"-v {self.output_dir}:/out "
            "ubuntu:22.04 "
            f"bash -lc {inner!r}"
        )


@dataclass(frozen=True)
class HardwareEncodingValidationPlan:
    gstreamer_plugin_probe: GStreamerPluginProbePlan
    scenarios: tuple[FFmpegVaapiLoadScenario, ...]
    metrics_fields: tuple[str, ...]

    @classmethod
    def four_camera_default(
        cls,
        render_device: str = "/dev/dri/renderD128",
        card_device: str = "/dev/dri/card1",
        output_dir: str = "/tmp/mine-teleop-vaapi",
        duration_seconds: int = 5,
        gstreamer_plugin_probe: GStreamerPluginProbePlan | None = None,
        ffmpeg_binary: str = "ffmpeg",
        ffprobe_binary: str = "ffprobe",
        vainfo_binary: str = "vainfo",
        libva_drivers_path: str = "/usr/lib/x86_64-linux-gnu/dri",
    ) -> "HardwareEncodingValidationPlan":
        camera_ids = ("front", "rear", "left", "right")
        realtime_lanes = tuple(
            FFmpegVaapiProbeLane(
                lane_id=f"{camera_id}-realtime-720p30",
                width=1280,
                height=720,
                fps=30,
                bitrate="3M",
            )
            for camera_id in camera_ids
        )
        recording_lanes = tuple(
            FFmpegVaapiProbeLane(
                lane_id=f"{camera_id}-recording-source",
                width=1920,
                height=1080,
                fps=30,
                bitrate="8M",
            )
            for camera_id in camera_ids
        )
        return cls(
            gstreamer_plugin_probe=gstreamer_plugin_probe or GStreamerPluginProbePlan.default(),
            scenarios=(
                FFmpegVaapiLoadScenario(
                    name="four-camera-realtime-720p30",
                    render_device=render_device,
                    card_device=card_device,
                    output_dir=output_dir,
                    duration_seconds=duration_seconds,
                    lanes=realtime_lanes,
                    ffmpeg_binary=ffmpeg_binary,
                    ffprobe_binary=ffprobe_binary,
                    vainfo_binary=vainfo_binary,
                    libva_drivers_path=libva_drivers_path,
                ),
                FFmpegVaapiLoadScenario(
                    name="four-camera-recording-source",
                    render_device=render_device,
                    card_device=card_device,
                    output_dir=output_dir,
                    duration_seconds=duration_seconds,
                    lanes=recording_lanes,
                    ffmpeg_binary=ffmpeg_binary,
                    ffprobe_binary=ffprobe_binary,
                    vainfo_binary=vainfo_binary,
                    libva_drivers_path=libva_drivers_path,
                ),
                FFmpegVaapiLoadScenario(
                    name="four-camera-realtime-plus-recording",
                    render_device=render_device,
                    card_device=card_device,
                    output_dir=output_dir,
                    duration_seconds=duration_seconds,
                    lanes=(*realtime_lanes, *recording_lanes),
                    ffmpeg_binary=ffmpeg_binary,
                    ffprobe_binary=ffprobe_binary,
                    vainfo_binary=vainfo_binary,
                    libva_drivers_path=libva_drivers_path,
                ),
            ),
            metrics_fields=(
                "cpu_percent",
                "gpu_percent",
                "memory_mb",
                "disk_write_mb_s",
                "temperature_c",
                "encoded_fps",
                "bitrate_kbps",
                "dropped_frames",
            ),
        )


@dataclass(frozen=True)
class HardwareEncodingLaneValidation:
    lane_id: str
    codec_name: str
    width: int
    height: int
    fps: float
    bit_rate: int
    passed: bool
    failures: tuple[str, ...]

    @property
    def bitrate_kbps(self) -> int:
        return self.bit_rate // 1000

    def to_record(self, scenario_name: str) -> Dict[str, object]:
        return {
            "event": "hardware_encoding_lane",
            "scenario": scenario_name,
            "lane_id": self.lane_id,
            "codec_name": self.codec_name,
            "width": self.width,
            "height": self.height,
            "fps": self.fps,
            "bitrate_kbps": self.bitrate_kbps,
            "passed": self.passed,
            "failures": list(self.failures),
        }


@dataclass(frozen=True)
class HardwareEncodingValidationReport:
    scenario_name: str
    lanes: tuple[HardwareEncodingLaneValidation, ...]
    metrics: Dict[str, object]
    failures: tuple[str, ...]

    @property
    def passed(self) -> bool:
        return not self.failures

    @classmethod
    def from_ffprobe_outputs(
        cls,
        scenario: FFmpegVaapiLoadScenario,
        ffprobe_outputs: Dict[str, str],
        metrics: Dict[str, object],
        min_fps_ratio: float = 0.95,
    ) -> "HardwareEncodingValidationReport":
        lanes: list[HardwareEncodingLaneValidation] = []
        failures: list[str] = []
        for expected_lane in scenario.lanes:
            lane_output = ffprobe_outputs.get(expected_lane.lane_id)
            if lane_output is None:
                failure = f"{expected_lane.lane_id}: missing ffprobe output"
                failures.append(failure)
                lanes.append(
                    HardwareEncodingLaneValidation(
                        lane_id=expected_lane.lane_id,
                        codec_name="",
                        width=0,
                        height=0,
                        fps=0.0,
                        bit_rate=0,
                        passed=False,
                        failures=(failure,),
                    )
                )
                continue

            parsed = _parse_ffprobe_key_value_output(lane_output)
            codec_name = parsed.get("codec_name", "")
            width = _parse_int(parsed.get("width", "0"))
            height = _parse_int(parsed.get("height", "0"))
            fps = _parse_fraction(parsed.get("avg_frame_rate", "0/1"))
            bit_rate = _parse_int(parsed.get("bit_rate", "0"))
            lane_failures: list[str] = []
            if codec_name != "h264":
                lane_failures.append(f"{expected_lane.lane_id}: expected h264 got {codec_name or 'missing'}")
            if width != expected_lane.width or height != expected_lane.height:
                lane_failures.append(
                    f"{expected_lane.lane_id}: expected {expected_lane.width}x{expected_lane.height} got {width}x{height}"
                )
            if fps < expected_lane.fps * min_fps_ratio:
                lane_failures.append(f"{expected_lane.lane_id}: fps {fps:.2f} below expected {expected_lane.fps}")
            if bit_rate <= 0:
                lane_failures.append(f"{expected_lane.lane_id}: bit_rate must be positive")
            failures.extend(lane_failures)
            lanes.append(
                HardwareEncodingLaneValidation(
                    lane_id=expected_lane.lane_id,
                    codec_name=codec_name,
                    width=width,
                    height=height,
                    fps=fps,
                    bit_rate=bit_rate,
                    passed=not lane_failures,
                    failures=tuple(lane_failures),
                )
            )

        return cls(
            scenario_name=scenario.name,
            lanes=tuple(lanes),
            metrics=dict(metrics),
            failures=tuple(failures),
        )

    def to_jsonl(self) -> tuple[str, ...]:
        summary = {
            "event": "hardware_encoding_validation",
            "scenario": self.scenario_name,
            "passed": self.passed,
            "lane_count": len(self.lanes),
            "failures": list(self.failures),
        }
        lines = [json.dumps(summary, ensure_ascii=False, sort_keys=True)]
        lines.extend(json.dumps(lane.to_record(self.scenario_name), ensure_ascii=False, sort_keys=True) for lane in self.lanes)
        lines.append(
            json.dumps(
                {
                    "event": "hardware_encoding_metrics",
                    "scenario": self.scenario_name,
                    "metrics": self.metrics,
                },
                ensure_ascii=False,
                sort_keys=True,
            )
        )
        return tuple(lines)


def _parse_ffprobe_key_value_output(output: str) -> Dict[str, str]:
    parsed: Dict[str, str] = {}
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if not line or "=" not in line:
            continue
        key, value = line.split("=", 1)
        parsed[key.strip()] = value.strip()
    return parsed


def _parse_int(value: str) -> int:
    try:
        return int(value)
    except (TypeError, ValueError):
        return 0


def _parse_fraction(value: str) -> float:
    try:
        if "/" in value:
            numerator, denominator = value.split("/", 1)
            denominator_value = float(denominator)
            if denominator_value == 0:
                return 0.0
            return float(numerator) / denominator_value
        return float(value)
    except (TypeError, ValueError):
        return 0.0


@dataclass(frozen=True)
class Frame:
    camera_id: str
    seq: int
    width: int
    height: int
    timestamp_ms: int
    pattern: str


class V4L2CameraSource:
    def __init__(self, camera_id: str, device_path: Path | str, width: int, height: int, fps: int) -> None:
        if width <= 0 or height <= 0 or fps <= 0:
            raise ValueError("V4L2 width, height, and fps must be positive")
        path = Path(device_path)
        if not path.exists():
            raise FileNotFoundError(f"V4L2 device not found: {path}")
        self.camera_id = camera_id
        self.device_path = str(path)
        self.width = width
        self.height = height
        self.fps = fps

    def gst_source_fragment(self) -> str:
        return (
            f"v4l2src device={self.device_path} ! "
            f"video/x-raw,width={self.width},height={self.height},framerate={self.fps}/1"
        )


class FileReplaySource:
    def __init__(self, camera_id: str, replay_path: Path | str) -> None:
        self.camera_id = camera_id
        self.replay_path = Path(replay_path)
        self._records = self._load_records()
        self._index = 0

    def read_frame(self, now_ms: int) -> Frame:
        if self._index >= len(self._records):
            raise RuntimeError(f"file replay exhausted for camera {self.camera_id}")
        record = self._records[self._index]
        self._index += 1
        return Frame(
            camera_id=self.camera_id,
            seq=int(record["seq"]),
            width=int(record["width"]),
            height=int(record["height"]),
            timestamp_ms=int(record["timestamp_ms"]),
            pattern=str(record["pattern"]),
        )

    def _load_records(self) -> list[dict[str, object]]:
        if not self.replay_path.exists():
            raise FileNotFoundError(self.replay_path)
        records = []
        for line_number, raw_line in enumerate(self.replay_path.read_text(encoding="utf-8").splitlines(), start=1):
            line = raw_line.strip()
            if not line:
                continue
            record = json.loads(line)
            if not isinstance(record, dict):
                raise ValueError(f"file replay line {line_number} must be a JSON object")
            for field in ("seq", "width", "height", "timestamp_ms", "pattern"):
                if field not in record:
                    raise ValueError(f"file replay line {line_number} missing {field}")
            _replay_non_negative_int(record["seq"], line_number, "seq")
            _replay_positive_int(record["width"], line_number, "width")
            _replay_positive_int(record["height"], line_number, "height")
            _replay_non_negative_int(record["timestamp_ms"], line_number, "timestamp_ms")
            _replay_non_empty_string(record["pattern"], line_number, "pattern")
            records.append(record)
        return records


def _replay_non_negative_int(value: object, line_number: int, field: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise ValueError(f"file replay line {line_number} {field} must be a non-negative integer")


def _replay_positive_int(value: object, line_number: int, field: str) -> None:
    if isinstance(value, bool) or not isinstance(value, int) or value <= 0:
        raise ValueError(f"file replay line {line_number} {field} must be a positive integer")


def _replay_non_empty_string(value: object, line_number: int, field: str) -> None:
    if not isinstance(value, str) or not value:
        raise ValueError(f"file replay line {line_number} {field} must be a non-empty string")


class TestPatternSource:
    def __init__(
        self,
        camera_id: str,
        width: int,
        height: int,
        fps: int,
        fail_after_frames: Optional[int] = None,
    ) -> None:
        self.camera_id = camera_id
        self.width = width
        self.height = height
        self.fps = fps
        self.fail_after_frames = fail_after_frames
        self._seq = 0

    def read_frame(self, now_ms: int) -> Frame:
        if self.fail_after_frames is not None and self._seq >= self.fail_after_frames:
            raise RuntimeError("simulated camera failure")
        self._seq += 1
        return Frame(
            camera_id=self.camera_id,
            seq=self._seq,
            width=self.width,
            height=self.height,
            timestamp_ms=now_ms,
            pattern=f"smpte-bars-{self._seq % 8}",
        )


@dataclass(frozen=True)
class CameraPollResult:
    status: str
    frame: Frame | None = None
    message: str = ""


class MediaSupervisor:
    def __init__(self, sources: Dict[str, TestPatternSource]) -> None:
        self.sources = sources

    def poll_once(self, now_ms: int) -> Dict[str, CameraPollResult]:
        results: Dict[str, CameraPollResult] = {}
        for camera_id, source in self.sources.items():
            try:
                results[camera_id] = CameraPollResult("ok", frame=source.read_frame(now_ms))
            except Exception as exc:
                results[camera_id] = CameraPollResult("error", message=str(exc))
        return results
