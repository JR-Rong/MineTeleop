#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
platform="${1:-linux/amd64}"
architecture="${platform#linux/}"
output_root="${2:-$repo_root/dist/cpp-ubuntu22.04-$architecture}"
image="mine-teleop-cpp-ubuntu22.04:$architecture"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-2}"
temporary="$(mktemp -d)"

cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

mkdir -p "$output_root"

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
  --config /opt/mine-teleop/share/mine-teleop/configs/vehicle-agent.dev.yaml

docker buildx build \
  --platform "$platform" \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --target artifact \
  --output "type=local,dest=$temporary/artifact" \
  -f "$repo_root/deployments/cpp/Dockerfile.build" \
  "$repo_root"

cp -a "$temporary/artifact/." "$output_root/"
tar -C "$output_root" -czf "$output_root/mine-teleop-ubuntu22.04-$architecture.tar.gz" \
  --exclude "mine-teleop-ubuntu22.04-$architecture.tar.gz" .

if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$output_root/mine-teleop-ubuntu22.04-$architecture.tar.gz" \
    > "$output_root/mine-teleop-ubuntu22.04-$architecture.tar.gz.sha256"
else
  shasum -a 256 "$output_root/mine-teleop-ubuntu22.04-$architecture.tar.gz" \
    > "$output_root/mine-teleop-ubuntu22.04-$architecture.tar.gz.sha256"
fi

printf 'BUNDLE_DIR=%s\n' "$output_root"
