#pragma once

// One predicate for every isolated zero-speed mock bench behavior: bench-only
// diagnostics and the attestation a native mock client requires before it
// sends any control. Real adapters, non-bench IDs or non-zero speed never pass.
#include <nlohmann/json.hpp>
#include <string_view>

namespace mine_teleop::detail {

inline constexpr std::string_view kIsolatedMockBenchCapability = "isolated-mock-bench-v1";

inline bool isolated_mock_bench(std::string_view vehicle_id, std::string_view adapter_type,
                                double max_speed_kph, std::string_view commissioning_mode) {
  return vehicle_id.starts_with("test_bench_") && adapter_type == "mock" &&
      max_speed_kph == 0.0 && commissioning_mode == "bench";
}

// Per-command bench tracing needs all three: compiled into this binary (a release bundle compiles it out),
// requested by the vehicle config, and an attested isolated bench. Never part of the attestation itself.
constexpr bool isolated_mock_trace_enabled(bool compiled, bool control_log_commands, bool attested) {
  return compiled && control_log_commands && attested;
}

inline bool isolated_mock_bench_offer(const nlohmann::json& payload) {
  if (!payload.is_object() || !payload.contains("isolated_mock_bench_capability")) return false;
  const auto& value = payload.at("isolated_mock_bench_capability");
  return value.is_string() && value.get_ref<const std::string&>() == kIsolatedMockBenchCapability;
}

}  // namespace mine_teleop::detail
