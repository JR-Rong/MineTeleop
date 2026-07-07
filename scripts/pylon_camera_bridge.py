#!/usr/bin/env python3
from __future__ import annotations

import os
import sys
from pathlib import Path


def main(argv: list[str] | None = None) -> int:
    args = list(sys.argv[1:] if argv is None else argv)
    for candidate in _candidate_binaries():
        if candidate.is_file() and os.access(candidate, os.X_OK):
            os.execv(str(candidate), [str(candidate), *args])
    searched = ", ".join(str(path) for path in _candidate_binaries())
    print(f"pylon-camera-bridge binary not found; searched: {searched}", file=sys.stderr)
    print("Run scripts/run_vehicle_live_media.sh on the vehicle to compile it from scripts/pylon_camera_bridge.cpp.", file=sys.stderr)
    return 2


def _candidate_binaries() -> list[Path]:
    candidates: list[Path] = []
    override = os.environ.get("MINE_TELEOP_PYLON_BRIDGE_BIN")
    if override:
        candidates.append(Path(override))
    executable_dir = Path(sys.executable).resolve().parent
    candidates.append(executable_dir / "pylon-camera-bridge")
    try:
        repo_root = Path(__file__).resolve().parents[1]
        candidates.append(repo_root / "bin" / "pylon-camera-bridge")
    except IndexError:
        pass
    candidates.extend(
        [
            Path("/home/user/mine-teleop/bin/pylon-camera-bridge"),
            Path("/opt/mine-teleop/bin/pylon-camera-bridge"),
        ]
    )
    unique: list[Path] = []
    seen: set[str] = set()
    for candidate in candidates:
        key = str(candidate)
        if key not in seen:
            unique.append(candidate)
            seen.add(key)
    return unique


if __name__ == "__main__":
    raise SystemExit(main())
