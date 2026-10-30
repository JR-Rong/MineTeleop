#pragma once

#include <cstdint>
#include <mutex>

namespace mine_teleop::detail {

// The UI thread supplies a foreground keyboard heartbeat. This state is only
// used by the isolated, zero-speed mock viewer; it cannot authorize motion.
class NativeMockKeyboard {
 public:
  struct Keys { bool forward{}, left{}, right{}, brake{}; };
  struct Input { double throttle{}, steering{}, brake{}; };

  void observe(bool focused, Keys keys, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    focused_ = focused && now_ms > 0;
    keys_ = focused_ ? keys : Keys{};
    observed_at_ms_ = focused_ ? now_ms : 0;
    if (!focused_) needs_release_ = true;
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
    return focused_ && observed_at_ms_ > 0 && now_ms >= observed_at_ms_ &&
        now_ms - observed_at_ms_ < 150;
  }

  Input sample(bool safety_ready, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
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
 private:
  std::mutex mutex_;
  Keys keys_{};
  std::int64_t observed_at_ms_{};
  bool focused_{false};
  bool needs_release_{true};
};
}  // namespace mine_teleop::detail
