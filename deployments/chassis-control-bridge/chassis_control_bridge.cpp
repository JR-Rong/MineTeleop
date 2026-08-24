#include "global_variables.h"
#include "mine_teleop/vcu.hpp"
#include "mine_teleop_chassis_bridge.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <linux/can.h>
#include <linux/can/raw.h>
#include <memory>
#include <mutex>
#include <net/if.h>
#include <stdexcept>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <thread>
#include <unordered_map>
#include <unistd.h>
#include <vector>

// Initialize is implemented by ChassisControl but only declared by its
// monolithic public header, which duplicates types from the internal headers
// needed for GetControlInfo(). Keep one type definition set and declare the
// function here.
bool Initialize(const VehicleParam& vehicle, const std::string& can_channel);

namespace {

using Clock = std::chrono::steady_clock;
using mine_teleop::vcu::CanFrame;
using mine_teleop::vcu::Command;
using mine_teleop::vcu::ParallelController;

constexpr int kWheelCount = 8;
constexpr double kWheelRadiusM = 0.55;
constexpr double kTrackM = 2.2;
constexpr double kWheelBaseM = 4.4;
constexpr double kMaxSteeringAngleRad = 0.5235987755982988;  // 30 degrees
constexpr double kRadiansToDegrees = 57.29577951308232;
constexpr double kFeedbackTimeoutSeconds = 0.5;
constexpr double kDisarmTimeoutSeconds = 15.0;
constexpr std::uintmax_t kDefaultLogMaxBytes = 128U * 1024U * 1024U;
constexpr int kDefaultLogRotations = 10;
constexpr std::array<std::uint32_t, 29> kCriticalFeedbackIds{
    0x18F0F5D0U, 0x18F2F5D0U, 0x18F3F5D0U, 0x18F5F5D0U, 0x18CFF4D0U,
    0x18A0F4D0U, 0x18A3F4D0U, 0x18A6F4D0U, 0x18A9F4D0U,
    0x18ACF4D0U, 0x18AFF4D0U, 0x18B2F4D0U, 0x18B5F4D0U,
    0x18A1F4D0U, 0x18A4F4D0U, 0x18A7F4D0U, 0x18AAF4D0U,
    0x18ADF4D0U, 0x18B0F4D0U, 0x18B3F4D0U, 0x18B6F4D0U,
    0x18C0F4D0U, 0x18C1F4D0U, 0x18C2F4D0U, 0x18C3F4D0U,
    0x18C8F4D0U, 0x18C9F4D0U, 0x18CAF4D0U, 0x18CBF4D0U,
};

std::mutex g_api_mutex;

int handshake_state_code(mine_teleop::vcu::State state) {
  using State = mine_teleop::vcu::State;
  switch (state) {
    case State::Standby:
      return MINE_TELEOP_VCU_STANDBY;
    case State::Initial:
      return MINE_TELEOP_VCU_INITIAL;
    case State::WaitParallelHandshake:
      return MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE;
    case State::WaitParkingBrakeReleased:
      return MINE_TELEOP_VCU_WAIT_PARKING_BRAKE_RELEASED;
    case State::WaitGear:
      return MINE_TELEOP_VCU_WAIT_GEAR;
    case State::WaitActuatorModes:
      return MINE_TELEOP_VCU_WAIT_ACTUATOR_MODES;
    case State::Ready:
      return MINE_TELEOP_VCU_READY;
    case State::DisarmTorque:
      return MINE_TELEOP_VCU_DISARM_TORQUE;
    case State::DisarmStop:
      return MINE_TELEOP_VCU_DISARM_STOP;
    case State::DisarmNeutral:
      return MINE_TELEOP_VCU_DISARM_NEUTRAL;
    case State::DisarmParkingBrake:
      return MINE_TELEOP_VCU_DISARM_PARKING_BRAKE;
    case State::DisarmManual:
      return MINE_TELEOP_VCU_DISARM_MANUAL;
    case State::Disarmed:
      return MINE_TELEOP_VCU_DISARMED;
    case State::Fault:
      return MINE_TELEOP_VCU_FAULT;
  }
  return MINE_TELEOP_VCU_FAULT;
}

bool is_disarming(mine_teleop::vcu::State state) {
  using State = mine_teleop::vcu::State;
  return state == State::DisarmTorque ||
         state == State::DisarmStop ||
         state == State::DisarmNeutral ||
         state == State::DisarmParkingBrake ||
         state == State::DisarmManual;
}

double clamp_value(double value, double minimum, double maximum) {
  return std::max(minimum, std::min(maximum, value));
}

float clamp_float(double value, double minimum, double maximum) {
  return static_cast<float>(clamp_value(value, minimum, maximum));
}

WheelParam make_wheel_param(int index) {
  WheelParam wheel{};
  wheel.feture_name = "mine-teleop";
  wheel.mu = 0.7F;
  wheel.slip_threshold = 0.2F;
  wheel.wheel_width = 0.4F;
  wheel.wheel_radius = static_cast<float>(kWheelRadiusM);
  wheel.wheel_pressure = 0.0F;
  wheel.max_electric_torque = 2500.0F;
  wheel.max_genera_torque = 1500.0F;

  const int axle = index / 2;
  const bool left = (index % 2) == 0;
  const float x = static_cast<float>((1.5 - axle) * (kWheelBaseM / 3.0));
  const float y = static_cast<float>(left ? kTrackM / 2.0 : -kTrackM / 2.0);
  wheel.wheel_position = {x, y, 0.0F};
  return wheel;
}

VehicleParam make_vehicle_param() {
  VehicleParam vehicle{};
  vehicle.vechicle_static_param.wheel_count = kWheelCount;
  vehicle.vechicle_dynamic_param.mass = 18000.0F;
  vehicle.vechicle_dynamic_param.cog_position = {0.0F, 0.0F, 1.2F};
  vehicle.vechicle_dynamic_param.wheel_params.clear();
  for (int index = 0; index < kWheelCount; ++index) {
    vehicle.vechicle_dynamic_param.wheel_params.push_back(make_wheel_param(index));
  }
  return vehicle;
}

VehicleState make_vehicle_state(
    double current_speed_mps,
    int target_gear,
    double target_vx,
    double target_ax,
    double full_scale_motor_torque_nm,
    const double* steering_values,
    int steering_count) {
  VehicleState state{};
  state.cur_velocity = clamp_float(current_speed_mps, -20.0, 20.0);
  state.target_velocity = {clamp_float(target_vx, 0.0, 20.0), 0.0F};
  // The runtime sends normalized longitudinal input. Scale traction only;
  // negative acceleration remains the independently calibrated braking path.
  const double scaled_target_ax = mine_teleop_chassis_scaled_target_acceleration(
      target_ax, full_scale_motor_torque_nm);
  state.target_acceleration = {clamp_float(scaled_target_ax, -8.0, 4.0), 0.0F};
  state.target_gear = target_gear;
  state.target_position = {0.0F, 0.0F, 0.0F};
  state.vehicle_posture = {0.0F, 0.0F, 0.0F};
  state.vehicle_position = {0.0F, 0.0F, 0.0F};
  state.target_steering_angle.assign(kWheelCount, 0.0F);

  const int axis_count = std::max(
      0,
      std::min(steering_count, static_cast<int>(mine_teleop::vcu::kSteeringAxisCount)));
  for (int axis = 0; axis < axis_count; ++axis) {
    const auto angle_rad = clamp_value(steering_values[axis], -1.0, 1.0) * kMaxSteeringAngleRad;
    state.target_steering_angle[axis * 2] = static_cast<float>(angle_rad);
    state.target_steering_angle[axis * 2 + 1] = static_cast<float>(angle_rad);
  }
  state.tier_state.assign(kWheelCount, WheelState{});
  return state;
}

Command command_from_chassis_control(int gear) {
  const auto& controls = GetControlInfo();
  if (controls.size() < mine_teleop::vcu::kMotorCount) {
    throw std::runtime_error("ChassisControl did not produce eight wheel controls");
  }

  Command command;
  command.gear = gear;
  for (std::size_t index = 0; index < mine_teleop::vcu::kMotorCount; ++index) {
    command.motor_torque_nm[index] =
        clamp_value(controls[index].wheel_torque, -800.0, 838.3);
    // This integration uses torque mode. ChassisControl's wheel_speed is wheel
    // rad/s, while the DBC field is motor rpm, so no dimensionally invalid value
    // is placed in the ignored speed request.
    command.motor_speed_rpm[index] = 0.0;
    command.brake_pressure_bar[index] =
        clamp_value(controls[index].ehb_brk_pres_req, 0.0, 409.5);
  }
  for (std::size_t axis = 0; axis < mine_teleop::vcu::kSteeringAxisCount; ++axis) {
    const auto& control = controls[axis * 2U];
    command.steering_angle_deg[axis] =
        clamp_value(control.eps_ang_req, -30.0, 30.0);
    command.steering_speed_degps[axis] =
        clamp_value(control.eps_ang_spd_req * kRadiansToDegrees, 0.0, 255.0);
  }
  return command;
}

std::string utc_timestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now.time_since_epoch()) %
                            1000;
  std::tm utc{};
  gmtime_r(&seconds, &utc);
  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << milliseconds.count() << 'Z';
  return output.str();
}

std::string json_escape(std::string_view value) {
  std::ostringstream output;
  for (const unsigned char character : value) {
    switch (character) {
      case '"':
        output << "\\\"";
        break;
      case '\\':
        output << "\\\\";
        break;
      case '\b':
        output << "\\b";
        break;
      case '\f':
        output << "\\f";
        break;
      case '\n':
        output << "\\n";
        break;
      case '\r':
        output << "\\r";
        break;
      case '\t':
        output << "\\t";
        break;
      default:
        if (character < 0x20U) {
          output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                 << static_cast<int>(character) << std::dec;
        } else {
          output << static_cast<char>(character);
        }
    }
  }
  return output.str();
}

void emit_bridge_diagnostic(
    std::string_view event,
    std::string_view issue_code,
    std::string_view stage,
    std::string_view error,
    std::string_view operator_action,
    std::string_view extra = {}) {
  std::cerr << "{\"ts\":\"" << utc_timestamp()
            << "\",\"event\":\"" << json_escape(event)
            << "\",\"subsystem\":\"vcu_can\",\"severity\":\"error\""
            << ",\"issue_code\":\"" << json_escape(issue_code)
            << "\",\"stage\":\"" << json_escape(stage)
            << "\",\"retryable\":true"
            << ",\"error\":\"" << json_escape(error)
            << "\",\"operator_action\":\"" << json_escape(operator_action)
            << "\",\"safety_action\":\"local_full_stop\"";
  if (!extra.empty()) std::cerr << ',' << extra;
  std::cerr << '}' << std::endl;
}

std::string frame_json(const CanFrame& frame) {
  std::ostringstream output;
  output << "{\"id\":\"0x" << std::uppercase << std::hex << std::setw(8)
         << std::setfill('0') << frame.id << "\",\"extended\":"
         << (frame.extended ? "true" : "false") << ",\"dlc\":" << std::dec
         << static_cast<int>(frame.dlc) << ",\"data\":\"";
  for (std::size_t index = 0; index < frame.dlc && index < frame.data.size(); ++index) {
    if (index != 0) output << ' ';
    output << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
           << static_cast<int>(frame.data[index]);
  }
  output << "\"}";
  return output.str();
}

template <std::size_t Size>
void append_double_array(
    std::ostringstream& output,
    const std::array<double, Size>& values) {
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) output << ',';
    output << std::fixed << std::setprecision(3) << values[index];
  }
  output << ']';
}

template <typename T, std::size_t Size>
void append_array(std::ostringstream& output, const std::array<T, Size>& values) {
  output << '[';
  for (std::size_t index = 0; index < values.size(); ++index) {
    if (index != 0) output << ',';
    output << values[index];
  }
  output << ']';
}

std::uintmax_t positive_environment_integer(
    const char* name,
    std::uintmax_t fallback) {
  const char* value = std::getenv(name);
  if (value == nullptr || value[0] == '\0') return fallback;
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0) return fallback;
  return static_cast<std::uintmax_t>(parsed);
}

class ProtocolLogger {
 public:
  bool open(const std::string& can_interface) {
    const char* configured_path = std::getenv("MINE_TELEOP_VCU_LOG_PATH");
    path_ = configured_path != nullptr && configured_path[0] != '\0'
        ? std::filesystem::path(configured_path)
        : std::filesystem::path("/var/log/mine-teleop/vcu-can.jsonl");
    max_bytes_ = positive_environment_integer(
        "MINE_TELEOP_VCU_LOG_MAX_BYTES",
        kDefaultLogMaxBytes);
    rotations_ = static_cast<int>(std::min<std::uintmax_t>(
        positive_environment_integer(
            "MINE_TELEOP_VCU_LOG_ROTATIONS",
            kDefaultLogRotations),
        100));

    std::error_code error;
    if (!path_.parent_path().empty()) {
      std::filesystem::create_directories(path_.parent_path(), error);
      if (error) {
        emit_bridge_diagnostic(
            "vehicle_vcu_log_open_failed",
            "vcu_log_directory_create_failed",
            "vcu_log_open",
            error.message(),
            "Create the VCU log directory and grant the vehicle runtime write permission.",
            "\"log_path\":\"" + json_escape(path_.string()) + "\"");
        return false;
      }
    }
    stream_.open(path_, std::ios::out | std::ios::app);
    if (!stream_.is_open()) {
      emit_bridge_diagnostic(
          "vehicle_vcu_log_open_failed",
          "vcu_log_file_open_failed",
          "vcu_log_open",
          std::strerror(errno),
          "Check the VCU log path, parent permissions, free space, and read-only filesystem state.",
          "\"log_path\":\"" + json_escape(path_.string()) + "\"");
      return false;
    }
    bytes_ = std::filesystem::exists(path_, error)
        ? std::filesystem::file_size(path_, error)
        : 0;
    if (error) bytes_ = 0;
    last_flush_ = Clock::now();

    std::ostringstream details;
    details << "\"can_interface\":\"" << can_interface
            << "\",\"protocol\":\"JYR010_VCU_20260714\""
            << ",\"tx_period_ms\":" << mine_teleop::vcu::kTransmitPeriodMs
            << ",\"feedback_timeout_ms\":"
            << static_cast<int>(kFeedbackTimeoutSeconds * 1000.0)
            << ",\"log_max_bytes\":" << max_bytes_
            << ",\"log_rotations\":" << rotations_;
    event("session_start", details.str(), true);
    std::cerr << "{\"ts\":\"" << utc_timestamp()
              << "\",\"event\":\"vehicle_vcu_log_ready\""
              << ",\"subsystem\":\"vcu_can\",\"severity\":\"info\""
              << ",\"issue_code\":\"vcu_log_ready\",\"stage\":\"vcu_log_open\""
              << ",\"log_path\":\"" << json_escape(path_.string())
              << "\",\"can_interface\":\"" << json_escape(can_interface)
              << "\",\"operator_action\":\"No action is required.\"}"
              << std::endl;
    return true;
  }

  void close() {
    if (!stream_.is_open()) return;
    event("session_end", "", true);
    stream_.close();
  }

  void event(
      const std::string& name,
      const std::string& details,
      bool force_flush = false) {
    std::ostringstream line;
    line << "{\"ts\":\"" << utc_timestamp() << "\",\"kind\":\"event\",\"name\":\""
         << name << "\"";
    if (!details.empty()) line << ',' << details;
    line << '}';
    write(line.str(), force_flush);
  }

  void issue(
      const std::string& name,
      std::string_view issue_code,
      std::string_view stage,
      std::string_view error,
      std::string_view operator_action,
      std::string_view safety_action,
      const std::string& details = {},
      bool force_flush = true) {
    std::ostringstream fields;
    fields << "\"issue_code\":\"" << json_escape(issue_code)
           << "\",\"stage\":\"" << json_escape(stage)
           << "\",\"retryable\":true"
           << ",\"error\":\"" << json_escape(error)
           << "\",\"operator_action\":\"" << json_escape(operator_action)
           << "\",\"safety_action\":\"" << json_escape(safety_action) << "\"";
    if (!details.empty()) fields << ',' << details;
    event(name, fields.str(), force_flush);
  }

  void command(const std::string& name, const Command& command) {
    std::ostringstream details;
    details << "\"command\":\"" << name << "\",\"gear\":" << command.gear
            << ",\"motor_torque_nm\":";
    append_double_array(details, command.motor_torque_nm);
    details << ",\"motor_speed_rpm\":";
    append_double_array(details, command.motor_speed_rpm);
    details << ",\"steering_angle_deg\":";
    append_double_array(details, command.steering_angle_deg);
    details << ",\"steering_speed_degps\":";
    append_double_array(details, command.steering_speed_degps);
    details << ",\"brake_pressure_bar\":";
    append_double_array(details, command.brake_pressure_bar);
    details << ",\"fault_reset\":" << (command.fault_reset ? "true" : "false");
    event("control_parameters", details.str(), false);
  }

  void received(const CanFrame& frame, mine_teleop::vcu::State state) {
    std::ostringstream line;
    line << "{\"ts\":\"" << utc_timestamp()
         << "\",\"kind\":\"can_rx\",\"state\":\""
         << mine_teleop::vcu::state_name(state) << "\",\"frame\":"
         << frame_json(frame) << '}';
    write(line.str(), false);
  }

  void transmitted(
      const std::vector<CanFrame>& frames,
      mine_teleop::vcu::State state,
      std::uint64_t cycle,
      const std::vector<std::uint32_t>& failed_ids) {
    std::ostringstream line;
    line << "{\"ts\":\"" << utc_timestamp()
         << "\",\"kind\":\"can_tx_batch\",\"cycle\":" << cycle
         << ",\"state\":\"" << mine_teleop::vcu::state_name(state)
         << "\",\"send_ok\":" << (failed_ids.empty() ? "true" : "false")
         << ",\"failed_ids\":[";
    for (std::size_t index = 0; index < failed_ids.size(); ++index) {
      if (index != 0) line << ',';
      line << "\"0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << failed_ids[index] << "\"";
    }
    line << "],\"frames\":[";
    for (std::size_t index = 0; index < frames.size(); ++index) {
      if (index != 0) line << ',';
      line << frame_json(frames[index]);
    }
    line << "]}";
    write(line.str(), false);
  }

  void feedback(
      const mine_teleop::vcu::Feedback& feedback,
      mine_teleop::vcu::State state) {
    std::ostringstream details;
    details << "\"state\":\"" << mine_teleop::vcu::state_name(state)
            << "\",\"handshake_status\":" << feedback.handshake_status
            << ",\"handshake_valid\":" << (feedback.handshake_valid ? "true" : "false")
            << ",\"gear\":" << feedback.gear
            << ",\"gear_valid\":" << (feedback.gear_valid ? "true" : "false")
            << ",\"driver_gear_request\":" << feedback.driver_gear_request
            << ",\"driver_gear_request_valid\":"
            << (feedback.driver_gear_request_valid ? "true" : "false")
            << ",\"emergency_switch\":" << feedback.emergency_switch
            << ",\"speed_mps\":" << std::fixed << std::setprecision(3)
            << feedback.speed_mps
            << ",\"speed_valid\":" << (feedback.speed_valid ? "true" : "false")
            << ",\"parking_brake_status\":";
    append_array(details, feedback.parking_brake_status);
    details << ",\"parking_brake_valid\":";
    append_array(details, feedback.parking_brake_valid);
    details << ",\"motor_mode\":";
    append_array(details, feedback.motor_mode);
    details << ",\"motor_mode_valid\":";
    append_array(details, feedback.motor_mode_valid);
    details << ",\"motor_torque_nm\":";
    append_double_array(details, feedback.motor_torque_nm);
    details << ",\"motor_torque_valid\":";
    append_array(details, feedback.motor_torque_valid);
    details << ",\"steering_mode\":";
    append_array(details, feedback.steering_mode);
    details << ",\"steering_angle_deg\":";
    append_double_array(details, feedback.steering_angle_deg);
    details << ",\"steering_valid\":";
    append_array(details, feedback.steering_valid);
    details << ",\"brake_mode\":";
    append_array(details, feedback.brake_mode);
    details << ",\"brake_pressure_bar\":";
    append_double_array(details, feedback.brake_pressure_bar);
    details << ",\"brake_valid\":";
    append_array(details, feedback.brake_valid);
    event("feedback_snapshot", details.str(), false);
  }

  [[nodiscard]] const std::filesystem::path& path() const { return path_; }

 private:
  void rotate() {
    stream_.flush();
    stream_.close();
    std::error_code error;
    const auto report_rotation_error = [&] {
      if (!error) return;
      report_write_failure(
          "vcu_log_rotation_failed",
          error.message());
      error.clear();
    };
    if (rotations_ > 0) {
      std::filesystem::remove(
          path_.string() + "." + std::to_string(rotations_),
          error);
      report_rotation_error();
      for (int index = rotations_ - 1; index >= 1; --index) {
        const auto source = std::filesystem::path(
            path_.string() + "." + std::to_string(index));
        const auto target = std::filesystem::path(
            path_.string() + "." + std::to_string(index + 1));
        if (std::filesystem::exists(source, error)) {
          report_rotation_error();
          error.clear();
          std::filesystem::rename(source, target, error);
          report_rotation_error();
        } else {
          report_rotation_error();
        }
      }
      error.clear();
      if (std::filesystem::exists(path_, error)) {
        report_rotation_error();
        error.clear();
        std::filesystem::rename(path_, path_.string() + ".1", error);
        report_rotation_error();
      } else {
        report_rotation_error();
      }
    } else {
      std::filesystem::remove(path_, error);
      report_rotation_error();
    }
    stream_.open(path_, std::ios::out | std::ios::trunc);
    bytes_ = 0;
    if (!stream_.is_open()) {
      report_write_failure(
          "vcu_log_rotation_reopen_failed",
          "log rotation could not reopen the active VCU log file");
    }
  }

  void write(const std::string& line, bool force_flush) {
    std::lock_guard<std::mutex> lock(write_mutex_);
    if (!stream_.is_open()) return;
    if (bytes_ + line.size() + 1U > max_bytes_) rotate();
    if (!stream_.is_open()) return;
    stream_ << line << '\n';
    if (!stream_) {
      report_write_failure(
          "vcu_log_write_failed",
          "writing the VCU JSONL log failed");
      return;
    }
    bytes_ += line.size() + 1U;
    const auto now = Clock::now();
    if (force_flush ||
        std::chrono::duration<double>(now - last_flush_).count() >= 1.0) {
      stream_.flush();
      if (!stream_) {
        report_write_failure(
            "vcu_log_flush_failed",
            "flushing the VCU JSONL log failed");
        return;
      }
      last_flush_ = now;
    }
  }

  void report_write_failure(
      std::string_view issue_code,
      std::string_view error) {
    if (write_failure_reported_) return;
    write_failure_reported_ = true;
    emit_bridge_diagnostic(
        "vehicle_vcu_log_write_failed",
        issue_code,
        "vcu_log_write",
        error,
        "Stop field testing, preserve available logs, and repair filesystem space/permissions.",
        "\"log_path\":\"" + json_escape(path_.string()) + "\"");
  }

  std::filesystem::path path_;
  std::ofstream stream_;
  std::uintmax_t max_bytes_{kDefaultLogMaxBytes};
  std::uintmax_t bytes_{0};
  int rotations_{kDefaultLogRotations};
  Clock::time_point last_flush_{};
  std::mutex write_mutex_;
  bool write_failure_reported_{false};
};

class SocketCan {
 public:
  ~SocketCan() { close(); }

  bool open(const std::string& interface_name) {
    if (interface_name.empty() || interface_name.size() >= IFNAMSIZ) {
      set_error("validate_interface", EINVAL);
      return false;
    }
    fd_ = ::socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (fd_ < 0) {
      set_error("socket", errno);
      return false;
    }

    ifreq request{};
    std::memcpy(request.ifr_name, interface_name.c_str(), interface_name.size() + 1U);
    if (::ioctl(fd_, SIOCGIFINDEX, &request) < 0) {
      const int error = errno;
      close();
      set_error("resolve_interface_index", error);
      return false;
    }

    sockaddr_can address{};
    address.can_family = AF_CAN;
    address.can_ifindex = request.ifr_ifindex;
    if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
      const int error = errno;
      close();
      set_error("bind", error);
      return false;
    }

    const int flags = ::fcntl(fd_, F_GETFL, 0);
    if (flags < 0) {
      const int error = errno;
      close();
      set_error("fcntl_get_flags", error);
      return false;
    }
    if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
      const int error = errno;
      close();
      set_error("fcntl_set_nonblocking", error);
      return false;
    }
    last_errno_ = 0;
    last_stage_ = "open";
    return true;
  }

  void close() {
    if (fd_ >= 0) {
      ::close(fd_);
      fd_ = -1;
    }
  }

  int receive(CanFrame& output) const {
    can_frame frame{};
    const auto read_size = ::read(fd_, &frame, sizeof(frame));
    if (read_size < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
      set_error("read", errno);
      return -errno;
    }
    if (read_size != static_cast<ssize_t>(sizeof(frame))) {
      set_error("read_size", EMSGSIZE);
      return -EMSGSIZE;
    }
    if ((frame.can_id & CAN_ERR_FLAG) != 0U || (frame.can_id & CAN_RTR_FLAG) != 0U) {
      last_frame_flags_ = frame.can_id;
      return 2;
    }

    output = CanFrame{};
    output.extended = (frame.can_id & CAN_EFF_FLAG) != 0U;
    output.id = frame.can_id & (output.extended ? CAN_EFF_MASK : CAN_SFF_MASK);
    output.dlc = std::min<std::uint8_t>(frame.can_dlc, 8);
    std::copy_n(frame.data, output.dlc, output.data.begin());
    return 1;
  }

  bool send(const CanFrame& input) const {
    can_frame frame{};
    frame.can_id = input.id | (input.extended ? CAN_EFF_FLAG : 0U);
    frame.can_dlc = std::min<std::uint8_t>(input.dlc, 8);
    std::copy_n(input.data.begin(), frame.can_dlc, frame.data);
    const auto written = ::write(fd_, &frame, sizeof(frame));
    if (written == static_cast<ssize_t>(sizeof(frame))) return true;
    set_error("write", written < 0 ? errno : EIO);
    return false;
  }

  [[nodiscard]] int last_errno() const { return last_errno_; }
  [[nodiscard]] const std::string& last_stage() const { return last_stage_; }
  [[nodiscard]] canid_t last_frame_flags() const { return last_frame_flags_; }

 private:
  void set_error(std::string stage, int error) const {
    last_stage_ = std::move(stage);
    last_errno_ = error;
  }

  int fd_{-1};
  mutable int last_errno_{0};
  mutable std::string last_stage_;
  mutable canid_t last_frame_flags_{0};
};

class BridgeRuntime {
 public:
  BridgeRuntime(std::string can_interface, double full_scale_motor_torque_nm)
      : can_interface_(std::move(can_interface)),
        full_scale_motor_torque_nm_(full_scale_motor_torque_nm) {
    last_feedback_.vehicle_speed_valid = 0;
    for (double& angle : last_feedback_.eps_angle) {
      angle = std::numeric_limits<double>::quiet_NaN();
    }
  }

  ~BridgeRuntime() {
    try {
      close();
    } catch (...) {
    }
  }

  int start(const Command& initial, const Command& emergency) {
    if (!controller_.set_command(initial) ||
        !controller_.set_emergency_command(emergency)) {
      emit_bridge_diagnostic(
          "vehicle_vcu_start_failed",
          "vcu_initial_command_invalid",
          "vcu_command_initialization",
          "initial or emergency command is outside the JYR010 command limits",
          "Check ChassisControl output units and configured vehicle parameters.");
      return -2;
    }
    if (!logger_.open(can_interface_)) return -4;
    logger_.event(
        "vehicle_parameters",
        "\"wheel_count\":8,\"mass_kg\":18000.0,\"wheel_radius_m\":0.55,"
        "\"track_m\":2.2,\"wheelbase_m\":4.4,\"max_steering_angle_deg\":30.0,"
        "\"emergency_deceleration_mps2\":-8.0,\"motor_control_mode\":\"torque\","
        "\"full_scale_motor_torque_nm\":" +
            std::to_string(full_scale_motor_torque_nm_),
        true);
    logger_.command("initial", initial);
    logger_.command("emergency", emergency);
    if (!socket_.open(can_interface_)) {
      const auto error = socket_.last_errno();
      logger_.issue(
          "socket_open_failed",
          "socketcan_open_failed",
          socket_.last_stage(),
          std::strerror(error),
          "Verify the CAN interface name, bring the interface up, and check SocketCAN permissions.",
          "control_not_started",
          "\"can_interface\":\"" + json_escape(can_interface_) +
              "\",\"errno\":" + std::to_string(error));
      logger_.close();
      return -3;
    }
    logger_.event(
        "socket_opened",
        "\"issue_code\":\"socketcan_ready\",\"stage\":\"socketcan_open\","
        "\"can_interface\":\"" +
            json_escape(can_interface_) +
            "\",\"operator_action\":\"No action is required.\"",
        true);
    running_.store(true);
    io_thread_ = std::thread(&BridgeRuntime::io_loop, this);
    return 0;
  }

  bool apply(const Command& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load() || io_error_ != 0) {
      log_operation_rejected_locked(
          "control_apply_rejected",
          "vcu_control_runtime_unavailable",
          "vcu_control_apply",
          "VCU bridge is not running or has a latched I/O error",
          "Inspect the preceding CAN fault and restart only after the interface/VCU is healthy.");
      return false;
    }
    if (!controller_.set_command(command)) {
      log_operation_rejected_locked(
          "control_apply_rejected",
          "vcu_control_command_invalid",
          "vcu_control_validate",
          "command is outside JYR010 limits or invalid for the current state",
          "Check ChassisControl units, field limits, and the current VCU handshake state.");
      return false;
    }
    software_estop_ = false;
    logger_.command("driver", command);
    return true;
  }

  bool emergency_stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load()) {
      log_operation_rejected_locked(
          "emergency_stop_rejected",
          "vcu_emergency_stop_runtime_unavailable",
          "vcu_emergency_stop",
          "VCU bridge is not running",
          "Use the independent hardware safety path and inspect why the bridge stopped.");
      return false;
    }
    controller_.emergency_stop();
    software_estop_ = true;
    logger_.event(
        "emergency_stop",
        "\"issue_code\":\"vcu_emergency_stop_applied\","
        "\"stage\":\"vcu_emergency_stop\","
        "\"operator_action\":\"Confirm the stop state and investigate the trigger before reset\","
        "\"safety_action\":\"local_full_stop\","
        "\"state\":\"" +
            std::string(mine_teleop::vcu::state_name(controller_.state())) + "\"",
        true);
    return true;
  }

  bool request_parallel_handshake() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load() || io_error_ != 0) {
      log_operation_rejected_locked(
          "parallel_handshake_rejected",
          "vcu_handshake_runtime_unavailable",
          "vcu_handshake_request",
          "VCU bridge is not running or has a latched I/O error",
          "Repair the CAN/VCU fault before requesting control authority.");
      return false;
    }
    const auto now = Clock::now();
    const auto state_before = controller_.state();
    if (!parking_gate_fresh_locked(now) || !controller_.parking_ready() ||
        !controller_.request_parallel_handshake()) {
      logger_.event(
          "parallel_handshake_rejected",
          "\"issue_code\":\"vcu_handshake_gate_rejected\","
          "\"stage\":\"vcu_handshake_gate\","
          "\"retryable\":true,"
          "\"operator_action\":\"Satisfy fresh feedback, N gear, zero speed, all electronic parking brakes applied, and manual handshake state 3\","
          "\"safety_action\":\"remain_in_standby\"," +
              handshake_gate_json_locked(now),
          true);
      return false;
    }
    software_estop_ = false;
    logger_.event(
        "parallel_handshake_requested",
        "\"issue_code\":\"vcu_handshake_requested\","
        "\"stage\":\"vcu_handshake_request\","
        "\"operator_action\":\"Wait for reused intelligent-driving handshake state 5 and all actuator readiness feedback\","
        "\"safety_action\":\"remain_stopped_until_ready\","
        "\"from\":\"" +
            std::string(mine_teleop::vcu::state_name(state_before)) +
            "\",\"to\":\"" +
            std::string(mine_teleop::vcu::state_name(controller_.state())) +
            "\"," + handshake_gate_json_locked(now),
        true);
    condition_.notify_all();
    return true;
  }

  bool request_park() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_.load()) {
      log_operation_rejected_locked(
          "parallel_handshake_disconnect_rejected",
          "vcu_disconnect_runtime_unavailable",
          "vcu_disarm_request",
          "VCU bridge is not running",
          "Use the independent hardware safety path and inspect why the bridge stopped.");
      return false;
    }
    const auto state_before = controller_.state();
    controller_.request_disarm();
    software_estop_ = true;
    if (state_before != controller_.state()) {
      logger_.event(
          "parallel_handshake_disconnect_requested",
          "\"issue_code\":\"vcu_disarm_requested\","
          "\"stage\":\"vcu_disarm_request\","
          "\"operator_action\":\"Wait for zero torque, N, EPB park, and manual handshake state 3\","
          "\"safety_action\":\"local_full_stop\","
          "\"from\":\"" +
              std::string(mine_teleop::vcu::state_name(state_before)) +
              "\",\"to\":\"" +
              std::string(mine_teleop::vcu::state_name(controller_.state())) + "\"",
          true);
    }
    condition_.notify_all();
    return true;
  }

  void inject(const MineTeleopChassisFeedback& feedback) {
    std::lock_guard<std::mutex> lock(mutex_);
    CanFrame frame{mine_teleop::vcu::ids::kWvcuHandshake};
    frame.data[1] = static_cast<std::uint8_t>(feedback.shake_hand_status);
    ingest_locked(frame);

    frame = CanFrame{mine_teleop::vcu::ids::kWvcuVehicleStatus};
    frame.data[0] = static_cast<std::uint8_t>((feedback.gear_status & 0x03) << 2);
    ingest_locked(frame);

    frame = CanFrame{mine_teleop::vcu::ids::kWvcuParkingBrake};
    for (std::size_t index = 0; index < mine_teleop::vcu::kParkingBrakeCount; ++index) {
      frame.data[index * 2U] = static_cast<std::uint8_t>(feedback.epb_status[index]);
    }
    ingest_locked(frame);

    const std::array<std::uint32_t, 8> motor_ids{
        0x18A1F4D0U, 0x18A4F4D0U, 0x18A7F4D0U, 0x18AAF4D0U,
        0x18ADF4D0U, 0x18B0F4D0U, 0x18B3F4D0U, 0x18B6F4D0U};
    const std::array<std::uint32_t, 8> motor_torque_ids{
        0x18A0F4D0U, 0x18A3F4D0U, 0x18A6F4D0U, 0x18A9F4D0U,
        0x18ACF4D0U, 0x18AFF4D0U, 0x18B2F4D0U, 0x18B5F4D0U};
    for (std::size_t index = 0; index < motor_ids.size(); ++index) {
      frame = CanFrame{motor_ids[index]};
      frame.data[0] = static_cast<std::uint8_t>((feedback.mcu_mode[index] & 0x0F) << 4);
      ingest_locked(frame);
      frame = CanFrame{motor_torque_ids[index]};
      // The compatibility ABI has no torque field. Inject a physical zero
      // rather than reusing an unobservable commanded value.
      frame.data[2] = 0x40;
      frame.data[3] = 0x1F;
      ingest_locked(frame);
    }

    const std::array<std::uint32_t, 4> steering_ids{
        0x18C0F4D0U, 0x18C1F4D0U, 0x18C2F4D0U, 0x18C3F4D0U};
    for (std::size_t index = 0; index < steering_ids.size(); ++index) {
      frame = CanFrame{steering_ids[index]};
      frame.data[0] = static_cast<std::uint8_t>(feedback.eps_mode[index]);
      const double angle = std::isfinite(feedback.eps_angle[index])
          ? clamp_value(feedback.eps_angle[index], -1575.0, 1575.0)
          : 0.0;
      const auto raw = static_cast<std::uint16_t>(std::llround((angle + 1575.0) / 0.1));
      frame.data[1] = static_cast<std::uint8_t>(raw & 0xFFU);
      frame.data[2] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
      ingest_locked(frame);
    }

    const std::array<std::uint32_t, 4> brake_ids{
        0x18C8F4D0U, 0x18C9F4D0U, 0x18CAF4D0U, 0x18CBF4D0U};
    for (std::size_t pair = 0; pair < brake_ids.size(); ++pair) {
      frame = CanFrame{brake_ids[pair]};
      frame.data[0] = static_cast<std::uint8_t>(feedback.ehb_mode[pair * 2U]);
      frame.data[4] = static_cast<std::uint8_t>(feedback.ehb_mode[pair * 2U + 1U]);
      ingest_locked(frame);
    }

    if (feedback.vehicle_speed_valid != 0) {
      frame = CanFrame{mine_teleop::vcu::ids::kWvcuVehicleSpeed};
      const auto speed_kph = clamp_value(feedback.vehicle_speed * 3.6, -500.0, 6053.5);
      const auto raw = static_cast<std::uint16_t>(std::llround((speed_kph + 500.0) / 0.1));
      frame.data[0] = static_cast<std::uint8_t>(raw & 0xFFU);
      frame.data[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
      ingest_locked(frame);
    }
    if (feedback.driver_gear_request_valid != 0) {
      frame = CanFrame{mine_teleop::vcu::ids::kWvcuDriverIntention};
      frame.data[7] = static_cast<std::uint8_t>(
          (feedback.driver_gear_request & 0x07) << 1);
      ingest_locked(frame);
    }
  }

  int poll(MineTeleopChassisFeedback& feedback) {
    std::lock_guard<std::mutex> lock(mutex_);
    feedback = last_feedback_;
    if (!running_.load()) return -2;
    if (io_error_ != 0) return io_error_;
    if (!controller_.ready() || !controller_.feedback_complete()) return 1;
    if (last_polled_generation_ == feedback_generation_) return 1;
    last_polled_generation_ = feedback_generation_;
    return 0;
  }

  MineTeleopChassisHandshakeStatus handshake_status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MineTeleopChassisHandshakeStatus status{};
    const auto state = controller_.state();
    const auto& feedback = controller_.feedback();
    status.state = handshake_state_code(state);
    status.requested = controller_.handshake_requested() ? 1 : 0;
    status.ready = controller_.ready() ? 1 : 0;
    status.disarming = is_disarming(state) ? 1 : 0;
    status.parking_ready =
        (parking_gate_fresh_locked(Clock::now()) && controller_.parking_ready())
        ? 1
        : 0;
    status.driver_gear_request = feedback.driver_gear_request;
    status.driver_gear_request_valid =
        feedback.driver_gear_request_valid ? 1 : 0;
    status.handshake_status = feedback.handshake_status;
    status.handshake_valid = feedback.handshake_valid ? 1 : 0;
    for (std::size_t index = 0; index < mine_teleop::vcu::kParkingBrakeCount; ++index) {
      status.epb_status[index] = feedback.parking_brake_status[index];
      status.epb_valid[index] = feedback.parking_brake_valid[index] ? 1 : 0;
    }
    status.speed_mps = feedback.speed_mps;
    status.speed_valid = feedback.speed_valid ? 1 : 0;
    return status;
  }

  MineTeleopChassisTelemetry telemetry() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return telemetry_;
  }

  MineTeleopChassisCanFeedbackV1 can_feedback_v1() const {
    std::lock_guard<std::mutex> lock(mutex_);
    MineTeleopChassisCanFeedbackV1 result{};
    const auto& feedback = controller_.feedback();
    const auto now = Clock::now();
    result.feedback_fresh = feedback_fresh_locked(now) ? 1 : 0;
    result.max_feedback_age_ms = 0;
    for (const auto id : kCriticalFeedbackIds) {
      const auto found = last_seen_.find(id);
      if (found == last_seen_.end()) {
        result.max_feedback_age_ms = -1;
        break;
      }
      result.max_feedback_age_ms = std::max<long long>(
          result.max_feedback_age_ms,
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - found->second).count());
    }
    result.speed_mps = feedback.speed_mps;
    result.speed_valid = feedback.speed_valid ? 1 : 0;
    result.gear = feedback.gear;
    result.gear_valid = feedback.gear_valid ? 1 : 0;
    result.emergency_switch = feedback.emergency_switch;
    result.driver_gear_request = feedback.driver_gear_request;
    result.driver_gear_request_valid =
        feedback.driver_gear_request_valid ? 1 : 0;
    result.handshake_status = feedback.handshake_status;
    result.handshake_valid = feedback.handshake_valid ? 1 : 0;
    for (std::size_t index = 0; index < mine_teleop::vcu::kParkingBrakeCount; ++index) {
      result.epb_status[index] = feedback.parking_brake_status[index];
      result.epb_valid[index] = feedback.parking_brake_valid[index] ? 1 : 0;
    }
    for (std::size_t index = 0; index < mine_teleop::vcu::kMotorCount; ++index) {
      result.motor_mode[index] = feedback.motor_mode[index];
      result.motor_mode_valid[index] = feedback.motor_mode_valid[index] ? 1 : 0;
      result.motor_torque_nm[index] = feedback.motor_torque_nm[index];
      result.motor_torque_valid[index] = feedback.motor_torque_valid[index] ? 1 : 0;
      result.motor_speed_rpm[index] = feedback.motor_speed_rpm[index];
      result.motor_speed_valid[index] = feedback.motor_speed_valid[index] ? 1 : 0;
      result.brake_mode[index] = feedback.brake_mode[index];
      result.brake_valid[index] = feedback.brake_valid[index] ? 1 : 0;
      result.brake_pressure_bar[index] = feedback.brake_pressure_bar[index];
    }
    for (std::size_t index = 0; index < mine_teleop::vcu::kSteeringAxisCount; ++index) {
      result.steering_mode[index] = feedback.steering_mode[index];
      result.steering_valid[index] = feedback.steering_valid[index] ? 1 : 0;
      result.steering_angle_deg[index] = feedback.steering_angle_deg[index];
    }
    return result;
  }

  double speed_mps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return telemetry_.speed_mps;
  }

  double full_scale_motor_torque_nm() const {
    return full_scale_motor_torque_nm_;
  }

  void log_api_failure(
      std::string_view operation,
      std::string_view issue_code,
      std::string_view error,
      std::string_view operator_action) {
    std::lock_guard<std::mutex> lock(mutex_);
    logger_.issue(
        "bridge_api_operation_failed",
        issue_code,
        operation,
        error,
        operator_action,
        "local_full_stop",
        "\"running\":" + std::string(running_.load() ? "true" : "false") +
            ",\"io_error\":" + std::to_string(io_error_));
  }

  void close() {
    if (!running_.load()) {
      if (io_thread_.joinable()) io_thread_.join();
      socket_.close();
      logger_.close();
      return;
    }

    {
      std::lock_guard<std::mutex> lock(mutex_);
      controller_.request_disarm();
      software_estop_ = true;
      logger_.event(
          "disarm_requested",
          "\"issue_code\":\"vcu_disarm_requested\","
          "\"stage\":\"vcu_close\","
          "\"operator_action\":\"Wait for disarm_complete before removing power\","
          "\"safety_action\":\"local_full_stop\","
          "\"state\":\"" +
              std::string(mine_teleop::vcu::state_name(controller_.state())) + "\"",
          true);
    }
    bool disarmed = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      disarmed = condition_.wait_for(
          lock,
          std::chrono::duration<double>(kDisarmTimeoutSeconds),
          [&] { return controller_.disarmed() || !running_.load(); });
    }
    if (!disarmed) {
      std::lock_guard<std::mutex> lock(mutex_);
      logger_.issue(
          "disarm_timeout",
          "vcu_disarm_timeout",
          "vcu_close",
          "VCU did not complete the reverse handshake within the timeout",
          "Keep the vehicle isolated; inspect feedback/state and use the independent hardware safety path.",
          "local_full_stop",
          "\"timeout_ms\":" +
              std::to_string(static_cast<int>(kDisarmTimeoutSeconds * 1000.0)) +
              ",\"state\":\"" +
              std::string(mine_teleop::vcu::state_name(controller_.state())) + "\"");
      logger_.feedback(controller_.feedback(), controller_.state());
    } else {
      std::lock_guard<std::mutex> lock(mutex_);
      logger_.event(
          "disarm_complete",
          "\"issue_code\":\"vcu_disarm_complete\","
          "\"stage\":\"vcu_close\","
          "\"operator_action\":\"No action is required.\","
          "\"safety_action\":\"local_full_stop_confirmed\"",
          true);
    }
    running_.store(false);
    condition_.notify_all();
    if (io_thread_.joinable()) io_thread_.join();
    socket_.close();
    logger_.close();
  }

 private:
  void ingest_locked(const CanFrame& frame) {
    if (!controller_.ingest(frame)) {
      ++ignored_rx_count_;
      last_ignored_rx_id_ = frame.id;
      return;
    }
    logger_.received(frame, controller_.state());
    last_seen_[frame.id] = Clock::now();
    ++feedback_generation_;
    update_feedback_locked();
  }

  bool feedback_fresh_locked(Clock::time_point now) const {
    for (const auto id : kCriticalFeedbackIds) {
      const auto found = last_seen_.find(id);
      if (found == last_seen_.end() ||
          std::chrono::duration<double>(now - found->second).count() >
              kFeedbackTimeoutSeconds) {
        return false;
      }
    }
    return true;
  }

  bool feedback_id_fresh_locked(
      std::uint32_t id,
      Clock::time_point now) const {
    const auto found = last_seen_.find(id);
    return found != last_seen_.end() &&
           std::chrono::duration<double>(now - found->second).count() <=
               kFeedbackTimeoutSeconds;
  }

  bool parking_gate_fresh_locked(Clock::time_point now) const {
    return feedback_id_fresh_locked(
               mine_teleop::vcu::ids::kWvcuHandshake,
               now) &&
           feedback_id_fresh_locked(
               mine_teleop::vcu::ids::kWvcuDriverIntention,
               now) &&
           feedback_id_fresh_locked(
               mine_teleop::vcu::ids::kWvcuVehicleSpeed,
               now) &&
           feedback_id_fresh_locked(
               mine_teleop::vcu::ids::kWvcuParkingBrake,
               now);
  }

  std::string handshake_gate_json_locked(Clock::time_point now) const {
    const auto& feedback = controller_.feedback();
    std::ostringstream output;
    output << "\"state\":\""
           << mine_teleop::vcu::state_name(controller_.state())
           << "\",\"feedback_fresh\":"
           << (parking_gate_fresh_locked(now) ? "true" : "false")
           << ",\"parking_ready\":"
           << (controller_.parking_ready() ? "true" : "false")
           << ",\"driver_gear_request\":" << feedback.driver_gear_request
           << ",\"driver_gear_request_valid\":"
           << (feedback.driver_gear_request_valid ? "true" : "false")
           << ",\"handshake_status\":" << feedback.handshake_status
           << ",\"handshake_valid\":"
           << (feedback.handshake_valid ? "true" : "false")
           << ",\"speed_mps\":" << std::fixed << std::setprecision(3)
           << feedback.speed_mps
           << ",\"speed_valid\":"
           << (feedback.speed_valid ? "true" : "false")
           << ",\"parking_brake_status\":";
    append_array(output, feedback.parking_brake_status);
    output << ",\"parking_brake_valid\":";
    append_array(output, feedback.parking_brake_valid);
    return output.str();
  }

  std::string stale_feedback_ids_locked(Clock::time_point now) const {
    std::ostringstream output;
    output << "\"stale_ids\":[";
    bool first = true;
    for (const auto id : kCriticalFeedbackIds) {
      const auto found = last_seen_.find(id);
      if (found != last_seen_.end() &&
          std::chrono::duration<double>(now - found->second).count() <=
              kFeedbackTimeoutSeconds) {
        continue;
      }
      if (!first) output << ',';
      first = false;
      output << "\"0x" << std::uppercase << std::hex << std::setw(8)
             << std::setfill('0') << id << "\"";
    }
    output << ']';
    return output.str();
  }

  std::string stale_feedback_ages_locked(Clock::time_point now) const {
    std::ostringstream output;
    output << "\"stale_feedback\":[";
    bool first = true;
    for (const auto id : kCriticalFeedbackIds) {
      const auto found = last_seen_.find(id);
      const auto age_ms = found == last_seen_.end()
          ? -1
          : std::chrono::duration_cast<std::chrono::milliseconds>(
                now - found->second)
                .count();
      if (age_ms >= 0 &&
          age_ms <= static_cast<long long>(kFeedbackTimeoutSeconds * 1000.0)) {
        continue;
      }
      if (!first) output << ',';
      first = false;
      output << "{\"id\":\"0x" << std::uppercase << std::hex << std::setw(8)
             << std::setfill('0') << id << std::dec
             << "\",\"age_ms\":" << age_ms << '}';
    }
    output << ']';
    return output.str();
  }

  void log_operation_rejected_locked(
      std::string_view name,
      std::string_view issue_code,
      std::string_view stage,
      std::string_view error,
      std::string_view operator_action) {
    const auto now = Clock::now();
    if (last_operation_rejection_code_ == issue_code &&
        now - last_operation_rejection_log_ < std::chrono::seconds(1)) {
      return;
    }
    last_operation_rejection_code_ = std::string(issue_code);
    last_operation_rejection_log_ = now;
    logger_.issue(
        std::string(name),
        issue_code,
        stage,
        error,
        operator_action,
        "local_full_stop",
        "\"running\":" + std::string(running_.load() ? "true" : "false") +
            ",\"io_error\":" + std::to_string(io_error_) +
            ",\"state\":\"" +
            std::string(mine_teleop::vcu::state_name(controller_.state())) + "\"");
  }

  void log_ignored_rx_locked(Clock::time_point now) {
    if (now < next_ignored_rx_log_) return;
    if (ignored_rx_count_ > 0) {
      std::ostringstream details;
      details << "\"ignored_count\":" << ignored_rx_count_
              << ",\"last_id\":\"0x" << std::uppercase << std::hex
              << std::setw(8) << std::setfill('0') << last_ignored_rx_id_
              << "\"";
      logger_.issue(
          "can_rx_ignored_summary",
          "can_rx_unrecognized_or_invalid",
          "can_decode",
          "CAN frames were not recognized by the JYR010 protocol decoder",
          "If an expected feedback ID is missing, verify protocol version, extended-ID flags, and DLC.",
          "none",
          details.str(),
          false);
      ignored_rx_count_ = 0;
    }
    if (special_rx_count_ > 0) {
      std::ostringstream details;
      details << "\"ignored_count\":" << special_rx_count_
              << ",\"last_can_flags\":\"0x" << std::uppercase << std::hex
              << last_special_rx_flags_ << "\"";
      logger_.issue(
          "can_error_or_rtr_frame_ignored",
          "can_error_or_rtr_frame_received",
          "socketcan_receive",
          "SocketCAN delivered CAN error or RTR frames that are not VCU feedback",
          "Inspect interface error counters and kernel CAN diagnostics if this repeats.",
          "none",
          details.str(),
          false);
      special_rx_count_ = 0;
    }
    next_ignored_rx_log_ = now + std::chrono::seconds(1);
  }

  void update_feedback_locked() {
    const auto& feedback = controller_.feedback();
    last_feedback_.shake_hand_status = feedback.handshake_status;
    last_feedback_.gear_status = feedback.gear;
    for (std::size_t index = 0; index < mine_teleop::vcu::kParkingBrakeCount; ++index) {
      last_feedback_.epb_status[index] = feedback.parking_brake_status[index];
    }
    for (std::size_t index = 0; index < mine_teleop::vcu::kMotorCount; ++index) {
      last_feedback_.mcu_mode[index] = feedback.motor_mode[index];
      last_feedback_.ehb_mode[index] = feedback.brake_mode[index];
    }
    for (std::size_t index = 0; index < mine_teleop::vcu::kSteeringAxisCount; ++index) {
      last_feedback_.eps_mode[index] = feedback.steering_mode[index];
      last_feedback_.eps_angle[index] = feedback.steering_valid[index]
          ? feedback.steering_angle_deg[index]
          : std::numeric_limits<double>::quiet_NaN();
    }
    last_feedback_.vehicle_speed = feedback.speed_mps;
    last_feedback_.vehicle_speed_valid = feedback.speed_valid ? 1 : 0;
    last_feedback_.driver_gear_request = feedback.driver_gear_request;
    last_feedback_.driver_gear_request_valid =
        feedback.driver_gear_request_valid ? 1 : 0;

    telemetry_.speed_mps = feedback.speed_valid ? feedback.speed_mps : 0.0;
    telemetry_.gear = feedback.gear_valid ? feedback.gear : 1;
    telemetry_.steering_feedback = feedback.steering_valid[0]
        ? clamp_value(feedback.steering_angle_deg[0] / 30.0, -1.0, 1.0)
        : 0.0;
    double max_positive_torque = 0.0;
    for (std::size_t index = 0; index < mine_teleop::vcu::kMotorCount; ++index) {
      if (feedback.motor_torque_valid[index]) {
        max_positive_torque = std::max(max_positive_torque, feedback.motor_torque_nm[index]);
      }
    }
    telemetry_.throttle_feedback = clamp_value(max_positive_torque / 838.3, 0.0, 1.0);
    double max_brake_pressure = 0.0;
    for (std::size_t index = 0; index < mine_teleop::vcu::kBrakeCount; ++index) {
      if (feedback.brake_valid[index]) {
        max_brake_pressure = std::max(max_brake_pressure, feedback.brake_pressure_bar[index]);
      }
    }
    telemetry_.brake_feedback = clamp_value(max_brake_pressure / 409.5, 0.0, 1.0);
    telemetry_.estop =
        (software_estop_ || feedback.emergency_switch != 0) ? 1 : 0;
  }

  void io_loop() noexcept {
    try {
      io_loop_impl();
    } catch (const std::exception& error) {
      std::lock_guard<std::mutex> lock(mutex_);
      io_error_ = -EFAULT;
      controller_.transport_fault();
      software_estop_ = true;
      try {
        logger_.issue(
            "io_thread_exception",
            "vcu_io_thread_exception",
            "vcu_io_loop",
            error.what(),
            "Keep the vehicle stopped and inspect the exception plus preceding CAN events.",
            "local_full_stop");
      } catch (...) {
      }
      running_.store(false);
      condition_.notify_all();
    } catch (...) {
      std::lock_guard<std::mutex> lock(mutex_);
      io_error_ = -EFAULT;
      controller_.transport_fault();
      software_estop_ = true;
      try {
        logger_.issue(
            "io_thread_exception",
            "vcu_io_thread_unknown_exception",
            "vcu_io_loop",
            "unknown non-standard exception",
            "Keep the vehicle stopped and inspect the preceding CAN events.",
            "local_full_stop");
      } catch (...) {
      }
      running_.store(false);
      condition_.notify_all();
    }
  }

  void io_loop_impl() {
    auto next_tick = Clock::now();
    auto next_feedback_log = Clock::now();
    auto next_deadline_log = Clock::now();
    int consecutive_send_failures = 0;
    bool receive_failure_reported = false;
    std::uint64_t transmit_cycle = 0;
    while (running_.load()) {
      for (;;) {
        CanFrame frame;
        const int receive_result = socket_.receive(frame);
        if (receive_result == 0) break;
        if (receive_result < 0) {
          std::lock_guard<std::mutex> lock(mutex_);
          io_error_ = receive_result;
          controller_.transport_fault();
          software_estop_ = true;
          if (!receive_failure_reported) {
            const auto error = -receive_result;
            logger_.issue(
                "can_receive_failed",
                "socketcan_receive_failed",
                socket_.last_stage(),
                std::strerror(error),
                "Inspect CAN interface state, kernel logs, wiring, termination, and bus-off counters.",
                "local_full_stop",
                "\"error_code\":" + std::to_string(receive_result) +
                    ",\"errno\":" + std::to_string(error) +
                    ",\"can_interface\":\"" + json_escape(can_interface_) + "\"");
            receive_failure_reported = true;
          }
          break;
        }
        if (receive_result == 1) {
          std::lock_guard<std::mutex> lock(mutex_);
          ingest_locked(frame);
        } else if (receive_result == 2) {
          std::lock_guard<std::mutex> lock(mutex_);
          ++special_rx_count_;
          last_special_rx_flags_ = socket_.last_frame_flags();
        }
      }

      std::vector<CanFrame> frames;
      mine_teleop::vcu::State transmit_state;
      {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto state_before = controller_.state();
        const auto now = Clock::now();
        log_ignored_rx_locked(now);
        if (controller_.ready() && !feedback_fresh_locked(now)) {
          io_error_ = -ETIMEDOUT;
          controller_.transport_fault();
          software_estop_ = true;
          logger_.issue(
              "feedback_timeout",
              "vcu_critical_feedback_timeout",
              "vcu_feedback_watchdog",
              "one or more critical VCU feedback IDs exceeded the freshness deadline",
              "Inspect stale_feedback ages, CAN wiring/load, VCU power/state, and protocol ID mapping.",
              "local_full_stop",
              "\"timeout_ms\":" +
                  std::to_string(static_cast<int>(kFeedbackTimeoutSeconds * 1000.0)) +
                  "," + stale_feedback_ids_locked(now) +
                  "," + stale_feedback_ages_locked(now));
        }
        frames = controller_.tick();
        transmit_state = controller_.state();
        if (state_before != transmit_state) {
          logger_.event(
              "state_transition",
              "\"from\":\"" +
                  std::string(mine_teleop::vcu::state_name(state_before)) +
                  "\",\"to\":\"" +
                  std::string(mine_teleop::vcu::state_name(transmit_state)) + "\"",
              true);
          logger_.feedback(controller_.feedback(), transmit_state);
        } else if (Clock::now() >= next_feedback_log) {
          logger_.feedback(controller_.feedback(), transmit_state);
          next_feedback_log = Clock::now() + std::chrono::milliseconds(500);
        }
        update_feedback_locked();
        condition_.notify_all();
      }

      std::vector<std::uint32_t> failed_send_ids;
      for (const auto& frame : frames) {
        if (!socket_.send(frame)) failed_send_ids.push_back(frame.id);
      }
      logger_.transmitted(
          frames,
          transmit_state,
          ++transmit_cycle,
          failed_send_ids);
      if (failed_send_ids.empty()) {
        consecutive_send_failures = 0;
      } else if (++consecutive_send_failures == 3) {
        std::lock_guard<std::mutex> lock(mutex_);
        io_error_ = -EIO;
        controller_.transport_fault();
        software_estop_ = true;
        std::ostringstream failed_ids;
        failed_ids << "\"failed_ids\":[";
        for (std::size_t index = 0; index < failed_send_ids.size(); ++index) {
          if (index != 0) failed_ids << ',';
          failed_ids << "\"0x" << std::uppercase << std::hex << std::setw(8)
                     << std::setfill('0') << failed_send_ids[index] << "\"";
        }
        failed_ids << ']';
        logger_.issue(
            "can_send_failed",
            "socketcan_send_failed",
            socket_.last_stage(),
            std::strerror(socket_.last_errno()),
            "Inspect CAN interface state, bus-off counters, wiring, and kernel logs.",
            "local_full_stop",
            "\"errno\":" + std::to_string(socket_.last_errno()) +
                ",\"consecutive_failures\":" +
                std::to_string(consecutive_send_failures) +
                ",\"failed_ids_count\":" +
                std::to_string(failed_send_ids.size()) + "," +
                failed_ids.str());
      }

      next_tick += std::chrono::milliseconds(mine_teleop::vcu::kTransmitPeriodMs);
      const auto now = Clock::now();
      if (next_tick <= now) {
        const auto lag_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - next_tick)
                                .count();
        if (now >= next_deadline_log) {
          logger_.event(
              "tx_deadline_miss",
              "\"issue_code\":\"vcu_tx_deadline_missed\","
              "\"stage\":\"vcu_tx_scheduler\","
              "\"retryable\":true,"
              "\"operator_action\":\"Check CPU scheduling latency and system load\","
              "\"safety_action\":\"monitor_and_stop_if_repeated\","
              "\"lag_ms\":" + std::to_string(lag_ms),
              false);
          next_deadline_log = now + std::chrono::seconds(1);
        }
        next_tick =
            now + std::chrono::milliseconds(mine_teleop::vcu::kTransmitPeriodMs);
      }
      std::this_thread::sleep_until(next_tick);
    }
  }

  std::string can_interface_;
  double full_scale_motor_torque_nm_;
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  SocketCan socket_;
  ProtocolLogger logger_;
  ParallelController controller_;
  std::atomic<bool> running_{false};
  std::thread io_thread_;
  std::unordered_map<std::uint32_t, Clock::time_point> last_seen_;
  std::uint64_t feedback_generation_{0};
  std::uint64_t last_polled_generation_{0};
  int io_error_{0};
  bool software_estop_{false};
  std::uint64_t ignored_rx_count_{0};
  std::uint32_t last_ignored_rx_id_{0};
  std::uint64_t special_rx_count_{0};
  canid_t last_special_rx_flags_{0};
  Clock::time_point next_ignored_rx_log_{};
  Clock::time_point last_operation_rejection_log_{};
  std::string last_operation_rejection_code_;
  MineTeleopChassisFeedback last_feedback_{};
  MineTeleopChassisTelemetry telemetry_{0.0, 1, 0.0, 0.0, 0.0, 0};
};

std::unique_ptr<BridgeRuntime> g_runtime;

int open_bridge_locked(
    const char* can_interface,
    double full_scale_motor_torque_nm) {
  if (can_interface == nullptr || can_interface[0] == '\0' || g_runtime) {
    emit_bridge_diagnostic(
        "vehicle_vcu_start_failed",
        g_runtime ? "vcu_bridge_already_open" : "vcu_can_interface_invalid",
        "bridge_open",
        g_runtime ? "VCU bridge is already open" : "CAN interface name is empty",
        g_runtime
            ? "Close the existing bridge instance before opening another."
            : "Configure a non-empty SocketCAN interface such as can0.");
    return -1;
  }

  VehicleParam vehicle = make_vehicle_param();
  if (!Initialize(vehicle, can_interface)) {
    emit_bridge_diagnostic(
        "vehicle_vcu_start_failed",
        "chassis_control_initialize_failed",
        "chassis_control_initialize",
        "ChassisControl Initialize returned false",
        "Check ChassisControl dependencies, vehicle parameters, and CAN channel configuration.");
    return -2;
  }

  const std::array<double, mine_teleop::vcu::kSteeringAxisCount> zero_steering{};
  if (!UpdateVehicleState(make_vehicle_state(
          0.0,
          1,
          0.0,
          -8.0,
          full_scale_motor_torque_nm,
          zero_steering.data(),
          static_cast<int>(zero_steering.size())))) {
    emit_bridge_diagnostic(
        "vehicle_vcu_start_failed",
        "chassis_control_emergency_seed_failed",
        "chassis_control_initial_command",
        "ChassisControl rejected the emergency-stop seed state",
        "Check ChassisControl input ranges and units.");
    return -2;
  }
  const auto emergency = command_from_chassis_control(1);

  if (!UpdateVehicleState(make_vehicle_state(
          0.0,
          1,
          0.0,
          0.0,
          full_scale_motor_torque_nm,
          zero_steering.data(),
          static_cast<int>(zero_steering.size())))) {
    emit_bridge_diagnostic(
        "vehicle_vcu_start_failed",
        "chassis_control_initial_seed_failed",
        "chassis_control_initial_command",
        "ChassisControl rejected the initial neutral seed state",
        "Check ChassisControl input ranges and units.");
    return -2;
  }
  const auto initial = command_from_chassis_control(1);

  auto runtime = std::make_unique<BridgeRuntime>(
      can_interface, full_scale_motor_torque_nm);
  const int start_result = runtime->start(initial, emergency);
  if (start_result != 0) return start_result;
  g_runtime = std::move(runtime);
  return 0;
}

int open_bridge(
    const char* can_interface,
    double full_scale_motor_torque_nm) noexcept {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    return open_bridge_locked(can_interface, full_scale_motor_torque_nm);
  } catch (const std::exception& error) {
    emit_bridge_diagnostic(
        "vehicle_vcu_start_failed",
        "vcu_bridge_open_exception",
        "bridge_open",
        error.what(),
        "Check the exception and bridge/ChassisControl runtime dependencies.");
    return -5;
  } catch (...) {
    emit_bridge_diagnostic(
        "vehicle_vcu_start_failed",
        "vcu_bridge_open_unknown_exception",
        "bridge_open",
        "unknown non-standard exception",
        "Check bridge/ChassisControl runtime dependencies.");
    return -5;
  }
}

}  // namespace

extern "C" int mine_teleop_chassis_open(const char* can_interface) {
  return open_bridge(
      can_interface,
      MINE_TELEOP_CHASSIS_DEFAULT_FULL_SCALE_MOTOR_TORQUE_NM);
}

extern "C" int mine_teleop_chassis_open_v1(
    const MineTeleopChassisOpenConfigV1* config) {
  if (config == nullptr ||
      config->struct_size != sizeof(MineTeleopChassisOpenConfigV1) ||
      !std::isfinite(config->full_scale_motor_torque_nm) ||
      config->full_scale_motor_torque_nm < 0.0 ||
      config->full_scale_motor_torque_nm >
          MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM) {
    emit_bridge_diagnostic(
        "vehicle_vcu_start_failed",
        "vcu_open_config_invalid",
        "bridge_open_config",
        "open_v1 config size or full-scale motor torque is invalid",
        "Provide the current V1 struct and a finite torque in [0, 165] Nm.");
    return -1;
  }
  return open_bridge(
      config->can_interface, config->full_scale_motor_torque_nm);
}

extern "C" int mine_teleop_chassis_apply_state(
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count) {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime || steering_values == nullptr || steering_count < 0 ||
        target_gear < 1 || target_gear > 4) {
      if (g_runtime) {
        g_runtime->log_api_failure(
            "bridge_apply_state",
            "vcu_apply_arguments_invalid",
            "invalid gear, steering pointer, or steering count",
            "Check the runtime-to-bridge ABI arguments and configured steering axes.");
      }
      return -1;
    }
    if (target_gear == 4) return g_runtime->request_park() ? 0 : -3;

    const auto state = make_vehicle_state(
        g_runtime->speed_mps(),
        target_gear,
        target_vx,
        target_ax,
        g_runtime->full_scale_motor_torque_nm(),
        steering_values,
        steering_count);
    if (!UpdateVehicleState(state)) {
      g_runtime->log_api_failure(
          "chassis_control_update",
          "chassis_control_update_failed",
          "ChassisControl UpdateVehicleState returned false",
          "Check command ranges, units, and current ChassisControl state.");
      return -2;
    }
    return g_runtime->apply(command_from_chassis_control(target_gear)) ? 0 : -3;
  } catch (const std::exception& error) {
    if (g_runtime) {
      g_runtime->log_api_failure(
          "bridge_apply_state",
          "vcu_apply_exception",
          error.what(),
          "Check ChassisControl output size/values and the bridge ABI.");
    } else {
      emit_bridge_diagnostic(
          "vehicle_vcu_api_failed",
          "vcu_apply_exception",
          "bridge_apply_state",
          error.what(),
          "Check ChassisControl output size/values and the bridge ABI.");
    }
    return -5;
  } catch (...) {
    emit_bridge_diagnostic(
        "vehicle_vcu_api_failed",
        "vcu_apply_unknown_exception",
        "bridge_apply_state",
        "unknown non-standard exception",
        "Check the bridge and ChassisControl runtime.");
    return -5;
  }
}

extern "C" int mine_teleop_chassis_emergency_stop() {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime) return -1;
    return g_runtime->emergency_stop() ? 0 : -2;
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_request_parallel_handshake() {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime) return -1;
    return g_runtime->request_parallel_handshake() ? 0 : -2;
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_disconnect_parallel_handshake() {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime) return -1;
    return g_runtime->request_park() ? 0 : -2;
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_read_handshake_status(
    MineTeleopChassisHandshakeStatus* status) {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime || status == nullptr) return -1;
    *status = g_runtime->handshake_status();
    return 0;
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_update_feedback(
    const MineTeleopChassisFeedback* feedback) {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime || feedback == nullptr) return -1;
    g_runtime->inject(*feedback);
    return 0;
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_poll_feedback(
    MineTeleopChassisFeedback* feedback) {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime || feedback == nullptr) return -1;
    return g_runtime->poll(*feedback);
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_read_telemetry(
    MineTeleopChassisTelemetry* telemetry) {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime || telemetry == nullptr) return -1;
    *telemetry = g_runtime->telemetry();
    return 0;
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_read_can_feedback_v1(
    MineTeleopChassisCanFeedbackV1* feedback) {
  try {
    std::lock_guard<std::mutex> lock(g_api_mutex);
    if (!g_runtime || feedback == nullptr) return -1;
    *feedback = g_runtime->can_feedback_v1();
    return 0;
  } catch (...) {
    return -5;
  }
}

extern "C" int mine_teleop_chassis_close() {
  try {
    std::unique_ptr<BridgeRuntime> runtime;
    {
      std::lock_guard<std::mutex> lock(g_api_mutex);
      runtime = std::move(g_runtime);
    }
    if (runtime) runtime->close();
    return 0;
  } catch (...) {
    return -5;
  }
}
