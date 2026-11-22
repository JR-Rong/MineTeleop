// Compile the owning implementation, not an extracted/reimplemented run loop.
// Private inspection is confined to the friend below; no shipping test hooks.
#include "../src/webrtc_media.cpp"

namespace mine_teleop {
struct VehicleMediaRuntimeTestAccess {
  using Impl = VehicleMediaRuntime::Impl;
  struct Retired {
    GstElement* peer{};
    GstWebRTCDataChannel* channel{};
    std::string id;
    std::shared_ptr<Impl::MediaSignalState> state;
    ~Retired() { if (peer) gst_object_unref(peer); if (channel) g_object_unref(channel); }
  };
  static Json snapshot(VehicleMediaRuntime& runtime) {
    auto& impl=*runtime.impl_;
    auto result=impl.media_scope_status();
    {
      std::lock_guard lock(impl.media_signal_state->mutex);
      result["pending_candidates"]=impl.media_signal_state->pending_remote_candidates.size();
      result["remote_ufrag_count"]=impl.media_signal_state->remote_ufrags.size();
    }
    std::lock_guard lock(impl.control_mutex);
    result["control_started"]=impl.control_service_started;
    result["control_present"]=bool(impl.control_service);
    result["channel_present"]=impl.control_channel!=nullptr;
    result["control_open"]=impl.control_link_open.load();
    result["control_inhibited"]=impl.control_inhibited.load();
    result["remote_candidates"]=impl.remote_ice_candidate_count.load();
    return result;
  }
  static void retain_and_open_mock(VehicleMediaRuntime& runtime, Retired& old) {
    auto& impl=*runtime.impl_;
    {
      std::lock_guard lock(impl.media_signal_state->mutex);
      old.state=impl.media_signal_state; old.id=old.state->negotiation_id;
      old.peer=GST_ELEMENT(gst_object_ref(old.state->peer));
    }
    {
      std::lock_guard lock(impl.control_mutex);
      old.channel=GST_WEBRTC_DATA_CHANNEL(g_object_ref(impl.control_channel));
    }
    // Deterministic lifecycle precondition. Only test_bench_* + mock adapter
    // can enter this fixture. This does not simulate a completed VCU handshake.
    Impl::on_control_channel_open(old.channel,&impl);
  }
  static bool can_open_mock(VehicleMediaRuntime& runtime) {
    return runtime.impl_->critical_cameras_ready();
  }
  static void late_callbacks(VehicleMediaRuntime& runtime, Retired& old) {
    auto& impl=*runtime.impl_;
    auto* context=new Impl::MediaOfferContext{old.state,GST_ELEMENT(gst_object_ref(old.peer)),old.id};
    auto* promise=gst_promise_new_with_change_func(Impl::on_answer_applied,context,
        +[](gpointer value){delete static_cast<Impl::MediaOfferContext*>(value);});
    gst_promise_reply(promise,gst_structure_new_empty("application/x-gst-promise"));
    impl.apply_remote_ice({{"candidate","candidate:old 1 UDP 100 127.0.0.1 50000 typ host"},
                          {"sdpMLineIndex",0},{"media_negotiation_id",old.id}});
    Impl::on_control_channel_open(old.channel,&impl);
    Impl::on_control_channel_close(old.channel,&impl);
    gchar late_message[]="{}";
    Impl::on_control_message_string(old.channel,late_message,&impl);
  }
  static bool fresh_configuration(VehicleMediaRuntime& runtime, const Retired& old) {
    auto& impl=*runtime.impl_;
    std::lock_guard lock(impl.media_signal_state->mutex);
    return impl.media_signal_state->peer!=old.peer && impl.media_signal_state->negotiation_id!=old.id &&
        impl.ice_configuration.at("ice_servers").at(0).at("username")=="fresh-fixture";
  }
};
} // namespace mine_teleop

namespace {
using namespace mine_teleop;
using Access=VehicleMediaRuntimeTestAccess;
using namespace std::chrono_literals;
void require_runtime(bool value,const char* message) { if(!value)throw std::runtime_error(message); }
template<class F> void until(F condition,const char* message) {
  const auto end=std::chrono::steady_clock::now()+8s;
  while(!condition()) { require_runtime(std::chrono::steady_clock::now()<end,message);std::this_thread::sleep_for(10ms); }
}
struct AnswerPeer {
  GstElement* pipeline=gst_pipeline_new(nullptr);
  GstElement* peer=gst_element_factory_make("webrtcbin",nullptr);
  AnswerPeer() {
    require_runtime(peer!=nullptr,"webrtcbin unavailable");gst_bin_add(GST_BIN(pipeline),peer);
    require_runtime(gst_element_set_state(pipeline,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"receiver PLAYING failed");
  }
  ~AnswerPeer() { gst_element_set_state(pipeline,GST_STATE_NULL);gst_object_unref(pipeline); }
  void apply(const char* action,GstWebRTCSessionDescription* value) {
    auto* promise=gst_promise_new();g_signal_emit_by_name(peer,action,value,promise);
    const auto result=gst_promise_wait(promise);
    const auto* reply=gst_promise_get_reply(promise);
    const bool ok=result==GST_PROMISE_RESULT_REPLIED && (!reply||!gst_structure_has_field(reply,"error"));
    gst_promise_unref(promise);require_runtime(ok,"SDP apply failed");
  }
  std::string answer(const std::string& text) {
    GstSDPMessage* sdp=nullptr;gst_sdp_message_new(&sdp);
    require_runtime(gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(text.data()),text.size(),sdp)==GST_SDP_OK,"bad offer");
    auto* offer=gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER,sdp);
    apply("set-remote-description",offer);gst_webrtc_session_description_free(offer);
    auto* promise=gst_promise_new();g_signal_emit_by_name(peer,"create-answer",nullptr,promise);
    require_runtime(gst_promise_wait(promise)==GST_PROMISE_RESULT_REPLIED,"answer did not complete");
    GstWebRTCSessionDescription* answer=nullptr;
    const bool valid=gst_structure_get(gst_promise_get_reply(promise),"answer",GST_TYPE_WEBRTC_SESSION_DESCRIPTION,&answer,nullptr);
    gst_promise_unref(promise);require_runtime(valid&&answer,"answer unavailable");
    apply("set-local-description",answer);
    auto* raw=gst_sdp_message_as_text(answer->sdp);std::string result(raw);g_free(raw);gst_webrtc_session_description_free(answer);
    return result;
  }
};
void verify_runtime(bool shutdown_during_refresh) {
  auto config=load_vehicle_config("configs/vehicle-agent.dev.yaml");
  config.vehicle_id="test_bench_ice_runtime"; config.runtime.mock_control_diagnostic=true;
  config.runtime.control_enabled=true; config.runtime.control_log_commands=false;
  config.cameras.clear();config.recording.enabled=false;config.upload.enabled=false;
  config.cloud.ice_direct_timeout_ms=1200;config.runtime.teleop_poll_interval_ms=10;
  validate_mock_control_diagnostic(config);
  SignalingServerConfig server_config;
  server_config.driver_passwords={{"fixture-driver","fixture-password"}};
  server_config.device_tokens={{config.vehicle_id,"fixture-device"}};
  server_config.driver_vehicle_permissions={{"fixture-driver",{config.vehicle_id}}};
  server_config.api_rate_limit_requests=12000;
  server_config.stun_urls={"stun:127.0.0.1:34789"};
  auto service=std::make_shared<SignalingService>(server_config);
  std::stop_source shutdown;
  std::atomic<int> fetches{0},offers{0};
  std::atomic<bool> closed_before_refresh{false};
  std::mutex delayed_mutex;
  Json delayed_messages=Json::array();
  std::atomic<bool> delayed_delivered{false};
  VehicleMediaRuntime* current=nullptr;
  SimpleHttpServer server("127.0.0.1",0,[&](const HttpRequest& request) {
    auto response=service->handle(request);
    if(response.status==200 && request.path.ends_with("/ice_servers")) {
      const int count=++fetches;
      if(count==2) {
        const auto state=Access::snapshot(*current);
        closed_before_refresh=!state.at("control_started").get<bool>() && !state.at("control_present").get<bool>() &&
            !state.at("channel_present").get<bool>() && !state.at("answer_applied").get<bool>();
        if(shutdown_during_refresh)shutdown.request_stop();
      }
      // Deliberately unreachable loopback TURN. This test proves lifecycle and
      // credential refresh; ice_fallback_gstreamer_tests proves actual TURN IO.
      return ServerResponse::json(200,{{"ice_servers",Json::array({{{"urls","turn:127.0.0.1:34789?transport=udp"},
          {"username",count==1?"retired-fixture":"fresh-fixture"},{"credential","fixture-only"}}})}});
    }
    if(request.method=="POST" && request.path.ends_with("/messages") && request.json_body().value("type","")=="webrtc_offer")++offers;
    if(response.status==200 && request.method=="GET" && request.path.ends_with("/messages") &&
        request.query.contains("recipient") && request.query.at("recipient")==config.vehicle_id) {
      std::lock_guard lock(delayed_mutex);
      if(!delayed_messages.empty()) {
        auto body=Json::parse(response.body);
        for(const auto& message:delayed_messages)body["messages"].push_back(message);
        delayed_messages.clear();delayed_delivered=true;
        return ServerResponse::json(200,body);
      }
    }
    return response;
  // Keep cloud-control WSS unavailable: only the explicitly injected channel
  // open may create a mock service, avoiding a second, racing authority path.
  },8*1024*1024,[&](SocketHandle socket,const HttpRequest& request){
    if(request.query.contains("recipient") && request.query.at("recipient")==config.vehicle_id)return false;
    return service->handle_websocket(socket,request);
  });
  server.start();
  const auto url="http://127.0.0.1:"+std::to_string(server.port());
  VehicleMediaRuntime runtime(config,url,"fixture-device");current=&runtime;
  DriverConfig dc;dc.driver_id="fixture-driver";dc.signaling_url=url;dc.browser_event_log_path.clear();
  DriverConsoleRuntime driver(dc,config.vehicle_id,"fixture-password");
  std::atomic<bool> finished{false};std::string failure;
  std::jthread worker([&] {
    try { static_cast<void>(runtime.run(0,0,0,shutdown.get_token())); }
    catch(const std::exception& error){failure=error.what();}
    finished=true;
  });
  // Always cancel/join before runtime, driver or server destruction, including
  // failed assertions. No test failure can leave a live background service.
  struct Cleanup {std::stop_source& stop;std::jthread& worker;~Cleanup(){stop.request_stop();worker.join();}} cleanup{shutdown,worker};
  static_cast<void>(driver.login());
  until([&]{const auto vehicles=driver.vehicles();for(const auto& item:vehicles.at("vehicles"))if(item.value("vehicle_id","")==config.vehicle_id&&item.value("online",false))return true;return false;},"vehicle not online");
  const auto session=driver.connect(config.vehicle_id,"runtime-fallback-fixture");
  const Json scope{{"source_session_id",session.at("session_id")},{"source_control_session_generation",session.at("control_session_generation")}};
  const auto receive_offer=[&] {
    Json offer;
    until([&]{const auto messages=driver.poll_signaling();for(const auto& message:messages.at("messages"))if(message.value("type","")=="webrtc_offer")offer=message.at("payload");return !offer.is_null();},"runtime offer missing");
    return offer;
  };
  const auto send_answer=[&](const Json& offer,AnswerPeer& receiver) {
    auto payload=scope;payload.update({{"type","answer"},{"sdp",receiver.answer(offer.at("sdp"))},{"media_negotiation_id",offer.at("media_negotiation_id")}});
    static_cast<void>(driver.send_webrtc_answer(payload));return payload;
  };
  const auto first=receive_offer();require_runtime(first.at("ice_route_mode")=="direct","first stage not direct");
  AnswerPeer first_receiver;
  const auto old_answer=send_answer(first,first_receiver);
  until([&]{return Access::snapshot(runtime).at("answer_applied").get<bool>();},"matching answer not applied");
  until([&]{return Access::can_open_mock(runtime);},"initial mock safety gate not ready");
  Access::Retired old;Access::retain_and_open_mock(runtime,old);
  require_runtime(Access::snapshot(runtime).at("control_started").get<bool>(),"mock control precondition not established");
  // Withhold all remote candidates. Actual run() observes answer-applied, hits
  // its direct deadline, closes control and initiates a fresh credential GET.
  until([&]{return fetches.load()==2;},"production fallback did not fetch new credentials");
  require_runtime(closed_before_refresh,"control/peer was live during credential refresh");
  if(shutdown_during_refresh) {
    until([&]{return finished.load();},"shutdown during refresh did not terminate");
    require_runtime(offers==1,"shutdown created a replacement negotiation");
  } else {
    const auto second=receive_offer();
    require_runtime(second.at("ice_route_mode")=="all" && Access::fresh_configuration(runtime,old),"fallback reused peer, ID or credentials");
    const auto before=Access::snapshot(runtime);
    require_runtime(!before.at("answer_applied").get<bool>() && !before.at("control_started").get<bool>(),"readiness inherited by replacement");
    Access::late_callbacks(runtime,old);
    bool rejected=false;
    try { static_cast<void>(driver.send_webrtc_answer(old_answer)); }
    catch(const std::runtime_error& error) { rejected=std::string(error.what()).find("negotiation changed")!=std::string::npos; }
    require_runtime(rejected,"controller accepted a stale answer");
    // Also model messages already in flight before the controller fence moved:
    // they reach the real vehicle polling loop, beyond controller-side checks.
    {
      std::lock_guard lock(delayed_mutex);
      delayed_messages=Json::array({{{"type","webrtc_answer"},{"payload",old_answer}},
          {{"type","ice_candidate"},{"payload",{{"candidate","candidate:old 1 UDP 100 127.0.0.1 50000 typ host"},
           {"sdpMLineIndex",0},{"media_negotiation_id",old.id}}}}});
    }
    until([&]{return delayed_delivered.load();},"stale messages did not reach production poll");
    std::this_thread::sleep_for(150ms);
    require_runtime(Access::snapshot(runtime)==before,"retired callbacks/signaling polluted replacement");
    AnswerPeer second_receiver;send_answer(second,second_receiver);
    until([&]{return Access::snapshot(runtime).at("answer_applied").get<bool>();},"replacement answer not applied");
    until([&]{return Access::can_open_mock(runtime);},"replacement mock safety gate not ready");
    Access::Retired replacement;Access::retain_and_open_mock(runtime,replacement);
    require_runtime(Access::snapshot(runtime).at("control_started").get<bool>(),"fresh mock service failed to start");
    const auto active=Access::snapshot(runtime);Access::late_callbacks(runtime,old);
    require_runtime(Access::snapshot(runtime)==active,"old DataChannel affected fresh mock service");
    std::this_thread::sleep_for(1400ms);
    require_runtime(fetches==2 && offers==2,"fallback retried more than once");
    shutdown.request_stop();until([&]{return finished.load();},"runtime shutdown did not terminate");
  }
  require_runtime(failure=="vehicle media stop requested","runtime ended for an unexpected reason");
  const auto stopped=Access::snapshot(runtime);
  require_runtime(!stopped.at("control_started").get<bool>() && !stopped.at("control_present").get<bool>(),"shutdown left control service alive");
  std::cout<<"PASS actual VehicleMediaRuntime "<<(shutdown_during_refresh?"shutdown during credential refresh":"answer/deadline/teardown/fresh credentials/peer/stale callbacks/bounded retry")<<'\n';
}
}
int main(int argc,char** argv) {
  gst_init(&argc,&argv);
  try {verify_runtime(false);verify_runtime(true);return 0;}
  catch(const std::exception& error){std::cerr<<"FAIL runtime integration: "<<error.what()<<'\n';return 1;}
}
