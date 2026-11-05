#pragma once

#include "mine_teleop/detail/native_control_publication.hpp"
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>

namespace mine_teleop::detail {

// Synchronous, single-owner courier for one session, media attempt and input
// owner. The owner must serialize tick and retirement with scope changes.
// Runtime bridge authenticates feedback and signs commands; this class never
// treats successful local enqueue as delivery or control readiness.
class NativeControlCourier {
 public:
  using Json = nlohmann::json;
  struct Io {
    std::function<std::optional<std::string>()> receive;
    std::function<bool()> direct_udp;
    std::function<Json(const Json&)> bridge;
    std::function<bool(const std::string&)> send;
    std::function<void(const Json&)> status;
  };
  explicit NativeControlCourier(NativeCourierScope scope)
      : scope_(std::move(scope)), publication_(scope_) {}

  bool tick(const NativeCourierScope& current, const Io& io) {
    if (retired_) return false;
    if (current != scope_) { retire(); return false; }
    if (!io.receive || !io.direct_udp || !io.bridge || !io.send)
      throw std::invalid_argument("native courier requires complete IO");
    for (unsigned count = 0; count < 64; ++count) {
      auto bytes = io.receive();
      if (!bytes) break;
      if (bytes->empty() || bytes->size() > 64 * 1024 || bytes->find('\0') != std::string::npos) {
        retire(); return false;
      }
      auto value = Json::parse(*bytes, nullptr, false);
      if (value.is_discarded() || !value.is_object()) { retire(); return false; }
      if (value.value("session_id", "") != scope_.media.session) continue;
      const auto event = value.value("event", "");
      if (value.value("protocol", "") != kControlTransportCapability) {
        // Legacy statuses travel on the authenticated peer channel. The
        // caller must independently validate session, sequence and profile.
        if (io.status && (event == "session_control_profile_status" || event == "vcu_handshake_status" ||
                          event == "vehicle_telemetry" || event == "control_command_rejected")) io.status(value);
        continue;
      }
      if (event == "vehicle_control_result") {
        if (value.contains("emergency_stop") || !safe_positive(value, "command_seq") || !safe_positive(value, "result_seq")) continue;
        if (results_.size() >= 64) { retire(); return false; }
        results_.push_back(std::move(value));
      } else if (event == "vehicle_control_status" && value.contains("status") && value.at("status").is_object() &&
                 value.contains("feedback_transport") && value.at("feedback_transport").is_object()) {
        const auto& status = value.at("status");
        const auto& transport = value.at("feedback_transport");
        const auto kind = status.value("event", "");
        if ((kind != "vehicle_telemetry" && kind != "vcu_handshake_status" &&
             kind != "session_control_profile_status" && kind != "control_command_rejected") ||
            !safe_positive(status, "control_status_seq") ||
            transport.value("protocol", "") != "native-feedback-transport-v1" ||
            transport.value("media_attempt", "") != scope_.media.attempt ||
            !safe_positive(transport, "epoch")) continue;
        auto found = statuses_.find(kind);
        if (found == statuses_.end() ||
            transport.at("epoch").get<std::uint64_t>() > found->second.at("feedback_transport").at("epoch").get<std::uint64_t>() ||
            (transport.at("epoch") == found->second.at("feedback_transport").at("epoch") &&
             status.at("control_status_seq").get<std::uint64_t>() > found->second.at("status").at("control_status_seq").get<std::uint64_t>()))
          statuses_[kind] = std::move(value);
      }
    }
    const auto started = std::chrono::steady_clock::now();
    const bool direct = io.direct_udp();
    Json request{{"source_session_id", scope_.media.session},
                 {"source_control_session_generation", scope_.media.generation},
                 {"media_negotiation_id", scope_.media.attempt},
                 {"ui_instance_id", scope_.owner}, {"ui_control_epoch", scope_.owner_epoch},
                 {"dc_ready", direct}, {"dc_network_path", direct ? "p2p" : "unknown"},
                 {"last_seen_seq", publication_.last_seen()}};
    if (!results_.empty()) request["vehicle_result"] = results_.front();
    if (!statuses_.empty()) {
      request["vehicle_statuses"] = Json::array();
      for (const auto& [kind, status] : statuses_) { (void)kind; request["vehicle_statuses"].push_back(status); }
    }
    const auto response = io.bridge(request);
    if (!response.is_object() || response.value("protocol", "") != kControlTransportCapability) {
      retire(); return false;
    }
    if (!results_.empty()) results_.pop_front();
    statuses_.clear();
    if (io.status && response.contains("statuses") && response.at("statuses").is_array())
      for (const auto& status : response.at("statuses")) io.status(status);
    const bool still_direct = direct && io.direct_udp();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const auto bytes = publication_.take(current, response, still_direct, elapsed);
    if (bytes && still_direct) static_cast<void>(io.send(*bytes));
    return true;
  }
  void retire() { retired_ = true; publication_.retire(); results_.clear(); statuses_.clear(); }
  [[nodiscard]] bool retired() const { return retired_; }
 private:
  static bool safe_positive(const Json& object, const char* field) {
    const auto it = object.find(field);
    if (it == object.end() || !it->is_number_integer()) return false;
    try {
      const auto value = it->get<std::int64_t>();
      return value > 0 && value <= 9007199254740991LL;
    } catch (const Json::exception&) { return false; }
  }
  const NativeCourierScope scope_;
  NativeControlPublication publication_;
  std::deque<Json> results_;
  std::map<std::string, Json> statuses_;
  bool retired_{};
};
}  // namespace mine_teleop::detail
