#include "mine_teleop/bench/mock_control_trace.hpp"

#include <atomic>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using mine_teleop::detail::MockControlTrace;
using Json = nlohmann::json;
using GuardState = MockControlTrace::GuardState;

void require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }

// The recorder was extracted from run_mock_control() in windows_native_viewer.cpp. This is that code as it stood
// before the extraction, with the lambda captures turned into members and the stderr write turned into a returned
// line. It is the oracle for the differential tests below; do not "simplify" it.
class Original {
 public:
  Original(bool enabled, std::string session_id) : enabled_(enabled), session_id_(std::move(session_id)) {}

  void guard(std::int64_t now_ms, std::int64_t authorized_at, std::int64_t authorization_now_ms, const GuardState& state) {
    if (!enabled_) return;
    if (!previous_guard_ || *previous_guard_ != state) {
      std::lock_guard lock(guard_mutex_);
      if (guard_events_.size() < 128) guard_events_.push_back({now_ms, authorized_at, authorization_now_ms, state});
      else ++guard_dropped_;
      previous_guard_ = state;
    }
  }
  void input(std::int64_t now_ms, std::int64_t wall_ms, std::uint64_t intent_seq, unsigned key_mask, char gear) {
    if (enabled_ && (key_mask != last_input_keys_ || now_ms - last_input_trace_at_ >= 1000)) {
      std::lock_guard lock(input_trace_mutex_);
      if (input_events_.size() < 128) input_events_.push_back({now_ms, wall_ms, intent_seq, key_mask, gear});
      else ++input_trace_dropped_;
      last_input_keys_ = key_mask; last_input_trace_at_ = now_ms;
    }
  }
  void dc(const Json& record) {
    if (!enabled_) return;
    if (dc_send_trace_.size() < 128) dc_send_trace_.push_back(record);
    else ++dc_send_trace_dropped_;
  }
  std::vector<std::string> flush_dc() {
    std::vector<std::string> lines;
    if (!enabled_ || (dc_send_trace_.empty() && !dc_send_trace_dropped_)) return lines;
    const Json trace{{"event", "native_mock_dc_batch"}, {"session_id", session_id_},
        {"records", std::move(dc_send_trace_)}, {"dropped", dc_send_trace_dropped_}};
    lines.push_back(trace.dump());
    dc_send_trace_ = Json::array(); dc_send_trace_dropped_ = 0;
    return lines;
  }
  std::vector<std::string> flush_guard() {
    std::vector<std::string> lines;
    if (!enabled_) return lines;
    std::vector<GuardEvent> events;
    std::uint64_t dropped = 0;
    { std::lock_guard lock(guard_mutex_); events.swap(guard_events_); std::swap(dropped, guard_dropped_); }
    if (!events.empty() || dropped) {
      Json records = Json::array();
      for (const auto& event : events) {
        const auto& s = event.state;
        records.push_back({{"monotonic_ms", event.at}, {"focused", s.focused}, {"ui_alive", s.ui_alive},
            {"clock_ok", s.clock_ok}, {"video_ready", s.video_ready}, {"video_sample_healthy", s.video_sample_healthy},
            {"authorized", s.authorized}, {"ready", s.ready},
            {"remote_ready", s.remote_ready < 0 ? Json(nullptr) : Json(s.remote_ready != 0)},
            {"direct_ready", s.direct_ready < 0 ? Json(nullptr) : Json(s.direct_ready != 0)},
            {"authorization_age_ms", event.authorization_at > 0 ?
                Json(event.authorization_now - event.authorization_at) : Json(nullptr)},
            {"bad_revision", s.bad_revision}, {"brake_revision", s.brake_revision},
            {"video_faults", {{"unusable", s.video_faults.unusable}, {"stale", s.video_faults.stale},
                {"invalid_frame", s.video_faults.invalid_frame}, {"replaced", s.video_faults.replaced},
                {"explicit_fault", s.video_faults.explicit_fault}, {"clock", s.video_faults.clock}}}});
      }
      lines.push_back(Json{{"event", "native_mock_guard_batch"}, {"records", std::move(records)},
          {"dropped", dropped}}.dump());
    }
    std::vector<InputEvent> inputs;
    dropped = 0;
    { std::lock_guard lock(input_trace_mutex_); inputs.swap(input_events_); std::swap(dropped, input_trace_dropped_); }
    if (!inputs.empty() || dropped) {
      Json input_records = Json::array();
      for (const auto& event : inputs) input_records.push_back({{"monotonic_ms", event.at}, {"wall_utc_ms", event.wall_ms},
          {"intent_seq", event.intent_seq}, {"keys", event.keys}, {"gear", std::string(1, event.gear)}});
      lines.push_back(Json{{"event", "native_mock_input_batch"}, {"session_id", session_id_}, {"records", std::move(input_records)},
          {"dropped", dropped}}.dump());
    }
    return lines;
  }

 private:
  struct GuardEvent { std::int64_t at, authorization_at, authorization_now; GuardState state; };
  struct InputEvent { std::int64_t at, wall_ms; std::uint64_t intent_seq; unsigned keys; char gear; };
  bool enabled_;
  std::string session_id_;
  std::mutex guard_mutex_;
  std::vector<GuardEvent> guard_events_;
  std::uint64_t guard_dropped_ = 0;
  std::optional<GuardState> previous_guard_;
  std::mutex input_trace_mutex_;
  std::vector<InputEvent> input_events_;
  std::uint64_t input_trace_dropped_ = 0;
  std::int64_t last_input_trace_at_ = 0;
  unsigned last_input_keys_ = 256;
  Json dc_send_trace_ = Json::array();
  std::uint64_t dc_send_trace_dropped_ = 0;
};

GuardState guard_state(unsigned bits, std::uint64_t revision = 0) {
  GuardState state{};
  state.focused = bits & 1U; state.ui_alive = bits & 2U; state.clock_ok = bits & 4U; state.video_ready = bits & 8U;
  state.video_sample_healthy = bits & 16U; state.authorized = bits & 32U; state.ready = bits & 64U;
  state.remote_ready = (bits & 128U) ? 1 : ((bits & 256U) ? 0 : -1);
  state.direct_ready = (bits & 512U) ? 1 : ((bits & 1024U) ? 0 : -1);
  state.bad_revision = revision; state.brake_revision = revision * 2;
  state.video_faults.stale = bits; state.video_faults.clock = revision;
  return state;
}
Json dc_record(std::uint64_t seq) {
  return {{"seq", seq}, {"started_monotonic_ms", static_cast<std::int64_t>(seq * 10)},
          {"completed_monotonic_ms", static_cast<std::int64_t>(seq * 10 + 1)}, {"queued", seq % 3 != 0},
          {"wire_format", "json"}, {"wire_bytes", 100 + seq}};
}

// A deterministic event stream applied to both implementations; every flush must match byte for byte.
void differential(bool enabled, std::uint64_t seed, int events, int flush_every) {
  MockControlTrace trace(enabled, "session-001");
  Original original(enabled, "session-001");
  std::uint64_t state = seed;
  const auto next = [&] { state = state * 6364136223846793005ULL + 1442695040888963407ULL; return state >> 33U; };
  std::int64_t now = 1000;
  std::uint64_t seq = 0;
  for (int index = 0; index < events; ++index) {
    now += static_cast<std::int64_t>(next() % 400);
    switch (next() % 3) {
      case 0: {  // Few distinct states, so repeated ones exercise the consecutive-duplicate filter.
        const auto bits = static_cast<unsigned>(next() % 6) * 37U;
        const auto authorized_at = static_cast<std::int64_t>(next() % 2 ? now - 20 : 0);
        const auto s = guard_state(bits, next() % 2);
        trace.record_guard(now, authorized_at, now + 3, s);
        original.guard(now, authorized_at, now + 3, s);
        break;
      }
      case 1: {
        const auto keys = static_cast<unsigned>(next() % 4);
        const auto gear = "PRND"[next() % 4];
        trace.note_input(now, now + 5000000, ++seq, keys, gear);
        original.input(now, now + 5000000, seq, keys, gear);
        break;
      }
      default: {
        const auto record = dc_record(++seq);
        trace.record_dc_send(record);
        original.dc(record);
      }
    }
    if (flush_every > 0 && (index + 1) % flush_every == 0) {
      require(trace.flush_dc() == original.flush_dc(), "dc batch differs from the original");
      require(trace.flush_guard_and_input() == original.flush_guard(), "guard/input batches differ from the original");
    }
  }
  require(trace.flush_dc() == original.flush_dc(), "final dc batch differs from the original");
  require(trace.flush_guard_and_input() == original.flush_guard(), "final guard/input batches differ from the original");
}

void output_matches_the_original_byte_for_byte() {
  for (const bool enabled : {true, false})
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
      differential(enabled, seed, 0, 0);       // Nothing recorded.
      differential(enabled, seed, 40, 0);      // Under the bound.
      differential(enabled, seed, 700, 0);     // Far past the bound of 128 per kind: drop counts must match.
      differential(enabled, seed, 700, 25);    // Frequent flushes: buffers and counters reset the same way.
      differential(enabled, seed, 300, 130);   // Flushes land around the bound.
    }
}

void bound_drop_count_and_reset() {
  MockControlTrace trace(true, "session-001");
  for (std::uint64_t index = 0; index < 200; ++index) trace.record_dc_send(dc_record(index + 1));
  auto lines = trace.flush_dc();
  require(lines.size() == 1, "one dc batch expected");
  auto batch = Json::parse(lines.front());
  require(batch.at("event") == "native_mock_dc_batch" && batch.at("session_id") == "session-001", "dc batch identity wrong");
  require(batch.at("records").size() == 128 && batch.at("dropped") == 72, "dc bound or drop count wrong");
  require(batch.at("records").front().at("seq") == 1 && batch.at("records").back().at("seq") == 128,
          "the first 128 records must be kept, not the last");
  require(trace.flush_dc().empty(), "an empty buffer produced a batch");
  trace.record_dc_send(dc_record(999));
  batch = Json::parse(trace.flush_dc().front());
  require(batch.at("records").size() == 1 && batch.at("dropped") == 0, "flush did not reset the buffer and drop counter");

  for (std::uint64_t index = 0; index < 300; ++index)
    trace.record_guard(static_cast<std::int64_t>(index), 0, 0, guard_state(static_cast<unsigned>(index & 1U ? 3 : 5), index));
  lines = trace.flush_guard_and_input();
  require(lines.size() == 1, "one guard batch expected");
  batch = Json::parse(lines.front());
  require(batch.at("records").size() == 128 && batch.at("dropped") == 172, "guard bound or drop count wrong");
}

void event_names_and_field_sets() {
  MockControlTrace trace(true, "session-007");
  trace.record_guard(10, 0, 30, guard_state(1 | 2 | 128, 4));
  trace.record_guard(20, 5, 30, guard_state(512 | 4, 5));
  trace.note_input(40, 1700000000000, 9, 3, 'D');
  trace.record_dc_send(dc_record(1));
  const auto dc = Json::parse(trace.flush_dc().at(0));
  require(dc.size() == 4 && dc.contains("event") && dc.contains("session_id") && dc.contains("records") && dc.contains("dropped"),
          "dc batch field set changed");
  const auto lines = trace.flush_guard_and_input();
  require(lines.size() == 2, "guard and input batches expected, in that order");
  const auto guard = Json::parse(lines.at(0));
  require(guard.at("event") == "native_mock_guard_batch", "guard event name changed");
  require(guard.size() == 3 && !guard.contains("session_id"), "the guard batch has never carried a session id");
  const auto& first = guard.at("records").at(0);
  require(first.size() == 14, "guard record field count changed");
  for (const char* key : {"monotonic_ms", "focused", "ui_alive", "clock_ok", "video_ready", "video_sample_healthy",
                          "authorized", "ready", "remote_ready", "direct_ready", "authorization_age_ms", "bad_revision",
                          "brake_revision", "video_faults"})
    require(first.contains(key), std::string("guard record lost ") + key);
  require(first.at("authorization_age_ms").is_null(), "authorization age must be null when never authorized");
  require(first.at("remote_ready") == true && first.at("direct_ready").is_null(), "ready flags must map 1/0/-1 to true/false/null");
  require(guard.at("records").at(1).at("authorization_age_ms") == 25 && guard.at("records").at(1).at("direct_ready") == true,
          "authorization age or direct readiness wrong");
  require(first.at("video_faults").size() == 6, "video fault counters changed");
  const auto input = Json::parse(lines.at(1));
  require(input.at("event") == "native_mock_input_batch" && input.at("session_id") == "session-007", "input batch identity wrong");
  const auto& record = input.at("records").at(0);
  require(record.size() == 5 && record.at("keys") == 3 && record.at("gear") == "D" && record.at("wall_utc_ms") == 1700000000000LL,
          "input record fields changed");
}

void disabled_trace_records_and_prints_nothing() {
  MockControlTrace trace(false, "session-001");
  require(!trace.enabled(), "a disabled trace reported itself enabled");
  trace.record_guard(1, 0, 0, guard_state(1));
  trace.note_input(1, 1, 1, 1, 'N');
  trace.record_dc_send(dc_record(1));
  require(trace.flush_dc().empty() && trace.flush_guard_and_input().empty(), "a disabled trace printed something");
}

void guard_filter_and_input_throttle_survive_a_flush() {
  MockControlTrace trace(true, "session-001");
  trace.record_guard(1, 0, 0, guard_state(5));
  trace.record_guard(2, 0, 0, guard_state(5));  // Same state: filtered.
  require(Json::parse(trace.flush_guard_and_input().at(0)).at("records").size() == 1, "a repeated state was recorded");
  // The filter belongs to the recording thread, not to the buffer: a flush must not forget the last state.
  trace.record_guard(3, 0, 0, guard_state(5));
  require(trace.flush_guard_and_input().empty(), "a flush made the next identical state look new");
  trace.record_guard(4, 0, 0, guard_state(6));
  require(Json::parse(trace.flush_guard_and_input().at(0)).at("records").size() == 1, "a changed state was not recorded");

  trace.note_input(5000, 1, 1, 7, 'D');   // First call always records (the last-keys sentinel is 256).
  trace.note_input(5500, 2, 2, 7, 'D');   // Same keys, under a second: skipped.
  trace.note_input(5999, 3, 3, 7, 'D');   // Still skipped (999 ms).
  trace.note_input(6000, 4, 4, 7, 'D');   // Exactly one second after the last recorded one: recorded.
  trace.note_input(6001, 5, 5, 3, 'D');   // Keys changed: recorded.
  const auto input = Json::parse(trace.flush_guard_and_input().at(0));
  require(input.at("records").size() == 3 && input.at("records").at(0).at("intent_seq") == 1 &&
              input.at("records").at(1).at("intent_seq") == 4 && input.at("records").at(2).at("intent_seq") == 5,
          "input throttle changed");
  trace.note_input(6100, 6, 6, 3, 'D');   // The throttle state also survives the flush.
  require(trace.flush_guard_and_input().empty(), "a flush reset the input throttle");
}

// One recording thread, one flushing thread, as in the viewer. Nothing may be lost or counted twice.
void concurrent_recording_and_flushing_conserves_every_event() {
  MockControlTrace trace(true, "session-001");
  constexpr int kEvents = 6000;
  std::atomic<bool> done{false};
  std::uint64_t seen = 0, dropped = 0;
  const auto drain = [&] {
    for (const auto& line : trace.flush_guard_and_input()) {
      const auto batch = Json::parse(line);
      seen += batch.at("records").size();
      dropped += batch.at("dropped").get<std::uint64_t>();
    }
  };
  std::thread flusher([&] { while (!done.load()) { drain(); std::this_thread::yield(); } });
  for (int index = 0; index < kEvents; ++index) {
    trace.record_guard(index, 0, 0, guard_state(static_cast<unsigned>(index % 2 ? 3 : 5), static_cast<std::uint64_t>(index)));
    trace.note_input(index * 2000LL, index, static_cast<std::uint64_t>(index) + 1, static_cast<unsigned>(index % 5), 'D');
  }
  done.store(true);
  flusher.join();
  drain();
  // Every guard call changes the state (the revision differs) and every input call changes the key mask or is >= 1 s apart.
  require(seen + dropped == static_cast<std::uint64_t>(kEvents) * 2, "events were lost or counted twice under concurrency");
}
}  // namespace

int main() {
  try {
    output_matches_the_original_byte_for_byte();
    bound_drop_count_and_reset();
    event_names_and_field_sets();
    disabled_trace_records_and_prints_nothing();
    guard_filter_and_input_throttle_survive_a_flush();
    concurrent_recording_and_flushing_conserves_every_event();
    std::cout << "PASS mock control trace: byte-identical to the original, bounds, field contract, filters, concurrency\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
