#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
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
    retired_through_sequence_ = 0;
    retired_oldest_sequence_ = 0;
    retired_oldest_monotonic_ms_ = 0;
  }

  void note_sent(std::uint64_t sequence, std::int64_t sent_monotonic_ms,
      bool safety_packet = false) {
    if (sequence == 0 || sequence <= last_sent_sequence_) {
      throw std::invalid_argument(
          "native control acknowledgement sequences must increase");
    }
    if (pending_.size() >= kMaxPendingPackets) {
      if (!safety_packet) {
        throw std::runtime_error("native control acknowledgement backlog exceeded 128 packets");
      }
      // An ESTOP must not turn a successful socket write into a fabricated
      // transport failure. Retain exactly 128 sent identities; older records
      // become a conservative diagnostic summary, never ACK liveness evidence.
      // This also works when skipped local send attempts leave sequence gaps.
      if (retired_oldest_sequence_ == 0) {
        retired_oldest_sequence_ = pending_.front().first;
        retired_oldest_monotonic_ms_ = pending_.front().second;
      }
      retired_through_sequence_ = pending_.front().first;
      pending_.pop_front();
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
    // Retired ACKs (including holes in that old range) cannot prove that a
    // currently retained packet arrived, renew a stalled path or clear faults.
    if (sequence <= retired_through_sequence_) return false;
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
    retired_through_sequence_ = 0;
    retired_oldest_sequence_ = 0;
    retired_oldest_monotonic_ms_ = 0;
    last_ack_sequence_ = sequence;
    progress_monotonic_ms_ = received_monotonic_ms;
    return true;
  }

  [[nodiscard]] std::int64_t stall_age_ms(std::int64_t now_monotonic_ms) const noexcept {
    if (pending_.empty() || now_monotonic_ms <= progress_monotonic_ms_) return 0;
    return now_monotonic_ms - progress_monotonic_ms_;
  }

  [[nodiscard]] std::uint64_t oldest_sequence() const noexcept {
    if (retired_oldest_sequence_ != 0) return retired_oldest_sequence_;
    return pending_.empty() ? 0 : pending_.front().first;
  }

  [[nodiscard]] std::int64_t oldest_age_ms(
      std::int64_t now_monotonic_ms) const noexcept {
    if (pending_.empty()) return 0;
    const auto oldest_sent = retired_oldest_sequence_ != 0
        ? retired_oldest_monotonic_ms_ : pending_.front().second;
    if (now_monotonic_ms <= oldest_sent) {
      return 0;
    }
    return now_monotonic_ms - oldest_sent;
  }

  [[nodiscard]] std::size_t pending_count() const noexcept {
    return pending_.size();
  }
  [[nodiscard]] std::int64_t last_ack_packet_age_ms() const noexcept { return last_ack_packet_age_ms_; }
  [[nodiscard]] std::uint64_t retired_through_sequence() const noexcept { return retired_through_sequence_; }

 private:
  std::deque<std::pair<std::uint64_t, std::int64_t>> pending_;
  std::uint64_t last_sent_sequence_{0};
  std::uint64_t last_ack_sequence_{0};
  std::int64_t progress_monotonic_ms_{0};
  std::int64_t last_ack_packet_age_ms_{0};
  std::uint64_t retired_through_sequence_{0};
  std::uint64_t retired_oldest_sequence_{0};
  std::int64_t retired_oldest_monotonic_ms_{0};
};

// ACK packet-age anomalies are counted per advancing received sample. A stalled
// return path is a separate absence timeout. The vehicle owns the common 300ms
// healthy window and release/repress interlock across all readiness categories.
class NativeControlAckRecovery {
 public:
  static constexpr int kStallMs = 500;
  static constexpr int kFreshAckMs = 200;
  static constexpr unsigned kConsecutiveAnomalies = 3;
  struct Transition { bool entered{false}; bool recovered{false}; bool reconnect_due{false}; };

  // Only a new control-session generation resets event history. The receiver
  // must fence that generation; any same-session revision change (even a
  // decrease) is conservative evidence that its healthy window must restart.
  void reset() { *this = {}; }
  void enter(std::int64_t now, int timeout_ms, std::uint64_t ack_seq) {
    if (waiting_) return;
    advance_revision(brake_sample_revision_);
    waiting_ = true;
    timeout_ms_ = timeout_ms;
    deadline_ = now + timeout_ms;
    observed_ack_seq_ = ack_seq;
  }
  void connection_reopened(std::int64_t now) {
    if (!waiting_) return;
    // This bounds only a transport attempt with no ACK progress. Recoverable
    // readiness samples never force a new session merely for lasting 5s.
    deadline_ = now + timeout_ms_;
    observed_ack_seq_ = 0;
    consecutive_anomalies_ = 0;
    // Reopening a socket within the same session never erases fault history.
    // A fresh ACK may restore this category, but its next carrier must still
    // report the brake/bad events which happened before transport recovery.
  }
  // The caller must first prove this ACK advanced an actually sent identity.
  // Duplicate/regressing/retired ACKs must not be supplied as normal samples.
  Transition note_ack_progress(std::int64_t now, std::uint64_t ack_seq,
      std::int64_t ack_packet_age_ms, int timeout_ms) {
    Transition result;
    if (ack_seq == 0 || ack_seq <= observed_ack_seq_) return result;
    observed_ack_seq_ = ack_seq;
    if (ack_packet_age_ms > kFreshAckMs) {
      advance_revision(bad_sample_revision_);
      consecutive_anomalies_ = std::min(consecutive_anomalies_ + 1, kConsecutiveAnomalies);
      if (!waiting_ && consecutive_anomalies_ >= kConsecutiveAnomalies) {
        enter(now, timeout_ms, ack_seq);
        result.entered = true;
      }
    } else {
      consecutive_anomalies_ = 0;
      if (waiting_) {
        waiting_ = false;
        result.recovered = true;
      }
    }
    if (waiting_) deadline_ = now + timeout_ms_;
    return result;
  }
  Transition observe(std::int64_t now, std::size_t pending, std::int64_t stall_ms,
      std::uint64_t ack_seq, std::int64_t ack_packet_age_ms, int timeout_ms) {
    Transition result;
    if (ack_seq > observed_ack_seq_) {
      result = note_ack_progress(now, ack_seq, ack_packet_age_ms, timeout_ms);
    }
    if (!waiting_ && pending > 0 && stall_ms >= kStallMs) {
      enter(now, timeout_ms, ack_seq);
      result.entered = true;
    }
    if (!waiting_) return result;
    if (stall_ms >= kStallMs && now >= deadline_) result.reconnect_due = true;
    return result;
  }
  [[nodiscard]] bool waiting() const { return waiting_; }
  [[nodiscard]] std::int64_t deadline() const { return waiting_ ? deadline_ : 0; }
  [[nodiscard]] std::uint64_t sample_sequence() const { return observed_ack_seq_; }
  [[nodiscard]] unsigned consecutive_anomalies() const { return consecutive_anomalies_; }
  [[nodiscard]] std::uint64_t bad_sample_revision() const { return bad_sample_revision_; }
  [[nodiscard]] std::uint64_t brake_sample_revision() const { return brake_sample_revision_; }
  [[nodiscard]] bool latest_sample_healthy() const {
    return observed_ack_seq_ > 0 && consecutive_anomalies_ == 0 && !waiting_;
  }
 private:
  static void advance_revision(std::uint64_t& revision) noexcept {
    if (revision != std::numeric_limits<std::uint64_t>::max()) ++revision;
  }
  bool waiting_{false};
  int timeout_ms_{5000};
  std::int64_t deadline_{0};
  std::uint64_t observed_ack_seq_{0};
  unsigned consecutive_anomalies_{0};
  std::uint64_t bad_sample_revision_{0};
  std::uint64_t brake_sample_revision_{0};
};

}  // namespace mine_teleop::detail
