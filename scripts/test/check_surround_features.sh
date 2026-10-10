#!/usr/bin/env bash
# Run inside scripts/test/Dockerfile.features; no camera, CAN or public relay.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
feature_build_dir="${MINE_TELEOP_FEATURE_BUILD_DIR:-/tmp/mine-teleop-feature-build}"
test "$(pkg-config --modversion gstreamer-webrtc-1.0)" = "1.28.5"
cmake -S "$repo_dir" -B "$feature_build_dir" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DMINE_TELEOP_BUILD_TESTS=ON \
  -DMINE_TELEOP_BUILD_PORTABLE_CONTROL_TESTS=ON \
  -DMINE_TELEOP_SOFTWARE_MEDIA_FIXTURES=ON \
  -DMINE_TELEOP_BUILD_ARAVIS_CAMERA_BRIDGE=OFF \
  -DMINE_TELEOP_BUILD_VENDOR_CAMERA_BRIDGES=OFF
cmake --build "$feature_build_dir" -j "${MINE_TELEOP_FEATURE_JOBS:-2}"
ctest --test-dir "$feature_build_dir" --output-on-failure -j 1
python3 -m venv /tmp/mine-teleop-calibration-venv
for calibration_attempt in 1 2 3; do
  if /tmp/mine-teleop-calibration-venv/bin/python -m pip install --no-cache-dir \
      --index-url https://pypi.org/simple -r "$repo_dir/tools/calibration/requirements.txt"; then
    break
  fi
  # A truncated download must be fetched again with the original index hash;
  # never disable integrity checking or accept a mismatched wheel.
  test "$calibration_attempt" -lt 3
done
/tmp/mine-teleop-calibration-venv/bin/python "$repo_dir/scripts/test/calibration_fixture_test.py"
/tmp/mine-teleop-calibration-venv/bin/python "$repo_dir/scripts/test/calibration_solver_fixture.py"
python3 "$repo_dir/scripts/test/maintenance_fixture_test.py" "$feature_build_dir/mine-teleop"
