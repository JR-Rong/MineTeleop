#include "mine_teleop/detail/native_video_health.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>

using mine_teleop::detail::NativeVideoHealth;
namespace {
void require(bool condition, const char* text) { if (!condition) throw std::runtime_error(text); }
void test_two_tracks_and_stale_recovery() {
  NativeVideoHealth health({"front", "rear"}, 500);
  require(health.attach("front", 1) && health.attach("rear", 1), "attach failed");
  require(health.set_usable("front", 1, true) && health.set_usable("rear", 1, true), "track not usable");
  require(!health.snapshot(0).ready, "missing frames admitted");
  for (std::uint64_t seq = 1; seq <= 3; ++seq) {
    require(health.render_submitted("front", 1, seq, static_cast<std::int64_t>(seq * 10)), "front frame rejected");
    require(health.render_submitted("rear", 1, seq, static_cast<std::int64_t>(seq * 10)), "rear frame rejected");
  }
  require(health.snapshot(30).ready, "two fresh tracks not admitted");
  require(!health.render_submitted("rear", 1, 3, 40), "duplicate frame renewed freshness");
  require(!health.render_submitted("rear", 1, 2, 50), "older frame renewed freshness");
  require(health.render_submitted("front", 1, 4, 510), "new front frame rejected");
  auto stale = health.snapshot(530);
  require(!stale.ready && !stale.sample_healthy && stale.bad_revision == 1 && stale.brake_revision == 1,
          "rear stale boundary did not withdraw readiness");
  require(health.snapshot(531).brake_revision == 1, "stale polling repeated the fault");
  for (std::uint64_t seq = 4; seq <= 6; ++seq)
    require(health.render_submitted("rear", 1, seq, static_cast<std::int64_t>(530 + seq)), "recovery frame rejected");
  require(health.snapshot(536).ready, "three new frames did not recover both tracks");
}
void test_generation_and_lifecycle_fencing() {
  NativeVideoHealth health({"front"}, 500);
  require(health.attach("front", 1) && health.set_usable("front", 1, true), "initial track failed");
  for (std::uint64_t seq = 1; seq <= 3; ++seq)
    require(health.render_submitted("front", 1, seq, static_cast<std::int64_t>(seq)), "initial frame rejected");
  require(health.snapshot(3).ready, "initial track not ready");
  require(health.attach("front", 2), "replacement rejected");
  require(!health.render_submitted("front", 1, 4, 4), "retired callback accepted");
  require(!health.attach("front", 1) && !health.set_usable("front", 1, true), "old generation accepted");
  require(health.set_usable("front", 2, true), "replacement not usable");
  for (std::uint64_t seq = 1; seq <= 3; ++seq)
    require(health.render_submitted("front", 2, seq, static_cast<std::int64_t>(4 + seq)), "new generation frame rejected");
  auto ready = health.snapshot(7);
  require(ready.ready && ready.brake_revision == 1, "replacement did not retain brake revision");
  require(health.set_usable("front", 2, false), "track loss rejected");
  require(!health.snapshot(8).ready && health.snapshot(8).brake_revision == 2, "track loss did not revoke");
  require(health.set_usable("front", 2, true), "track restore rejected");
  require(!health.render_submitted("front", 2, 3, 9), "old frame recovered track");
  for (std::uint64_t seq = 4; seq <= 6; ++seq)
    require(health.render_submitted("front", 2, seq, static_cast<std::int64_t>(6 + seq)), "new recovery frame rejected");
  require(health.snapshot(12).ready, "track did not recover with three fresh frames");
}
void test_clock_and_bad_sample_fail_closed() {
  NativeVideoHealth health({"front"}, 100);
  require(health.attach("front", 1) && health.set_usable("front", 1, true), "track setup failed");
  require(health.render_submitted("front", 1, 0, 10), "zero PTS is valid");
  require(!health.render_submitted("front", 1, std::numeric_limits<std::uint64_t>::max(), 11), "invalid PTS accepted");
  require(!health.snapshot(11).ready && health.snapshot(11).brake_revision == 1, "invalid frame did not revoke");
  require(health.render_submitted("front", 1, 1, 12), "first new frame rejected");
  require(!health.snapshot(11).sample_healthy, "clock regression accepted");
  require(!health.render_submitted("front", 1, 2, 13) && !health.snapshot(13).ready, "clock fault recovered without new session");
}
}
int main() {
  try {
    test_two_tracks_and_stale_recovery();
    test_generation_and_lifecycle_fencing();
    test_clock_and_bad_sample_fail_closed();
    std::cout << "PASS dual critical-video freshness, generation and clock fencing\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
