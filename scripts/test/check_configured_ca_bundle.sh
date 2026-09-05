#!/usr/bin/env bash
set -euo pipefail

runtime="${1:?mine-teleop runtime path is required}"
source_config="${2:?field vehicle config path is required}"
source_ca="${3:?field CA path is required}"
fixture_root="$(mktemp -d)"

cleanup() {
  rm -rf -- "$fixture_root"
}
trap cleanup EXIT

cp "$source_config" "$fixture_root/vehicle-agent.yaml"
if "$runtime" config-check \
  --config "$fixture_root/vehicle-agent.yaml" \
  --verify-configured-ca-bundle >/dev/null 2>&1; then
  printf 'configured CA verification accepted a missing file\n' >&2
  exit 1
fi

cp "$source_ca" "$fixture_root/mine-teleop-field-root.crt"
result="$(
  "$runtime" config-check \
    --config "$fixture_root/vehicle-agent.yaml" \
    --verify-configured-ca-bundle
)"
[[ "$result" == *'"ca_bundle_file_checked":true'* && "$result" == *'"passed":true'* ]] || {
  printf 'configured CA verification did not report success: %s\n' "$result" >&2
  exit 1
}

printf 'configured_ca_bundle_check=passed\n'
