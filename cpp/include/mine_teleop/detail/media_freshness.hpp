#pragma once

#include <chrono>
#include <limits>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace mine_teleop::detail {

inline std::optional<std::int64_t> checked_device_timestamp_us(
    std::int64_t seconds, std::int64_t microseconds) {
  if (seconds < 0 || microseconds < 0 || microseconds >= 1000000 ||
      seconds > (std::numeric_limits<std::int64_t>::max() - microseconds) / 1000000) return std::nullopt;
  return seconds * 1000000 + microseconds;
}

struct CaptureSteadyTime {
  std::chrono::steady_clock::time_point value;
  bool used_device_timestamp{false};
};
inline CaptureSteadyTime select_capture_steady_time(
    std::chrono::steady_clock::time_point read_started,
    std::chrono::steady_clock::time_point read_finished,
    std::optional<std::int64_t> device_monotonic_us) {
  using Clock = std::chrono::steady_clock;
  const auto largest_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::duration::max()).count();
  if (device_monotonic_us && *device_monotonic_us > 0 && *device_monotonic_us <= largest_us) {
    const auto device_time = Clock::time_point(std::chrono::duration_cast<Clock::duration>(
        std::chrono::microseconds(*device_monotonic_us)));
    if (device_time <= read_finished) return {device_time, true};
  }
  return {read_started, false};
}

enum class AccessUnitCodec { H264, H265 };

struct AccessUnitInfo {
  bool valid{false};
  bool picture{false};
  bool idr{false};
  bool parameter_sets{false};
};

// The production parser provides byte-stream, AU-aligned buffers. A non-delta
// GstBuffer is insufficient: H.265 CRA pictures can retain leading references.
// Resuming a discarded reference chain requires an IDR with its codec headers.
inline AccessUnitInfo inspect_access_unit(
    AccessUnitCodec codec, std::span<const std::uint8_t> bytes) {
  AccessUnitInfo result;
  unsigned parameter_sets = 0;
  bool non_idr_picture = false;
  std::size_t offset = 0;
  const auto start_code = [&](std::size_t at) -> std::size_t {
    if (at + 3 <= bytes.size() && bytes[at] == 0 && bytes[at + 1] == 0) {
      if (bytes[at + 2] == 1) return 3;
      if (at + 4 <= bytes.size() && bytes[at + 2] == 0 && bytes[at + 3] == 1) return 4;
    }
    return 0;
  };
  while (offset < bytes.size()) {
    const auto prefix = start_code(offset);
    if (prefix == 0) return {};
    const auto begin = offset + prefix;
    auto end = begin;
    while (end < bytes.size() && start_code(end) == 0) ++end;
    if (begin == end || (bytes[begin] & 0x80) != 0) return {};
    if (codec == AccessUnitCodec::H264) {
      const auto type = bytes[begin] & 0x1f;
      if (type == 0 || type >= 24) return {};
      if (type >= 1 && type <= 5) {
        result.picture = true;
        result.idr = result.idr || type == 5;
        non_idr_picture = non_idr_picture || type != 5;
      }
      if (type == 7) parameter_sets |= 1;
      if (type == 8) parameter_sets |= 2;
    } else {
      if (end - begin < 2 || (bytes[begin + 1] & 7) == 0) return {};
      const auto type = (bytes[begin] >> 1) & 0x3f;
      if (type <= 31) {
        result.picture = true;
        result.idr = result.idr || type == 19 || type == 20;
        non_idr_picture = non_idr_picture || (type != 19 && type != 20);
      }
      if (type == 32) parameter_sets |= 1;
      if (type == 33) parameter_sets |= 2;
      if (type == 34) parameter_sets |= 4;
    }
    offset = end;
  }
  result.valid = !bytes.empty();
  result.idr = result.idr && !non_idr_picture;
  result.parameter_sets = parameter_sets == (codec == AccessUnitCodec::H264 ? 3U : 7U);
  return result;
}

// Picture identity comes from segment-normalized capture PTS, not a polling
// tick. Only a strictly newer known picture can change this category's count.
class MediaSampleAnomalies {
 public:
  [[nodiscard]] bool observe(std::optional<std::uint64_t> id, bool usable) {
    if (!id || (last_id_ && *id <= *last_id_)) return false;
    last_id_ = id;
    if (usable) consecutive_ = 0;
    else {
      ++bad_sample_revision_;
      if (consecutive_ < 3 && ++consecutive_ == 3) ++brake_sample_revision_;
    }
    return true;
  }
  [[nodiscard]] std::optional<std::uint64_t> last_id() const { return last_id_; }
  [[nodiscard]] unsigned consecutive() const { return consecutive_; }
  // Recovery readers can miss a complete bad -> good episode between reads.
  // These revisions preserve that history after a new normal picture clears
  // the current count; reaching three once produces one brake event.
  [[nodiscard]] std::uint64_t bad_sample_revision() const { return bad_sample_revision_; }
  [[nodiscard]] std::uint64_t brake_sample_revision() const { return brake_sample_revision_; }
  void reset_stream() { last_id_.reset(); } // Does not make an old fault healthy.
 private:
  std::optional<std::uint64_t> last_id_;
  unsigned consecutive_{0};
  std::uint64_t bad_sample_revision_{0};
  std::uint64_t brake_sample_revision_{0};
};

enum class FreshnessDropReason { None, Stale, InvalidTimestamp, InvalidAccessUnit, WaitingIdr };
struct FreshnessDecision {
  bool forward{false};
  bool picture{false};
  bool request_key_unit{false};
  FreshnessDropReason reason{FreshnessDropReason::None};
};

class EncodedFreshnessGate {
 public:
  explicit EncodedFreshnessGate(std::uint64_t max_age_ms) : max_age_ms_(max_age_ms) {}

  [[nodiscard]] FreshnessDecision observe(
      AccessUnitInfo access_unit, std::optional<std::uint64_t> age_ms) {
    FreshnessDropReason reason = FreshnessDropReason::None;
    if (!age_ms) reason = FreshnessDropReason::InvalidTimestamp;
    else if (*age_ms >= max_age_ms_) reason = FreshnessDropReason::Stale;
    else if (!access_unit.valid) reason = FreshnessDropReason::InvalidAccessUnit;
    if (reason != FreshnessDropReason::None) {
      const bool request = !waiting_idr_ || key_request_pending_;
      key_request_pending_ = false;
      waiting_idr_ = true;
      return {false, access_unit.picture, request, reason};
    }
    if (waiting_idr_) {
      if (!access_unit.idr || !access_unit.parameter_sets) {
        const bool request = key_request_pending_;
        key_request_pending_ = false;
        return {false, access_unit.picture, request, FreshnessDropReason::WaitingIdr};
      }
      waiting_idr_ = false;
      key_request_pending_ = false;
    }
    return {true, access_unit.picture, false, FreshnessDropReason::None};
  }

  void set_max_age_ms(std::uint64_t value) { max_age_ms_ = value; }
  void reset() { waiting_idr_ = true; key_request_pending_ = true; }
  [[nodiscard]] bool waiting_idr() const { return waiting_idr_; }

 private:
  std::uint64_t max_age_ms_;
  bool waiting_idr_{true};
  bool key_request_pending_{true};
};

}  // namespace mine_teleop::detail
