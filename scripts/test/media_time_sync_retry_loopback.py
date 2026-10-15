#!/usr/bin/env python3
"""Exercise the production media-service retry loop with synthetic HTTP only.

Requires a Linux namespace containing only loopback. No session is granted,
so camera, ICE, adapter and recording initialization must never be reached.
"""
import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import platform
import signal
import socket
import subprocess
import threading
import time
from urllib.parse import parse_qs, urlsplit


class Fixture(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, scenario):
        self.scenario = scenario
        self.lock = threading.Lock()
        self.requests = []
        self.time_requests = 0
        self.registrations = 0
        self.discoveries = 0
        self.faulted = False
        self.unexpected = []
        super().__init__(("127.0.0.1", 0), Handler)


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def respond(self, status, value):
        body = json.dumps(value).encode()
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass  # Expected after the real 500ms curl timeout.

    def do_GET(self):
        parsed = urlsplit(self.path)
        now = int(time.time() * 1000)
        with self.server.lock:
            self.server.requests.append({"path": parsed.path, "at_monotonic": time.monotonic()})
            if parsed.path == "/time":
                self.server.time_requests += 1
                inject = not self.server.faulted and (
                    self.server.scenario != "after-session-end" or self.server.discoveries > 0)
                if inject:
                    self.server.faulted = True
            elif parsed.path == "/vehicles/synthetic-vehicle/session":
                self.server.discoveries += 1
                discovery = self.server.discoveries
                inject = False
            else:
                self.server.unexpected.append(parsed.path)
                inject = False
        if parsed.path == "/time":
            sent = int(parse_qs(parsed.query)["client_send_ms"][0])
            if inject and self.server.scenario in {"initial-timeout", "after-session-end", "always-timeout"}:
                time.sleep(0.7)
            elif self.server.scenario == "always-timeout":
                time.sleep(0.7)
            if inject and self.server.scenario == "unauthorized":
                self.respond(401, {"error": "synthetic unauthorized"})
            elif inject and self.server.scenario == "invalid-sample":
                self.respond(200, {"client_send_ms": sent + 1, "server_receive_ms": now, "server_send_ms": now})
            else:
                self.respond(200, {"client_send_ms": sent, "server_receive_ms": now,
                                   "server_send_ms": int(time.time() * 1000)})
        elif parsed.path == "/vehicles/synthetic-vehicle/session":
            if self.server.scenario == "after-session-end" and discovery == 1:
                self.respond(409, {"error": "synthetic session ended", "issue_code": "session_not_active"})
            else:
                self.respond(200, {"session_id": ""})
        else:
            self.respond(404, {"error": "unexpected synthetic request"})

    def do_POST(self):
        size = int(self.headers.get("Content-Length", "0"))
        if size > 65536:
            self.respond(413, {})
            return
        self.rfile.read(size)
        with self.server.lock:
            self.server.requests.append({"path": self.path, "at_monotonic": time.monotonic()})
            if self.path == "/vehicles/online":
                self.server.registrations += 1
            else:
                self.server.unexpected.append(self.path)
        if self.path == "/vehicles/online":
            self.respond(200, {"connection_generation": 1})
        else:
            self.respond(404, {"error": "unexpected synthetic request"})


def events(path):
    result = []
    for line in path.read_text(errors="replace").splitlines():
        try:
            value = json.loads(line)
        except ValueError:
            continue
        if isinstance(value, dict):
            result.append(value)
    return result


def scenario(binary, output, name, environment):
    root = output / name
    root.mkdir(exist_ok=True)
    fixture = Fixture(name)
    server_thread = threading.Thread(target=fixture.serve_forever, daemon=True)
    server_thread.start()
    config = root / "vehicle.synthetic.yaml"
    config.write_text(json.dumps({
        "vehicle": {"id": "synthetic-vehicle"},
        "cloud": {"signaling_url": f"http://127.0.0.1:{fixture.server_port}"},
        "runtime": {"control_enabled": False, "media_enabled": True, "teleop_poll_interval_ms": 20},
        "media": {"realtime_profiles": {"synthetic": {"width": 160, "height": 120,
                    "fps": 20, "bitrate_kbps": 200}}},
        "cameras": [{"id": "synthetic-camera", "device": "testsrc", "realtime_profile": "synthetic"}],
        "field_safety": {"commissioning_mode": "bench", "max_speed_kph": 0, "require_time_sync": True},
        "hardware": {"can": {"interface": "synthetic-no-can"}},
        "vehicle_adapter": {"type": "mock"},
        "recording": {"enabled": False}, "upload": {"enabled": False},
    }))
    stdout, stderr = root / "runtime.stdout", root / "runtime.stderr"
    process = None
    start = time.monotonic()
    try:
        with stdout.open("wb") as out, stderr.open("wb") as err:
            process = subprocess.Popen([str(binary), "vehicle-runtime", "--config", str(config),
                "--device-token", "loopback-synthetic-device"], cwd=root, env=environment,
                stdout=out, stderr=err, start_new_session=True)
        target = 2 if name == "after-session-end" else 1
        while time.monotonic() - start < 8:
            code = process.poll()
            observed = events(stdout) + events(stderr)
            names = [value.get("event") for value in observed]
            with fixture.lock:
                registered, discovered = fixture.registrations, fixture.discoveries
                requests = list(fixture.requests)
            if name in {"unauthorized", "invalid-sample"}:
                if code is not None:
                    assert code == 2 and registered == 0, "invalid initial clock must fail before registration"
                    assert "vehicle_media_time_sync_retry" not in names, "nontransport sample failure was retried"
                    break
            elif name == "always-timeout":
                if code is not None:
                    raise AssertionError(f"transient initial time failure terminated runtime: {code}")
                if names.count("vehicle_media_time_sync_retry") >= 2:
                    assert registered == 0, "failed clock registered a vehicle"
                    timings = [r["at_monotonic"] for r in requests if r["path"] == "/time"]
                    assert len(timings) >= 2 and min(b - a for a, b in zip(timings, timings[1:])) >= 1.35, "retry has no bounded backoff"
                    break
            elif code is not None:
                raise AssertionError(f"transient initial time failure terminated runtime: {code}")
            elif registered >= target and discovered >= target + 2:
                assert "vehicle_media_time_sync_retry" in names, "production retry diagnostic absent"
                assert fixture.time_requests >= 7 * target + 1, "registration preceded a full good time cohort"
                if name == "after-session-end":
                    assert "vehicle_media_session_ended" in names, "session-end reconstruction was not exercised"
                break
            time.sleep(0.02)
        else:
            raise AssertionError("production media service did not reach the required retry/failure boundary")
        assert not fixture.unexpected, "fixture unexpectedly reached an active media/control stage"
        assert not any(value.get("event") in {"vehicle_media_lane_configured", "vehicle_vcu_adapter_ready",
            "vehicle_webrtc_offer_created", "vehicle_recording_attempt_started"} for value in observed), "no session was granted"
        return {"scenario": name, "passed": True, "registrations": registered,
                "time_requests": fixture.time_requests, "elapsed_s": round(time.monotonic() - start, 3)}
    finally:
        if process is not None:
            # The runtime forks its media child; terminate/reap the entire owned group.
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=2)
        fixture.shutdown()
        fixture.server_close()
        server_thread.join(timeout=2)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if platform.system() != "Linux" or {name for _, name in socket.if_nameindex()} != {"lo"}:
        parser.error("a Linux network namespace containing only lo is required")
    binary = args.binary.resolve(strict=True)
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items()
        if not key.startswith("MINE_TELEOP_") and key not in {"LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "NODE_OPTIONS"}
        and key.lower() not in {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}}
    results = []
    for name in ["initial-timeout", "after-session-end", "always-timeout", "unauthorized", "invalid-sample"]:
        try:
            results.append(scenario(binary, output, name, environment))
        except (AssertionError, OSError, subprocess.SubprocessError) as error:
            results.append({"scenario": name, "passed": False, "error": str(error)})
    (output / "result.json").write_text(json.dumps(results, indent=2) + "\n")
    print(json.dumps(results))
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
