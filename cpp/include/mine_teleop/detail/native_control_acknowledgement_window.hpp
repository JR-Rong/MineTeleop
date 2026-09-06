#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <utility>

namespace mine_teleop::detail {

// Tracks the age of the oldest packet that was actually sent and has not yet
// been acknowledged. A moving pipeline must not inherit the age of packets
// that the cloud has already acknowledged.
class NativeControlAcknowledgementWindow {
 public:
  void reset() noexcept { pending_.clear(); }

  void note_sent(std::uint64_t sequence, std::int64_t sent_monotonic_ms) {
    if (sequence == 0 ||
        (!pending_.empty() && sequence <= pending_.back().first)) {
      throw std::invalid_argument(
          "native control acknowledgement sequences must increase");
    }
    pending_.emplace_back(sequence, sent_monotonic_ms);
  }

  void acknowledge_through(std::uint64_t sequence) noexcept {
    while (!pending_.empty() && pending_.front().first <= sequence) {
      pending_.pop_front();
    }
  }

  [[nodiscard]] std::uint64_t oldest_sequence() const noexcept {
    return pending_.empty() ? 0 : pending_.front().first;
  }

  [[nodiscard]] std::int64_t oldest_age_ms(
      std::int64_t now_monotonic_ms) const noexcept {
    if (pending_.empty() || now_monotonic_ms <= pending_.front().second) {
      return 0;
    }
    return now_monotonic_ms - pending_.front().second;
  }

  [[nodiscard]] std::size_t pending_count() const noexcept {
    return pending_.size();
  }

 private:
  std::deque<std::pair<std::uint64_t, std::int64_t>> pending_;
};

}  // namespace mine_teleop::detail
