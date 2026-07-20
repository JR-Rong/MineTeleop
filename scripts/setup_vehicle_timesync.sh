#!/usr/bin/env bash
set -euo pipefail

DEFAULT_NTP_SERVERS="ntp.aliyun.com ntp.tencent.com time.cloudflare.com pool.ntp.org"
NTP_SERVERS="${MINE_TELEOP_NTP_SERVERS:-$DEFAULT_NTP_SERVERS}"
TIMESYNC_BACKEND="${MINE_TELEOP_TIMESYNC_BACKEND:-auto}"
CHRONY_CONF="${MINE_TELEOP_CHRONY_CONF:-/etc/chrony/conf.d/mine-teleop.conf}"
TIMESYNCD_CONF="${MINE_TELEOP_TIMESYNCD_CONF:-/etc/systemd/timesyncd.conf.d/mine-teleop.conf}"
CHECK_ONLY=0

usage() {
  cat <<'USAGE'
Usage: scripts/setup_vehicle_timesync.sh [--check] [--backend auto|timesyncd|chrony] [--servers "host1 host2"]

Configures NTP time sync on the vehicle host. In auto mode the script uses an
existing chrony installation when present; otherwise it configures
systemd-timesyncd. Force chrony only when the target package state allows
installing it. The script uses sudo when it is not already running as root and
never embeds a password.

Environment:
  MINE_TELEOP_NTP_SERVERS   Space-separated NTP server list.
  MINE_TELEOP_TIMESYNC_BACKEND
                            auto, timesyncd, or chrony. Default auto.
  MINE_TELEOP_CHRONY_CONF   chrony config path, default /etc/chrony/conf.d/mine-teleop.conf.
  MINE_TELEOP_TIMESYNCD_CONF
                            systemd-timesyncd config path.
USAGE
}

while (($#)); do
  case "$1" in
    --check)
      CHECK_ONLY=1
      shift
      ;;
    --servers)
      NTP_SERVERS="${2:?--servers requires a value}"
      shift 2
      ;;
    --backend)
      TIMESYNC_BACKEND="${2:?--backend requires a value}"
      shift 2
      ;;
    --conf)
      CHRONY_CONF="${2:?--conf requires a value}"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 2
      ;;
  esac
done

run_sudo() {
  if [[ "$(id -u)" == "0" ]]; then
    "$@"
  else
    sudo "$@"
  fi
}

print_timesync_status() {
  echo "== timedatectl status =="
  timedatectl status || true
  echo
  echo "== chronyc tracking =="
  if command -v chronyc >/dev/null 2>&1; then
    chronyc tracking || true
  else
    echo "chronyc is not installed; using timedatectl/systemd-timesyncd status when chrony is unavailable."
  fi
  echo
  echo "== chronyc sources -v =="
  if command -v chronyc >/dev/null 2>&1; then
    chronyc sources -v || true
  else
    echo "chronyc is not installed; no chrony sources to print."
  fi
  echo
  echo "== timedatectl timesync-status =="
  timedatectl timesync-status || true
}

install_chrony_if_needed() {
  if command -v chronyc >/dev/null 2>&1; then
    return
  fi
  if ! command -v apt-get >/dev/null 2>&1; then
    echo "chrony is missing and apt-get is unavailable; install chrony manually first" >&2
    exit 2
  fi
  run_sudo apt-get update
  run_sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends chrony
}

ensure_chrony_confdir() {
  local main_conf="/etc/chrony/chrony.conf"
  if [[ ! -f "$main_conf" ]]; then
    return
  fi
  if grep -Eq '^[[:space:]]*(confdir|include)[[:space:]]+/etc/chrony/conf\.d' "$main_conf"; then
    return
  fi
  echo "confdir /etc/chrony/conf.d" | run_sudo tee -a "$main_conf" >/dev/null
}

write_mine_teleop_chrony_conf() {
  local tmp
  tmp="$(mktemp)"
  {
    echo "# Managed by MineTeleop. Override servers with MINE_TELEOP_NTP_SERVERS."
    echo "# Generated at $(date -u +%Y-%m-%dT%H:%M:%SZ)."
    for server in $NTP_SERVERS; do
      echo "server $server iburst"
    done
    echo "makestep 0.1 3"
    echo "rtcsync"
  } >"$tmp"
  run_sudo mkdir -p "$(dirname "$CHRONY_CONF")"
  run_sudo install -m 0644 "$tmp" "$CHRONY_CONF"
  rm -f "$tmp"
}

configure_chrony() {
  install_chrony_if_needed
  ensure_chrony_confdir
  write_mine_teleop_chrony_conf
  if command -v systemctl >/dev/null 2>&1; then
    run_sudo systemctl enable --now chrony
    run_sudo systemctl restart chrony
  else
    run_sudo service chrony restart
  fi
  chronyc -a makestep || true
}

write_mine_teleop_timesyncd_conf() {
  local tmp
  tmp="$(mktemp)"
  {
    echo "# Managed by MineTeleop. Override servers with MINE_TELEOP_NTP_SERVERS."
    echo "# Generated at $(date -u +%Y-%m-%dT%H:%M:%SZ)."
    echo "[Time]"
    echo "NTP=$NTP_SERVERS"
    echo "FallbackNTP=pool.ntp.org"
  } >"$tmp"
  run_sudo mkdir -p "$(dirname "$TIMESYNCD_CONF")"
  run_sudo install -m 0644 "$tmp" "$TIMESYNCD_CONF"
  rm -f "$tmp"
}

configure_timesyncd() {
  if ! command -v timedatectl >/dev/null 2>&1; then
    echo "timedatectl is unavailable; set MINE_TELEOP_TIMESYNC_BACKEND=chrony or configure NTP manually" >&2
    exit 2
  fi
  write_mine_teleop_timesyncd_conf
  if command -v systemctl >/dev/null 2>&1; then
    run_sudo systemctl enable --now systemd-timesyncd
    run_sudo systemctl restart systemd-timesyncd
  else
    run_sudo service systemd-timesyncd restart
  fi
  run_sudo timedatectl set-ntp true || true
}

if [[ "$CHECK_ONLY" == "1" ]]; then
  print_timesync_status
  exit 0
fi

case "$TIMESYNC_BACKEND" in
  auto)
    if command -v chronyc >/dev/null 2>&1; then
      configure_chrony
    else
      configure_timesyncd
    fi
    ;;
  chrony)
    configure_chrony
    ;;
  timesyncd|systemd-timesyncd|ntp)
    configure_timesyncd
    ;;
  *)
    echo "Unsupported backend: $TIMESYNC_BACKEND" >&2
    usage >&2
    exit 2
    ;;
esac
print_timesync_status
