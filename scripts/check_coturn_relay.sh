#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/check_coturn_relay.sh [options]

Options:
  --credentials-file PATH  Two lines: REST username, then REST credential.
  --config PATH            Coturn template (default deployments/turnserver/turnserver.conf.template).
  --image IMAGE            Coturn image (default coturn/coturn:4.6.2).
  --log-output PATH        Preserve the raw coturn log at this path.
  --realm REALM            Test realm (default teleop.local).

Environment:
  MINE_TELEOP_DOCKER_COMMAND             Docker command prefix, for example
                                         "colima ssh -- docker".
  MINE_TELEOP_TURN_STATIC_AUTH_SECRET    Shared test secret. The default is
                                         an ephemeral, non-production value.
  MINE_TELEOP_TURN_USERNAME              Optional signaling-issued REST username.
  MINE_TELEOP_TURN_CREDENTIAL            Matching REST credential.
EOF
}

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
config_path="$repo_root/deployments/turnserver/turnserver.conf.template"
credentials_file=""
image="coturn/coturn:4.6.2"
log_output=""
realm="teleop.local"
while (($#)); do
  case "$1" in
    --credentials-file)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      credentials_file="$2"
      shift 2
      ;;
    --config)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      config_path="$2"
      shift 2
      ;;
    --image)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      image="$2"
      shift 2
      ;;
    --log-output)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      log_output="$2"
      shift 2
      ;;
    --realm)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      realm="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      printf 'unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

[[ -f "$config_path" ]] || { printf 'coturn config template does not exist: %s\n' "$config_path" >&2; exit 2; }
command -v openssl >/dev/null || { printf 'openssl is required\n' >&2; exit 2; }

docker_command="${MINE_TELEOP_DOCKER_COMMAND:-docker}"
read -r -a docker_prefix <<<"$docker_command"
((${#docker_prefix[@]} > 0)) || { printf 'MINE_TELEOP_DOCKER_COMMAND is empty\n' >&2; exit 2; }
docker_cmd() {
  "${docker_prefix[@]}" "$@"
}
docker_cmd version >/dev/null

secret="${MINE_TELEOP_TURN_STATIC_AUTH_SECRET:-mine-teleop-local-relay-check-secret}"
[[ -n "$secret" && "$secret" != *$'\n'* && "$secret" != *$'\r'* ]] || {
  printf 'TURN static auth secret must be a non-empty single line\n' >&2
  exit 2
}
[[ "$realm" =~ ^[A-Za-z0-9._-]+$ ]] || { printf 'realm contains unsupported characters\n' >&2; exit 2; }

hmac_credential() {
  printf %s "$1" | openssl dgst -binary -sha1 -hmac "$secret" | base64 | tr -d '\r\n'
}

credential_source="generated"
turn_username="${MINE_TELEOP_TURN_USERNAME:-}"
turn_credential="${MINE_TELEOP_TURN_CREDENTIAL:-}"
if [[ -n "$credentials_file" && ( -n "$turn_username" || -n "$turn_credential" ) ]]; then
  printf 'use either --credentials-file or credential environment variables, not both\n' >&2
  exit 2
elif [[ -n "$credentials_file" ]]; then
  [[ -f "$credentials_file" ]] || { printf 'credentials file does not exist: %s\n' "$credentials_file" >&2; exit 2; }
  turn_username="$(sed -n '1p' "$credentials_file")"
  turn_credential="$(sed -n '2p' "$credentials_file")"
  [[ -n "$turn_username" && -n "$turn_credential" ]] || { printf 'credentials file must contain two non-empty lines\n' >&2; exit 2; }
  credential_source="provided"
elif [[ -n "$turn_username" || -n "$turn_credential" ]]; then
  [[ -n "$turn_username" && -n "$turn_credential" ]] || {
    printf 'both MINE_TELEOP_TURN_USERNAME and MINE_TELEOP_TURN_CREDENTIAL are required\n' >&2
    exit 2
  }
  credential_source="provided"
else
  expires_at_seconds="$(($(date +%s) + 180))"
  turn_username="${expires_at_seconds}:${realm}:session-relay-check:driver-relay-check"
  turn_credential="$(hmac_credential "$turn_username")"
fi
expires_at_seconds="${turn_username%%:*}"
[[ "$expires_at_seconds" =~ ^[0-9]+$ && "$expires_at_seconds" -gt "$(date +%s)" ]] || {
  printf 'TURN REST username is already expired or malformed\n' >&2
  exit 2
}

server_options=()
realm_placeholder='${MINE_TELEOP_TURN_REALM}'
secret_placeholder='${MINE_TELEOP_TURN_STATIC_AUTH_SECRET}'
while IFS= read -r config_line || [[ -n "$config_line" ]]; do
  config_line="${config_line%$'\r'}"
  [[ -z "$config_line" || "$config_line" == \#* ]] && continue
  config_line="${config_line//$realm_placeholder/$realm}"
  config_line="${config_line//$secret_placeholder/$secret}"
  [[ "$config_line" != *'${'* ]] || { printf 'unresolved coturn template value\n' >&2; exit 2; }
  server_options+=("--$config_line")
done <"$config_path"

run_id="$$-$(date +%s)"
network="mine-teleop-turn-check-$run_id"
server="mine-teleop-turn-server-$run_id"
peer="mine-teleop-turn-peer-$run_id"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-turn-check.XXXXXX")"
server_log="$tmp_dir/coturn.log"
udp_output="$tmp_dir/udp.out"
tcp_output="$tmp_dir/tcp.out"
invalid_output="$tmp_dir/invalid.out"
anonymous_output="$tmp_dir/anonymous.out"
expired_output="$tmp_dir/expired.out"
cleanup() {
  docker_cmd rm -f "$peer" "$server" >/dev/null 2>&1 || true
  docker_cmd network rm "$network" >/dev/null 2>&1 || true
  rm -rf "$tmp_dir"
}
trap cleanup EXIT INT TERM

if docker_cmd network inspect "$network" >/dev/null 2>&1 ||
   docker_cmd container inspect "$server" >/dev/null 2>&1 ||
   docker_cmd container inspect "$peer" >/dev/null 2>&1; then
  printf 'isolated coturn test resource already exists\n' >&2
  exit 2
fi

docker_cmd network create "$network" >/dev/null
docker_cmd run -d \
  --name "$server" \
  --network "$network" \
  --entrypoint turnserver \
  "$image" \
  "${server_options[@]}" \
  --no-cli \
  --log-file=stdout \
  >/dev/null

ready=0
for _ in 1 2 3 4 5 6 7 8 9 10; do
  if [[ "$(docker_cmd inspect -f '{{.State.Running}}' "$server" 2>/dev/null || true)" == "true" ]] &&
     docker_cmd logs "$server" 2>&1 | grep -q 'Relay ports initialization done'; then
    ready=1
    break
  fi
  sleep 1
done
if [[ "$ready" -ne 1 ]]; then
  docker_cmd logs "$server" 2>&1 | tail -n 80 >&2 || true
  printf 'coturn did not become ready\n' >&2
  exit 2
fi

server_ip="$(docker_cmd inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$server")"
docker_cmd run -d \
  --name "$peer" \
  --network "$network" \
  --entrypoint turnutils_peer \
  "$image" \
  -p 3480 \
  >/dev/null
peer_ip="$(docker_cmd inspect -f '{{range .NetworkSettings.Networks}}{{.IPAddress}}{{end}}' "$peer")"

set +e
docker_cmd run --rm --network "$network" --entrypoint turnutils_uclient "$image" \
  -u "$turn_username" -w "$turn_credential" -e "$peer_ip" -r 3480 -n 20 -l 160 "$server_ip" \
  >"$udp_output" 2>&1
udp_status=$?
docker_cmd run --rm --network "$network" --entrypoint turnutils_uclient "$image" \
  -t -u "$turn_username" -w "$turn_credential" -e "$peer_ip" -r 3480 -n 20 -l 160 "$server_ip" \
  >"$tcp_output" 2>&1
tcp_status=$?
docker_cmd run --rm --network "$network" --entrypoint turnutils_uclient "$image" \
  -u "$turn_username" -w invalid-credential -e "$peer_ip" -r 3480 -n 1 "$server_ip" \
  >"$invalid_output" 2>&1
invalid_status=$?
docker_cmd run --rm --network "$network" --entrypoint turnutils_uclient "$image" \
  -e "$peer_ip" -r 3480 -n 1 "$server_ip" \
  >"$anonymous_output" 2>&1
anonymous_status=$?
expired_username="$(($(date +%s) - 60)):${realm}:session-expired:driver-expired"
expired_credential="$(hmac_credential "$expired_username")"
docker_cmd run --rm --network "$network" --entrypoint turnutils_uclient "$image" \
  -u "$expired_username" -w "$expired_credential" -e "$peer_ip" -r 3480 -n 1 "$server_ip" \
  >"$expired_output" 2>&1
expired_status=$?
set -e

sleep 1
docker_cmd logs "$server" >"$server_log" 2>&1
if [[ -n "$log_output" ]]; then
  umask 077
  mkdir -p "$(dirname -- "$log_output")"
  cp "$server_log" "$log_output"
fi

[[ "$udp_status" -eq 0 && "$tcp_status" -eq 0 ]] || { printf 'valid TURN UDP/TCP allocation failed\n' >&2; exit 2; }
grep -q 'Total lost packets 0' "$udp_output" || { printf 'TURN UDP relay reported packet loss\n' >&2; exit 2; }
grep -q 'Total lost packets 0' "$tcp_output" || { printf 'TURN-over-TCP relay reported packet loss\n' >&2; exit 2; }
[[ "$invalid_status" -ne 0 && "$anonymous_status" -ne 0 && "$expired_status" -ne 0 ]] || {
  printf 'coturn accepted invalid, anonymous, or expired credentials\n' >&2
  exit 2
}

allocation_successes="$(grep -c 'ALLOCATE processed, success' "$server_log" || true)"
usage_records="$(grep ': usage:' "$server_log" | grep -vc ': peer usage:' || true)"
relay_payload_bytes="$(awk '
  /: peer usage:/ {
    if (match($0, /rb=[0-9]+/)) total += substr($0, RSTART + 3, RLENGTH - 3)
    if (match($0, /sb=[0-9]+/)) total += substr($0, RSTART + 3, RLENGTH - 3)
  }
  END { print total + 0 }
' "$server_log")"
[[ "$allocation_successes" -ge 2 && "$usage_records" -ge 2 && "$relay_payload_bytes" -gt 0 ]] || {
  printf 'coturn log did not prove allocations and relayed payload bytes\n' >&2
  exit 2
}

printf 'coturn_relay_check=passed credential_source=%s udp_packets=20 udp_loss_percent=0 tcp_packets=20 tcp_loss_percent=0 invalid_rejected=true anonymous_rejected=true expired_rejected=true allocation_successes=%s usage_records=%s relay_payload_bytes=%s\n' \
  "$credential_source" "$allocation_successes" "$usage_records" "$relay_payload_bytes"
