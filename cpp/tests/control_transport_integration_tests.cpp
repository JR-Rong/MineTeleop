#include "mine_teleop/server.hpp"
#include "mine_teleop/websocket.hpp"
#include "control_time_fixture.hpp"

#include <chrono>
#include <deque>
#include <iostream>
#include <thread>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
std::int64_t utc() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::int64_t steady() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

// Only the DC byte courier is mocked. Native producer, cloud HTTP/WSS,
// authentication, transport fence and the common safety service are real.
struct Fixture {
  std::shared_ptr<SignalingService> cloud;
  std::unique_ptr<SimpleHttpServer> server;
  std::unique_ptr<DriverConsoleRuntime> driver;
  std::unique_ptr<test::LogicalTimeControlService> vehicle;
  HttpClient http;
  WebSocketClient vehicle_socket;
  ControlTransportFence fence;
  std::string base, session, control_token, signing_key;
  std::uint64_t generation{0}, ui_epoch{0}, intent_seq{0}, result_seq{0}, last_dc_seq{0};
  std::uint64_t wss_commands{0}, dc_commands{0};
  std::int64_t last_input{0};
  std::deque<std::pair<std::int64_t, Json>> delayed;
  std::vector<Json> received_statuses;
  explicit Fixture() {
    SignalingServerConfig config;
    config.driver_passwords = {{"transport-driver", "fixture-password"}};
    config.device_tokens = {{"transport-vehicle", "fixture-device"}};
    config.driver_vehicle_permissions = {{"transport-driver", {"transport-vehicle"}}};
    cloud = std::make_shared<SignalingService>(config);
    server = std::make_unique<SimpleHttpServer>("127.0.0.1", 0,
        [this](const HttpRequest& request) { return cloud->handle(request); }, 8 * 1024 * 1024,
        [this](SocketHandle socket, const HttpRequest& request) { return cloud->handle_websocket(socket, request); });
    server->start(); base = "http://127.0.0.1:" + std::to_string(server->port());
    const auto online = http.post_json_response(base + "/vehicles/online", {{"vehicle_id", "transport-vehicle"},
        {"device_token", "fixture-device"}, {"connection_id", "offline-transport-runtime"},
        {"native_control_capabilities", kNativeControlCapabilities}});
    generation = online.at("connection_generation").get<std::uint64_t>();
    DriverConfig driver_config; driver_config.enable_experimental_control_transport = true; driver_config.driver_id = "transport-driver"; driver_config.signaling_url = base;
    driver = std::make_unique<DriverConsoleRuntime>(driver_config, "transport-vehicle", "fixture-password");
    const auto connected = driver->connect("transport-vehicle", "owner");
    expect(!connected.contains("transport_mac_key"), "local UI connect response exposed signing material");
    session = connected.at("session_id").get<std::string>();
    const auto discovered = http.get_json(base + "/vehicles/transport-vehicle/session?connection_generation=" + std::to_string(generation),
        {{"X-Mine-Teleop-Device-Token", "fixture-device"}});
    control_token = discovered.at("control_token").get<std::string>();
    signing_key = discovered.at("transport_mac_key").get<std::string>();
    auto config_vehicle = load_vehicle_config("configs/vehicle-agent.dev.yaml"); config_vehicle.vehicle_id = "transport-vehicle";
    vehicle = std::make_unique<test::LogicalTimeControlService>(config_vehicle, "transport-driver", session,
        control_token, std::make_unique<MockVehicleAdapter>());
    vehicle->start(utc());
    SessionControlProfileRequest profile; profile.vehicle_id = "transport-vehicle"; profile.driver_id = "transport-driver";
    profile.session_id = session; profile.control_token = control_token; profile.seq = 1; profile.sent_at_utc_ms = utc();
    expect(vehicle->receive_session_profile(profile, utc()).accepted, "offline vehicle profile rejected");
    fence.reset("transport-vehicle", "transport-driver", session, control_token);
    vehicle_socket.connect(signaling_websocket_url(base, session, "transport-vehicle", std::to_string(generation)) +
        "&types=control_command&transport_protocol=" + std::string(kControlTransportCapability),
        {{"X-Mine-Teleop-Device-Token", "fixture-device"}});
    auto offer = ProtocolMetadata{kProtocolVersion, "transport-vehicle", "transport-driver", session, 1, utc()}.to_json();
    offer.update({{"sender", "transport-vehicle"}, {"recipient", "transport-driver"}, {"device_token", "fixture-device"},
        {"connection_generation", generation}, {"type", "webrtc_offer"},
        {"payload", {{"type", "offer"}, {"sdp", "v=0\r\na=ice-ufrag:offline\r\n"},
                     {"media_negotiation_id", "media-a"}, {"control_transport_capability", kControlTransportCapability}}}});
    static_cast<void>(http.post_json_response(base + "/signaling/" + session + "/messages", offer));
    static_cast<void>(driver->poll_signaling());
    input();
  }
  ~Fixture() { driver.reset(); vehicle_socket.close(); server->stop(); vehicle->close(); }
  void input() {
    const auto status = driver->status();
    auto result = driver->update_control_intent({{"session_id", session}, {"session_generation", status.at("control_session_generation")},
        {"ui_instance_id", "owner"}, {"ui_control_epoch", ui_epoch}, {"takeover", ui_epoch == 0},
        {"intent_seq", ++intent_seq}, {"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0},
        {"estop", false}, {"critical_video_ready", true}, {"critical_video_sample_healthy", true},
        {"critical_video_bad_sample_revision", std::uint64_t{0}}, {"critical_video_brake_revision", std::uint64_t{0}}});
    expect(result.at("accepted").get<bool>(), "native neutral lease update rejected");
    ui_epoch = result.at("ui_control_epoch").get<std::uint64_t>(); last_input = steady();
  }
  Json scope(bool ready) {
    const auto status = driver->status();
    return {{"source_session_id", session}, {"source_control_session_generation", status.at("control_session_generation")},
        {"media_negotiation_id", "media-a"}, {"ui_instance_id", "owner"}, {"ui_control_epoch", ui_epoch},
        {"dc_ready", ready}, {"last_seen_seq", last_dc_seq}};
  }
  Json apply(const Json& payload, ControlPath path) {
    if (payload.contains("control_transport")) expect(verify_transport_message(payload, signing_key, false), "native byte authentication failed");
    const auto command = ControlCommand::from_json(payload);
    const auto decision = fence.admit(command, payload.contains("control_transport") ? &payload.at("control_transport") : nullptr,
        path, "media-a", utc(), utc() + 10000, 200, true);
    ReceiveResult applied;
    if (decision.admitted) {
      if (decision.changed) vehicle->set_control_braking_wait(true, utc(), ControlRecoveryCause::ControlLink);
      applied = vehicle->receive_command(command, utc());
    }
    Json result = {{"event", "vehicle_control_result"}, {"protocol", kControlTransportCapability},
        {"session_id", session}, {"vehicle_id", "transport-vehicle"}, {"driver_id", "transport-driver"},
        {"command_seq", command.seq}, {"intent_seq", payload.at("intent_seq")}, {"result_seq", ++result_seq},
        {"accepted", decision.admitted && applied.accepted}, {"rearm_required", false}, {"control_status_seq", 0},
        {"transport_admitted", decision.admitted}, {"transport_selected", decision.admitted && payload.contains("control_transport") &&
            payload.at("control_transport").at("action") == "select"}, {"authority_epoch", fence.epoch()},
        {"reason", decision.admitted ? applied.reason : std::string(decision.reason)}};
    if (payload.contains("control_transport")) result["control_transport"] = payload.at("control_transport");
    result["vehicle_mac"] = transport_message_mac(result, signing_key, true);
    return result;
  }
  void pump(bool dc_ready) {
    if (steady() - last_input >= 60) input();
    const auto received = vehicle_socket.receive_json(std::chrono::milliseconds(1));
    if (received.status == WebSocketReceiveStatus::Message && received.message.value("event", "") == "signaling_messages") {
      const auto& messages = received.message.at("messages");
      const auto& payload = messages.back().at("payload");
      ++wss_commands;
      auto result = apply(payload, ControlPath::Wss);
      vehicle_socket.send_json({{"event", "signaling_delivery_ack"}, {"delivery_cursor", received.message.at("delivery_cursor")},
          {"trace_session_id", session}, {"seq", payload.at("seq")}, {"intent_seq", payload.at("intent_seq")}});
      // Model the reported distinction: cloud ingress remains fast while the
      // actual vehicle result is slower than the simulated DC byte path.
      delayed.emplace_back(steady() + 60, std::move(result));
    }
    while (!delayed.empty() && delayed.front().first <= steady()) {
      vehicle_socket.send_json(delayed.front().second); delayed.pop_front();
    }
    auto request = scope(dc_ready);
    auto response = driver->control_transport_bridge(request);
    for (const auto& status : response.at("statuses")) received_statuses.push_back(status);
    if (dc_ready && response.at("frame_json").is_string()) {
      const auto payload = Json::parse(response.at("frame_json").get<std::string>());
      expect(payload.at("seq").get<std::uint64_t>() > last_dc_seq, "bridge replayed a frame");
      last_dc_seq = payload.at("seq").get<std::uint64_t>(); ++dc_commands;
      request = scope(true); request["vehicle_result"] = apply(payload, ControlPath::DataChannel);
      static_cast<void>(driver->control_transport_bridge(request));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
};
void integration() {
  Fixture fixture;
  const auto deadline = steady() + 10000;
  bool qualified = false;
  while (steady() < deadline) {
    fixture.pump(true);
    const auto state = fixture.driver->status().at("native_control").at("control_transport");
    if (state.at("path") == "dc" && !state.at("selecting").get<bool>() && !state.at("qualifying").get<bool>()) { qualified = true; break; }
  }
  expect(qualified && fixture.dc_commands >= 3 && fixture.wss_commands >= 3, "real native producer did not qualify the faster simulated DC path");
  expect(fixture.driver->status().dump().find(fixture.signing_key) == std::string::npos, "native status exposed signing material");
  auto retired = fixture.scope(true); retired["ui_control_epoch"] = 0;
  bool rejected = false; try { static_cast<void>(fixture.driver->control_transport_bridge(retired)); } catch (const std::exception&) { rejected = true; }
  expect(rejected, "retired page accessed native control bytes");
  const auto fallback_deadline = steady() + 4000;
  bool fallback = false;
  while (steady() < fallback_deadline) {
    fixture.pump(false);
    const auto state = fixture.driver->status().at("native_control").at("control_transport");
    if (state.at("path") == "wss" && state.at("epoch").get<std::uint64_t>() >= 2 && !state.at("selecting").get<bool>()) { fallback = true; break; }
  }
  expect(fallback && fixture.fence.path() == ControlPath::Wss, "real native producer failed the same-session WSS fallback barrier");
  std::uint64_t status_sequence = 0;
  for (const auto event : {"vehicle_telemetry", "vcu_handshake_status", "session_control_profile_status", "control_command_rejected"}) {
    fixture.vehicle_socket.send_json({{"event", "vehicle_control_status"}, {"protocol", kControlTransportCapability},
        {"session_id", fixture.session}, {"vehicle_id", "transport-vehicle"}, {"driver_id", "transport-driver"},
        {"status", {{"event", event}, {"control_status_seq", ++status_sequence}}}});
  }
  const auto status_deadline = steady() + 2000;
  while (steady() < status_deadline && fixture.received_statuses.size() < 4) fixture.pump(false);
  expect(fixture.received_statuses.size() == 4, "WSS fallback did not carry all four status kinds to the scoped UI courier");
  for (std::size_t index = 0; index < fixture.received_statuses.size(); ++index) {
    expect(fixture.received_statuses[index].at("control_status_seq").get<std::uint64_t>() == index + 1,
        "WSS status return reordered or duplicated vehicle status sequences");
    expect(fixture.received_statuses[index].at("session_id") == fixture.session, "WSS status lost session scope");
  }
}
}  // namespace
int main() {
  try { integration(); std::cout << "Native/cloud/scoped-byte-courier/shared-service integration passed; loopback only, no WebRTC or hardware proof\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
