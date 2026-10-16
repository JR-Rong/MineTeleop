#include "mine_teleop/detail/feedback_transport.hpp"
#include "mine_teleop/media.hpp"
#include <mutex>

#include <iostream>
#include <atomic>
#include <map>
#include <vector>

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
}
int main() {
  try { cloud_only_startup_retains_all_safety_gates(); cloud_bootstrap_feedback_requires_live_authenticated_lease(); publish_all_four_independent_axes(); std::cout << "Actual vehicle feedback publisher: four axes, real MAC and bounded snapshots passed; fake Gst send, no ABI/network proof\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
