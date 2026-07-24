#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
package_root="$script_dir"

prefix="/opt/mine-teleop"
config_dir="/etc/mine-teleop"
signaling_config=""
identity_secrets_dir=""
turn_secret_file=""
turn_realm=""
turn_host=""
environment_file=""
caddy_config=""
haproxy_config=""
install_packages="true"
start_services="true"
dry_run="false"
self_test="false"

usage() {
  cat <<'EOF'
Usage:
  sudo ./deploy-cloud.sh [options]

Install or upgrade the Mine Teleop cloud bundle on Ubuntu 22.04 x86_64. The
script installs the signaling service under /opt/mine-teleop, installs and
groups signaling/coturn/Caddy/HAProxy with mine-teleop-cloud.target, validates
configuration, starts the target, and checks http://127.0.0.1:8765/health.

Configuration options:
  --signaling-config PATH      Identity YAML installed as signaling-server.yaml.
  --identity-secrets-dir PATH  Directory whose regular files are installed with
                               mode 0600 under /etc/mine-teleop/secrets.
  --turn-secret-file PATH      Coturn REST shared-secret file.
  --turn-realm REALM           Coturn realm and credential-signing realm.
  --turn-host HOST             Public STUN/TURN host. Defaults to TURN realm.
  --env-file PATH              Optional systemd EnvironmentFile replacement.
  --caddy-config PATH          Caddyfile replacement.
  --haproxy-config PATH        HAProxy configuration replacement.

Behavior options:
  --skip-package-install       Do not apt-install caddy, coturn, curl, haproxy.
  --no-start                   Install files and units without starting services.
  --dry-run                    Validate inputs and print the deployment plan.
  --self-test                  Validate only the extracted package itself.
  -h, --help                   Show this help.

Existing /etc/mine-teleop and proxy configuration is reused unless a replacement
is explicitly supplied. The package never creates credentials.
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

require_value() {
  local option="$1"
  local value="${2:-}"
  [[ -n "$value" ]] || die "$option requires a value"
}

absolute_file() {
  local path="$1"
  local directory
  directory="$(CDPATH= cd -- "$(dirname -- "$path")" && pwd)"
  printf '%s/%s\n' "$directory" "$(basename -- "$path")"
}

require_package_layout() {
  local required
  for required in \
    "$package_root/bin/mine-teleop-signaling-server" \
    "$package_root/lib/ld-linux-x86-64.so.2" \
    "$package_root/deployments/systemd/mine-teleop-signaling-server.service" \
    "$package_root/deployments/systemd/mine-teleop-turn-server.service" \
    "$package_root/deployments/systemd/mine-teleop-cloud.target" \
    "$package_root/deployments/turnserver/turnserver.conf.template" \
    "$package_root/scripts/render_turnserver_config.sh"; do
    [[ -e "$required" ]] || die "cloud package is incomplete: missing ${required#"$package_root/"}"
  done
}

while (($#)); do
  case "$1" in
    --signaling-config)
      require_value "$1" "${2:-}"
      signaling_config="$2"
      shift 2
      ;;
    --identity-secrets-dir)
      require_value "$1" "${2:-}"
      identity_secrets_dir="$2"
      shift 2
      ;;
    --turn-secret-file)
      require_value "$1" "${2:-}"
      turn_secret_file="$2"
      shift 2
      ;;
    --turn-realm)
      require_value "$1" "${2:-}"
      turn_realm="$2"
      shift 2
      ;;
    --turn-host)
      require_value "$1" "${2:-}"
      turn_host="$2"
      shift 2
      ;;
    --env-file)
      require_value "$1" "${2:-}"
      environment_file="$2"
      shift 2
      ;;
    --caddy-config)
      require_value "$1" "${2:-}"
      caddy_config="$2"
      shift 2
      ;;
    --haproxy-config)
      require_value "$1" "${2:-}"
      haproxy_config="$2"
      shift 2
      ;;
    --skip-package-install)
      install_packages="false"
      shift
      ;;
    --no-start)
      start_services="false"
      shift
      ;;
    --dry-run)
      dry_run="true"
      shift
      ;;
    --self-test)
      self_test="true"
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      die "unknown option: $1"
      ;;
  esac
done

require_package_layout

if [[ "$self_test" == "true" ]]; then
  bash -n "$package_root/deploy-cloud.sh" \
    "$package_root/scripts/render_turnserver_config.sh"
  "$package_root/lib/ld-linux-x86-64.so.2" \
    --library-path "$package_root/lib" \
    "$package_root/bin/mine-teleop-signaling-server" \
    --version
  "$package_root/scripts/render_turnserver_config.sh" --self-test
  printf 'cloud_bundle_self_test=passed\n'
  exit 0
fi

for input_file in \
  "$signaling_config" \
  "$turn_secret_file" \
  "$environment_file" \
  "$caddy_config" \
  "$haproxy_config"; do
  if [[ -n "$input_file" && ! -f "$input_file" ]]; then
    die "input file does not exist: $input_file"
  fi
done
if [[ -n "$identity_secrets_dir" ]]; then
  [[ -d "$identity_secrets_dir" ]] || {
    die "identity secrets directory does not exist: $identity_secrets_dir"
  }
  [[ -n "$(find "$identity_secrets_dir" -maxdepth 1 -type f -print -quit)" ]] || {
    die "identity secrets directory contains no regular files"
  }
fi

[[ -z "$turn_realm" || "$turn_realm" =~ ^[A-Za-z0-9._-]+$ ]] || {
  die "--turn-realm contains unsupported characters"
}
[[ -z "$turn_host" || "$turn_host" =~ ^[A-Za-z0-9.-]+$ ]] || {
  die "--turn-host must be a hostname without scheme, port, or path"
}

for variable_name in \
  signaling_config \
  turn_secret_file \
  environment_file \
  caddy_config \
  haproxy_config; do
  value="${!variable_name}"
  if [[ -n "$value" ]]; then
    printf -v "$variable_name" '%s' "$(absolute_file "$value")"
  fi
done
if [[ -n "$identity_secrets_dir" ]]; then
  identity_secrets_dir="$(CDPATH= cd -- "$identity_secrets_dir" && pwd)"
fi

state_file="$config_dir/cloud-bundle.env"
turn_config_path="$config_dir/turnserver.conf"
turn_secret_path="$config_dir/secrets/turn-static-auth.secret"
signaling_config_path="$config_dir/signaling-server.yaml"
caddy_config_path="/etc/caddy/Caddyfile"
haproxy_config_path="/etc/haproxy/haproxy.cfg"
override_path="/etc/systemd/system/mine-teleop-signaling-server.service.d/zz-mine-teleop-cloud-bundle.conf"

if [[ -z "$turn_realm" && -f "$state_file" ]]; then
  turn_realm="$(sed -n 's/^MINE_TELEOP_TURN_REALM=//p' "$state_file" | head -n 1)"
fi
if [[ -z "$turn_realm" && -f "$turn_config_path" ]]; then
  turn_realm="$(sed -n 's/^realm=//p' "$turn_config_path" | head -n 1)"
fi
if [[ -z "$turn_host" && -f "$state_file" ]]; then
  turn_host="$(sed -n 's/^MINE_TELEOP_TURN_HOST=//p' "$state_file" | head -n 1)"
fi
if [[ -z "$turn_host" && -n "$turn_realm" ]]; then
  turn_host="$turn_realm"
fi
[[ -z "$turn_realm" || "$turn_realm" =~ ^[A-Za-z0-9._-]+$ ]] || {
  die "stored TURN realm contains unsupported characters"
}
[[ -z "$turn_host" || "$turn_host" =~ ^[A-Za-z0-9.-]+$ ]] || {
  die "stored TURN host contains unsupported characters"
}

if [[ "$start_services" == "true" ]]; then
  [[ -n "$signaling_config" || -f "$signaling_config_path" ]] || {
    die "first start requires --signaling-config"
  }
  [[ -n "$turn_secret_file" || -f "$turn_secret_path" ]] || {
    die "first start requires --turn-secret-file"
  }
  [[ -n "$turn_realm" ]] || die "first start requires --turn-realm"
  [[ -n "$turn_host" ]] || die "first start requires --turn-host"
  [[ -n "$caddy_config" || -f "$caddy_config_path" ]] || {
    die "first start requires --caddy-config"
  }
  [[ -n "$haproxy_config" || -f "$haproxy_config_path" ]] || {
    die "first start requires --haproxy-config"
  }
fi

if [[ "$dry_run" == "true" ]]; then
  printf '%s\n' \
    "package_root=$package_root" \
    "install_prefix=$prefix" \
    "config_dir=$config_dir" \
    "install_packages=$install_packages" \
    "start_services=$start_services" \
    "signaling_config=${signaling_config:-preserve-existing}" \
    "identity_secrets_dir=${identity_secrets_dir:-preserve-existing}" \
    "turn_realm=${turn_realm:-not-configured}" \
    "turn_host=${turn_host:-not-configured}" \
    "caddy_config=${caddy_config:-preserve-existing}" \
    "haproxy_config=${haproxy_config:-preserve-existing}" \
    'cloud_bundle_deploy_dry_run=passed'
  exit 0
fi

[[ "$(uname -s)" == "Linux" ]] || die "deployment target must be Linux"
case "$(uname -m)" in
  x86_64|amd64) ;;
  *) die "deployment target must be x86_64/amd64" ;;
esac
[[ "$EUID" -eq 0 ]] || die "run this deployment script with sudo"
command -v systemctl >/dev/null 2>&1 || die "systemd is required"

if [[ "$install_packages" == "true" ]]; then
  printf '==> installing cloud service packages\n'
  if ! apt-get -o Acquire::Retries=5 update; then
    die "apt-get update failed; fix DNS/repository access and retry"
  fi
  if ! DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=5 install -y \
    --no-install-recommends \
    ca-certificates \
    caddy \
    coturn \
    curl \
    haproxy; then
    die "package installation failed; fix apt sources or preinstall packages and use --skip-package-install"
  fi
fi

for required_command in caddy curl haproxy turnserver; do
  command -v "$required_command" >/dev/null 2>&1 || {
    die "required command is missing: $required_command"
  }
done

printf '==> stopping the existing cloud target\n'
systemctl stop mine-teleop-cloud.target 2>/dev/null || true
systemctl disable --now coturn.service 2>/dev/null || true

deployment_timestamp="$(date -u +%Y%m%d-%H%M%S)"
backup_root="/var/backups/mine-teleop/$deployment_timestamp"
previous_prefix=""
mkdir -p "$backup_root"

backup_existing() {
  local path="$1"
  local destination
  if [[ -e "$path" || -L "$path" ]]; then
    destination="$backup_root$path"
    mkdir -p "$(dirname -- "$destination")"
    cp -a "$path" "$destination"
  fi
}

install_config_file() {
  local source="$1"
  local destination="$2"
  local mode="$3"
  if [[ -e "$destination" && "$source" -ef "$destination" ]]; then
    chmod "$mode" "$destination"
    return
  fi
  backup_existing "$destination"
  install -D -m "$mode" "$source" "$destination"
}

package_real="$(CDPATH= cd -- "$package_root" && pwd -P)"
prefix_real=""
if [[ -d "$prefix" ]]; then
  prefix_real="$(CDPATH= cd -- "$prefix" && pwd -P)"
fi
if [[ "$package_real" != "$prefix_real" ]]; then
  printf '==> installing application bundle under %s\n' "$prefix"
  staging_prefix="${prefix}.new.$$"
  [[ ! -e "$staging_prefix" ]] || die "staging path already exists: $staging_prefix"
  mkdir -p "$staging_prefix"
  cp -a "$package_root/." "$staging_prefix/"
  if [[ -e "$prefix" || -L "$prefix" ]]; then
    previous_prefix="${prefix}.previous-$deployment_timestamp"
    [[ ! -e "$previous_prefix" ]] || die "backup path already exists: $previous_prefix"
    mv "$prefix" "$previous_prefix"
  fi
  mv "$staging_prefix" "$prefix"
else
  printf '==> application bundle is already installed under %s\n' "$prefix"
fi

install -d -m 0750 "$config_dir" "$config_dir/secrets" "$config_dir/tls"
if [[ -n "$environment_file" ]]; then
  install_config_file "$environment_file" "$config_dir/mine-teleop.env" 0600
elif [[ ! -f "$config_dir/mine-teleop.env" ]]; then
  install -m 0600 /dev/null "$config_dir/mine-teleop.env"
fi
if [[ -n "$signaling_config" ]]; then
  install_config_file "$signaling_config" "$signaling_config_path" 0640
fi
if [[ -n "$identity_secrets_dir" ]]; then
  while IFS= read -r -d '' secret_path; do
    install_config_file \
      "$secret_path" \
      "$config_dir/secrets/$(basename -- "$secret_path")" \
      0600
  done < <(find "$identity_secrets_dir" -maxdepth 1 -type f -print0)
fi
if [[ -n "$turn_secret_file" ]]; then
  install_config_file "$turn_secret_file" "$turn_secret_path" 0600
fi
if [[ -n "$caddy_config" ]]; then
  install_config_file "$caddy_config" "$caddy_config_path" 0644
fi
if [[ -n "$haproxy_config" ]]; then
  install_config_file "$haproxy_config" "$haproxy_config_path" 0644
fi

if [[ -n "$turn_realm" ]]; then
  [[ -f "$turn_secret_path" ]] || die "TURN secret is missing: $turn_secret_path"
  backup_existing "$turn_config_path"
  "$prefix/scripts/render_turnserver_config.sh" \
    --template "$prefix/deployments/turnserver/turnserver.conf.template" \
    --realm "$turn_realm" \
    --secret-file "$turn_secret_path" \
    --output "$turn_config_path"
fi

printf '==> installing systemd units\n'
for unit in \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  mine-teleop-cloud.target; do
  install_config_file \
    "$prefix/deployments/systemd/$unit" \
    "/etc/systemd/system/$unit" \
    0644
done
install_config_file \
  "$prefix/deployments/systemd/caddy.service.d/mine-teleop-cloud.conf" \
  "/etc/systemd/system/caddy.service.d/mine-teleop-cloud.conf" \
  0644
install_config_file \
  "$prefix/deployments/systemd/haproxy.service.d/mine-teleop-cloud.conf" \
  "/etc/systemd/system/haproxy.service.d/mine-teleop-cloud.conf" \
  0644

if [[ -n "$turn_realm" && -n "$turn_host" ]]; then
  override_temporary="$(mktemp)"
  cat >"$override_temporary" <<EOF
[Service]
ExecStart=
ExecStart=/opt/mine-teleop/lib/ld-linux-x86-64.so.2 --library-path /opt/mine-teleop/lib /opt/mine-teleop/bin/mine-teleop-signaling-server --config /etc/mine-teleop/signaling-server.yaml --host 127.0.0.1 --port 8765 --driver-token-ttl-ms 3600000 --control-token-ttl-ms 300000 --vehicle-heartbeat-ms 15000 --driver-heartbeat-ms 15000 --trusted-proxy-addresses 127.0.0.1,::1 --stun-urls stun:${turn_host}:3478 --turn-urls turn:${turn_host}:3478?transport=udp,turn:${turn_host}:3478?transport=tcp,turn:${turn_host}:6000?transport=tcp,turn:${turn_host}:443?transport=tcp --turn-realm ${turn_realm} --turn-static-auth-secret-file /etc/mine-teleop/secrets/turn-static-auth.secret --turn-credential-ttl-seconds 600 --api-rate-limit-requests 6000 --audit-log /var/log/mine-teleop/signaling-audit.jsonl --audit-log-retention-days 7
EOF
  install_config_file "$override_temporary" "$override_path" 0644
  rm -f "$override_temporary"

  state_temporary="$(mktemp)"
  printf '%s\n' \
    "MINE_TELEOP_TURN_REALM=$turn_realm" \
    "MINE_TELEOP_TURN_HOST=$turn_host" \
    >"$state_temporary"
  install_config_file "$state_temporary" "$state_file" 0644
  rm -f "$state_temporary"
fi

printf '==> validating installed configuration\n'
if [[ -f "$signaling_config_path" ]]; then
  signaling_validation=(
    "$prefix/lib/ld-linux-x86-64.so.2"
    --library-path "$prefix/lib"
    "$prefix/bin/mine-teleop-signaling-server"
    --config "$signaling_config_path"
  )
  if [[ -n "$turn_realm" ]]; then
    signaling_validation+=(
      --turn-realm "$turn_realm"
      --turn-static-auth-secret-file "$turn_secret_path"
    )
  fi
  signaling_validation+=(--validate-config)
  "${signaling_validation[@]}"
elif [[ "$start_services" == "true" ]]; then
  die "signaling configuration is missing: $signaling_config_path"
fi
if [[ -f "$caddy_config_path" ]]; then
  caddy validate --config "$caddy_config_path"
elif [[ "$start_services" == "true" ]]; then
  die "Caddy configuration is missing: $caddy_config_path"
fi
if [[ -f "$haproxy_config_path" ]]; then
  haproxy -c -f "$haproxy_config_path"
elif [[ "$start_services" == "true" ]]; then
  die "HAProxy configuration is missing: $haproxy_config_path"
fi
if [[ "$start_services" == "true" ]]; then
  [[ -f "$turn_config_path" ]] || die "coturn configuration is missing: $turn_config_path"
  [[ -f "$override_path" ]] || die "signaling systemd override is missing: $override_path"
fi

systemctl daemon-reload

if [[ "$start_services" == "false" ]]; then
  printf '%s\n' \
    "cloud_bundle_deploy=installed-not-started" \
    "backup_root=$backup_root" \
    "previous_application=${previous_prefix:-none}"
  exit 0
fi

printf '==> enabling and starting mine-teleop-cloud.target\n'
systemctl enable mine-teleop-cloud.target
systemctl restart mine-teleop-cloud.target

health_ok="false"
for _ in $(seq 1 20); do
  if curl --fail --silent --show-error \
    http://127.0.0.1:8765/health >/dev/null; then
    health_ok="true"
    break
  fi
  sleep 1
done
if [[ "$health_ok" != "true" ]]; then
  systemctl --no-pager --full status \
    mine-teleop-cloud.target \
    mine-teleop-signaling-server.service \
    mine-teleop-turn-server.service \
    caddy.service \
    haproxy.service || true
  journalctl --no-pager -n 100 \
    -u mine-teleop-signaling-server.service \
    -u mine-teleop-turn-server.service || true
  die "cloud target started without a healthy signaling endpoint"
fi

printf '%s\n' \
  "installed_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  "application_prefix=$prefix" \
  >"$config_dir/.cloud-bundle-managed"
chmod 0644 "$config_dir/.cloud-bundle-managed"

systemctl --no-pager --full status \
  mine-teleop-cloud.target \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  caddy.service \
  haproxy.service

printf '%s\n' \
  "cloud_bundle_deploy=passed" \
  "health_url=http://127.0.0.1:8765/health" \
  "backup_root=$backup_root" \
  "previous_application=${previous_prefix:-none}"
