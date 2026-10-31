#pragma once

#include "mine_teleop/detail/control_transport.hpp"
#include <array>
#include <optional>

namespace mine_teleop::detail {
// Isolated mock experiment: carry the original safety and identity fields
// unchanged. Full telemetry is still sent periodically by the caller.
inline std::optional<Json> mock_compact_telemetry(const Json& input) {
  try {
    if (!input.is_object() || input.at("event") != "vehicle_telemetry" ||
        !input.at("control_timing").is_object() || !input.at("speed_mps").is_number() ||
        !input.at("gear").is_string() || !input.at("safety_state").is_string() ||
        !input.at("estop").is_boolean()) return std::nullopt;
    static constexpr std::array fields{"event", "protocol_version", "vehicle_id", "driver_id",
        "session_id", "seq", "sent_at_utc_ms", "control_status_seq", "safety_state",
        "control_timing", "speed_mps", "gear", "estop", "stop_source", "stop_reason", "stop_sequence"};
    Json result = Json::object();
    for (const auto* field : fields) result[field] = input.at(field);
    result["mock_telemetry_summary"] = true;
    if (result.dump().size() > 1200) return std::nullopt;
    return result;
  } catch (const Json::exception&) { return std::nullopt; }
}
}  // namespace mine_teleop::detail
