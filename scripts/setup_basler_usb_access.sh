#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
RULE_SOURCE="$REPO_ROOT/deployments/udev/99-mine-teleop-basler.rules"
RULE_DESTINATION="/etc/udev/rules.d/99-mine-teleop-basler.rules"
TARGET_USER="${1:-${SUDO_USER:-}}"

if [[ "$(id -u)" -ne 0 ]]; then
  printf 'error: run with sudo: sudo %s [vehicle-user]\n' "$0" >&2
  exit 2
fi
if [[ -z "$TARGET_USER" || "$TARGET_USER" == "root" ]]; then
  printf 'error: provide the non-root vehicle runtime user\n' >&2
  exit 2
fi
if ! id "$TARGET_USER" >/dev/null 2>&1; then
  printf 'error: user does not exist: %s\n' "$TARGET_USER" >&2
  exit 2
fi
if [[ ! -f "$RULE_SOURCE" ]]; then
  printf 'error: udev rule is missing: %s\n' "$RULE_SOURCE" >&2
  exit 2
fi

install -m 0644 "$RULE_SOURCE" "$RULE_DESTINATION"
usermod -aG video "$TARGET_USER"
udevadm control --reload-rules
udevadm trigger --action=change --subsystem-match=usb --attr-match=idVendor=2676
udevadm settle

printf 'Basler USB access installed for user %s. Reconnect the login session before starting mine-teleop.\n' "$TARGET_USER"
