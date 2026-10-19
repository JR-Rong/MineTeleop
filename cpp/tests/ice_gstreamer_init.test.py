#!/usr/bin/env python3
"""Actual frozen vehicle callbacks with real Gst, synthetic RTP and fake signaling.

No HTTP, vehicle/VCU, cameras, configured STUN/TURN, remote ICE candidates or
external signaling endpoint. Real Gst may bind local UDP and gather candidates.
Run only in an isolated network-none/loopback test environment; the execution
wrapper or CI must record that isolation independently.
This is initialization and callback ownership coverage, not RTP reachability,
production TLS/WSS, or full VehicleMediaRuntime stop/recording cleanup.
"""
import argparse
import datetime
import hashlib
import json
import shlex
from pathlib import Path
import subprocess
import sys

CPP_ROOT = Path(__file__).resolve().parents[1]


def extract(source, begin, end):
    first = source.index(begin)
    return source[first:source.index(end, first)]


PRELUDE = r'''
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>
using Json=nlohmann::json;
namespace detail { constexpr std::string_view kControlTransportCapability="native-control-transport-v1"; constexpr std::string_view kEmergencyStopCapability="native-stop-only-v1"; constexpr std::string_view kFeedbackTransportCapability="native-feedback-transport-v1"; }
using namespace std::chrono_literals;
enum class VideoCodec {H264,H265};
const char* to_string(VideoCodec value){return value==VideoCodec::H264?"h264":"h265";}
std::string random_token(int){static std::atomic<unsigned> n{0};return "isolated-gst-epoch-"+std::to_string(++n);}
struct FakeSignaling {
  std::mutex mutex;
  std::vector<std::pair<std::string,Json>> sent;
  std::int64_t now_ms() const{return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
  void send(const std::string& type,const Json& payload){std::lock_guard lock(mutex);sent.emplace_back(type,payload);}
  const std::string& transport_mac_key() const {static const std::string key;return key;}
};
struct CallbackGate {
  std::mutex mutex;std::condition_variable cv;void* target=nullptr;
  bool enabled=false,entered=false,released=false;
  bool timed_out=false,body_started=false,body_started_after_retirement=false;
  bool body_completed=false,callback_failed=false;
} gate;
struct Impl {
'''

MEMBERS = r'''
  struct {struct {int experimental_direct_authority_max_ms=0;} runtime;} config;
  struct Lane {struct {std::string id;} camera;struct {int width=16,height=16,fps=20,bitrate_kbps=1;} profile;};
  std::vector<std::unique_ptr<Lane>> lanes;
  struct {VideoCodec codec=VideoCodec::H264,backend=VideoCodec::H264;} active_candidate;
  std::atomic<std::uint64_t> local_ice_candidate_count{0};
  std::atomic<bool> stop_requested{false};
  unsigned feedback_wakeups=0;
  void wake_native_control_feedback() noexcept {++feedback_wakeups;}
  std::shared_ptr<MediaSignalState> media_signal_state=std::make_shared<MediaSignalState>();
  FakeSignaling signaling;
  std::mutex diagnostics_mutex;std::vector<std::string> errors,diagnostics;
  void emit_diagnostic(std::string_view event,std::string_view,std::string_view,std::string_view,std::string_view,bool,Json) {std::lock_guard lock(diagnostics_mutex);diagnostics.emplace_back(event);}
  void set_pipeline_error(std::string_view error,std::string_view,std::string_view,std::string_view,bool){std::lock_guard lock(diagnostics_mutex);errors.emplace_back(error);}
  static void on_offer_created(GstPromise* promise,gpointer data) noexcept {
    bool controlled=false;
    try {
      const auto* context=static_cast<MediaOfferContext*>(data);
      const auto state=context->state;
      {
        std::unique_lock lock(gate.mutex);
        controlled=gate.enabled&&gate.target==state.get();
        if(controlled) {
          gate.entered=true;gate.cv.notify_all();
          gate.timed_out=!gate.cv.wait_for(lock,5s,[]{return gate.released;});
        }
      }
      if(controlled) {
        bool retired=false;
        {std::lock_guard lock(state->mutex);retired=state->owner==nullptr&&state->peer==nullptr;}
        std::lock_guard lock(gate.mutex);
        gate.body_started=true;gate.body_started_after_retirement=retired;
      }
      // The actual body may unref the promise and destroy context. Never
      // access context/data/promise after this call or unref them in catch.
      on_offer_created_actual(promise,data);
      if(controlled) {std::lock_guard lock(gate.mutex);gate.body_completed=true;gate.cv.notify_all();}
    } catch(...) {
      // A C callback never propagates a C++ exception. Failure is latched and
      // asserted by the main test; completion means a normal actual return.
      std::lock_guard lock(gate.mutex);gate.callback_failed=true;gate.cv.notify_all();
    }
  }
};
void expect(bool value,const char* message){if(!value)throw std::runtime_error(message);}
bool wait_for(const std::function<bool()>& predicate,std::chrono::milliseconds timeout=5s) {
  const auto until=std::chrono::steady_clock::now()+timeout;
  do {while(g_main_context_iteration(nullptr,FALSE)){}if(predicate())return true;std::this_thread::sleep_for(5ms);}while(std::chrono::steady_clock::now()<until);
  return predicate();
}
struct Fixture {
  Impl impl;GstElement* pipeline=nullptr;GstElement* peer=nullptr;
  GstWebRTCDataChannel* channel=nullptr;std::vector<GstElement*> sources;
  GWeakRef peer_weak{},pipeline_weak{};bool weak_initialized=false,stopped=false;
  std::shared_ptr<std::atomic<unsigned>> signals=std::make_shared<std::atomic<unsigned>>(0);
  Fixture() {
    try {
    pipeline=gst_pipeline_new(nullptr);peer=gst_element_factory_make("webrtcbin",nullptr);
    expect(pipeline&&peer,"webrtcbin/pipeline construction failed");
    g_object_set(peer,"bundle-policy",GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,"latency",0,nullptr);
    expect(gst_bin_add(GST_BIN(pipeline),peer),"cannot add webrtcbin");gst_object_ref(peer);
    g_weak_ref_init(&peer_weak,G_OBJECT(peer));g_weak_ref_init(&pipeline_weak,G_OBJECT(pipeline));weak_initialized=true;
    for(unsigned index=0;index<4;index++) {
      auto lane=std::make_unique<Impl::Lane>();lane->camera.id="synthetic-"+std::to_string(index);impl.lanes.push_back(std::move(lane));
      auto* source=gst_element_factory_make("appsrc",nullptr);auto* queue=gst_element_factory_make("queue",nullptr);
      expect(source&&queue,"synthetic appsrc/queue construction failed");
      auto* caps=gst_caps_new_simple("application/x-rtp","media",G_TYPE_STRING,"video",
          "encoding-name",G_TYPE_STRING,"H264","clock-rate",G_TYPE_INT,90000,
          "payload",G_TYPE_INT,96,"ssrc",G_TYPE_UINT,1000U+index,
          "packetization-mode",G_TYPE_STRING,"1",nullptr);
      gst_app_src_set_caps(GST_APP_SRC(source),caps);gst_caps_unref(caps);
      g_object_set(source,"is-live",TRUE,"format",GST_FORMAT_TIME,"block",FALSE,nullptr);
      expect(gst_bin_add(GST_BIN(pipeline),source)&&gst_bin_add(GST_BIN(pipeline),queue),"cannot add synthetic lane");
      expect(gst_element_link(source,queue),"cannot link appsrc to queue");
      auto* src=gst_element_get_static_pad(queue,"src");auto* sink=gst_element_request_pad_simple(peer,"sink_%u");
      expect(src&&sink&&gst_pad_link(src,sink)==GST_PAD_LINK_OK,"cannot link fixed RTP lane to webrtcbin");
      gst_object_unref(src);gst_object_unref(sink);sources.push_back(source);
    }
    impl.media_signal_state->begin_attempt(&impl,peer,"isolated-gst-session");
    const auto connect=[&](const char* signal,GCallback callback) {
      auto* state=new std::shared_ptr<Impl::MediaSignalState>(impl.media_signal_state);
      g_signal_connect_data(peer,signal,callback,state,
          +[](gpointer data,GClosure*){delete static_cast<std::shared_ptr<Impl::MediaSignalState>*>(data);},static_cast<GConnectFlags>(0));
    };
    connect("on-negotiation-needed",G_CALLBACK(Impl::on_negotiation_needed));
    connect("on-ice-candidate",G_CALLBACK(Impl::on_ice_candidate));
    auto* observer=new std::shared_ptr<std::atomic<unsigned>>(signals);
    g_signal_connect_data(peer,"on-negotiation-needed",G_CALLBACK(+[](GstElement*,gpointer data){++**static_cast<std::shared_ptr<std::atomic<unsigned>>*>(data);}),observer,
        +[](gpointer data,GClosure*){delete static_cast<std::shared_ptr<std::atomic<unsigned>>*>(data);},static_cast<GConnectFlags>(0));
    } catch(...) {try{stop();}catch(...){ }if(weak_initialized){g_weak_ref_clear(&peer_weak);g_weak_ref_clear(&pipeline_weak);weak_initialized=false;}throw;}
  }
  void confirm_state(GstState target,const char* message) {
    GstState current=GST_STATE_VOID_PENDING,pending=GST_STATE_VOID_PENDING;
    const auto result=gst_element_get_state(pipeline,&current,&pending,3*GST_SECOND);
    expect(result!=GST_STATE_CHANGE_FAILURE&&result!=GST_STATE_CHANGE_ASYNC&&current==target&&pending==GST_STATE_VOID_PENDING,message);
    current=GST_STATE_VOID_PENDING;pending=GST_STATE_VOID_PENDING;
    const auto peer_result=gst_element_get_state(peer,&current,&pending,0);
    expect(peer_result!=GST_STATE_CHANGE_FAILURE&&peer_result!=GST_STATE_CHANGE_ASYNC&&current==target&&pending==GST_STATE_VOID_PENDING,"webrtcbin did not reach pipeline target state");
  }
  void start() {
    expect(gst_element_set_state(pipeline,GST_STATE_READY)!=GST_STATE_CHANGE_FAILURE,"pipeline READY failed");
    confirm_state(GST_STATE_READY,"pipeline did not reach READY within3s");
    // Exactly the production READY-before-control-channel options. No channel
    // callbacks start VCU/native services, and there is no remote peer.
    auto* options=gst_structure_new("mine-teleop-control-data-channel","ordered",G_TYPE_BOOLEAN,FALSE,
        "max-retransmits",G_TYPE_INT,0,"protocol",G_TYPE_STRING,"mine-teleop-control-v1",
        "negotiated",G_TYPE_BOOLEAN,FALSE,nullptr);
    g_signal_emit_by_name(peer,"create-data-channel","control",options,&channel);gst_structure_free(options);
    expect(channel!=nullptr,"control DataChannel creation in READY failed");
    expect(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"pipeline PLAYING failed");
    for(unsigned tick=0;tick<4;tick++) {
      for(unsigned index=0;index<sources.size();index++) {
        const guint8 packet[16]={0x80,0x60,0,static_cast<guint8>(tick),0,0,0,0,0,0,3,static_cast<guint8>(0xe8+index),0x65,0x88,0x84,0x21};
        auto* buffer=gst_buffer_new_allocate(nullptr,sizeof(packet),nullptr);gst_buffer_fill(buffer,0,packet,sizeof(packet));
        GST_BUFFER_PTS(buffer)=tick*GST_SECOND/20;GST_BUFFER_DURATION(buffer)=GST_SECOND/20;
        expect(gst_app_src_push_buffer(GST_APP_SRC(sources[index]),buffer)==GST_FLOW_OK,"synthetic RTP admission failed");
      }
    }
    confirm_state(GST_STATE_PLAYING,"pipeline did not reach PLAYING within3s");
  }
  Json offer() {
    Json result;
    const bool available=wait_for([&]{
      {std::lock_guard lock(impl.diagnostics_mutex);if(!impl.errors.empty())throw std::runtime_error(impl.errors.front());}
      std::lock_guard lock(impl.media_signal_state->mutex);
      for(const auto& value:impl.media_signal_state->pending)if(value.first=="webrtc_offer"){result=value.second;return true;}
      return false;
    });
    expect(available,"initial actual Gst offer did not complete in5s");return result;
  }
  void stop() {
    if(stopped)return;
    stopped=true;
    impl.stop_scope();
    expect(impl.stop_requested.load()&&impl.feedback_wakeups==1,"stop scope did not notify the control owner");
    {std::lock_guard lock(gate.mutex);gate.released=true;gate.cv.notify_all();}
    if(channel){gst_webrtc_data_channel_close(channel);g_object_unref(channel);channel=nullptr;}
    bool reached_null=true;
    if(pipeline) {
      const auto requested=gst_element_set_state(pipeline,GST_STATE_NULL);
      GstState current=GST_STATE_VOID_PENDING,pending=GST_STATE_VOID_PENDING;
      const auto result=gst_element_get_state(pipeline,&current,&pending,3*GST_SECOND);
      reached_null=requested!=GST_STATE_CHANGE_FAILURE&&result!=GST_STATE_CHANGE_FAILURE&&result!=GST_STATE_CHANGE_ASYNC&&current==GST_STATE_NULL&&pending==GST_STATE_VOID_PENDING;
      if(peer) {
        current=GST_STATE_VOID_PENDING;pending=GST_STATE_VOID_PENDING;
        const auto peer_result=gst_element_get_state(peer,&current,&pending,0);
        reached_null=reached_null&&peer_result!=GST_STATE_CHANGE_FAILURE&&peer_result!=GST_STATE_CHANGE_ASYNC&&current==GST_STATE_NULL&&pending==GST_STATE_VOID_PENDING;
      }
      gst_object_unref(pipeline);pipeline=nullptr;
    }
    if(peer){gst_object_unref(peer);peer=nullptr;}
    expect(reached_null,"pipeline/webrtcbin did not reach NULL within3s");
  }
  bool released() {
    return wait_for([&]{auto* p=g_weak_ref_get(&peer_weak);auto* q=g_weak_ref_get(&pipeline_weak);const bool gone=!p&&!q;if(p)g_object_unref(p);if(q)g_object_unref(q);return gone;});
  }
  ~Fixture(){try{stop();}catch(...){ }if(weak_initialized){g_weak_ref_clear(&peer_weak);g_weak_ref_clear(&pipeline_weak);}}
};
void case_initial_and_repeat() {
  Fixture fixture;fixture.start();const auto offer=fixture.offer();
  auto* sdp=static_cast<GstSDPMessage*>(nullptr);expect(gst_sdp_message_new(&sdp)==GST_SDP_OK,"SDP allocation failed");
  const auto text=offer.at("sdp").get<std::string>();
  expect(gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(text.data()),text.size(),sdp)==GST_SDP_OK,"initial actual SDP parse failed");
  unsigned videos=0,applications=0;
  for(guint index=0;index<gst_sdp_message_medias_len(sdp);index++){
    const auto* media=gst_sdp_message_get_media(sdp,index);const std::string type=gst_sdp_media_get_media(media);
    videos+=type=="video";applications+=type=="application";
  }
  gst_sdp_message_free(sdp);
  std::cout<<"OBS initial_video_sections="<<videos<<" application_sections="<<applications<<" negotiation_signals="<<fixture.signals->load()<<'\n';
  expect(videos==4&&applications==1,"first actual offer omitted fixed lanes or control DataChannel");
  expect(offer.at("media_tracks").size()==4,"producer initial tracks metadata incomplete");
  const auto epoch=offer.at("media_negotiation_id");
  {std::lock_guard lock(gate.mutex);expect(!gate.callback_failed,"actual offer callback threw across its wrapper");}
  std::cout<<"PASS ICE-GST-INIT-001\n";
  g_signal_emit_by_name(fixture.peer,"on-negotiation-needed");
  wait_for([]{return false;},100ms);
  {std::lock_guard lock(fixture.impl.media_signal_state->mutex);expect(fixture.impl.media_signal_state->offer_count==1&&fixture.impl.media_signal_state->negotiation_id==epoch.get<std::string>(),"repeated real negotiation signal changed immutable epoch");}
  fixture.stop();expect(fixture.released(),"normal real Gst peer/pipeline refs did not release in5s");
  std::cout<<"PASS ICE-GST-INIT-002\n";
}
void case_pending_callback_teardown() {
  Fixture fixture;
  {std::lock_guard lock(gate.mutex);gate.enabled=true;gate.target=fixture.impl.media_signal_state.get();gate.entered=false;gate.released=false;gate.timed_out=false;gate.body_started=false;gate.body_started_after_retirement=false;gate.body_completed=false;gate.callback_failed=false;}
  fixture.start();
  expect(wait_for([]{std::lock_guard lock(gate.mutex);return gate.entered;},4s),"real offer callback did not enter controlled pending barrier");
  // Release the callback before NULL joins Gst's task thread. The actual body
  // runs after its production scope invalidation, without blocking NULL.
  fixture.stop();expect(fixture.released(),"pending callback captured Gst references did not release in5s");
  expect(wait_for([]{std::lock_guard lock(gate.mutex);return gate.body_completed||gate.callback_failed;},1s),"gated actual offer body did not return after retirement");
  {
    std::lock_guard lock(gate.mutex);
    std::cout<<"OBS gate_entered="<<gate.entered<<" timed_out="<<gate.timed_out<<" body_started="<<gate.body_started<<" after_retirement="<<gate.body_started_after_retirement<<" completed="<<gate.body_completed<<" callback_failed="<<gate.callback_failed<<'\n';
    expect(gate.entered&&!gate.timed_out&&gate.body_started&&gate.body_started_after_retirement&&gate.body_completed&&!gate.callback_failed,"gate did not prove actual body started and completed after retirement without timeout");
  }
  {std::lock_guard lock(fixture.impl.media_signal_state->mutex);expect(fixture.impl.media_signal_state->pending.empty(),"retired real callback left outgoing signaling");}
  {std::lock_guard lock(fixture.impl.diagnostics_mutex);expect(fixture.impl.errors.empty(),"retired real callback reported an owner error");}
  {std::lock_guard lock(gate.mutex);gate.enabled=false;gate.target=nullptr;}
  std::cout<<"PASS ICE-GST-INIT-003\n";
}
void apply_description(GstElement* peer,const char* action,GstWebRTCSessionDescription* description) {
  auto* promise=gst_promise_new();g_signal_emit_by_name(peer,action,description,promise);
  expect(gst_promise_wait(promise)==GST_PROMISE_RESULT_REPLIED,"fixture description did not reply");
  const auto* reply=gst_promise_get_reply(promise);
  const bool rejected=reply&&gst_structure_has_field(reply,"error");
  gst_promise_unref(promise);expect(!rejected,"fixture description rejected");
}
void case_answer_completion(bool rejected) {
  Fixture fixture;fixture.start();const auto offer_json=fixture.offer();
  GstSDPMessage* sdp=nullptr;gst_sdp_message_new(&sdp);
  const std::string text=rejected?"v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n":offer_json.at("sdp").get<std::string>();
  expect(gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(text.data()),text.size(),sdp)==GST_SDP_OK,"fixture SDP syntax invalid");
  auto* answer=gst_webrtc_session_description_new(rejected?GST_WEBRTC_SDP_TYPE_ANSWER:GST_WEBRTC_SDP_TYPE_OFFER,sdp);
  if(!rejected) {
    auto* receiver=gst_element_factory_make("webrtcbin",nullptr);expect(receiver,"receiver unavailable");
    expect(gst_element_set_state(receiver,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"receiver did not start");
    apply_description(receiver,"set-remote-description",answer);gst_webrtc_session_description_free(answer);answer=nullptr;
    auto* promise=gst_promise_new();g_signal_emit_by_name(receiver,"create-answer",nullptr,promise);
    expect(gst_promise_wait(promise)==GST_PROMISE_RESULT_REPLIED,"receiver answer did not reply");
    const auto* reply=gst_promise_get_reply(promise);
    expect(reply&&gst_structure_get(reply,"answer",GST_TYPE_WEBRTC_SESSION_DESCRIPTION,&answer,nullptr)&&answer,"receiver answer missing");
    gst_promise_unref(promise);gst_element_set_state(receiver,GST_STATE_NULL);gst_object_unref(receiver);
  }
  const auto state=fixture.impl.media_signal_state;
  Impl::MediaOfferContext* context=nullptr;
  {std::lock_guard lock(state->mutex);state->answer_apply_pending=true;state->answer_applied=false;
    context=new Impl::MediaOfferContext{state,GST_ELEMENT(gst_object_ref(fixture.peer)),state->negotiation_id};}
  auto* promise=gst_promise_new_with_change_func(Impl::on_answer_applied,context,
      +[](gpointer value){delete static_cast<Impl::MediaOfferContext*>(value);});
  g_signal_emit_by_name(fixture.peer,"set-remote-description",answer,promise);gst_webrtc_session_description_free(answer);
  expect(wait_for([&]{std::lock_guard lock(state->mutex);return !state->answer_apply_pending;}),"actual answer callback did not finish");
  {std::lock_guard lock(state->mutex);expect(state->answer_applied!=rejected,"actual answer outcome was misreported");}
  {std::lock_guard lock(fixture.impl.diagnostics_mutex);
    expect(fixture.impl.errors.size()==(rejected?1U:0U),"actual answer rejection was swallowed");
    expect(std::count(fixture.impl.diagnostics.begin(),fixture.impl.diagnostics.end(),"vehicle_webrtc_answer_applied")== (rejected?0:1),"actual answer emitted a false success milestone");}
  fixture.stop();expect(fixture.released(),"answer callback retained its peer after teardown");
  std::cout<<(rejected?"PASS ICE-GST-ANSWER-002":"PASS ICE-GST-ANSWER-001")<<'\n';
}
int main(){
  gst_init(nullptr,nullptr);bool missing=false;
  for(const char* name:{"webrtcbin","nicesrc","nicesink","appsrc","queue"}) {
    auto* factory=gst_element_factory_find(name);if(!factory){missing=true;std::cerr<<"BLOCKED missing Gst factory "<<name<<'\n';}else gst_object_unref(factory);
  }
  if(missing){gst_deinit();return 2;}
  int result=0;try{case_initial_and_repeat();case_pending_callback_teardown();case_answer_completion(false);case_answer_completion(true);}
  catch(const std::exception& error){std::cerr<<"FAIL actual Gst initialization: "<<error.what()<<'\n';result=1;}
  gst_deinit();return result;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--source", type=Path, default=CPP_ROOT / "src/webrtc_media.cpp")
    parser.add_argument("--compiler", default="/usr/bin/c++")
    parser.add_argument("--json-include")
    parser.add_argument("--pkg-config", default="pkg-config")
    parser.add_argument("--compile-only", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text()
    methods = extract(source, "  struct MediaSignalState {", "  static constexpr std::size_t kControlTrace")
    methods += extract(source, "  static void on_ice_candidate(", "  static void on_control_channel_open(")
    offer_methods = extract(source, "  static void on_offer_created(", "  void emit_diagnostic(")
    # Only the binding name changes: the real promise/SDP ownership body stays
    # byte-for-byte. The wrapper gates delivery to test retirement safely.
    methods += offer_methods.replace("static void on_offer_created(", "static void on_offer_created_actual(", 1)
    methods += extract(source, "  void flush_outgoing_signals()", "  void apply_remote_ice(")
    methods += extract(source, "  void stop_pipeline() {", "    GstWebRTCDataChannel* channel_to_close")\
        .replace("void stop_pipeline() {", "void stop_scope() {", 1) + "  }\n"
    unit = args.output / "actual-gst-initialization.cpp"
    unit.write_text(PRELUDE + methods + MEMBERS)
    binary = args.output / "actual-gst-initialization"
    dependencies = ["gstreamer-app-1.0", "gstreamer-webrtc-1.0", "gstreamer-sdp-1.0"]
    flags = subprocess.run([args.pkg_config, "--cflags", "--libs", *dependencies], capture_output=True, text=True)
    (args.output / "pkg-config.log").write_text(flags.stdout + flags.stderr)
    start = datetime.datetime.now(datetime.timezone.utc).isoformat()
    command = [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    includes = args.json_include.split(";") if args.json_include is not None else [str(p) for p in (CPP_ROOT.parent / "offline-deps").glob("json-*/include")]
    if args.json_include is None and len(includes) > 1:
        raise RuntimeError("ambiguous JSON includes; pass --json-include")
    command += ["-I" + value for value in includes if value]
    command += [str(unit), *shlex.split(flags.stdout), "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True) if flags.returncode == 0 else None
    (args.output / "build.log").write_text(built.stdout + built.stderr if built else "pkg-config failed; compilation not run\n")
    if flags.returncode:
        print((flags.stdout + flags.stderr)[-8000:], file=sys.stderr)
    elif built.returncode:
        print((built.stdout + built.stderr)[-8000:], file=sys.stderr)
    timed_out = False
    result = None
    if built and built.returncode == 0 and not args.compile_only:
        try:
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=35)
        except subprocess.TimeoutExpired as error:
            timed_out = True
            stdout = error.stdout.decode(errors="replace") if isinstance(error.stdout, bytes) else error.stdout or ""
            stderr = error.stderr.decode(errors="replace") if isinstance(error.stderr, bytes) else error.stderr or ""
            (args.output / "result.log").write_text(stdout + stderr + "\nTIMEOUT after35s; runtime test failed\n")
    if result:
        (args.output / "result.log").write_text(result.stdout + result.stderr)
        if result.returncode:
            print((result.stdout + result.stderr)[-8000:], file=sys.stderr)
    end = datetime.datetime.now(datetime.timezone.utc).isoformat()
    status = "compile_failed" if not built or built.returncode else "compiled_not_run" if args.compile_only else "timed_out" if timed_out else "passed" if result.returncode == 0 else "blocked_missing_plugins" if result.returncode == 2 else "failed"
    manifest = {"scope":__doc__, "status":status, "source_sha256":hashlib.sha256(source.encode()).hexdigest(),
        "test_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "generated_cpp_sha256":hashlib.sha256(unit.read_bytes()).hexdigest(),
        "start_utc":start,"end_utc":end,"compile_argv":command,"pkg_config_exit":flags.returncode,
        "compile_exit":built.returncode if built else None,"run_exit":result.returncode if result else None,"timed_out":timed_out,
        "binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest() if built and built.returncode == 0 else None,
        "limits":"No external ICE/RTP reachability, TLS/WSS, VCU, camera, or full production stop/recording cleanup proof; compile-only is not runtime PASS."}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(json.dumps(manifest, indent=2))
    return flags.returncode or (built.returncode if built else 1) or (result.returncode if result else 1 if timed_out else 0)


if __name__ == "__main__":
    raise SystemExit(main())
