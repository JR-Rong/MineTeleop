#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
image="${MINE_TELEOP_DRIVER_CONSOLE_IMAGE:-mine-teleop-driver-console:local}"
container_name="${MINE_TELEOP_DRIVER_CONSOLE_CONTAINER:-mine-teleop-driver-console}"
host_port="${MINE_TELEOP_DRIVER_CONSOLE_PORT:-8080}"
signaling_url="${MINE_TELEOP_DRIVER_CONSOLE_SIGNALING_HTTP_URL:-http://host.docker.internal:8765}"
vehicle_id="${MINE_TELEOP_DRIVER_CONSOLE_VEHICLE_ID:-vehicle-001}"
password="${MINE_TELEOP_DRIVER_CONSOLE_PASSWORD:-dev-password}"

docker build -f "$repo_root/deployments/container/Dockerfile.control" -t "$image" "$repo_root"
docker rm -f "$container_name" >/dev/null 2>&1 || true

exec docker run --rm \
  --name "$container_name" \
  --security-opt "no-new-privileges:true" \
  --cap-drop ALL \
  --add-host host.docker.internal:host-gateway \
  -e MINE_TELEOP_DRIVER_PASSWORD="$password" \
  -p "127.0.0.1:${host_port}:8080" \
  "$image" driver-console \
    --config /opt/mine-teleop/share/mine-teleop/configs/driver-console.dev.yaml \
    --host 0.0.0.0 --port 8080 \
    --signaling-http-url "$signaling_url" \
    --vehicle-id "$vehicle_id"
