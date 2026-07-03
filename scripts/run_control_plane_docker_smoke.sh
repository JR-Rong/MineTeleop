#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
image="${MINE_TELEOP_CONTROL_SMOKE_IMAGE:-mine-teleop-control-smoke:local}"
server_name="${MINE_TELEOP_CONTROL_SMOKE_SERVER:-mine-teleop-signaling-smoke-$$}"
console_name="${MINE_TELEOP_CONTROL_SMOKE_CONSOLE:-mine-teleop-driver-console-smoke-$$}"
smoke_name="${MINE_TELEOP_CONTROL_SMOKE_CONTAINER:-mine-teleop-control-smoke-$$}"
artifact_dir="${MINE_TELEOP_CONTROL_SMOKE_ARTIFACT_DIR:-$repo_root/.local/control-plane-smoke/$(date +%Y%m%d-%H%M%S)}"

cleanup() {
  docker rm -f "$smoke_name" >/dev/null 2>&1 || true
  docker rm -f "$console_name" >/dev/null 2>&1 || true
  docker rm -f "$server_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT

mkdir -p "$artifact_dir"

if [ "${MINE_TELEOP_CONTROL_SMOKE_SKIP_BUILD:-0}" != "1" ]; then
  docker build \
    -f "$repo_root/deployments/container/Dockerfile.control" \
    -t "$image" \
    "$repo_root"
fi

docker rm -f "$server_name" "$console_name" "$smoke_name" >/dev/null 2>&1 || true

docker run -d \
  --name "$server_name" \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  -w /opt/mine-teleop \
  "$image" \
  sh -c 'mkdir -p /tmp/mine-teleop-control-smoke && exec python3 signaling-server/signaling_server.py --serve --host 127.0.0.1 --port 8765 --audit-log /tmp/mine-teleop-control-smoke/signaling-audit.jsonl' >/dev/null

for _ in $(seq 1 30); do
  if docker exec "$server_name" python3 -c \
    "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/health', timeout=2).read()" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

docker exec "$server_name" python3 -c \
  "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/health', timeout=2).read()" >/dev/null

docker run -d \
  --name "$console_name" \
  --network "container:$server_name" \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  -e MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL=http://127.0.0.1:8765 \
  -e MINE_TELEOP_DRIVER_CONSOLE_HOST=127.0.0.1 \
  -e MINE_TELEOP_DRIVER_CONSOLE_PORT=8080 \
  -e MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID=vehicle-001 \
  -e MINE_TELEOP_DRIVER_CONSOLE_PASSWORD=dev-password \
  -e MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG=/tmp/mine-teleop-control-smoke/driver-ops.jsonl \
  -w /opt/mine-teleop \
  "$image" >/dev/null

for _ in $(seq 1 30); do
  if docker exec "$server_name" python3 -c \
    "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/health', timeout=2).read()" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

docker exec "$server_name" python3 -c \
  "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8080/health', timeout=2).read()" >/dev/null

docker create \
  --name "$smoke_name" \
  --network "container:$server_name" \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  -w /opt/mine-teleop \
  "$image" \
  python3 scripts/control_plane_smoke.py \
    --signaling-url http://127.0.0.1:8765 \
    --driver-console-url http://127.0.0.1:8080 \
    --artifact-dir /tmp/mine-teleop-control-smoke >/dev/null

set +e
docker start -a "$smoke_name" | tee "$artifact_dir/control-plane-smoke.log"
smoke_status=${PIPESTATUS[0]}
set -e

docker cp "$smoke_name:/tmp/mine-teleop-control-smoke/." "$artifact_dir/" >/dev/null 2>&1 || true

if [ "$smoke_status" -ne 0 ]; then
  docker logs "$server_name" >"$artifact_dir/signaling-server.log" 2>&1 || true
  docker logs "$console_name" >"$artifact_dir/driver-console.log" 2>&1 || true
  printf 'CONTROL_PLANE_SMOKE_ARTIFACT_DIR=%s\n' "$artifact_dir"
  exit "$smoke_status"
fi

printf 'CONTROL_PLANE_SMOKE_ARTIFACT_DIR=%s\n' "$artifact_dir"
