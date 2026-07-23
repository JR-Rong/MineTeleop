#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
output_dir="${1:-$repo_root/dist}"
host_machine_arch="$(uname -m)"
requested_arch="${MINE_TELEOP_MACOS_ARCH:-$host_machine_arch}"
case "$requested_arch" in
  arm64) package_arch="arm64"; cmake_arch="arm64" ;;
  x64|x86_64) package_arch="x64"; cmake_arch="x86_64" ;;
  *) echo "unsupported MINE_TELEOP_MACOS_ARCH: $requested_arch (expected arm64 or x64)" >&2; exit 2 ;;
esac
skip_run_tests="${MINE_TELEOP_SKIP_RUN_TESTS:-0}"
if [[ "$skip_run_tests" != "0" && "$skip_run_tests" != "1" ]]; then
  echo "MINE_TELEOP_SKIP_RUN_TESTS must be 0 or 1" >&2
  exit 2
fi
if [[ "$cmake_arch" != "$host_machine_arch" && "$skip_run_tests" != "1" ]]; then
  echo "target $package_arch cannot be executed on host $host_machine_arch; install Rosetta or set MINE_TELEOP_SKIP_RUN_TESTS=1 for a clearly marked build-only package" >&2
  exit 2
fi

build_root="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-control-macos.XXXXXX")"
reused_build_dir="no"
if [[ -n "${MINE_TELEOP_MACOS_BUILD_DIR:-}" ]]; then
  build_dir="$(cd "$MINE_TELEOP_MACOS_BUILD_DIR" && pwd)"
  reused_build_dir="yes"
else
  build_dir="$build_root/build"
fi
package_name="mine-teleop-control-macos-${package_arch}-$(date -u +%Y%m%d-%H%M%S)"
package_root="$build_root/$package_name"
mkdir -p "$output_dir" "$package_root/bin" "$package_root/lib" "$package_root/config" \
  "$package_root/certs" "$package_root/protocol/v1"

cmake_args=(
  -S "$repo_root"
  -B "$build_dir"
  -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_OSX_ARCHITECTURES=$cmake_arch"
  -DMINE_TELEOP_BUILD_VEHICLE_RUNTIME=OFF
  -DMINE_TELEOP_BUILD_CONTROL_CLIENT=ON
  -DMINE_TELEOP_BUILD_SIGNALING_SERVER=OFF
  -DMINE_TELEOP_BUILD_TESTS=ON
  -DMINE_TELEOP_FETCH_MISSING_DEPS=ON
)
if [[ -n "${FETCHCONTENT_SOURCE_DIR_YAML_CPP:-}" ]]; then
  cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_YAML_CPP=$FETCHCONTENT_SOURCE_DIR_YAML_CPP")
fi
if [[ -n "${FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON:-}" ]]; then
  cmake_args+=("-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=$FETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON")
fi

if [[ "$reused_build_dir" == "no" ]]; then
  cmake "${cmake_args[@]}"
elif [[ ! -f "$build_dir/CMakeCache.txt" ]]; then
  echo "MINE_TELEOP_MACOS_BUILD_DIR is not a configured CMake build directory: $build_dir" >&2
  exit 2
fi
cmake --build "$build_dir" --parallel "${MINE_TELEOP_BUILD_JOBS:-4}"
tests_executed="yes"
if [[ "$skip_run_tests" == "1" ]]; then
  tests_executed="no (cross-compiled build only)"
  echo "warning: runtime tests skipped for $package_arch" >&2
else
  ctest --test-dir "$build_dir" --output-on-failure
fi

install -m 0755 "$build_dir/mine-teleop-control" "$package_root/bin/mine-teleop-control"
install -m 0755 "$repo_root/packaging/macos/run-control.command" "$package_root/run-control.command"
install -m 0644 "$repo_root/packaging/macos/README.txt" "$package_root/README.txt"
install -m 0644 "$repo_root/configs/driver-console.dev.yaml" "$package_root/config/driver-console.yaml"
install -m 0644 \
  "$repo_root/configs/driver-console.three-machine.dev.yaml" \
  "$package_root/config/driver-console.three-machine.yaml"
install -m 0644 \
  "$repo_root/configs/mine-teleop-field-root.crt" \
  "$package_root/config/mine-teleop-field-root.crt"
install -m 0644 /etc/ssl/cert.pem "$package_root/certs/cacert.pem"
cp -R "$repo_root/protocol/v1/." "$package_root/protocol/v1/"

printf '%s\n' \
  "target_arch=$package_arch" \
  "host_arch=$host_machine_arch" \
  "runtime_tests_executed=$tests_executed" \
  "reused_build_dir=$reused_build_dir" \
  "built_at_utc=$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
  "code_signing=ad-hoc" \
  >"$package_root/BUILD-INFO.txt"

codesign --force --sign - "$package_root/bin/mine-teleop-control"
codesign --verify --strict "$package_root/bin/mine-teleop-control"

if otool -L "$package_root/bin/mine-teleop-control" | grep -E '/opt/homebrew|/usr/local' >/dev/null; then
  echo "control executable still references a package-manager path" >&2
  exit 2
fi
if otool -L "$package_root/bin/mine-teleop-control" | grep -E 'libcrypto|libssl' >/dev/null; then
  echo "control executable unexpectedly references OpenSSL on macOS" >&2
  exit 2
fi

archive="$output_dir/$package_name.tar.gz"
tar -czf "$archive" -C "$build_root" "$package_name"
shasum -a 256 "$archive" | tee "$archive.sha256"
"$repo_root/scripts/check_macos_control_bundle.sh" "$archive"
echo "macos_control_bundle=$archive"
echo "macos_control_bundle_root=$package_root"
