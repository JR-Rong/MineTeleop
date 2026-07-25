#!/usr/bin/env bash
set -euo pipefail

archive="${1:-}"
if [[ -z "$archive" || ! -f "$archive" ]]; then
  printf 'usage: %s /path/to/mine-teleop-vehicle-ubuntu22.04-ARCH-*.tar.gz\n' "$0" >&2
  exit 2
fi
archive="$(cd "$(dirname "$archive")" && pwd)/$(basename "$archive")"
checksum_file="$archive.sha256"
if [[ ! -f "$checksum_file" ]]; then
  printf 'bundle checksum is missing: %s\n' "$checksum_file" >&2
  exit 2
fi

expected_checksum="$(awk 'NR == 1 {print $1}' "$checksum_file")"
if command -v sha256sum >/dev/null 2>&1; then
  actual_checksum="$(sha256sum "$archive" | awk '{print $1}')"
else
  actual_checksum="$(shasum -a 256 "$archive" | awk '{print $1}')"
fi
if [[ -z "$expected_checksum" || "$actual_checksum" != "$expected_checksum" ]]; then
  printf 'bundle checksum mismatch: expected=%s actual=%s\n' "$expected_checksum" "$actual_checksum" >&2
  exit 2
fi

entries="$(tar -tzf "$archive")"
if printf '%s\n' "$entries" | awk '/(^\/|(^|\/)\.\.(\/|$))/ {found=1} END {exit !found}'; then
  printf 'bundle contains an unsafe archive path\n' >&2
  exit 2
fi
package_name="$(printf '%s\n' "$entries" | sed -n '1s#/.*##p')"
if [[ -z "$package_name" ]] || ! printf '%s\n' "$entries" | awk -F/ -v root="$package_name" '$1 != root {invalid=1} END {exit invalid}'; then
  printf 'bundle must contain exactly one top-level package directory\n' >&2
  exit 2
fi
for required_path in \
  "$package_name/bin/mine-teleop" \
  "$package_name/bin/mine-teleop-run" \
  "$package_name/bin/mine-teleop-aravis-camera" \
  "$package_name/config/vehicle-agent.yaml" \
  "$package_name/config/mine-teleop-field-root.crt" \
  "$package_name/deployments/udev/99-mine-teleop-basler.rules" \
  "$package_name/scripts/setup_basler_usb_access.sh" \
  "$package_name/BUILD-INFO.txt" \
  "$package_name/README.txt"; do
  if ! printf '%s\n' "$entries" | awk -v target="$required_path" '$0 == target {found=1} END {exit !found}'; then
    printf 'required bundle path is missing: %s\n' "$required_path" >&2
    exit 2
  fi
done
if printf '%s\n' "$entries" | awk 'tolower($0) ~ /(^|\/)(device-token|driver-password|turn-static-auth\.secret)$/ {found=1} END {exit !found}'; then
  printf 'bundle contains a credential file\n' >&2
  exit 2
fi

container_id=""
cleanup() {
  if [[ -n "$container_id" ]]; then
    docker rm -f "$container_id" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

container_id="$(docker create --platform linux/amd64 \
  --entrypoint /bin/sh \
  ubuntu:22.04 \
  -euc '
    mkdir -p /tmp/check
    tar -xzf /tmp/mine-teleop-bundle.tar.gz -C /tmp/check
    package_name="$(tar -tzf /tmp/mine-teleop-bundle.tar.gz | sed -n "1s#/.*##p")"
    root="/tmp/check/$package_name"
    test -s "$root/config/vehicle-agent.yaml"
    test -s "$root/config/mine-teleop-field-root.crt"
    test ! -e "$root/config/device-token"
    "$root/bin/mine-teleop-run" version
    "$root/bin/mine-teleop-run" config-check --config "$root/config/vehicle-agent.yaml"
    library_path="$root/lib:$root/lib/vendor/chassis:$root/lib/vendor/mvs"
    export LD_LIBRARY_PATH="$library_path"
    "$root/bin/mine-teleop-signaling-server" --version
    "$root/bin/mine-teleop-control" --version
    "$root/bin/mine-teleop-aravis-camera" --list --json | grep -E "\"device_count\":[0-9]+" >/dev/null
    export GST_PLUGIN_SYSTEM_PATH_1_0=
    export GST_PLUGIN_PATH_1_0="$root/lib/gstreamer-1.0"
    export GST_PLUGIN_SCANNER="$root/bin/gst-plugin-scanner"
    export GST_REGISTRY_FORK=no
    export GST_REGISTRY=/tmp/mine-teleop-gstreamer-registry.bin
    for plugin in webrtcbin srtpenc sctpenc sctpdec dtlsenc dtlsdec nicesrc v4l2src va nvcodec h264parse rtph264pay; do
      "$root/bin/gst-inspect-1.0" "$plugin" >/dev/null
    done
  ')"
docker cp "$archive" "$container_id:/tmp/mine-teleop-bundle.tar.gz"
docker start --attach "$container_id"
docker rm "$container_id" >/dev/null
container_id=""
trap - EXIT

printf 'ubuntu_bundle_check=passed archive=%s sha256=%s\n' "$archive" "$actual_checksum"
