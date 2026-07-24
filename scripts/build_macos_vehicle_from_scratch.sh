#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
platform="${1:-linux/amd64}"
package_timestamp="$(date -u +%Y%m%d-%H%M%S)"
output_root="${2:-$repo_root/dist/mine-teleop-vehicle-ubuntu22.04-x64-$package_timestamp}"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-1}"
vehicle_config="${MINE_TELEOP_VEHICLE_CONFIG:-$repo_root/configs/vehicle-agent.three-machine.field.yaml}"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-vehicle-from-scratch.XXXXXX")"

cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

[[ "$(uname -s)" == "Darwin" ]] || die "this entrypoint is for macOS"
[[ "$platform" == "linux/amd64" ]] || {
  die "the vehicle package currently supports only linux/amd64"
}
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || {
  die "MINE_TELEOP_BUILD_JOBS must be a positive integer"
}
[[ -f "$vehicle_config" ]] || die "vehicle configuration does not exist: $vehicle_config"
[[ ! -e "$output_root" && ! -e "$output_root.tar.gz" ]] || {
  die "bundle output already exists: $output_root"
}
command -v docker >/dev/null 2>&1 || die "Docker Desktop or Colima is required"
docker info >/dev/null 2>&1 || die "Docker is not running"
docker buildx version >/dev/null 2>&1 || die "docker buildx is required"

mkdir -p "$(dirname "$output_root")"
source_commit="$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || printf unknown)"
if [[ -n "$(git -C "$repo_root" status --porcelain 2>/dev/null || true)" ]]; then
  source_commit="$source_commit-dirty"
fi
built_at_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

printf '==> building the vehicle package from a clean Ubuntu 22.04 amd64 base\n'
printf '==> no MINE_TELEOP_BASE_BUNDLE_ARCHIVE is used\n'
docker buildx build \
  --platform "$platform" \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --build-arg "MINE_TELEOP_SOURCE_COMMIT=$source_commit" \
  --build-arg "MINE_TELEOP_BUILT_AT_UTC=$built_at_utc" \
  --target artifact \
  --output "type=local,dest=$temporary/artifact" \
  -f "$repo_root/deployments/cpp/Dockerfile.macos-from-scratch" \
  "$repo_root"

mv "$temporary/artifact" "$output_root"
install -m 0644 "$vehicle_config" "$output_root/config/vehicle-agent.yaml"
install -m 0644 \
  "$repo_root/configs/mine-teleop-field-root.crt" \
  "$output_root/config/mine-teleop-field-root.crt"
install -m 0644 \
  "$repo_root/deployments/udev/99-mine-teleop-basler.rules" \
  "$output_root/deployments/udev/99-mine-teleop-basler.rules"
install -m 0755 \
  "$repo_root/scripts/setup_basler_usb_access.sh" \
  "$output_root/scripts/setup_basler_usb_access.sh"
install -m 0644 \
  "$repo_root/packaging/ubuntu-vehicle/README.txt" \
  "$output_root/README.txt"
printf '%s\n' \
  "source_commit=$source_commit" \
  "target_platform=$platform" \
  'target_architecture=amd64' \
  'build_type=Release' \
  "vehicle_config=$(basename "$vehicle_config")" \
  'third_party_runtime_source=ubuntu-22.04-packages+source-built-libsrtp' \
  'third_party_runtime_sha256=not-applicable' \
  'runtime_tests_executed=yes' \
  "built_at_utc=$built_at_utc" \
  > "$output_root/BUILD-INFO.txt"

archive="$output_root.tar.gz"
COPYFILE_DISABLE=1 tar --no-xattrs \
  -C "$(dirname "$output_root")" \
  -czf "$archive" \
  "$(basename "$output_root")"

if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$archive" > "$archive.sha256"
else
  shasum -a 256 "$archive" > "$archive.sha256"
fi

"$repo_root/scripts/check_cpp_ubuntu_bundle.sh" "$archive"
printf 'VEHICLE_BUNDLE_DIR=%s\n' "$output_root"
printf 'VEHICLE_BUNDLE_ARCHIVE=%s\n' "$archive"
printf 'VEHICLE_BUNDLE_SHA256=%s\n' "$archive.sha256"
