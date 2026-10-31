#pragma once

#include <cstdint>

namespace mine_teleop::detail {

// Startup-only, isolated mock profile delivery. A successful send is not a
// vehicle acknowledgement. Each attempt must reuse the same scoped request
// and sequence: a cleared profile must never be re-enabled by a retry.
class NativeMockProfileRetry {
 public:
  static constexpr unsigned kMaxAttempts = 4;
  // Four attempts fit within the existing two-second profile age limit.
  static constexpr std::int64_t kRetryMs = 300;

  [[nodiscard]] bool due(bool gates_ready, bool transport_selected, bool accepted, std::int64_t now) const {
    return gates_ready && transport_selected && !accepted && now >= 0 && attempts_ < kMaxAttempts &&
        (attempts_ == 0 || (now >= last_sent_ms_ && now - last_sent_ms_ >= kRetryMs));
  }
  void sent(std::int64_t now) { ++attempts_; last_sent_ms_ = now; }
  [[nodiscard]] unsigned attempts() const { return attempts_; }

 private:
  unsigned attempts_{0};
  std::int64_t last_sent_ms_{-1};
};

}  // namespace mine_teleop::detail
