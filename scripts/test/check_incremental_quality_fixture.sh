#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
tmp_dir="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-quality-fixture.XXXXXX")"
trap 'rm -rf "$tmp_dir"' EXIT

fixture_repo="$tmp_dir/repo"
tool_dir="$tmp_dir/tools"
tool_log="$tmp_dir/tool.log"
mkdir -p "$fixture_repo/scripts/test" "$tool_dir"
cp "$repo_root/scripts/test/check_incremental_quality.sh" "$fixture_repo/scripts/test/check_incremental_quality.sh"
chmod 0755 "$fixture_repo/scripts/test/check_incremental_quality.sh"

cat > "$tool_dir/clang-format" <<'TOOL'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--version" ]]; then
  printf '%s\n' 'clang-format version 18.1.8'
  exit 0
fi
declare -a path_args=()
for arg in "$@"; do
  case "$arg" in
    --*) ;;
    *) path_args+=("$arg") ;;
  esac
done
{
  printf '%s' 'clang-format'
  for arg in "$@"; do
    printf ' <%q>' "$arg"
  done
  printf '\n'
} >> "${MINE_TELEOP_FAKE_TOOL_LOG:?}"
for path_arg in "${path_args[@]}"; do
  case "$path_arg" in
    *"untracked new.cpp")
      if [[ " $* " != *' --dry-run '* || " $* " != *' --Werror '* ]]; then
        printf 'expected full-file dry-run clang-format for %s\n' "$path_arg" >&2
        exit 1
      fi
      ;;
  esac
done
TOOL

cat > "$tool_dir/git-clang-format" <<'TOOL'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--version" ]]; then
  printf '%s\n' 'git-clang-format version 18.1.8'
  exit 0
fi
{
  printf '%s' 'git-clang-format'
  for arg in "$@"; do
    printf ' <%q>' "$arg"
  done
  printf '\n'
} >> "${MINE_TELEOP_FAKE_TOOL_LOG:?}"
if [[ "${MINE_TELEOP_FAKE_GIT_CLANG_FORMAT_STATUS:-0}" != 0 ]]; then
  printf '%s\n' '--- a/cpp/existing.cpp'
  printf '%s\n' '+++ b/cpp/existing.cpp'
  exit "${MINE_TELEOP_FAKE_GIT_CLANG_FORMAT_STATUS}"
fi
TOOL
chmod 0755 "$tool_dir/clang-format" "$tool_dir/git-clang-format"

cat > "$tool_dir/git-clang-format-bad" <<'TOOL'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' 'git-clang-format version 17.0.6'
TOOL
chmod 0755 "$tool_dir/git-clang-format-bad"

cd "$fixture_repo"
git init -q
git config user.email "fixture@example.invalid"
git config user.name "Incremental Quality Fixture"

mkdir -p cpp
printf '%s\n' \
  'int existing() {' \
  '  return 1;' \
  '}' > cpp/existing.cpp
printf '%s\n' \
  'int renamed() {' \
  '  return 1;' \
  '}' > "cpp/rename old.cpp"
printf '%s\n' \
  'int removed() {' \
  '  return 1;' \
  '}' > cpp/delete.cpp
printf 'int crlf() {\r\n  return 1;\r\n}\r\n' > cpp/crlf.cpp
git add scripts/test/check_incremental_quality.sh cpp
git commit -q -m base
base_sha="$(git rev-parse HEAD)"

git checkout -q -b feature
printf '%s\n' \
  'int existing() {' \
  '  return 2;' \
  '}' > cpp/existing.cpp
git mv "cpp/rename old.cpp" "cpp/rename new.cpp"
printf '%s\n' \
  'int renamed() {' \
  '  return 2;' \
  '}' > "cpp/rename new.cpp"
rm cpp/delete.cpp
printf 'int crlf() {\r\n  return 2;\r\n}\r\n' > cpp/crlf.cpp
new_special_file='cpp/new special.cpp'
printf '%s\n' \
  'int special() {' \
  '  return 3;' \
  '}' > "$new_special_file"
git add cpp
git commit -q -m feature

git checkout -q -b target-main "$base_sha"
mkdir -p scripts
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf main-advanced\\n' > scripts/main-advanced.sh
git add scripts/main-advanced.sh
git commit -q -m "advance main"
main_sha="$(git rev-parse HEAD)"

git checkout -q feature
printf '%s\n' \
  'int existing() {' \
  '  return 4;' \
  '}' > cpp/existing.cpp
printf '%s\n' \
  'int staged_new() {' \
  '  return 5;' \
  '}' > "cpp/staged new.cpp"
git add "cpp/staged new.cpp"
printf '%s\n' \
  'int untracked_new() {' \
  '  return 6;' \
  '}' > "cpp/untracked new.cpp"

before_status="$(git status --porcelain=v1 -z | od -An -tx1 | tr -d ' \n')"
output="$(
  MINE_TELEOP_CLANG_FORMAT="$tool_dir/clang-format" \
  MINE_TELEOP_GIT_CLANG_FORMAT="$tool_dir/git-clang-format" \
  MINE_TELEOP_FAKE_TOOL_LOG="$tool_log" \
  bash scripts/test/check_incremental_quality.sh --base "$main_sha" 2>&1
)"
after_status="$(git status --porcelain=v1 -z | od -An -tx1 | tr -d ' \n')"

if [[ "$before_status" != "$after_status" ]]; then
  printf '%s\n' 'incremental quality fixture changed worktree or index' >&2
  exit 1
fi
grep -F "incremental_quality_requested_base=$main_sha" <<< "$output" >/dev/null
grep -F "incremental_quality_base=$base_sha" <<< "$output" >/dev/null
grep -F 'cpp=6 untracked_cpp=1' <<< "$output" >/dev/null
grep -F 'incremental_quality_git_clang_format=passed scope=committed' <<< "$output" >/dev/null
grep -F 'incremental_quality_git_clang_format=passed scope=staged' <<< "$output" >/dev/null
grep -F 'incremental_quality_git_clang_format=passed scope=worktree' <<< "$output" >/dev/null
grep -F 'incremental_quality_clang_format_untracked_full=passed files=1' <<< "$output" >/dev/null
grep -F 'git-clang-format <--diff> <--binary>' "$tool_log" >/dev/null
grep -F "<$base_sha> <HEAD>" "$tool_log" >/dev/null
grep -F '<--staged>' "$tool_log" >/dev/null
grep -F 'cpp/rename\ new.cpp' "$tool_log" >/dev/null
grep -F 'cpp/crlf.cpp' "$tool_log" >/dev/null
grep -F 'cpp/staged\ new.cpp' "$tool_log" >/dev/null
grep -F 'cpp/untracked\ new.cpp' "$tool_log" >/dev/null

format_failure_log="$tmp_dir/format-failure.log"
if MINE_TELEOP_CLANG_FORMAT="$tool_dir/clang-format" \
  MINE_TELEOP_GIT_CLANG_FORMAT="$tool_dir/git-clang-format" \
  MINE_TELEOP_FAKE_TOOL_LOG="$tool_log" \
  MINE_TELEOP_FAKE_GIT_CLANG_FORMAT_STATUS=1 \
  bash scripts/test/check_incremental_quality.sh --base "$main_sha" >"$format_failure_log" 2>&1; then
  printf '%s\n' 'incremental quality fixture accepted a git-clang-format diff' >&2
  exit 1
fi
grep -F 'incremental_quality_git_clang_format=failed scope=committed status=1' \
  "$format_failure_log" >/dev/null
grep -F -- '--- a/cpp/existing.cpp' "$format_failure_log" >/dev/null

newline_cpp=$'cpp/new\nname.cpp'
printf '%s\n' 'int newline_path() { return 7; }' > "$newline_cpp"
newline_failure_log="$tmp_dir/newline-failure.log"
if MINE_TELEOP_CLANG_FORMAT="$tool_dir/clang-format" \
  MINE_TELEOP_GIT_CLANG_FORMAT="$tool_dir/git-clang-format" \
  MINE_TELEOP_FAKE_TOOL_LOG="$tool_log" \
  bash scripts/test/check_incremental_quality.sh --base "$main_sha" >"$newline_failure_log" 2>&1; then
  printf '%s\n' 'incremental quality fixture accepted a newline C++ path' >&2
  exit 1
fi
grep -F 'git-clang-format cannot safely check C/C++ path with a newline' \
  "$newline_failure_log" >/dev/null
rm -- "$newline_cpp"

unrelated_tree="$(git mktree </dev/null)"
unrelated_sha="$(printf '%s\n' unrelated | git commit-tree "$unrelated_tree")"
if MINE_TELEOP_CLANG_FORMAT="$tool_dir/clang-format" \
  MINE_TELEOP_GIT_CLANG_FORMAT="$tool_dir/git-clang-format" \
  MINE_TELEOP_FAKE_TOOL_LOG="$tool_log" \
  bash scripts/test/check_incremental_quality.sh --base "$unrelated_sha" >/dev/null 2>&1; then
  printf '%s\n' 'incremental quality fixture accepted unrelated baseline' >&2
  exit 1
fi

if MINE_TELEOP_CLANG_FORMAT="$tool_dir/clang-format" \
  MINE_TELEOP_GIT_CLANG_FORMAT="$tool_dir/git-clang-format-bad" \
  MINE_TELEOP_FAKE_TOOL_LOG="$tool_log" \
  bash scripts/test/check_incremental_quality.sh --base "$main_sha" >/dev/null 2>&1; then
  printf '%s\n' 'incremental quality fixture accepted wrong git-clang-format version' >&2
  exit 1
fi

printf '%s\n' 'incremental_quality_fixture=passed'
