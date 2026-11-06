#pragma once

// Bounded tolerance for transient control-link and video freshness loss.
// Pure logic: no I/O and no locking. The owner serializes every call.
// See docs/64-transient-link-hold-design.md.
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace mine_teleop::detail {

inline constexpr std::string_view kTransientHoldCapability = "transient-hold-v1";

struct TransientHoldConfig {
  bool enabled{false};
  int max_hold_ms{700};
  double hold_brake{0.15};
  int resume_fresh_commands{2};
  int budget_window_ms{60000};
  int max_holds_per_window{6};
  int max_hold_time_per_window_ms{3000};

  void validate() const {
    const auto fail = [](const char* text) { throw std::invalid_argument(text); };
    if (max_hold_ms < 100 || max_hold_ms > 1500) fail("control.transient_hold.max_hold_ms must be 100..1500");
    if (!(hold_brake >= 0.0 && hold_brake <= 0.3)) fail("control.transient_hold.hold_brake must be 0..0.3");
    if (resume_fresh_commands < 1 || resume_fresh_commands > 5)
      fail("control.transient_hold.resume_fresh_commands must be 1..5");
    if (budget_window_ms < 10000 || budget_window_ms > 600000)
      fail("control.transient_hold.budget_window_ms must be 10000..600000");
    if (max_holds_per_window < 1 || max_holds_per_window > 100)
      fail("control.transient_hold.max_holds_per_window must be 1..100");
    if (max_hold_time_per_window_ms < 100 || max_hold_time_per_window_ms > budget_window_ms)
      fail("control.transient_hold.max_hold_time_per_window_ms must be 100..budget_window_ms");
  }
};

enum class TransientCause : unsigned {
  ControlGap = 1U << 0U,
  LateCommands = 1U << 1U,
  DriverVideoStale = 1U << 2U,
  VehicleVideoStale = 1U << 3U,
};

inline constexpr std::string_view to_string(TransientCause cause) {
  switch (cause) {
    case TransientCause::ControlGap: return "control_gap";
    case TransientCause::LateCommands: return "late_commands";
    case TransientCause::DriverVideoStale: return "driver_video_stale";
    case TransientCause::VehicleVideoStale: return "vehicle_video_stale";
  }
  return "unknown";
}

enum class TransientHoldAction { None, Enter, Hold, Resume, Escalate };

class TransientHoldPolicy {
 public:
  explicit TransientHoldPolicy(TransientHoldConfig config = {}) : config_(config) {
    if (config_.enabled) config_.validate();
  }

  [[nodiscard]] const TransientHoldConfig& config() const { return config_; }
  [[nodiscard]] bool active() const { return entered_ms_.has_value(); }
  [[nodiscard]] bool escalated() const { return escalated_; }
  [[nodiscard]] unsigned causes() const { return causes_; }
  [[nodiscard]] std::string_view escalation_reason() const { return last_reason_; }

  TransientHoldAction set_cause(TransientCause cause, bool present, std::int64_t now_ms) {
    if (!config_.enabled || escalated_) return TransientHoldAction::None;
    if (!observe_clock(now_ms)) return escalate(now_ms, "clock_reversed");
    const auto bit = static_cast<unsigned>(cause);
    if (!present) {
      causes_ &= ~bit;
      return active() ? TransientHoldAction::Hold : TransientHoldAction::None;
    }
    causes_ |= bit;
    fresh_commands_ = 0;
    if (active()) return TransientHoldAction::Hold;
    prune(now_ms);
    entered_ms_ = now_ms;
    if (static_cast<int>(history_.size()) + 1 > config_.max_holds_per_window)
      return escalate(now_ms, "budget_count_exceeded");
    if (hold_time_in_window(now_ms) >= config_.max_hold_time_per_window_ms)
      return escalate(now_ms, "budget_time_exceeded");
    return TransientHoldAction::Enter;
  }

  TransientHoldAction note_fresh_command(std::int64_t now_ms) {
    if (!config_.enabled || escalated_ || !active()) return TransientHoldAction::None;
    if (!observe_clock(now_ms)) return escalate(now_ms, "clock_reversed");
    if (causes_ != 0) {
      fresh_commands_ = 0;
      return TransientHoldAction::Hold;
    }
    if (++fresh_commands_ < config_.resume_fresh_commands) return TransientHoldAction::Hold;
    finish(now_ms, "resumed", {});
    return TransientHoldAction::Resume;
  }

  TransientHoldAction tick(std::int64_t now_ms) {
    if (!config_.enabled || escalated_ || !active()) return TransientHoldAction::None;
    if (!observe_clock(now_ms)) return escalate(now_ms, "clock_reversed");
    if (now_ms - *entered_ms_ >= config_.max_hold_ms) return escalate(now_ms, "hold_duration_exceeded");
    if (hold_time_in_window(now_ms) > config_.max_hold_time_per_window_ms)
      return escalate(now_ms, "budget_time_exceeded");
    return TransientHoldAction::Hold;
  }

  // A fault-class cause or a transport epoch change while a hold is active.
  TransientHoldAction escalate_active(std::int64_t now_ms, std::string_view reason) {
    if (!active() || escalated_) return TransientHoldAction::None;
    return escalate(now_ms, reason);
  }

  // The existing braking wait recovered; new holds may start again.
  void escalation_cleared() {
    escalated_ = false;
    causes_ = 0;
    fresh_commands_ = 0;
  }

  void reset() {
    entered_ms_.reset();
    last_now_.reset();
    causes_ = 0;
    fresh_commands_ = 0;
    escalated_ = false;
    history_.clear();
    last_ = nullptr;
    last_reason_.clear();
  }

  [[nodiscard]] nlohmann::json status(std::int64_t now_ms, bool negotiated) const {
    const auto window_start = now_ms - config_.budget_window_ms;
    int holds = active() ? 1 : 0;
    for (const auto& episode : history_)
      if (episode.start_ms + episode.duration_ms >= window_start) ++holds;
    return {{"enabled", config_.enabled && negotiated}, {"active", active()}, {"causes", cause_names(causes_)},
            {"entered_at_monotonic_ms", entered_ms_ ? nlohmann::json(*entered_ms_) : nlohmann::json(nullptr)},
            {"elapsed_ms", entered_ms_ && now_ms >= *entered_ms_ ? nlohmann::json(now_ms - *entered_ms_) : nlohmann::json(nullptr)},
            {"max_hold_ms", config_.max_hold_ms}, {"hold_brake", config_.hold_brake},
            {"holds_in_window", holds}, {"hold_time_in_window_ms", hold_time_in_window(now_ms)},
            {"budget_window_ms", config_.budget_window_ms}, {"escalated", escalated_}, {"last", last_}};
  }

 private:
  struct Episode {
    std::int64_t start_ms;
    std::int64_t duration_ms;
  };

  static nlohmann::json cause_names(unsigned causes) {
    auto names = nlohmann::json::array();
    for (const auto cause : {TransientCause::ControlGap, TransientCause::LateCommands,
                             TransientCause::DriverVideoStale, TransientCause::VehicleVideoStale})
      if (causes & static_cast<unsigned>(cause)) names.push_back(std::string(to_string(cause)));
    return names;
  }

  bool observe_clock(std::int64_t now_ms) {
    if (now_ms < 0 || (last_now_ && now_ms < *last_now_)) return false;
    last_now_ = now_ms;
    return true;
  }

  void prune(std::int64_t now_ms) {
    const auto window_start = now_ms - config_.budget_window_ms;
    while (!history_.empty() && history_.front().start_ms + history_.front().duration_ms < window_start)
      history_.pop_front();
  }

  [[nodiscard]] std::int64_t hold_time_in_window(std::int64_t now_ms) const {
    const auto window_start = now_ms - config_.budget_window_ms;
    std::int64_t total = 0;
    for (const auto& episode : history_) {
      const auto begin = std::max(episode.start_ms, window_start);
      const auto end = episode.start_ms + episode.duration_ms;
      if (end > begin) total += end - begin;
    }
    if (entered_ms_ && now_ms > *entered_ms_) total += now_ms - std::max(*entered_ms_, window_start);
    return total;
  }

  TransientHoldAction escalate(std::int64_t now_ms, std::string_view reason) {
    if (!entered_ms_) entered_ms_ = last_now_.value_or(std::max<std::int64_t>(0, now_ms));
    finish(std::max(now_ms, *entered_ms_), "escalated", reason);
    escalated_ = true;
    return TransientHoldAction::Escalate;
  }

  void finish(std::int64_t now_ms, std::string_view outcome, std::string_view reason) {
    const auto start = *entered_ms_;
    const auto duration = std::max<std::int64_t>(0, now_ms - start);
    last_ = {{"causes", cause_names(causes_)}, {"duration_ms", duration}, {"outcome", std::string(outcome)},
             {"escalation_reason", reason.empty() ? nlohmann::json(nullptr) : nlohmann::json(std::string(reason))}};
    last_reason_ = std::string(reason);
    history_.push_back({start, duration});
    while (static_cast<int>(history_.size()) > config_.max_holds_per_window + 1) history_.pop_front();
    entered_ms_.reset();
    causes_ = 0;
    fresh_commands_ = 0;
  }

  TransientHoldConfig config_;
  std::optional<std::int64_t> entered_ms_;
  std::optional<std::int64_t> last_now_;
  unsigned causes_{0};
  int fresh_commands_{0};
  bool escalated_{false};
  std::deque<Episode> history_;
  nlohmann::json last_ = nullptr;
  std::string last_reason_;
};

struct TransientHoldOffer {
  int control_tolerance_ms{0};
  int video_tolerance_ms{0};
  int max_hold_ms{0};
};

// Driver-side admission of the vehicle capability. Anything unexpected keeps
// both endpoints on the existing behavior.
inline std::optional<TransientHoldOffer> parse_transient_hold_offer(const nlohmann::json& payload) {
  try {
    if (!payload.is_object() || !payload.contains("transient_hold_capability") ||
        !payload.at("transient_hold_capability").is_string() ||
        payload.at("transient_hold_capability").get_ref<const std::string&>() != kTransientHoldCapability ||
        !payload.contains("transient_hold") || !payload.at("transient_hold").is_object())
      return std::nullopt;
    const auto& details = payload.at("transient_hold");
    const auto integer = [&](const char* key, std::int64_t low, std::int64_t high) -> std::optional<int> {
      if (!details.contains(key) || !details.at(key).is_number_integer()) return std::nullopt;
      const auto value = details.at(key).get<std::int64_t>();
      if (value < low || value > high) return std::nullopt;
      return static_cast<int>(value);
    };
    const auto control = integer("control_tolerance_ms", 400, 2000);
    const auto video = integer("video_tolerance_ms", 1, 4500);
    const auto hold = integer("max_hold_ms", 100, 1500);
    if (!control || !video || !hold) return std::nullopt;
    return TransientHoldOffer{*control, *video, *hold};
  } catch (const nlohmann::json::exception&) {
    return std::nullopt;
  }
}

enum class VehicleVideoLaneState { Fresh, Stale, Fault };

// Stale or IDR-wait drops are transient; invalid samples and lane faults are not.
inline VehicleVideoLaneState classify_vehicle_video_lane(bool disabled, bool faulted, bool has_freshness,
    bool invalid_sample_blocked, bool encoded_fresh, unsigned consecutive_anomalies, bool awaiting_recovery_frames) {
  if (disabled || faulted || !has_freshness || invalid_sample_blocked) return VehicleVideoLaneState::Fault;
  if (!encoded_fresh || consecutive_anomalies != 0 || awaiting_recovery_frames) return VehicleVideoLaneState::Stale;
  return VehicleVideoLaneState::Fresh;
}

}  // namespace mine_teleop::detail
