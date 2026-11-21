#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace mine_teleop {

using SocketHandle = std::intptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(-1);

struct WifiStreamingStatus {
  bool api_available{false};
  std::uint32_t connected_interfaces{0};
  std::uint32_t enabled_interfaces{0};
  std::uint32_t background_scan_disabled_interfaces{0};
  std::uint32_t error_code{0};
};

// A Windows WLAN client request, released when the owning client exits. This
// does not persist adapter settings; other platforms and unavailable APIs are
// best-effort no-ops and never affect control readiness or safety deadlines.
class ScopedWifiStreaming {
 public:
  ScopedWifiStreaming() noexcept;
  ~ScopedWifiStreaming();
  ScopedWifiStreaming(const ScopedWifiStreaming&) = delete;
  ScopedWifiStreaming& operator=(const ScopedWifiStreaming&) = delete;
  [[nodiscard]] const WifiStreamingStatus& status() const noexcept { return status_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  WifiStreamingStatus status_;
};

[[nodiscard]] bool is_loopback_bind_address(std::string_view host);
[[nodiscard]] std::string platform_name();
[[nodiscard]] bool open_default_browser(std::string_view url, std::string& error);
void initialize_network_process();

}  // namespace mine_teleop
