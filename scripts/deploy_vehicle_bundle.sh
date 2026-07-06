#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

BUNDLE="$REPO_ROOT/dist/mine-teleop-ubuntu-x86_64.tar.gz"
SSH_USER=""
SSH_HOST=""
SSH_PORT="22"
SSH_KEY="${MINE_TELEOP_VEHICLE_SSH_KEY:-}"
REMOTE_DIR=""
REMOTE_ARCHIVE="/tmp/mine-teleop-ubuntu-x86_64.tar.gz"
DRIVER_CONSOLE_URL=""
SIGNALING_HTTP_URL=""
DEVICE_TOKEN="${MINE_TELEOP_VEHICLE_DEVICE_TOKEN:-}"
MEDIA_FRAMES="1"
FRAME_INTERVAL_MS="33"
RUN_CONTROL_TELEOP="false"
TELEOP_DURATION_MS="15000"
TELEOP_SESSION_WAIT_MS="15000"
DRY_RUN="false"
SSH_OPTIONS=()

usage() {
  cat <<'EOF'
Usage:
  scripts/deploy_vehicle_bundle.sh [options]

Deploy the no-Docker-on-target Ubuntu vehicle bundle over SSH, unpack it under
the remote user's home directory, and run smoke commands from the bundled files.

Required: --host and --user (or MINE_TELEOP_VEHICLE_SSH_HOST / _USER). Prefer
key-based auth via --ssh-key or MINE_TELEOP_VEHICLE_SSH_KEY.

Options:
  --bundle PATH                Local bundle archive. Default: dist/mine-teleop-ubuntu-x86_64.tar.gz
  --user USER                  SSH user (required).
  --host HOST                  SSH host (required).
  --port PORT                  SSH port. Default: 22
  --ssh-key PATH               SSH identity file for key-based auth.
  --remote-dir PATH            Remote install directory. Default: /home/<user>/mine-teleop
  --remote-archive PATH        Remote temporary archive path. Default: /tmp/mine-teleop-ubuntu-x86_64.tar.gz
  --driver-console-url URL     After deploy, send encoded test frames to this driver console URL.
  --media-frames COUNT         Number of media frames to send when --driver-console-url is set. Default: 1
  --frame-interval-ms MS       Delay between media frame ticks. Default: 33
  --signaling-http-url URL     Signaling HTTP URL for vehicle-agent --teleop.
  --run-control-teleop         Run vehicle-agent --teleop and print accepted control command JSONL logs.
  --teleop-duration-ms MS      Control teleop run duration. Default: 15000
  --teleop-session-wait-ms MS  How long to wait for an active session. Default: 15000
  --device-token TOKEN         Vehicle device token for signaling/upload APIs (required for smoke calls).
  --ssh-option OPTION          Extra -o option passed to ssh/scp. Can be repeated.
  --dry-run                    Print commands without connecting or reading the bundle.
  -h, --help                   Show this help.

Examples:
  scripts/deploy_vehicle_bundle.sh --host HOST --user USER --dry-run
  scripts/deploy_vehicle_bundle.sh --host HOST --user USER --ssh-key ~/.ssh/id_ed25519 --driver-console-url http://CONTROL_HOST:8080
  scripts/deploy_vehicle_bundle.sh --host HOST --user USER --signaling-http-url http://CONTROL_HOST:8765 --run-control-teleop
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
    --driver-console-url)
      require_value "$1" "${2:-}"
      DRIVER_CONSOLE_URL="$2"
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
    --run-control-teleop)
      RUN_CONTROL_TELEOP="true"
      shift
      ;;
    --teleop-duration-ms)
      require_value "$1" "${2:-}"
      TELEOP_DURATION_MS="$2"
      shift 2
      ;;
    --teleop-session-wait-ms)
      require_value "$1" "${2:-}"
      TELEOP_SESSION_WAIT_MS="$2"
      shift 2
      ;;
    --device-token)
      require_value "$1" "${2:-}"
      DEVICE_TOKEN="$2"
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
[[ "$TELEOP_DURATION_MS" =~ ^[0-9]+$ ]] || die "--teleop-duration-ms must be an integer"
[[ "$TELEOP_SESSION_WAIT_MS" =~ ^[0-9]+$ ]] || die "--teleop-session-wait-ms must be an integer"

if [[ "$DRY_RUN" != "true" ]]; then
  [[ -n "$SSH_HOST" ]] || die "--host (or MINE_TELEOP_VEHICLE_SSH_HOST) is required"
  [[ -n "$SSH_USER" ]] || die "--user (or MINE_TELEOP_VEHICLE_SSH_USER) is required"
fi
if [[ -z "$REMOTE_DIR" ]]; then
  REMOTE_DIR="/home/${SSH_USER:-user}/mine-teleop"
fi
if [[ "$RUN_CONTROL_TELEOP" == "true" && -z "$SIGNALING_HTTP_URL" ]]; then
  die "--run-control-teleop requires --signaling-http-url"
fi
if [[ "$DRY_RUN" != "true" && ! -f "$BUNDLE" ]]; then
  die "bundle archive not found: $BUNDLE"
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
rm -rf "$REMOTE_DIR/bin" "$REMOTE_DIR/lib" "$REMOTE_DIR/configs" "$REMOTE_DIR/docs" "$REMOTE_DIR/manifest"
mv "$REMOTE_DIR/.extracting/"* "$REMOTE_DIR/"
rmdir "$REMOTE_DIR/.extracting"
rm -f "$REMOTE_ARCHIVE"
cd "$REMOTE_DIR"
test -x bin/mine-teleop
test -x bin/ffmpeg
test -x bin/ffprobe
test -x bin/vainfo
test -f lib/libmine_teleop_chassis_bridge.so
test -f lib/libchassis_control.so
bin/mine-teleop --list
bin/ffmpeg -hide_banner -hwaccels
EOF
)"

if [[ -n "$DRIVER_CONSOLE_URL" ]]; then
  run_remote "send encoded camera frames to driver console" "$(cat <<EOF
set -euo pipefail
cd "$REMOTE_DIR"
bin/mine-teleop vehicle-media-agent \\
  --config "$REMOTE_DIR/configs/vehicle-agent.dev.yaml" \\
  --mode teleop \\
  --driver-console-url "$DRIVER_CONSOLE_URL" \\
  --frames "$MEDIA_FRAMES" \\
  --frame-interval-ms "$FRAME_INTERVAL_MS" \\
  --ffmpeg-binary "$REMOTE_DIR/bin/ffmpeg" \\
  --json
EOF
)"
fi

if [[ "$RUN_CONTROL_TELEOP" == "true" ]]; then
  run_remote "receive control commands and print JSONL logs" "$(cat <<EOF
set -euo pipefail
cd "$REMOTE_DIR"
bin/mine-teleop vehicle-agent \\
  --config "$REMOTE_DIR/configs/vehicle-agent.dev.yaml" \\
  --teleop \\
  --signaling-http-url "$SIGNALING_HTTP_URL" \\
  --device-token "$DEVICE_TOKEN" \\
  --teleop-duration-ms "$TELEOP_DURATION_MS" \\
  --teleop-session-wait-ms "$TELEOP_SESSION_WAIT_MS" \\
  --teleop-log-controls
EOF
)"
fi

printf '==> vehicle bundle deployment flow finished\n'
