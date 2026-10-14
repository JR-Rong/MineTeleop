#!/usr/bin/env python3
"""Run the fixed production control-smoke against local subprocess mocks only.

Execute inside a Linux network namespace with only loopback enabled. This is
control-plane delivery validation, not a camera or vehicle acceptance test.
"""
import argparse
import json
import os
from pathlib import Path
import platform
import socket
import subprocess
import time
import urllib.request


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--output-dir", required=True, type=Path)
    args = parser.parse_args()
    if platform.system() != "Linux" or {name for _, name in socket.if_nameindex()} != {"lo"}:
        parser.error("a Linux network namespace containing only lo is required")
    binary = args.binary.resolve(strict=True)
    if binary.name != "mine-teleop" or not binary.is_file():
        parser.error("--binary must identify the built mine-teleop executable")
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("MINE_TELEOP_") and key not in
                   {"LD_PRELOAD", "DYLD_INSERT_LIBRARIES", "NODE_OPTIONS"} and
                   key.lower() not in {"http_proxy", "https_proxy", "all_proxy", "no_proxy"}}
    direct_http = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    ports = []
    # Keep both sockets reserved until the unique ports have been selected.
    reservations = [socket.socket() for _ in range(2)]
    for reservation in reservations:
        reservation.bind(("127.0.0.1", 0))
        ports.append(reservation.getsockname()[1])
    for reservation in reservations:
        reservation.close()
    cloud_url, console_url = (f"http://127.0.0.1:{port}" for port in ports)
    configuration = output / "driver.synthetic.yaml"
    configuration.write_text(json.dumps({
        "driver": {"id": "driver-console-001"},
        "cloud": {"auth_url": cloud_url + "/auth", "signaling_url": cloud_url + "/signaling"},
        "logging": {"browser_event_log": str(output / "browser-events.jsonl")},
    }), encoding="utf-8")
    commands = [
        [str(binary), "signaling-server", "--host", "127.0.0.1", "--port", str(ports[0]),
         "--driver-password", "loopback-synthetic-password", "--device-token", "loopback-synthetic-device"],
        [str(binary), "driver-console", "--host", "127.0.0.1", "--port", str(ports[1]),
         "--config", str(configuration), "--driver-password", "loopback-synthetic-password",
         "--signaling-http-url", cloud_url],
        [str(binary), "control-smoke", "--signaling-http-url", cloud_url,
         "--driver-console-url", console_url, "--device-token", "loopback-synthetic-device"],
    ]
    (output / "commands.json").write_text(json.dumps(commands, indent=2), encoding="utf-8")
    processes, logs = [], []
    try:
        for index, (command, origin) in enumerate(zip(commands[:2], (cloud_url, console_url))):
            log = (output / f"server-{index}.log").open("wb")
            logs.append(log)
            process = subprocess.Popen(command, cwd=output, env=environment, stdout=log, stderr=subprocess.STDOUT)
            processes.append(process)
            deadline = time.monotonic() + 5
            while True:
                if process.poll() is not None:
                    raise RuntimeError(f"local mock {index} exited: {process.returncode}")
                try:
                    with direct_http.open(origin + "/health", timeout=0.2) as response:
                        if response.status == 200:
                            break
                except OSError:
                    pass
                if time.monotonic() > deadline:
                    raise RuntimeError(f"local mock {index} did not become healthy")
                time.sleep(0.02)
        completed = subprocess.run(commands[2], cwd=output, env=environment, capture_output=True, timeout=15)
        (output / "control-smoke.stdout.jsonl").write_bytes(completed.stdout)
        (output / "control-smoke.stderr.log").write_bytes(completed.stderr)
        result = {"exit_code": completed.returncode, "network_interfaces": ["lo"],
                  "scope": "synthetic control-plane only", "video_observation_performed": False}
        (output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps(result))
        return completed.returncode
    finally:
        for process in reversed(processes):
            process.terminate()
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=2)
        for log in logs:
            log.close()


if __name__ == "__main__":
    raise SystemExit(main())
