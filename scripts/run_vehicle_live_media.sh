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
REALTIME_PROFILE="${MINE_TELEOP_REALTIME_PROFILE:-realtime_720p}"
CAPTURE_WIDTH="${MINE_TELEOP_CAPTURE_WIDTH:-1280}"
CAPTURE_HEIGHT="${MINE_TELEOP_CAPTURE_HEIGHT:-720}"
CAPTURE_FPS="${MINE_TELEOP_CAPTURE_FPS:-30}"
CAMERA_DEVICE="${MINE_TELEOP_CAMERA_DEVICE:-}"
CAMERA_DEVICES="${MINE_TELEOP_CAMERA_DEVICES:-}"
ENABLE_MVS_CAMERA="${MINE_TELEOP_ENABLE_MVS_CAMERA:-1}"
MVS_SDK_DIR="${MINE_TELEOP_MVS_SDK_DIR:-/opt/MVS}"
MVS_CAMERAS="${MINE_TELEOP_MVS_CAMERAS:-}"
MVS_CAPTURE_WIDTH="${MINE_TELEOP_MVS_CAPTURE_WIDTH:-1280}"
MVS_CAPTURE_HEIGHT="${MINE_TELEOP_MVS_CAPTURE_HEIGHT:-1024}"
MVS_CAPTURE_FPS="${MINE_TELEOP_MVS_CAPTURE_FPS:-15}"
MVS_JPEG_QUALITY="${MINE_TELEOP_MVS_JPEG_QUALITY:-80}"
ENABLE_PYLON_CAMERA="${MINE_TELEOP_ENABLE_PYLON_CAMERA:-1}"
PYLON_ROOT="${MINE_TELEOP_PYLON_ROOT:-/opt/pylon}"
PYLON_BRIDGE_BIN="${MINE_TELEOP_PYLON_BRIDGE_BIN:-$INSTALL_DIR/bin/pylon-camera-bridge}"
PYLON_CAMERAS="${MINE_TELEOP_PYLON_CAMERAS:-}"
PYLON_CAPTURE_WIDTH="${MINE_TELEOP_PYLON_CAPTURE_WIDTH:-1280}"
PYLON_CAPTURE_HEIGHT="${MINE_TELEOP_PYLON_CAPTURE_HEIGHT:-1024}"
PYLON_CAPTURE_FPS="${MINE_TELEOP_PYLON_CAPTURE_FPS:-15}"
export MINE_TELEOP_MVS_JPEG_QUALITY="$MVS_JPEG_QUALITY"
export MINE_TELEOP_PYLON_BRIDGE_BIN="$PYLON_BRIDGE_BIN"
if [[ -d "$PYLON_ROOT/lib" ]]; then
  export LD_LIBRARY_PATH="$PYLON_ROOT/lib:${LD_LIBRARY_PATH:-}"
fi
RETRY_SECONDS="${MINE_TELEOP_RETRY_SECONDS:-2}"
HEALTH_RETRIES="${MINE_TELEOP_HEALTH_RETRIES:-5}"
HEALTH_URL="${DRIVER_CONSOLE_URL%/}/health"
REQUESTED_CAMERA_CONTROL_PROFILE="${MINE_TELEOP_CAMERA_CONTROL_PROFILE:-}"
if [[ -n "$REQUESTED_CAMERA_CONTROL_PROFILE" ]]; then
  CAMERA_CONTROL_PROFILE="$REQUESTED_CAMERA_CONTROL_PROFILE"
elif [[ "${MINE_TELEOP_CAMERA_LOW_LIGHT:-}" == "0" ]]; then
  CAMERA_CONTROL_PROFILE="off"
elif [[ "${MINE_TELEOP_CAMERA_LOW_LIGHT:-}" == "1" ]]; then
  CAMERA_CONTROL_PROFILE="low_light"
else
  CAMERA_CONTROL_PROFILE="adaptive"
fi
CAMERA_BRIGHTNESS="${MINE_TELEOP_CAMERA_BRIGHTNESS:-24}"
CAMERA_CONTRAST="${MINE_TELEOP_CAMERA_CONTRAST:-40}"
CAMERA_GAIN="${MINE_TELEOP_CAMERA_GAIN:-96}"
CAMERA_GAMMA="${MINE_TELEOP_CAMERA_GAMMA:-450}"
CAMERA_BACKLIGHT="${MINE_TELEOP_CAMERA_BACKLIGHT:-2}"
CAMERA_EXPOSURE_DYNAMIC_FRAMERATE="${MINE_TELEOP_CAMERA_EXPOSURE_DYNAMIC_FRAMERATE:-1}"
CAMERA_AUTO_EXPOSURE="${MINE_TELEOP_CAMERA_AUTO_EXPOSURE:-3}"
CAMERA_EXPOSURE_ABSOLUTE="${MINE_TELEOP_CAMERA_EXPOSURE_ABSOLUTE:-}"
CAMERA_ADAPTIVE_BRIGHTNESS="${MINE_TELEOP_CAMERA_ADAPTIVE_BRIGHTNESS:-0}"
CAMERA_ADAPTIVE_CONTRAST="${MINE_TELEOP_CAMERA_ADAPTIVE_CONTRAST:-32}"
CAMERA_ADAPTIVE_GAMMA="${MINE_TELEOP_CAMERA_ADAPTIVE_GAMMA:-300}"
CAMERA_ADAPTIVE_BACKLIGHT="${MINE_TELEOP_CAMERA_ADAPTIVE_BACKLIGHT:-1}"
CAMERA_WHITE_BALANCE_AUTO="${MINE_TELEOP_CAMERA_WHITE_BALANCE_AUTO:-1}"
CAMERA_GAIN_AUTOMATIC="${MINE_TELEOP_CAMERA_GAIN_AUTOMATIC:-1}"
CAMERA_POWER_LINE_FREQUENCY="${MINE_TELEOP_CAMERA_POWER_LINE_FREQUENCY:-1}"

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

mvs_camera_id_for_index() {
  case "$1" in
    0) printf 'hikrobot\n' ;;
    *) printf 'hikrobot%s\n' "$((1 + $1))" ;;
  esac
}

normalize_mvs_camera_pairs() {
  local raw="$1"
  local index=0
  local token
  for token in $raw; do
    if [[ "$token" == *=* ]]; then
      printf '%s\n' "$token"
    else
      printf '%s=%s\n' "$(mvs_camera_id_for_index "$index")" "$token"
    fi
    index=$((index + 1))
  done
}

is_mvs_camera_device() {
  [[ "$1" == "mvs" || "$1" == mvs:* || "$1" == hikrobot:* ]]
}

pylon_camera_id_for_index() {
  case "$1" in
    0) printf 'basler\n' ;;
    *) printf 'basler%s\n' "$((1 + $1))" ;;
  esac
}

normalize_pylon_camera_pairs() {
  local raw="$1"
  local index=0
  local token
  for token in $raw; do
    if [[ "$token" == *=* ]]; then
      printf '%s\n' "$token"
    else
      printf '%s=%s\n' "$(pylon_camera_id_for_index "$index")" "$token"
    fi
    index=$((index + 1))
  done
}

is_pylon_camera_device() {
  [[ "$1" == "pylon" || "$1" == pylon:* || "$1" == basler:* ]]
}

is_bridge_camera_device() {
  is_mvs_camera_device "$1" || is_pylon_camera_device "$1"
}

ensure_pylon_camera_bridge() {
  if [[ -x "$PYLON_BRIDGE_BIN" ]]; then
    return 0
  fi
  [[ -x "$PYLON_ROOT/bin/pylon-config" ]] || return 1
  command -v g++ >/dev/null 2>&1 || return 1
  local source="$INSTALL_DIR/scripts/pylon_camera_bridge.cpp"
  [[ -f "$source" ]] || return 1
  mkdir -p "$(dirname "$PYLON_BRIDGE_BIN")"
  local pylon_flags
  pylon_flags="$("$PYLON_ROOT/bin/pylon-config" --cflags --libs)"
  # shellcheck disable=SC2086
  g++ -std=c++17 "$source" -o "$PYLON_BRIDGE_BIN" $pylon_flags -Wl,-rpath,"$PYLON_ROOT/lib"
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

find_mvs_camera_devices() {
  if [[ -n "$MVS_CAMERAS" ]]; then
    normalize_mvs_camera_pairs "$MVS_CAMERAS"
    return
  fi
  [[ "$ENABLE_MVS_CAMERA" == "1" ]] || return 0
  [[ "$FRAME_CODEC" == "mjpeg" ]] || {
    echo "Hikrobot/MVS camera auto-detection requires MINE_TELEOP_FRAME_CODEC=mjpeg" >&2
    return 0
  }
  [[ -d "$MVS_SDK_DIR" ]] || return 0

  local list_json=""
  if [[ -x "$MINE_TELEOP_BIN" ]]; then
    list_json=$("$MINE_TELEOP_BIN" mvs-camera-bridge --sdk-root "$MVS_SDK_DIR" --list --json 2>/dev/null || true)
  fi
  if [[ -n "$list_json" ]]; then
    if python3 - "$list_json" <<'PY'
import json
import sys

raw = sys.argv[1]
start = raw.find("{")
if start < 0:
    raise SystemExit(1)
payload = json.loads(raw[start:])
for index, _device in enumerate(payload.get("devices", [])):
    camera_id = "hikrobot" if index == 0 else f"hikrobot{index + 1}"
    print(f"{camera_id}=mvs:{index}")
PY
    then
      return
    fi
  fi

  if command -v lsusb >/dev/null 2>&1 && lsusb | grep -qi "Hikrobot"; then
    printf 'hikrobot=mvs:0\n'
  fi
}

find_pylon_camera_devices() {
  if [[ -n "$PYLON_CAMERAS" ]]; then
    ensure_pylon_camera_bridge || {
      echo "Basler/pylon camera mapping is configured but pylon bridge is unavailable" >&2
      return 1
    }
    normalize_pylon_camera_pairs "$PYLON_CAMERAS"
    return
  fi
  [[ "$ENABLE_PYLON_CAMERA" == "1" ]] || return 0
  [[ "$FRAME_CODEC" == "mjpeg" ]] || {
    echo "Basler/pylon camera auto-detection requires MINE_TELEOP_FRAME_CODEC=mjpeg" >&2
    return 0
  }
  [[ -d "$PYLON_ROOT" ]] || return 0

  local list_json=""
  if ensure_pylon_camera_bridge; then
    list_json=$("$PYLON_BRIDGE_BIN" --list --json 2>/dev/null || true)
  fi
  if [[ -n "$list_json" ]]; then
    if python3 - "$list_json" <<'PY'
import json
import sys

raw = sys.argv[1]
start = raw.find("{")
if start < 0:
    raise SystemExit(1)
payload = json.loads(raw[start:])
for index, _device in enumerate(payload.get("devices", [])):
    camera_id = "basler" if index == 0 else f"basler{index + 1}"
    print(f"{camera_id}=pylon:{index}")
PY
    then
      return
    fi
  fi

  if command -v lsusb >/dev/null 2>&1 && lsusb | grep -qi "Basler"; then
    printf 'basler=pylon:0\n'
  fi
}

set_camera_control() {
  local camera_device="$1"
  local control_name="$2"
  local control_value="$3"
  v4l2-ctl -d "$camera_device" -c "${control_name}=${control_value}" >/dev/null 2>&1 || true
}

apply_adaptive_camera_controls() {
  local camera_device="$1"
  set_camera_control "$camera_device" auto_exposure "$CAMERA_AUTO_EXPOSURE"
  set_camera_control "$camera_device" exposure_dynamic_framerate "$CAMERA_EXPOSURE_DYNAMIC_FRAMERATE"
  set_camera_control "$camera_device" gain_automatic "$CAMERA_GAIN_AUTOMATIC"
  set_camera_control "$camera_device" white_balance_temperature_auto "$CAMERA_WHITE_BALANCE_AUTO"
  set_camera_control "$camera_device" power_line_frequency "$CAMERA_POWER_LINE_FREQUENCY"
  set_camera_control "$camera_device" brightness "$CAMERA_ADAPTIVE_BRIGHTNESS"
  set_camera_control "$camera_device" contrast "$CAMERA_ADAPTIVE_CONTRAST"
  set_camera_control "$camera_device" gamma "$CAMERA_ADAPTIVE_GAMMA"
  set_camera_control "$camera_device" backlight_compensation "$CAMERA_ADAPTIVE_BACKLIGHT"
}

apply_low_light_camera_controls() {
  local camera_device="$1"
  set_camera_control "$camera_device" auto_exposure "$CAMERA_AUTO_EXPOSURE"
  set_camera_control "$camera_device" brightness "$CAMERA_BRIGHTNESS"
  set_camera_control "$camera_device" contrast "$CAMERA_CONTRAST"
  set_camera_control "$camera_device" gain "$CAMERA_GAIN"
  set_camera_control "$camera_device" gamma "$CAMERA_GAMMA"
  set_camera_control "$camera_device" backlight_compensation "$CAMERA_BACKLIGHT"
  set_camera_control "$camera_device" exposure_dynamic_framerate "$CAMERA_EXPOSURE_DYNAMIC_FRAMERATE"
}

apply_manual_exposure_override() {
  local camera_device="$1"
  [[ -n "$CAMERA_EXPOSURE_ABSOLUTE" ]] || return 0
  set_camera_control "$camera_device" auto_exposure 1
  set_camera_control "$camera_device" exposure_time_absolute "$CAMERA_EXPOSURE_ABSOLUTE"
}

configure_camera_device() {
  local camera_device="$1"
  is_bridge_camera_device "$camera_device" && return 0
  if command -v v4l2-ctl >/dev/null 2>&1; then
    v4l2-ctl -d "$camera_device" \
      --set-fmt-video="width=${CAPTURE_WIDTH},height=${CAPTURE_HEIGHT},pixelformat=MJPG" \
      --set-parm="$CAPTURE_FPS" >/dev/null 2>&1 || true
    case "$CAMERA_CONTROL_PROFILE" in
      adaptive)
        apply_adaptive_camera_controls "$camera_device"
        ;;
      low_light)
        apply_low_light_camera_controls "$camera_device"
        ;;
      off|none|disabled)
        ;;
      *)
        echo "unsupported MINE_TELEOP_CAMERA_CONTROL_PROFILE: $CAMERA_CONTROL_PROFILE" >&2
        return 1
        ;;
    esac
    apply_manual_exposure_override "$camera_device"
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
  python3 - "$CONFIG_SOURCE" "$CONFIG_LIVE" "$CAPTURE_WIDTH" "$CAPTURE_HEIGHT" "$CAPTURE_FPS" "$REALTIME_PROFILE" "$@" <<'PY'
from pathlib import Path
import os
import sys

source, dest, width, height, fps, realtime_profile, *pairs = sys.argv[1:]
mvs_width = os.environ.get("MINE_TELEOP_MVS_CAPTURE_WIDTH", "1280")
mvs_height = os.environ.get("MINE_TELEOP_MVS_CAPTURE_HEIGHT", "1024")
mvs_fps = os.environ.get("MINE_TELEOP_MVS_CAPTURE_FPS", "15")
pylon_width = os.environ.get("MINE_TELEOP_PYLON_CAPTURE_WIDTH", "1280")
pylon_height = os.environ.get("MINE_TELEOP_PYLON_CAPTURE_HEIGHT", "1024")
pylon_fps = os.environ.get("MINE_TELEOP_PYLON_CAPTURE_FPS", "15")
text = Path(source).read_text(encoding="utf-8")
if "\ncameras:\n" not in text or "\nhardware:\n" not in text:
    raise SystemExit("source config must contain top-level cameras and hardware sections")
prefix, rest = text.split("\ncameras:\n", 1)
_old_cameras, suffix = rest.split("\nhardware:\n", 1)
lines = [prefix.rstrip(), "", "cameras:"]
for pair in pairs:
    camera_id, device = pair.split("=", 1)
    is_mvs = device == "mvs" or device.startswith(("mvs:", "hikrobot:"))
    is_pylon = device == "pylon" or device.startswith(("pylon:", "basler:"))
    camera_width = pylon_width if is_pylon else mvs_width if is_mvs else width
    camera_height = pylon_height if is_pylon else mvs_height if is_mvs else height
    camera_fps = pylon_fps if is_pylon else mvs_fps if is_mvs else fps
    lines.extend(
        [
            f"  - id: {camera_id}",
            "    enabled: true",
            f"    device: {device}",
            f"    capture_width: {camera_width}",
            f"    capture_height: {camera_height}",
            f"    capture_fps: {camera_fps}",
            f"    realtime_profile: {realtime_profile}",
            "    record_profile: record_source_h264",
        ]
    )
lines.extend(["", "hardware:", suffix.rstrip()])
text = "\n".join(lines)
Path(dest).write_text(text, encoding="utf-8")
PY
}

mapfile -t v4l2_camera_device_pairs < <(find_camera_devices)
mapfile -t mvs_camera_device_pairs < <(find_mvs_camera_devices)
mapfile -t pylon_camera_device_pairs < <(find_pylon_camera_devices)
camera_device_pairs=("${v4l2_camera_device_pairs[@]}" "${mvs_camera_device_pairs[@]}" "${pylon_camera_device_pairs[@]}")
if [[ "${#camera_device_pairs[@]}" -eq 0 ]]; then
  echo "no camera device found" >&2
  exit 1
fi
for pair in "${camera_device_pairs[@]}"; do
  camera_id="${pair%%=*}"
  camera_device="${pair#*=}"
  is_bridge_camera_device "$camera_device" || [[ -e "$camera_device" ]] || {
    echo "configured camera device does not exist for $camera_id: $camera_device" >&2
    exit 1
  }
  configure_camera_device "$camera_device"
done
write_live_config "${camera_device_pairs[@]}"

echo "Using cameras: ${camera_device_pairs[*]}"
echo "Frame codec: $FRAME_CODEC"
echo "Realtime profile: $REALTIME_PROFILE"
echo "Camera control profile: $CAMERA_CONTROL_PROFILE"
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
