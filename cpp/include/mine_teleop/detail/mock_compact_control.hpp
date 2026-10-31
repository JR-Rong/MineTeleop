#pragma once

// Isolated mock-only wire experiment. The vehicle reconstructs the exact
// signed JSON before running the unchanged authentication and safety gates.
#include "mine_teleop/detail/control_transport.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mine_teleop::detail {
inline constexpr std::string_view kMockCompactControl = "mock-compact-control-cbor-v1";
inline bool mock_compact_control_allowed(bool explicitly_enabled, std::string_view vehicle,
                                         std::string_view adapter, double max_speed_kph) {
  return explicitly_enabled && vehicle.starts_with("test_bench_") && adapter == "mock" && max_speed_kph == 0.0;
}
struct MockCompactScope {
  std::string vehicle, driver, session, token, attempt;
};

inline std::optional<Json> decode_mock_compact_control(std::span<const std::uint8_t> wire,
                                                       const MockCompactScope& scope) {
  try {
    if (wire.size() < 4 || wire.size() > 256 || wire[0] != 'M' || wire[1] != 'T' || wire[2] != 1 || wire[3] != 0x8f ||
        scope.vehicle.empty() || scope.driver.empty() || scope.session.empty() || scope.token.empty() || scope.attempt.empty())
      return std::nullopt;
    auto a = Json::from_cbor(wire.begin() + 3, wire.end(), true, false);
    if (!a.is_array() || a.size() != 15 || !a.at(14).is_binary()) return std::nullopt;
    for (unsigned i : {0u, 1u, 2u, 7u, 8u, 9u, 10u, 11u, 12u, 13u})
      if (!a.at(i).is_number_unsigned()) return std::nullopt;
    for (unsigned i : {4u, 5u, 6u}) if (!a.at(i).is_number_float()) return std::nullopt;
    if (!a.at(3).is_string()) return std::nullopt;
    const auto flags = a.at(7).get<std::uint64_t>();
    if ((flags & ~std::uint64_t{0x7ff}) != 0 || (flags & 1) != 0) return std::nullopt;
    const auto& mac = a.at(14).get_binary();
    if (mac.size() != 32) return std::nullopt;
    static constexpr char hex[] = "0123456789abcdef";
    std::string digest; digest.reserve(64);
    for (auto b : mac) { digest.push_back(hex[b >> 4]); digest.push_back(hex[b & 15]); }
    return Json{{"protocol_version", a.at(0)}, {"vehicle_id", scope.vehicle}, {"driver_id", scope.driver},
        {"session_id", scope.session}, {"seq", a.at(1)}, {"sent_at_utc_ms", a.at(2)}, {"type", "control_command"},
        {"gear", a.at(3)}, {"steering", a.at(4)}, {"throttle", a.at(5)}, {"brake", a.at(6)},
        {"estop", false}, {"operator_active", bool(flags & (1 << 1))},
        {"mock_control_diagnostic", bool(flags & (1 << 2))}, {"critical_video_ready", bool(flags & (1 << 3))},
        {"control_time_sync_ready", bool(flags & (1 << 4))}, {"operator_input_ready", bool(flags & (1 << 5))},
        {"control_link_ready", bool(flags & (1 << 6))}, {"recovery_sample_healthy", bool(flags & (1 << 7))},
        {"intent_fresh", bool(flags & (1 << 8))}, {"intent_requires_fresh_input", bool(flags & (1 << 9))},
        {"recovery_bad_sample_revision", a.at(8)}, {"recovery_brake_revision", a.at(9)},
        {"intent_seq", a.at(10)}, {"control_token", scope.token},
        {"control_transport", {{"protocol", kControlTransportCapability}, {"path", "dc"},
            {"media_attempt", scope.attempt}, {"epoch", a.at(11)}, {"from_epoch", a.at(12)},
            {"min_command_seq", a.at(13)}, {"action", (flags & (1 << 10)) ? "select" : "command"}}},
        {"native_mac", digest}};
  } catch (const Json::exception&) { return std::nullopt; }
}

inline std::optional<std::vector<std::uint8_t>> encode_mock_compact_control(const Json& j) {
  try {
    if (!j.is_object() || j.at("type") != "control_command" || j.at("estop") != false ||
        j.at("control_transport").at("protocol") != kControlTransportCapability ||
        j.at("control_transport").at("path") != "dc") return std::nullopt;
    const auto& t = j.at("control_transport");
    if (t.at("action") != "select" && t.at("action") != "command") return std::nullopt;
    MockCompactScope scope{j.at("vehicle_id").get<std::string>(), j.at("driver_id").get<std::string>(),
        j.at("session_id").get<std::string>(), j.at("control_token").get<std::string>(),
        t.at("media_attempt").get<std::string>()};
    std::uint64_t flags = 0;
    const std::array<const char*, 9> names{"operator_active", "mock_control_diagnostic", "critical_video_ready",
        "control_time_sync_ready", "operator_input_ready", "control_link_ready", "recovery_sample_healthy",
        "intent_fresh", "intent_requires_fresh_input"};
    for (std::size_t i = 0; i < names.size(); ++i) if (j.at(names[i]).get<bool>()) flags |= std::uint64_t{1} << (i + 1);
    if (t.at("action") == "select") flags |= 1 << 10;
    const auto text = j.at("native_mac").get<std::string>();
    if (text.size() != 64) return std::nullopt;
    const auto nibble = [](char c) -> int { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    std::vector<std::uint8_t> mac; mac.reserve(32);
    for (std::size_t i = 0; i < text.size(); i += 2) {
      const auto a = nibble(text[i]), b = nibble(text[i + 1]);
      if (a < 0 || b < 0) return std::nullopt;
      mac.push_back(static_cast<std::uint8_t>((a << 4) | b));
    }
    Json array = Json::array({j.at("protocol_version"), j.at("seq"), j.at("sent_at_utc_ms"), j.at("gear"),
        j.at("steering"), j.at("throttle"), j.at("brake"), flags, j.at("recovery_bad_sample_revision"),
        j.at("recovery_brake_revision"), j.at("intent_seq"), t.at("epoch"), t.at("from_epoch"),
        t.at("min_command_seq"), Json::binary(mac)});
    auto body = Json::to_cbor(array);
    std::vector<std::uint8_t> wire{'M', 'T', 1}; wire.insert(wire.end(), body.begin(), body.end());
    auto decoded = decode_mock_compact_control(wire, scope);
    // Check serialized identity, not numeric JSON equality: the MAC signs dump().
    if (!decoded || decoded->dump() != j.dump()) return std::nullopt;
    return wire;
  } catch (const Json::exception&) { return std::nullopt; }
}
}  // namespace mine_teleop::detail
