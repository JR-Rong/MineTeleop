#pragma once

#include <cstdint>
#include <chrono>
#include <mutex>

namespace mine_teleop::detail {

// A fresh observation cannot come from the future or survive the input lease.
inline bool native_mock_input_fresh(std::int64_t observed_ms, std::int64_t now_ms) {
  return observed_ms > 0 && now_ms >= observed_ms && now_ms - observed_ms < 200;
}

inline bool native_mock_clock_fresh(std::int64_t observed_ms, std::int64_t now_ms) {
  return observed_ms > 0 && now_ms >= observed_ms && now_ms - observed_ms < 500;
}

// The UI thread supplies a foreground keyboard heartbeat. This state is only
// used by the isolated, zero-speed mock viewer; it cannot authorize motion.
class NativeMockKeyboard {
 public:
  struct Keys { bool forward{}, left{}, right{}, brake{}; };
  struct Input { double throttle{}, steering{}, brake{}; };

  void observe(bool focused, Keys keys, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    observe_locked(focused, keys, now_ms);
  }

  void withdraw() {
    std::lock_guard lock(mutex_);
    focused_ = false;
    observed_at_ms_ = 0;
    keys_ = {};
    needs_release_ = true;
  }

  bool heartbeat_alive(std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    return heartbeat_alive_locked(now_ms);
  }

  bool heartbeat_alive_now() {
    std::lock_guard lock(mutex_);
    return heartbeat_alive_locked(now());
  }

  Input sample(bool safety_ready, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    return sample_locked(safety_ready, now_ms);
  }

  Input sample_now(bool safety_ready) {
    std::lock_guard lock(mutex_);
    return sample_locked(safety_ready, now());
  }
 private:
  static std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void observe_locked(bool focused, Keys keys, std::int64_t now_ms) {
    focused_ = focused && now_ms > 0;
    keys_ = focused_ ? keys : Keys{};
    observed_at_ms_ = focused_ ? now_ms : 0;
    if (!focused_) needs_release_ = true;
  }

  bool heartbeat_alive_locked(std::int64_t now_ms) const {
    return focused_ && observed_at_ms_ > 0 && now_ms >= observed_at_ms_ &&
        now_ms - observed_at_ms_ < 150;
  }

  Input sample_locked(bool safety_ready, std::int64_t now_ms) {
    if (!safety_ready || !focused_ || observed_at_ms_ <= 0 || now_ms < observed_at_ms_ ||
        now_ms - observed_at_ms_ >= 150) {
      needs_release_ = true;
      return {};
    }
    const bool any = keys_.forward || keys_.left || keys_.right || keys_.brake;
    if (needs_release_) {
      if (!any) needs_release_ = false;
      return {};
    }
    // Braking wins over forward input. Opposing steering keys cancel out.
    return {keys_.forward && !keys_.brake ? 0.25 : 0.0,
            keys_.left == keys_.right ? 0.0 : (keys_.left ? -0.25 : 0.25),
            keys_.brake ? 0.3 : 0.0};
  }
  std::mutex mutex_;
  Keys keys_{};
  std::int64_t observed_at_ms_{};
  bool focused_{false};
  bool needs_release_{true};
};
}  // namespace mine_teleop::detail
