#include "global_variables.h"
#include "mine_teleop_chassis_bridge.h"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

bool Initialize(const VehicleParam&, const std::string&) { return true; }

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

    const auto log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-diagnostics-smoke.jsonl");
    std::error_code error;
    std::filesystem::remove(log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", log_path.c_str(), 1);
    const int socket_result =
        mine_teleop_chassis_open("mtmissing0");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    expect(socket_result == -3, "missing SocketCAN interface was not rejected");

    const auto events = read_json_lines(log_path);
    bool socket_failure_found = false;
    for (const auto& event : events) {
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
