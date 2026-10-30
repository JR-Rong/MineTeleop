#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mine_teleop::detail {

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
    ready_ = false;
    try {
      if (!status.is_object() || monotonic_ms < 0 || synchronized_utc_ms < 0 || uncertainty_ms < 0 || uncertainty_ms > 25 ||
          status.value("session_id", "") != session_ || status.value("vehicle_id", "") != vehicle_ ||
          status.value("adapter_type", "") != "mock" || status.value("mock_control_diagnostic", true) ||
          !status.contains("media_negotiation") || !status.at("media_negotiation").is_object() ||
          status.at("media_negotiation").value("id", "") != attempt_ ||
          !status.at("media_negotiation").value("answer_applied", false)) return false;
      const auto reported = status.at("reported_at_utc_ms").get<std::int64_t>();
      if (reported <= last_reported_ || reported < synchronized_utc_ms - 3000 - uncertainty_ms ||
          reported > synchronized_utc_ms + uncertainty_ms) return false;
      const auto& sync = status.at("time_sync");
      const auto remote_uncertainty = sync.at("uncertainty_ms").get<std::int64_t>();
      if (!sync.value("synchronized", false) || remote_uncertainty < 0 || remote_uncertainty > 25) return false;
      const auto& lanes = status.at("lanes");
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
      if (advancing) { last_good_ms_ = monotonic_ms; ready_ = true; }
      return ready_;
    } catch (const Json::exception&) { return false; }
  }
  bool ready(std::int64_t now_ms) const {
    std::lock_guard lock(mutex_);
    return ready_ && now_ms >= last_good_ms_ && now_ms - last_good_ms_ < 3000;
  }
 private:
  const std::string session_, vehicle_, attempt_;
  mutable std::mutex mutex_;
  std::map<std::string, std::uint64_t> ids_;
  std::int64_t last_good_ms_{}, last_reported_{};
  bool initialized_{}, ready_{};
};
}  // namespace mine_teleop::detail
