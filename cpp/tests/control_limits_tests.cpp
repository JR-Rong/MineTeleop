#include "mine_teleop/control_limits.hpp"
#include "mine_teleop/core.hpp"
#include "mine_teleop_chassis_bridge.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace {

namespace limits = mine_teleop::control_limits;

static_assert(
    limits::kMaxFullScaleMotorTorqueNm ==
    MINE_TELEOP_CHASSIS_MAX_FULL_SCALE_MOTOR_TORQUE_NM);
static_assert(
    limits::kMaxOrdinaryBrakePressureBar ==
    MINE_TELEOP_CHASSIS_MAX_ORDINARY_BRAKE_PRESSURE_BAR);
static_assert(
    limits::kBrakePressureResolutionBar ==
    MINE_TELEOP_CHASSIS_BRAKE_PRESSURE_RESOLUTION_BAR);
static_assert(
    limits::kMaxMotorTorqueRiseRateNmPerSecond ==
    MINE_TELEOP_CHASSIS_MAX_MOTOR_TORQUE_RISE_RATE_NM_PER_SECOND);
static_assert(
    limits::kMinSpeedFeedbackTimeoutMs ==
    MINE_TELEOP_CHASSIS_MIN_SPEED_FEEDBACK_TIMEOUT_MS);
static_assert(
    limits::kMaxSpeedFeedbackTimeoutMs ==
    MINE_TELEOP_CHASSIS_MAX_SPEED_FEEDBACK_TIMEOUT_MS);
static_assert(limits::kMinSpeedPidMaxDtMs == MINE_TELEOP_CHASSIS_MIN_SPEED_PID_MAX_DT_MS);
static_assert(limits::kMaxSpeedPidMaxDtMs == MINE_TELEOP_CHASSIS_MAX_SPEED_PID_MAX_DT_MS);
static_assert(limits::kMaxSpeedPidGain == MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN);
static_assert(
    limits::kMaxSpeedPidDerivativeFilterTauMs ==
    MINE_TELEOP_CHASSIS_MAX_DERIVATIVE_FILTER_TAU_MS);
static_assert(
    limits::kMaxHardOverspeedMarginMps ==
    MINE_TELEOP_CHASSIS_MAX_HARD_OVERSPEED_MARGIN_MPS);
static_assert(limits::kMaxNormalizedSteeringRequest == MINE_TELEOP_CHASSIS_MAX_STEERING_REQUEST);
static_assert(
    std::extent_v<decltype(MineTeleopChassisFeedback::eps_angle)> ==
    limits::kSteeringAxisCount);
static_assert(
    std::extent_v<decltype(MineTeleopChassisCanFeedbackV1::steering_angle_deg)> ==
    limits::kSteeringAxisCount);

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void expect_near(
    double actual,
    double expected,
    const std::string& message,
    double tolerance = 1e-12) {
  if (std::abs(actual - expected) > tolerance) {
    throw std::runtime_error(
        message + ": expected " + std::to_string(expected) + ", got " +
        std::to_string(actual));
  }
}

template <typename Callable>
void expect_throws(Callable&& callable, const std::string& message) {
  try {
    callable();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(message);
}

mine_teleop::SessionControlProfile valid_profile() {
  mine_teleop::SessionControlProfile profile;
  profile.target_speed_kph = 10.0;
  profile.max_motor_torque_nm = 100.0;
  profile.max_brake_pressure_bar = 100.0;
  profile.service_brake_pressure_bar = 25.0;
  profile.hard_brake_pressure_bar = 75.0;
  profile.max_steering_angle_deg = 20.0;
  return profile;
}

mine_teleop::SessionControlProfileRequest profile_request(
    std::uint64_t sequence,
    mine_teleop::SessionControlProfile profile) {
  mine_teleop::SessionControlProfileRequest request;
  request.vehicle_id = "vehicle-r10";
  request.driver_id = "driver-r10";
  request.session_id = "session-r10";
  request.seq = sequence;
  request.sent_at_utc_ms = 1'000;
  request.control_token = "r10-control-token";
  request.profile = std::move(profile);
  return request;
}

mine_teleop::VehicleConfig vehicle_config() {
  mine_teleop::VehicleConfig config;
  config.vehicle_id = "vehicle-r10";
  config.control.deceleration_profile = {{0, 1.0}};
  config.field_safety.commissioning_mode = "bench";
  config.field_safety.max_speed_kph = 10.0;
  config.field_safety.max_throttle = 1.0;
  config.field_safety.full_scale_motor_torque_nm = 100.0;
  config.field_safety.max_brake_pressure_bar = 100.0;
  config.field_safety.max_steering_angle_deg = 20.0;
  return config;
}

void test_units_and_axis_helpers() {
  expect_near(
      limits::meters_per_second_to_kilometers_per_hour(
          limits::kChassisControlMaxTargetSpeedMps),
      limits::kChassisControlMaxTargetSpeedKph,
      "m/s to km/h changed target-speed semantics");
  expect_near(
      limits::kilometers_per_hour_to_meters_per_second(
          limits::kChassisControlMaxTargetSpeedKph),
      limits::kChassisControlMaxTargetSpeedMps,
      "km/h to m/s changed target-speed semantics");
  expect_near(
      limits::steering_degrees_to_normalized_request(
          limits::kMaxSteeringAngleDeg),
      limits::kMaxNormalizedSteeringRequest,
      "maximum steering degrees did not map to the normalized maximum");
  expect_near(
      limits::normalized_steering_request_to_degrees(
          -limits::kMaxNormalizedSteeringRequest),
      -limits::kMaxSteeringAngleDeg,
      "negative normalized steering did not preserve direction");

  const auto axes = limits::broadcast_steering_request(-0.25);
  for (const auto value : axes) {
    expect_near(value, -0.25, "four-axis broadcast did not preserve each axis value");
  }
  expect(
      limits::bounded_steering_axis_count(-1) == 0U &&
          limits::bounded_steering_axis_count(0) == 0U &&
          limits::bounded_steering_axis_count(1) == 1U &&
          limits::bounded_steering_axis_count(4) == limits::kSteeringAxisCount &&
          limits::bounded_steering_axis_count(5) == limits::kSteeringAxisCount,
      "steering axis count was not bounded to the fixed four-axis ABI");
}

void test_strict_finite_boundaries_and_quantization() {
  const double maximum = limits::kMaxOrdinaryBrakePressureBar;
  expect(limits::is_finite_inclusive(0.0, 0.0, maximum), "zero was rejected");
  expect(limits::is_finite_inclusive(maximum, 0.0, maximum), "exact cap was rejected");
  expect(
      !limits::is_finite_inclusive(
          std::nextafter(maximum, std::numeric_limits<double>::infinity()),
          0.0,
          maximum),
      "next representable value above a hard cap was accepted");
  expect(
      !limits::is_finite_inclusive(
          std::nextafter(0.0, -std::numeric_limits<double>::infinity()),
          0.0,
          maximum),
      "negative adjacent value was accepted");
  expect(
      !limits::is_finite_inclusive(
          std::numeric_limits<double>::quiet_NaN(), 0.0, maximum) &&
          !limits::is_finite_inclusive(
              std::numeric_limits<double>::infinity(), 0.0, maximum) &&
          !limits::is_finite_inclusive(
              -std::numeric_limits<double>::infinity(), 0.0, maximum),
      "non-finite values were accepted by a numerical rule");

  const std::array<double, 9> pressures{
      std::numeric_limits<double>::quiet_NaN(),
      std::numeric_limits<double>::infinity(),
      -0.1,
      0.0,
      0.099,
      0.1,
      10.19,
      maximum,
      std::nextafter(maximum, std::numeric_limits<double>::infinity()),
  };
  for (const auto pressure : pressures) {
    const double cpp_quantized =
        limits::quantize_ordinary_brake_pressure_bar_toward_zero(pressure);
    expect(
        cpp_quantized == mine_teleop_chassis_quantize_brake_pressure_bar(pressure),
        "C++ and C ABI brake pressure quantization drifted");
    expect(
        cpp_quantized >= 0.0 && cpp_quantized <= maximum,
        "brake pressure quantization escaped the ordinary pressure range");
    if (std::isfinite(pressure) && pressure > 0.0 && pressure <= maximum) {
      expect(
          cpp_quantized <= pressure,
          "brake pressure quantization rounded away from zero");
    }
  }
  expect_near(
      limits::quantize_ordinary_brake_pressure_bar_toward_zero(10.19),
      10.1,
      "brake pressure quantization did not floor to the DBC resolution");

  MineTeleopChassisSpeedPidConfig pid{
      MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN,
      MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN,
      MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN,
      MINE_TELEOP_CHASSIS_MAX_DERIVATIVE_FILTER_TAU_MS,
      MINE_TELEOP_CHASSIS_MAX_SPEED_PID_MAX_DT_MS};
  expect(
      mine_teleop_chassis_speed_pid_config_is_valid(&pid) != 0,
      "C ABI accepted cap was rejected");
  pid.kp = std::nextafter(
      MINE_TELEOP_CHASSIS_MAX_SPEED_PID_GAIN,
      std::numeric_limits<double>::infinity());
  expect(
      mine_teleop_chassis_speed_pid_config_is_valid(&pid) == 0,
      "C ABI accepted a next-representable PID value above its cap");
}

void test_protocol_and_vehicle_limits_are_independent() {
  auto global_cap = valid_profile();
  global_cap.target_speed_kph = limits::kChassisControlMaxTargetSpeedKph;
  global_cap.validate();
  global_cap.target_speed_kph = std::nextafter(
      limits::kChassisControlMaxTargetSpeedKph,
      std::numeric_limits<double>::infinity());
  expect_throws(
      [&] { global_cap.validate(); },
      "protocol profile accepted a next-representable target speed above its cap");

  for (const double invalid_value : {
           std::numeric_limits<double>::quiet_NaN(),
           std::numeric_limits<double>::infinity(),
           -1.0}) {
    auto profile = valid_profile();
    profile.target_speed_kph = invalid_value;
    expect_throws(
        [&] { profile.validate(); },
        "protocol profile accepted a non-finite or negative target speed");
  }

  mine_teleop::VehicleControlService service(
      vehicle_config(),
      "driver-r10",
      "session-r10",
      "r10-control-token",
      std::make_unique<mine_teleop::MockVehicleAdapter>());
  service.start(1'000);

  auto exact = profile_request(1, valid_profile());
  const auto exact_result = service.receive_session_profile(exact, 1'000);
  expect(exact_result.accepted, "vehicle ceiling rejected an exact profile limit");

  struct VehicleLimitCase {
    const char* expected_reason;
    void (*raise)(mine_teleop::SessionControlProfile&);
  };
  const std::array<VehicleLimitCase, 4> cases{{
      {"target_speed_exceeds_vehicle_limit", [](mine_teleop::SessionControlProfile& profile) {
         profile.target_speed_kph = std::nextafter(
             10.0, std::numeric_limits<double>::infinity());
       }},
      {"motor_torque_exceeds_vehicle_limit", [](mine_teleop::SessionControlProfile& profile) {
         profile.max_motor_torque_nm = std::nextafter(
             100.0, std::numeric_limits<double>::infinity());
       }},
      {"brake_pressure_exceeds_vehicle_limit", [](mine_teleop::SessionControlProfile& profile) {
         profile.max_brake_pressure_bar = std::nextafter(
             100.0, std::numeric_limits<double>::infinity());
       }},
      {"steering_exceeds_vehicle_limit", [](mine_teleop::SessionControlProfile& profile) {
         profile.max_steering_angle_deg = std::nextafter(
             20.0, std::numeric_limits<double>::infinity());
       }},
  }};
  std::uint64_t sequence = 2;
  for (const auto& item : cases) {
    auto profile = valid_profile();
    item.raise(profile);
    profile.validate();
    const auto result = service.receive_session_profile(
        profile_request(sequence++, std::move(profile)),
        1'000);
    expect(
        !result.accepted && result.reason == item.expected_reason,
        std::string("vehicle layer did not independently reject ") +
            item.expected_reason);
  }
  service.close();
}

}  // namespace

int main() {
  try {
    test_units_and_axis_helpers();
    test_strict_finite_boundaries_and_quantization();
    test_protocol_and_vehicle_limits_are_independent();
    std::cout << "control_limits_tests=passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "control_limits_tests=failed error=" << error.what() << '\n';
    return 1;
  }
}
