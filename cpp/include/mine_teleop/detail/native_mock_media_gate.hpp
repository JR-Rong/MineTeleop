#pragma once

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdint>
#include <initializer_list>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mine_teleop::detail {

// Keep only evidence consumed by the gate; absent and malformed fields stay
// absent or malformed so the regular validation remains fail closed.
inline nlohmann::json native_mock_media_evidence(const nlohmann::json& status) {
  using Json = nlohmann::json;
  if (!status.is_object()) return status;
  Json result = Json::object();
  for (const auto* key : {"session_id", "vehicle_id", "adapter_type", "mock_control_diagnostic",
                          "reported_at_utc_ms", "media_negotiation", "time_sync"})
    if (status.contains(key)) result[key] = status.at(key);
  if (status.contains("lanes")) {
    result["lanes"] = status.at("lanes");
    if (result["lanes"].is_array()) {
      for (auto& lane : result["lanes"]) {
        if (!lane.is_object()) continue;
        Json selected = Json::object();
        for (const auto* key : {"camera_id", "critical_for_control", "lane_state", "send_waiting_idr",
                                "send_invalid_sample_blocked", "send_last_sample_id_ns", "fresh_send_frames"})
          if (lane.contains(key)) selected[key] = lane.at(key);
        lane = std::move(selected);
      }
    }
  }
  // The status may contain large unrelated negotiation/clock fields.
  for (const auto& [key, fields] : std::initializer_list<std::pair<const char*, std::vector<const char*>>>{
           {"media_negotiation", {"id", "answer_applied"}}, {"time_sync", {"synchronized", "uncertainty_ms"}}}) {
    if (!result.contains(key) || !result[key].is_object()) continue;
    Json selected = Json::object();
    for (const auto* field : fields) if (result[key].contains(field)) selected[field] = result[key].at(field);
    result[key] = std::move(selected);
  }
  return result;
}

// Neutral mock bench only: remote status remains independent evidence from
// local frame submission. Callers must also enforce peer/session, renderer,
// input lease, SCTP path, control feedback and clock readiness.
class NativeMockMediaGate {
 public:
  using Json = nlohmann::json;
  NativeMockMediaGate(std::string session, std::string vehicle, std::string attempt,
                      const std::vector<std::string>& cameras)
      : session_(std::move(session)), vehicle_(std::move(vehicle)), attempt_(std::move(attempt)) {
    if (session_.empty() || !vehicle_.starts_with("test_bench_") || attempt_.empty() || cameras.size() != 2)
      throw std::invalid_argument("invalid isolated mock media scope");
    for (const auto& camera : cameras)
      if (camera.empty() || !ids_.emplace(camera, 0).second) throw std::invalid_argument("invalid mock camera list");
  }
  bool observe(const Json& status, std::int64_t monotonic_ms, std::int64_t synchronized_utc_ms,
               std::int64_t uncertainty_ms) {
    std::lock_guard lock(mutex_);
    try {
      const auto evidence = native_mock_media_evidence(status);
      // A previously validated identical observation may arrive over the
      // other path, even after a newer observation. It never renews the TTL
      // or restores readiness after a failure.
      if (monotonic_ms >= 0 && synchronized_utc_ms >= 0 && uncertainty_ms >= 0 && uncertainty_ms <= 25 &&
          evidence.contains("reported_at_utc_ms") && evidence.at("reported_at_utc_ms").is_number_integer()) {
        const auto reported = evidence.at("reported_at_utc_ms").get<std::int64_t>();
        if (reported >= synchronized_utc_ms - 3000 - uncertainty_ms && reported <= synchronized_utc_ms + uncertainty_ms)
          for (const auto& previous : validated_) if (evidence == previous) return ready_locked(monotonic_ms);
      }
      ready_ = false;
      reason_ = "scope";
      if (!status.is_object() || monotonic_ms < 0 || synchronized_utc_ms < 0 || uncertainty_ms < 0 || uncertainty_ms > 25 ||
          status.value("session_id", "") != session_ || status.value("vehicle_id", "") != vehicle_ ||
          status.value("adapter_type", "") != "mock" || status.value("mock_control_diagnostic", true) ||
          !status.contains("media_negotiation") || !status.at("media_negotiation").is_object() ||
          status.at("media_negotiation").value("id", "") != attempt_ ||
          !status.at("media_negotiation").value("answer_applied", false)) return false;
      const auto reported = status.at("reported_at_utc_ms").get<std::int64_t>();
      reason_ = "timestamp";
      if (reported <= last_reported_ || reported < synchronized_utc_ms - 3000 - uncertainty_ms ||
          reported > synchronized_utc_ms + uncertainty_ms) return false;
      const auto& sync = status.at("time_sync");
      reason_ = "remote_clock";
      const auto remote_uncertainty = sync.at("uncertainty_ms").get<std::int64_t>();
      if (!sync.value("synchronized", false) || remote_uncertainty < 0 || remote_uncertainty > 25) return false;
      const auto& lanes = status.at("lanes");
      reason_ = "lanes";
      if (!lanes.is_array() || lanes.size() != ids_.size()) return false;
      std::map<std::string, std::uint64_t> next;
      for (const auto& lane : lanes) {
        const auto camera = lane.at("camera_id").get<std::string>();
        if (!ids_.contains(camera) || next.contains(camera) || !lane.value("critical_for_control", false) ||
            lane.value("lane_state", "") != "active" || lane.value("send_waiting_idr", true) ||
            lane.value("send_invalid_sample_blocked", true) || !lane.contains("send_last_sample_id_ns") ||
            !lane.at("send_last_sample_id_ns").is_number_unsigned() || lane.value("fresh_send_frames", std::uint64_t{0}) == 0)
          return false;
        next[camera] = lane.at("send_last_sample_id_ns").get<std::uint64_t>();
      }
      bool advancing = initialized_;
      for (const auto& [camera, sample] : next) if (sample <= ids_.at(camera)) advancing = false;
      ids_ = std::move(next);
      initialized_ = true;
      last_reported_ = reported;
      if (validated_.size() == 4) validated_.erase(validated_.begin());
      validated_.push_back(evidence);
      reason_ = advancing ? "ready" : "not_advancing";
      if (advancing) { last_good_ms_ = monotonic_ms; ready_ = true; }
      return ready_;
    } catch (const Json::exception&) { ready_ = false; reason_ = "malformed"; return false; }
  }
  bool ready(std::int64_t now_ms) const {
    std::lock_guard lock(mutex_);
    return ready_locked(now_ms);
  }
  bool ready_now() const {
    std::lock_guard lock(mutex_);
    return ready_locked(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
  }
  std::string reason(std::int64_t now_ms) const {
    std::lock_guard lock(mutex_);
    if (ready_ && (now_ms < last_good_ms_ || now_ms - last_good_ms_ >= 3000)) return "expired";
    return reason_;
  }
 private:
  bool ready_locked(std::int64_t now_ms) const {
    return ready_ && now_ms >= last_good_ms_ && now_ms - last_good_ms_ < 3000;
  }
  const std::string session_, vehicle_, attempt_;
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t> ids_;
  std::vector<Json> validated_;
  std::int64_t last_good_ms_{}, last_reported_{};
  bool initialized_{}, ready_{};
  std::string reason_{"waiting"};
};
}  // namespace mine_teleop::detail
