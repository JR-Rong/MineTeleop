#pragma once
#include "common.hpp"

#include <algorithm>
#include <limits>
#include <span>

namespace mine_teleop::path_selection_models {
struct FeedbackSelection {
  Authority authority;
  std::uint64_t from_epoch{}, epoch{}, status_floor{};
  Route route{Route::Wss};
  std::string media_attempt;
  Preconditions prerequisites;
};
struct Status {
  Authority authority;
  std::uint64_t feedback_epoch{}, seq{};
  Route route{Route::Wss};
  std::string media_attempt;
  unsigned kind{}; // The four existing status kinds, never a ControlCommand.
  Preconditions prerequisites;
};
class FeedbackFence {
 public:
  explicit FeedbackFence(Authority authority) : authority_(std::move(authority)) {}
  bool select(const FeedbackSelection& value) {
    if (!authority_.valid() || value.authority != authority_ || !value.prerequisites.satisfied() ||
        value.media_attempt.size() > 128 || (value.route == Route::Dc && value.media_attempt.empty())) return false;
    if (last_selection_ && value.epoch == epoch_ && value.from_epoch == last_selection_->from_epoch &&
        value.route == route_ && value.media_attempt == attempt_ && value.status_floor == floor_) return true;
    if (epoch_ == std::numeric_limits<std::uint64_t>::max() || value.from_epoch != epoch_ || value.epoch != epoch_ + 1 ||
        value.status_floor < last_seq_) return false;
    epoch_ = value.epoch; route_ = value.route; attempt_ = value.media_attempt;
    floor_ = value.status_floor; last_seq_ = floor_; last_selection_ = value; return true;
  }
  bool eligible(const Status& value) const {
    return authority_.valid() && value.authority == authority_ && value.prerequisites.satisfied() &&
        value.feedback_epoch == epoch_ && value.route == route_ && value.media_attempt == attempt_ &&
        value.kind < 4 && value.seq > floor_;
  }
  bool accept(const Status& value) {
    if (!eligible(value) || value.seq <= last_seq_) return false;
    last_seq_ = value.seq; return true;
  }
  std::uint64_t epoch() const { return epoch_; }
  std::uint64_t watermark() const { return last_seq_; }
 private:
  Authority authority_;
  std::uint64_t epoch_{}, floor_{}, last_seq_{};
  Route route_{Route::Wss};
  std::string attempt_;
  std::optional<FeedbackSelection> last_selection_;
};
// Coalesce four kinds BEFORE advancing the UI's global watermark. Reject old
// epochs before they can evict a valid current snapshot with a higher seq.
inline std::optional<std::array<std::optional<Status>, 4>> latest_status_batch(
    const FeedbackFence& fence, std::span<const Status> incoming) {
  if (incoming.size() > 16) return std::nullopt; // Explicit fixture bound, not a runtime limit.
  std::array<std::optional<Status>, 4> latest;
  for (const auto& value : incoming) if (fence.eligible(value) &&
      (!latest[value.kind] || value.seq > latest[value.kind]->seq)) latest[value.kind] = value;
  std::sort(latest.begin(), latest.end(), [](const auto& left, const auto& right) {
    if (!left) return false;
    return !right || left->seq < right->seq;
  });
  return latest;
}

class PairedReturnProbe {
 public:
  struct Reply {
    Authority authority;
    std::uint64_t cohort{}, nonce{}, produced_seq{};
    Digest same_produced_event{};
    Route return_route{Route::Wss};
    std::string media_attempt;
    Preconditions prerequisites;
  };
  struct Sample { std::int64_t dc_round_trip_ms, wss_round_trip_ms, dc_minus_wss_ms; };
  PairedReturnProbe(Authority authority, std::uint64_t cohort) : authority_(std::move(authority)), cohort_(cohort) {}
  bool begin(std::uint64_t nonce, std::int64_t started, std::int64_t deadline, std::string attempt) {
    if (!authority_.valid() || !cohort_ || !nonce || nonce <= last_nonce_ || started < 0 || deadline <= started ||
        attempt.empty() || attempt.size() > 128 || started < last_time_ || (pending_ && started <= pending_->deadline)) return false;
    pending_ = Pending{nonce, started, deadline, std::move(attempt), {}, {}, {}, {}};
    last_nonce_ = nonce; last_time_ = started; return true;
  }
  std::optional<Sample> receive(const Reply& reply, std::int64_t received) {
    if (!pending_ || reply.authority != authority_ || reply.cohort != cohort_ || reply.nonce != pending_->nonce ||
        reply.media_attempt != pending_->attempt || !reply.prerequisites.satisfied() || !reply.produced_seq ||
        received < pending_->started || received < last_time_ || received > pending_->deadline) return {};
    auto& age = reply.return_route == Route::Dc ? pending_->dc_age : pending_->wss_age;
    if (age) return {}; // Duplicate can't add progress or replace the first arrival.
    if (pending_->produced && (*pending_->produced != reply.produced_seq || pending_->digest != reply.same_produced_event)) return {};
    pending_->produced = reply.produced_seq; pending_->digest = reply.same_produced_event;
    last_time_ = received;
    age = received - pending_->started;
    if (!pending_->dc_age || !pending_->wss_age) return {};
    const Sample result{*pending_->dc_age, *pending_->wss_age, *pending_->dc_age - *pending_->wss_age};
    pending_.reset(); ++completed_; return result;
  }
  std::uint64_t completed() const { return completed_; }
 private:
  struct Pending {
    std::uint64_t nonce; std::int64_t started, deadline; std::string attempt;
    std::optional<std::uint64_t> produced; Digest digest;
    std::optional<std::int64_t> dc_age, wss_age;
  };
  Authority authority_;
  std::uint64_t cohort_{}, last_nonce_{}, completed_{};
  std::int64_t last_time_{-1};
  std::optional<Pending> pending_;
};
}  // namespace mine_teleop::path_selection_models
