#!/usr/bin/env bash
set -euo pipefail

if [[ "$(uname -s)" != "Linux" || "$EUID" -ne 0 ]]; then
  printf 'cloud_no_start_rollback=skipped reason=linux_root_required\n'
  exit 0
fi

repository_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
fixture_root="$(mktemp -d)"

cleanup() {
  rm -rf -- "$fixture_root"
}
trap cleanup EXIT

fail() {
  printf 'cloud no-start rollback test failed: %s\n' "$*" >&2
  exit 1
}

test_root="$fixture_root/root"
package_root="$fixture_root/package"
stub_dir="$fixture_root/bin"
systemctl_log="$fixture_root/systemctl.log"
deployment_log="$fixture_root/deployment.log"
installed_caddy="$test_root/etc/caddy/Caddyfile"
mkdir -p \
  "$stub_dir" \
  "$package_root/bin" \
  "$package_root/lib" \
  "$package_root/deployments/systemd/caddy.service.d" \
  "$package_root/deployments/systemd/haproxy.service.d" \
  "$package_root/deployments/turnserver" \
  "$package_root/scripts" \
  "$test_root/opt/mine-teleop" \
  "$test_root/etc/caddy" \
  "$test_root/var/tmp" \
  "$test_root/var/backups/mine-teleop"

sed \
  -e "s|/opt/mine-teleop|$test_root/opt/mine-teleop|g" \
  -e "s|/etc/mine-teleop|$test_root/etc/mine-teleop|g" \
  -e "s|/etc/caddy|$test_root/etc/caddy|g" \
  -e "s|/etc/haproxy|$test_root/etc/haproxy|g" \
  -e "s|/etc/systemd|$test_root/etc/systemd|g" \
  -e "s|/var/backups/mine-teleop|$test_root/var/backups/mine-teleop|g" \
  -e "s|/var/tmp/mine-teleop-cloud-candidate|$test_root/var/tmp/mine-teleop-cloud-candidate|g" \
  -e "s|/var/log/mine-teleop|$test_root/var/log/mine-teleop|g" \
  "$repository_root/scripts/deploy/deploy_cloud_bundle.sh" >"$package_root/deploy-cloud.sh"
chmod 0755 "$package_root/deploy-cloud.sh"

cat >"$package_root/lib/ld-linux-x86-64.so.2" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "--library-path" ]]; then shift 2; fi
exec "$@"
EOF
cat >"$package_root/bin/mine-teleop-signaling-server" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exit 0
EOF
cat >"$package_root/scripts/render_turnserver_config.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exit 0
EOF
chmod 0755 \
  "$package_root/lib/ld-linux-x86-64.so.2" \
  "$package_root/bin/mine-teleop-signaling-server" \
  "$package_root/scripts/render_turnserver_config.sh"

for unit in \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  mine-teleop-cloud.target; do
  printf '[Unit]\nDescription=fixture\n' >"$package_root/deployments/systemd/$unit"
done
printf '[Service]\n' >"$package_root/deployments/systemd/caddy.service.d/mine-teleop-cloud.conf"
printf '[Service]\n' >"$package_root/deployments/systemd/haproxy.service.d/mine-teleop-cloud.conf"
printf 'fixture\n' >"$package_root/deployments/turnserver/turnserver.conf.template"

cat >"$stub_dir/systemctl" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
printf '%s\n' "$*" >>"$MINE_TELEOP_TEST_SYSTEMCTL_LOG"
case "${1:-}" in
  is-active|is-enabled) exit 0 ;;
  *) exit 0 ;;
esac
EOF
cat >"$stub_dir/caddy" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
config=""
while (($#)); do
  case "$1" in
    --config)
      config="$2"
      shift 2
      ;;
    *) shift ;;
  esac
done
if [[ "$config" == "$MINE_TELEOP_TEST_INSTALLED_CADDY" ]]; then exit 41; fi
exit 0
EOF
cat >"$stub_dir/uname" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
case "${1:-}" in
  -s) printf 'Linux\n' ;;
  -m) printf 'x86_64\n' ;;
  *) printf 'Linux\n' ;;
esac
EOF
for command_name in curl haproxy turnserver; do
  cat >"$stub_dir/$command_name" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exit 0
EOF
done
chmod 0755 "$stub_dir"/*

printf 'old-application\n' >"$test_root/opt/mine-teleop/old-marker"
printf 'old-caddy\n' >"$installed_caddy"
candidate_caddy="$fixture_root/candidate.Caddyfile"
printf 'candidate-caddy\n' >"$candidate_caddy"
: >"$systemctl_log"

if PATH="$stub_dir:$PATH" \
  MINE_TELEOP_TEST_SYSTEMCTL_LOG="$systemctl_log" \
  MINE_TELEOP_TEST_INSTALLED_CADDY="$installed_caddy" \
  bash "$package_root/deploy-cloud.sh" \
    --skip-package-install \
    --no-start \
    --caddy-config "$candidate_caddy" >"$deployment_log" 2>&1; then
  fail "installed Caddy validation failure unexpectedly committed"
fi

grep -q '^old-application$' "$test_root/opt/mine-teleop/old-marker" ||
  fail "application bundle was not restored"
grep -q '^old-caddy$' "$installed_caddy" || fail "Caddy configuration was not restored"
if grep -Eq '^(stop|start|restart|enable|disable)( |$)' "$systemctl_log"; then
  fail "--no-start rollback changed service state: $(tr '\n' ';' <"$systemctl_log")"
fi
grep -Fxq 'daemon-reload' "$systemctl_log" ||
  fail "restored unit files were not reloaded: $(tr '\n' ';' <"$deployment_log")"

printf 'cloud_no_start_rollback=passed\n'
