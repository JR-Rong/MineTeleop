#include "mine_teleop/detail/feedback_transport.hpp"
#include "mine_teleop/detail/emergency_stop.hpp"
#include "mine_teleop/media.hpp"
#include <mutex>

#include <iostream>
#include <atomic>
#include <map>
#include <vector>
#include <algorithm>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
struct FakeChannel { std::vector<Json> sent; };
void gst_webrtc_data_channel_send_string(FakeChannel* channel, const char* bytes) { channel->sent.push_back(Json::parse(bytes)); }
struct Publisher {
  struct { struct { int experimental_direct_authority_max_ms{600}; } runtime; std::string vehicle_id{"v"}; } config;
  struct {
    std::int64_t clock{1000}, expiry{2000};
    std::int64_t now_ms() const { return clock; }
    std::int64_t control_lease_expiry() const { return expiry; }
    std::string session_id() const { return "s"; }
    std::string driver_id() const { return "d"; }
    std::string transport_mac_key() const { return std::string(64, 'k'); }
  } signaling;
  FakeChannel* control_channel{};
  bool control_link_open{true};
  std::atomic<bool> native_control_websocket_connected{false}, native_control_transport_cloud_capable{false};
  ControlTransportFence control_transport_fence;
  FeedbackTransportFence feedback_transport_fence;
  std::map<std::string, Json> latest_feedback_status, pending_control_status;
#include "feedback_vehicle_method.inc"
};
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
Json selection(ControlPath path) {
  FeedbackTransportStamp stamp{1, 0, 7, 0, path, "a", "issuer"};
  auto value = stamp.json(); value["select"] = true; value["probe"] = false; return value;
}
// Runs the production startup body with a real common service and Mock adapter.
// No GStreamer, CAN, sockets, clocks or host settings are changed.
struct Startup {
  VehicleConfig config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  struct {
    std::int64_t clock{1000}, expiry{2000};
    std::int64_t now_ms() const { return clock; }
    std::int64_t control_lease_expiry() const { return expiry; }
    std::string session_id() const { return "s"; }
    std::string driver_id() const { return "d"; }
    std::string control_token() const { return "t"; }
    std::string transport_mac_key() const { return std::string(64, 'k'); }
  } signaling;
  std::atomic<bool> native_control_websocket_connected{true}, native_control_transport_cloud_capable{true};
  std::atomic<bool> stop_requested{false}, control_inhibited{false};
  bool control_link_open{false}, control_service_started{false}, cameras_ready{true};
  FakeChannel* control_channel{};
  std::mutex control_mutex;
  std::string control_service_issue_code;
  std::uint64_t last_vehicle_telemetry_seq{};
  std::unique_ptr<VehicleControlService> control_service;
  std::shared_ptr<SessionControlRecoveryLatch> control_recovery_latch = std::make_shared<SessionControlRecoveryLatch>();
  Startup() { config.runtime.control_enabled = true; config.vehicle_adapter.type = "mock"; static_cast<void>(control_recovery_latch->enter_session("s")); }
  ~Startup() { if (control_service) control_service->close(); }
  bool critical_cameras_ready() const { return cameras_ready; }
  std::string_view recovery_inhibition_issue() const { return "fixture_inhibited"; }
  void invalidate_native_control_trusted_gear_locked() {}
  template<class... Args> void emit_diagnostic(Args&&...) {}
  void emit_diagnostic(std::string_view, std::string_view, std::string_view, std::string_view,
      std::string_view, bool, const Json&) {}
#include "control_startup_method.inc"
};

struct EarlyResult : Startup {
  enum class ControlMessageTransport { DataChannel, NativeSignaling };
  ControlTransportFence control_transport_fence;
  FeedbackTransportFence feedback_transport_fence;
  DirectAuthorityLease direct_authority;
  EmergencyStopReceiver emergency_stop_receiver;
  std::uint64_t control_result_seq{}, control_status_seq{}, rejected_control_commands{};
  Json latest_control_result;
  std::vector<Json> publication_traces;
  void enqueue_control_trace(Json value) { publication_traces.push_back(std::move(value)); }
  void note_control_trace_drop() { throw std::runtime_error("unexpected publication trace loss"); }
  std::string trace_reason;
  bool receive_apply_invoked{};
  static std::int64_t steady_now_ms() { return 1000; }
  template<class... Times>
  void queue_control_trace(const ReceiveResult*, std::string_view reason, bool invoked, Times...) {
    if (config.runtime.control_log_commands) trace_reason = reason;
    receive_apply_invoked = invoked;
  }
  void deliver(ControlCommand command, ControlMessageTransport transport, FakeChannel* channel, Json* native_feedback,
      const Json& message = Json::object()) {
    const std::uint64_t intent_seq = 9;
    const std::string current_attempt = "fixture-attempt";
#include "control_early_result.inc"
    throw std::runtime_error("early rejection fixture unexpectedly passed its guard");
  }
};

void transport_expiry_has_signed_reason_and_trace_without_apply() {
  for (const bool trace : {false, true}) for (const bool dc : {false, true}) for (const int offset : {-201, 201}) {
    EarlyResult value;
    expect(value.start_control_service(), "expiry fixture failed to start mock service");
    value.config.runtime.control_log_commands = trace;
    value.config.control.max_command_gap_ms = 200;
    FakeChannel channel;
    value.control_channel = &channel;
    value.control_link_open = true;
    value.control_transport_fence.reset(value.config.vehicle_id, "d", "s", "t");
    ControlCommand command;
    command.vehicle_id = value.config.vehicle_id; command.driver_id = "d"; command.session_id = "s";
    command.control_token = "t"; command.seq = 42; command.sent_at_utc_ms = 1000 + offset;
    const ControlTransportStamp stamp{1, 0, 42, dc ? ControlPath::DataChannel : ControlPath::Wss,
        dc ? "fixture-attempt" : "", true};
    Json feedback;
    value.deliver(command, dc ? EarlyResult::ControlMessageTransport::DataChannel : EarlyResult::ControlMessageTransport::NativeSignaling,
        &channel, &feedback, {{"control_transport", stamp.to_json()}});
    expect(feedback.at("reason") == "transport_command_expired" && !feedback.at("accepted").get<bool>() &&
        !feedback.at("transport_admitted").get<bool>() && !feedback.at("rearm_required").get<bool>(),
        "transport rejection lost reason or changed control semantics");
    expect(verify_transport_message(feedback, value.signaling.transport_mac_key(), true), "expiry result lost authentication");
    expect(trace ? value.trace_reason == "transport_command_expired" : value.trace_reason.empty(),
        "transport admission rejection skipped its diagnostic trace");
    expect(value.publication_traces.size() == (trace ? 1U : 0U), "publication trace enablement changed");
    if (trace) {
      const auto& published = value.publication_traces.front();
      expect(published.at("seq") == command.seq && published.at("dc_send_invoked") == dc &&
          published.at("wss_mailbox_updated") == true && published.at("result_mutex_wait_ms") == 0,
          "publication trace lost the actual send path or command correlation");
      expect(!published.contains("vehicle_mac") && !published.contains("control_token"), "trace leaked authentication material");
    }
    expect(value.rejected_control_commands == 1 && !value.receive_apply_invoked &&
        value.control_service->adapter_status().applied_command_count == 0,
        "expired command reached mock apply or changed rejection count");
  }
}

void early_rejection_publishes_precise_signed_result() {
  // All four reasons, both service-unavailable states, both receive paths, and
  // tracing disabled/enabled. Gst is a byte sink; there is no real connection.
  for (const bool trace : {false, true}) for (const bool dc : {false, true}) for (int gate = 0; gate != 5; ++gate) {
    EarlyResult value;
    expect(value.start_control_service(), "mock service fixture did not start");
    value.config.runtime.control_log_commands = trace;
    FakeChannel channel;
    value.control_channel = &channel;
    value.control_link_open = true;
    const char* reason = nullptr;
    if (gate == 0) { value.stop_requested = true; value.control_inhibited = true; reason = "runtime_stop_requested"; }
    if (gate == 1) { value.control_inhibited = true; reason = "control_inhibited"; }
    if (gate == 2) { value.control_service_started = false; reason = "control_service_unavailable"; }
    if (gate == 3) { value.control_service->close(); value.control_service.reset(); reason = "control_service_unavailable"; }
    if (gate == 4) { value.control_link_open = false; value.native_control_websocket_connected = false; reason = "control_link_not_open"; }
    ControlCommand command;
    command.vehicle_id = value.config.vehicle_id; command.driver_id = "d"; command.session_id = "s";
    command.control_token = "t"; command.seq = 42;
    Json feedback;
    value.deliver(command, dc ? EarlyResult::ControlMessageTransport::DataChannel : EarlyResult::ControlMessageTransport::NativeSignaling,
        &channel, &feedback);
    const auto verify = [&](const Json& result) {
      expect(result.at("reason") == reason, "published result lost the precise early rejection reason");
      expect(!result.at("accepted").get<bool>() && !result.at("rearm_required").get<bool>() &&
          !result.at("transport_admitted").get<bool>() && !result.at("transport_selected").get<bool>(),
          "diagnostic rejection changed acceptance, rearm or transport semantics");
      expect(result.at("command_seq") == 42 && result.at("intent_seq") == 9 && result.at("result_seq") == 1,
          "early result lost sequence correlation");
      expect(verify_transport_message(result, value.signaling.transport_mac_key(), true), "early result MAC is invalid");
      const auto bounded = bounded_vehicle_control_result(result, "s", value.config.vehicle_id, "d");
      expect(bounded.at("reason") == reason && verify_transport_message(bounded, value.signaling.transport_mac_key(), true),
          "wire normalization dropped the reason or changed the signed result");
    };
    verify(feedback);
    expect(value.latest_control_result == feedback, "native feedback and WSS result differ");
    expect(channel.sent.size() == (dc && value.control_link_open ? 1U : 0U), "early result used an unavailable DC");
    for (const auto& sent : channel.sent) verify(sent);
    expect(value.rejected_control_commands == 1 && !value.receive_apply_invoked,
        "early rejection invoked the common receiver or counted incorrectly");
    expect(trace ? value.trace_reason == reason : value.trace_reason.empty(), "trace setting changed rejection diagnostics");
    if (value.control_service)
      expect(value.control_service->adapter_status().applied_command_count == 0, "early rejection applied a mock command");
  }
}
void cloud_only_startup_retains_all_safety_gates() {
  for (int gate = 0; gate != 7; ++gate) {
    Startup value;
    if (gate == 0) value.native_control_websocket_connected = false;
    if (gate == 1) value.native_control_transport_cloud_capable = false;
    if (gate == 2) value.signaling.expiry = value.signaling.clock;
    if (gate == 3) value.cameras_ready = false;
    if (gate == 4) value.control_inhibited = true;
    if (gate == 5) value.stop_requested = true;
    if (gate == 6) value.config.runtime.control_enabled = false;
    expect(!value.start_control_service() && !value.control_service_started && !value.control_service,
        "cloud-only startup opened an adapter across a failed safety gate");
  }
  Startup live;
  expect(live.start_control_service() && live.control_service_started && live.control_service &&
      !live.control_service->session_control_profile().value("active", false),
      "live authenticated WSS did not bootstrap the common safety service without DC, or armed a profile");
}
void publish_all_four_independent_axes() {
  for (const auto command : {ControlPath::Wss, ControlPath::DataChannel}) for (const auto feedback : {ControlPath::Wss, ControlPath::DataChannel}) {
    Publisher publisher; FakeChannel channel; publisher.control_channel = &channel;
    publisher.control_transport_fence.reset("v", "d", "s", "t");
    ControlCommand value; value.vehicle_id = "v"; value.driver_id = "d"; value.session_id = "s"; value.control_token = "t";
    value.seq = 1; value.sent_at_utc_ms = 1000;
    ControlTransportStamp command_stamp{1, 0, 1, command, command == ControlPath::DataChannel ? "a" : "", true};
    auto json = command_stamp.to_json();
    expect(publisher.control_transport_fence.admit(value, &json, command, "a", 1000, 2000, 200, true).admitted, "command fixture failed");
    expect(publisher.feedback_transport_fence.select(selection(feedback), {{"issuer", "issuer"}, {"authority_epoch", 7}}, "a", 0), "feedback fixture failed");
    std::uint64_t seq = 0;
    for (const auto* kind : {"vehicle_telemetry", "vcu_handshake_status", "session_control_profile_status", "control_command_rejected"})
      publisher.send_control_status_locked({{"event", kind}, {"control_status_seq", ++seq}, {"evidence_received_at_utc_ms", 123}});
    expect(publisher.latest_feedback_status.size() == 4, "snapshot cache did not retain four kinds");
    expect(feedback == ControlPath::Wss ? publisher.pending_control_status.size() == 4 && channel.sent.empty() :
        publisher.pending_control_status.empty() && channel.sent.size() == 4, "actual vehicle publisher coupled feedback to command path");
    const auto verify = [&](const Json& message) {
      expect(message.at("feedback_transport").at("path") == control_path_name(feedback), "wrong feedback stamp");
      expect(message.at("status").at("evidence_received_at_utc_ms") == 123, "snapshot retimestamped CAN evidence");
      expect(verify_transport_message(message, publisher.signaling.transport_mac_key(), true), "actual publisher did not authenticate status");
      const auto bounded = bounded_vehicle_control_status(message, "s", "v", "d");
      expect(verify_transport_message(bounded, publisher.signaling.transport_mac_key(), true), "status canonicalization broke real MAC");
    };
    for (const auto& [kind, message] : publisher.pending_control_status) { static_cast<void>(kind); verify(message); }
    if (feedback == ControlPath::Wss) {
      const auto ordered = publisher.ordered_pending_status();
      expect(ordered.size() == 4, "WSS ordering lost a wrapped status");
      expect(ordered[0].at("status").at("event") == "vehicle_telemetry" &&
          ordered[1].at("status").at("event") == "vcu_handshake_status" &&
          ordered[2].at("status").at("event") == "session_control_profile_status" &&
          ordered[3].at("status").at("event") == "control_command_rejected",
          "WSS ordering changed authenticated feedback publication order");
      for (const auto& message : ordered) verify(message);
    }
    for (const auto& message : channel.sent) verify(message);
    publisher.send_control_status_locked({{"event", "vehicle_telemetry"}, {"session_id", "old"}, {"control_status_seq", 999}});
    for (int i = 0; i < 100; ++i) publisher.send_control_status_locked({{"event", "unknown" + std::to_string(i)}, {"control_status_seq", 999}});
    expect(publisher.latest_feedback_status.size() == 4 && publisher.latest_feedback_status.at("vehicle_telemetry").at("control_status_seq") == 1,
        "old identity or arbitrary kind poisoned bounded snapshot cache");
  }
}
void cloud_bootstrap_feedback_requires_live_authenticated_lease() {
  Publisher publisher;
  publisher.config.runtime.experimental_direct_authority_max_ms = 0;
  publisher.control_link_open = false;
  publisher.native_control_websocket_connected = true;
  publisher.native_control_transport_cloud_capable = true;
  expect(publisher.cloud_control_authority_live_locked(), "live cloud bootstrap was unavailable");
  publisher.send_control_status_locked({{"event", "vcu_handshake_status"}, {"control_status_seq", 1}});
  expect(publisher.pending_control_status.size() == 1, "WSS-only startup lost adapter readiness status");
  for (int kind = 0; kind != 3; ++kind) {
    publisher.pending_control_status.clear();
    publisher.native_control_websocket_connected = kind != 0;
    publisher.native_control_transport_cloud_capable = kind != 1;
    publisher.signaling.expiry = kind == 2 ? 1000 : 2000;
    expect(!publisher.cloud_control_authority_live_locked(), "closed/unsupported/expired cloud granted bootstrap authority");
    publisher.send_control_status_locked({{"event", "vehicle_telemetry"}, {"control_status_seq", 2}});
    expect(publisher.pending_control_status.empty(), "invalid cloud bootstrapped feedback");
  }
}
void raw_vcu_details_are_not_a_transport_wrapper() {
  Publisher publisher;
  publisher.config.runtime.experimental_direct_authority_max_ms = 0;
  publisher.control_link_open = false;
  publisher.native_control_websocket_connected = true;
  publisher.native_control_transport_cloud_capable = true;
  const Json vcu = {{"event", "vcu_handshake_status"}, {"control_status_seq", 3},
      {"status", {{"state", "standby"}, {"ready", false}}}};
  const Json telemetry = {{"event", "vehicle_telemetry"}, {"control_status_seq", 2}};
  const Json profile = {{"event", "session_control_profile_status"}, {"control_status_seq", 1}};
  const Json rejection = {{"event", "control_command_rejected"}, {"control_status_seq", 4}};
  for (const auto& message : {vcu, telemetry, rejection, profile})
    publisher.send_control_status_locked(message);
  const auto retained = publisher.pending_control_status;
  expect(publisher.ordered_pending_status() == std::vector<Json>{profile, telemetry, vcu, rejection},
      "raw VCU details hid the top-level status publication sequence");
  expect(publisher.pending_control_status == retained, "ordering mutated retained status payloads");

  // Both forms can share the send queue during feedback-path negotiation.
  const Json wrapped = {{"event", "vehicle_control_status"}, {"status", telemetry}};
  publisher.pending_control_status["vehicle_telemetry"] = wrapped;
  expect(publisher.ordered_pending_status() == std::vector<Json>{profile, wrapped, vcu, rejection},
      "mixed raw and wrapped feedback did not retain publication order");
  publisher.pending_control_status["vehicle_telemetry"] = {
      {"event", "vehicle_control_status"}, {"status", {{"event", "vehicle_telemetry"}}}};
  bool rejected = false;
  try { static_cast<void>(publisher.ordered_pending_status()); }
  catch (const Json::out_of_range&) { rejected = true; }
  expect(rejected, "malformed wrapper without a publication sequence was silently admitted");
}
}
int main() {
  try { transport_expiry_has_signed_reason_and_trace_without_apply(); raw_vcu_details_are_not_a_transport_wrapper(); early_rejection_publishes_precise_signed_result(); cloud_only_startup_retains_all_safety_gates(); cloud_bootstrap_feedback_requires_live_authenticated_lease(); publish_all_four_independent_axes(); std::cout << "Actual vehicle feedback publisher: expiry rejection trace, raw/wrapped status ordering, early rejection reasons, four axes, real MAC and bounded snapshots passed; fake Gst send, no ABI/network proof\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
