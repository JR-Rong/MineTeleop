#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace mine_teleop::detail {

// Diagnostic only. Cloud ingress ACKs must never retire these identities.
// The caller authenticates session/vehicle/driver and serializes access. Keep
// this window across same-session socket reconnects; reset for a new session.
class VehicleResultLatencyWindow {
 public:
  static constexpr std::size_t kCapacity = 128;
  struct Sample {
    std::uint64_t command_seq;
    std::uint64_t intent_seq;
    std::int64_t round_trip_ms;
    std::int64_t received_monotonic_ms;
    bool accepted;
  };
  struct Snapshot {
    std::optional<Sample> last;
    std::size_t retained_sends;
    std::uint64_t samples;
    std::uint64_t evicted_sends;
    std::uint64_t uncorrelated_results;
    std::uint64_t invalid_timing;
  };

  void reset() noexcept { *this = {}; }

  // Call after a successful socket write, supplying its START time so the
  // measurement includes write duration. Fixed storage cannot fail a send or
  // ESTOP because diagnostic bookkeeping is full.
  void note_sent(std::uint64_t seq, std::uint64_t intent_seq,
      std::int64_t send_started_monotonic_ms) noexcept {
    if (seq == 0 || intent_seq == 0 || seq <= last_sent_seq_ || send_started_monotonic_ms < 0) return;
    if (size_ == kCapacity) {
      pop_front();
      ++evicted_sends_;
    }
    sends_[(head_ + size_) % kCapacity] = {seq, intent_seq, send_started_monotonic_ms};
    ++size_;
    last_sent_seq_ = seq;
  }

  // Only an exact sent command AND intent can produce a sample. Results for
  // superseded/evicted sends stay unknown; duplicates cannot renew progress.
  // accepted means a software control-service result, never physical CAN ACK.
  std::optional<Sample> note_result(std::uint64_t seq, std::uint64_t intent_seq,
      std::int64_t received_monotonic_ms, bool accepted) noexcept {
    if (last_ && seq <= last_->command_seq) return std::nullopt;
    std::size_t index = 0;
    for (; index < size_; ++index) {
      const auto& sent = sends_[(head_ + index) % kCapacity];
      if (sent.seq != seq) continue;
      if (sent.intent_seq != intent_seq) break;
      if (received_monotonic_ms < sent.started_ms) {
        ++invalid_timing_;
        return std::nullopt;
      }
      last_ = Sample{seq, intent_seq, received_monotonic_ms - sent.started_ms,
          received_monotonic_ms, accepted};
      ++samples_;
      // Latest-mailbox semantics allow earlier sends to have been superseded.
      // This does not infer any received/accepted result for those sequences.
      for (std::size_t count = 0; count <= index; ++count) pop_front();
      return last_;
    }
    ++uncorrelated_results_;
    return std::nullopt;
  }

  [[nodiscard]] Snapshot snapshot() const noexcept {
    return {last_, size_, samples_, evicted_sends_, uncorrelated_results_, invalid_timing_};
  }

 private:
  struct Sent { std::uint64_t seq{0}; std::uint64_t intent_seq{0}; std::int64_t started_ms{0}; };
  void pop_front() noexcept { head_ = (head_ + 1) % kCapacity; --size_; }
  std::array<Sent, kCapacity> sends_{};
  std::size_t head_{0};
  std::size_t size_{0};
  std::uint64_t last_sent_seq_{0};
  std::optional<Sample> last_;
  std::uint64_t samples_{0};
  std::uint64_t evicted_sends_{0};
  std::uint64_t uncorrelated_results_{0};
  std::uint64_t invalid_timing_{0};
};

}  // namespace mine_teleop::detail
