#!/usr/bin/env bash
set -euo pipefail

launcher="${1:-}"
if [[ -z "$launcher" || ! -x "$launcher" ]]; then
  printf 'usage: %s /path/to/mine-teleop-run\n' "$0" >&2
  exit 2
fi
launcher="$(cd "$(dirname "$launcher")" && pwd)/$(basename "$launcher")"
temporary="$(mktemp -d)"
cleanup() {
  if [[ -n "${flood_launcher_pid:-}" ]]; then
    kill -KILL "$flood_launcher_pid" >/dev/null 2>&1 || true
    wait "$flood_launcher_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "${flood_child_pid:-}" ]]; then
    kill -KILL "$flood_child_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "${waiting_pid:-}" ]]; then
    kill -TERM "$waiting_pid" >/dev/null 2>&1 || true
    wait "$waiting_pid" >/dev/null 2>&1 || true
  fi
  if [[ -n "${tail_holder_pid:-}" ]]; then
    kill -KILL "$tail_holder_pid" >/dev/null 2>&1 || true
  fi
  rm -rf "$temporary"
}
trap cleanup EXIT
umask 027

package_root="$temporary/package"
mkdir -p "$package_root/bin" "$package_root/config"
cp "$launcher" "$package_root/bin/mine-teleop-run"
fake_runtime="$package_root/bin/mine-teleop"
{
  printf '%s\n' '#!/usr/bin/env bash'
  printf '%s\n' 'set -u'
  printf '%s\n' 'if [[ "${1:-}" != "vehicle-runtime" ]]; then'
  printf '%s\n' '  printf "direct-command=%s\\n" "${1:-none}"'
  printf '%s\n' '  exit 0'
  printf '%s\n' 'fi'
  printf '%s\n' 'printf "runtime-stdout-ready\\n"'
  printf '%s\n' 'printf "runtime-stderr-ready\\n" >&2'
  printf '%s\n' '(printf "grandchild-stderr\\n" >&2) &'
  printf '%s\n' 'wait $!'
  printf '%s\n' 'if [[ "${MINE_TELEOP_TEST_FLOOD:-0}" == "1" ]]; then'
  printf '%s\n' '  trap '\''printf "runtime-flood-term-observed\\n" > "$MINE_TELEOP_TEST_TERM_MARKER"; exit 143'\'' TERM'
  printf '%s\n' '  printf "%s\\n" "$$" > "$MINE_TELEOP_TEST_CHILD_PID_FILE"'
  printf '%s\n' '  while :; do printf "runtime-flood-abcdefghijklmnopqrstuvwxyz0123456789\\n"; done'
  printf '%s\n' 'fi'
  printf '%s\n' 'if [[ "${MINE_TELEOP_TEST_BURST:-0}" == "1" ]]; then'
  printf '%s\n' '  for index in $(seq 1 80); do'
  printf '%s\n' '    printf "burst-%03d-abcdefghijklmnopqrstuvwxyz0123456789\\n" "$index"'
  printf '%s\n' '  done'
  printf '%s\n' 'fi'
  printf '%s\n' 'if [[ "${MINE_TELEOP_TEST_VENDOR_CHATTER:-0}" == "1" ]]; then'
  printf '%s\n' '  printf "UpdateVehicle"'
  printf '%s\n' '  sleep 0.2'
  printf '%s\n' '  printf "State\\n"'
  printf '%s\n' '  printf "UpdateVehicleState diagnostic detail\\n"'
  printf '%s\n' '  printf '\''UpdateVehicleState{"event":"runtime-interleaved-after"}\n'\'''
  printf '%s\n' '  printf '\''{"event":"runtime-interleaved-before"}UpdateVehicleState\n'\'''
  printf '%s\n' '  printf "[2026-09-03 07:34:12.345] [I] [1234] [Arming] vendor-info-chatter\\n"'
  printf '%s\n' '  printf "\\033[1m\\033[31m[2026-09-03 07:34:12.346] [E] [1234] [GLOBAL] vendor-error-chatter\\n\\033[0m"'
  printf '%s\n' '  printf '\''{"event":"runtime-kept","detail":"UpdateVehicleState"}\n'\'''
  printf '%s\n' '  printf '\''{"event":"json-split'\'''
  printf '%s\n' '  sleep 0.2'
  printf '%s\n' '  printf -- '\''-line"}\n'\'''
  printf '%s\n' '  printf "unterminated-tail"'
  printf '%s\n' 'fi'
  printf '%s\n' 'if [[ "${MINE_TELEOP_TEST_TAIL_HOLDER:-0}" == "1" ]]; then'
  printf '%s\n' '  (printf "forced-drain-unterminated-tail"; sleep 30) &'
  printf '%s\n' '  printf "%s\\n" "$!" > "$MINE_TELEOP_TEST_TAIL_HOLDER_PID_FILE"'
  printf '%s\n' 'fi'
  printf '%s\n' 'if [[ "${MINE_TELEOP_TEST_WAIT:-0}" == "1" ]]; then'
  printf '%s\n' '  trap '\''printf "runtime-term-observed\\n"; exit 143'\'' TERM'
  printf '%s\n' '  while :; do sleep 0.1; done'
  printf '%s\n' 'fi'
  printf '%s\n' 'exit "${MINE_TELEOP_TEST_EXIT_CODE:-7}"'
} > "$fake_runtime"
chmod 0755 "$fake_runtime"

basic_log="$temporary/basic/vehicle-runtime.log"
set +e
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$basic_log" \
MINE_TELEOP_VEHICLE_RUNTIME_LOG_MAX_BYTES=4096 \
MINE_TELEOP_VEHICLE_RUNTIME_LOG_ROTATIONS=2 \
  "$package_root/bin/mine-teleop-run" >"$temporary/basic.stdout" 2>"$temporary/basic.stderr"
basic_status=$?
set -e
[[ "$basic_status" -eq 7 ]] || {
  printf 'runtime exit status was not preserved: %s\n' "$basic_status" >&2
  exit 1
}
grep -F 'runtime-stdout-ready' "$temporary/basic.stdout" >/dev/null
grep -F 'runtime-stderr-ready' "$temporary/basic.stderr" >/dev/null
grep -F 'vehicle_runtime_log_ready' "$basic_log" >/dev/null
grep -F 'runtime-stdout-ready' "$basic_log" >/dev/null
grep -F 'runtime-stderr-ready' "$basic_log" >/dev/null
grep -F 'grandchild-stderr' "$basic_log" >/dev/null
[[ "$(stat -c '%a' "$basic_log")" == "640" ]]
[[ "$(stat -c '%a' "$(dirname "$basic_log")")" == "750" ]]

direct_log="$temporary/direct/vehicle-runtime.log"
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$direct_log" \
  "$package_root/bin/mine-teleop-run" version >"$temporary/direct.stdout"
grep -F 'direct-command=version' "$temporary/direct.stdout" >/dev/null
[[ ! -e "$direct_log" && ! -e "$direct_log.lock" ]]

rotation_log="$temporary/rotation/vehicle-runtime.log"
set +e
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$rotation_log" \
MINE_TELEOP_VEHICLE_RUNTIME_LOG_MAX_BYTES=128 \
MINE_TELEOP_VEHICLE_RUNTIME_LOG_ROTATIONS=2 \
MINE_TELEOP_TEST_BURST=1 \
  "$package_root/bin/mine-teleop-run" >/dev/null 2>/dev/null
rotation_status=$?
set -e
[[ "$rotation_status" -eq 7 ]]
for path in "$rotation_log" "$rotation_log.1" "$rotation_log.2"; do
  [[ -f "$path" ]]
  [[ "$(stat -c '%s' "$path")" -le 128 ]]
done
[[ ! -e "$rotation_log.3" ]]

locked_log="$temporary/locked/vehicle-runtime.log"
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$locked_log" \
MINE_TELEOP_TEST_WAIT=1 \
  "$package_root/bin/mine-teleop-run" >"$temporary/locked.stdout" 2>"$temporary/locked.stderr" &
waiting_pid=$!
for _ in $(seq 1 100); do
  [[ -f "$locked_log" ]] && grep -F 'runtime-stdout-ready' "$locked_log" >/dev/null && break
  sleep 0.05
done
grep -F 'runtime-stdout-ready' "$locked_log" >/dev/null
set +e
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$locked_log" \
  "$package_root/bin/mine-teleop-run" >"$temporary/second.stdout" 2>"$temporary/second.stderr"
second_status=$?
set -e
[[ "$second_status" -eq 126 ]]
grep -F 'another vehicle runtime already owns this log' "$temporary/second.stderr" >/dev/null

kill -TERM "$waiting_pid"
set +e
wait "$waiting_pid"
waiting_status=$?
set -e
waiting_pid=""
[[ "$waiting_status" -eq 143 ]] || {
  printf 'SIGTERM exit status was not preserved: %s\n' "$waiting_status" >&2
  exit 1
}
grep -F 'runtime-term-observed' "$locked_log" >/dev/null

blocked_fifo="$temporary/blocked-terminal.fifo"
flood_child_file="$temporary/flood-child.pid"
flood_term_marker="$temporary/flood-term.marker"
flood_log="$temporary/flood/vehicle-runtime.log"
mkfifo "$blocked_fifo"
exec 9<>"$blocked_fifo"
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$flood_log" \
MINE_TELEOP_TEST_FLOOD=1 \
MINE_TELEOP_TEST_CHILD_PID_FILE="$flood_child_file" \
MINE_TELEOP_TEST_TERM_MARKER="$flood_term_marker" \
  "$package_root/bin/mine-teleop-run" >"$blocked_fifo" 2>"$temporary/flood.stderr" &
flood_launcher_pid=$!
for _ in $(seq 1 100); do
  [[ -s "$flood_child_file" ]] && break
  sleep 0.02
done
[[ -s "$flood_child_file" ]]
flood_child_pid="$(<"$flood_child_file")"
sleep 0.2
kill -TERM "$flood_launcher_pid"
for _ in $(seq 1 100); do
  if ! kill -0 "$flood_launcher_pid" >/dev/null 2>&1; then break; fi
  sleep 0.02
done
if kill -0 "$flood_launcher_pid" >/dev/null 2>&1; then
  printf 'launcher did not forward targeted SIGTERM while terminal output was blocked\n' >&2
  kill -KILL "$flood_launcher_pid" >/dev/null 2>&1 || true
  wait "$flood_launcher_pid" >/dev/null 2>&1 || true
  flood_launcher_pid=""
  exit 1
fi
set +e
wait "$flood_launcher_pid"
flood_status=$?
set -e
flood_launcher_pid=""
exec 9>&-
[[ "$flood_status" -eq 143 ]] || {
  printf 'blocked-terminal SIGTERM exit status was not preserved: %s\n' "$flood_status" >&2
  exit 1
}
grep -F 'runtime-flood-term-observed' "$flood_term_marker" >/dev/null
if kill -0 "$flood_child_pid" >/dev/null 2>&1; then
  printf 'runtime child survived targeted SIGTERM relay\n' >&2
  exit 1
fi
flood_child_pid=""

symlink_target="$temporary/symlink-target.log"
symlink_log="$temporary/symlink/vehicle-runtime.log"
mkdir -p "$(dirname "$symlink_log")"
touch "$symlink_target"
ln -s "$symlink_target" "$symlink_log"
set +e
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$symlink_log" \
  "$package_root/bin/mine-teleop-run" >"$temporary/symlink.stdout" 2>"$temporary/symlink.stderr"
symlink_status=$?
set -e
[[ "$symlink_status" -eq 126 ]]
grep -F 'runtime_log_open_failed' "$temporary/symlink.stderr" >/dev/null
[[ ! -s "$symlink_target" ]]

filter_log="$temporary/filter/vehicle-runtime.log"
set +e
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$filter_log" \
MINE_TELEOP_TEST_VENDOR_CHATTER=1 \
  "$package_root/bin/mine-teleop-run" >"$temporary/filter.stdout" 2>"$temporary/filter.stderr"
filter_status=$?
set -e
[[ "$filter_status" -eq 7 ]]
# Recognizable vendor chatter stays on the terminal but is not persisted.
grep -Fx 'UpdateVehicleState' "$temporary/filter.stdout" >/dev/null
grep -F 'vendor-info-chatter' "$temporary/filter.stdout" >/dev/null
grep -F 'vendor-error-chatter' "$temporary/filter.stdout" >/dev/null
if grep -Fx 'UpdateVehicleState' "$filter_log" >/dev/null; then
  printf 'exact UpdateVehicleState stdout chatter was persisted to the runtime log\n' >&2
  exit 1
fi
if grep -F 'vendor-info-chatter' "$filter_log" >/dev/null ||
   grep -F 'vendor-error-chatter' "$filter_log" >/dev/null; then
  printf 'formatted vendor chatter was persisted to the runtime log\n' >&2
  exit 1
fi
if LC_ALL=C grep -q $'\033' "$filter_log"; then
  printf 'vendor ANSI escape residue was persisted to the runtime log\n' >&2
  exit 1
fi
# Similar diagnostics and structured runtime output remain recorded.
grep -F 'UpdateVehicleState diagnostic detail' "$filter_log" >/dev/null
grep -F '"event":"runtime-kept"' "$filter_log" >/dev/null
grep -Fx '{"event":"runtime-interleaved-after"}' "$filter_log" >/dev/null
grep -Fx '{"event":"runtime-interleaved-before"}' "$filter_log" >/dev/null
# A line split across reads is reassembled, and an unterminated tail is
# flushed when the stream closes.
grep -F '{"event":"json-split-line"}' "$filter_log" >/dev/null
grep -F 'unterminated-tail' "$filter_log" >/dev/null

forced_tail_log="$temporary/forced-tail/vehicle-runtime.log"
forced_tail_pid_file="$temporary/forced-tail.pid"
set +e
MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$forced_tail_log" \
MINE_TELEOP_TEST_TAIL_HOLDER=1 \
MINE_TELEOP_TEST_TAIL_HOLDER_PID_FILE="$forced_tail_pid_file" \
  "$package_root/bin/mine-teleop-run" >/dev/null 2>/dev/null
forced_tail_status=$?
set -e
[[ "$forced_tail_status" -eq 7 ]]
[[ -s "$forced_tail_pid_file" ]]
tail_holder_pid="$(<"$forced_tail_pid_file")"
grep -F 'forced-drain-unterminated-tail' "$forced_tail_log" >/dev/null
kill -KILL "$tail_holder_pid" >/dev/null 2>&1 || true
tail_holder_pid=""

printf 'vehicle_runtime_log_relay_check=passed\n'
