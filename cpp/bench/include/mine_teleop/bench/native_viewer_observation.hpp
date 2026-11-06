#pragma once
#include <cstdint>
#include <string_view>
#include <vector>

namespace mine_teleop::detail {
enum class ViewerPath { unknown, direct_udp, relay, other };

inline ViewerPath viewer_path(std::string_view local, std::string_view remote,
                              std::string_view local_transport, std::string_view remote_transport) {
  if (local == "relay" || remote == "relay") return ViewerPath::relay;
  const auto direct = [](std::string_view type) { return type == "host" || type == "srflx" || type == "prflx"; };
  if (!direct(local) || !direct(remote)) return ViewerPath::unknown;
  return local_transport == "udp" && remote_transport == "udp" ? ViewerPath::direct_udp : ViewerPath::other;
}

inline bool viewer_draws_advance(const std::vector<std::uint64_t>& current,
                                 const std::vector<std::uint64_t>& previous, std::size_t expected) {
  if (!expected || current.size() != expected || previous.size() != expected) return false;
  for (std::size_t index = 0; index < expected; ++index)
    if (current[index] <= previous[index]) return false;
  return true;
}

inline bool viewer_observation_stale(std::int64_t now_ms, std::int64_t previous_ms) {
  return previous_ms > 0 && (now_ms < previous_ms || now_ms - previous_ms > 2500);
}
}  // namespace mine_teleop::detail
