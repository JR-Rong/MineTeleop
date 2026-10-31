// In-process ICE fixture only: no signaling service, vehicle or control inputs.
#include "mine_teleop/detail/native_control_channel.hpp"
#include "mine_teleop/detail/native_control_courier.hpp"
#include "mine_teleop/detail/mock_compact_control.hpp"
#include <agent.h>
#include <gio/gnetworking.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using mine_teleop::detail::NativeControlChannel;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Peer {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* rtc = gst_element_factory_make("webrtcbin", nullptr);
  NiceAgent* agent{};
  GstWebRTCDataChannel* channel{};
  GstWebRTCDataChannel* decoy{};
  std::mutex mutex;
  std::vector<std::pair<guint, std::string>> candidates;
  std::atomic<bool> open{false}, received{false}, courier_command_received{false}, compact_received{false};
  Peer() {
    require(pipeline && rtc, "webrtcbin unavailable");
    gst_bin_add(GST_BIN(pipeline), rtc);
    GObject* ice = nullptr;
    g_object_get(rtc, "ice-agent", &ice, nullptr);
    require(ice != nullptr, "ICE unavailable");
    g_object_get(ice, "agent", &agent, nullptr); g_object_unref(ice);
    require(agent != nullptr, "libnice unavailable");
    NiceAddress address; nice_address_init(&address);
    require(nice_address_set_from_string(&address, "127.0.0.1") && nice_agent_add_local_address(agent, &address), "loopback bind failed");
    g_object_set(agent, "ice-tcp", FALSE, nullptr);
    g_signal_connect(rtc, "on-ice-candidate", G_CALLBACK(+[](GstElement*, guint index, gchar* value, gpointer data) {
      auto& self = *static_cast<Peer*>(data);
      std::lock_guard lock(self.mutex); self.candidates.emplace_back(index, value);
    }), this);
    require(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "READY failed");
  }
  ~Peer() {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (channel) g_object_unref(channel);
    if (decoy) g_object_unref(decoy);
    if (agent) g_object_unref(agent);
    gst_object_unref(pipeline);
  }
  void start(bool initiator) {
    if (initiator) {
      g_signal_emit_by_name(rtc, "create-data-channel", "unrelated", nullptr, &decoy);
      require(decoy != nullptr, "decoy channel unavailable");
      auto* options = gst_structure_new("control", "ordered", G_TYPE_BOOLEAN, FALSE, "max-retransmits", G_TYPE_INT, 0,
                                        "protocol", G_TYPE_STRING, "mine-teleop-control-v1", nullptr);
      g_signal_emit_by_name(rtc, "create-data-channel", "control", options, &channel); gst_structure_free(options);
      require(channel != nullptr, "channel unavailable");
      g_signal_connect(channel, "on-open", G_CALLBACK(+[](GstWebRTCDataChannel*, gpointer data) { static_cast<Peer*>(data)->open = true; }), this);
      g_signal_connect(channel, "on-message-string", G_CALLBACK(+[](GstWebRTCDataChannel*, gchar* text, gpointer data) {
        if (text && std::string(text) == "response") static_cast<Peer*>(data)->received = true;
        if (text && std::string(text).find("\"fixture_command\":true") != std::string::npos)
          static_cast<Peer*>(data)->courier_command_received = true;
      }), this);
      g_signal_connect(channel, "on-message-data", G_CALLBACK((+[](GstWebRTCDataChannel*, GBytes* bytes, gpointer data) {
        if (!bytes) return;
        gsize size = 0; const auto* raw = static_cast<const std::uint8_t*>(g_bytes_get_data(bytes, &size));
        if (!raw) return;
        const mine_teleop::detail::MockCompactScope scope{"vehicle-001", "driver-001", "session-001", "fixture", "media-a"};
        const auto value = mine_teleop::detail::decode_mock_compact_control(std::span<const std::uint8_t>(raw, size), scope);
        const std::string key = "01234567890123456789012345678901";
        if (value && mine_teleop::detail::verify_transport_message(*value, key, false) && value->at("seq") == 42)
          static_cast<Peer*>(data)->compact_received = true;
      })), this);
    }
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "PLAYING failed");
  }
  static void apply(GstElement* rtc, const char* action, GstWebRTCSessionDescription* desc) {
    auto* promise = gst_promise_new(); g_signal_emit_by_name(rtc, action, desc, promise);
    const auto status = gst_promise_wait(promise); const auto* reply = gst_promise_get_reply(promise);
    const bool ok = status == GST_PROMISE_RESULT_REPLIED && (!reply || !gst_structure_has_field(reply, "error"));
    gst_promise_unref(promise); require(ok, "SDP application failed");
  }
  void describe(Peer& remote, bool offer) {
    auto* promise = gst_promise_new();
    g_signal_emit_by_name(rtc, offer ? "create-offer" : "create-answer", nullptr, promise);
    require(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED, "SDP creation failed");
    GstWebRTCSessionDescription* desc = nullptr; const auto* reply = gst_promise_get_reply(promise);
    require(reply && gst_structure_get(reply, offer ? "offer" : "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &desc, nullptr), "SDP missing");
    gst_promise_unref(promise); apply(rtc, "set-local-description", desc); apply(remote.rtc, "set-remote-description", desc);
    gst_webrtc_session_description_free(desc);
  }
  void deliver(Peer& other) {
    std::vector<std::pair<guint, std::string>> pending;
    { std::lock_guard lock(mutex); pending.swap(candidates); }
    for (const auto& [index, text] : pending) g_signal_emit_by_name(other.rtc, "add-ice-candidate", index, text.c_str());
  }
};
std::optional<std::vector<std::uint8_t>> compact_fixture() {
  mine_teleop::ControlCommand command;
  command.vehicle_id = "vehicle-001"; command.driver_id = "driver-001";
  command.session_id = "session-001"; command.control_token = "fixture";
  command.seq = 42; command.sent_at_utc_ms = 123456789; command.gear = "N";
  auto frame = command.to_json();
  frame["intent_seq"] = 43; frame["intent_fresh"] = true; frame["intent_requires_fresh_input"] = false;
  frame["control_transport"] = mine_teleop::detail::ControlTransportStamp{
      2, 1, 42, mine_teleop::detail::ControlPath::DataChannel, "media-a", false}.to_json();
  frame["native_mac"] = mine_teleop::detail::transport_message_mac(
      frame, "01234567890123456789012345678901", false);
  return mine_teleop::detail::encode_mock_compact_control(frame);
}
}
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  // libnice parses numeric addresses through getaddrinfo; on Windows this
  // must run after Winsock initialization even for 127.0.0.1.
  g_networking_init();
  try {
    using mine_teleop::detail::native_direct_udp_candidate;
    require(native_direct_udp_candidate("host", "udp") && native_direct_udp_candidate("srflx", "UDP") &&
            native_direct_udp_candidate("prflx", "udp"), "direct UDP candidate rejected");
    require(!native_direct_udp_candidate("relay", "udp") && !native_direct_udp_candidate("host", "tcp") &&
            !native_direct_udp_candidate("", "udp"), "relay, TCP or unknown candidate admitted");
    bool rejected_null = false;
    try { NativeControlChannel invalid(nullptr, {"fixture", 1, "attempt"}); }
    catch (const std::invalid_argument&) { rejected_null = true; }
    require(rejected_null, "null peer must be rejected");
    Peer a, b;
#if GST_CHECK_VERSION(1, 22, 0)
    NativeControlChannel courier(b.rtc, {"fixture", 1, "attempt"});
    require(!courier.ready() && !courier.direct_udp() && !courier.send("before-open"), "unopened channel accepted send");
    a.start(true); b.start(false); a.describe(b, true); b.describe(a, false);
    bool sent = false, delivered = false, responded = false;
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (std::chrono::steady_clock::now() < deadline && !a.received) {
      a.deliver(b); b.deliver(a);
      if (a.open && !sent) { g_signal_emit_by_name(a.channel, "send-string", "request"); sent = true; }
      if (auto value = courier.pop()) { require(*value == "request", "incorrect inbound message"); delivered = true; }
      if (delivered && courier.ready() && !responded) responded = courier.send("response");
      std::this_thread::sleep_for(10ms);
    }
    if (!delivered || !a.received)
      std::cerr << "loopback state: initiator_open=" << a.open << " courier_ready=" << courier.ready()
                << " request_sent=" << sent << " request_received=" << delivered
                << " response_queued=" << responded << " response_received=" << a.received << '\n';
    require(delivered && a.received, "bidirectional DataChannel delivery failed");
    const auto compact = compact_fixture();
    require(compact && courier.send_mock_compact(*compact), "compact mock message did not queue");
    const auto compact_deadline = std::chrono::steady_clock::now() + 3s;
    while (!a.compact_received && std::chrono::steady_clock::now() < compact_deadline)
      std::this_thread::sleep_for(10ms);
    require(a.compact_received, "signed compact mock message did not survive DataChannel transport");
#if GST_CHECK_VERSION(1, 22, 0)
    const auto send_stats = courier.send_stats();
    require(send_stats.queued >= 1 && send_stats.unavailable >= 1 &&
            send_stats.attempted == send_stats.queued + send_stats.buffered +
                send_stats.unavailable + send_stats.errors,
            "channel send diagnostics do not account for queued and rejected attempts");
#endif
#if GST_CHECK_VERSION(1, 28, 0)
    const auto route_deadline = std::chrono::steady_clock::now() + 3s;
    while (!courier.direct_udp() && std::chrono::steady_clock::now() < route_deadline) std::this_thread::sleep_for(10ms);
    require(courier.direct_udp(), "SCTP transport did not attest the loopback UDP host pair");
    require(courier.direct_udp("127.0.0.1", "127.0.0.1") &&
            !courier.direct_udp("127.0.0.1", "192.0.2.1"), "SCTP candidate endpoint guard failed");
    {
      using mine_teleop::detail::NativeControlCourier;
      using mine_teleop::detail::NativeCourierScope;
      using Json = nlohmann::json;
      NativeCourierScope scope{{"fixture", 1, "attempt"}, "fixture-owner", 1};
      NativeControlCourier bridge(scope);
      bool ack_sent = false, ack_bridged = false;
      const Json command{{"seq", std::uint64_t{1}}, {"session_id", "fixture"}, {"fixture_command", true},
          {"control_transport", {{"protocol", "native-control-transport-v1"}, {"path", "dc"}, {"media_attempt", "attempt"}}}};
      NativeControlCourier::Io io{
          [&] { return courier.pop(); }, [&] { return courier.direct_udp(); },
          [&](const Json& request) {
            if (request.contains("vehicle_result")) ack_bridged = request.at("vehicle_result").value("result_seq", 0) == 1;
            return Json{{"protocol", "native-control-transport-v1"}, {"enabled", true}, {"valid_for_ms", 100},
                {"frame", command}, {"frame_json", command.dump()}, {"statuses", Json::array()}};
          }, [&](const std::string& bytes) { return courier.send(bytes); }, {}};
      const auto courier_deadline = std::chrono::steady_clock::now() + 5s;
      while (!ack_bridged && std::chrono::steady_clock::now() < courier_deadline) {
        require(bridge.tick(scope, io), "native courier unexpectedly retired");
        if (a.courier_command_received && !ack_sent) {
          const Json ack{{"event", "vehicle_control_result"}, {"protocol", "native-control-transport-v1"},
              {"session_id", "fixture"}, {"command_seq", 1}, {"result_seq", 1}};
          g_signal_emit_by_name(a.channel, "send-string", ack.dump().c_str()); ack_sent = true;
        }
        std::this_thread::sleep_for(10ms);
      }
      require(a.courier_command_received && ack_bridged, "native courier did not exchange command and receipt over SCTP");
      bridge.retire();
    }
#else
    require(!courier.direct_udp(), "old GStreamer must not infer SCTP route from media");
#endif
    auto* duplicate_options = gst_structure_new("control", "ordered", G_TYPE_BOOLEAN, FALSE,
        "max-retransmits", G_TYPE_INT, 0, "protocol", G_TYPE_STRING, "mine-teleop-control-v1", nullptr);
    GstWebRTCDataChannel* duplicate = nullptr;
    g_signal_emit_by_name(a.rtc, "create-data-channel", "control", duplicate_options, &duplicate);
    gst_structure_free(duplicate_options);
    require(duplicate != nullptr, "duplicate channel unavailable");
    const auto duplicate_deadline = std::chrono::steady_clock::now() + 5s;
    while (courier.ready() && std::chrono::steady_clock::now() < duplicate_deadline)
      std::this_thread::sleep_for(10ms);
    require(!courier.ready() && !courier.send("after-duplicate"), "duplicate channel did not fail closed");
    g_object_unref(duplicate);
    courier.retire();
    require(!courier.ready() && !courier.direct_udp() && !courier.send("after-retire") && !courier.pop(), "retired channel still usable");
    std::cout << "PASS scoped DataChannel loopback delivery and retirement\n";
#else
    bool rejected = false;
    try { NativeControlChannel courier(b.rtc, {"fixture", 1, "attempt"}); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "old GStreamer must reject control channel setup");
    // The older stack cannot support the native courier's early observation,
    // but can still exercise the vehicle-facing binary channel and decoder.
    g_signal_connect(b.rtc, "on-data-channel", G_CALLBACK(+[](GstElement*, GstWebRTCDataChannel* channel, gpointer data) {
      auto* peer = static_cast<Peer*>(data);
      gchar* label = nullptr; g_object_get(channel, "label", &label, nullptr);
      const bool control = label && std::string_view(label) == "control";
      g_free(label); if (!control) return;
      peer->channel = GST_WEBRTC_DATA_CHANNEL(g_object_ref(channel));
      g_signal_connect(channel, "on-open", G_CALLBACK(+[](GstWebRTCDataChannel*, gpointer owner) {
        static_cast<Peer*>(owner)->open = true;
      }), peer);
      GstWebRTCDataChannelState state = GST_WEBRTC_DATA_CHANNEL_STATE_CLOSED;
      g_object_get(channel, "ready-state", &state, nullptr);
      if (state == GST_WEBRTC_DATA_CHANNEL_STATE_OPEN) peer->open = true;
    }), &b);
    a.start(true); b.start(false); a.describe(b, true); b.describe(a, false);
    const auto open_deadline = std::chrono::steady_clock::now() + 15s;
    while (!(a.open && b.open) && std::chrono::steady_clock::now() < open_deadline) {
      a.deliver(b); b.deliver(a); std::this_thread::sleep_for(10ms);
    }
    require(a.open && b.open, "old GStreamer binary loopback channel did not open");
    const auto compact = compact_fixture(); require(compact.has_value(), "compact fixture could not encode");
    auto* compact_bytes = g_bytes_new(compact->data(), compact->size());
    g_signal_emit_by_name(b.channel, "send-data", compact_bytes); g_bytes_unref(compact_bytes);
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!a.compact_received && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(10ms);
    require(a.compact_received, "old GStreamer binary channel changed the signed mock message");
    std::cout << "PASS fail closed without early DataChannel observation\n";
#endif
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
