#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${MINE_TELEOP_INSTALL_DIR:-/home/user/mine-teleop}"
DRIVER_CONSOLE_URL="${MINE_TELEOP_DRIVER_CONSOLE_URL:-http://127.0.0.1:18080}"
CONFIG_SOURCE="${MINE_TELEOP_CONFIG_SOURCE:-$INSTALL_DIR/configs/vehicle-agent.dev.yaml}"
CONFIG_LIVE="${MINE_TELEOP_CONFIG_LIVE:-$INSTALL_DIR/configs/vehicle-agent.live.yaml}"
MINE_TELEOP_BIN="${MINE_TELEOP_BIN:-$INSTALL_DIR/bin/mine-teleop}"
FFMPEG_BIN="${MINE_TELEOP_FFMPEG_BIN:-$INSTALL_DIR/bin/ffmpeg}"
LOG_PATH="${MINE_TELEOP_MEDIA_LOG:-$INSTALL_DIR/logs/vehicle-media-live.jsonl}"
FRAMES="${MINE_TELEOP_MEDIA_FRAMES:-300}"
FRAME_INTERVAL_MS="${MINE_TELEOP_FRAME_INTERVAL_MS:-33}"
STREAM_DURATION_MS="${MINE_TELEOP_STREAM_DURATION_MS:-60000}"
FRAME_CODEC="${MINE_TELEOP_FRAME_CODEC:-mjpeg}"
CAPTURE_WIDTH="${MINE_TELEOP_CAPTURE_WIDTH:-1280}"
CAPTURE_HEIGHT="${MINE_TELEOP_CAPTURE_HEIGHT:-720}"
CAPTURE_FPS="${MINE_TELEOP_CAPTURE_FPS:-30}"
CAMERA_DEVICE="${MINE_TELEOP_CAMERA_DEVICE:-}"
CAMERA_DEVICES="${MINE_TELEOP_CAMERA_DEVICES:-}"
RETRY_SECONDS="${MINE_TELEOP_RETRY_SECONDS:-2}"
HEALTH_RETRIES="${MINE_TELEOP_HEALTH_RETRIES:-5}"
HEALTH_URL="${DRIVER_CONSOLE_URL%/}/health"
LOW_LIGHT="${MINE_TELEOP_CAMERA_LOW_LIGHT:-1}"
CAMERA_BRIGHTNESS="${MINE_TELEOP_CAMERA_BRIGHTNESS:-24}"
CAMERA_GAIN="${MINE_TELEOP_CAMERA_GAIN:-96}"
CAMERA_GAMMA="${MINE_TELEOP_CAMERA_GAMMA:-450}"
CAMERA_BACKLIGHT="${MINE_TELEOP_CAMERA_BACKLIGHT:-2}"
CAMERA_EXPOSURE_DYNAMIC_FRAMERATE="${MINE_TELEOP_CAMERA_EXPOSURE_DYNAMIC_FRAMERATE:-1}"
CAMERA_AUTO_EXPOSURE="${MINE_TELEOP_CAMERA_AUTO_EXPOSURE:-3}"
CAMERA_EXPOSURE_ABSOLUTE="${MINE_TELEOP_CAMERA_EXPOSURE_ABSOLUTE:-}"

camera_id_for_index() {
  case "$1" in
    0) printf 'front\n' ;;
    1) printf 'rear\n' ;;
    2) printf 'left\n' ;;
    3) printf 'right\n' ;;
    *) printf 'camera%s\n' "$((1 + $1))" ;;
  esac
}

normalize_camera_device_pairs() {
  local raw="$1"
  local index=0
  local token
  for token in $raw; do
    if [[ "$token" == *=* ]]; then
      printf '%s\n' "$token"
    else
      printf '%s=%s\n' "$(camera_id_for_index "$index")" "$token"
    fi
    index=$((index + 1))
  done
}

find_camera_devices() {
  if [[ -n "$CAMERA_DEVICES" ]]; then
    normalize_camera_device_pairs "$CAMERA_DEVICES"
    return
  fi
  if [[ -n "$CAMERA_DEVICE" ]]; then
    [[ -e "$CAMERA_DEVICE" ]] || {
      echo "configured camera device does not exist: $CAMERA_DEVICE" >&2
      return 1
    }
    printf 'front=%s\n' "$CAMERA_DEVICE"
    return
  fi

  if ! command -v v4l2-ctl >/dev/null 2>&1; then
    echo "v4l2-ctl is required for camera auto-detection; set MINE_TELEOP_CAMERA_DEVICES='front=/dev/videoN ...'" >&2
    return 1
  fi

  local dev
  local index=0
  for dev in /dev/video*; do
    [[ -e "$dev" ]] || continue
    if ! v4l2-ctl -d "$dev" --all 2>/dev/null | grep -q "Video Capture"; then
      continue
    fi
    printf '%s=%s\n' "$(camera_id_for_index "$index")" "$dev"
    index=$((index + 1))
  done

  if [[ "$index" -gt 0 ]]; then
    return 0
  fi

  echo "no Video Capture device found under /dev/video*" >&2
  return 1
}

configure_camera_device() {
  local camera_device="$1"
  if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl -d "$camera_device" \
      --set-fmt-video="width=${CAPTURE_WIDTH},height=${CAPTURE_HEIGHT},pixelformat=MJPG" \
      --set-parm="$CAPTURE_FPS" >/dev/null 2>&1 || true
    if [[ "$LOW_LIGHT" == "1" ]]; then
      v4l2-ctl -d "$camera_device" \
        -c "auto_exposure=${CAMERA_AUTO_EXPOSURE}" \
        -c "brightness=${CAMERA_BRIGHTNESS}" \
        -c "gain=${CAMERA_GAIN}" \
        -c "gamma=${CAMERA_GAMMA}" \
        -c "backlight_compensation=${CAMERA_BACKLIGHT}" \
        -c "exposure_dynamic_framerate=${CAMERA_EXPOSURE_DYNAMIC_FRAMERATE}" >/dev/null 2>&1 || true
      if [[ -n "$CAMERA_EXPOSURE_ABSOLUTE" ]]; then
        v4l2-ctl -d "$camera_device" \
          -c auto_exposure=1 \
          -c "exposure_time_absolute=${CAMERA_EXPOSURE_ABSOLUTE}" >/dev/null 2>&1 || true
      fi
    fi
  fi
}

wait_for_driver_console() {
  for _ in $(seq 1 "$HEALTH_RETRIES"); do
    if curl -fsS --max-time 5 "$HEALTH_URL" >/dev/null 2>&1; then
      return 0
    fi
    sleep 1
  done

  cat >&2 <<EOF
driver console is not reachable from the vehicle: $HEALTH_URL
Start or repair the reverse tunnel from the local machine first:
  MINE_TELEOP_VEHICLE_SSH_PASSWORD=... scripts/start_live_control_plane_tunnel.sh
EOF
  return 1
}

write_live_config() {
  mkdir -p "$(dirname "$CONFIG_LIVE")" "$(dirname "$LOG_PATH")"
  python3 - "$CONFIG_SOURCE" "$CONFIG_LIVE" "$CAPTURE_WIDTH" "$CAPTURE_HEIGHT" "$CAPTURE_FPS" "$@" <<'PY'
from pathlib import Path
import sys

source, dest, width, height, fps, *pairs = sys.argv[1:]
text = Path(source).read_text(encoding="utf-8")
if "\ncameras:\n" not in text or "\nhardware:\n" not in text:
    raise SystemExit("source config must contain top-level cameras and hardware sections")
prefix, rest = text.split("\ncameras:\n", 1)
_old_cameras, suffix = rest.split("\nhardware:\n", 1)
lines = [prefix.rstrip(), "", "cameras:"]
for pair in pairs:
    camera_id, device = pair.split("=", 1)
    lines.extend(
        [
            f"  - id: {camera_id}",
            "    enabled: true",
            f"    device: {device}",
            f"    capture_width: {width}",
            f"    capture_height: {height}",
            f"    capture_fps: {fps}",
            "    realtime_profile: realtime_720p",
            "    record_profile: record_source_h264",
        ]
    )
lines.extend(["", "hardware:", suffix.lstrip()])
text = "\n".join(lines)
Path(dest).write_text(text, encoding="utf-8")
PY
}

mapfile -t camera_device_pairs < <(find_camera_devices)
for pair in "${camera_device_pairs[@]}"; do
  camera_id="${pair%%=*}"
  camera_device="${pair#*=}"
  [[ -e "$camera_device" ]] || {
    echo "configured camera device does not exist for $camera_id: $camera_device" >&2
    exit 1
  }
  configure_camera_device "$camera_device"
done
write_live_config "${camera_device_pairs[@]}"

echo "Using cameras: ${camera_device_pairs[*]}"
echo "Frame codec: $FRAME_CODEC"
echo "Low light profile: $LOW_LIGHT"
echo "Driver console: $DRIVER_CONSOLE_URL"
echo "Driver console health: $HEALTH_URL"
echo "Live config: $CONFIG_LIVE"
echo "Log: $LOG_PATH"

while true; do
  if ! wait_for_driver_console; then
    sleep "$RETRY_SECONDS"
    continue
  fi
  if ! "$MINE_TELEOP_BIN" vehicle-media-agent \
    --config "$CONFIG_LIVE" \
    --mode teleop \
    --driver-console-url "$DRIVER_CONSOLE_URL" \
    --stream \
    --duration-ms "$STREAM_DURATION_MS" \
    --frames "$FRAMES" \
    --frame-interval-ms "$FRAME_INTERVAL_MS" \
    --frame-codec "$FRAME_CODEC" \
    --ffmpeg-binary "$FFMPEG_BIN" \
    --json | tee -a "$LOG_PATH"; then
    echo "vehicle-media-agent exited; retrying in ${RETRY_SECONDS}s" >&2
  fi
  sleep 1
done
