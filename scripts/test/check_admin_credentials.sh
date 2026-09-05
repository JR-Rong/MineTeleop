#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"

if ! command -v python3 >/dev/null 2>&1 ||
  ! python3 -c 'import yaml' >/dev/null 2>&1; then
  printf 'admin_credentials_test=skipped reason=python3_pyyaml_unavailable\n'
  exit 0
fi

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-admin-test.XXXXXX")"
cleanup() {
  rm -rf -- "$temporary_root"
}
trap cleanup EXIT

fixture_config="$temporary_root/signaling-server.yaml"
original_config="$temporary_root/signaling-server.original.yaml"
secrets_dir="$temporary_root/secrets"
validator_ok="$temporary_root/validator-ok"
validator_fail="$temporary_root/validator-fail"
cp "$repo_root/configs/signaling-server.2x2.dev.yaml" "$fixture_config"
cp "$fixture_config" "$original_config"
mkdir -p "$secrets_dir"

printf '#!/usr/bin/env bash\nexit 0\n' >"$validator_ok"
printf '#!/usr/bin/env bash\nprintf "forced validation failure\\n" >&2\nexit 42\n' >"$validator_fail"
chmod 0755 "$validator_ok" "$validator_fail"

driver_plan="$(
  MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" \
    "$repo_root/scripts/admin/add_driver.sh" \
      --id driver-credential-dry-run \
      --config "$repo_root/configs/signaling-server.2x2.dev.yaml" \
      --vehicles vehicle-001 \
      --dry-run
)"
expected_driver_secret="$repo_root/.local/secrets/signaling-server.2x2.dev/driver-credential-dry-run.password"
[[ "$driver_plan" == *"$expected_driver_secret"* ]] || {
  printf 'add_driver repo credential path escaped .local: %s\n' "$driver_plan" >&2
  exit 2
}
[[ "$driver_plan" == *'password_file: ../.local/secrets/signaling-server.2x2.dev/driver-credential-dry-run.password'* ]] || {
  printf 'add_driver did not render the repository-relative .local reference\n' >&2
  exit 2
}

vehicle_plan="$(
  MINE_TELEOP_SIGNALING_BIN="$validator_ok" \
    "$repo_root/scripts/admin/add_vehicle.sh" \
      --id vehicle-credential-dry-run \
      --config "$repo_root/configs/signaling-server.2x2.dev.yaml" \
      --dry-run
)"
expected_vehicle_secret="$repo_root/.local/secrets/signaling-server.2x2.dev/vehicle-credential-dry-run.token"
[[ "$vehicle_plan" == *"$expected_vehicle_secret"* ]] || {
  printf 'add_vehicle repo credential path escaped .local: %s\n' "$vehicle_plan" >&2
  exit 2
}
[[ "$vehicle_plan" == *'device_token_file: ../.local/secrets/signaling-server.2x2.dev/vehicle-credential-dry-run.token'* ]] || {
  printf 'add_vehicle did not render the repository-relative .local reference\n' >&2
  exit 2
}

external_secret_dir="$repo_root/.local/secrets/external-yaml-contract"
external_vehicle_plan="$(
  MINE_TELEOP_SIGNALING_BIN="$validator_ok" \
    "$repo_root/scripts/admin/add_vehicle.sh" \
      --id vehicle-external-yaml-dry-run \
      --config "$fixture_config" \
      --secrets-dir "$external_secret_dir" \
      --dry-run
)"
expected_external_secret="$external_secret_dir/vehicle-external-yaml-dry-run.token"
[[ "$external_vehicle_plan" == *"device_token_file: $expected_external_secret"* ]] || {
  printf 'add_vehicle made a repository-relative token path for an external YAML\n' >&2
  exit 2
}
[[ "$external_vehicle_plan" != *'device_token_file: ../.local/'* ]] || {
  printf 'add_vehicle external YAML retained the invalid ../.local shortcut\n' >&2
  exit 2
}

MINE_TELEOP_SIGNALING_SERVER_BIN="$validator_ok" \
  "$repo_root/scripts/admin/add_driver.sh" \
    --id driver-credential-applied \
    --config "$fixture_config" \
    --vehicles vehicle-001 \
    --secrets-dir "$secrets_dir" \
    >/dev/null
[[ -s "$secrets_dir/driver-credential-applied.password" ]] || {
  printf 'add_driver did not create the requested password file\n' >&2
  exit 2
}
grep -q 'id: driver-credential-applied' "$fixture_config" || {
  printf 'add_driver did not publish the validated config\n' >&2
  exit 2
}

cp "$original_config" "$fixture_config"
printf 'original-token\n' >"$secrets_dir/vehicle-token-restore.token"
if MINE_TELEOP_SIGNALING_BIN="$validator_fail" \
  "$repo_root/scripts/admin/add_vehicle.sh" \
    --id vehicle-token-restore \
    --config "$fixture_config" \
    --secrets-dir "$secrets_dir" \
    --force \
    >/dev/null 2>&1; then
  printf 'add_vehicle accepted a deliberately rejected candidate config\n' >&2
  exit 2
fi
cmp -s "$fixture_config" "$original_config" || {
  printf 'add_vehicle changed the config after validation failure\n' >&2
  exit 2
}
[[ "$(tr -d '[:space:]' <"$secrets_dir/vehicle-token-restore.token")" == 'original-token' ]] || {
  printf 'add_vehicle did not restore the pre-existing token after validation failure\n' >&2
  exit 2
}
if find "$secrets_dir" -maxdepth 1 \
    \( -name '.vehicle-token-restore.token.*' -o -name '.vehicle-token-restore.token.backup.*' \) \
    -print -quit | grep -q .; then
  printf 'add_vehicle left a token staging or backup file behind\n' >&2
  exit 2
fi

printf 'admin_credentials_test=passed\n'
