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
    std::uint32_t actual_runtime_control_v1_size,
    std::uint32_t actual_runtime_control_v2_size,
    std::uint32_t actual_stop_context_v1_size) {
  return
      "chassis bridge ABI mismatch: expected version 6 and V4 config size " +
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
      std::to_string(actual_runtime_control_v1_size) +
      "; runtime control V2 size expected " +
      std::to_string(sizeof(MineTeleopChassisRuntimeControlConfigV2)) +
      ", got " +
      std::to_string(actual_runtime_control_v2_size) +
      "; stop context V1 size expected " +
      std::to_string(sizeof(MineTeleopChassisStopContextV1)) + ", got " +
      std::to_string(actual_stop_context_v1_size);
}

void expect_rejected(
    const std::filesystem::path& library_path,
    std::uint32_t actual_version,
    std::uint32_t actual_v4_size,
    std::uint32_t actual_runtime_control_v1_size,
    std::uint32_t actual_runtime_control_v2_size,
    std::uint32_t actual_stop_context_v1_size) {
  expect(
      std::filesystem::is_regular_file(library_path),
      "ABI fixture shared library is missing");
  const auto expected = expected_mismatch(
      actual_version,
      actual_v4_size,
      actual_runtime_control_v1_size,
      actual_runtime_control_v2_size,
      actual_stop_context_v1_size);
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

void expect_accepted(const std::filesystem::path& library_path) {
  expect(
      std::filesystem::is_regular_file(library_path),
      "compatible ABI fixture shared library is missing");
  mine_teleop::validate_chassis_bridge_abi(library_path);
}

void expect_missing_symbol_rejected(
    const std::filesystem::path& library_path,
    std::string_view symbol) {
  expect(
      std::filesystem::is_regular_file(library_path),
      "missing-capability ABI fixture shared library is missing");
  try {
    mine_teleop::validate_chassis_bridge_abi(library_path);
  } catch (const std::runtime_error& error) {
    expect(
        std::string_view(error.what()).find(
            std::string("dynamic library is missing required symbol ") +
            std::string(symbol)) != std::string_view::npos,
        "bridge with a missing capability was rejected for an unexpected reason");
    return;
  }
  throw std::runtime_error(
      "bridge with a missing capability was accepted");
}

}  // namespace

int main(int argc, char** argv) {
  try {
    expect(
        argc == 13,
        "expected one V5 and eleven capability/size V6 fixture paths");
    expect_rejected(
        argv[1],
        5U,
        static_cast<std::uint32_t>(sizeof(MineTeleopChassisOpenConfigV4)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV2)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisStopContextV1)));
    expect_accepted(argv[2]);
    expect_rejected(
        argv[3],
        6U,
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisOpenConfigV4) - 1U),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV2)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisStopContextV1)));
    expect_missing_symbol_rejected(
        argv[4], "mine_teleop_chassis_apply_state_v2");
    expect_missing_symbol_rejected(
        argv[5], "mine_teleop_chassis_configure_runtime_control_v1");
    expect_missing_symbol_rejected(
        argv[6], "mine_teleop_chassis_runtime_control_config_v2_size");
    expect_missing_symbol_rejected(
        argv[7], "mine_teleop_chassis_configure_runtime_control_v2");
    expect_rejected(
        argv[8],
        6U,
        static_cast<std::uint32_t>(sizeof(MineTeleopChassisOpenConfigV4)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1) - 1U),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV2)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisStopContextV1)));
    expect_rejected(
        argv[9],
        6U,
        static_cast<std::uint32_t>(sizeof(MineTeleopChassisOpenConfigV4)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV2) - 1U),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisStopContextV1)));
    expect_missing_symbol_rejected(
        argv[10], "mine_teleop_chassis_stop_context_v1_size");
    expect_rejected(
        argv[11],
        6U,
        static_cast<std::uint32_t>(sizeof(MineTeleopChassisOpenConfigV4)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV1)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisRuntimeControlConfigV2)),
        static_cast<std::uint32_t>(
            sizeof(MineTeleopChassisStopContextV1) - 1U));
    expect_missing_symbol_rejected(
        argv[12], "mine_teleop_chassis_set_stop_context_v1");
    std::cout << "chassis_bridge_abi_loader_tests=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "chassis_bridge_abi_loader_tests=failed error="
              << error.what() << '\n';
    return 1;
  }
}
