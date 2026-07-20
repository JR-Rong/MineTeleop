#!/usr/bin/env bash
set -euo pipefail

install_dir="${MINE_TELEOP_INSTALL_DIR:-/opt/mine-teleop}"
mine_teleop_bin="${MINE_TELEOP_BIN:-$install_dir/mine-teleop}"
config="${MINE_TELEOP_CONFIG_LIVE:-/etc/mine-teleop/vehicle-agent.yaml}"
driver_console_url="${MINE_TELEOP_DRIVER_CONSOLE_URL:-http://127.0.0.1:8080}"
retry_seconds="${MINE_TELEOP_RETRY_SECONDS:-2}"
log_path="${MINE_TELEOP_MEDIA_LOG:-/var/log/mine-teleop/vehicle-media-live.jsonl}"

if [[ ! -x "$mine_teleop_bin" ]]; then
  printf 'native runtime is missing or not executable: %s\n' "$mine_teleop_bin" >&2
  exit 2
fi
if [[ ! -f "$config" ]]; then
  printf 'vehicle configuration is missing: %s\n' "$config" >&2
  exit 2
fi

mkdir -p "$(dirname "$log_path")"
"$mine_teleop_bin" config-check --config "$config"
"$mine_teleop_bin" vehicle-agent --config "$config" --preflight

while true; do
  if ! "$mine_teleop_bin" http-health --url "${driver_console_url%/}/health" >/dev/null 2>&1; then
    printf 'driver console is unavailable: %s\n' "$driver_console_url" >&2
    sleep "$retry_seconds"
    continue
  fi
  if ! "$mine_teleop_bin" vehicle-media-agent \
    --config "$config" \
    --driver-console-url "$driver_console_url" \
    --record \
    --recording-root "${MINE_TELEOP_RECORDING_ROOT:-/var/lib/mine-teleop/recordings}" \
    --service 2>&1 | tee -a "$log_path"; then
    printf 'native vehicle media agent exited; retrying in %ss\n' "$retry_seconds" >&2
  fi
  sleep "$retry_seconds"
done
