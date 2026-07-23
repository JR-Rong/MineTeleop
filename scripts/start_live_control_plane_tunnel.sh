#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"

HOST="${MINE_TELEOP_VEHICLE_SSH_HOST:-}"
PORT="${MINE_TELEOP_VEHICLE_SSH_PORT:-22}"
USER_NAME="${MINE_TELEOP_VEHICLE_SSH_USER:-}"
SSH_KEY="${MINE_TELEOP_VEHICLE_SSH_KEY:-}"
STRICT_HOST_KEY="${MINE_TELEOP_SSH_STRICT_HOST_KEY:-accept-new}"
LOCAL_PORT="${MINE_TELEOP_DRIVER_CONSOLE_LOCAL_PORT:-8080}"
REMOTE_PORT="${MINE_TELEOP_DRIVER_CONSOLE_REMOTE_PORT:-18080}"
SIGNALING_LOCAL_PORT="${MINE_TELEOP_SIGNALING_LOCAL_PORT:-8765}"
SIGNALING_REMOTE_PORT="${MINE_TELEOP_SIGNALING_REMOTE_PORT:-18765}"
SERVER_NAME="${MINE_TELEOP_CONTROL_PLANE_SERVER:-mine-teleop-signaling-preview}"
CONSOLE_NAME="${MINE_TELEOP_CONTROL_PLANE_CONSOLE:-mine-teleop-driver-console-preview}"
LOG_PATH="${MINE_TELEOP_CONTROL_PLANE_LOG:-$REPO_ROOT/.local/control-plane-live.log}"
OPEN_BROWSER="${MINE_TELEOP_OPEN_BROWSER:-1}"

if [[ -z "$HOST" || -z "$USER_NAME" ]]; then
  echo "set MINE_TELEOP_VEHICLE_SSH_HOST and MINE_TELEOP_VEHICLE_SSH_USER (no hardcoded default host)" >&2
  exit 1
fi

# Prefer key-based auth. StrictHostKeyChecking defaults to accept-new; set
# MINE_TELEOP_SSH_STRICT_HOST_KEY=yes with a pre-seeded known_hosts to avoid
# trust-on-first-use (MITM) exposure on untrusted networks.
SSH_COMMON_OPTS=(-o "StrictHostKeyChecking=${STRICT_HOST_KEY}")
if [[ -n "$SSH_KEY" ]]; then
  SSH_COMMON_OPTS+=(-i "$SSH_KEY" -o IdentitiesOnly=yes)
fi

health_url="http://127.0.0.1:${LOCAL_PORT}/health"
signaling_health_url="http://127.0.0.1:${SIGNALING_LOCAL_PORT}/health"
console_url="http://127.0.0.1:${LOCAL_PORT}"
tunnel_pattern="ssh .* -R ${REMOTE_PORT}:127.0.0.1:${LOCAL_PORT} .*${HOST}"

start_control_plane() {
  if curl -fsS "$health_url" >/dev/null 2>&1 && curl -fsS "$signaling_health_url" >/dev/null 2>&1; then
    return
  fi

  mkdir -p "$(dirname "$LOG_PATH")"
  (
    cd "$REPO_ROOT"
    nohup env \
      MINE_TELEOP_CONTROL_PLANE_SERVER="$SERVER_NAME" \
      MINE_TELEOP_CONTROL_PLANE_CONSOLE="$CONSOLE_NAME" \
      MINE_TELEOP_CONTROL_PLANE_CONSOLE_PORT="$LOCAL_PORT" \
      MINE_TELEOP_CONTROL_PLANE_SIGNALING_PORT="$SIGNALING_LOCAL_PORT" \
      scripts/run_control_plane_docker.sh >"$LOG_PATH" 2>&1 &
  )

  for _ in $(seq 1 90); do
    if curl -fsS "$health_url" >/dev/null 2>&1 && curl -fsS "$signaling_health_url" >/dev/null 2>&1; then
      return
    fi
    sleep 1
  done

  echo "control plane did not become healthy; see $LOG_PATH" >&2
  exit 1
}

start_reverse_tunnel() {
  pkill -f "$tunnel_pattern" >/dev/null 2>&1 || true

  if [[ -n "${MINE_TELEOP_VEHICLE_SSH_PASSWORD:-}" ]]; then
    if ! command -v expect >/dev/null 2>&1; then
      echo "expect is required when MINE_TELEOP_VEHICLE_SSH_PASSWORD is set" >&2
      exit 1
    fi
    MINE_TELEOP_TUNNEL_HOST="$HOST" \
      MINE_TELEOP_TUNNEL_PORT="$PORT" \
      MINE_TELEOP_TUNNEL_USER="$USER_NAME" \
      MINE_TELEOP_TUNNEL_LOCAL_PORT="$LOCAL_PORT" \
      MINE_TELEOP_TUNNEL_REMOTE_PORT="$REMOTE_PORT" \
      MINE_TELEOP_TUNNEL_SIGNALING_LOCAL_PORT="$SIGNALING_LOCAL_PORT" \
      MINE_TELEOP_TUNNEL_SIGNALING_REMOTE_PORT="$SIGNALING_REMOTE_PORT" \
      MINE_TELEOP_TUNNEL_STRICT="$STRICT_HOST_KEY" \
      expect <<'EXP'
set timeout 30
set host $env(MINE_TELEOP_TUNNEL_HOST)
set port $env(MINE_TELEOP_TUNNEL_PORT)
set user $env(MINE_TELEOP_TUNNEL_USER)
set local_port $env(MINE_TELEOP_TUNNEL_LOCAL_PORT)
set remote_port $env(MINE_TELEOP_TUNNEL_REMOTE_PORT)
set signaling_local_port $env(MINE_TELEOP_TUNNEL_SIGNALING_LOCAL_PORT)
set remote_signaling_port $env(MINE_TELEOP_TUNNEL_SIGNALING_REMOTE_PORT)
set strict $env(MINE_TELEOP_TUNNEL_STRICT)
spawn ssh -p $port -fN -o ExitOnForwardFailure=yes -o ServerAliveInterval=30 -o ServerAliveCountMax=3 -o StrictHostKeyChecking=$strict -R ${remote_port}:127.0.0.1:${local_port} -R ${remote_signaling_port}:127.0.0.1:${signaling_local_port} ${user}@${host}
expect {
  -re "Are you sure.*" { send "yes\r"; exp_continue }
  -re "(?i)password:" { send "$env(MINE_TELEOP_VEHICLE_SSH_PASSWORD)\r"; exp_continue }
  eof
}
catch wait result
exit [lindex $result 3]
EXP
  else
    ssh -p "$PORT" \
      -fN \
      -o ExitOnForwardFailure=yes \
      -o ServerAliveInterval=30 \
      -o ServerAliveCountMax=3 \
      "${SSH_COMMON_OPTS[@]}" \
      -R "${REMOTE_PORT}:127.0.0.1:${LOCAL_PORT}" \
      -R "${SIGNALING_REMOTE_PORT}:127.0.0.1:${SIGNALING_LOCAL_PORT}" \
      "${USER_NAME}@${HOST}"
  fi
}

run_remote_command() {
  local command="$1"
  if [[ -n "${MINE_TELEOP_VEHICLE_SSH_PASSWORD:-}" ]]; then
    MINE_TELEOP_TUNNEL_HOST="$HOST" \
      MINE_TELEOP_TUNNEL_PORT="$PORT" \
      MINE_TELEOP_TUNNEL_USER="$USER_NAME" \
      MINE_TELEOP_REMOTE_COMMAND="$command" \
      MINE_TELEOP_TUNNEL_STRICT="$STRICT_HOST_KEY" \
      expect <<'EXP'
set timeout 30
set host $env(MINE_TELEOP_TUNNEL_HOST)
set port $env(MINE_TELEOP_TUNNEL_PORT)
set user $env(MINE_TELEOP_TUNNEL_USER)
set strict $env(MINE_TELEOP_TUNNEL_STRICT)
spawn ssh -p $port -o StrictHostKeyChecking=$strict ${user}@${host} $env(MINE_TELEOP_REMOTE_COMMAND)
expect {
  -re "Are you sure.*" { send "yes\r"; exp_continue }
  -re "(?i)password:" { send "$env(MINE_TELEOP_VEHICLE_SSH_PASSWORD)\r"; exp_continue }
  eof
}
catch wait result
exit [lindex $result 3]
EXP
  else
    ssh -p "$PORT" "${SSH_COMMON_OPTS[@]}" "${USER_NAME}@${HOST}" "$command"
  fi
}

verify_reverse_tunnel() {
  local remote_health_url="http://127.0.0.1:${REMOTE_PORT}/health"
  local remote_signaling_health_url="http://127.0.0.1:${SIGNALING_REMOTE_PORT}/health"
  for _ in $(seq 1 10); do
    if run_remote_command "curl -fsS --max-time 5 '$remote_health_url' >/dev/null && curl -fsS --max-time 5 '$remote_signaling_health_url' >/dev/null"; then
      echo "Reverse tunnel healthy: vehicle $remote_health_url -> $console_url"
      echo "Reverse tunnel healthy: vehicle $remote_signaling_health_url -> http://127.0.0.1:${SIGNALING_LOCAL_PORT}"
      return
    fi
    sleep 1
  done

  cat >&2 <<EOF
reverse tunnel did not become reachable from the vehicle.
Check that local Docker is healthy at $health_url and that SSH remote forwarding is allowed.
EOF
  exit 1
}

start_control_plane
start_reverse_tunnel
verify_reverse_tunnel

if [[ "$OPEN_BROWSER" == "1" ]] && command -v open >/dev/null 2>&1; then
  open "$console_url"
fi

cat <<EOF
DRIVER_CONSOLE_URL=$console_url
VEHICLE_SIDE_DRIVER_CONSOLE_URL=http://127.0.0.1:${REMOTE_PORT}
VEHICLE_SIDE_SIGNALING_HTTP_URL=http://127.0.0.1:${SIGNALING_REMOTE_PORT}

Run media on the vehicle:
  /home/user/mine-teleop/scripts/run_vehicle_live_media.sh

The same vehicle media process owns the unordered/unreliable WebRTC DataChannel,
validates each control command, and writes redacted receive/safety events to its media log.
EOF
