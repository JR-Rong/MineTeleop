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

fail_test() {
  printf '%s\n' "$*" >&2
  exit 1
}

assert_driver_identity() {
  local yaml_path="$1" driver_id="$2" password_file="$3" vehicle_id="$4"
  python3 - "$yaml_path" "$driver_id" "$password_file" "$vehicle_id" <<'PYTHON'
import sys

import yaml

yaml_path, driver_id, password_file, vehicle_id = sys.argv[1:]
with open(yaml_path, "r", encoding="utf-8") as handle:
    document = yaml.safe_load(handle)
drivers = document["auth"]["drivers"]
matches = [entry for entry in drivers
           if isinstance(entry, dict) and entry.get("id") == driver_id]
assert len(matches) == 1, (driver_id, drivers)
entry = matches[0]
assert entry.get("password_file") == password_file, entry
assert vehicle_id in entry.get("vehicles", []), entry
PYTHON
}

assert_vehicle_identity() {
  local yaml_path="$1" vehicle_id="$2" token_file="$3"
  python3 - "$yaml_path" "$vehicle_id" "$token_file" <<'PYTHON'
import sys

import yaml

yaml_path, vehicle_id, token_file = sys.argv[1:]
with open(yaml_path, "r", encoding="utf-8") as handle:
    document = yaml.safe_load(handle)
vehicles = document["auth"]["vehicles"]
matches = [entry for entry in vehicles
           if isinstance(entry, dict) and entry.get("id") == vehicle_id]
assert len(matches) == 1, (vehicle_id, vehicles)
assert matches[0].get("device_token_file") == token_file, matches[0]
PYTHON
}

assert_driver_permission() {
  local yaml_path="$1" driver_id="$2" vehicle_id="$3"
  python3 - "$yaml_path" "$driver_id" "$vehicle_id" <<'PYTHON'
import sys

import yaml

yaml_path, driver_id, vehicle_id = sys.argv[1:]
with open(yaml_path, "r", encoding="utf-8") as handle:
    document = yaml.safe_load(handle)
matches = [entry for entry in document["auth"]["drivers"]
           if isinstance(entry, dict) and entry.get("id") == driver_id]
assert len(matches) == 1, (driver_id, document["auth"]["drivers"])
assert vehicle_id in matches[0].get("vehicles", []), matches[0]
PYTHON
}

write_seed_fixture() {
  local fixture_dir="$1" fixture_config="$2"
  local fixture_secrets="$fixture_dir/secrets"
  mkdir -p -- "$fixture_secrets"
  printf '%s\n' 'driver-password-1' >"$fixture_secrets/driver-console-001.password"
  printf '%s\n' 'vehicle-token-1' >"$fixture_secrets/vehicle-001.token"
  chmod 0600 -- "$fixture_secrets/driver-console-001.password" \
    "$fixture_secrets/vehicle-001.token"
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
    >"$fixture_config"
}

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

assert_driver_identity "$config_path" driver-console-002 \
  secrets/driver-console-002.password vehicle-001
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
assert_vehicle_identity "$config_path" vehicle-002 secrets/vehicle-002.token
assert_driver_permission "$config_path" driver-console-001 vehicle-002
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
assert_driver_identity "$config_path" driver-console-003 \
  secrets/driver-console-003.password vehicle-001

# Reproduce the installed cloud layout and ensure neither script silently skips
# validation when the binary must be launched through the bundled ELF loader.
bundle_dir="$temporary/opt/mine-teleop"
mkdir -p -- "$bundle_dir/bin" "$bundle_dir/lib"
cp -- "$add_driver" "$bundle_dir/add-driver.sh"
cp -- "$add_vehicle" "$bundle_dir/add-vehicle.sh"
cp -- "$validator" "$bundle_dir/bin/mine-teleop-signaling-server"
system_loader=""
for loader_candidate in \
    /lib64/ld-linux-x86-64.so.2 \
    /lib/ld-linux-aarch64.so.1; do
  if [[ -x "$loader_candidate" ]]; then
    system_loader="$loader_candidate"
    break
  fi
done
[[ -n "$system_loader" ]] || fail_test 'cannot find the Linux ELF loader for the cloud bundle test'
cp -- "$system_loader" "$bundle_dir/lib/ld-linux-x86-64.so.2"
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

# A non-zero validator result remains fatal even when one diagnostic mentions
# a missing environment variable. PR #10 treated that substring as blanket
# permission to publish a candidate containing additional validation errors.
mixed_failure_validator="$temporary/mixed-failure-validator"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "%s\n" "environment variable is unset or empty: LEGACY_PASSWORD" >&2' \
  'printf "%s\n" "duplicate vehicle permission: invalid candidate" >&2' \
  'exit 9' \
  >"$mixed_failure_validator"
chmod 0755 -- "$mixed_failure_validator"

rejected_snapshot="$temporary/rejected-validator-before.yaml"
cp -- "$config_path" "$rejected_snapshot"
if MINE_TELEOP_SIGNALING_SERVER_BIN="$mixed_failure_validator" "$add_driver" \
    --id driver-validator-rejected \
    --config "$config_path" \
    --vehicles vehicle-001 \
    --backup-dir "$backup_dir" >"$temporary/rejected-driver.out" 2>&1; then
  fail_test 'add_driver accepted a non-zero validator result containing an environment diagnostic'
fi
cmp -- "$rejected_snapshot" "$config_path"
[[ ! -e "$secrets_dir/driver-validator-rejected.password" ]]
[[ ! -L "$secrets_dir/driver-validator-rejected.password" ]]

if MINE_TELEOP_SIGNALING_BIN="$mixed_failure_validator" "$add_vehicle" \
    --id vehicle-validator-rejected \
    --config "$config_path" \
    --backup-dir "$backup_dir" >"$temporary/rejected-vehicle.out" 2>&1; then
  fail_test 'add_vehicle accepted a non-zero validator result containing an environment diagnostic'
fi
cmp -- "$rejected_snapshot" "$config_path"
[[ ! -e "$secrets_dir/vehicle-validator-rejected.token" ]]
[[ ! -L "$secrets_dir/vehicle-validator-rejected.token" ]]

# A dangling credential symlink is not visible to `test -e`. The driver tool
# must reject it before openssl can follow the link and write outside secrets/.
outside_password="$temporary/outside-driver-password"
dangling_password="$secrets_dir/driver-symlink.password"
ln -s -- "$outside_password" "$dangling_password"
symlink_snapshot="$temporary/dangling-symlink-before.yaml"
cp -- "$config_path" "$symlink_snapshot"
if MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
    --id driver-symlink \
    --config "$config_path" \
    --vehicles vehicle-001 \
    --backup-dir "$backup_dir" >"$temporary/dangling-symlink.out" 2>&1; then
  fail_test 'add_driver followed a dangling credential symlink'
fi
cmp -- "$symlink_snapshot" "$config_path"
[[ -L "$dangling_password" ]]
[[ ! -e "$outside_password" ]]

# --force is not permission to replace a symlink. Refuse it without changing
# either the config, the link, or the file outside the secrets directory.
outside_token="$temporary/outside-vehicle-token"
printf '%s\n' 'outside-token-must-not-change' >"$outside_token"
token_symlink="$secrets_dir/vehicle-token-symlink.token"
ln -s -- "$outside_token" "$token_symlink"
token_symlink_snapshot="$temporary/token-symlink-before.yaml"
cp -- "$config_path" "$token_symlink_snapshot"
if MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
    --id vehicle-token-symlink \
    --config "$config_path" \
    --force \
    --backup-dir "$backup_dir" >"$temporary/token-symlink.out" 2>&1; then
  fail_test 'add_vehicle --force accepted a token symlink'
fi
cmp -- "$token_symlink_snapshot" "$config_path"
[[ -L "$token_symlink" ]]
[[ "$(tr -d '\r\n' <"$outside_token")" == 'outside-token-must-not-change' ]]

# Reject a symlink supplied as --config for both tools. Replacing the link with
# a regular file would split the apparent config path from its original target.
config_link_dir="$temporary/config-link"
config_link_target="$config_link_dir/signaling-server.target.yaml"
config_link_path="$config_link_dir/signaling-server.yaml"
mkdir -p -- "$config_link_dir"
write_seed_fixture "$config_link_dir" "$config_link_target"
ln -s -- "$(basename -- "$config_link_target")" "$config_link_path"
config_link_snapshot="$temporary/config-link-before.yaml"
cp -- "$config_link_target" "$config_link_snapshot"
if MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
    --id driver-config-symlink \
    --config "$config_link_path" \
    --vehicles vehicle-001 \
    --backup-dir "$config_link_dir/backups" >"$temporary/config-link-driver.out" 2>&1; then
  fail_test 'add_driver accepted a symlink as --config'
fi
cmp -- "$config_link_snapshot" "$config_link_target"
[[ -L "$config_link_path" ]]
[[ ! -e "$config_link_dir/secrets/driver-config-symlink.password" ]]

if MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
    --id vehicle-config-symlink \
    --config "$config_link_path" \
    --backup-dir "$config_link_dir/backups" >"$temporary/config-link-vehicle.out" 2>&1; then
  fail_test 'add_vehicle accepted a symlink as --config'
fi
cmp -- "$config_link_snapshot" "$config_link_target"
[[ -L "$config_link_path" ]]
[[ ! -e "$config_link_dir/secrets/vehicle-config-symlink.token" ]]

# Credential and backup directories are also trust boundaries. A symlink in
# either position must not redirect generated secrets or backups elsewhere.
outside_secrets="$temporary/outside-secrets"
linked_secrets="$temporary/linked-secrets"
mkdir -p -- "$outside_secrets"
ln -s -- "$outside_secrets" "$linked_secrets"
directory_link_snapshot="$temporary/directory-link-before.yaml"
cp -- "$config_path" "$directory_link_snapshot"
if MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
    --id driver-linked-secrets \
    --config "$config_path" \
    --vehicles vehicle-001 \
    --secrets-dir "$linked_secrets" \
    --backup-dir "$backup_dir" >"$temporary/linked-secrets.out" 2>&1; then
  fail_test 'add_driver accepted a symlink as --secrets-dir'
fi
cmp -- "$directory_link_snapshot" "$config_path"
[[ -L "$linked_secrets" ]]
[[ ! -e "$outside_secrets/driver-linked-secrets.password" ]]

outside_backups="$temporary/outside-backups"
linked_backups="$temporary/linked-backups"
mkdir -p -- "$outside_backups"
ln -s -- "$outside_backups" "$linked_backups"
if MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
    --id vehicle-linked-backups \
    --config "$config_path" \
    --backup-dir "$linked_backups" >"$temporary/linked-backups.out" 2>&1; then
  fail_test 'add_vehicle accepted a symlink as --backup-dir'
fi
cmp -- "$directory_link_snapshot" "$config_path"
[[ -L "$linked_backups" ]]
[[ -z "$(find "$outside_backups" -mindepth 1 -maxdepth 1 -print -quit)" ]]
[[ ! -e "$secrets_dir/vehicle-linked-backups.token" ]]

# The final component can be a normal directory while an ancestor is a
# symlink. Reject that path before hardening the real directory, adding files,
# or publishing YAML.
ancestor_real_parent="$temporary/ancestor-real-parent"
ancestor_managed="$ancestor_real_parent/managed"
ancestor_alias="$temporary/ancestor-alias"
ancestor_sentinel="$ancestor_managed/sentinel"
mkdir -p -- "$ancestor_managed"
chmod 0750 -- "$ancestor_managed"
printf '%s\n' 'managed-directory-must-not-change' >"$ancestor_sentinel"
chmod 0600 -- "$ancestor_sentinel"
ln -s -- "$ancestor_real_parent" "$ancestor_alias"
aliased_managed="$ancestor_alias/managed"
[[ ! -L "$aliased_managed" ]]
ancestor_parent_mode="$(stat -c '%a' -- "$ancestor_real_parent")"
ancestor_managed_mode="$(stat -c '%a' -- "$ancestor_managed")"
ancestor_sentinel_checksum="$(cksum "$ancestor_sentinel")"

if MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
    --id driver-ancestor-symlink \
    --config "$config_path" \
    --vehicles vehicle-001 \
    --secrets-dir "$aliased_managed" \
    --backup-dir "$backup_dir" >"$temporary/ancestor-secrets.out" 2>&1; then
  fail_test 'add_driver accepted a secrets path with a symlink ancestor'
fi
cmp -- "$directory_link_snapshot" "$config_path"
[[ "$(stat -c '%a' -- "$ancestor_real_parent")" == "$ancestor_parent_mode" ]]
[[ "$(stat -c '%a' -- "$ancestor_managed")" == "$ancestor_managed_mode" ]]
[[ "$(cksum "$ancestor_sentinel")" == "$ancestor_sentinel_checksum" ]]
[[ "$(find "$ancestor_managed" -mindepth 1 -maxdepth 1 -print)" == "$ancestor_sentinel" ]]
[[ ! -e "$ancestor_managed/driver-ancestor-symlink.password" ]]

if MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
    --id vehicle-ancestor-symlink \
    --config "$config_path" \
    --backup-dir "$aliased_managed" >"$temporary/ancestor-backups.out" 2>&1; then
  fail_test 'add_vehicle accepted a backup path with a symlink ancestor'
fi
cmp -- "$directory_link_snapshot" "$config_path"
[[ "$(stat -c '%a' -- "$ancestor_real_parent")" == "$ancestor_parent_mode" ]]
[[ "$(stat -c '%a' -- "$ancestor_managed")" == "$ancestor_managed_mode" ]]
[[ "$(cksum "$ancestor_sentinel")" == "$ancestor_sentinel_checksum" ]]
[[ "$(find "$ancestor_managed" -mindepth 1 -maxdepth 1 -print)" == "$ancestor_sentinel" ]]
[[ ! -e "$secrets_dir/vehicle-ancestor-symlink.token" ]]

# Existing administrative directories must not be group/world writable. Test
# both credential and backup destinations so a permissive pre-created path
# cannot bypass the private-directory creation mode.
unsafe_secrets="$temporary/unsafe-secrets"
mkdir -p -- "$unsafe_secrets"
chmod 0777 -- "$unsafe_secrets"
if MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
    --id driver-unsafe-secrets \
    --config "$config_path" \
    --vehicles vehicle-001 \
    --secrets-dir "$unsafe_secrets" \
    --backup-dir "$backup_dir" >"$temporary/unsafe-secrets.out" 2>&1; then
  fail_test 'add_driver accepted a group/world-writable secrets directory'
fi
cmp -- "$directory_link_snapshot" "$config_path"
[[ ! -e "$unsafe_secrets/driver-unsafe-secrets.password" ]]

unsafe_backups="$temporary/unsafe-backups"
mkdir -p -- "$unsafe_backups"
chmod 0777 -- "$unsafe_backups"
if MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
    --id vehicle-unsafe-backups \
    --config "$config_path" \
    --backup-dir "$unsafe_backups" >"$temporary/unsafe-backups.out" 2>&1; then
  fail_test 'add_vehicle accepted a group/world-writable backup directory'
fi
cmp -- "$directory_link_snapshot" "$config_path"
[[ ! -e "$secrets_dir/vehicle-unsafe-backups.token" ]]

# Root-only container runs can also exercise the owner check without making
# non-root CI depend on chown privileges.
if [[ "$EUID" -eq 0 ]]; then
  foreign_secrets="$temporary/foreign-owned-secrets"
  mkdir -p -- "$foreign_secrets"
  chmod 0700 -- "$foreign_secrets"
  chown 65534 -- "$foreign_secrets"
  if MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
      --id driver-foreign-secrets \
      --config "$config_path" \
      --vehicles vehicle-001 \
      --secrets-dir "$foreign_secrets" \
      --backup-dir "$backup_dir" >"$temporary/foreign-secrets.out" 2>&1; then
    fail_test 'add_driver accepted a secrets directory owned by another uid'
  fi
  cmp -- "$directory_link_snapshot" "$config_path"
  [[ ! -e "$foreign_secrets/driver-foreign-secrets.password" ]]
fi

# Slow the real validator enough for two processes to overlap. With the shared
# config lock each process must re-read the other process's committed update;
# without serialization the last rename deterministically loses one identity.
slow_validator="$temporary/slow-validator"
{
  printf '%s\n' '#!/usr/bin/env bash' 'sleep 0.2'
  printf 'exec %q "$@"\n' "$validator"
} >"$slow_validator"
chmod 0755 -- "$slow_validator"

MINE_TELEOP_SIGNALING_SERVER_BIN="$slow_validator" "$add_driver" \
  --id driver-concurrent-a \
  --config "$config_path" \
  --vehicles vehicle-001 \
  --backup-dir "$backup_dir" >"$temporary/concurrent-driver-a.out" 2>&1 &
driver_a_pid=$!
MINE_TELEOP_SIGNALING_SERVER_BIN="$slow_validator" "$add_driver" \
  --id driver-concurrent-b \
  --config "$config_path" \
  --vehicles vehicle-001 \
  --backup-dir "$backup_dir" >"$temporary/concurrent-driver-b.out" 2>&1 &
driver_b_pid=$!
concurrent_failed=0
wait "$driver_a_pid" || concurrent_failed=1
wait "$driver_b_pid" || concurrent_failed=1
if [[ "$concurrent_failed" -ne 0 ]]; then
  sed 's/^/driver A: /' "$temporary/concurrent-driver-a.out" >&2
  sed 's/^/driver B: /' "$temporary/concurrent-driver-b.out" >&2
  fail_test 'one of the concurrent driver additions failed'
fi
assert_driver_identity "$config_path" driver-concurrent-a \
  secrets/driver-concurrent-a.password vehicle-001
assert_driver_identity "$config_path" driver-concurrent-b \
  secrets/driver-concurrent-b.password vehicle-001
[[ -s "$secrets_dir/driver-concurrent-a.password" ]]
[[ -s "$secrets_dir/driver-concurrent-b.password" ]]

MINE_TELEOP_SIGNALING_BIN="$slow_validator" "$add_vehicle" \
  --id vehicle-concurrent-a \
  --config "$config_path" \
  --assign-to-driver driver-console-001 \
  --backup-dir "$backup_dir" >"$temporary/concurrent-vehicle-a.out" 2>&1 &
vehicle_a_pid=$!
MINE_TELEOP_SIGNALING_BIN="$slow_validator" "$add_vehicle" \
  --id vehicle-concurrent-b \
  --config "$config_path" \
  --assign-to-driver driver-console-001 \
  --backup-dir "$backup_dir" >"$temporary/concurrent-vehicle-b.out" 2>&1 &
vehicle_b_pid=$!
concurrent_failed=0
wait "$vehicle_a_pid" || concurrent_failed=1
wait "$vehicle_b_pid" || concurrent_failed=1
if [[ "$concurrent_failed" -ne 0 ]]; then
  sed 's/^/vehicle A: /' "$temporary/concurrent-vehicle-a.out" >&2
  sed 's/^/vehicle B: /' "$temporary/concurrent-vehicle-b.out" >&2
  fail_test 'one of the concurrent vehicle additions failed'
fi
assert_vehicle_identity "$config_path" vehicle-concurrent-a \
  secrets/vehicle-concurrent-a.token
assert_vehicle_identity "$config_path" vehicle-concurrent-b \
  secrets/vehicle-concurrent-b.token
assert_driver_permission "$config_path" driver-console-001 vehicle-concurrent-a
assert_driver_permission "$config_path" driver-console-001 vehicle-concurrent-b
[[ -s "$secrets_dir/vehicle-concurrent-a.token" ]]
[[ -s "$secrets_dir/vehicle-concurrent-b.token" ]]

# Force the Python fallback even on hosts that have mikefarah/yq installed.
# Quoted section keys are legal YAML, and YAML-reserved words supplied as IDs
# must remain strings after the text editor inserts them.
fallback_bin="$temporary/fallback-bin"
mkdir -p -- "$fallback_bin"
printf '%s\n' '#!/usr/bin/env bash' 'printf "%s\n" "yq 3.4.1"' >"$fallback_bin/yq"
chmod 0755 -- "$fallback_bin/yq"

quoted_dir="$temporary/quoted-config"
quoted_secrets="$quoted_dir/secrets"
quoted_backups="$quoted_dir/backups"
mkdir -p -- "$quoted_secrets"
printf '%s\n' 'seed-driver-password' >"$quoted_secrets/seed-driver.password"
printf '%s\n' 'seed-vehicle-token' >"$quoted_secrets/seed-vehicle.token"
chmod 0600 -- "$quoted_secrets/seed-driver.password" "$quoted_secrets/seed-vehicle.token"
quoted_config="$quoted_dir/signaling-server.yaml"
printf '%s\n' \
  '"auth":' \
  '  "drivers":' \
  '    - "id": seed-driver' \
  '      password_file: secrets/seed-driver.password' \
  '      vehicles:' \
  '        - seed-vehicle' \
  '  "vehicles":' \
  '    - "id": seed-vehicle' \
  '      device_token_file: secrets/seed-vehicle.token' \
  >"$quoted_config"

PATH="$fallback_bin:$PATH" MINE_TELEOP_SIGNALING_SERVER_BIN="$validator" "$add_driver" \
  --id true \
  --config "$quoted_config" \
  --vehicles seed-vehicle \
  --backup-dir "$quoted_backups" >"$temporary/quoted-driver.out"
PATH="$fallback_bin:$PATH" MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
  --id null \
  --config "$quoted_config" \
  --assign-to-driver true \
  --assign-to-driver seed-driver \
  --backup-dir "$quoted_backups" >"$temporary/quoted-vehicle.out"
assert_driver_identity "$quoted_config" seed-driver \
  secrets/seed-driver.password seed-vehicle
assert_driver_identity "$quoted_config" true secrets/true.password seed-vehicle
assert_vehicle_identity "$quoted_config" seed-vehicle secrets/seed-vehicle.token
assert_vehicle_identity "$quoted_config" null secrets/null.token
assert_driver_permission "$quoted_config" true null
assert_driver_permission "$quoted_config" seed-driver null
"$validator" --config "$quoted_config" --validate-config >/dev/null

# The Python editor cannot safely rewrite a non-empty inline auth.vehicles
# sequence. Dry-run must exercise that renderer and fail before any apply-time
# credential, backup, or config mutation can occur.
inline_dir="$temporary/inline-config"
inline_secrets="$inline_dir/secrets"
mkdir -p -- "$inline_secrets"
printf '%s\n' 'inline-seed-token' >"$inline_secrets/inline-seed.token"
chmod 0600 -- "$inline_secrets/inline-seed.token"
inline_config="$inline_dir/signaling-server.yaml"
printf '%s\n' \
  'auth:' \
  '  drivers: []' \
  '  vehicles: [{id: inline-seed, device_token_file: secrets/inline-seed.token}]' \
  >"$inline_config"
inline_snapshot="$temporary/inline-before.yaml"
cp -- "$inline_config" "$inline_snapshot"
if PATH="$fallback_bin:$PATH" MINE_TELEOP_SIGNALING_BIN="$validator" "$add_vehicle" \
    --id inline-added \
    --config "$inline_config" \
    --backup-dir "$inline_dir/backups" \
    --dry-run >"$temporary/inline-dry-run.out" 2>&1; then
  fail_test 'add_vehicle dry-run accepted YAML that its apply path cannot edit'
fi
grep -qi 'auth.vehicles.*inline sequence' "$temporary/inline-dry-run.out"
cmp -- "$inline_snapshot" "$inline_config"
[[ ! -e "$inline_secrets/inline-added.token" ]]
[[ ! -d "$inline_dir/backups" ]]

# In a standalone cloud install there is no source-tree build fallback. Both
# tools must fail closed when their packaged validator is missing; warning and
# applying an unvalidated identity would make the safety check illusory.
standalone_root="$temporary/standalone"
standalone_install="$standalone_root/opt/mine-teleop"
standalone_config_dir="$standalone_root/etc/mine-teleop"
standalone_config="$standalone_config_dir/signaling-server.yaml"
mkdir -p -- "$standalone_install"
cp -- "$add_driver" "$standalone_install/add-driver.sh"
cp -- "$add_vehicle" "$standalone_install/add-vehicle.sh"
chmod 0755 -- "$standalone_install/add-driver.sh" "$standalone_install/add-vehicle.sh"
mkdir -p -- "$standalone_config_dir"
write_seed_fixture "$standalone_config_dir" "$standalone_config"
standalone_snapshot="$temporary/standalone-before.yaml"
cp -- "$standalone_config" "$standalone_snapshot"

# Use a closed PATH containing only normal script dependencies, deliberately
# omitting mine-teleop-signaling-server even if the test host installed one.
no_validator_path="$temporary/no-validator-path"
mkdir -p -- "$no_validator_path"
for dependency in \
    bash basename cat chmod chown cp date dirname env flock grep ln mkdir mktemp \
    mv openssl python3 realpath readlink rm rmdir sed sleep stat tr; do
  dependency_path="$(command -v "$dependency")"
  [[ -n "$dependency_path" ]] || fail_test "required test dependency is missing: $dependency"
  ln -s -- "$dependency_path" "$no_validator_path/$dependency"
done

if /usr/bin/env -u MINE_TELEOP_SIGNALING_SERVER_BIN -u MINE_TELEOP_SIGNALING_BIN \
    -u MINE_TELEOP_BUILD_DIR PATH="$no_validator_path" \
    "$standalone_install/add-driver.sh" \
    --id driver-without-validator \
    --config "$standalone_config" \
    --vehicles vehicle-001 \
    --backup-dir "$standalone_config_dir/backups" \
    >"$temporary/no-validator-driver.out" 2>&1; then
  fail_test 'standalone add-driver applied an identity without a validator'
fi
cmp -- "$standalone_snapshot" "$standalone_config"
[[ ! -e "$standalone_config_dir/secrets/driver-without-validator.password" ]]

if /usr/bin/env -u MINE_TELEOP_SIGNALING_SERVER_BIN -u MINE_TELEOP_SIGNALING_BIN \
    -u MINE_TELEOP_BUILD_DIR PATH="$no_validator_path" \
    "$standalone_install/add-vehicle.sh" \
    --id vehicle-without-validator \
    --config "$standalone_config" \
    --backup-dir "$standalone_config_dir/backups" \
    >"$temporary/no-validator-vehicle.out" 2>&1; then
  fail_test 'standalone add-vehicle applied an identity without a validator'
fi
cmp -- "$standalone_snapshot" "$standalone_config"
[[ ! -e "$standalone_config_dir/secrets/vehicle-without-validator.token" ]]

printf 'admin_identity_scripts_check=passed\n'
