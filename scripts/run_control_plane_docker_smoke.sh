#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
image="${MINE_TELEOP_CONTROL_SMOKE_IMAGE:-mine-teleop-control-smoke:local}"
server_name="${MINE_TELEOP_CONTROL_SMOKE_SERVER:-mine-teleop-signaling-smoke-$$}"
console_name="${MINE_TELEOP_CONTROL_SMOKE_CONSOLE:-mine-teleop-driver-console-smoke-$$}"
artifact_dir="${MINE_TELEOP_CONTROL_SMOKE_ARTIFACT_DIR:-$repo_root/.local/control-plane-smoke/$(date +%Y%m%d-%H%M%S)}"

cleanup() {
  docker rm -f "$console_name" >/dev/null 2>&1 || true
  docker rm -f "$server_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT
mkdir -p "$artifact_dir"

if [ "${MINE_TELEOP_CONTROL_SMOKE_SKIP_BUILD:-0}" != "1" ]; then
  docker build -f "$repo_root/deployments/container/Dockerfile.control" -t "$image" "$repo_root"
fi

docker run -d --name "$server_name" --security-opt "no-new-privileges:true" --cap-drop ALL \
  "$image" signaling-server --host 127.0.0.1 --port 8765 >/dev/null

for _ in $(seq 1 30); do
  if docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop \
    http-health --url http://127.0.0.1:8765/health >/dev/null 2>&1; then break; fi
  sleep 0.5
done

docker run -d --name "$console_name" --network "container:$server_name" \
  --security-opt "no-new-privileges:true" --cap-drop ALL \
  "$image" driver-console \
    --config /opt/mine-teleop/share/mine-teleop/configs/driver-console.dev.yaml \
    --host 127.0.0.1 --port 8080 \
    --signaling-http-url http://127.0.0.1:8765 >/dev/null

for _ in $(seq 1 30); do
  if docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop \
    http-health --url http://127.0.0.1:8080/health >/dev/null 2>&1; then break; fi
  sleep 0.5
done

docker exec "$server_name" /opt/mine-teleop/bin/mine-teleop control-smoke \
  --signaling-http-url http://127.0.0.1:8765 \
  --driver-console-url http://127.0.0.1:8080 \
  --config /opt/mine-teleop/share/mine-teleop/configs/vehicle-agent.dev.yaml \
  | tee "$artifact_dir/control-plane-smoke.jsonl"

docker logs "$server_name" >"$artifact_dir/signaling-server.log" 2>&1 || true
docker logs "$console_name" >"$artifact_dir/driver-console.log" 2>&1 || true
printf 'CONTROL_PLANE_SMOKE_ARTIFACT_DIR=%s\n' "$artifact_dir"
