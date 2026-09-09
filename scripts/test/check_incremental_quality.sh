#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/test/check_incremental_quality.sh [options]

Checks only changed or new relevant files after the common ancestor of --base
and HEAD, plus staged, unstaged, and untracked changes. It never rewrites files.

Options:
  --base <git-revision>       Compare against this revision. Defaults to
                              MINE_TELEOP_QUALITY_BASE, origin/main, or main.
  --compile-commands <path>   Directory containing compile_commands.json for
                              bounded clang-tidy checks of changed C++ TUs.
  --require-format            Fail when clang-format/git-clang-format 18.1.8
                              are unavailable.
  --require-eslint            Fail when ESLint 9 is unavailable for changed JS.
  --require-tidy              Fail unless --compile-commands and clang-tidy 18
                              are available for changed C++ translation units.
  --help                      Show this help.

Tool paths can be overridden with MINE_TELEOP_CLANG_FORMAT,
MINE_TELEOP_GIT_CLANG_FORMAT, MINE_TELEOP_CLANG_TIDY, and MINE_TELEOP_ESLINT.
Pass the target branch tip, push predecessor, or explicit predecessor as --base;
the script uses its merge-base with HEAD as the comparison baseline.
USAGE
}

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
cd "$repo_root"

quality_base="${MINE_TELEOP_QUALITY_BASE:-}"
compile_commands_dir=""
require_format=0
require_eslint=0
require_tidy=0

while (($# > 0)); do
  case "$1" in
    --base)
      [[ $# -ge 2 ]] || { printf '%s\n' 'missing value for --base' >&2; exit 2; }
      quality_base="$2"
      shift 2
      ;;
    --compile-commands)
      [[ $# -ge 2 ]] || { printf '%s\n' 'missing value for --compile-commands' >&2; exit 2; }
      compile_commands_dir="$2"
      shift 2
      ;;
    --require-format)
      require_format=1
      shift
      ;;
    --require-eslint)
      require_eslint=1
      shift
      ;;
    --require-tidy)
      require_tidy=1
      shift
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      printf 'unknown option: %s\n' "$1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

if [[ -z "$quality_base" ]]; then
  if git rev-parse --verify --quiet origin/main^{commit} >/dev/null; then
    quality_base=origin/main
  elif git rev-parse --verify --quiet main^{commit} >/dev/null; then
    quality_base=main
  else
    printf '%s\n' 'cannot choose a quality baseline; pass --base <git-revision>' >&2
    exit 2
  fi
fi

quality_base_input_sha="$(git rev-parse --verify "${quality_base}^{commit}")" || {
  printf 'invalid quality baseline: %s\n' "$quality_base" >&2
  exit 2
}
quality_base_sha="$(git merge-base "$quality_base_input_sha" HEAD)" || {
  printf 'quality baseline has no common ancestor with HEAD: %s\n' "$quality_base" >&2
  exit 2
}
quality_base_sha="$(git rev-parse --verify "${quality_base_sha}^{commit}")" || {
  printf 'invalid quality merge-base for: %s\n' "$quality_base" >&2
  exit 2
}

declare -a changed_files=()
append_unique_file() {
  local candidate="$1"
  local existing
  if ((${#changed_files[@]} > 0)); then
    for existing in "${changed_files[@]}"; do
      [[ "$existing" == "$candidate" ]] && return
    done
  fi
  changed_files+=("$candidate")
}
read_paths() {
  local candidate
  while IFS= read -r -d '' candidate; do
    append_unique_file "$candidate"
  done
}

read_paths < <(git diff --name-only --diff-filter=ACMR --find-renames -z "$quality_base_sha" HEAD)
read_paths < <(git diff --name-only --diff-filter=ACMR --find-renames -z)
read_paths < <(git diff --cached --name-only --diff-filter=ACMR --find-renames -z)
read_paths < <(git ls-files --others --exclude-standard -z)

is_relevant_file() {
  case "$1" in
    node_modules/*) return 1 ;;
    cpp/*|scripts/*|protocol/*|deployments/chassis-control-bridge/*|*.js|*.cjs|*.mjs) return 0 ;;
    *) return 1 ;;
  esac
}

declare -a relevant_files=()
declare -a cpp_files=()
declare -a existing_cpp_files=()
declare -a new_cpp_files=()
declare -a cpp_translation_units=()
declare -a shell_files=()
declare -a js_files=()

path_exists_at_base() {
  git cat-file -e "$quality_base_sha:$1" 2>/dev/null
}

renamed_from_base_in_diff() {
  local target="$1"
  shift
  local status old_path new_path path
  while IFS= read -r -d '' status; do
    case "$status" in
      R*)
        IFS= read -r -d '' old_path || return 1
        IFS= read -r -d '' new_path || return 1
        if [[ "$new_path" == "$target" ]] && path_exists_at_base "$old_path"; then
          return 0
        fi
        ;;
      *)
        IFS= read -r -d '' path || return 1
        ;;
    esac
  done < <(git diff --name-status --find-renames -z "$@")
  return 1
}

is_existing_at_base_or_rename() {
  local path="$1"
  path_exists_at_base "$path" && return 0
  renamed_from_base_in_diff "$path" "$quality_base_sha" HEAD && return 0
  renamed_from_base_in_diff "$path" --cached && return 0
  renamed_from_base_in_diff "$path" && return 0
  return 1
}

if ((${#changed_files[@]} > 0)); then
  for changed_file in "${changed_files[@]}"; do
    [[ -f "$changed_file" ]] || continue
    is_relevant_file "$changed_file" || continue
    relevant_files+=("$changed_file")
    case "$changed_file" in
      *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx)
        cpp_files+=("$changed_file")
        if is_existing_at_base_or_rename "$changed_file"; then
          existing_cpp_files+=("$changed_file")
        else
          new_cpp_files+=("$changed_file")
        fi
        case "$changed_file" in
          *.c|*.cc|*.cpp|*.cxx) cpp_translation_units+=("$changed_file") ;;
        esac
        ;;
      *.sh|*.bash) shell_files+=("$changed_file") ;;
      *.js|*.cjs|*.mjs) js_files+=("$changed_file") ;;
    esac
  done
fi

printf 'incremental_quality_requested_base=%s\n' "$quality_base_input_sha"
printf 'incremental_quality_base=%s\n' "$quality_base_sha"
printf 'incremental_quality_files=%s cpp=%s existing_cpp=%s new_cpp=%s shell=%s js=%s\n' \
  "${#relevant_files[@]}" "${#cpp_files[@]}" "${#existing_cpp_files[@]}" "${#new_cpp_files[@]}" "${#shell_files[@]}" "${#js_files[@]}"

if ((${#relevant_files[@]} > 0)); then
  git diff --check "$quality_base_sha" HEAD -- "${relevant_files[@]}"
  git diff --check -- "${relevant_files[@]}"
  git diff --cached --check -- "${relevant_files[@]}"
fi

if ((${#shell_files[@]} > 0)); then
  for shell_file in "${shell_files[@]}"; do
    bash -n "$shell_file"
  done
  printf 'incremental_quality_bash=passed files=%s\n' "${#shell_files[@]}"
fi

if ((${#js_files[@]} > 0)); then
  if ! command -v node >/dev/null 2>&1; then
    printf '%s\n' 'node is required for changed JavaScript files' >&2
    exit 2
  fi
  for js_file in "${js_files[@]}"; do
    node --check "$js_file"
  done
  printf 'incremental_quality_node_syntax=passed files=%s\n' "${#js_files[@]}"
fi

required_clang_tool_version="18.1.8"

choose_tool() {
  local override="$1"
  local versioned_name="$2"
  local fallback_name="$3"
  if [[ -n "$override" ]]; then
    printf '%s\n' "$override"
  elif command -v "$versioned_name" >/dev/null 2>&1; then
    printf '%s\n' "$versioned_name"
  else
    printf '%s\n' "$fallback_name"
  fi
}

require_tool_version() {
  local tool_label="$1"
  local tool_bin="$2"
  local expected_fragment="$3"
  local tool_version
  if ! command -v "$tool_bin" >/dev/null 2>&1; then
    printf '%s %s is required\n' "$tool_label" "$required_clang_tool_version" >&2
    exit 2
  fi
  tool_version="$("$tool_bin" --version)" || {
    printf '%s version detection failed: %s\n' "$tool_label" "$tool_bin" >&2
    exit 2
  }
  if [[ "$tool_version" != *"$expected_fragment"* ]]; then
    printf '%s %s is required, found: %s\n' "$tool_label" "$required_clang_tool_version" "$tool_version" >&2
    exit 2
  fi
}

emit_line_args_from_diff() {
  local target="$1"
  shift
  declare -a diff_args=()
  while (($# > 0)); do
    [[ "$1" == "--" ]] && { shift; break; }
    diff_args+=("$1")
    shift
  done
  local hunk start count end
  while IFS= read -r hunk; do
    [[ "$hunk" =~ ^@@[[:space:]]-[0-9]+(,[0-9]+)?[[:space:]]\+([0-9]+)(,([0-9]+))?[[:space:]]@@ ]] || continue
    start="${BASH_REMATCH[2]}"
    count="${BASH_REMATCH[4]:-1}"
    [[ "$count" == "0" ]] && continue
    end=$((start + count - 1))
    printf '%s\0' "--lines=$start:$end"
  done < <(git diff --unified=0 --no-ext-diff --find-renames "${diff_args[@]}" -- "$@")
}

collect_changed_line_args_from_diff() {
  local target="$1"
  shift
  local status old_path new_path path rename_seen
  rename_seen=0
  while IFS= read -r -d '' status; do
    case "$status" in
      R*)
        IFS= read -r -d '' old_path || return 1
        IFS= read -r -d '' new_path || return 1
        if [[ "$new_path" == "$target" ]]; then
          emit_line_args_from_diff "$target" "$@" -- "$old_path" "$target"
          rename_seen=1
        fi
        ;;
      *)
        IFS= read -r -d '' path || return 1
        ;;
    esac
  done < <(git diff --name-status --find-renames -z "$@")
  if ((rename_seen == 0)); then
    emit_line_args_from_diff "$target" "$@" -- "$target"
  fi
}

clang_format_bin="$(choose_tool "${MINE_TELEOP_CLANG_FORMAT:-}" clang-format-18 clang-format)"
git_clang_format_bin="$(choose_tool "${MINE_TELEOP_GIT_CLANG_FORMAT:-}" git-clang-format-18 git-clang-format)"
if ((require_format || ${#cpp_files[@]} > 0)); then
  require_tool_version "clang-format" "$clang_format_bin" "clang-format version $required_clang_tool_version"
  require_tool_version "git-clang-format" "$git_clang_format_bin" "git-clang-format version $required_clang_tool_version"
fi
if ((${#new_cpp_files[@]} > 0)); then
  "$clang_format_bin" --dry-run --Werror --style=file "${new_cpp_files[@]}"
  printf 'incremental_quality_clang_format_full=passed files=%s\n' "${#new_cpp_files[@]}"
fi
if ((${#existing_cpp_files[@]} > 0)); then
  for cpp_file in "${existing_cpp_files[@]}"; do
    declare -a line_args=()
    while IFS= read -r -d '' line_arg; do
      line_args+=("$line_arg")
    done < <({
      collect_changed_line_args_from_diff "$cpp_file" "$quality_base_sha" HEAD
      collect_changed_line_args_from_diff "$cpp_file" "$quality_base_sha"
      collect_changed_line_args_from_diff "$cpp_file"
      collect_changed_line_args_from_diff "$cpp_file" --cached
    })
    if ((${#line_args[@]} > 0)); then
      "$clang_format_bin" --dry-run --Werror --style=file "${line_args[@]}" "$cpp_file"
    fi
  done
  printf 'incremental_quality_clang_format_hunks=passed files=%s\n' "${#existing_cpp_files[@]}"
fi

eslint_bin="${MINE_TELEOP_ESLINT:-}"
if [[ -z "$eslint_bin" && -x "$repo_root/node_modules/.bin/eslint" ]]; then
  eslint_bin="$repo_root/node_modules/.bin/eslint"
elif [[ -z "$eslint_bin" ]] && command -v eslint >/dev/null 2>&1; then
  eslint_bin="eslint"
fi
if ((${#js_files[@]} > 0)); then
  if [[ -n "$eslint_bin" ]]; then
    eslint_version="$($eslint_bin --version)"
    if [[ "$eslint_version" != v9.* ]]; then
      printf 'ESLint 9 is required, found: %s\n' "$eslint_version" >&2
      exit 2
    fi
    "$eslint_bin" --config "$repo_root/.eslint.config.cjs" "${js_files[@]}"
    printf 'incremental_quality_eslint=passed files=%s\n' "${#js_files[@]}"
  elif ((require_eslint)); then
    printf '%s\n' 'ESLint 9 is required for changed JavaScript files' >&2
    exit 2
  else
    printf '%s\n' 'incremental_quality_eslint=skipped reason=eslint-9-unavailable'
  fi
fi

clang_tidy_bin="${MINE_TELEOP_CLANG_TIDY:-clang-tidy}"
if ((${#cpp_translation_units[@]} > 0)); then
  if [[ -n "$compile_commands_dir" && ! -f "$compile_commands_dir/compile_commands.json" ]]; then
    printf 'compile_commands.json not found: %s\n' "$compile_commands_dir" >&2
    exit 2
  fi
  if [[ -n "$compile_commands_dir" ]] && command -v "$clang_tidy_bin" >/dev/null 2>&1; then
    clang_tidy_version="$($clang_tidy_bin --version)"
    if [[ "$clang_tidy_version" != *"version 18"* ]]; then
      printf 'clang-tidy 18 is required, found: %s\n' "$clang_tidy_version" >&2
      exit 2
    fi
    "$clang_tidy_bin" -p "$compile_commands_dir" --config-file "$repo_root/.clang-tidy" "${cpp_translation_units[@]}"
    printf 'incremental_quality_clang_tidy=passed files=%s\n' "${#cpp_translation_units[@]}"
  elif ((require_tidy)); then
    printf '%s\n' 'clang-tidy 18 and --compile-commands are required for changed C++ translation units' >&2
    exit 2
  else
    printf '%s\n' 'incremental_quality_clang_tidy=skipped reason=compile-commands-or-clang-tidy-18-unavailable'
  fi
elif ((require_tidy)); then
  printf '%s\n' 'incremental_quality_clang_tidy=not-applicable reason=no-changed-cpp-translation-units'
fi

printf '%s\n' 'incremental_quality=passed'
