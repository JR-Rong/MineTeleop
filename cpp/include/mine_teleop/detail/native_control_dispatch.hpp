#pragma once

#include "mine_teleop/core.hpp"

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

}  // namespace mine_teleop::detail
