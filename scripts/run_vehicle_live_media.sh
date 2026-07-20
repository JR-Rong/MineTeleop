#!/usr/bin/env bash
set -euo pipefail

install_dir="${MINE_TELEOP_INSTALL_DIR:-/opt/mine-teleop}"
mine_teleop_bin="${MINE_TELEOP_BIN:-$install_dir/bin/mine-teleop}"
mine_teleop_loader="${MINE_TELEOP_LOADER:-$install_dir/lib/ld-linux-x86-64.so.2}"
mine_teleop_library_path="$install_dir/lib:$install_dir/lib/vendor/chassis:$install_dir/lib/vendor/mvs:$install_dir/lib/vendor/pylon"
config="${MINE_TELEOP_CONFIG_LIVE:-/etc/mine-teleop/vehicle-agent.yaml}"
signaling_http_url="${MINE_TELEOP_SIGNALING_HTTP_URL:-http://127.0.0.1:8765}"
device_token="${MINE_TELEOP_DEVICE_TOKEN:-}"
retry_seconds="${MINE_TELEOP_RETRY_SECONDS:-2}"
log_path="${MINE_TELEOP_MEDIA_LOG:-/var/log/mine-teleop/vehicle-media-live.jsonl}"

if [[ ! -x "$mine_teleop_bin" ]]; then
  printf 'native runtime is missing or not executable: %s\n' "$mine_teleop_bin" >&2
  exit 2
fi
if [[ ! -x "$mine_teleop_loader" ]]; then
  printf 'bundled dynamic loader is missing or not executable: %s\n' "$mine_teleop_loader" >&2
  exit 2
fi
if [[ ! -f "$config" ]]; then
  printf 'vehicle configuration is missing: %s\n' "$config" >&2
  exit 2
fi
if [[ -z "$device_token" ]]; then
  printf 'MINE_TELEOP_DEVICE_TOKEN is required\n' >&2
  exit 2
fi

mkdir -p "$(dirname "$log_path")"
export GST_PLUGIN_SYSTEM_PATH_1_0=
export GST_PLUGIN_PATH_1_0="$install_dir/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$install_dir/bin/gst-plugin-scanner"
export GST_REGISTRY="${MINE_TELEOP_GST_REGISTRY:-/var/tmp/mine-teleop-gstreamer-registry-${UID:-0}.bin}"
export LIBVA_DRIVERS_PATH="$install_dir/lib/dri"
export LD_LIBRARY_PATH="$mine_teleop_library_path"

run_mine_teleop() {
  "$mine_teleop_loader" --library-path "$mine_teleop_library_path" "$mine_teleop_bin" "$@"
}

run_mine_teleop config-check --config "$config"
run_mine_teleop vehicle-agent --config "$config" --preflight

while true; do
  if ! run_mine_teleop http-health --url "${signaling_http_url%/}/health" >/dev/null 2>&1; then
    printf 'signaling server is unavailable: %s\n' "$signaling_http_url" >&2
    sleep "$retry_seconds"
    continue
  fi
  if ! run_mine_teleop vehicle-media-agent \
    --config "$config" \
    --signaling-http-url "$signaling_http_url" \
    --device-token "$device_token" \
    --record \
    --recording-root "${MINE_TELEOP_RECORDING_ROOT:-/var/lib/mine-teleop/recordings}" \
    --service 2>&1 | tee -a "$log_path"; then
    printf 'native vehicle media agent exited; retrying in %ss\n' "$retry_seconds" >&2
  fi
  sleep "$retry_seconds"
done
