#pragma once

#include <cstdint>

namespace mine_teleop::detail {

// Only the isolated zero-speed dual-path mock may discard these replaceable
// copies. The caller must continue publishing the original evidence on WSS.
inline constexpr std::uint64_t kMockDcBackpressureBytes = 32 * 1024;

inline bool mock_skip_dc_telemetry(bool isolated_mock, bool wss_copy_available,
                                   std::uint64_t buffered_bytes) {
  return isolated_mock && wss_copy_available && buffered_bytes >= kMockDcBackpressureBytes;
}

inline bool mock_skip_dc_result(bool isolated_dual_mock, bool wss_copy_available,
                                bool accepted, bool ordinary_command,
                                std::uint64_t buffered_bytes) {
  return isolated_dual_mock && wss_copy_available && accepted && ordinary_command &&
      buffered_bytes >= kMockDcBackpressureBytes;
}

}  // namespace mine_teleop::detail
