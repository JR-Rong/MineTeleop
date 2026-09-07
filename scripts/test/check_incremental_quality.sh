#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage: scripts/test/check_incremental_quality.sh [options]

Checks only changed or new relevant files after --base, plus staged, unstaged,
and untracked changes. It never rewrites files.

Options:
  --base <git-revision>       Compare against this revision. Defaults to
                              MINE_TELEOP_QUALITY_BASE, origin/main, or main.
  --compile-commands <path>   Directory containing compile_commands.json for
                              bounded clang-tidy checks of changed C++ TUs.
  --require-format            Fail when clang-format 18 is unavailable.
  --require-eslint            Fail when ESLint 9 is unavailable for changed JS.
  --require-tidy              Fail unless --compile-commands and clang-tidy 18
                              are available for changed C++ translation units.
  --help                      Show this help.

Tool paths can be overridden with MINE_TELEOP_CLANG_FORMAT,
MINE_TELEOP_CLANG_TIDY, and MINE_TELEOP_ESLINT. Run with --base set to the
current PR head to keep an incremental PR check focused on new work.
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

quality_base_sha="$(git rev-parse --verify "${quality_base}^{commit}")" || {
  printf 'invalid quality baseline: %s\n' "$quality_base" >&2
  exit 2
}
if ! git merge-base --is-ancestor "$quality_base_sha" HEAD; then
  printf 'quality baseline is not an ancestor of HEAD: %s\n' "$quality_base" >&2
  exit 2
fi

declare -a changed_files=()
append_unique_file() {
  local candidate="$1"
  local existing
  for existing in "${changed_files[@]-}"; do
    [[ "$existing" == "$candidate" ]] && return
  done
  changed_files+=("$candidate")
}
read_paths() {
  local candidate
  while IFS= read -r -d '' candidate; do
    append_unique_file "$candidate"
  done
}

read_paths < <(git diff --name-only --diff-filter=ACMR -z "$quality_base_sha...HEAD")
read_paths < <(git diff --name-only --diff-filter=ACMR -z)
read_paths < <(git diff --cached --name-only --diff-filter=ACMR -z)
read_paths < <(git ls-files --others --exclude-standard -z)

is_relevant_file() {
  case "$1" in
    cpp/*|scripts/*|protocol/*|deployments/chassis-control-bridge/*|*.js|*.cjs|*.mjs) return 0 ;;
    *) return 1 ;;
  esac
}

declare -a relevant_files=()
declare -a cpp_files=()
declare -a cpp_translation_units=()
declare -a shell_files=()
declare -a js_files=()
for changed_file in "${changed_files[@]-}"; do
  [[ -f "$changed_file" ]] || continue
  is_relevant_file "$changed_file" || continue
  relevant_files+=("$changed_file")
  case "$changed_file" in
    *.c|*.cc|*.cpp|*.cxx|*.h|*.hh|*.hpp|*.hxx)
      cpp_files+=("$changed_file")
      case "$changed_file" in
        *.c|*.cc|*.cpp|*.cxx) cpp_translation_units+=("$changed_file") ;;
      esac
      ;;
    *.sh|*.bash) shell_files+=("$changed_file") ;;
    *.js|*.cjs|*.mjs) js_files+=("$changed_file") ;;
  esac
done

printf 'incremental_quality_base=%s\n' "$quality_base_sha"
printf 'incremental_quality_files=%s cpp=%s shell=%s js=%s\n' "${#relevant_files[@]}" "${#cpp_files[@]}" "${#shell_files[@]}" "${#js_files[@]}"

if ((${#relevant_files[@]} > 0)); then
  git diff --check "$quality_base_sha...HEAD" -- "${relevant_files[@]}"
  git diff --check -- "${relevant_files[@]}"
  git diff --cached --check -- "${relevant_files[@]}"
fi

for shell_file in "${shell_files[@]-}"; do
  bash -n "$shell_file"
done
if ((${#shell_files[@]} > 0)); then
  printf 'incremental_quality_bash=passed files=%s\n' "${#shell_files[@]}"
fi

if ((${#js_files[@]} > 0)); then
  if ! command -v node >/dev/null 2>&1; then
    printf '%s\n' 'node is required for changed JavaScript files' >&2
    exit 2
  fi
  for js_file in "${js_files[@]-}"; do
    node --check "$js_file"
  done
  printf 'incremental_quality_node_syntax=passed files=%s\n' "${#js_files[@]}"
fi

clang_format_bin="${MINE_TELEOP_CLANG_FORMAT:-clang-format}"
if ((${#cpp_files[@]} > 0)); then
  if command -v "$clang_format_bin" >/dev/null 2>&1; then
    clang_format_version="$($clang_format_bin --version)"
    if [[ "$clang_format_version" != *"version 18"* ]]; then
      printf 'clang-format 18 is required, found: %s\n' "$clang_format_version" >&2
      exit 2
    fi
    "$clang_format_bin" --dry-run --Werror --style=file "${cpp_files[@]}"
    printf 'incremental_quality_clang_format=passed files=%s\n' "${#cpp_files[@]}"
  elif ((require_format)); then
    printf '%s\n' 'clang-format 18 is required for changed C++ files' >&2
    exit 2
  else
    printf '%s\n' 'incremental_quality_clang_format=skipped reason=clang-format-18-unavailable'
  fi
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
