#!/usr/bin/env bash
set -euo pipefail
# Replaces the legacy static-secret probe. Use the version-pinned feature image
# or an Ubuntu 24.04 test container; never exercise a production allocation.
exec python3 "$(dirname -- "$0")/relay_manager_test.py" "$@"
