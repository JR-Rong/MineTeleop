#!/usr/bin/env bash
#
# add_driver.sh - register a new driver identity in a signaling server config.
#
# Generates a random password file (0600), derives an Argon2id verifier file
# (0600), and appends a hash-backed driver entry to the `auth.drivers`
# sequence consumed by `mine-teleop-signaling-server --config`.
#
# Usage:
#   add_driver.sh --id DRIVER_ID --config YAML_PATH --vehicles ID[,ID...]
#                 [--secrets-dir DIR] [--dry-run]
#   add_driver.sh --help
#
# Example:
#   scripts/admin/add_driver.sh \
#     --id driver-console-003 \
#     --config configs/signaling-server.2x2.dev.yaml \
#     --vehicles vehicle-001,vehicle-002
#
# The script mirrors the startup checks in `load_signaling_identity_config`
# (cpp/src/server.cpp): unique driver ids, a non-empty vehicle list without
# duplicates, and every referenced vehicle declared under `auth.vehicles`.
# The config is only replaced after the modified copy validates, so a failure
# leaves the original file untouched.
#
# YAML editing backend: `yq` (https://github.com/mikefarah/yq, v4) when present,
# otherwise a python3 + PyYAML fallback that inserts lines in place. Both keep
# surrounding comments and formatting intact.
#
# Environment:
#   MINE_TELEOP_SIGNALING_SERVER_BIN  explicit path to mine-teleop-signaling-server
#   MINE_TELEOP_SIGNALING_BIN         same, accepted for parity with add_vehicle.sh
#   NO_COLOR                          disable colored output
#
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"

if [[ -t 2 && -z "${NO_COLOR:-}" ]]; then
  color_reset=$'\033[0m'
  color_red=$'\033[31m'
  color_green=$'\033[32m'
  color_yellow=$'\033[33m'
  color_blue=$'\033[34m'
  color_bold=$'\033[1m'
else
  color_reset='' color_red='' color_green='' color_yellow='' color_blue='' color_bold=''
fi

info() { printf '%s==>%s %s\n' "$color_blue" "$color_reset" "$*"; }
ok() { printf '%s  ok%s %s\n' "$color_green" "$color_reset" "$*"; }
warn() { printf '%swarn%s %s\n' "$color_yellow" "$color_reset" "$*" >&2; }
die() {
  printf '%serror%s %s\n' "$color_red" "$color_reset" "$*" >&2
  exit 2
}

usage() {
  cat <<'EOF'
Usage:
  add_driver.sh --id DRIVER_ID --config YAML_PATH --vehicles ID[,ID...] [options]
  add_driver.sh --help

Required:
  --id DRIVER_ID            new driver identity, e.g. driver-console-003
  --config YAML_PATH        signaling server multi-identity YAML
  --vehicles ID[,ID...]     comma-separated vehicles the driver may control;
                            each one must already exist under auth.vehicles

Options:
  --secrets-dir DIR         credential directory (default: .local for repo configs,
                            otherwise secrets/ next to the config)
  --password-stdin          read a supplied password once from standard input
  --dry-run                 report the planned changes without writing anything
  --help                    show this help

Requires: openssl, argon2, plus one YAML backend: yq (mikefarah/yq v4) or
python3 with PyYAML (used automatically when yq is unavailable).
EOF
}

# ---------------------------------------------------------------------------
# YAML backend
# ---------------------------------------------------------------------------

# Keep this user-facing entrypoint and its credential transaction local; the
# shared library owns only engine dispatch and YAML edits.
# shellcheck source=lib/yaml_editor.sh
source "$script_dir/lib/yaml_editor.sh"

driver_id=''
config_path=''
secrets_dir=''
vehicles_csv=''
dry_run=0
password_stdin=0

require_value() {
  [[ $# -ge 2 && -n "$2" ]] || die "$1 requires a value"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --id)
      require_value "$1" "${2:-}"
      driver_id="$2"
      shift 2
      ;;
    --config)
      require_value "$1" "${2:-}"
      config_path="$2"
      shift 2
      ;;
    --secrets-dir)
      require_value "$1" "${2:-}"
      secrets_dir="$2"
      shift 2
      ;;
    --vehicles)
      require_value "$1" "${2:-}"
      vehicles_csv="$2"
      shift 2
      ;;
    --dry-run)
      dry_run=1
      shift
      ;;
    --password-stdin)
      password_stdin=1
      shift
      ;;
    --help | -h)
      usage
      exit 0
      ;;
    *)
      usage >&2
      die "unknown argument: $1"
      ;;
  esac
done

[[ -n "$driver_id" ]] || { usage >&2; die "--id is required"; }
[[ -n "$config_path" ]] || { usage >&2; die "--config is required"; }
[[ -n "$vehicles_csv" ]] || { usage >&2; die "--vehicles is required and must not be empty"; }

[[ "$driver_id" =~ ^[A-Za-z0-9][A-Za-z0-9._-]*$ ]] ||
  die "driver id must start alphanumeric and contain only letters, digits, '.', '_' or '-': $driver_id"

command -v openssl >/dev/null 2>&1 || die "openssl is required but was not found in PATH"
command -v argon2 >/dev/null 2>&1 || die "argon2 is required but was not found in PATH"
select_yaml_engine

[[ -f "$config_path" ]] || die "config file does not exist: $config_path"
[[ -r "$config_path" ]] || die "config file is not readable: $config_path"
config_dir="$(CDPATH= cd -- "$(dirname -- "$config_path")" && pwd)"
config_name="$(basename -- "$config_path")"
config_path="$config_dir/$config_name"
[[ -w "$config_dir" ]] || die "config directory is not writable: $config_dir"
if [[ $dry_run -eq 0 ]]; then
  [[ -w "$config_path" ]] || die "config file is not writable: $config_path"
fi

# Keep credentials generated for repository development configs out of the
# distributable configs tree. Installed /etc-style configs continue to use a
# sibling secrets directory. An explicit --secrets-dir is resolved against the
# caller's CWD.
if [[ -z "$secrets_dir" ]]; then
  if [[ "$config_dir" == "$repo_root/configs" ]]; then
    secrets_dir="$repo_root/.local/secrets/$(basename -- "$config_path" .yaml)"
  else
    secrets_dir="$config_dir/secrets"
  fi
elif [[ "$secrets_dir" != /* ]]; then
  secrets_dir="$PWD/$secrets_dir"
fi
secrets_dir="${secrets_dir%/}"

# Structural checks before touching anything: a malformed config would make the
# append silently reshape the document.
[[ "$(yaml_tag "$config_path" auth 2>/dev/null || true)" == '!!map' ]] ||
  die "auth mapping is required in $config_path"
[[ "$(yaml_tag "$config_path" auth.drivers)" == '!!seq' ]] ||
  die "auth.drivers must be a sequence in $config_path"
[[ "$(yaml_tag "$config_path" auth.vehicles)" == '!!seq' ]] ||
  die "auth.vehicles must be a sequence in $config_path"
[[ "$(yaml_length "$config_path" vehicles)" -gt 0 ]] ||
  die "auth.vehicles must be a non-empty sequence in $config_path"

# Read in the parent shell: a backend failure must abort here, because an
# unreadable section would otherwise look like an empty one and defeat the
# duplicate and membership checks below.
drivers_raw="$(yaml_ids "$config_path" drivers)" ||
  die "cannot read auth.drivers from $config_path (is it valid YAML?)"
vehicles_raw="$(yaml_ids "$config_path" vehicles)" ||
  die "cannot read auth.vehicles from $config_path (is it valid YAML?)"
existing_drivers=()
while IFS= read -r line; do
  [[ -z "$line" ]] || existing_drivers+=("$line")
done <<<"$drivers_raw"
known_vehicles=()
while IFS= read -r line; do
  [[ -z "$line" ]] || known_vehicles+=("$line")
done <<<"$vehicles_raw"

for existing in "${existing_drivers[@]+"${existing_drivers[@]}"}"; do
  [[ "$existing" == "$driver_id" ]] &&
    die "driver id already exists in $config_name: $driver_id (the server rejects duplicate driver ids)"
done

# Parse and validate the requested vehicle permissions.
requested_vehicles=()
IFS=',' read -r -a raw_vehicles <<<"$vehicles_csv"
for raw in "${raw_vehicles[@]}"; do
  vehicle="${raw#"${raw%%[![:space:]]*}"}"
  vehicle="${vehicle%"${vehicle##*[![:space:]]}"}"
  [[ -n "$vehicle" ]] || die "--vehicles contains an empty entry: '$vehicles_csv'"
  for seen in "${requested_vehicles[@]+"${requested_vehicles[@]}"}"; do
    [[ "$seen" == "$vehicle" ]] &&
      die "--vehicles lists '$vehicle' more than once (the server rejects duplicate vehicle permissions)"
  done
  vehicle_known=0
  for known in "${known_vehicles[@]}"; do
    [[ "$known" == "$vehicle" ]] && vehicle_known=1 && break
  done
  [[ $vehicle_known -eq 1 ]] ||
    die "vehicle '$vehicle' is not declared under auth.vehicles in $config_name; known vehicles: ${known_vehicles[*]}"
  requested_vehicles+=("$vehicle")
done
[[ ${#requested_vehicles[@]} -gt 0 ]] || die "--vehicles must list at least one vehicle"

password_path="$secrets_dir/$driver_id.password"
password_hash_path="$secrets_dir/$driver_id.password.argon2id"
for credential_path in "$password_path" "$password_hash_path"; do
  if [[ -e "$credential_path" ]]; then
    die "credential file already exists: $credential_path
        refusing to overwrite an existing credential; remove or rename it first"
  fi
done

# password_hash_file is resolved relative to the config directory by the
# server, so record a relative path whenever the secrets directory lives under
# it. -m keeps this working before the credential (or its directory) exists.
password_hash_file_value="$password_hash_path"
if [[ "$config_dir" == "$repo_root/configs" && "$password_hash_path" == "$repo_root/.local/"* ]]; then
  password_hash_file_value="../${password_hash_path#"$repo_root"/}"
elif relative="$(realpath -m --relative-to="$config_dir" -- "$password_hash_path" 2>/dev/null)" &&
  [[ -n "$relative" && "$relative" != /* && "$relative" != ../* ]]; then
  password_hash_file_value="$relative"
fi

vehicles_display="$(
  IFS=','
  printf '%s' "${requested_vehicles[*]}"
)"

signaling_binary=''
signaling_bin_variable=''
# Both names are accepted so the admin scripts share one knob; the more specific
# one wins when the caller exported both.
if [[ -n "${MINE_TELEOP_SIGNALING_SERVER_BIN:-}" ]]; then
  signaling_bin_variable='MINE_TELEOP_SIGNALING_SERVER_BIN'
  signaling_binary="$MINE_TELEOP_SIGNALING_SERVER_BIN"
elif [[ -n "${MINE_TELEOP_SIGNALING_BIN:-}" ]]; then
  signaling_bin_variable='MINE_TELEOP_SIGNALING_BIN'
  signaling_binary="$MINE_TELEOP_SIGNALING_BIN"
fi
if [[ -n "$signaling_binary" ]]; then
  [[ -x "$signaling_binary" ]] ||
    die "$signaling_bin_variable is not executable: $signaling_binary"
elif command -v mine-teleop-signaling-server >/dev/null 2>&1; then
  signaling_binary="$(command -v mine-teleop-signaling-server)"
fi

# Render the modified document into a sibling temp file: relative verifier
# paths then resolve exactly as they will once the file is in place.
created_secrets_dir="no"
created_password="no"
created_password_hash="no"
password_stage=''
password_hash_stage=''
work_path="$(mktemp "$config_dir/.${config_name}.add-driver.XXXXXX")"
cleanup() {
  rm -f -- "$work_path"
  [[ -n "$password_stage" ]] && rm -f -- "$password_stage"
  [[ -n "$password_hash_stage" ]] && rm -f -- "$password_hash_stage"
  [[ "$created_password" == "yes" ]] && rm -f -- "$password_path"
  [[ "$created_password_hash" == "yes" ]] && rm -f -- "$password_hash_path"
  [[ "$created_secrets_dir" == "yes" ]] && rmdir -- "$secrets_dir" 2>/dev/null
  return 0
}
trap cleanup EXIT
cat -- "$config_path" >"$work_path"

MINE_TELEOP_NEW_VEHICLES="$vehicles_display"
yaml_add_driver "$work_path" "$driver_id" "$password_hash_file_value" "$vehicles_display" ||
  die "the $yaml_engine backend failed to append the driver entry; $config_name was not modified"

[[ "$(yaml_ids "$work_path" drivers | grep -c -x -F -- "$driver_id")" == '1' ]] ||
  die "the rendered config does not contain exactly one '$driver_id' entry; $config_name was not modified"

if [[ $dry_run -eq 1 ]]; then
  info "dry run: no files were created or modified"
  printf '\n%splanned credential%s\n' "$color_bold" "$color_reset"
  printf '  openssl rand -base64 32 > %s\n' "$password_path"
  printf '  chmod 0600 %s\n' "$password_path"
  printf '  argon2 <password-from-stdin> -id -t 3 -m 16 -p 1 -e > %s\n' "$password_hash_path"
  printf '  chmod 0600 %s\n' "$password_hash_path"
  [[ -d "$secrets_dir" ]] || printf '  (creates directory %s with mode 0700)\n' "$secrets_dir"
  printf '\n%splanned %s change%s\n' "$color_bold" "$config_name" "$color_reset"
  if command -v diff >/dev/null 2>&1; then
    diff -u --label "$config_name" --label "$config_name (after)" \
      -- "$config_path" "$work_path" || true
  else
    yaml_show_driver "$work_path" "$driver_id"
  fi
  if [[ -n "$signaling_binary" ]]; then
    printf '\n'
    info "would validate with $signaling_binary --validate-config"
  fi
  exit 0
fi

# Credentials first: the config must never reference a verifier file that does
# not exist yet.
if [[ ! -d "$secrets_dir" ]]; then
  (umask 077 && mkdir -p -- "$secrets_dir") || die "cannot create secrets directory: $secrets_dir"
  created_secrets_dir="yes"
  chmod 0700 "$secrets_dir"
  ok "created secrets directory $secrets_dir (mode 0700)"
fi
[[ -w "$secrets_dir" ]] || die "secrets directory is not writable: $secrets_dir"

password_stage="$(mktemp "$secrets_dir/.${driver_id}.password.XXXXXX")"
password_hash_stage="$(mktemp "$secrets_dir/.${driver_id}.password.argon2id.XXXXXX")"
chmod 0600 "$password_stage" "$password_hash_stage"
if [[ $password_stdin -eq 1 ]]; then
  if ! IFS= read -r supplied_password; then
    die "--password-stdin did not receive a password"
  fi
  if [[ -z "$supplied_password" ]]; then
    unset supplied_password
    die "--password-stdin received an empty password"
  fi
  printf '%s\n' "$supplied_password" >"$password_stage"
  unset supplied_password
elif ! (umask 077 && openssl rand -base64 32 >"$password_stage"); then
  die "openssl failed to generate a password for $driver_id"
fi
[[ -n "$(tr -d '\r\n' <"$password_stage")" ]] ||
  die "generated credential is empty after trimming: $password_stage"
if ! argon2_salt="$(openssl rand -hex 16)" || [[ -z "$argon2_salt" ]]; then
  die "openssl failed to generate an Argon2id salt for $driver_id"
fi
if ! tr -d '\r\n' <"$password_stage" |
  argon2 "$argon2_salt" -id -t 3 -m 16 -p 1 -e >"$password_hash_stage"; then
  unset argon2_salt
  die "argon2 failed to derive a verifier for $driver_id"
fi
unset argon2_salt
grep -Eq '^\$argon2id\$v=19\$m=65536,t=3,p=1\$' "$password_hash_stage" ||
  die "argon2 did not produce the required Argon2id verifier policy"
mv -f -- "$password_stage" "$password_path"
password_stage=''
created_password="yes"
mv -f -- "$password_hash_stage" "$password_hash_path"
password_hash_stage=''
created_password_hash="yes"
chmod 0600 "$password_path" "$password_hash_path"
ok "generated password and Argon2id verifier for $driver_id (mode 0600)"

validate_config() {
  local target="$1" label="$2" output status=0
  output="$("$signaling_binary" --validate-config --config "$target" 2>&1)" || status=$?
  if [[ $status -eq 0 ]]; then
    ok "$label validated by mine-teleop-signaling-server"
    return 0
  fi
  # Pre-existing identities that read their secret from the environment cannot
  # be validated without those variables exported; that failure is unrelated to
  # the entry added here.
  if grep -q 'environment variable is unset or empty' <<<"$output"; then
    warn "skipped full validation of $label: an existing identity reads its secret from the environment"
    warn "$output"
    return 0
  fi
  printf '%s\n' "$output" >&2
  return "$status"
}

if [[ -n "$signaling_binary" ]]; then
  validate_config "$work_path" "pending config" ||
    die "validation rejected the new driver entry; $config_name and the credential were left unchanged"
else
  warn "mine-teleop-signaling-server not found in PATH; skipped --validate-config"
  warn "set MINE_TELEOP_SIGNALING_SERVER_BIN to validate with a locally built binary"
fi

# Publish the new config, preserving the original file mode.
config_mode="$(stat -c '%a' -- "$config_path" 2>/dev/null || stat -f '%Lp' -- "$config_path")"
chmod "$config_mode" "$work_path"
mv -f -- "$work_path" "$config_path"
trap - EXIT
ok "updated $config_path"

if [[ -n "$signaling_binary" ]]; then
  validate_config "$config_path" "$config_name" ||
    warn "the installed config failed validation; inspect $config_path"
fi

printf '\n%sdriver added%s\n' "$color_bold$color_green" "$color_reset"
printf '  driver id      %s\n' "$driver_id"
printf '  config         %s\n' "$config_path"
printf '  password file  %s (mode 0600; do not commit)\n' "$password_path"
printf '  verifier file  %s (password_hash_file: %s)\n' "$password_hash_path" "$password_hash_file_value"
printf '  vehicles       %s\n' "${requested_vehicles[*]}"
printf '\n%snext steps%s\n' "$color_bold" "$color_reset"
printf '  1. restart the signaling server so the new identity is loaded:\n'
printf '       sudo systemctl restart mine-teleop-signaling-server\n'
printf '  2. share the password with %s over a secure channel (never email or chat):\n' "$driver_id"
printf '       cat %s\n' "$password_path"
printf '  3. keep %s out of version control and backed up with restricted access.\n' "$secrets_dir"
