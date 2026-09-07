#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>

namespace mine_teleop::control_limits {

// These are numerical/units rules only. Protocol authorization, vehicle
// state, and C ABI shape validation deliberately remain at their own layers.
inline constexpr double kKilometersPerHourPerMeterPerSecond = 3.6;
inline constexpr double kChassisControlMaxTargetSpeedMps = 20.0;
inline constexpr double kChassisControlMaxTargetSpeedKph =
    kChassisControlMaxTargetSpeedMps * kKilometersPerHourPerMeterPerSecond;
inline constexpr double kMaxSteeringAngleDeg = 30.0;
inline constexpr double kMaxNormalizedSteeringRequest = 1.0;
inline constexpr std::size_t kSteeringAxisCount = 4U;

inline constexpr double kDefaultFullScaleMotorTorqueNm = 300.0;
inline constexpr double kMaxFullScaleMotorTorqueNm = 640.0;
inline constexpr double kDefaultMotorTorqueRiseRateNmPerSecond = 0.0;
inline constexpr double kMaxMotorTorqueRiseRateNmPerSecond = 32000.0;
inline constexpr double kDefaultMaxBrakePressureBar = 100.0;
inline constexpr double kMaxOrdinaryBrakePressureBar = 327.6;
inline constexpr double kMaxEmergencyBrakePressureBar = 409.5;
inline constexpr double kBrakePressureResolutionBar = 0.1;

inline constexpr int kMinSpeedFeedbackTimeoutMs = 20;
inline constexpr int kMaxSpeedFeedbackTimeoutMs = 500;
inline constexpr int kMinSpeedPidMaxDtMs = 20;
inline constexpr int kMaxSpeedPidMaxDtMs = 200;
inline constexpr double kMaxSpeedPidGain = 100.0;
inline constexpr double kMaxSpeedPidDerivativeFilterTauMs = 2000.0;
inline constexpr double kMaxHardOverspeedMarginMps = 10.0;
inline constexpr double kMaxHardOverspeedMarginKph =
    kMaxHardOverspeedMarginMps * kKilometersPerHourPerMeterPerSecond;

[[nodiscard]] constexpr double meters_per_second_to_kilometers_per_hour(
    double meters_per_second) noexcept {
  return meters_per_second * kKilometersPerHourPerMeterPerSecond;
}

[[nodiscard]] constexpr double kilometers_per_hour_to_meters_per_second(
    double kilometers_per_hour) noexcept {
  return kilometers_per_hour / kKilometersPerHourPerMeterPerSecond;
}

[[nodiscard]] constexpr double steering_degrees_to_normalized_request(
    double steering_angle_deg) noexcept {
  return steering_angle_deg / kMaxSteeringAngleDeg;
}

[[nodiscard]] constexpr double normalized_steering_request_to_degrees(
    double normalized_request) noexcept {
  return normalized_request * kMaxSteeringAngleDeg;
}

[[nodiscard]] inline bool is_finite_inclusive(
    double value,
    double minimum,
    double maximum) noexcept {
  return std::isfinite(value) && std::isfinite(minimum) &&
      std::isfinite(maximum) && minimum <= maximum && value >= minimum &&
      value <= maximum;
}

// Hard caps are exact. Callers must not add an epsilon to a physical or
// protocol limit before using this predicate.
[[nodiscard]] inline bool exceeds_hard_limit(
    double value,
    double maximum) noexcept {
  return !std::isfinite(value) || !std::isfinite(maximum) || value > maximum;
}

[[nodiscard]] constexpr std::size_t bounded_steering_axis_count(
    int requested_axis_count) noexcept {
  if (requested_axis_count <= 0) return 0U;
  const auto requested = static_cast<std::size_t>(requested_axis_count);
  return std::min(requested, kSteeringAxisCount);
}

[[nodiscard]] constexpr std::array<double, kSteeringAxisCount>
broadcast_steering_request(double normalized_request) noexcept {
  std::array<double, kSteeringAxisCount> values{};
  for (auto& value : values) value = normalized_request;
  return values;
}

// DBC brake pressure is Q0.1 bar. Keep the historical toward-zero rounding:
// emitted ordinary brake pressure must never exceed its accepted input.
[[nodiscard]] inline double quantize_ordinary_brake_pressure_bar_toward_zero(
    double brake_pressure_bar) noexcept {
  if (!(brake_pressure_bar > 0.0)) return 0.0;
  const double bounded = std::min(
      brake_pressure_bar,
      kMaxOrdinaryBrakePressureBar);
  return std::floor(std::nextafter(
                        bounded / kBrakePressureResolutionBar,
                        std::numeric_limits<double>::max())) *
      kBrakePressureResolutionBar;
}

}  // namespace mine_teleop::control_limits
