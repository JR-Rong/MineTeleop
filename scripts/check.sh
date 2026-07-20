#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
image="mine-teleop-cpp-check:local"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-2}"

docker build \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --target build \
  -f "$repo_root/deployments/cpp/Dockerfile.build" \
  "$repo_root"

docker build \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --target runtime \
  -t "$image" \
  -f "$repo_root/deployments/cpp/Dockerfile.build" \
  "$repo_root"

docker run --rm "$image" version
docker run --rm "$image" config-check \
  --config /opt/mine-teleop/share/mine-teleop/configs/vehicle-agent.dev.yaml

if rg -n '[p]ython3' \
  "$repo_root/deployments" \
  "$repo_root/scripts" \
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
