// In-process ICE fixture only: no signaling service, vehicle or control inputs.
#include "mine_teleop/detail/native_control_channel.hpp"
#include <agent.h>
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
  std::mutex mutex;
  std::vector<std::pair<guint, std::string>> candidates;
  std::atomic<bool> open{false}, received{false};
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
    if (agent) g_object_unref(agent);
    gst_object_unref(pipeline);
  }
  void start(bool initiator) {
    if (initiator) {
      auto* options = gst_structure_new("control", "ordered", G_TYPE_BOOLEAN, FALSE, "max-retransmits", G_TYPE_INT, 0,
                                        "protocol", G_TYPE_STRING, "mine-teleop-control-v1", nullptr);
      g_signal_emit_by_name(rtc, "create-data-channel", "control", options, &channel); gst_structure_free(options);
      require(channel != nullptr, "channel unavailable");
      g_signal_connect(channel, "on-open", G_CALLBACK(+[](GstWebRTCDataChannel*, gpointer data) { static_cast<Peer*>(data)->open = true; }), this);
      g_signal_connect(channel, "on-message-string", G_CALLBACK(+[](GstWebRTCDataChannel*, gchar* text, gpointer data) {
        if (text && std::string(text) == "response") static_cast<Peer*>(data)->received = true;
      }), this);
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
}
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    Peer a, b;
#if GST_CHECK_VERSION(1, 22, 0)
    NativeControlChannel courier(b.rtc, {"fixture", 1, "attempt"});
    require(!courier.ready() && !courier.send("before-open"), "unopened channel accepted send");
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
    require(delivered && a.received, "bidirectional DataChannel delivery failed");
    courier.retire();
    require(!courier.ready() && !courier.send("after-retire") && !courier.pop(), "retired channel still usable");
    std::cout << "PASS scoped DataChannel loopback delivery and retirement\n";
#else
    bool rejected = false;
    try { NativeControlChannel courier(b.rtc, {"fixture", 1, "attempt"}); }
    catch (const std::runtime_error&) { rejected = true; }
    require(rejected, "old GStreamer must reject control channel setup");
    std::cout << "PASS fail closed without early DataChannel observation\n";
#endif
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
