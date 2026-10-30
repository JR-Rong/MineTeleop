#include "mine_teleop/detail/native_control_publication.hpp"
#include <iostream>
#include <stdexcept>

using namespace mine_teleop::detail;
using Json = nlohmann::json;
namespace {
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
Json publication(std::uint64_t sequence) {
  Json frame{{"seq", sequence}, {"session_id", "session"},
      {"control_transport", {{"protocol", kControlTransportCapability}, {"path", "dc"}, {"media_attempt", "attempt"}}}};
  return {{"protocol", kControlTransportCapability}, {"enabled", true}, {"frame", frame},
          {"frame_json", frame.dump()}, {"valid_for_ms", 50}};
}
}
int main() {
  try {
    const NativeCourierScope scope{{"session", 3, "attempt"}, "operator", 2};
    NativeControlPublication courier(scope);
    require(courier.take(scope, publication(1), true, 1).has_value(), "eligible frame rejected");
    require(!courier.take(scope, publication(1), true, 1), "a sent frame was replayed");
    require(!courier.take(scope, publication(2), true, 50) && courier.last_seen() == 2, "expired frame not discarded");
    require(!courier.take(scope, publication(2), true, 0), "expired frame retried");
    require(!courier.take(scope, publication(3), false, 1), "closed channel accepted a frame");
    require(!courier.take(scope, publication(3), true, 1), "blocked frame retried");
    auto wrong_attempt = publication(4);
    wrong_attempt["frame"]["control_transport"]["media_attempt"] = "old";
    wrong_attempt["frame_json"] = wrong_attempt["frame"].dump();
    require(!courier.take(scope, wrong_attempt, true, 1), "old attempt admitted");
    auto bytes_changed = publication(5);
    bytes_changed["frame_json"] = publication(6).at("frame_json");
    require(!courier.take(scope, bytes_changed, true, 1), "frame and bytes mismatch admitted");
    require(!courier.take(scope, publication(6), true, -1), "invalid elapsed time admitted");
    require(!courier.take(scope, publication(9007199254740992ULL), true, 0), "unsafe sequence admitted");
    require(courier.take(scope, publication(7), true, 1).has_value(), "valid successor rejected");
    auto replacement = scope; ++replacement.owner_epoch;
    require(!courier.take(replacement, publication(8), true, 1), "new owner reused old scope");
    require(!courier.take(scope, publication(8), true, 1), "retired owner recovered after scope change");
    NativeControlPublication closed(scope); closed.retire();
    require(!closed.take(scope, publication(1), true, 0), "retired courier sent a frame");
    std::cout << "PASS native publication expiry, replay, route and owner fencing\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
