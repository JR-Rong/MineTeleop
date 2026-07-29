#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
chassis_control_root="${MINE_TELEOP_CHASSIS_CONTROL_ROOT:-$repo_root/../ChassisControl}"
chassis_control_library="${MINE_TELEOP_CHASSIS_CONTROL_LIBRARY:-$repo_root/../MinePilot/libchassis_control.so}"
output_dir="${MINE_TELEOP_CHASSIS_RUNTIME_OUTPUT_DIR:-$repo_root/vendor/chassis/lib}"
platform="${MINE_TELEOP_CHASSIS_PLATFORM:-linux/amd64}"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-2}"

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

[[ "$platform" == "linux/amd64" ]] || {
  die "the chassis runtime currently supports only linux/amd64"
}
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || {
  die "MINE_TELEOP_BUILD_JOBS must be a positive integer"
}
[[ -d "$chassis_control_root" ]] || {
  die "ChassisControl source directory does not exist: $chassis_control_root"
}
[[ -f "$chassis_control_root/include/global_variables.h" ]] || {
  die "ChassisControl header is missing: $chassis_control_root/include/global_variables.h"
}
[[ -s "$chassis_control_library" ]] || {
  die "ChassisControl runtime library does not exist or is empty: $chassis_control_library"
}
command -v docker >/dev/null 2>&1 || die "Docker is required"
docker info >/dev/null 2>&1 || die "Docker is not running"
docker buildx version >/dev/null 2>&1 || die "docker buildx is required"

temporary="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-chassis-runtime.XXXXXX")"
cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

mkdir -p "$temporary/chassis-runtime"
install -m 0755 \
  "$chassis_control_library" \
  "$temporary/chassis-runtime/libchassis_control.so"

docker buildx build \
  --platform "$platform" \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --build-context "chassis_control_headers=$chassis_control_root/include" \
  --build-context "chassis_runtime=$temporary/chassis-runtime" \
  --target artifact \
  --output "type=local,dest=$temporary/artifact" \
  -f "$repo_root/deployments/chassis-control-bridge/Dockerfile.build" \
  "$repo_root"

mkdir -p "$output_dir"
for library in \
  libmine_teleop_chassis_bridge.so \
  libchassis_control.so; do
  source_path="$temporary/artifact/lib/vendor/chassis/$library"
  [[ -s "$source_path" ]] || die "built chassis runtime is missing: $source_path"
  install -m 0755 "$source_path" "$output_dir/$library"
done

if command -v sha256sum >/dev/null 2>&1; then
  sha256sum \
    "$output_dir/libmine_teleop_chassis_bridge.so" \
    "$output_dir/libchassis_control.so"
else
  shasum -a 256 \
    "$output_dir/libmine_teleop_chassis_bridge.so" \
    "$output_dir/libchassis_control.so"
fi
printf 'CHASSIS_RUNTIME_DIR=%s\n' "$output_dir"
