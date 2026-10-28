// Isolated loopback fault injection. No camera, signaling service or vehicle control.
#include "mine_teleop/detail/gstreamer_ice_servers.hpp"
#include "mine_teleop/detail/ice_route_policy.hpp"
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <agent.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
using mine_teleop::detail::configure_webrtc_ice_servers;
using mine_teleop::detail::IceRoutePolicy;
using Json = nlohmann::json;
namespace {
void check(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
std::int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
struct Peer {
  GstElement* pipeline = gst_pipeline_new(nullptr);
  GstElement* rtc = gst_element_factory_make("webrtcbin", nullptr);
  NiceAgent* agent = nullptr;
  GstWebRTCDataChannel* channel = nullptr;
  std::atomic<bool> open{false}, received{false}, selected{false}, relay{false};
  std::mutex mutex;
  std::vector<std::pair<guint,std::string>> candidates;
  Peer(const std::string& ip, const Json& servers, bool turn) {
    check(rtc != nullptr, "webrtcbin unavailable");
    gst_bin_add(GST_BIN(pipeline), rtc);
    const auto applied = configure_webrtc_ice_servers(rtc, servers, turn);
    check(applied.at("turn_applied") == (turn && !servers.empty()), "TURN application mismatch");
    GObject* ice = nullptr;
    g_object_get(rtc, "ice-agent", &ice, nullptr);
    check(ice != nullptr, "ICE object unavailable");
    g_object_get(ice, "agent", &agent, nullptr); g_object_unref(ice);
    check(agent != nullptr, "libnice unavailable");
    NiceAddress address; nice_address_init(&address);
    check(nice_address_set_from_string(&address, ip.c_str()) && nice_agent_add_local_address(agent, &address), "loopback bind failed");
    g_object_set(agent, "ice-tcp", FALSE, nullptr);
    g_signal_connect(agent, "new-selected-pair-full", G_CALLBACK(+[](NiceAgent*,guint,guint,NiceCandidate* local,NiceCandidate* remote,gpointer data) {
      auto& self = *static_cast<Peer*>(data);
      self.relay = local->type == NICE_CANDIDATE_TYPE_RELAYED || remote->type == NICE_CANDIDATE_TYPE_RELAYED;
      self.selected = true;
    }), this);
    g_signal_connect(rtc, "on-ice-candidate", G_CALLBACK(+[](GstElement*,guint index,gchar* text,gpointer data) {
      auto& self = *static_cast<Peer*>(data); std::lock_guard lock(self.mutex);
      self.candidates.emplace_back(index, text);
    }), this);
    g_signal_connect(rtc, "on-data-channel", G_CALLBACK(+[](GstElement*,GstWebRTCDataChannel* channel,gpointer data) {
      g_signal_connect(channel, "on-message-string", G_CALLBACK(+[](GstWebRTCDataChannel*,gchar* value,gpointer context) {
        if (std::string(value) == "fallback-fixture") static_cast<Peer*>(context)->received = true;
      }), data);
    }), this);
    check(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "READY failed");
  }
  ~Peer() {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (channel) g_object_unref(channel);
    if (agent) g_object_unref(agent);
    gst_object_unref(pipeline);
  }
  void start(bool offerer) {
    if (offerer) {
      g_signal_emit_by_name(rtc, "create-data-channel", "test", nullptr, &channel);
      check(channel != nullptr, "DataChannel unavailable");
      g_signal_connect(channel, "on-open", G_CALLBACK(+[](GstWebRTCDataChannel*,gpointer data) { static_cast<Peer*>(data)->open = true; }), this);
    }
    check(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "PLAYING failed");
  }
  static void apply(GstElement* target, const char* method, GstWebRTCSessionDescription* description) {
    auto* promise = gst_promise_new();
    g_signal_emit_by_name(target, method, description, promise);
    const auto status = gst_promise_wait(promise);
    const auto* reply = gst_promise_get_reply(promise);
    const bool valid = status == GST_PROMISE_RESULT_REPLIED && (!reply || !gst_structure_has_field(reply,"error"));
    gst_promise_unref(promise); check(valid,"SDP application failed");
  }
  void describe(Peer& remote, bool offer) {
    auto* promise = gst_promise_new();
    g_signal_emit_by_name(rtc, offer ? "create-offer" : "create-answer", nullptr, promise);
    check(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED, "SDP creation failed");
    GstWebRTCSessionDescription* description = nullptr;
    const auto* reply = gst_promise_get_reply(promise);
    check(reply && gst_structure_get(reply, offer ? "offer" : "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &description, nullptr), "SDP missing");
    gst_promise_unref(promise);
    apply(rtc,"set-local-description",description);
    apply(remote.rtc,"set-remote-description",description);
    gst_webrtc_session_description_free(description);
  }
  void deliver(Peer& remote, bool relay_only) {
    std::vector<std::pair<guint,std::string>> pending;
    { std::lock_guard lock(mutex); pending.swap(candidates); }
    for (const auto& [index, text] : pending)
      if (!relay_only || text.find(" typ relay") != std::string::npos)
        g_signal_emit_by_name(remote.rtc,"add-ice-candidate",index,text.c_str());
  }
};
void verify_server_configuration() {
  auto* peer = gst_element_factory_make("webrtcbin", nullptr);
  check(peer != nullptr,"webrtcbin unavailable");
  const Json servers=Json::array({{{"urls",{"stun:[::1]:34789","turn:127.0.0.1:34789?transport=udp"}},
                                  {"username","u:name"},{"credential","p@ss"}}});
  const auto direct=configure_webrtc_ice_servers(peer,servers,false);
  gchar* stun=nullptr; gchar* turn=nullptr;
  g_object_get(peer,"stun-server",&stun,"turn-server",&turn,nullptr);
  const bool direct_ok=direct.at("stun_applied")==true && direct.at("turn_applied")==false &&
      stun && std::string(stun)=="stun://[::1]:34789" && !turn;
  g_free(stun);g_free(turn);
  const auto fallback=configure_webrtc_ice_servers(peer,servers,true);
  g_object_get(peer,"turn-server",&turn,nullptr);
  // GStreamer 1.20 serializes percent signs again in this getter. Actual
  // escaped REST credentials are verified by TURN allocation and delivery below.
  const bool fallback_ok=fallback.at("turn_applied")==true && turn;
  g_free(turn);gst_object_unref(peer);
  check(direct_ok && fallback_ok,"IPv6 STUN / TURN filtering or credential escaping failed");
}
void negotiate(Peer& a, Peer& b) { a.start(true); b.start(false); a.describe(b,true); b.describe(a,false); }
void verify_delivery(Peer& a, Peer& b, bool relay_only) {
  const auto end = now_ms()+15000;
  bool sent = false;
  while (now_ms()<end && !b.received) {
    a.deliver(b,relay_only); b.deliver(a,relay_only);
    if (a.open && !sent) { g_signal_emit_by_name(a.channel,"send-string","fallback-fixture"); sent = true; }
    std::this_thread::sleep_for(10ms);
  }
  check(b.received && a.selected && b.selected,"no DataChannel delivery on selected pair");
  check(!relay_only || (a.relay && b.relay),"fault injection did not select TURN");
}
}
int main(int argc, char** argv) {
  gst_init(&argc,&argv);
  try {
    check(argc==4,"usage: ice-fallback-tests TURN_PORT FIXTURE_USER FIXTURE_CREDENTIAL");
    verify_server_configuration();
    for (const auto* ip : {"127.0.0.1","::1"}) {
      Peer a(ip,Json::array(),false), b(ip,Json::array(),false);
      negotiate(a,b); verify_delivery(a,b,false);
      check(!a.relay && !b.relay,"direct loopback unexpectedly relayed");
      std::cout<<"PASS direct UDP DataChannel "<<ip<<'\n';
    }
    const Json servers=Json::array({{{"urls","turn:127.0.0.1:"+std::string(argv[1])+"?transport=udp"},{"username",argv[2]},{"credential",argv[3]}}});
    IceRoutePolicy policy(300,true,false);
    {
      Peer a("127.0.0.1",servers,false), b("127.0.0.1",servers,false);
      policy.begin_attempt(); negotiate(a,b);
      // Deliberately withhold all trickle candidates: no NAT inference.
      const auto end=now_ms()+3000;
      bool retry=false;
      while (now_ms()<end && !(retry=policy.allow_relay_retry(now_ms(),true,a.open,false))) std::this_thread::sleep_for(10ms);
      check(retry && !a.open && !b.received,"direct failure did not trigger bounded retry");
    } // Old peers are destroyed before TURN is admitted.
    check(policy.mode()=="all" && policy.fallbacks()==1,"fallback route mode incorrect");
    {
      Peer a("127.0.0.1",servers,true), b("127.0.0.1",servers,true);
      policy.begin_attempt(); negotiate(a,b); verify_delivery(a,b,true);
      check(!policy.allow_relay_retry(now_ms()+100000,true,false,true),"fallback loop is unbounded");
    }
    std::cout<<"PASS blocked direct -> fresh peers -> selected TURN/UDP -> delivered DataChannel\n";
    return 0;
  } catch(const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
