#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# shellcheck disable=SC1090
source "$repo_root/deployments/base-images.lock.env"
run_tests="OFF"
positional_args=()
for argument in "$@"; do
  if [[ "$argument" == "test" ]]; then
    run_tests="ON"
  else
    positional_args+=("$argument")
  fi
done
if [[ "${#positional_args[@]}" -gt 2 ]]; then
  printf 'usage: %s [test] [platform] [output-directory]\n' "$0" >&2
  exit 2
fi
platform="${positional_args[0]:-linux/amd64}"
architecture="${platform#linux/}"
case "$architecture" in
  amd64) package_architecture="x64" ;;
  arm64) package_architecture="arm64" ;;
  *) echo "unsupported architecture: $architecture" >&2; exit 2 ;;
esac
package_timestamp="$(date -u +%Y%m%d-%H%M%S)"
output_dir="${positional_args[1]:-$repo_root/dist}"
package_name="mine-teleop-control-ubuntu22.04-${package_architecture}-${package_timestamp}"
output_root="$output_dir/$package_name"
build_jobs="${MINE_TELEOP_BUILD_JOBS:-$(nproc)}"
source_commit="$(git -C "$repo_root" rev-parse HEAD 2>/dev/null || printf unknown)"
if git -C "$repo_root" status --porcelain >/dev/null 2>&1; then
  if [[ -n "$(git -C "$repo_root" status --porcelain)" ]]; then
    source_tree_state=dirty
  else
    source_tree_state=clean
  fi
else
  source_tree_state=unavailable
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker is required but not found" >&2
  exit 1
fi

temporary="$(mktemp -d)"
cleanup() { rm -rf "$temporary"; }
trap cleanup EXIT

# ------------------------------------------------------------------
# Docker build
# ------------------------------------------------------------------
echo "==> Building control client (platform=$platform, jobs=$build_jobs, tests=$run_tests)"
docker buildx build \
  --platform "$platform" \
  --build-arg "MINE_TELEOP_BUILD_JOBS=$build_jobs" \
  --build-arg "MINE_TELEOP_BUILD_TESTS=$run_tests" \
  --target artifact \
  --output "type=local,dest=$temporary/artifact" \
  -f "$repo_root/deployments/cpp/Dockerfile.control" \
  "$repo_root"

# ------------------------------------------------------------------
# Package
# ------------------------------------------------------------------
mkdir -p "$output_root/bin" "$output_root/lib" "$output_root/config" \
  "$output_root/certs" "$output_root/protocol/v1"

install -m 0755 "$temporary/artifact/bin/mine-teleop-control" "$output_root/bin/mine-teleop-control"
install -m 0644 "$repo_root/configs/driver-console.dev.yaml" "$output_root/config/driver-console.yaml"
install -m 0644 \
  "$repo_root/configs/driver-console.three-machine.dev.yaml" \
  "$output_root/config/driver-console.three-machine.yaml"
install -m 0644 \
  "$repo_root/configs/mine-teleop-field-root.crt" \
  "$output_root/config/mine-teleop-field-root.crt"
install -m 0644 /etc/ssl/certs/ca-certificates.crt "$output_root/certs/cacert.pem"
cp -R "$repo_root/protocol/v1/." "$output_root/protocol/v1/"

# Launcher script
cat > "$output_root/mine-teleop-control" <<'LAUNCHER'
#!/usr/bin/env bash
set -euo pipefail
root="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
export LD_LIBRARY_PATH="$root/lib:${LD_LIBRARY_PATH:-}"
export SSL_CERT_FILE="${SSL_CERT_FILE:-$root/certs/cacert.pem}"
exec "$root/bin/mine-teleop-control" "$@"
LAUNCHER
chmod +x "$output_root/mine-teleop-control"

printf '%s\n' \
  "source_commit=$source_commit" \
  "source_tree_state=$source_tree_state" \
  "target_platform=$platform" \
  "target_architecture=$architecture" \
  "runtime_tests_executed=$([[ "$run_tests" == "ON" ]] && printf yes || printf no)" \
  'build_hardening=target-scoped' \
  "base_image_reference=$MINE_TELEOP_UBUNTU_2204_REFERENCE" \
  "base_image_index_digest=$MINE_TELEOP_UBUNTU_2204_INDEX_DIGEST" \
  'reproducibility_level=dependency-traceable' \
  'offline_rebuild=not-established' \
  'bit_for_bit_reproducible=not-established' \
  "built_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  >"$output_root/BUILD-INFO.txt"

# ------------------------------------------------------------------
# Archive
# ------------------------------------------------------------------
archive="$output_root.tar.gz"
tar --no-xattrs -C "$output_dir" -czf "$archive" "$package_name"
if command -v sha256sum >/dev/null 2>&1; then
  sha256sum "$archive" > "$archive.sha256"
else
  shasum -a 256 "$archive" > "$archive.sha256"
fi

printf 'BUNDLE_DIR=%s\n' "$output_root"
printf 'BUNDLE_ARCHIVE=%s\n' "$archive"
