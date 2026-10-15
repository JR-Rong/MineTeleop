#pragma once

#include "mine_teleop/core.hpp"

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace mine_teleop::detail {

inline constexpr std::string_view kControlTransportCapability = "native-control-transport-v1";
std::string control_transport_mac(std::string_view value, std::string_view key);
inline std::string transport_message_mac(const Json& value, std::string_view key, bool vehicle_result) {
  auto unsigned_value = value;
  unsigned_value.erase(vehicle_result ? "vehicle_mac" : "native_mac");
  return control_transport_mac(std::string(vehicle_result ? "vehicle-result\n" : "native-command\n") + unsigned_value.dump(), key);
}
inline bool verify_transport_message(const Json& value, std::string_view key, bool vehicle_result) {
  const auto field = vehicle_result ? "vehicle_mac" : "native_mac";
  if (key.size() < 32 || !value.contains(field) || !value.at(field).is_string()) return false;
  const auto supplied = value.at(field).get<std::string>();
  if (supplied.size() != 64) return false;
  const auto expected = transport_message_mac(value, key, vehicle_result);
  unsigned char different = 0;
  for (std::size_t index = 0; index < expected.size(); ++index)
    different |= static_cast<unsigned char>(expected[index]) ^ static_cast<unsigned char>(supplied[index]);
  return different == 0;
}
enum class ControlPath { Wss, DataChannel };
inline std::string_view control_path_name(ControlPath path) { return path == ControlPath::Wss ? "wss" : "dc"; }

struct ControlTransportStamp {
  std::uint64_t epoch{0};
  std::uint64_t from_epoch{0};
  std::uint64_t min_command_seq{0};
  ControlPath path{ControlPath::Wss};
  std::string media_attempt;
  bool select{false};

  static std::uint64_t uint(const Json& value, const char* key) {
    const auto& field = value.at(key);
    if (!field.is_number_unsigned() && !(field.is_number_integer() && field.get<std::int64_t>() >= 0))
      throw std::invalid_argument("transport sequence must be an unsigned integer");
    return field.get<std::uint64_t>();
  }
  static ControlTransportStamp from_json(const Json& value) {
    if (!value.is_object() || value.value("protocol", "") != kControlTransportCapability)
      throw std::invalid_argument("unsupported control transport protocol");
    for (const auto& [key, ignored] : value.items()) {
      static_cast<void>(ignored);
      if (key != "protocol" && key != "epoch" && key != "from_epoch" && key != "min_command_seq" &&
          key != "path" && key != "media_attempt" && key != "action")
        throw std::invalid_argument("unknown control transport field");
    }
    ControlTransportStamp result;
    result.epoch = uint(value, "epoch");
    result.from_epoch = uint(value, "from_epoch");
    result.min_command_seq = uint(value, "min_command_seq");
    const auto path = value.at("path").get<std::string>();
    if (path != "wss" && path != "dc") throw std::invalid_argument("unknown control transport path");
    result.path = path == "dc" ? ControlPath::DataChannel : ControlPath::Wss;
    result.media_attempt = value.at("media_attempt").get<std::string>();
    const auto action = value.at("action").get<std::string>();
    if (action != "select" && action != "command") throw std::invalid_argument("unknown transport action");
    result.select = action == "select";
    if (result.epoch == 0 || result.min_command_seq == 0 || result.media_attempt.size() > 128 ||
        (result.path == ControlPath::DataChannel && result.media_attempt.empty()))
      throw std::invalid_argument("invalid control transport identity");
    return result;
  }
  Json to_json() const {
    return {{"protocol", kControlTransportCapability}, {"epoch", epoch}, {"from_epoch", from_epoch},
        {"min_command_seq", min_command_seq}, {"path", control_path_name(path)},
        {"media_attempt", media_attempt}, {"action", select ? "select" : "command"}};
  }
};

inline Json bounded_vehicle_control_result(const Json& value, std::string_view session,
    std::string_view vehicle, std::string_view driver) {
  if (!value.is_object() || value.value("event", "") != "vehicle_control_result" ||
      value.value("protocol", "") != kControlTransportCapability ||
      value.value("session_id", "") != session || value.value("vehicle_id", "") != vehicle ||
      value.value("driver_id", "") != driver)
    throw std::invalid_argument("vehicle result scope is invalid");
  Json result = {{"event", "vehicle_control_result"}, {"protocol", kControlTransportCapability},
      {"session_id", session}, {"vehicle_id", vehicle}, {"driver_id", driver}};
  for (const char* key : {"command_seq", "intent_seq", "result_seq", "control_status_seq"}) {
    const auto number = ControlTransportStamp::uint(value, key);
    if (number == 0 && std::string_view(key) != "control_status_seq")
      throw std::invalid_argument("vehicle result sequence is zero");
    result[key] = number;
  }
  result["authority_epoch"] = ControlTransportStamp::uint(value, "authority_epoch");
  for (const char* key : {"accepted", "rearm_required", "transport_admitted", "transport_selected"}) {
    if (!value.at(key).is_boolean()) throw std::invalid_argument("vehicle result flag is invalid");
    result[key] = value.at(key);
  }
  if (result.at("rearm_required").get<bool>() && (result.at("accepted").get<bool>() ||
      result.at("control_status_seq").get<std::uint64_t>() == 0))
    throw std::invalid_argument("vehicle rearm result is invalid");
  const auto reason = value.at("reason").get<std::string>();
  if (reason.size() > 128) throw std::invalid_argument("vehicle result reason is too long");
  result["reason"] = reason;
  if (value.contains("vehicle_mac")) result["vehicle_mac"] = value.at("vehicle_mac");
  if (value.contains("control_transport"))
    result["control_transport"] = ControlTransportStamp::from_json(value.at("control_transport")).to_json();
  return result;
}

inline Json bounded_vehicle_control_status(const Json& value, std::string_view session,
    std::string_view vehicle, std::string_view driver) {
  if (!value.is_object() || value.value("event", "") != "vehicle_control_status" ||
      value.value("protocol", "") != kControlTransportCapability || value.value("session_id", "") != session ||
      value.value("vehicle_id", "") != vehicle || value.value("driver_id", "") != driver)
    throw std::invalid_argument("vehicle status scope is invalid");
  auto message = value.at("status");
  const auto event = message.value("event", "");
  if (!message.is_object() || (event != "vehicle_telemetry" && event != "vcu_handshake_status" &&
      event != "session_control_profile_status" && event != "control_command_rejected") ||
      ControlTransportStamp::uint(message, "control_status_seq") == 0 || message.dump().size() > 48 * 1024)
    throw std::invalid_argument("vehicle status kind or size is invalid");
  for (const auto& [key, expected] : {std::pair{"session_id", session}, {"vehicle_id", vehicle}, {"driver_id", driver}}) {
    if (message.contains(key) && message.at(key) != expected) throw std::invalid_argument("vehicle status identity mismatch");
    message[key] = expected;
  }
  return {{"event", "vehicle_control_status"}, {"protocol", kControlTransportCapability},
      {"session_id", session}, {"vehicle_id", vehicle}, {"driver_id", driver}, {"status", std::move(message)}};
}

// Session-owned, serialized by the SAME mutex as VehicleControlService. It
// consumes admitted identities once, including rejected service results. It
// never refreshes the safety watchdog; only the common service can do that.
class ControlTransportFence {
 public:
  struct Decision { bool admitted; bool changed; std::string_view reason; };
  void reset(std::string vehicle, std::string driver, std::string session, std::string token) {
    vehicle_ = std::move(vehicle); driver_ = std::move(driver); session_ = std::move(session); token_ = std::move(token);
    epoch_ = last_seq_ = min_seq_ = from_epoch_ = 0; path_ = ControlPath::Wss; attempt_.clear();
  }
  Decision admit(const ControlCommand& command, const Json* stamp_json, ControlPath received_path,
      std::string_view current_media_attempt, std::int64_t now_utc_ms, std::int64_t lease_expires_at_ms,
      int max_command_gap_ms, bool cloud_authority_live) {
    if (session_.empty() || command.vehicle_id != vehicle_ || command.driver_id != driver_ ||
        command.session_id != session_ || command.control_token != token_)
      return {false, false, "transport_authority_mismatch"};
    if (command.seq == 0 || command.seq <= last_seq_) return {false, false, "transport_sequence_replayed"};
    // Direct control still fails closed with the existing cloud authority.
    // A valid ESTOP retains the legacy receiver's own expiry/sequence rules.
    if (!command.estop && (epoch_ != 0 || stamp_json) &&
        (!cloud_authority_live || lease_expires_at_ms <= now_utc_ms))
      return {false, false, "transport_authority_unavailable"};
    if (!command.estop && (command.sent_at_utc_ms < now_utc_ms - max_command_gap_ms ||
        command.sent_at_utc_ms > now_utc_ms + max_command_gap_ms))
      return {false, false, "transport_command_expired"};
    // Emergency WSS bypasses ONLY the ordinary path/epoch barrier. Identity,
    // token, global sequence and the common receiver's ESTOP latch still apply.
    if (command.estop && received_path == ControlPath::Wss) {
      last_seq_ = command.seq; return {true, false, "emergency_wss"};
    }
    if (!stamp_json) {
      if (epoch_ != 0 || received_path != ControlPath::Wss) return {false, false, "transport_stamp_required"};
      last_seq_ = command.seq; return {true, false, "legacy_wss"};
    }
    const auto stamp = ControlTransportStamp::from_json(*stamp_json);
    if (stamp.path != received_path || (stamp.path == ControlPath::DataChannel && stamp.media_attempt != current_media_attempt))
      return {false, false, "transport_path_or_attempt_mismatch"};
    bool changed = false;
    if (stamp.select) {
      if (command.throttle != 0.0 || command.steering != 0.0 || command.estop)
        return {false, false, "transport_selection_requires_neutral"};
      const bool repeated = epoch_ == stamp.epoch && from_epoch_ == stamp.from_epoch && path_ == stamp.path &&
          attempt_ == stamp.media_attempt && min_seq_ == stamp.min_command_seq;
      if (!repeated) {
        if (epoch_ == std::numeric_limits<std::uint64_t>::max() || stamp.from_epoch != epoch_ ||
            stamp.epoch != epoch_ + 1 || stamp.min_command_seq <= last_seq_ || command.seq < stamp.min_command_seq)
          return {false, false, "transport_epoch_conflict"};
        from_epoch_ = epoch_; epoch_ = stamp.epoch; path_ = stamp.path;
        min_seq_ = stamp.min_command_seq; attempt_ = stamp.media_attempt; changed = true;
      }
    } else if (stamp.epoch != epoch_ || stamp.path != path_ || stamp.media_attempt != attempt_ ||
        stamp.from_epoch != from_epoch_ || stamp.min_command_seq != min_seq_ || command.seq < min_seq_)
      return {false, false, "transport_epoch_retired"};
    last_seq_ = command.seq;
    return {true, changed, stamp.select ? "transport_selected" : "transport_command"};
  }
  void note_legacy_sequence(std::uint64_t seq) { if (seq > last_seq_) last_seq_ = seq; }
  [[nodiscard]] bool negotiated() const { return epoch_ != 0; }
  [[nodiscard]] std::uint64_t epoch() const { return epoch_; }
  [[nodiscard]] ControlPath path() const { return path_; }
  [[nodiscard]] const std::string& attempt() const { return attempt_; }
 private:
  std::string vehicle_, driver_, session_, token_, attempt_;
  std::uint64_t epoch_{0}, last_seq_{0}, min_seq_{0}, from_epoch_{0};
  ControlPath path_{ControlPath::Wss};
};

}  // namespace mine_teleop::detail
