#pragma once

#include <cstdint>

namespace mine_teleop {

// UTC values are carried on the wire and used for audit/display.  Monotonic
// values are process-local and must only be used for elapsed-time decisions.
// Keeping the two wrappers non-interchangeable prevents a future watchdog
// from silently accepting a synchronized/wall-clock timestamp.
struct UtcMillis {
  std::int64_t value{0};

  constexpr explicit UtcMillis(std::int64_t next_value = 0) : value(next_value) {}
  constexpr bool operator==(const UtcMillis&) const = default;
};

struct MonotonicMillis {
  std::int64_t value{0};

  constexpr explicit MonotonicMillis(std::int64_t next_value = 0) : value(next_value) {}
  constexpr bool operator==(const MonotonicMillis&) const = default;
};

struct ClockSample {
  UtcMillis utc;
  MonotonicMillis monotonic;
};

[[nodiscard]] UtcMillis utc_now_ms();
[[nodiscard]] MonotonicMillis process_monotonic_now_ms();

}  // namespace mine_teleop
