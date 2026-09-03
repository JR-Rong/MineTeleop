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
    std::uint32_t actual_v4_size,
    std::uint32_t actual_runtime_control_size) {
  return
      "chassis bridge ABI mismatch: expected version 4 and V4 config size " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV4)) +
      ", got version " + std::to_string(actual_version) + " and size " +
      std::to_string(actual_v4_size) + "; legacy V3 size expected " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV3)) + ", got " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV3)) +
      "; legacy V2 size expected " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV2)) + ", got " +
      std::to_string(sizeof(MineTeleopChassisOpenConfigV2)) +
      "; runtime control V1 size expected " +
      std::to_string(sizeof(MineTeleopChassisRuntimeControlConfigV1)) +
      ", got " +
      std::to_string(actual_runtime_control_size);
}

void expect_rejected(
    const std::filesystem::path& library_path,
    std::uint32_t actual_version,
    std::uint32_t actual_v4_size,
    std::uint32_t actual_runtime_control_size) {
  expect(
      std::filesystem::is_regular_file(library_path),
      "ABI fixture shared library is missing");
  const auto expected = expected_mismatch(
      actual_version,
      actual_v4_size,
      actual_runtime_control_size);
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
        "V4 bridge without apply_state_v2 was rejected for an unexpected reason");
    return;
  }
  throw std::runtime_error("V4 bridge without apply_state_v2 was accepted");
}

void expect_missing_runtime_control_rejected(
    const std::filesystem::path& library_path) {
  expect(
      std::filesystem::is_regular_file(library_path),
      "missing-runtime-control ABI fixture shared library is missing");
  try {
    mine_teleop::validate_chassis_bridge_abi(library_path);
  } catch (const std::runtime_error& error) {
    expect(
        std::string_view(error.what()).find(
            "dynamic library is missing required symbol "
            "mine_teleop_chassis_configure_runtime_control_v1") !=
            std::string_view::npos,
        "V4 bridge without runtime configuration was rejected for an unexpected reason");
    return;
  }
  throw std::runtime_error(
      "V4 bridge without runtime configuration was accepted");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    expect(
        argc == 6,
        "expected legacy V3, malformed V4, and three capability/size V4 fixture paths");
    expect_rejected(
        argv[1],
        3U,
        static_cast<std::uint32_t>(sizeof(MineTeleopChassisOpenConfigV4)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1)));
    expect_rejected(
        argv[2],
        4U,
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisOpenConfigV4) - 1U),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1)));
    expect_missing_apply_v2_rejected(argv[3]);
    expect_missing_runtime_control_rejected(argv[4]);
    expect_rejected(
        argv[5],
        4U,
        static_cast<std::uint32_t>(sizeof(MineTeleopChassisOpenConfigV4)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1) - 1U));
    std::cout << "chassis_bridge_abi_loader_tests=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "chassis_bridge_abi_loader_tests=failed error="
              << error.what() << '\n';
    return 1;
  }
}
