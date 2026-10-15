#include "mine_teleop/detail/feedback_transport.hpp"

#include <iostream>
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
    std::string session_id() const { return "s"; }
    std::string driver_id() const { return "d"; }
    std::string transport_mac_key() const { return std::string(64, 'k'); }
  } signaling;
  FakeChannel* control_channel{};
  bool control_link_open{true};
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
}
int main() {
  try { publish_all_four_independent_axes(); std::cout << "Actual vehicle feedback publisher: four axes, real MAC and bounded snapshots passed; fake Gst send, no ABI/network proof\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
