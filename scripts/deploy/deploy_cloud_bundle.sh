#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
if [[ -d "$script_dir/../../deployments" && -d "$script_dir/../../packaging" ]]; then
  package_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
else
  package_root="$script_dir"
fi

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
script stages and validates the complete candidate before cutover, installs the
signaling service under /opt/mine-teleop, groups signaling/coturn/Caddy/HAProxy
with mine-teleop-cloud.target, starts the target, and checks
http://127.0.0.1:8765/health. A failed cutover restores the prior files,
application bundle, enablement state, and running services.

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
  --no-start                   Validate and install without stopping or starting services.
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

deployment_timestamp="$(date -u +%Y%m%d-%H%M%S)"
backup_root="/var/backups/mine-teleop/${deployment_timestamp}-$$"
previous_prefix=""
candidate_root="$(mktemp -d /var/tmp/mine-teleop-cloud-candidate.XXXXXX)"
candidate_prefix="$package_root"
candidate_prefix_cleanup=""
mutation_started="false"
deployment_committed="false"
prefix_replacement_started="false"
prefix_had_existing="false"
changed_paths=()
managed_units=(
  mine-teleop-signaling-server.service
  mine-teleop-turn-server.service
  caddy.service
  haproxy.service
)
previously_active_units=()
cloud_target_was_active="false"
cloud_target_was_enabled="false"
distribution_coturn_was_active="false"
distribution_coturn_was_enabled="false"
service_state_mutated="false"

record_changed_path() {
  local path="$1"
  local recorded
  for recorded in "${changed_paths[@]:-}"; do
    [[ "$recorded" != "$path" ]] || return
  done
  changed_paths+=("$path")
}

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
  record_changed_path "$destination"
  local staged_destination="${destination}.candidate.$$"
  rm -f -- "$staged_destination"
  install -D -m "$mode" "$source" "$staged_destination" || {
    rm -f -- "$staged_destination"
    return 1
  }
  mv -f "$staged_destination" "$destination" || {
    rm -f -- "$staged_destination"
    return 1
  }
}

unit_was_active() {
  local expected="$1"
  local unit
  for unit in "${previously_active_units[@]:-}"; do
    [[ "$unit" != "$expected" ]] || return 0
  done
  return 1
}

rollback_deployment() {
  local index
  local path
  local saved

  set +e
  printf '==> deployment failed; restoring the previous cloud installation\n' >&2
  if [[ "$service_state_mutated" == "true" ]]; then
    systemctl stop mine-teleop-cloud.target >/dev/null 2>&1 || true
  fi

  for ((index = ${#changed_paths[@]} - 1; index >= 0; --index)); do
    path="${changed_paths[$index]}"
    saved="$backup_root$path"
    rm -rf -- "$path"
    if [[ -e "$saved" || -L "$saved" ]]; then
      mkdir -p "$(dirname -- "$path")"
      cp -a "$saved" "$path"
    fi
  done

  if [[ "$prefix_replacement_started" == "true" ]]; then
    if [[ -n "$previous_prefix" && ( -e "$previous_prefix" || -L "$previous_prefix" ) ]]; then
      rm -rf -- "$prefix"
      mv "$previous_prefix" "$prefix"
    elif [[ "$prefix_had_existing" != "true" && ! -e "$candidate_prefix" && ! -L "$candidate_prefix" ]]; then
      # The candidate move completed before a signal was delivered, but there
      # was no prior installation to restore.
      rm -rf -- "$prefix"
    fi
  fi

  systemctl daemon-reload >/dev/null 2>&1 || true
  if [[ "$service_state_mutated" == "true" ]]; then
    if [[ "$cloud_target_was_enabled" == "true" ]]; then
      systemctl enable mine-teleop-cloud.target >/dev/null 2>&1 || true
    else
      systemctl disable mine-teleop-cloud.target >/dev/null 2>&1 || true
    fi
    if [[ "$distribution_coturn_was_enabled" == "true" ]]; then
      systemctl enable coturn.service >/dev/null 2>&1 || true
    else
      systemctl disable coturn.service >/dev/null 2>&1 || true
    fi

    if [[ "$cloud_target_was_active" == "true" ]]; then
      systemctl start mine-teleop-cloud.target >/dev/null 2>&1 || true
    else
      for path in "${managed_units[@]}"; do
        if unit_was_active "$path"; then
          systemctl start "$path" >/dev/null 2>&1 || true
        else
          systemctl stop "$path" >/dev/null 2>&1 || true
        fi
      done
    fi
    if [[ "$distribution_coturn_was_active" == "true" ]]; then
      systemctl start coturn.service >/dev/null 2>&1 || true
    else
      systemctl stop coturn.service >/dev/null 2>&1 || true
    fi
  fi
  printf 'rollback_backup=%s\n' "$backup_root" >&2
}

finish_deployment() {
  local status=$?
  trap - EXIT
  if [[ "$status" -ne 0 && "$mutation_started" == "true" && "$deployment_committed" != "true" ]]; then
    rollback_deployment
  fi
  if [[ -n "$candidate_prefix_cleanup" ]]; then
    rm -rf -- "$candidate_prefix_cleanup"
  fi
  rm -rf -- "$candidate_root"
  exit "$status"
}
trap finish_deployment EXIT

package_real="$(CDPATH= cd -- "$package_root" && pwd -P)"
prefix_real=""
if [[ -d "$prefix" ]]; then
  prefix_real="$(CDPATH= cd -- "$prefix" && pwd -P)"
fi
if [[ "$package_real" != "$prefix_real" ]]; then
  candidate_prefix="${prefix}.candidate.$$"
  candidate_prefix_cleanup="$candidate_prefix"
  [[ ! -e "$candidate_prefix" && ! -L "$candidate_prefix" ]] || {
    die "candidate application path already exists: $candidate_prefix"
  }
  mkdir -p "$candidate_prefix"
  cp -a "$package_root/." "$candidate_prefix/"
fi

candidate_config_dir="$candidate_root/etc/mine-teleop"
candidate_signaling_config="$candidate_config_dir/signaling-server.yaml"
candidate_environment_file="$candidate_config_dir/mine-teleop.env"
candidate_turn_secret="$candidate_config_dir/secrets/turn-static-auth.secret"
candidate_turn_config="$candidate_config_dir/turnserver.conf"
candidate_state_file="$candidate_config_dir/cloud-bundle.env"
candidate_override="$candidate_root$override_path"
candidate_caddy_dir="$candidate_root/etc/caddy"
candidate_haproxy_dir="$candidate_root/etc/haproxy"
candidate_caddy_config="$candidate_caddy_dir/Caddyfile"
candidate_haproxy_config="$candidate_haproxy_dir/haproxy.cfg"

mkdir -p \
  "$candidate_config_dir/secrets" \
  "$candidate_config_dir/tls" \
  "$candidate_caddy_dir" \
  "$candidate_haproxy_dir" \
  "$(dirname -- "$candidate_override")"
if [[ -d "$config_dir" ]]; then
  cp -a "$config_dir/." "$candidate_config_dir/"
fi
if [[ -d /etc/caddy ]]; then
  cp -a /etc/caddy/. "$candidate_caddy_dir/"
fi
if [[ -d /etc/haproxy ]]; then
  cp -a /etc/haproxy/. "$candidate_haproxy_dir/"
fi

if [[ -n "$environment_file" ]]; then
  install -m 0600 "$environment_file" "$candidate_environment_file"
elif [[ ! -f "$candidate_environment_file" ]]; then
  install -m 0600 /dev/null "$candidate_environment_file"
fi
if [[ -n "$signaling_config" ]]; then
  install -m 0640 "$signaling_config" "$candidate_signaling_config"
fi
if [[ -n "$identity_secrets_dir" ]]; then
  while IFS= read -r -d '' secret_path; do
    install -m 0600 \
      "$secret_path" \
      "$candidate_config_dir/secrets/$(basename -- "$secret_path")"
  done < <(find "$identity_secrets_dir" -maxdepth 1 -type f -print0)
fi
if [[ -n "$turn_secret_file" ]]; then
  install -m 0600 "$turn_secret_file" "$candidate_turn_secret"
fi
if [[ -n "$caddy_config" ]]; then
  install -m 0644 "$caddy_config" "$candidate_caddy_config"
fi
if [[ -n "$haproxy_config" ]]; then
  install -m 0644 "$haproxy_config" "$candidate_haproxy_config"
fi

if [[ -n "$turn_realm" ]]; then
  [[ -f "$candidate_turn_secret" ]] || die "TURN secret is missing from the candidate configuration"
  "$candidate_prefix/scripts/render_turnserver_config.sh" \
    --template "$candidate_prefix/deployments/turnserver/turnserver.conf.template" \
    --realm "$turn_realm" \
    --secret-file "$candidate_turn_secret" \
    --output "$candidate_turn_config"
fi

if [[ -n "$turn_realm" && -n "$turn_host" ]]; then
  cat >"$candidate_override" <<EOF
[Service]
ExecStart=
ExecStart=/opt/mine-teleop/lib/ld-linux-x86-64.so.2 --library-path /opt/mine-teleop/lib /opt/mine-teleop/bin/mine-teleop-signaling-server --config /etc/mine-teleop/signaling-server.yaml --host 127.0.0.1 --port 8765 --driver-token-ttl-ms 3600000 --control-token-ttl-ms 300000 --vehicle-heartbeat-ms 15000 --driver-heartbeat-ms 15000 --trusted-proxy-addresses 127.0.0.1,::1 --stun-urls stun:${turn_host}:3478 --turn-urls turn:${turn_host}:3478?transport=udp,turn:${turn_host}:3478?transport=tcp,turn:${turn_host}:6000?transport=tcp,turn:${turn_host}:443?transport=tcp --turn-realm ${turn_realm} --turn-static-auth-secret-file /etc/mine-teleop/secrets/turn-static-auth.secret --turn-credential-ttl-seconds 600 --api-rate-limit-requests 6000 --audit-log /var/log/mine-teleop/signaling-audit.jsonl --audit-log-retention-days 7
EOF
  printf '%s\n' \
    "MINE_TELEOP_TURN_REALM=$turn_realm" \
    "MINE_TELEOP_TURN_HOST=$turn_host" \
    >"$candidate_state_file"
fi

printf '==> validating the complete candidate configuration before cutover\n'
if [[ -f "$candidate_signaling_config" ]]; then
  signaling_validation=(
    "$candidate_prefix/lib/ld-linux-x86-64.so.2"
    --library-path "$candidate_prefix/lib"
    "$candidate_prefix/bin/mine-teleop-signaling-server"
    --config "$candidate_signaling_config"
  )
  if [[ -n "$turn_realm" ]]; then
    signaling_validation+=(
      --turn-realm "$turn_realm"
      --turn-static-auth-secret-file "$candidate_turn_secret"
    )
  fi
  signaling_validation+=(--validate-config)
  if grep -Eq '^[[:space:]]*(password_env|device_token_env):' "$candidate_signaling_config"; then
    command -v systemd-run >/dev/null 2>&1 || {
      die "systemd-run is required to validate identity variables from the candidate environment file"
    }
    systemd-run \
      --quiet \
      --wait \
      --pipe \
      --collect \
      --service-type=exec \
      --unit="mine-teleop-config-validate-$$" \
      --property="EnvironmentFile=$candidate_environment_file" \
      "${signaling_validation[@]}"
  else
    "${signaling_validation[@]}"
  fi
elif [[ "$start_services" == "true" ]]; then
  die "signaling configuration is missing from the candidate installation"
fi
if [[ -f "$candidate_caddy_config" ]]; then
  caddy validate --config "$candidate_caddy_config"
elif [[ "$start_services" == "true" ]]; then
  die "Caddy configuration is missing from the candidate installation"
fi
if [[ -f "$candidate_haproxy_config" ]]; then
  haproxy -c -f "$candidate_haproxy_config"
elif [[ "$start_services" == "true" ]]; then
  die "HAProxy configuration is missing from the candidate installation"
fi
if [[ "$start_services" == "true" ]]; then
  [[ -f "$candidate_turn_config" ]] || die "coturn configuration is missing from the candidate installation"
  [[ -f "$candidate_override" ]] || die "signaling systemd override is missing from the candidate installation"
fi

mkdir -p "$backup_root"
if systemctl is-active --quiet mine-teleop-cloud.target; then
  cloud_target_was_active="true"
fi
if systemctl is-enabled --quiet mine-teleop-cloud.target; then
  cloud_target_was_enabled="true"
fi
if systemctl is-active --quiet coturn.service; then
  distribution_coturn_was_active="true"
fi
if systemctl is-enabled --quiet coturn.service; then
  distribution_coturn_was_enabled="true"
fi
for unit in "${managed_units[@]}"; do
  if systemctl is-active --quiet "$unit"; then
    previously_active_units+=("$unit")
  fi
done

mutation_started="true"
if [[ "$start_services" == "true" ]]; then
  printf '==> candidate passed; stopping the existing cloud target for cutover\n'
  service_state_mutated="true"
  systemctl stop mine-teleop-cloud.target 2>/dev/null || true
  systemctl disable --now coturn.service 2>/dev/null || true
fi

if [[ "$package_real" != "$prefix_real" ]]; then
  printf '==> switching the application bundle under %s\n' "$prefix"
  previous_prefix="${prefix}.previous-$deployment_timestamp"
  [[ ! -e "$previous_prefix" && ! -L "$previous_prefix" ]] || {
    die "backup path already exists: $previous_prefix"
  }
  prefix_replacement_started="true"
  if [[ -e "$prefix" || -L "$prefix" ]]; then
    prefix_had_existing="true"
    mv "$prefix" "$previous_prefix"
  fi
  mkdir -p "$(dirname -- "$prefix")"
  mv "$candidate_prefix" "$prefix"
else
  printf '==> application bundle is already installed under %s\n' "$prefix"
fi

install -d -m 0750 "$config_dir" "$config_dir/secrets" "$config_dir/tls"
if [[ -n "$environment_file" || ! -f "$config_dir/mine-teleop.env" ]]; then
  install_config_file "$candidate_environment_file" "$config_dir/mine-teleop.env" 0600
fi
if [[ -n "$signaling_config" ]]; then
  install_config_file "$candidate_signaling_config" "$signaling_config_path" 0640
fi
if [[ -n "$identity_secrets_dir" ]]; then
  while IFS= read -r -d '' secret_path; do
    install_config_file \
      "$candidate_config_dir/secrets/$(basename -- "$secret_path")" \
      "$config_dir/secrets/$(basename -- "$secret_path")" \
      0600
  done < <(find "$identity_secrets_dir" -maxdepth 1 -type f -print0)
fi
if [[ -n "$turn_secret_file" ]]; then
  install_config_file "$candidate_turn_secret" "$turn_secret_path" 0600
fi
if [[ -n "$caddy_config" ]]; then
  install_config_file "$candidate_caddy_config" "$caddy_config_path" 0644
fi
if [[ -n "$haproxy_config" ]]; then
  install_config_file "$candidate_haproxy_config" "$haproxy_config_path" 0644
fi
if [[ -n "$turn_realm" ]]; then
  install_config_file "$candidate_turn_config" "$turn_config_path" 0640
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
  install_config_file "$candidate_override" "$override_path" 0644
  install_config_file "$candidate_state_file" "$state_file" 0644
fi

printf '==> validating installed configuration\n'
if [[ -f "$signaling_config_path" ]]; then
  installed_signaling_validation=(
    "$prefix/lib/ld-linux-x86-64.so.2"
    --library-path "$prefix/lib"
    "$prefix/bin/mine-teleop-signaling-server"
    --config "$signaling_config_path"
  )
  if [[ -n "$turn_realm" ]]; then
    installed_signaling_validation+=(
      --turn-realm "$turn_realm"
      --turn-static-auth-secret-file "$turn_secret_path"
    )
  fi
  installed_signaling_validation+=(--validate-config)
  if grep -Eq '^[[:space:]]*(password_env|device_token_env):' "$signaling_config_path"; then
    systemd-run \
      --quiet \
      --wait \
      --pipe \
      --collect \
      --service-type=exec \
      --unit="mine-teleop-installed-config-validate-$$" \
      --property="EnvironmentFile=$config_dir/mine-teleop.env" \
      "${installed_signaling_validation[@]}"
  else
    "${installed_signaling_validation[@]}"
  fi
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

systemctl daemon-reload

if [[ "$start_services" == "false" ]]; then
  deployment_committed="true"
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

candidate_managed_marker="$candidate_config_dir/.cloud-bundle-managed"
printf '%s\n' \
  "installed_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  "application_prefix=$prefix" \
  >"$candidate_managed_marker"
install_config_file "$candidate_managed_marker" "$config_dir/.cloud-bundle-managed" 0644

systemctl --no-pager --full status \
  mine-teleop-cloud.target \
  mine-teleop-signaling-server.service \
  mine-teleop-turn-server.service \
  caddy.service \
  haproxy.service

deployment_committed="true"
printf '%s\n' \
  "cloud_bundle_deploy=passed" \
  "health_url=http://127.0.0.1:8765/health" \
  "backup_root=$backup_root" \
  "previous_application=${previous_prefix:-none}"
