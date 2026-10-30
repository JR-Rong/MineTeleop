// Synthetic two-camera WebRTC loopback: no device, signaling or vehicle control.
#include "native_receiver_core.hpp"
#include <gio/gnetworking.h>
#include <set>

using namespace mine_teleop::bench;
namespace {
void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Sender {
  GstElement *pipeline{}, *peer{};
  GstWebRTCDataChannel* channel{};
  std::atomic<bool> channel_open{false}, channel_received{false};
  std::mutex mutex;
  std::deque<Json> candidates;
  Sender() {
    GError* error = nullptr;
    pipeline = gst_parse_launch(
      "webrtcbin name=peer bundle-policy=max-bundle "
      "videotestsrc is-live=true pattern=ball ! video/x-raw,width=320,height=240,framerate=30/1 ! "
      "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 ! h264parse ! rtph264pay pt=96 ! "
      "application/x-rtp,media=video,encoding-name=H264,payload=96 ! queue ! peer. "
      "videotestsrc is-live=true pattern=snow ! video/x-raw,width=320,height=240,framerate=30/1 ! "
      "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=30 ! h264parse ! rtph264pay pt=97 ! "
      "application/x-rtp,media=video,encoding-name=H264,payload=97 ! queue ! peer.", &error);
    if (error) { const std::string text = error->message; g_clear_error(&error); throw std::runtime_error(text); }
    require(pipeline != nullptr, "synthetic sender unavailable");
    peer = gst_bin_get_by_name(GST_BIN(pipeline), "peer");
    require(peer != nullptr, "sender peer unavailable");
    GObject* ice = nullptr;
    NiceAgent* agent = nullptr;
    g_object_get(peer, "ice-agent", &ice, nullptr);
    require(ice != nullptr, "sender ICE unavailable");
    g_object_get(ice, "agent", &agent, nullptr); g_object_unref(ice);
    require(agent != nullptr, "sender libnice unavailable");
    NiceAddress address; nice_address_init(&address);
    const bool bound = nice_address_set_from_string(&address, "127.0.0.1") && nice_agent_add_local_address(agent, &address);
    g_object_set(agent, "ice-tcp", FALSE, nullptr); g_object_unref(agent);
    require(bound, "cannot bind synthetic sender to loopback");
    g_signal_connect(peer, "on-ice-candidate", G_CALLBACK(+[](GstElement*, guint mline, gchar* value, gpointer data) {
      auto& self = *static_cast<Sender*>(data); std::lock_guard lock(self.mutex);
      self.candidates.push_back({{"candidate", value}, {"sdpMLineIndex", mline}});
    }), this);
#if GST_CHECK_VERSION(1, 22, 0)
    require(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "sender READY failed");
    auto* channel_options = gst_structure_new("control", "ordered", G_TYPE_BOOLEAN, FALSE,
        "max-retransmits", G_TYPE_INT, 0, "protocol", G_TYPE_STRING, "mine-teleop-control-v1", nullptr);
    g_signal_emit_by_name(peer, "create-data-channel", "control", channel_options, &channel);
    gst_structure_free(channel_options);
    require(channel != nullptr, "sender control channel unavailable");
    g_signal_connect(channel, "on-open", G_CALLBACK(+[](GstWebRTCDataChannel*, gpointer data) {
      static_cast<Sender*>(data)->channel_open = true;
    }), this);
    g_signal_connect(channel, "on-message-string", G_CALLBACK(+[](GstWebRTCDataChannel*, gchar* value, gpointer data) {
      if (value && std::string(value) == "response") static_cast<Sender*>(data)->channel_received = true;
    }), this);
#endif
    require(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "sender failed to start");
  }
  ~Sender() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL);
    if (channel) { g_signal_handlers_disconnect_by_data(channel, this); g_object_unref(channel); }
    if (peer) { g_signal_handlers_disconnect_by_data(peer, this); gst_object_unref(peer); }
    if (pipeline) gst_object_unref(pipeline);
  }
  std::string offer() {
    auto reply = request([&](GstPromise* promise) { g_signal_emit_by_name(peer, "create-offer", nullptr, promise); });
    GstWebRTCSessionDescription* offer = nullptr;
    require(reply && gst_structure_get(reply.get(), "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr) && offer, "offer missing");
    request([&](GstPromise* promise) { g_signal_emit_by_name(peer, "set-local-description", offer, promise); });
    gchar* sdp = gst_sdp_message_as_text(offer->sdp);
    const std::string text = sdp ? sdp : "";
    g_free(sdp); gst_webrtc_session_description_free(offer);
    return text;
  }
  void answer(const std::string& text) {
    GstSDPMessage* sdp = nullptr;
    require(gst_sdp_message_new(&sdp) == GST_SDP_OK, "SDP allocation failed");
    require(gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(text.data()), static_cast<guint>(text.size()), sdp) == GST_SDP_OK, "bad answer");
    auto* description = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
    request([&](GstPromise* promise) { g_signal_emit_by_name(peer, "set-remote-description", description, promise); });
    gst_webrtc_session_description_free(description);
  }
  void exchange(Receiver& receiver) {
    std::deque<Json> pending;
    { std::lock_guard lock(mutex); pending.swap(candidates); }
    for (auto& candidate : pending) {
      candidate["media_negotiation_id"] = "test-attempt";
      receiver.candidate(candidate);
    }
    for (const auto& candidate : receiver.candidates())
      g_signal_emit_by_name(peer, "add-ice-candidate", candidate.at("sdpMLineIndex").get<guint>(),
                            candidate.at("candidate").get<std::string>().c_str());
  }
};
}
int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  g_networking_init();
  try {
    for (const auto* name : {"videotestsrc", "x264enc", "h264parse", "rtph264pay", "webrtcbin", "avdec_h264"}) {
      auto* factory = gst_element_factory_find(name);
      if (!factory) { std::cerr << "missing synthetic test plugin " << name << '\n'; return 77; }
      gst_object_unref(factory);
    }
    Sender sender;
    Options options;
    options.camera_ids = {"rear", "front"};
    options.fixture_loopback = true;
#if GST_CHECK_VERSION(1, 22, 0)
    options.control_scope = mine_teleop::detail::NativeMediaScope{"fixture-session", 1, "test-attempt"};
#endif
    Receiver receiver(options); receiver.start();
    const Json offer{{"media_negotiation_id", "test-attempt"}, {"sdp", sender.offer()},
                     {"media_tracks", Json::array({{{"camera_id", "front"}}, {{"camera_id", "rear"}}})}};
    sender.answer(receiver.answer(offer));
    const auto deadline = Clock::now() + 15s;
    bool passed = false;
    bool sent = false, delivered = false, responded = false;
    while (Clock::now() < deadline) {
      sender.exchange(receiver); receiver.check();
      std::set<std::string> identities;
      const auto tracks = receiver.counters();
      for (const auto& track : tracks)
        if (track.value("decoded", 0) >= 5) identities.insert(track.value("camera_id", ""));
#if GST_CHECK_VERSION(1, 22, 0)
      auto* channel = receiver.control_channel();
      if (sender.channel_open && !sent) { g_signal_emit_by_name(sender.channel, "send-string", "request"); sent = true; }
      if (auto value = channel->pop()) { require(*value == "request", "incorrect control message"); delivered = true; }
      if (delivered && channel->ready() && !responded) responded = channel->send("response");
      const bool control_passed = sender.channel_received && delivered;
#else
      const bool control_passed = true;
      (void)sent; (void)delivered; (void)responded;
#endif
      if (identities == std::set<std::string>{"front", "rear"} && control_passed) { passed = true; break; }
      std::this_thread::sleep_for(10ms);
    }
    const auto pair = receiver.selected_pair();
    receiver.stop();
    require(passed, "two decoded tracks did not retain their negotiated camera identities");
    require(pair.is_object() && pair.value("local_address", "") == "127.0.0.1" &&
            pair.value("remote_address", "") == "127.0.0.1" && pair.value("local_transport", "") == "udp" &&
            pair.value("remote_transport", "") == "udp", "fixture did not remain on loopback UDP");
    std::cout << "PASS actual WebRTC pad-to-camera MID mapping with two synthetic streams";
#if GST_CHECK_VERSION(1, 22, 0)
    std::cout << " and scoped DataChannel";
#endif
    std::cout << '\n';
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
