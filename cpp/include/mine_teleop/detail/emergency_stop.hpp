#pragma once

#include "mine_teleop/detail/direct_authority.hpp"

#include <array>

namespace mine_teleop::detail {
inline constexpr std::string_view kEmergencyStopCapability = "native-stop-only-v1";
inline constexpr std::int64_t kNativeBridgePublicationMaxAgeMs = 100; // Existing stage-2 courier allowance, not cloud failure detection.

// Serialized by the SAME mutex as the real VehicleControlService. This class
// authenticates stop-only bytes; it never applies a command or renews a lease.
class EmergencyStopReceiver {
 public:
  enum class Action { ApplyOnce, Pending, Cached, Reject };
  struct Decision { Action action; std::string_view reason; Json cached; };
  void reset(std::string vehicle, std::string driver, std::string session, std::string token) {
    *this = {}; vehicle_ = std::move(vehicle); driver_ = std::move(driver); session_ = std::move(session); token_ = std::move(token);
  }
  void observe_latched(bool latched) {
    if (was_latched_ && !latched) {
      if (reset_epoch_ == std::numeric_limits<std::uint64_t>::max()) exhausted_ = true;
      else ++reset_epoch_;
    }
    was_latched_ = latched;
  }
  std::uint64_t reset_epoch() const { return reset_epoch_; }
  Decision inspect(const Json& message, std::string_view key, const Json& current_authority,
      std::string_view media_attempt, bool data_channel, std::int64_t now_utc, int max_command_age_ms) {
    try {
      const auto command = ControlCommand::from_json(message);
      if (session_.empty() || command.vehicle_id != vehicle_ || command.driver_id != driver_ || command.session_id != session_ ||
          command.control_token != token_ || exhausted_ || !command.estop || command.throttle != 0.0 || command.steering != 0.0 || command.brake != 1.0 ||
          command.operator_active || !verify_transport_message(message, key, false)) return reject("emergency_not_authenticated_stop_only");
      const auto& stop = message.at("emergency_stop");
      const auto& authority = stop.at("authority");
      if (!stop.is_object() || stop.dump().size() > 1024 || stop.value("protocol", "") != kEmergencyStopCapability ||
          !current_authority.is_object() || authority.value("issuer", "") != current_authority.value("issuer", "") ||
          ControlTransportStamp::uint(authority, "authority_epoch") != current_authority.at("authority_epoch").get<std::uint64_t>() ||
          ControlTransportStamp::uint(authority, "lease_seq") == 0 ||
          ControlTransportStamp::uint(authority, "lease_seq") > current_authority.at("lease_seq").get<std::uint64_t>() ||
          ControlTransportStamp::uint(stop, "reset_epoch") != reset_epoch_)
        return reject("emergency_authority_or_reset_retired");
      const auto attempt = stop.at("media_attempt").get<std::string>();
      if (attempt.empty() || attempt.size() > 128 || (data_channel && attempt != media_attempt)) return reject("emergency_media_attempt_retired");
      if (!command.seq || ControlTransportStamp::uint(message, "seq") != command.seq || ControlTransportStamp::uint(message, "intent_seq") == 0 || max_command_age_ms <= 0 || now_utc < 0 || command.sent_at_utc_ms < 0 ||
          (command.sent_at_utc_ms <= now_utc ? now_utc - command.sent_at_utc_ms > max_command_age_ms : command.sent_at_utc_ms - now_utc > max_command_age_ms))
        return reject("emergency_command_expired");
      const auto mac = message.at("native_mac").get<std::string>();
      for (const auto& entry : entries_) if (entry && entry->seq == command.seq) {
        if (entry->mac != mac) return reject("emergency_sequence_conflict");
        return {entry->result.is_object() ? Action::Cached : Action::Pending, "emergency_duplicate", entry->result};
      }
      if (command.seq <= highest_seq_) return reject("emergency_sequence_replayed");
      std::optional<std::size_t> slot;
      for (std::size_t index = 0; index < entries_.size(); ++index) {
        if (!entries_[index]) { slot = index; break; }
        if (entries_[index]->result.is_object() && (!slot || entries_[index]->seq < entries_[*slot]->seq)) slot = index;
      }
      if (!slot) return reject("emergency_result_capacity");
      entries_[*slot] = Entry{command.seq, message.at("intent_seq").get<std::uint64_t>(), mac, Json(nullptr)};
      highest_seq_ = command.seq;
      return {Action::ApplyOnce, "emergency_authenticated", Json(nullptr)};
    } catch (const std::exception&) { return reject("emergency_invalid_message"); }
  }
  bool complete(const Json& command, const Json& result) {
    const auto seq = command.at("seq").get<std::uint64_t>();
    for (auto& entry : entries_) if (entry && entry->seq == seq && entry->mac == command.at("native_mac").get_ref<const std::string&>() &&
        result.at("command_seq") == seq && result.at("intent_seq") == entry->intent) {
      if (entry->result.is_object()) return entry->result == result;
      entry->result = result; return true;
    }
    return false;
  }
 private:
  static Decision reject(std::string_view reason) { return {Action::Reject, reason, Json(nullptr)}; }
  struct Entry { std::uint64_t seq, intent; std::string mac; Json result; };
  std::array<std::optional<Entry>, 64> entries_;
  std::uint64_t highest_seq_{}, reset_epoch_{};
  std::string vehicle_, driver_, session_, token_;
  bool was_latched_{}, exhausted_{};
};
}  // namespace mine_teleop::detail
