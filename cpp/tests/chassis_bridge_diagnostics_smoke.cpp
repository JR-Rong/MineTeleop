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

    const auto log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-diagnostics-smoke.jsonl");
    std::error_code error;
    std::filesystem::remove(log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", log_path.c_str(), 1);
    const MineTeleopChassisOpenConfigV1 valid_config{
        sizeof(MineTeleopChassisOpenConfigV1), "mtmissing0", 82.5};
    const int socket_result = mine_teleop_chassis_open_v1(&valid_config);
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    expect(socket_result == -3, "missing SocketCAN interface was not rejected");

    const auto events = read_json_lines(log_path);
    bool socket_failure_found = false;
    bool vehicle_parameters_found = false;
    for (const auto& event : events) {
      if (event.value("name", "") == "vehicle_parameters") {
        vehicle_parameters_found = true;
        expect(
            std::abs(event.value("full_scale_motor_torque_nm", -1.0) - 82.5) < 1e-9,
            "configured full-scale torque is missing from bridge log");
      }
      if (event.value("name", "") != "socket_open_failed") continue;
      socket_failure_found = true;
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
    expect(vehicle_parameters_found, "vehicle parameter event is missing");
    expect(socket_failure_found, "SocketCAN failure event is missing");
    std::filesystem::remove(log_path, error);

    std::cout << "chassis_bridge_diagnostics_smoke=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "chassis_bridge_diagnostics_smoke=failed error="
              << error.what() << '\n';
    return 1;
  }
}
