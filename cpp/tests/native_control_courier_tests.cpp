#include "mine_teleop/detail/native_control_courier.hpp"
#include <iostream>
#include <stdexcept>

using mine_teleop::detail::NativeControlCourier;
using mine_teleop::detail::NativeCourierScope;
using Json = nlohmann::json;
namespace {
void require(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
Json frame(std::uint64_t sequence) {
  return {{"seq", sequence}, {"session_id", "session"},
          {"control_transport", {{"protocol", "native-control-transport-v1"}, {"path", "dc"}, {"media_attempt", "attempt"}}}};
}
void test_scope_route_and_receipts() {
  NativeCourierScope scope{{"session", 1, "attempt"}, "owner", 1};
  NativeControlCourier courier(scope);
  std::deque<std::string> incoming;
  Json request;
  bool direct = true;
  unsigned sends = 0, bridges = 0;
  std::uint64_t sequence = 1;
  NativeControlCourier::Io io{
      [&]() -> std::optional<std::string> { if (incoming.empty()) return {}; auto value = incoming.front(); incoming.pop_front(); return value; },
      [&] { return direct; },
      [&](const Json& input) { request = input; ++bridges; const auto f = frame(sequence);
        return Json{{"protocol", "native-control-transport-v1"}, {"enabled", true},
                    {"valid_for_ms", 100}, {"frame", f}, {"frame_json", f.dump()}, {"statuses", Json::array()}}; },
      [&](const std::string& bytes) { require(Json::parse(bytes) == frame(sequence), "changed signed bytes"); ++sends; return true; },
      {}};
  incoming.push_back(Json{{"event", "vehicle_control_result"}, {"protocol", "native-control-transport-v1"},
      {"session_id", "session"}, {"command_seq", 1}, {"result_seq", 1}}.dump());
  require(courier.tick(scope, io) && sends == 1 && request.at("vehicle_result").at("result_seq") == 1,
      "direct frame or result not passed to bridge");
  require(courier.tick(scope, io) && sends == 1 && !request.contains("vehicle_result"), "replayed command or receipt");
  direct = false; sequence = 2;
  require(courier.tick(scope, io) && sends == 1 && request.at("dc_ready") == false, "unknown route sent command");
  direct = true;
  require(courier.tick(scope, io) && sends == 1, "discarded old command replayed after route recovery");
  sequence = 3;
  require(courier.tick(scope, io) && sends == 2, "new direct frame not delivered");
  auto retired_scope = scope; retired_scope.owner_epoch = 2;
  require(!courier.tick(retired_scope, io) && courier.retired() && sends == 2, "changed owner did not retire courier");
  require(!courier.tick(scope, io) && bridges == 5, "retired courier resumed");
}
void test_bad_input_and_expiry() {
  NativeCourierScope scope{{"session", 1, "attempt"}, "owner", 1};
  NativeControlCourier bad(scope);
  bool sent = false;
  NativeControlCourier::Io io{[] { return std::optional<std::string>("{"); }, [] { return true; },
      [](const Json&) { return Json::object(); }, [&](const std::string&) { sent = true; return true; }, {}};
  require(!bad.tick(scope, io) && bad.retired() && !sent, "malformed input was accepted");
  NativeControlCourier expired(scope);
  io.receive = [] { return std::optional<std::string>{}; };
  io.bridge = [](const Json&) { const auto f = frame(1); return Json{{"protocol", "native-control-transport-v1"},
      {"enabled", true}, {"valid_for_ms", 0}, {"frame", f}, {"frame_json", f.dump()}}; };
  require(expired.tick(scope, io) && !sent, "expired frame was sent");
}
}
int main() {
  try { test_scope_route_and_receipts(); test_bad_input_and_expiry();
    std::cout << "PASS native scoped courier route, receipt, expiry and retirement\n"; return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
