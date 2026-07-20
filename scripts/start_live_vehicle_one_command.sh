#!/usr/bin/env bash
set -euo pipefail

# Edit this block once, then run this script on the vehicle:
#   scripts/start_live_vehicle_one_command.sh
INSTALL_DIR="/home/user/mine-teleop"
CONFIG="/etc/mine-teleop/vehicle-agent.yaml"
SIGNALING_HTTP_URL="http://127.0.0.1:8765"
DEVICE_TOKEN="replace-with-device-token"
RECORDING_ROOT="/var/lib/mine-teleop/recordings"
MEDIA_LOG="/var/log/mine-teleop/vehicle-media-live.jsonl"
RETRY_SECONDS="2"
STOP_EXISTING_MEDIA="1"

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"

stop_existing_media() {
  local pids
  pids="$(
    ps -eo pid,args | awk -v install_dir="$INSTALL_DIR" '
      ($2=="bash" && ($3=="scripts/run_vehicle_live_media.sh" || $3==install_dir "/scripts/run_vehicle_live_media.sh")) ||
      ($2==install_dir "/lib/ld-linux-x86-64.so.2" && $0 ~ /bin\/mine-teleop/ &&
        ($0 ~ /vehicle-media-agent/ || $0 ~ /mine-teleop-mvs-camera/)) ||
      ($2==install_dir "/bin/mine-teleop-aravis-camera") ||
      ($2==install_dir "/bin/mine-teleop-mvs-camera") {
        print $1
      }
    '
  )"
  if [[ -n "$pids" ]]; then
    # shellcheck disable=SC2086
    kill -TERM $pids 2>/dev/null || true
    sleep 2
  fi
}

if [[ "$STOP_EXISTING_MEDIA" == "1" ]]; then
  stop_existing_media
fi

cd "$INSTALL_DIR"
exec env \
  MINE_TELEOP_INSTALL_DIR="$INSTALL_DIR" \
  MINE_TELEOP_CONFIG_LIVE="$CONFIG" \
  MINE_TELEOP_SIGNALING_HTTP_URL="$SIGNALING_HTTP_URL" \
  MINE_TELEOP_DEVICE_TOKEN="$DEVICE_TOKEN" \
  MINE_TELEOP_RECORDING_ROOT="$RECORDING_ROOT" \
  MINE_TELEOP_MEDIA_LOG="$MEDIA_LOG" \
  MINE_TELEOP_RETRY_SECONDS="$RETRY_SECONDS" \
  "$SCRIPT_DIR/run_vehicle_live_media.sh"
