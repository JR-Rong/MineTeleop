#!/usr/bin/env bash
set -Eeuo pipefail

DEFAULT_FRP_VERSION="0.69.1"
DEFAULT_SERVER_ADDR="60.205.213.254"
DEFAULT_SERVER_PORT="7000"
DEFAULT_REMOTE_PORT="6000"
DEFAULT_LOCAL_IP="127.0.0.1"
DEFAULT_LOCAL_PORT="22"
DEFAULT_PROXY_NAME="ubuntu-ssh-6000"
DEFAULT_DOWNLOAD_BASE_URL="http://60.205.213.254/frp/releases/download"

CONFIG_PATH="/etc/frp/frpc.toml"
SERVICE_PATH="/etc/systemd/system/frpc.service"

FRP_VERSION="$DEFAULT_FRP_VERSION"
SERVER_ADDR="$DEFAULT_SERVER_ADDR"
SERVER_PORT="$DEFAULT_SERVER_PORT"
REMOTE_PORT="$DEFAULT_REMOTE_PORT"
LOCAL_IP="$DEFAULT_LOCAL_IP"
LOCAL_PORT="$DEFAULT_LOCAL_PORT"
PROXY_NAME="$DEFAULT_PROXY_NAME"
DOWNLOAD_BASE_URL="$DEFAULT_DOWNLOAD_BASE_URL"
TOKEN="${FRPC_AUTH_TOKEN:-${FRP_AUTH_TOKEN:-}}"
TOKEN_FILE=""
DRY_RUN=0
INSTALL_OPENSSH=1
SKIP_PACKAGE_INSTALL=0

usage() {
  cat <<'USAGE'
Usage:
  FRPC_AUTH_TOKEN='<frps-token>' bash scripts/setup_ubuntu_frpc.sh
  bash scripts/setup_ubuntu_frpc.sh --token-file /root/frpc-token.txt
  bash scripts/setup_ubuntu_frpc.sh --dry-run --token '<frps-token>'

Defaults:
  server addr: 60.205.213.254
  server port: 7000 (frps control port)
  remote port: 6000 (server-exposed frpc proxy port)
  local target: 127.0.0.1:22 on this Ubuntu client
  frp version: 0.69.1
  download base URL: http://60.205.213.254/frp/releases/download

Options:
  --token TOKEN          FRP auth token. Prefer FRPC_AUTH_TOKEN or --token-file.
  --token-file PATH      Read the FRP auth token from a local file.
  --server-addr HOST     frps host or IP.
  --server-port PORT     frps control port, usually 7000.
  --remote-port PORT     Public TCP port registered on frps, usually 6000 here.
  --local-ip IP          Local service IP on this Ubuntu client.
  --local-port PORT      Local service port on this Ubuntu client.
  --proxy-name NAME      FRP proxy name.
  --frp-version VERSION  FRP release version to install.
  --download-base-url URL
                         Base URL containing v<VERSION>/frp_<VERSION>_linux_<ARCH>.tar.gz.
  --skip-openssh         Do not install/enable openssh-server.
  --skip-package-install Do not run apt-get; use this on hosts with broken apt.
  --dry-run              Print generated config and unit without installing.
  -h, --help             Show this help.

Server-side requirement:
  frps allowPorts and the cloud security group must allow the chosen remote port.
  For the defaults, allow 6000/tcp on 60.205.213.254.
USAGE
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 1
}

root_run() {
  if [[ "$(id -u)" == "0" ]]; then
    "$@"
    return
  fi
  command -v sudo >/dev/null 2>&1 || die "this command needs root or sudo: $*"
  sudo "$@"
}

toml_string() {
  local value="$1"
  value="${value//\\/\\\\}"
  value="${value//\"/\\\"}"
  value="${value//$'\r'/}"
  value="${value//$'\n'/}"
  printf '"%s"' "$value"
}

require_port() {
  local name="$1"
  local value="$2"
  if ! [[ "$value" =~ ^[0-9]+$ ]]; then
    die "$name must be a TCP port number, got: $value"
  fi
  if (( value < 1 || value > 65535 )); then
    die "$name must be between 1 and 65535, got: $value"
  fi
}

parse_args() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --token)
        [[ $# -ge 2 ]] || die "--token requires a value"
        TOKEN="$2"
        shift 2
        ;;
      --token-file)
        [[ $# -ge 2 ]] || die "--token-file requires a value"
        TOKEN_FILE="$2"
        shift 2
        ;;
      --server-addr)
        [[ $# -ge 2 ]] || die "--server-addr requires a value"
        SERVER_ADDR="$2"
        shift 2
        ;;
      --server-port)
        [[ $# -ge 2 ]] || die "--server-port requires a value"
        SERVER_PORT="$2"
        shift 2
        ;;
      --remote-port)
        [[ $# -ge 2 ]] || die "--remote-port requires a value"
        REMOTE_PORT="$2"
        shift 2
        ;;
      --local-ip)
        [[ $# -ge 2 ]] || die "--local-ip requires a value"
        LOCAL_IP="$2"
        shift 2
        ;;
      --local-port)
        [[ $# -ge 2 ]] || die "--local-port requires a value"
        LOCAL_PORT="$2"
        shift 2
        ;;
      --proxy-name)
        [[ $# -ge 2 ]] || die "--proxy-name requires a value"
        PROXY_NAME="$2"
        shift 2
        ;;
      --frp-version)
        [[ $# -ge 2 ]] || die "--frp-version requires a value"
        FRP_VERSION="$2"
        shift 2
        ;;
      --download-base-url)
        [[ $# -ge 2 ]] || die "--download-base-url requires a value"
        DOWNLOAD_BASE_URL="$2"
        shift 2
        ;;
      --skip-openssh)
        INSTALL_OPENSSH=0
        shift
        ;;
      --skip-package-install)
        SKIP_PACKAGE_INSTALL=1
        shift
        ;;
      --dry-run)
        DRY_RUN=1
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
}

load_token() {
  if [[ -n "$TOKEN_FILE" ]]; then
    [[ -r "$TOKEN_FILE" ]] || die "token file is not readable: $TOKEN_FILE"
    TOKEN="$(head -n 1 "$TOKEN_FILE" | tr -d '\r\n')"
  fi

  if [[ -z "$TOKEN" && -t 0 ]]; then
    read -r -s -p "FRP auth token: " TOKEN
    printf '\n'
  fi

  [[ -n "$TOKEN" ]] || die "FRP auth token is required. Use FRPC_AUTH_TOKEN, --token-file, or --token."
}

validate_inputs() {
  require_port "--server-port" "$SERVER_PORT"
  require_port "--remote-port" "$REMOTE_PORT"
  require_port "--local-port" "$LOCAL_PORT"
  [[ "$PROXY_NAME" =~ ^[A-Za-z0-9_.-]+$ ]] || die "--proxy-name may contain only letters, numbers, dot, underscore, and dash"
}

render_config() {
  cat <<EOF
serverAddr = $(toml_string "$SERVER_ADDR")
serverPort = $SERVER_PORT

auth.method = "token"
auth.token = $(toml_string "$TOKEN")
auth.additionalScopes = ["HeartBeats", "NewWorkConns"]

transport.tls.enable = true
transport.tcpMux = false

log.to = "/var/log/frpc.log"
log.level = "info"
log.maxDays = 7

[[proxies]]
name = $(toml_string "$PROXY_NAME")
type = "tcp"
localIP = $(toml_string "$LOCAL_IP")
localPort = $LOCAL_PORT
remotePort = $REMOTE_PORT
EOF
}

render_service() {
  cat <<EOF
[Unit]
Description=FRP Client
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/local/bin/frpc -c $CONFIG_PATH
Restart=always
RestartSec=5
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
EOF
}

print_dry_run() {
  local arch
  arch="$(detect_frp_arch)"
  cat <<EOF
# Would install frpc $FRP_VERSION and write these files.
# Download URL: $(frp_archive_url "$arch")
# Server-side allowPorts/security group must include $REMOTE_PORT/tcp.

# $CONFIG_PATH
EOF
  render_config
  cat <<EOF

# $SERVICE_PATH
EOF
  render_service
  cat <<EOF

# External validation after deploy:
#   ssh -p $REMOTE_PORT <ubuntu-user>@$SERVER_ADDR hostname
# Client-side logs:
#   sudo journalctl -u frpc.service -n 80 --no-pager
EOF
}

detect_frp_arch() {
  local machine
  machine="$(uname -m)"
  case "$machine" in
    x86_64|amd64)
      printf 'amd64'
      ;;
    aarch64|arm64)
      printf 'arm64'
      ;;
    armv7l|armv7)
      printf 'arm'
      ;;
    *)
      die "unsupported CPU architecture for FRP release: $machine"
      ;;
  esac
}

frp_archive_url() {
  local arch="$1"
  local base="${DOWNLOAD_BASE_URL%/}"
  printf '%s/v%s/frp_%s_linux_%s.tar.gz' "$base" "$FRP_VERSION" "$FRP_VERSION" "$arch"
}

download_tool_available() {
  command -v curl >/dev/null 2>&1 || command -v wget >/dev/null 2>&1
}

require_download_tool() {
  download_tool_available || die "curl or wget is required when package installation is skipped"
}

download_file() {
  local url="$1"
  local destination="$2"
  if command -v curl >/dev/null 2>&1; then
    if curl -fsSL --retry 5 --retry-all-errors --retry-delay 3 --connect-timeout 20 "$url" -o "$destination"; then
      return
    fi
    if command -v wget >/dev/null 2>&1; then
      printf 'warning: curl download failed; trying wget for %s\n' "$url" >&2
    else
      return 1
    fi
  fi
  if command -v wget >/dev/null 2>&1; then
    wget -q --tries=5 --timeout=30 -O "$destination" "$url"
    return
  fi
  die "curl or wget is required to download $url"
}

find_ssh_service() {
  local unit
  for unit in ssh.service sshd.service; do
    if systemctl cat "$unit" >/dev/null 2>&1; then
      printf '%s\n' "$unit"
      return 0
    fi
  done
  return 1
}

enable_existing_ssh_service() {
  local unit
  if unit="$(find_ssh_service)"; then
    root_run systemctl enable --now "$unit"
    return 0
  fi
  return 1
}

install_packages() {
  local packages=()
  local ssh_service_ready=0

  if ! download_tool_available; then
    packages+=(curl)
  fi
  if ! command -v tar >/dev/null 2>&1; then
    packages+=(tar)
  fi
  if [[ "$INSTALL_OPENSSH" == "1" && "$LOCAL_PORT" == "22" ]]; then
    if enable_existing_ssh_service; then
      ssh_service_ready=1
    else
      packages+=(openssh-server)
    fi
  fi

  if [[ "$SKIP_PACKAGE_INSTALL" == "1" ]]; then
    require_download_tool
    command -v tar >/dev/null 2>&1 || die "tar is required when package installation is skipped"
    if [[ "$INSTALL_OPENSSH" == "1" && "$LOCAL_PORT" == "22" && "$ssh_service_ready" != "1" ]]; then
      printf 'warning: openssh-server service was not found; remote port %s may not reach SSH until local port 22 is listening.\n' "$REMOTE_PORT" >&2
    fi
    return
  fi

  if ((${#packages[@]} == 0)); then
    return
  fi

  if command -v apt-get >/dev/null 2>&1; then
    root_run apt-get update
    root_run env DEBIAN_FRONTEND=noninteractive apt-get install -y "${packages[@]}"
    if [[ "$INSTALL_OPENSSH" == "1" && "$LOCAL_PORT" == "22" ]]; then
      enable_existing_ssh_service || die "openssh-server installed but ssh.service/sshd.service was not found"
    fi
    return
  fi

  require_download_tool
  command -v tar >/dev/null 2>&1 || die "tar is required"
}

install_frpc_binary() {
  local arch tmp url archive extracted
  arch="$(detect_frp_arch)"
  tmp="$(mktemp -d)"
  url="$(frp_archive_url "$arch")"
  archive="$tmp/frp.tar.gz"
  extracted="$tmp/frp_${FRP_VERSION}_linux_${arch}"

  trap 'rm -rf "$tmp"' RETURN

  download_file "$url" "$archive"
  tar -xzf "$archive" -C "$tmp"
  [[ -x "$extracted/frpc" ]] || die "frpc binary not found in release archive: $url"
  root_run install -m 0755 "$extracted/frpc" /usr/local/bin/frpc
}

backup_if_exists() {
  local path="$1"
  local stamp
  stamp="$(date +%Y%m%d%H%M%S)"
  if root_run test -e "$path"; then
    root_run cp -a "$path" "$path.bak.$stamp"
    printf 'backed up %s to %s.bak.%s\n' "$path" "$path" "$stamp"
  fi
}

write_managed_files() {
  local config_tmp service_tmp
  config_tmp="$(mktemp)"
  service_tmp="$(mktemp)"
  trap 'rm -f "$config_tmp" "$service_tmp"' RETURN

  render_config >"$config_tmp"
  render_service >"$service_tmp"

  root_run install -d -m 0755 /etc/frp
  backup_if_exists "$CONFIG_PATH"
  backup_if_exists "$SERVICE_PATH"
  root_run install -m 0600 "$config_tmp" "$CONFIG_PATH"
  root_run install -m 0644 "$service_tmp" "$SERVICE_PATH"
}

apply_install() {
  [[ "$(uname -s)" == "Linux" ]] || die "apply mode must run on the Ubuntu/Linux frpc client"
  command -v systemctl >/dev/null 2>&1 || die "systemd systemctl is required"

  install_packages
  install_frpc_binary
  write_managed_files

  root_run /usr/local/bin/frpc verify -c "$CONFIG_PATH"
  root_run systemctl daemon-reload
  root_run systemctl enable --now frpc.service

  cat <<EOF
frpc is installed and frpc.service was started.

Client-side checks:
  sudo systemctl status frpc.service --no-pager
  sudo journalctl -u frpc.service -n 80 --no-pager

External check after frps allowPorts and cloud security group allow $REMOTE_PORT/tcp:
  ssh -p $REMOTE_PORT <ubuntu-user>@$SERVER_ADDR hostname
EOF
}

main() {
  parse_args "$@"
  load_token
  validate_inputs

  if [[ "$DRY_RUN" == "1" ]]; then
    print_dry_run
    return
  fi

  apply_install
}

main "$@"
