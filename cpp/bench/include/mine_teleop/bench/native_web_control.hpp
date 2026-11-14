#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace mine_teleop::detail {

// The same normalized control contract as control_logic.js::deriveControl.
// This computes an intent only; authority and input leases are checked by the caller.
struct NativeWebControl {
  struct Keys { bool up{}, down{}, left{}, right{}, service_brake{}, hard_brake{}; };
  struct Gamepad { double steering{}, throttle{}, brake{}; };
  struct Limits { double throttle{}, brake{}, service_brake{}, hard_brake{}, steering{}; };
  struct Result { char gear{'N'}; double steering{}, throttle{}, brake{}; bool estop{}; };

  static Gamepad standard_gamepad(double axis, double throttle, double brake, double center,
                                  double range, double deadzone, bool inverted) {
    if (!std::isfinite(axis) || !std::isfinite(throttle) || !std::isfinite(brake) ||
        !std::isfinite(center) || !std::isfinite(range) || range <= 0 ||
        !std::isfinite(deadzone) || deadzone < 0 || deadzone >= 1) return {};
    double steering = (axis - center) / range;
    if (inverted) steering = -steering;
    const auto deadzone_axis = [deadzone](double value) {
      const double magnitude = std::abs(value);
      return magnitude <= deadzone ? 0.0 : std::copysign((magnitude - deadzone) / (1 - deadzone), value);
    };
    const auto deadzone_pedal = [deadzone](double value) {
      return value <= deadzone ? 0.0 : (value - deadzone) / (1 - deadzone);
    };
    return {std::clamp(deadzone_axis(steering), -1.0, 1.0),
            std::clamp(deadzone_pedal(throttle), 0.0, 1.0),
            std::clamp(deadzone_pedal(brake), 0.0, 1.0)};
  }

  static Result derive(Keys keys, Gamepad pad, char selected_gear, Limits limits,
                       double steering_scale, bool estop = false) {
    const auto finite = [](double value) { return std::isfinite(value) ? value : 0.0; };
    const double max_throttle = std::clamp(finite(limits.throttle), 0.0, 1.0);
    const double max_brake = std::clamp(finite(limits.brake), 0.0, 327.6);
    const double hard = std::clamp(finite(limits.hard_brake), 0.0, max_brake);
    const double service = std::clamp(finite(limits.service_brake), 0.0, hard);
    const double max_steering = std::max(0.0, finite(limits.steering));
    const double scale = std::max(std::numeric_limits<double>::epsilon(),
        std::isfinite(steering_scale) ? steering_scale : 1.0);
    const char gear = selected_gear == 'D' || selected_gear == 'R' ? selected_gear : 'N';
    double steering = finite(pad.steering);
    if (keys.left || keys.right) steering = keys.left == keys.right ? 0.0 : keys.left ? -1.0 : 1.0;
    double throttle = std::clamp(finite(pad.throttle), 0.0, 1.0);
    if (keys.up != keys.down) throttle = (keys.up ? 'D' : 'R') == gear ? 1.0 : 0.0;
    else if (keys.up && keys.down) throttle = 0.0;
    if (gear == 'N') throttle = 0.0;
    const double pedal = std::clamp(finite(pad.brake), 0.0, 1.0);
    double brake = max_brake > 0 ? pedal : 0.0;
    if (keys.service_brake && max_brake > 0) brake = std::max(brake, service / max_brake);
    if (keys.hard_brake && max_brake > 0) brake = std::max(brake, hard / max_brake);
    if (keys.service_brake || keys.hard_brake || pedal > 0) throttle = 0.0;
    return {gear, std::clamp(steering, -1.0, 1.0) * (max_steering / scale),
            throttle * max_throttle, std::clamp(brake, 0.0, max_brake > 0 ? 1.0 : 0.0), estop};
  }
};

}  // namespace mine_teleop::detail
