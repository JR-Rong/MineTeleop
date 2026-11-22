#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
image="${MINE_TELEOP_CONTROL_PLANE_IMAGE:-mine-teleop-control-plane:local}"
server_name="${MINE_TELEOP_CONTROL_PLANE_SERVER:-mine-teleop-signaling-local}"
console_name="${MINE_TELEOP_CONTROL_PLANE_CONSOLE:-mine-teleop-driver-console-local}"
console_port="${MINE_TELEOP_CONTROL_PLANE_CONSOLE_PORT:-8080}"
signaling_port="${MINE_TELEOP_CONTROL_PLANE_SIGNALING_PORT:-8765}"
vehicle_id="${MINE_TELEOP_CONTROL_PLANE_VEHICLE_ID:-vehicle-001}"
password="${MINE_TELEOP_CONTROL_PLANE_PASSWORD:-dev-password}"
device_token="${MINE_TELEOP_DEVICE_TOKEN:-dev-device-secret}"

# Host networking keeps the browser-facing console on the host loopback.
# Docker Desktop's bridge cannot publish a container-loopback listener.
if [ "$(uname -s)" != "Linux" ]; then
  printf '%s\n' 'Interactive Docker control plane requires Linux host networking. On macOS/Windows use the native desktop client; for isolated HTTP checks use scripts/test/run_control_plane_docker_smoke.sh.' >&2
  exit 2
fi

cleanup() {
  docker rm -f "$console_name" >/dev/null 2>&1 || true
  docker rm -f "$server_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM

docker build -f "$repo_root/deployments/container/Dockerfile.control" -t "$image" "$repo_root"
cleanup

docker run -d \
  --name "$server_name" \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  -e MINE_TELEOP_DRIVER_PASSWORD="$password" \
  -e MINE_TELEOP_DEVICE_TOKEN="$device_token" \
  --network host \
  "$image" signaling-server \
    --host 127.0.0.1 --port "$signaling_port" \
    --vehicle-id "$vehicle_id" \
    --audit-log /tmp/signaling-audit.jsonl >/dev/null

for _ in $(seq 1 30); do
  if docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop \
    http-health --url "http://127.0.0.1:${signaling_port}/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done
docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop \
  http-health --url "http://127.0.0.1:${signaling_port}/health" >/dev/null
docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop \
  vehicle-online --signaling-http-url "http://127.0.0.1:${signaling_port}" \
  --vehicle-id "$vehicle_id" --device-token "$device_token" >/dev/null

docker run -d \
  --name "$console_name" \
  --network host \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  -e MINE_TELEOP_DRIVER_PASSWORD="$password" \
  "$image" driver-console \
    --config /opt/mine-teleop/share/mine-teleop/configs/driver-console.dev.yaml \
    --host 127.0.0.1 --port "$console_port" \
    --signaling-http-url "http://127.0.0.1:${signaling_port}" \
    --vehicle-id "$vehicle_id" >/dev/null

for _ in $(seq 1 30); do
  if docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop \
    http-health --url "http://127.0.0.1:${console_port}/health" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done
docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop \
  http-health --url "http://127.0.0.1:${console_port}/health" >/dev/null

printf 'DRIVER_CONSOLE_URL=http://127.0.0.1:%s\n' "$console_port"
printf 'SIGNALING_HTTP_URL=http://127.0.0.1:%s\n' "$signaling_port"
printf 'Press Ctrl-C to stop the native C++ control plane.\n'

while true; do
  server_running="$(docker inspect -f '{{.State.Running}}' "$server_name" 2>/dev/null || true)"
  console_running="$(docker inspect -f '{{.State.Running}}' "$console_name" 2>/dev/null || true)"
  if [ "$server_running" != "true" ] || [ "$console_running" != "true" ]; then
    docker logs "$server_name" >&2 || true
    docker logs "$console_name" >&2 || true
    exit 1
  fi
  sleep 2
done
