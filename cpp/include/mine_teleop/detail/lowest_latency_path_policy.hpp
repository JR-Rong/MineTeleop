#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>

namespace mine_teleop::detail {

enum class NetworkPath { P2p, TurnUdp, TurnTcp, TurnTls, TurnUnknown, CloudWss };
enum class PathMeasurement {
  ControlVehicleResultRoundTrip, VideoCaptureToPresentation, FeedbackProducedToReceived,
  CloudIngressRoundTrip, IceRoundTrip
};

// An advisory policy for a future negotiated transport integration. It never
// sends, changes ICE nomination, grants authority or marks a path selected.
// Health includes the caller's existing loss/readiness/clock/security gates;
// selectable means the transport can actually select this route, not just that
// an ICE check succeeded. No guessed/default safety or switching thresholds.
class LowestLatencyPathPolicy {
 public:
  struct Config {
    PathMeasurement measurement;
    std::int64_t max_sample_age_ms;
    std::uint64_t min_samples;
    double improvement_ms;
    double improvement_fraction;
    std::int64_t stable_for_ms;
    std::int64_t min_dwell_ms;
  };
  struct Observation {
    NetworkPath path;
    std::uint64_t session_generation;
    PathMeasurement measurement;
    bool healthy;
    bool selectable;
    std::uint64_t transport_attempt;
    std::uint64_t samples;
    std::int64_t progress_monotonic_ms;
    double latency_ms;
  };
  enum class Reason { NoHealthyPath, KeepCurrent, InitialPath, UnhealthyFallback, Stabilizing, BetterPath, InvalidSnapshot };
  struct Recommendation { std::optional<NetworkPath> path; Reason reason; };

  explicit LowestLatencyPathPolicy(Config config) : config_(config) {
    if (config.max_sample_age_ms <= 0 || config.min_samples == 0 ||
        !std::isfinite(config.improvement_ms) || config.improvement_ms < 0 ||
        !std::isfinite(config.improvement_fraction) || config.improvement_fraction < 0 ||
        config.improvement_fraction > 1 || config.stable_for_ms < 0 || config.min_dwell_ms < 0 ||
        config.measurement == PathMeasurement::CloudIngressRoundTrip ||
        config.measurement == PathMeasurement::IceRoundTrip)
      throw std::invalid_argument("path policy requires explicit end-to-end measurement and valid budgets");
  }

  void reset(std::uint64_t session_generation) noexcept {
    generation_ = session_generation;
    current_.reset();
    challenger_.reset();
    selected_at_ms_ = last_observed_ms_ = 0;
    current_attempt_ = challenger_attempt_ = 0;
  }

  // Call ONLY after the caller has completed the negotiated epoch barrier and
  // required safety recovery. Recommendations alone do not switch transport.
  bool confirm_selected(NetworkPath path, std::uint64_t session_generation,
      std::uint64_t transport_attempt, std::int64_t now) noexcept {
    if (generation_ == 0 || session_generation != generation_ || transport_attempt == 0 ||
        now < 0 || now < last_observed_ms_) return false;
    current_ = path;
    current_attempt_ = transport_attempt;
    selected_at_ms_ = now;
    challenger_.reset();
    return true;
  }

  Recommendation recommend(std::int64_t now, std::span<const Observation> observations) noexcept {
    if (generation_ == 0 || now < 0 || now < last_observed_ms_ || observations.size() > 6) {
      challenger_.reset();
      return {std::nullopt, Reason::InvalidSnapshot};
    }
    last_observed_ms_ = now;
    const Observation* active = nullptr;
    const Observation* best = nullptr;
    for (std::size_t index = 0; index < observations.size(); ++index) {
      const auto& value = observations[index];
      for (std::size_t prior = 0; prior < index; ++prior) {
        if (observations[prior].path == value.path) {
          challenger_.reset();
          return {std::nullopt, Reason::InvalidSnapshot};
        }
      }
      if (!usable(value, now)) continue;
      if (current_ == value.path && current_attempt_ == value.transport_attempt) active = &value;
      if (!best || value.latency_ms < best->latency_ms ||
          (value.latency_ms == best->latency_ms && current_ == value.path)) best = &value;
    }
    if (!best) { challenger_.reset(); return {std::nullopt, Reason::NoHealthyPath}; }
    if (!active) {
      challenger_.reset();
      return {best->path, current_ ? Reason::UnhealthyFallback : Reason::InitialPath};
    }
    const auto improvement = active->latency_ms - best->latency_ms;
    if (best->path == active->path || improvement <= 0 || improvement < config_.improvement_ms ||
        improvement < active->latency_ms * config_.improvement_fraction) {
      challenger_.reset();
      return {active->path, Reason::KeepCurrent};
    }
    if (challenger_ != best->path || challenger_attempt_ != best->transport_attempt) {
      challenger_ = best->path;
      challenger_attempt_ = best->transport_attempt;
      challenger_since_ms_ = now;
      challenger_first_progress_ms_ = best->progress_monotonic_ms;
    }
    // Repeated reads of one sample do not establish a stable improving path.
    if (now < selected_at_ms_ || now - selected_at_ms_ < config_.min_dwell_ms ||
        now - challenger_since_ms_ < config_.stable_for_ms ||
        best->progress_monotonic_ms <= challenger_first_progress_ms_)
      return {active->path, Reason::Stabilizing};
    return {best->path, Reason::BetterPath};
  }

 private:
  bool usable(const Observation& value, std::int64_t now) const noexcept {
    return value.session_generation == generation_ && value.measurement == config_.measurement &&
        value.healthy && value.selectable && value.transport_attempt > 0 && value.samples >= config_.min_samples &&
        value.progress_monotonic_ms >= 0 && value.progress_monotonic_ms <= now &&
        now - value.progress_monotonic_ms <= config_.max_sample_age_ms &&
        std::isfinite(value.latency_ms) && value.latency_ms >= 0;
  }
  Config config_;
  std::uint64_t generation_{0};
  std::optional<NetworkPath> current_;
  std::optional<NetworkPath> challenger_;
  std::uint64_t current_attempt_{0};
  std::uint64_t challenger_attempt_{0};
  std::int64_t selected_at_ms_{0};
  std::int64_t last_observed_ms_{0};
  std::int64_t challenger_since_ms_{0};
  std::int64_t challenger_first_progress_ms_{0};
};

}  // namespace mine_teleop::detail
