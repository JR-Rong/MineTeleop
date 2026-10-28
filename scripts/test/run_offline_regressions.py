#!/usr/bin/env python3
"""Run fixed local regressions, optionally adding a network-isolated CTest build.

No dependency install, package launch, URL, configuration, or test filter is
accepted. Native tests must run in a Linux namespace containing only loopback.
The caller builds the native targets first; this script never downloads sources.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import socket
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
PURE_TESTS = (
    ("release helpers", (sys.executable, "-m", "unittest", "discover", "-s", "scripts/ci", "-p", "test_*.py", "-v")),
    ("desktop helpers", ("node", "--test", "desktop/test/log-directory.test.cjs", "desktop/test/diagnostic-log.test.cjs", "desktop/test/native-controller.test.cjs", "desktop/test/native-process.test.cjs", "desktop/test/main-network-policy.test.cjs", "desktop/test/bench-network-policy.test.cjs")),
    ("control logic", ("node", "cpp/tests/control_logic.test.cjs")),
    ("control transport courier", ("node", "cpp/tests/control_transport.test.cjs")),
    ("camera presentation", ("node", "cpp/tests/camera_view.test.cjs")),
    ("session log presentation", ("node", "cpp/tests/session_logs.test.cjs")),
    ("scene state", ("node", "cpp/tests/scene_state.test.mjs")),
)


def clean_environment():
    # In particular, MINE_TELEOP_TEST_WSS_URL enables a genuinely external test
    # in control_tests.cpp. Desktop native roots also require separate evidence.
    env = {key: value for key, value in os.environ.items() if not key.startswith("MINE_TELEOP_")}
    for key in ("NODE_OPTIONS", "LD_PRELOAD", "DYLD_INSERT_LIBRARIES"):
        env.pop(key, None)
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    return env


def require_loopback_namespace():
    if platform.system() != "Linux":
        raise ValueError("native mode requires Linux with an isolated loopback-only network namespace")
    names = {name for _, name in socket.if_nameindex()}
    if names != {"lo"}:
        raise ValueError("native mode requires only the lo interface; run inside unshare --net or docker --network none")


def registered_native_tests(build, env):
    # --no-tests=error was only added in CTest 3.26. The supported Ubuntu
    # baseline supplies 3.22, so inspect the JSON inventory before execution.
    listing = subprocess.run(("ctest", "--test-dir", str(build), "--show-only=json-v1"), cwd=ROOT, env=env,
                             check=True, text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=30)
    tests = json.loads(listing.stdout).get("tests", [])
    if not tests:
        raise ValueError("native build contains no registered tests")
    return [test["name"] for test in tests]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native-build-dir", type=Path, help="add every registered CTest test from an already-built directory; requires isolated Linux networking")
    args = parser.parse_args()
    commands = list(PURE_TESTS)
    env = clean_environment()
    if args.native_build_dir is not None:
        try:
            require_loopback_namespace()
            build = args.native_build_dir.resolve(strict=True)
            if not (build / "CTestTestfile.cmake").is_file():
                raise ValueError("native build directory has no CTestTestfile.cmake; configure with BUILD_TESTS=ON")
            tests = registered_native_tests(build, env)
            print("Registered native tests: " + ", ".join(tests), flush=True)
        except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
            parser.error(str(error))
        commands.append(("all native CTest suites", ("ctest", "--test-dir", str(build), "--output-on-failure", "--timeout", "180")))
    failures = []
    print("Fixed offline regressions; MINE_TELEOP_* and loader/test overrides removed.", flush=True)
    real_desktop_cases = 4 if sys.platform == "win32" else 6
    print(f"Pure desktop mode skips {real_desktop_cases} real-binary lifecycle cases; these are not passes.", flush=True)
    for label, command in commands:
        print(f"\n[{label}] {' '.join(command)}", flush=True)
        if shutil.which(command[0], path=env.get("PATH")) is None:
            print(f"FAILED: required executable is unavailable: {command[0]}", flush=True)
            failures.append(label)
            continue
        start = time.monotonic()
        try:
            result = subprocess.run(command, cwd=ROOT, env=env, check=False, timeout=900 if label == "all native CTest suites" else 180)
            status = result.returncode
        except subprocess.TimeoutExpired:
            status = 124
        print(f"[{label}] exit_code={status} elapsed_s={time.monotonic() - start:.3f}", flush=True)
        if status != 0:
            failures.append(label)
    if failures:
        print("FAILED suites: " + ", ".join(failures), flush=True)
        return 1
    print("All selected suites passed. Hardware, packaged desktop lifecycle, and field transport require separate evidence.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
