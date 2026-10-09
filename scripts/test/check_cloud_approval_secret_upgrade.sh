#!/usr/bin/env bash
set -euo pipefail

# Run the actual deploy script and signaling binary in disposable containers.
# Only service-manager/proxy commands are stubbed; no host /etc or /opt is touched.
bundle="${1:-}"
[[ -d "$bundle" && -f "$bundle/deploy-cloud.sh" ]] || {
  printf 'usage: %s /path/to/extracted-cloud-bundle\n' "$0" >&2
  exit 2
}
bundle="$(cd "$bundle" && pwd)"
temporary="$(mktemp -d)"
container_id=""
cleanup() {
  if [[ -n "$container_id" ]]; then docker rm -f "$container_id" >/dev/null 2>&1 || true; fi
  rm -rf "$temporary"
}
trap cleanup EXIT
cat >"$temporary/check.sh" <<'CONTAINER'
set -eu
set -o pipefail
mkdir -p /opt/mine-teleop/config /etc/mine-teleop/secrets /tmp/stubs /tmp/input
printf 'legacy-approval\n' >/opt/mine-teleop/config/app-token
printf 'identity-secret\n' >/etc/mine-teleop/secrets/identity
printf 'legacy-approval\n' >/tmp/expected
if [[ "$SCENARIO" == preserve ]]; then
  printf 'persistent-approval\n' >/etc/mine-teleop/secrets/app-token
  chmod 600 /etc/mine-teleop/secrets/app-token
  cp /etc/mine-teleop/secrets/app-token /tmp/expected
elif [[ "$SCENARIO" == override ]]; then
  printf 'explicit-approval\n' >/tmp/input/app-token
  cp /tmp/input/app-token /tmp/expected
fi
cat >/etc/mine-teleop/signaling-server.yaml <<'YAML'
auth:
  drivers:
    - id: fixture-driver
      password_file: secrets/identity
      vehicles: [fixture-vehicle]
  vehicles:
    - id: fixture-vehicle
      device_token_file: secrets/identity
      mobile_approval_required: true
YAML
for command in systemctl caddy curl haproxy turnserver; do
  ln -s /bin/true "/tmp/stubs/$command"
done
export PATH="/tmp/stubs:$PATH"
options=(--skip-package-install --no-start)
if [[ "$SCENARIO" == override ]]; then options+=(--identity-secrets-dir /tmp/input); fi
cd /tmp
bash /bundle/deploy-cloud.sh "${options[@]}"
cmp /tmp/expected /etc/mine-teleop/secrets/app-token
test "$(stat -c %a /etc/mine-teleop/secrets/app-token)" = 600
test ! -e /opt/mine-teleop/config/app-token
test -f /opt/mine-teleop.previous-*/config/app-token

# Repeat a real directory replacement, with no explicit secret input this time.
sleep 1
bash /bundle/deploy-cloud.sh --skip-package-install --no-start
cmp /tmp/expected /etc/mine-teleop/secrets/app-token
signal=(/opt/mine-teleop/lib/ld-linux-x86-64.so.2 --library-path /opt/mine-teleop/lib
  /opt/mine-teleop/bin/mine-teleop-signaling-server --config /etc/mine-teleop/signaling-server.yaml)
for cwd in /tmp /opt/mine-teleop; do
  cd "$cwd"
  "${signal[@]}" --validate-config
done

# Start from the systemd working directory and verify the retained password can
# actually log in, beyond merely passing configuration validation.
"${signal[@]}" --host 127.0.0.1 --port 18765 >/tmp/signaling.log 2>&1 &
server_pid=$!
trap 'kill "$server_pid" 2>/dev/null || true; wait "$server_pid" || true' EXIT
ready=false
for _ in $(seq 1 100); do
  if (echo >/dev/tcp/127.0.0.1/18765) 2>/dev/null; then ready=true; break; fi
  sleep 0.05
done
if [[ "$ready" != true ]]; then cat /tmp/signaling.log; exit 1; fi
payload="{\"password\":\"$(cat /tmp/expected)\"}"
exec 3<>/dev/tcp/127.0.0.1/18765
printf 'POST /mobile/api/login HTTP/1.1\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: %s\r\nConnection: close\r\n\r\n%s' "${#payload}" "$payload" >&3
IFS= read -r status <&3
[[ "$status" == *' 200 '* ]] || { printf 'login failed: %s\n' "$status"; exit 1; }
cat <&3 >/dev/null
exec 3<&- 3>&-
printf 'cloud_approval_upgrade=%s passed (two upgrades, two validation directories, runtime login)\n' "$SCENARIO"
CONTAINER
for scenario in migrate preserve override; do
  container_id="$(docker create --platform linux/amd64 -e "SCENARIO=$scenario" \
    ubuntu:22.04 bash /tmp/check.sh)"
  docker cp "$bundle" "$container_id:/bundle"
  docker cp "$temporary/check.sh" "$container_id:/tmp/check.sh"
  docker start --attach "$container_id"
  docker rm "$container_id" >/dev/null
  container_id=""
done
