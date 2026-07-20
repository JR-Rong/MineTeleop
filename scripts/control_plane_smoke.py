#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path
from typing import Any
from urllib import error, request

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from mine_teleop.config import load_driver_config, load_vehicle_config


def main() -> int:
    parser = argparse.ArgumentParser(description="Run a Docker-local control-plane media/control smoke.")
    parser.add_argument("--signaling-url", default="http://127.0.0.1:8765")
    parser.add_argument("--driver-console-url", default="http://127.0.0.1:8080")
    parser.add_argument("--driver-config", default="configs/driver-console.dev.yaml")
    parser.add_argument("--vehicle-config", default="configs/vehicle-agent.dev.yaml")
    parser.add_argument("--artifact-dir", default="/tmp/mine-teleop-control-smoke")
    parser.add_argument("--wait-timeout-seconds", type=float, default=20.0)
    args = parser.parse_args()

    artifact_dir = Path(args.artifact_dir)
    artifact_dir.mkdir(parents=True, exist_ok=True)

    try:
        summary = run_smoke(
            signaling_url=args.signaling_url.rstrip("/"),
            driver_console_url=args.driver_console_url.rstrip("/"),
            driver_config_path=Path(args.driver_config),
            vehicle_config_path=Path(args.vehicle_config),
            artifact_dir=artifact_dir,
            wait_timeout_seconds=args.wait_timeout_seconds,
        )
    except Exception as exc:
        summary = {
            "event": "control_plane_docker_smoke",
            "passed": False,
            "error": str(exc),
        }
        _write_summary(artifact_dir, summary)
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
        return 1

    _write_summary(artifact_dir, summary)
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    return 0


def run_smoke(
    *,
    signaling_url: str,
    driver_console_url: str,
    driver_config_path: Path,
    vehicle_config_path: Path,
    artifact_dir: Path,
    wait_timeout_seconds: float,
) -> dict[str, Any]:
    _wait_for_health(signaling_url, timeout_seconds=wait_timeout_seconds)
    _wait_for_health(driver_console_url, timeout_seconds=wait_timeout_seconds)

    driver_config = load_driver_config(driver_config_path)
    vehicle_config = load_vehicle_config(vehicle_config_path)
    vehicle_id = vehicle_config.vehicle_id
    driver_id = driver_config.driver_id

    _json_post(
        f"{signaling_url}/vehicles/online",
        {"vehicle_id": vehicle_id, "device_token": "dev-device-secret"},
    )
    console_connected = _json_post(f"{driver_console_url}/api/connect", {})
    session_id = str(console_connected["session"]["session_id"])

    offer_payload = {
        "type": "offer",
        "sdp": (
            "v=0\r\n"
            "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
            "a=rtpmap:102 H264/90000\r\n"
            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
            "a=sctp-port:5000\r\n"
        ),
        "media_tracks": [
            {
                "camera_id": "front",
                "codec": "h264",
                "width": 320,
                "height": 180,
                "fps": 15,
                "bitrate_kbps": 800,
            }
        ],
    }
    queued = _json_post(
        f"{signaling_url}/signaling/{session_id}/messages",
        {
            "sender": vehicle_id,
            "device_token": "dev-device-secret",
            "recipient": driver_id,
            "type": "webrtc_offer",
            "payload": offer_payload,
        },
    )
    if queued.get("queued") != 1:
        raise RuntimeError(f"unexpected signaling queue result: {queued}")

    polled = _json_post(f"{driver_console_url}/api/poll-signaling", {})
    if polled.get("received_messages") != 1:
        raise RuntimeError(f"driver console did not receive exactly one media signaling message: {polled}")
    console_snapshot = polled["snapshot"]
    front_status = console_snapshot["dashboard"]["cameras"]["front"]
    if front_status["state"] != "connected":
        raise RuntimeError(f"driver console did not mark front camera connected: {front_status}")
    if not polled.get("messages") or polled["messages"][0].get("type") != "webrtc_offer":
        raise RuntimeError(f"driver console did not return offer payload for browser WebRTC: {polled}")

    answer = _forward_webrtc_answer_through_driver_console(
        driver_console_url=driver_console_url,
        signaling_url=signaling_url,
        session_id=session_id,
        vehicle_id=vehicle_id,
    )
    ice = _exchange_webrtc_ice_candidates_through_driver_console(
        driver_console_url=driver_console_url,
        signaling_url=signaling_url,
        session_id=session_id,
        vehicle_id=vehicle_id,
        driver_id=driver_id,
    )
    frame = _decode_test_frame(artifact_dir, driver_console_url=driver_console_url)
    console_snapshot = _json_get(f"{driver_console_url}/api/status")
    control = _forward_control_commands_through_driver_console(
        driver_console_url=driver_console_url,
        signaling_url=signaling_url,
        session_id=session_id,
        vehicle_id=vehicle_id,
    )

    return {
        "event": "control_plane_docker_smoke",
        "passed": True,
        "control_console_received_image": frame["control_console_received_image"],
        "vehicle_received_steering": control["vehicle_received_steering"],
        "vehicle_received_acceleration": control["vehicle_received_acceleration"],
        "vehicle_received_deceleration": control["vehicle_received_deceleration"],
        "signaling": {
            "health": "ok",
            "session_id": session_id,
            "media_offer_received": True,
            "webrtc_answer_forwarded": answer["webrtc_answer_forwarded"],
            "remote_ice_candidate_received": ice["remote_ice_candidate_received"],
            "local_ice_candidate_forwarded": ice["local_ice_candidate_forwarded"],
            "queued_messages": queued["queued"],
        },
        "media": {
            "control_console_received_image": frame["control_console_received_image"],
            "frame_received": True,
            "frame_path": str(frame["path"]),
            "frame_size_bytes": frame["size_bytes"],
            "frame_sequences": frame["frame_sequences"],
            "latency": frame["latency"],
            "decoded_frame_count_by_camera": console_snapshot.get("decoded_frame_count_by_camera", {}),
            "dashboard": console_snapshot["dashboard"],
        },
        "control": control,
    }


def _forward_webrtc_answer_through_driver_console(
    *,
    driver_console_url: str,
    signaling_url: str,
    session_id: str,
    vehicle_id: str,
) -> dict[str, Any]:
    response = _json_post(
        f"{driver_console_url}/api/webrtc/answer",
        {
            "type": "answer",
            "sdp": (
                "v=0\r\n"
                "m=video 9 UDP/TLS/RTP/SAVPF 102\r\n"
                "a=rtpmap:102 H264/90000\r\n"
                "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                "a=sctp-port:5000\r\n"
            ),
        },
    )
    if response.get("queued") != 1:
        raise RuntimeError(f"driver console did not queue WebRTC answer: {response}")
    messages = _json_get(
        f"{signaling_url}/signaling/{session_id}/messages?recipient={vehicle_id}&device_token=dev-device-secret"
    )
    answers = [message for message in messages.get("messages", []) if message.get("type") == "webrtc_answer"]
    if len(answers) != 1:
        raise RuntimeError(f"vehicle did not receive one WebRTC answer: {messages}")
    return {"webrtc_answer_forwarded": True}


def _exchange_webrtc_ice_candidates_through_driver_console(
    *,
    driver_console_url: str,
    signaling_url: str,
    session_id: str,
    vehicle_id: str,
    driver_id: str,
) -> dict[str, Any]:
    remote_candidate = {
        "candidate": "candidate:remote 1 udp 2122260223 127.0.0.1 5001 typ host",
        "sdpMid": "0",
        "sdpMLineIndex": 0,
    }
    queued = _json_post(
        f"{signaling_url}/signaling/{session_id}/messages",
        {
            "sender": vehicle_id,
            "device_token": "dev-device-secret",
            "recipient": driver_id,
            "type": "ice_candidate",
            "payload": remote_candidate,
        },
    )
    if queued.get("queued") != 1:
        raise RuntimeError(f"unexpected remote ICE queue result: {queued}")
    polled = _json_post(f"{driver_console_url}/api/poll-signaling", {})
    remote_candidates = [
        message for message in polled.get("messages", []) if message.get("type") == "ice_candidate"
    ]
    if len(remote_candidates) != 1:
        raise RuntimeError(f"driver console did not receive remote ICE candidate: {polled}")

    local_candidate = {
        "candidate": "candidate:local 1 udp 2122260223 127.0.0.1 5002 typ host",
        "sdpMid": "0",
        "sdpMLineIndex": 0,
    }
    response = _json_post(
        f"{driver_console_url}/api/webrtc/ice-candidate",
        {"candidate": local_candidate},
    )
    if response.get("queued") != 1:
        raise RuntimeError(f"driver console did not queue local ICE candidate: {response}")
    messages = _json_get(
        f"{signaling_url}/signaling/{session_id}/messages?recipient={vehicle_id}&device_token=dev-device-secret"
    )
    local_candidates = [
        message for message in messages.get("messages", []) if message.get("type") == "ice_candidate"
    ]
    if len(local_candidates) != 1:
        raise RuntimeError(f"vehicle did not receive local ICE candidate: {messages}")
    return {
        "remote_ice_candidate_received": True,
        "local_ice_candidate_forwarded": True,
    }


def _forward_control_commands_through_driver_console(
    *,
    driver_console_url: str,
    signaling_url: str,
    session_id: str,
    vehicle_id: str,
) -> dict[str, Any]:
    for now_ms in (0, 50, 100):
        response = _json_post(
            f"{driver_console_url}/api/control",
            {"gear": "D", "throttle": 0.5, "steering": 0.0, "brake": 0.0, "now_ms": now_ms},
        )
        if not response.get("sent"):
            raise RuntimeError(f"driver console did not send control at {now_ms} ms: {response}")
    for now_ms, payload in (
        (150, {"steering_axis": -0.25, "throttle_axis": 0.6, "brake_axis": 0.0}),
        (200, {"steering_axis": 0.1, "throttle_axis": 0.0, "brake_axis": 0.35}),
    ):
        response = _json_post(
            f"{driver_console_url}/api/control/gamepad",
            {"gear": "D", "now_ms": now_ms, **payload},
        )
        if not response.get("sent"):
            raise RuntimeError(f"driver console did not send gamepad control at {now_ms} ms: {response}")
    messages = _json_get(
        f"{signaling_url}/signaling/{session_id}/messages?recipient={vehicle_id}&device_token=dev-device-secret"
    )
    control_messages = [message for message in messages.get("messages", []) if message.get("type") == "control_command"]
    if len(control_messages) != 5:
        raise RuntimeError(f"vehicle did not receive five control commands: {messages}")
    payloads = [message["payload"] for message in control_messages]
    summary = _summarize_control_payloads(payloads, software_count=3, gamepad_count=2)
    if not summary["vehicle_received_steering"]:
        raise RuntimeError(f"vehicle did not receive a steering command: {payloads}")
    if not summary["vehicle_received_acceleration"]:
        raise RuntimeError(f"vehicle did not receive an acceleration command: {payloads}")
    if not summary["vehicle_received_deceleration"]:
        raise RuntimeError(f"vehicle did not receive a deceleration command: {payloads}")
    return summary


def _summarize_control_payloads(
    payloads: list[dict[str, Any]], *, software_count: int, gamepad_count: int
) -> dict[str, Any]:
    steering_values = [payload["steering"] for payload in payloads]
    throttle_values = [payload["throttle"] for payload in payloads]
    brake_values = [payload["brake"] for payload in payloads]
    return {
        "commands_generated": len(payloads),
        "software_control_commands": software_count,
        "gamepad_control_commands": gamepad_count,
        "commands_forwarded": len(payloads),
        "vehicle_received_commands": len(payloads),
        "seq": [payload["seq"] for payload in payloads],
        "steering_values": steering_values,
        "throttle_values": throttle_values,
        "brake_values": brake_values,
        "vehicle_received_steering": any(abs(value) > 0.0 for value in steering_values),
        "vehicle_received_acceleration": any(value > 0.0 for value in throttle_values),
        "vehicle_received_deceleration": any(value > 0.0 for value in brake_values),
        "last_gamepad_command": {
            "steering": payloads[-1]["steering"],
            "throttle": payloads[-1]["throttle"],
            "brake": payloads[-1]["brake"],
        },
        "authority_token_present": all(bool(payload.get("authority_token")) for payload in payloads),
    }


def _decode_test_frame(artifact_dir: Path, *, driver_console_url: str) -> dict[str, Any]:
    ffmpeg = shutil.which("ffmpeg")
    if not ffmpeg:
        raise RuntimeError("ffmpeg is required in the control-plane smoke container")
    encoded_path = artifact_dir / "front-source.h264"
    frame_path = artifact_dir / "front-received.png"
    subprocess.run(
        [
            ffmpeg,
            "-hide_banner",
            "-loglevel",
            "error",
            "-y",
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=320x180:rate=1",
            "-frames:v",
            "1",
            "-c:v",
            "libx264",
            "-preset",
            "ultrafast",
            "-tune",
            "zerolatency",
            "-f",
            "h264",
            str(encoded_path),
        ],
        check=True,
    )
    encoded_base64 = base64.b64encode(encoded_path.read_bytes()).decode("ascii")
    decoded_results: list[dict[str, Any]] = []
    for expected_sequence in (1, 2):
        captured_at_ms = int(time.time() * 1000) - 100
        encoded_at_ms = captured_at_ms + 20
        sent_at_ms = int(time.time() * 1000)
        decoded = _json_post(
            f"{driver_console_url}/api/media/frame",
            {
                "camera_id": "front",
                "codec": "h264",
                "payload_base64": encoded_base64,
                "captured_at_ms": captured_at_ms,
                "encoded_at_ms": encoded_at_ms,
                "sent_at_ms": sent_at_ms,
            },
        )
        if not decoded.get("frame_received"):
            raise RuntimeError(f"driver console did not decode posted frame: {decoded}")
        if decoded.get("frame_sequence") != expected_sequence:
            raise RuntimeError(f"unexpected decoded frame sequence {decoded}")
        decoded_results.append(decoded)
    console_status = _json_get(f"{driver_console_url}/api/status")
    decoded_count = console_status.get("decoded_frame_count_by_camera", {}).get("front")
    if decoded_count != 2:
        raise RuntimeError(f"driver console did not record two decoded front frames: {console_status}")
    frame_path.write_bytes(_bytes_get(f"{driver_console_url}/api/frame/front.png"))
    size_bytes = frame_path.stat().st_size
    if size_bytes <= 0:
        raise RuntimeError("decoded frame is empty")
    return {
        "control_console_received_image": True,
        "path": frame_path,
        "size_bytes": size_bytes,
        "frame_sequences": [decoded["frame_sequence"] for decoded in decoded_results],
        "latency": {
            "end_to_end_latency_ms": decoded_results[-1].get("end_to_end_latency_ms", 0),
            "transport_latency_ms": decoded_results[-1].get("transport_latency_ms", 0),
            "decode_latency_ms": decoded_results[-1].get("decode_latency_ms", 0),
            "encode_latency_ms": decoded_results[-1].get("encode_latency_ms", 0),
        },
    }


def _wait_for_health(signaling_url: str, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            health = _json_get(f"{signaling_url}/health")
            if health.get("status") == "ok":
                return
        except Exception as exc:  # wait loop reports the last health error
            last_error = str(exc)
        time.sleep(0.2)
    raise RuntimeError(f"signaling health did not become ok: {last_error}")


def _json_get(url: str) -> dict[str, Any]:
    with request.urlopen(url, timeout=5) as response:
        return _decode_response(response.read())


def _bytes_get(url: str) -> bytes:
    with request.urlopen(url, timeout=5) as response:
        return response.read()


def _json_post(url: str, payload: dict[str, Any]) -> dict[str, Any]:
    body = json.dumps(payload).encode("utf-8")
    req = request.Request(url, data=body, method="POST", headers={"Content-Type": "application/json"})
    try:
        with request.urlopen(req, timeout=5) as response:
            return _decode_response(response.read())
    except error.HTTPError as exc:
        try:
            payload = _decode_response(exc.read())
        except Exception:
            payload = {"error": exc.reason}
        raise RuntimeError(f"POST {url} failed with {exc.code}: {payload}") from exc


def _decode_response(body: bytes) -> dict[str, Any]:
    data = json.loads(body.decode("utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError("expected JSON object response")
    return data


def _write_summary(artifact_dir: Path, summary: dict[str, Any]) -> None:
    artifact_dir.mkdir(parents=True, exist_ok=True)
    (artifact_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


if __name__ == "__main__":
    raise SystemExit(main())
