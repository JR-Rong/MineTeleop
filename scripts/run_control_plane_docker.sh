#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
image="${MINE_TELEOP_CONTROL_PLANE_IMAGE:-mine-teleop-control-plane:local}"
server_name="${MINE_TELEOP_CONTROL_PLANE_SERVER:-mine-teleop-signaling-local}"
console_name="${MINE_TELEOP_CONTROL_PLANE_CONSOLE:-mine-teleop-driver-console-local}"
console_port="${MINE_TELEOP_CONTROL_PLANE_CONSOLE_PORT:-8080}"
signaling_port="${MINE_TELEOP_CONTROL_PLANE_SIGNALING_PORT:-8765}"
vehicle_id="${MINE_TELEOP_CONTROL_PLANE_VEHICLE_ID:-vehicle-001}"
password="${MINE_TELEOP_CONTROL_PLANE_PASSWORD:-dev-password}"

cleanup() {
  docker rm -f "$console_name" >/dev/null 2>&1 || true
  docker rm -f "$server_name" >/dev/null 2>&1 || true
}

stop_control_plane() {
  cleanup
  exit 0
}

trap cleanup EXIT
trap stop_control_plane INT TERM

docker build \
  -f "$repo_root/deployments/container/Dockerfile.control" \
  -t "$image" \
  "$repo_root"

cleanup

docker run -d \
  --name "$server_name" \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  -p "127.0.0.1:${signaling_port}:8765" \
  -p "127.0.0.1:${console_port}:8080" \
  -w /opt/mine-teleop \
  "$image" \
  sh -c 'mkdir -p /tmp/mine-teleop-control-plane && exec python3 signaling-server/signaling_server.py --serve --host 0.0.0.0 --port 8765 --audit-log /tmp/mine-teleop-control-plane/signaling-audit.jsonl --allow-insecure-nonloopback-dev' >/dev/null

for _ in $(seq 1 30); do
  if docker exec "$server_name" python3 -c \
    "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/health', timeout=2).read()" >/dev/null 2>&1; then
    break
  fi
  sleep 0.5
done

docker exec "$server_name" python3 -c \
  "import urllib.request; urllib.request.urlopen('http://127.0.0.1:8765/health', timeout=2).read()" >/dev/null

docker exec "$server_name" python3 -c \
  "import json, urllib.request; body=json.dumps({'vehicle_id':'$vehicle_id','device_token':'dev-device-secret'}).encode('utf-8'); req=urllib.request.Request('http://127.0.0.1:8765/vehicles/online', data=body, method='POST', headers={'Content-Type':'application/json'}); urllib.request.urlopen(req, timeout=2).read()" >/dev/null

docker run -d \
  --name "$console_name" \
  --network "container:$server_name" \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  -e MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL="http://127.0.0.1:8765" \
  -e MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID="$vehicle_id" \
  -e MINE_TELEOP_DRIVER_CONSOLE_PASSWORD="$password" \
  -e MINE_TELEOP_DRIVER_CONSOLE_HOST="0.0.0.0" \
  -e MINE_TELEOP_DRIVER_CONSOLE_PORT="8080" \
  -e MINE_TELEOP_DRIVER_CONSOLE_OPERATION_LOG="/tmp/mine-teleop-control-plane/driver-ops.jsonl" \
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

echo "DRIVER_CONSOLE_URL=http://127.0.0.1:${console_port}"
echo "SIGNALING_HTTP_URL=http://127.0.0.1:${signaling_port}"
echo "Press Ctrl-C to stop the local Docker control plane."

while true; do
  server_running="$(docker inspect -f '{{.State.Running}}' "$server_name" 2>/dev/null || true)"
  console_running="$(docker inspect -f '{{.State.Running}}' "$console_name" 2>/dev/null || true)"
  if [ "$server_running" != "true" ] || [ "$console_running" != "true" ]; then
    echo "One or more control-plane containers exited." >&2
    docker logs "$server_name" >&2 || true
    docker logs "$console_name" >&2 || true
    exit 1
  fi
  sleep 2
done
