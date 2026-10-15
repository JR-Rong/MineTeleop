#include "mine_teleop/detail/control_transport.hpp"
#include "mine_teleop/detail/control_transport_sender.hpp"
#include "control_time_fixture.hpp"
#include "mine_teleop/server.hpp"

#include <iostream>
#include <stdexcept>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<class F> void throws(F action) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } expect(failed, "invalid protocol accepted"); }
ControlCommand command(std::uint64_t seq, std::int64_t now) {
  ControlCommand value; value.vehicle_id = "vehicle-001"; value.driver_id = "driver-001";
  value.session_id = "session-001"; value.control_token = "fixture"; value.seq = seq;
  value.sent_at_utc_ms = now; value.gear = "D"; return value;
}
ControlTransportFence fence() {
  ControlTransportFence value; value.reset("vehicle-001", "driver-001", "session-001", "fixture"); return value;
}
Json stamp(std::uint64_t epoch, std::uint64_t from, ControlPath path, std::uint64_t minimum, bool select = true) {
  return ControlTransportStamp{epoch, from, minimum, path, path == ControlPath::DataChannel ? "media-a" : "", select}.to_json();
}
auto admit(ControlTransportFence& value, const ControlCommand& cmd, const Json* wire, ControlPath path, std::int64_t now) {
  return value.admit(cmd, wire, path, "media-a", now, 100000, 200, true);
}
void version_scope_and_expiry() {
  expect(!DriverConfig{}.enable_experimental_control_transport, "experimental transport default must remain closed pending authority review");
  auto value = fence(); auto cmd = command(1, 1000); auto wire = stamp(1, 0, ControlPath::DataChannel, 1);
  expect(!admit(value, cmd, nullptr, ControlPath::DataChannel, 1000).admitted, "legacy DC re-enabled");
  cmd.control_token = "wrong"; expect(!admit(value, cmd, &wire, ControlPath::DataChannel, 1000).admitted, "wrong token admitted");
  cmd = command(1, 799); expect(!admit(value, cmd, &wire, ControlPath::DataChannel, 1000).admitted, "old packet retimestamped");
  cmd.sent_at_utc_ms = 1201; expect(!admit(value, cmd, &wire, ControlPath::DataChannel, 1000).admitted, "future packet admitted");
  cmd = command(1, 1000); expect(!value.admit(cmd, &wire, ControlPath::DataChannel, "media-b", 1000, 100000, 200, true).admitted, "old media attempt admitted");
  expect(!value.admit(cmd, &wire, ControlPath::DataChannel, "media-a", 1000, 1000, 200, true).admitted, "expired lease admitted");
  expect(!value.admit(cmd, &wire, ControlPath::DataChannel, "media-a", 1000, 100000, 200, false).admitted, "cloud revocation bypassed");
  auto invalid = wire; invalid["protocol"] = "future"; throws([&]{ ControlTransportStamp::from_json(invalid); });
  invalid = wire; invalid["epoch"] = -1; throws([&]{ ControlTransportStamp::from_json(invalid); });
  invalid = wire; invalid["unexpected_semantics"] = true; throws([&]{ ControlTransportStamp::from_json(invalid); });
}
void single_receiver_epoch_replay_and_emergency() {
  auto value = fence(); auto cmd = command(1, 1000);
  expect(admit(value, cmd, nullptr, ControlPath::Wss, 1000).admitted, "legacy WSS broken");
  auto wire = stamp(1, 0, ControlPath::DataChannel, 2); cmd = command(2, 1050); cmd.throttle = .5;
  expect(!admit(value, cmd, &wire, ControlPath::DataChannel, 1050).admitted, "moving selection accepted");
  cmd.throttle = 0; auto result = admit(value, cmd, &wire, ControlPath::DataChannel, 1050);
  expect(result.admitted && result.changed, "DC selection failed");
  cmd = command(3, 1100); result = admit(value, cmd, &wire, ControlPath::DataChannel, 1100);
  expect(result.admitted && !result.changed, "lost selection result was not idempotent");
  cmd = command(999, 1101); expect(!admit(value, cmd, nullptr, ControlPath::Wss, 1101).admitted, "late old-path high sequence applied");
  wire = stamp(2, 1, ControlPath::Wss, 4); cmd = command(4, 1150);
  expect(admit(value, cmd, &wire, ControlPath::Wss, 1150).changed, "same-session WSS fallback failed");
  auto old = stamp(1, 0, ControlPath::DataChannel, 2, false); cmd = command(1000, 1151);
  expect(!admit(value, cmd, &old, ControlPath::DataChannel, 1151).admitted, "retired epoch applied");
  cmd = command(5, 1200); cmd.estop = true;
  expect(admit(value, cmd, nullptr, ControlPath::Wss, 1200).admitted, "epoch blocked fresh WSS ESTOP");
  expect(!admit(value, cmd, nullptr, ControlPath::Wss, 1200).admitted, "duplicate ESTOP renewed the receiver");
}
Json payload(std::uint64_t seq, std::int64_t now) {
  auto value = command(seq, now).to_json(); value["intent_seq"] = seq + 100;
  value["intent_fresh"] = true; return value;
}
Json response(const Json& frame, bool accepted = true, bool selected = false) {
  return {{"event", "vehicle_control_result"}, {"protocol", kControlTransportCapability},
      {"session_id", "session-001"}, {"vehicle_id", "vehicle-001"}, {"driver_id", "driver-001"},
      {"command_seq", frame.at("seq")}, {"intent_seq", frame.at("intent_seq")}, {"result_seq", frame.at("seq")},
      {"accepted", accepted}, {"rearm_required", false}, {"control_status_seq", 0}, {"reason", "fixture"},
      {"transport_admitted", true}, {"transport_selected", selected},
      {"authority_epoch", frame.contains("control_transport") ? frame.at("control_transport").at("epoch") : Json(0)},
      {"control_transport", frame.value("control_transport", Json(nullptr))}};
}
void baseline(ControlTransportSender& sender) {
  for (std::uint64_t seq = 1; seq <= 3; ++seq) {
    const auto frame = payload(seq, seq * 50); sender.note_sent(frame, ControlPath::Wss, seq * 50);
    auto result = response(frame); result.erase("control_transport");
    expect(sender.note_result(result, seq * 50 + 40, 5000).matched, "WSS baseline failed");
  }
}
void exact_result_and_neutral_qualification() {
  ControlTransportSender sender; baseline(sender);
  expect(sender.consider_dc("media-a", 200, 5000) && sender.holding_motion(), "qualification did not hold motion");
  for (std::uint64_t seq = 4; seq <= 6; ++seq) {
    auto frame = payload(seq, seq * 50); sender.decorate(frame); sender.note_sent(frame, ControlPath::DataChannel, seq * 50);
    auto result = response(frame, true, seq == 4);
    if (seq == 4) {
      auto invalid = result; invalid["intent_seq"] = 999;
      expect(!sender.note_result(invalid, 210, 5000).matched, "unmatched result renewed route");
    }
    expect(sender.note_result(result, seq * 50 + 20, 5000).matched, "exact DC result lost");
    expect(!sender.note_result(result, seq * 50 + 21, 5000).matched, "duplicate result counted twice");
    if (seq < 6) expect(sender.holding_motion(), "one result qualified motion");
  }
  expect(sender.path() == ControlPath::DataChannel && !sender.holding_motion(), "faster qualified DC not retained");
  expect(sender.observe(false, "media-a", 400, 5000) && sender.path() == ControlPath::Wss && sender.selecting(), "DC loss did not fence WSS fallback");
}
void slower_dc_and_lost_selection() {
  ControlTransportSender sender; baseline(sender); expect(sender.consider_dc("media-a", 200, 5000), "trial missing");
  for (std::uint64_t seq = 4; seq <= 6; ++seq) {
    auto frame = payload(seq, seq * 100); sender.decorate(frame); sender.note_sent(frame, ControlPath::DataChannel, seq * 100);
    expect(sender.note_result(response(frame, true, seq == 4), seq * 100 + 80, 5000).matched, "slow result lost");
  }
  expect(sender.path() == ControlPath::Wss && sender.selecting(), "DC topology overrode faster WSS result");
  auto frame = payload(7, 800); sender.decorate(frame); sender.note_sent(frame, ControlPath::Wss, 800);
  auto conflict = response(frame, false); conflict["authority_epoch"] = 0; conflict["reason"] = "transport_epoch_conflict";
  expect(sender.note_result(conflict, 820, 5000).boundary, "lost request did not reconcile authority");
  frame = payload(8, 850); sender.decorate(frame);
  expect(frame["control_transport"]["epoch"] == 1 && frame["control_transport"]["from_epoch"] == 0,
      "reconciled selection skipped the actual vehicle epoch");
}
void cloud_ingress_without_vehicle_result_can_only_start_a_neutral_trial() {
  ControlTransportSender sender;
  sender.note_sent(payload(1, 0), ControlPath::Wss, 0);
  expect(!sender.consider_dc("media-a", 499, 5000), "trial began before the existing stall boundary");
  expect(sender.consider_dc("media-a", 500, 5000) && sender.selecting() && sender.holding_motion(),
      "fast cloud ingress without vehicle results stranded WSS forever");
  sender.observe(false, "media-a", 501, 5000);
  expect(!sender.consider_dc("media-a", 502, 5000), "failed candidate retried without debounce");
}

void healthy_proven_p2p_is_preferred_to_a_faster_cloud_backup() {
  ControlTransportSender initial; initial.set_data_channel_is_p2p(true);
  expect(initial.consider_dc("media-a", 0, 5000) && initial.holding_motion(), "default proven direct path waited for cloud performance baseline");
  ControlTransportSender sender; sender.set_data_channel_is_p2p(true); baseline(sender);
  expect(sender.consider_dc("media-a", 200, 5000), "direct trial lost after faster WSS baseline");
  for (std::uint64_t seq = 4; seq <= 6; ++seq) {
    auto frame = payload(seq, seq * 50); sender.decorate(frame); sender.note_sent(frame, ControlPath::DataChannel, seq * 50);
    expect(sender.note_result(response(frame, true, seq == 4), seq * 50 + 80, 5000).matched, "healthy direct qualification lost");
  }
  expect(sender.path() == ControlPath::DataChannel && !sender.holding_motion(), "healthy direct path never qualified");
  expect(!sender.observe(true, "media-a", 450, 5000) && sender.path() == ControlPath::DataChannel,
      "healthy direct path left merely because cloud might be faster");
  expect(sender.observe(false, "media-a", 451, 5000) && sender.path() == ControlPath::Wss,
      "direct preference prevented genuine cloud fallback");
  expect(!sender.consider_dc("media-a", 452, 5000), "direct failure retried without existing recovery debounce");
  auto fallback = payload(7, 452); sender.decorate(fallback); sender.note_sent(fallback, ControlPath::Wss, 452);
  expect(sender.note_result(response(fallback, true, true), 472, 5000).matched && !sender.selecting(),
      "fallback did not complete its own epoch barrier before another trial");
  expect(sender.consider_dc("media-a", 5451, 5000), "recovered direct candidate was never retried");
}

void signatures_bind_native_bytes_and_vehicle_results() {
  const std::string key = "01234567890123456789012345678901"; // Public test vector, never a deployed key.
  expect(control_transport_mac("abc", key) == "d5c75dc69d8243aa0882a04342d9ff7c73537a0311a7950800607be225594009", "SHA256 MAC disagrees with independent test vector");
  auto frame = payload(1, 1000); frame["control_transport"] = stamp(1, 0, ControlPath::DataChannel, 1);
  frame["native_mac"] = transport_message_mac(frame, key, false);
  expect(verify_transport_message(Json::parse(frame.dump()), key, false), "original native bytes lost authentication");
  auto forged = frame; forged["throttle"] = .7;
  expect(!verify_transport_message(forged, key, false), "page could forge actuation");
  forged = frame; forged["control_transport"]["epoch"] = 2;
  expect(!verify_transport_message(forged, key, false), "page could forge a new epoch");
  forged = frame; forged["throttle"] = 0; // JS parse/stringify can change 0.0 to 0.
  expect(!verify_transport_message(forged, key, false), "raw-byte courier requirement was lost");
  auto result = response(frame); result["vehicle_mac"] = transport_message_mac(result, key, true);
  expect(verify_transport_message(result, key, true), "vehicle result MAC invalid");
  auto bounded = bounded_vehicle_control_result(result, "session-001", "vehicle-001", "driver-001");
  expect(verify_transport_message(bounded, key, true), "cloud canonicalization changed authenticated vehicle result");
  bounded["accepted"] = false;
  expect(!verify_transport_message(bounded, key, true), "page could forge vehicle acceptance");
  expect(!verify_transport_message(frame, key, true), "command signature reflected as vehicle result");
}

void private_signing_material_is_not_public_session_state() {
  SignalingServerConfig config;
  config.driver_passwords = {{"fixture-driver", "fixture-password"}};
  config.device_tokens = {{"fixture-vehicle", "fixture-device"}};
  config.driver_vehicle_permissions = {{"fixture-driver", {"fixture-vehicle"}}};
  SignalingService service(config);
  const auto post = [&](std::string path, Json body) {
    HttpRequest request; request.method = "POST"; request.path = path; request.target = path;
    request.peer_address = "127.0.0.1"; request.body = body.dump();
    const auto response = service.handle(request);
    expect(response.status == 200, "fixture cloud POST rejected"); return Json::parse(response.body);
  };
  const auto login = post("/auth/driver_login", {{"driver_id", "fixture-driver"}, {"password", "fixture-password"}});
  const auto online = post("/vehicles/online", {{"vehicle_id", "fixture-vehicle"}, {"device_token", "fixture-device"},
      {"connection_id", "fixture-runtime"}, {"native_control_capabilities", kNativeControlCapabilities}});
  const auto session = post("/sessions", {{"driver_id", "fixture-driver"}, {"vehicle_id", "fixture-vehicle"},
      {"token", login.at("token")}, {"required_control_transport_capability", kControlTransportCapability}});
  expect(session.at("transport_mac_key").get<std::string>().size() >= 32, "native key not issued");
  HttpRequest public_state; public_state.method = "GET"; public_state.path = "/sessions/" + session.at("session_id").get<std::string>();
  public_state.target = public_state.path; public_state.peer_address = "127.0.0.1";
  public_state.query = {{"actor", "fixture-driver"}}; public_state.headers = {{"x-mine-teleop-driver-token", login.at("token").get<std::string>()}};
  const auto public_response = service.handle(public_state);
  expect(public_response.status == 200 && !Json::parse(public_response.body).contains("transport_mac_key"), "public session serializer exposed native signing material");
  HttpRequest vehicle_state; vehicle_state.method = "GET"; vehicle_state.path = "/vehicles/fixture-vehicle/session";
  vehicle_state.target = vehicle_state.path; vehicle_state.peer_address = "127.0.0.1";
  vehicle_state.query = {{"connection_generation", std::to_string(online.at("connection_generation").get<std::uint64_t>())}};
  vehicle_state.headers = {{"x-mine-teleop-device-token", "fixture-device"}};
  const auto vehicle_response = service.handle(vehicle_state);
  expect(vehicle_response.status == 200 && Json::parse(vehicle_response.body).at("transport_mac_key") == session.at("transport_mac_key"),
      "authenticated vehicle did not receive the same session key");
}

void bounded_status_rejects_unsupported_kinds_and_scope_mismatch() {
  Json status = {{"event", "vehicle_control_status"}, {"protocol", kControlTransportCapability},
      {"session_id", "session-001"}, {"vehicle_id", "vehicle-001"}, {"driver_id", "driver-001"},
      {"status", {{"event", "vehicle_telemetry"}, {"control_status_seq", 1}}}};
  expect(bounded_vehicle_control_status(status, "session-001", "vehicle-001", "driver-001").at("status").at("session_id") == "session-001",
      "authenticated status lost normalized session identity");
  auto invalid = status; invalid["status"]["driver_id"] = "other";
  throws([&] { bounded_vehicle_control_status(invalid, "session-001", "vehicle-001", "driver-001"); });
  invalid = status; invalid["status"]["event"] = "control_command";
  throws([&] { bounded_vehicle_control_status(invalid, "session-001", "vehicle-001", "driver-001"); });
  invalid = status; invalid["status"]["control_status_seq"] = 0;
  throws([&] { bounded_vehicle_control_status(invalid, "session-001", "vehicle-001", "driver-001"); });
  invalid = status; invalid["status"]["oversized"] = std::string(48 * 1024, 'x');
  throws([&] { bounded_vehicle_control_status(invalid, "session-001", "vehicle-001", "driver-001"); });
}

void real_service_switch_does_not_disable_watchdog() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<MockVehicleAdapter>();
  test::LogicalTimeControlService service(config, "driver-001", "session-001", "fixture", std::move(adapter));
  service.start(0);
  SessionControlProfileRequest profile; profile.vehicle_id = "vehicle-001"; profile.driver_id = "driver-001";
  profile.session_id = "session-001"; profile.control_token = "fixture"; profile.seq = 1;
  expect(service.receive_session_profile(profile, 0).accepted, "profile not ready");
  auto value = fence(); auto cmd = command(1, 0); cmd.throttle = .5;
  expect(admit(value, cmd, nullptr, ControlPath::Wss, 0).admitted && service.receive_command(cmd, 0).accepted, "initial WSS motion failed");
  auto wire = stamp(1, 0, ControlPath::DataChannel, 2); cmd = command(2, 50); cmd.control_link_ready = false; cmd.operator_input_ready = false;
  const auto selected = admit(value, cmd, &wire, ControlPath::DataChannel, 50);
  expect(selected.changed, "selection failed");
  service.set_control_braking_wait(true, 50, ControlRecoveryCause::ControlLink);
  const auto braking = service.receive_command(cmd, 50);
  if (!(braking.accepted && braking.command && braking.command->brake == 1))
    throw std::runtime_error("switch brake result: " + braking.reason + "; accepted=" + std::to_string(braking.accepted) +
        "; brake=" + (braking.command ? std::to_string(braking.command->brake) : "no-command"));
  cmd = command(900, 100); cmd.throttle = .8;
  expect(!admit(value, cmd, nullptr, ControlPath::Wss, 100).admitted, "old path refreshed watchdog");
  service.tick(50 + config.control.control_timeout_ms);
  expect(service.safety_state() == SafetyState::TimeoutBrake && service.session_control_profile().at("active") == false,
      "transport feature suppressed existing hard timeout/profile clear");
  service.close();
}
}  // namespace
int main() {
  try {
    version_scope_and_expiry(); single_receiver_epoch_replay_and_emergency(); exact_result_and_neutral_qualification();
    slower_dc_and_lost_selection(); cloud_ingress_without_vehicle_result_can_only_start_a_neutral_trial(); healthy_proven_p2p_is_preferred_to_a_faster_cloud_backup(); signatures_bind_native_bytes_and_vehicle_results(); private_signing_material_is_not_public_session_state(); bounded_status_rejects_unsupported_kinds_and_scope_mismatch(); real_service_switch_does_not_disable_watchdog();
    std::cout << "10 control transport cases passed; real common safety service, no sockets\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
