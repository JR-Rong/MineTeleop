#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

BUNDLE="$REPO_ROOT/dist/cpp-ubuntu22.04-amd64.tar.gz"
CONFIG="${MINE_TELEOP_VEHICLE_CONFIG:-}"
SSH_USER=""
SSH_HOST=""
SSH_PORT="22"
SSH_KEY="${MINE_TELEOP_VEHICLE_SSH_KEY:-}"
REMOTE_DIR=""
REMOTE_ARCHIVE="/tmp/mine-teleop-ubuntu-x86_64.tar.gz"
SIGNALING_HTTP_URL=""
DEVICE_TOKEN="${MINE_TELEOP_VEHICLE_DEVICE_TOKEN:-}"
DEVICE_TOKEN_FILE="${MINE_TELEOP_VEHICLE_DEVICE_TOKEN_FILE:-}"
TEMP_DEVICE_TOKEN_FILE=""
MEDIA_FRAMES="1"
FRAME_INTERVAL_MS="33"
RUN_LIVE_TELEOP="false"
LIVE_TELEOP_DURATION_MS="15000"
DRY_RUN="false"
SSH_OPTIONS=()

cleanup() {
  if [[ -n "$TEMP_DEVICE_TOKEN_FILE" && -f "$TEMP_DEVICE_TOKEN_FILE" ]]; then
    rm -f "$TEMP_DEVICE_TOKEN_FILE"
  fi
}
trap cleanup EXIT

usage() {
  cat <<'EOF'
Usage:
  scripts/deploy_vehicle_bundle.sh [options]

Deploy the no-Docker-on-target Ubuntu vehicle bundle over SSH, unpack it under
the remote user's home directory, and run smoke commands from the bundled files.

Required: --host and --user (or MINE_TELEOP_VEHICLE_SSH_HOST / _USER). Prefer
key-based auth via --ssh-key or MINE_TELEOP_VEHICLE_SSH_KEY.

Options:
  --bundle PATH                Local x64 bundle archive.
  --config PATH                Optional vehicle YAML override; the bundle carries a default.
  --user USER                  SSH user (required).
  --host HOST                  SSH host (required).
  --port PORT                  SSH port. Default: 22
  --ssh-key PATH               SSH identity file for key-based auth.
  --remote-dir PATH            Remote install directory. Default: /home/<user>/mine-teleop
  --remote-archive PATH        Remote temporary archive path. Default: /tmp/mine-teleop-ubuntu-x86_64.tar.gz
  --media-frames COUNT         WebRTC frames per camera. Default: 1; set 0 to skip.
  --frame-interval-ms MS       Optional capture throttle. Default: 33
  --signaling-http-url URL     Signaling URL for control and WebRTC media.
  --run-live-teleop            Run the WebRTC media + DataChannel control agent.
  --live-teleop-duration-ms MS Live WebRTC/DataChannel duration. Default: 15000
  --device-token-file PATH     Vehicle device-token file; uploaded with mode 0600.
  --device-token TOKEN         Compatibility input; converted to a protected temporary file.
  --ssh-option OPTION          Extra -o option passed to ssh/scp. Can be repeated.
  --dry-run                    Print commands without connecting or reading the bundle.
  -h, --help                   Show this help.

Examples:
  scripts/deploy_vehicle_bundle.sh --host HOST --user USER --dry-run
  scripts/deploy_vehicle_bundle.sh --host HOST --user USER --signaling-http-url https://SIGNALING_HOST --device-token-file PATH
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

require_value() {
  local name="$1"
  local value="${2:-}"
  [[ -n "$value" ]] || die "$name requires a value"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --bundle)
      require_value "$1" "${2:-}"
      BUNDLE="$2"
      shift 2
      ;;
    --config)
      require_value "$1" "${2:-}"
      CONFIG="$2"
      shift 2
      ;;
    --user)
      require_value "$1" "${2:-}"
      SSH_USER="$2"
      shift 2
      ;;
    --host)
      require_value "$1" "${2:-}"
      SSH_HOST="$2"
      shift 2
      ;;
    --port)
      require_value "$1" "${2:-}"
      SSH_PORT="$2"
      shift 2
      ;;
    --ssh-key)
      require_value "$1" "${2:-}"
      SSH_KEY="$2"
      shift 2
      ;;
    --remote-dir)
      require_value "$1" "${2:-}"
      REMOTE_DIR="$2"
      shift 2
      ;;
    --remote-archive)
      require_value "$1" "${2:-}"
      REMOTE_ARCHIVE="$2"
      shift 2
      ;;
    --media-frames)
      require_value "$1" "${2:-}"
      MEDIA_FRAMES="$2"
      shift 2
      ;;
    --frame-interval-ms)
      require_value "$1" "${2:-}"
      FRAME_INTERVAL_MS="$2"
      shift 2
      ;;
    --signaling-http-url)
      require_value "$1" "${2:-}"
      SIGNALING_HTTP_URL="$2"
      shift 2
      ;;
    --run-live-teleop)
      RUN_LIVE_TELEOP="true"
      shift
      ;;
    --live-teleop-duration-ms)
      require_value "$1" "${2:-}"
      LIVE_TELEOP_DURATION_MS="$2"
      shift 2
      ;;
    --device-token)
      require_value "$1" "${2:-}"
      DEVICE_TOKEN="$2"
      shift 2
      ;;
    --device-token-file)
      require_value "$1" "${2:-}"
      DEVICE_TOKEN_FILE="$2"
      shift 2
      ;;
    --ssh-option)
      require_value "$1" "${2:-}"
      SSH_OPTIONS+=("-o" "$2")
      shift 2
      ;;
    --dry-run)
      DRY_RUN="true"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

[[ "$SSH_PORT" =~ ^[0-9]+$ ]] || die "--port must be an integer"
[[ "$MEDIA_FRAMES" =~ ^[0-9]+$ ]] || die "--media-frames must be an integer"
[[ "$FRAME_INTERVAL_MS" =~ ^[0-9]+$ ]] || die "--frame-interval-ms must be an integer"
[[ "$LIVE_TELEOP_DURATION_MS" =~ ^[0-9]+$ ]] || die "--live-teleop-duration-ms must be an integer"

if [[ "$DRY_RUN" != "true" ]]; then
  [[ -n "$SSH_HOST" ]] || die "--host (or MINE_TELEOP_VEHICLE_SSH_HOST) is required"
  [[ -n "$SSH_USER" ]] || die "--user (or MINE_TELEOP_VEHICLE_SSH_USER) is required"
fi
if [[ -z "$REMOTE_DIR" ]]; then
  REMOTE_DIR="/home/${SSH_USER:-user}/mine-teleop"
fi
if [[ "$RUN_LIVE_TELEOP" == "true" && -z "$SIGNALING_HTTP_URL" ]]; then
  die "--run-live-teleop requires --signaling-http-url"
fi
if [[ -n "$SIGNALING_HTTP_URL" && ( "$MEDIA_FRAMES" != "0" || "$RUN_LIVE_TELEOP" == "true" ) &&
      -z "$DEVICE_TOKEN" && -z "$DEVICE_TOKEN_FILE" ]]; then
  die "WebRTC media/DataChannel run requires --device-token-file"
fi
if [[ "$DRY_RUN" != "true" && ! -f "$BUNDLE" ]]; then
  die "bundle archive not found: $BUNDLE"
fi
if [[ "$DRY_RUN" != "true" && -n "$CONFIG" && ! -f "$CONFIG" ]]; then
  die "vehicle config not found: $CONFIG"
fi
if [[ "$DRY_RUN" != "true" && -n "$DEVICE_TOKEN_FILE" && ! -f "$DEVICE_TOKEN_FILE" ]]; then
  die "vehicle device-token file not found: $DEVICE_TOKEN_FILE"
fi
if [[ -z "$DEVICE_TOKEN_FILE" && -n "$DEVICE_TOKEN" ]]; then
  if [[ "$DRY_RUN" == "true" ]]; then
    DEVICE_TOKEN_FILE="<generated-device-token-file>"
  else
    TEMP_DEVICE_TOKEN_FILE="$(mktemp)"
    chmod 0600 "$TEMP_DEVICE_TOKEN_FILE"
    printf '%s\n' "$DEVICE_TOKEN" >"$TEMP_DEVICE_TOKEN_FILE"
    DEVICE_TOKEN_FILE="$TEMP_DEVICE_TOKEN_FILE"
  fi
fi

SSH_TARGET="$SSH_USER@$SSH_HOST"
SSH_BASE=(ssh -p "$SSH_PORT")
SCP_BASE=(scp -P "$SSH_PORT")
if [[ -n "$SSH_KEY" ]]; then
  SSH_BASE+=(-i "$SSH_KEY" -o IdentitiesOnly=yes)
  SCP_BASE+=(-i "$SSH_KEY" -o IdentitiesOnly=yes)
fi
if [[ ${#SSH_OPTIONS[@]} -gt 0 ]]; then
  SSH_BASE+=("${SSH_OPTIONS[@]}")
  SCP_BASE+=("${SSH_OPTIONS[@]}")
fi
SSH_BASE+=("$SSH_TARGET")

print_cmd() {
  printf '+'
  local arg
  for arg in "$@"; do
    printf ' %q' "$arg"
  done
  printf '\n'
}

run_cmd() {
  if [[ "$DRY_RUN" == "true" ]]; then
    print_cmd "$@"
  else
    "$@"
  fi
}

run_remote() {
  local description="$1"
  local script="$2"
  printf '==> %s\n' "$description"
  if [[ "$DRY_RUN" == "true" ]]; then
    print_cmd "${SSH_BASE[@]}" "bash -s"
    printf '%s\n' "$script"
  else
    "${SSH_BASE[@]}" "bash -s" <<<"$script"
  fi
}

printf '==> deploying %s to %s:%s\n' "$BUNDLE" "$SSH_TARGET" "$REMOTE_DIR"
run_remote "prepare remote directory" "$(cat <<EOF
set -euo pipefail
mkdir -p "$REMOTE_DIR" "$REMOTE_DIR/logs" "$REMOTE_DIR/data/recordings" "$REMOTE_DIR/data/uploader" "$REMOTE_DIR/data/uploader-archive"
EOF
)"

printf '==> uploading bundle archive\n'
run_cmd "${SCP_BASE[@]}" "$BUNDLE" "$SSH_TARGET:$REMOTE_ARCHIVE"

run_remote "unpack bundle and verify bundled executables" "$(cat <<EOF
set -euo pipefail
rm -rf "$REMOTE_DIR/.extracting"
mkdir -p "$REMOTE_DIR/.extracting"
tar -xzf "$REMOTE_ARCHIVE" -C "$REMOTE_DIR/.extracting" --strip-components=1
rm -rf "$REMOTE_DIR/bin" "$REMOTE_DIR/lib"
cp -a "$REMOTE_DIR/.extracting/." "$REMOTE_DIR/"
rm -rf "$REMOTE_DIR/.extracting"
rm -f "$REMOTE_ARCHIVE"
cd "$REMOTE_DIR"
test -x bin/mine-teleop
test -x bin/mine-teleop-run
test -x bin/vainfo
test -f config/vehicle-agent.yaml
test -x lib/ld-linux-x86-64.so.2
export GST_PLUGIN_SYSTEM_PATH_1_0=
export GST_PLUGIN_PATH_1_0="$REMOTE_DIR/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$REMOTE_DIR/bin/gst-plugin-scanner"
export GST_REGISTRY_FORK=no
export GST_REGISTRY="$REMOTE_DIR/.gstreamer-registry.bin"
export LIBVA_DRIVERS_PATH="$REMOTE_DIR/lib/dri"
export LD_LIBRARY_PATH="$REMOTE_DIR/lib"
bin/mine-teleop-run version
bin/mine-teleop-run config-check --config "$REMOTE_DIR/config/vehicle-agent.yaml"
EOF
)"

if [[ -n "$CONFIG" ]]; then
  printf '==> uploading vehicle configuration override\n'
  run_cmd "${SCP_BASE[@]}" "$CONFIG" "$SSH_TARGET:$REMOTE_DIR/config/vehicle-agent.yaml"
  run_remote "verify vehicle configuration override" "$(cat <<EOF
set -euo pipefail
cd "$REMOTE_DIR"
bin/mine-teleop-run config-check --config "$REMOTE_DIR/config/vehicle-agent.yaml"
EOF
  )"
fi

if [[ -n "$DEVICE_TOKEN_FILE" ]]; then
  printf '==> uploading protected vehicle device token\n'
  run_cmd "${SCP_BASE[@]}" "$DEVICE_TOKEN_FILE" "$SSH_TARGET:$REMOTE_DIR/config/device-token"
  run_remote "protect vehicle device token" "$(cat <<EOF
set -euo pipefail
chmod 0600 "$REMOTE_DIR/config/device-token"
test -s "$REMOTE_DIR/config/device-token"
EOF
)"
fi

if [[ "$MEDIA_FRAMES" != "0" && -n "$SIGNALING_HTTP_URL" ]]; then
  run_remote "run WebRTC hardware media smoke" "$(cat <<EOF
set -euo pipefail
cd "$REMOTE_DIR"
export GST_PLUGIN_SYSTEM_PATH_1_0=
export GST_PLUGIN_PATH_1_0="$REMOTE_DIR/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$REMOTE_DIR/bin/gst-plugin-scanner"
export GST_REGISTRY_FORK=no
export GST_REGISTRY="$REMOTE_DIR/.gstreamer-registry.bin"
export LIBVA_DRIVERS_PATH="$REMOTE_DIR/lib/dri"
export LD_LIBRARY_PATH="$REMOTE_DIR/lib"
bin/mine-teleop-run vehicle-media-agent \\
  --config "$REMOTE_DIR/config/vehicle-agent.yaml" \\
  --signaling-http-url "$SIGNALING_HTTP_URL" \\
  --frames "$MEDIA_FRAMES" \\
  --capture-interval-ms "$FRAME_INTERVAL_MS"
EOF
)"
fi

if [[ "$RUN_LIVE_TELEOP" == "true" ]]; then
  run_remote "run WebRTC media and DataChannel control" "$(cat <<EOF
set -euo pipefail
cd "$REMOTE_DIR"
export GST_PLUGIN_SYSTEM_PATH_1_0=
export GST_PLUGIN_PATH_1_0="$REMOTE_DIR/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$REMOTE_DIR/bin/gst-plugin-scanner"
export GST_REGISTRY_FORK=no
export GST_REGISTRY="$REMOTE_DIR/.gstreamer-registry.bin"
export LIBVA_DRIVERS_PATH="$REMOTE_DIR/lib/dri"
bin/mine-teleop-run vehicle-media-agent \\
  --config "$REMOTE_DIR/config/vehicle-agent.yaml" \\
  --signaling-http-url "$SIGNALING_HTTP_URL" \\
  --frames 0 \\
  --duration-ms "$LIVE_TELEOP_DURATION_MS" \\
  --capture-interval-ms "$FRAME_INTERVAL_MS"
EOF
)"
fi

printf '==> vehicle bundle deployment flow finished\n'
