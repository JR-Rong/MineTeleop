#pragma once

#include "mine_teleop/core.hpp"

#include <algorithm>
#include <string_view>

namespace mine_teleop::detail {

// Revalidate after potentially slow preparation without retimestamping a
// prepared command or treating a newer refresh as a new wire deadline.
inline std::string_view native_control_dispatch_rejection(
    const NativeControlIntentSample& prepared,
    const NativeControlIntentSample& current,
    std::int64_t preparation_age_ms,
    int lease_ms) {
  if (prepared.intent.estop) return {};
  if (preparation_age_ms < 0 || preparation_age_ms >= lease_ms)
    return "prepared_input_expired_no_io";
  if (!current.active || prepared.owner_ui_instance_id != current.owner_ui_instance_id ||
      prepared.ui_control_epoch != current.ui_control_epoch ||
      prepared.intent != current.intent || prepared.fresh != current.fresh ||
      prepared.requires_fresh_input != current.requires_fresh_input ||
      prepared.refresh_bad_sample_revision != current.refresh_bad_sample_revision ||
      prepared.refresh_brake_revision != current.refresh_brake_revision)
    return "prepared_input_retired_no_io";
  return {};
}

// The courier may run independently of the input/render thread. A newer
// refresh cannot extend the deadline of an already-published wire frame.
inline std::int64_t native_control_publication_valid_for_ms(
    const NativeControlIntentSample& published,
    const NativeControlIntentSample& current,
    std::int64_t publication_age_ms,
    int publication_budget_ms,
    int lease_ms) {
  if (publication_age_ms < 0 || publication_age_ms >= publication_budget_ms ||
      !native_control_dispatch_rejection(published, current, publication_age_ms, lease_ms).empty())
    return 0;
  auto remaining = static_cast<std::int64_t>(publication_budget_ms) - publication_age_ms;
  if (published.fresh && !published.intent.estop) {
    if (published.age_ms < 0 || current.age_ms < 0) return 0;
    remaining = std::min({remaining, static_cast<std::int64_t>(lease_ms) - published.age_ms - publication_age_ms,
                         static_cast<std::int64_t>(lease_ms) - current.age_ms});
  }
  return std::max<std::int64_t>(0, remaining);
}

}  // namespace mine_teleop::detail
