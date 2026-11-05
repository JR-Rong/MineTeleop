#pragma once

#include "mine_teleop/detail/control_transport.hpp"
#include "mine_teleop/detail/native_control_acknowledgement_window.hpp"

#include <array>
#include <deque>
#include <optional>

namespace mine_teleop::detail {

inline bool mock_idle_reprobe_eligible(bool enabled, bool dc_ready, bool ack_waiting,
                                       const NativeControlIntentSample& sample, bool clock_ready) {
  return enabled && dc_ready && !ack_waiting && clock_ready && sample.active && sample.fresh &&
      !sample.requires_fresh_input && sample.ui_control_epoch != 0 && sample.refresh_sample_healthy &&
      sample.intent.gear == "N" && sample.intent.is_neutral() && sample.intent.critical_video_ready &&
      sample.intent.critical_video_sample_healthy;
}

// Serialized by DriverConsoleRuntime's control socket mutex. Native assigns
// every command sequence; the browser is a bounded byte courier, never a source.
class ControlTransportSender {
 public:
  struct Receipt { bool matched{false}; bool boundary{false}; std::int64_t round_trip_ms{0}; bool current{false}; };
  void reset() { *this = {}; }
  void reset_feedback_comparison() { wss_.reset(); dc_.reset(); baseline_wss_ms_.reset(); }
  void set_data_channel_is_p2p(bool value) { dc_is_p2p_ = value; }
  [[nodiscard]] ControlPath path() const { return stamp_.path; }
  [[nodiscard]] bool owns_dc_publication(const Json& payload) const {
    if (path() != ControlPath::DataChannel || !payload.is_object() ||
        !payload.contains("control_transport")) return false;
    const auto& wire = payload.at("control_transport");
    return wire.is_object() && wire.value("path", "") == "dc" &&
        wire.value("epoch", std::uint64_t{0}) == stamp_.epoch &&
        wire.value("media_attempt", "") == stamp_.media_attempt;
  }
  [[nodiscard]] bool negotiated() const { return stamp_.epoch != 0; }
  [[nodiscard]] bool selecting() const { return selecting_; }
  [[nodiscard]] bool holding_motion() const { return selecting_ || qualifying_; }
  [[nodiscard]] const std::string& attempt() const { return stamp_.media_attempt; }
  [[nodiscard]] std::uint64_t revision() const { return revision_; }

  void begin(ControlPath path, std::string attempt, std::int64_t now, int recovery_timeout_ms) {
    if (path == ControlPath::Wss && stamp_.path == ControlPath::DataChannel) {
      failed_trials_ = std::min(failed_trials_ + 1, kMaxTrialsPerAttempt);
    }
    // A timed-out selection may already have committed at the vehicle. Try
    // retiring that epoch; an authenticated conflict result reconciles below.
    const auto previous = std::max(remote_epoch_, stamp_.epoch);
    if (previous == std::numeric_limits<std::uint64_t>::max())
      throw std::runtime_error("control transport epoch exhausted");
    stamp_ = {previous + 1, previous, 0, path, std::move(attempt), true};
    selecting_ = true; qualifying_ = path == ControlPath::DataChannel;
    deadline_ = now + recovery_timeout_ms;
    if (qualifying_) { baseline_wss_ms_ = wss_.mean(); dc_.reset(); last_dc_result_ms_ = now; }
    else {
      const auto delay = std::min<std::int64_t>(60000,
          static_cast<std::int64_t>(recovery_timeout_ms) * (std::int64_t{1} << (failed_trials_ > 0 ? failed_trials_ - 1 : 0)));
      next_trial_ms_ = now + delay; first_wss_sent_ms_ = -1; wss_.reset();
    }
    ++revision_;
  }

  // Healthy proven P2P is preferred even if WSS is momentarily faster.
  // Relay/unknown DC retains real-result comparison against WSS. A trial holds
  // motion until DC also has three actual accepted neutral command results.
  // Repeated failures back off and exhaust this attempt's budget. A new media
  // attempt permits new evidence; a ready/unknown UI flap does not reset it.
  // The explicit isolated-mock option may permit one additional trial only
  // after a sustained fresh neutral owner and healthy WSS evidence.
  void observe_mock_idle_reprobe(bool eligible, std::uint64_t owner_epoch, std::int64_t now) {
    if (!eligible || !owner_epoch || now < 0 || path() != ControlPath::Wss || holding_motion() ||
        !dc_is_p2p_ || !wss_.healthy(now)) {
      mock_idle_since_.reset(); mock_idle_epoch_ = 0; mock_idle_last_ms_ = -1; return;
    }
    if (!mock_idle_since_ || mock_idle_epoch_ != owner_epoch || now < mock_idle_last_ms_ ||
        now - mock_idle_last_ms_ >= NativeControlAckRecovery::kFreshAckMs) {
      mock_idle_since_ = now; mock_idle_epoch_ = owner_epoch;
    }
    mock_idle_last_ms_ = now;
  }
  bool consider_dc(std::string_view attempt, std::int64_t now, int timeout_ms) {
    const bool unhealthy_wss = first_wss_sent_ms_ >= 0 && now >= first_wss_sent_ms_ &&
        now - first_wss_sent_ms_ >= NativeControlAckRecovery::kStallMs &&
        (last_wss_result_ms_ < 0 || now - last_wss_result_ms_ > NativeControlAckRecovery::kStallMs ||
         last_wss_result_age_ms_ > NativeControlAckRecovery::kFreshAckMs);
    if (attempt.empty() || path() != ControlPath::Wss || holding_motion() ||
        (!dc_is_p2p_ && !wss_.healthy(now) && !unhealthy_wss)) return false;
    if (trial_attempt_ == attempt) {
      if (now < next_trial_ms_ || (!dc_is_p2p_ && !unhealthy_wss)) return false;
      if (failed_trials_ >= kMaxTrialsPerAttempt) {
        if (mock_idle_reprobe_used_ || !mock_idle_since_ || now < *mock_idle_since_ ||
            now - *mock_idle_since_ < kMockIdleReprobeMs || now < mock_idle_last_ms_ ||
            now - mock_idle_last_ms_ >= NativeControlAckRecovery::kFreshAckMs || !wss_.healthy(now)) return false;
        mock_idle_reprobe_used_ = true;
        mock_idle_since_.reset(); mock_idle_epoch_ = 0; mock_idle_last_ms_ = -1;
      }
    }
    if (trial_attempt_ != attempt) {
      failed_trials_ = 0;
      profile_blocked_dc_ = false;
      profile_recovery_retry_used_ = false;
      mock_idle_reprobe_used_ = false;
      mock_idle_since_.reset(); mock_idle_epoch_ = 0; mock_idle_last_ms_ = -1;
    }
    trial_attempt_ = attempt;
    begin(ControlPath::DataChannel, std::string(attempt), now, timeout_ms);
    return true;
  }

  // This only withdraws motion and requests a fenced WSS fallback. No queued
  // DC command can become ordinary WSS traffic by relabelling its epoch.
  bool observe(bool dc_ready, std::string_view attempt, std::int64_t now, int timeout_ms) {
    if (path() == ControlPath::DataChannel && ((!dc_ready || stamp_.media_attempt != attempt) ||
        (last_dc_result_ms_ >= 0 && now - last_dc_result_ms_ > NativeControlAckRecovery::kStallMs) ||
        (holding_motion() && now >= deadline_))) {
      begin(ControlPath::Wss, {}, now, timeout_ms);
      return true;
    }
    return false;
  }

  void decorate(Json& payload) {
    if (!negotiated()) return;
    if (stamp_.min_command_seq == 0) stamp_.min_command_seq = payload.at("seq").get<std::uint64_t>();
    auto wire = stamp_; wire.select = selecting_;
    payload["control_transport"] = wire.to_json();
  }

  void note_sent(const Json& payload, ControlPath actual_path, std::int64_t started_ms) {
    if (actual_path == ControlPath::Wss && !payload.at("estop").get<bool>() && first_wss_sent_ms_ < 0)
      first_wss_sent_ms_ = started_ms;
    if (sent_.size() == 128) sent_.pop_front();
    sent_.push_back({payload.at("seq").get<std::uint64_t>(), payload.at("intent_seq").get<std::uint64_t>(),
        actual_path, payload.value("control_transport", Json(nullptr)), started_ms,
        payload.at("throttle").get<double>() == 0.0 && payload.at("steering").get<double>() == 0.0,
        payload.at("estop").get<bool>()});
  }

  Receipt note_result(const Json& result, std::int64_t now, int timeout_ms) {
    const auto seq = result.at("command_seq").get<std::uint64_t>();
    auto found = std::find_if(sent_.begin(), sent_.end(), [&](const Sent& sent) { return sent.seq == seq; });
    if (found == sent_.end() || found->intent != result.at("intent_seq").get<std::uint64_t>() ||
        found->stamp != result.value("control_transport", Json(nullptr)) || now < found->started || seq <= last_result_seq_)
      return {};
    const auto sent = *found;
    const auto age = now - sent.started;
    sent_.erase(sent_.begin(), std::next(found));
    last_result_seq_ = seq;
    Receipt receipt{true, false, age};
    const auto actual_epoch = result.at("authority_epoch").get<std::uint64_t>();
    if (selecting_ && !sent.stamp.is_null() && sent.stamp.at("epoch") == stamp_.epoch &&
        sent.stamp.at("path").get_ref<const std::string&>() == control_path_name(stamp_.path)) {
      remote_epoch_ = actual_epoch;
      if (result.at("transport_selected").get<bool>() && actual_epoch == stamp_.epoch) {
        selecting_ = false; stamp_.select = false; ++revision_; receipt.boundary = true;
      } else if (result.value("reason", "") == "transport_epoch_conflict") {
        // Rebase only from a result bound to an exact locally sent selection.
        stamp_.epoch = actual_epoch; stamp_.min_command_seq = 0;
        begin(ControlPath::Wss, {}, now, timeout_ms); receipt.boundary = true;
      }
    }
    const bool current = (!negotiated() && sent.stamp.is_null() && sent.path == ControlPath::Wss) ||
        (!sent.stamp.is_null() && sent.stamp.at("epoch") == stamp_.epoch &&
         sent.stamp.at("path").get_ref<const std::string&>() == control_path_name(stamp_.path) && sent.stamp.at("media_attempt") == stamp_.media_attempt);
    receipt.current = current;
    if (current && sent.path == ControlPath::Wss) { last_wss_result_ms_ = now; last_wss_result_age_ms_ = age; }
    if (current && sent.path == ControlPath::DataChannel && result.at("transport_admitted").get<bool>())
      last_dc_result_ms_ = now;
    // A DC trial can happen before the operator has confirmed the required
    // session profile. If that is the reason it was rejected, one later
    // authenticated, fresh neutral WSS acceptance permits one new bounded
    // trial budget. It does not bypass backoff, epoch fencing or DC health.
    if (current && sent.path == ControlPath::DataChannel && result.at("transport_admitted").get<bool>() &&
        !result.at("accepted").get<bool>() && age <= NativeControlAckRecovery::kFreshAckMs &&
        result.value("reason", "") == "session_profile_not_ready") profile_blocked_dc_ = true;
    if (current && sent.path == ControlPath::Wss && sent.neutral && !sent.estop &&
        result.at("accepted").get<bool>() && result.at("transport_admitted").get<bool>() &&
        age <= NativeControlAckRecovery::kFreshAckMs &&
        profile_blocked_dc_ && !profile_recovery_retry_used_) {
      failed_trials_ = 0;
      profile_blocked_dc_ = false;
      profile_recovery_retry_used_ = true;
    }
    if (current && !sent.estop && (!result.at("accepted").get<bool>() || age > NativeControlAckRecovery::kFreshAckMs))
      (sent.path == ControlPath::Wss ? wss_ : dc_).reset();
    if (current && result.at("accepted").get<bool>() && !sent.estop && age <= NativeControlAckRecovery::kFreshAckMs) {
      auto& health = sent.path == ControlPath::Wss ? wss_ : dc_;
      health.add(age, now);
      if (qualifying_ && !selecting_ && sent.path == ControlPath::DataChannel && sent.neutral && dc_.healthy(now)) {
        if (!dc_is_p2p_ && baseline_wss_ms_ && dc_.mean() && *dc_.mean() >= *baseline_wss_ms_) {
          begin(ControlPath::Wss, {}, now, timeout_ms);
        } else { qualifying_ = false; ++revision_; }
        receipt.boundary = true;
      }
    }
    return receipt;
  }

  Json status() const {
    return {{"protocol", kControlTransportCapability}, {"epoch", stamp_.epoch},
        {"path", control_path_name(path())}, {"media_attempt", attempt()}, {"selecting", selecting_},
        {"qualifying", qualifying_}, {"dc_path_is_p2p", dc_is_p2p_}, {"revision", revision_}, {"retained_sends", sent_.size()},
        {"failed_dc_trials", failed_trials_}, {"dc_trial_limit", kMaxTrialsPerAttempt},
        {"mock_idle_reprobe_used", mock_idle_reprobe_used_},
        {"next_dc_trial_monotonic_ms", next_trial_ms_},
        {"wss_result_mean_ms", wss_.mean() ? Json(*wss_.mean()) : Json(nullptr)},
        {"dc_result_mean_ms", dc_.mean() ? Json(*dc_.mean()) : Json(nullptr)}};
  }

 private:
  static constexpr unsigned kMaxTrialsPerAttempt = 3;
  static constexpr std::int64_t kMockIdleReprobeMs = 60000;
  unsigned failed_trials_{0};
  bool mock_idle_reprobe_used_{false};
  std::optional<std::int64_t> mock_idle_since_;
  std::uint64_t mock_idle_epoch_{0};
  std::int64_t mock_idle_last_ms_{-1};
  bool profile_blocked_dc_{false}, profile_recovery_retry_used_{false};
  struct Health {
    std::array<std::int64_t, NativeControlAckRecovery::kConsecutiveAnomalies> ages{};
    std::size_t count{0}, index{0}; std::int64_t progress{0};
    void reset() { *this = {}; }
    void add(std::int64_t age, std::int64_t now) { ages[index++ % ages.size()] = age; count = std::min(count + 1, ages.size()); progress = now; }
    std::optional<double> mean() const {
      if (count != ages.size()) return std::nullopt;
      double total = 0; for (auto age : ages) total += static_cast<double>(age); return total / static_cast<double>(ages.size());
    }
    bool healthy(std::int64_t now) const { return mean().has_value() && now >= progress && now - progress <= NativeControlAckRecovery::kFreshAckMs; }
  };
  struct Sent { std::uint64_t seq, intent; ControlPath path; Json stamp; std::int64_t started; bool neutral, estop; };
  ControlTransportStamp stamp_;
  bool selecting_{false}, qualifying_{false}, dc_is_p2p_{false};
  std::uint64_t remote_epoch_{0}, revision_{0}, last_result_seq_{0};
  std::int64_t deadline_{0}, last_dc_result_ms_{-1}, first_wss_sent_ms_{-1}, next_trial_ms_{0}, last_wss_result_ms_{-1}, last_wss_result_age_ms_{0};
  std::string trial_attempt_;
  std::deque<Sent> sent_;
  Health wss_, dc_;
  std::optional<double> baseline_wss_ms_;
};

}  // namespace mine_teleop::detail
