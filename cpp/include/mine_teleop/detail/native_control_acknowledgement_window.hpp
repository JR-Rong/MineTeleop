#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
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
    last_ack_packet_age_ms_ = 0;
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
    last_ack_packet_age_ms_ = std::max<std::int64_t>(0, received_monotonic_ms - acknowledged->second);
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
  [[nodiscard]] std::int64_t last_ack_packet_age_ms() const noexcept { return last_ack_packet_age_ms_; }

 private:
  std::deque<std::pair<std::uint64_t, std::int64_t>> pending_;
  std::uint64_t last_sent_sequence_{0};
  std::uint64_t last_ack_sequence_{0};
  std::int64_t progress_monotonic_ms_{0};
  std::int64_t last_ack_packet_age_ms_{0};
};

// A stalled return ACK path withdraws motion before declaring a live socket
// broken. Recovery needs continuing ACKs for recent, actually sent packets.
class NativeControlAckRecovery {
 public:
  static constexpr int kStallMs = 500;
  static constexpr int kFreshAckMs = 200;
  static constexpr int kStableMs = 300;
  struct Transition { bool entered{false}; bool recovered{false}; bool reconnect_due{false}; };

  void reset() { *this = {}; }
  void enter(std::int64_t now, int timeout_ms, std::uint64_t ack_seq) {
    if (waiting_) return;
    waiting_ = true;
    timeout_ms_ = timeout_ms;
    deadline_ = now + timeout_ms;
    observed_ack_seq_ = ack_seq;
    healthy_since_.reset();
  }
  void connection_reopened(std::int64_t now) {
    if (!waiting_) return;
    // This only bounds the new transport attempt. The vehicle keeps the first
    // session recovery deadline across reconnects and readiness changes.
    deadline_ = now + timeout_ms_;
    observed_ack_seq_ = 0;
    healthy_since_.reset();
  }
  Transition observe(std::int64_t now, std::size_t pending, std::int64_t stall_ms,
      std::uint64_t ack_seq, std::int64_t ack_packet_age_ms, int timeout_ms) {
    Transition result;
    if (!waiting_ && pending > 0 && stall_ms >= kStallMs) {
      enter(now, timeout_ms, ack_seq);
      result.entered = true;
    }
    if (!waiting_) return result;
    if (now >= deadline_) { result.reconnect_due = true; return result; }
    if (stall_ms >= kStallMs || ack_packet_age_ms > kFreshAckMs) healthy_since_.reset();
    if (ack_seq > observed_ack_seq_) {
      observed_ack_seq_ = ack_seq;
      if (stall_ms < kStallMs && ack_packet_age_ms <= kFreshAckMs) {
        if (!healthy_since_) healthy_since_ = now;
        else if (now - *healthy_since_ >= kStableMs) {
          waiting_ = false;
          healthy_since_.reset();
          result.recovered = true;
        }
      }
    }
    return result;
  }
  [[nodiscard]] bool waiting() const { return waiting_; }
  [[nodiscard]] std::int64_t deadline() const { return waiting_ ? deadline_ : 0; }
 private:
  bool waiting_{false};
  int timeout_ms_{5000};
  std::int64_t deadline_{0};
  std::uint64_t observed_ack_seq_{0};
  std::optional<std::int64_t> healthy_since_;
};

}  // namespace mine_teleop::detail
