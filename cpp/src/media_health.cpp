#include "mine_teleop/media_health.hpp"
#include <algorithm>
#include <limits>
namespace mine_teleop {
void MediaHealth::reset(const std::vector<std::string> &ids,
                        std::int64_t start) {
  std::lock_guard lock(mutex_);
  states_.clear();
  outputs_.clear();
  started_ = start;
  for (const auto &id : ids)
    states_.emplace(id, State{});
}
bool MediaHealth::captured(const SourceFrameIdentity &f, std::int64_t now) {
  std::lock_guard lock(mutex_);
  auto it = states_.find(f.camera_id);
  if (!f.valid || f.captured_steady_ms <= 0 || f.captured_steady_ms > now)
    return false;
  if (it == states_.end())
    return true;
  auto &s = it->second;
  if (s.generation != f.source_generation) {
    if (f.source_generation < s.generation)
      return false;
    s = State{};
    s.generation = f.source_generation;
  }
  // Capture already extends V4L2 wrap into a monotonic 64-bit identity.
  const auto step =
      f.sequence > s.captured_seq ? f.sequence - s.captured_seq : 0;
  if (s.seen && (!step || f.captured_steady_ms <= s.captured))
    return false;
  s.seen = true;
  s.captured_seq = f.sequence;
  s.captured = f.captured_steady_ms;
  return true;
}
bool MediaHealth::composed(std::uint64_t output,
                           const std::vector<SourceFrameIdentity> &fs,
                           std::int64_t now, int age, int skew) {
  std::lock_guard lock(mutex_);
  (void)output;
  if (fs.empty())
    return false;
  auto lo = std::numeric_limits<std::int64_t>::max(), hi = std::int64_t{};
  for (const auto &f : fs) {
    if (!f.valid || f.captured_steady_ms <= 0 || f.captured_steady_ms > now ||
        now - f.captured_steady_ms > age)
      return false;
    lo = std::min(lo, f.captured_steady_ms);
    hi = std::max(hi, f.captured_steady_ms);
    auto it = states_.find(f.camera_id);
    if (it != states_.end() &&
        (!it->second.seen || it->second.generation != f.source_generation ||
         f.sequence > it->second.captured_seq))
      return false;
  }
  return hi - lo <= skew;
}
bool MediaHealth::encoded(std::uint64_t output,
                          const std::vector<SourceFrameIdentity> &fs,
                          std::int64_t now, bool healthy) {
  if (!healthy || fs.empty())
    return false;
  std::lock_guard lock(mutex_);
  // Every source used by this output must advance; otherwise this encoded
  // output cannot credit any of its source cameras with healthy progress.
  for (const auto &f : fs) {
    auto it = states_.find(f.camera_id);
    if (it == states_.end())
      continue;
    const auto &s = it->second;
    const auto step = f.sequence > s.used_seq ? f.sequence - s.used_seq : 0;
    if (!f.valid || !s.seen || s.generation != f.source_generation ||
        (s.used && (!step || f.captured_steady_ms <= s.consumed)))
      return false;
  }
  for (const auto &f : fs) {
    auto it = states_.find(f.camera_id);
    if (it == states_.end())
      continue;
    auto &s = it->second;
    s.used = true;
    s.used_seq = f.sequence;
    s.consumed = f.captured_steady_ms;
    s.encoded = now;
  }
  outputs_[output] = now;
  return true;
}
std::string MediaHealth::stale(std::int64_t now, int timeout,
                               bool required) const {
  std::lock_guard lock(mutex_);
  for (const auto &[id, s] : states_) {
    if (required && (!s.seen || !s.used))
      return id + ": awaiting raw/composed/encoded frames";
    if (now - (s.seen ? s.captured : started_) > timeout)
      return id + ": raw input stalled";
    if (now - (s.used ? s.consumed : started_) > timeout)
      return id + ": consumed input stalled";
    if (now - (s.used ? s.encoded : started_) > timeout)
      return id + ": encoded output stalled";
  }
  return {};
}
Json MediaHealth::snapshot(std::int64_t now) const {
  std::lock_guard lock(mutex_);
  Json out = Json::array();
  for (const auto &[id, s] : states_)
    out.push_back({{"camera_id", id},
                   {"source_sequence", s.captured_seq},
                   {"source_generation", s.generation},
                   {"used_sequence", s.used_seq},
                   {"raw_age_ms", now - (s.seen ? s.captured : started_)},
                   {"consumed_age_ms", now - (s.used ? s.consumed : started_)},
                   {"encoded_age_ms", now - (s.used ? s.encoded : started_)}});
  return out;
}
} // namespace mine_teleop
