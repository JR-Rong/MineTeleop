#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace mine_teleop::detail {

// State for one control session's critical views. A caller must only report
// `render_submitted` after submitting a validated new frame to the actual visible
// renderer; decoded buffers and a pre-draw callback are not such evidence.
// Capture freshness and authenticated camera identity remain separate gates.
class NativeVideoHealth {
 public:
  struct Snapshot {
    bool ready{}, sample_healthy{};
    std::uint64_t bad_revision{}, brake_revision{};
  };

  explicit NativeVideoHealth(const std::vector<std::string>& critical_cameras, std::int64_t stale_ms)
      : stale_ms_(stale_ms) {
    if (critical_cameras.empty() || critical_cameras.size() > 8 || stale_ms <= 0)
      throw std::invalid_argument("invalid native critical video configuration");
    for (const auto& camera : critical_cameras)
      if (camera.empty() || !tracks_.emplace(camera, Track{}).second)
        throw std::invalid_argument("invalid or duplicate critical camera");
  }

  // Generations are strictly increasing per camera. Late callbacks from a
  // previous track cannot modify the current generation.
  bool attach(const std::string& camera, std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    auto it = tracks_.find(camera);
    if (it == tracks_.end() || generation == 0 || generation <= it->second.generation) return false;
    if (it->second.generation != 0) {
      if (it->second.failed) bump_revisions();
      else bad(it->second);
    }
    it->second = Track{};
    it->second.generation = generation;
    return true;
  }

  bool set_usable(const std::string& camera, std::uint64_t generation, bool usable) {
    std::lock_guard lock(mutex_);
    auto* track = current(camera, generation);
    if (!track) return false;
    if (!usable && track->usable) bad(*track);
    track->usable = usable;
    return true;
  }

  bool render_submitted(const std::string& camera, std::uint64_t generation,
                 std::uint64_t frame_pts, std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    return render_submitted_locked(camera, generation, frame_pts, now_ms);
  }
  // Capture the timestamp while holding the same lock as snapshots, so two
  // threads cannot appear to move a monotonic clock backwards due to stalls.
  bool render_submitted_now(const std::string& camera, std::uint64_t generation, std::uint64_t frame_pts) {
    std::lock_guard lock(mutex_);
    return render_submitted_locked(camera, generation, frame_pts, now());
  }

  bool fault(const std::string& camera, std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    auto* track = current(camera, generation);
    if (!track) return false;
    bad(*track);
    return true;
  }

  Snapshot snapshot(std::int64_t now_ms) {
    std::lock_guard lock(mutex_);
    return snapshot_locked(now_ms);
  }
  Snapshot snapshot_now() {
    std::lock_guard lock(mutex_);
    return snapshot_locked(now());
  }

 private:
  static std::int64_t now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  bool render_submitted_locked(const std::string& camera, std::uint64_t generation,
                               std::uint64_t frame_pts, std::int64_t now_ms) {
    auto* track = current(camera, generation);
    if (!track || !track->usable || !observe_clock(now_ms)) return false;
    if (frame_pts == (std::numeric_limits<std::uint64_t>::max)()) { bad(*track); return false; }
    if (track->last_pts && frame_pts <= *track->last_pts) return false;
    if (track->last_submitted && now_ms - *track->last_submitted >= stale_ms_) bad(*track);
    track->last_pts = frame_pts;
    track->last_submitted = now_ms;
    track->failed = false;
    track->consecutive = (std::min)(3, track->consecutive + 1);
    return true;
  }

  Snapshot snapshot_locked(std::int64_t now_ms) {
    if (!observe_clock(now_ms)) return {false, false, bad_revision_, brake_revision_};
    bool healthy = !overflow_ && !clock_fault_;
    bool ready = healthy;
    for (auto& [id, track] : tracks_) {
      (void)id;
      if (track.last_submitted && now_ms - *track.last_submitted >= stale_ms_) bad(track);
      const bool current_sample = track.usable && !track.failed && track.last_submitted &&
          now_ms >= *track.last_submitted && now_ms - *track.last_submitted < stale_ms_;
      healthy = healthy && current_sample;
      ready = ready && current_sample && track.consecutive >= 3;
    }
    return {ready && !overflow_, healthy && !overflow_, bad_revision_, brake_revision_};
  }

  struct Track {
    std::uint64_t generation{};
    std::optional<std::uint64_t> last_pts;
    std::optional<std::int64_t> last_submitted;
    int consecutive{};
    bool usable{}, failed{true};
  };
  Track* current(const std::string& camera, std::uint64_t generation) {
    auto it = tracks_.find(camera);
    return it != tracks_.end() && generation != 0 && generation == it->second.generation ? &it->second : nullptr;
  }
  void bad(Track& track) {
    if (!track.failed) bump_revisions();
    track.failed = true;
    track.consecutive = 0;
    track.last_submitted.reset();
  }
  void bump_revisions() {
    constexpr std::uint64_t limit = 9007199254740991ULL;
    if (bad_revision_ == limit || brake_revision_ == limit) overflow_ = true;
    else { ++bad_revision_; ++brake_revision_; }
  }
  bool observe_clock(std::int64_t now_ms) {
    if (now_ms < 0 || (last_time_ && now_ms < *last_time_)) {
      clock_fault_ = true;
      for (auto& [id, track] : tracks_) { (void)id; bad(track); }
      return false;
    }
    last_time_ = now_ms;
    return !clock_fault_;
  }
  const std::int64_t stale_ms_;
  std::mutex mutex_;
  std::map<std::string, Track> tracks_;
  std::optional<std::int64_t> last_time_;
  std::uint64_t bad_revision_{}, brake_revision_{};
  bool overflow_{}, clock_fault_{};
};
}  // namespace mine_teleop::detail
