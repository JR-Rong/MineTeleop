#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/media.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/upload.hpp"
#include "mine_teleop/video.hpp"

#include <gst/gst.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mine_teleop::ControlCommand;

class TestFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

void expect_near(double actual, double expected, double epsilon, std::string_view message) {
  if (std::abs(actual - expected) > epsilon) {
    throw TestFailure(std::string(message) + ": expected " + std::to_string(expected) + ", got " +
                      std::to_string(actual));
  }
}

template <typename Function>
void expect_throws(Function&& function, std::string_view message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw TestFailure(std::string(message));
}

mine_teleop::Json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw TestFailure("cannot open JSON test vector: " + path.string());
  mine_teleop::Json value;
  input >> value;
  return value;
}

std::string read_text(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw TestFailure("cannot open test input: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void replace_once(
    std::string& value,
    std::string_view needle,
    std::string_view replacement) {
  const auto position = value.find(needle);
  if (position == std::string::npos) {
    throw TestFailure("cannot find test config anchor: " + std::string(needle));
  }
  value.replace(position, needle.size(), replacement);
}

std::filesystem::path write_temp_vehicle_config(
    std::string_view suffix,
    const std::string& contents) {
  const auto path = std::filesystem::path("/tmp") /
      ("mine-teleop-vehicle-config-" + std::string(suffix) + "-" +
       mine_teleop::random_token(6) + ".yaml");
  std::ofstream output(path);
  if (!output) throw TestFailure("cannot create temporary vehicle config");
  output << contents;
  return path;
}

ControlCommand command(std::uint64_t seq = 1, std::int64_t timestamp_ms = 0) {
  ControlCommand value;
  value.vehicle_id = "vehicle-001";
  value.driver_id = "driver-001";
  value.session_id = "session-001";
  value.seq = seq;
  value.sent_at_utc_ms = timestamp_ms;
  value.control_token = "token";
  value.gear = "D";
  value.steering = 0.25;
  value.throttle = 0.5;
  value.brake = 0.0;
  return value;
}

mine_teleop::SessionControlProfileRequest session_profile_request(
    std::uint64_t seq,
    std::int64_t timestamp_ms,
    double target_speed_kph = 20.0,
    double max_motor_torque_nm = 100.0,
    double max_brake_pressure_bar = 100.0,
    double service_brake_pressure_bar = 30.0,
    double hard_brake_pressure_bar = 100.0) {
  mine_teleop::SessionControlProfileRequest request;
  request.vehicle_id = "vehicle-001";
  request.driver_id = "driver-001";
  request.session_id = "session-001";
  request.seq = seq;
  request.sent_at_utc_ms = timestamp_ms;
  request.control_token = "token";
  request.profile.profile_version =
      mine_teleop::kSessionControlProfileVersion;
  request.profile.target_speed_kph = target_speed_kph;
  request.profile.max_motor_torque_nm = max_motor_torque_nm;
  request.profile.max_brake_pressure_bar = max_brake_pressure_bar;
  request.profile.service_brake_pressure_bar = service_brake_pressure_bar;
  request.profile.hard_brake_pressure_bar = hard_brake_pressure_bar;
  request.profile.max_steering_angle_deg = 3.0;
  request.profile.speed_pid_kp = 1.0;
  request.profile.speed_pid_ki = 0.2;
  request.profile.speed_pid_kd = 0.0;
  request.profile.speed_pid_derivative_filter_tau_ms = 100.0;
  request.profile.speed_pid_max_dt_ms = 100;
  return request;
}

void activate_session_profile(
    mine_teleop::VehicleControlService& service,
    std::uint64_t seq = 1,
    std::int64_t timestamp_ms = 0) {
  const auto result = service.receive_session_profile(
      session_profile_request(seq, timestamp_ms),
      timestamp_ms);
  expect(result.accepted, "session control profile was rejected: " + result.reason);
}

mine_teleop::Json signaling_request(
    std::string_view vehicle_id,
    std::string_view driver_id,
    std::string_view session_id,
    std::uint64_t seq,
    std::int64_t sent_at_utc_ms,
    std::string_view sender,
    std::string_view recipient,
    std::string_view credential_name,
    std::string_view credential,
    std::string_view type,
    const mine_teleop::Json& payload) {
  auto value = mine_teleop::ProtocolMetadata{
                   mine_teleop::kProtocolVersion,
                   std::string(vehicle_id),
                   std::string(driver_id),
                   std::string(session_id),
                   seq,
                   sent_at_utc_ms}
                   .to_json();
  value["sender"] = sender;
  value["recipient"] = recipient;
  value[std::string(credential_name)] = std::string(credential);
  value["type"] = type;
  value["payload"] = payload;
  return value;
}

class NoFeedbackAdapter final : public mine_teleop::VehicleAdapter {
 public:
  void open() override { opened = true; }
  void close() override { opened = false; }
  [[nodiscard]] std::uint64_t configure_runtime_control_profile(
      const mine_teleop::SessionControlProfile& profile,
      std::uint64_t revision) override {
    session_motor_torque_limit = profile.max_motor_torque_nm;
    session_brake_pressure_limit = profile.max_brake_pressure_bar;
    runtime_profile_revision = revision;
    return revision;
  }
  void clear_runtime_control_profile() override {
    session_motor_torque_limit = 0.0;
    session_brake_pressure_limit = 0.0;
    runtime_profile_revision = 0;
  }
  [[nodiscard]] double session_motor_torque_limit_nm() const override {
    return session_motor_torque_limit;
  }
  [[nodiscard]] double session_brake_pressure_limit_bar() const override {
    return session_brake_pressure_limit;
  }
  void apply_control(const ControlCommand&) override { ++applied_commands; }
  void apply_safe_stop(
      const mine_teleop::ControlOutput& output,
      mine_teleop::VehicleStopContext) override {
    last_safe_output = output;
    ++safe_stops;
  }
  bool poll_feedback() override { return false; }
  bool request_vcu_handshake() override { return false; }
  bool disconnect_vcu_handshake() override { return false; }
  [[nodiscard]] bool feedback_ready() const override { return false; }
  [[nodiscard]] mine_teleop::VcuHandshakeStatus vcu_handshake_status() const override {
    mine_teleop::VcuHandshakeStatus status;
    status.supported = true;
    status.state = "standby";
    status.parking_ready = true;
    return status;
  }
  [[nodiscard]] mine_teleop::VehicleTelemetry read_telemetry() override { return {}; }
  [[nodiscard]] mine_teleop::VehicleAdapterStatus status() const override {
    return {
        "no_feedback",
        opened,
        true,
        "can0",
        "",
        applied_commands,
        safe_stops,
        "",
        false,
        0,
        0,
    };
  }

  bool opened{false};
  std::uint64_t applied_commands{0};
  std::uint64_t safe_stops{0};
  double session_motor_torque_limit{0.0};
  double session_brake_pressure_limit{0.0};
  std::uint64_t runtime_profile_revision{0};
  mine_teleop::ControlOutput last_safe_output;
};

class AdapterOwnedSafeStopAdapter final : public mine_teleop::VehicleAdapter {
 public:
  AdapterOwnedSafeStopAdapter() {
    handshake.supported = true;
    handshake.state = "ready";
    handshake.ready = true;
    handshake.parking_ready = true;
  }

  void open() override { opened = true; }
  void close() override { opened = false; }

  [[nodiscard]] std::uint64_t configure_runtime_control_profile(
      const mine_teleop::SessionControlProfile& profile,
      std::uint64_t revision) override {
    ++control_limit_updates;
    if (control_limit_update_throws) {
      throw std::runtime_error("adapter control limit apply failed");
    }
    session_motor_torque_limit = profile.max_motor_torque_nm;
    session_brake_pressure_limit = profile.max_brake_pressure_bar;
    runtime_profile_revision = revision;
    return revision;
  }

  void clear_runtime_control_profile() override {
    session_motor_torque_limit = 0.0;
    session_brake_pressure_limit = 0.0;
    runtime_profile_revision = 0;
  }

  [[nodiscard]] double session_motor_torque_limit_nm() const override {
    return session_motor_torque_limit;
  }

  [[nodiscard]] double session_brake_pressure_limit_bar() const override {
    return session_brake_pressure_limit;
  }

  void apply_control(const ControlCommand& command) override {
    ++control_attempts;
    if (structured_rejection_issue_code) {
      throw mine_teleop::VehicleAdapterControlRejected(
          *structured_rejection_issue_code,
          -3);
    }
    if (rejected_control_gear && command.gear == *rejected_control_gear) {
      throw std::runtime_error("adapter rejected control gear");
    }
    if (owns_safe_stop()) {
      throw std::runtime_error("adapter-owned safe stop rejected ordinary control");
    }
    last_control = command;
    ++applied_commands;
  }

  void apply_safe_stop(
      const mine_teleop::ControlOutput& output,
      mine_teleop::VehicleStopContext) override {
    ++safe_stop_attempts;
    if (safe_stop_throws) {
      throw std::runtime_error("adapter safe stop failed");
    }
    if (owns_safe_stop() && !output.estop && output.brake < 1.0) {
      ++rejected_ordinary_safe_stops;
      throw std::runtime_error(
          "adapter-owned safe stop rejected duplicate ordinary safe stop");
    }
    last_safe_output = output;
    ++safe_stops;
  }

  bool poll_feedback() override {
    if (poll_throws) throw std::runtime_error("adapter feedback poll failed");
    return true;
  }

  bool request_vcu_handshake() override {
    ++handshake_requests;
    if (!handshake_succeeds) return false;
    telemetry.estop = false;
    handshake.state = "initial";
    handshake.ready = false;
    handshake.disarming = false;
    return true;
  }

  bool disconnect_vcu_handshake() override {
    telemetry.estop = true;
    handshake.state = "disarm_torque";
    handshake.ready = false;
    handshake.disarming = true;
    return true;
  }

  [[nodiscard]] bool feedback_ready() const override {
    return opened && (!handshake.supported || handshake.ready);
  }
  [[nodiscard]] mine_teleop::VcuHandshakeStatus vcu_handshake_status() const override {
    if (handshake_status_throws) {
      throw std::runtime_error("adapter handshake status failed");
    }
    return handshake;
  }
  [[nodiscard]] mine_teleop::VehicleTelemetry read_telemetry() override {
    if (telemetry_throws) throw std::runtime_error("adapter telemetry failed");
    return telemetry;
  }
  [[nodiscard]] mine_teleop::VehicleAdapterStatus status() const override {
    return {
        "adapter_owned_safe_stop",
        opened,
        true,
        "can0",
        "",
        applied_commands,
        safe_stops,
        "",
        true,
        500000,
        1000,
    };
  }

  void set_safe_stop(bool estop, bool ready, bool disarming) {
    telemetry.estop = estop;
    handshake.state = disarming ? "disarm_torque" : (ready ? "ready" : "disarmed");
    handshake.ready = ready;
    handshake.disarming = disarming;
  }

  [[nodiscard]] bool owns_safe_stop() const {
    return telemetry.estop || handshake.disarming;
  }

  bool opened{false};
  bool poll_throws{false};
  bool telemetry_throws{false};
  bool handshake_status_throws{false};
  bool handshake_succeeds{true};
  bool control_limit_update_throws{false};
  bool safe_stop_throws{false};
  std::uint64_t control_attempts{0};
  std::uint64_t applied_commands{0};
  std::uint64_t safe_stop_attempts{0};
  std::uint64_t safe_stops{0};
  std::uint64_t rejected_ordinary_safe_stops{0};
  std::uint64_t handshake_requests{0};
  std::uint64_t control_limit_updates{0};
  double session_motor_torque_limit{0.0};
  double session_brake_pressure_limit{0.0};
  std::uint64_t runtime_profile_revision{0};
  std::optional<std::string> structured_rejection_issue_code;
  std::optional<std::string> rejected_control_gear;
  std::optional<ControlCommand> last_control;
  mine_teleop::ControlOutput last_safe_output;
  mine_teleop::VehicleTelemetry telemetry;
  mine_teleop::VcuHandshakeStatus handshake;
};

void activate_adapter_owned_session_profile(
    mine_teleop::VehicleControlService& service,
    AdapterOwnedSafeStopAdapter& adapter,
    std::uint64_t seq = 1,
    std::int64_t timestamp_ms = 0) {
  adapter.handshake.state = "standby";
  adapter.handshake.ready = false;
  adapter.handshake.disarming = false;
  adapter.handshake.parking_ready = true;
  activate_session_profile(service, seq, timestamp_ms);
  adapter.set_safe_stop(false, true, false);
}

void test_config_loads_current_vehicle_yaml() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  expect(config.vehicle_id == "vehicle-001", "vehicle id mismatch");
  expect(config.control.control_timeout_ms == 800, "control timeout mismatch");
  expect(config.enabled_cameras().size() == 1, "enabled camera count mismatch");
  expect(config.realtime_profile("realtime_720p").fps == 30, "profile fps mismatch");
  expect(config.vehicle_adapter.type == "mock", "adapter type mismatch");
  expect(config.hardware.can_bitrate == 500000, "CAN bitrate mismatch");
  expect(config.hardware.can_tx_queue_length == 100, "CAN tx queue length mismatch");
  expect_near(
      config.field_safety.full_scale_motor_torque_nm,
      300.0,
      1e-9,
      "safe default full-scale motor torque changed");
  expect_near(
      config.field_safety.motor_torque_rise_rate_nm_per_s,
      0.0,
      1e-9,
      "safe default motor torque rise shaping changed");
  expect_near(
      config.field_safety.max_brake_pressure_bar,
      100.0,
      1e-9,
      "safe default ordinary brake pressure changed");
}

void test_vehicle_config_rejects_unimplemented_control_safety_options() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  auto variable_rate = base;
  replace_once(variable_rate, "rate_hz: 20", "rate_hz: 25");
  const auto variable_rate_path =
      write_temp_vehicle_config("variable-control-rate", variable_rate);
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::load_vehicle_config(variable_rate_path));
      },
      "non-20Hz configuration was accepted by the fixed upstream command rate");

  const std::vector<std::pair<std::string, std::string>> invalid_timing{
      {"max_command_gap_ms: 200", "max_command_gap_ms: 0"},
      {"degraded_timeout_ms: 300", "degraded_timeout_ms: 60001"},
      {"control_timeout_ms: 800", "control_timeout_ms: 60001"},
      {"control_timeout_ms: 800", "control_timeout_ms: 300"},
  };
  std::vector<std::filesystem::path> invalid_timing_paths;
  for (const auto& [from, to] : invalid_timing) {
    auto invalid = base;
    replace_once(invalid, from, to);
    const auto path = write_temp_vehicle_config("invalid-control-timing", invalid);
    invalid_timing_paths.push_back(path);
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_vehicle_config(path)); },
        "invalid or unbounded control timing was accepted");
  }

  auto remote_only_reset = base;
  replace_once(
      remote_only_reset,
      "require_local_estop_reset: true",
      "require_local_estop_reset: false");
  const auto remote_only_reset_path =
      write_temp_vehicle_config("remote-only-estop-reset", remote_only_reset);
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::load_vehicle_config(remote_only_reset_path));
      },
      "remote-only ESTOP reset configuration was accepted despite local confirmation being mandatory");

  std::error_code error;
  std::filesystem::remove(variable_rate_path, error);
  std::filesystem::remove(remote_only_reset_path, error);
  for (const auto& path : invalid_timing_paths) {
    std::filesystem::remove(path, error);
  }
}

void test_vehicle_config_validates_full_scale_motor_torque() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  for (const std::string invalid : {"-0.1", "640.1", ".nan"}) {
    auto contents = base;
    replace_once(
        contents,
        "field_safety:\n",
        "field_safety:\n  full_scale_motor_torque_nm: " + invalid + "\n");
    const auto path = write_temp_vehicle_config("invalid-torque", contents);
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_vehicle_config(path)); },
        "invalid full-scale motor torque was accepted");
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  auto disabled_contents = base;
  replace_once(
      disabled_contents,
      "field_safety:\n",
      "field_safety:\n  full_scale_motor_torque_nm: 0\n");
  const auto disabled_path = write_temp_vehicle_config("disabled-torque", disabled_contents);
  const auto disabled_config = mine_teleop::load_vehicle_config(disabled_path);
  expect_near(
      disabled_config.field_safety.full_scale_motor_torque_nm,
      0.0,
      1e-9,
      "zero full-scale torque did not disable traction");
  std::error_code error;
  std::filesystem::remove(disabled_path, error);

  auto non_mock_contents = base;
  replace_once(
      non_mock_contents,
      "field_safety:\n",
      "field_safety:\n"
      "  max_throttle: 0.10\n"
      "  motor_torque_rise_rate_nm_per_s: 0.0\n"
      "  speed_feedback_timeout_ms: 200\n"
      "  speed_pid_kp: 1.0\n"
      "  speed_pid_ki: 0.2\n"
      "  speed_pid_kd: 0.0\n"
      "  speed_pid_derivative_filter_tau_ms: 100.0\n"
      "  speed_pid_max_dt_ms: 100\n"
      "  hard_overspeed_margin_kph: 3.6\n"
      "  max_brake_pressure_bar: 100.0\n"
      "  max_steering_angle_deg: 5.0\n");
  replace_once(
      non_mock_contents,
      "vehicle_adapter:\n  type: mock",
      "vehicle_adapter:\n"
      "  type: dynamic_library\n"
      "  integration:\n"
      "    chassis_control:\n"
      "      can_interface: can0\n"
      "      bridge_library_path: /tmp/libmine_teleop_chassis_bridge.so");
  const auto non_mock_path = write_temp_vehicle_config(
      "missing-explicit-torque", non_mock_contents);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(non_mock_path)); },
      "non-mock adapter accepted an implicit full-scale motor torque");
  std::filesystem::remove(non_mock_path, error);
}

void test_vehicle_config_requires_physical_brake_pressure_units() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  for (const std::string invalid : {"-0.1", "327.7", ".nan"}) {
    auto contents = base;
    replace_once(
        contents,
        "field_safety:\n",
        "field_safety:\n  max_brake_pressure_bar: " + invalid + "\n");
    const auto path = write_temp_vehicle_config("invalid-brake-pressure", contents);
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_vehicle_config(path)); },
        "invalid ordinary brake pressure was accepted");
    std::error_code error;
    std::filesystem::remove(path, error);
  }

  auto boundary_contents = base;
  replace_once(
      boundary_contents,
      "field_safety:\n",
      "field_safety:\n  max_brake_pressure_bar: 327.6\n");
  const auto boundary_path =
      write_temp_vehicle_config("brake-pressure-boundary", boundary_contents);
  expect_near(
      mine_teleop::load_vehicle_config(boundary_path)
          .field_safety.max_brake_pressure_bar,
      327.6,
      1e-9,
      "80-percent DBC ordinary brake pressure boundary was rejected");
  std::error_code error;
  std::filesystem::remove(boundary_path, error);

  auto legacy_contents = base;
  replace_once(
      legacy_contents,
      "field_safety:\n",
      "field_safety:\n  max_brake: 1.0\n");
  const auto legacy_path =
      write_temp_vehicle_config("legacy-normalized-brake", legacy_contents);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(legacy_path)); },
      "legacy normalized max_brake was silently interpreted as physical pressure");
  std::filesystem::remove(legacy_path, error);
}

void test_vehicle_config_requires_monotonic_final_full_safety_brake() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  auto missing_full = base;
  replace_once(
      missing_full,
      "brake: vehicle_defined_max_safe",
      "brake: 0.9");
  const auto missing_full_path =
      write_temp_vehicle_config("timeout-missing-full-brake", missing_full);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(missing_full_path)); },
      "timeout profile without a final full safety brake was accepted");

  auto decreasing = base;
  replace_once(
      decreasing,
      "      - after_ms: 500\n        brake: 0.6",
      "      - after_ms: 500\n        brake: 0.2");
  const auto decreasing_path =
      write_temp_vehicle_config("timeout-decreasing-brake", decreasing);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(decreasing_path)); },
      "timeout profile that reduced brake pressure was accepted");

  auto delayed_first = base;
  replace_once(delayed_first, "- after_ms: 0", "- after_ms: 1");
  const auto delayed_first_path =
      write_temp_vehicle_config("timeout-delayed-first-stage", delayed_first);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(delayed_first_path)); },
      "timeout profile whose first stage did not start at zero was accepted");

  auto duplicate_time = base;
  replace_once(duplicate_time, "- after_ms: 500", "- after_ms: 0");
  const auto duplicate_time_path =
      write_temp_vehicle_config("timeout-duplicate-stage", duplicate_time);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(duplicate_time_path)); },
      "timeout profile with duplicate stage times was accepted");

  auto unordered = base;
  replace_once(
      unordered,
      "      - after_ms: 0\n        brake: 0.3\n"
      "      - after_ms: 500\n        brake: 0.6",
      "      - after_ms: 500\n        brake: 0.6\n"
      "      - after_ms: 0\n        brake: 0.3");
  const auto unordered_path =
      write_temp_vehicle_config("timeout-unordered-stage", unordered);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(unordered_path)); },
      "timeout profile declared out of order was silently reordered");

  std::error_code error;
  std::filesystem::remove(missing_full_path, error);
  std::filesystem::remove(decreasing_path, error);
  std::filesystem::remove(delayed_first_path, error);
  std::filesystem::remove(duplicate_time_path, error);
  std::filesystem::remove(unordered_path, error);
}

void test_vehicle_config_validates_local_speed_pid_safety_fields() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  const std::vector<std::pair<std::string, std::string>> invalid_fields{
      {"speed_feedback_timeout_ms", "19"},
      {"speed_feedback_timeout_ms", "501"},
      {"motor_torque_rise_rate_nm_per_s", "-0.1"},
      {"motor_torque_rise_rate_nm_per_s", "32000.1"},
      {"motor_torque_rise_rate_nm_per_s", ".nan"},
      {"speed_pid_kp", "0"},
      {"speed_pid_kp", ".nan"},
      {"speed_pid_ki", "-0.1"},
      {"speed_pid_kd", "100.1"},
      {"speed_pid_derivative_filter_tau_ms", "2000.1"},
      {"speed_pid_max_dt_ms", "19"},
      {"speed_pid_max_dt_ms", "201"},
      {"hard_overspeed_margin_kph", "0"},
      {"hard_overspeed_margin_kph", "36.1"},
  };
  std::error_code error;
  for (const auto& [name, value] : invalid_fields) {
    auto contents = base;
    replace_once(
        contents,
        "field_safety:\n",
        "field_safety:\n  " + name + ": " + value + "\n");
    const auto path = write_temp_vehicle_config("invalid-local-speed-pid", contents);
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_vehicle_config(path)); },
        "invalid local speed PID safety field was accepted: " + name);
    std::filesystem::remove(path, error);
  }
}

void test_vehicle_camera_recovery_config_defaults_explicit_values_and_boundaries() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  const auto default_config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  const auto default_camera = default_config.enabled_cameras().front();
  expect(default_camera.critical_for_control, "camera must default to control-critical");
  expect(default_camera.reopen_attempts == 3, "camera reopen attempt default changed");
  expect(default_camera.reopen_backoff_ms == 500, "camera reopen backoff default changed");

  auto explicit_contents = "runtime:\n  control_enabled: false\n  media_enabled: true\n\n" + base;
  replace_once(
      explicit_contents,
      "  - id: front\n",
      "  - id: front\n"
      "    critical_for_control: false\n"
      "    reopen_attempts: 0\n"
      "    reopen_backoff_ms: 60000\n");
  const auto explicit_path = write_temp_vehicle_config("explicit-camera-recovery", explicit_contents);
  const auto explicit_config = mine_teleop::load_vehicle_config(explicit_path);
  const auto explicit_camera = explicit_config.enabled_cameras().front();
  expect(!explicit_camera.critical_for_control, "explicit noncritical camera setting was ignored");
  expect(explicit_camera.reopen_attempts == 0, "zero reopen attempts was not accepted");
  expect(explicit_camera.reopen_backoff_ms == 60000, "maximum reopen backoff was not accepted");
  std::error_code error;
  std::filesystem::remove(explicit_path, error);

  auto boundary_contents = base;
  replace_once(
      boundary_contents,
      "  - id: front\n",
      "  - id: front\n"
      "    reopen_attempts: 10\n"
      "    reopen_backoff_ms: 0\n");
  const auto boundary_path = write_temp_vehicle_config("camera-recovery-boundaries", boundary_contents);
  const auto boundary_camera = mine_teleop::load_vehicle_config(boundary_path).enabled_cameras().front();
  expect(boundary_camera.reopen_attempts == 10, "maximum reopen attempts was not accepted");
  expect(boundary_camera.reopen_backoff_ms == 0, "zero reopen backoff was not accepted");
  std::filesystem::remove(boundary_path, error);

  for (const int invalid_attempts : {-1, 11}) {
    auto contents = base;
    replace_once(
        contents,
        "  - id: front\n",
        "  - id: front\n    reopen_attempts: " + std::to_string(invalid_attempts) + "\n");
    const auto path = write_temp_vehicle_config("invalid-camera-reopen-attempts", contents);
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_vehicle_config(path)); },
        "out-of-range camera reopen attempts was accepted");
    std::filesystem::remove(path, error);
  }
  for (const int invalid_backoff_ms : {-1, 60001}) {
    auto contents = base;
    replace_once(
        contents,
        "  - id: front\n",
        "  - id: front\n    reopen_backoff_ms: " + std::to_string(invalid_backoff_ms) + "\n");
    const auto path = write_temp_vehicle_config("invalid-camera-reopen-backoff", contents);
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_vehicle_config(path)); },
        "out-of-range camera reopen backoff was accepted");
    std::filesystem::remove(path, error);
  }
}

void test_control_enabled_vehicle_requires_an_enabled_critical_camera() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  auto no_critical_contents = base;
  replace_once(
      no_critical_contents,
      "  - id: front\n",
      "  - id: front\n    critical_for_control: false\n");
  const auto no_critical_path = write_temp_vehicle_config("no-critical-camera", no_critical_contents);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(no_critical_path)); },
      "control-enabled vehicle accepted no enabled critical cameras");
  std::error_code error;
  std::filesystem::remove(no_critical_path, error);

  auto one_critical_contents = no_critical_contents;
  replace_once(
      one_critical_contents,
      "  - id: rear\n    enabled: false\n",
      "  - id: rear\n    enabled: true\n    critical_for_control: true\n");
  const auto one_critical_path = write_temp_vehicle_config("one-critical-camera", one_critical_contents);
  const auto enabled_cameras = mine_teleop::load_vehicle_config(one_critical_path).enabled_cameras();
  expect(enabled_cameras.size() == 2, "expected both configured cameras to be enabled");
  expect(
      std::count_if(enabled_cameras.begin(), enabled_cameras.end(), [](const auto& camera) {
        return camera.critical_for_control;
      }) == 1,
      "control-enabled vehicle did not preserve exactly one critical camera");
  std::filesystem::remove(one_critical_path, error);
}

void test_camera_failure_decision_is_bounded_and_fail_closed() {
  mine_teleop::CameraConfig camera;
  camera.critical_for_control = true;
  camera.reopen_attempts = 2;

  const auto first_failure = mine_teleop::camera_failure_decision(camera, 1, true);
  expect(first_failure.inhibit_control, "critical camera failure did not inhibit control");
  expect(
      first_failure.lane_action == mine_teleop::CameraFailureAction::ReopenLane,
      "retryable critical camera failure did not reopen within its budget");

  const auto exhausted = mine_teleop::camera_failure_decision(camera, 3, true);
  expect(exhausted.inhibit_control, "exhausted critical camera failure released control");
  expect(
      exhausted.lane_action == mine_teleop::CameraFailureAction::DisableLane,
      "critical camera reopened after exhausting its retry budget");

  camera.critical_for_control = false;
  const auto noncritical = mine_teleop::camera_failure_decision(camera, 2, true);
  expect(!noncritical.inhibit_control, "noncritical camera failure inhibited control");
  expect(
      noncritical.lane_action == mine_teleop::CameraFailureAction::ReopenLane,
      "retryable noncritical camera did not use its reopen budget");
  const auto nonretryable = mine_teleop::camera_failure_decision(camera, 1, false);
  expect(
      nonretryable.lane_action == mine_teleop::CameraFailureAction::DisableLane,
      "nonretryable camera failure attempted to reopen");
  expect_throws(
      [&] { static_cast<void>(mine_teleop::camera_failure_decision(camera, 0, true)); },
      "nonpositive camera failure count was accepted");
}

void test_camera_source_classification_is_canonical() {
  using mine_teleop::CameraSourceKind;
  const std::vector<std::pair<std::string_view, CameraSourceKind>> cases{
      {"testsrc", CameraSourceKind::TestSource},
      {"mvs", CameraSourceKind::Mvs},
      {"mvs:index=1", CameraSourceKind::Mvs},
      {"hikrobot", CameraSourceKind::Mvs},
      {"hikrobot:serial=DA123", CameraSourceKind::Mvs},
      {"aravis", CameraSourceKind::Aravis},
      {"aravis:model=ace", CameraSourceKind::Aravis},
      {"basler", CameraSourceKind::Aravis},
      {"basler:serial=25192546", CameraSourceKind::Aravis},
      {"pylon", CameraSourceKind::Aravis},
      {"pylon:serial=legacy", CameraSourceKind::Aravis},
      {"/dev/video0", CameraSourceKind::V4l2},
      {"/dev/v4l/by-path/example-video-index0", CameraSourceKind::V4l2},
  };

  mine_teleop::CameraConfig camera;
  expect(camera.backend == "auto", "camera backend default changed from auto");
  for (const auto& [selector, expected] : cases) {
    expect(
        mine_teleop::classify_camera_source(selector) == expected,
        "camera selector was assigned to the wrong source kind: " + std::string(selector));
    camera.device = selector;
    expect(
        mine_teleop::classify_camera_source(camera) == expected,
        "auto backend changed legacy camera selector classification: " + std::string(selector));
  }
  camera.backend = "ccg2";
  camera.device = "/dev/video0";
  expect(
      mine_teleop::classify_camera_source(camera) == CameraSourceKind::Ccg2,
      "explicit CCG2 backend did not override the ordinary V4L2 selector");
  expect(
      mine_teleop::camera_source_kind_name(CameraSourceKind::TestSource) == "testsrc" &&
          mine_teleop::camera_source_kind_name(CameraSourceKind::Mvs) == "vendor_sdk" &&
          mine_teleop::camera_source_kind_name(CameraSourceKind::Aravis) == "aravis" &&
          mine_teleop::camera_source_kind_name(CameraSourceKind::V4l2) == "v4l2" &&
          mine_teleop::camera_source_kind_name(CameraSourceKind::Ccg2) == "ccg2",
      "camera source diagnostic names changed");
}

void test_camera_input_spec_is_explicit_for_ccg2_and_legacy_safe() {
  mine_teleop::MediaProfile realtime;
  realtime.width = 1280;
  realtime.height = 720;
  realtime.fps = 25;

  mine_teleop::CameraConfig legacy;
  legacy.id = "legacy-v4l2";
  legacy.device = "/dev/video2";
  legacy.capture_width = 1920;
  legacy.capture_height = 1080;
  legacy.capture_fps = 30;
  const auto legacy_input = mine_teleop::camera_input_spec(legacy, realtime);
  expect(
      legacy_input.codec == "mjpeg" && legacy_input.width == realtime.width &&
          legacy_input.height == realtime.height && legacy_input.fps == realtime.fps,
      "legacy camera input stopped following the realtime MJPEG profile");

  auto ccg2 = legacy;
  ccg2.id = "ccg2-channel";
  ccg2.backend = "ccg2";
  ccg2.device = "/dev/video0";
  const auto ccg2_input = mine_teleop::camera_input_spec(ccg2, realtime);
  expect(
      ccg2_input.codec == "uyvy" && ccg2_input.width == ccg2.capture_width &&
          ccg2_input.height == ccg2.capture_height && ccg2_input.fps == ccg2.capture_fps,
      "CCG2 input did not use the configured raw capture dimensions and FPS");
}

void test_camera_backend_config_rejects_unknown_and_invalid_ccg2_modes() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");
  auto unknown_backend = base;
  replace_once(
      unknown_backend,
      "  - id: front\n",
      "  - id: front\n    backend: typo\n");
  const auto unknown_path = write_temp_vehicle_config("unknown-camera-backend", unknown_backend);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(unknown_path)); },
      "unknown camera backend was accepted");
  std::error_code error;
  std::filesystem::remove(unknown_path, error);

  auto odd_width = base;
  replace_once(
      odd_width,
      "  - id: front\n",
      "  - id: front\n    backend: ccg2\n");
  replace_once(odd_width, "    capture_width: 1920\n", "    capture_width: 1919\n");
  const auto odd_width_path = write_temp_vehicle_config("odd-ccg2-width", odd_width);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(odd_width_path)); },
      "odd CCG2 capture width was accepted");
  std::filesystem::remove(odd_width_path, error);
}

void test_ccg2_uyvy_row_packing_handles_stride_and_rejects_invalid_frames() {
  const std::string tight = "ABCDEFGHIJKLMNOP";
  expect(
      mine_teleop::pack_uyvy_rows(tight, 4, 2, 8) == tight,
      "tightly packed UYVY frame changed during row packing");

  const std::string padded = "ABCDEFGHxyIJKLMNOPzz";
  expect(
      mine_teleop::pack_uyvy_rows(padded, 4, 2, 10) == tight,
      "UYVY row padding was copied into the visible frame");

  const std::string trailing = "ABCDEFGHtrailing-driver-bytes";
  expect(
      mine_teleop::pack_uyvy_rows(trailing, 2, 2, 4) == "ABCDEFGH",
      "CCG2 trailing driver bytes changed the negotiated visible image");

  expect_throws(
      [&] { static_cast<void>(mine_teleop::pack_uyvy_rows("ABCDEF", 3, 1, 6)); },
      "odd-width UYVY frame was accepted");
  expect_throws(
      [&] { static_cast<void>(mine_teleop::pack_uyvy_rows(padded.substr(0, 17), 4, 2, 10)); },
      "short padded UYVY frame was accepted");
}

void test_camera_input_pipeline_keeps_legacy_jpeg_and_adds_raw_ccg2() {
  mine_teleop::MediaProfile output;
  output.width = 1280;
  output.height = 720;
  output.fps = 30;

  const mine_teleop::CameraInputSpec legacy{"mjpeg", 1280, 720, 30};
  const auto legacy_pipeline =
      mine_teleop::build_camera_input_pipeline("source_legacy", legacy, output);
  expect(
      legacy_pipeline.find("caps=image/jpeg") != std::string::npos &&
          legacy_pipeline.find("jpegdec") != std::string::npos &&
          legacy_pipeline.find("max-bytes=524288") != std::string::npos,
      "legacy camera pipeline no longer preserves bounded MJPEG decode input");

  const mine_teleop::CameraInputSpec ccg2{"uyvy", 1920, 1080, 30};
  const auto ccg2_pipeline =
      mine_teleop::build_camera_input_pipeline("source_ccg2", ccg2, output);
  expect(
      ccg2_pipeline.find("video/x-raw,format=UYVY") != std::string::npos &&
          ccg2_pipeline.find("width=1920,height=1080,framerate=30/1") != std::string::npos,
      "CCG2 pipeline does not advertise its negotiated raw UYVY input");
  expect(
      ccg2_pipeline.find("jpegdec") == std::string::npos &&
          ccg2_pipeline.find("max-bytes=0") != std::string::npos,
      "CCG2 pipeline retained the legacy JPEG decoder or byte cap");
}

void test_camera_input_pipeline_resamples_only_mismatched_ccg2_fps() {
  mine_teleop::MediaProfile output;
  output.width = 1280;
  output.height = 720;
  output.fps = 25;

  const mine_teleop::CameraInputSpec ccg2_30_fps{"uyvy", 1920, 1080, 30};
  const auto converted =
      mine_teleop::build_camera_input_pipeline("source_ccg2_30", ccg2_30_fps, output);
  expect(
      converted.find("videorate") != std::string::npos,
      "CCG2 30 FPS input did not add videorate for a 25 FPS output");

  const mine_teleop::CameraInputSpec ccg2_25_fps{"uyvy", 1920, 1080, 25};
  const auto unchanged =
      mine_teleop::build_camera_input_pipeline("source_ccg2_25", ccg2_25_fps, output);
  expect(
      unchanged.find("videorate") == std::string::npos,
      "CCG2 input added videorate even though input and output FPS match");

  const mine_teleop::CameraInputSpec legacy_30_fps{"mjpeg", 1280, 720, 30};
  const auto legacy =
      mine_teleop::build_camera_input_pipeline("source_legacy_30", legacy_30_fps, output);
  expect(
      legacy.find("videorate") == std::string::npos,
      "legacy MJPEG pipeline behavior changed when input and output FPS differ");
}

void test_ccg2_camera_input_pipeline_is_gstreamer_parseable() {
  mine_teleop::MediaProfile output;
  output.width = 1280;
  output.height = 720;
  output.fps = 25;
  const mine_teleop::CameraInputSpec input{"uyvy", 1920, 1080, 30};
  const auto description =
      mine_teleop::build_camera_input_pipeline("source_ccg2_parse", input, output) +
      "! fakesink sync=false";

  GError* init_error = nullptr;
  if (!gst_init_check(nullptr, nullptr, &init_error)) {
    const std::string message = init_error == nullptr ? "unknown error" : init_error->message;
    if (init_error != nullptr) g_error_free(init_error);
    throw TestFailure("GStreamer initialization failed: " + message);
  }

  GError* parse_error = nullptr;
  GstElement* pipeline = gst_parse_launch(description.c_str(), &parse_error);
  const std::string error = parse_error == nullptr ? "" : parse_error->message;
  const bool parsed = pipeline != nullptr && error.empty();
  if (parse_error != nullptr) g_error_free(parse_error);
  if (pipeline != nullptr) gst_object_unref(pipeline);
  expect(
      parsed,
      "GStreamer could not parse the CCG2 30-to-25 FPS input pipeline: " + error);
}

void test_v4l2_sequence_gap_handles_first_consecutive_missing_and_wrap() {
  expect(
      mine_teleop::v4l2_sequence_gap(std::nullopt, 42U) == 0,
      "first V4L2 frame was reported as a sequence gap");
  expect(
      mine_teleop::v4l2_sequence_gap(std::optional<std::uint32_t>{41U}, 42U) == 0,
      "consecutive V4L2 frames were reported as a sequence gap");
  expect(
      mine_teleop::v4l2_sequence_gap(std::optional<std::uint32_t>{41U}, 45U) == 3,
      "V4L2 sequence gap did not count all missing frames");
  expect(
      mine_teleop::v4l2_sequence_gap(
          std::optional<std::uint32_t>{std::numeric_limits<std::uint32_t>::max()},
          0U) == 0,
      "V4L2 sequence wrap was reported as a missing frame");
}

void test_camera_issue_classification_distinguishes_ccg2_fps_and_buffer_faults() {
  for (const std::string_view error : {
           "CCG2 driver returned invalid timeperframe for /dev/video0: numerator=0, denominator=30",
           "CCG2 driver returned unexpected timeperframe for /dev/video0: requested_fps=30, numerator=1, denominator=25",
       }) {
    const auto issue = mine_teleop::classify_camera_issue(error);
    expect(
        issue.code == "camera_ccg2_fps_mismatch",
        "CCG2 timeperframe validation did not use the dedicated FPS mismatch issue code");
    expect(!issue.retryable, "CCG2 timeperframe mismatch was marked retryable");
  }

  const auto buffer_error = mine_teleop::classify_camera_issue(
      "CCG2 V4L2 buffer flagged error for /dev/video0: sequence=42, gap_from_last_delivered=1");
  expect(
      buffer_error.code == "camera_ccg2_buffer_error",
      "CCG2 errored capture buffer did not use the dedicated issue code");
  expect(buffer_error.retryable, "CCG2 errored capture buffer was marked nonretryable");

  const auto generic_fps = mine_teleop::classify_camera_issue(
      "VIDIOC_S_PARM failed for /dev/video0: Invalid argument");
  expect(
      generic_fps.code == "camera_fps_rejected",
      "generic V4L2 FPS rejection changed issue code");
  expect(
      generic_fps.action.find("MJPEG") == std::string_view::npos,
      "generic V4L2 FPS recovery action incorrectly assumes MJPEG input");
}

void test_ccg2_example_config_defines_two_explicit_capture_lanes() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.ccg2-8m.yaml");
  expect(
      config.hardware.preferred_encoder == "vaapi",
      "CCG2 example config does not prefer the Intel VAAPI encoder");
  expect(
      config.hardware.fallback_encoder == "nvenc",
      "CCG2 example config does not retain NVENC as the fallback encoder");
  const auto cameras = config.enabled_cameras();
  expect(cameras.size() == 2, "CCG2 example config does not enable exactly two capture lanes");
  for (std::size_t index = 0; index < cameras.size(); ++index) {
    const auto& camera = cameras.at(index);
    expect(
        camera.id == "ccg2_channel_" + std::to_string(index),
        "CCG2 example camera ID no longer matches its channel index");
    expect(
        camera.backend == "ccg2" &&
            camera.device == "/dev/ccg2-channel-" + std::to_string(index),
        "CCG2 example camera does not explicitly bind the expected V4L2 node");
    expect(
        camera.capture_width == 1920 && camera.capture_height == 1080 &&
            camera.capture_fps == 30,
        "CCG2 example camera capture mode is not 1920x1080 at 30 FPS");
    expect(
        camera.realtime_profile == "realtime_720p30" &&
            camera.record_profile == "reuse_realtime",
        "CCG2 example camera profile bindings changed");
  }
}

void test_missing_v4l2_path_remains_retryable() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto camera = config.enabled_cameras().front();
  camera.device = (std::filesystem::path("/tmp") /
                   ("mine-teleop-missing-camera-" + mine_teleop::random_token(8)))
                      .string();
  auto capture_profile = config.realtime_profile(camera.realtime_profile);
  capture_profile.codec = "mjpeg";
  capture_profile.encoder = "native";
  mine_teleop::CameraFrameSource source(camera, capture_profile);

  try {
    static_cast<void>(source.next(1));
  } catch (const std::exception& error) {
    const auto issue = mine_teleop::classify_camera_issue(error.what());
    expect(issue.code == "camera_open_failed", "missing V4L2 node used the wrong issue code");
    expect(issue.retryable, "missing V4L2 node was marked nonretryable");
    camera.reopen_attempts = 2;
    const auto decision = mine_teleop::camera_failure_decision(camera, 1, issue.retryable);
    expect(
        decision.lane_action == mine_teleop::CameraFailureAction::ReopenLane,
        "missing V4L2 node bypassed the bounded lane reopen policy");
    return;
  }
  throw TestFailure("missing V4L2 node unexpectedly produced a frame");
}

void test_media_signaling_sequence_is_monotonic_within_scope_and_resets_between_scopes() {
  mine_teleop::MediaSignalingSequence sequence;
  expect(sequence.current() == 0, "new media signaling sequence was not zero");
  expect(sequence.next(7, "session-a") == 1, "first media sequence did not start at one");
  expect(sequence.next(7, "session-a") == 2, "media sequence did not increase within one scope");
  expect(sequence.current() == 2, "current media sequence did not track the last allocation");
  expect(sequence.next(7, "session-b") == 1, "new session did not reset media sequence");
  expect(sequence.next(8, "session-b") == 1, "new connection generation did not reset media sequence");
  expect(sequence.next(8, "session-b") == 2, "new scope did not remain monotonic after reset");
  expect_throws(
      [&] { static_cast<void>(sequence.next(0, "session-b")); },
      "zero connection generation was accepted as a media sequence scope");
  expect_throws(
      [&] { static_cast<void>(sequence.next(8, "")); },
      "empty session id was accepted as a media sequence scope");
}

void test_critical_camera_control_latch_persists_until_a_new_session() {
  mine_teleop::CriticalCameraControlLatch latch;
  expect(!latch.enter_session("session-a"), "new session unexpectedly inherited camera inhibition");
  expect(!latch.inhibited_for("session-a"), "new session started inhibited");
  expect(latch.inhibit("session-a"), "first camera fault did not transition the session latch");
  expect(latch.inhibited_for("session-a"), "camera fault did not remain latched");
  expect(!latch.inhibit("session-a"), "repeated camera fault was not idempotent");
  expect(
      latch.enter_session("session-a"),
      "same-session runtime reconstruction cleared camera control inhibition");
  expect(!latch.enter_session("session-b"), "new session retained the previous session's inhibition");
  expect(!latch.inhibited_for("session-b"), "new session did not start uninhibited");
  expect_throws(
      [&] { static_cast<void>(latch.inhibit("session-a")); },
      "stale runtime was allowed to inhibit a different active session");
  expect(!latch.inhibited_for("session-b"), "stale-session fault changed the active session latch");
  expect_throws(
      [&] { static_cast<void>(latch.enter_session("")); },
      "empty session id was accepted by the critical-camera latch");
  expect_throws(
      [&] { static_cast<void>(latch.inhibited_for("session-a")); },
      "latch state was exposed for a non-active session");
}

void test_media_signaling_error_classification_supports_structured_and_legacy_conflicts() {
  using Kind = mine_teleop::MediaSignalingErrorKind;
  struct ConflictCase {
    std::string issue_code;
    std::string legacy_message;
    Kind expected;
  };
  const std::vector<ConflictCase> cases{
      {"session_not_active", "session is not active", Kind::SessionEnded},
      {"vehicle_offline", "vehicle is offline", Kind::ConnectionRefresh},
      {"vehicle_connection_generation_stale", "vehicle connection generation is stale", Kind::ConnectionStale},
      {"signaling_sequence_older", "signaling message sequence is older than the previous message", Kind::SequenceConflict},
      {"signaling_sequence_reused", "signaling message sequence was reused with different content", Kind::SequenceConflict},
  };
  for (const auto& value : cases) {
    expect(
        mine_teleop::classify_media_signaling_error(
            mine_teleop::HttpStatusError(409, "structured conflict", value.issue_code)) == value.expected,
        "structured signaling conflict was misclassified: " + value.issue_code);
    expect(
        mine_teleop::classify_media_signaling_error(
            mine_teleop::HttpStatusError(409, "legacy conflict: " + value.legacy_message)) == value.expected,
        "legacy signaling conflict was misclassified: " + value.legacy_message);
  }
  expect(
      mine_teleop::classify_media_signaling_error(mine_teleop::HttpStatusError(404, "missing")) ==
          Kind::SessionEnded,
      "404 signaling response was not treated as an ended session");
  expect(
      mine_teleop::classify_media_signaling_error(mine_teleop::HttpStatusError(503, "unavailable")) ==
          Kind::ServiceUnavailable,
      "5xx signaling response was not treated as service unavailable");
  expect(
      mine_teleop::classify_media_signaling_error(mine_teleop::HttpStatusError(409, "unknown conflict")) ==
          Kind::Fatal,
      "unknown 409 signaling conflict was not fail-closed");
  expect(
      mine_teleop::classify_media_signaling_error(mine_teleop::HttpStatusError(401, "unauthorized")) ==
          Kind::Fatal,
      "nonretryable signaling response was not fatal");
}

void test_vehicle_config_validates_chassis_control_speed_range() {
  const auto base = read_text("configs/vehicle-agent.dev.yaml");

  auto boundary_contents = base;
  replace_once(boundary_contents, "max_speed_kph: 40", "max_speed_kph: 72");
  const auto boundary_path = write_temp_vehicle_config("speed-boundary", boundary_contents);
  const auto boundary_config = mine_teleop::load_vehicle_config(boundary_path);
  expect_near(
      boundary_config.field_safety.max_speed_kph,
      72.0,
      1e-9,
      "20 m/s ChassisControl speed boundary was rejected");
  std::error_code error;
  std::filesystem::remove(boundary_path, error);

  auto excessive_contents = base;
  replace_once(excessive_contents, "max_speed_kph: 40", "max_speed_kph: 72.1");
  const auto excessive_path = write_temp_vehicle_config("speed-excessive", excessive_contents);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(excessive_path)); },
      "target speed above the ChassisControl 20 m/s limit was accepted");
  std::filesystem::remove(excessive_path, error);

  auto non_mock_boundary_contents = base;
  replace_once(
      non_mock_boundary_contents,
      "field_safety:\n",
      "field_safety:\n"
      "  max_throttle: 0.10\n"
      "  full_scale_motor_torque_nm: 41.25\n"
      "  motor_torque_rise_rate_nm_per_s: 0.0\n"
      "  speed_feedback_timeout_ms: 200\n"
      "  speed_pid_kp: 1.0\n"
      "  speed_pid_ki: 0.2\n"
      "  speed_pid_kd: 0.0\n"
      "  speed_pid_derivative_filter_tau_ms: 100.0\n"
      "  speed_pid_max_dt_ms: 100\n"
      "  hard_overspeed_margin_kph: 3.6\n"
      "  max_brake_pressure_bar: 100.0\n"
      "  max_steering_angle_deg: 5.0\n");
  replace_once(
      non_mock_boundary_contents,
      "vehicle_adapter:\n  type: mock",
      "vehicle_adapter:\n"
      "  type: dynamic_library\n"
      "  integration:\n"
      "    chassis_control:\n"
      "      can_interface: can0\n"
      "      bridge_library_path: /tmp/libmine_teleop_chassis_bridge.so");
  replace_once(
      non_mock_boundary_contents, "max_speed_kph: 40", "max_speed_kph: 1");
  const auto non_mock_boundary_path = write_temp_vehicle_config(
      "speed-minimum-boundary", non_mock_boundary_contents);
  const auto non_mock_boundary_config =
      mine_teleop::load_vehicle_config(non_mock_boundary_path);
  expect_near(
      non_mock_boundary_config.field_safety.max_speed_kph,
      1.0,
      1e-9,
      "one km/h local PID target-speed boundary was rejected");
  std::filesystem::remove(non_mock_boundary_path, error);

  auto missing_rise_rate_contents = non_mock_boundary_contents;
  replace_once(
      missing_rise_rate_contents,
      "  motor_torque_rise_rate_nm_per_s: 0.0\n",
      "");
  const auto missing_rise_rate_path = write_temp_vehicle_config(
      "missing-explicit-motor-torque-rise-rate",
      missing_rise_rate_contents);
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::load_vehicle_config(missing_rise_rate_path));
      },
      "non-mock adapter accepted an implicit motor torque rise rate");
  std::filesystem::remove(missing_rise_rate_path, error);

  auto missing_pid_contents = non_mock_boundary_contents;
  replace_once(missing_pid_contents, "  speed_pid_kd: 0.0\n", "");
  const auto missing_pid_path = write_temp_vehicle_config(
      "missing-explicit-speed-pid", missing_pid_contents);
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_vehicle_config(missing_pid_path)); },
      "non-mock adapter accepted an implicit speed PID gain");
  std::filesystem::remove(missing_pid_path, error);

  auto sub_resolution_contents = non_mock_boundary_contents;
  replace_once(
      sub_resolution_contents, "max_speed_kph: 1", "max_speed_kph: 0.5");
  const auto sub_resolution_path = write_temp_vehicle_config(
      "speed-below-vcu-resolution", sub_resolution_contents);
  expect_near(
      mine_teleop::load_vehicle_config(sub_resolution_path).field_safety.max_speed_kph,
      0.5,
      1e-9,
      "local PID rejected a target below the unused VCU speed-request resolution");
  std::filesystem::remove(sub_resolution_path, error);
}

void test_dynamic_adapter_target_speed_uses_configured_ceiling() {
  auto value = command();
  value.throttle = 0.01;
  constexpr double max_speed_mps = 7.5;

  for (const std::string gear : {"D", "R"}) {
    value.gear = gear;
    expect_near(
        mine_teleop::dynamic_adapter_target_speed_mps(value, max_speed_mps),
        0.01 * max_speed_mps,
        1e-9,
        "analog throttle did not select a proportional local PID target speed");
  }

  value.gear = "D";
  value.throttle = 1.0;
  expect_near(
      mine_teleop::dynamic_adapter_target_speed_mps(value, max_speed_mps),
      max_speed_mps,
      1e-9,
      "full analog throttle did not select the configured maximum target speed");
  value.throttle = 0.0;
  expect_near(
      mine_teleop::dynamic_adapter_target_speed_mps(value, max_speed_mps),
      0.0,
      1e-9,
      "released traction retained a target speed");
  value.throttle = 0.5;
  value.brake = 0.01;
  expect_near(
      mine_teleop::dynamic_adapter_target_speed_mps(value, max_speed_mps),
      0.0,
      1e-9,
      "braking retained a target speed");

  value.brake = 0.0;
  for (const std::string gear : {"N", "P"}) {
    value.gear = gear;
    expect_near(
        mine_teleop::dynamic_adapter_target_speed_mps(value, max_speed_mps),
        0.0,
        1e-9,
        "non-driving gear exposed a target speed");
  }
}

void test_dynamic_adapter_brake_overrides_throttle() {
  auto value = command();
  value.throttle = 0.8;
  value.brake = 0.3;
  expect_near(
      mine_teleop::dynamic_adapter_target_acceleration(value),
      -0.3,
      1e-9,
      "simultaneous throttle and brake produced traction");
  expect_near(
      mine_teleop::dynamic_adapter_target_acceleration(value, 0.5, 30.0, 100.0),
      -0.09,
      1e-9,
      "physical brake request was not normalized against the immutable vehicle ceiling");

  value.brake = 0.0;
  expect_near(
      mine_teleop::dynamic_adapter_target_acceleration(value),
      1.0,
      1e-9,
      "released brake did not restore the configured traction ceiling");
  expect_near(
      mine_teleop::dynamic_adapter_target_acceleration(value, 0.25),
      0.25,
      1e-9,
      "session torque ceiling was not encoded into the bridge intent");
}

void test_bench_config_drives_unified_vehicle_runtime() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.bench.yaml");
  expect(config.runtime.control_enabled, "bench runtime control is disabled");
  expect(config.runtime.media_enabled, "bench runtime media is disabled");
  expect(config.runtime.control_log_commands, "bench runtime control logging is disabled");
  expect(config.runtime.teleop_poll_interval_ms == 50, "bench teleop poll interval changed");
  expect(config.runtime.media_frame_timeout_ms == 3000, "bench media frame timeout changed");
  expect(config.recording.enabled, "bench recording is disabled");
  expect(
      config.cloud.device_token_file == std::filesystem::path("configs/device-token"),
      "relative device token file was not resolved from the config directory");
}

void test_field_config_pins_tls_route_without_system_dns() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.three-machine.field.yaml");
  expect(
      config.cloud.signaling_url == "wss://teleop-field.internal:6000/signaling",
      "field vehicle signaling URL does not use the private TLS name");
  expect(
      config.cloud.resolve_entries ==
          std::vector<std::string>{"teleop-field.internal:6000:60.205.213.254"},
      "field vehicle resolver override changed");
  expect(
      config.cloud.ca_bundle == std::filesystem::path("configs/mine-teleop-field-root.crt"),
      "field vehicle CA path was not resolved relative to its config");
  expect(std::filesystem::is_regular_file(config.cloud.ca_bundle), "field vehicle CA bundle is missing");
  expect(
      config.runtime.teleop_poll_interval_ms == 500,
      "field vehicle session discovery interval is not rate-limit safe");
  expect(config.cloud.ice_transport_policy == "all", "field vehicle ICE policy is not the safe default");
  expect(mine_teleop::ice_transport_policy_is_valid("all"), "all ICE policy was rejected");
  expect(mine_teleop::ice_transport_policy_is_valid("relay"), "relay ICE policy was rejected");
  expect(!mine_teleop::ice_transport_policy_is_valid("direct"), "unknown ICE policy was accepted");
  expect(
      config.field_safety.max_time_sync_uncertainty_ms == 25,
      "field vehicle time synchronization limit is not 25ms");
  expect_near(
      config.field_safety.max_throttle,
      0.10,
      1e-9,
      "field vehicle throttle hard limit changed");
  expect_near(
      config.field_safety.full_scale_motor_torque_nm,
      300.0,
      1e-9,
      "field vehicle full-scale motor torque changed");
  expect_near(
      config.field_safety.motor_torque_rise_rate_nm_per_s,
      0.0,
      1e-9,
      "field vehicle motor torque rise shaping is not explicitly disabled");
  expect(
      config.field_safety.speed_feedback_timeout_ms == 200 &&
          config.field_safety.speed_pid_max_dt_ms == 100,
      "field vehicle local speed PID timing changed");
  expect_near(
      config.field_safety.speed_pid_kp,
      1.0,
      1e-9,
      "field vehicle speed PID Kp changed");
  expect_near(
      config.field_safety.speed_pid_ki,
      0.2,
      1e-9,
      "field vehicle speed PID Ki changed");
  expect_near(
      config.field_safety.hard_overspeed_margin_kph,
      3.6,
      1e-9,
      "field vehicle hard overspeed margin changed");
  expect_near(
      config.redacted_summary().at("full_scale_motor_torque_nm").get<double>(),
      300.0,
      1e-9,
      "effective vehicle config omitted full-scale motor torque");
  expect_near(
      config.redacted_summary()
          .at("motor_torque_rise_rate_nm_per_s")
          .get<double>(),
      0.0,
      1e-9,
      "effective vehicle config omitted motor torque rise shaping");
  expect_near(
      config.field_safety.max_brake_pressure_bar,
      100.0,
      1e-9,
      "field vehicle ordinary-brake pressure hard limit changed");
  expect_near(
      config.redacted_summary().at("max_brake_pressure_bar").get<double>(),
      100.0,
      1e-9,
      "effective vehicle config omitted ordinary-brake pressure hard limit");
  expect_near(
      config.field_safety.max_steering_angle_deg,
      5.0,
      1e-9,
      "field vehicle steering hard limit changed");
  expect(
      config.field_safety.require_can_feedback_before_control,
      "field vehicle CAN feedback gate is disabled");
  expect(config.hardware.can_interface == "can1", "field vehicle CAN interface is not can1");
  expect(config.hardware.can_bitrate == 500000, "field vehicle CAN bitrate is not 500 kbit/s");
  expect(
      config.hardware.can_tx_queue_length == 100,
      "field vehicle CAN tx queue length is not 100");
}

void test_control_command_json_round_trip_and_validation() {
  const auto original = command(7, 1234);
  const auto parsed = ControlCommand::from_json(original.to_json());
  expect(parsed.seq == 7, "sequence did not round trip");
  expect_near(parsed.steering, 0.25, 1e-9, "steering did not round trip");
  auto invalid = original;
  invalid.throttle = 1.5;
  expect_throws([&] { invalid.validate(); }, "invalid throttle was accepted");
}

void test_session_control_profile_json_round_trip_and_physical_units() {
  const auto request = session_profile_request(9, 1234, 12.0, 300.0, 100.0, 30.0, 80.0);
  const auto encoded = request.to_json();
  expect(
      encoded.at("type") == "session_control_profile",
      "session profile used a wire type inconsistent with the data channel");
  const auto parsed = mine_teleop::SessionControlProfileRequest::from_json(encoded);
  expect(parsed == request, "session profile request did not round trip");

  mine_teleop::SessionControlProfileResult result;
  result.vehicle_id = request.vehicle_id;
  result.driver_id = request.driver_id;
  result.session_id = request.session_id;
  result.seq = request.seq;
  result.sent_at_utc_ms = request.sent_at_utc_ms;
  result.accepted = true;
  result.applied_revision = request.seq;
  result.reason = "accepted";
  result.effective_profile = request.profile;
  const auto ack = result.to_json();
  expect(
      ack.at("last_request_seq") == request.seq &&
          ack.at("applied_revision") == request.seq &&
          ack.at("active").get<bool>() && ack.at("accepted").get<bool>() &&
          ack.at("effective_profile").is_object(),
      "profile ACK did not expose the canonical reducer shape");
  expect(
      mine_teleop::SessionControlProfileResult::from_json(ack) == result,
      "session profile ACK did not round trip");
  auto inconsistent_ack = ack;
  inconsistent_ack["active"] = false;
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::SessionControlProfileResult::from_json(inconsistent_ack));
      },
      "profile ACK accepted an active flag inconsistent with its effective profile");
  inconsistent_ack = ack;
  inconsistent_ack.erase("applied_revision");
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::SessionControlProfileResult::from_json(inconsistent_ack));
      },
      "profile ACK accepted a missing applied_revision");
  inconsistent_ack = ack;
  inconsistent_ack["applied_revision"] = request.seq + 1;
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::SessionControlProfileResult::from_json(inconsistent_ack));
      },
      "profile ACK accepted an applied_revision different from its seq");
  inconsistent_ack = ack;
  inconsistent_ack["last_request_seq"] = request.seq + 1;
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::SessionControlProfileResult::from_json(inconsistent_ack));
      },
      "profile ACK accepted a last_request_seq inconsistent with its envelope");

  auto invalid = request;
  invalid.profile.service_brake_pressure_bar = 90.0;
  invalid.profile.hard_brake_pressure_bar = 80.0;
  expect_throws(
      [&] { invalid.validate(); },
      "service brake pressure above hard brake pressure was accepted");
  invalid = request;
  invalid.profile.max_motor_torque_nm = 640.1;
  expect_throws(
      [&] { invalid.validate(); },
      "ordinary motor torque above the 80-percent code cap was accepted");
  invalid = request;
  invalid.profile.max_brake_pressure_bar = 327.7;
  invalid.profile.hard_brake_pressure_bar = 327.7;
  expect_throws(
      [&] { invalid.validate(); },
      "ordinary brake pressure above the 80-percent code cap was accepted");
  invalid = request;
  invalid.profile.profile_version = 1;
  expect_throws(
      [&] { invalid.validate(); },
      "legacy session profile version was accepted");
  invalid = request;
  invalid.profile.speed_pid_kp = 0.0;
  expect_throws(
      [&] { invalid.validate(); },
      "zero proportional PID gain was accepted");
  invalid = request;
  invalid.profile.speed_pid_max_dt_ms = 201;
  expect_throws(
      [&] { invalid.validate(); },
      "out-of-range PID max dt was accepted");
  invalid = request;
  invalid.profile.motor_torque_rise_rate_nm_per_s = -1.0;
  expect_throws(
      [&] { invalid.validate(); },
      "negative motor torque rise rate was accepted");
  invalid = request;
  invalid.profile.motor_torque_rise_rate_nm_per_s = 32000.1;
  expect_throws(
      [&] { invalid.validate(); },
      "motor torque rise rate above the physical envelope was accepted");
  auto missing_rise_rate = encoded;
  missing_rise_rate.erase("motor_torque_rise_rate_nm_per_s");
  expect_throws(
      [&] {
        static_cast<void>(
            mine_teleop::SessionControlProfileRequest::from_json(missing_rise_rate));
      },
      "session profile without the rise rate field was accepted");
}

void test_shared_protocol_v1_vectors_and_session_states() {
  const auto valid_path = std::filesystem::path("protocol/v1/control-command.valid.json");
  const auto valid = read_json(valid_path);
  const auto parsed = ControlCommand::from_json(valid);
  expect(parsed.protocol_version == mine_teleop::kProtocolVersion, "protocol version changed");
  expect(parsed.driver_id == "driver-001", "driver id did not parse from shared vector");
  expect(parsed.sent_at_utc_ms == 1780000000000, "UTC timestamp did not parse from shared vector");
  expect(!parsed.to_json().contains("future_extension"), "unknown extension leaked into the command model");

  const std::vector<std::string> required_metadata{
      "protocol_version", "vehicle_id", "driver_id", "session_id", "seq", "sent_at_utc_ms"};
  for (const auto& field : required_metadata) {
    auto missing = valid;
    missing.erase(field);
    expect_throws(
        [&] { static_cast<void>(ControlCommand::from_json(missing)); },
        "missing required protocol metadata was accepted: " + field);
  }
  auto missing_token = valid;
  missing_token.erase("control_token");
  expect_throws(
      [&] { static_cast<void>(ControlCommand::from_json(missing_token)); },
      "control command without control_token was accepted");
  auto wrong_version = valid;
  wrong_version["protocol_version"] = mine_teleop::kProtocolVersion + 1;
  expect_throws(
      [&] { static_cast<void>(ControlCommand::from_json(wrong_version)); },
      "incompatible protocol version was accepted");
  expect_throws(
      [&] {
        static_cast<void>(ControlCommand::from_json(
            read_json("protocol/v1/control-command.invalid-missing-driver-id.json")));
      },
      "shared negative protocol vector was accepted");

  const std::vector<mine_teleop::SessionState> states{
      mine_teleop::SessionState::Offline,
      mine_teleop::SessionState::Online,
      mine_teleop::SessionState::Reserved,
      mine_teleop::SessionState::Connecting,
      mine_teleop::SessionState::Active,
      mine_teleop::SessionState::Degraded,
      mine_teleop::SessionState::Stopping,
      mine_teleop::SessionState::Closed,
  };
  for (const auto state : states) {
    expect(
        mine_teleop::session_state_from_string(mine_teleop::to_string(state)) == state,
        "session state did not round trip");
  }
  expect_throws(
      [] { static_cast<void>(mine_teleop::session_state_from_string("SESSION_ACTIVE")); },
      "legacy session state was silently accepted");
}

void test_control_receiver_enforces_token_sequence_and_gap() {
  mine_teleop::ControlReceiver receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto first = command(1, 0);
  first.control_token = "wrong";
  expect(receiver.accept(first, 0).reason == "control_token_invalid", "wrong token was accepted");
  first.control_token = "token";
  first.driver_id = "driver-other";
  expect(receiver.accept(first, 0).reason == "wrong_driver", "wrong driver was accepted");
  first.driver_id = "driver-001";
  expect(receiver.accept(first, 0).accepted, "first command was rejected");
  expect(receiver.accept(first, 50).reason == "old_seq", "old sequence was accepted");
  auto late = command(2, 500);
  late.control_token = "token";
  expect(receiver.accept(late, 500).reason == "command_gap_exceeded", "large command gap was accepted");
  late.estop = true;
  expect(receiver.accept(late, 500).accepted, "estop should bypass command gap rejection");

  mine_teleop::ControlReceiver recovery_receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto recovery_first = command(1, 0);
  recovery_first.control_token = "token";
  expect(recovery_receiver.accept(recovery_first, 0).accepted, "recovery first command was rejected");
  auto recovery_gap = command(2, 500);
  recovery_gap.control_token = "token";
  expect(recovery_receiver.accept(recovery_gap, 500).reason == "command_gap_exceeded", "recovery gap was not detected");
  auto recovery_next = command(3, 550);
  recovery_next.control_token = "token";
  expect(recovery_receiver.accept(recovery_next, 550).accepted, "receiver did not recover after command gap");

  mine_teleop::ControlReceiver synchronized_receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto stale = command(1, 0);
  stale.control_token = "token";
  expect(synchronized_receiver.accept(stale, 201).reason == "command_age_exceeded", "stale command was accepted");
  stale.sent_at_utc_ms = 201;
  stale.estop = true;
  expect(synchronized_receiver.accept(stale, 500).accepted, "stale estop should remain acceptable");

  mine_teleop::ControlReceiver future_receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto future = command(1, 201);
  future.control_token = "token";
  expect(future_receiver.accept(future, 0).reason == "command_timestamp_in_future", "future command was accepted");
}

void test_mailbox_keeps_only_latest_command() {
  mine_teleop::LatestControlCommandMailbox mailbox;
  mailbox.publish(command(1));
  mailbox.publish(command(2));
  expect(mailbox.pending_count() == 1, "mailbox should contain one command");
  expect(mailbox.dropped_count() == 1, "mailbox should count overwritten command");
  expect(mailbox.pop_latest()->seq == 2, "mailbox did not preserve latest command");
}

void test_safety_timeout_profile_and_estop_latch() {
  expect_throws(
      [] {
        mine_teleop::SafetyStateMachine invalid(
            300, 800, {{0, 0.3}, {500, 0.6}});
      },
      "SafetyStateMachine accepted a timeout profile without final full brake");
  expect_throws(
      [] {
        mine_teleop::SafetyStateMachine invalid(
            300, 800, {{1, 0.3}, {500, 1.0}});
      },
      "SafetyStateMachine accepted a timeout profile not starting at zero");
  expect_throws(
      [] {
        mine_teleop::SafetyStateMachine invalid(
            300, 800, {{0, 0.3}, {500, 0.2}, {1500, 1.0}});
      },
      "SafetyStateMachine accepted a decreasing timeout brake profile");
  expect_throws(
      [] {
        mine_teleop::SafetyStateMachine invalid(
            300, 800, {{0, 0.3}, {0, 0.6}, {1500, 1.0}});
      },
      "SafetyStateMachine accepted duplicate timeout stage times");
  expect_throws(
      [] {
        mine_teleop::SafetyStateMachine invalid(
            300, 800, {{500, 0.6}, {0, 0.3}, {1500, 1.0}});
      },
      "SafetyStateMachine silently reordered timeout stages");
  mine_teleop::SafetyStateMachine safety(
      300,
      800,
      {{0, 0.3}, {500, 0.6}, {1500, 1.0}});
  safety.mark_ready(0);
  auto value = command(1, 0);
  safety.on_valid_command(value, 0);
  safety.tick(300);
  expect(safety.state() == mine_teleop::SafetyState::Degraded, "degraded state not entered");
  safety.tick(800);
  expect(safety.state() == mine_teleop::SafetyState::TimeoutBrake, "timeout state not entered");
  expect_near(safety.current_output(800).brake, 0.3, 1e-9, "initial timeout brake mismatch");
  expect_near(safety.current_output(1300).brake, 0.6, 1e-9, "second timeout brake mismatch");
  expect_near(safety.current_output(2300).brake, 1.0, 1e-9, "maximum timeout brake mismatch");
  expect(
      !safety.current_output(1300).full_emergency_brake &&
          safety.current_output(2300).full_emergency_brake,
      "timeout stages did not distinguish ordinary pressure from final full-DBC braking");

  value.seq = 2;
  value.estop = true;
  safety.on_valid_command(value, 2400);
  expect(safety.state() == mine_teleop::SafetyState::Estop, "estop did not latch");
  value.seq = 3;
  value.estop = false;
  safety.on_valid_command(value, 2450);
  expect(safety.state() == mine_teleop::SafetyState::Estop, "drive command cleared estop latch");
  expect(!safety.reset_estop(false, "operator", 2500), "estop reset without local confirmation");
  expect(safety.reset_estop(true, "operator", 2500), "confirmed estop reset failed");
}

void test_session_control_profile_ack_sequence_limits_and_clear() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  expect_near(
      adapter_view->session_motor_torque_limit_nm(),
      0.0,
      1e-9,
      "service start exposed traction before profile ACK");
  expect(
      service.receive_command(command(1, 0), 0).reason ==
          "session_control_profile_required",
      "ordinary control was accepted before a session profile ACK");

  const auto first_request = session_profile_request(1, 0, 20.0, 100.0, 100.0, 30.0, 80.0);
  const auto first = service.receive_session_profile(first_request, 0);
  expect(
      first.accepted && !first.idempotent && first.applied_revision == 1,
      "first session profile was rejected or ACKed with the wrong revision");
  const auto first_status = service.session_control_profile();
  expect(
      first_status.at("last_request_seq") == 1 &&
          first_status.at("applied_revision") == 1 &&
          first_status.at("active").get<bool>() &&
          first_status.at("accepted").get<bool>() &&
          !first_status.at("idempotent").get<bool>() &&
          first_status.at("reason") == "accepted" &&
          first_status.at("effective_profile") == first.effective_profile->to_json(),
      "telemetry profile status did not match the canonical ACK shape");
  expect_near(
      adapter_view->session_motor_torque_limit_nm(),
      100.0,
      1e-9,
      "ACK preceded the adapter torque-limit update");
  expect_near(
      adapter_view->session_brake_pressure_limit_bar(),
      100.0,
      1e-9,
      "ACK preceded the adapter brake-pressure update");

  const auto delayed_retry = service.receive_session_profile(first_request, 1000);
  expect(
      delayed_retry.accepted && delayed_retry.idempotent,
      "lost ACK retry was rejected after the original timestamp aged out");
  auto conflict = first_request;
  conflict.sent_at_utc_ms = 1000;
  conflict.profile.max_motor_torque_nm = 90.0;
  const auto conflict_result = service.receive_session_profile(conflict, 1000);
  expect(
      !conflict_result.accepted &&
          conflict_result.reason == "profile_seq_conflict",
      "same profile sequence with a different payload was not rejected");

  const auto active_command = service.receive_command(command(2, 100), 100);
  expect(
      active_command.accepted,
      "profile-authorized command was rejected: " + active_command.reason);
  expect_near(
      active_command.command->throttle,
      0.5,
      1e-9,
      "session target speed did not cap the command target");

  const auto lower = service.receive_session_profile(
      session_profile_request(2, 1020, 10.0, 50.0, 100.0, 30.0, 80.0),
      1020);
  expect(lower.accepted, "live target/torque decrease was rejected");
  expect(
      adapter_view->status().applied_command_count == 2,
      "target/torque decrease was ACKed before reapplying the lower live ceiling");
  expect_near(
      adapter_view->session_motor_torque_limit_nm(),
      50.0,
      1e-9,
      "lower torque ACK did not reflect the adapter state");
  expect(
      service.receive_session_profile(first_request, 1030).reason == "old_seq",
      "older profile sequence was not rejected");

  const auto raised = service.receive_session_profile(
      session_profile_request(3, 1040, 12.0, 60.0, 100.0, 30.0, 80.0),
      1040);
  expect(raised.accepted, "mock bench profile increase bypass was rejected");
  expect(
      adapter_view->status().applied_command_count == 2,
      "profile increase replayed a stale command with newly raised authority");
  auto estop = command(3, 1050);
  estop.estop = true;
  expect(service.receive_command(estop, 1050).accepted, "ESTOP was rejected");
  const auto cleared_status = service.session_control_profile();
  expect(
      !cleared_status.at("active").get<bool>() &&
          !cleared_status.at("accepted").get<bool>() &&
          cleared_status.at("reason") == "session_profile_cleared" &&
          cleared_status.at("effective_profile").is_null(),
      "ESTOP retained an active or accepted session profile");
  expect_near(
      adapter_view->session_motor_torque_limit_nm(),
      0.0,
      1e-9,
      "ESTOP did not withdraw session traction authority");
  const auto cleared_retry = service.receive_session_profile(
      session_profile_request(3, 1040, 12.0, 60.0, 100.0, 30.0, 80.0),
      2000);
  expect(
      !cleared_retry.accepted && cleared_retry.idempotent &&
          cleared_retry.reason == "session_profile_cleared",
      "same-sequence retry silently reactivated a cleared profile");
  service.close();
}

void test_session_control_profile_uses_independent_two_second_age_window() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);

  auto wrong_identity = session_profile_request(1, 0);
  wrong_identity.vehicle_id = "vehicle-002";
  expect(
      service.receive_session_profile(wrong_identity, 0).reason == "wrong_vehicle",
      "session profile for a different vehicle was accepted");
  wrong_identity = session_profile_request(1, 0);
  wrong_identity.driver_id = "driver-002";
  expect(
      service.receive_session_profile(wrong_identity, 0).reason == "wrong_driver",
      "session profile for a different driver was accepted");
  wrong_identity = session_profile_request(1, 0);
  wrong_identity.session_id = "session-002";
  expect(
      service.receive_session_profile(wrong_identity, 0).reason == "wrong_session",
      "session profile for a different session was accepted");

  const auto future = service.receive_session_profile(
      session_profile_request(
          1,
          mine_teleop::kSessionControlProfileMaxAgeMs + 1),
      0);
  expect(
      !future.accepted && future.reason == "profile_timestamp_in_future",
      "new profile beyond the independent future-skew window was accepted");

  const auto delayed_first = session_profile_request(1, 0);
  const auto accepted = service.receive_session_profile(delayed_first, 500);
  expect(
      accepted.accepted && !accepted.idempotent,
      "new profile older than the ordinary 200-ms control window was rejected");

  const auto too_old = service.receive_session_profile(
      session_profile_request(2, 0),
      mine_teleop::kSessionControlProfileMaxAgeMs + 1);
  expect(
      !too_old.accepted && too_old.reason == "profile_age_exceeded",
      "new profile older than the independent two-second window was accepted");

  const auto very_late_retry = service.receive_session_profile(
      delayed_first,
      mine_teleop::kSessionControlProfileMaxAgeMs * 3);
  expect(
      very_late_retry.accepted && very_late_retry.idempotent,
      "known same-sequence retry was rejected by the profile age window");
  service.close();
}

void test_real_adapter_profile_changes_require_parking_and_apply_before_ack() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
  auto* adapter_view = adapter.get();
  adapter_view->handshake.parking_ready = false;
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);

  const auto blocked = service.receive_session_profile(
      session_profile_request(1, 0, 10.0, 100.0, 100.0, 30.0, 80.0),
      0);
  expect(
      !blocked.accepted &&
          blocked.reason == "parking_ready_required_for_profile_increase",
      "first nonzero real-adapter profile bypassed parking_ready");
  expect_near(
      adapter_view->session_brake_pressure_limit_bar(),
      0.0,
      1e-9,
      "rejected profile partially changed adapter brake authority");

  adapter_view->handshake.parking_ready = true;
  const auto ready_blocked = service.receive_session_profile(
      session_profile_request(2, 10, 10.0, 100.0, 100.0, 30.0, 80.0),
      10);
  expect(
      !ready_blocked.accepted &&
          ready_blocked.reason ==
              "standby_or_disarmed_required_for_profile_change",
      "parked Ready state bypassed the profile configuration state gate");
  adapter_view->handshake.state = "standby";
  const auto accepted = service.receive_session_profile(
      session_profile_request(3, 20, 10.0, 100.0, 100.0, 30.0, 80.0),
      20);
  expect(accepted.accepted, "parking-ready real-adapter profile was rejected");
  adapter_view->handshake.state = "ready";
  adapter_view->handshake.parking_ready = false;
  const auto decreased = service.receive_session_profile(
      session_profile_request(4, 30, 8.0, 80.0, 100.0, 30.0, 80.0),
      30);
  expect(decreased.accepted, "target/torque decrease required parking_ready");
  const auto brake_change = service.receive_session_profile(
      session_profile_request(5, 40, 8.0, 80.0, 90.0, 20.0, 70.0),
      40);
  expect(
      !brake_change.accepted &&
          brake_change.reason == "parking_ready_required_for_profile_increase",
      "brake pressure change bypassed parking_ready");
  const auto target_raise = service.receive_session_profile(
      session_profile_request(6, 50, 9.0, 80.0, 100.0, 30.0, 80.0),
      50);
  expect(
      !target_raise.accepted &&
          target_raise.reason == "parking_ready_required_for_profile_increase",
      "target-speed increase bypassed parking_ready");

  adapter_view->handshake.parking_ready = true;
  adapter_view->handshake.state = "disarmed";
  adapter_view->control_limit_update_throws = true;
  const auto apply_failed = service.receive_session_profile(
      session_profile_request(7, 60, 8.0, 70.0, 90.0, 20.0, 70.0),
      60);
  expect(
      !apply_failed.accepted &&
          apply_failed.reason == "adapter_session_profile_apply_failed" &&
          !service.session_control_profile().at("active").get<bool>(),
      "profile was ACKed before adapter application completed");
  adapter_view->control_limit_update_throws = false;

  const auto restored = service.receive_session_profile(
      session_profile_request(8, 70, 8.0, 80.0, 100.0, 30.0, 80.0),
      70);
  expect(
      restored.accepted,
      "parking-ready baseline profile was not restored after apply failure");

  // A rise-rate-only change joins the PID parking gate: identical envelope
  // values with only motor_torque_rise_rate_nm_per_s changed still requires
  // parking, and is applied once parked.
  adapter_view->handshake.parking_ready = false;
  auto rise_rate_change = session_profile_request(9, 80, 8.0, 80.0, 100.0, 30.0, 80.0);
  rise_rate_change.profile.motor_torque_rise_rate_nm_per_s = 50.0;
  const auto rise_rate_blocked = service.receive_session_profile(rise_rate_change, 80);
  expect(
      !rise_rate_blocked.accepted &&
          rise_rate_blocked.reason == "parking_ready_required_for_profile_increase",
      "motor torque rise-rate change bypassed parking_ready");
  adapter_view->handshake.parking_ready = true;
  auto rise_rate_apply = session_profile_request(10, 90, 8.0, 80.0, 100.0, 30.0, 80.0);
  rise_rate_apply.profile.motor_torque_rise_rate_nm_per_s = 50.0;
  const auto rise_rate_accepted = service.receive_session_profile(rise_rate_apply, 90);
  expect(
      rise_rate_accepted.accepted,
      "parked rise-rate change was rejected");
  service.close();
}

void test_control_service_commits_only_successfully_applied_commands() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  const mine_teleop::VehicleAdapterControlRejected unknown_rejection(
      "untrusted adapter detail / secret sentinel",
      -3);
  expect(
      unknown_rejection.issue_code() == "vcu_control_apply_rejected" &&
          std::string(unknown_rejection.what()).find("secret sentinel") ==
              std::string::npos,
      "adapter rejection exposed an issue outside the stable allowlist");
  {
    auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 10000);
    service.start(0);
    activate_adapter_owned_session_profile(service, *adapter_view);

    adapter_view->structured_rejection_issue_code =
        "vcu_drive_gear_change_moving_or_stale";
    const auto rejected = service.receive_command(command(1, 0), 0);
    expect(
        !rejected.accepted && !rejected.command &&
            rejected.reason == "adapter_control_rejected" &&
            rejected.issue_code == "vcu_drive_gear_change_moving_or_stale" &&
            service.safety_state() == mine_teleop::SafetyState::Standby,
        "structured adapter rejection was not returned without committing safety state");
    adapter_view->structured_rejection_issue_code.reset();
    expect(
        service.receive_command(command(2, 1), 1).accepted,
        "fresh command did not recover after a structured adapter rejection");
    service.close();
  }

  {
    auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 10000);
    service.start(0);
    activate_adapter_owned_session_profile(service, *adapter_view);

    expect(
        service.receive_command(command(1, 0), 0).accepted,
        "initial D command was rejected");
    adapter_view->rejected_control_gear = "R";
    auto rejected_reverse = command(2, 100);
    rejected_reverse.gear = "R";
    expect_throws(
        [&] { static_cast<void>(service.receive_command(rejected_reverse, 100)); },
        "adapter control rejection did not propagate");

    adapter_view->rejected_control_gear.reset();
    const auto replay = service.receive_command(rejected_reverse, 110);
    expect(
        !replay.accepted && replay.reason == "old_seq",
        "failed adapter application did not consume its command sequence");

    service.tick(300);
    expect(
        service.safety_state() == mine_teleop::SafetyState::Degraded,
        "failed adapter application refreshed the outer safety watchdog");
    expect(
        adapter_view->last_safe_output.gear == "D",
        "failed reverse application replaced the last successfully applied gear");
    service.close();
  }

  {
    auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 10000);
    service.start(0);
    activate_adapter_owned_session_profile(service, *adapter_view);
    expect(
        service.receive_command(command(1, 0), 0).accepted,
        "control command before overdue ESTOP was rejected");
    service.tick(300);

    auto estop = command(2, 810);
    estop.estop = true;
    adapter_view->safe_stop_throws = true;
    expect_throws(
        [&] { static_cast<void>(service.receive_command(estop, 810)); },
        "overdue adapter ESTOP failure did not propagate");
    expect(
        service.safety_state() == mine_teleop::SafetyState::Estop,
        "overdue adapter ESTOP failure rolled back the outer ESTOP latch");
    expect(
        !service.session_control_profile().at("active").get<bool>() &&
            adapter_view->session_motor_torque_limit_nm() == 0.0,
        "overdue adapter ESTOP failure retained session traction authority");

    adapter_view->safe_stop_throws = false;
    const auto replay = service.receive_command(estop, 811);
    expect(
        !replay.accepted && replay.reason == "old_seq" &&
            service.safety_state() == mine_teleop::SafetyState::Estop,
        "failed ESTOP application did not remain latched with its sequence consumed");
    service.close();
  }
}

void test_control_service_reports_safe_stop_output_after_timeout() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  activate_session_profile(service);
  expect(service.receive_command(command(1, 0), 0).accepted, "control command was rejected");
  service.tick(800);
  expect(service.safety_state() == mine_teleop::SafetyState::TimeoutBrake, "service did not enter timeout");
  expect(
      !service.session_control_profile().at("active").get<bool>(),
      "hard control timeout retained the session control profile");
  expect_near(
      adapter_view->session_motor_torque_limit_nm(),
      0.0,
      1e-9,
      "hard control timeout retained session traction authority");
  service.tick(1300);
  const auto telemetry = adapter_view->read_telemetry();
  expect_near(telemetry.throttle_feedback, 0.0, 1e-9, "timeout telemetry retained stale throttle");
  expect_near(telemetry.brake_feedback, 0.6, 1e-9, "timeout telemetry did not report safe brake");
  const auto gap_rearm = service.receive_command(command(2, 1310), 1310);
  expect(
      !gap_rearm.accepted && gap_rearm.reason == "command_gap_exceeded",
      "hard-timeout receiver did not re-arm on the first fresh command");
  const auto blocked = service.receive_command(command(3, 1320), 1320);
  expect(
      !blocked.accepted && blocked.reason == "session_control_profile_required",
      "hard control timeout recovered without explicit profile re-authorization");
  service.close();
}

void test_control_service_recovers_from_degraded_command_gap_without_profile_reapply() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  activate_session_profile(service);
  expect(service.receive_command(command(1, 0), 0).accepted, "control command was rejected");

  service.tick(300);
  expect(
      service.safety_state() == mine_teleop::SafetyState::Degraded,
      "service did not enter the recoverable degraded state");
  expect(
      service.session_control_profile().at("active").get<bool>(),
      "recoverable command jitter revoked the session control profile");
  expect_near(
      adapter_view->session_motor_torque_limit_nm(),
      100.0,
      1e-9,
      "recoverable command jitter withdrew the acknowledged torque ceiling");
  const auto degraded_telemetry = adapter_view->read_telemetry();
  expect_near(
      degraded_telemetry.throttle_feedback,
      0.0,
      1e-9,
      "degraded state retained stale traction while preserving the profile");

  const auto gap_rearm = service.receive_command(command(2, 350), 350);
  expect(
      !gap_rearm.accepted && gap_rearm.reason == "command_gap_exceeded",
      "first fresh command after the gap did not re-arm receiver timing");
  const auto held_input = service.receive_command(command(3, 360), 360);
  expect(
      !held_input.accepted && held_input.reason == "degraded_neutral_required",
      "degraded control resumed stale held input before an explicit neutral command");
  auto neutral = command(4, 370);
  neutral.steering = 0.0;
  neutral.throttle = 0.0;
  const auto recovered = service.receive_command(neutral, 370);
  expect(recovered.accepted, "fresh neutral command did not recover degraded control");
  expect(
      service.safety_state() == mine_teleop::SafetyState::ControlActive,
      "fresh neutral command did not restore active control");
  expect(
      service.receive_command(command(5, 380), 380).accepted,
      "fresh input remained blocked after explicit neutral recovery");
  expect(
      service.session_control_profile().at("active").get<bool>() &&
          adapter_view->session_motor_torque_limit_nm() == 100.0,
      "degraded recovery required an unsafe profile re-application");
  service.close();
}

void test_control_service_receive_path_cannot_bypass_hard_timeout() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  activate_session_profile(service);
  expect(service.receive_command(command(1, 0), 0).accepted, "control command was rejected");
  const auto controls_before_timeout = adapter_view->status().applied_command_count;

  service.tick(300);
  expect(
      service.safety_state() == mine_teleop::SafetyState::Degraded,
      "service did not enter degraded before receive-path timeout test");

  const auto gap_rearm = service.receive_command(command(2, 810), 810);
  expect(
      !gap_rearm.accepted && gap_rearm.reason == "command_gap_exceeded",
      "first packet after a hard timeout did not re-arm receiver timing");
  expect(
      service.safety_state() == mine_teleop::SafetyState::TimeoutBrake &&
          !service.session_control_profile().at("active").get<bool>(),
      "receive path failed to advance the hard timeout and revoke the profile");

  const auto blocked = service.receive_command(command(3, 820), 820);
  expect(
      !blocked.accepted && blocked.reason == "session_control_profile_required",
      "fresh packets bypassed profile re-authorization after the hard timeout");
  expect(
      adapter_view->status().applied_command_count == controls_before_timeout,
      "a command reached the adapter after receive-path hard timeout");
  service.close();
}

void test_control_service_preserves_physical_brake_across_degraded_timeout() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  {
    auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 10000);
    service.start(0);
    adapter_view->handshake.state = "standby";
    adapter_view->handshake.ready = false;
    const auto profile = service.receive_session_profile(
        session_profile_request(1, 0, 20.0, 100.0, 20.0, 10.0, 20.0),
        0);
    expect(profile.accepted, "20 bar session profile was rejected");
    adapter_view->set_safe_stop(false, true, false);
    auto braking = command(1, 0);
    braking.throttle = 0.8;
    braking.brake = 0.5;
    expect(
        service.receive_command(braking, 0).accepted,
        "session-scaled braking command was rejected");

    service.tick(300);
    expect(
        service.safety_state() == mine_teleop::SafetyState::Degraded,
        "service did not enter Degraded for brake-unit preservation test");
    expect_near(
        adapter_view->last_safe_output.brake,
        0.1,
        1e-9,
        "Degraded amplified a 10 bar session request above 10 bar");
    expect(
        !adapter_view->last_safe_output.full_emergency_brake,
        "Degraded session braking was mislabeled as full-DBC emergency braking");

    service.tick(800);
    expect_near(
        adapter_view->last_safe_output.brake,
        0.3,
        1e-9,
        "first timeout stage lost vehicle-normalized safety semantics");
    expect(
        !adapter_view->last_safe_output.full_emergency_brake,
        "first timeout stage incorrectly requested full-DBC emergency braking");
    service.tick(1300);
    expect_near(
        adapter_view->last_safe_output.brake,
        0.6,
        1e-9,
        "second timeout stage lost vehicle-normalized safety semantics");
    expect(
        !adapter_view->last_safe_output.full_emergency_brake,
        "second timeout stage incorrectly requested full-DBC emergency braking");
    service.tick(2300);
    expect(
        adapter_view->last_safe_output.full_emergency_brake &&
            adapter_view->last_safe_output.brake == 1.0,
        "final timeout stage did not retain full-DBC emergency braking");
    service.close();
  }

  {
    auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 10000);
    service.start(0);
    activate_adapter_owned_session_profile(service, *adapter_view);
    auto full_ordinary_brake = command(1, 0);
    full_ordinary_brake.throttle = 0.0;
    full_ordinary_brake.brake = 1.0;
    expect(
        service.receive_command(full_ordinary_brake, 0).accepted,
        "100 bar ordinary braking command was rejected");
    service.tick(300);
    expect(
        adapter_view->last_safe_output.brake == 1.0 &&
            !adapter_view->last_safe_output.full_emergency_brake,
        "Degraded confused ordinary 100 bar with full-DBC emergency braking");
    service.close();
  }
}

void test_control_service_defers_to_adapter_owned_safe_stop_until_fresh_handshake() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 10000);
  service.start(0);
  activate_adapter_owned_session_profile(service, *adapter_view);

  expect(
      service.receive_command(command(1, 0), 0).accepted,
      "initial control command was rejected");
  expect(
      adapter_view->applied_commands == 1 && adapter_view->control_attempts == 1,
      "initial control command did not reach the adapter exactly once");

  adapter_view->set_safe_stop(true, true, false);
  service.tick(100);
  const auto blocked = service.receive_command(command(2, 110), 110);
  expect(
      !blocked.accepted && blocked.reason == "session_control_profile_required",
      "adapter-owned safe stop did not withdraw ordinary profile authority");
  expect(
      service.safety_state() == mine_teleop::SafetyState::ControlActive &&
          adapter_view->control_attempts == 1,
      "blocked control refreshed outer safety state or reached the adapter");

  adapter_view->handshake_status_throws = true;
  service.tick(350);
  adapter_view->handshake_status_throws = false;
  expect(
      service.safety_state() == mine_teleop::SafetyState::Degraded,
      "outer safety state did not survive an unreadable adapter interlock");
  adapter_view->set_safe_stop(false, false, true);
  service.tick(900);
  expect(
      service.safety_state() == mine_teleop::SafetyState::TimeoutBrake,
      "outer safety state did not reach timeout while the adapter disarmed");
  expect(
      adapter_view->safe_stop_attempts == 0 &&
          adapter_view->rejected_ordinary_safe_stops == 0,
      "outer timeout repeated an ordinary safe stop owned by the adapter");

  adapter_view->set_safe_stop(false, false, false);
  activate_session_profile(service, 2, 900);

  adapter_view->handshake_succeeds = false;
  expect(
      !service.request_vcu_handshake(),
      "failed adapter handshake was reported as successful");
  expect(
      service.safety_state() == mine_teleop::SafetyState::TimeoutBrake &&
          adapter_view->handshake_requests == 1,
      "failed adapter handshake cleared the recoverable outer state");

  adapter_view->handshake_succeeds = true;
  expect(
      service.request_vcu_handshake(),
      "explicit adapter handshake recovery was rejected");
  expect(
      service.safety_state() == mine_teleop::SafetyState::Standby,
      "successful adapter handshake did not reset the recoverable outer state to Standby");
  service.tick(2000);
  expect(
      service.safety_state() == mine_teleop::SafetyState::Standby &&
          adapter_view->applied_commands == 1 &&
          adapter_view->safe_stop_attempts == 0,
      "post-handshake Standby replayed an old command or timeout output");

  expect(
      !adapter_view->feedback_ready(),
      "fake adapter incorrectly reported Ready while the handshake was Initial");
  adapter_view->set_safe_stop(false, true, false);
  const auto gap_rearm = service.receive_command(command(3, 2010), 2010);
  expect(
      !gap_rearm.accepted && gap_rearm.reason == "command_gap_exceeded",
      "first heartbeat after the intentional handshake gap did not re-arm timing");
  expect(
      service.safety_state() == mine_teleop::SafetyState::Standby &&
          adapter_view->control_attempts == 1,
      "gap re-arm heartbeat replayed control after the handshake");
  const auto fresh = service.receive_command(command(4, 2020), 2020);
  expect(fresh.accepted, "fresh post-handshake control command was rejected");
  expect(
      service.safety_state() == mine_teleop::SafetyState::ControlActive &&
          adapter_view->applied_commands == 2 &&
          adapter_view->last_control && adapter_view->last_control->seq == 4,
      "fresh post-handshake command did not exclusively restore control");
  service.close();
}

void test_adapter_handshake_does_not_clear_outer_estop_or_fault() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  {
    auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 100);
    service.start(0);
    adapter_view->set_safe_stop(true, true, false);
    auto estop = command(1, 0);
    estop.estop = true;
    expect(
        service.receive_command(estop, 0).accepted &&
            service.safety_state() == mine_teleop::SafetyState::Estop,
        "outer ESTOP command did not latch while the adapter owned the stop");
    expect(
        adapter_view->safe_stop_attempts == 1 &&
            adapter_view->last_safe_output.estop &&
            adapter_view->rejected_ordinary_safe_stops == 0,
        "outer ESTOP did not safely reassert the emergency stop");
    expect(
        !service.request_vcu_handshake() && adapter_view->handshake_requests == 0,
        "outer ESTOP allowed the adapter handshake to start");
    expect(
        service.safety_state() == mine_teleop::SafetyState::Estop,
        "rejected adapter handshake incorrectly cleared outer ESTOP");
    service.close();
  }

  {
    auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
    auto* adapter_view = adapter.get();
    adapter_view->telemetry_throws = true;
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 100);
    service.start(0);
    activate_adapter_owned_session_profile(service, *adapter_view);
    const auto failed = service.receive_command(command(1, 0), 0);
    expect(
        !failed.accepted && failed.reason == "adapter_safety_status_unavailable" &&
            service.safety_state() == mine_teleop::SafetyState::Fault,
        "adapter safety observation failure did not establish the outer Fault guard");
    adapter_view->telemetry_throws = false;
    expect(
        !service.request_vcu_handshake() && adapter_view->handshake_requests == 0,
        "outer Fault allowed the adapter handshake to start");
    expect(
        service.safety_state() == mine_teleop::SafetyState::Fault,
        "rejected adapter handshake incorrectly cleared outer Fault");
    service.close();
  }
}

void test_reset_estop_rejects_unreadable_disarming_and_hard_adapter_stops() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<AdapterOwnedSafeStopAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);

  auto estop = command(1, 0);
  estop.estop = true;
  expect(service.receive_command(estop, 0).accepted, "outer ESTOP command was rejected");
  expect(adapter_view->safe_stop_attempts == 1, "outer ESTOP was not applied");

  adapter_view->set_safe_stop(false, false, true);
  expect(
      !service.reset_estop(true, "operator", 5) &&
          service.safety_state() == mine_teleop::SafetyState::Estop &&
          adapter_view->safe_stop_attempts == 1,
      "adapter disarming allowed an outer ESTOP reset or ordinary apply");

  adapter_view->set_safe_stop(true, true, false);
  adapter_view->telemetry_throws = true;
  expect(
      !service.reset_estop(true, "operator", 10) &&
          service.safety_state() == mine_teleop::SafetyState::Estop,
      "unreadable adapter interlock allowed the outer ESTOP reset");
  adapter_view->telemetry_throws = false;
  expect(
      !service.reset_estop(true, "operator", 20) &&
          service.safety_state() == mine_teleop::SafetyState::Estop,
      "physical or hard adapter stop incorrectly cleared the outer ESTOP");
  expect(
      adapter_view->safe_stop_attempts == 2 &&
          adapter_view->rejected_ordinary_safe_stops == 1,
      "physical or hard adapter stop did not reject exactly one same-gear zero reset attempt");
  service.close();
}

void test_reset_estop_clears_soft_stop_before_fresh_control() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);

  auto estop = command(1, 0);
  estop.estop = true;
  expect(
      service.receive_command(estop, 0).accepted &&
          service.safety_state() == mine_teleop::SafetyState::Estop &&
          adapter_view->read_telemetry().estop,
      "soft outer ESTOP did not reach the mock adapter");
  expect(
      service.reset_estop(true, "operator", 10) &&
          service.safety_state() == mine_teleop::SafetyState::Standby &&
          !adapter_view->read_telemetry().estop &&
          adapter_view->read_telemetry().gear == "D",
      "authorized reset did not clear the adapter soft stop while preserving actual D");

  activate_session_profile(service, 1, 15);

  const auto fresh = service.receive_command(command(2, 20), 20);
  expect(
      fresh.accepted &&
          service.safety_state() == mine_teleop::SafetyState::ControlActive &&
          adapter_view->read_telemetry().throttle_feedback > 0.0,
      "fresh control did not recover after the authorized soft ESTOP reset");
  service.close();
}

void test_control_service_applies_vehicle_hard_limits() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  config.field_safety.max_throttle = 0.10;
  config.field_safety.max_brake_pressure_bar = 100.0;
  config.field_safety.max_steering_angle_deg = 3.0;
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  const auto profile = service.receive_session_profile(
      session_profile_request(1, 0, 4.0, 100.0, 100.0, 30.0, 80.0),
      0);
  expect(profile.accepted, "hard-limit test profile was rejected");

  auto requested = command(1, 0);
  requested.brake = 0.80;
  const auto result = service.receive_command(requested, 0);
  expect(result.accepted && result.command.has_value(), "limited control command was rejected");
  expect_near(result.command->throttle, 0.10, 1e-9, "vehicle throttle hard limit was not applied");
  expect_near(result.command->brake, 0.80, 1e-9, "normalized brake intent was rewritten before physical mapping");
  expect_near(result.command->steering, 0.10, 1e-9, "vehicle steering hard limit was not applied");
  expect(
      std::find(result.warnings.begin(), result.warnings.end(), "vehicle_max_throttle_applied") !=
          result.warnings.end(),
      "throttle limit application was not reported");
  expect(
      std::find(result.warnings.begin(), result.warnings.end(), "vehicle_max_steering_applied") !=
          result.warnings.end(),
      "steering limit application was not reported");
  const auto telemetry = adapter_view->read_telemetry();
  expect_near(telemetry.throttle_feedback, 0.10, 1e-9, "adapter received uncapped throttle");
  expect_near(telemetry.brake_feedback, 0.80, 1e-9, "adapter received a rewritten normalized brake intent");
  expect_near(telemetry.steering_feedback, 0.10, 1e-9, "adapter received uncapped steering");
  const auto limits = service.control_limits();
  expect_near(limits.at("max_throttle").get<double>(), 0.10, 1e-9, "reported throttle limit mismatch");
  expect_near(
      limits.at("full_scale_motor_torque_nm").get<double>(),
      300.0,
      1e-9,
      "reported full-scale motor torque mismatch");
  expect_near(
      limits.at("max_brake_pressure_bar").get<double>(),
      100.0,
      1e-9,
      "reported physical brake-pressure limit mismatch");
  expect_near(
      limits.at("max_steering_angle_deg").get<double>(),
      3.0,
      1e-9,
      "reported steering limit mismatch");
  expect_near(
      limits.at("default_speed_pid_kp").get<double>(),
      config.field_safety.speed_pid_kp,
      1e-9,
      "reported default PID kp mismatch");
  expect(
      limits.at("default_speed_pid_max_dt_ms") ==
              config.field_safety.speed_pid_max_dt_ms &&
          limits.at("speed_pid_limits").at("kp").at("min") == 0.0 &&
          limits.at("speed_pid_limits").at("kp").at("max") == 100.0 &&
          limits.at("speed_pid_limits").at("max_dt_ms").at("min") == 20 &&
          limits.at("speed_pid_limits").at("max_dt_ms").at("max") == 200,
      "reported PID defaults or absolute bounds mismatch");
  expect(
      limits.at("default_motor_torque_rise_rate_nm_per_s") ==
              config.field_safety.motor_torque_rise_rate_nm_per_s &&
          limits.at("motor_torque_rise_rate_limits_nm_per_s").at("min") ==
              0.0 &&
          limits.at("motor_torque_rise_rate_limits_nm_per_s").at("max") ==
              mine_teleop::kMaxMotorTorqueRiseRateNmPerSecond,
      "reported rise-rate default or absolute bounds mismatch");
  mine_teleop::Json expected_deceleration_profile =
      mine_teleop::Json::array();
  for (const auto& stage : config.control.deceleration_profile) {
    expected_deceleration_profile.push_back(
        {{"after_ms", stage.after_ms}, {"brake", stage.brake}});
  }
  const mine_teleop::Json expected_read_only_control_safety{
      {"control_rate_hz", config.control.rate_hz},
      {"max_command_gap_ms", config.control.max_command_gap_ms},
      {"degraded_timeout_ms", config.control.degraded_timeout_ms},
      {"control_timeout_ms", config.control.control_timeout_ms},
      {"deceleration_profile", expected_deceleration_profile},
      {"speed_feedback_timeout_ms",
       config.field_safety.speed_feedback_timeout_ms},
      {"hard_overspeed_margin_kph",
       config.field_safety.hard_overspeed_margin_kph},
      {"require_can_feedback_before_control",
       config.field_safety.require_can_feedback_before_control},
      {"require_local_estop_reset",
       config.field_safety.require_local_estop_reset},
      {"require_time_sync", config.field_safety.require_time_sync},
      {"max_time_sync_uncertainty_ms",
       config.field_safety.max_time_sync_uncertainty_ms},
      {"time_sync_interval_ms", config.field_safety.time_sync_interval_ms},
      {"time_sync_samples", config.field_safety.time_sync_samples},
      {"commissioning_mode", config.field_safety.commissioning_mode},
  };
  expect(
      limits.at("read_only_control_safety") ==
          expected_read_only_control_safety,
      "read-only control safety report omitted or altered validated vehicle settings");
  service.close();
  expect_near(
      adapter_view->read_telemetry().brake_feedback,
      1.0,
      1e-9,
      "ordinary brake limit weakened disconnect safe-stop output");
}

void test_control_service_applies_session_steering_limit() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  config.field_safety.max_steering_angle_deg = 5.0;
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  auto profile = session_profile_request(1, 0);
  profile.profile.max_steering_angle_deg = 3.0;
  const auto profile_result = service.receive_session_profile(profile, 0);
  expect(profile_result.accepted, "session steering profile was rejected");

  auto requested = command(1, 0);
  requested.steering = 0.25;
  const auto result = service.receive_command(requested, 0);
  expect(
      result.accepted && result.command.has_value(),
      "session steering-limited command was rejected");
  expect_near(
      result.command->steering,
      0.10,
      1e-9,
      "active session steering ceiling was not applied");
  expect(
      std::find(
          result.warnings.begin(),
          result.warnings.end(),
          "session_max_steering_applied") != result.warnings.end(),
      "session steering ceiling application was not reported");
  expect_near(
      adapter_view->read_telemetry().steering_feedback,
      0.10,
      1e-9,
      "adapter received steering above the active session ceiling");
  service.close();
}

void test_control_service_bounds_telemetry_history() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  mine_teleop::VehicleControlService service(
      config,
      "driver-001",
      "session-001",
      "token",
      std::make_unique<mine_teleop::MockVehicleAdapter>(),
      1);
  service.start(0);
  constexpr std::size_t total_samples = mine_teleop::kMaxVehicleTelemetryHistory * 2 + 1;
  for (std::size_t sample = 0; sample < total_samples; ++sample) {
    service.tick(static_cast<std::int64_t>(sample));
  }
  const auto& history = service.telemetry_history();
  expect(
      history.size() == mine_teleop::kMaxVehicleTelemetryHistory,
      "vehicle telemetry history grew beyond its retention bound");
  expect(
      history.front().at("seq").get<std::uint64_t>() == total_samples - history.size() + 1,
      "vehicle telemetry history did not discard its oldest sample");
  expect(
      history.back().at("seq").get<std::uint64_t>() == total_samples,
      "vehicle telemetry history did not retain its newest sample");
  expect(
      history.back().at("can_feedback").at("supported").get<bool>() == false,
      "mock telemetry incorrectly advertised measured CAN feedback");
  expect(
      history.back().at("stop_source").get<std::string>() == "none" &&
          history.back().at("stop_reason").get<std::string>() == "none" &&
          history.back().at("stop_sequence").get<std::uint64_t>() == 0,
      "idle telemetry omitted or misreported stop provenance");
  const auto summary = service.summary();
  expect(summary.at("telemetry_count").get<std::uint64_t>() == total_samples, "telemetry total count was truncated");
  expect(
      summary.at("telemetry_retained_count").get<std::size_t>() == mine_teleop::kMaxVehicleTelemetryHistory,
      "telemetry retained count does not match the bounded history");

  auto estop = command(
      static_cast<std::uint64_t>(total_samples + 1),
      static_cast<std::int64_t>(total_samples + 1));
  estop.estop = true;
  expect(
      service.receive_command(
          estop,
          static_cast<std::int64_t>(total_samples + 1)).accepted,
      "page ESTOP was rejected while checking telemetry provenance");
  service.tick(static_cast<std::int64_t>(total_samples + 2));
  const auto& stopped = service.telemetry_history().back();
  expect(
      stopped.at("stop_source").get<std::string>() == "page_request" &&
          stopped.at("stop_reason").get<std::string>() == "operator_estop" &&
          stopped.at("stop_sequence").get<std::uint64_t>() > 0,
      "page ESTOP provenance did not reach vehicle telemetry");
  service.close();
}

void test_control_service_close_preserves_stop_provenance() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  {
    auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 100);
    service.start(0);
    service.close({
        mine_teleop::VehicleStopSource::SoftwareFault,
        mine_teleop::VehicleStopReason::CriticalCameraFailed});
    const auto telemetry = adapter_view->read_telemetry();
    expect(
        telemetry.estop && telemetry.stop_source == "software_fault" &&
            telemetry.stop_reason == "critical_camera_failed" &&
            telemetry.stop_sequence == 1,
        "critical-camera close lost its software-fault stop provenance");
  }
  {
    auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 100);
    service.start(0);
    service.close({
        mine_teleop::VehicleStopSource::SoftwareFault,
        mine_teleop::VehicleStopReason::MediaPipelineFailed});
    const auto telemetry = adapter_view->read_telemetry();
    expect(
        telemetry.estop && telemetry.stop_source == "software_fault" &&
            telemetry.stop_reason == "media_pipeline_failed" &&
            telemetry.stop_sequence == 1,
        "media-pipeline close lost its software-fault stop provenance");
  }
  {
    auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
    auto* adapter_view = adapter.get();
    mine_teleop::VehicleControlService service(
        config, "driver-001", "session-001", "token", std::move(adapter), 100);
    service.start(0);
    service.close();
    const auto telemetry = adapter_view->read_telemetry();
    expect(
        telemetry.estop && telemetry.stop_source == "session_loss" &&
            telemetry.stop_reason == "session_lost" &&
            telemetry.stop_sequence == 1,
        "ordinary service close no longer reports session-loss provenance");
  }
}

void test_control_service_requires_feedback_before_actuation_but_allows_estop() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  config.field_safety.require_can_feedback_before_control = true;
  auto adapter = std::make_unique<NoFeedbackAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  activate_session_profile(service);

  const auto rejected = service.receive_command(command(1, 0), 0);
  expect(!rejected.accepted && rejected.reason == "can_feedback_missing", "control was not gated on CAN feedback");
  expect(adapter_view->applied_commands == 0, "control reached chassis without CAN feedback");
  expect(adapter_view->safe_stops == 1, "missing feedback did not issue a safe stop");
  expect_near(adapter_view->last_safe_output.brake, 1.0, 1e-9, "feedback gate did not use full brake");

  auto estop = command(2, 10);
  estop.estop = true;
  const auto accepted_estop = service.receive_command(estop, 10);
  expect(accepted_estop.accepted, "estop must bypass the feedback gate");
  expect(service.safety_state() == mine_teleop::SafetyState::Estop, "estop did not latch without feedback");
  expect_near(
      adapter_view->last_safe_output.brake,
      1.0,
      1e-9,
      "ordinary brake limit weakened emergency-stop output");
  service.close();
}

void test_fault_output_fails_safe() {
  mine_teleop::SafetyStateMachine safety(
      300, 800, {{0, 0.3}, {1500, 1.0}});
  safety.mark_ready(0);
  safety.mark_fault();
  const auto output = safety.current_output(0);
  expect_near(output.brake, 1.0, 1e-9, "fault output must command full brake");
  expect_near(output.throttle, 0.0, 1e-9, "fault output must clear throttle");
  expect(output.full_emergency_brake, "fault output did not request full-DBC braking");
}

void test_native_signaling_webrtc_message_isolation() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-1", "secret"}, {"driver-2", "secret-2"}};
  config.device_tokens = {{"vehicle-001", "device-secret"}};
  config.driver_vehicle_permissions = {
      {"driver-1", {"vehicle-001"}},
      {"driver-2", {"vehicle-001"}}};
  config.max_sdp_bytes = 64;
  config.max_ice_candidate_bytes = 32;
  config.signaling_message_ttl_ms = 50;
  const auto audit_path = std::filesystem::path("/tmp") / ("mine-teleop-audit-" + mine_teleop::random_token(6) + ".jsonl");
  config.audit_log_path = audit_path.string();
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;

  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "device-secret"},
       {"connection_id", "vehicle-test-connection"}});
  expect(online.value("state", "") == "online", "vehicle did not register online");
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
  const auto login = http.post_json_response(
      base + "/auth/driver_login", {{"driver_id", "driver-1"}, {"password", "secret"}});
  const auto driver_token = login.at("token").get<std::string>();
  const auto session = http.post_json_response(
      base + "/sessions", {{"driver_id", "driver-1"}, {"vehicle_id", "vehicle-001"}, {"token", driver_token}});
  expect(session.value("state", "") == "active", "new session did not become active");
  const auto vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=device-secret&connection_generation=" +
      std::to_string(vehicle_generation));
  expect(
      vehicle_session.value("control_token", "") == session.value("control_token", ""),
      "vehicle did not receive the current control token");

  const auto second_login = http.post_json_response(
      base + "/auth/driver_login", {{"driver_id", "driver-2"}, {"password", "secret-2"}});
  const auto second_driver_token = second_login.at("token").get<std::string>();
  const auto conflict = http.post_json(
      base + "/sessions",
      {{"driver_id", "driver-2"}, {"vehicle_id", "vehicle-001"}, {"token", second_driver_token}});
  expect(conflict.status == 409, "second driver stole an active vehicle session");

  const auto session_id = session.at("session_id").get<std::string>();
  auto missing_metadata = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "driver-1",
      "vehicle-001",
      "token",
      driver_token,
      "media_capabilities",
      {{"codecs", {"h264", "h265"}}});
  missing_metadata.erase("driver_id");
  expect(
      http.post_json(base + "/signaling/" + session_id + "/messages", missing_metadata).status == 400,
      "signaling message without required protocol metadata was accepted");

  auto wrong_version = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "driver-1",
      "vehicle-001",
      "token",
      driver_token,
      "media_capabilities",
      {{"codecs", {"h264", "h265"}}});
  wrong_version["protocol_version"] = mine_teleop::kProtocolVersion + 1;
  expect(
      http.post_json(base + "/signaling/" + session_id + "/messages", wrong_version).status == 400,
      "signaling message with an unsupported protocol version was accepted");

  auto media_request = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "driver-1",
      "vehicle-001",
      "token",
      driver_token,
      "media_capabilities",
      {{"codecs", {"h264", "h265"}}});
  media_request["future_extension"] = {{"safe_to_ignore", true}};
  const auto media = http.post_json_response(base + "/signaling/" + session_id + "/messages", media_request);
  expect(media.value("queued", 0) == 1, "media capabilities were not queued");
  const auto duplicate_media =
      http.post_json_response(base + "/signaling/" + session_id + "/messages", media_request);
  expect(
      duplicate_media.value("duplicate", false) &&
          duplicate_media.value("message_id", "") == media.value("message_id", "") &&
          duplicate_media.value("delivery_cursor", std::uint64_t{0}) ==
              media.value("delivery_cursor", std::uint64_t{0}),
      "identical signaling retry did not return its stable acknowledgement");
  auto conflicting_media = media_request;
  conflicting_media["payload"] = {{"codecs", {"h264"}}};
  const auto conflicting_media_response =
      http.post_json(base + "/signaling/" + session_id + "/messages", conflicting_media);
  expect(
      conflicting_media_response.status == 409,
      "signaling sequence reuse with different content was accepted");
  const auto conflicting_media_body = mine_teleop::Json::parse(conflicting_media_response.body);
  expect(
      conflicting_media_body.value("issue_code", "") == "signaling_sequence_reused",
      "signaling sequence reuse conflict omitted its structured issue code");
  expect(
      conflicting_media_body.value("received_seq", std::uint64_t{0}) == 1 &&
          conflicting_media_body.value("last_accepted_seq", std::uint64_t{0}) == 1,
      "signaling sequence reuse conflict omitted its sequence details");

  expect(
      http.post_json(
              base + "/signaling/" + session_id + "/messages",
              signaling_request(
                  "vehicle-001",
                  "driver-1",
                  session_id,
                  2,
                  mine_teleop::now_ms(),
                  "driver-1",
                  "vehicle-001",
                  "token",
                  driver_token,
                  "webrtc_answer",
                  {{"type", "answer"}, {"sdp", std::string(65, 's')}}))
              .status == 400,
      "oversized WebRTC SDP was accepted");
  expect(
      http.post_json(
              base + "/signaling/" + session_id + "/messages",
              signaling_request(
                  "vehicle-001",
                  "driver-1",
                  session_id,
                  2,
                  mine_teleop::now_ms(),
                  "driver-1",
                  "vehicle-001",
                  "token",
                  driver_token,
                  "ice_candidate",
                  {{"candidate", std::string(33, 'c')}}))
              .status == 400,
      "oversized WebRTC ICE candidate was accepted");

  auto control = command(2, mine_teleop::now_ms());
  control.session_id = session.at("session_id").get<std::string>();
  control.driver_id = "driver-1";
  control.control_token = session.at("control_token").get<std::string>();
  const auto rejected_control_route = http.post_json(
      base + "/signaling/" + control.session_id + "/messages",
      signaling_request(
          control.vehicle_id,
          control.driver_id,
          control.session_id,
          control.seq,
          control.sent_at_utc_ms,
          "driver-1",
          "vehicle-001",
          "token",
          driver_token,
          "control_command",
          control.to_json()));
  expect(rejected_control_route.status == 400, "signaling server accepted a DataChannel control command");

  const auto media_messages = http.get_json(
      base + "/signaling/" + control.session_id +
      "/messages?recipient=vehicle-001&device_token=device-secret&connection_generation=" +
      std::to_string(vehicle_generation) + "&types=media_capabilities");
  expect(media_messages.at("messages").size() == 1, "control polling consumed WebRTC capabilities");
  expect(media_messages.at("messages").at(0).value("type", "") == "media_capabilities", "media message type changed");
  auto stale_offer = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "vehicle-001",
      "driver-1",
      "device_token",
      "device-secret",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0"}});
  stale_offer["connection_generation"] = vehicle_generation;
  expect(
      http.post_json_response(base + "/signaling/" + session_id + "/messages", stale_offer).value("queued", 0) == 1,
      "fresh WebRTC offer was not queued");
  std::this_thread::sleep_for(std::chrono::milliseconds(70));
  const auto expired_offers = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=driver-1&token=" +
      http.url_encode(driver_token) + "&types=webrtc_offer");
  expect(expired_offers.at("messages").empty(), "expired WebRTC offer was redelivered after reconnect polling");
  const auto fallback = http.post_json_response(
      base + "/signaling/" + control.session_id + "/messages",
      signaling_request(
          "vehicle-001",
          "driver-1",
          control.session_id,
          3,
          mine_teleop::now_ms(),
          "driver-1",
          "vehicle-001",
          "token",
          driver_token,
          "media_fallback",
          {{"codec", "h264"}, {"reason", "decode_fps_below_20"}}));
  expect(fallback.value("queued", 0) == 1, "media fallback was not queued");
  const auto older_media_response = http.post_json(
      base + "/signaling/" + control.session_id + "/messages",
      signaling_request(
          "vehicle-001",
          "driver-1",
          control.session_id,
          2,
          mine_teleop::now_ms(),
          "driver-1",
          "vehicle-001",
          "token",
          driver_token,
          "media_capabilities",
          {{"codecs", {"h264"}}}));
  expect(older_media_response.status == 409, "older signaling sequence was accepted");
  const auto older_media_body = mine_teleop::Json::parse(older_media_response.body);
  expect(
      older_media_body.value("issue_code", "") == "signaling_sequence_older",
      "older signaling conflict omitted its structured issue code");
  expect(
      older_media_body.value("received_seq", std::uint64_t{0}) == 2 &&
          older_media_body.value("last_accepted_seq", std::uint64_t{0}) == 3,
      "older signaling conflict omitted its sequence details");
  bool decoded_structured_conflict = false;
  try {
    static_cast<void>(http.post_json_response(
        base + "/signaling/" + control.session_id + "/messages",
        signaling_request(
            "vehicle-001",
            "driver-1",
            control.session_id,
            2,
            mine_teleop::now_ms(),
            "driver-1",
            "vehicle-001",
            "token",
            driver_token,
            "media_capabilities",
            {{"codecs", {"h264"}}})));
  } catch (const mine_teleop::HttpStatusError& error) {
    decoded_structured_conflict =
        error.status() == 409 &&
        error.issue_code() == "signaling_sequence_older" &&
        mine_teleop::classify_media_signaling_error(error) ==
            mine_teleop::MediaSignalingErrorKind::SequenceConflict &&
        error.response_body().find("last_accepted_seq") != std::string::npos;
  }
  expect(
      decoded_structured_conflict,
      "HTTP client did not preserve and classify the structured signaling conflict");
  const auto fallback_messages = http.get_json(
      base + "/signaling/" + control.session_id +
      "/messages?recipient=vehicle-001&device_token=device-secret&connection_generation=" +
      std::to_string(vehicle_generation) + "&types=media_fallback");
  expect(fallback_messages.at("messages").size() == 1, "vehicle did not receive media fallback");
  expect(
      fallback_messages.at("messages").at(0).value("type", "") == "media_fallback",
      "media fallback message type changed");
  const auto ended = http.post_json_response(
      base + "/sessions/" + control.session_id + "/end",
      {{"actor", "driver-1"}, {"token", driver_token}, {"reason", "test_complete"}});
  expect(ended.value("state", "") == "closed", "ended session did not reach closed state");
  expect(!ended.contains("control_token"), "closed session exposed its old control token");

  const auto replacement = http.post_json_response(
      base + "/sessions",
      {{"driver_id", "driver-2"}, {"vehicle_id", "vehicle-001"}, {"token", second_driver_token}});
  expect(replacement.value("state", "") == "active", "replacement session did not become active");
  expect(
      replacement.value("control_token", "") != control.control_token,
      "replacement session reused the previous control token");
  auto old_token_command = command(1, mine_teleop::now_ms());
  old_token_command.session_id = replacement.at("session_id").get<std::string>();
  old_token_command.driver_id = "driver-2";
  old_token_command.control_token = control.control_token;
  mine_teleop::ControlReceiver replacement_receiver(
      "vehicle-001",
      "driver-2",
      old_token_command.session_id,
      200,
      mine_teleop::kProtocolVersion,
      true,
      replacement.at("control_token").get<std::string>());
  const auto old_token_replay = replacement_receiver.accept(old_token_command, mine_teleop::now_ms());
  expect(
      !old_token_replay.accepted && old_token_replay.reason == "control_token_invalid",
      "replacement vehicle receiver accepted the previous control token");
  server.stop();

  std::ifstream audit_input(audit_path);
  const std::string audit_log((std::istreambuf_iterator<char>(audit_input)), std::istreambuf_iterator<char>());
  for (const std::string state : {"reserved", "connecting", "active", "stopping", "closed"}) {
    expect(audit_log.find("\"to\":\"" + state + "\"") != std::string::npos, "missing audited session state: " + state);
  }
  expect(
      audit_log.find("\"event\":\"signaling_retry_acknowledged\"") != std::string::npos,
      "idempotent signaling retry was not audited");
  expect(audit_log.find("control-token-") == std::string::npos, "audit log leaked a control token");
  std::filesystem::remove(audit_path);
}

void test_signaling_presence_generation_and_automatic_release() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-1", "secret-1"}, {"driver-2", "secret-2"}};
  config.device_tokens = {{"vehicle-1", "device-1"}, {"vehicle-2", "device-2"}};
  config.driver_vehicle_permissions = {
      {"driver-1", {"vehicle-1"}},
      {"driver-2", {"vehicle-2"}}};
  config.admin_token = "test-admin-token";
  config.control_token_ttl_ms = 200;
  config.vehicle_heartbeat_timeout_ms = 500;
  config.driver_heartbeat_timeout_ms = 500;
  config.connection_reaper_interval_ms = 5;
  config.stun_urls = {"stun:turn.example.test:3478"};
  config.turn_urls = {
      "turn:turn.example.test:3478?transport=udp",
      "turns:turn.example.test:5349?transport=tcp"};
  config.turn_realm = "teleop.example.test";
  config.turn_static_auth_secret = "test-static-auth-secret";
  config.turn_credential_ttl_seconds = 60;
  const auto audit_path = std::filesystem::path("/tmp") /
      ("mine-teleop-presence-audit-" + mine_teleop::random_token(6) + ".jsonl");
  config.audit_log_path = audit_path.string();
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;

  const auto register_vehicle = [&](std::string_view vehicle_id, std::string_view token, std::string_view connection_id) {
    return http.post_json_response(
        base + "/vehicles/online",
        {{"vehicle_id", vehicle_id}, {"device_token", token}, {"connection_id", connection_id}});
  };
  const auto login_driver = [&](std::string_view driver_id, std::string_view password) {
    return http.post_json_response(
        base + "/auth/driver_login", {{"driver_id", driver_id}, {"password", password}});
  };
  const auto create_session = [&](std::string_view driver_id, std::string_view vehicle_id, std::string_view token) {
    return http.post_json_response(
        base + "/sessions", {{"driver_id", driver_id}, {"vehicle_id", vehicle_id}, {"token", token}});
  };

  const auto vehicle_1 = register_vehicle("vehicle-1", "device-1", "vehicle-1-runtime-a");
  const auto vehicle_1_generation = vehicle_1.at("connection_generation").get<std::uint64_t>();
  const auto vehicle_1_refresh = register_vehicle("vehicle-1", "device-1", "vehicle-1-runtime-a");
  expect(
      vehicle_1_refresh.at("connection_generation").get<std::uint64_t>() == vehicle_1_generation,
      "same vehicle connection retry created a new generation");
  expect(
      vehicle_1_refresh.value("duplicate_policy", "") == "same_connection_refresh",
      "same vehicle connection retry policy is not explicit");
  const auto vehicle_2 = register_vehicle("vehicle-2", "device-2", "vehicle-2-runtime-a");
  const auto vehicle_2_generation = vehicle_2.at("connection_generation").get<std::uint64_t>();

  const auto driver_1 = login_driver("driver-1", "secret-1");
  const auto driver_2 = login_driver("driver-2", "secret-2");
  const auto driver_1_token = driver_1.at("token").get<std::string>();
  auto driver_2_token = driver_2.at("token").get<std::string>();
  const auto session_1 = create_session("driver-1", "vehicle-1", driver_1_token);
  expect(
      http.post_json(
              base + "/sessions",
              {{"driver_id", "driver-1"}, {"vehicle_id", "vehicle-2"}, {"token", driver_1_token}})
              .status == 401,
      "driver created a session for a vehicle outside its permission list");
  const auto session_2 = create_session("driver-2", "vehicle-2", driver_2_token);
  expect(session_1.value("state", "") == "active" && session_2.value("state", "") == "active", "2x2 sessions did not become active");
  expect(
      http.post_json(
              base + "/auth/driver_login",
              {{"driver_id", "driver-1"}, {"password", "secret-1"}})
              .status == 409,
      "password-only duplicate driver login replaced an active connection");
  const auto health_2x2 = http.get_json(base + "/health");
  expect(health_2x2.value("online_vehicles", 0) == 2, "two vehicles were not simultaneously online");
  expect(health_2x2.value("online_drivers", 0) == 2, "two drivers were not simultaneously online");
  expect(health_2x2.value("active_sessions", 0) == 2, "two independent sessions were not simultaneously active");

  const auto driver_ice = http.get_json(
      base + "/sessions/" + session_1.at("session_id").get<std::string>() +
      "/ice_servers?actor=driver-1&token=" + http.url_encode(driver_1_token));
  const auto vehicle_ice = http.get_json(
      base + "/sessions/" + session_1.at("session_id").get<std::string>() +
      "/ice_servers?actor=vehicle-1&device_token=device-1&connection_generation=" +
      std::to_string(vehicle_1_generation));
  expect(driver_ice.at("ice_servers").size() == 2, "driver did not receive STUN and TURN ICE entries");
  expect(vehicle_ice.at("ice_servers").size() == 2, "vehicle did not receive STUN and TURN ICE entries");
  expect(
      driver_ice.at("ice_servers").at(0).at("urls") == vehicle_ice.at("ice_servers").at(0).at("urls") &&
          driver_ice.at("ice_servers").at(1).at("urls") == vehicle_ice.at("ice_servers").at(1).at("urls"),
      "driver and vehicle received different ICE endpoints");
  const auto driver_turn_username = driver_ice.at("ice_servers").at(1).at("username").get<std::string>();
  const auto vehicle_turn_username = vehicle_ice.at("ice_servers").at(1).at("username").get<std::string>();
  const auto driver_turn_credential = driver_ice.at("ice_servers").at(1).at("credential").get<std::string>();
  expect(
      driver_turn_username.find(session_1.at("session_id").get<std::string>()) != std::string::npos &&
          driver_turn_username.ends_with(":driver-1"),
      "driver TURN username is not bound to its session and actor");
  expect(
      vehicle_turn_username.ends_with(":vehicle-1") && vehicle_turn_username != driver_turn_username,
      "vehicle TURN username is not independently actor-bound");
  expect(!driver_turn_credential.empty(), "TURN REST credential is empty");
  expect(
      driver_ice.value("expires_at_utc_ms", 0LL) > mine_teleop::now_ms(),
      "TURN REST credential is not short-lived");

  const auto cross_vehicle = http.post_json(
      base + "/signaling/" + session_2.at("session_id").get<std::string>() + "/messages",
      signaling_request(
          "vehicle-2",
          "driver-2",
          session_2.at("session_id").get<std::string>(),
          1,
          mine_teleop::now_ms(),
          "driver-1",
          "vehicle-2",
          "token",
          driver_1_token,
          "media_capabilities",
          {{"codecs", {"h264"}}}));
  expect(cross_vehicle.status == 401, "driver from another session crossed the vehicle boundary");

  const auto revoked_driver = http.post_json_response(
      base + "/admin/revoke/driver",
      {{"admin_token", "test-admin-token"}, {"id", "driver-2"}});
  expect(revoked_driver.value("state", "") == "revoked", "admin did not revoke one driver");
  const auto health_after_driver_revoke = http.get_json(base + "/health");
  expect(
      health_after_driver_revoke.value("online_drivers", 0) == 1 &&
          health_after_driver_revoke.value("active_sessions", 0) == 1,
      "revoking driver-2 affected driver-1 or failed to close driver-2 session");
  expect(
      http.post_json(
              base + "/auth/driver_heartbeat",
              {{"driver_id", "driver-2"}, {"token", driver_2_token}})
              .status == 401,
      "revoked driver token remained valid");
  static_cast<void>(http.post_json_response(
      base + "/admin/restore/driver",
      {{"admin_token", "test-admin-token"}, {"id", "driver-2"}}));
  driver_2_token = login_driver("driver-2", "secret-2").at("token").get<std::string>();
  const auto restored_driver_session = create_session("driver-2", "vehicle-2", driver_2_token);
  expect(restored_driver_session.value("state", "") == "active", "restored driver could not regain permitted control");

  const auto vehicle_1_replacement = register_vehicle("vehicle-1", "device-1", "vehicle-1-runtime-b");
  const auto replacement_generation = vehicle_1_replacement.at("connection_generation").get<std::uint64_t>();
  expect(replacement_generation != vehicle_1_generation, "replacement vehicle connection reused its old generation");
  expect(
      vehicle_1_replacement.value("duplicate_policy", "") == "replace_previous_connection",
      "replacement vehicle connection policy is not explicit");
  expect(
      http.post_json(
              base + "/vehicles/heartbeat",
              {{"vehicle_id", "vehicle-1"},
               {"device_token", "device-1"},
               {"connection_generation", vehicle_1_generation}})
              .status == 409,
      "stale vehicle generation refreshed the replacement connection");
  expect(
      http.get(
              base + "/vehicles/vehicle-1/session?device_token=device-1&connection_generation=" +
              std::to_string(vehicle_1_generation))
              .status == 409,
      "stale vehicle generation discovered a replacement session");

  const auto replacement_session = create_session("driver-1", "vehicle-1", driver_1_token);
  expect(
      replacement_session.value("control_token", "") != session_1.value("control_token", ""),
      "replacement session reused the closed session control token");

  const auto logout = http.post_json_response(
      base + "/auth/driver_logout",
      {{"driver_id", "driver-2"}, {"token", driver_2_token}, {"reason", "browser_page_closed"}});
  expect(logout.value("state", "") == "offline", "driver logout did not transition offline");
  const auto vehicle_2_session = http.get_json(
      base + "/vehicles/vehicle-2/session?device_token=device-2&connection_generation=" +
      std::to_string(vehicle_2_generation));
  expect(vehicle_2_session.value("session_id", "").empty(), "driver logout did not release vehicle control authority");
  expect(
      http.post_json(
              base + "/auth/driver_heartbeat",
              {{"driver_id", "driver-2"}, {"token", driver_2_token}})
              .status == 401,
      "logged-out driver token remained valid");
  const auto revoked_vehicle = http.post_json_response(
      base + "/admin/revoke/vehicle",
      {{"admin_token", "test-admin-token"}, {"id", "vehicle-2"}});
  expect(revoked_vehicle.value("state", "") == "revoked", "admin did not revoke one vehicle");
  const auto health_after_vehicle_revoke = http.get_json(base + "/health");
  expect(
      health_after_vehicle_revoke.value("online_vehicles", 0) == 1,
      "revoking vehicle-2 affected vehicle-1");
  expect(
      http.post_json(
              base + "/vehicles/online",
              {{"vehicle_id", "vehicle-2"},
               {"device_token", "device-2"},
               {"connection_id", "vehicle-2-runtime-b"}})
              .status == 401,
      "revoked vehicle unexpectedly registered online");
  static_cast<void>(http.post_json_response(
      base + "/admin/restore/vehicle",
      {{"admin_token", "test-admin-token"}, {"id", "vehicle-2"}}));

  const auto control_expiry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
  mine_teleop::Json after_control_expiry;
  do {
    after_control_expiry = http.get_json(base + "/health");
    if (after_control_expiry.value("active_sessions", 0) == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < control_expiry_deadline);
  expect(after_control_expiry.value("active_sessions", 0) == 0, "short-lived control token did not release its session");

  const auto vehicle_1_after_expiry = http.get_json(
      base + "/vehicles/vehicle-1/session?device_token=device-1&connection_generation=" +
      std::to_string(replacement_generation));
  expect(vehicle_1_after_expiry.value("session_id", "").empty(), "expired control token still exposed an active session");
  static_cast<void>(http.post_json_response(
      base + "/auth/driver_heartbeat", {{"driver_id", "driver-1"}, {"token", driver_1_token}}));

  const auto heartbeat_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
  mine_teleop::Json after_heartbeat_timeout;
  do {
    after_heartbeat_timeout = http.get_json(base + "/health");
    if (after_heartbeat_timeout.value("online_vehicles", 0) == 0 &&
        after_heartbeat_timeout.value("online_drivers", 0) == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < heartbeat_deadline);
  expect(after_heartbeat_timeout.value("online_vehicles", 0) == 0, "vehicle heartbeat timeout did not transition offline");
  expect(after_heartbeat_timeout.value("online_drivers", 0) == 0, "driver heartbeat timeout did not transition offline");
  expect(
      http.get(
              base + "/vehicles/vehicle-1/session?device_token=device-1&connection_generation=" +
              std::to_string(replacement_generation))
              .status == 409,
      "offline vehicle connection was reported as an authentication failure instead of a recoverable conflict");
  server.stop();

  std::ifstream audit_input(audit_path);
  const std::string audit_log((std::istreambuf_iterator<char>(audit_input)), std::istreambuf_iterator<char>());
  for (const std::string event : {
           "vehicle_connection_replaced",
           "driver_logout",
           "driver_revoked",
           "driver_restored",
           "vehicle_revoked",
           "vehicle_restored",
           "control_authority_expired",
           "ice_servers_issued",
           "vehicle_offline",
           "driver_offline"}) {
    expect(audit_log.find("\"event\":\"" + event + "\"") != std::string::npos, "missing presence audit event: " + event);
  }
  expect(audit_log.find("driver-token-") == std::string::npos, "presence audit leaked a driver token");
  expect(audit_log.find("control-token-") == std::string::npos, "presence audit leaked a control token");
  expect(audit_log.find(driver_turn_credential) == std::string::npos, "presence audit leaked a TURN credential");
  std::filesystem::remove(audit_path);
}

void test_signaling_time_sync_common_domain() {
  auto service = std::make_shared<mine_teleop::SignalingService>(mine_teleop::SignalingServerConfig{});
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  mine_teleop::SynchronizedClock clock;
  const auto status = clock.synchronize(http, base, 7);
  expect(status.synchronized, "clock did not synchronize to signaling time");
  expect(status.sample_count == 7, "clock synchronization sample count changed");
  expect(status.acceptable(25), "local signaling clock uncertainty exceeded 25ms");
  const auto first = clock.now_ms();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto second = clock.now_ms();
  expect(second >= first, "synchronized clock moved backwards");
  const auto direct = http.get_json(base + "/time?client_send_ms=12345");
  expect(direct.value("client_send_ms", 0) == 12345, "time endpoint did not echo the client timestamp");
  expect(direct.value("time_domain", "") == "signaling_server", "time endpoint domain changed");
  server.stop();
}

void test_driver_config_and_hardware_encoder_priority() {
  const auto config = mine_teleop::load_driver_config("configs/driver-console.dev.yaml");
  expect(config.driver_id == "driver-console-001", "driver config id mismatch");
  const auto vehicle = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  expect(vehicle.hardware.preferred_encoder == "nvenc", "NVIDIA is not the preferred encoder");
  expect(vehicle.hardware.fallback_encoder == "vaapi", "Intel VAAPI is not the fallback encoder");
  expect(vehicle.hardware.preferred_codec == "h265", "H.265 is not the preferred codec");
  const auto candidates = mine_teleop::encoder_candidate_order(vehicle.hardware, mine_teleop::VideoCodec::H265);
  expect(candidates.size() == 2, "hardware encoder fallback candidate is missing");
  expect(candidates.at(0).backend == mine_teleop::EncoderBackend::Nvenc, "NVENC priority changed");
  expect(candidates.at(1).backend == mine_teleop::EncoderBackend::Vaapi, "VAAPI fallback priority changed");
  expect(candidates.at(0).codec == mine_teleop::VideoCodec::H265, "preferred candidate codec changed");
}

void test_nvenc_pipeline_stage_tracks_gstreamer_property_compatibility() {
  const mine_teleop::VideoEncoderSettings settings{2500, 30};
  const auto stage_120 =
      mine_teleop::build_nvenc_pipeline_stage("nvh264enc", settings, "encoder_front", 1, 20);
  const auto stage_122 =
      mine_teleop::build_nvenc_pipeline_stage("nvh264enc", settings, "encoder_front", 1, 22);
  const auto stage_124 =
      mine_teleop::build_nvenc_pipeline_stage("nvh264enc", settings, "encoder_front", 1, 24);

  for (const auto* stage : {&stage_120, &stage_122, &stage_124}) {
    expect(
        stage->starts_with("nvh264enc name=encoder_front "),
        "NVENC pipeline stage lost its selected factory or element name");
    for (const std::string_view property : {
             "bitrate=2500",
             "gop-size=30",
             "bframes=0",
             "zerolatency=true",
             "rc-lookahead=0",
             "rc-mode=cbr",
         }) {
      expect(
          stage->find(property) != std::string::npos,
          "NVENC pipeline stage lost common low-latency property " + std::string(property));
    }
  }

  expect(
      stage_120.find("preset=") == std::string::npos &&
          stage_120.find("tune=") == std::string::npos,
      "GStreamer 1.20 NVENC stage included unsupported preset or tune properties");
  expect(
      stage_122.find("preset=p1") != std::string::npos &&
          stage_122.find("tune=") == std::string::npos,
      "GStreamer 1.22 NVENC stage did not apply only the supported preset property");
  expect(
      stage_124.find("preset=p1") != std::string::npos &&
          stage_124.find("tune=ultra-low-latency") != std::string::npos,
      "GStreamer 1.24 NVENC stage did not apply preset and ultra-low-latency tune");
}

void test_native_testsrc_acquisition_does_not_spawn_ffmpeg() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  const auto camera = config.enabled_cameras().front();
  auto capture_profile = config.realtime_profile(camera.realtime_profile);
  capture_profile.codec = "mjpeg";
  capture_profile.encoder = "native";
  mine_teleop::CameraFrameSource source(camera, capture_profile);
  expect(source.command().empty(), "native test source unexpectedly configured an external media process");
  const auto frame = source.next(1);
  expect(frame.codec == "mjpeg", "native test source did not produce MJPEG");
  expect(frame.payload.size() > 100, "native test source produced an unexpectedly small JPEG");
  expect(
      static_cast<unsigned char>(frame.payload.front()) == 0xFF &&
          static_cast<unsigned char>(frame.payload.at(1)) == 0xD8 &&
          static_cast<unsigned char>(frame.payload.at(frame.payload.size() - 2)) == 0xFF &&
          static_cast<unsigned char>(frame.payload.back()) == 0xD9,
      "native test source payload is not a complete JPEG");
}

void test_basler_camera_uses_minimal_aravis_bridge() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto camera = config.enabled_cameras().front();
  camera.device = "basler:serial=25192546";
  auto capture_profile = config.realtime_profile(camera.realtime_profile);
  capture_profile.codec = "mjpeg";
  capture_profile.encoder = "native";
  mine_teleop::CameraFrameSource source(camera, capture_profile);
  const auto& command = source.command();
  expect(!command.empty(), "Basler camera did not configure an Aravis bridge");
  expect(
      std::filesystem::path(command.front()).filename() == "mine-teleop-aravis-camera",
      "Basler camera selected a non-Aravis bridge");
  expect(
      std::find(command.begin(), command.end(), "--serial") != command.end() &&
          std::find(command.begin(), command.end(), "25192546") != command.end(),
      "Aravis bridge did not preserve the Basler serial selector");
  expect(
      std::find(command.begin(), command.end(), "--jpeg-quality") != command.end(),
      "Aravis bridge did not receive the bounded JPEG quality setting");
}

void test_native_driver_to_vehicle_data_channel_payload() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "dev-device-secret"}};
  signaling_config.control_token_ttl_ms = 100;
  signaling_config.connection_reaper_interval_ms = 5;
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());

  auto vehicle_config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "dev-device-secret"},
       {"connection_id", "data-channel-test-vehicle"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
  auto driver_config = mine_teleop::load_driver_config("configs/driver-console.dev.yaml");
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connection = driver.connect();
  expect(connection.value("connected", false), "driver failed to connect");
  const auto reconnected = driver.connect();
  expect(
      reconnected.value("session_id", "") == connection.value("session_id", ""),
      "driver reconnect did not reuse the active session");
  const auto vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=dev-device-secret&connection_generation=" +
      std::to_string(vehicle_generation));
  const auto prepared = driver.send_control({{"gear", "D"}, {"steering", 0.1}, {"throttle", 0.2}, {"brake", 0.0}});
  expect(prepared.value("prepared", false), "driver did not prepare a DataChannel command");
  expect(prepared.value("transport", "") == "webrtc_data_channel", "driver selected the wrong control transport");
  const auto control = mine_teleop::ControlCommand::from_json(prepared.at("command"));
  expect(control.session_id == connection.value("session_id", ""), "DataChannel command used the wrong session");
  expect(
      control.control_token == vehicle_session.value("control_token", ""),
      "DataChannel command did not use the server-issued control token");
  mine_teleop::VehicleControlService receiver(
      vehicle_config,
      "driver-console-001",
      control.session_id,
      vehicle_session.at("control_token").get<std::string>(),
      std::make_unique<mine_teleop::MockVehicleAdapter>());
  const auto received_at_ms = mine_teleop::now_ms();
  receiver.start(received_at_ms);
  auto profile = session_profile_request(1, received_at_ms);
  profile.driver_id = "driver-console-001";
  profile.session_id = control.session_id;
  profile.control_token = vehicle_session.at("control_token").get<std::string>();
  expect(
      receiver.receive_session_profile(profile, received_at_ms).accepted,
      "vehicle receiver did not ACK the session profile before control");
  const auto applied = receiver.receive_command(control, received_at_ms);
  expect(applied.accepted, "vehicle receiver did not accept the DataChannel command payload");
  const auto duplicate = receiver.receive_command(control, received_at_ms + 1);
  expect(!duplicate.accepted && duplicate.reason == "old_seq", "vehicle receiver accepted a duplicate command");
  receiver.close();
  const auto expiry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  mine_teleop::Json after_expiry;
  do {
    after_expiry = http.get_json(base + "/health");
    if (after_expiry.value("active_sessions", 0) == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < expiry_deadline);
  expect(after_expiry.value("active_sessions", 0) == 0, "server did not expire the short-lived control session");
  const auto renewed_connection = driver.connect();
  expect(
      renewed_connection.value("session_id", "") != connection.value("session_id", ""),
      "driver console reused a server-expired local session");
  const auto disconnected = driver.disconnect("test_disconnect");
  expect(disconnected.value("state", "") == "offline", "driver console disconnect did not release authority");
  const auto released_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=dev-device-secret&connection_generation=" +
      std::to_string(vehicle_generation));
  expect(released_session.value("session_id", "").empty(), "vehicle still discovered a session after driver disconnect");
  server.stop();
}

void test_driver_login_lists_only_authorized_vehicles() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {
      {"driver-console-001", "dev-password"},
      {"driver-console-002", "other-password"},
  };
  signaling_config.device_tokens = {
      {"vehicle-001", "vehicle-secret-1"},
      {"vehicle-002", "vehicle-secret-2"},
  };
  signaling_config.driver_vehicle_permissions = {
      {"driver-console-001", {"vehicle-001"}},
      {"driver-console-002", {"vehicle-002"}},
  };
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "authorized-list-vehicle-1"}}));
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-002"},
       {"device_token", "vehicle-secret-2"},
       {"connection_id", "authorized-list-vehicle-2"}}));

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto login = driver.login("dev-password");
  expect(login.value("authenticated", false), "driver login did not authenticate");
  expect(!login.contains("token"), "local page response exposed the driver token");
  const auto vehicles = login.at("vehicles");
  expect(vehicles.size() == 1, "driver saw a vehicle outside its allowlist");
  expect(vehicles.at(0).value("vehicle_id", "") == "vehicle-001", "authorized vehicle list returned the wrong vehicle");
  expect(vehicles.at(0).value("online", false), "authorized online vehicle was reported offline");
  expect(vehicles.at(0).value("controllable", false), "authorized idle vehicle was not controllable");
  bool unauthorized_rejected = false;
  try {
    static_cast<void>(driver.connect("vehicle-002"));
  } catch (const std::invalid_argument&) {
    unauthorized_rejected = true;
  }
  expect(unauthorized_rejected, "driver selected a vehicle outside its allowlist");
  static_cast<void>(driver.disconnect("authorized_list_test"));
  server.stop();
}

void test_driver_console_page_keeps_waiting_state_during_background_safety_ticks() {
  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001";
  config.signaling_url = "http://127.0.0.1:1";
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "dev-password");
  mine_teleop::DriverConsoleHttpApp app(runtime);
  mine_teleop::HttpRequest request;
  request.method = "GET";
  request.path = "/";
  const auto response = app.handle(request);
  expect(response.status == 200, "driver console page did not load");
  expect(
      response.body.find("async function send(extra={},announceUnavailable=true)") != std::string::npos,
      "driver console page cannot distinguish background safety ticks from user control attempts");
  expect(
      response.body.find("function enqueueControlHeartbeat()") != std::string::npos &&
          response.body.find("pending = {extra: {}, announceUnavailable: false, waiters: []}") !=
              std::string::npos &&
          response.body.find("sendPendingControlProfile();enqueueControlHeartbeat()") !=
              std::string::npos,
      "background safety tick still announces a control fault while waiting for media");
  expect(
      response.body.find("webrtcLabel.textContent='等待车端媒体'") != std::string::npos,
      "driver console page does not expose the pending vehicle-media state");
}

void test_local_archive_uploader_is_atomic_and_resumable() {
  const auto root = std::filesystem::path("/tmp") / ("mine-teleop-upload-test-" + mine_teleop::random_token(6));
  const auto recordings = root / "recordings";
  const auto archive = root / "archive";
  const auto segment_dir = recordings / "vehicle-001" / "session-001" / "front";
  std::filesystem::create_directories(segment_dir);
  const auto video = segment_dir / "segment-001.mp4";
  const auto metadata = segment_dir / "segment-001.json";
  {
    std::ofstream output(video, std::ios::binary);
    output << "native-segment-payload";
  }
  {
    std::ofstream output(metadata);
    output << mine_teleop::Json({
        {"vehicle_id", "vehicle-001"},
        {"session_id", "session-001"},
        {"camera_id", "front"},
        {"segment_id", "segment-001"},
        {"upload_state", "pending"},
    }).dump();
  }
  mine_teleop::LocalArchiveUploader uploader(recordings, archive);
  const auto result = uploader.process_once();
  expect(result.action == "uploaded", "pending segment was not archived");
  expect(std::filesystem::is_regular_file(archive / result.object_path), "archived video is missing");
  expect(mine_teleop::sha256_file(video) == mine_teleop::sha256_file(archive / result.object_path), "archive hash mismatch");
  expect(uploader.process_once().action == "idle", "uploaded segment was processed twice");
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"config_loads_current_vehicle_yaml", test_config_loads_current_vehicle_yaml},
      {"vehicle_config_rejects_unimplemented_control_safety_options", test_vehicle_config_rejects_unimplemented_control_safety_options},
      {"vehicle_config_validates_full_scale_motor_torque", test_vehicle_config_validates_full_scale_motor_torque},
      {"vehicle_config_requires_physical_brake_pressure_units", test_vehicle_config_requires_physical_brake_pressure_units},
      {"vehicle_config_requires_monotonic_final_full_safety_brake", test_vehicle_config_requires_monotonic_final_full_safety_brake},
      {"vehicle_config_validates_local_speed_pid_safety_fields", test_vehicle_config_validates_local_speed_pid_safety_fields},
      {"vehicle_camera_recovery_config_defaults_explicit_values_and_boundaries", test_vehicle_camera_recovery_config_defaults_explicit_values_and_boundaries},
      {"control_enabled_vehicle_requires_an_enabled_critical_camera", test_control_enabled_vehicle_requires_an_enabled_critical_camera},
      {"camera_failure_decision_is_bounded_and_fail_closed", test_camera_failure_decision_is_bounded_and_fail_closed},
      {"camera_source_classification_is_canonical", test_camera_source_classification_is_canonical},
      {"camera_input_spec_is_explicit_for_ccg2_and_legacy_safe", test_camera_input_spec_is_explicit_for_ccg2_and_legacy_safe},
      {"camera_backend_config_rejects_unknown_and_invalid_ccg2_modes", test_camera_backend_config_rejects_unknown_and_invalid_ccg2_modes},
      {"ccg2_uyvy_row_packing_handles_stride_and_rejects_invalid_frames", test_ccg2_uyvy_row_packing_handles_stride_and_rejects_invalid_frames},
      {"camera_input_pipeline_keeps_legacy_jpeg_and_adds_raw_ccg2", test_camera_input_pipeline_keeps_legacy_jpeg_and_adds_raw_ccg2},
      {"camera_input_pipeline_resamples_only_mismatched_ccg2_fps", test_camera_input_pipeline_resamples_only_mismatched_ccg2_fps},
      {"ccg2_camera_input_pipeline_is_gstreamer_parseable", test_ccg2_camera_input_pipeline_is_gstreamer_parseable},
      {"v4l2_sequence_gap_handles_first_consecutive_missing_and_wrap", test_v4l2_sequence_gap_handles_first_consecutive_missing_and_wrap},
      {"camera_issue_classification_distinguishes_ccg2_fps_and_buffer_faults", test_camera_issue_classification_distinguishes_ccg2_fps_and_buffer_faults},
      {"ccg2_example_config_defines_two_explicit_capture_lanes", test_ccg2_example_config_defines_two_explicit_capture_lanes},
      {"missing_v4l2_path_remains_retryable", test_missing_v4l2_path_remains_retryable},
      {"media_signaling_sequence_is_monotonic_within_scope_and_resets_between_scopes", test_media_signaling_sequence_is_monotonic_within_scope_and_resets_between_scopes},
      {"critical_camera_control_latch_persists_until_a_new_session", test_critical_camera_control_latch_persists_until_a_new_session},
      {"media_signaling_error_classification_supports_structured_and_legacy_conflicts", test_media_signaling_error_classification_supports_structured_and_legacy_conflicts},
      {"vehicle_config_validates_chassis_control_speed_range", test_vehicle_config_validates_chassis_control_speed_range},
      {"dynamic_adapter_target_speed_uses_configured_ceiling", test_dynamic_adapter_target_speed_uses_configured_ceiling},
      {"dynamic_adapter_brake_overrides_throttle", test_dynamic_adapter_brake_overrides_throttle},
      {"bench_config_drives_unified_vehicle_runtime", test_bench_config_drives_unified_vehicle_runtime},
      {"field_config_pins_tls_route_without_system_dns", test_field_config_pins_tls_route_without_system_dns},
      {"control_command_json_round_trip_and_validation", test_control_command_json_round_trip_and_validation},
      {"session_control_profile_json_round_trip_and_physical_units", test_session_control_profile_json_round_trip_and_physical_units},
      {"shared_protocol_v1_vectors_and_session_states", test_shared_protocol_v1_vectors_and_session_states},
      {"control_receiver_enforces_token_sequence_and_gap", test_control_receiver_enforces_token_sequence_and_gap},
      {"mailbox_keeps_only_latest_command", test_mailbox_keeps_only_latest_command},
      {"safety_timeout_profile_and_estop_latch", test_safety_timeout_profile_and_estop_latch},
      {"session_control_profile_ack_sequence_limits_and_clear", test_session_control_profile_ack_sequence_limits_and_clear},
      {"session_control_profile_uses_independent_two_second_age_window", test_session_control_profile_uses_independent_two_second_age_window},
      {"real_adapter_profile_changes_require_parking_and_apply_before_ack", test_real_adapter_profile_changes_require_parking_and_apply_before_ack},
      {"control_service_commits_only_successfully_applied_commands",
       test_control_service_commits_only_successfully_applied_commands},
      {"control_service_reports_safe_stop_output_after_timeout", test_control_service_reports_safe_stop_output_after_timeout},
      {"control_service_recovers_from_degraded_command_gap_without_profile_reapply",
       test_control_service_recovers_from_degraded_command_gap_without_profile_reapply},
      {"control_service_receive_path_cannot_bypass_hard_timeout",
       test_control_service_receive_path_cannot_bypass_hard_timeout},
      {"control_service_preserves_physical_brake_across_degraded_timeout", test_control_service_preserves_physical_brake_across_degraded_timeout},
      {"control_service_defers_to_adapter_owned_safe_stop_until_fresh_handshake",
       test_control_service_defers_to_adapter_owned_safe_stop_until_fresh_handshake},
      {"adapter_handshake_does_not_clear_outer_estop_or_fault",
       test_adapter_handshake_does_not_clear_outer_estop_or_fault},
      {"reset_estop_rejects_unreadable_disarming_and_hard_adapter_stops",
       test_reset_estop_rejects_unreadable_disarming_and_hard_adapter_stops},
      {"reset_estop_clears_soft_stop_before_fresh_control",
       test_reset_estop_clears_soft_stop_before_fresh_control},
      {"control_service_applies_vehicle_hard_limits", test_control_service_applies_vehicle_hard_limits},
      {"control_service_applies_session_steering_limit", test_control_service_applies_session_steering_limit},
      {"control_service_bounds_telemetry_history", test_control_service_bounds_telemetry_history},
      {"control_service_close_preserves_stop_provenance",
       test_control_service_close_preserves_stop_provenance},
      {"control_service_requires_feedback_before_actuation_but_allows_estop", test_control_service_requires_feedback_before_actuation_but_allows_estop},
      {"fault_output_fails_safe", test_fault_output_fails_safe},
      {"native_signaling_webrtc_message_isolation", test_native_signaling_webrtc_message_isolation},
      {"signaling_presence_generation_and_automatic_release", test_signaling_presence_generation_and_automatic_release},
      {"signaling_time_sync_common_domain", test_signaling_time_sync_common_domain},
      {"driver_config_and_hardware_encoder_priority", test_driver_config_and_hardware_encoder_priority},
      {"nvenc_pipeline_stage_tracks_gstreamer_property_compatibility", test_nvenc_pipeline_stage_tracks_gstreamer_property_compatibility},
      {"native_testsrc_acquisition_does_not_spawn_ffmpeg", test_native_testsrc_acquisition_does_not_spawn_ffmpeg},
      {"basler_camera_uses_minimal_aravis_bridge", test_basler_camera_uses_minimal_aravis_bridge},
      {"native_driver_to_vehicle_data_channel_payload", test_native_driver_to_vehicle_data_channel_payload},
      {"driver_console_page_keeps_waiting_state_during_background_safety_ticks", test_driver_console_page_keeps_waiting_state_during_background_safety_ticks},
      {"driver_login_lists_only_authorized_vehicles", test_driver_login_lists_only_authorized_vehicles},
      {"local_archive_uploader_is_atomic_and_resumable", test_local_archive_uploader_is_atomic_and_resumable},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << "SUMMARY passed=" << (tests.size() - static_cast<std::size_t>(failures))
            << " failed=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
