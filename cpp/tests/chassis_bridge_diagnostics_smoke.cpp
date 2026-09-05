#include "global_variables.h"
#include "mine_teleop_chassis_bridge.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <linux/can.h>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

int g_initialize_calls = 0;
std::mutex g_vendor_mutex;
std::vector<ControlInfo> g_controls(8);
std::vector<VehicleState> g_vehicle_states;
std::set<std::thread::id> g_vendor_update_threads;
double g_forced_vendor_motor_torque_nm = -1.0;
double g_forced_vendor_brake_pressure_bar = -1.0;
bool g_update_vehicle_state_result = true;

bool Initialize(const VehicleParam&, const std::string&) {
  ++g_initialize_calls;
  return true;
}

const std::vector<ControlInfo>& GetControlInfo() {
  return g_controls;
}

bool UpdateVehicleState(const VehicleState& state) {
  std::lock_guard<std::mutex> lock(g_vendor_mutex);
  g_vehicle_states.push_back(state);
  g_vendor_update_threads.insert(std::this_thread::get_id());
  const double normalized = state.target_acceleration[0];
  const double direction = state.target_gear == 2 ? -1.0 : 1.0;
  for (std::size_t index = 0; index < g_controls.size(); ++index) {
    auto& control = g_controls[index];
    control.wheel_torque = g_forced_vendor_motor_torque_nm >= 0.0
        ? direction * g_forced_vendor_motor_torque_nm
        : (normalized > 0.0 ? direction * normalized * 300.0 : 0.0);
    control.wheel_speed = 0.0;
    control.ehb_brk_pres_req = g_forced_vendor_brake_pressure_bar >= 0.0
        ? g_forced_vendor_brake_pressure_bar
        : (normalized < 0.0
               ? std::min(-normalized * 10.0, 409.5)
               : 0.0);
    control.eps_ang_req = state.target_steering_angle[index];
    control.eps_ang_spd_req = 0.0;
  }
  return g_update_vehicle_state_result;
}

namespace {

using Json = nlohmann::json;

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

MineTeleopChassisOpenConfigV2 valid_v2_config(
    const char* can_interface,
    int control_timeout_ms = 800) {
  return {
      sizeof(MineTeleopChassisOpenConfigV2),
      can_interface,
      41.25,
      10.0,
      control_timeout_ms,
      200,
      1.0,
      0.2,
      0.0,
      100.0,
      100,
      1.0,
  };
}

MineTeleopChassisOpenConfigV3 valid_v3_config(
    const char* can_interface,
    int control_timeout_ms = 800) {
  return {
      sizeof(MineTeleopChassisOpenConfigV3),
      can_interface,
      300.0,
      10.0,
      control_timeout_ms,
      200,
      1.0,
      0.2,
      0.0,
      100.0,
      100,
      1.0,
      100.0,
  };
}

MineTeleopChassisOpenConfigV4 valid_v4_config(
    const char* can_interface,
    int control_timeout_ms = 800,
    double motor_torque_rise_rate_nm_per_s = 0.0) {
  const auto v3 = valid_v3_config(can_interface, control_timeout_ms);
  return {
      sizeof(MineTeleopChassisOpenConfigV4),
      v3.can_interface,
      v3.full_scale_motor_torque_nm,
      v3.hard_speed_limit_mps,
      v3.control_timeout_ms,
      v3.speed_feedback_timeout_ms,
      v3.speed_pid_kp,
      v3.speed_pid_ki,
      v3.speed_pid_kd,
      v3.speed_pid_derivative_filter_tau_ms,
      v3.speed_pid_max_dt_ms,
      v3.hard_overspeed_margin_mps,
      v3.max_ordinary_brake_pressure_bar,
      motor_torque_rise_rate_nm_per_s,
  };
}

MineTeleopChassisRuntimeControlConfigV1 valid_legacy_runtime_control_config(
    std::uint64_t revision,
    double target_speed_limit_mps,
    double max_motor_torque_nm,
    double max_brake_pressure_bar) {
  return {
      sizeof(MineTeleopChassisRuntimeControlConfigV1),
      MINE_TELEOP_CHASSIS_LEGACY_SESSION_CONTROL_PROFILE_VERSION,
      revision,
      target_speed_limit_mps,
      max_motor_torque_nm,
      max_brake_pressure_bar,
      1.0,
      1.0,
      0.2,
      0.0,
      100.0,
      100,
      0U,
  };
}

MineTeleopChassisRuntimeControlConfigV2 valid_runtime_control_config(
    std::uint64_t revision,
    double target_speed_limit_mps,
    double max_motor_torque_nm,
    double max_brake_pressure_bar,
    double motor_torque_rise_rate_nm_per_s = 0.0) {
  return {
      sizeof(MineTeleopChassisRuntimeControlConfigV2),
      MINE_TELEOP_CHASSIS_SESSION_CONTROL_PROFILE_VERSION,
      revision,
      target_speed_limit_mps,
      max_motor_torque_nm,
      max_brake_pressure_bar,
      1.0,
      1.0,
      0.2,
      0.0,
      100.0,
      100,
      0U,
      motor_torque_rise_rate_nm_per_s,
  };
}

MineTeleopChassisFeedback runtime_feedback(
    int handshake,
    int gear,
    int parking_brake,
    double speed_mps) {
  MineTeleopChassisFeedback feedback{};
  feedback.shake_hand_status = handshake;
  feedback.gear_status = gear;
  feedback.vehicle_speed = speed_mps;
  feedback.vehicle_speed_valid = 1;
  feedback.driver_gear_request = 1;
  feedback.driver_gear_request_valid = 1;
  for (int& value : feedback.epb_status) value = parking_brake;
  for (int& value : feedback.mcu_mode) value = 1;
  for (int& value : feedback.eps_mode) value = 1;
  for (double& value : feedback.eps_angle) value = 0.0;
  for (int& value : feedback.ehb_mode) value = 1;
  return feedback;
}

void send_vehicle_status_feedback_frame(
    int fd,
    int gear,
    int emergency_switch,
    int vmc_fault_code = 0) {
  can_frame frame{};
  frame.can_id = 0x18F2F5D0U | CAN_EFF_FLAG;
  frame.can_dlc = 8;
  frame.data[0] = static_cast<std::uint8_t>(
      ((gear & 0x03) << 2) | (emergency_switch & 0x03));
  frame.data[2] = static_cast<std::uint8_t>(vmc_fault_code);
  expect(
      ::send(fd, &frame, sizeof(frame), 0) ==
          static_cast<ssize_t>(sizeof(frame)),
      "vehicle-status feedback frame injection failed");
}

void send_handshake_feedback_frame(int fd, int handshake_status) {
  can_frame frame{};
  frame.can_id = 0x18F0F5D0U | CAN_EFF_FLAG;
  frame.can_dlc = 8;
  frame.data[1] = static_cast<std::uint8_t>(handshake_status);
  expect(
      ::send(fd, &frame, sizeof(frame), 0) ==
          static_cast<ssize_t>(sizeof(frame)),
      "handshake feedback frame injection failed");
}

void send_driver_switch_feedback_frame(
    int fd,
    int parking_brake_switch,
    int brake_pedal_switch) {
  can_frame frame{};
  frame.can_id = 0x18F6F5D0U | CAN_EFF_FLAG;
  frame.can_dlc = 8;
  frame.data[2] = static_cast<std::uint8_t>(
      (brake_pedal_switch & 0x03) << 6);
  frame.data[3] = static_cast<std::uint8_t>(parking_brake_switch & 0x03);
  expect(
      ::send(fd, &frame, sizeof(frame), 0) ==
          static_cast<ssize_t>(sizeof(frame)),
      "driver-switch feedback frame injection failed");
}

template <typename Predicate>
bool wait_until(Predicate predicate, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return predicate();
}

bool wait_for_handshake_state(int expected, int timeout_ms = 500) {
  return wait_until(
      [&] {
        MineTeleopChassisHandshakeStatus status{};
        return mine_teleop_chassis_read_handshake_status(&status) == 0 &&
            status.state == expected;
      },
      timeout_ms);
}

void complete_runtime_arming_to_ready(int drive_gear) {
  auto feedback = runtime_feedback(5, 1, 2, 0.0);
  expect(
      mine_teleop_chassis_update_feedback(&feedback) == 0,
      "runtime handshake feedback injection failed");
  expect(
      wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARKING_BRAKE_RELEASED),
      "runtime did not accept intelligent handshake status");
  feedback = runtime_feedback(5, 1, 1, 0.0);
  expect(
      mine_teleop_chassis_update_feedback(&feedback) == 0,
      "runtime EPB release feedback injection failed");
  expect(
      wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
      "runtime did not reach gear wait");
  feedback = runtime_feedback(5, drive_gear, 1, 0.0);
  expect(
      mine_teleop_chassis_update_feedback(&feedback) == 0,
      "runtime drive-gear feedback injection failed");
  expect(
      wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_ACTUATOR_MODES),
      "runtime did not reach actuator-mode wait");
  expect(
      mine_teleop_chassis_update_feedback(&feedback) == 0,
      "runtime actuator feedback injection failed");
  expect(
      wait_for_handshake_state(MINE_TELEOP_VCU_READY),
      "runtime did not become Ready");
}

void drive_runtime_disarm_phase(
    int source_state,
    int target_state,
    const MineTeleopChassisFeedback& feedback,
    const char* scenario,
    const char* phase) {
  constexpr int kTestOnlyPhaseTimeoutMs = 1500;
  const std::string context =
      std::string(scenario) + " disarm " + phase;
  int observed_state = -1;
  const bool reached = wait_until(
      [&] {
        MineTeleopChassisHandshakeStatus status{};
        expect(
            mine_teleop_chassis_read_handshake_status(&status) == 0,
            context + " status read failed");
        observed_state = status.state;
        if (observed_state == target_state) return true;
        expect(
            observed_state == source_state,
            context + " reached unexpected state=" +
                std::to_string(observed_state) +
                " source=" + std::to_string(source_state) +
                " target=" + std::to_string(target_state));
        expect(
            mine_teleop_chassis_update_feedback(&feedback) == 0,
            context + " feedback injection failed");
        return false;
      },
      kTestOnlyPhaseTimeoutMs);
  expect(
      reached,
      context + " timed out in state=" + std::to_string(observed_state) +
          " waiting for=" + std::to_string(target_state));
}

void complete_runtime_disarm(int actual_gear, const char* scenario) {
  auto feedback = runtime_feedback(5, actual_gear, 1, 0.0);
  // Hold DisarmStop until the test has observed it. A full compatibility
  // feedback batch would otherwise also provide the next phase's fresh zero
  // speed and can make this exact intermediate state scheduler-dependent.
  feedback.vehicle_speed_valid = 0;
  drive_runtime_disarm_phase(
      MINE_TELEOP_VCU_DISARM_TORQUE,
      MINE_TELEOP_VCU_DISARM_STOP,
      feedback,
      scenario,
      "zero-torque confirmation");

  feedback = runtime_feedback(5, actual_gear, 1, 0.0);
  drive_runtime_disarm_phase(
      MINE_TELEOP_VCU_DISARM_STOP,
      MINE_TELEOP_VCU_DISARM_NEUTRAL,
      feedback,
      scenario,
      "zero-speed confirmation");

  feedback = runtime_feedback(5, 1, 1, 0.0);
  drive_runtime_disarm_phase(
      MINE_TELEOP_VCU_DISARM_NEUTRAL,
      MINE_TELEOP_VCU_DISARM_PARKING_BRAKE,
      feedback,
      scenario,
      "neutral confirmation");

  feedback = runtime_feedback(5, 1, 2, 0.0);
  drive_runtime_disarm_phase(
      MINE_TELEOP_VCU_DISARM_PARKING_BRAKE,
      MINE_TELEOP_VCU_DISARM_MANUAL,
      feedback,
      scenario,
      "parking-brake confirmation");

  feedback = runtime_feedback(3, 1, 2, 0.0);
  drive_runtime_disarm_phase(
      MINE_TELEOP_VCU_DISARM_MANUAL,
      MINE_TELEOP_VCU_DISARMED,
      feedback,
      scenario,
      "manual-handshake confirmation");
}

std::vector<can_frame> drain_can_frames(int fd, int duration_ms) {
  std::vector<can_frame> frames;
  const auto deadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(duration_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    can_frame frame{};
    const auto received = ::recv(fd, &frame, sizeof(frame), MSG_DONTWAIT);
    if (received == static_cast<ssize_t>(sizeof(frame))) {
      frames.push_back(frame);
      continue;
    }
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
      throw std::runtime_error("failed to read adopted bridge transport");
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return frames;
}

std::uint64_t little_endian_payload(const can_frame& frame) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(frame.data[index]) << (index * 8U);
  }
  return value;
}

std::uint64_t can_signal(
    const can_frame& frame,
    unsigned start,
    unsigned length) {
  const auto mask = length == 64
      ? std::numeric_limits<std::uint64_t>::max()
      : (std::uint64_t{1} << length) - 1U;
  return (little_endian_payload(frame) >> start) & mask;
}

const can_frame& last_frame_with_id(
    const std::vector<can_frame>& frames,
    std::uint32_t id) {
  const auto found = std::find_if(
      frames.rbegin(),
      frames.rend(),
      [&](const auto& frame) { return (frame.can_id & CAN_EFF_MASK) == id; });
  if (found == frames.rend()) {
    throw std::runtime_error("expected CAN frame was not transmitted");
  }
  return *found;
}

void expect_all_motor_torque_raw(
    const std::vector<can_frame>& frames,
    std::uint64_t expected_raw,
    const std::string& context) {
  for (std::uint32_t index = 0; index < 8; ++index) {
    expect(
        can_signal(
            last_frame_with_id(frames, 0x18F0D0F5U + (index << 16U)),
            8,
            14) == expected_raw,
        context + " motor channel " + std::to_string(index) + " mismatch");
  }
}

void expect_all_brake_pressure_raw(
    const std::vector<can_frame>& frames,
    std::uint64_t expected_raw,
    const std::string& context) {
  for (const auto id : {0x18FFD0F5U, 0x18FAD0F5U}) {
    const auto& frame = last_frame_with_id(frames, id);
    for (unsigned local = 0; local < 4; ++local) {
      expect(
          can_signal(frame, local * 16U + 4U, 12) == expected_raw,
          context + " brake channel mismatch");
    }
  }
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

std::size_t count_logged_events(
    const std::filesystem::path& path,
    const std::string& event_name) {
  std::ifstream input(path);
  expect(static_cast<bool>(input), "VCU JSONL smoke log was not created");
  const auto needle = "\"name\":\"" + event_name + "\"";
  std::size_t count = 0;
  std::string line;
  while (std::getline(input, line)) {
    if (line.find(needle) != std::string::npos) ++count;
  }
  return count;
}

}  // namespace

int main() {
  try {
    expect(
        mine_teleop_chassis_abi_version() == 6U &&
            mine_teleop_chassis_open_config_v2_size() ==
                sizeof(MineTeleopChassisOpenConfigV2) &&
            mine_teleop_chassis_open_config_v3_size() ==
                sizeof(MineTeleopChassisOpenConfigV3) &&
            mine_teleop_chassis_open_config_v4_size() ==
                sizeof(MineTeleopChassisOpenConfigV4) &&
            mine_teleop_chassis_runtime_control_config_v1_size() ==
                sizeof(MineTeleopChassisRuntimeControlConfigV1) &&
            mine_teleop_chassis_runtime_control_config_v2_size() ==
                sizeof(MineTeleopChassisRuntimeControlConfigV2) &&
            mine_teleop_chassis_stop_context_v1_size() ==
                sizeof(MineTeleopChassisStopContextV1) &&
            sizeof(MineTeleopChassisRuntimeControlConfigV1) == 88U &&
            sizeof(MineTeleopChassisRuntimeControlConfigV2) == 96U &&
            sizeof(MineTeleopChassisStopContextV1) == 16U &&
            sizeof(MineTeleopChassisTelemetry) == 64U &&
            sizeof(MineTeleopChassisApplyResultV1) == 16U &&
            sizeof(MineTeleopChassisRuntimeControlResultV1) == 24U,
        "bridge ABI version or versioned open-config size query is inconsistent");
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
    invalid_config.full_scale_motor_torque_nm = 640.1;
    expect(
        mine_teleop_chassis_open_v1(&invalid_config) == -1,
        "unreachable full-scale torque was accepted");
    expect(g_initialize_calls == 0, "invalid open_v1 config reached ChassisControl Initialize");

    auto invalid_v2_config = valid_v2_config("mtmissing0");
    invalid_v2_config.struct_size = sizeof(invalid_v2_config) - 1;
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
    invalid_v2_config.control_timeout_ms = 800;
    invalid_v2_config.speed_feedback_timeout_ms =
        MINE_TELEOP_CHASSIS_MIN_SPEED_FEEDBACK_TIMEOUT_MS - 1;
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "sub-period speed feedback timeout was accepted");
    invalid_v2_config.speed_feedback_timeout_ms = 200;
    invalid_v2_config.speed_pid_kp =
        std::numeric_limits<double>::quiet_NaN();
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "non-finite speed PID gain was accepted");
    invalid_v2_config.speed_pid_kp = 1.0;
    invalid_v2_config.speed_pid_max_dt_ms =
        MINE_TELEOP_CHASSIS_MAX_SPEED_PID_MAX_DT_MS + 1;
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "unbounded speed PID dt was accepted");
    invalid_v2_config.speed_pid_max_dt_ms = 100;
    invalid_v2_config.hard_overspeed_margin_mps = 0.0;
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "zero hard overspeed margin was accepted");
    expect(g_initialize_calls == 0, "invalid open_v2 config reached ChassisControl Initialize");

    auto invalid_v3_config = valid_v3_config("mtmissing0");
    invalid_v3_config.struct_size = sizeof(MineTeleopChassisOpenConfigV2);
    expect(
        mine_teleop_chassis_open_v3(&invalid_v3_config) == -1,
        "V2-sized struct was accepted by open_v3");
    invalid_v3_config.struct_size = sizeof(invalid_v3_config);
    invalid_v3_config.max_ordinary_brake_pressure_bar = 327.7;
    expect(
        mine_teleop_chassis_open_v3(&invalid_v3_config) == -1,
        "open_v3 accepted ordinary brake pressure above the code cap");
    invalid_v3_config.max_ordinary_brake_pressure_bar =
        std::numeric_limits<double>::quiet_NaN();
    expect(
        mine_teleop_chassis_open_v3(&invalid_v3_config) == -1,
        "open_v3 accepted non-finite ordinary brake pressure");
    auto invalid_v4_config = valid_v4_config("mtmissing0");
    invalid_v4_config.struct_size = sizeof(MineTeleopChassisOpenConfigV3);
    expect(
        mine_teleop_chassis_open_v4(&invalid_v4_config) == -1,
        "V3-sized struct was accepted by open_v4");
    invalid_v4_config.struct_size = sizeof(invalid_v4_config);
    for (const double invalid_rate : {
             -0.1,
             MINE_TELEOP_CHASSIS_MAX_MOTOR_TORQUE_RISE_RATE_NM_PER_SECOND + 0.1,
             std::numeric_limits<double>::quiet_NaN()}) {
      invalid_v4_config.motor_torque_rise_rate_nm_per_s = invalid_rate;
      expect(
          mine_teleop_chassis_open_v4(&invalid_v4_config) == -1,
          "open_v4 accepted an invalid motor torque rise rate");
    }
    invalid_v2_config = valid_v2_config("mtmissing0");
    invalid_v2_config.struct_size = sizeof(MineTeleopChassisOpenConfigV3);
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "V3-sized struct was accepted by legacy open_v2");
    expect(
        g_initialize_calls == 0,
        "invalid open_v3/open_v4 config reached ChassisControl Initialize");
    g_update_vehicle_state_result = false;
    std::ostringstream rejected_seed_diagnostics;
    previous = std::cerr.rdbuf(rejected_seed_diagnostics.rdbuf());
    auto rejected_seed_config = valid_v4_config("mtmissing0");
    const int rejected_seed_result =
        mine_teleop_chassis_open_v4(&rejected_seed_config);
    std::cerr.rdbuf(previous);
    g_update_vehicle_state_result = true;
    expect(
        rejected_seed_result == -2 &&
            rejected_seed_diagnostics.str().find(
                "chassis_control_emergency_seed_failed") != std::string::npos,
        "UpdateVehicleState false did not reject the initial emergency seed");
    g_initialize_calls = 0;
    expect(
        std::abs(mine_teleop_chassis_motor_torque_limit_nm(0.3334, 300.0) - 100.0) <
            1e-9 &&
            std::abs(mine_teleop_chassis_motor_torque_limit_nm(0.5, 200.0) - 100.0) <
                1e-9 &&
            std::abs(mine_teleop_chassis_motor_torque_limit_nm(2.0, 640.0) - 640.0) <
                1e-9 &&
            std::abs(mine_teleop_chassis_motor_torque_limit_nm(1.0, 1000.0) - 640.0) <
                1e-9 &&
            mine_teleop_chassis_motor_torque_limit_nm(-0.3, 300.0) == 0.0 &&
            mine_teleop_chassis_motor_torque_limit_nm(
                std::numeric_limits<double>::quiet_NaN(), 300.0) == 0.0,
        "normalized PID output did not map directly to the bounded per-motor session torque");
    expect(
        std::abs(mine_teleop_chassis_rise_limited_motor_torque_nm(
                     0.0, 100.0, 300.0, 0.02) -
                 6.0) < 1e-9 &&
            std::abs(mine_teleop_chassis_rise_limited_motor_torque_nm(
                         6.0, 100.0, 300.0, 0.02) -
                     12.0) < 1e-9 &&
            std::abs(mine_teleop_chassis_rise_limited_motor_torque_nm(
                         100.0, 40.0, 300.0, 0.02) -
                     40.0) < 1e-9 &&
            mine_teleop_chassis_rise_limited_motor_torque_nm(
                100.0, 0.0, 300.0, 0.0) == 0.0 &&
            mine_teleop_chassis_rise_limited_motor_torque_nm(
                0.0, 100.0, 0.0, 0.02) == 100.0 &&
            std::abs(mine_teleop_chassis_rise_limited_motor_torque_nm(
                         0.0, 100.0, 1.0, 0.02) -
                     0.02) < 1e-9 &&
            mine_teleop_chassis_rise_limited_motor_torque_nm(
                0.0,
                100.0,
                std::numeric_limits<double>::quiet_NaN(),
                0.02) == 0.0,
        "configurable per-motor torque rise shaping or immediate withdrawal is incorrect");
    double sub_resolution_ramp_nm = 0.0;
    for (int index = 0; index < 5; ++index) {
      sub_resolution_ramp_nm =
          mine_teleop_chassis_rise_limited_motor_torque_nm(
              sub_resolution_ramp_nm,
              100.0,
              1.0,
              0.02);
    }
    expect(
        std::abs(sub_resolution_ramp_nm - 0.1) < 1e-9 &&
            std::abs(
                mine_teleop_chassis_directional_motor_torque_nm(
                    3,
                    sub_resolution_ramp_nm) -
                0.1) < 1e-9,
        "sub-DBC-resolution rise state was quantized each cycle and stalled below 0.1 Nm");
    expect(
        mine_teleop_chassis_directional_motor_torque_nm(3, 100.0) == 100.0 &&
            mine_teleop_chassis_directional_motor_torque_nm(2, 100.0) == -100.0 &&
            mine_teleop_chassis_directional_motor_torque_nm(1, 100.0) == 0.0 &&
            mine_teleop_chassis_directional_motor_torque_nm(
                3, std::numeric_limits<double>::quiet_NaN()) == 0.0,
        "direct per-motor torque direction was not fail-closed for D/R/N");
    expect(
        std::abs(mine_teleop_chassis_quantize_brake_pressure_bar(100.09) - 100.0) < 1e-9 &&
            std::abs(mine_teleop_chassis_quantize_brake_pressure_bar(327.7) - 327.6) < 1e-9 &&
            mine_teleop_chassis_quantize_brake_pressure_bar(0.0) == 0.0,
        "ordinary brake pressure was not capped and quantized toward zero");
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

    const MineTeleopChassisSpeedPidConfig pid_config{
        1.0, 0.5, 0.1, 100.0, 100};
    MineTeleopChassisSpeedPidState pid_state{};
    const double saturated_pid = mine_teleop_chassis_speed_pid_step(
        &pid_config, &pid_state, 5.0, 0.0, 1.0, 0.02);
    expect(
        std::abs(saturated_pid - 1.0) < 1e-9,
        "large local speed error did not saturate at the sole normalized full-scale ceiling");
    expect(
        std::abs(pid_state.integral) < 1e-9,
        "speed PID integrated further into positive output saturation");

    const MineTeleopChassisSpeedPidConfig actuator_limited_pid{
        0.10, 1.0, 0.0, 0.0, 100};
    MineTeleopChassisSpeedPidState actuator_limited_state{};
    const double first_reachable_torque_nm =
        mine_teleop_chassis_rise_limited_motor_torque_nm(
            0.0,
            100.0,
            300.0,
            0.02);
    const double actuator_limited_output =
        mine_teleop_chassis_speed_pid_step(
            &actuator_limited_pid,
            &actuator_limited_state,
            1.0,
            0.0,
            first_reachable_torque_nm / 100.0,
            0.02);
    expect(
        std::abs(actuator_limited_output - 0.06) < 1e-9 &&
            std::abs(actuator_limited_state.integral) < 1e-9,
        "configured torque ramp was applied after the PID and allowed hidden integral windup");

    const MineTeleopChassisSpeedPidConfig proportional_pid_low{
        0.15, 0.0, 0.0, 0.0, 100};
    const MineTeleopChassisSpeedPidConfig proportional_pid_high{
        0.30, 0.0, 0.0, 0.0, 100};
    MineTeleopChassisSpeedPidState proportional_low_state{};
    MineTeleopChassisSpeedPidState proportional_high_state{};
    const double proportional_low = mine_teleop_chassis_speed_pid_step(
        &proportional_pid_low,
        &proportional_low_state,
        2.0,
        0.0,
        1.0,
        0.02);
    const double proportional_high = mine_teleop_chassis_speed_pid_step(
        &proportional_pid_high,
        &proportional_high_state,
        2.0,
        0.0,
        1.0,
        0.02);
    expect(
        std::abs(
            mine_teleop_chassis_motor_torque_limit_nm(
                proportional_low, 300.0) -
            90.0) < 1e-9 &&
            std::abs(
                mine_teleop_chassis_motor_torque_limit_nm(
                    proportional_high, 300.0) -
                180.0) < 1e-9,
        "proportional PID output did not scale direct per-motor torque");

    pid_state = MineTeleopChassisSpeedPidState{0.25, 0.0, 5.0, 1};
    const double hold_pid = mine_teleop_chassis_speed_pid_step(
        &pid_config, &pid_state, 5.0, 5.0, 1.0, 0.02);
    expect(
        hold_pid > 0.0 && std::abs(hold_pid - 0.25) < 1e-9,
        "target-speed regulation forced a positive integral hold torque to zero");

    expect(
        mine_teleop_chassis_speed_pid_setpoint_requires_reset(
            1, 3, 3, 5.0, 5.03) == 0 &&
            mine_teleop_chassis_speed_pid_setpoint_requires_reset(
                1, 3, 3, 5.0, 5.049) == 0,
        "small analog target-speed jitter reset the fixed PID reference");
    pid_state = MineTeleopChassisSpeedPidState{0.25, 0.0, 5.03, 1};
    const double jitter_hold_pid = mine_teleop_chassis_speed_pid_step(
        &pid_config, &pid_state, 5.03, 5.03, 1.0, 0.02);
    expect(
        std::abs(jitter_hold_pid - 0.25) < 1e-9 &&
            std::abs(pid_state.integral - 0.25) < 1e-9,
        "sub-deadband target jitter discarded the PID integral hold state");
    expect(
        mine_teleop_chassis_speed_pid_setpoint_requires_reset(
            1, 3, 3, 5.0, 5.051) == 1 &&
            mine_teleop_chassis_speed_pid_setpoint_requires_reset(
                1, 3, 3, 5.0, 4.94) == 1 &&
            mine_teleop_chassis_speed_pid_setpoint_requires_reset(
                1, 3, 2, 5.0, 5.0) == 1,
        "cumulative target drift, a material decrease, or D/R change did not reset the PID reference");
    mine_teleop_chassis_speed_pid_reset(&pid_state);
    const double reset_target_pid = mine_teleop_chassis_speed_pid_step(
        &pid_config, &pid_state, 4.94, 4.94, 1.0, 0.02);
    expect(
        reset_target_pid == 0.0 && pid_state.integral == 0.0,
        "material target decrease retained stale integral torque after reset");

    MineTeleopChassisSpeedPidState d_pid_state{};
    MineTeleopChassisSpeedPidState r_pid_state{};
    const double d_output = mine_teleop_chassis_speed_pid_step(
        &pid_config, &d_pid_state, 5.0, 4.5, 1.0, 0.02);
    const double r_output = mine_teleop_chassis_speed_pid_step(
        &pid_config, &r_pid_state, 5.0, -(-4.5), 1.0, 0.02);
    expect(
        std::abs(d_output - r_output) < 1e-9,
        "D/R direction normalization produced materially different PID output");
    expect(
        mine_teleop_chassis_opposite_direction_motion(3, -0.11) == 1 &&
            mine_teleop_chassis_opposite_direction_motion(2, 0.11) == 1 &&
            mine_teleop_chassis_opposite_direction_motion(3, 0.11) == 0 &&
            mine_teleop_chassis_opposite_direction_motion(2, -0.11) == 0,
        "opposite-direction D/R motion was not rejected symmetrically");

    pid_state.integral = 0.5;
    pid_state.initialized = 1;
    expect(
        mine_teleop_chassis_speed_pid_step(
            &pid_config,
            &pid_state,
            5.0,
            std::numeric_limits<double>::quiet_NaN(),
            1.0,
            0.02) == 0.0 &&
            pid_state.initialized == 0 && pid_state.integral == 0.0,
        "non-finite speed feedback did not reset the PID to zero traction");
    pid_state.integral = std::numeric_limits<double>::quiet_NaN();
    pid_state.initialized = 1;
    expect(
        mine_teleop_chassis_speed_pid_step(
            &pid_config, &pid_state, 5.0, 4.0, 1.0, 0.02) == 0.0 &&
            pid_state.initialized == 0 && pid_state.integral == 0.0,
        "non-finite PID state did not fail closed and reset");
    pid_state.integral = 0.5;
    pid_state.initialized = 1;
    expect(
        mine_teleop_chassis_speed_pid_step(
            &pid_config, &pid_state, 5.0, 4.0, 1.0, 0.101) == 0.0 &&
            pid_state.initialized == 0 && pid_state.integral == 0.0,
        "abnormal control dt did not reset the PID to zero traction");

    expect(
        mine_teleop_chassis_hard_overspeed_latch(0, 10.0, 11.0, 1.0) == 0 &&
            mine_teleop_chassis_hard_overspeed_latch(0, 10.0, 11.01, 1.0) == 1 &&
            mine_teleop_chassis_hard_overspeed_latch(0, 0.0, 1.01, 1.0) == 1 &&
            mine_teleop_chassis_hard_overspeed_latch(1, 10.0, 0.0, 1.0) == 1,
        "hard overspeed boundary or latch persistence is incorrect");
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
    auto configured_v2 = valid_v2_config("mtmissing0", 900);
    configured_v2.full_scale_motor_torque_nm = 82.5;
    const int v2_socket_result = mine_teleop_chassis_open_v2(&configured_v2);
    auto configured_v3 = valid_v3_config("mtmissing0", 950);
    configured_v3.full_scale_motor_torque_nm = 82.5;
    configured_v3.max_ordinary_brake_pressure_bar = 100.0;
    const int v3_socket_result = mine_teleop_chassis_open_v3(&configured_v3);
    auto configured_v4 = valid_v4_config("mtmissing0", 975, 1234.0);
    configured_v4.full_scale_motor_torque_nm = 82.5;
    configured_v4.max_ordinary_brake_pressure_bar = 100.0;
    const int v4_socket_result = mine_teleop_chassis_open_v4(&configured_v4);
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    expect(
        v1_socket_result == -3 && v2_socket_result == -3 &&
            v3_socket_result == -3 && v4_socket_result == -3,
        "missing SocketCAN interface was not rejected by every versioned open path");
    expect(
        g_initialize_calls == 4,
        "valid open_v1/open_v2/open_v3/open_v4 did not each reach ChassisControl Initialize");

    const auto events = read_json_lines(log_path);
    int socket_failure_count = 0;
    bool v1_default_parameters_found = false;
    bool v2_configured_parameters_found = false;
    bool v3_physical_pressure_parameters_found = false;
    bool v4_torque_rise_rate_parameters_found = false;
    for (const auto& event : events) {
      if (event.value("name", "") == "vehicle_parameters") {
        expect(
            std::abs(event.value("full_scale_motor_torque_nm", -1.0) - 82.5) < 1e-9,
            "configured full-scale torque is missing from bridge log");
        const int timeout_ms = event.value("control_timeout_ms", -1);
        v1_default_parameters_found |=
            timeout_ms == MINE_TELEOP_CHASSIS_DEFAULT_CONTROL_TIMEOUT_MS;
        v2_configured_parameters_found |= timeout_ms == 900;
        v3_physical_pressure_parameters_found |=
            timeout_ms == 950 &&
            event.value("physical_brake_input", false) &&
            std::abs(event.value("max_ordinary_brake_pressure_bar", -1.0) -
                     100.0) < 1e-9;
        v4_torque_rise_rate_parameters_found |=
            timeout_ms == 975 &&
            std::abs(
                event.value("motor_torque_rise_rate_nm_per_s", -1.0) -
                1234.0) < 1e-9;
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
    expect(
        v3_physical_pressure_parameters_found,
        "open_v3 physical pressure contract is missing from bridge log");
    expect(
        v4_torque_rise_rate_parameters_found,
        "open_v4 torque rise-rate contract is missing from bridge log");
    expect(socket_failure_count == 4, "versioned SocketCAN failure events are missing");
    std::filesystem::remove(log_path, error);

    int transport[2]{-1, -1};
    expect(
        ::socketpair(AF_UNIX, SOCK_DGRAM, 0, transport) == 0,
        "unprivileged adopted bridge transport could not be created");
    const auto runtime_log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-runtime-smoke.jsonl");
    std::filesystem::remove(runtime_log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", runtime_log_path.c_str(), 1);
    const auto adopted_fd = std::to_string(transport[0]);
    ::setenv("MINE_TELEOP_CHASSIS_TEST_FD", adopted_fd.c_str(), 1);
    auto runtime_config = valid_v2_config("mt-test", 180);
    runtime_config.speed_feedback_timeout_ms = 100;
    expect(
        mine_teleop_chassis_open_v2(&runtime_config) == 0,
        "V2 bridge did not adopt the unprivileged smoke transport");
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_FD");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_vehicle_states.clear();
      g_vendor_update_threads.clear();
    }

    auto feedback = runtime_feedback(3, 1, 2, 11.1);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "standby high-speed feedback injection failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    feedback = runtime_feedback(3, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "initial runtime feedback injection failed");
    const std::array<double, 4> steering{0.2, 0.2, 0.2, 0.2};
    expect(
        mine_teleop_chassis_apply_state(
            1, 1.0, 0.01, steering.data(), steering.size()) == -3,
        "traction without an active runtime profile was not rejected");
    auto runtime_profile =
        valid_runtime_control_config(1, 5.0, 41.25, 0.0);
    MineTeleopChassisRuntimeControlResultV1 runtime_profile_result{};
    auto wrong_size_runtime_profile = runtime_profile;
    wrong_size_runtime_profile.struct_size =
        sizeof(MineTeleopChassisRuntimeControlConfigV1);
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &wrong_size_runtime_profile, &runtime_profile_result) == -1 &&
            runtime_profile_result.issue_id ==
                MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_ARGUMENTS_INVALID &&
            runtime_profile_result.applied_revision == 0,
        "runtime-control V2 accepted the legacy V1 struct size");
    const int initial_profile_result =
        mine_teleop_chassis_configure_runtime_control_v2(
            &runtime_profile, &runtime_profile_result);
    expect(
        initial_profile_result == 0 &&
            runtime_profile_result.result_code == 0 &&
            runtime_profile_result.issue_id ==
                MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_NONE &&
            runtime_profile_result.applied_revision == 1,
        "initial parked Standby runtime profile was not applied atomically: code=" +
            std::to_string(initial_profile_result) + " issue=" +
            std::to_string(runtime_profile_result.issue_id));
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &runtime_profile, &runtime_profile_result) == -3 &&
            runtime_profile_result.issue_id ==
                MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_STALE_REVISION &&
            runtime_profile_result.applied_revision == 0,
        "duplicate runtime profile revision was not rejected without partial commit");

    for (const double invalid_rise_rate :
         {std::numeric_limits<double>::quiet_NaN(), -1.0, 32000.1}) {
      auto invalid_rise_profile =
          valid_runtime_control_config(2, 5.0, 41.25, 0.0, invalid_rise_rate);
      expect(
          mine_teleop_chassis_configure_runtime_control_v2(
              &invalid_rise_profile, &runtime_profile_result) == -1 &&
              runtime_profile_result.issue_id ==
                  MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_ARGUMENTS_INVALID &&
              runtime_profile_result.applied_revision == 0,
          "out-of-envelope rise rate was not rejected before any commit");
    }
    // A rise-rate-only change shares the PID parking gate: it applies while
    // parked in Standby, and restoring the open-config value keeps the
    // downstream torque expectations valid.
    auto rise_rate_profile =
        valid_runtime_control_config(2, 5.0, 41.25, 0.0, 50.0);
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &rise_rate_profile, &runtime_profile_result) == 0 &&
            runtime_profile_result.applied_revision == 2,
        "parked Standby rise-rate-only profile change was rejected");
    auto restore_rise_rate_profile =
        valid_runtime_control_config(3, 5.0, 41.25, 0.0, 0.0);
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &restore_rise_rate_profile, &runtime_profile_result) == 0 &&
            runtime_profile_result.applied_revision == 3,
        "parked Standby rise-rate restore was rejected");
    expect(
        mine_teleop_chassis_apply_state(
            1, 5.1, 0.01, steering.data(), steering.size()) == -3,
        "runtime profile limit violation was not rejected");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0,
        "standby speed incorrectly latched an authority-state hard fuse or the fresh gate rejected the initial handshake");
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "fresh zero-speed N-to-D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "runtime did not reach parallel-handshake wait");
    static_cast<void>(drain_can_frames(transport[1], 540));
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "first handshake wait incorrectly armed the post-Ready apply or critical-feedback watchdog");
    complete_runtime_arming_to_ready(3);
    feedback = runtime_feedback(5, 3, 1, 0.0);

    auto ready_pid_update = runtime_profile;
    ready_pid_update.profile_revision = 4;
    ready_pid_update.speed_pid_kp = 2.0;
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &ready_pid_update, &runtime_profile_result) == -3 &&
            runtime_profile_result.issue_id ==
                MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_PARKING_REQUIRED &&
            runtime_profile_result.applied_revision == 0,
        "Ready-state PID update bypassed the stopped Standby/Disarmed gate");
    auto ready_rise_rate_update = runtime_profile;
    ready_rise_rate_update.profile_revision = 4;
    ready_rise_rate_update.motor_torque_rise_rate_nm_per_s = 50.0;
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &ready_rise_rate_update, &runtime_profile_result) == -3 &&
            runtime_profile_result.issue_id ==
                MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_PARKING_REQUIRED &&
            runtime_profile_result.applied_revision == 0,
        "Ready-state rise-rate update bypassed the stopped Standby/Disarmed gate");

    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_forced_vendor_motor_torque_nm = 1000.0;
    }
    static_cast<void>(drain_can_frames(transport[1], 10));
    for (int index = 0; index < 4; ++index) {
      expect(
          mine_teleop_chassis_update_feedback(&feedback) == 0 &&
              mine_teleop_chassis_apply_state(
                  3, 5.0, 0.01, steering.data(), steering.size()) == 0,
          "ready direct-torque feedback or apply refresh failed");
      static_cast<void>(drain_can_frames(transport[1], 45));
    }
    expect(mine_teleop_chassis_update_feedback(&feedback) == 0, "ready feedback refresh failed");
    const auto pid_frames = drain_can_frames(transport[1], 40);
    const auto& pid_motor = last_frame_with_id(pid_frames, 0x18F0D0F5U);
    const auto& pid_speed = last_frame_with_id(pid_frames, 0x18FED0F5U);
    expect(
        can_signal(pid_motor, 8, 14) == 8412,
        "normalized speed PID did not directly reach the 41.2 Nm per-motor session limit");
    expect(
        can_signal(pid_speed, 0, 8) == 0 && can_signal(pid_speed, 8, 8) == 0,
        "Ready local PID emitted a VCU vehicle-speed request");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      expect(
          g_vehicle_states.size() >= 3,
          "vendor control was not recalculated over multiple 20 ms ticks without another apply");
      expect(
          g_vendor_update_threads.size() == 1,
          "runtime vendor updates executed from more than the single IO thread");
      expect(
          g_vehicle_states.back().target_acceleration[0] == 0.0F,
          "traction PID output still entered the vendor acceleration-to-torque path");
      expect(
          g_controls.front().wheel_torque == 1000.0,
          "forced vendor torque fixture did not exercise the direct-torque override");
    }
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_forced_vendor_motor_torque_nm = -1.0;
    }

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "pre-software-ESTOP Ready-D feedback refresh failed");
    static_cast<void>(drain_can_frames(transport[1], 10));
    const MineTeleopChassisStopContextV1 operator_stop_context{
        sizeof(MineTeleopChassisStopContextV1),
        MINE_TELEOP_CHASSIS_STOP_SOURCE_DRIVER_PAGE,
        MINE_TELEOP_CHASSIS_STOP_REASON_OPERATOR_ESTOP,
        0U};
    expect(
        mine_teleop_chassis_set_stop_context_v1(&operator_stop_context) == 0 &&
            mine_teleop_chassis_emergency_stop() == 0,
        "Ready-D software emergency stop was rejected");
    const auto software_stop_frames = drain_can_frames(transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(software_stop_frames, 0x18F0D0F5U), 8, 14) ==
                8000 &&
            can_signal(
                last_frame_with_id(software_stop_frames, 0x18FFD0F5U), 4, 12) >
                0 &&
            can_signal(
                last_frame_with_id(software_stop_frames, 0x18FED0F5U), 0, 8) ==
                0 &&
            can_signal(
                last_frame_with_id(software_stop_frames, 0x18FED0F5U), 8, 8) ==
                0,
        "Ready-D software emergency did not produce zero torque, EHB braking, and speed Q0");
    MineTeleopChassisTelemetry software_stop_telemetry{};
    MineTeleopChassisHandshakeStatus software_stop_status{};
    expect(
        mine_teleop_chassis_read_telemetry(&software_stop_telemetry) == 0 &&
            software_stop_telemetry.estop == 1 &&
            software_stop_telemetry.stop_source ==
                MINE_TELEOP_CHASSIS_STOP_SOURCE_DRIVER_PAGE &&
            software_stop_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_OPERATOR_ESTOP &&
            software_stop_telemetry.stop_sequence > 0 &&
            mine_teleop_chassis_read_handshake_status(&software_stop_status) == 0 &&
            software_stop_status.state == MINE_TELEOP_VCU_READY &&
            software_stop_status.ready == 1 &&
            software_stop_status.disarming == 0,
        "Ready-D software emergency unexpectedly left Ready before reset");

    const std::array<double, 4> zero_steering{};
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                3, 0.0, 0.0, zero_steering.data(), zero_steering.size()) == 0,
        "same-D zero-output software ESTOP reset was rejected");
    expect(
        wait_until(
            [] {
              MineTeleopChassisTelemetry telemetry{};
              MineTeleopChassisHandshakeStatus status{};
              return mine_teleop_chassis_read_telemetry(&telemetry) == 0 &&
                  mine_teleop_chassis_read_handshake_status(&status) == 0 &&
                  telemetry.estop == 0 &&
                  status.state == MINE_TELEOP_VCU_READY &&
                  status.ready == 1 &&
                  status.disarming == 0;
            },
            100),
        "same-D zero output did not clear software ESTOP while retaining Ready");
    const auto software_reset_frames = drain_can_frames(transport[1], 30);
    expect(
        can_signal(
            last_frame_with_id(software_reset_frames, 0x18F0D0F5U), 8, 14) ==
                8000 &&
            can_signal(
                last_frame_with_id(software_reset_frames, 0x18FFD0F5U), 4, 12) ==
                0,
        "same-D zero-output reset retained stale traction or braking");

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "fresh D traction was rejected after software ESTOP reset");
    const auto software_recovery_frames = drain_can_frames(transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(software_recovery_frames, 0x18F0D0F5U), 8, 14) >
            8000,
        "fresh D intent did not restore traction after software ESTOP reset");
    expect(
        mine_teleop_chassis_read_telemetry(&software_stop_telemetry) == 0 &&
            software_stop_telemetry.estop == 0 &&
            mine_teleop_chassis_read_handshake_status(&software_stop_status) == 0 &&
            software_stop_status.state == MINE_TELEOP_VCU_READY,
        "fresh D traction did not retain cleared ESTOP and Ready state");

    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, -0.3, steering.data(), steering.size()) == 0,
        "ordinary brake intent was rejected");
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
    const auto brake_frames = drain_can_frames(transport[1], 25);
    const auto& brake_motor = last_frame_with_id(brake_frames, 0x18F0D0F5U);
    const auto& brake_ehb = last_frame_with_id(brake_frames, 0x18FFD0F5U);
    const auto& brake_speed = last_frame_with_id(brake_frames, 0x18FED0F5U);
    expect(
        can_signal(brake_motor, 8, 14) == 8000,
        "brake intent retained traction torque");
    expect(
        can_signal(brake_ehb, 4, 12) == 30,
        "brake intent did not continue through ChassisControl to EHB pressure");
    expect(
        can_signal(brake_speed, 0, 8) == 0 &&
            can_signal(brake_speed, 8, 8) == 0,
        "brake intent emitted a VCU vehicle-speed request");

    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_forced_vendor_brake_pressure_bar = 400.0;
    }
    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, -0.3, steering.data(), steering.size()) == 0,
        "legacy vendor-pressure cap test intent was rejected");
    const auto vendor_pressure_cap_frames = drain_can_frames(transport[1], 45);
    expect_all_motor_torque_raw(
        vendor_pressure_cap_frames,
        8000,
        "legacy vendor pressure cap");
    expect_all_brake_pressure_raw(
        vendor_pressure_cap_frames,
        3276,
        "legacy vendor pressure cap");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_forced_vendor_brake_pressure_bar = -1.0;
    }

    const auto preexisting_control_apply_timeouts =
        count_logged_events(runtime_log_path, "control_apply_timeout");
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "traction did not resume after ordinary brake release");
    feedback = runtime_feedback(5, 3, 1, 4.9);
    for (int index = 0; index < 4; ++index) {
      std::this_thread::sleep_for(std::chrono::milliseconds(55));
      expect(mine_teleop_chassis_update_feedback(&feedback) == 0, "watchdog feedback refresh failed");
    }
    const auto watchdog_frames = drain_can_frames(transport[1], 40);
    const auto& watchdog_motor = last_frame_with_id(watchdog_frames, 0x18F0D0F5U);
    const auto& watchdog_ehb = last_frame_with_id(watchdog_frames, 0x18FFD0F5U);
    const auto& watchdog_speed = last_frame_with_id(watchdog_frames, 0x18FED0F5U);
    expect(
        can_signal(watchdog_motor, 8, 14) == 8000 &&
            can_signal(watchdog_ehb, 4, 12) > 0 &&
            can_signal(watchdog_speed, 0, 8) == 0 &&
            can_signal(watchdog_speed, 8, 8) == 0,
        "apply timeout did not produce zero torque, EHB safe braking, and Q0");
    MineTeleopChassisTelemetry timeout_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(&timeout_telemetry) == 0 &&
            timeout_telemetry.estop == 1 &&
            timeout_telemetry.stop_source ==
                MINE_TELEOP_CHASSIS_STOP_SOURCE_WATCHDOG &&
            timeout_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_CONTROL_APPLY_TIMEOUT,
        "apply timeout did not expose the local ESTOP state");

    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "normal apply did not recover the non-hard apply watchdog latch");
    const auto post_timeout_recovery_frames = drain_can_frames(transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(post_timeout_recovery_frames, 0x18F0D0F5U),
            8,
            14) > 8000,
        "post-timeout apply did not restart direct torque from zero");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      expect(
          !g_vehicle_states.empty() &&
              g_vehicle_states.back().target_acceleration[0] == 0.0F,
          "post-timeout traction re-entered the vendor acceleration-to-torque path");
    }
    feedback = runtime_feedback(5, 3, 1, 1.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "pre-shift moving feedback refresh failed");
    MineTeleopChassisApplyResultV1 initial_moving_neutral_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            1,
            0.0,
            0.0,
            steering.data(),
            steering.size(),
            &initial_moving_neutral_result) == -3 &&
            initial_moving_neutral_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "initial moving D-to-N request bypassed the post-Ready gear-change gate");
    const auto initial_moving_neutral_frames = drain_can_frames(transport[1], 35);
    expect_all_motor_torque_raw(
        initial_moving_neutral_frames,
        8000,
        "initial moving D-to-N rejection");
    expect(
        can_signal(
            last_frame_with_id(initial_moving_neutral_frames, 0x18FCD0F5U),
            8,
            8) == 3,
        "initial moving D-to-N rejection did not retain D");
    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "retained D zero-traction command did not clear the initial N rejection");

    MineTeleopChassisApplyResultV1 moving_shift_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            2,
            0.0,
            0.0,
            steering.data(),
            steering.size(),
            &moving_shift_result) == -3 &&
            moving_shift_result.struct_size ==
                sizeof(MineTeleopChassisApplyResultV1) &&
            moving_shift_result.result_code == -3 &&
            moving_shift_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "moving D-to-R rejection did not return the stable gear-gate issue");
    expect(
        mine_teleop_chassis_apply_state(
            2, 0.0, 0.0, steering.data(), steering.size()) == -3,
        "legacy apply entry point did not remain fail-closed for a moving shift");

    MineTeleopChassisApplyResultV1 moving_neutral_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            1,
            0.0,
            0.0,
            steering.data(),
            steering.size(),
            &moving_neutral_result) == -3 &&
            moving_neutral_result.result_code == -3 &&
            moving_neutral_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "moving D-to-N request bypassed the post-Ready gear-change gate");
    const auto moving_neutral_frames = drain_can_frames(transport[1], 35);
    expect_all_motor_torque_raw(
        moving_neutral_frames,
        8000,
        "moving D-to-N rejection");
    expect(
        can_signal(
            last_frame_with_id(
                moving_neutral_frames,
                0x18FCD0F5U),
            8,
            8) == 3,
        "moving D-to-N rejection did not retain the accepted D request");
    MineTeleopChassisTelemetry moving_neutral_telemetry{};
    MineTeleopChassisHandshakeStatus moving_neutral_status{};
    expect(
        mine_teleop_chassis_read_telemetry(&moving_neutral_telemetry) == 0 &&
            moving_neutral_telemetry.estop == 0 &&
            mine_teleop_chassis_read_handshake_status(&moving_neutral_status) == 0 &&
            moving_neutral_status.state == MINE_TELEOP_VCU_READY,
        "moving D-to-N rejection entered WaitGear or latched ESTOP");

    feedback = runtime_feedback(5, 3, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "stopped feedback refresh after moving shift rejection failed");
    MineTeleopChassisApplyResultV1 stopped_repeated_shift_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            2,
            0.0,
            0.0,
            steering.data(),
            steering.size(),
            &stopped_repeated_shift_result) == -3 &&
            stopped_repeated_shift_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "rejected R heartbeat became accepted automatically after speed reached zero");
    MineTeleopChassisApplyResultV1 retained_drive_traction_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            3,
            5.0,
            0.01,
            steering.data(),
            steering.size(),
            &retained_drive_traction_result) == -3 &&
            retained_drive_traction_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "positive traction in retained D cleared the rejected-gear interlock");
    MineTeleopChassisApplyResultV1 rejected_shift_brake_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            2,
            0.0,
            -0.3,
            steering.data(),
            steering.size(),
            &rejected_shift_brake_result) == -3 &&
            rejected_shift_brake_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "braking alongside the inhibited R request unexpectedly cleared the interlock");
    const auto rejected_shift_brake_frames = drain_can_frames(transport[1], 35);
    expect_all_motor_torque_raw(
        rejected_shift_brake_frames,
        8000,
        "braking during rejected gear change");
    expect_all_brake_pressure_raw(
        rejected_shift_brake_frames,
        30,
        "braking during rejected gear change");
    expect(
        can_signal(
            last_frame_with_id(rejected_shift_brake_frames, 0x18FCD0F5U),
            8,
            8) == 3,
        "braking during rejected R request did not retain D");

    MineTeleopChassisApplyResultV1 retained_drive_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            3,
            0.0,
            0.0,
            steering.data(),
            steering.size(),
            &retained_drive_result) == 0 &&
            retained_drive_result.result_code == 0 &&
            retained_drive_result.issue_id == MINE_TELEOP_CHASSIS_APPLY_ISSUE_NONE,
        "retained D zero-traction heartbeat did not recover after moving shift rejection");

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "pre-shift feedback refresh failed");
    MineTeleopChassisApplyResultV1 recovered_shift_result{
        0U,
        -99,
        MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        99U};
    expect(
        mine_teleop_chassis_apply_state_v2(
            2,
            0.0,
            0.0,
            steering.data(),
            steering.size(),
            &recovered_shift_result) == 0 &&
            recovered_shift_result.struct_size ==
                sizeof(MineTeleopChassisApplyResultV1) &&
            recovered_shift_result.result_code == 0 &&
            recovered_shift_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_NONE &&
            recovered_shift_result.reserved == 0U,
        "fresh stopped D-to-R intent was rejected or retained the stale issue");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
        "post-Ready gear change did not enter retained WaitGear");
    expect(
        wait_until(
            [&] {
              expect(
                  mine_teleop_chassis_update_feedback(&feedback) == 0,
                  "retained WaitGear watchdog feedback refresh failed");
              MineTeleopChassisHandshakeStatus status{};
              return mine_teleop_chassis_read_handshake_status(&status) == 0 &&
                  status.state == MINE_TELEOP_VCU_DISARM_TORQUE;
            },
            1500),
        "post-Ready WaitGear did not retain the apply watchdog or enter safe disarm");
    const auto retained_watchdog_frames = drain_can_frames(transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(retained_watchdog_frames, 0x18F0D0F5U),
            8,
            14) == 8000 &&
            can_signal(
                last_frame_with_id(retained_watchdog_frames, 0x18FFD0F5U),
                4,
                12) > 0,
        "retained WaitGear apply timeout did not produce zero torque and EHB safety braking");
    complete_runtime_disarm(3, "retained WaitGear apply-watchdog");

    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0,
        "post-watchdog disarmed controller could not request a new handshake");
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "post-watchdog D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "post-watchdog runtime did not restart the handshake");
    complete_runtime_arming_to_ready(3);

    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "zero-traction intent was rejected before Ready overspeed checking");
    feedback = runtime_feedback(5, 3, 1, 11.1);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "Ready overspeed feedback injection failed");
    std::this_thread::sleep_for(std::chrono::milliseconds(35));
    MineTeleopChassisTelemetry hard_speed_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(&hard_speed_telemetry) == 0 &&
            hard_speed_telemetry.estop == 1,
        "Ready zero-traction overspeed did not latch the local ESTOP");
    const auto hard_speed_frames = drain_can_frames(transport[1], 40);
    expect(
        can_signal(last_frame_with_id(hard_speed_frames, 0x18F0D0F5U), 8, 14) ==
                8000 &&
            can_signal(last_frame_with_id(hard_speed_frames, 0x18FFD0F5U), 4, 12) >
                0,
        "Ready hard-speed fuse did not produce zero torque and EHB safety braking");
    MineTeleopChassisApplyResultV1 hard_speed_apply_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            3,
            5.0,
            0.01,
            steering.data(),
            steering.size(),
            &hard_speed_apply_result) == -3 &&
            hard_speed_apply_result.result_code == -3 &&
            hard_speed_apply_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_HARD_OVERSPEED_LATCHED &&
            hard_speed_apply_result.issue_id !=
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "ordinary apply cleared or misclassified the Ready hard-speed latch");
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == -3,
        "legacy apply entry point cleared the Ready hard-speed latch");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == -2,
        "hard-speed latch cleared without completed Disarmed recovery");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "hard-speed latch prevented explicit disarm");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_DISARM_TORQUE),
        "explicit hard-speed recovery did not start the reverse handshake");
    complete_runtime_disarm(3, "hard-speed recovery");

    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0,
        "fresh stopped N/EPB/manual handshake did not clear the hard-speed latch");
    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "post-hard-speed D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "post-hard-speed runtime did not restart the handshake");
    complete_runtime_arming_to_ready(3);

    feedback = runtime_feedback(5, 3, 1, -0.2);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "Ready-D opposite-direction feedback injection failed");
    MineTeleopChassisTelemetry opposite_direction_telemetry{};
    expect(
        wait_until(
            [&] {
              return mine_teleop_chassis_read_telemetry(
                         &opposite_direction_telemetry) == 0 &&
                  opposite_direction_telemetry.estop == 1 &&
                  opposite_direction_telemetry.stop_source ==
                      MINE_TELEOP_CHASSIS_STOP_SOURCE_SOFTWARE_FAULT &&
                  opposite_direction_telemetry.stop_reason ==
                      MINE_TELEOP_CHASSIS_STOP_REASON_OPPOSITE_DIRECTION_MOTION;
            },
            100),
        "Ready-D reverse motion did not latch opposite-direction stop provenance");
    expect(
        opposite_direction_telemetry.stop_sequence > 0,
        "opposite-direction stop did not advance its provenance sequence");
    const auto opposite_direction_stop_sequence =
        opposite_direction_telemetry.stop_sequence;
    const auto opposite_direction_frames = drain_can_frames(transport[1], 40);
    expect_all_motor_torque_raw(
        opposite_direction_frames,
        8000,
        "Ready-D opposite-direction stop");
    expect(
        can_signal(
            last_frame_with_id(opposite_direction_frames, 0x18FFD0F5U),
            4,
            12) > 0 &&
            can_signal(
                last_frame_with_id(opposite_direction_frames, 0x18FED0F5U),
                8,
                8) == 0,
        "opposite-direction stop did not produce EHB safety braking and speed Q0");
    MineTeleopChassisApplyResultV1 opposite_direction_apply_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            3,
            5.0,
            0.01,
            steering.data(),
            steering.size(),
            &opposite_direction_apply_result) == -3 &&
            opposite_direction_apply_result.result_code == -3 &&
            opposite_direction_apply_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_HARD_OVERSPEED_LATCHED,
        "ordinary apply cleared the opposite-direction safety latch");
    MineTeleopChassisTelemetry retained_opposite_direction_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(
            &retained_opposite_direction_telemetry) == 0 &&
            retained_opposite_direction_telemetry.estop == 1 &&
            retained_opposite_direction_telemetry.stop_source ==
                MINE_TELEOP_CHASSIS_STOP_SOURCE_SOFTWARE_FAULT &&
            retained_opposite_direction_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_OPPOSITE_DIRECTION_MOTION &&
            retained_opposite_direction_telemetry.stop_sequence ==
                opposite_direction_stop_sequence,
        "rejected apply replaced the latched opposite-direction root cause");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == -2,
        "opposite-direction latch cleared without completed Disarmed recovery");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "opposite-direction latch prevented explicit disarm");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_DISARM_TORQUE),
        "explicit opposite-direction recovery did not start the reverse handshake");
    complete_runtime_disarm(3, "opposite-direction recovery");

    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0,
        "post-opposite-direction handshake recovery failed");
    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "post-opposite-direction D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "post-opposite-direction runtime did not restart the handshake");
    complete_runtime_arming_to_ready(3);

    feedback = runtime_feedback(5, 3, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "pre-arming-motion feedback refresh failed");
    expect(
        mine_teleop_chassis_apply_state(
            2, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "fresh stopped shift intent before arming-motion test was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
        "arming-motion test did not reach WaitGear");
    feedback = runtime_feedback(5, 3, 1, 0.2);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "WaitGear motion feedback injection failed");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_DISARM_TORQUE),
        "fresh WaitGear motion above 0.1 m/s did not latch and enter safe disarm");
    const auto arming_motion_frames = drain_can_frames(transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(arming_motion_frames, 0x18F0D0F5U), 8, 14) ==
                8000 &&
            can_signal(
                last_frame_with_id(arming_motion_frames, 0x18FFD0F5U), 4, 12) >
                0,
        "WaitGear motion latch did not produce zero torque and EHB safety braking");
    expect(
        mine_teleop_chassis_apply_state(
            2, 0.0, 0.0, steering.data(), steering.size()) == -3,
        "ordinary apply cleared the WaitGear motion latch");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "WaitGear motion latch prevented explicit disarm");
    complete_runtime_disarm(3, "WaitGear motion recovery");

    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0,
        "post-arming-motion handshake recovery failed");
    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "post-arming-motion D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "post-arming-motion runtime did not restart the handshake");
    complete_runtime_arming_to_ready(3);

    feedback = runtime_feedback(5, 3, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "pre-critical-watchdog feedback refresh failed");
    expect(
        mine_teleop_chassis_apply_state(
            2, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "critical-watchdog shift intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
        "critical-watchdog test did not reach retained WaitGear");
    feedback = runtime_feedback(5, 2, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "critical-watchdog R feedback injection failed");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_ACTUATOR_MODES),
        "critical-watchdog test did not reach retained WaitActuatorModes");
    for (int index = 0; index < 4; ++index) {
      static_cast<void>(drain_can_frames(transport[1], 100));
      expect(
          mine_teleop_chassis_apply_state(
              2, 0.0, 0.0, steering.data(), steering.size()) == 0,
          "fresh apply was rejected before the retained critical-feedback deadline");
    }
    static_cast<void>(drain_can_frames(transport[1], 140));
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_FAULT),
        "post-Ready WaitActuatorModes did not retain the critical-feedback watchdog");
    const auto critical_frames = drain_can_frames(transport[1], 40);
    expect(
        can_signal(last_frame_with_id(critical_frames, 0x18F0D0F5U), 8, 14) ==
                8000 &&
            can_signal(last_frame_with_id(critical_frames, 0x18FFD0F5U), 4, 12) >
                0,
        "retained critical-feedback timeout did not produce zero torque and EHB safety braking");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "critical-feedback fault prevented explicit disarm");
    complete_runtime_disarm(2, "critical-feedback fault recovery");
    expect(mine_teleop_chassis_close() == 0, "runtime smoke bridge did not close cleanly");
    ::close(transport[0]);
    ::close(transport[1]);

    const auto runtime_events = read_json_lines(runtime_log_path);
    expect(
        count_logged_events(runtime_log_path, "control_apply_timeout") ==
            preexisting_control_apply_timeouts + 2,
        "Ready and retained-WaitGear apply watchdogs did not each add one latch/log");
    expect(
        std::count_if(runtime_events.begin(), runtime_events.end(), [](const auto& event) {
          return event.value("name", "") == "hard_overspeed_latched";
        }) == 1,
        "Ready hard overspeed did not latch/log exactly once");
    expect(
        std::count_if(runtime_events.begin(), runtime_events.end(), [](const auto& event) {
          return event.value("name", "") ==
                  "opposite_direction_motion_latched" &&
              event.value("issue_code", "") ==
                  "vcu_opposite_direction_motion" &&
              event.value("safety_action", "") == "local_full_stop";
        }) == 1,
        "Ready opposite-direction motion did not latch/log exactly once");
    expect(
        std::count_if(runtime_events.begin(), runtime_events.end(), [](const auto& event) {
          return event.value("name", "") == "arming_motion_latched";
        }) == 1,
        "stationary WaitGear motion did not latch/log exactly once");
    expect(
        std::count_if(runtime_events.begin(), runtime_events.end(), [](const auto& event) {
          return event.value("name", "") == "feedback_timeout";
        }) == 1,
        "post-Ready retained-state critical feedback did not time out exactly once");
    expect(
        std::any_of(runtime_events.begin(), runtime_events.end(), [](const auto& event) {
          return event.value("name", "") == "control_apply_rejected" &&
              event.value("issue_code", "") ==
                  "vcu_drive_gear_change_moving_or_stale" &&
              event.value("safety_action", "") ==
                  "traction_withdrawn_retained_gear";
        }),
        "gear rejection log did not distinguish retained-gear traction withdrawal from ESTOP");
    for (const std::string issue_code : {
             "vcu_runtime_control_profile_inactive",
             "vcu_runtime_control_profile_limit_exceeded"}) {
      expect(
          std::any_of(
              runtime_events.begin(),
              runtime_events.end(),
              [&](const auto& event) {
                return event.value("name", "") == "control_apply_rejected" &&
                    event.value("issue_code", "") == issue_code &&
                    event.value("safety_action", "") == "traction_withdrawn";
              }),
          issue_code + " log incorrectly claimed a full stop");
    }
    std::filesystem::remove(runtime_log_path, error);

    int pressure_transport[2]{-1, -1};
    expect(
        ::socketpair(AF_UNIX, SOCK_DGRAM, 0, pressure_transport) == 0,
        "V4 pressure/rise-rate adopted transport could not be created");
    const auto pressure_log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-pressure-smoke.jsonl");
    std::filesystem::remove(pressure_log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", pressure_log_path.c_str(), 1);
    const auto pressure_adopted_fd = std::to_string(pressure_transport[0]);
    ::setenv("MINE_TELEOP_CHASSIS_TEST_FD", pressure_adopted_fd.c_str(), 1);
    auto pressure_config = valid_v4_config("mt-test", 800, 300.0);
    pressure_config.full_scale_motor_torque_nm = 640.0;
    pressure_config.max_ordinary_brake_pressure_bar = 327.6;
    expect(
        mine_teleop_chassis_open_v4(&pressure_config) == 0,
        "V4 bridge did not adopt the physical-pressure/rise-rate smoke transport");
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_FD");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_vehicle_states.clear();
    }
    feedback = runtime_feedback(3, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "V4 pressure/rise-rate runtime initial feedback failed");
    // The legacy 88-byte V1 profile cannot override rise-rate shaping; the
    // following ramp therefore proves that the V4 open-time 300 Nm/s value is
    // retained while the old symbol remains usable.
    auto pressure_profile =
        valid_legacy_runtime_control_config(1, 10.0, 100.0, 327.6);
    pressure_profile.speed_pid_kp = 0.19;
    pressure_profile.speed_pid_ki = 1.0;
    pressure_profile.speed_pid_kd = 0.0;
    auto wrong_size_legacy_profile = pressure_profile;
    wrong_size_legacy_profile.struct_size =
        sizeof(MineTeleopChassisRuntimeControlConfigV2);
    expect(
        mine_teleop_chassis_configure_runtime_control_v1(
            &wrong_size_legacy_profile, &runtime_profile_result) == -1 &&
            runtime_profile_result.issue_id ==
                MINE_TELEOP_CHASSIS_RUNTIME_CONTROL_ISSUE_ARGUMENTS_INVALID &&
            runtime_profile_result.applied_revision == 0,
        "runtime-control V1 accepted the V2 struct size");
    expect(
        mine_teleop_chassis_configure_runtime_control_v1(
            &pressure_profile, &runtime_profile_result) == 0 &&
            runtime_profile_result.applied_revision == 1 &&
            mine_teleop_chassis_request_parallel_handshake() == 0 &&
            mine_teleop_chassis_apply_state(
                3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "V4 pressure/rise-rate runtime initial gate, handshake, or D intent failed");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "V4 pressure/rise-rate runtime did not begin arming");
    complete_runtime_arming_to_ready(3);
    feedback = runtime_feedback(5, 3, 1, 0.0);
    static_cast<void>(drain_can_frames(pressure_transport[1], 20));

    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_forced_vendor_motor_torque_nm = 1000.0;
    }
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                3, 5.0, 0.1, steering.data(), steering.size()) == 0,
        "V4 forward torque-cap intent was rejected");
    const auto first_forward_ramp_frames =
        drain_can_frames(pressure_transport[1], 60);
    expect(
        std::any_of(
            first_forward_ramp_frames.begin(),
            first_forward_ramp_frames.end(),
            [](const auto& frame) {
              if ((frame.can_id & CAN_EFF_MASK) != 0x18F0D0F5U) {
                return false;
              }
              const auto raw = can_signal(frame, 8, 14);
              return raw > 8000 && raw < 9000;
            }),
        "V4 configured rise rate did not bound the initial forward torque below the session cap");

    // Keep the proportional term above the actuator-reachable ceiling for
    // less than the 100 Nm / 300 Nm/s ramp duration. A PID that integrates
    // behind an outer slew limiter will retain torque after feedback reaches
    // the target; the actuator-aware ceiling must instead leave no hidden I.
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                3, 5.0, 0.1, steering.data(), steering.size()) == 0,
        "V4 actuator-aware anti-windup refresh failed");
    static_cast<void>(drain_can_frames(pressure_transport[1], 45));
    feedback = runtime_feedback(5, 3, 1, 5.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                3, 5.0, 0.1, steering.data(), steering.size()) == 0,
        "V4 actuator-aware anti-windup target-speed update failed");
    const auto no_hidden_integral_frames =
        drain_can_frames(pressure_transport[1], 80);
    expect_all_motor_torque_raw(
        no_hidden_integral_frames,
        8000,
        "V4 actuator-aware anti-windup at target speed");

    auto wait_for_direct_torque_raw = [&](int gear,
                                          std::uint64_t expected_raw,
                                          const std::string& context) {
      for (int attempt = 0; attempt < 20; ++attempt) {
        expect(
            mine_teleop_chassis_update_feedback(&feedback) == 0 &&
                mine_teleop_chassis_apply_state(
                    gear, 5.0, 0.1, steering.data(), steering.size()) == 0,
            context + " refresh failed");
        const auto frames = drain_can_frames(pressure_transport[1], 45);
        const auto latest_first_motor = std::find_if(
            frames.rbegin(),
            frames.rend(),
            [](const auto& frame) {
              return (frame.can_id & CAN_EFF_MASK) == 0x18F0D0F5U;
            });
        if (latest_first_motor != frames.rend() &&
            can_signal(*latest_first_motor, 8, 14) == expected_raw) {
          // The datagram drain may stop midway through a 16-frame transmit
          // batch, leaving later channels from the preceding cycle. Hold the
          // cap for one more complete cycle before comparing all eight.
          expect(
              mine_teleop_chassis_update_feedback(&feedback) == 0 &&
                  mine_teleop_chassis_apply_state(
                      gear,
                      5.0,
                      0.1,
                      steering.data(),
                      steering.size()) == 0,
              context + " stable-cap refresh failed");
          const auto stable_frames =
              drain_can_frames(pressure_transport[1], 80);
          expect_all_motor_torque_raw(
              stable_frames,
              expected_raw,
              context);
          return;
        }
      }
      throw std::runtime_error(
          context + " did not reach the expected torque before deadline");
    };

    feedback = runtime_feedback(5, 3, 1, 0.0);
    wait_for_direct_torque_raw(
        3,
        9000,
        "V4 D +100 Nm direct session cap");

    expect(
        mine_teleop_chassis_apply_state(
            2, 5.0, 0.1, steering.data(), steering.size()) == 0,
        "V4 stopped D-to-R torque-cap intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
        "V4 stopped D-to-R change did not enter gear wait");
    feedback = runtime_feedback(5, 2, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_ACTUATOR_MODES),
        "V4 stopped D-to-R change did not accept reverse feedback");
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_READY),
        "V4 stopped D-to-R change did not return to Ready");
    static_cast<void>(drain_can_frames(pressure_transport[1], 20));
    expect(
        mine_teleop_chassis_apply_state(
            2, 5.0, 0.1, steering.data(), steering.size()) == 0,
        "V4 reverse torque-cap intent was rejected");
    const auto first_reverse_ramp_frames =
        drain_can_frames(pressure_transport[1], 35);
    const auto first_reverse_ramp_raw = can_signal(
        last_frame_with_id(first_reverse_ramp_frames, 0x18F0D0F5U),
        8,
        14);
    expect(
        first_reverse_ramp_raw > 7000 && first_reverse_ramp_raw < 8000,
        "V4 D-to-R transition retained old torque or bypassed the configured reverse ramp");
    wait_for_direct_torque_raw(
        2,
        7000,
        "V4 R -100 Nm direct session cap");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_forced_vendor_motor_torque_nm = -1.0;
    }

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, -(30.0 / 327.6), steering.data(), steering.size()) == 0,
        "V4 30 bar service-brake intent was rejected");
    const auto service_brake_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(
        service_brake_frames,
        8000,
        "V4 service brake");
    expect_all_brake_pressure_raw(
        service_brake_frames,
        300,
        "V4 service brake");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      expect(
          !g_vehicle_states.empty() &&
              std::abs(g_vehicle_states.back().target_acceleration[0]) < 1e-9 &&
              std::abs(g_vehicle_states.back().target_steering_angle[0]) > 1e-6,
          "V4 service brake retained traction input or discarded steering");
    }

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, -(100.0 / 327.6), steering.data(), steering.size()) == 0,
        "V4 100 bar hard-brake intent was rejected");
    const auto hard_brake_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(hard_brake_frames, 8000, "V4 hard brake");
    expect_all_brake_pressure_raw(hard_brake_frames, 1000, "V4 hard brake");
    MineTeleopChassisTelemetry ordinary_brake_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(&ordinary_brake_telemetry) == 0 &&
            ordinary_brake_telemetry.estop == 0,
        "ordinary 100 bar pressure incorrectly latched ESTOP");

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, -1.0, steering.data(), steering.size()) == 0,
        "V4 ordinary code-maximum brake intent was rejected");
    const auto max_ordinary_brake_frames =
        drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(
        max_ordinary_brake_frames,
        8000,
        "V4 maximum ordinary brake");
    expect_all_brake_pressure_raw(
        max_ordinary_brake_frames,
        3276,
        "V4 maximum ordinary brake");
    expect(
        mine_teleop_chassis_read_telemetry(&ordinary_brake_telemetry) == 0 &&
            ordinary_brake_telemetry.estop == 0,
        "ordinary 327.6 bar code maximum incorrectly latched ESTOP");

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "V4 ordinary brake release was rejected");
    const auto release_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(release_frames, 8000, "V4 brake release");
    expect_all_brake_pressure_raw(release_frames, 0, "V4 brake release");

    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_update_vehicle_state_result = false;
    }
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_FAULT),
        "runtime UpdateVehicleState false did not latch the V4 bridge fault");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_update_vehicle_state_result = true;
    }
    const auto v4_runtime_fault_frames =
        drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(
        v4_runtime_fault_frames,
        8000,
        "V4 runtime UpdateVehicleState fault");
    expect_all_brake_pressure_raw(
        v4_runtime_fault_frames,
        4095,
        "V4 runtime UpdateVehicleState fault");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "V4 runtime UpdateVehicleState fault prevented explicit disarm");
    complete_runtime_disarm(2, "V4 runtime UpdateVehicleState fault recovery");
    expect(
        mine_teleop_chassis_close() == 0,
        "V4 pressure/rise-rate runtime did not close cleanly");
    ::close(pressure_transport[0]);
    ::close(pressure_transport[1]);
    const auto pressure_events = read_json_lines(pressure_log_path);
    expect(
        std::any_of(
            pressure_events.begin(),
            pressure_events.end(),
            [](const auto& event) {
              return event.value("name", "") == "disarm_complete";
            }) &&
            std::none_of(
                pressure_events.begin(),
                pressure_events.end(),
                [](const auto& event) {
                  return event.value("name", "") == "disarm_timeout";
                }),
        "V4 runtime UpdateVehicleState fault did not complete disarm without timeout");
    std::filesystem::remove(pressure_log_path, error);

    int arming_transport[2]{-1, -1};
    expect(
        ::socketpair(AF_UNIX, SOCK_DGRAM, 0, arming_transport) == 0,
        "arming-watchdog adopted transport could not be created");
    const auto arming_log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-arming-watchdog-smoke.jsonl");
    std::filesystem::remove(arming_log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", arming_log_path.c_str(), 1);
    const auto arming_adopted_fd = std::to_string(arming_transport[0]);
    ::setenv("MINE_TELEOP_CHASSIS_TEST_FD", arming_adopted_fd.c_str(), 1);
    expect(
        mine_teleop_chassis_open_v2(&runtime_config) == 0,
        "arming-watchdog bridge did not adopt the unprivileged transport");
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_FD");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    feedback = runtime_feedback(3, 1, 2, 0.0);
    auto arming_profile =
        valid_runtime_control_config(1, 5.0, 41.25, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_configure_runtime_control_v2(
                &arming_profile, &runtime_profile_result) == 0 &&
            mine_teleop_chassis_request_parallel_handshake() == 0,
        "arming-watchdog initial gate or handshake failed");
    expect(
        mine_teleop_chassis_apply_state(
            3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "arming-watchdog D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "arming-watchdog runtime did not reach handshake wait");
    feedback = runtime_feedback(5, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(
                MINE_TELEOP_VCU_WAIT_PARKING_BRAKE_RELEASED),
        "arming-watchdog runtime did not reach EPB-release wait");
    feedback = runtime_feedback(5, 1, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
        "arming-watchdog runtime did not reach WaitGear after EPB release");
    static_cast<void>(drain_can_frames(arming_transport[1], 540));
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_FAULT),
        "initial WaitGear CAN silence exceeded its entry grace without a safe fault");
    const auto arming_timeout_frames = drain_can_frames(arming_transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(arming_timeout_frames, 0x18F0D0F5U), 8, 14) ==
                8000 &&
            can_signal(
                last_frame_with_id(arming_timeout_frames, 0x18FFD0F5U), 4, 12) >
                0,
        "initial arming CAN silence did not produce zero torque and EHB safety braking");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "initial arming feedback fault prevented explicit disarm");
    expect(
        mine_teleop_chassis_clear_runtime_control_v1(
            &runtime_profile_result) == 0,
        "initial arming feedback fault prevented session profile clear");
    complete_runtime_disarm(1, "initial-arming feedback-fault recovery");
    arming_profile.profile_revision = 2;
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &arming_profile, &runtime_profile_result) == 0 &&
            runtime_profile_result.applied_revision == 2 &&
            mine_teleop_chassis_request_parallel_handshake() == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "profile plus handshake reconnect did not recover the completed arming-only timeout");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "recovered arming-timeout handshake could not be disconnected");
    feedback = runtime_feedback(3, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_DISARMED),
        "recovered arming-timeout disconnect did not complete manual disarm");
    MineTeleopChassisTelemetry disconnect_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(&disconnect_telemetry) == 0 &&
            disconnect_telemetry.estop == 1 &&
            disconnect_telemetry.stop_source ==
                MINE_TELEOP_CHASSIS_STOP_SOURCE_DRIVER_PAGE &&
            disconnect_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_VCU_HANDSHAKE_DISCONNECT,
        "explicit page disconnect did not expose page-disconnect provenance");
    const MineTeleopChassisStopContextV1 session_stop_context{
        sizeof(MineTeleopChassisStopContextV1),
        MINE_TELEOP_CHASSIS_STOP_SOURCE_SESSION,
        MINE_TELEOP_CHASSIS_STOP_REASON_SESSION_LOST,
        0U};
    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0 &&
            mine_teleop_chassis_set_stop_context_v1(&session_stop_context) == 0 &&
            mine_teleop_chassis_emergency_stop() == 0 &&
            mine_teleop_chassis_read_telemetry(&disconnect_telemetry) == 0 &&
            disconnect_telemetry.estop == 1 &&
            disconnect_telemetry.stop_source ==
                MINE_TELEOP_CHASSIS_STOP_SOURCE_SESSION &&
            disconnect_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_SESSION_LOST,
        "session-loss stop context was not consumed by the next stop action");
    expect(
        mine_teleop_chassis_close() == 0,
        "arming-watchdog runtime did not close cleanly");
    ::close(arming_transport[0]);
    ::close(arming_transport[1]);
    const auto arming_events = read_json_lines(arming_log_path);
    expect(
        std::count_if(arming_events.begin(), arming_events.end(), [](const auto& event) {
          return event.value("name", "") == "arming_feedback_timeout";
        }) == 1,
        "initial arming feedback deadline did not latch/log exactly once");
    std::filesystem::remove(arming_log_path, error);

    int physical_transport[2]{-1, -1};
    expect(
        ::socketpair(AF_UNIX, SOCK_DGRAM, 0, physical_transport) == 0,
        "physical-ESTOP adopted transport could not be created");
    const auto physical_log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-physical-estop-smoke.jsonl");
    std::filesystem::remove(physical_log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", physical_log_path.c_str(), 1);
    const auto physical_adopted_fd = std::to_string(physical_transport[0]);
    ::setenv("MINE_TELEOP_CHASSIS_TEST_FD", physical_adopted_fd.c_str(), 1);
    auto physical_config = valid_v2_config("mt-test", 800);
    expect(
        mine_teleop_chassis_open_v2(&physical_config) == 0,
        "physical-ESTOP bridge did not adopt the unprivileged transport");
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_FD");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");

    feedback = runtime_feedback(3, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "physical-ESTOP runtime initial feedback failed");
    auto physical_profile =
        valid_runtime_control_config(1, 5.0, 41.25, 0.0);
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &physical_profile, &runtime_profile_result) == 0 &&
            runtime_profile_result.applied_revision == 1 &&
            mine_teleop_chassis_request_parallel_handshake() == 0,
        "physical-ESTOP runtime initial gate or handshake failed");
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "physical-ESTOP runtime D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "physical-ESTOP runtime did not begin arming");
    complete_runtime_arming_to_ready(3);
    static_cast<void>(drain_can_frames(physical_transport[1], 30));

    // Send an asserted and released VehicleStatus back-to-back. The ingest-time
    // latch must retain even when both frames are drained in one 20 ms cycle.
    send_vehicle_status_feedback_frame(physical_transport[1], 3, 1);
    send_vehicle_status_feedback_frame(physical_transport[1], 3, 0);
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_DISARM_TORQUE),
        "physical emergency pulse did not latch and enter reverse disarm");
    const auto physical_stop_frames = drain_can_frames(physical_transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(physical_stop_frames, 0x18F0D0F5U), 8, 14) ==
                8000 &&
            can_signal(
                last_frame_with_id(physical_stop_frames, 0x18FFD0F5U), 4, 12) >
                0 &&
            can_signal(
                last_frame_with_id(physical_stop_frames, 0x18FED0F5U), 8, 8) ==
                0,
        "physical emergency did not transmit zero torque, EHB braking, and speed Q0");
    MineTeleopChassisTelemetry physical_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(&physical_telemetry) == 0 &&
            physical_telemetry.estop == 1 &&
            physical_telemetry.stop_source ==
                MINE_TELEOP_CHASSIS_STOP_SOURCE_PHYSICAL_EMERGENCY &&
            physical_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_PHYSICAL_EMERGENCY_SWITCH,
        "released physical emergency pulse did not remain visible as latched ESTOP");
    expect(
        mine_teleop_chassis_clear_runtime_control_v1(
            &runtime_profile_result) == 0 &&
            runtime_profile_result.result_code == 0 &&
            runtime_profile_result.applied_revision == 0,
        "runtime profile clear was rejected while a physical stop was latched");
    MineTeleopChassisApplyResultV1 physical_apply_result{};
    expect(
        mine_teleop_chassis_apply_state_v2(
            3,
            5.0,
            0.01,
            steering.data(),
            steering.size(),
            &physical_apply_result) == -3 &&
            physical_apply_result.result_code == -3 &&
            physical_apply_result.issue_id ==
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_PHYSICAL_EMERGENCY_LATCHED &&
            physical_apply_result.issue_id !=
                MINE_TELEOP_CHASSIS_APPLY_ISSUE_DRIVE_GEAR_CHANGE_MOVING_OR_STALE,
        "ordinary apply cleared or misclassified the physical emergency latch");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == -2,
        "physical emergency latch recovered before completed disarm");

    complete_runtime_disarm(3, "physical-emergency recovery");
    expect(
        mine_teleop_chassis_read_telemetry(&physical_telemetry) == 0 &&
            physical_telemetry.estop == 1,
        "completed disarm implicitly cleared physical emergency telemetry");
    physical_profile.profile_revision = 2;
    expect(
        mine_teleop_chassis_configure_runtime_control_v2(
            &physical_profile, &runtime_profile_result) == 0 &&
            runtime_profile_result.applied_revision == 2,
        "parked Disarmed runtime profile reconfiguration was rejected");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0,
        "released physical emergency did not recover through explicit stopped handshake");
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "post-physical-emergency D intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "post-physical-emergency runtime did not restart arming");
    complete_runtime_arming_to_ready(3);
    const auto recovered_frames = drain_can_frames(physical_transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(recovered_frames, 0x18F0D0F5U), 8, 14) > 8000,
        "explicit post-disarm handshake did not restore PID traction authority");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "post-physical-emergency runtime could not explicitly disarm");
    complete_runtime_disarm(3, "post-physical-emergency disconnect");

    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "handshake-revocation scenario could not start a fresh handshake");
    feedback = runtime_feedback(5, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(
                MINE_TELEOP_VCU_WAIT_PARKING_BRAKE_RELEASED),
        "handshake-revocation scenario did not accept status 5");
    send_vehicle_status_feedback_frame(physical_transport[1], 1, 0, 8);
    send_driver_switch_feedback_frame(physical_transport[1], 2, 1);
    expect(
        wait_until(
            [] {
              MineTeleopChassisHandshakeStatus status{};
              return mine_teleop_chassis_read_handshake_status(&status) == 0 &&
                  status.vmc_fault_code_valid == 1 &&
                  status.vmc_fault_code == 8 &&
                  status.parking_brake_switch_valid == 1 &&
                  status.parking_brake_switch == 2 &&
                  status.brake_pedal_switch_valid == 1 &&
                  status.brake_pedal_switch == 1;
            },
            200),
        "0x18F2/0x18F6 diagnostic fields were not exposed by the bridge");
    // Both frames may be drained before one 20 ms control tick. The later
    // status 5 must not overwrite the status-3 revocation event.
    send_handshake_feedback_frame(physical_transport[1], 3);
    send_handshake_feedback_frame(physical_transport[1], 5);
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_DISARM_TORQUE),
        "fresh status 3 did not revoke accepted status 5 and enter staged safe disarm");
    MineTeleopChassisHandshakeStatus revoked_status{};
    MineTeleopChassisTelemetry revoked_telemetry{};
    expect(
        mine_teleop_chassis_read_handshake_status(&revoked_status) == 0 &&
            revoked_status.handshake_status == 5 &&
            revoked_status.handshake_revoked == 1 &&
            revoked_status.revoked_handshake_status == 3 &&
            revoked_status.vmc_fault_code_valid == 1 &&
            revoked_status.vmc_fault_code == 8 &&
            mine_teleop_chassis_read_telemetry(&revoked_telemetry) == 0 &&
            revoked_telemetry.estop == 1 &&
            revoked_telemetry.stop_source ==
                MINE_TELEOP_CHASSIS_STOP_SOURCE_SOFTWARE_FAULT &&
            revoked_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_HANDSHAKE_REVOKED,
        "handshake revoke did not expose its status, VMC code, and stop provenance");
    const auto revoked_frames = drain_can_frames(physical_transport[1], 40);
    expect(
        can_signal(
            last_frame_with_id(revoked_frames, 0x18FCD0F5U), 0, 8) == 0,
        "handshake revoke continued transmitting ShakeReq instead of withdrawing it");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == -2,
        "handshake revoke recovered without completing disarm first");
    complete_runtime_disarm(1, "handshake-revoked recovery");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "handshake revoke did not require and accept a new explicit request");
    expect(
        mine_teleop_chassis_disconnect_parallel_handshake() == 0,
        "post-revocation explicit retry could not be disconnected");
    feedback = runtime_feedback(3, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_DISARMED),
        "post-revocation retry did not complete manual disarm");
    expect(
        mine_teleop_chassis_close() == 0,
        "physical-ESTOP runtime did not close cleanly");
    ::close(physical_transport[0]);
    ::close(physical_transport[1]);
    const auto physical_events = read_json_lines(physical_log_path);
    expect(
        std::count_if(
            physical_events.begin(),
            physical_events.end(),
            [](const auto& event) {
              return event.value("name", "") == "physical_emergency_latched";
            }) == 1,
        "physical emergency pulse did not latch/log exactly once");
    const auto revoked_event = std::find_if(
        physical_events.begin(),
        physical_events.end(),
        [](const auto& event) {
          return event.value("name", "") == "handshake_revoked";
        });
    expect(
        revoked_event != physical_events.end() &&
            std::count_if(
                physical_events.begin(),
                physical_events.end(),
                [](const auto& event) {
                  return event.value("name", "") == "handshake_revoked";
                }) == 1 &&
            revoked_event->value("vmc_fault_code", -1) == 8 &&
            revoked_event->value("parking_brake_switch", -1) == 2 &&
            revoked_event->value("brake_pedal_switch", -1) == 1 &&
            revoked_event->value("stop_source", "") == "software_fault" &&
            revoked_event->value("stop_reason", "") == "handshake_revoked",
        "handshake revoke diagnostic was missing, duplicated, or incomplete");
    std::filesystem::remove(physical_log_path, error);

    std::cout << "chassis_bridge_diagnostics_smoke=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "chassis_bridge_diagnostics_smoke=failed error="
              << error.what() << '\n';
    return 1;
  }
}
