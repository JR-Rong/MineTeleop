#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace mine_teleop::detail {

// Inspect only the clear RTP header. RTP/RTCP mux excludes the ambiguous
// marker-set payload types 64..95; DTLS and incomplete headers are ignored.
inline std::optional<std::uint32_t> rtp_marker_ssrc(std::span<const std::uint8_t> header) {
  if (header.size() < 12 || (header[0] & 0xc0U) != 0x80U || (header[1] & 0x80U) == 0 ||
      (header[1] >= 192U && header[1] <= 223U)) return std::nullopt;
  return (static_cast<std::uint32_t>(header[8]) << 24) |
      (static_cast<std::uint32_t>(header[9]) << 16) |
      (static_cast<std::uint32_t>(header[10]) << 8) | header[11];
}

class RtpMarkerCounters {
 public:
  struct Sample { std::uint32_t ssrc; std::uint64_t count; std::int64_t last_ms; std::int64_t max_gap_ms; };
  void clear() { std::lock_guard lock(mutex_); samples_.clear(); dropped_sources_ = 0; }
  void observe(std::uint32_t ssrc, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    auto it = samples_.find(ssrc);
    if (it == samples_.end()) {
      if (samples_.size() == 16) { ++dropped_sources_; return; }
      it = samples_.emplace(ssrc, Sample{ssrc, 0, 0, 0}).first;
    }
    auto& sample = it->second;
    if (sample.count && now_ms >= sample.last_ms)
      sample.max_gap_ms = std::max(sample.max_gap_ms, now_ms - sample.last_ms);
    sample.last_ms = now_ms;
    ++sample.count;
  }
  std::pair<std::vector<Sample>, std::uint64_t> snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<Sample> result;
    result.reserve(samples_.size());
    for (const auto& [_, sample] : samples_) result.push_back(sample);
    return {std::move(result), dropped_sources_};
  }
 private:
  mutable std::mutex mutex_;
  std::map<std::uint32_t, Sample> samples_;
  std::uint64_t dropped_sources_{};
};

}  // namespace mine_teleop::detail
