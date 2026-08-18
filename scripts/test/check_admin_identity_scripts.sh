#!/usr/bin/env bash
set -euo pipefail
umask 077

validator="${1:-}"
[[ -n "$validator" && -x "$validator" ]] || {
  printf 'usage: %s /path/to/mine-teleop-signaling-server\n' "$0" >&2
  exit 2
}

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
add_driver="$repo_root/scripts/admin/add_driver.sh"
add_vehicle="$repo_root/scripts/admin/add_vehicle.sh"
temporary="$(mktemp -d)"
cleanup() { rm -rf -- "$temporary"; }
trap cleanup EXIT

config_dir="$temporary/etc/mine-teleop"
secrets_dir="$config_dir/secrets"
backup_dir="$temporary/backups"
mkdir -p -- "$secrets_dir"
printf '%s\n' 'driver-password-1' >"$secrets_dir/driver-console-001.password"
printf '%s\n' 'vehicle-token-1' >"$secrets_dir/vehicle-001.token"
chmod 0600 -- "$secrets_dir/driver-console-001.password" "$secrets_dir/vehicle-001.token"
config_path="$config_dir/signaling-server.yaml"
printf '%s\n' \
  'auth:' \
  '  drivers:' \
  '    - id: driver-console-001' \
  '      password_file: secrets/driver-console-001.password' \
  '      vehicles:' \
  '        - vehicle-001' \
  '  vehicles:' \
  '    - id: vehicle-001' \
  '      device_token_file: secrets/vehicle-001.token' \
  >"$config_path"

initial_config="$temporary/initial.yaml"
cp -- "$config_path" "$initial_config"
driver_output="$(MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
  --id driver-console-002 \
  --config "$config_path" \
  --vehicles vehicle-001 \
  --backup-dir "$backup_dir")"

grep -q '^    - id: driver-console-002$' "$config_path"
grep -q '^      password_file: secrets/driver-console-002.password$' "$config_path"
[[ -s "$secrets_dir/driver-console-002.password" ]]
[[ "$(stat -c '%a' -- "$secrets_dir/driver-console-002.password")" == '600' ]]
driver_backup="$(sed -n 's/^  backup[[:space:]]*//p' <<<"$driver_output")"
[[ -f "$driver_backup" ]]
cmp -- "$initial_config" "$driver_backup"
grep -q 'cloud restart not required' <<<"$driver_output"
generated_password="$(tr -d '\r\n' <"$secrets_dir/driver-console-002.password")"
[[ "$driver_output" != *"$generated_password"* ]]

vehicle_output="$(MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
  --id vehicle-002 \
  --config "$config_path" \
  --assign-to-driver driver-console-001 \
  --backup-dir "$backup_dir")"
grep -q '^    - id: vehicle-002$' "$config_path"
grep -q '^      device_token_file: secrets/vehicle-002.token$' "$config_path"
grep -A8 '^    - id: driver-console-001$' "$config_path" | grep -q 'vehicle-002'
[[ -s "$secrets_dir/vehicle-002.token" ]]
[[ "$(stat -c '%a' -- "$secrets_dir/vehicle-002.token")" == '600' ]]
vehicle_backup="$(sed -n 's/^  backup[[:space:]]*//p' <<<"$vehicle_output")"
[[ -f "$vehicle_backup" ]]
grep -q 'cloud restart not required' <<<"$vehicle_output"
generated_token="$(tr -d '[:space:]' <"$secrets_dir/vehicle-002.token")"
[[ "$vehicle_output" != *"$generated_token"* ]]

"$validator" --config "$config_path" --validate-config |
  grep -q '"driver_count":2.*"permission_count":3.*"vehicle_count":2'

# A forced replacement of an orphan token must restore the previous secret if
# validation fails. PR #10 deleted the old token in this path.
printf '%s\n' 'pre-existing-orphan-token' >"$secrets_dir/vehicle-003.token"
before_force="$(cksum "$config_path")"
if MINE_TELEOP_SIGNALING_BIN=/bin/false "$add_vehicle" \
    --id vehicle-003 \
    --config "$config_path" \
    --force \
    --backup-dir "$backup_dir" >/dev/null 2>&1; then
  printf 'add_vehicle unexpectedly accepted a config rejected by the validator\n' >&2
  exit 1
fi
[[ "$(tr -d '\r\n' <"$secrets_dir/vehicle-003.token")" == 'pre-existing-orphan-token' ]]
[[ "$before_force" == "$(cksum "$config_path")" ]]

# Hold the shared admin lock and prove both scripts wait instead of reading and
# publishing the same stale configuration snapshot.
exec 8>"${config_path}.admin.lock"
flock -x 8
MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
  --id driver-console-003 \
  --config "$config_path" \
  --vehicles vehicle-001 \
  --backup-dir "$backup_dir" >"$temporary/locked-driver.out" 2>&1 &
blocked_pid=$!
sleep 0.1
kill -0 "$blocked_pid"
flock -u 8
wait "$blocked_pid"
grep -q '^    - id: driver-console-003$' "$config_path"

# Reproduce the installed cloud layout and ensure neither script silently skips
# validation when the binary must be launched through the bundled ELF loader.
bundle_dir="$temporary/opt/mine-teleop"
mkdir -p -- "$bundle_dir/bin" "$bundle_dir/lib"
cp -- "$add_driver" "$bundle_dir/add-driver.sh"
cp -- "$add_vehicle" "$bundle_dir/add-vehicle.sh"
cp -- "$validator" "$bundle_dir/bin/mine-teleop-signaling-server"
cp -- /lib64/ld-linux-x86-64.so.2 "$bundle_dir/lib/ld-linux-x86-64.so.2"
chmod 0755 -- "$bundle_dir/add-driver.sh" "$bundle_dir/add-vehicle.sh" \
  "$bundle_dir/bin/mine-teleop-signaling-server" "$bundle_dir/lib/ld-linux-x86-64.so.2"

installed_driver_output="$(env -u MINE_TELEOP_SIGNALING_SERVER_BIN -u MINE_TELEOP_SIGNALING_BIN \
  "$bundle_dir/add-driver.sh" \
  --id driver-console-004 \
  --config "$config_path" \
  --vehicles vehicle-001 \
  --backup-dir "$backup_dir")"
grep -q 'validated by mine-teleop-signaling-server' <<<"$installed_driver_output"

installed_vehicle_output="$(env -u MINE_TELEOP_SIGNALING_SERVER_BIN -u MINE_TELEOP_SIGNALING_BIN \
  "$bundle_dir/add-vehicle.sh" \
  --id vehicle-004 \
  --config "$config_path" \
  --backup-dir "$backup_dir")"
grep -q 'validation       passed' <<<"$installed_vehicle_output"

printf 'admin_identity_scripts_check=passed\n'
