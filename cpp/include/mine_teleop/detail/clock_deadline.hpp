#pragma once

#include <cstdint>
#include <limits>

#include "mine_teleop/time.hpp"

namespace mine_teleop::detail {

constexpr std::int64_t saturating_deadline_ms(std::int64_t start_ms, std::int64_t duration_ms) {
  if (duration_ms <= 0)
    return start_ms;
  return start_ms >= std::numeric_limits<std::int64_t>::max() - duration_ms
             ? std::numeric_limits<std::int64_t>::max()
             : start_ms + duration_ms;
}

constexpr bool monotonic_deadline_reached(MonotonicMillis now, std::int64_t deadline_monotonic_ms) {
  return now.value >= deadline_monotonic_ms;
}

// A server expiry is a UTC protocol value. Snapshot the remaining lifetime at
// response receipt so later local wall-clock changes cannot shift a local
// renewal or expiry decision. The server remains the final authority.
constexpr MonotonicMillis local_monotonic_deadline_from_utc_expiry(UtcMillis expires_at_utc,
                                                                   ClockSample received_at) {
  if (expires_at_utc.value <= received_at.utc.value)
    return received_at.monotonic;
  return MonotonicMillis{saturating_deadline_ms(received_at.monotonic.value,
                                                expires_at_utc.value - received_at.utc.value)};
}

}  // namespace mine_teleop::detail
