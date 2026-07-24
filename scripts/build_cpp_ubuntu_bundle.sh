#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
platform="${1:-linux/amd64}"
architecture="${platform#linux/}"
case "$architecture" in
  amd64) package_architecture="x64" ;;
  *) package_architecture="$architecture" ;;
esac
package_timestamp="$(date -u +%Y%m%d-%H%M%S)"
output_root="${2:-$repo_root/dist/mine-teleop-vehicle-ubuntu22.04-$package_architecture-$package_timestamp}"
image="mine-teleop-cpp-ubuntu22.04:$architecture"
build_image="$image-build"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-2}"
vehicle_config="${MINE_TELEOP_VEHICLE_CONFIG:-$repo_root/configs/vehicle-agent.three-machine.field.yaml}"
base_bundle_archive="${MINE_TELEOP_BASE_BUNDLE_ARCHIVE:-}"

if [[ "$(uname -s)" == "Darwin" && "$(uname -m)" == "arm64" &&
      -z "$base_bundle_archive" ]]; then
  exec "$script_dir/build_macos_vehicle_from_scratch.sh" "$@"
fi

temporary="$(mktemp -d)"
temporary_container=""

cleanup() {
  if [[ -n "$temporary_container" ]]; then
    docker rm -f "$temporary_container" >/dev/null 2>&1 || true
  fi
  rm -rf "$temporary"
}
trap cleanup EXIT

if [[ ! -f "$vehicle_config" ]]; then
  printf 'vehicle configuration does not exist: %s\n' "$vehicle_config" >&2
  exit 2
fi
if [[ -e "$output_root" ]]; then
  printf 'bundle output already exists: %s\n' "$output_root" >&2
  exit 2
fi
mkdir -p "$(dirname "$output_root")"

third_party_runtime_source="from-source"
third_party_runtime_sha256="none"
if [[ -z "$base_bundle_archive" ]]; then
  docker buildx build \
    --platform "$platform" \
    --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
    --target runtime \
    --load \
    -t "$image" \
    -f "$repo_root/deployments/cpp/Dockerfile.build" \
    "$repo_root"

  docker run --rm "$image" version
  docker run --rm "$image" config-check \
    --config /opt/mine-teleop/config/vehicle-agent.yaml

  docker buildx build \
    --platform "$platform" \
    --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
    --target artifact \
    --output "type=local,dest=$temporary/artifact" \
    -f "$repo_root/deployments/cpp/Dockerfile.build" \
    "$repo_root"

  mv "$temporary/artifact" "$output_root"
else
  if [[ ! -f "$base_bundle_archive" ]]; then
    printf 'base bundle archive does not exist: %s\n' "$base_bundle_archive" >&2
    exit 2
  fi
  base_bundle_archive="$(cd "$(dirname "$base_bundle_archive")" && pwd)/$(basename "$base_bundle_archive")"
  base_checksum_file="$base_bundle_archive.sha256"
  if [[ ! -f "$base_checksum_file" ]]; then
    printf 'base bundle checksum is missing: %s\n' "$base_checksum_file" >&2
    exit 2
  fi
  expected_base_checksum="$(awk 'NR == 1 {print $1}' "$base_checksum_file")"
  if command -v sha256sum >/dev/null 2>&1; then
    actual_base_checksum="$(sha256sum "$base_bundle_archive" | awk '{print $1}')"
  else
    actual_base_checksum="$(shasum -a 256 "$base_bundle_archive" | awk '{print $1}')"
  fi
  if [[ -z "$expected_base_checksum" || "$actual_base_checksum" != "$expected_base_checksum" ]]; then
    printf 'base bundle checksum mismatch: expected=%s actual=%s\n' \
      "$expected_base_checksum" "$actual_base_checksum" >&2
    exit 2
  fi
  third_party_runtime_sha256="$actual_base_checksum"
  base_entries="$(tar -tzf "$base_bundle_archive")"
  if printf '%s\n' "$base_entries" | awk '/(^\/|(^|\/)\.\.(\/|$))/ {found=1} END {exit !found}'; then
    printf 'base bundle contains an unsafe archive path\n' >&2
    exit 2
  fi
  mkdir -p "$temporary/base"
  tar -xzf "$base_bundle_archive" -C "$temporary/base"
  base_roots=("$temporary/base"/*)
  if [[ "${#base_roots[@]}" -ne 1 || ! -d "${base_roots[0]}" ]]; then
    printf 'base bundle must contain exactly one top-level package directory\n' >&2
    exit 2
  fi

  docker buildx build \
    --platform "$platform" \
    --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
    --target build \
    --load \
    -t "$build_image" \
    -f "$repo_root/deployments/cpp/Dockerfile.build" \
    "$repo_root"
  docker run --rm --platform "$platform" "$build_image" \
    /opt/mine-teleop/bin/mine-teleop version
  docker run --rm --platform "$platform" "$build_image" \
    /opt/mine-teleop/bin/mine-teleop config-check \
    --config /opt/mine-teleop/share/mine-teleop/configs/vehicle-agent.dev.yaml

  mkdir -p "$output_root"
  cp -a "${base_roots[0]}/." "$output_root/"
  temporary_container="$(docker create --platform "$platform" "$build_image")"
  docker cp "$temporary_container:/opt/mine-teleop/bin/." "$output_root/bin/"
  docker rm "$temporary_container" >/dev/null
  temporary_container=""
  third_party_runtime_source="$(basename "$base_bundle_archive")"
fi

install -m 0644 "$vehicle_config" "$output_root/config/vehicle-agent.yaml"
install -m 0644 "$repo_root/configs/mine-teleop-field-root.crt" "$output_root/config/mine-teleop-field-root.crt"
install -m 0644 "$repo_root/deployments/udev/99-mine-teleop-basler.rules" \
  "$output_root/deployments/udev/99-mine-teleop-basler.rules"
install -m 0755 "$repo_root/scripts/setup_basler_usb_access.sh" \
  "$output_root/scripts/setup_basler_usb_access.sh"
install -m 0644 "$repo_root/packaging/ubuntu-vehicle/README.txt" "$output_root/README.txt"
printf '%s\n' \
  "target_platform=$platform" \
  "target_architecture=$architecture" \
  "vehicle_config=$(basename "$vehicle_config")" \
  "third_party_runtime_source=$third_party_runtime_source" \
  "third_party_runtime_sha256=$third_party_runtime_sha256" \
  "runtime_tests_executed=yes" \
  "built_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  >"$output_root/BUILD-INFO.txt"
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
printf 'BUNDLE_DIR=%s\n' "$output_root"
printf 'BUNDLE_ARCHIVE=%s\n' "$archive"
