#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage:
  scripts/render_turnserver_config.sh --realm REALM --secret-file PATH --output PATH [--template PATH]
  scripts/render_turnserver_config.sh --self-test

Render the coturn template atomically with mode 0600. The shared secret is read
from a file and is never printed.
EOF
}

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"

render_config() {
  local template_path="$1"
  local output_path="$2"
  local realm="$3"
  local secret_path="$4"
  local secret output_dir temporary_path line

  [[ -f "$template_path" ]] || { printf 'coturn template does not exist: %s\n' "$template_path" >&2; return 2; }
  [[ -f "$secret_path" && -r "$secret_path" ]] || { printf 'TURN secret file is not readable: %s\n' "$secret_path" >&2; return 2; }
  [[ "$realm" =~ ^[A-Za-z0-9._-]+$ ]] || { printf 'realm contains unsupported characters\n' >&2; return 2; }
  [[ "$output_path" != "$template_path" ]] || { printf 'output must not overwrite the template\n' >&2; return 2; }

  secret="$(sed -n '1p' "$secret_path")"
  [[ -n "$secret" && "$secret" != *$'\r'* && "$secret" != *$'\n'* ]] || {
    printf 'TURN secret must be a non-empty single line\n' >&2
    return 2
  }
  if [[ -n "$(sed -n '2p' "$secret_path")" ]]; then
    printf 'TURN secret file must contain exactly one non-empty line\n' >&2
    return 2
  fi

  output_dir="$(dirname -- "$output_path")"
  [[ -d "$output_dir" ]] || { printf 'output directory does not exist: %s\n' "$output_dir" >&2; return 2; }
  umask 077
  temporary_path="$(mktemp "$output_dir/.turnserver.conf.XXXXXX")"
  trap 'rm -f "$temporary_path"' RETURN
  realm_placeholder='${MINE_TELEOP_TURN_REALM}'
  secret_placeholder='${MINE_TELEOP_TURN_STATIC_AUTH_SECRET}'
  while IFS= read -r line || [[ -n "$line" ]]; do
    line="${line//$realm_placeholder/$realm}"
    line="${line//$secret_placeholder/$secret}"
    printf '%s\n' "$line"
  done <"$template_path" >"$temporary_path"
  if grep -n '\${MINE_TELEOP_TURN_' "$temporary_path" >/dev/null; then
    printf 'rendered coturn config still contains unresolved placeholders\n' >&2
    return 2
  fi
  chmod 600 "$temporary_path"
  mv -f "$temporary_path" "$output_path"
  trap - RETURN
  printf 'turnserver_config_render=passed output=%s mode=600\n' "$output_path"
}

self_test() {
  local test_dir secret_path output_path output
  test_dir="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-turn-render.XXXXXX")"
  secret_path="$test_dir/secret"
  output_path="$test_dir/turnserver.conf"
  trap 'rm -rf "$test_dir"' RETURN
  printf '%s\n' 'self-test-shared-secret' >"$secret_path"
  output="$(render_config "$repo_root/deployments/turnserver/turnserver.conf.template" "$output_path" "teleop.self.test" "$secret_path")"
  grep -q '^realm=teleop.self.test$' "$output_path"
  grep -q '^static-auth-secret=self-test-shared-secret$' "$output_path"
  [[ "$(stat -c '%a' "$output_path" 2>/dev/null || stat -f '%Lp' "$output_path")" == "600" ]]
  if grep -q 'self-test-shared-secret' <<<"$output"; then
    printf 'turnserver renderer leaked the shared secret\n' >&2
    return 2
  fi
  printf 'turnserver_config_render_self_test=passed\n'
}

if [[ "${1:-}" == "--self-test" ]]; then
  self_test
  exit 0
fi

template_path="$repo_root/deployments/turnserver/turnserver.conf.template"
output_path=""
realm=""
secret_path=""
while (($#)); do
  case "$1" in
    --template)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      template_path="$2"
      shift 2
      ;;
    --output)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      output_path="$2"
      shift 2
      ;;
    --realm)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      realm="$2"
      shift 2
      ;;
    --secret-file)
      [[ $# -ge 2 ]] || { usage >&2; exit 2; }
      secret_path="$2"
      shift 2
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
[[ -n "$output_path" && -n "$realm" && -n "$secret_path" ]] || { usage >&2; exit 2; }
render_config "$template_path" "$output_path" "$realm" "$secret_path"
