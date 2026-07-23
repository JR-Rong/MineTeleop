#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/coturn_usage_report.sh --log PATH [--require-relay-bytes]
  scripts/coturn_usage_report.sh --self-test

Parse coturn "usage" and "peer usage" records into credential-free JSONL.
The report keeps session/actor ownership and byte/packet counters, but never
prints the original REST username or credential.
EOF
}

run_report() {
  local log_path="$1"
  local require_relay_bytes="$2"

  awk -v require_relay_bytes="$require_relay_bytes" '
    function number_after(line, key,    pattern, value) {
      pattern = key "=[0-9]+"
      if (!match(line, pattern)) return 0
      value = substr(line, RSTART + length(key) + 1, RLENGTH - length(key) - 1)
      return value + 0
    }
    function json_escape(value) {
      gsub(/\\/, "\\\\", value)
      gsub(/\"/, "\\\"", value)
      gsub(/\t/, "\\t", value)
      gsub(/\r/, "\\r", value)
      gsub(/\n/, "\\n", value)
      return value
    }
    function decode_username(username,    count, parts) {
      decoded_session = ""
      decoded_actor = ""
      count = split(username, parts, ":")
      if (count >= 4) {
        decoded_session = parts[count - 1]
        decoded_actor = parts[count]
      } else if (count == 2) {
        decoded_session = parts[1]
        decoded_actor = parts[2]
      } else {
        return 0
      }
      if (decoded_session !~ /^[A-Za-z0-9._-]+$/ || decoded_actor !~ /^[A-Za-z0-9._-]+$/) return 0
      return 1
    }
    {
      input_lines++
      is_peer = index($0, ": peer usage:") > 0
      is_usage = index($0, ": usage:") > 0
      if (!is_peer && !is_usage) next

      if (!match($0, /session [0-9]+:/)) {
        ignored_usage_records++
        next
      }
      coturn_session = substr($0, RSTART + 8, RLENGTH - 9)

      if (is_peer) {
        peer_rp[coturn_session] = number_after($0, "rp")
        peer_rb[coturn_session] = number_after($0, "rb")
        peer_sp[coturn_session] = number_after($0, "sp")
        peer_sb[coturn_session] = number_after($0, "sb")
        next
      }

      if (!match($0, /username=<[^>]*>/)) {
        ignored_usage_records++
        next
      }
      username = substr($0, RSTART + 10, RLENGTH - 11)
      if (!decode_username(username)) {
        ignored_usage_records++
        next
      }

      valid[coturn_session] = 1
      session_id[coturn_session] = decoded_session
      actor[coturn_session] = decoded_actor
      client_rp[coturn_session] = number_after($0, "rp")
      client_rb[coturn_session] = number_after($0, "rb")
      client_sp[coturn_session] = number_after($0, "sp")
      client_sb[coturn_session] = number_after($0, "sb")
      logical_sessions[decoded_session] = 1
    }
    END {
      usage_records = 0
      session_count = 0
      client_bytes_total = 0
      relay_payload_bytes_total = 0
      for (logical_session in logical_sessions) session_count++
      for (coturn_session in valid) {
        usage_records++
        client_bytes_total += client_rb[coturn_session] + client_sb[coturn_session]
        relay_payload_bytes_total += peer_rb[coturn_session] + peer_sb[coturn_session]
      }
      passed = usage_records > 0 && (!require_relay_bytes || relay_payload_bytes_total > 0)
      printf "{\"event\":\"coturn_usage_report\",\"passed\":%s,\"input_lines\":%d,\"usage_records\":%d,\"ignored_usage_records\":%d,\"session_count\":%d,\"turn_client_bytes_total\":%d,\"relay_payload_bytes_total\":%d}\n", passed ? "true" : "false", input_lines, usage_records, ignored_usage_records, session_count, client_bytes_total, relay_payload_bytes_total
      for (coturn_session in valid) {
        printf "{\"event\":\"coturn_usage_sample\",\"session_id\":\"%s\",\"actor\":\"%s\",\"client_packets_received\":%d,\"client_bytes_received\":%d,\"client_packets_sent\":%d,\"client_bytes_sent\":%d,\"peer_packets_received\":%d,\"peer_bytes_received\":%d,\"peer_packets_sent\":%d,\"peer_bytes_sent\":%d}\n", json_escape(session_id[coturn_session]), json_escape(actor[coturn_session]), client_rp[coturn_session], client_rb[coturn_session], client_sp[coturn_session], client_sb[coturn_session], peer_rp[coturn_session], peer_rb[coturn_session], peer_sp[coturn_session], peer_sb[coturn_session]
      }
      if (!passed) exit 2
    }
  ' "$log_path"
}

self_test() {
  local test_dir test_log output
  test_dir="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-coturn-report.XXXXXX")"
  test_log="$test_dir/coturn.log"
  trap 'rm -rf "$test_dir"' RETURN
  printf '%s\n' \
    '2026-07-21T22:51:39+0000(7): INFO: session 000000000000000001: usage: realm=<teleop.local>, username=<1784674400:teleop.local:session-000001:driver-001>, rp=14, rb=2112, sp=14, sb=1416' \
    '2026-07-21T22:51:39+0000(7): INFO: session 000000000000000001: peer usage: realm=<teleop.local>, username=<1784674400:teleop.local:session-000001:driver-001>, rp=3, rb=480, sp=3, sb=480' \
    '2026-07-21T22:51:40+0000(8): INFO: session 000000000000000002: usage: realm=<teleop.local>, username=<session-000002:vehicle-002>, rp=10, rb=1200, sp=9, sb=900' \
    '2026-07-21T22:51:40+0000(8): INFO: session 000000000000000002: peer usage: realm=<teleop.local>, username=<session-000002:vehicle-002>, rp=2, rb=320, sp=2, sb=320' \
    '2026-07-21T22:51:41+0000(8): INFO: unrelated line' \
    >"$test_log"
  output="$(run_report "$test_log" 1)"
  grep -q '"passed":true' <<<"$output"
  grep -q '"usage_records":2' <<<"$output"
  grep -q '"session_count":2' <<<"$output"
  grep -q '"turn_client_bytes_total":5628' <<<"$output"
  grep -q '"relay_payload_bytes_total":1600' <<<"$output"
  if grep -q '1784674400:teleop.local' <<<"$output"; then
    printf 'coturn usage report leaked the REST username\n' >&2
    return 2
  fi
  printf 'coturn_usage_report_self_test=passed\n'
}

log_path=""
require_relay_bytes=0
if [[ "${1:-}" == "--self-test" ]]; then
  self_test
  exit 0
fi
while (($#)); do
  case "$1" in
    --log)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      log_path="$2"
      shift 2
      ;;
    --require-relay-bytes)
      require_relay_bytes=1
      shift
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
[[ -n "$log_path" && -f "$log_path" ]] || { printf 'coturn log does not exist: %s\n' "$log_path" >&2; exit 2; }
run_report "$log_path" "$require_relay_bytes"
