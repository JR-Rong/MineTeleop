#!/usr/bin/env bash
#
# add_driver.sh - register a new driver identity in a signaling server config.
#
# Generates a random password file (0600) and appends a driver entry to the
# `auth.drivers` sequence of the multi-identity YAML consumed by
# `mine-teleop-signaling-server --config`.
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
  --dry-run                 report the planned changes without writing anything
  --help                    show this help

Requires: openssl, plus one YAML backend: yq (mikefarah/yq v4) or python3 with
PyYAML (used automatically when yq is unavailable).
EOF
}

# ---------------------------------------------------------------------------
# YAML backend
# ---------------------------------------------------------------------------

yaml_engine=""

select_yaml_engine() {
  if command -v yq >/dev/null 2>&1 && yq --version 2>/dev/null | grep -qi 'mikefarah\|version v4'; then
    yaml_engine="yq"
    return 0
  fi
  if command -v python3 >/dev/null 2>&1 && python3 -c 'import yaml' >/dev/null 2>&1; then
    yaml_engine="python"
    if command -v yq >/dev/null 2>&1; then
      warn "the yq on PATH is not mikefarah/yq v4; using the python3 fallback editor"
    else
      warn "yq (https://github.com/mikefarah/yq) not found; using the python3 fallback editor"
    fi
    return 0
  fi
  die "no YAML backend available: install mikefarah/yq v4, or python3 with PyYAML"
}

# Embedded fallback editor. Reads/edits the YAML as text so comments, key order
# and indentation survive; PyYAML is only used for the read-only queries.
python_yaml() {
  python3 - "$@" <<'PYTHON'
import re
import sys

mode, path = sys.argv[1], sys.argv[2]
arguments = sys.argv[3:]


def die(message):
    sys.stderr.write("yaml edit failed: %s\n" % message)
    raise SystemExit(2)


def load_text():
    with open(path, "r", encoding="utf-8") as handle:
        text = handle.read()
    trailing_newline = text.endswith("\n")
    lines = text.split("\n")
    if trailing_newline:
        lines.pop()
    return lines, trailing_newline


def store_text(lines, trailing_newline):
    text = "\n".join(lines)
    if trailing_newline:
        text += "\n"
    with open(path, "w", encoding="utf-8") as handle:
        handle.write(text)


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def is_filler(line):
    stripped = line.strip()
    return stripped == "" or stripped.startswith("#")


def block_end(lines, key_index, limit=None):
    """Index just past the last content line owned by the key at key_index."""
    base = indent_of(lines[key_index])
    limit = len(lines) if limit is None else limit
    end = key_index + 1
    index = key_index + 1
    while index < limit:
        line = lines[index]
        if is_filler(line):
            index += 1
            continue
        current = indent_of(line)
        if current > base or (current == base and line.lstrip().startswith("- ")):
            end = index + 1
            index += 1
            continue
        break
    return end


def find_key(lines, start, limit, key, indent=None):
    pattern = re.compile(r"^( *)" + re.escape(key) + r":( *)(.*)$")
    for index in range(start, limit):
        match = pattern.match(lines[index])
        if match is None:
            continue
        if indent is not None and len(match.group(1)) != indent:
            continue
        return index, match.group(3).strip()
    return -1, ""


def child_indent(lines, key_index, limit, default):
    for index in range(key_index + 1, limit):
        if is_filler(lines[index]):
            continue
        return indent_of(lines[index])
    return default


def sequence_indent(lines, start, limit, default):
    for index in range(start, limit):
        line = lines[index]
        if is_filler(line):
            continue
        if line.lstrip().startswith("- "):
            return indent_of(line)
    return default


def auth_section(lines, section):
    auth_index, auth_inline = find_key(lines, 0, len(lines), "auth", indent=0)
    if auth_index < 0:
        die("top level `auth` mapping not found")
    if auth_inline:
        die("`auth` must be a block mapping, found an inline value")
    auth_limit = block_end(lines, auth_index)
    auth_child = child_indent(lines, auth_index, auth_limit, 2)
    section_index, section_inline = find_key(
        lines, auth_index + 1, auth_limit, section, indent=auth_child)
    return auth_index, auth_limit, auth_child, section_index, section_inline


def quote(value):
    if re.match(r"^[A-Za-z0-9._/-]+$", value):
        return value
    return '"%s"' % value.replace("\\", "\\\\").replace('"', '\\"')


def parsed_document():
    try:
        import yaml
    except ImportError:
        die("PyYAML is required for the python fallback backend")
    with open(path, "r", encoding="utf-8") as handle:
        document = yaml.safe_load(handle)
    if document is None:
        document = {}
    if not isinstance(document, dict):
        die("config root must be a mapping")
    return document


def entries(section):
    document = parsed_document()
    auth = document.get("auth") or {}
    if not isinstance(auth, dict):
        die("`auth` must be a mapping")
    items = auth.get(section) or []
    if not isinstance(items, list):
        die("`auth.%s` must be a sequence" % section)
    return items


def driver_span(lines, driver_id):
    """(start, stop, item_indent) of the `auth.drivers` entry with this id."""
    _, auth_limit, auth_child, drivers_index, drivers_inline = auth_section(lines, "drivers")
    if drivers_index < 0:
        die("`auth.drivers` not found")
    if drivers_inline:
        die("`auth.drivers` uses an inline sequence; install mikefarah/yq v4 to edit it")
    drivers_limit = block_end(lines, drivers_index, auth_limit)
    item = sequence_indent(lines, drivers_index + 1, drivers_limit, auth_child + 2)
    starts = [index for index in range(drivers_index + 1, drivers_limit)
              if indent_of(lines[index]) == item and lines[index].lstrip().startswith("- ")]
    for position, start in enumerate(starts):
        stop = starts[position + 1] if position + 1 < len(starts) else drivers_limit
        head = re.match(r"^ *- +id: *(.*)$", lines[start])
        matched = head is not None and head.group(1).strip().strip("\"'") == driver_id
        if not matched:
            body, _ = find_key(lines, start + 1, stop, "id", indent=item + 2)
            if body >= 0:
                matched = lines[body].split(":", 1)[1].strip().strip("\"'") == driver_id
        if matched:
            return start, stop, item
    return -1, -1, item


if mode == "tag":
    node = parsed_document()
    for key in arguments[0].split("."):
        if not isinstance(node, dict):
            node = None
            break
        node = node.get(key)
    kinds = ((bool, "!!bool"), (dict, "!!map"), (list, "!!seq"),
             (str, "!!str"), (int, "!!int"), (float, "!!float"))
    printed = "!!null"
    if node is not None:
        printed = "!!unknown"
        for kind, name in kinds:
            if isinstance(node, kind):
                printed = name
                break
    print(printed)
    raise SystemExit(0)

if mode == "length":
    print(len(entries(arguments[0])))
    raise SystemExit(0)

if mode == "ids":
    for entry in entries(arguments[0]):
        if isinstance(entry, dict) and entry.get("id") is not None:
            print(entry["id"])
    raise SystemExit(0)

if mode == "show-driver":
    lines, _ = load_text()
    start, stop, _ = driver_span(lines, arguments[0])
    if start < 0:
        die("driver `%s` not found under auth.drivers" % arguments[0])
    while stop > start and is_filler(lines[stop - 1]):
        stop -= 1
    for line in lines[start:stop]:
        print(line)
    raise SystemExit(0)

if mode == "add-driver":
    driver_id, password_file = arguments[0], arguments[1]
    vehicles = [item for item in arguments[2].split(",") if item]
    if not vehicles:
        die("the new driver needs at least one vehicle")
    lines, trailing_newline = load_text()
    auth_index, auth_limit, auth_child, index, inline = auth_section(lines, "drivers")

    payload = []
    if index < 0:
        item = auth_child + 2
        insert_at = block_end(lines, auth_index)
        payload.append(" " * auth_child + "drivers:")
    else:
        if inline and inline != "[]":
            die("`auth.drivers` uses an inline sequence; install mikefarah/yq v4 to edit it")
        if inline == "[]":
            lines[index] = " " * auth_child + "drivers:"
            item = auth_child + 2
            insert_at = index + 1
        else:
            limit = block_end(lines, index, auth_limit)
            item = sequence_indent(lines, index + 1, limit, auth_child + 2)
            insert_at = limit

    payload.extend([
        " " * item + "- id: " + quote(driver_id),
        " " * (item + 2) + "password_file: " + quote(password_file),
        " " * (item + 2) + "vehicles:",
    ])
    payload.extend(" " * (item + 4) + "- " + quote(vehicle) for vehicle in vehicles)

    lines[insert_at:insert_at] = payload
    store_text(lines, trailing_newline)
    raise SystemExit(0)

die("unknown mode: %s" % mode)
PYTHON
}

# Tag of a node, in yq's `!!map` / `!!seq` / `!!null` notation.
yaml_tag() {
  local file="$1" path="$2"
  case "$yaml_engine" in
    yq)
      # Literal paths rather than a dynamic key: `.auth[strenv(...)]` support
      # varies across yq v4 releases.
      case "$path" in
        auth) yq '.auth | tag' -- "$file" ;;
        auth.drivers) yq '.auth.drivers | tag' -- "$file" ;;
        auth.vehicles) yq '.auth.vehicles | tag' -- "$file" ;;
        *) die "internal error: unsupported yaml path $path" ;;
      esac
      ;;
    python) python_yaml tag "$file" "$path" ;;
  esac
}

yaml_length() {
  local file="$1" section="$2"
  case "$yaml_engine" in
    yq)
      if [[ "$section" == "vehicles" ]]; then
        yq '(.auth.vehicles // []) | length' -- "$file"
      else
        yq '(.auth.drivers // []) | length' -- "$file"
      fi
      ;;
    python) python_yaml length "$file" "$section" ;;
  esac
}

yaml_ids() {
  local file="$1" section="$2"
  case "$yaml_engine" in
    yq)
      if [[ "$section" == "vehicles" ]]; then
        yq '(.auth.vehicles // [])[].id // ""' -- "$file"
      else
        yq '(.auth.drivers // [])[].id // ""' -- "$file"
      fi
      ;;
    python) python_yaml ids "$file" "$section" ;;
  esac
}

yaml_add_driver() {
  local file="$1" driver="$2" password_file="$3" vehicles="$4"
  case "$yaml_engine" in
    yq)
      MINE_TELEOP_NEW_DRIVER_ID="$driver" \
        MINE_TELEOP_NEW_PASSWORD_FILE="$password_file" \
        MINE_TELEOP_NEW_VEHICLES="$vehicles" \
        yq -i '.auth.drivers += [{
          "id": strenv(MINE_TELEOP_NEW_DRIVER_ID),
          "password_file": strenv(MINE_TELEOP_NEW_PASSWORD_FILE),
          "vehicles": (strenv(MINE_TELEOP_NEW_VEHICLES) | split(","))
        }]' -- "$file"
      ;;
    python) python_yaml add-driver "$file" "$driver" "$password_file" "$vehicles" ;;
  esac
}

yaml_show_driver() {
  local file="$1" driver="$2"
  case "$yaml_engine" in
    yq)
      MINE_TELEOP_NEW_DRIVER_ID="$driver" yq \
        '(.auth.drivers // [])[] | select(.id == strenv(MINE_TELEOP_NEW_DRIVER_ID))' -- "$file"
      ;;
    python) python_yaml show-driver "$file" "$driver" ;;
  esac
}

driver_id=''
config_path=''
secrets_dir=''
vehicles_csv=''
dry_run=0

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

for existing in "${existing_drivers[@]}"; do
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
if [[ -e "$password_path" ]]; then
  die "credential file already exists: $password_path
      refusing to overwrite an existing credential; remove or rename it first"
fi

# password_file is resolved relative to the config directory by the server, so
# record a relative path whenever the secrets directory lives under it. -m keeps
# this working before the credential (or its directory) exists.
password_file_value="$password_path"
if [[ "$config_dir" == "$repo_root/configs" && "$password_path" == "$repo_root/.local/"* ]]; then
  password_file_value="../${password_path#"$repo_root"/}"
elif relative="$(realpath -m --relative-to="$config_dir" -- "$password_path" 2>/dev/null)" &&
  [[ -n "$relative" && "$relative" != /* && "$relative" != ../* ]]; then
  password_file_value="$relative"
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

# Render the modified document into a sibling temp file: relative password_file
# paths then resolve exactly as they will once the file is in place.
created_secrets_dir="no"
created_password="no"
work_path="$(mktemp "$config_dir/.${config_name}.add-driver.XXXXXX")"
cleanup() {
  rm -f -- "$work_path"
  [[ "$created_password" == "yes" ]] && rm -f -- "$password_path"
  [[ "$created_secrets_dir" == "yes" ]] && rmdir -- "$secrets_dir" 2>/dev/null
  return 0
}
trap cleanup EXIT
cat -- "$config_path" >"$work_path"

MINE_TELEOP_NEW_VEHICLES="$vehicles_display"
yaml_add_driver "$work_path" "$driver_id" "$password_file_value" "$vehicles_display" ||
  die "the $yaml_engine backend failed to append the driver entry; $config_name was not modified"

[[ "$(yaml_ids "$work_path" drivers | grep -c -x -F -- "$driver_id")" == '1' ]] ||
  die "the rendered config does not contain exactly one '$driver_id' entry; $config_name was not modified"

if [[ $dry_run -eq 1 ]]; then
  info "dry run: no files were created or modified"
  printf '\n%splanned credential%s\n' "$color_bold" "$color_reset"
  printf '  openssl rand -base64 32 > %s\n' "$password_path"
  printf '  chmod 0600 %s\n' "$password_path"
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

# Credential first: the config must never reference a password file that does
# not exist yet.
if [[ ! -d "$secrets_dir" ]]; then
  (umask 077 && mkdir -p -- "$secrets_dir") || die "cannot create secrets directory: $secrets_dir"
  created_secrets_dir="yes"
  chmod 0700 "$secrets_dir"
  ok "created secrets directory $secrets_dir (mode 0700)"
fi
[[ -w "$secrets_dir" ]] || die "secrets directory is not writable: $secrets_dir"

if ! (umask 077 && openssl rand -base64 32 >"$password_path"); then
  rm -f -- "$password_path"
  die "openssl failed to generate a password for $driver_id"
fi
created_password="yes"
chmod 0600 "$password_path"
[[ -n "$(tr -d '\r\n' <"$password_path")" ]] ||
  die "generated credential is empty after trimming: $password_path"
ok "generated credential $password_path (mode 0600)"

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
printf '  password file  %s (password_file: %s)\n' "$password_path" "$password_file_value"
printf '  vehicles       %s\n' "${requested_vehicles[*]}"
printf '\n%snext steps%s\n' "$color_bold" "$color_reset"
printf '  1. restart the signaling server so the new identity is loaded:\n'
printf '       sudo systemctl restart mine-teleop-signaling-server\n'
printf '  2. share the password with %s over a secure channel (never email or chat):\n' "$driver_id"
printf '       cat %s\n' "$password_path"
printf '  3. keep %s out of version control and backed up with restricted access.\n' "$secrets_dir"
