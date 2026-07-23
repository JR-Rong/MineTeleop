#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
image="mine-teleop-cpp-check:local"
build_image="$image-build"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-2}"
platform="${MINE_TELEOP_CHECK_PLATFORM:-}"
if [[ -n "$platform" ]]; then
  target_arch="${platform#linux/}"
else
  target_arch="$(docker info --format '{{.Architecture}}')"
  case "$target_arch" in
    aarch64) target_arch="arm64" ;;
    x86_64) target_arch="amd64" ;;
  esac
  platform="linux/$target_arch"
fi

docker build \
  --platform "$platform" \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --target build \
  -t "$build_image" \
  -f "$repo_root/deployments/cpp/Dockerfile.build" \
  "$repo_root"

docker run --rm --platform "$platform" "$build_image" /opt/mine-teleop/bin/mine-teleop version
docker run --rm --platform "$platform" "$build_image" /opt/mine-teleop/bin/mine-teleop config-check \
  --config /opt/mine-teleop/share/mine-teleop/configs/vehicle-agent.dev.yaml

if [[ "$target_arch" == "amd64" ]]; then
  docker build \
    --platform "$platform" \
    --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
    --target runtime \
    -t "$image" \
    -f "$repo_root/deployments/cpp/Dockerfile.build" \
    "$repo_root"
  docker run --rm --platform "$platform" "$image" version
  docker run --rm --platform "$platform" "$image" config-check \
    --config /opt/mine-teleop/share/mine-teleop/configs/vehicle-agent.dev.yaml
  docker run --rm --platform "$platform" --entrypoint /bin/sh "$image" -c '! command -v py""thon3 >/dev/null'
else
  printf 'runtime_bundle_check=skipped target=linux/amd64 current_arch=%s\n' "$target_arch"
fi

if rg -n '[p]ython3' \
  "$repo_root/scripts" \
  "$repo_root/cpp" \
  "$repo_root/CMakeLists.txt" \
  "$repo_root/README.md"; then
  printf 'production source still references Python\n' >&2
  exit 2
fi

if find "$repo_root" \
  -path "$repo_root/.git" -prune -o \
  -path "$repo_root/docs/superpowers" -prune -o \
  -name '*.py' -print -quit | grep -q .; then
  printf 'Python source remains in the active repository\n' >&2
  exit 2
fi

printf 'native_cpp_check=passed\n'
