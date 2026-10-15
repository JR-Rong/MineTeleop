#include "mine_teleop/detail/feedback_transport.hpp"

#include <iostream>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
const std::string key(64, 'f');
void expect(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
struct Harness {
  FeedbackTransportSender sender;
  FeedbackTransportFence car;
  Json authority = {{"issuer", "fixture-issuer"}, {"authority_epoch", 7}, {"lease_seq", 1}};
  std::string attempt{"media-a"};
  std::uint64_t seq{}, results{}, forward_epoch{3};
  Json command(std::int64_t now, ControlPath forward = ControlPath::Wss, bool p2p = true) {
    sender.observe(true, p2p, attempt, authority, forward, forward_epoch, now, 5000);
    ControlCommand command; command.vehicle_id = "v"; command.driver_id = "d"; command.session_id = "s";
    command.control_token = "fixture-token"; command.seq = ++seq; command.sent_at_utc_ms = now;
    auto value = command.to_json(); value["intent_seq"] = seq;
    sender.decorate(value); value["native_mac"] = transport_message_mac(value, key, false);
    sender.sent(value, forward, now); return value;
  }
  Json reply(const Json& command, ControlPath forward = ControlPath::Wss) {
    expect(verify_transport_message(command, key, false), "real command MAC failed");
    expect(car.select(command.at("feedback_request"), authority, attempt, 3), "production vehicle selection rejected");
    Json result = {{"event", "vehicle_control_result"}, {"protocol", kControlTransportCapability},
        {"session_id", "s"}, {"vehicle_id", "v"}, {"driver_id", "d"}, {"command_seq", command.at("seq")},
        {"intent_seq", command.at("intent_seq")}, {"result_seq", ++results}, {"accepted", true}, {"rearm_required", false},
        {"control_status_seq", 0}, {"transport_admitted", true}, {"transport_selected", false}, {"authority_epoch", 3},
        {"reason", "accepted"}, {"feedback_request", command.at("feedback_request")}, {"feedback_transport", car.state()},
        {"command_received_path", control_path_name(forward)}};
    result["vehicle_mac"] = transport_message_mac(result, key, true); return result;
  }
  FeedbackTransportSender::Receipt receive(const Json& result, ControlPath returned, std::int64_t now) {
    expect(verify_transport_message(result, key, true), "real result MAC failed");
    const auto bounded = bounded_vehicle_control_result(result, "s", "v", "d");
    expect(verify_transport_message(bounded, key, true), "bounded result changed signed bytes");
    return sender.result(bounded, returned, now);
  }
  Json status(const Json& stamp, std::uint64_t seq) {
    Json value = {{"event", "vehicle_control_status"}, {"protocol", kControlTransportCapability},
        {"session_id", "s"}, {"vehicle_id", "v"}, {"driver_id", "d"}, {"feedback_transport", stamp},
        {"status", {{"event", "vehicle_telemetry"}, {"session_id", "s"}, {"vehicle_id", "v"}, {"driver_id", "d"}, {"control_status_seq", seq}}}};
    value["vehicle_mac"] = transport_message_mac(value, key, true); return value;
  }
  Json qualify(ControlPath forward = ControlPath::Wss) {
    for (int i = 0; i < 3; ++i) {
      const auto now = 1000 + i * 50; const auto value = reply(command(now, forward), forward);
      expect(receive(value, ControlPath::Wss, now + 5).progress, "first exact result failed to advance");
      expect(!receive(value, ControlPath::DataChannel, now + 10).progress, "mirror advanced same event twice");
    }
    return reply(command(1150, forward), forward);
  }
};
void vehicle_selection_cas_scope_and_no_motion_dependency() {
  Harness h; auto value = h.command(1000); auto request = value.at("feedback_request");
  auto bad = request; bad["authority_epoch"] = 6;
  expect(!h.car.select(bad, h.authority, h.attempt, 3), "retired authority selected feedback");
  expect(!h.car.select(request, h.authority, "retired-attempt", 3), "old media selected feedback");
  expect(h.car.select(request, h.authority, h.attempt, 3), "initial CAS failed");
  expect(h.car.select(request, h.authority, h.attempt, 99) && h.car.state().at("status_floor") == 3, "duplicate selection changed status floor");
  bad = request; bad["epoch"] = 20; bad["from_epoch"] = 19;
  expect(!h.car.select(bad, h.authority, h.attempt, 3), "epoch jumped");
}
void four_forward_return_combinations_and_preferred_p2p() {
  for (auto forward : {ControlPath::Wss, ControlPath::DataChannel}) {
    Harness h; auto selected = h.qualify(forward);
    expect(selected.at("feedback_transport").at("path") == "dc", "faster WSS displaced healthy preferred P2P feedback");
    expect(h.receive(selected, ControlPath::DataChannel, 1160).progress, "DC feedback failed on independent forward path");
    expect(!h.receive(selected, ControlPath::Wss, 1170).progress, "WSS mirror falsely advanced again");
    auto next = h.command(1200, forward); auto state = h.sender.status();
    expect(state.at("path") == "dc" && next.at("feedback_request").at("epoch") == 2, "healthy P2P oscillated to faster cloud");
    expect(state.at("dc_paired_round_trip_ms").is_number() && state.at("wss_paired_round_trip_ms").is_number(), "paired same-event timing was not exposed");
    ++h.forward_epoch; const auto changed = h.command(1250, forward);
    expect(changed.at("feedback_request").at("path") == "dc" && h.sender.status().at("dc_paired_round_trip_ms").is_null(),
        "forward cohort change mixed old paired timing or spuriously changed healthy feedback axis");
  }
}
void feedback_loss_falls_back_without_changing_command_axis() {
  Harness h; auto selected = h.qualify(ControlPath::DataChannel);
  expect(h.receive(selected, ControlPath::DataChannel, 1160).progress, "initial direct result missing");
  for (int i = 0; i < 5; ++i) {
    const auto now = 1200 + i * 100; const auto value = h.reply(h.command(now, ControlPath::DataChannel), ControlPath::DataChannel);
    static_cast<void>(h.receive(value, ControlPath::Wss, now + 10));
  }
  const auto value = h.command(1700, ControlPath::DataChannel);
  expect(value.at("feedback_request").at("path") == "wss" && h.forward_epoch == 3, "feedback loss did not independently select cloud backup");
  const auto result = h.reply(value, ControlPath::DataChannel);
  expect(h.receive(result, ControlPath::Wss, 1710).progress, "DC command with WSS feedback failed");
  expect(h.command(1750, ControlPath::DataChannel).at("feedback_request").at("path") == "wss", "failed DC return flapped before existing retry timeout");
}
void old_epoch_high_sequence_is_filtered_before_status_watermark() {
  Harness h; auto old = h.reply(h.command(1000)); h.receive(old, ControlPath::Wss, 1005); h.receive(old, ControlPath::DataChannel, 1010);
  for (int i = 1; i < 3; ++i) { auto value = h.reply(h.command(1000 + i * 50)); h.receive(value, ControlPath::Wss, 1005 + i * 50); h.receive(value, ControlPath::DataChannel, 1010 + i * 50); }
  const auto selected = h.reply(h.command(1150)); h.receive(selected, ControlPath::DataChannel, 1160);
  auto retired = h.status(old.at("feedback_transport"), std::numeric_limits<std::uint64_t>::max());
  expect(!h.sender.status(retired, ControlPath::Wss), "old epoch poisoned current status watermark");
  auto current = h.status(selected.at("feedback_transport"), 4);
  expect(h.sender.status(current, ControlPath::DataChannel), "current low sequence lost after retired high sequence");
  expect(!h.sender.status(current, ControlPath::DataChannel), "duplicate status advanced freshness");
  expect(!h.sender.status(h.status(selected.at("feedback_transport"), 5), ControlPath::Wss), "unselected status route advanced state");
  expect(h.sender.status(h.status(selected.at("feedback_transport"), 5), ControlPath::DataChannel), "wrong route poisoned watermark");
}
void exact_results_reject_duplicates_reordering_and_context_changes() {
  Harness h; auto first = h.reply(h.command(1000)); auto second = h.reply(h.command(1050));
  expect(h.receive(second, ControlPath::Wss, 1055).progress, "new result failed");
  expect(!h.receive(first, ControlPath::Wss, 1060).progress, "reordered result advanced command progress");
  expect(!h.receive(second, ControlPath::Wss, 1065).progress, "duplicate refreshed progress");
  auto bad = second; bad["command_received_path"] = "dc"; bad["vehicle_mac"] = transport_message_mac(bad, key, true);
  expect(!h.receive(bad, ControlPath::DataChannel, 1070).progress, "cached DC receipt fabricated WSS forward metric");
  h.forward_epoch = 4; static_cast<void>(h.command(1100));
  expect(!h.sender.matches_event(second) && !h.receive(second, ControlPath::DataChannel, 1110).progress, "old forward cohort affected new measurement");
  h.authority["authority_epoch"] = 8; static_cast<void>(h.command(1150));
  expect(!h.receive(second, ControlPath::DataChannel, 1160).progress, "retired short authority result updated feedback");
}
void lost_selection_reconciles_only_from_its_exact_result() {
  Harness h;
  for (int i = 0; i < 3; ++i) {
    auto value = h.reply(h.command(1000 + i * 50));
    h.receive(value, ControlPath::Wss, 1005 + i * 50); h.receive(value, ControlPath::DataChannel, 1010 + i * 50);
  }
  auto lost = h.command(1150); // DC selection epoch2 is lost before reaching vehicle epoch1.
  expect(lost.at("feedback_request").at("path") == "dc", "fixture did not propose DC");
  h.sender.observe(false, true, h.attempt, h.authority, ControlPath::Wss, h.forward_epoch, 1170, 5000);
  auto retry = h.command(1200);
  expect(!h.car.select(retry.at("feedback_request"), h.authority, h.attempt, 3), "lost CAS did not conflict");
  Json conflict = {{"event", "vehicle_control_result"}, {"protocol", kControlTransportCapability},
      {"session_id", "s"}, {"vehicle_id", "v"}, {"driver_id", "d"}, {"command_seq", retry.at("seq")},
      {"intent_seq", retry.at("intent_seq")}, {"result_seq", ++h.results}, {"accepted", true}, {"rearm_required", false},
      {"control_status_seq", 0}, {"transport_admitted", true}, {"transport_selected", false}, {"authority_epoch", 3},
      {"reason", "accepted"}, {"feedback_request", retry.at("feedback_request")}, {"feedback_transport", h.car.state()}, {"command_received_path", "wss"}};
  conflict["vehicle_mac"] = transport_message_mac(conflict, key, true);
  expect(!h.receive(conflict, ControlPath::Wss, 1205).progress, "conflict advanced command health");
  const auto rebased = h.command(1250); const auto current = h.reply(rebased);
  expect(rebased.at("feedback_request").at("from_epoch") == 1 && h.receive(current, ControlPath::Wss, 1255).progress,
      "exact authenticated CAS result could not reconcile lost proposal");
}
void bounded_history_and_one_probe() {
  Harness h; auto first = h.reply(h.command(1000));
  expect(h.sender.status().at("probe_pending") == true, "probe was not reserved");
  expect(!h.command(1050).at("feedback_request").at("probe").get<bool>(), "multiple paired probes were in flight");
  for (int i = 0; i < 512; ++i) static_cast<void>(h.command(1100 + i));
  expect(h.sender.status().at("retained_sends") == 128 && !h.sender.matches_event(first), "feedback history grew or retained evicted old event");
}
}
int main() {
  try {
    vehicle_selection_cas_scope_and_no_motion_dependency(); four_forward_return_combinations_and_preferred_p2p();
    feedback_loss_falls_back_without_changing_command_axis(); old_epoch_high_sequence_is_filtered_before_status_watermark();
    exact_results_reject_duplicates_reordering_and_context_changes(); lost_selection_reconciles_only_from_its_exact_result(); bounded_history_and_one_probe();
    std::cout << "7 production feedback transport groups passed; real MAC, four forward/return combinations, no sockets\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
