#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf 'macOS 2x2 control verification must run on macOS\n' >&2
  exit 2
fi

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
build_dir="${MINE_TELEOP_BUILD_DIR:-${1:-$repo_root/build/macos-control}}"
server_binary="$build_dir/mine-teleop-signaling-server"
control_binary="$build_dir/mine-teleop-control"
identity_config="$repo_root/configs/signaling-server.2x2.dev.yaml"
soak_seconds="${MINE_TELEOP_SOAK_SECONDS:-0}"
soak_interval_seconds="${MINE_TELEOP_SOAK_INTERVAL_SECONDS:-5}"
soak_max_rss_delta_kib="${MINE_TELEOP_SOAK_MAX_RSS_DELTA_KIB:-16384}"
soak_max_fd_delta="${MINE_TELEOP_SOAK_MAX_FD_DELTA:-8}"
keep_evidence="${MINE_TELEOP_KEEP_EVIDENCE:-0}"

for numeric_setting in soak_seconds soak_interval_seconds soak_max_rss_delta_kib soak_max_fd_delta; do
  numeric_value="${!numeric_setting}"
  if [[ ! "$numeric_value" =~ ^[0-9]+$ ]]; then
    printf '%s must be a non-negative integer\n' "$numeric_setting" >&2
    exit 2
  fi
done
if [[ "$soak_interval_seconds" == "0" ]]; then
  printf 'soak_interval_seconds must be positive\n' >&2
  exit 2
fi
if [[ "$keep_evidence" != "0" && "$keep_evidence" != "1" ]]; then
  printf 'MINE_TELEOP_KEEP_EVIDENCE must be 0 or 1\n' >&2
  exit 2
fi

for command in curl jq lsof mktemp ps trash; do
  if ! command -v "$command" >/dev/null 2>&1; then
    printf 'required command is unavailable: %s\n' "$command" >&2
    exit 2
  fi
done
for required_file in "$server_binary" "$control_binary" "$identity_config"; do
  if [[ ! -f "$required_file" ]]; then
    printf 'required 2x2 verification input is missing: %s\n' "$required_file" >&2
    printf 'usage: %s /path/to/cmake-build-directory\n' "$0" >&2
    exit 2
  fi
done

umask 077
runtime_dir="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-macos-2x2.XXXXXX")"
server_pid=""
control_one_pid=""
control_two_pid=""

stop_process() {
  local pid="${1:-}"
  if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    kill -INT "$pid" 2>/dev/null || true
    for _ in $(seq 1 50); do
      if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid" 2>/dev/null || true
        return
      fi
      sleep 0.05
    done
    kill -TERM "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  fi
}

cleanup() {
  stop_process "$control_one_pid"
  stop_process "$control_two_pid"
  stop_process "$server_pid"
  if [[ "$keep_evidence" == "0" ]]; then
    trash "$runtime_dir" 2>/dev/null || true
  fi
}
trap cleanup EXIT

fail() {
  printf 'macOS 2x2 control verification failed: %s\n' "$1" >&2
  exit 1
}

wait_for_event_port() {
  local log_path="$1"
  local event_name="$2"
  local pid="$3"
  local port=""
  for _ in $(seq 1 150); do
    port="$(jq -r --arg event "$event_name" 'select(.event == $event) | .port' "$log_path" 2>/dev/null | head -n 1 || true)"
    if [[ "$port" =~ ^[0-9]+$ ]] && curl -fsS "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
      printf '%s\n' "$port"
      return
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
      fail "$event_name process exited before its loopback listener became healthy"
    fi
    sleep 0.05
  done
  fail "$event_name did not report a healthy loopback port"
}

post_json() {
  local url="$1"
  local body="$2"
  local output_path="$3"
  curl -fsS -X POST -H 'content-type: application/json' --data "$body" "$url" >"$output_path"
}

wait_for_status() {
  local url="$1"
  local output_path="$2"
  shift 2
  for _ in $(seq 1 150); do
    if curl -fsS "$url" >"$output_path" 2>/dev/null && jq -e "$@" "$output_path" >/dev/null; then
      return
    fi
    sleep 0.05
  done
  fail "status condition did not become true for $url"
}

driver_one_password="mac-process-driver-one-$PPID-$$"
driver_two_password="mac-process-driver-two-$PPID-$$"
vehicle_one_token="mac-process-vehicle-one-$PPID-$$"
vehicle_two_token="mac-process-vehicle-two-$PPID-$$"
audit_log="$runtime_dir/signaling-audit.jsonl"
server_stdout="$runtime_dir/signaling.stdout.jsonl"
server_stderr="$runtime_dir/signaling.stderr.log"
server_args=(
  --config "$identity_config"
  --host 127.0.0.1
  --port 0
  --audit-log "$audit_log"
)
if (( soak_seconds > 0 )); then
  server_args+=(--driver-token-ttl-ms 3600000 --control-token-ttl-ms 120000)
fi

MINE_TELEOP_DRIVER_001_PASSWORD="$driver_one_password" \
MINE_TELEOP_DRIVER_002_PASSWORD="$driver_two_password" \
MINE_TELEOP_VEHICLE_001_TOKEN="$vehicle_one_token" \
MINE_TELEOP_VEHICLE_002_TOKEN="$vehicle_two_token" \
  "$server_binary" "${server_args[@]}" \
    >"$server_stdout" 2>"$server_stderr" &
server_pid=$!
server_port="$(wait_for_event_port "$server_stdout" signaling_server_started "$server_pid")"
server_origin="http://127.0.0.1:$server_port"
signaling_url="ws://127.0.0.1:$server_port/signaling"

post_json "$server_origin/vehicles/online" \
  "$(jq -nc --arg id vehicle-001 --arg token "$vehicle_one_token" \
    '{vehicle_id:$id,device_token:$token,connection_id:"mac-process-vehicle-001"}')" \
  "$runtime_dir/vehicle-one-online.json"
post_json "$server_origin/vehicles/online" \
  "$(jq -nc --arg id vehicle-002 --arg token "$vehicle_two_token" \
    '{vehicle_id:$id,device_token:$token,connection_id:"mac-process-vehicle-002"}')" \
  "$runtime_dir/vehicle-two-online.json"
vehicle_one_generation="$(jq -er '.connection_generation' "$runtime_dir/vehicle-one-online.json")"
vehicle_two_generation="$(jq -er '.connection_generation' "$runtime_dir/vehicle-two-online.json")"

control_one_stdout="$runtime_dir/control-one.stdout.jsonl"
control_two_stdout="$runtime_dir/control-two.stdout.jsonl"
MINE_TELEOP_DRIVER_PASSWORD="$driver_one_password" \
  "$control_binary" \
    --config "$repo_root/configs/driver-console.dev.yaml" \
    --signaling-url "$signaling_url" \
    --port 0 \
    --no-open-browser \
    >"$control_one_stdout" 2>"$runtime_dir/control-one.stderr.log" &
control_one_pid=$!
MINE_TELEOP_DRIVER_PASSWORD="$driver_two_password" \
  "$control_binary" \
    --config "$repo_root/configs/driver-console-002.dev.yaml" \
    --signaling-url "$signaling_url" \
    --port 0 \
    --no-open-browser \
    >"$control_two_stdout" 2>"$runtime_dir/control-two.stderr.log" &
control_two_pid=$!
control_one_port="$(wait_for_event_port "$control_one_stdout" control_client_started "$control_one_pid")"
control_two_port="$(wait_for_event_port "$control_two_stdout" control_client_started "$control_two_pid")"
control_one_origin="http://127.0.0.1:$control_one_port"
control_two_origin="http://127.0.0.1:$control_two_port"

post_json "$control_one_origin/api/login" \
  "$(jq -nc --arg password "$driver_one_password" '{password:$password}')" \
  "$runtime_dir/driver-one-login.json"
post_json "$control_two_origin/api/login" \
  "$(jq -nc --arg password "$driver_two_password" '{password:$password}')" \
  "$runtime_dir/driver-two-login.json"
post_json "$control_one_origin/api/connect" '{"vehicle_id":"vehicle-001"}' "$runtime_dir/session-one.json"
post_json "$control_two_origin/api/connect" '{"vehicle_id":"vehicle-002"}' "$runtime_dir/session-two.json"

session_one="$(jq -er '.session_id' "$runtime_dir/session-one.json")"
session_two="$(jq -er '.session_id' "$runtime_dir/session-two.json")"
[[ -n "$session_one" && -n "$session_two" && "$session_one" != "$session_two" ]] || \
  fail 'simultaneous sessions were not independent'

wait_for_status "$control_one_origin/api/status" "$runtime_dir/status-one.json" \
  --arg session "$session_one" \
  '.connected and .signaling_websocket_connected and .vehicle_id == "vehicle-001" and .session_id == $session'
wait_for_status "$control_two_origin/api/status" "$runtime_dir/status-two.json" \
  --arg session "$session_two" \
  '.connected and .signaling_websocket_connected and .vehicle_id == "vehicle-002" and .session_id == $session'
wait_for_status "$server_origin/health" "$runtime_dir/health-active.json" \
  '.online_vehicles == 2 and .online_drivers == 2 and .active_sessions == 2'

post_json "$control_one_origin/api/control" \
  '{"gear":"N","steering":-0.25,"throttle":0.1,"brake":0.0}' \
  "$runtime_dir/control-one.json"
post_json "$control_two_origin/api/control" \
  '{"gear":"N","steering":0.25,"throttle":0.2,"brake":0.0}' \
  "$runtime_dir/control-two.json"
jq -e --arg session "$session_one" \
  '.command.vehicle_id == "vehicle-001" and .command.driver_id == "driver-console-001" and .command.session_id == $session and .command.seq == 1' \
  "$runtime_dir/control-one.json" >/dev/null || fail 'driver one control command crossed identities'
jq -e --arg session "$session_two" \
  '.command.vehicle_id == "vehicle-002" and .command.driver_id == "driver-console-002" and .command.session_id == $session and .command.seq == 1' \
  "$runtime_dir/control-two.json" >/dev/null || fail 'driver two control command crossed identities'
control_token_one="$(jq -er '.command.control_token' "$runtime_dir/control-one.json")"
control_token_two="$(jq -er '.command.control_token' "$runtime_dir/control-two.json")"
[[ "$control_token_one" != "$control_token_two" ]] || fail 'independent sessions shared a control token'

rejection_one_status="$(curl -sS -o "$runtime_dir/rejection-one.json" -w '%{http_code}' \
  -X POST -H 'content-type: application/json' --data '{"vehicle_id":"vehicle-002"}' \
  "$control_one_origin/api/connect")"
rejection_two_status="$(curl -sS -o "$runtime_dir/rejection-two.json" -w '%{http_code}' \
  -X POST -H 'content-type: application/json' --data '{"vehicle_id":"vehicle-001"}' \
  "$control_two_origin/api/connect")"
[[ "$rejection_one_status" == "400" && "$rejection_two_status" == "400" ]] || \
  fail 'cross-vehicle requests were not rejected with HTTP 400'

wait_for_status "$control_one_origin/api/status" "$runtime_dir/status-one-retained.json" \
  --arg session "$session_one" \
  '.connected and .signaling_websocket_connected and .vehicle_id == "vehicle-001" and .session_id == $session'
wait_for_status "$control_two_origin/api/status" "$runtime_dir/status-two-retained.json" \
  --arg session "$session_two" \
  '.connected and .signaling_websocket_connected and .vehicle_id == "vehicle-002" and .session_id == $session'

soak_summary=""
if (( soak_seconds > 0 )); then
  samples_path="$runtime_dir/soak-samples.csv"
  printf 'elapsed_s,server_rss_kib,control1_rss_kib,control2_rss_kib,total_rss_kib,server_fds,control1_fds,control2_fds,active_sessions,online_drivers,online_vehicles\n' \
    >"$samples_path"
  soak_started_at="$(date +%s)"
  next_progress=0
  while true; do
    elapsed=$(( $(date +%s) - soak_started_at ))
    if (( elapsed >= soak_seconds )); then
      break
    fi
    for pid in "$server_pid" "$control_one_pid" "$control_two_pid"; do
      if ! kill -0 "$pid" 2>/dev/null; then
        fail "soak process exited after ${elapsed}s: $pid"
      fi
    done
    post_json "$server_origin/vehicles/heartbeat" \
      "$(jq -nc --arg id vehicle-001 --arg token "$vehicle_one_token" --argjson generation "$vehicle_one_generation" \
        '{vehicle_id:$id,device_token:$token,connection_generation:$generation}')" \
      "$runtime_dir/vehicle-one-heartbeat.json"
    post_json "$server_origin/vehicles/heartbeat" \
      "$(jq -nc --arg id vehicle-002 --arg token "$vehicle_two_token" --argjson generation "$vehicle_two_generation" \
        '{vehicle_id:$id,device_token:$token,connection_generation:$generation}')" \
      "$runtime_dir/vehicle-two-heartbeat.json"
    curl -fsS "$control_one_origin/api/vehicles" >"$runtime_dir/vehicles-one.json"
    curl -fsS "$control_two_origin/api/vehicles" >"$runtime_dir/vehicles-two.json"
    curl -fsS "$control_one_origin/api/status" >"$runtime_dir/status-one-soak.json"
    curl -fsS "$control_two_origin/api/status" >"$runtime_dir/status-two-soak.json"
    curl -fsS "$server_origin/health" >"$runtime_dir/health-soak.json"
    active_sessions="$(jq -er '.active_sessions' "$runtime_dir/health-soak.json")"
    online_drivers="$(jq -er '.online_drivers' "$runtime_dir/health-soak.json")"
    online_vehicles="$(jq -er '.online_vehicles' "$runtime_dir/health-soak.json")"
    if [[ "$active_sessions" != "2" || "$online_drivers" != "2" || "$online_vehicles" != "2" ]]; then
      fail "soak health changed after ${elapsed}s: $(cat "$runtime_dir/health-soak.json")"
    fi
    server_rss="$(ps -o rss= -p "$server_pid" | awk '{print $1 + 0}')"
    control_one_rss="$(ps -o rss= -p "$control_one_pid" | awk '{print $1 + 0}')"
    control_two_rss="$(ps -o rss= -p "$control_two_pid" | awk '{print $1 + 0}')"
    total_rss=$((server_rss + control_one_rss + control_two_rss))
    server_fds="$(lsof -p "$server_pid" 2>/dev/null | wc -l | tr -d ' ')"
    control_one_fds="$(lsof -p "$control_one_pid" 2>/dev/null | wc -l | tr -d ' ')"
    control_two_fds="$(lsof -p "$control_two_pid" 2>/dev/null | wc -l | tr -d ' ')"
    printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n' \
      "$elapsed" "$server_rss" "$control_one_rss" "$control_two_rss" "$total_rss" \
      "$server_fds" "$control_one_fds" "$control_two_fds" \
      "$active_sessions" "$online_drivers" "$online_vehicles" >>"$samples_path"
    if (( elapsed >= next_progress )); then
      printf 'macos_2x2_soak_progress elapsed_s=%s active_sessions=%s rss_kib=%s fds=%s/%s/%s\n' \
        "$elapsed" "$active_sessions" "$total_rss" "$server_fds" "$control_one_fds" "$control_two_fds"
      next_progress=$((next_progress + 60))
    fi
    sleep "$soak_interval_seconds"
  done

  early_start=0
  early_end=$((soak_seconds / 2))
  late_start=$((soak_seconds / 2))
  if (( soak_seconds >= 600 )); then
    early_start=60
    early_end=300
    late_start=$((soak_seconds - 300))
  fi
  soak_summary="$(awk -F, -v early_start="$early_start" -v early_end="$early_end" -v late_start="$late_start" '
    NR == 2 {first_rss=$5; first_fds=$6+$7+$8}
    NR > 1 && $1 >= early_start && $1 < early_end {early_rss+=$5; early_count++}
    NR > 1 && $1 >= late_start {late_rss+=$5; late_count++}
    NR > 1 {last_rss=$5; last_fds=$6+$7+$8; if ($5>max_rss) max_rss=$5}
    END {
      early=early_count ? early_rss/early_count : first_rss
      late=late_count ? late_rss/late_count : last_rss
      printf "first_rss_kib=%d early_avg_rss_kib=%d late_avg_rss_kib=%d final_rss_kib=%d max_rss_kib=%d rss_delta_kib=%d first_fds=%d final_fds=%d fd_delta=%d", first_rss, early, late, last_rss, max_rss, late-early, first_fds, last_fds, last_fds-first_fds
    }' "$samples_path")"
  rss_delta="$(printf '%s' "$soak_summary" | sed -n 's/.*rss_delta_kib=\(-*[0-9][0-9]*\).*/\1/p')"
  fd_delta="$(printf '%s' "$soak_summary" | sed -n 's/.*fd_delta=\(-*[0-9][0-9]*\).*/\1/p')"
  if (( rss_delta > soak_max_rss_delta_kib || fd_delta > soak_max_fd_delta )); then
    fail "soak resource growth exceeded limits: $soak_summary"
  fi
fi

post_json "$control_one_origin/api/disconnect" '{"reason":"mac_process_2x2_complete"}' "$runtime_dir/disconnect-one.json"
post_json "$control_two_origin/api/disconnect" '{"reason":"mac_process_2x2_complete"}' "$runtime_dir/disconnect-two.json"
wait_for_status "$server_origin/health" "$runtime_dir/health-released.json" \
  '.online_vehicles == 2 and .online_drivers == 0 and .active_sessions == 0'

stop_process "$control_one_pid"
control_one_pid=""
stop_process "$control_two_pid"
control_two_pid=""
stop_process "$server_pid"
server_pid=""

for expected in "$session_one" "$session_two" driver-console-001 driver-console-002 vehicle-001 vehicle-002; do
  grep -F "$expected" "$audit_log" >/dev/null || fail "audit omitted expected correlation value: $expected"
done
for secret in \
  "$driver_one_password" "$driver_two_password" "$vehicle_one_token" "$vehicle_two_token" \
  "$control_token_one" "$control_token_two"; do
  if grep -F "$secret" "$audit_log" >/dev/null; then
    fail 'signaling audit leaked a credential or control token'
  fi
done
jq -s -e '[.[] | .service_instance_id | select(type == "string" and length > 0)] | length > 0 and (unique | length == 1)' "$audit_log" >/dev/null || \
  fail 'audit records did not share one service instance ID'

printf 'macos_2x2_process_check=passed active_sessions=2 released_sessions=0 sessions=%s,%s\n' \
  "$session_one" "$session_two"
if (( soak_seconds > 0 )); then
  evidence_location="trashed-after-check"
  if [[ "$keep_evidence" == "1" ]]; then
    evidence_location="$runtime_dir"
  fi
  printf 'macos_2x2_soak=passed duration_s=%s %s evidence=%s\n' \
    "$soak_seconds" "$soak_summary" "$evidence_location"
fi
