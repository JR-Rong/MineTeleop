#!/usr/bin/env python3
from __future__ import annotations

import argparse
import base64
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any
from urllib import request
from urllib.parse import quote, urlparse


def main() -> int:
    parser = argparse.ArgumentParser(description="Exercise the Docker control-plane operator page in a real browser.")
    parser.add_argument("--driver-console-url", default="http://127.0.0.1:8080")
    parser.add_argument(
        "--signaling-container",
        default="",
        help="Docker container name for the loopback-only signaling-server used by the browser smoke.",
    )
    parser.add_argument("--chrome-binary", default="", help="Override Chrome/Chromium binary path.")
    parser.add_argument("--artifact-dir", default=".local/control-plane-browser-smoke")
    parser.add_argument("--wait-timeout-seconds", type=float, default=20.0)
    args = parser.parse_args()

    artifact_dir = Path(args.artifact_dir)
    artifact_dir.mkdir(parents=True, exist_ok=True)

    try:
        summary = run_browser_smoke(
            driver_console_url=args.driver_console_url.rstrip("/"),
            signaling_container=args.signaling_container,
            chrome_binary=args.chrome_binary,
            artifact_dir=artifact_dir,
            wait_timeout_seconds=args.wait_timeout_seconds,
        )
    except Exception as exc:
        summary = {"event": "browser_control_plane_smoke", "passed": False, "error": str(exc)}
        _write_json(artifact_dir / "browser-smoke-summary.json", summary)
        print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
        return 1

    _write_json(artifact_dir / "browser-smoke-summary.json", summary)
    print(json.dumps(summary, ensure_ascii=False, sort_keys=True))
    return 0


def run_browser_smoke(
    *,
    driver_console_url: str,
    signaling_container: str,
    chrome_binary: str,
    artifact_dir: Path,
    wait_timeout_seconds: float,
) -> dict[str, Any]:
    _wait_for_health(driver_console_url, timeout_seconds=wait_timeout_seconds)
    chrome_path = chrome_binary or _find_chrome_binary()
    with tempfile.TemporaryDirectory(prefix="mine-teleop-chrome-") as user_data_dir:
        remote_debugging_port = _free_port()
        proc = subprocess.Popen(
            [
                chrome_path,
                "--headless=new",
                "--disable-background-networking",
                "--disable-dev-shm-usage",
                "--disable-gpu",
                "--no-default-browser-check",
                "--no-first-run",
                f"--remote-debugging-port={remote_debugging_port}",
                f"--user-data-dir={user_data_dir}",
                "about:blank",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            ws_url = _open_devtools_target(remote_debugging_port, driver_console_url, wait_timeout_seconds)
            cdp = ChromeDevToolsConnection(ws_url, timeout_seconds=wait_timeout_seconds)
            vehicle_cdp = None
            try:
                cdp.command("Page.enable")
                cdp.command("Runtime.enable")
                cdp.command("Page.navigate", {"url": driver_console_url})
                _wait_for_page_ready(cdp, timeout_seconds=wait_timeout_seconds)
                result = _exercise_operator_page(cdp)
                if signaling_container:
                    vehicle_ws_url = _open_devtools_target(remote_debugging_port, "about:blank", wait_timeout_seconds)
                    vehicle_cdp = ChromeDevToolsConnection(vehicle_ws_url, timeout_seconds=wait_timeout_seconds)
                    vehicle_cdp.command("Page.enable")
                    vehicle_cdp.command("Runtime.enable")
                    _wait_for_page_ready(vehicle_cdp, timeout_seconds=wait_timeout_seconds)
                    loopback = _exercise_webrtc_via_signaling(
                        driver_cdp=cdp,
                        vehicle_cdp=vehicle_cdp,
                        signaling_container=signaling_container,
                        session_id=result["sessionId"],
                        driver_id=result["driverId"],
                        vehicle_id=result["vehicleId"],
                        wait_timeout_seconds=wait_timeout_seconds,
                    )
                else:
                    loopback = _exercise_webrtc_loopback_peer(cdp)
                operator_panel = _driver_operator_panel_summary(cdp)
                screenshot = cdp.command("Page.captureScreenshot", {"format": "png", "captureBeyondViewport": True})
                screenshot_path = artifact_dir / "operator-page.png"
                screenshot_path.write_bytes(base64.b64decode(screenshot["data"]))
            finally:
                if vehicle_cdp is not None:
                    vehicle_cdp.close()
                cdp.close()
        finally:
            proc.terminate()
            try:
                proc.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate(timeout=5)

    summary = {
        "event": "browser_control_plane_smoke",
        "passed": True,
        "driver_console_url": driver_console_url,
        "chrome_binary": chrome_path,
        "session_state": result["sessionState"],
        "keyboard_control_command_sent": result["keyboardControlCommandSent"],
        "software_control_command_sent": result["softwareControlCommandSent"],
        "control_command_sent": result["control_command_sent"],
        "last_command_seq": result["lastCommand"]["seq"],
        "last_command_gear": result["lastCommand"]["gear"],
        "webrtc_available": result["webrtcAvailable"],
        "gamepad_mapping_present": result["gamepadMappingPresent"],
        "vehicle_peer_offer_forwarded_via_signaling": loopback["vehicle_peer_offer_forwarded_via_signaling"],
        "webrtc_answer_received_via_signaling": loopback["webrtc_answer_received_via_signaling"],
        "local_ice_candidate_received_via_signaling": loopback["local_ice_candidate_received_via_signaling"],
        "remote_ice_candidate_forwarded_via_signaling": loopback["remote_ice_candidate_forwarded_via_signaling"],
        "loopback_webrtc_media_received": loopback["loopback_webrtc_media_received"],
        "datachannel_control_command_received": loopback["datachannel_control_command_received"],
        "datachannel_last_command_seq": loopback["datachannel_last_command_seq"],
        "datachannel_last_command_gear": loopback["datachannel_last_command_gear"],
        "driver_video_element_attached": loopback["driver_video_element_attached"],
        "vehicle_peer_connection_state": loopback["vehicle_peer_connection_state"],
        "webrtc_loopback_transport": "signaling_queue" if signaling_container else "in_page_direct",
        "footer": result["footer"],
        "operator_connect_form_vehicle_id": result["operatorConnectFormVehicleId"],
        "operator_session_state_text": operator_panel["operator_session_state_text"],
        "operator_control_authority_text": operator_panel["operator_control_authority_text"],
        "operator_signaling_state_text": operator_panel["operator_signaling_state_text"],
        "operator_camera_summary_text": operator_panel["operator_camera_summary_text"],
        "operator_command_summary_text": operator_panel["operator_command_summary_text"],
        "operator_webrtc_state_text": operator_panel["operator_webrtc_state_text"],
        "operator_datachannel_state_text": operator_panel["operator_datachannel_state_text"],
        "screenshot_path": str(screenshot_path),
    }
    if summary["session_state"] != "SESSION_ACTIVE":
        raise RuntimeError(f"operator page did not enter SESSION_ACTIVE: {summary}")
    if not summary["keyboard_control_command_sent"]:
        raise RuntimeError(f"operator page did not send keyboard control: {summary}")
    if not summary["software_control_command_sent"]:
        raise RuntimeError(f"operator page did not send software control: {summary}")
    if not summary["loopback_webrtc_media_received"]:
        raise RuntimeError(f"operator page did not attach a loopback WebRTC media track: {summary}")
    if not summary["datachannel_control_command_received"]:
        raise RuntimeError(f"operator page did not deliver control over WebRTC DataChannel: {summary}")
    if summary["operator_session_state_text"] != "SESSION_ACTIVE":
        raise RuntimeError(f"operator status panel did not show active session: {summary}")
    if summary["operator_control_authority_text"] != "active":
        raise RuntimeError(f"operator status panel did not show active control authority: {summary}")
    if not summary["operator_camera_summary_text"].startswith("1/4 connected"):
        raise RuntimeError(f"operator status panel did not show connected camera count: {summary}")
    if "seq" not in summary["operator_command_summary_text"]:
        raise RuntimeError(f"operator status panel did not show last control command: {summary}")
    if summary["operator_datachannel_state_text"] in {"closed", "connecting"}:
        raise RuntimeError(f"operator status panel did not show DataChannel activity: {summary}")
    return summary


def _exercise_operator_page(cdp: "ChromeDevToolsConnection") -> dict[str, Any]:
    expression = r"""
    (async () => {
      const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
      if (typeof connectConsole !== 'function') throw new Error('connectConsole() is missing');
      if (typeof sendControl !== 'function') throw new Error('sendControl() is missing');
      if (typeof sendKeyboardControl !== 'function') throw new Error('sendKeyboardControl() is missing');
      document.getElementById('connect-vehicle-id').value = 'vehicle-001';
      document.getElementById('connect-password').value = 'dev-password';
      await connectConsole();
      await wait(100);
      const connected = await (await fetch('/api/status')).json();
      if (connected.session.state !== 'SESSION_ACTIVE') {
        throw new Error(`expected SESSION_ACTIVE, got ${connected.session.state}`);
      }
      keyboardControlEnabled = false;
      const keyboardResponse = await postJson('/api/control/keyboard', {
        keys: ['W', 'D'],
        gear: 'D',
        now_ms: Date.now() + 1000
      });
      if (keyboardResponse.sent) sendCommandOverDataChannel(keyboardResponse.command);
      const keyboardStatus = keyboardResponse.snapshot;
      await wait(80);
      await sendControl({gear:'D', steering:0.1, throttle:0.25, brake:0.0, now_ms: Date.now() + 2000});
      await wait(80);
      const finalStatus = await (await fetch('/api/status')).json();
      const keyboardCommand = keyboardStatus.last_command || {};
      const lastCommand = finalStatus.last_command || {};
      return {
        title: document.title,
        footer: document.getElementById('footer').textContent,
        operatorConnectFormVehicleId: document.getElementById('connect-vehicle-id').value,
        sessionState: finalStatus.session.state,
        sessionId: finalStatus.session.session_id,
        driverId: finalStatus.driver_id,
        vehicleId: finalStatus.vehicle_id,
        keyboardControlCommandSent: keyboardCommand.type === 'control_command' && keyboardCommand.throttle === 1 && keyboardCommand.steering === 1,
        softwareControlCommandSent: lastCommand.type === 'control_command' && lastCommand.gear === 'D' && lastCommand.throttle === 0.25 && lastCommand.steering === 0.1,
        control_command_sent: lastCommand.type === 'control_command',
        lastCommand,
        webrtcAvailable: typeof RTCPeerConnection === 'function',
        gamepadMappingPresent: typeof gamepadMapping === 'object' && gamepadMapping.enabled === true
      };
    })()
    """
    value = cdp.evaluate(expression, await_promise=True)
    if not isinstance(value, dict):
        raise RuntimeError(f"unexpected browser evaluation result: {value!r}")
    return value


def _exercise_webrtc_loopback_peer(cdp: "ChromeDevToolsConnection") -> dict[str, Any]:
    expression = r"""
    (async () => {
      const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
      const waitFor = async (predicate, label, timeoutMs = 8000) => {
        const deadline = Date.now() + timeoutMs;
        while (Date.now() < deadline) {
          const value = predicate();
          if (value) return value;
          await wait(50);
        }
        throw new Error(`timed out waiting for ${label}`);
      };
      if (typeof startWebRtcFromOffer !== 'function') throw new Error('startWebRtcFromOffer is missing');
      if (typeof sendControl !== 'function') throw new Error('sendControl is missing');
      if (typeof RTCPeerConnection !== 'function') throw new Error('RTCPeerConnection is missing');
      const canvas = document.createElement('canvas');
      canvas.width = 320;
      canvas.height = 180;
      const ctx = canvas.getContext('2d');
      ctx.fillStyle = '#102030';
      ctx.fillRect(0, 0, canvas.width, canvas.height);
      ctx.fillStyle = '#f0c040';
      ctx.font = '24px sans-serif';
      ctx.fillText('vehicle-front', 32, 92);
      const stream = canvas.captureStream(5);
      const vehiclePeer = new RTCPeerConnection({});
      let receivedDataChannel = null;
      const receivedCommands = [];
      vehiclePeer.ondatachannel = (event) => {
        receivedDataChannel = event.channel;
        receivedDataChannel.onmessage = (message) => {
          receivedCommands.push(JSON.parse(message.data));
        };
      };
      vehiclePeer.createDataChannel('vehicle-negotiation');
      for (const track of stream.getTracks()) {
        vehiclePeer.addTrack(track, stream);
      }
      const originalPostJson = postJson;
      postJson = async (path, payload) => {
        if (path === '/api/webrtc/ice-candidate' && payload && payload.candidate) {
          try {
            await vehiclePeer.addIceCandidate(new RTCIceCandidate(payload.candidate));
          } catch (err) {
            if (!String(err).includes('remoteDescription')) throw err;
          }
        }
        return originalPostJson(path, payload);
      };
      vehiclePeer.onicecandidate = async (event) => {
        if (event.candidate && peerConnection) {
          await peerConnection.addIceCandidate(event.candidate);
        }
      };
      const offer = await vehiclePeer.createOffer();
      await vehiclePeer.setLocalDescription(offer);
      await startWebRtcFromOffer({
        type: vehiclePeer.localDescription.type,
        sdp: vehiclePeer.localDescription.sdp,
        media_tracks: [{camera_id:'front', codec:'h264', width:320, height:180, fps:5, bitrate_kbps:400}]
      });
      await vehiclePeer.setRemoteDescription(peerConnection.localDescription);
      await waitFor(() => {
        const video = document.getElementById('webrtc-video-front');
        return video && video.srcObject && video.srcObject.getVideoTracks().length > 0;
      }, 'driver video element');
      await waitFor(() => receivedDataChannel && receivedDataChannel.readyState === 'open', 'vehicle data channel open');
      await waitFor(() => controlDataChannel && controlDataChannel.readyState === 'open', 'driver data channel open');
      await wait(120);
      await sendControl({gear:'D', steering:0.2, throttle:0.3, brake:0.0, now_ms: Date.now() + 1000});
      await waitFor(() => receivedCommands.length > 0, 'data channel command');
      const video = document.getElementById('webrtc-video-front');
      const command = receivedCommands[receivedCommands.length - 1];
      return {
        vehicle_peer_offer_forwarded_via_signaling: false,
        webrtc_answer_received_via_signaling: false,
        local_ice_candidate_received_via_signaling: false,
        remote_ice_candidate_forwarded_via_signaling: false,
        loopback_webrtc_media_received: Boolean(video && video.srcObject && video.srcObject.getVideoTracks().length > 0),
        driver_video_element_attached: Boolean(video),
        datachannel_control_command_received: command && command.type === 'control_command',
        datachannel_last_command_seq: command ? command.seq : null,
        datachannel_last_command_gear: command ? command.gear : null,
        vehicle_peer_connection_state: vehiclePeer.connectionState,
      };
    })()
    """
    value = cdp.evaluate(expression, await_promise=True)
    if not isinstance(value, dict):
        raise RuntimeError(f"unexpected browser WebRTC loopback result: {value!r}")
    return value


def _exercise_webrtc_via_signaling(
    *,
    driver_cdp: "ChromeDevToolsConnection",
    vehicle_cdp: "ChromeDevToolsConnection",
    signaling_container: str,
    session_id: str,
    driver_id: str,
    vehicle_id: str,
    wait_timeout_seconds: float,
) -> dict[str, Any]:
    offer_payload = _vehicle_create_offer(vehicle_cdp)
    queued = _signaling_post(
        signaling_container,
        f"/signaling/{session_id}/messages",
        {
            "sender": vehicle_id,
            "device_token": "dev-device-secret",
            "recipient": driver_id,
            "type": "webrtc_offer",
            "payload": offer_payload,
        },
    )
    vehicle_peer_offer_forwarded_via_signaling = queued.get("queued") == 1
    if not vehicle_peer_offer_forwarded_via_signaling:
        raise RuntimeError(f"vehicle offer was not queued through signaling: {queued}")

    _driver_poll_signaling(driver_cdp)
    deadline = time.monotonic() + wait_timeout_seconds
    webrtc_answer_received_via_signaling = False
    local_ice_candidate_received_via_signaling = False
    remote_ice_candidate_forwarded_via_signaling = False
    control_command_sent = False
    last_summary: dict[str, Any] = {}
    while time.monotonic() < deadline:
        for candidate in _vehicle_take_local_ice_candidates(vehicle_cdp):
            result = _signaling_post(
                signaling_container,
                f"/signaling/{session_id}/messages",
                {
                    "sender": vehicle_id,
                    "device_token": "dev-device-secret",
                    "recipient": driver_id,
                    "type": "ice_candidate",
                    "payload": candidate,
                },
            )
            if result.get("queued") == 1:
                remote_ice_candidate_forwarded_via_signaling = True
        if remote_ice_candidate_forwarded_via_signaling:
            _driver_poll_signaling(driver_cdp)

        vehicle_messages = _signaling_get(
            signaling_container,
            f"/signaling/{session_id}/messages?recipient={vehicle_id}&device_token=dev-device-secret",
        )
        for message in vehicle_messages.get("messages", []):
            if not isinstance(message, dict):
                continue
            payload = message.get("payload", {})
            if message.get("type") == "webrtc_answer" and isinstance(payload, dict):
                _vehicle_accept_answer(vehicle_cdp, payload)
                webrtc_answer_received_via_signaling = True
            if message.get("type") == "ice_candidate" and isinstance(payload, dict):
                _vehicle_add_ice_candidate(vehicle_cdp, payload)
                local_ice_candidate_received_via_signaling = True

        last_summary = _vehicle_webrtc_summary(vehicle_cdp)
        driver_summary = _driver_webrtc_summary(driver_cdp)
        if (
            webrtc_answer_received_via_signaling
            and driver_summary.get("driver_video_element_attached")
            and last_summary.get("vehicle_datachannel_open")
            and driver_summary.get("driver_datachannel_open")
            and not control_command_sent
        ):
            _driver_send_datachannel_control(driver_cdp)
            control_command_sent = True

        last_summary = _vehicle_webrtc_summary(vehicle_cdp)
        if control_command_sent and last_summary.get("datachannel_control_command_received"):
            return {
                "vehicle_peer_offer_forwarded_via_signaling": vehicle_peer_offer_forwarded_via_signaling,
                "webrtc_answer_received_via_signaling": webrtc_answer_received_via_signaling,
                "local_ice_candidate_received_via_signaling": local_ice_candidate_received_via_signaling,
                "remote_ice_candidate_forwarded_via_signaling": remote_ice_candidate_forwarded_via_signaling,
                "loopback_webrtc_media_received": bool(driver_summary.get("loopback_webrtc_media_received")),
                "driver_video_element_attached": bool(driver_summary.get("driver_video_element_attached")),
                "datachannel_control_command_received": True,
                "datachannel_last_command_seq": last_summary.get("datachannel_last_command_seq"),
                "datachannel_last_command_gear": last_summary.get("datachannel_last_command_gear"),
                "vehicle_peer_connection_state": last_summary.get("vehicle_peer_connection_state", ""),
            }
        time.sleep(0.1)
    raise RuntimeError(
        "timed out waiting for signaling WebRTC loopback "
        + json.dumps(
            {
                "answer": webrtc_answer_received_via_signaling,
                "driver_ice": local_ice_candidate_received_via_signaling,
                "vehicle_ice": remote_ice_candidate_forwarded_via_signaling,
                "vehicle": last_summary,
                "driver": _driver_webrtc_summary(driver_cdp),
            },
            sort_keys=True,
        )
    )


def _vehicle_create_offer(vehicle_cdp: "ChromeDevToolsConnection") -> dict[str, Any]:
    value = vehicle_cdp.evaluate(
        r"""
        (async () => {
          const canvas = document.createElement('canvas');
          canvas.width = 320;
          canvas.height = 180;
          const ctx = canvas.getContext('2d');
          ctx.fillStyle = '#102030';
          ctx.fillRect(0, 0, canvas.width, canvas.height);
          ctx.fillStyle = '#f0c040';
          ctx.font = '24px sans-serif';
          ctx.fillText('vehicle-front', 32, 92);
          const stream = canvas.captureStream(5);
          const peer = new RTCPeerConnection({});
          const state = {
            peer,
            localCandidates: [],
            localCandidateCursor: 0,
            receivedDataChannel: null,
            receivedCommands: [],
            remoteDescriptionSet: false
          };
          window.__mineTeleopVehicle = state;
          peer.onicecandidate = (event) => {
            if (event.candidate) {
              state.localCandidates.push(event.candidate.toJSON ? event.candidate.toJSON() : event.candidate);
            }
          };
          peer.ondatachannel = (event) => {
            state.receivedDataChannel = event.channel;
            state.receivedDataChannel.onmessage = (message) => {
              state.receivedCommands.push(JSON.parse(message.data));
            };
          };
          peer.createDataChannel('vehicle-negotiation');
          for (const track of stream.getTracks()) {
            peer.addTrack(track, stream);
          }
          const offer = await peer.createOffer();
          await peer.setLocalDescription(offer);
          return {
            type: peer.localDescription.type,
            sdp: peer.localDescription.sdp,
            media_tracks: [{camera_id:'front', codec:'h264', width:320, height:180, fps:5, bitrate_kbps:400}]
          };
        })()
        """,
        await_promise=True,
    )
    if not isinstance(value, dict) or value.get("type") != "offer" or not value.get("sdp"):
        raise RuntimeError(f"vehicle browser did not create a WebRTC offer: {value!r}")
    return value


def _vehicle_take_local_ice_candidates(vehicle_cdp: "ChromeDevToolsConnection") -> list[dict[str, Any]]:
    value = vehicle_cdp.evaluate(
        r"""
        (() => {
          const state = window.__mineTeleopVehicle;
          if (!state) return [];
          const candidates = state.localCandidates.slice(state.localCandidateCursor);
          state.localCandidateCursor = state.localCandidates.length;
          return candidates;
        })()
        """,
        await_promise=False,
    )
    return [candidate for candidate in value if isinstance(candidate, dict)] if isinstance(value, list) else []


def _vehicle_accept_answer(vehicle_cdp: "ChromeDevToolsConnection", answer: dict[str, Any]) -> None:
    vehicle_cdp.evaluate(
        f"""
        (async () => {{
          const state = window.__mineTeleopVehicle;
          if (!state) throw new Error('vehicle peer is missing');
          if (!state.remoteDescriptionSet) {{
            await state.peer.setRemoteDescription({json.dumps(answer)});
            state.remoteDescriptionSet = true;
          }}
          return true;
        }})()
        """,
        await_promise=True,
    )


def _vehicle_add_ice_candidate(vehicle_cdp: "ChromeDevToolsConnection", candidate: dict[str, Any]) -> None:
    vehicle_cdp.evaluate(
        f"""
        (async () => {{
          const state = window.__mineTeleopVehicle;
          if (!state) throw new Error('vehicle peer is missing');
          await state.peer.addIceCandidate(new RTCIceCandidate({json.dumps(candidate)}));
          return true;
        }})()
        """,
        await_promise=True,
    )


def _vehicle_webrtc_summary(vehicle_cdp: "ChromeDevToolsConnection") -> dict[str, Any]:
    value = vehicle_cdp.evaluate(
        r"""
        (() => {
          const state = window.__mineTeleopVehicle || {};
          const command = state.receivedCommands && state.receivedCommands.length
            ? state.receivedCommands[state.receivedCommands.length - 1]
            : null;
          return {
            vehicle_peer_connection_state: state.peer ? state.peer.connectionState : '',
            vehicle_datachannel_open: Boolean(state.receivedDataChannel && state.receivedDataChannel.readyState === 'open'),
            datachannel_control_command_received: Boolean(command && command.type === 'control_command'),
            datachannel_last_command_seq: command ? command.seq : null,
            datachannel_last_command_gear: command ? command.gear : null
          };
        })()
        """,
        await_promise=False,
    )
    return value if isinstance(value, dict) else {}


def _driver_poll_signaling(driver_cdp: "ChromeDevToolsConnection") -> None:
    driver_cdp.evaluate("(async () => { await pollSignaling(); return true; })()", await_promise=True)


def _driver_send_datachannel_control(driver_cdp: "ChromeDevToolsConnection") -> None:
    driver_cdp.evaluate(
        "(async () => { await sendControl({gear:'D', steering:0.2, throttle:0.3, brake:0.0, now_ms: Date.now() + 1000}); return true; })()",
        await_promise=True,
    )


def _driver_webrtc_summary(driver_cdp: "ChromeDevToolsConnection") -> dict[str, Any]:
    value = driver_cdp.evaluate(
        r"""
        (() => {
          const video = document.getElementById('webrtc-video-front');
          return {
            loopback_webrtc_media_received: Boolean(video && video.srcObject && video.srcObject.getVideoTracks().length > 0),
            driver_video_element_attached: Boolean(video),
            driver_datachannel_open: Boolean(controlDataChannel && controlDataChannel.readyState === 'open')
          };
        })()
        """,
        await_promise=False,
    )
    return value if isinstance(value, dict) else {}


def _driver_operator_panel_summary(driver_cdp: "ChromeDevToolsConnection") -> dict[str, Any]:
    value = driver_cdp.evaluate(
        r"""
        (() => {
          const text = (id) => {
            const node = document.getElementById(id);
            return node ? node.textContent : '';
          };
          const sessionState = document.getElementById('operator-session-state');
          return {
            operator_session_state_text: sessionState ? sessionState.textContent : '',
            operator_control_authority_text: text('operator-control-authority'),
            operator_signaling_state_text: text('operator-signaling-state'),
            operator_camera_summary_text: text('operator-camera-summary'),
            operator_command_summary_text: text('operator-command-summary'),
            operator_webrtc_state_text: text('operator-webrtc-state'),
            operator_datachannel_state_text: text('operator-datachannel-state')
          };
        })()
        """,
        await_promise=False,
    )
    return value if isinstance(value, dict) else {}


class ChromeDevToolsConnection:
    def __init__(self, ws_url: str, *, timeout_seconds: float) -> None:
        self.ws_url = ws_url
        self.timeout_seconds = timeout_seconds
        self.sock = _connect_websocket(ws_url, timeout_seconds=timeout_seconds)
        self.next_id = 0

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    def command(self, method: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        self.next_id += 1
        message_id = self.next_id
        self._send_json({"id": message_id, "method": method, "params": params or {}})
        deadline = time.monotonic() + self.timeout_seconds
        while time.monotonic() < deadline:
            message = self._recv_json(deadline)
            if message.get("id") != message_id:
                continue
            if "error" in message:
                raise RuntimeError(f"CDP {method} failed: {message['error']}")
            result = message.get("result", {})
            if not isinstance(result, dict):
                raise RuntimeError(f"CDP {method} returned non-object result: {message}")
            return result
        raise TimeoutError(f"timed out waiting for CDP response to {method}")

    def evaluate(self, expression: str, *, await_promise: bool = False) -> Any:
        result = self.command(
            "Runtime.evaluate",
            {
                "expression": expression,
                "awaitPromise": await_promise,
                "returnByValue": True,
            },
        )
        if "exceptionDetails" in result:
            raise RuntimeError(f"browser evaluation failed: {result['exceptionDetails']}")
        remote = result.get("result", {})
        if not isinstance(remote, dict):
            raise RuntimeError(f"unexpected Runtime.evaluate response: {result}")
        return remote.get("value")

    def _send_json(self, payload: dict[str, Any]) -> None:
        _send_websocket_text(self.sock, json.dumps(payload, separators=(",", ":")).encode("utf-8"))

    def _recv_json(self, deadline: float) -> dict[str, Any]:
        text = _recv_websocket_text(self.sock, deadline)
        payload = json.loads(text)
        if not isinstance(payload, dict):
            raise RuntimeError(f"unexpected websocket JSON payload: {payload}")
        return payload


def _find_chrome_binary() -> str:
    candidates = [
        "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome",
        "/Applications/Chromium.app/Contents/MacOS/Chromium",
        shutil.which("google-chrome"),
        shutil.which("google-chrome-stable"),
        shutil.which("chromium"),
        shutil.which("chromium-browser"),
    ]
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(candidate)
    raise RuntimeError("Chrome/Chromium binary was not found; pass --chrome-binary")


def _open_devtools_target(port: int, url: str, timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    quoted_url = quote(url, safe="")
    while time.monotonic() < deadline:
        try:
            req = request.Request(f"http://127.0.0.1:{port}/json/new?{quoted_url}", method="PUT")
            payload = json.loads(request.urlopen(req, timeout=2).read().decode("utf-8"))
            ws_url = payload.get("webSocketDebuggerUrl")
            if isinstance(ws_url, str) and ws_url:
                return ws_url
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.2)
    raise RuntimeError(f"Chrome DevTools target did not become ready: {last_error}")


def _wait_for_page_ready(cdp: ChromeDevToolsConnection, *, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            if cdp.evaluate("document.readyState") == "complete":
                return
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.1)
    raise TimeoutError(f"page did not finish loading: {last_error}")


def _connect_websocket(ws_url: str, *, timeout_seconds: float) -> socket.socket:
    parsed = urlparse(ws_url)
    if parsed.scheme != "ws":
        raise ValueError(f"only ws:// DevTools URLs are supported: {ws_url}")
    host = parsed.hostname or "127.0.0.1"
    port = parsed.port or 80
    path = parsed.path or "/"
    if parsed.query:
        path += f"?{parsed.query}"
    sock = socket.create_connection((host, port), timeout=timeout_seconds)
    sock.settimeout(timeout_seconds)
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    request_text = (
        f"GET {path} HTTP/1.1\r\n"
        f"Host: {host}:{port}\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n"
    )
    sock.sendall(request_text.encode("ascii"))
    response = _read_until(sock, b"\r\n\r\n", timeout_seconds)
    if b" 101 " not in response.split(b"\r\n", 1)[0]:
        sock.close()
        raise RuntimeError(f"DevTools websocket handshake failed: {response.decode('latin1', errors='replace')}")
    return sock


def _send_websocket_text(sock: socket.socket, payload: bytes) -> None:
    header = bytearray([0x81])
    length = len(payload)
    if length < 126:
        header.append(0x80 | length)
    elif length <= 0xFFFF:
        header.append(0x80 | 126)
        header.extend(struct.pack("!H", length))
    else:
        header.append(0x80 | 127)
        header.extend(struct.pack("!Q", length))
    mask = os.urandom(4)
    header.extend(mask)
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(bytes(header) + masked)


def _recv_websocket_text(sock: socket.socket, deadline: float) -> str:
    while True:
        header = _recv_exact(sock, 2, deadline)
        opcode = header[0] & 0x0F
        masked = bool(header[1] & 0x80)
        length = header[1] & 0x7F
        if length == 126:
            length = struct.unpack("!H", _recv_exact(sock, 2, deadline))[0]
        elif length == 127:
            length = struct.unpack("!Q", _recv_exact(sock, 8, deadline))[0]
        mask = _recv_exact(sock, 4, deadline) if masked else b""
        payload = _recv_exact(sock, length, deadline) if length else b""
        if masked:
            payload = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
        if opcode == 1:
            return payload.decode("utf-8")
        if opcode == 8:
            raise RuntimeError("DevTools websocket closed")
        if opcode == 9:
            _send_websocket_pong(sock, payload)


def _send_websocket_pong(sock: socket.socket, payload: bytes) -> None:
    header = bytearray([0x8A])
    length = len(payload)
    if length >= 126:
        payload = payload[:125]
        length = len(payload)
    header.append(0x80 | length)
    mask = os.urandom(4)
    header.extend(mask)
    masked = bytes(byte ^ mask[index % 4] for index, byte in enumerate(payload))
    sock.sendall(bytes(header) + masked)


def _recv_exact(sock: socket.socket, length: int, deadline: float) -> bytes:
    chunks: list[bytes] = []
    remaining = length
    while remaining:
        timeout = max(0.1, deadline - time.monotonic())
        sock.settimeout(timeout)
        chunk = sock.recv(remaining)
        if not chunk:
            raise RuntimeError("socket closed while reading websocket frame")
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _read_until(sock: socket.socket, marker: bytes, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    data = bytearray()
    while marker not in data:
        timeout = max(0.1, deadline - time.monotonic())
        sock.settimeout(timeout)
        chunk = sock.recv(1)
        if not chunk:
            raise RuntimeError("socket closed during websocket handshake")
        data.extend(chunk)
    return bytes(data)


def _wait_for_health(driver_console_url: str, *, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    last_error = ""
    while time.monotonic() < deadline:
        try:
            health = json.loads(request.urlopen(f"{driver_console_url}/health", timeout=2).read().decode("utf-8"))
            if health.get("status") == "ok":
                return
        except Exception as exc:
            last_error = str(exc)
        time.sleep(0.2)
    raise RuntimeError(f"driver console health did not become ok: {last_error}")


def _signaling_post(container: str, path: str, payload: dict[str, Any]) -> dict[str, Any]:
    return _docker_signaling_json(container, "POST", path, payload)


def _signaling_get(container: str, path: str) -> dict[str, Any]:
    return _docker_signaling_json(container, "GET", path, None)


def _docker_signaling_json(
    container: str,
    method: str,
    path: str,
    payload: dict[str, Any] | None,
) -> dict[str, Any]:
    # Uses docker exec so the host browser smoke can reach the loopback-only signaling server
    # without publishing a signaling port from the Docker control-plane stack.
    code = r"""
import json
import sys
import urllib.request

request_payload = json.load(sys.stdin)
url = "http://127.0.0.1:8765" + request_payload["path"]
method = request_payload["method"]
body = None
headers = {}
if method == "POST":
    body = json.dumps(request_payload["payload"]).encode("utf-8")
    headers["Content-Type"] = "application/json"
req = urllib.request.Request(url, data=body, method=method, headers=headers)
with urllib.request.urlopen(req, timeout=5) as response:
    sys.stdout.write(response.read().decode("utf-8"))
"""
    result = subprocess.run(
        ["docker", "exec", "-i", container, "python3", "-c", code],
        input=json.dumps({"method": method, "path": path, "payload": payload}),
        text=True,
        capture_output=True,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"docker exec signaling {method} {path} failed with {result.returncode}: "
            f"stdout={result.stdout!r} stderr={result.stderr!r}"
        )
    data = json.loads(result.stdout)
    if not isinstance(data, dict):
        raise RuntimeError(f"signaling {method} {path} returned non-object JSON: {data!r}")
    return data


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        return int(sock.getsockname()[1])


def _write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2, sort_keys=True) + "\n", encoding="utf-8")


if __name__ == "__main__":
    raise SystemExit(main())
