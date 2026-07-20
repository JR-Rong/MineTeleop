#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
artifact_dir="${MINE_TELEOP_BROWSER_SMOKE_ARTIFACT_DIR:-$repo_root/.local/control-plane-browser-smoke/$(date +%Y%m%d-%H%M%S)}"
console_port="${MINE_TELEOP_BROWSER_SMOKE_CONSOLE_PORT:-18081}"
server_name="${MINE_TELEOP_BROWSER_SMOKE_SERVER:-mine-teleop-signaling-browser-smoke-$$}"
console_name="${MINE_TELEOP_BROWSER_SMOKE_CONSOLE:-mine-teleop-driver-console-browser-smoke-$$}"
runner_log="$artifact_dir/control-plane-runner.log"

mkdir -p "$artifact_dir"

cleanup() {
  if [ -n "${runner_pid:-}" ]; then
    kill -TERM "$runner_pid" >/dev/null 2>&1 || true
    wait "$runner_pid" >/dev/null 2>&1 || true
  fi
  docker rm -f "$console_name" "$server_name" >/dev/null 2>&1 || true
}
trap cleanup EXIT

export MINE_TELEOP_CONTROL_PLANE_SERVER="$server_name"
export MINE_TELEOP_CONTROL_PLANE_CONSOLE="$console_name"
export MINE_TELEOP_CONTROL_PLANE_CONSOLE_PORT="$console_port"

"$repo_root/scripts/run_control_plane_docker.sh" >"$runner_log" 2>&1 &
runner_pid=$!

for _ in $(seq 1 80); do
  if grep -q "DRIVER_CONSOLE_URL=http://127.0.0.1:${console_port}" "$runner_log"; then
    break
  fi
  if ! kill -0 "$runner_pid" >/dev/null 2>&1; then
    cat "$runner_log" >&2 || true
    exit 1
  fi
  sleep 0.5
done

if ! grep -q "DRIVER_CONSOLE_URL=http://127.0.0.1:${console_port}" "$runner_log"; then
  cat "$runner_log" >&2 || true
  echo "driver console URL was not printed" >&2
  exit 1
fi

"$repo_root/scripts/control_plane_browser_smoke.py" \
  --driver-console-url "http://127.0.0.1:${console_port}" \
  --signaling-container "$server_name" \
  --artifact-dir "$artifact_dir"

printf 'BROWSER_SMOKE_ARTIFACT_DIR=%s\n' "$artifact_dir"
