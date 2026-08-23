#include "global_variables.h"
#include "mine_teleop_chassis_bridge.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

int g_initialize_calls = 0;

bool Initialize(const VehicleParam&, const std::string&) {
  ++g_initialize_calls;
  return true;
}

const std::vector<ControlInfo>& GetControlInfo() {
  static const std::vector<ControlInfo> controls(8);
  return controls;
}

bool UpdateVehicleState(const VehicleState&) { return true; }

namespace {

using Json = nlohmann::json;

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

std::vector<Json> read_json_lines(const std::filesystem::path& path) {
  std::ifstream input(path);
  expect(static_cast<bool>(input), "VCU JSONL smoke log was not created");
  std::vector<Json> result;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) result.push_back(Json::parse(line));
  }
  return result;
}

}  // namespace

int main() {
  try {
    std::ostringstream diagnostics;
    auto* previous = std::cerr.rdbuf(diagnostics.rdbuf());
    const int invalid_result = mine_teleop_chassis_open("");
    std::cerr.rdbuf(previous);
    expect(invalid_result == -1, "empty CAN interface was not rejected");
    const auto invalid_event = Json::parse(diagnostics.str());
    expect(
        invalid_event.value("issue_code", "") == "vcu_can_interface_invalid",
        "bootstrap issue_code is missing");
    expect(
        invalid_event.value("stage", "") == "bridge_open",
        "bootstrap stage is missing");
    expect(
        invalid_event.value("retryable", false),
        "bootstrap retryable flag is missing");
    expect(
        invalid_event.value("safety_action", "") == "local_full_stop",
        "bootstrap safety action is missing");
    expect(g_initialize_calls == 0, "empty interface reached ChassisControl Initialize");

    MineTeleopChassisOpenConfigV1 invalid_config{};
    invalid_config.struct_size = sizeof(invalid_config) - 1;
    invalid_config.can_interface = "mtmissing0";
    invalid_config.full_scale_motor_torque_nm = 41.25;
    expect(
        mine_teleop_chassis_open_v1(&invalid_config) == -1,
        "incorrect open_v1 struct size was accepted");
    invalid_config.struct_size = sizeof(invalid_config);
    invalid_config.full_scale_motor_torque_nm =
        std::numeric_limits<double>::quiet_NaN();
    expect(
        mine_teleop_chassis_open_v1(&invalid_config) == -1,
        "non-finite full-scale torque was accepted");
    invalid_config.full_scale_motor_torque_nm = -0.1;
    expect(
        mine_teleop_chassis_open_v1(&invalid_config) == -1,
        "negative full-scale torque was accepted");
    invalid_config.full_scale_motor_torque_nm = 165.1;
    expect(
        mine_teleop_chassis_open_v1(&invalid_config) == -1,
        "unreachable full-scale torque was accepted");
    expect(g_initialize_calls == 0, "invalid open_v1 config reached ChassisControl Initialize");

    MineTeleopChassisOpenConfigV2 invalid_v2_config{};
    invalid_v2_config.struct_size = sizeof(invalid_v2_config) - 1;
    invalid_v2_config.can_interface = "mtmissing0";
    invalid_v2_config.full_scale_motor_torque_nm = 41.25;
    invalid_v2_config.control_timeout_ms = 800;
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "incorrect open_v2 struct size was accepted");
    invalid_v2_config.struct_size = sizeof(invalid_v2_config);
    invalid_v2_config.full_scale_motor_torque_nm =
        std::numeric_limits<double>::quiet_NaN();
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "open_v2 accepted non-finite full-scale torque");
    invalid_v2_config.full_scale_motor_torque_nm = 41.25;
    invalid_v2_config.control_timeout_ms =
        MINE_TELEOP_CHASSIS_MIN_CONTROL_TIMEOUT_MS - 1;
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "control timeout below one transmit period was accepted");
    invalid_v2_config.control_timeout_ms =
        MINE_TELEOP_CHASSIS_MAX_CONTROL_TIMEOUT_MS + 1;
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "unbounded control timeout was accepted");
    expect(g_initialize_calls == 0, "invalid open_v2 config reached ChassisControl Initialize");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(1.0, 82.5) - 2.0) < 1e-9,
        "full throttle did not map to the configured traction acceleration");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(0.10, 82.5) - 0.20) < 1e-9,
        "partial throttle did not scale linearly");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(1.0, 0.0)) < 1e-9,
        "zero full-scale torque did not disable traction");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(-1.0, 165.0) + 1.0) < 1e-9,
        "traction configuration changed the braking path");
    expect(
        std::abs(mine_teleop_chassis_motor_torque_limit_nm(0.10, 41.25) - 4.1) <
            1e-9 &&
            std::abs(mine_teleop_chassis_motor_torque_limit_nm(2.0, 41.25) - 41.2) <
                1e-9 &&
            std::abs(mine_teleop_chassis_motor_torque_limit_nm(-0.30, 41.25)) <
                1e-9,
        "per-channel motor torque ceiling does not follow normalized traction");
    expect(
        std::abs(
            mine_teleop_chassis_clamp_motor_torque_nm(100.0, 0.10, 41.25) -
            4.1) < 1e-9 &&
            std::abs(
                mine_teleop_chassis_clamp_motor_torque_nm(-100.0, 0.10, 41.25) +
                4.1) < 1e-9 &&
            std::abs(
                mine_teleop_chassis_clamp_motor_torque_nm(3.0, 0.10, 41.25) -
                3.0) < 1e-9 &&
            std::abs(
                mine_teleop_chassis_clamp_motor_torque_nm(100.0, -0.30, 41.25)) <
                1e-9 &&
            std::abs(
                mine_teleop_chassis_clamp_motor_torque_nm(100.0, 0.10, 41.6) -
                4.1) < 1e-9 &&
            std::abs(
                mine_teleop_chassis_clamp_motor_torque_nm(-100.0, 0.10, 41.6) +
                4.1) < 1e-9,
        "vendor motor torque was not quantized toward zero before the authoritative clamp");
    expect(
        std::abs(mine_teleop_chassis_target_speed_request_kph(7.5) - 27.0) < 1e-9,
        "target speed was not converted from m/s to km/h");
    expect(
        std::abs(mine_teleop_chassis_target_speed_request_kph(0.0)) < 1e-9,
        "zero target speed did not remain zero");
    expect(
        std::abs(mine_teleop_chassis_target_speed_request_kph(100.0) - 255.0) < 1e-9,
        "target speed was not bounded to the VCU field");
    expect(
        mine_teleop_chassis_vehicle_speed_request_valid(3, 7.5, 0.10, 41.25) == 1 &&
            mine_teleop_chassis_vehicle_speed_request_valid(2, 7.5, 0.10, 41.25) == 1,
        "D/R positive traction did not enable the vehicle-speed request");
    expect(
        mine_teleop_chassis_vehicle_speed_request_valid(3, 0.0, 0.0, 41.25) == 0,
        "released traction retained a valid zero-speed request");
    expect(
        mine_teleop_chassis_vehicle_speed_request_valid(
            3, 0.5 / 3.6, 0.10, 41.25) == 0,
        "a sub-resolution target exposed a valid zero-speed VCU request");
    expect(
        mine_teleop_chassis_vehicle_speed_request_valid(3, 0.0, -0.30, 41.25) == 0,
        "ordinary braking retained a valid zero-speed request");
    expect(
        mine_teleop_chassis_vehicle_speed_request_valid(3, 7.5, 0.10, 0.0) == 0,
        "disabled traction exposed a valid VCU speed request");
    expect(
        mine_teleop_chassis_vehicle_speed_request_valid(1, 7.5, 0.10, 41.25) == 0,
        "neutral exposed a valid VCU speed request");

    const double nan = std::numeric_limits<double>::quiet_NaN();
    expect(
        mine_teleop_chassis_control_output_is_finite(0.0, 0.0, 0.0, 0.0, 0.0) == 1,
        "finite ChassisControl output was rejected");
    expect(
        mine_teleop_chassis_control_output_is_finite(nan, 0.0, 0.0, 0.0, 0.0) == 0 &&
            mine_teleop_chassis_control_output_is_finite(0.0, nan, 0.0, 0.0, 0.0) == 0 &&
            mine_teleop_chassis_control_output_is_finite(0.0, 0.0, nan, 0.0, 0.0) == 0 &&
            mine_teleop_chassis_control_output_is_finite(0.0, 0.0, 0.0, nan, 0.0) == 0 &&
            mine_teleop_chassis_control_output_is_finite(0.0, 0.0, 0.0, 0.0, nan) == 0,
        "non-finite ChassisControl output was not rejected before saturation");

    expect(
        mine_teleop_chassis_control_watchdog_expired(1, 1, 0, 799, 800) == 0 &&
            mine_teleop_chassis_control_watchdog_expired(1, 1, 0, 800, 800) == 1 &&
            mine_teleop_chassis_control_watchdog_expired(0, 1, 0, 800, 800) == 0 &&
            mine_teleop_chassis_control_watchdog_expired(1, 0, 0, 800, 800) == 0 &&
            mine_teleop_chassis_control_watchdog_expired(1, 1, 1, 800, 800) == 0,
        "control-apply watchdog readiness, deadline, or one-shot latch policy is incorrect");

    const auto log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-diagnostics-smoke.jsonl");
    std::error_code error;
    std::filesystem::remove(log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", log_path.c_str(), 1);
    const MineTeleopChassisOpenConfigV1 valid_v1_config{
        sizeof(MineTeleopChassisOpenConfigV1), "mtmissing0", 82.5};
    const int v1_socket_result = mine_teleop_chassis_open_v1(&valid_v1_config);
    const MineTeleopChassisOpenConfigV2 valid_v2_config{
        sizeof(MineTeleopChassisOpenConfigV2), "mtmissing0", 82.5, 900};
    const int v2_socket_result = mine_teleop_chassis_open_v2(&valid_v2_config);
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    expect(
        v1_socket_result == -3 && v2_socket_result == -3,
        "missing SocketCAN interface was not rejected by both versioned open paths");
    expect(
        g_initialize_calls == 2,
        "valid open_v1/open_v2 did not each reach ChassisControl Initialize");

    const auto events = read_json_lines(log_path);
    int socket_failure_count = 0;
    bool v1_default_parameters_found = false;
    bool v2_configured_parameters_found = false;
    for (const auto& event : events) {
      if (event.value("name", "") == "vehicle_parameters") {
        expect(
            std::abs(event.value("full_scale_motor_torque_nm", -1.0) - 82.5) < 1e-9,
            "configured full-scale torque is missing from bridge log");
        const int timeout_ms = event.value("control_timeout_ms", -1);
        v1_default_parameters_found |=
            timeout_ms == MINE_TELEOP_CHASSIS_DEFAULT_CONTROL_TIMEOUT_MS;
        v2_configured_parameters_found |= timeout_ms == 900;
      }
      if (event.value("name", "") != "socket_open_failed") continue;
      ++socket_failure_count;
      expect(
          event.value("issue_code", "") == "socketcan_open_failed",
          "SocketCAN issue_code is missing");
      expect(
          event.value("stage", "") == "resolve_interface_index",
          "SocketCAN open stage is not specific");
      expect(
          event.value("errno", 0) != 0,
          "SocketCAN errno is missing");
      expect(
          !event.value("error", "").empty(),
          "SocketCAN error text is missing");
      expect(
          !event.value("operator_action", "").empty(),
          "SocketCAN operator action is missing");
    }
    expect(
        v1_default_parameters_found,
        "open_v1 did not use the compatible default control timeout");
    expect(
        v2_configured_parameters_found,
        "open_v2 configured control timeout is missing from bridge log");
    expect(socket_failure_count == 2, "versioned SocketCAN failure events are missing");
    std::filesystem::remove(log_path, error);

    std::cout << "chassis_bridge_diagnostics_smoke=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "chassis_bridge_diagnostics_smoke=failed error="
              << error.what() << '\n';
    return 1;
  }
}
