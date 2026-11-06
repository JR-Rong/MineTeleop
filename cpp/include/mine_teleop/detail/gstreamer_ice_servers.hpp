#pragma once
#include <gst/gst.h>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

namespace mine_teleop::detail {
inline nlohmann::json configure_webrtc_ice_servers(GstElement* peer, const nlohmann::json& servers, bool allow_turn) {
  bool stun = false, turn = false;
  const auto escape = [](const std::string& value) {
    gchar* encoded = g_uri_escape_string(value.c_str(), nullptr, FALSE);
    const std::string result(encoded); g_free(encoded); return result;
  };
  for (const auto& server : servers) {
    if (!server.is_object()) continue;
    auto urls = server.value("urls", nlohmann::json::array());
    if (urls.is_string()) urls = nlohmann::json::array({urls});
    if (!urls.is_array()) continue;
    for (const auto& value : urls) {
      if (!value.is_string()) continue;
      const auto url = value.get<std::string>();
      const auto separator = url.find(':');
      if (separator == std::string::npos) continue;
      const auto scheme = url.substr(0, separator);
      auto remainder = url.substr(separator + 1);
      while (remainder.starts_with("//")) remainder.erase(0, 2);
      if (!stun && (scheme == "stun" || scheme == "stuns")) {
        const auto uri = scheme + "://" + remainder;
        g_object_set(peer, "stun-server", uri.c_str(), nullptr); stun = true;
      }
      if (allow_turn && (scheme == "turn" || scheme == "turns")) {
        const auto username = server.value("username", ""), credential = server.value("credential", "");
        if (username.empty() || credential.empty()) throw std::runtime_error("TURN ICE server lacks credentials");
        const auto uri = scheme + "://" + escape(username) + ":" + escape(credential) + "@" + remainder;
        if (!turn) { g_object_set(peer, "turn-server", uri.c_str(), nullptr); turn = true; }
        else {
          gboolean added = FALSE;
          g_signal_emit_by_name(peer, "add-turn-server", uri.c_str(), &added);
          if (!added) throw std::runtime_error("webrtcbin rejected an additional TURN server");
        }
      }
    }
  }
  return {{"stun_applied", stun}, {"turn_applied", turn}};
}
}  // namespace mine_teleop::detail
