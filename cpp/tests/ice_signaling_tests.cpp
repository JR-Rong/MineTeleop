#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/websocket.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using mine_teleop::Json;
void expect(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template <class Function> void rejected(Function action, const char* reason) {
  try { action(); } catch (const std::exception&) { return; }
  throw std::runtime_error(reason);
}
struct Fixture {
  std::shared_ptr<mine_teleop::SignalingService> service;
  std::unique_ptr<mine_teleop::SimpleHttpServer> server;
  mine_teleop::HttpClient http;
  std::unique_ptr<mine_teleop::DriverConsoleRuntime> driver;
  std::string base;
  std::uint64_t vehicle_generation{};
  std::uint64_t vehicle_sequence{0};
  explicit Fixture(bool websocket) {
    mine_teleop::SignalingServerConfig config;
    config.driver_passwords = {{"ice-driver", "fixture-password"}};
    config.device_tokens = {{"ice-vehicle", "fixture-device-token"}};
    config.driver_vehicle_permissions = {{"ice-driver", {"ice-vehicle"}}};
    service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
    server = std::make_unique<mine_teleop::SimpleHttpServer>("127.0.0.1", 0,
        [this](const auto& request) { return service->handle(request); }, 8 * 1024 * 1024,
        [this](int socket, const auto& request) { return service->handle_websocket(socket, request); });
    server->start(); base = "http://127.0.0.1:" + std::to_string(server->port());
    const auto online = http.post_json_response(base + "/vehicles/online",
        {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities},
         {"vehicle_id", "ice-vehicle"}, {"device_token", "fixture-device-token"},
         {"connection_id", "ice-vehicle-runtime"}});
    vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
    mine_teleop::DriverConfig driver_config;
    driver_config.driver_id = "ice-driver";
    driver_config.signaling_url = websocket ? "ws://127.0.0.1:" + std::to_string(server->port()) + "/signaling" : base;
    driver = std::make_unique<mine_teleop::DriverConsoleRuntime>(driver_config, "ice-vehicle", "fixture-password");
    static_cast<void>(driver->connect("ice-vehicle"));
    expect(driver->status().at("native_control").at("control_transport").at("experimental_enabled") == false,
        "default driver activated unreviewed direct-control authority semantics");
  }
  ~Fixture() { driver.reset(); server->stop(); }
  Json scope() const {
    const auto status = driver->status();
    return {{"source_session_id", status.at("session_id")},
            {"source_control_session_generation", status.at("control_session_generation")}};
  }
  void offer(const std::string& id, bool legacy_allowed = true) {
    const auto session=driver->status().at("session_id").get<std::string>();
    const auto now=std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    auto message=mine_teleop::ProtocolMetadata{mine_teleop::kProtocolVersion,"ice-vehicle","ice-driver",session,++vehicle_sequence,now}.to_json();
    message.update({{"sender","ice-vehicle"},{"recipient","ice-driver"},{"device_token","fixture-device-token"},
        {"connection_generation",vehicle_generation},{"type","webrtc_offer"},
        {"payload",{{"type","offer"},{"sdp","v=0\r\na=ice-ufrag:fixture-offer\r\n"},{"media_negotiation_id",id},{"legacy_bootstrap_allowed",legacy_allowed}}}});
    static_cast<void>(http.post_json_response(base+"/signaling/"+session+"/messages",message));
    expect(driver->poll_signaling().at("messages").size()==1,"fixture offer was not delivered to runtime/page");
  }
  Json messages() {
    return http.get_json(base + "/signaling/" + driver->status().at("session_id").get<std::string>() +
        "/messages?recipient=ice-vehicle&device_token=fixture-device-token&connection_generation=" +
        std::to_string(vehicle_generation) + "&types=webrtc_answer,ice_candidate,media_fallback").at("messages");
  }
};
Json answer(Json scope, const char* id="offer-a") {
  scope.update({{"type", "answer"}, {"sdp", "v=0\r\na=ice-ufrag:fixture-answer\r\n"}, {"media_negotiation_id", id}});
  return scope;
}
Json candidate(Json scope, const char* id="offer-a") {
  scope.update({{"media_negotiation_id", id}, {"candidate", {{"candidate", "candidate:fixture 1 udp 1 127.0.0.1 9000 typ host"},
      {"usernameFragment", "fixture-answer"}, {"sdpMLineIndex", 0}}}});
  return scope;
}
Json fallback(Json scope) { scope.update({{"codec", "h264"}, {"media_negotiation_id", "offer-a"}});return scope; }
Json metrics(Json scope, const char* state="connected") {
  scope.update({{"connection_state", state}, {"connection_method", "direct"}, {"turn_in_use", false},
      {"time_sync", {{"synchronized", true}, {"uncertainty_ms", 1}}}, {"streams", Json::array()}});return scope;
}
void test_forward_scope_and_negotiation(bool websocket) {
  Fixture f(websocket); const auto scope=f.scope();
  static_cast<void>(f.driver->send_webrtc_answer(answer(scope)));
  static_cast<void>(f.driver->send_webrtc_ice_candidate(candidate(scope)));
  static_cast<void>(f.driver->send_media_fallback(fallback(scope)));
  const auto messages=f.messages();expect(messages.size()==3,"current media signals were lost");
  for(const auto& message:messages) {
    expect(message.at("payload").value("media_negotiation_id", "") == "offer-a", "negotiation id lost in actual send/cloud queue");
    expect(message.at("session_id")==scope.at("source_session_id"),"media signal envelope changed source session");
  }
  expect(messages[1].at("payload").value("usernameFragment", "")=="fixture-answer","candidate payload was not normalized/retained");
  auto stale=scope;stale["source_control_session_generation"]=scope.at("source_control_session_generation").get<std::uint64_t>()+1;
  rejected([&]{static_cast<void>(f.driver->send_webrtc_answer(answer(stale)));},"stale answer forwarded");
  rejected([&]{static_cast<void>(f.driver->send_webrtc_ice_candidate(candidate(stale)));},"stale candidate forwarded");
  rejected([&]{static_cast<void>(f.driver->send_media_fallback(fallback(stale)));},"stale codec fallback forwarded");
  expect(f.messages().empty(),"stale source crossed actual cloud signaling queue");
  auto incomplete=scope;incomplete.erase("source_control_session_generation");
  rejected([&]{static_cast<void>(f.driver->send_webrtc_answer(answer(incomplete)));},"incomplete source scope accepted");
  auto malformed=answer(scope);malformed["media_negotiation_id"]=std::string(129,'x');
  rejected([&]{static_cast<void>(f.driver->send_webrtc_answer(malformed));},"unbounded negotiation identity accepted");
}
void test_reconnect_and_metrics_fence(bool websocket) {
  Fixture f(websocket);const auto old=f.scope();
  static_cast<void>(f.driver->ingest_webrtc_metrics(metrics(old)));
  rejected([&]{auto bad=old;bad["source_session_id"]="stale-session";static_cast<void>(f.driver->ingest_webrtc_metrics(metrics(bad,"failed")));},"stale metrics accepted");
  expect(f.driver->status().at("webrtc_metrics").value("connection_state", "")=="connected","stale metrics changed current health cache");
  static_cast<void>(f.driver->disconnect("fixture-reconnect"));
  static_cast<void>(f.driver->login("fixture-password"));
  static_cast<void>(f.driver->connect("ice-vehicle"));const auto current=f.scope();
  expect(current.at("source_session_id")!=old.at("source_session_id"),"fixture did not create a replacement session");
  rejected([&]{static_cast<void>(f.driver->send_webrtc_answer(answer(old)));},"old answer sent in replacement session");
  rejected([&]{static_cast<void>(f.driver->send_webrtc_ice_candidate(candidate(old)));},"old candidate sent in replacement session");
  rejected([&]{static_cast<void>(f.driver->send_media_fallback(fallback(old)));},"old fallback sent in replacement session");
  rejected([&]{static_cast<void>(f.driver->ingest_webrtc_metrics(metrics(old,"failed")));},"old metrics survived replacement session");
  expect(f.messages().empty(),"old media callback produced replacement cloud messages");
  static_cast<void>(f.driver->send_webrtc_answer(answer(current,"offer-b")));
  expect(f.messages().at(0).at("payload").value("media_negotiation_id", "")=="offer-b","replacement answer failed");
  // Older clients can still make an API call without the new source fields.
  auto legacy = answer(Json::object()); legacy.erase("media_negotiation_id");
  static_cast<void>(f.driver->send_webrtc_answer(legacy));
  expect(f.messages().size()==1,"legacy API compatibility was accidentally removed");
}
void test_mixed_version_and_same_session_metrics(bool websocket) {
  Fixture f(websocket);const auto scope=f.scope();f.offer("offer-unpublished-retry",false);
  auto unsupported=answer(scope);unsupported.erase("media_negotiation_id");
  rejected([&]{static_cast<void>(f.driver->send_webrtc_answer(unsupported));},"producer-declared unpublished retry silently admitted");
  static_cast<void>(f.driver->disconnect("fixture-new-bootstrap"));static_cast<void>(f.driver->login("fixture-password"));static_cast<void>(f.driver->connect("ice-vehicle"));
  const auto current_scope=f.scope();f.offer("offer-a");
  auto legacy=answer(current_scope);legacy.erase("media_negotiation_id");
  static_cast<void>(f.driver->send_webrtc_answer(legacy));expect(f.messages().size()==1,"legacy first bootstrap rejected");
  f.offer("offer-b");
  bool diagnostic=false;
  try { static_cast<void>(f.driver->send_webrtc_answer(legacy)); }
  catch(const std::exception& error) { diagnostic=std::string(error.what()).find("重新打开控制页面并建立新会话")!=std::string::npos; }
  expect(diagnostic,"retry missing id lacks visible error/next action");
  rejected([&]{static_cast<void>(f.driver->send_webrtc_ice_candidate(candidate(current_scope,"offer-a")));},"old same-session candidate forwarded");
  rejected([&]{static_cast<void>(f.driver->send_media_fallback(fallback(current_scope)));},"old same-session codec fallback forwarded");
  auto current=metrics(current_scope);current["media_negotiation_id"]="offer-b";
  static_cast<void>(f.driver->ingest_webrtc_metrics(current));
  auto old=metrics(current_scope,"failed");old["media_negotiation_id"]="offer-a";
  rejected([&]{static_cast<void>(f.driver->ingest_webrtc_metrics(old));},"old same-session metrics accepted");
  expect(f.driver->status().at("webrtc_metrics").value("media_negotiation_id","")=="offer-b"&&
      f.driver->status().at("webrtc_metrics").value("connection_state","")=="connected","old offer overwrote current native metrics cache");
  expect(!f.driver->ingest_webrtc_metrics(current).value("reported",true),"old rejected metrics changed current audit transition key");
  expect(f.messages().empty(),"stale/missing scope produced vehicle signaling");
  static_cast<void>(f.driver->send_webrtc_answer(answer(current_scope,"offer-b")));expect(f.messages().size()==1,"new offer answer rejected");
  f.offer(""); // Mixed new controller + legacy vehicle cannot fence retries.
  rejected([&]{static_cast<void>(f.driver->send_webrtc_answer(legacy));},"legacy vehicle retry was silently admitted without a provable offer id");
}

}  // namespace
int main() {
  int failures=0;
  for(const bool websocket:{false,true}) {
    for(const bool reconnect:{false,true}) {
      const std::string id=std::string("ICE-NATIVE-")+(websocket?"WSS":"HTTP")+(reconnect?"-002":"-001");
      try { if(reconnect)test_reconnect_and_metrics_fence(websocket);else test_forward_scope_and_negotiation(websocket);
        std::cout<<"PASS "<<id<<'\n'; }
      catch(const std::exception& error) { ++failures;std::cerr<<"FAIL "<<id<<": "<<error.what()<<'\n'; }
    }
  }
  for(const bool websocket:{false,true}) {
    const std::string id=std::string("ICE-MIXED-NATIVE-")+(websocket?"WSS":"HTTP");
    try { test_mixed_version_and_same_session_metrics(websocket);std::cout<<"PASS "<<id<<'\n'; }
    catch(const std::exception& error){++failures;std::cerr<<"FAIL "<<id<<": "<<error.what()<<'\n';}
  }
  return failures==0?0:1;
}
