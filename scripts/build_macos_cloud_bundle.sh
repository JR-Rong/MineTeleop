#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
platform="${1:-linux/amd64}"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-2}"
package_timestamp="$(date -u +%Y%m%d-%H%M%S)"
output_root="${2:-$repo_root/dist/mine-teleop-cloud-ubuntu22.04-x64-$package_timestamp}"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-cloud-build.XXXXXX")"

cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

[[ "$platform" == "linux/amd64" ]] || {
  die "the cloud systemd package currently supports only linux/amd64"
}
[[ "$build_jobs" =~ ^[1-9][0-9]*$ ]] || {
  die "MINE_TELEOP_BUILD_JOBS must be a positive integer"
}
[[ ! -e "$output_root" && ! -e "$output_root.tar.gz" ]] || {
  die "bundle output already exists: $output_root"
}
command -v docker >/dev/null 2>&1 || die "Docker Desktop is required"
docker info >/dev/null 2>&1 || die "Docker Desktop is not running"
docker buildx version >/dev/null 2>&1 || die "docker buildx is required"

mkdir -p "$(dirname "$output_root")"
source_commit="$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || printf unknown)"
if [[ -n "$(git -C "$repo_root" status --porcelain 2>/dev/null || true)" ]]; then
  source_commit="$source_commit-dirty"
fi
built_at_utc="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

printf '==> building Mine Teleop cloud package for %s\n' "$platform"
docker buildx build \
  --platform "$platform" \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --build-arg "MINE_TELEOP_SOURCE_COMMIT=$source_commit" \
  --build-arg "MINE_TELEOP_BUILT_AT_UTC=$built_at_utc" \
  --build-arg "MINE_TELEOP_TARGET_PLATFORM=$platform" \
  --target artifact \
  --output "type=local,dest=$temporary/artifact" \
  -f "$repo_root/deployments/cloud/Dockerfile.build" \
  "$repo_root"

printf '==> validating extracted package in Ubuntu 22.04 amd64\n'
COPYFILE_DISABLE=1 tar --no-xattrs -C "$temporary/artifact" -cf - . \
  | docker run --rm -i \
      --platform "$platform" \
      ubuntu:22.04 \
      bash -c '
        mkdir -p /opt/mine-teleop
        tar -C /opt/mine-teleop -xf -
        exec /opt/mine-teleop/deploy-cloud.sh --self-test
      '

mv "$temporary/artifact" "$output_root"

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

printf 'CLOUD_BUNDLE_DIR=%s\n' "$output_root"
printf 'CLOUD_BUNDLE_ARCHIVE=%s\n' "$archive"
printf 'CLOUD_BUNDLE_SHA256=%s\n' "$archive.sha256"
