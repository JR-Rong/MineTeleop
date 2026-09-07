#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
fixture="$script_dir/fixtures/chassis-abi-preflight/chassis_abi_preflight_fixture.c"
contract="$repo_root/abi/chassis-bridge/v6/preflight-contract.json"
preflight="$repo_root/tools/chassis_bridge_abi_preflight.py"
compiler="${CC:-cc}"
python="${PYTHON:-python3}"
temporary="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-chassis-abi-preflight.XXXXXX")"

cleanup() {
  rm -rf "$temporary"
}
trap cleanup EXIT

fail() {
  printf 'chassis_abi_preflight_test=failed reason=%s\n' "$*" >&2
  exit 1
}

command -v "$compiler" >/dev/null || fail "C compiler is unavailable: $compiler"
command -v "$python" >/dev/null || fail "Python is unavailable: $python"
[[ -f "$fixture" ]] || fail "fixture is missing: $fixture"
[[ -f "$contract" ]] || fail "contract is missing: $contract"
[[ -f "$preflight" ]] || fail "preflight tool is missing: $preflight"

case "$(uname -s)" in
  Linux) shared_flags=(-fPIC -shared) ;;
  Darwin) shared_flags=(-dynamiclib) ;;
  *) fail "unsupported fixture platform: $(uname -s)" ;;
esac

build_fixture() {
  local output="$1"
  shift
  "$compiler" -std=c11 -Wall -Wextra -Werror "${shared_flags[@]}" \
    -I"$repo_root/deployments/chassis-control-bridge" \
    "$@" "$fixture" -o "$output"
}

assert_no_can_initialization() {
  [[ ! -e "$marker" ]] || fail "preflight invoked a fixture open/CAN-init entry point"
}

marker="$temporary/can-init.marker"
valid_bridge="$temporary/libmine_teleop_chassis_bridge-valid.so"
wrong_version_bridge="$temporary/libmine_teleop_chassis_bridge-v5.so"
wrong_size_bridge="$temporary/libmine_teleop_chassis_bridge-wrong-v4-size.so"
missing_apply_v2_bridge="$temporary/libmine_teleop_chassis_bridge-missing-apply-v2.so"
missing_telemetry_bridge="$temporary/libmine_teleop_chassis_bridge-missing-telemetry.so"

build_fixture "$valid_bridge"
build_fixture "$wrong_version_bridge" -DMINE_TELEOP_ABI_FIXTURE_VERSION=5U
build_fixture "$wrong_size_bridge" -DMINE_TELEOP_ABI_FIXTURE_WRONG_V4_SIZE=1
build_fixture "$missing_apply_v2_bridge" -DMINE_TELEOP_ABI_FIXTURE_HAS_APPLY_STATE_V2=0
build_fixture "$missing_telemetry_bridge" -DMINE_TELEOP_ABI_FIXTURE_HAS_READ_TELEMETRY=0

package_root="$temporary/package"
package_bridge="$package_root/lib/vendor/chassis/libmine_teleop_chassis_bridge.so"
mkdir -p "$(dirname -- "$package_bridge")"
cp "$valid_bridge" "$package_bridge"

success_output="$temporary/package-success.json"
if ! env MINE_TELEOP_ABI_PRECHECK_MARKER="$marker" \
  "$python" "$preflight" --contract "$contract" --package-root "$package_root" \
  --profile vehicle_agent_pre_can >"$success_output"; then
  sed -n '1p' "$success_output" >&2
  fail "valid package preflight failed"
fi
grep -F '"passed":true' "$success_output" >/dev/null || fail "valid package was not accepted"
grep -F '"profile":"vehicle_agent_pre_can"' "$success_output" >/dev/null || fail "wrong profile result"
assert_no_can_initialization

wrong_version_output="$temporary/wrong-version.json"
if env MINE_TELEOP_ABI_PRECHECK_MARKER="$marker" \
  "$python" "$preflight" --contract "$contract" \
  --bridge-library "$wrong_version_bridge" --profile vehicle_agent_pre_can \
  >"$wrong_version_output"; then
  fail "ABI version 5 bridge was accepted"
fi
grep -F '"code":"abi_value_mismatch"' "$wrong_version_output" >/dev/null || \
  fail "wrong ABI version did not report abi_value_mismatch"
grep -F '"symbol":"mine_teleop_chassis_abi_version"' "$wrong_version_output" >/dev/null || \
  fail "wrong ABI version reported the wrong query"
assert_no_can_initialization

wrong_size_output="$temporary/wrong-size.json"
if env MINE_TELEOP_ABI_PRECHECK_MARKER="$marker" \
  "$python" "$preflight" --contract "$contract" \
  --bridge-library "$wrong_size_bridge" --profile vehicle_agent_pre_can \
  >"$wrong_size_output"; then
  fail "bridge with the wrong V4 POD size was accepted"
fi
grep -F '"code":"abi_value_mismatch"' "$wrong_size_output" >/dev/null || \
  fail "wrong V4 POD size did not report abi_value_mismatch"
grep -F '"symbol":"mine_teleop_chassis_open_config_v4_size"' "$wrong_size_output" >/dev/null || \
  fail "wrong V4 POD size reported the wrong query"
assert_no_can_initialization

missing_apply_v2_output="$temporary/missing-apply-v2.json"
if env MINE_TELEOP_ABI_PRECHECK_MARKER="$marker" \
  "$python" "$preflight" --contract "$contract" \
  --bridge-library "$missing_apply_v2_bridge" --profile config_check_pre_can \
  >"$missing_apply_v2_output"; then
  fail "bridge without apply_state_v2 was accepted"
fi
grep -F '"code":"missing_required_symbol"' "$missing_apply_v2_output" >/dev/null || \
  fail "missing apply_state_v2 did not report missing_required_symbol"
grep -F '"symbol":"mine_teleop_chassis_apply_state_v2"' "$missing_apply_v2_output" >/dev/null || \
  fail "missing apply_state_v2 reported the wrong symbol"
assert_no_can_initialization

config_check_output="$temporary/config-check-only.json"
env MINE_TELEOP_ABI_PRECHECK_MARKER="$marker" \
  "$python" "$preflight" --contract "$contract" \
  --bridge-library "$missing_telemetry_bridge" --profile config_check_pre_can \
  >"$config_check_output"
grep -F '"passed":true' "$config_check_output" >/dev/null || \
  fail "config-check profile unexpectedly required adapter-only telemetry"
assert_no_can_initialization

missing_telemetry_output="$temporary/missing-telemetry.json"
if env MINE_TELEOP_ABI_PRECHECK_MARKER="$marker" \
  "$python" "$preflight" --contract "$contract" \
  --bridge-library "$missing_telemetry_bridge" --profile vehicle_agent_pre_can \
  >"$missing_telemetry_output"; then
  fail "full vehicle-agent pre-CAN profile accepted missing telemetry"
fi
grep -F '"code":"missing_required_symbol"' "$missing_telemetry_output" >/dev/null || \
  fail "missing telemetry did not report missing_required_symbol"
grep -F '"symbol":"mine_teleop_chassis_read_telemetry"' "$missing_telemetry_output" >/dev/null || \
  fail "missing telemetry reported the wrong symbol"
assert_no_can_initialization

printf 'chassis_abi_preflight_test=passed\n'
