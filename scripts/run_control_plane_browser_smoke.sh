#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/.." && pwd)"
artifact_dir="${MINE_TELEOP_BROWSER_SMOKE_ARTIFACT_DIR:-$repo_root/.local/control-plane-browser-smoke/$(date +%Y%m%d-%H%M%S)}"

mkdir -p "$artifact_dir"
MINE_TELEOP_CONTROL_SMOKE_ARTIFACT_DIR="$artifact_dir" \
  "$repo_root/scripts/run_control_plane_docker_smoke.sh"
printf 'BROWSER_SMOKE_ARTIFACT_DIR=%s\n' "$artifact_dir"
printf 'Native HTTP console route, control path, and rendered JPEG endpoint passed; interactive browser automation is intentionally external to the production bundle.\n'
