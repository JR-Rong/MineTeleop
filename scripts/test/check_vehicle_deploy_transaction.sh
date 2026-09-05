#!/usr/bin/env bash
set -euo pipefail

repository_root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
launcher_under_test="${1:-}"
fixture_root="$(mktemp -d)"

cleanup() {
  rm -rf -- "$fixture_root"
}
trap cleanup EXIT

fail() {
  printf 'vehicle deploy transaction test failed: %s\n' "$*" >&2
  exit 1
}

if [[ -n "$launcher_under_test" && ! -x "$launcher_under_test" ]]; then
  fail "launcher is not executable: $launcher_under_test"
fi

stub_dir="$fixture_root/bin"
package_parent="$fixture_root/package"
package_root="$package_parent/mine-teleop-fixture"
remote_dir="$fixture_root/remote/mine-teleop"
remote_archive="$fixture_root/remote/mine-teleop.tar.gz"
bundle="$fixture_root/mine-teleop-fixture.tar.gz"
mkdir -p \
  "$stub_dir" \
  "$package_root/bin" \
  "$package_root/config" \
  "$package_root/lib/vendor/chassis" \
  "$remote_dir/bin" \
  "$remote_dir/lib" \
  "$remote_dir/config" \
  "$(dirname -- "$remote_archive")"

cat >"$package_root/bin/mine-teleop" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
command_name="${1:-}"
shift || true
case "$command_name" in
  version)
    printf 'mine-teleop fixture\n'
    ;;
  config-check)
    config=""
    verify_ca="false"
    while (($#)); do
      case "$1" in
        --config)
          config="$2"
          shift 2
          ;;
        --chassis-bridge-library)
          shift 2
          ;;
        --verify-configured-ca-bundle)
          verify_ca="true"
          shift
          ;;
        *) shift ;;
      esac
    done
    [[ -n "$config" && -s "$config" ]] || exit 9
    ! grep -q 'INVALID_CANDIDATE' "$config" || exit 10
    if [[ "$config" == */config/vehicle-agent.yaml ]] &&
      grep -q 'FAIL_FINAL_CHECK' "$config"; then
      exit 12
    fi
    if [[ "$verify_ca" == "true" ]]; then
      ca_bundle="$(sed -n 's/^[[:space:]]*ca_bundle:[[:space:]]*//p' "$config" | head -n 1)"
      ca_bundle="${ca_bundle%\"}"
      ca_bundle="${ca_bundle#\"}"
      if [[ -n "$ca_bundle" ]]; then
        if [[ "$ca_bundle" != /* ]]; then
          ca_bundle="$(dirname -- "$config")/$ca_bundle"
        fi
        [[ -s "$ca_bundle" ]] || exit 13
      fi
    fi
    printf '{"chassis_bridge_abi":{"passed":true,"version":6}}\n'
    ;;
  vehicle-runtime)
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
    [[ -n "$config" ]] || exit 14
    config_dir="$(dirname -- "$config")"
    ca_bundle="$(sed -n 's/^[[:space:]]*ca_bundle:[[:space:]]*//p' "$config" | head -n 1)"
    ca_bundle="${ca_bundle%\"}"
    ca_bundle="${ca_bundle#\"}"
    ca_found="false"
    if [[ -n "$ca_bundle" ]]; then
      [[ "$ca_bundle" == /* ]] || ca_bundle="$config_dir/$ca_bundle"
      [[ -s "$ca_bundle" ]] && ca_found="true"
    fi
    if [[ -n "${MINE_TELEOP_TEST_RUNTIME_CAPTURE:-}" ]]; then
      {
        printf 'command=vehicle-runtime\n'
        printf 'config=%s\n' "$config"
        printf 'working_directory=%s\n' "$PWD"
        [[ -s "$config_dir/device-token" ]] &&
          printf 'default_token_found=true\n' || printf 'default_token_found=false\n'
        printf 'default_ca_found=%s\n' "$ca_found"
      } >"$MINE_TELEOP_TEST_RUNTIME_CAPTURE"
    fi
    ;;
  *) exit 11 ;;
esac
EOF
chmod 0755 "$package_root/bin/mine-teleop"

if [[ -n "$launcher_under_test" ]]; then
  cp "$launcher_under_test" "$package_root/bin/mine-teleop-run"
else
  cat >"$package_root/bin/mine-teleop-run" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
exec "$root/bin/mine-teleop" "$@"
EOF
fi
chmod 0755 "$package_root/bin/mine-teleop-run"
cp "$package_root/bin/mine-teleop" "$package_root/bin/vainfo"
chmod 0755 "$package_root/bin/vainfo"
install -m 0755 /usr/bin/true "$package_root/lib/ld-linux-x86-64.so.2"
printf 'fixture bridge\n' >"$package_root/lib/vendor/chassis/libmine_teleop_chassis_bridge.so"
cat >"$package_root/config/vehicle-agent.yaml" <<'EOF'
bundle_default: true
cloud:
  ca_bundle: mine-teleop-field-root.crt
EOF
printf 'bundle-managed-ca\n' >"$package_root/config/mine-teleop-field-root.crt"
printf 'bundle-system-ca\n' >"$package_root/config/ca-certificates.crt"
tar -czf "$bundle" -C "$package_parent" "$(basename -- "$package_root")"

cat >"$stub_dir/scp" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
while (($# > 2)); do shift; done
source_path="$1"
destination="${2#*:}"
mkdir -p "$(dirname -- "$destination")"
cp "$source_path" "$destination"
EOF
cat >"$stub_dir/ssh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
exec bash -s
EOF
cat >"$stub_dir/mv" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "-Tf" ]]; then
  source_path="$2"
  destination="$3"
  rm -f -- "$destination"
  exec /bin/mv -f "$source_path" "$destination"
fi
exec /bin/mv "$@"
EOF
chmod 0755 "$stub_dir/mv" "$stub_dir/scp" "$stub_dir/ssh"

printf 'legacy launcher\n' >"$remote_dir/bin/mine-teleop-run"
printf 'legacy library\n' >"$remote_dir/lib/liblegacy.so"
cat >"$remote_dir/config/vehicle-agent.yaml" <<'EOF'
site_calibration: preserve-me
cloud:
  ca_bundle: mine-teleop-field-root.crt
EOF
printf 'site-token-preserve-me\n' >"$remote_dir/config/device-token"
printf 'site-custom-ca\n' >"$remote_dir/config/mine-teleop-field-root.crt"
printf 'INVALID_CANDIDATE: stale-override-must-not-win\n' >"$remote_archive.vehicle-agent.yaml"

PATH="$stub_dir:$PATH" bash "$repository_root/scripts/deploy/deploy_vehicle_bundle.sh" \
  --bundle "$bundle" \
  --host fixture \
  --user fixture \
  --remote-dir "$remote_dir" \
  --remote-archive "$remote_archive" \
  --media-frames 0 >/dev/null

grep -q 'site_calibration: preserve-me' "$remote_dir/config/vehicle-agent.yaml" ||
  fail "upgrade without --config overwrote the persistent site configuration"
grep -q 'site-token-preserve-me' "$remote_dir/config/device-token" ||
  fail "upgrade without --device-token-file overwrote the persistent device token"
grep -q 'site-custom-ca' "$remote_dir/config/mine-teleop-field-root.crt" ||
  fail "upgrade overwrote the persistent site CA"
[[ -L "$remote_dir/current" ]] || fail "successful deployment did not publish current"
[[ -L "$remote_dir/bin" && "$(readlink "$remote_dir/bin")" == "current/bin" ]] ||
  fail "successful deployment did not migrate the stable bin entry"
[[ -L "$remote_dir/lib" && "$(readlink "$remote_dir/lib")" == "current/lib" ]] ||
  fail "successful deployment did not migrate the stable lib entry"
first_release="$(readlink "$remote_dir/current")"
[[ -d "$first_release" ]] || fail "current release symlink target is missing"

if [[ -n "$launcher_under_test" ]]; then
  launcher_capture="$fixture_root/launcher-result.log"
  MINE_TELEOP_TEST_RUNTIME_CAPTURE="$launcher_capture" \
    MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH="$fixture_root/vehicle-runtime.log" \
    "$remote_dir/bin/mine-teleop-run" >/dev/null 2>&1
  grep -Fxq "config=$remote_dir/config/vehicle-agent.yaml" "$launcher_capture" ||
    fail "real launcher did not select the persistent vehicle config"
  grep -Fxq "working_directory=$remote_dir" "$launcher_capture" ||
    fail "real launcher ran mutable runtime state inside the release directory"
  grep -Fxq 'default_token_found=true' "$launcher_capture" ||
    fail "real launcher did not expose the persistent token beside its default config"
  grep -Fxq 'default_ca_found=true' "$launcher_capture" ||
    fail "real launcher default config did not resolve the persistent CA"
fi

fresh_remote="$fixture_root/fresh/mine-teleop"
fresh_archive="$fixture_root/fresh/mine-teleop.tar.gz"
PATH="$stub_dir:$PATH" bash "$repository_root/scripts/deploy/deploy_vehicle_bundle.sh" \
  --bundle "$bundle" \
  --host fixture \
  --user fixture \
  --remote-dir "$fresh_remote" \
  --remote-archive "$fresh_archive" \
  --media-frames 0 >/dev/null
grep -q 'bundle-managed-ca' "$fresh_remote/config/mine-teleop-field-root.crt" ||
  fail "fresh deployment omitted the CA referenced by the persistent YAML"
[[ -L "$fresh_remote/bin" && -L "$fresh_remote/lib" ]] ||
  fail "fresh deployment omitted stable bin/lib entries"

final_failure_config="$fixture_root/final-failure-vehicle.yaml"
replacement_token="$fixture_root/replacement-device-token"
cat >"$final_failure_config" <<'EOF'
FAIL_FINAL_CHECK: true
cloud:
  ca_bundle: mine-teleop-field-root.crt
EOF
printf 'replacement-token-must-not-publish\n' >"$replacement_token"
if PATH="$stub_dir:$PATH" bash "$repository_root/scripts/deploy/deploy_vehicle_bundle.sh" \
  --bundle "$bundle" \
  --config "$final_failure_config" \
  --device-token-file "$replacement_token" \
  --host fixture \
  --user fixture \
  --remote-dir "$remote_dir" \
  --remote-archive "$remote_archive" \
  --media-frames 0 >/dev/null 2>&1; then
  fail "post-activation config failure unexpectedly committed"
fi
[[ "$(readlink "$remote_dir/current")" == "$first_release" ]] ||
  fail "post-activation failure did not restore the active release"
grep -q 'site_calibration: preserve-me' "$remote_dir/config/vehicle-agent.yaml" ||
  fail "post-activation failure did not restore the persistent config"
grep -q 'site-token-preserve-me' "$remote_dir/config/device-token" ||
  fail "post-activation failure did not restore the persistent token"

legacy_failure_remote="$fixture_root/legacy-failure/mine-teleop"
legacy_failure_archive="$fixture_root/legacy-failure/mine-teleop.tar.gz"
mkdir -p \
  "$legacy_failure_remote/bin" \
  "$legacy_failure_remote/lib" \
  "$legacy_failure_remote/config"
printf 'legacy-bin-survives\n' >"$legacy_failure_remote/bin/legacy-marker"
printf 'legacy-lib-survives\n' >"$legacy_failure_remote/lib/legacy-marker"
printf 'legacy-config-survives\n' >"$legacy_failure_remote/config/vehicle-agent.yaml"
printf 'legacy-token-survives\n' >"$legacy_failure_remote/config/device-token"
if PATH="$stub_dir:$PATH" bash "$repository_root/scripts/deploy/deploy_vehicle_bundle.sh" \
  --bundle "$bundle" \
  --config "$final_failure_config" \
  --device-token-file "$replacement_token" \
  --host fixture \
  --user fixture \
  --remote-dir "$legacy_failure_remote" \
  --remote-archive "$legacy_failure_archive" \
  --media-frames 0 >/dev/null 2>&1; then
  fail "first migration with a post-activation failure unexpectedly committed"
fi
[[ -d "$legacy_failure_remote/bin" && ! -L "$legacy_failure_remote/bin" &&
    -f "$legacy_failure_remote/bin/legacy-marker" ]] ||
  fail "failed first migration did not restore the legacy bin directory"
[[ -d "$legacy_failure_remote/lib" && ! -L "$legacy_failure_remote/lib" &&
    -f "$legacy_failure_remote/lib/legacy-marker" ]] ||
  fail "failed first migration did not restore the legacy lib directory"
[[ ! -e "$legacy_failure_remote/current" && ! -L "$legacy_failure_remote/current" ]] ||
  fail "failed first migration left an active release"
grep -q 'legacy-config-survives' "$legacy_failure_remote/config/vehicle-agent.yaml" ||
  fail "failed first migration did not restore the site config"
grep -q 'legacy-token-survives' "$legacy_failure_remote/config/device-token" ||
  fail "failed first migration did not restore the site token"
[[ ! -e "$legacy_failure_remote/config/mine-teleop-field-root.crt" &&
    ! -e "$legacy_failure_remote/config/ca-certificates.crt" ]] ||
  fail "failed first migration left newly published managed CA files"

invalid_config="$fixture_root/invalid-vehicle.yaml"
printf 'INVALID_CANDIDATE: true\n' >"$invalid_config"
if PATH="$stub_dir:$PATH" bash "$repository_root/scripts/deploy/deploy_vehicle_bundle.sh" \
  --bundle "$bundle" \
  --config "$invalid_config" \
  --device-token-file "$replacement_token" \
  --host fixture \
  --user fixture \
  --remote-dir "$remote_dir" \
  --remote-archive "$remote_archive" \
  --media-frames 0 >/dev/null 2>&1; then
  fail "invalid candidate config unexpectedly activated"
fi

[[ "$(readlink "$remote_dir/current")" == "$first_release" ]] ||
  fail "failed preflight changed the active release"
grep -q 'site_calibration: preserve-me' "$remote_dir/config/vehicle-agent.yaml" ||
  fail "failed preflight changed the persistent site configuration"
grep -q 'site-token-preserve-me' "$remote_dir/config/device-token" ||
  fail "failed preflight changed the persistent device token"
[[ ! -e "$remote_archive" && ! -e "$remote_archive.vehicle-agent.yaml" &&
    ! -e "$remote_archive.device-token" ]] ||
  fail "failed preflight left uploaded candidate files behind"
[[ "$(find "$remote_dir/.releases" -mindepth 1 -maxdepth 1 -type d | wc -l | tr -d ' ')" == "1" ]] ||
  fail "failed deployment left a partial release directory"

printf 'vehicle_deploy_transaction=passed\n'
