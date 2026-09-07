#!/usr/bin/env bash
# Shared YAML backend dispatch for add_driver.sh and add_vehicle.sh.
#
# The callers retain their own validation, secret-path, and transaction
# semantics.  This library intentionally contains only engine selection and
# YAML query/edit operations.  It expects the caller to provide warn().

yaml_editor_lib_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
yaml_editor_python="$yaml_editor_lib_dir/yaml_editor.py"
yaml_engine=""

yaml_editor_error() {
  local message="$*"
  if declare -F die >/dev/null 2>&1; then
    die "$message"
  fi
  if declare -F fail >/dev/null 2>&1; then
    fail "$message"
  fi
  printf '%s\n' "$message" >&2
  return 2
}

select_yaml_engine() {
  if command -v yq >/dev/null 2>&1 && yq --version 2>/dev/null | grep -qi 'mikefarah\|version v4'; then
    yaml_engine="yq"
    return 0
  fi
  if command -v python3 >/dev/null 2>&1 && python3 -c 'import yaml' >/dev/null 2>&1; then
    [[ -r "$yaml_editor_python" ]] ||
      yaml_editor_error "shared python YAML editor is missing: $yaml_editor_python"
    yaml_engine="python"
    if command -v yq >/dev/null 2>&1; then
      warn "the yq on PATH is not mikefarah/yq v4; using the python3 fallback editor"
    else
      warn "yq (https://github.com/mikefarah/yq) not found; using the python3 fallback editor"
    fi
    return 0
  fi
  yaml_editor_error "no YAML backend available: install mikefarah/yq v4, or python3 with PyYAML"
}

# The fallback mutates text only after PyYAML has read the document, retaining
# comments, key order, indentation, and trailing-newline behavior.
python_yaml() {
  python3 "$yaml_editor_python" "$@"
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
        *)
          yaml_editor_error "yaml editor internal error: unsupported yaml path $path"
          ;;
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
      # Literal paths rather than a dynamic key: `.auth[strenv(...)]` support
      # varies across yq v4 releases.
      if [[ "$section" == "vehicles" ]]; then
        yq eval '(.auth.vehicles // [])[].id // ""' -- "$file"
      else
        yq eval '(.auth.drivers // [])[].id // ""' -- "$file"
      fi
      ;;
    python) python_yaml ids "$file" "$section" ;;
  esac
}

yaml_driver_vehicles() {
  local file="$1" driver="$2"
  case "$yaml_engine" in
    yq)
      DRIVER="$driver" yq eval \
        '(.auth.drivers // [])[] | select(.id == strenv(DRIVER)) | (.vehicles // [])[]' -- "$file"
      ;;
    python) python_yaml driver-vehicles "$file" "$driver" ;;
  esac
}

yaml_add_driver() {
  local file="$1" driver="$2" password_hash_file="$3" vehicles="$4"
  case "$yaml_engine" in
    yq)
      MINE_TELEOP_NEW_DRIVER_ID="$driver" \
        MINE_TELEOP_NEW_PASSWORD_HASH_FILE="$password_hash_file" \
        MINE_TELEOP_NEW_VEHICLES="$vehicles" \
        yq -i '.auth.drivers += [{
          "id": strenv(MINE_TELEOP_NEW_DRIVER_ID),
          "password_hash_file": strenv(MINE_TELEOP_NEW_PASSWORD_HASH_FILE),
          "vehicles": (strenv(MINE_TELEOP_NEW_VEHICLES) | split(","))
        }]' -- "$file"
      ;;
    python) python_yaml add-driver "$file" "$driver" "$password_hash_file" "$vehicles" ;;
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

yaml_add_vehicle() {
  local file="$1" vehicle="$2" token="$3"
  case "$yaml_engine" in
    yq)
      VEHICLE="$vehicle" TOKEN="$token" yq eval -i \
        '.auth.vehicles += [{"id": strenv(VEHICLE), "device_token_file": strenv(TOKEN)}]' -- "$file"
      ;;
    python) python_yaml add-vehicle "$file" "$vehicle" "$token" ;;
  esac
}

yaml_assign_vehicle() {
  local file="$1" driver="$2" vehicle="$3"
  case "$yaml_engine" in
    yq)
      DRIVER="$driver" VEHICLE="$vehicle" yq eval -i \
        '(.auth.drivers[] | select(.id == strenv(DRIVER)) | .vehicles) += [strenv(VEHICLE)]' -- "$file"
      ;;
    python) python_yaml assign "$file" "$driver" "$vehicle" ;;
  esac
}
