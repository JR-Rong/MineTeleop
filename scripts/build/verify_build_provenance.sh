#!/usr/bin/env bash
set -euo pipefail

export LC_ALL=C

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
source_dir="$repo_root"
build_dir=""
output_dir=""
source_revision=""
check_lock_only=no
dynamic_elfs=()
static_elfs=()
artifacts=()

usage() {
  cat <<'EOF'
Usage:
  verify_build_provenance.sh --check-lock [--source-dir DIRECTORY]
  verify_build_provenance.sh --build-dir DIRECTORY --output-dir DIRECTORY \
    [--source-dir DIRECTORY] [--source-revision REVISION] \
    [--dynamic-elf PATH]... [--static-elf PATH]... [--artifact PATH]...

The first form verifies that every Ubuntu Dockerfile uses the repository's
verified OCI index lock. The second records source, lock, compiler, CMake and
artifact inputs, and verifies requested Linux ELF files with readelf.

Use --dynamic-elf for PIE executables and shared objects. Use --static-elf
for mine-teleop-run or another intentionally static executable:
it is checked for the absence of an interpreter and dynamic dependencies, not
for PIE/RELRO/BIND_NOW properties that do not apply to a static ELF.
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 2
}

sha256_file() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | awk '{print $1}'
  else
    shasum -a 256 "$1" | awk '{print $1}'
  fi
}

cache_value() {
  local variable="$1"
  local cache_file="$2"
  sed -n -E "s|^${variable}:[^=]*=(.*)$|\\1|p" "$cache_file" | head -n 1
}

check_base_image_lock() {
  local lock_file="$source_dir/deployments/base-images.lock.env"
  [[ -f "$lock_file" ]] || die "base image lock is missing: $lock_file"
  # This repository-controlled file contains only shell assignments and is
  # deliberately consumed by its build and verification scripts.
  # shellcheck disable=SC1090
  source "$lock_file"
  [[ -n "${MINE_TELEOP_UBUNTU_2204_REFERENCE:-}" ]] || die "base image reference is empty"
  [[ "$MINE_TELEOP_UBUNTU_2204_REFERENCE" == *"@${MINE_TELEOP_UBUNTU_2204_INDEX_DIGEST:-}" ]] ||
    die "base image reference and index digest disagree"

  local dockerfile
  local dockerfiles=(
    deployments/chassis-control-bridge/Dockerfile.build
    deployments/cloud/Dockerfile.build
    deployments/container/Dockerfile.control
    deployments/cpp/Dockerfile.build
    deployments/cpp/Dockerfile.control
    deployments/cpp/Dockerfile.macos-from-scratch
  )
  for dockerfile in "${dockerfiles[@]}"; do
    local path="$source_dir/$dockerfile"
    [[ -f "$path" ]] || die "expected Dockerfile is missing: $path"
    grep -Fqx "ARG MINE_TELEOP_UBUNTU_BASE=$MINE_TELEOP_UBUNTU_2204_REFERENCE" "$path" ||
      die "Dockerfile base-image default does not match lock: $dockerfile"
    grep -Fq 'FROM ' "$path" || die "Dockerfile has no FROM directive: $dockerfile"
  done
  grep -Fqx 'COPY cmake/Hardening.cmake cmake/Hardening.cmake' \
    "$source_dir/deployments/chassis-control-bridge/Dockerfile.build" ||
    die "the standalone chassis bridge Docker context omits cmake/Hardening.cmake"

  printf 'base_image_lock=passed reference=%s resolved_at=%s\n' \
    "$MINE_TELEOP_UBUNTU_2204_REFERENCE" \
    "${MINE_TELEOP_UBUNTU_2204_RESOLVED_AT_UTC:-unknown}"
}

while [[ "$#" -gt 0 ]]; do
  case "$1" in
    --check-lock)
      check_lock_only=yes
      shift
      ;;
    --source-dir)
      [[ "$#" -ge 2 ]] || die "--source-dir requires a value"
      source_dir="$2"
      shift 2
      ;;
    --source-revision)
      [[ "$#" -ge 2 ]] || die "--source-revision requires a value"
      source_revision="$2"
      shift 2
      ;;
    --build-dir)
      [[ "$#" -ge 2 ]] || die "--build-dir requires a value"
      build_dir="$2"
      shift 2
      ;;
    --output-dir)
      [[ "$#" -ge 2 ]] || die "--output-dir requires a value"
      output_dir="$2"
      shift 2
      ;;
    --dynamic-elf)
      [[ "$#" -ge 2 ]] || die "--dynamic-elf requires a value"
      dynamic_elfs+=("$2")
      shift 2
      ;;
    --static-elf)
      [[ "$#" -ge 2 ]] || die "--static-elf requires a value"
      static_elfs+=("$2")
      shift 2
      ;;
    --artifact)
      [[ "$#" -ge 2 ]] || die "--artifact requires a value"
      artifacts+=("$2")
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      die "unknown argument: $1"
      ;;
  esac
done

source_dir="$(CDPATH= cd -- "$source_dir" && pwd)"

if [[ "$check_lock_only" == yes ]]; then
  [[ -z "$build_dir$output_dir$source_revision" && "${#dynamic_elfs[@]}" -eq 0 &&
     "${#static_elfs[@]}" -eq 0 && "${#artifacts[@]}" -eq 0 ]] ||
    die "--check-lock cannot be combined with build evidence arguments"
  check_base_image_lock
  exit 0
fi

[[ -n "$build_dir" ]] || die "--build-dir is required"
[[ -n "$output_dir" ]] || die "--output-dir is required"
[[ "${#dynamic_elfs[@]}" -gt 0 || "${#static_elfs[@]}" -gt 0 ]] ||
  die "at least one --dynamic-elf or --static-elf is required"
[[ -d "$build_dir" ]] || die "build directory does not exist: $build_dir"
[[ -f "$build_dir/CMakeCache.txt" ]] || die "CMakeCache.txt is missing: $build_dir"
[[ ! -e "$output_dir" ]] || die "output directory already exists: $output_dir"
command -v readelf >/dev/null 2>&1 || die "readelf is required for ELF verification"

check_base_image_lock

mkdir -p "$output_dir/elf"
cache_file="$build_dir/CMakeCache.txt"

if [[ -z "$source_revision" ]]; then
  if git -C "$source_dir" rev-parse HEAD >/dev/null 2>&1; then
    source_revision="$(git -C "$source_dir" rev-parse HEAD)"
  else
    source_revision=unavailable
  fi
fi

if git -C "$source_dir" status --porcelain >/dev/null 2>&1; then
  if [[ -n "$(git -C "$source_dir" status --porcelain)" ]]; then
    source_tree_state=dirty
  else
    source_tree_state=clean
  fi
else
  source_tree_state=unavailable
fi

cxx_compiler="$(cache_value CMAKE_CXX_COMPILER "$cache_file")"
{
  printf 'generated_at_utc=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'source_revision=%s\n' "$source_revision"
  printf 'source_tree_state=%s\n' "$source_tree_state"
  printf 'cmake_generator=%s\n' "$(cache_value CMAKE_GENERATOR "$cache_file")"
  printf 'cmake_build_type=%s\n' "$(cache_value CMAKE_BUILD_TYPE "$cache_file")"
  printf 'cxx_compiler=%s\n' "$cxx_compiler"
  printf 'hardening_enabled=%s\n' "$(cache_value MINE_TELEOP_ENABLE_HARDENING "$cache_file")"
  printf 'sanitizers_enabled=%s\n' "$(cache_value MINE_TELEOP_ENABLE_SANITIZERS "$cache_file")"
  printf 'sanitizer_set=%s\n' "$(cache_value MINE_TELEOP_SANITIZERS "$cache_file")"
  printf 'base_image_reference=%s\n' "$MINE_TELEOP_UBUNTU_2204_REFERENCE"
  printf 'base_image_index_digest=%s\n' "$MINE_TELEOP_UBUNTU_2204_INDEX_DIGEST"
  printf 'reproducibility_level=dependency-traceable\n'
  printf 'offline_rebuild=not-established\n'
  printf 'bit_for_bit_reproducible=not-established\n'
} > "$output_dir/inputs.env"

{
  cmake --version | sed -n '1p'
  if [[ -n "$cxx_compiler" && -x "$cxx_compiler" ]]; then
    "$cxx_compiler" --version | sed -n '1p'
  else
    printf 'compiler_version=unavailable\n'
  fi
} > "$output_dir/toolchain.txt"

if command -v dpkg-query >/dev/null 2>&1; then
  dpkg-query -W -f='${binary:Package}=${Version}\n' | sort > "$output_dir/dpkg-packages.txt"
else
  printf 'dpkg-query=unavailable\n' > "$output_dir/dpkg-packages.txt"
fi

{
  grep -E '^MINE_TELEOP_(ENABLE_HARDENING|ENABLE_SANITIZERS|SANITIZERS|HAVE_|STATIC_LAUNCHER)' \
    "$cache_file" | sort || true
  grep -E '^CMAKE_(BUILD_TYPE|CXX_COMPILER|CXX_FLAGS)' "$cache_file" | sort || true
} > "$output_dir/cmake-cache-selection.txt"

input_paths=(
  CMakeLists.txt
  CMakePresets.json
  cmake/Hardening.cmake
  deployments/base-images.lock.env
  packaging/windows/dependencies.lock.json
)
{
  for input_path in "${input_paths[@]}"; do
    if [[ -f "$source_dir/$input_path" ]]; then
      printf '%s  %s\n' "$(sha256_file "$source_dir/$input_path")" "$input_path"
    else
      printf 'missing  %s\n' "$input_path"
    fi
  done
} | sort -k2 > "$output_dir/build-inputs.sha256"

if [[ -f "$build_dir/compile_commands.json" ]]; then
  grep -Eo -- '-fstack-protector-strong|-D_FORTIFY_SOURCE=3|-fsanitize=[^ "\\]+' \
    "$build_dir/compile_commands.json" | sort -u > "$output_dir/compile-hardening-flags.txt" || true
else
  printf 'compile_commands=unavailable\n' > "$output_dir/compile-hardening-flags.txt"
fi

if [[ "$(cache_value MINE_TELEOP_HAVE_STACK_PROTECTOR_STRONG "$cache_file")" == 1 ]] &&
   [[ "$(cache_value MINE_TELEOP_ENABLE_HARDENING "$cache_file")" == ON ]] &&
   [[ -f "$build_dir/compile_commands.json" ]] &&
   ! grep -Fqx -- '-fstack-protector-strong' "$output_dir/compile-hardening-flags.txt"; then
  die "CMake detected stack-protector support but compile commands omit it"
fi

if [[ "$(cache_value MINE_TELEOP_ENABLE_SANITIZERS "$cache_file")" == ON ]] &&
   [[ -f "$build_dir/compile_commands.json" ]] &&
   ! grep -Fq -- '-fsanitize=' "$output_dir/compile-hardening-flags.txt"; then
  die "sanitizers are enabled but compile commands omit -fsanitize"
fi

verify_dynamic_elf() {
  local path="$1"
  local index="$2"
  local name
  local elf_kind
  name="$(printf '%03d-%s' "$index" "$(basename "$path")")"
  [[ -f "$path" ]] || die "dynamic ELF is missing: $path"
  readelf -W -h "$path" > "$output_dir/elf/$name.header.txt"
  readelf -W -l "$path" > "$output_dir/elf/$name.program.txt"
  readelf -W -d "$path" > "$output_dir/elf/$name.dynamic.txt"
  readelf -W -s "$path" > "$output_dir/elf/$name.symbols.txt"
  grep -Eq 'Type:[[:space:]]+DYN' "$output_dir/elf/$name.header.txt" ||
    die "dynamic ELF is not PIE/shared-object type DYN: $path"
  if grep -Fq 'Requesting program interpreter' "$output_dir/elf/$name.program.txt"; then
    elf_kind=pie-executable
  else
    grep -Fq '(SONAME)' "$output_dir/elf/$name.dynamic.txt" ||
      die "dynamic ELF is neither a PIE executable nor a shared object: $path"
    elf_kind=shared-object
  fi
  grep -Fq 'GNU_RELRO' "$output_dir/elf/$name.program.txt" ||
    die "dynamic ELF lacks GNU_RELRO: $path"
  grep -Eq 'FLAGS.*BIND_NOW|FLAGS_1.*NOW' "$output_dir/elf/$name.dynamic.txt" ||
    die "dynamic ELF lacks BIND_NOW: $path"
  if grep -Eq 'GNU_STACK.*RWE' "$output_dir/elf/$name.program.txt"; then
    die "dynamic ELF has an executable GNU_STACK: $path"
  fi
  printf 'dynamic\t%s\t%s\tkind=%s\tstack_canary_symbol=%s\n' \
    "$path" \
    "$(sha256_file "$path")" \
    "$elf_kind" \
    "$(grep -Fq '__stack_chk_fail' "$output_dir/elf/$name.symbols.txt" && printf present || printf not-found)" \
    >> "$output_dir/elf-properties.tsv"
}

verify_static_elf() {
  local path="$1"
  local index="$2"
  local name
  name="$(printf '%03d-%s' "$index" "$(basename "$path")")"
  [[ -f "$path" ]] || die "static ELF is missing: $path"
  readelf -W -h "$path" > "$output_dir/elf/$name.header.txt"
  readelf -W -l "$path" > "$output_dir/elf/$name.program.txt"
  readelf -W -d "$path" > "$output_dir/elf/$name.dynamic.txt"
  grep -Eq 'Type:[[:space:]]+EXEC' "$output_dir/elf/$name.header.txt" ||
    die "static ELF is not executable type EXEC: $path"
  grep -Fq 'Requesting program interpreter' "$output_dir/elf/$name.program.txt" &&
    die "static ELF unexpectedly has a program interpreter: $path"
  grep -Fq '(NEEDED)' "$output_dir/elf/$name.dynamic.txt" &&
    die "static ELF unexpectedly has dynamic dependencies: $path"
  printf 'static\t%s\t%s\tdynamic_elf_hardening=not-applicable\n' \
    "$path" \
    "$(sha256_file "$path")" \
    >> "$output_dir/elf-properties.tsv"
}

: > "$output_dir/elf-properties.tsv"
elf_index=0
for elf in "${dynamic_elfs[@]}"; do
  elf_index=$((elf_index + 1))
  verify_dynamic_elf "$elf" "$elf_index"
done
for elf in "${static_elfs[@]}"; do
  elf_index=$((elf_index + 1))
  verify_static_elf "$elf" "$elf_index"
done

: > "$output_dir/artifacts.sha256"
for artifact in "${artifacts[@]}"; do
  [[ -f "$artifact" ]] || die "artifact is missing: $artifact"
  printf '%s  %s\n' "$(sha256_file "$artifact")" "$artifact" >> "$output_dir/artifacts.sha256"
done

printf 'build_provenance=passed output=%s\n' "$output_dir"
