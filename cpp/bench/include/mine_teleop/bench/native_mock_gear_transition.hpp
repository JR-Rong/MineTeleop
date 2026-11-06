#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <optional>

namespace mine_teleop::detail {

// The caller serializes this state with session ownership and delivers only
// authenticated status events for the current session and media attempt.
class NativeMockGearTransition {
 public:
  enum class Rejection { unmatched, matched };
  struct Pending { char from, to; std::uint64_t status_floor, generation; };

  void reset() { *this = {}; }
  bool observe_status(std::uint64_t status) {
    if (!safe(status) || status <= last_status_) return false;
    last_status_ = status;
    return true;
  }
  std::optional<std::uint64_t> begin(char from, char to) {
    if (!valid_gear(from) || !valid_gear(to) || from == to || !last_status_ || pending_ || !safe(generation_ + 1))
      return std::nullopt;
    pending_ = Pending{from, to, last_status_, ++generation_};
    forwarded_.clear();
    return generation_;
  }
  void forwarded(std::uint64_t generation, std::uint64_t intent, char gear) {
    if (!pending_ || pending_->generation != generation || pending_->to != gear || !safe(intent) ||
        std::find(forwarded_.begin(), forwarded_.end(), intent) != forwarded_.end()) return;
    if (forwarded_.size() == 64) forwarded_.pop_front();
    forwarded_.push_back(intent);
  }
  bool confirm(std::uint64_t status, char actual_gear, bool fresh_feedback) {
    if (!pending_ || status != last_status_ || status <= pending_->status_floor ||
        forwarded_.empty() || !fresh_feedback || actual_gear != pending_->to) return false;
    pending_.reset(); forwarded_.clear();
    return true;
  }
  Rejection reject(std::uint64_t status, std::uint64_t intent) {
    if (pending_ && status == last_status_ && status > pending_->status_floor && safe(intent) &&
        std::find(forwarded_.begin(), forwarded_.end(), intent) != forwarded_.end()) return Rejection::matched;
    return Rejection::unmatched;
  }
  void cancel() { pending_.reset(); forwarded_.clear(); }
  [[nodiscard]] std::optional<Pending> pending() const { return pending_; }
 private:
  static bool valid_gear(char gear) { return gear == 'N' || gear == 'D' || gear == 'R'; }
  static bool safe(std::uint64_t value) { return value > 0 && value <= 9007199254740991ULL; }
  std::uint64_t last_status_{0}, generation_{0};
  std::optional<Pending> pending_;
  std::deque<std::uint64_t> forwarded_;
};
}  // namespace mine_teleop::detail
