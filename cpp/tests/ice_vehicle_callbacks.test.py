#!/usr/bin/env python3
"""Compile actual vehicle signaling methods with fake Gst primitives.

This is a deterministic ownership/order mock, not GStreamer ABI, UDP/NAT,
or production webrtc_media.cpp compilation. No socket or service is used.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src/webrtc_media.cpp"


def section(source, begin, end):
    first = source.index(begin)
    return source[first:source.index(end, first)]


PRELUDE = r'''
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <utility>
#include <nlohmann/json.hpp>
using Json=nlohmann::json;
namespace detail { constexpr std::string_view kControlTransportCapability = "native-control-transport-v1"; constexpr std::string_view kEmergencyStopCapability = "native-stop-only-v1"; constexpr std::string_view kFeedbackTransportCapability = "native-feedback-transport-v1"; }
using guint=unsigned;using guint8=unsigned char;using gchar=char;using gpointer=void*;
struct GstSDPMessage {std::string text;};
struct GstWebRTCSessionDescription {GstSDPMessage* sdp;};
struct GstStructure {std::string sdp;};
struct GstPromise {void(*callback)(GstPromise*,void*){};void* data{};void(*destroy)(void*){};GstStructure reply;bool replied=true;};
struct GstElement {int refs=1;std::vector<GstPromise*> offers;std::vector<std::string> local,remote,added;};
#define GST_ELEMENT(x) (x)
constexpr int GST_PROMISE_RESULT_REPLIED=1,GST_TYPE_WEBRTC_SESSION_DESCRIPTION=0,GST_SDP_OK=0,GST_WEBRTC_SDP_TYPE_ANSWER=1;
GstElement* gst_object_ref(GstElement* p){++p->refs;return p;}
void gst_object_unref(GstElement* p){if(--p->refs<1)throw std::runtime_error("mock peer reference underflow");}
int gst_promise_wait(GstPromise* p){return p->replied?GST_PROMISE_RESULT_REPLIED:0;}
void gst_promise_unref(GstPromise* p){if(p->destroy)p->destroy(p->data);delete p;}
const GstStructure* gst_promise_get_reply(GstPromise* p){return &p->reply;}
void gst_structure_get(const GstStructure* reply,const char*,int,GstWebRTCSessionDescription** out,void*) {
  *out=new GstWebRTCSessionDescription{new GstSDPMessage{reply->sdp}};
}
gchar* gst_sdp_message_as_text(GstSDPMessage* sdp){auto* result=new char[sdp->text.size()+1];std::copy(sdp->text.begin(),sdp->text.end(),result);result[sdp->text.size()]=0;return result;}
void g_free(gchar* text){delete[] text;}
void gst_webrtc_session_description_free(GstWebRTCSessionDescription* d){delete d->sdp;delete d;}
std::function<void()> before_description;
GstPromise* gst_promise_new(){auto callback=std::exchange(before_description,{});if(callback)callback();return new GstPromise;}
GstPromise* gst_promise_new_with_change_func(void(*callback)(GstPromise*,void*),void* data,void(*destroy)(void*)) {
  auto* p=new GstPromise;p->callback=callback;p->data=data;p->destroy=destroy;return p;
}
void gst_promise_interrupt(GstPromise*){}
std::function<void(GstElement*)> local_hook;
std::function<void()> parse_hook;
bool invalid_parse=false;
void g_signal_emit_by_name(GstElement* p,const char*,std::nullptr_t,GstPromise* promise){p->offers.push_back(promise);}
void g_signal_emit_by_name(GstElement* p,const char* signal,GstWebRTCSessionDescription* description,GstPromise*) {
  if(std::string(signal)=="set-local-description") {p->local.push_back(description->sdp->text);if(local_hook)local_hook(p);}
  else p->remote.push_back(description->sdp->text);
}
void g_signal_emit_by_name(GstElement* p,const char*,guint,const char* text){p->added.emplace_back(text);}
int gst_sdp_message_new(GstSDPMessage** sdp){*sdp=new GstSDPMessage;return GST_SDP_OK;}
int gst_sdp_message_parse_buffer(const guint8* text,std::size_t size,GstSDPMessage* sdp) {
  sdp->text.assign(reinterpret_cast<const char*>(text),size);if(parse_hook)parse_hook();return invalid_parse?1:GST_SDP_OK;
}
void gst_sdp_message_free(GstSDPMessage* sdp){delete sdp;}
GstWebRTCSessionDescription* gst_webrtc_session_description_new(int,GstSDPMessage* sdp){return new GstWebRTCSessionDescription{sdp};}
std::string random_token(int){static int n=0;return "mock-negotiation-"+std::to_string(++n);}
enum class VideoCodec{H264,H265};
const char* to_string(VideoCodec value){return value==VideoCodec::H264?"h264":"h265";}
struct Signaling {Json incoming={{"messages",Json::array()}};std::vector<std::pair<std::string,Json>> sent;
  Json poll(const char*){return std::exchange(incoming,Json{{"messages",Json::array()}});}
  void send(const std::string& type,const Json& payload){sent.emplace_back(type,payload);}
  std::int64_t now_ms(){return 42;}
  const std::string& transport_mac_key() const { static const std::string key(32, 'f'); return key; }
};
struct Impl {
'''
MEMBERS = r'''
  struct { struct { int experimental_direct_authority_max_ms=0; } runtime; } config;
  void flush_control_status() {} // Control status has separate protocol/service coverage.
  void process_control_management(const Json&,const Json&) {}
  struct Lane {struct {std::string id="front";} camera;struct {int width=1,height=1,fps=1,bitrate_kbps=1;} profile;};
  std::vector<std::unique_ptr<Lane>> lanes;
  struct {VideoCodec codec=VideoCodec::H264,backend=VideoCodec::H264;} active_candidate;
  std::atomic<std::uint64_t> local_ice_candidate_count{0},remote_ice_candidate_count{0};
  std::shared_ptr<MediaSignalState> media_signal_state=std::make_shared<MediaSignalState>();
  Signaling signaling;bool answer_received=false,codec_fallback_requested=false;std::optional<std::int64_t> answer_received_at_ms;
  std::vector<std::string> errors,diagnostics;
  void emit_diagnostic(std::string_view event,std::string_view,std::string_view,std::string_view,std::string_view,bool,Json) {diagnostics.emplace_back(event);}
  void set_pipeline_error(std::string_view error,std::string_view,std::string_view,std::string_view,bool){errors.emplace_back(error);}
};
void expect(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
void activate(Impl& impl,GstElement& peer,std::size_t attempt=1) {
  auto state=impl.media_signal_state;
#ifdef HAS_MEDIA_SCOPE_STATUS
  state->begin_attempt(&impl,&peer,"mock-session");
  expect(state->attempt_count==attempt,"begin_attempt session counter differs from fixture attempt");
#else
  state->owner=&impl;state->peer=&peer;state->attempt_count=attempt;
  state->negotiation_id.clear();state->local_description_ready=false;state->offer_queued=false;
  state->local_ufrags.clear();state->remote_ufrags.clear();state->pending_remote_candidates.clear();state->pending_local_candidates.clear();state->pending.clear();
#endif
}
void negotiate(Impl& impl,GstElement& peer){auto state=impl.media_signal_state;Impl::on_negotiation_needed(&peer,&state);}
void complete(GstElement& peer,const std::string& ufrag="local",bool failed=false) {
  expect(!peer.offers.empty(),"fixture promise missing");auto* promise=peer.offers.front();peer.offers.erase(peer.offers.begin());
  promise->reply.sdp="v=0\r\na=ice-ufrag:"+ufrag+"\r\n";promise->replied=!failed;promise->callback(promise,promise->data);
}
Json ice(const std::string& id,const std::string& ufrag="remote") {
  Json payload={{"candidate","candidate:fixture 1 udp 1 127.0.0.1 9000 typ host"},{"sdpMLineIndex",0}};
  if(!id.empty())payload["media_negotiation_id"]=id;
  if(!ufrag.empty())payload["usernameFragment"]=ufrag;
  return payload;
}
void received(Impl& impl,const std::string& type,Json payload) {
  impl.signaling.incoming={{"messages",Json::array({{{"type",type},{"payload",std::move(payload)}}})}};impl.process_signaling();
}
void answer(Impl& impl,const std::string& id,const std::string& ufrag="remote") {
  Json payload={{"sdp","v=0\r\na=ice-ufrag:"+ufrag+"\r\n"}};if(!id.empty())payload["media_negotiation_id"]=id;received(impl,"webrtc_answer",payload);
}
void case_order() {
  Impl impl;GstElement peer;activate(impl,peer);negotiate(impl,peer);
  local_hook=[&](GstElement* source){auto state=impl.media_signal_state;char candidate[]="candidate:fixture 1 udp 1 127.0.0.1 9000 typ host ufrag local";
    Impl::on_ice_candidate(source,0,candidate,&state);impl.flush_outgoing_signals();expect(impl.signaling.sent.empty(),"candidate escaped before offer queue");};
  complete(peer);local_hook={};impl.flush_outgoing_signals();
  expect(impl.signaling.sent.size()==2,"offer/candidate missing");expect(impl.signaling.sent[0].first=="webrtc_offer"&&impl.signaling.sent[1].first=="ice_candidate","producer ICE arrived before offer");
  expect(impl.signaling.sent[0].second["media_negotiation_id"]==impl.signaling.sent[1].second["media_negotiation_id"],"producer candidate scope differed");expect(peer.refs==1,"offer context leaked peer reference");
#ifdef HAS_MEDIA_SCOPE_STATUS
  expect(impl.signaling.sent[0].second.value("legacy_bootstrap_allowed",false),"producer bootstrap declaration missing");
#endif
}
void case_late_callbacks() {
  Impl impl;GstElement old,replacement;activate(impl,old);negotiate(impl,old);auto state=impl.media_signal_state;
  state->owner=nullptr;state->peer=nullptr;activate(impl,replacement,2);negotiate(impl,replacement);const auto active=state->negotiation_id;
  complete(old,"old");char candidate[]="candidate:old 1 udp 1 127.0.0.1 9000 typ host";Impl::on_ice_candidate(&old,0,candidate,&state);
  expect(old.local.empty()&&replacement.local.empty()&&state->pending.empty(),"old promise/candidate touched replacement");
  expect(state->negotiation_id==active&&impl.errors.empty(),"old callback damaged current identity/error state");
  complete(replacement);impl.flush_outgoing_signals();expect(impl.signaling.sent.size()==1,"old callback left outgoing signaling");expect(old.refs==1&&replacement.refs==1,"late callback reference leaked");
  GstElement third;activate(impl,third,3);negotiate(impl,third);state->owner=nullptr;state->peer=nullptr;complete(third,"ignored",true);expect(impl.errors.empty(),"retired failed promise surfaced as current error");
}
void case_remote_order() {
  Impl impl;GstElement peer;activate(impl,peer);negotiate(impl,peer);complete(peer);const auto id=impl.media_signal_state->negotiation_id;
  received(impl,"ice_candidate",ice(id));received(impl,"ice_candidate",ice(id,"wrong"));received(impl,"ice_candidate",ice("old"));
  expect(peer.added.empty()&&impl.media_signal_state->pending_remote_candidates.size()==2,"pre-answer candidates not bounded/scoped in application");
  answer(impl,id);expect(peer.remote.size()==1&&peer.added.size()==1&&impl.answer_received,"matching answer did not drain matching ICE");
  expect(impl.media_signal_state->pending_remote_candidates.empty(),"pending candidate not drained");answer(impl,"old");expect(peer.remote.size()==1,"old answer applied");
}
void case_legacy_retry() {
  Impl impl;GstElement peer;activate(impl,peer);negotiate(impl,peer);complete(peer);answer(impl,"");expect(peer.remote.size()==1,"legacy first bootstrap broken");
  activate(impl,peer,2);negotiate(impl,peer);complete(peer);const auto id=impl.media_signal_state->negotiation_id;
  answer(impl,"");expect(peer.remote.size()==1,"untagged retry answer contaminated current offer");
#ifdef HAS_MEDIA_SCOPE_STATUS
  expect(impl.media_scope_status().value("issue_code","")=="media_negotiation_scope_missing"&&
      !impl.media_scope_status().value("next_action","").empty(),"legacy retry lacks visible status/next action");
#endif
  received(impl,"ice_candidate",ice("",""));expect(impl.media_signal_state->pending_remote_candidates.empty(),"unknown legacy retry candidate retained");
  answer(impl,id);received(impl,"ice_candidate",ice(id,""));received(impl,"ice_candidate",ice("","remote"));
  expect(peer.remote.size()==2&&peer.added.size()==2,"current tagged/ufrag retry rejected");
}
void case_stale_invalid_answer() {
  Impl impl;GstElement peer;activate(impl,peer);negotiate(impl,peer);complete(peer);const auto old=impl.media_signal_state->negotiation_id;
  GstElement replacement;
  parse_hook=[&]{activate(impl,replacement,2);negotiate(impl,replacement);};invalid_parse=true;answer(impl,old);parse_hook={};invalid_parse=false;
  expect(impl.errors.empty()&&peer.remote.empty(),"old invalid answer error failed replacement attempt");complete(replacement);
}
void case_bounded_pending() {
  Impl impl;GstElement peer;activate(impl,peer);negotiate(impl,peer);complete(peer);const auto id=impl.media_signal_state->negotiation_id;
  for(int n=0;n<128;n++)received(impl,"ice_candidate",ice(id));
  bool rejected=false;
  try{received(impl,"ice_candidate",ice(id));}catch(const std::runtime_error&){rejected=true;}
  expect(rejected&&impl.media_signal_state->pending_remote_candidates.size()==128,"remote queue exceeded bound");
}
void case_offer_check_to_emit() {
  Impl impl;GstElement peer;activate(impl,peer);negotiate(impl,peer);const auto original=impl.media_signal_state->negotiation_id;
  before_description=[&]{negotiate(impl,peer);if(!peer.offers.empty())complete(peer,"replacement");};
  complete(peer,"original");
  expect(impl.media_signal_state->negotiation_id==original,"offer epoch changed in check-to-emit gap");
  expect(peer.local.size()==1&&peer.local[0].find("original")!=std::string::npos,"old offer overwrote a newer same-peer description");
  negotiate(impl,peer);expect(peer.offers.empty(),"duplicate negotiation signal created a second offer");
}
void case_answer_check_to_emit() {
  Impl impl;GstElement peer;activate(impl,peer);negotiate(impl,peer);complete(peer);const auto original=impl.media_signal_state->negotiation_id;
  before_description=[&]{negotiate(impl,peer);if(!peer.offers.empty())complete(peer,"replacement");};
  answer(impl,original);
  expect(impl.media_signal_state->negotiation_id==original,"answer epoch changed in check-to-emit gap");
  expect(peer.local.size()==1&&peer.remote.size()==1&&impl.answer_received,"old answer contaminated a newer same-peer offer");
}
void case_failed_unpublished_attempt() {
#ifdef HAS_MEDIA_SCOPE_STATUS
  Impl impl;GstElement failed,peer;activate(impl,failed); // Fails before any offer.
  activate(impl,peer,2);negotiate(impl,peer);complete(peer);impl.flush_outgoing_signals();
  expect(!impl.signaling.sent[0].second.value("legacy_bootstrap_allowed",true),"unpublished previous attempt not declared to native peer");
  answer(impl,"");expect(!impl.answer_received&&!impl.media_scope_status().value("issue_code","").empty(),"missing id retry did not report visible issue");
  GstElement new_session;impl.media_signal_state->begin_attempt(&impl,&new_session,"new-mock-session");negotiate(impl,new_session);complete(new_session);
  expect(impl.media_signal_state->attempt_count==1&&impl.media_signal_state->offer_count==1,"fresh session retained legacy retry counters");
#endif
}
int main(){int failed=0;const std::vector<std::pair<const char*,void(*)()>> cases={
  {"ICE-VEHICLE-MOCK-001",case_order},{"ICE-VEHICLE-MOCK-002",case_late_callbacks},
  {"ICE-VEHICLE-MOCK-003",case_remote_order},{"ICE-VEHICLE-MOCK-004",case_legacy_retry},
  {"ICE-VEHICLE-MOCK-005",case_stale_invalid_answer},{"ICE-VEHICLE-MOCK-006",case_bounded_pending}};
  auto selected=cases;
#ifdef RACE_ONLY
  selected.clear();
#endif
  selected.emplace_back("ICE-VEHICLE-RACE-001",case_offer_check_to_emit);
  selected.emplace_back("ICE-VEHICLE-RACE-002",case_answer_check_to_emit);
#ifndef RACE_ONLY
  selected.emplace_back("ICE-VEHICLE-MIXED-001",case_failed_unpublished_attempt);
#endif
  for(const auto& [id,run]:selected){try{run();std::cout<<"PASS "<<id<<'\n';}catch(const std::exception& e){++failed;std::cerr<<"FAIL "<<id<<": "<<e.what()<<'\n';}}
  return failed?1:0;
}
'''


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--source", type=Path, default=SOURCE)
    parser.add_argument("--race-only", action="store_true")
    parser.add_argument("--compiler", default="/usr/bin/c++")
    parser.add_argument("--json-include")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = args.source.read_text()
    methods = section(source, "  struct MediaSignalState {", "  static constexpr std::size_t kControlTrace")
    methods += section(source, "  static void on_ice_candidate(", "  static void on_control_channel_open(")
    methods += section(source, "  static void on_offer_created(", "  void emit_diagnostic(")
    methods += section(source, "  void flush_outgoing_signals()", "  void poll_bus()")
    unit = args.output / "actual-vehicle-methods-mock.cpp"
    definitions = "#define HAS_MEDIA_SCOPE_STATUS\n" if "media_scope_status()" in source else ""
    if args.race_only:
        definitions += "#define RACE_ONLY\n"
    unit.write_text(definitions + PRELUDE + methods + MEMBERS)
    binary = args.output / "actual-vehicle-methods-mock"
    includes = list((ROOT.parent / "offline-deps").glob("json-*/include"))
    if args.json_include is not None:
        includes = [Path(value) for value in args.json_include.split(";") if value]
    elif len(includes) > 1:
        raise RuntimeError("ambiguous local JSON include; pass --json-include")
    # With no local checkout/override, use the compiler's system include path.
    # Missing dependencies fail compilation and never become a skipped PASS.
    command = [args.compiler, "-std=c++20", "-Wall", "-Wextra", "-Wpedantic", "-Werror"]
    command += ["-I" + str(value) for value in includes]
    if args.sanitize:
        command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
    command += [str(unit), "-o", str(binary)]
    built = subprocess.run(command, capture_output=True, text=True)
    (args.output / "build.log").write_text(built.stdout + built.stderr)
    # CTest's output-on-failure must show the compiler/runtime diagnostics,
    # rather than only the artifact manifest and an opaque exit code.
    if built.stdout:
        print(built.stdout, end="", file=sys.stdout)
    if built.stderr:
        print(built.stderr, end="", file=sys.stderr)
    result = subprocess.run([str(binary)], capture_output=True, text=True) if built.returncode == 0 else None
    if result:
        (args.output / "result.log").write_text(result.stdout + result.stderr)
        if result.stdout:
            print(result.stdout, end="", file=sys.stdout)
        if result.stderr:
            print(result.stderr, end="", file=sys.stderr)
    manifest = {"scope":__doc__, "source_sha256":hashlib.sha256(source.encode()).hexdigest(), "source_path":str(args.source),
        "script_sha256":hashlib.sha256(Path(__file__).read_bytes()).hexdigest(), "command":command,
        "generated_unit_sha256":hashlib.sha256(unit.read_bytes()).hexdigest(),"compile_exit":built.returncode,
        "run_exit":result.returncode if result else None,
        "binary_sha256":hashlib.sha256(binary.read_bytes()).hexdigest() if built.returncode == 0 and binary.exists() else None}
    (args.output / "manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    print(json.dumps(manifest, indent=2))
    return built.returncode or (result.returncode if result else 1)


if __name__ == "__main__":
    raise SystemExit(main())
