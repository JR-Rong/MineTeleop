#pragma once

// Bounded bench tracing for the Windows native mock client (run_mock_control in windows_native_viewer.cpp):
// input-guard transitions, accepted operator input and DataChannel sends. Extracted from that function so it
// compiles and is tested on every platform; it has no safety role and never influences control. The viewer
// prints the returned lines to stderr. Output is byte-for-byte what the inline code produced (see the
// differential test in mock_control_trace_tests.cpp).
#include "mine_teleop/bench/native_video_health.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mine_teleop::detail {

class MockControlTrace {
 public:
  // At most this many records of each kind are kept between flushes; the rest are only counted.
  static constexpr std::size_t kMaxRecords = 128;

  // -1 in remote_ready/direct_ready means the check was short-circuited by an earlier failed one.
  struct GuardState {
    bool focused, ui_alive, clock_ok, video_ready, video_sample_healthy, authorized, ready;
    int remote_ready, direct_ready;
    std::uint64_t bad_revision, brake_revision;
    NativeVideoHealth::FaultCounts video_faults;
    bool operator==(const GuardState&) const = default;
  };

  MockControlTrace(bool enabled, std::string session_id)
      : enabled_(enabled), session_id_(std::move(session_id)) {}

  [[nodiscard]] bool enabled() const { return enabled_; }

  // Called by the input worker. A state equal to the previously seen one is not recorded again, whether or not
  // the buffer was flushed in between.
  void record_guard(std::int64_t at, std::int64_t authorization_at, std::int64_t authorization_now, const GuardState& state) {
    if (!enabled_) return;
    std::lock_guard lock(guard_mutex_);
    if (previous_guard_ && *previous_guard_ == state) return;
    if (guard_events_.size() < kMaxRecords) guard_events_.push_back({at, authorization_at, authorization_now, state});
    else ++guard_dropped_;
    previous_guard_ = state;
  }

  // Called by the input worker for every accepted operator input while armed. Recorded when the key mask changed
  // or at least one second passed since the last recorded one.
  void note_input(std::int64_t at, std::int64_t wall_ms, std::uint64_t intent_seq, unsigned key_mask, char gear) {
    if (!enabled_) return;
    std::lock_guard lock(input_mutex_);
    if (key_mask == last_input_keys_ && at - last_input_at_ < 1000) return;
    if (input_events_.size() < kMaxRecords) input_events_.push_back({at, wall_ms, intent_seq, key_mask, gear});
    else ++input_dropped_;
    last_input_keys_ = key_mask;
    last_input_at_ = at;
  }

  // Called by the courier loop for each DataChannel send.
  void record_dc_send(const nlohmann::json& record) {
    if (!enabled_) return;
    std::lock_guard lock(dc_mutex_);
    if (dc_records_.size() < kMaxRecords) dc_records_.push_back(record);
    else ++dc_dropped_;
  }

  // Each flush returns the JSON lines to write to stderr, in order, and empties what it reported.
  [[nodiscard]] std::vector<std::string> flush_dc() {
    std::vector<std::string> lines;
    if (!enabled_) return lines;
    nlohmann::json records = nlohmann::json::array();
    std::uint64_t dropped = 0;
    {
      std::lock_guard lock(dc_mutex_);
      if (dc_records_.empty() && !dc_dropped_) return lines;
      records.swap(dc_records_);
      std::swap(dropped, dc_dropped_);
    }
    lines.push_back(nlohmann::json{{"event", "native_mock_dc_batch"}, {"session_id", session_id_},
        {"records", std::move(records)}, {"dropped", dropped}}.dump());
    return lines;
  }

  [[nodiscard]] std::vector<std::string> flush_guard_and_input() {
    std::vector<std::string> lines;
    if (!enabled_) return lines;
    std::vector<GuardEvent> events;
    std::uint64_t dropped = 0;
    {
      std::lock_guard lock(guard_mutex_);
      events.swap(guard_events_);
      std::swap(dropped, guard_dropped_);
    }
    if (!events.empty() || dropped) {
      nlohmann::json records = nlohmann::json::array();
      for (const auto& event : events) {
        const auto& s = event.state;
        records.push_back({{"monotonic_ms", event.at}, {"focused", s.focused}, {"ui_alive", s.ui_alive},
            {"clock_ok", s.clock_ok}, {"video_ready", s.video_ready}, {"video_sample_healthy", s.video_sample_healthy},
            {"authorized", s.authorized}, {"ready", s.ready},
            {"remote_ready", s.remote_ready < 0 ? nlohmann::json(nullptr) : nlohmann::json(s.remote_ready != 0)},
            {"direct_ready", s.direct_ready < 0 ? nlohmann::json(nullptr) : nlohmann::json(s.direct_ready != 0)},
            {"authorization_age_ms", event.authorization_at > 0 ?
                nlohmann::json(event.authorization_now - event.authorization_at) : nlohmann::json(nullptr)},
            {"bad_revision", s.bad_revision}, {"brake_revision", s.brake_revision},
            {"video_faults", {{"unusable", s.video_faults.unusable}, {"stale", s.video_faults.stale},
                {"invalid_frame", s.video_faults.invalid_frame}, {"replaced", s.video_faults.replaced},
                {"explicit_fault", s.video_faults.explicit_fault}, {"clock", s.video_faults.clock}}}});
      }
      // The guard batch has never carried a session id; the input and DataChannel batches do.
      lines.push_back(nlohmann::json{{"event", "native_mock_guard_batch"}, {"records", std::move(records)},
          {"dropped", dropped}}.dump());
    }
    std::vector<InputEvent> inputs;
    dropped = 0;
    {
      std::lock_guard lock(input_mutex_);
      inputs.swap(input_events_);
      std::swap(dropped, input_dropped_);
    }
    if (!inputs.empty() || dropped) {
      nlohmann::json records = nlohmann::json::array();
      for (const auto& event : inputs)
        records.push_back({{"monotonic_ms", event.at}, {"wall_utc_ms", event.wall_ms}, {"intent_seq", event.intent_seq},
            {"keys", event.keys}, {"gear", std::string(1, event.gear)}});
      lines.push_back(nlohmann::json{{"event", "native_mock_input_batch"}, {"session_id", session_id_},
          {"records", std::move(records)}, {"dropped", dropped}}.dump());
    }
    return lines;
  }

 private:
  struct GuardEvent { std::int64_t at, authorization_at, authorization_now; GuardState state; };
  struct InputEvent { std::int64_t at, wall_ms; std::uint64_t intent_seq; unsigned keys; char gear; };

  const bool enabled_;
  const std::string session_id_;
  std::mutex guard_mutex_;
  std::vector<GuardEvent> guard_events_;
  std::uint64_t guard_dropped_{0};
  std::optional<GuardState> previous_guard_;
  std::mutex input_mutex_;
  std::vector<InputEvent> input_events_;
  std::uint64_t input_dropped_{0};
  std::int64_t last_input_at_{0};
  unsigned last_input_keys_{256};
  std::mutex dc_mutex_;
  // Copy-initialize: brace-initializing a json from json::array() yields [[]], not [].
  nlohmann::json dc_records_ = nlohmann::json::array();
  std::uint64_t dc_dropped_{0};
};

}  // namespace mine_teleop::detail
