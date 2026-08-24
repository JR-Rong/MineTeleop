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
    control.wheel_torque = normalized > 0.0
        ? direction * (g_forced_vendor_motor_torque_nm >= 0.0
              ? g_forced_vendor_motor_torque_nm
              : normalized * 300.0)
        : 0.0;
    control.wheel_speed = 0.0;
    control.ehb_brk_pres_req = g_forced_vendor_brake_pressure_bar >= 0.0
        ? g_forced_vendor_brake_pressure_bar
        : (normalized < 0.0
               ? std::min(-normalized * 10.0, 409.5)
               : 0.0);
    control.eps_ang_req = state.target_steering_angle[index];
    control.eps_ang_spd_req = 0.0;
  }
  return true;
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
    int emergency_switch) {
  can_frame frame{};
  frame.can_id = 0x18F2F5D0U | CAN_EFF_FLAG;
  frame.can_dlc = 8;
  frame.data[0] = static_cast<std::uint8_t>(
      ((gear & 0x03) << 2) | (emergency_switch & 0x03));
  expect(
      ::send(fd, &frame, sizeof(frame), 0) ==
          static_cast<ssize_t>(sizeof(frame)),
      "vehicle-status feedback frame injection failed");
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
        mine_teleop_chassis_abi_version() == 3U &&
            mine_teleop_chassis_open_config_v2_size() ==
                sizeof(MineTeleopChassisOpenConfigV2) &&
            mine_teleop_chassis_open_config_v3_size() ==
                sizeof(MineTeleopChassisOpenConfigV3),
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
    invalid_v2_config = valid_v2_config("mtmissing0");
    invalid_v2_config.struct_size = sizeof(MineTeleopChassisOpenConfigV3);
    expect(
        mine_teleop_chassis_open_v2(&invalid_v2_config) == -1,
        "V3-sized struct was accepted by legacy open_v2");
    expect(g_initialize_calls == 0, "invalid open_v3 config reached ChassisControl Initialize");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(1.0, 82.5) - 0.275) < 1e-9,
        "full throttle did not map to the configured traction acceleration");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(0.10, 82.5) - 0.0275) < 1e-9,
        "partial throttle did not scale linearly");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(1.0, 0.0)) < 1e-9,
        "zero full-scale torque did not disable traction");
    expect(
        std::abs(mine_teleop_chassis_scaled_target_acceleration(-1.0, 640.0) + 1.0) < 1e-9,
        "traction configuration changed the braking path");
    expect(
        std::abs(mine_teleop_chassis_quantize_brake_pressure_bar(100.09) - 100.0) < 1e-9 &&
            std::abs(mine_teleop_chassis_quantize_brake_pressure_bar(327.7) - 327.6) < 1e-9 &&
            mine_teleop_chassis_quantize_brake_pressure_bar(0.0) == 0.0,
        "ordinary brake pressure was not capped and quantized toward zero");
    expect(
        std::abs(mine_teleop_chassis_motor_torque_limit_nm(0.10, 41.25) - 4.1) <
            1e-9 &&
            std::abs(mine_teleop_chassis_motor_torque_limit_nm(2.0, 41.25) - 41.2) <
                1e-9 &&
            std::abs(mine_teleop_chassis_motor_torque_limit_nm(1.0, 41.2) - 41.2) <
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
        std::abs(
            mine_teleop_chassis_clamp_motor_torque_nm(4.19, 1.0, 41.25) -
            4.1) < 1e-9 &&
            std::abs(
                mine_teleop_chassis_clamp_motor_torque_nm(-4.19, 1.0, 41.25) +
                4.1) < 1e-9,
        "in-range vendor torque was not quantized toward zero at 0.1 Nm");
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
        std::abs(mine_teleop_chassis_clamp_directional_motor_torque_nm(
                     100.0, 3, 1.0, 41.25) -
                 41.2) < 1e-9 &&
            std::abs(mine_teleop_chassis_clamp_directional_motor_torque_nm(
                         -100.0, 2, 1.0, 41.25) +
                     41.2) < 1e-9 &&
            mine_teleop_chassis_clamp_directional_motor_torque_nm(
                -100.0, 3, 1.0, 41.25) == 0.0 &&
            mine_teleop_chassis_clamp_directional_motor_torque_nm(
                100.0, 2, 1.0, 41.25) == 0.0 &&
            std::abs(mine_teleop_chassis_clamp_directional_motor_torque_nm(
                         1000.0, 3, 1.0, 640.0) -
                     640.0) < 1e-9 &&
            std::abs(mine_teleop_chassis_clamp_directional_motor_torque_nm(
                         -1000.0, 2, 1.0, 640.0) +
                     640.0) < 1e-9,
        "D/R directional torque hard limit admitted opposite-sign torque");

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
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    expect(
        v1_socket_result == -3 && v2_socket_result == -3 &&
            v3_socket_result == -3,
        "missing SocketCAN interface was not rejected by every versioned open path");
    expect(
        g_initialize_calls == 3,
        "valid open_v1/open_v2/open_v3 did not each reach ChassisControl Initialize");

    const auto events = read_json_lines(log_path);
    int socket_failure_count = 0;
    bool v1_default_parameters_found = false;
    bool v2_configured_parameters_found = false;
    bool v3_physical_pressure_parameters_found = false;
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
    expect(socket_failure_count == 3, "versioned SocketCAN failure events are missing");
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
    expect(
        mine_teleop_chassis_request_parallel_handshake() == 0,
        "standby speed incorrectly latched an authority-state hard fuse or the fresh gate rejected the initial handshake");
    const std::array<double, 4> steering{0.2, 0.2, 0.2, 0.2};
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

    static_cast<void>(drain_can_frames(transport[1], 10));
    std::this_thread::sleep_for(std::chrono::milliseconds(70));
    expect(mine_teleop_chassis_update_feedback(&feedback) == 0, "ready feedback refresh failed");
    const auto pid_frames = drain_can_frames(transport[1], 40);
    const auto& pid_motor = last_frame_with_id(pid_frames, 0x18F0D0F5U);
    const auto& pid_speed = last_frame_with_id(pid_frames, 0x18FED0F5U);
    expect(
        can_signal(pid_motor, 8, 14) == 8004,
        "0.01 session traction ceiling did not cap PID output at 0.4 Nm");
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
          g_vehicle_states.back().target_acceleration[0] > 0.0013F &&
              g_vehicle_states.back().target_acceleration[0] < 0.0015F,
          "large PID error did not saturate at the session torque ceiling");
    }

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "pre-software-ESTOP Ready-D feedback refresh failed");
    static_cast<void>(drain_can_frames(transport[1], 10));
    expect(
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
            timeout_telemetry.estop == 1,
        "apply timeout did not expose the local ESTOP state");

    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == 0,
        "normal apply did not recover the non-hard apply watchdog latch");
    expect(
        wait_until(
            [] {
              std::lock_guard<std::mutex> lock(g_vendor_mutex);
              return !g_vehicle_states.empty() &&
                  g_vehicle_states.back().target_acceleration[0] >= 0.0013F;
            },
            100),
        "post-timeout apply did not run the reset PID in the 20 ms IO loop");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      expect(
          !g_vehicle_states.empty() &&
              g_vehicle_states.back().target_acceleration[0] >= 0.0013F &&
              g_vehicle_states.back().target_acceleration[0] < 0.0015F,
          "post-timeout PID retained pre-stop integral instead of restarting from reset state");
    }
    feedback = runtime_feedback(5, 3, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0,
        "pre-shift feedback refresh failed");
    expect(
        mine_teleop_chassis_apply_state(
            2, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "fresh stopped D-to-R intent was rejected");
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
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == -3,
        "ordinary apply cleared the Ready hard-speed latch");
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
          return event.value("name", "") == "arming_motion_latched";
        }) == 1,
        "stationary WaitGear motion did not latch/log exactly once");
    expect(
        std::count_if(runtime_events.begin(), runtime_events.end(), [](const auto& event) {
          return event.value("name", "") == "feedback_timeout";
        }) == 1,
        "post-Ready retained-state critical feedback did not time out exactly once");
    std::filesystem::remove(runtime_log_path, error);

    int pressure_transport[2]{-1, -1};
    expect(
        ::socketpair(AF_UNIX, SOCK_DGRAM, 0, pressure_transport) == 0,
        "V3 pressure-mode adopted transport could not be created");
    const auto pressure_log_path =
        std::filesystem::path("/tmp/mine-teleop-vcu-pressure-smoke.jsonl");
    std::filesystem::remove(pressure_log_path, error);
    ::setenv("MINE_TELEOP_VCU_LOG_PATH", pressure_log_path.c_str(), 1);
    const auto pressure_adopted_fd = std::to_string(pressure_transport[0]);
    ::setenv("MINE_TELEOP_CHASSIS_TEST_FD", pressure_adopted_fd.c_str(), 1);
    auto pressure_config = valid_v3_config("mt-test", 800);
    pressure_config.full_scale_motor_torque_nm = 640.0;
    pressure_config.max_ordinary_brake_pressure_bar = 327.6;
    expect(
        mine_teleop_chassis_open_v3(&pressure_config) == 0,
        "V3 bridge did not adopt the physical-pressure smoke transport");
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_FD");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_vehicle_states.clear();
    }
    feedback = runtime_feedback(3, 1, 2, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_request_parallel_handshake() == 0 &&
            mine_teleop_chassis_apply_state(
                3, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "V3 pressure runtime initial gate, handshake, or D intent failed");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
        "V3 pressure runtime did not begin arming");
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
                3, 5.0, 1.0, steering.data(), steering.size()) == 0,
        "V3 forward torque-cap intent was rejected");
    const auto forward_cap_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(
        forward_cap_frames,
        14400,
        "V3 D +640 Nm symmetric code cap");

    expect(
        mine_teleop_chassis_apply_state(
            2, 5.0, 1.0, steering.data(), steering.size()) == 0,
        "V3 stopped D-to-R torque-cap intent was rejected");
    expect(
        wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
        "V3 stopped D-to-R change did not enter gear wait");
    feedback = runtime_feedback(5, 2, 1, 0.0);
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_ACTUATOR_MODES),
        "V3 stopped D-to-R change did not accept reverse feedback");
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            wait_for_handshake_state(MINE_TELEOP_VCU_READY),
        "V3 stopped D-to-R change did not return to Ready");
    static_cast<void>(drain_can_frames(pressure_transport[1], 20));
    expect(
        mine_teleop_chassis_apply_state(
            2, 5.0, 1.0, steering.data(), steering.size()) == 0,
        "V3 reverse torque-cap intent was rejected");
    const auto reverse_cap_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(
        reverse_cap_frames,
        1600,
        "V3 R -640 Nm symmetric code cap");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      g_forced_vendor_motor_torque_nm = -1.0;
    }

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, -(30.0 / 327.6), steering.data(), steering.size()) == 0,
        "V3 30 bar service-brake intent was rejected");
    const auto service_brake_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(
        service_brake_frames,
        8000,
        "V3 service brake");
    expect_all_brake_pressure_raw(
        service_brake_frames,
        300,
        "V3 service brake");
    {
      std::lock_guard<std::mutex> lock(g_vendor_mutex);
      expect(
          !g_vehicle_states.empty() &&
              std::abs(g_vehicle_states.back().target_acceleration[0]) < 1e-9 &&
              std::abs(g_vehicle_states.back().target_steering_angle[0]) > 1e-6,
          "V3 service brake retained traction input or discarded steering");
    }

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, -(100.0 / 327.6), steering.data(), steering.size()) == 0,
        "V3 100 bar hard-brake intent was rejected");
    const auto hard_brake_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(hard_brake_frames, 8000, "V3 hard brake");
    expect_all_brake_pressure_raw(hard_brake_frames, 1000, "V3 hard brake");
    MineTeleopChassisTelemetry ordinary_brake_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(&ordinary_brake_telemetry) == 0 &&
            ordinary_brake_telemetry.estop == 0,
        "ordinary 100 bar pressure incorrectly latched ESTOP");

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, -1.0, steering.data(), steering.size()) == 0,
        "V3 ordinary code-maximum brake intent was rejected");
    const auto max_ordinary_brake_frames =
        drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(
        max_ordinary_brake_frames,
        8000,
        "V3 maximum ordinary brake");
    expect_all_brake_pressure_raw(
        max_ordinary_brake_frames,
        3276,
        "V3 maximum ordinary brake");
    expect(
        mine_teleop_chassis_read_telemetry(&ordinary_brake_telemetry) == 0 &&
            ordinary_brake_telemetry.estop == 0,
        "ordinary 327.6 bar code maximum incorrectly latched ESTOP");

    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
            mine_teleop_chassis_apply_state(
                2, 0.0, 0.0, steering.data(), steering.size()) == 0,
        "V3 ordinary brake release was rejected");
    const auto release_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(release_frames, 8000, "V3 brake release");
    expect_all_brake_pressure_raw(release_frames, 0, "V3 brake release");

    expect(
        mine_teleop_chassis_emergency_stop() == 0,
        "V3 software ESTOP was rejected");
    const auto v3_estop_frames = drain_can_frames(pressure_transport[1], 45);
    expect_all_motor_torque_raw(v3_estop_frames, 8000, "V3 ESTOP");
    expect_all_brake_pressure_raw(v3_estop_frames, 4095, "V3 ESTOP");
    expect(
        mine_teleop_chassis_close() == 0,
        "V3 pressure runtime did not close cleanly");
    ::close(pressure_transport[0]);
    ::close(pressure_transport[1]);
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
    expect(
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
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
    complete_runtime_disarm(1, "initial-arming feedback-fault recovery");
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
        mine_teleop_chassis_update_feedback(&feedback) == 0 &&
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
            physical_telemetry.estop == 1,
        "released physical emergency pulse did not remain visible as latched ESTOP");
    expect(
        mine_teleop_chassis_apply_state(
            3, 5.0, 0.01, steering.data(), steering.size()) == -3,
        "ordinary apply cleared the physical emergency latch");
    expect(
        mine_teleop_chassis_request_parallel_handshake() == -2,
        "physical emergency latch recovered before completed disarm");

    complete_runtime_disarm(3, "physical-emergency recovery");
    expect(
        mine_teleop_chassis_read_telemetry(&physical_telemetry) == 0 &&
            physical_telemetry.estop == 1,
        "completed disarm implicitly cleared physical emergency telemetry");
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
    std::filesystem::remove(physical_log_path, error);

    std::cout << "chassis_bridge_diagnostics_smoke=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "chassis_bridge_diagnostics_smoke=failed error="
              << error.what() << '\n';
    return 1;
  }
}
