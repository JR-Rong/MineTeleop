#pragma once

#include <cstdint>
#include <chrono>
#include <cmath>
#include <mutex>
#include <limits>
#include <stdexcept>
#include "mine_teleop/detail/native_web_control.hpp"

namespace mine_teleop::detail {

// A fresh observation cannot come from the future or survive the input lease.
inline bool native_mock_input_fresh(std::int64_t observed_ms, std::int64_t now_ms) {
  return observed_ms > 0 && now_ms >= observed_ms && now_ms - observed_ms < 200;
}

inline bool native_mock_clock_fresh(std::int64_t observed_ms, std::int64_t now_ms) {
  return observed_ms > 0 && now_ms >= observed_ms && now_ms - observed_ms < 500;
}

// Both endpoints synchronize independently to the same signaling clock. Use
// their combined uncertainty conservatively; receipt time alone cannot prove
// a delayed gear report is current.
inline bool native_mock_source_fresh(std::int64_t source_ms, std::int64_t wall_ms,
                                     std::int64_t offset_ms, std::int64_t uncertainty_ms) {
  if (source_ms <= 0 || wall_ms <= 0 || uncertainty_ms < 0 || uncertainty_ms > 25 ||
      (offset_ms > 0 && wall_ms > std::numeric_limits<std::int64_t>::max() - offset_ms) ||
      (offset_ms < 0 && wall_ms < std::numeric_limits<std::int64_t>::min() - offset_ms)) return false;
  const auto now_ms = wall_ms + offset_ms;
  if (now_ms <= 0) return false;
  // The vehicle's existing admission gate caps its independent error at 25 ms.
  const auto uncertainty = uncertainty_ms + 25;
  if (source_ms > now_ms) return source_ms - now_ms <= uncertainty;
  return now_ms - source_ms < 200 - uncertainty;
}

// The UI thread supplies a foreground keyboard heartbeat. This state is only
// used by the isolated, zero-speed mock viewer; it cannot authorize motion.
class NativeMockKeyboard {
 public:
  struct Keys { bool forward{}, left{}, right{}, brake{}, hard_brake{}, reverse{}; };
  struct Gamepad { bool connected{}; double steering{}, throttle{}, brake{}; unsigned index{}; };
  struct Input { double throttle{}, steering{}, brake{}; char gear{'N'}; std::uint8_t key_mask{}; bool armed{}; };

  void set_control_limits(double throttle, double steering_deg, double steering_scale_deg) {
    if (!std::isfinite(throttle) || throttle < 0 || throttle > 1 ||
        !std::isfinite(steering_deg) || steering_deg < 0 ||
        !std::isfinite(steering_scale_deg) || steering_scale_deg <= 0)
      throw std::invalid_argument("invalid confirmed control limits");
    std::lock_guard lock(mutex_);
    if (throttle_limit_ != throttle || steering_limit_ != steering_deg || steering_scale_ != steering_scale_deg) {
      throttle_limit_ = throttle; steering_limit_ = steering_deg; steering_scale_ = steering_scale_deg;
      needs_release_ = true;
      gamepad_needs_neutral_ = true;
    }
  }

  // Match the confirmed vehicle profile rather than assuming a fixed pressure.
  // A changed profile invalidates held input until the operator releases it.
  void set_brake_profile(double service, double hard, double maximum) {
    if (!std::isfinite(service) || !std::isfinite(hard) || !std::isfinite(maximum) ||
        service < 0 || hard < service || maximum < hard)
      throw std::invalid_argument("invalid confirmed brake profile");
    const double service_ratio = maximum > 0 ? service / maximum : 0;
    const double hard_ratio = maximum > 0 ? hard / maximum : 0;
    std::lock_guard lock(mutex_);
    if (!profile_ready_ || service != service_pressure_ || hard != hard_pressure_ || maximum != maximum_pressure_) {
      service_brake_ = service_ratio;
      hard_brake_ = hard_ratio;
      service_pressure_ = service; hard_pressure_ = hard; maximum_pressure_ = maximum;
      brake_available_ = maximum > 0;
      profile_ready_ = true;
      needs_release_ = true;
      gamepad_needs_neutral_ = true;
    }
  }

  void observe(bool focused, Keys keys, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    observe_locked(focused, keys, {}, now_ms);
  }

  void observe(bool focused, Keys keys, Gamepad gamepad, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    observe_locked(focused, keys, gamepad, now_ms);
  }

  void withdraw() {
    std::lock_guard lock(mutex_);
    focused_ = false;
    observed_at_ms_ = 0;
    keys_ = {};
    gamepad_ = {};
    needs_release_ = true;
    gamepad_needs_neutral_ = true;
  }

  bool heartbeat_alive(std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    return heartbeat_alive_locked(now_ms);
  }

  bool heartbeat_alive_now() {
    std::lock_guard lock(mutex_);
    return heartbeat_alive_locked(now());
  }

  Input sample(bool safety_ready, std::int64_t now_ms, char confirmed_gear = 'N') {
    std::lock_guard lock(mutex_);
    return sample_locked(safety_ready, now_ms, confirmed_gear);
  }

  Input sample_now(bool safety_ready, char confirmed_gear = 'N') {
    std::lock_guard lock(mutex_);
    return sample_locked(safety_ready, now(), confirmed_gear);
  }
 private:
  static std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void observe_locked(bool focused, Keys keys, Gamepad gamepad, std::int64_t now_ms) {
    focused_ = focused && now_ms > 0;
    keys_ = focused_ ? keys : Keys{};
    if (!focused_ || gamepad.connected != gamepad_.connected ||
        (gamepad.connected && gamepad.index != gamepad_.index)) gamepad_needs_neutral_ = true;
    gamepad_ = focused_ ? gamepad : Gamepad{};
    observed_at_ms_ = focused_ ? now_ms : 0;
    if (!focused_) { needs_release_ = true; gamepad_needs_neutral_ = true; }
  }

  bool heartbeat_alive_locked(std::int64_t now_ms) const {
    return focused_ && observed_at_ms_ > 0 && now_ms >= observed_at_ms_ &&
        now_ms - observed_at_ms_ < 150;
  }

  Input sample_locked(bool safety_ready, std::int64_t now_ms, char confirmed_gear) {
    if (!safety_ready || !profile_ready_ || !focused_ || observed_at_ms_ <= 0 || now_ms < observed_at_ms_ ||
        now_ms - observed_at_ms_ >= 150 || (confirmed_gear != 'N' && confirmed_gear != 'D' && confirmed_gear != 'R')) {
      needs_release_ = true;
      gamepad_needs_neutral_ = true;
      return {};
    }
    const bool any = keys_.forward || keys_.reverse || keys_.left || keys_.right || keys_.brake || keys_.hard_brake;
    if (needs_release_) {
      if (!any) needs_release_ = false;
      return {};
    }
    NativeWebControl::Gamepad pad{};
    if (gamepad_.connected) {
      if (gamepad_needs_neutral_) {
        if (gamepad_.throttle == 0 && gamepad_.brake == 0) gamepad_needs_neutral_ = false;
      } else pad = {gamepad_.steering, gamepad_.throttle, gamepad_.brake};
    }
    // Ask for a gear change with no traction or steering, and wait for fresh
    // vehicle telemetry to confirm it before applying the held direction.
    const bool braking = keys_.brake || keys_.hard_brake || pad.brake > 0;
    const auto key_mask = static_cast<std::uint8_t>((keys_.forward ? 1 : 0) | (keys_.reverse ? 2 : 0) |
        (keys_.left ? 4 : 0) | (keys_.right ? 8 : 0) | (keys_.brake ? 16 : 0) | (keys_.hard_brake ? 32 : 0));
    const char requested = keys_.forward != keys_.reverse ? (keys_.forward ? 'D' : 'R') :
        (!keys_.forward && !keys_.reverse && pad.throttle > 0 && confirmed_gear == 'N' ? 'D' : confirmed_gear);
    if (requested != confirmed_gear) gamepad_needs_neutral_ = true;
    if (!braking && requested != confirmed_gear) return {0.0, 0.0, 0.0, requested, key_mask, true};
    // The web contract scales both axes by the acknowledged effective profile.
    // Normalized brake ratios are passed as pressures against a unit maximum.
    const auto result = NativeWebControl::derive(
        {keys_.forward, keys_.reverse, keys_.left, keys_.right, keys_.brake, keys_.hard_brake},
        pad, confirmed_gear, {throttle_limit_, brake_available_ ? 1.0 : 0.0,
            service_brake_, hard_brake_, steering_limit_}, steering_scale_);
    return {result.throttle, result.steering, result.brake, result.gear, key_mask, true};
  }
  std::mutex mutex_;
  Keys keys_{};
  Gamepad gamepad_{};
  std::int64_t observed_at_ms_{};
  bool focused_{false};
  bool needs_release_{true};
  bool gamepad_needs_neutral_{true};
  bool profile_ready_{false};
  bool brake_available_{false};
  double service_brake_{0}, hard_brake_{0};
  double service_pressure_{0}, hard_pressure_{0}, maximum_pressure_{0};
  double throttle_limit_{0}, steering_limit_{0}, steering_scale_{30};
};
}  // namespace mine_teleop::detail
