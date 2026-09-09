#!/usr/bin/env bash
set -euo pipefail

script_dir="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(CDPATH= cd -- "$script_dir/../.." && pwd)"
cxx_bin="${CXX:-c++}"

if ! command -v "$cxx_bin" >/dev/null 2>&1; then
  printf 'test_support_header=skipped reason=cxx_unavailable compiler=%s\n' "$cxx_bin"
  exit 0
fi

temporary_root="$(mktemp -d "${TMPDIR:-/tmp}/mine-teleop-test-support.XXXXXX")"
cleanup() {
  rm -rf -- "$temporary_root"
}
trap cleanup EXIT

probe_binary="$temporary_root/test-support-probe"
"$cxx_bin" -std=c++17 -I "$repo_root" -x c++ -o "$probe_binary" - <<'CPP'
#include "cpp/tests/test_support.hpp"

#include <stdexcept>
#include <string>

bool has_file_and_line(const std::string& message) {
  const auto first_separator = message.find(':');
  if (first_separator == std::string::npos) return false;
  const auto second_separator = message.find(':', first_separator + 1);
  if (second_separator == std::string::npos || second_separator == first_separator + 1) {
    return false;
  }
  return message.find_first_not_of("0123456789", first_separator + 1) == second_separator;
}

int main() {
  MINE_TELEOP_EXPECT(true, "a passing expectation failed");
  MINE_TELEOP_EXPECT_THROWS(
      [] { throw std::runtime_error("expected exception"); },
      "an expected standard exception was not accepted");

  try {
    MINE_TELEOP_EXPECT(false, "location-bearing failure");
  } catch (const mine_teleop::test::TestFailure& failure) {
    const std::string message = failure.what();
    if (message.find("location-bearing failure") == std::string::npos ||
        !has_file_and_line(message)) {
      return 2;
    }
  }

  try {
    MINE_TELEOP_EXPECT_THROWS([] {}, "missing exception");
  } catch (const mine_teleop::test::TestFailure& failure) {
    const std::string message = failure.what();
    if (message.find("missing exception") == std::string::npos || !has_file_and_line(message)) {
      return 3;
    }
    return 0;
  }
  return 4;
}
CPP

"$probe_binary"
printf 'test_support_header=passed compiler=%s standard=c++17\n' "$cxx_bin"
