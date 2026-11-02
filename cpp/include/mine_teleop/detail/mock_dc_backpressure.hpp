#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>

namespace mine_teleop::detail {

// Only the isolated zero-speed dual-path mock may discard these replaceable
// copies. The caller must continue publishing the original evidence on WSS.
inline constexpr std::uint64_t kMockDcBackpressureBytes = 32 * 1024;

inline bool mock_replaceable_vcu_snapshot(const nlohmann::json& message) {
  try {
    if (!message.is_object() || message.value("event", "") != "vcu_handshake_status" ||
        message.value("result", "") != "status_update" || !message.value("adapter_ready", false) ||
        message.contains("issue_code") || message.contains("control_recovery") ||
        !message.contains("status") || !message.at("status").is_object()) return false;
    const auto& status = message.at("status");
    return status.value("state", "") == "unsupported" && status.contains("supported") &&
        status.at("supported").is_boolean() && !status.at("supported").get<bool>() &&
        !status.value("ready", true) && !status.value("requested", true) &&
        !status.value("handshake_revoked", true) && !status.value("disarming", true) &&
        !status.value("vmc_fault_code_valid", true) && !status.value("speed_valid", true);
  } catch (const nlohmann::json::exception&) { return false; }
}

inline bool mock_skip_dc_snapshot(bool isolated_mock, bool wss_copy_available,
                                  bool replaceable, std::uint64_t buffered_bytes) {
  return isolated_mock && wss_copy_available && replaceable && buffered_bytes >= kMockDcBackpressureBytes;
}

inline bool mock_skip_dc_result(bool isolated_dual_mock, bool wss_copy_available,
                                bool accepted, bool ordinary_command,
                                std::uint64_t buffered_bytes) {
  return isolated_dual_mock && wss_copy_available && accepted && ordinary_command &&
      buffered_bytes >= kMockDcBackpressureBytes;
}

}  // namespace mine_teleop::detail
