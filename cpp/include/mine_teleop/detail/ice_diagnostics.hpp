#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <sstream>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

namespace mine_teleop::detail {
inline nlohmann::json ice_address_summary(const std::string& address) {
  // Scope IDs are interface-local metadata, never part of diagnostic output.
  const auto zone = address.find('%');
  const auto parsed = address.find(':') != std::string::npos && zone != std::string::npos
      ? address.substr(0, zone) : address;
  std::array<unsigned char, 16> bytes{};
  std::string family = "unknown", scope = "unknown";
  if (inet_pton(AF_INET, address.c_str(), bytes.data()) == 1) {
    family = "ipv4";
    const auto a = bytes[0], b = bytes[1], c = bytes[2];
    scope = a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ? "private"
      : a == 100 && b >= 64 && b <= 127 ? "shared_cgnat"
      : a == 127 ? "loopback" : a == 169 && b == 254 ? "link_local"
      : (a == 192 && b == 0 && c == 2) || (a == 198 && b == 51 && c == 100) || (a == 203 && b == 0 && c == 113) ? "documentation"
      : a == 198 && (b == 18 || b == 19) ? "benchmark"
      : a >= 224 && a < 240 ? "multicast" : a == 0 || a >= 240 || (a == 192 && b == 0 && c == 0) || (a == 192 && b == 88 && c == 99) ? "reserved" : "public";
  } else if (inet_pton(AF_INET6, parsed.c_str(), bytes.data()) == 1) {
    family = "ipv6";
    bool first15zero = true;
    for (std::size_t i = 0; i < 15; ++i) first15zero = first15zero && bytes[i] == 0;
    scope = first15zero && bytes[15] == 1 ? "loopback" : first15zero && bytes[15] == 0 ? "unspecified"
      : (bytes[0] & 0xfe) == 0xfc ? "private" : bytes[0] == 0xfe && (bytes[1] & 0xc0) == 0x80 ? "link_local"
      : bytes[0] == 0xff ? "multicast"
      : ((bytes[0] == 0x20 && bytes[1] == 1 && bytes[2] == 0x0d && bytes[3] == 0xb8) || (bytes[0] == 0x3f && bytes[1] == 0xff && (bytes[2] & 0xf0) == 0)) ? "documentation"
      : (bytes[0] & 0xe0) == 0x20 ? "public" : "reserved";
  } else if (address.ends_with(".local")) { family = "mdns"; scope = "obfuscated"; }
  return {{"address_family", family}, {"address_class", scope}};
}
inline nlohmann::json ice_candidate_summary(std::string_view candidate) {
  std::istringstream input{std::string(candidate)};
  std::string foundation, component, protocol, priority, address, port, marker, type;
  input >> foundation >> component >> protocol >> priority >> address >> port >> marker >> type;
  auto result = ice_address_summary(address);
  result["candidate_type"] = marker == "typ" && (type == "host" || type == "srflx" || type == "prflx" || type == "relay") ? type : "unknown";
  result["protocol"] = protocol == "udp" || protocol == "UDP" ? "udp" : protocol == "tcp" || protocol == "TCP" ? "tcp" : "unknown";
  return result; // No address, port, foundation, ufrag or server credential.
}
inline bool ice_has_turn(const nlohmann::json& configuration) {
  for (const auto& server : configuration.value("ice_servers", nlohmann::json::array())) {
    if (!server.is_object()) continue;
    auto urls = server.value("urls", nlohmann::json::array());
    if (urls.is_string()) urls = nlohmann::json::array({urls});
    if (!urls.is_array()) continue;
    for (const auto& value : urls) if (value.is_string()) {
      const auto url = value.get<std::string>();
      if (url.starts_with("turn:") || url.starts_with("turns:")) return true;
    }
  }
  return false;
}
inline std::string ice_direct_sdp(const std::string& sdp) {
  std::istringstream input(sdp);
  std::string result, line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("a=candidate:") && ice_candidate_summary(line.substr(2))["candidate_type"] == "relay") continue;
    result += line + "\r\n";
  }
  return result;
}
}  // namespace mine_teleop::detail
