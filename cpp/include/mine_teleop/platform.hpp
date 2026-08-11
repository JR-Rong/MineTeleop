#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace mine_teleop {

using SocketHandle = std::intptr_t;
inline constexpr SocketHandle kInvalidSocket = static_cast<SocketHandle>(-1);

[[nodiscard]] bool is_loopback_bind_address(std::string_view host);
[[nodiscard]] std::string platform_name();
[[nodiscard]] bool open_default_browser(std::string_view url, std::string& error);
void initialize_network_process();

}  // namespace mine_teleop
