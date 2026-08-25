#include "mine_teleop/core.hpp"
#include "mine_teleop_chassis_bridge.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void expect(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

std::string expected_mismatch(
    std::uint32_t actual_version,
    std::uint32_t actual_v3_size) {
  return
      "chassis bridge ABI mismatch: expected version 3 and V3 config size " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV3)) +
      ", got version " + std::to_string(actual_version) + " and size " +
      std::to_string(actual_v3_size) + "; legacy V2 size expected " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV2)) + ", got " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV2));
}

void expect_rejected(
    const std::filesystem::path& library_path,
    std::uint32_t actual_version,
    std::uint32_t actual_v3_size) {
  expect(
      std::filesystem::is_regular_file(library_path),
      "ABI fixture shared library is missing");
  const auto expected = expected_mismatch(actual_version, actual_v3_size);
  try {
    mine_teleop::validate_chassis_bridge_abi(library_path);
  } catch (const std::runtime_error& error) {
    expect(
        error.what() == expected,
        "ABI fixture was rejected for an unexpected reason");
    return;
  }
  throw std::runtime_error("incompatible chassis bridge ABI was accepted");
}

void expect_missing_apply_v2_rejected(
    const std::filesystem::path& library_path) {
  expect(
      std::filesystem::is_regular_file(library_path),
      "missing-capability ABI fixture shared library is missing");
  try {
    mine_teleop::validate_chassis_bridge_abi(library_path);
  } catch (const std::runtime_error& error) {
    expect(
        std::string_view(error.what()).find(
            "dynamic library is missing required symbol "
            "mine_teleop_chassis_apply_state_v2") != std::string_view::npos,
        "V3 bridge without apply_state_v2 was rejected for an unexpected reason");
    return;
  }
  throw std::runtime_error("V3 bridge without apply_state_v2 was accepted");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    expect(
        argc == 4,
        "expected V2, malformed V3, and missing-capability V3 fixture paths");
    expect_rejected(
        argv[1],
        2U,
        static_cast<std::uint32_t>(sizeof(MineTeleopChassisOpenConfigV3)));
    expect_rejected(
        argv[2],
        3U,
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisOpenConfigV3) - 1U));
    expect_missing_apply_v2_rejected(argv[3]);
    std::cout << "chassis_bridge_abi_loader_tests=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "chassis_bridge_abi_loader_tests=failed error="
              << error.what() << '\n';
    return 1;
  }
}
