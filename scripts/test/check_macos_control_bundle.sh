#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Darwin" ]]; then
  printf 'macOS bundle verification must run on macOS\n' >&2
  exit 2
fi

archive="${1:-}"
if [[ -z "$archive" || ! -f "$archive" ]]; then
  printf 'usage: %s /path/to/mine-teleop-control-macos-ARCH-*.tar.gz\n' "$0" >&2
  exit 2
fi
archive="$(cd "$(dirname "$archive")" && pwd)/$(basename "$archive")"
checksum_file="$archive.sha256"
if [[ ! -f "$checksum_file" ]]; then
  printf 'bundle checksum is missing: %s\n' "$checksum_file" >&2
  exit 2
fi
shasum -a 256 -c "$checksum_file"

entries="$(tar -tzf "$archive")"
if printf '%s\n' "$entries" | grep -E '(^/|(^|/)\.\.(/|$))' >/dev/null; then
  printf 'bundle contains an unsafe archive path\n' >&2
  exit 2
fi
package_name="$(printf '%s\n' "$entries" | sed -n '1s#/.*##p')"
if [[ -z "$package_name" ]] || ! printf '%s\n' "$entries" | awk -F/ -v root="$package_name" '$1 != root {exit 1}'; then
  printf 'bundle must contain exactly one top-level package directory\n' >&2
  exit 2
fi

verify_root="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-macos-package-check.XXXXXX")"
control_pid=""
cleanup() {
  if [[ -n "$control_pid" ]] && kill -0 "$control_pid" 2>/dev/null; then
    kill -INT "$control_pid" 2>/dev/null || true
    wait "$control_pid" 2>/dev/null || true
  fi
  rm -rf -- "$verify_root"
}
trap cleanup EXIT

tar -xzf "$archive" -C "$verify_root"
package_root="$verify_root/$package_name"
executable="$package_root/bin/mine-teleop-control"
for required_file in \
  "$executable" \
  "$package_root/run-control.command" \
  "$package_root/config/driver-console.yaml" \
  "$package_root/certs/cacert.pem" \
  "$package_root/protocol/v1/control-command.valid.json" \
  "$package_root/BUILD-INFO.txt" \
  "$package_root/README.txt"; do
  if [[ ! -s "$required_file" ]]; then
    printf 'required bundle file is missing or empty: %s\n' "$required_file" >&2
    exit 2
  fi
done

target_arch="$(sed -n 's/^target_arch=//p' "$package_root/BUILD-INFO.txt")"
case "$target_arch" in
  arm64) expected_arch="arm64" ;;
  x64) expected_arch="x86_64" ;;
  *) printf 'BUILD-INFO.txt has an unsupported target_arch: %s\n' "$target_arch" >&2; exit 2 ;;
esac
actual_archs="$(lipo -archs "$executable")"
if [[ " $actual_archs " != *" $expected_arch "* ]]; then
  printf 'Mach-O architecture mismatch: expected %s, found %s\n' "$expected_arch" "$actual_archs" >&2
  exit 2
fi
codesign --verify --strict "$executable"
dependencies="$(otool -L "$executable")"
if printf '%s\n' "$dependencies" | grep -E '/opt/homebrew|/usr/local|libcrypto|libssl' >/dev/null; then
  printf 'bundle contains a forbidden package-manager or OpenSSL dependency\n%s\n' "$dependencies" >&2
  exit 2
fi
printf 'bundle_static_check=passed target_arch=%s actual_archs=%s\n' "$target_arch" "$actual_archs"

if ! grep -Fx 'runtime_tests_executed=yes' "$package_root/BUILD-INFO.txt" >/dev/null; then
  printf 'bundle_runtime_check=skipped reason=BUILD-INFO-marks-build-only\n'
  exit 0
fi

launch_output="$verify_root/control.out"
MINE_TELEOP_DRIVER_PASSWORD=dev-password \
  "$package_root/run-control.command" --port 0 --no-open-browser >"$launch_output" 2>&1 &
control_pid=$!
port=""
for attempt in $(seq 1 100); do
  port="$(sed -n 's/.*"port":\([0-9][0-9]*\).*/\1/p' "$launch_output" | head -n 1)"
  if [[ -n "$port" ]] && curl -fsS "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
    break
  fi
  if ! kill -0 "$control_pid" 2>/dev/null; then
    printf 'packaged control client exited before becoming healthy\n' >&2
    cat "$launch_output" >&2
    exit 2
  fi
  sleep 0.1
done
if [[ -z "$port" ]]; then
  printf 'packaged control client did not report its loopback port\n' >&2
  exit 2
fi
health="$(curl -fsS "http://127.0.0.1:$port/health")"
listeners="$(lsof -nP -a -p "$control_pid" -iTCP -sTCP:LISTEN)"
if ! printf '%s\n' "$listeners" | grep -F "127.0.0.1:$port (LISTEN)" >/dev/null; then
  printf 'packaged control client is not listening on the expected loopback address\n%s\n' "$listeners" >&2
  exit 2
fi
if printf '%s\n' "$listeners" | grep -E '(\*|0\.0\.0\.0):[0-9]+ \(LISTEN\)' >/dev/null; then
  printf 'packaged control client unexpectedly opened a wildcard listener\n%s\n' "$listeners" >&2
  exit 2
fi

conflict_output="$verify_root/conflict.out"
set +e
MINE_TELEOP_DRIVER_PASSWORD=dev-password \
  "$package_root/run-control.command" --port "$port" --no-open-browser >"$conflict_output" 2>&1
conflict_status=$?
set -e
if [[ "$conflict_status" -eq 0 ]] || ! grep -E 'Address already in use|cannot bind HTTP listener' "$conflict_output" >/dev/null; then
  printf 'occupied loopback port did not produce the expected error\n' >&2
  cat "$conflict_output" >&2
  exit 2
fi

event_response="$(curl -fsS -X POST -H 'content-type: application/json' \
  --data '{"event":"package_log_probe","details":{"password":"must-not-leak","message":"package verification"}}' \
  "http://127.0.0.1:$port/api/browser-event")"
if [[ "$event_response" != *'"recorded":true'* ]]; then
  printf 'packaged browser event was not recorded: %s\n' "$event_response" >&2
  exit 2
fi
if command -v node >/dev/null 2>&1; then
  curl -fsS "http://127.0.0.1:$port/" | \
    sed -n '/<script>/,/<\/script>/p' | sed '1d;$d' | node --check -
  printf 'bundle_javascript_syntax_check=passed\n'
else
  printf 'bundle_javascript_syntax_check=skipped reason=node-not-installed\n'
fi

kill -INT "$control_pid"
wait "$control_pid"
control_pid=""
log_path="$package_root/.local/logs/control-browser-events.jsonl"
if [[ ! -s "$log_path" ]] || ! grep -F '"event":"package_log_probe"' "$log_path" >/dev/null || \
    ! grep -F '"password":"[redacted]"' "$log_path" >/dev/null; then
  printf 'packaged browser event log is missing its redacted probe event\n' >&2
  exit 2
fi
if grep -F 'must-not-leak' "$log_path" >/dev/null; then
  printf 'packaged browser event log contains an unredacted credential\n' >&2
  exit 2
fi
if lsof -nP -iTCP:"$port" -sTCP:LISTEN 2>/dev/null | grep -F "127.0.0.1:$port" >/dev/null; then
  printf 'packaged control client did not release its loopback port\n' >&2
  exit 2
fi
printf 'bundle_runtime_check=passed health=%s port=%s log=%s\n' "$health" "$port" ".local/logs/control-browser-events.jsonl"
