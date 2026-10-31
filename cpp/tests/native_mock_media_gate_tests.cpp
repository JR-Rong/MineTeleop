#include "mine_teleop/detail/native_mock_media_gate.hpp"
#include "mine_teleop/detail/native_mock_input_sequence.hpp"
#include "mine_teleop/detail/native_mock_profile_retry.hpp"
#include "mine_teleop/detail/native_mock_keyboard.hpp"
#include "mine_teleop/detail/dtls_record_trace.hpp"
#include <iostream>
#include <stdexcept>
using mine_teleop::detail::NativeMockMediaGate;
using Json = nlohmann::json;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Json status(std::int64_t time, std::uint64_t first, std::uint64_t second) {
  auto lane = [](const char* id, std::uint64_t sample) {
    return Json{{"camera_id", id}, {"critical_for_control", true}, {"lane_state", "active"},
        {"send_waiting_idr", false}, {"send_invalid_sample_blocked", false},
        {"send_last_sample_id_ns", sample}, {"fresh_send_frames", 1}};
  };
  return {{"session_id", "session"}, {"vehicle_id", "test_bench_one"}, {"adapter_type", "mock"},
      {"mock_control_diagnostic", false}, {"reported_at_utc_ms", time},
      {"media_negotiation", {{"id", "attempt"}, {"answer_applied", true}}},
      {"time_sync", {{"synchronized", true}, {"uncertainty_ms", 25}}},
      {"lanes", Json::array({lane("front", first), lane("rear", second)})}};
}
void test_gate() {
  NativeMockMediaGate gate("session", "test_bench_one", "attempt", {"front", "rear"});
  require(!gate.observe(status(10000, 1, 1), 100, 10000, 25), "first status cannot establish advancement");
  require(gate.observe(status(11000, 2, 2), 1100, 11000, 25) && gate.ready(1100), "two advancing streams rejected");
  require(gate.reason(1100) == "ready" && gate.reason(4100) == "expired", "ready/expiry diagnostics wrong");
  require(!gate.observe(status(12000, 3, 2), 2100, 12000, 25) && !gate.ready(2100), "stalled camera admitted");
  require(gate.reason(2100) == "not_advancing", "stalled camera diagnosis wrong");
  require(gate.observe(status(13000, 4, 3), 3100, 13000, 25), "advancing cameras did not recover");
  auto bad = status(14000, 5, 4); bad["adapter_type"] = "can";
  require(!gate.observe(bad, 4100, 14000, 25) && !gate.ready(4100), "non-mock status admitted");
  require(!gate.observe(status(14000, 5, 4), 4100, 14000, 26), "clock uncertainty was relaxed");
  bad = status(14000, 5, 4); bad["media_negotiation"]["id"] = "other";
  require(!gate.observe(bad, 4100, 14000, 25), "retired media attempt admitted");
  require(!gate.observe(status(9000, 5, 4), 4100, 14000, 25), "old status admitted");
  require(gate.reason(4100) == "timestamp", "old status diagnosis wrong");
  require(!gate.ready(6100), "expired remote status retained readiness");
}
void test_neutral_refresh_sequence() {
  mine_teleop::detail::NativeMockInputSequence sequence;
  Json input{{"ui_control_epoch", 1}, {"critical_video_ready", true},
             {"critical_video_bad_sample_revision", 0}, {"throttle", 0.0}};
  require(sequence.next(input) == 1 && sequence.next(input) == 1, "identical renewal changed input sequence");
  input["critical_video_ready"] = false;
  require(sequence.next(input) == 2 && sequence.next(input) == 2, "video withdrawal did not advance sequence");
  input["critical_video_ready"] = true;
  require(sequence.next(input) == 3, "video recovery reused retired input sequence");
  input["critical_video_bad_sample_revision"] = 1;
  require(sequence.next(input) == 4, "bad sample revision did not advance sequence");
  input["ui_control_epoch"] = 2;
  require(sequence.next(input) == 5, "owner epoch change did not advance sequence");
}
void test_profile_retry_stays_bounded_and_needs_live_guards() {
  mine_teleop::detail::NativeMockProfileRetry retry;
  require(!retry.due(false, true, false, 1, 0) && !retry.due(true, false, false, 1, 0) &&
          !retry.due(true, true, true, 1, 0) && !retry.due(true, true, false, 0, 0),
          "profile retry ignored startup gates");
  for (unsigned n = 0; n < retry.kMaxAttempts; ++n) {
    const auto t = static_cast<std::int64_t>(n) * retry.kRetryMs;
    require(retry.due(true, true, false, 1, t), "bounded profile attempt missing");
    retry.sent(1, t);
    require(!retry.due(true, true, false, 1, t + retry.kRetryMs - 1), "profile retry too early");
    require(!retry.due(true, true, true, 1, t + retry.kRetryMs), "accepted profile was resent");
    require(!retry.due(true, true, false, 1, t - 1), "backward clock enabled retry");
    require(!retry.due(true, true, false, 2, t + retry.kRetryMs), "owner handoff reused old profile");
  }
  require(!retry.due(true, true, false, 1, 60000) && retry.attempts() == retry.kMaxAttempts,
          "profile retry budget was not exhausted");
}
void test_dtls_header_trace_is_bounded_and_excludes_payloads() {
  using namespace mine_teleop::detail;
  std::array<std::uint8_t, 13> header{23, 0xfe, 0xfd, 0, 1, 0, 0, 0, 0, 0, 42, 0, 4};
  auto parsed = parse_dtls_record_header(header.data(), header.size());
  require(parsed && parsed->type == 23 && parsed->epoch == 1 && parsed->sequence == 42,
          "DTLS record header identity is wrong");
  require(!parse_dtls_record_header(header.data(), 12), "truncated record accepted");
  auto rtp = header; rtp[0] = 0x80;
  require(!parse_dtls_record_header(rtp.data(), rtp.size()), "non-DTLS packet accepted");
  auto invalid = header; invalid[2] = 0x01;
  require(!parse_dtls_record_header(invalid.data(), invalid.size()), "invalid DTLS version accepted");
  DtlsRecordTrace trace;
  for (std::size_t i = 0; i < DtlsRecordTrace::kCapacity + 3; ++i)
    trace.record(header.data(), header.size(), 1024, static_cast<std::int64_t>(i));
  auto batch = trace.drain();
  require(batch.records.size() == DtlsRecordTrace::kCapacity && batch.dropped == 3 &&
          batch.records.front().header.sequence == 42 && batch.records.front().bytes == 1024,
          "DTLS diagnostics did not bound the queue or account for loss");
  require(trace.drain().records.empty(), "DTLS trace drain retained records");
}
void test_mock_keyboard_rearms_only_after_release() {
  mine_teleop::detail::NativeMockKeyboard keyboard;
  using Keys = mine_teleop::detail::NativeMockKeyboard::Keys;
  keyboard.observe(true, Keys{}, 100);
  require(keyboard.heartbeat_alive(101), "foreground UI heartbeat not recorded");
  require(keyboard.sample(true, 101).throttle == 0.0, "initial neutral did not arm");
  keyboard.observe(true, Keys{true, true, false, false}, 110);
  auto input = keyboard.sample(true, 111);
  require(input.throttle == .25 && input.steering == -.25 && input.brake == 0.0,
          "foreground mock keys were not mapped");
  require(keyboard.sample(false, 112).throttle == 0.0, "safety loss did not neutralize input");
  require(keyboard.sample(true, 113).throttle == 0.0, "held key resumed after safety recovery");
  keyboard.observe(true, Keys{}, 120);
  static_cast<void>(keyboard.sample(true, 121));
  keyboard.observe(true, Keys{true, true, true, true}, 130);
  input = keyboard.sample(true, 131);
  require(input.throttle == 0.0 && input.steering == 0.0 && input.brake == .3,
          "brake priority or opposing steering handling failed");
  require(keyboard.sample(true, 280).brake == 0.0, "stale UI heartbeat retained input");
  require(!keyboard.heartbeat_alive(280), "stale UI heartbeat retained lease eligibility");
  keyboard.observe(true, Keys{true, false, false, false}, 281);
  require(keyboard.sample(true, 282).throttle == 0.0, "stale heartbeat resumed a held key");
  keyboard.withdraw();
  keyboard.observe(true, Keys{true, false, false, false}, 290);
  require(keyboard.sample(true, 291).throttle == 0.0, "focus recovery resumed a held key");
  keyboard.observe(true, Keys{}, 300); static_cast<void>(keyboard.sample(true, 301));
  keyboard.observe(true, Keys{true, false, false, false}, 310);
  require(keyboard.sample(true, 311).throttle == .25, "released key could not be freshly pressed");
}
void test_live_clock_keyboard_and_media_guard() {
  using Clock = std::chrono::steady_clock;
  const auto now = [] { return std::chrono::duration_cast<std::chrono::milliseconds>(
      Clock::now().time_since_epoch()).count(); };
  mine_teleop::detail::NativeMockKeyboard keyboard;
  using Keys = mine_teleop::detail::NativeMockKeyboard::Keys;
  keyboard.observe(true, Keys{}, now());
  require(keyboard.heartbeat_alive_now(), "live keyboard heartbeat missing");
  static_cast<void>(keyboard.sample_now(true));
  keyboard.observe(true, Keys{true, false, false, false}, now());
  require(keyboard.sample_now(true).throttle == .25, "live keyboard did not accept a fresh held key");
  require(keyboard.sample_now(false).throttle == 0, "live keyboard did not revoke an unsafe held key");
  require(keyboard.sample_now(true).throttle == 0, "live keyboard resumed without a fresh release");

  NativeMockMediaGate gate("session", "test_bench_one", "attempt", {"front", "rear"});
  require(!gate.observe(status(20000, 1, 1), now(), 20000, 25), "initial media sample unexpectedly ready");
  require(gate.observe(status(20001, 2, 2), now(), 20001, 25) && gate.ready_now(),
          "live media guard did not accept fresh advancing cameras");
  auto invalid = status(20002, 3, 3); invalid["adapter_type"] = "can";
  require(!gate.observe(invalid, now(), 20002, 25) && !gate.ready_now(),
          "live media guard did not revoke invalid evidence");
  using mine_teleop::detail::native_mock_input_fresh;
  require(!native_mock_input_fresh(0, 1000) && !native_mock_input_fresh(1001, 1000),
          "missing or future input observation was admitted");
  require(native_mock_input_fresh(1001, 1001) && native_mock_input_fresh(1001, 1200) &&
          !native_mock_input_fresh(1001, 1201), "200 ms input lease boundary changed");
}
}
int main() { try { test_gate(); test_neutral_refresh_sequence(); test_profile_retry_stays_bounded_and_needs_live_guards(); test_dtls_header_trace_is_bounded_and_excludes_payloads(); test_mock_keyboard_rearms_only_after_release(); test_live_clock_keyboard_and_media_guard();
  std::cout << "PASS scoped mock media, input renewal, keyboard and clock guard\n"; return 0; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
