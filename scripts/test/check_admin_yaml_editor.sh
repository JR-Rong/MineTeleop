#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
driver_script="$repo_root/scripts/admin/add_driver.sh"
vehicle_script="$repo_root/scripts/admin/add_vehicle.sh"

required=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --require)
      required=1
      shift
      ;;
    --help | -h)
      printf 'Usage: %s [--require]\n' "$(basename -- "$0")"
      exit 0
      ;;
    *)
      printf 'admin_yaml_editor_test=failed reason=unknown_argument argument=%s\n' "$1" >&2
      exit 2
      ;;
  esac
done

finish_prerequisite_unavailable() {
  local reason="$1"
  if [[ $required -eq 1 ]]; then
    printf 'admin_yaml_editor_test=failed reason=%s\n' "$reason"
    exit 2
  fi
  printf 'admin_yaml_editor_test=skipped reason=%s\n' "$reason"
  exit 0
}

command -v python3 >/dev/null 2>&1 ||
  finish_prerequisite_unavailable "python3_unavailable"
command -v argon2 >/dev/null 2>&1 ||
  finish_prerequisite_unavailable "argon2_cli_unavailable"
python3 -c 'import yaml' >/dev/null 2>&1 ||
  finish_prerequisite_unavailable "pyyaml_unavailable"

temporary_root="$(mktemp -d /tmp/mine-teleop-admin-yaml-test.XXXXXX)"
cleanup() {
  rm -rf -- "$temporary_root"
}
trap cleanup EXIT

fixture_config="$temporary_root/signaling-server.yaml"
original_config="$temporary_root/signaling-server.original.yaml"
secrets_dir="$temporary_root/secrets"
validator_ok="$temporary_root/validator-ok"
fallback_bin="$temporary_root/fallback-bin"
no_backend_bin="$temporary_root/no-backend-bin"

printf '#!/usr/bin/env bash\nexit 0\n' >"$validator_ok"
chmod 0755 "$validator_ok"
mkdir -p "$fallback_bin" "$no_backend_bin"
printf '#!/usr/bin/env bash\nprintf "yq version 3.4.1\\n"\n' >"$fallback_bin/yq"
printf '#!/usr/bin/env bash\nprintf "yq version 3.4.1\\n"\n' >"$no_backend_bin/yq"
printf '#!/usr/bin/env bash\nexit 1\n' >"$no_backend_bin/python3"
chmod 0755 "$fallback_bin/yq" "$no_backend_bin/yq" "$no_backend_bin/python3"

write_quoted_fixture() {
  printf '%s\n' \
    '# Unicode metadata must survive the fallback editor.' \
    'metadata:' \
    '  title: "巡检配置"' \
    'auth:' \
    '  drivers:' \
    '    - id: "driver-existing"' \
    '      password_file: /tmp/driver-existing.password' \
    '      vehicles: []' \
    '  vehicles:' \
    '    - id: "vehicle-quoted"' \
    '      device_token_file: /tmp/vehicle-quoted.token' \
    >"$fixture_config"
}

write_empty_vehicle_fixture() {
  printf '%s\n' \
    '# 空列表 and the Unicode metadata must remain intact.' \
    'metadata:' \
    '  title: "巡检配置"' \
    'auth:' \
    '  drivers:' \
    '    - id: "driver-empty"' \
    '      password_file: /tmp/driver-empty.password' \
    '      vehicles: []' \
    '  vehicles: []' \
    >"$fixture_config"
}

write_empty_driver_fixture() {
  printf '%s\n' \
    'metadata:' \
    '  title: "巡检配置"' \
    'auth:' \
    '  drivers: []' \
    '  vehicles:' \
    '    - id: "vehicle-bootstrap"' \
    '      device_token_file: /tmp/vehicle-bootstrap.token' \
    >"$fixture_config"
}

run_fallback() {
  PATH="$fallback_bin:$PATH" NO_COLOR=1 "$@"
}

expect_failure() {
  local label="$1"
  shift
  if "$@" >"$temporary_root/$label.stdout" 2>"$temporary_root/$label.stderr"; then
    printf '%s unexpectedly succeeded\n' "$label" >&2
    exit 2
  fi
}

file_mode() {
  stat -c '%a' -- "$1" 2>/dev/null || stat -f '%Lp' -- "$1"
}

if command -v yq >/dev/null 2>&1 && yq --version 2>/dev/null | grep -qi 'mikefarah\|version v4'; then
  write_empty_vehicle_fixture
  NO_COLOR=1 env MINE_TELEOP_SIGNALING_BIN="$validator_ok" "$vehicle_script" \
    --id vehicle-yq-applied --config "$fixture_config" --secrets-dir "$secrets_dir" \
    --assign-to-driver driver-empty >/dev/null
  grep -Fq 'id: vehicle-yq-applied' "$fixture_config" || {
    printf 'yq v4 did not add the vehicle\n' >&2
    exit 2
  }
  grep -Fqx '        - vehicle-yq-applied' "$fixture_config" || {
    printf 'yq v4 did not assign the vehicle to the driver\n' >&2
    exit 2
  }
fi

write_quoted_fixture
cp "$fixture_config" "$original_config"
expect_failure duplicate_driver \
  run_fallback env MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" "$driver_script" \
    --id driver-existing --config "$fixture_config" --vehicles vehicle-quoted \
    --secrets-dir "$secrets_dir"
cmp -s "$fixture_config" "$original_config" || {
  printf 'duplicate driver changed the config\n' >&2
  exit 2
}
[[ ! -e "$secrets_dir/driver-existing.password" ]] || {
  printf 'duplicate driver created a credential\n' >&2
  exit 2
}

expect_failure missing_driver_vehicle \
  run_fallback env MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" "$driver_script" \
    --id driver-missing-vehicle --config "$fixture_config" --vehicles vehicle-absent \
    --secrets-dir "$secrets_dir"
cmp -s "$fixture_config" "$original_config" || {
  printf 'missing vehicle changed the config\n' >&2
  exit 2
}

expect_failure duplicate_vehicle \
  run_fallback env MINE_TELEOP_SIGNALING_BIN="$validator_ok" "$vehicle_script" \
    --id vehicle-quoted --config "$fixture_config" --secrets-dir "$secrets_dir"
cmp -s "$fixture_config" "$original_config" || {
  printf 'duplicate vehicle changed the config\n' >&2
  exit 2
}

expect_failure missing_assignment_driver \
  run_fallback env MINE_TELEOP_SIGNALING_BIN="$validator_ok" "$vehicle_script" \
    --id vehicle-missing-driver --config "$fixture_config" --secrets-dir "$secrets_dir" \
    --assign-to-driver driver-absent
cmp -s "$fixture_config" "$original_config" || {
  printf 'missing assignment driver changed the config\n' >&2
  exit 2
}

fallback_plan="$(
  run_fallback env MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" "$driver_script" \
    --id driver-fallback-plan --config "$fixture_config" --vehicles vehicle-quoted \
    --secrets-dir "$secrets_dir" --dry-run 2>&1
)"
[[ "$fallback_plan" == *'using the python3 fallback editor'* ]] || {
  printf 'old yq did not select the python fallback\n' >&2
  exit 2
}
[[ "$fallback_plan" == *'driver-fallback-plan'* ]] || {
  printf 'fallback dry-run did not render a driver entry\n' >&2
  exit 2
}

run_fallback env MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" "$driver_script" \
  --id driver-fallback-applied --config "$fixture_config" --vehicles vehicle-quoted \
  --secrets-dir "$secrets_dir" >/dev/null
grep -Fq 'id: driver-fallback-applied' "$fixture_config" || {
  printf 'fallback did not add the driver\n' >&2
  exit 2
}
grep -Fq 'title: "巡检配置"' "$fixture_config" || {
  printf 'fallback did not preserve Unicode YAML\n' >&2
  exit 2
}
[[ "$(file_mode "$secrets_dir")" == 700 ]] || {
  printf 'fallback created secrets directory with an unexpected mode\n' >&2
  exit 2
}
[[ "$(file_mode "$secrets_dir/driver-fallback-applied.password")" == 600 ]] || {
  printf 'fallback created password with an unexpected mode\n' >&2
  exit 2
}
[[ "$(file_mode "$secrets_dir/driver-fallback-applied.password.argon2id")" == 600 ]] || {
  printf 'fallback created verifier with an unexpected mode\n' >&2
  exit 2
}
grep -Fq 'password_hash_file:' "$fixture_config" || {
  printf 'fallback did not add an Argon2id verifier reference\n' >&2
  exit 2
}

write_empty_vehicle_fixture
run_fallback env MINE_TELEOP_SIGNALING_BIN="$validator_ok" "$vehicle_script" \
  --id vehicle-from-empty --config "$fixture_config" --secrets-dir "$secrets_dir" \
  --assign-to-driver driver-empty --assign-to-driver driver-empty >/dev/null
grep -Fqx '  vehicles:' "$fixture_config" || {
  printf 'empty auth.vehicles was not expanded to a block sequence\n' >&2
  exit 2
}
grep -Fqx '      vehicles: [vehicle-from-empty]' "$fixture_config" || {
  printf 'empty driver permission list did not receive exactly one assignment\n' >&2
  exit 2
}

write_empty_driver_fixture
run_fallback env MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" "$driver_script" \
  --id driver-from-empty --config "$fixture_config" --vehicles vehicle-bootstrap \
  --secrets-dir "$secrets_dir" >/dev/null
grep -Fqx '  drivers:' "$fixture_config" || {
  printf 'empty auth.drivers was not expanded to a block sequence\n' >&2
  exit 2
}
grep -Fq 'id: driver-from-empty' "$fixture_config" || {
  printf 'empty auth.drivers did not receive a driver\n' >&2
  exit 2
}

packaged_admin="$temporary_root/packaged-admin"
mkdir -p "$packaged_admin"
cp -a "$repo_root/scripts/admin/." "$packaged_admin/"
write_empty_driver_fixture
run_fallback env MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" "$packaged_admin/add_driver.sh" \
  --id driver-packaged-helper --config "$fixture_config" --vehicles vehicle-bootstrap \
  --secrets-dir "$secrets_dir" >/dev/null
grep -Fq 'id: driver-packaged-helper' "$fixture_config" || {
  printf 'copied admin entrypoint could not locate the shared helper\n' >&2
  exit 2
}

write_quoted_fixture
cp "$fixture_config" "$original_config"
if PATH="$no_backend_bin:$PATH" NO_COLOR=1 "$driver_script" \
  --id driver-no-backend --config "$fixture_config" --vehicles vehicle-quoted \
  --secrets-dir "$secrets_dir" >"$temporary_root/no-backend.stdout" \
  2>"$temporary_root/no-backend.stderr"; then
  printf 'missing YAML backend unexpectedly succeeded\n' >&2
  exit 2
fi
grep -Fq 'no YAML backend available' "$temporary_root/no-backend.stderr" || {
  printf 'missing YAML backend did not explain the required installation\n' >&2
  exit 2
}
cmp -s "$fixture_config" "$original_config" || {
  printf 'missing YAML backend changed the config\n' >&2
  exit 2
}

printf 'admin_yaml_editor_test=passed\n'
