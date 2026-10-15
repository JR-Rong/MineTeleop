#include "mine_teleop/server.hpp"
#include "mine_teleop/websocket.hpp"
#include "control_time_fixture.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
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
  DirectAuthorityLease car_authority;
  EmergencyStopReceiver emergency_receiver;
  std::mutex car_mutex;
  std::jthread car_watchdog;
  std::filesystem::path authority_directory;
  bool direct_enabled{false}, cloud_online{true};
  std::uint64_t lease_nonce{0};
  std::string base, session, control_token, signing_key;
  std::uint64_t generation{0}, native_generation{0}, ui_epoch{0}, intent_seq{0}, result_seq{0}, last_dc_seq{0};
  std::uint64_t wss_commands{0}, dc_commands{0};
  std::int64_t last_input{0};
  std::deque<std::pair<std::int64_t, Json>> delayed;
  std::vector<Json> received_statuses;
  explicit Fixture(bool direct = false) : direct_enabled(direct) {
    SignalingServerConfig config;
    if (direct_enabled) {
      authority_directory = std::filesystem::temp_directory_path() / ("mine-direct-loopback-" + std::to_string(steady()));
      std::filesystem::create_directory(authority_directory);
      config.experimental_direct_authority_lease_ms = 600; // REVIEW fixture only; all product defaults remain disabled.
      config.experimental_direct_authority_journal = authority_directory / "authority.json";
    }
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
    driver_config.enable_experimental_direct_authority = direct_enabled;
    driver = std::make_unique<DriverConsoleRuntime>(driver_config, "transport-vehicle", "fixture-password");
    const auto connected = driver->connect("transport-vehicle", "owner");
    expect(!connected.contains("transport_mac_key"), "local UI connect response exposed signing material");
    session = connected.at("session_id").get<std::string>();
    native_generation = connected.at("control_session_generation").get<std::uint64_t>();
    const auto discovered = http.get_json(base + "/vehicles/transport-vehicle/session?connection_generation=" + std::to_string(generation),
        {{"X-Mine-Teleop-Device-Token", "fixture-device"}});
    control_token = discovered.at("control_token").get<std::string>();
    signing_key = discovered.at("transport_mac_key").get<std::string>();
    if (direct_enabled) {
      const auto& authority = discovered.at("direct_authority_configuration");
      car_authority.configure(authority.at("issuer").get<std::string>(), "transport-vehicle", "transport-driver", session, generation, 600);
    }
    auto config_vehicle = load_vehicle_config("configs/vehicle-agent.dev.yaml"); config_vehicle.vehicle_id = "transport-vehicle";
    vehicle = std::make_unique<test::LogicalTimeControlService>(config_vehicle, "transport-driver", session,
        control_token, std::make_unique<MockVehicleAdapter>());
    vehicle->start(utc());
    SessionControlProfileRequest profile; profile.vehicle_id = "transport-vehicle"; profile.driver_id = "transport-driver";
    profile.session_id = session; profile.control_token = control_token; profile.seq = 1; profile.sent_at_utc_ms = utc();
    expect(vehicle->receive_session_profile(profile, utc()).accepted, "offline vehicle profile rejected");
    fence.reset("transport-vehicle", "transport-driver", session, control_token);
    emergency_receiver.reset("transport-vehicle", "transport-driver", session, control_token);
    vehicle_socket.connect(signaling_websocket_url(base, session, "transport-vehicle", std::to_string(generation)) +
        "&types=control_command&transport_protocol=" + std::string(kControlTransportCapability),
        {{"X-Mine-Teleop-Device-Token", "fixture-device"}});
    auto offer = ProtocolMetadata{kProtocolVersion, "transport-vehicle", "transport-driver", session, 1, utc()}.to_json();
    offer.update({{"sender", "transport-vehicle"}, {"recipient", "transport-driver"}, {"device_token", "fixture-device"},
        {"connection_generation", generation}, {"type", "webrtc_offer"},
        {"payload", {{"type", "offer"}, {"sdp", "v=0\r\na=ice-ufrag:offline\r\n"},
                     {"media_negotiation_id", "media-a"}, {"control_transport_capability", kControlTransportCapability},
                     {"emergency_stop_capability", direct_enabled ? kEmergencyStopCapability : std::string_view{}}}}});
    static_cast<void>(http.post_json_response(base + "/signaling/" + session + "/messages", offer));
    static_cast<void>(driver->poll_signaling());
    input();
    if (direct_enabled) {
      renew_authority(true);
      car_watchdog = std::jthread([this](std::stop_token token) {
        while (!token.stop_requested()) {
          { std::lock_guard lock(car_mutex);
            if (car_authority.observe_expiry(steady())) vehicle->set_control_braking_wait(true, utc(), ControlRecoveryCause::ControlLink);
            vehicle->tick(utc()); emergency_receiver.observe_latched(vehicle->safety_state() == SafetyState::Estop); }
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
      });
    }
  }
  ~Fixture() {
    driver.reset(); car_watchdog.request_stop(); if (car_watchdog.joinable()) car_watchdog.join();
    vehicle_socket.close(); server->stop(); vehicle->close(); cloud.reset();
    if (!authority_directory.empty()) std::filesystem::remove_all(authority_directory);
  }
  void renew_authority(bool force = false) {
    if (!direct_enabled || !cloud_online) return;
    const auto nonce = "fixture-nonce-" + std::to_string(++lease_nonce);
    std::uint64_t retired = 0;
    { std::lock_guard lock(car_mutex);
      if (!force && !car_authority.renewal_due(steady())) return;
      if (!car_authority.valid(steady()) && car_authority.reference().is_object()) retired = car_authority.reference().at("authority_epoch").get<std::uint64_t>();
      car_authority.begin_request(nonce, steady()); }
    const auto grant = http.post_json_response(base + "/vehicles/transport-vehicle/direct-authority", {
        {"device_token", "fixture-device"}, {"connection_generation", generation}, {"session_id", session},
        {"nonce", nonce}, {"maximum_duration_ms", 600}, {"retired_authority_epoch", retired}});
    std::lock_guard lock(car_mutex);
    expect(car_authority.accept_authenticated_cloud(grant, steady(), utc(), 0), "vehicle did not accept real cloud short grant");
  }
  void stop_cloud() { cloud_online = false; server->stop(); vehicle_socket.close(); delayed.clear(); }
  void restore_cloud() {
    const auto port = server->port();
    server = std::make_unique<SimpleHttpServer>("127.0.0.1", port,
        [this](const HttpRequest& request) { return cloud->handle(request); }, 8 * 1024 * 1024,
        [this](SocketHandle socket, const HttpRequest& request) { return cloud->handle_websocket(socket, request); });
    server->start();
    vehicle_socket.connect(signaling_websocket_url(base, session, "transport-vehicle", std::to_string(generation)) +
        "&types=control_command&transport_protocol=" + std::string(kControlTransportCapability),
        {{"X-Mine-Teleop-Device-Token", "fixture-device"}});
    cloud_online = true;
  }
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
        {"dc_ready", ready}, {"dc_network_path", direct_enabled ? "p2p" : "unknown"}, {"last_seen_seq", last_dc_seq}};
  }
  Json apply(const Json& payload, ControlPath path) {
    std::lock_guard lock(car_mutex);
    if (payload.contains("control_transport")) expect(verify_transport_message(payload, signing_key, false), "native byte authentication failed");
    const auto command = ControlCommand::from_json(payload);
    bool verified_stop = false;
    if (payload.contains("emergency_stop")) {
      emergency_receiver.observe_latched(vehicle->safety_state() == SafetyState::Estop);
      const auto stop = emergency_receiver.inspect(payload, signing_key, car_authority.reference(), "media-a", path == ControlPath::DataChannel, utc(), 200);
      if (stop.action == EmergencyStopReceiver::Action::Cached) return stop.cached;
      expect(stop.action == EmergencyStopReceiver::Action::ApplyOnce, "fixture production emergency receiver rejected valid stop");
      verified_stop = true;
    }
    ControlTransportFence::Decision decision{false, false, "direct_authority_unavailable"};
    if (!direct_enabled || command.estop || car_authority.allows(payload.value("direct_authority", Json(nullptr)), steady()))
      decision = fence.admit(command, payload.contains("control_transport") ? &payload.at("control_transport") : nullptr,
          path, "media-a", utc(), utc() + 10000, 200, !direct_enabled || car_authority.valid(steady()), verified_stop);
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
    if (direct_enabled) {
      result["direct_authority"] = car_authority.status(steady());
      result["emergency_reset_epoch"] = emergency_receiver.reset_epoch();
      result["command_received_path"] = control_path_name(path);
    }
    if (verified_stop) {
      emergency_receiver.observe_latched(vehicle->safety_state() == SafetyState::Estop);
      result["emergency_stop"] = payload.at("emergency_stop");
      result["software_estop_latched"] = vehicle->safety_state() == SafetyState::Estop;
    }
    result["vehicle_mac"] = transport_message_mac(result, signing_key, true);
    if (verified_stop) expect(emergency_receiver.complete(payload, result), "production emergency cache did not commit result");
    return result;
  }
  void pump(bool dc_ready) {
    renew_authority();
    if (steady() - last_input >= 60) input();
    const auto received = cloud_online ? vehicle_socket.receive_json(std::chrono::milliseconds(1)) : WebSocketReceiveResult{};
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
    while (cloud_online && !delayed.empty() && delayed.front().first <= steady()) {
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

void actual_native_short_cloud_outage_and_independent_vehicle_expiry() {
  Fixture fixture(true);
  const auto ready_deadline = steady() + 8000;
  bool ready = false;
  while (steady() < ready_deadline) {
    fixture.pump(true);
    const auto authority = fixture.driver->status().at("native_control").at("control_transport").at("direct_authority");
    if (authority.value("continuation_available", false)) { ready = true; break; }
  }
  expect(ready, "real native producer never established a healthy short-authorized direct continuation");
  fixture.renew_authority(true);
  const auto refresh_deadline = steady() + 1000;
  std::int64_t remaining = 0;
  while (steady() < refresh_deadline) {
    fixture.pump(true);
    const auto authority = fixture.driver->status().at("native_control").at("control_transport").at("direct_authority");
    remaining = authority.value("remaining_ms", std::int64_t{0});
    if (authority.value("continuation_available", false) && remaining > 400) break;
  }
  expect(remaining > 400, "fixture did not observe a fresh vehicle-accepted grant before outage");
  const auto before = fixture.dc_commands;
  fixture.stop_cloud(); // Actual localhost HTTP/WSS sockets close, not just a boolean in the sender.
  const auto outage_deadline = steady() + 100;
  while (steady() < outage_deadline) fixture.pump(true);
  const auto during = fixture.driver->status().at("native_control").at("control_transport");
  expect(fixture.dc_commands > before && during.at("path") == "dc" && during.at("direct_authority").value("valid", false),
      "healthy direct command/result stream stopped solely because cloud sockets closed inside the accepted lease");
  fixture.restore_cloud(); fixture.renew_authority(true);
  const auto recovered_deadline = steady() + 1000;
  while (steady() < recovered_deadline) {
    fixture.pump(true);
    if (fixture.driver->status().at("native_control").at("control_transport").at("direct_authority").value("remaining_ms", 0) > 400) break;
  }
  expect(fixture.driver->status().at("native_control").at("control_transport").at("path") == "dc", "cloud recovery displaced a healthy preferred direct path");
  fixture.stop_cloud();
  // No pump/status dependency here: native may be stuck reconnecting to dead
  // WSS. The vehicle expiry/watchdog runs on its own thread as in production.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  { std::lock_guard lock(fixture.car_mutex);
    expect(!fixture.car_authority.valid(steady()) && (fixture.vehicle->control_braking_wait() || fixture.vehicle->safety_state() == SafetyState::TimeoutBrake),
        "cloud silence left vehicle authorization/motion live past independent deadline"); }
}

void emergency_broker_delivers_after_ordinary_wss_is_already_blocked() {
  std::atomic<bool> wss_waiting{false}, wss_finished{false}; // Outlive the fixture/server callbacks.
  Fixture fixture(true);
  const auto ready_deadline = steady() + 8000;
  bool ready = false;
  while (steady() < ready_deadline) {
    fixture.pump(true);
    if (fixture.driver->status().at("native_control").at("control_transport").at("direct_authority").value("continuation_available", false)) { ready = true; break; }
  }
  expect(ready, "emergency fixture never reached qualified direct state");
  fixture.renew_authority(true);
  const auto refresh_deadline = steady() + 1000;
  while (steady() < refresh_deadline) {
    fixture.pump(true);
    if (fixture.driver->status().at("native_control").at("control_transport").at("direct_authority").value("remaining_ms", 0) > 400) break;
  }
  auto unavailable = fixture.scope(false);
  static_cast<void>(fixture.driver->control_transport_bridge(unavailable));
  fixture.stop_cloud();
  const auto port = fixture.server->port();
  fixture.server = std::make_unique<SimpleHttpServer>("127.0.0.1", port,
      [&fixture](const HttpRequest& request) { return fixture.cloud->handle(request); }, 8 * 1024 * 1024,
      [&fixture, &wss_waiting, &wss_finished](SocketHandle socket, const HttpRequest& request) {
        const auto send_only = request.query.find("send_only");
        if (send_only != request.query.end() && send_only->second == "1") {
          wss_waiting.store(true); std::this_thread::sleep_for(std::chrono::milliseconds(700));
          wss_finished.store(true); return false;
        }
        return fixture.cloud->handle_websocket(socket, request);
      });
  fixture.server->start(); // Only the old localhost fixture port; no vehicle/cloud deployment.
  const auto press_stop = [&] {
    return fixture.driver->update_control_intent({{"session_id", fixture.session}, {"session_generation", fixture.native_generation},
        {"ui_instance_id", "owner"}, {"ui_control_epoch", fixture.ui_epoch}, {"takeover", false}, {"intent_seq", ++fixture.intent_seq},
        {"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}, {"estop", true},
        {"critical_video_ready", true}, {"critical_video_sample_healthy", true},
        {"critical_video_bad_sample_revision", std::uint64_t{0}}, {"critical_video_brake_revision", std::uint64_t{0}}});
  };
  const auto started = steady();
  while (!wss_waiting.load() && steady() - started < 2000) std::this_thread::sleep_for(std::chrono::milliseconds(5));
  expect(wss_waiting.load() && !wss_finished.load(), "fixture failed to block ordinary WSS before STOP exists");
  std::atomic<bool> bridge_entered{false}, bridge_done{false};
  std::jthread blocked_ordinary_bridge([&] {
    bridge_entered.store(true);
    try { static_cast<void>(fixture.driver->control_transport_bridge(unavailable)); } catch (...) {}
    bridge_done.store(true);
  });
  while (!bridge_entered.load()) std::this_thread::yield();
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(!bridge_done.load() && !wss_finished.load(), "fixture did not also block ordinary courier on WSS");
  const auto accepted = press_stop(); // First STOP happens strictly AFTER ordinary WSS entered its stall.
  expect(accepted.at("accepted") == true && !wss_finished.load(), "native STOP latch waited for the blocked ordinary WSS call");
  Json request = {{"source_session_id", fixture.session}, {"source_control_session_generation", fixture.native_generation},
      {"media_negotiation_id", "media-a"}, {"ui_instance_id", "owner"}, {"ui_control_epoch", fixture.ui_epoch},
      {"dc_ready", true}, {"last_seen_seq", 0}};
  const auto response = fixture.driver->emergency_stop_bridge(request); // Actual native endpoint implementation, no normal bridge mutex.
  expect(response.at("frame_json").is_string(), "blocked WSS prevented fetching already-published emergency bytes");
  const auto payload = Json::parse(response.at("frame_json").get<std::string>());
  expect(payload.at("estop") == true && payload.at("brake") == 1.0 && verify_transport_message(payload, fixture.signing_key, false), "native emergency bytes were forged or not stop-only");
  const auto result = fixture.apply(payload, ControlPath::DataChannel); // Production MAC/stop receiver + real common safety service.
  request["last_seen_seq"] = payload.at("seq"); request["vehicle_result"] = result;
  const auto confirmed = fixture.driver->emergency_stop_bridge(request);
  expect(confirmed.at("software_latched_confirmed") == true && confirmed.at("physical_execution_confirmed") == false &&
      confirmed.at("command_received_path") == "dc" && confirmed.at("feedback_path") == "dc" && !wss_finished.load(),
      "software stop confirmation waited for WSS or mislabeled actual delivery/physical state");
  auto tampered = result; tampered["software_estop_latched"] = false; request["vehicle_result"] = tampered;
  bool rejected = false; try { static_cast<void>(fixture.driver->emergency_stop_bridge(request)); } catch (const std::invalid_argument&) { rejected = true; }
  expect(rejected, "page could forge an emergency software result");
}
}  // namespace
int main() {
  try { integration(); actual_native_short_cloud_outage_and_independent_vehicle_expiry(); emergency_broker_delivers_after_ordinary_wss_is_already_blocked(); std::cout << "Native/cloud/scoped-byte-courier/shared-service integration passed, including cloud outage, short authority and blocked-WSS emergency; loopback only, no WebRTC or hardware proof\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
