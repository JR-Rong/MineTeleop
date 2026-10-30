#include "mine_teleop/detail/native_mock_media_gate.hpp"
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
  require(!gate.observe(status(12000, 3, 2), 2100, 12000, 25) && !gate.ready(2100), "stalled camera admitted");
  require(gate.observe(status(13000, 4, 3), 3100, 13000, 25), "advancing cameras did not recover");
  auto bad = status(14000, 5, 4); bad["adapter_type"] = "can";
  require(!gate.observe(bad, 4100, 14000, 25) && !gate.ready(4100), "non-mock status admitted");
  require(!gate.observe(status(14000, 5, 4), 4100, 14000, 26), "clock uncertainty was relaxed");
  bad = status(14000, 5, 4); bad["media_negotiation"]["id"] = "other";
  require(!gate.observe(bad, 4100, 14000, 25), "retired media attempt admitted");
  require(!gate.observe(status(9000, 5, 4), 4100, 14000, 25), "old status admitted");
  require(!gate.ready(6100), "expired remote status retained readiness");
}
}
int main() { try { test_gate(); std::cout << "PASS scoped mock media, sample progress and clock guard\n"; return 0; }
catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
