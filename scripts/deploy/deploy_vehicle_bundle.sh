#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)"

BUNDLE="$REPO_ROOT/dist/cpp-ubuntu22.04-amd64.tar.gz"
CONFIG="${MINE_TELEOP_VEHICLE_CONFIG:-}"
SSH_USER=""
SSH_HOST=""
SSH_PORT="22"
SSH_KEY="${MINE_TELEOP_VEHICLE_SSH_KEY:-}"
REMOTE_DIR=""
REMOTE_ARCHIVE="/tmp/mine-teleop-ubuntu-x86_64.tar.gz"
REMOTE_CONFIG_OVERRIDE=""
REMOTE_DEVICE_TOKEN_OVERRIDE=""
CONFIG_OVERRIDE_REQUESTED="false"
DEVICE_TOKEN_OVERRIDE_REQUESTED="false"
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
  scripts/deploy/deploy_vehicle_bundle.sh [options]

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
  scripts/deploy/deploy_vehicle_bundle.sh --host HOST --user USER --dry-run
  scripts/deploy/deploy_vehicle_bundle.sh --host HOST --user USER --signaling-http-url https://SIGNALING_HOST --device-token-file PATH
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
REMOTE_CONFIG_OVERRIDE="$REMOTE_ARCHIVE.vehicle-agent.yaml"
REMOTE_DEVICE_TOKEN_OVERRIDE="$REMOTE_ARCHIVE.device-token"
if [[ -n "$CONFIG" ]]; then
  CONFIG_OVERRIDE_REQUESTED="true"
fi
if [[ -n "$DEVICE_TOKEN_FILE" ]]; then
  DEVICE_TOKEN_OVERRIDE_REQUESTED="true"
fi
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
mkdir -p "$REMOTE_DIR" "$REMOTE_DIR/.releases" "$REMOTE_DIR/config" "$REMOTE_DIR/logs" "$REMOTE_DIR/data/recordings" "$REMOTE_DIR/data/uploader" "$REMOTE_DIR/data/uploader-archive"
EOF
)"

printf '==> uploading bundle archive\n'
run_cmd "${SCP_BASE[@]}" "$BUNDLE" "$SSH_TARGET:$REMOTE_ARCHIVE"

if [[ -n "$CONFIG" ]]; then
  printf '==> uploading vehicle configuration override for preflight\n'
  run_cmd "${SCP_BASE[@]}" "$CONFIG" "$SSH_TARGET:$REMOTE_CONFIG_OVERRIDE"
fi
if [[ -n "$DEVICE_TOKEN_FILE" ]]; then
  printf '==> uploading protected vehicle device token candidate\n'
  run_cmd "${SCP_BASE[@]}" "$DEVICE_TOKEN_FILE" "$SSH_TARGET:$REMOTE_DEVICE_TOKEN_OVERRIDE"
fi

run_remote "preflight candidate config and atomically activate release" "$(cat <<EOF
set -euo pipefail
release_id="\$(date -u +%Y%m%d-%H%M%S)-\$\$"
release_root="$REMOTE_DIR/.releases"
staging_release="\$release_root/.staging-\$release_id"
release_dir="\$release_root/\$release_id"
current_link="$REMOTE_DIR/current"
stable_bin="$REMOTE_DIR/bin"
stable_lib="$REMOTE_DIR/lib"
final_config="$REMOTE_DIR/config/vehicle-agent.yaml"
pending_config="$REMOTE_DIR/config/.vehicle-agent.yaml.next-\$release_id"
final_token="$REMOTE_DIR/config/device-token"
pending_token="$REMOTE_DIR/config/.device-token.next-\$release_id"
config_backup=""
token_backup=""
stable_bin_backup=""
stable_lib_backup=""
config_published="false"
token_published="false"
stable_bin_activation_started="false"
stable_lib_activation_started="false"
managed_ca_published=()
previous_current=""
activation_started="false"
committed="false"

rollback() {
  set +e
  if [[ "\$stable_lib_activation_started" == "true" ]]; then
    rm -rf "\$stable_lib"
    if [[ -n "\$stable_lib_backup" && ( -e "\$stable_lib_backup" || -L "\$stable_lib_backup" ) ]]; then
      mv "\$stable_lib_backup" "\$stable_lib"
    fi
  fi
  if [[ "\$stable_bin_activation_started" == "true" ]]; then
    rm -rf "\$stable_bin"
    if [[ -n "\$stable_bin_backup" && ( -e "\$stable_bin_backup" || -L "\$stable_bin_backup" ) ]]; then
      mv "\$stable_bin_backup" "\$stable_bin"
    fi
  fi
  if [[ "\$activation_started" == "true" ]]; then
    if [[ -n "\$previous_current" ]]; then
      ln -s "\$previous_current" "$REMOTE_DIR/.current.rollback-\$release_id"
      mv -Tf "$REMOTE_DIR/.current.rollback-\$release_id" "\$current_link"
    else
      rm -f "\$current_link"
    fi
  fi
  if [[ -n "\$config_backup" && -f "\$config_backup" ]]; then
    mv -f "\$config_backup" "\$final_config"
  elif [[ "\$config_published" == "true" ]]; then
    rm -f "\$final_config"
  fi
  if [[ -n "\$token_backup" && -f "\$token_backup" ]]; then
    mv -f "\$token_backup" "\$final_token"
  elif [[ "\$token_published" == "true" ]]; then
    rm -f "\$final_token"
  fi
  for managed_ca in "\${managed_ca_published[@]:-}"; do
    [[ -z "\$managed_ca" ]] || rm -f "\$managed_ca"
  done
  rm -rf "\$staging_release"
  rm -rf "\$release_dir"
  rm -f \
    "\$pending_config" \
    "\$pending_token" \
    "$REMOTE_DIR/.bin.next-\$release_id" \
    "$REMOTE_DIR/.lib.next-\$release_id" \
    "$REMOTE_DIR/config/".*.next-"\$release_id" \
    "$REMOTE_DIR/.current.next-\$release_id" \
    "$REMOTE_DIR/.current.rollback-\$release_id" \
    "$REMOTE_ARCHIVE" \
    "$REMOTE_CONFIG_OVERRIDE" \
    "$REMOTE_DEVICE_TOKEN_OVERRIDE"
}

finish() {
  status=\$?
  trap - EXIT
  if [[ "\$status" -ne 0 && "\$committed" != "true" ]]; then
    rollback
  fi
  exit "\$status"
}
trap finish EXIT

rm -rf "\$staging_release"
mkdir -p "\$staging_release"
tar -xzf "$REMOTE_ARCHIVE" -C "\$staging_release" --strip-components=1
test -x "\$staging_release/bin/mine-teleop"
test -x "\$staging_release/bin/mine-teleop-run"
test -s "\$staging_release/lib/vendor/chassis/libmine_teleop_chassis_bridge.so"

candidate_config="\$staging_release/config/vehicle-agent.yaml"
if [[ "$CONFIG_OVERRIDE_REQUESTED" == "true" && -s "$REMOTE_CONFIG_OVERRIDE" ]]; then
  candidate_config="$REMOTE_CONFIG_OVERRIDE"
elif [[ -s "\$final_config" ]]; then
  candidate_config="\$final_config"
fi
test -s "\$candidate_config"

export LD_LIBRARY_PATH="\$staging_release/lib:\$staging_release/lib/vendor/chassis:\$staging_release/lib/vendor/mvs"
"\$staging_release/bin/mine-teleop-run" version
"\$staging_release/bin/mine-teleop-run" config-check \
  --config "\$candidate_config" \
  --chassis-bridge-library "\$staging_release/lib/vendor/chassis/libmine_teleop_chassis_bridge.so" \
  | tee /dev/stderr \
  | grep -Eq '"chassis_bridge_abi":\\{[^}]*"passed":true[^}]*"version":6[^}]*\\}'

cp -p "\$candidate_config" "\$pending_config"
if [[ -f "\$final_config" ]]; then
  config_backup="$REMOTE_DIR/config/.vehicle-agent.yaml.backup-\$release_id"
  cp -p "\$final_config" "\$config_backup"
fi
if [[ "$DEVICE_TOKEN_OVERRIDE_REQUESTED" == "true" ]]; then
  test -s "$REMOTE_DEVICE_TOKEN_OVERRIDE"
  install -m 0600 "$REMOTE_DEVICE_TOKEN_OVERRIDE" "\$pending_token"
  if [[ -f "\$final_token" ]]; then
    token_backup="$REMOTE_DIR/config/.device-token.backup-\$release_id"
    cp -p "\$final_token" "\$token_backup"
  fi
fi
mv "\$staging_release" "\$release_dir"
if [[ -L "\$current_link" ]]; then
  previous_current="\$(readlink "\$current_link")"
elif [[ -e "\$current_link" ]]; then
  echo "current activation path exists and is not a symlink: \$current_link" >&2
  false
fi
activation_started="true"
ln -s "\$release_dir" "$REMOTE_DIR/.current.next-\$release_id"
mv -Tf "$REMOTE_DIR/.current.next-\$release_id" "\$current_link"

ln -s current/bin "$REMOTE_DIR/.bin.next-\$release_id"
if [[ -L "\$stable_bin" && "\$(readlink "\$stable_bin")" == "current/bin" ]]; then
  rm -f "$REMOTE_DIR/.bin.next-\$release_id"
else
  stable_bin_activation_started="true"
  if [[ -e "\$stable_bin" || -L "\$stable_bin" ]]; then
    stable_bin_backup="$REMOTE_DIR/.bin.backup-\$release_id"
    mv "\$stable_bin" "\$stable_bin_backup"
  fi
  mv -Tf "$REMOTE_DIR/.bin.next-\$release_id" "\$stable_bin"
fi

ln -s current/lib "$REMOTE_DIR/.lib.next-\$release_id"
if [[ -L "\$stable_lib" && "\$(readlink "\$stable_lib")" == "current/lib" ]]; then
  rm -f "$REMOTE_DIR/.lib.next-\$release_id"
else
  stable_lib_activation_started="true"
  if [[ -e "\$stable_lib" || -L "\$stable_lib" ]]; then
    stable_lib_backup="$REMOTE_DIR/.lib.backup-\$release_id"
    mv "\$stable_lib" "\$stable_lib_backup"
  fi
  mv -Tf "$REMOTE_DIR/.lib.next-\$release_id" "\$stable_lib"
fi

for bundled_ca in "\$release_dir"/config/*.crt "\$release_dir"/config/*.pem; do
  [[ -f "\$bundled_ca" ]] || continue
  ca_name="\$(basename -- "\$bundled_ca")"
  final_ca="$REMOTE_DIR/config/\$ca_name"
  if [[ ! -e "\$final_ca" && ! -L "\$final_ca" ]]; then
    pending_ca="$REMOTE_DIR/config/.\$ca_name.next-\$release_id"
    install -m 0644 "\$bundled_ca" "\$pending_ca"
    managed_ca_published+=("\$final_ca")
    mv -f "\$pending_ca" "\$final_ca"
  fi
done
if [[ "$DEVICE_TOKEN_OVERRIDE_REQUESTED" == "true" ]]; then
  token_published="true"
  mv -f "\$pending_token" "\$final_token"
fi
config_published="true"
mv -f "\$pending_config" "\$final_config"

cd "$REMOTE_DIR"
test -x current/bin/mine-teleop
test -x current/bin/mine-teleop-run
test -x current/bin/vainfo
test -f config/vehicle-agent.yaml
test -x current/lib/ld-linux-x86-64.so.2
export GST_PLUGIN_SYSTEM_PATH_1_0=
export GST_PLUGIN_PATH_1_0="$REMOTE_DIR/current/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$REMOTE_DIR/current/bin/gst-plugin-scanner"
export GST_REGISTRY_FORK=no
export GST_REGISTRY="$REMOTE_DIR/.gstreamer-registry.bin"
export LIBVA_DRIVERS_PATH="$REMOTE_DIR/current/lib/dri"
export LD_LIBRARY_PATH="$REMOTE_DIR/current/lib:$REMOTE_DIR/current/lib/vendor/chassis:$REMOTE_DIR/current/lib/vendor/mvs"
current/bin/mine-teleop-run version
current/bin/mine-teleop-run config-check \
  --config "$REMOTE_DIR/config/vehicle-agent.yaml" \
  --verify-configured-ca-bundle \
  --chassis-bridge-library "$REMOTE_DIR/current/lib/vendor/chassis/libmine_teleop_chassis_bridge.so" \
  | tee /dev/stderr \
  | grep -Eq '"chassis_bridge_abi":\\{[^}]*"passed":true[^}]*"version":6[^}]*\\}'
committed="true"
rm -f \
  "\$config_backup" \
  "\$token_backup" \
  "$REMOTE_ARCHIVE" \
  "$REMOTE_CONFIG_OVERRIDE" \
  "$REMOTE_DEVICE_TOKEN_OVERRIDE" || true
[[ -z "\$stable_bin_backup" ]] || rm -rf "\$stable_bin_backup"
[[ -z "\$stable_lib_backup" ]] || rm -rf "\$stable_lib_backup"
printf 'active_release=%s\n' "\$release_dir"
EOF
)"

if [[ "$MEDIA_FRAMES" != "0" && -n "$SIGNALING_HTTP_URL" ]]; then
  run_remote "run WebRTC hardware media smoke" "$(cat <<EOF
set -euo pipefail
cd "$REMOTE_DIR"
export GST_PLUGIN_SYSTEM_PATH_1_0=
export GST_PLUGIN_PATH_1_0="$REMOTE_DIR/current/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$REMOTE_DIR/current/bin/gst-plugin-scanner"
export GST_REGISTRY_FORK=no
export GST_REGISTRY="$REMOTE_DIR/.gstreamer-registry.bin"
export LIBVA_DRIVERS_PATH="$REMOTE_DIR/current/lib/dri"
export LD_LIBRARY_PATH="$REMOTE_DIR/current/lib"
current/bin/mine-teleop-run vehicle-media-agent \\
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
export GST_PLUGIN_PATH_1_0="$REMOTE_DIR/current/lib/gstreamer-1.0"
export GST_PLUGIN_SCANNER="$REMOTE_DIR/current/bin/gst-plugin-scanner"
export GST_REGISTRY_FORK=no
export GST_REGISTRY="$REMOTE_DIR/.gstreamer-registry.bin"
export LIBVA_DRIVERS_PATH="$REMOTE_DIR/current/lib/dri"
current/bin/mine-teleop-run vehicle-media-agent \\
  --config "$REMOTE_DIR/config/vehicle-agent.yaml" \\
  --signaling-http-url "$SIGNALING_HTTP_URL" \\
  --frames 0 \\
  --duration-ms "$LIVE_TELEOP_DURATION_MS" \\
  --capture-interval-ms "$FRAME_INTERVAL_MS"
EOF
)"
fi

printf '==> vehicle bundle deployment flow finished\n'
