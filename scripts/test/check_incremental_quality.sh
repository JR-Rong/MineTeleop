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
  --require-format            Fail when clang-format 18.1.8 or its paired
                              git-clang-format wrapper is unavailable.
  --require-eslint            Fail when ESLint 9 is unavailable for changed JS.
  --require-tidy              Fail unless --compile-commands and clang-tidy 18
                              are available for changed C++ translation units.
  --help                      Show this help.

Tool paths can be overridden with MINE_TELEOP_CLANG_FORMAT,
MINE_TELEOP_GIT_CLANG_FORMAT, MINE_TELEOP_GIT_CLANG_FORMAT_VERSION,
MINE_TELEOP_CLANG_TIDY, and MINE_TELEOP_ESLINT.
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
declare -a untracked_cpp_files=()
declare -a cpp_translation_units=()
declare -a shell_files=()
declare -a js_files=()

is_untracked_file() {
  local candidate
  while IFS= read -r -d '' candidate; do
    [[ "$candidate" == "$1" ]] && return 0
  done < <(git ls-files --others --exclude-standard -z -- "$1")
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
        is_untracked_file "$changed_file" && untracked_cpp_files+=("$changed_file")
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
printf 'incremental_quality_files=%s cpp=%s untracked_cpp=%s shell=%s js=%s\n' \
  "${#relevant_files[@]}" "${#cpp_files[@]}" "${#untracked_cpp_files[@]}" "${#shell_files[@]}" "${#js_files[@]}"

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

require_git_clang_format_wrapper() {
  local tool_bin="$1"
  local tool_help
  if ! command -v "$tool_bin" >/dev/null 2>&1; then
    printf 'git-clang-format %s is required\n' "$required_clang_tool_version" >&2
    exit 2
  fi
  tool_help="$("$tool_bin" --help 2>&1)" || {
    printf 'git-clang-format help check failed: %s\n' "$tool_bin" >&2
    exit 2
  }
  if [[ "$tool_help" != *'usage: git clang-format'* ]]; then
    printf 'git-clang-format wrapper is not recognized: %s\n' "$tool_bin" >&2
    exit 2
  fi
  if [[ -n "${MINE_TELEOP_GIT_CLANG_FORMAT_VERSION:-}" ]]; then
    if [[ "$MINE_TELEOP_GIT_CLANG_FORMAT_VERSION" != "$required_clang_tool_version" ]]; then
      printf 'git-clang-format %s is required, found verified version: %s\n' \
        "$required_clang_tool_version" "$MINE_TELEOP_GIT_CLANG_FORMAT_VERSION" >&2
      exit 2
    fi
  elif [[ -n "${MINE_TELEOP_GIT_CLANG_FORMAT:-}" ]]; then
    printf '%s\n' \
      'MINE_TELEOP_GIT_CLANG_FORMAT_VERSION is required when overriding git-clang-format' >&2
    exit 2
  fi
}

clang_format_bin="$(choose_tool "${MINE_TELEOP_CLANG_FORMAT:-}" clang-format-18 clang-format)"
git_clang_format_bin="$(choose_tool "${MINE_TELEOP_GIT_CLANG_FORMAT:-}" git-clang-format-18 git-clang-format)"
if ((require_format || ${#cpp_files[@]} > 0)); then
  require_tool_version "clang-format" "$clang_format_bin" "clang-format version $required_clang_tool_version"
  require_git_clang_format_wrapper "$git_clang_format_bin"
fi

# LLVM's fixed git-clang-format wrapper parses unified diffs line-by-line.
# A C/C++ pathname with a line break therefore cannot be represented safely by
# that wrapper; fail explicitly instead of silently dropping it from a gate.
ensure_git_clang_format_paths_are_supported() {
  local cpp_file
  for cpp_file in "${cpp_files[@]}"; do
    if [[ "$cpp_file" == *$'\n'* || "$cpp_file" == *$'\r'* ]]; then
      printf 'git-clang-format cannot safely check C/C++ path with a newline: %q\n' "$cpp_file" >&2
      return 2
    fi
  done
}

run_git_clang_format_diff() {
  local scope="$1"
  shift
  local output status existing_config_count
  output="$(mktemp "${TMPDIR:-/tmp}/mine-teleop-git-clang-format.XXXXXX")"
  existing_config_count="${GIT_CONFIG_COUNT:-0}"
  if env \
    "GIT_CONFIG_COUNT=$((existing_config_count + 1))" \
    "GIT_CONFIG_KEY_${existing_config_count}=diff.renames" \
    "GIT_CONFIG_VALUE_${existing_config_count}=true" \
    "$git_clang_format_bin" --diff --binary "$clang_format_bin" \
      --extensions c,cc,cpp,cxx,h,hh,hpp,hxx --style=file "$@" >"$output" 2>&1; then
    rm -f -- "$output"
    printf 'incremental_quality_git_clang_format=passed scope=%s\n' "$scope"
    return 0
  else
    status=$?
  fi
  printf 'incremental_quality_git_clang_format=failed scope=%s status=%s\n' "$scope" "$status" >&2
  cat -- "$output" >&2
  rm -f -- "$output"
  if ((status == 1)); then
    return 1
  fi
  return 2
}

if ((${#cpp_files[@]} > 0)); then
  ensure_git_clang_format_paths_are_supported
  if ! git diff --quiet "$quality_base_sha" HEAD -- "${cpp_files[@]}"; then
    run_git_clang_format_diff committed "$quality_base_sha" HEAD -- "${cpp_files[@]}"
  fi
  if ! git diff --cached --quiet HEAD -- "${cpp_files[@]}"; then
    run_git_clang_format_diff staged --staged -- "${cpp_files[@]}"
  fi
  if ! git diff --quiet -- "${cpp_files[@]}"; then
    run_git_clang_format_diff worktree HEAD -- "${cpp_files[@]}"
  fi
fi
if ((${#untracked_cpp_files[@]} > 0)); then
  "$clang_format_bin" --dry-run --Werror --style=file "${untracked_cpp_files[@]}"
  printf 'incremental_quality_clang_format_untracked_full=passed files=%s\n' \
    "${#untracked_cpp_files[@]}"
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
