#!/usr/bin/env bash
set -euo pipefail

INSTALL_DIR="${MINE_TELEOP_INSTALL_DIR:-/home/user/mine-teleop}"
DRIVER_CONSOLE_URL="${MINE_TELEOP_DRIVER_CONSOLE_URL:-http://127.0.0.1:18080}"
CONFIG_SOURCE="${MINE_TELEOP_CONFIG_SOURCE:-$INSTALL_DIR/configs/vehicle-agent.dev.yaml}"
CONFIG_LIVE="${MINE_TELEOP_CONFIG_LIVE:-$INSTALL_DIR/configs/vehicle-agent.live.yaml}"
MINE_TELEOP_BIN="${MINE_TELEOP_BIN:-$INSTALL_DIR/bin/mine-teleop}"
FFMPEG_BIN="${MINE_TELEOP_FFMPEG_BIN:-$INSTALL_DIR/bin/ffmpeg}"
LOG_PATH="${MINE_TELEOP_MEDIA_LOG:-$INSTALL_DIR/logs/vehicle-media-live.jsonl}"
FRAMES="${MINE_TELEOP_MEDIA_FRAMES:-30}"
FRAME_INTERVAL_MS="${MINE_TELEOP_FRAME_INTERVAL_MS:-33}"
CAPTURE_WIDTH="${MINE_TELEOP_CAPTURE_WIDTH:-1280}"
CAPTURE_HEIGHT="${MINE_TELEOP_CAPTURE_HEIGHT:-720}"
CAPTURE_FPS="${MINE_TELEOP_CAPTURE_FPS:-10}"
CAMERA_DEVICE="${MINE_TELEOP_CAMERA_DEVICE:-}"

find_camera_device() {
  if [[ -n "$CAMERA_DEVICE" ]]; then
    [[ -e "$CAMERA_DEVICE" ]] || {
      echo "configured camera device does not exist: $CAMERA_DEVICE" >&2
      return 1
    }
    printf '%s\n' "$CAMERA_DEVICE"
    return
  fi

  if ! command -v v4l2-ctl >/dev/null 2>&1; then
    echo "v4l2-ctl is required for camera auto-detection; set MINE_TELEOP_CAMERA_DEVICE=/dev/videoN" >&2
    return 1
  fi

  local dev
  for dev in /dev/video*; do
    [[ -e "$dev" ]] || continue
    if v4l2-ctl -d "$dev" --all 2>/dev/null | grep -q "Video Capture"; then
      printf '%s\n' "$dev"
      return
    fi
  done

  echo "no Video Capture device found under /dev/video*" >&2
  return 1
}

write_live_config() {
  local camera_device="$1"
  mkdir -p "$(dirname "$CONFIG_LIVE")" "$(dirname "$LOG_PATH")"
  python3 - "$CONFIG_SOURCE" "$CONFIG_LIVE" "$camera_device" "$CAPTURE_WIDTH" "$CAPTURE_HEIGHT" "$CAPTURE_FPS" <<'PY'
from pathlib import Path
import sys

source, dest, camera, width, height, fps = sys.argv[1:]
text = Path(source).read_text(encoding="utf-8")
for old, new in [
    ("device: testsrc", f"device: {camera}"),
    ("capture_width: 1920", f"capture_width: {width}"),
    ("capture_height: 1080", f"capture_height: {height}"),
    ("capture_fps: 30", f"capture_fps: {fps}"),
]:
    text = text.replace(old, new, 1)
Path(dest).write_text(text, encoding="utf-8")
PY
}

camera="$(find_camera_device)"
write_live_config "$camera"

echo "Using camera: $camera"
echo "Driver console: $DRIVER_CONSOLE_URL"
echo "Live config: $CONFIG_LIVE"
echo "Log: $LOG_PATH"

while true; do
  "$MINE_TELEOP_BIN" vehicle-media-agent \
    --config "$CONFIG_LIVE" \
    --mode teleop \
    --driver-console-url "$DRIVER_CONSOLE_URL" \
    --frames "$FRAMES" \
    --frame-interval-ms "$FRAME_INTERVAL_MS" \
    --ffmpeg-binary "$FFMPEG_BIN" \
    --json | tee -a "$LOG_PATH"
  sleep 1
done
