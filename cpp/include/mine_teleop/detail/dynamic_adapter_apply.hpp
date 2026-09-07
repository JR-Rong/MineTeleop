#pragma once

#include "mine_teleop/core.hpp"

#include <cstdint>
#include <string_view>

namespace mine_teleop::detail {

// These function signatures stay opaque at the C++ adapter boundary.  The
// concrete PODs are kept in core.cpp, where their ABI layout is checked.
using DynamicAdapterApplyV2Fn = int (*)(
    int,
    double,
    double,
    const double*,
    int,
    void*);
using DynamicAdapterSetStopContextV1Fn = int (*)(const void*);
using DynamicAdapterEmergencyStopFn = int (*)();

struct DynamicAdapterApplyV2Outcome {
  int result_code{0};
  std::uint32_t issue_id{0};
};

struct DynamicAdapterSafeStopInvocation {
  bool uses_emergency_stop{false};
  int set_stop_context_result{0};
  int emergency_stop_result{0};
};

[[nodiscard]] DynamicAdapterApplyV2Outcome invoke_dynamic_adapter_apply_v2(
    DynamicAdapterApplyV2Fn apply_v2,
    int target_gear,
    double target_vx,
    double target_ax,
    const double* steering_values,
    int steering_count);

// Ordinary stops use the same v2 result validation as control application.
// Emergency stops remain an independent context-then-emergency-stop sequence;
// callers retain responsibility for turning nonzero C ABI results into their
// adapter status/error state.
[[nodiscard]] DynamicAdapterSafeStopInvocation invoke_dynamic_adapter_safe_stop(
    DynamicAdapterApplyV2Fn apply_v2,
    DynamicAdapterSetStopContextV1Fn set_stop_context,
    DynamicAdapterEmergencyStopFn emergency_stop,
    int target_gear,
    const ControlOutput& output,
    VehicleStopContext context,
    std::string_view ordinary_error_context);

}  // namespace mine_teleop::detail
