#include "mine_teleop/detail/emergency_stop.hpp"
#include "control_time_fixture.hpp"

#include <iostream>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
const std::string key = "public-emergency-fixture-key-0123456789";
void expect(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
Json authority(std::uint64_t epoch = 10) { return {{"issuer", "fixture-issuer"}, {"authority_epoch", epoch}, {"lease_seq", 2}}; }
EmergencyStopReceiver receiver() { EmergencyStopReceiver result; result.reset("vehicle-001", "driver-001", "session-001", "fixture-token"); return result; }
Json frame(std::uint64_t seq, std::int64_t sent, std::uint64_t reset = 0) {
  ControlCommand command; command.vehicle_id = "vehicle-001"; command.driver_id = "driver-001"; command.session_id = "session-001";
  command.control_token = "fixture-token"; command.seq = seq; command.sent_at_utc_ms = sent; command.gear = "D";
  command.estop = true; command.brake = 1; command.operator_active = false;
  auto value = command.to_json(); value["intent_seq"] = 80; value["intent_fresh"] = true;
  value["emergency_stop"] = {{"protocol", kEmergencyStopCapability}, {"authority", authority()}, {"reset_epoch", reset}, {"media_attempt", "media-a"}};
  value["native_mac"] = transport_message_mac(value, key, false); return value;
}
Json result(const Json& input) {
  Json value = {{"event", "vehicle_control_result"}, {"protocol", kControlTransportCapability},
      {"session_id", "session-001"}, {"vehicle_id", "vehicle-001"}, {"driver_id", "driver-001"},
      {"command_seq", input.at("seq")}, {"intent_seq", input.at("intent_seq")}, {"result_seq", input.at("seq")},
      {"control_status_seq", 0}, {"authority_epoch", 0}, {"accepted", true}, {"rearm_required", false},
      {"transport_admitted", true}, {"transport_selected", false}, {"reason", "estop"},
      {"emergency_stop", input.at("emergency_stop")}, {"emergency_reset_epoch", 0}, {"software_estop_latched", true}};
  value["vehicle_mac"] = transport_message_mac(value, key, true); return value;
}
void authenticated_normalized_stop_only_and_scope() {
  for (unsigned scenario = 0; scenario < 9; ++scenario) {
    auto rx = receiver(); auto value = frame(1, 1000);
    if (scenario == 0) value["throttle"] = .7; // Also invalidates MAC.
    if (scenario == 1) value["driver_id"] = "other";
    if (scenario == 2) value["emergency_stop"]["authority"]["authority_epoch"] = 9;
    if (scenario == 3) value["emergency_stop"]["reset_epoch"] = 1;
    if (scenario == 4) value["emergency_stop"]["media_attempt"] = "retired";
    if (scenario == 5) value["estop"] = false;
    if (scenario == 6) value["brake"] = .5;
    if (scenario == 7) value["operator_active"] = true;
    if (scenario == 8) value["seq"] = 0;
    if (scenario != 0) value["native_mac"] = transport_message_mac(value, key, false);
    expect(rx.inspect(value, key, authority(), "media-a", true, 1000, 200).action == EmergencyStopReceiver::Action::Reject,
        "MAC, stop-only normalization or scope bypassed");
  }
  auto rx = receiver();
  expect(rx.inspect(frame(1, 799), key, authority(), "media-a", true, 1000, 200).action == EmergencyStopReceiver::Action::Reject, "expired stop crossed the original command-age boundary");
  expect(rx.inspect(frame(1, 1201), key, authority(), "media-a", true, 1000, 200).action == EmergencyStopReceiver::Action::Reject, "future stop accepted");
  expect(rx.inspect(frame(1, 1000), key, authority(), "new-media", false, 1000, 200).action == EmergencyStopReceiver::Action::ApplyOnce,
      "current WSS stop was fenced by ordinary media restart");
}
void same_bytes_across_dc_wss_apply_common_service_once() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  test::LogicalTimeControlService service(config, "driver-001", "session-001", "fixture-token", std::make_unique<MockVehicleAdapter>());
  service.start(0);
  auto rx = receiver(); auto value = frame(2, 50); const auto command = ControlCommand::from_json(value);
  ControlTransportFence fence; fence.reset("vehicle-001", "driver-001", "session-001", "fixture-token");
  expect(rx.inspect(value, key, authority(), "media-a", true, 50, 200).action == EmergencyStopReceiver::Action::ApplyOnce, "first valid DC stop rejected");
  expect(rx.inspect(value, key, authority(), "media-a", false, 50, 200).action == EmergencyStopReceiver::Action::Pending, "in-flight duplicate could apply twice");
  // No ordinary path stamp, no live cloud lease: only verified stop-only gets this exception.
  expect(!fence.admit(command, nullptr, ControlPath::DataChannel, "media-a", 50, 0, 200, false).admitted, "bare DC stop bypassed receiver authorization");
  expect(fence.admit(command, nullptr, ControlPath::DataChannel, "media-a", 50, 0, 200, false, true).admitted, "verified stop-only exception failed");
  expect(service.receive_command(command, 50).accepted && service.safety_state() == SafetyState::Estop, "real common service did not latch emergency");
  rx.observe_latched(true); const auto first_result = result(value);
  expect(rx.complete(value, first_result), "actual software result not cached");
  for (const auto data_channel : {false, true}) {
    const auto duplicate = rx.inspect(value, key, authority(), "media-a", data_channel, 100, 200);
    expect(duplicate.action == EmergencyStopReceiver::Action::Cached && duplicate.cached == first_result,
        "duplicate did not return the same immutable result without receive_command");
  }
  auto conflict = value; conflict["intent_seq"] = 81; conflict["native_mac"] = transport_message_mac(conflict, key, false);
  expect(rx.inspect(conflict, key, authority(), "media-a", true, 100, 200).action == EmergencyStopReceiver::Action::Reject, "same seq conflicting content accepted");
  const auto canonical = bounded_vehicle_control_result(first_result, "session-001", "vehicle-001", "driver-001");
  expect(verify_transport_message(canonical, key, true), "emergency result canonicalization changed signed semantics");
  service.close();
}
void local_reset_retires_old_emergency_before_cache_lookup() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  test::LogicalTimeControlService service(config, "driver-001", "session-001", "fixture-token", std::make_unique<MockVehicleAdapter>());
  service.start(0); auto rx = receiver(); auto first = frame(1, 50);
  expect(rx.inspect(first, key, authority(), "media-a", true, 50, 200).action == EmergencyStopReceiver::Action::ApplyOnce, "initial reset fixture rejected");
  expect(service.receive_command(ControlCommand::from_json(first), 50).accepted, "initial emergency failed");
  rx.observe_latched(true); rx.complete(first, result(first));
  expect(service.reset_estop(true, "local-fixture", 100), "confirmed local reset failed");
  rx.observe_latched(service.safety_state() == SafetyState::Estop);
  expect(rx.reset_epoch() == 1 && rx.inspect(first, key, authority(), "media-a", true, 100, 200).action == EmergencyStopReceiver::Action::Reject,
      "old cached stop relatched a locally reset vehicle");
  const auto current = frame(2, 100, 1);
  expect(rx.inspect(current, key, authority(), "media-a", true, 100, 200).action == EmergencyStopReceiver::Action::ApplyOnce,
      "fresh explicit stop after local reset rejected");
  service.close();
}
void fixed_cache_eviction_never_reapplies_an_old_seq() {
  auto rx = receiver();
  for (std::uint64_t seq = 1; seq <= 512; ++seq) {
    const auto value = frame(seq, static_cast<std::int64_t>(seq));
    expect(rx.inspect(value, key, authority(), "media-a", true, static_cast<std::int64_t>(seq), 200).action == EmergencyStopReceiver::Action::ApplyOnce, "bounded cache rejected advancing stop");
    expect(rx.complete(value, result(value)), "bounded cache lost current result");
  }
  expect(rx.inspect(frame(1, 1), key, authority(), "media-a", true, 20, 200).action == EmergencyStopReceiver::Action::Reject, "evicted identity reapplied");
}
}
int main() {
  try {
    authenticated_normalized_stop_only_and_scope(); same_bytes_across_dc_wss_apply_common_service_once();
    local_reset_retires_old_emergency_before_cache_lookup(); fixed_cache_eviction_never_reapplies_an_old_seq();
    std::cout << "4 production emergency receiver cases passed; real MAC/common service, no sockets or physical stop proof\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
