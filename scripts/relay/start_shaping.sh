#!/usr/bin/env bash
set -euo pipefail
: "${MINE_TELEOP_RELAY_INTERFACE:?public bottleneck interface must be configured}"
exec "$(dirname -- "$0")/shape_egress.sh" --apply "$MINE_TELEOP_RELAY_INTERFACE" "$(id -u mine-teleop-relay)"
