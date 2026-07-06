#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.config import load_vehicle_config
from mine_teleop.media import (
    EncoderChoice,
    FFmpegVaapiProbePlan,
    GStreamerPipelineBuilder,
    GStreamerPluginProbePlan,
    HardwareEncodingValidationPlan,
    HardwareEncodingValidationReport,
)
from mine_teleop.vehicle_media_runtime import (
    DriverConsoleFrameSink,
    FfmpegH264FrameEncoder,
    MjpegFrameEncoder,
    UploadApiClient,
    VehicleMediaRuntime,
)


def main() -> int:
    parser = argparse.ArgumentParser(description="Print Mine Teleop media-agent development plans.")
    parser.add_argument("--config", default="configs/vehicle-agent.dev.yaml")
    parser.add_argument(
        "--mode",
        choices=["pipeline", "vaapi-probe", "gst-probe", "hardware-probes", "hardware-report", "teleop"],
        default="pipeline",
    )
    parser.add_argument(
        "--probe-execution",
        choices=["host", "docker"],
        default="host",
        help="Emit host/bundle media probe commands by default; docker is for build-host development checks.",
    )
    parser.add_argument("--lanes", type=int, default=4, help="Number of parallel VAAPI probe lanes.")
    parser.add_argument("--scenario", help="Hardware validation scenario name for --mode hardware-report.")
    parser.add_argument(
        "--ffprobe-output",
        action="append",
        default=[],
        help="Lane ffprobe output mapping for --mode hardware-report, formatted as lane_id=/path/to/ffprobe.txt.",
    )
    parser.add_argument("--metrics-json", help="Optional JSON file with CPU/GPU/memory/disk/temperature metrics.")
    parser.add_argument("--driver-console-url", default="", help="Driver console HTTP URL for --mode teleop.")
    parser.add_argument("--frames", type=int, default=1, help="Number of frame ticks to send in --mode teleop.")
    parser.add_argument("--frame-interval-ms", type=int, default=33, help="Delay between frame ticks in --mode teleop.")
    parser.add_argument(
        "--frame-codec",
        choices=["h264", "mjpeg"],
        default="h264",
        help="Encoded frame codec for --mode teleop. mjpeg is lower latency for browser preview.",
    )
    parser.add_argument("--ffmpeg-binary", default="", help="Override ffmpeg binary for --mode teleop.")
    parser.add_argument(
        "--stream",
        action="store_true",
        help="Stream frames with a concurrent capture/send pipeline (drops stale frames) for --mode teleop.",
    )
    parser.add_argument(
        "--duration-ms",
        type=int,
        default=0,
        help="With --stream, stream for this many milliseconds (0 falls back to --frames).",
    )
    parser.add_argument("--json", action="store_true", help="Emit --mode teleop summary as JSON.")
    parser.add_argument("--record-upload-once", action="store_true", help="Record and upload the last encoded frame.")
    parser.add_argument("--recording-root", default="", help="Recording root for --record-upload-once.")
    parser.add_argument("--uploader-work-dir", default=".local/vehicle-media-uploader", help="Uploader work dir for --record-upload-once.")
    parser.add_argument("--archive-root", default="", help="Archive root for --record-upload-once.")
    parser.add_argument("--upload-api-base-url", default="", help="Upload API base URL for --record-upload-once.")
    parser.add_argument("--device-token", default="dev-device-secret", help="Vehicle device token for upload API calls.")
    args = parser.parse_args()

    config = load_vehicle_config(args.config)
    if args.mode == "teleop":
        return _run_teleop(config, args)
    encoding = config.hardware.encoding
    gstreamer_probe = GStreamerPluginProbePlan(
        hardware_plugins=tuple(encoding.gstreamer_hardware_plugins),
        fallback_plugins=tuple(encoding.gstreamer_fallback_plugins),
    )
    if args.mode == "pipeline":
        builder = GStreamerPipelineBuilder()
        for camera in config.enabled_cameras:
            profile = config.realtime_profiles[camera.realtime_profile]
            pipeline = builder.realtime_h264_pipeline(
                source_device=camera.device,
                width=int(profile.width),
                height=int(profile.height),
                fps=int(profile.fps),
                bitrate_kbps=profile.bitrate_kbps,
                encoder=EncoderChoice(profile.encoder, "configured"),
                keyframe_interval_frames=profile.keyframe_interval_frames or 30,
                encoder_name=f"{camera.camera_id}_realtime_encoder",
            )
            print(f"camera={camera.camera_id}")
            print(pipeline)
        return 0

    if args.mode == "gst-probe":
        print(gstreamer_probe.command)
        return 0

    if args.mode == "hardware-probes":
        plan = _hardware_encoding_plan_from_config(config, gstreamer_probe)
        print(f"gst_plugin_probe={plan.gstreamer_plugin_probe.command}")
        for scenario in plan.scenarios:
            print(f"scenario={scenario.name}")
            print(scenario.host_command() if args.probe_execution == "host" else scenario.docker_command())
        print(f"metrics={','.join(plan.metrics_fields)}")
        return 0

    if args.mode == "hardware-report":
        plan = _hardware_encoding_plan_from_config(config, gstreamer_probe)
        scenario = _find_scenario(plan, args.scenario)
        if scenario is None:
            print(f"unknown hardware validation scenario: {args.scenario}", file=sys.stderr)
            return 2
        ffprobe_outputs = _read_ffprobe_output_args(args.ffprobe_output)
        metrics = _read_metrics(args.metrics_json)
        report = HardwareEncodingValidationReport.from_ffprobe_outputs(scenario, ffprobe_outputs, metrics)
        for line in report.to_jsonl():
            print(line)
        return 0 if report.passed else 2

    probe = FFmpegVaapiProbePlan(
        render_device=encoding.vaapi_render_device,
        card_device=encoding.dri_card_device,
        output_dir=encoding.ffmpeg_probe_output_dir,
        lanes=args.lanes,
        width=1280,
        height=720,
        fps=30,
        duration_seconds=encoding.validation_duration_seconds,
        bitrate="4M",
        ffmpeg_binary=encoding.ffmpeg_binary,
        ffprobe_binary=encoding.ffprobe_binary,
        vainfo_binary=encoding.vainfo_binary,
        libva_drivers_path=encoding.libva_drivers_path,
    )
    print(probe.host_command() if args.probe_execution == "host" else probe.docker_command())
    return 0


def _run_teleop(config, args: argparse.Namespace) -> int:
    if not args.driver_console_url:
        print("--driver-console-url is required for --mode teleop", file=sys.stderr)
        return 2
    if args.frames <= 0:
        print("--frames must be positive", file=sys.stderr)
        return 2
    if args.frame_interval_ms < 0:
        print("--frame-interval-ms must be non-negative", file=sys.stderr)
        return 2
    ffmpeg_binary = args.ffmpeg_binary or config.hardware.encoding.ffmpeg_binary
    if args.frame_codec == "mjpeg":
        encoder = MjpegFrameEncoder(config, ffmpeg_binary=ffmpeg_binary)
    else:
        encoder = FfmpegH264FrameEncoder(config, ffmpeg_binary=ffmpeg_binary)
    runtime = VehicleMediaRuntime(config, frame_sink=DriverConsoleFrameSink(args.driver_console_url), encoder=encoder)
    try:
        if args.stream:
            if args.duration_ms > 0:
                summary = runtime.stream_frames(duration_ms=args.duration_ms)
            else:
                summary = runtime.stream_frames(frame_count=args.frames)
        else:
            summary = runtime.send_frames(frame_count=args.frames, frame_interval_ms=args.frame_interval_ms)
    finally:
        runtime.close()
    if args.record_upload_once:
        if runtime.last_frame is None:
            print("cannot record upload before a frame has been encoded", file=sys.stderr)
            return 2
        if not args.upload_api_base_url:
            print("--upload-api-base-url is required with --record-upload-once", file=sys.stderr)
            return 2
        uploader_work_dir = Path(args.uploader_work_dir)
        recording_root = Path(args.recording_root) if args.recording_root else Path(config.recording.root_dir)
        archive_root = Path(args.archive_root) if args.archive_root else uploader_work_dir / "archive"
        summary["recording_upload"] = runtime.record_and_upload_once(
            runtime.last_frame,
            recording_root=recording_root,
            queue_state_path=uploader_work_dir / "upload-queue.json",
            archive_root=archive_root,
            upload_api=UploadApiClient(args.upload_api_base_url, device_token=args.device_token),
        )
        summary["passed"] = bool(summary["passed"] and summary["recording_upload"]["action"] != "failed")
    if args.json:
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    else:
        print(
            f"vehicle_media_teleop sent_frames={summary['sent_frames']} "
            f"passed={str(summary['passed']).lower()} "
            f"latency_avg_ms={summary['latency']['end_to_end_latency_ms_avg']}"
        )
    return 0 if summary["passed"] else 1


def _hardware_encoding_plan_from_config(config, gstreamer_probe: GStreamerPluginProbePlan) -> HardwareEncodingValidationPlan:
    encoding = config.hardware.encoding
    return HardwareEncodingValidationPlan.four_camera_default(
        render_device=encoding.vaapi_render_device,
        card_device=encoding.dri_card_device,
        output_dir=encoding.ffmpeg_probe_output_dir,
        duration_seconds=encoding.validation_duration_seconds,
        gstreamer_plugin_probe=gstreamer_probe,
        ffmpeg_binary=encoding.ffmpeg_binary,
        ffprobe_binary=encoding.ffprobe_binary,
        vainfo_binary=encoding.vainfo_binary,
        libva_drivers_path=encoding.libva_drivers_path,
    )


def _find_scenario(plan: HardwareEncodingValidationPlan, scenario_name: str | None):
    if not scenario_name:
        return None
    for scenario in plan.scenarios:
        if scenario.name == scenario_name:
            return scenario
    return None


def _read_ffprobe_output_args(entries: list[str]) -> dict[str, str]:
    outputs: dict[str, str] = {}
    for entry in entries:
        if "=" not in entry:
            continue
        lane_id, path = entry.split("=", 1)
        outputs[lane_id] = Path(path).read_text(encoding="utf-8")
    return outputs


def _read_metrics(path: str | None) -> dict[str, object]:
    if not path:
        return {}
    return json.loads(Path(path).read_text(encoding="utf-8"))


if __name__ == "__main__":
    raise SystemExit(main())
