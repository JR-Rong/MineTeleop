#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <stdexcept>
#include <utility>

namespace mine_teleop::detail {

// ACK liveness follows cumulative sequence progress, independently of packet
// latency. Retain oldest-packet timing for diagnostics and bound the backlog.
class NativeControlAcknowledgementWindow {
 public:
  static constexpr std::size_t kMaxPendingPackets = 128;

  void reset() noexcept {
    pending_.clear();
    last_sent_sequence_ = 0;
    last_ack_sequence_ = 0;
    progress_monotonic_ms_ = 0;
  }

  void note_sent(std::uint64_t sequence, std::int64_t sent_monotonic_ms) {
    if (sequence == 0 || sequence <= last_sent_sequence_) {
      throw std::invalid_argument(
          "native control acknowledgement sequences must increase");
    }
    if (pending_.size() >= kMaxPendingPackets) {
      throw std::runtime_error("native control acknowledgement backlog exceeded 128 packets");
    }
    if (pending_.empty()) progress_monotonic_ms_ = sent_monotonic_ms;
    pending_.emplace_back(sequence, sent_monotonic_ms);
    last_sent_sequence_ = sequence;
  }

  // Duplicate or regressing ACKs cannot keep a stalled connection alive.
  bool acknowledge_through(std::uint64_t sequence, std::int64_t received_monotonic_ms) {
    if (sequence == 0 || sequence > last_sent_sequence_) {
      throw std::invalid_argument(
          "native control signaling acknowledgement sequence is invalid");
    }
    if (sequence <= last_ack_sequence_) return false;
    const auto acknowledged = std::lower_bound(
        pending_.begin(), pending_.end(), sequence,
        [](const auto& packet, std::uint64_t seq) { return packet.first < seq; });
    if (acknowledged == pending_.end() || acknowledged->first != sequence) {
      throw std::invalid_argument(
          "native control signaling acknowledgement sequence is invalid");
    }
    while (!pending_.empty() && pending_.front().first <= sequence) {
      pending_.pop_front();
    }
    last_ack_sequence_ = sequence;
    progress_monotonic_ms_ = received_monotonic_ms;
    return true;
  }

  [[nodiscard]] std::int64_t stall_age_ms(std::int64_t now_monotonic_ms) const noexcept {
    if (pending_.empty() || now_monotonic_ms <= progress_monotonic_ms_) return 0;
    return now_monotonic_ms - progress_monotonic_ms_;
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
  std::uint64_t last_sent_sequence_{0};
  std::uint64_t last_ack_sequence_{0};
  std::int64_t progress_monotonic_ms_{0};
};

}  // namespace mine_teleop::detail
