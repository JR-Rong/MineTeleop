#include "mine_teleop/server.hpp"
#include "mine_teleop/websocket.hpp"
#include "mine_teleop/detail/native_control_publication.hpp"
#include "mine_teleop/detail/native_control_courier.hpp"
#include "mine_teleop/detail/native_video_health.hpp"
#include "control_time_fixture.hpp"

#include <chrono>
#include <deque>
#include <filesystem>
#include <future>
#include <iostream>
#include <thread>

namespace mine_teleop {
// Inspect an unfetched native publication and inject a recovery observation
// while holding the real transport mutex. Sending/fallback/bridge stay real.
struct DriverConsoleTransportTestAccess {
  static bool status_does_not_wait_for_signaling(DriverConsoleRuntime& runtime) {
    std::unique_lock lock(runtime.signaling_websocket_mutex_);
    auto result = std::async(std::launch::async, [&] { return runtime.status(true); });
    const bool timely = result.wait_for(std::chrono::seconds(1)) == std::future_status::ready;
    lock.unlock();
    const auto value = result.get();
    return timely && value.at("signaling_socket_snapshot_available") == false &&
        value.at("signaling_websocket_connected").is_null() && value.contains("time_sync") &&
        value.at("native_control").contains("control_transport");
  }
  static Json withdraw_with_pending_motion(DriverConsoleRuntime& runtime, int cause) {
    std::lock_guard lock(runtime.control_signaling_websocket_mutex_);
    const auto frame = runtime.control_transport_latest_dc_;
    if (!frame.is_object() || frame.value("throttle", 0.0) <= 0.0) return nullptr;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    if (cause == 0) runtime.control_signaling_ack_recovery_.enter(now, 5000, runtime.control_signaling_last_ack_seq_);
    else if (cause == 1) runtime.control_dc_ack_recovery_.enter(now, 5000, runtime.control_dc_last_ack_seq_);
    else if (cause == 2) runtime.control_transport_dc_ready_ = false;
    else {
      runtime.control_signaling_ack_window_.reset();
      runtime.control_signaling_ack_window_.note_sent(runtime.native_control_last_seq_.load() + 1, now - 501);
    }
    return {{"frame", frame}, {"published_at", runtime.control_transport_latest_dc_at_ms_}};
  }
  static bool retired(DriverConsoleRuntime& runtime) {
    std::lock_guard lock(runtime.control_signaling_websocket_mutex_);
    return runtime.control_transport_sender_.path() == detail::ControlPath::Wss &&
        runtime.control_transport_latest_dc_.is_null() && runtime.control_dc_last_ack_seq_ == 0 &&
        runtime.control_dc_ack_window_.pending_count() == 0 && !runtime.control_dc_ack_recovery_.waiting();
  }
};
}

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
std::int64_t utc() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
std::int64_t steady() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void expect(bool value, std::string_view message) { if (!value) throw std::runtime_error(std::string(message)); }

void courier_status_does_not_wait_for_signaling_poll() {
  DriverConfig config;
  config.driver_id = "status-probe";
  config.signaling_url = "http://127.0.0.1:1";
  config.browser_event_log_path.clear();
  DriverConsoleRuntime driver(config, "test_bench_status", "fixture");
  expect(DriverConsoleTransportTestAccess::status_does_not_wait_for_signaling(driver),
      "courier status waited for signaling poll or exposed an unavailable socket snapshot");
}

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
  FeedbackTransportFence feedback_fence;
  bool feedback_enabled{false}, drop_dc_feedback{false}, inject_candidate_notification{false}, candidate_notification_sent{false};
  Json pending_dc_feedback, last_feedback_command, last_received_payload, lost_initial_feedback;
  Json first_vehicle_rejection;
  bool drop_initial_feedback{false}, pause_after_feedback_loss{false};
  std::string media_attempt{"media-a"};
  std::int64_t car_control_lease_expiry{};
  std::uint64_t feedback_status_sequence{};
  std::mutex car_mutex;
  std::jthread car_watchdog;
  std::filesystem::path authority_directory;
  bool direct_enabled{false}, cloud_online{true};
  bool refresh_input{true}, video_ready{true}, video_sample_healthy{true};
  std::uint64_t video_bad_revision{0}, video_brake_revision{0};
  NativeVideoHealth* synthetic_video_health{nullptr};
  std::string dc_network_path{"unknown"}, last_result_reason;
  double requested_throttle{0.0};
  std::uint64_t lease_nonce{0};
  std::string base, session, control_token, signing_key;
  std::uint64_t generation{0}, native_generation{0}, ui_epoch{0}, intent_seq{0}, result_seq{0}, last_dc_seq{0};
  std::uint64_t wss_commands{0}, dc_commands{0};
  std::int64_t last_input{0};
  std::deque<std::pair<std::int64_t, Json>> delayed;
  std::vector<Json> received_statuses;
  bool use_native_publication{false};
  std::unique_ptr<NativeControlPublication> native_publication;
  bool use_native_courier{false};
  std::unique_ptr<NativeControlCourier> native_courier;
  std::deque<std::string> courier_receipts;
  explicit Fixture(bool direct = false, bool feedback = false, bool product = false,
      std::string mode = "p2p_preferred") : feedback_enabled(feedback), direct_enabled(direct) {
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
    auto driver_config = product ? load_driver_config("configs/driver-console.dev.yaml") : DriverConfig{};
    driver_config.control_transport_mode = std::move(mode);
    driver_config.browser_event_log_path.clear();
    driver_config.enable_experimental_control_transport = !product;
    driver_config.driver_id = "transport-driver"; driver_config.signaling_url = base;
    dc_network_path = direct_enabled ? "p2p" : "unknown";
    driver_config.enable_experimental_direct_authority = direct_enabled;
    driver = std::make_unique<DriverConsoleRuntime>(driver_config, "transport-vehicle", "fixture-password");
    const auto connected = driver->connect("transport-vehicle", "owner");
    expect(!connected.contains("transport_mac_key"), "local UI connect response exposed signing material");
    session = connected.at("session_id").get<std::string>();
    native_generation = connected.at("control_session_generation").get<std::uint64_t>();
    const auto discovered = http.get_json(base + "/vehicles/transport-vehicle/session?connection_generation=" + std::to_string(generation),
        {{"X-Mine-Teleop-Device-Token", "fixture-device"}});
    control_token = discovered.at("control_token").get<std::string>();
    car_control_lease_expiry = discovered.at("control_token_expires_at_utc_ms").get<std::int64_t>();
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
                     {"media_negotiation_id", media_attempt}, {"control_transport_capability", kControlTransportCapability},
                     {"emergency_stop_capability", direct_enabled ? kEmergencyStopCapability : std::string_view{}},
                     {"feedback_transport_capability", feedback_enabled ? kFeedbackTransportCapability : std::string_view{}}}}});
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
    Json previous_authority;
    std::int64_t request_started_ms = 0;
    { std::lock_guard lock(car_mutex);
      if (!force && !car_authority.renewal_due(steady())) return;
      if (!car_authority.valid(steady()) && car_authority.reference().is_object()) retired = car_authority.reference().at("authority_epoch").get<std::uint64_t>();
      request_started_ms = steady();
      previous_authority = car_authority.status(request_started_ms);
      car_authority.begin_request(nonce, request_started_ms); }
    const auto grant = http.post_json_response(base + "/vehicles/transport-vehicle/direct-authority", {
        {"device_token", "fixture-device"}, {"connection_generation", generation}, {"session_id", session},
        {"nonce", nonce}, {"maximum_duration_ms", 600}, {"retired_authority_epoch", retired}});
    const auto response_received_ms = steady();
    std::lock_guard lock(car_mutex);
    const auto accepted_at_ms = steady(), accepted_at_utc_ms = utc();
    if (!car_authority.accept_authenticated_cloud(grant, accepted_at_ms, accepted_at_utc_ms, 0)) {
      const auto parsed = DirectAuthorityGrant::parse(grant);
      throw std::runtime_error("vehicle did not accept real cloud short grant: " + Json{
          {"http_elapsed_ms", response_received_ms - request_started_ms},
          {"lock_wait_ms", accepted_at_ms - response_received_ms},
          {"request_elapsed_ms", accepted_at_ms - request_started_ms},
          {"response_duration_ms", parsed.duration_ms},
          {"response_remaining_utc_ms", parsed.expires_utc_ms - accepted_at_utc_ms},
          {"response_epoch", parsed.epoch}, {"response_lease_seq", parsed.lease_seq},
          {"retired_epoch", retired}, {"previous_authority", previous_authority},
          {"current_authority", car_authority.status(accepted_at_ms)}}.dump());
    }
    car_control_lease_expiry = direct_authority_control_lease_expiry(grant);
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
    if (synthetic_video_health) {
      const auto health = synthetic_video_health->snapshot(steady());
      video_ready = health.ready;
      video_sample_healthy = health.sample_healthy;
      video_bad_revision = health.bad_revision;
      video_brake_revision = health.brake_revision;
    }
    auto result = driver->update_control_intent({{"session_id", session}, {"session_generation", status.at("control_session_generation")},
        {"ui_instance_id", "owner"}, {"ui_control_epoch", ui_epoch}, {"takeover", ui_epoch == 0},
        {"intent_seq", ++intent_seq}, {"gear", requested_throttle > 0 ? "D" : "N"}, {"steering", 0.0}, {"throttle", requested_throttle}, {"brake", 0.0},
        {"estop", false}, {"critical_video_ready", video_ready}, {"critical_video_sample_healthy", video_sample_healthy},
        {"critical_video_bad_sample_revision", video_bad_revision}, {"critical_video_brake_revision", video_brake_revision}});
    expect(result.at("accepted").get<bool>() || (requested_throttle > 0 && result.at("requires_fresh_input").get<bool>()), "native input update rejected outside the recovery interlock");
    ui_epoch = result.at("ui_control_epoch").get<std::uint64_t>(); last_input = steady();
  }
  Json scope(bool ready) {
    const auto status = driver->status();
    return {{"source_session_id", session}, {"source_control_session_generation", status.at("control_session_generation")},
        {"media_negotiation_id", media_attempt}, {"ui_instance_id", "owner"}, {"ui_control_epoch", ui_epoch},
        {"dc_ready", ready}, {"dc_network_path", dc_network_path}, {"last_seen_seq", last_dc_seq}};
  }
  Json apply(const Json& payload, ControlPath path) {
    std::lock_guard lock(car_mutex);
    last_received_payload = payload;
    if (payload.contains("control_transport")) expect(verify_transport_message(payload, signing_key, false), "native byte authentication failed");
    const auto command = ControlCommand::from_json(payload);
    bool verified_stop = false;
    if (payload.contains("emergency_stop")) {
      emergency_receiver.observe_latched(vehicle->safety_state() == SafetyState::Estop);
      const auto stop = emergency_receiver.inspect(payload, signing_key, car_authority.reference(), media_attempt, path == ControlPath::DataChannel, utc(), 200);
      if (stop.action == EmergencyStopReceiver::Action::Cached) return stop.cached;
      expect(stop.action == EmergencyStopReceiver::Action::ApplyOnce, "fixture production emergency receiver rejected valid stop");
      verified_stop = true;
    }
    ControlTransportFence::Decision decision{false, false, "direct_authority_unavailable"};
    if (!direct_enabled || command.estop || car_authority.allows(payload.value("direct_authority", Json(nullptr)), steady()))
      decision = fence.admit(command, payload.contains("control_transport") ? &payload.at("control_transport") : nullptr,
          path, media_attempt, utc(), car_control_lease_expiry, 200, direct_enabled ? car_authority.valid(steady()) : cloud_online, verified_stop);
    if (feedback_enabled && decision.admitted && !command.estop && payload.contains("feedback_request")) {
      static_cast<void>(feedback_fence.select(payload.at("feedback_request"), car_authority.reference(), media_attempt, feedback_status_sequence));
      last_feedback_command = payload;
    }
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
    if (feedback_enabled && payload.contains("feedback_request")) {
      result["feedback_request"] = payload.at("feedback_request"); result["feedback_transport"] = feedback_fence.response(payload.at("feedback_request"), car_authority.reference(), media_attempt, feedback_status_sequence);
    }
    if (direct_enabled) {
      result["direct_authority"] = car_authority.status(steady());
      result["emergency_reset_epoch"] = emergency_receiver.reset_epoch();
      result["command_received_path"] = control_path_name(path);
    }
    if (feedback_enabled && feedback_fence.negotiated()) result["feedback_transport"] = feedback_fence.state();
    if (verified_stop) {
      emergency_receiver.observe_latched(vehicle->safety_state() == SafetyState::Estop);
      result["emergency_stop"] = payload.at("emergency_stop");
      result["software_estop_latched"] = vehicle->safety_state() == SafetyState::Estop;
    }
    last_result_reason = result.at("reason").get<std::string>();
    if (!result.at("accepted").get<bool>() && first_vehicle_rejection.is_null()) {
      first_vehicle_rejection = {{"received_at_utc_ms", utc()}, {"command_seq", command.seq},
          {"intent_seq", payload.at("intent_seq")}, {"path", control_path_name(path)},
          {"command_age_ms", utc() - command.sent_at_utc_ms}, {"gear", command.gear},
          {"throttle", command.throttle}, {"reason", last_result_reason},
          {"transport_admitted", decision.admitted}, {"transport_changed", decision.changed},
          {"safety_state", to_string(vehicle->safety_state())},
          {"control_timing", vehicle->control_timing(utc())}};
    }
    result["vehicle_mac"] = transport_message_mac(result, signing_key, true);
    if (verified_stop) expect(emergency_receiver.complete(payload, result), "production emergency cache did not commit result");
    return result;
  }
  void route_feedback(const Json& result, const Json& payload) {
    const bool selected = payload.contains("feedback_request") && feedback_fence.negotiated();
    const bool probe = selected && payload.at("feedback_request").value("probe", false);
    if (inject_candidate_notification && selected && !candidate_notification_sent) {
      auto notification = feedback_status(++feedback_status_sequence, feedback_fence.state(), "control_command_rejected");
      notification["status"]["issue_code"] = "vcu_drive_gear_change_moving_or_stale";
      notification["status"]["command_seq"] = payload.at("seq"); notification["status"]["intent_seq"] = payload.at("intent_seq");
      notification["vehicle_mac"] = transport_message_mac(notification, signing_key, true);
      vehicle_socket.send_json(notification); candidate_notification_sent = true;
    }
    if (!selected || probe || feedback_fence.path() == ControlPath::Wss) delayed.emplace_back(steady() + 60, result);
    if (!drop_dc_feedback && selected && (probe || feedback_fence.path() == ControlPath::DataChannel)) pending_dc_feedback = result;
  }
  Json feedback_status(std::uint64_t sequence, Json stamp, std::string_view kind = "vehicle_telemetry") {
    Json result = {{"event", "vehicle_control_status"}, {"protocol", kControlTransportCapability},
        {"session_id", session}, {"vehicle_id", "transport-vehicle"}, {"driver_id", "transport-driver"},
        {"feedback_transport", std::move(stamp)}, {"status", {{"event", kind}, {"session_id", session},
        {"vehicle_id", "transport-vehicle"}, {"driver_id", "transport-driver"}, {"control_status_seq", sequence}}}};
    result["vehicle_mac"] = transport_message_mac(result, signing_key, true); return result;
  }
  bool drop_before_receiver(const Json& payload) {
    if (drop_initial_feedback && !lost_initial_feedback.is_object() && payload.contains("feedback_request")) {
      lost_initial_feedback = payload; pause_after_feedback_loss = true;
    }
    return pause_after_feedback_loss;
  }
  Json bridge(const Json& request) {
    auto response = driver->control_transport_bridge(request);
    // Every bridge call consumes pending native statuses. WSS feedback can
    // arrive between the fetch and the result upload in the same pump cycle.
    for (const auto& status : response.at("statuses")) received_statuses.push_back(status);
    return response;
  }
  void pump(bool dc_ready) {
    renew_authority();
    if (refresh_input && steady() - last_input >= 60) input();
    const auto received = cloud_online ? vehicle_socket.receive_json(std::chrono::milliseconds(1)) : WebSocketReceiveResult{};
    if (received.status == WebSocketReceiveStatus::Message && received.message.value("event", "") == "signaling_messages") {
      const auto& messages = received.message.at("messages");
      const auto& payload = messages.back().at("payload");
      ++wss_commands;
      const bool dropped = drop_before_receiver(payload);
      auto result = dropped ? Json(nullptr) : apply(payload, ControlPath::Wss);
      vehicle_socket.send_json({{"event", "signaling_delivery_ack"}, {"delivery_cursor", received.message.at("delivery_cursor")},
          {"trace_session_id", session}, {"seq", payload.at("seq")}, {"intent_seq", payload.at("intent_seq")}});
      // Model the reported distinction: cloud ingress remains fast while the
      // actual vehicle result is slower than the simulated DC byte path.
      if (!dropped) { if (feedback_enabled) route_feedback(result, payload); else delayed.emplace_back(steady() + 60, std::move(result)); }
    }
    while (cloud_online && !delayed.empty() && delayed.front().first <= steady()) {
      vehicle_socket.send_json(delayed.front().second); delayed.pop_front();
    }
    if (use_native_courier) {
      const NativeCourierScope current{{session, native_generation, media_attempt}, "owner", ui_epoch};
      if (!native_courier) native_courier = std::make_unique<NativeControlCourier>(current);
      NativeControlCourier::Io io{
          [&]() -> std::optional<std::string> {
            if (courier_receipts.empty()) return {};
            auto bytes = std::move(courier_receipts.front()); courier_receipts.pop_front(); return bytes;
          }, [&] { return dc_ready && dc_network_path == "p2p"; },
          [&](const Json& request) { return driver->control_transport_bridge(request); },
          [&](const std::string& bytes) {
            const auto payload = Json::parse(bytes);
            expect(payload.at("seq").get<std::uint64_t>() > last_dc_seq, "courier replayed a frame");
            last_dc_seq = payload.at("seq").get<std::uint64_t>(); ++dc_commands;
            courier_receipts.push_back(apply(payload, ControlPath::DataChannel).dump());
            return true;
          }, [&](const Json& status) { received_statuses.push_back(status); }};
      expect(native_courier->tick(current, io), "native courier retired unexpectedly");
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      return;
    }
    auto request = scope(dc_ready);
    if (pending_dc_feedback.is_object()) { request["vehicle_result"] = pending_dc_feedback; pending_dc_feedback = Json(nullptr); }
    const auto publication_started = steady();
    auto response = bridge(request);
    if (dc_ready && response.at("frame_json").is_string()) {
      std::string bytes = response.at("frame_json").get<std::string>();
      if (use_native_publication) {
        const NativeCourierScope current{{session, native_generation, media_attempt}, "owner", ui_epoch};
        if (!native_publication) native_publication = std::make_unique<NativeControlPublication>(current);
        const auto filtered = native_publication->take(current, response, dc_ready, steady() - publication_started);
        if (!filtered) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); return; }
        bytes = *filtered;
      }
      const auto payload = Json::parse(bytes);
      expect(payload.at("seq").get<std::uint64_t>() > last_dc_seq, "bridge replayed a frame");
      last_dc_seq = payload.at("seq").get<std::uint64_t>(); ++dc_commands;
      if (drop_before_receiver(payload)) { std::this_thread::sleep_for(std::chrono::milliseconds(5)); return; }
      request = scope(true);
      if (feedback_enabled) {
        route_feedback(apply(payload, ControlPath::DataChannel), payload);
        if (pending_dc_feedback.is_object()) { request["vehicle_result"] = pending_dc_feedback; pending_dc_feedback = Json(nullptr); }
      } else request["vehicle_result"] = apply(payload, ControlPath::DataChannel);
      static_cast<void>(bridge(request));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
};
void native_publication_bridge_integration() {
  Fixture fixture(false, false, true);
  fixture.dc_network_path = "p2p";
  fixture.use_native_publication = true;
  const auto deadline = steady() + 8000;
  while (steady() < deadline) {
    fixture.pump(true);
    const auto transport = fixture.driver->status().at("native_control").at("control_transport");
    if (fixture.dc_commands >= 3 && transport.at("path") == "dc" && !transport.at("selecting").get<bool>()) break;
  }
  expect(fixture.dc_commands >= 3 && fixture.native_publication && fixture.native_publication->last_seen() > 0,
      "native publication filter did not deliver signed bridge frames to the mock vehicle");
  std::cout << "[NATIVE_PUBLICATION] scoped real bridge, signed commands and mock vehicle feedback passed\n";
}
void native_courier_bridge_integration() {
  Fixture fixture(false, false, true);
  fixture.dc_network_path = "p2p";
  fixture.use_native_courier = true;
  const auto deadline = steady() + 8000;
  bool selected = false;
  while (steady() < deadline && !selected) {
    fixture.pump(true);
    const auto transport = fixture.driver->status().at("native_control").at("control_transport");
    selected = fixture.dc_commands >= 3 && transport.at("path") == "dc" && !transport.at("selecting").get<bool>();
  }
  expect(selected && fixture.native_courier && !fixture.native_courier->retired(),
      "native courier did not reconcile signed mock vehicle feedback");
  std::cout << "[NATIVE_COURIER] signed runtime commands and mock vehicle feedback reconciled; byte channel simulated\n";
}
void native_publication_safety_interlocks() {
  for (const bool lose_video : {true, false}) {
    Fixture fixture(false, false, true);
    fixture.dc_network_path = "p2p";
    fixture.use_native_publication = true;
    auto deadline = steady() + 8000;
    bool initially_ready = false, simulated_release = false;
    while (steady() < deadline && !initially_ready) {
      fixture.pump(true);
      std::lock_guard lock(fixture.car_mutex);
      const auto timing = fixture.vehicle->control_timing(utc());
      // This fixture has no VehicleMediaRuntime recovery coordinator. Model
      // its release explicitly and never treat this as a recovery test.
      if (!simulated_release && fixture.dc_commands >= 3 && timing.value("driver_critical_video_ready", false) &&
          timing.value("operator_input_ready", false) && timing.value("driver_time_sync_ready", false) &&
          timing.value("driver_control_link_ready", false)) {
        fixture.vehicle->set_control_braking_wait(false, utc(), ControlRecoveryCause::ControlLink);
        simulated_release = true;
      }
      initially_ready = fixture.dc_commands >= 3 && timing.value("driver_critical_video_ready", false) &&
          timing.value("operator_input_ready", false) && !fixture.vehicle->control_braking_wait();
    }
    if (!initially_ready) {
      std::lock_guard lock(fixture.car_mutex);
      const auto timing = fixture.vehicle->control_timing(utc());
      std::cerr << "mock precondition: dc=" << fixture.dc_commands
                << " video=" << timing.value("driver_critical_video_ready", false)
                << " input=" << timing.value("operator_input_ready", false)
                << " clock=" << timing.value("driver_time_sync_ready", false)
                << " link=" << timing.value("driver_control_link_ready", false)
                << " brake=" << fixture.vehicle->control_braking_wait()
                << " cause=" << timing.value("control_recovery_cause", "") << '\n';
    }
    expect(initially_ready, "native safety fixture did not reach the simulated wait release before fault injection");
    if (lose_video) {
      fixture.video_ready = false;
      fixture.video_sample_healthy = false;
      ++fixture.video_bad_revision;
      ++fixture.video_brake_revision;
      fixture.input();
    } else fixture.refresh_input = false;
    bool stopped = false;
    deadline = steady() + 4000;
    while (steady() < deadline && !stopped) {
      fixture.pump(true);
      std::lock_guard lock(fixture.car_mutex);
      const auto timing = fixture.vehicle->control_timing(utc());
      const bool lost_required_input = lose_video ? !timing.value("driver_critical_video_ready", true)
          : !timing.value("operator_input_ready", true);
      const auto cause = timing.value("control_recovery_cause", "");
      stopped = lost_required_input && fixture.vehicle->control_braking_wait() &&
          (lose_video ? (cause == "critical_video" || cause == "readiness_history") : cause == "input_lease");
    }
    expect(stopped, lose_video ? "mock vehicle did not brake after reported critical video loss"
                              : "mock vehicle did not brake after the input lease expired");
  }
  std::cout << "[NATIVE_SAFETY] reported video loss and input lease expiry reached mock braking; simulated courier, video health and initial release, no recovery proof\n";
}
void native_dual_video_bridge_integration() {
  NativeVideoHealth video({"front", "rear"}, 500);
  expect(video.attach("front", 1) && video.attach("rear", 1) &&
      video.set_usable("front", 1, true) && video.set_usable("rear", 1, true), "video fixture setup failed");
  Fixture fixture(false, false, true);
  fixture.dc_network_path = "p2p";
  fixture.use_native_publication = true;
  fixture.synthetic_video_health = &video;
  std::uint64_t seq = 0;
  auto present = [&](bool rear) {
    const auto now = steady();
    ++seq;
    expect(video.render_submitted("front", 1, seq, now), "front synthetic presentation rejected");
    if (rear) expect(video.render_submitted("rear", 1, seq, now), "rear synthetic presentation rejected");
  };
  bool initially_ready = false, simulated_release = false;
  auto deadline = steady() + 8000;
  while (steady() < deadline && !initially_ready) {
    present(true); fixture.pump(true);
    std::lock_guard lock(fixture.car_mutex);
    const auto timing = fixture.vehicle->control_timing(utc());
    if (!simulated_release && fixture.dc_commands >= 3 && timing.value("driver_critical_video_ready", false) &&
        timing.value("operator_input_ready", false) && timing.value("driver_time_sync_ready", false) &&
        timing.value("driver_control_link_ready", false)) {
      fixture.vehicle->set_control_braking_wait(false, utc(), ControlRecoveryCause::ControlLink);
      simulated_release = true;
    }
    initially_ready = simulated_release && !fixture.vehicle->control_braking_wait();
  }
  expect(initially_ready && video.snapshot(steady()).ready, "dual video fixture did not reach its simulated baseline");
  bool stopped = false;
  deadline = steady() + 4000;
  while (steady() < deadline && !stopped) {
    present(false); fixture.pump(true);
    std::lock_guard lock(fixture.car_mutex);
    const auto timing = fixture.vehicle->control_timing(utc());
    stopped = !timing.value("driver_critical_video_ready", true) && fixture.vehicle->control_braking_wait();
  }
  expect(stopped, "one stalled synthetic track did not revoke mock control");
  expect(!video.snapshot(steady()).ready && video.snapshot(steady()).brake_revision > 0,
      "stalled video did not retain a brake revision");
  std::cout << "[NATIVE_DUAL_VIDEO] one synthetic track stall revoked mock control; simulated presentation and courier, no recovery proof\n";
}
void product_recovery_withdraws_unfetched_motion() {
  for (int cause = 0; cause < 4; ++cause) {
    Fixture f(false, false, true); f.dc_network_path = "p2p";
    auto transport = [&] { return f.driver->status().at("native_control").at("control_transport"); };
    auto deadline = steady() + 8000;
    while (steady() < deadline) {
      f.pump(true);
      if (transport().at("path") == "dc" && !transport().at("qualifying").get<bool>() &&
          !transport().at("selecting").get<bool>()) break;
    }
    expect(transport().at("path") == "dc" && !transport().at("qualifying").get<bool>(), "DC not qualified");
    Json pending;
    // A qualified status can precede a sender-thread route-boundary rearm.
    // Re-establish neutral with live receipts before each bounded attempt.
    // If the motion sample races that rearm, resume pumping before the actual
    // ACK watchdog expires and try a fresh release/press; never retimestamp a
    // previously published command or fetch the frame being tested.
    for (int attempt = 0; attempt < 3 && pending.is_null(); ++attempt) {
      f.requested_throttle = 0.0; f.input();
      deadline = steady() + 150;
      while (steady() < deadline) f.pump(true);
      f.input(); f.requested_throttle = 0.4; f.input();
      deadline = steady() + 120;
      while (steady() < deadline && pending.is_null()) {
        pending = DriverConsoleTransportTestAccess::withdraw_with_pending_motion(*f.driver, cause);
        if (pending.is_null()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    expect(pending.is_object(), "no unfetched nonzero native publication; cause=" + std::to_string(cause) +
        "; native=" + f.driver->status().at("native_control").dump());
    expect(pending.at("frame").at("seq").get<std::uint64_t>() > f.last_dc_seq, "pending frame was already consumed");
    deadline = steady() + 80;
    while (steady() < deadline && transport().at("path") == "dc") std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const auto withdrawn = transport();
    expect(withdrawn.at("path") == "wss", "recovery did not withdraw DC; cause=" +
        std::to_string(cause) + "; state=" + withdrawn.dump());
    // No WSS select has been delivered to the vehicle. Neither expiry nor
    // replay rejection may hide a stale publication at this boundary.
    expect(steady() - pending.at("published_at").get<std::int64_t>() < kNativeBridgePublicationMaxAgeMs,
        "test missed the live publication window");
    expect(f.fence.path() == ControlPath::DataChannel, "vehicle already received fallback select");
    expect(f.bridge(f.scope(true)).at("frame_json").is_null(), "bridge returned retired nonzero DC publication");
    expect(DriverConsoleTransportTestAccess::retired(*f.driver), "fallback retained DC publication or ACK state");
  }
}

void product_wss_reconnect_retires_dc_and_recovers_ack() {
  Fixture fixture(false, false, true);
  fixture.dc_network_path = "p2p";
  auto transport = [&] { return fixture.driver->status().at("native_control").at("control_transport"); };
  auto deadline = steady() + 8000;
  while (steady() < deadline) {
    fixture.pump(true);
    if (transport().at("path") == "dc" && !transport().at("qualifying").get<bool>() &&
        !transport().at("selecting").get<bool>()) break;
  }
  expect(transport().at("path") == "dc" && !transport().at("qualifying").get<bool>(), "fixture never qualified DC");
  const auto old_epoch = transport().at("epoch").get<std::uint64_t>();
  const auto old_frame = fixture.last_received_payload;
  const auto wss_before = fixture.wss_commands;
  fixture.stop_cloud(); // Real TCP closure, not merely changing dc_ready.
  fixture.restore_cloud();
  bool recovered = false;
  deadline = steady() + 8000;
  while (steady() < deadline) {
    fixture.pump(true); // DC courier remains available throughout recovery.
    const auto native = fixture.driver->status().at("native_control");
    const auto route = native.at("control_transport");
    if (fixture.wss_commands > wss_before && route.at("path") == "wss" &&
        route.at("epoch").get<std::uint64_t>() > old_epoch && !route.at("selecting").get<bool>() &&
        !native.at("ack_braking_wait").get<bool>()) { recovered = true; break; }
  }
  expect(recovered, "WSS reconnect with available DC stranded ACK braking wait");
  expect(fixture.last_received_payload.at("throttle") == 0.0 &&
      fixture.last_received_payload.at("steering") == 0.0, "reconnect sent motion during neutral recovery");
  expect(!fixture.apply(old_frame, ControlPath::DataChannel).at("transport_admitted").get<bool>(),
      "retired pre-reconnect DC command was admitted");
}

void product_startup_and_p2p_cloud_recovery() {
  Fixture fixture(false, false, true);
  auto state = [&] { return fixture.driver->status().at("native_control").at("control_transport"); };
  auto until = [&](std::string_view path, std::uint64_t after_epoch, bool ready) {
    const auto deadline = steady() + 8000;
    while (steady() < deadline) {
      fixture.pump(ready); const auto value = state();
      if (value.at("path") == path && value.at("epoch").get<std::uint64_t>() > after_epoch &&
          !value.at("selecting").get<bool>() && !value.at("qualifying").get<bool>()) return value.at("epoch").get<std::uint64_t>();
    }
    throw std::runtime_error("product failed route qualification: " + Json{
        {"target_path", path}, {"after_epoch", after_epoch},
        {"first_vehicle_rejection", fixture.first_vehicle_rejection},
        {"last_reason", fixture.last_result_reason}, {"transport", state()}}.dump());
  };
  const auto cloud_epoch = until("wss", 0, false);
  expect(fixture.wss_commands > 0 && fixture.dc_commands == 0 && state().at("mode") == "p2p_preferred" &&
      state().at("experimental_enabled") == false && state().at("direct_authority").at("configured") == false,
      "normal YAML startup required experimental flags/DC/short authority");
  fixture.dc_network_path = "turn_unknown";
  const auto relay_deadline = steady() + 250;
  while (steady() < relay_deadline) fixture.pump(true);
  expect(fixture.dc_commands == 0 && state().at("path") == "wss", "TURN was called product P2P");
  fixture.dc_network_path = "p2p";
  const auto direct_epoch = until("dc", cloud_epoch, true);
  const auto retired = fixture.last_received_payload;
  // A held request crosses the route withdrawal. Native must keep it neutral
  // until a real release/repress, including the later return to P2P.
  fixture.input(); // Release the completed initial route boundary before pressing motion.
  fixture.requested_throttle = 0.4; fixture.input();
  fixture.dc_network_path = "unknown";
  // Keep the held request across the route change, but release it as soon as
  // the sender withdraws DC. Waiting for the whole WSS round trip while this
  // fixture maps a held throttle to D can prevent the vehicle from accepting
  // neutral in its previous gear; the real 800 ms watchdog then correctly
  // clears the profile before a slow CI runner finishes the switch.
  const auto withdraw_deadline = steady() + 600;
  while (steady() < withdraw_deadline && state().at("path") == "dc") fixture.pump(true);
  expect(state().at("path") == "wss" &&
      fixture.driver->status().at("native_control").at("requires_fresh_input") == true &&
      fixture.driver->status().at("native_control").at("effective_throttle") == 0.0,
      "held pre-switch motion was not withdrawn when selecting cloud fallback");
  fixture.requested_throttle = 0.0; fixture.input();
  const auto fallback_epoch = until("wss", direct_epoch, true);
  expect(fixture.fence.path() == ControlPath::Wss && fixture.apply(retired, ControlPath::DataChannel).at("transport_admitted") == false,
      "retired direct command re-entered after fallback");
  expect(fixture.last_received_payload.at("throttle") == 0.0,
      "cloud fallback sent nonzero motion before a fresh press");
  fixture.dc_network_path = "p2p";
  const auto recovered_epoch = until("dc", fallback_epoch, true);
  expect(recovered_epoch > fallback_epoch, "recovered P2P reused an old route epoch");
  fixture.requested_throttle = 0.4; fixture.input();
  expect(fixture.driver->status().at("native_control").at("requires_fresh_input") == true &&
      fixture.driver->status().at("native_control").at("effective_throttle") == 0.0,
      "P2P recovery bypassed the release/repress interlock");
  fixture.requested_throttle = 0.0; fixture.input();
  expect(fixture.driver->status().at("native_control").at("requires_fresh_input") == false,
      "actual neutral release did not retire the recovered input latch");
  auto fresh = fixture.last_received_payload;
  fresh["seq"] = fresh.at("seq").get<std::uint64_t>() + 1000;
  fresh["sent_at_utc_ms"] = utc();
  fresh["native_mac"] = transport_message_mac(fresh, fixture.signing_key, false);
  fixture.cloud_online = false;
  const auto denied = fixture.apply(fresh, ControlPath::DataChannel);
  fixture.cloud_online = true;
  expect(denied.at("transport_admitted") == false && denied.at("reason") == "transport_authority_unavailable",
      "product P2P survived loss of live cloud authority");
  fixture.car_control_lease_expiry = utc() - 1;
  expect(fixture.apply(fresh, ControlPath::DataChannel).at("reason") == "transport_authority_unavailable", "expired regular lease admitted P2P");
  std::cout << "[PRODUCT_DIRECT] normal YAML, live cloud authority, no-DC bootstrap, TURN exclusion, P2P/cloud/P2P epochs passed\n";
}
void product_cloud_only_never_trials_dc() {
  Fixture fixture(false, false, true, "cloud_only"); fixture.dc_network_path = "p2p";
  const auto deadline = steady() + 700;
  while (steady() < deadline) fixture.pump(true);
  const auto state = fixture.driver->status().at("native_control").at("control_transport");
  expect(state.at("epoch").get<std::uint64_t>() > 0 && state.at("path") == "wss" && fixture.dc_commands == 0 && fixture.wss_commands > 0,
      "cloud_only product configuration trialled direct commands");
}
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

void actual_feedback_selection_is_independent_and_rejects_retired_statuses() {
  Fixture fixture(true, true); fixture.drop_dc_feedback = true;
  bool ready = false;
  auto deadline = steady() + 8000;
  while (steady() < deadline) {
    fixture.pump(true);
    const auto state = fixture.driver->status().at("native_control").at("control_transport");
    if (state.at("path") == "dc" && !state.at("selecting").get<bool>() && !state.at("qualifying").get<bool>() &&
        state.at("feedback_transport").value("path", "") == "wss" && !state.at("feedback_transport").value("selecting", true)) { ready = true; break; }
  }
  expect(ready, "actual native could not use DC commands with independent WSS feedback");
  const auto before = fixture.driver->status().at("native_control").at("control_transport");
  const auto old_feedback = fixture.feedback_fence.state();
  fixture.vehicle_socket.send_json(fixture.feedback_status(++fixture.feedback_status_sequence, old_feedback));
  const auto status_deadline = steady() + 1500;
  while (steady() < status_deadline && fixture.received_statuses.empty()) fixture.pump(true);
  expect(!fixture.received_statuses.empty() && fixture.received_statuses.back().at("control_status_seq") == 1,
      "selected WSS feedback failed to carry authenticated status");
  fixture.drop_dc_feedback = false;
  ready = false; deadline = steady() + 5000;
  while (steady() < deadline) {
    fixture.pump(true);
    const auto state = fixture.driver->status().at("native_control").at("control_transport");
    if (state.at("feedback_transport").value("path", "") == "dc" && !state.at("feedback_transport").value("selecting", true)) { ready = true; break; }
  }
  expect(ready, "actual native did not prefer healthy P2P feedback after return health recovered");
  const auto after = fixture.driver->status().at("native_control").at("control_transport");
  expect(after.at("epoch") == before.at("epoch") && after.at("path") == "dc", "feedback switch changed ordinary command epoch/path");
  auto request = fixture.scope(true);
  request["vehicle_statuses"] = Json::array({fixture.feedback_status(std::numeric_limits<std::uint64_t>::max(), old_feedback),
      fixture.feedback_status(2, fixture.feedback_fence.state())});
  const auto response = fixture.driver->control_transport_bridge(request);
  expect(response.at("statuses").size() == 1 && response.at("statuses").front().at("control_status_seq") == 2,
      "retired high feedback sequence poisoned the actual native status reducer");
  const auto duplicate = fixture.driver->control_transport_bridge(request);
  expect(duplicate.at("statuses").empty(), "duplicate status falsely advanced native state");
  auto tampered = fixture.feedback_status(3, fixture.feedback_fence.state()); tampered["status"]["control_status_seq"] = 999;
  request["vehicle_statuses"] = Json::array({tampered, fixture.feedback_status(3, fixture.feedback_fence.state())});
  const auto clean = fixture.driver->control_transport_bridge(request);
  expect(clean.at("statuses").size() == 1 && clean.at("statuses").front().at("control_status_seq") == 3,
      "unverified high status poisoned native feedback watermark");
  request["vehicle_statuses"] = Json::array({fixture.feedback_status(5, fixture.feedback_fence.state(), "vcu_handshake_status"),
      fixture.feedback_status(4, fixture.feedback_fence.state())});
  const auto batch = fixture.driver->control_transport_bridge(request);
  expect(batch.at("statuses").size() == 2 && batch.at("statuses").front().at("control_status_seq") == 4 &&
      batch.at("statuses").back().at("control_status_seq") == 5, "four-kind batch advanced global watermark before ordering snapshots");
  fixture.feedback_status_sequence = 5;
  // Old epoch high seq traverses the actual cloud cache/socket as well.
  fixture.vehicle_socket.send_json(fixture.feedback_status(std::numeric_limits<std::uint64_t>::max(), old_feedback));
  fixture.drop_dc_feedback = true; ready = false; deadline = steady() + 3000;
  while (steady() < deadline) {
    fixture.pump(true);
    const auto state = fixture.driver->status().at("native_control").at("control_transport");
    if (state.at("feedback_transport").value("path", "") == "wss" && !state.at("feedback_transport").value("selecting", true)) { ready = true; break; }
  }
  expect(ready, "one-way DC return loss did not independently select WSS feedback backup");
  const auto fallback = fixture.driver->status().at("native_control").at("control_transport");
  expect(fallback.at("epoch") == before.at("epoch") && fallback.at("path") == "dc",
      "return-only fault changed healthy command axis: " + Json{
          {"before", before}, {"after", fallback}, {"last_reason", fixture.last_result_reason},
          {"first_vehicle_rejection", fixture.first_vehicle_rejection}}.dump());
  fixture.received_statuses.clear();
  fixture.vehicle_socket.send_json(fixture.feedback_status(++fixture.feedback_status_sequence, fixture.feedback_fence.state()));
  deadline = steady() + 1500;
  while (steady() < deadline && fixture.received_statuses.empty()) fixture.pump(true);
  expect(fixture.received_statuses.size() == 1 && fixture.received_statuses.front().at("control_status_seq") == 6,
      "retired high sequence poisoned actual cloud WSS status cache/socket watermark: received=" +
          Json(fixture.received_statuses).dump() + ", feedback=" + fixture.feedback_fence.state().dump());
}

void actual_cloud_explicit_denial_revokes_before_short_deadline() {
  Fixture fixture(true);
  bool ready = false; auto deadline = steady() + 8000;
  while (steady() < deadline) {
    fixture.pump(true);
    if (fixture.driver->status().at("native_control").at("control_transport").at("direct_authority").value("continuation_available", false)) { ready = true; break; }
  }
  expect(ready, "revoke fixture never became a healthy direct session");
  fixture.renew_authority(true);
  deadline = steady() + 1000;
  while (steady() < deadline) {
    fixture.pump(true);
    if (fixture.driver->status().at("native_control").at("control_transport").at("direct_authority").value("remaining_ms", 0) > 400) break;
  }
  Json socket_scope, fresh;
  { std::lock_guard lock(fixture.car_mutex); socket_scope = fixture.car_authority.scope(); fresh = fixture.last_received_payload; }
  static_cast<void>(fixture.http.post_json_response(fixture.base + "/sessions/" + fixture.session + "/control_authority/revoke",
      {{"actor", "transport-vehicle"}, {"device_token", "fixture-device"}, {"connection_generation", fixture.generation}, {"reason", "offline-fixture-explicit-revoke"}}));
  Json denial; deadline = steady() + 500;
  while (steady() < deadline) {
    const auto received = fixture.vehicle_socket.receive_json(std::chrono::milliseconds(10));
    if (received.status == WebSocketReceiveStatus::Message && received.message.value("event", "") == "signaling_authority_lost") { denial = received.message; break; }
  }
  expect(denial.is_object() && denial.contains("authority_scope"), "real cloud did not emit scoped explicit denial");
  { std::lock_guard lock(fixture.car_mutex);
    expect(fixture.car_authority.valid(steady()), "revocation arrived only after natural fixture lease expiry");
    expect(fixture.car_authority.observe_authenticated_cloud_denial(denial, socket_scope), "real cloud scope did not match accepted vehicle authority");
    fixture.vehicle->set_control_braking_wait(true, utc(), ControlRecoveryCause::ControlLink);
    expect(fixture.vehicle->control_braking_wait(), "real common service did not enter immediate revoke braking"); }
  fresh["seq"] = fresh.at("seq").get<std::uint64_t>() + 1; fresh["sent_at_utc_ms"] = utc();
  fresh["native_mac"] = transport_message_mac(fresh, fixture.signing_key, false);
  expect(!fixture.apply(fresh, ControlPath::DataChannel).at("accepted").get<bool>(), "fresh DC command remained admitted after explicit denial");
  bool native_revoked = false; deadline = steady() + 500;
  while (steady() < deadline) {
    const auto authority = fixture.driver->status().at("native_control").at("control_transport").at("direct_authority");
    if (authority.value("revoked", false)) { native_revoked = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(native_revoked, "real native treated explicit cloud denial as bounded transport grace");
}
void websocket_upgrade_status_preserves_denial_vs_transient_service_failure() {
  for (const int code : {401, 403, 404, 409, 503}) {
    SimpleHttpServer server("127.0.0.1", 0, [code](const HttpRequest&) { return ServerResponse::json(code, {{"error", "fixture"}}); });
    server.start(); WebSocketClient client(std::chrono::milliseconds(200)); bool observed = false;
    try { client.connect("ws://127.0.0.1:" + std::to_string(server.port()) + "/fixture-denied"); }
    catch (const HttpStatusError& error) { observed = error.status() == code && transient_cloud_failure(error) == (code == 503); }
    server.stop(); expect(observed, "authenticated WebSocket upgrade denial was erased into an indistinguishable transport error");
  }
}

void review_r4_late_stop_result_is_historical_not_current() {
  Fixture fixture(true); const auto deadline = steady() + 8000; bool ready = false;
  while (steady() < deadline) { fixture.pump(true); if (fixture.driver->status().at("native_control").at("control_transport").at("direct_authority").value("continuation_available", false)) { ready = true; break; } }
  expect(ready, "R4 fixture did not qualify");
  static_cast<void>(fixture.driver->update_control_intent({{"session_id", fixture.session}, {"session_generation", fixture.native_generation},
      {"ui_instance_id", "owner"}, {"ui_control_epoch", fixture.ui_epoch}, {"takeover", false}, {"intent_seq", ++fixture.intent_seq},
      {"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}, {"estop", true}, {"critical_video_ready", true}}));
  auto request = fixture.scope(true); request["last_seen_seq"] = 0;
  const auto published = fixture.driver->emergency_stop_bridge(request);
  expect(published.at("frame_json").is_string(), "R4 STOP not published");
  const auto frame = Json::parse(published.at("frame_json").get<std::string>());
  const auto stored = fixture.apply(frame, ControlPath::DataChannel);
  { std::lock_guard lock(fixture.car_mutex); expect(fixture.vehicle->reset_estop(true, "local-fixture", utc()), "R4 local reset rejected");
    fixture.emergency_receiver.observe_latched(fixture.vehicle->safety_state() == SafetyState::Estop); }
  std::this_thread::sleep_for(std::chrono::milliseconds(260)); request["vehicle_result"] = stored;
  const auto result = fixture.driver->emergency_stop_bridge(request);
  { std::lock_guard lock(fixture.car_mutex); expect(fixture.vehicle->safety_state() != SafetyState::Estop && fixture.emergency_receiver.reset_epoch() == 1, "R4 reset unexpectedly changed"); }
  expect(result.at("round_trip_ms").get<std::int64_t>() > NativeControlAckRecovery::kFreshAckMs, "R4 fixture delay did not cross existing200ms");
  expect(result.at("software_latched_confirmed") == false, "COUNTEREXAMPLE R4: late old STOP result became fresh confirmation after local reset");
  expect(result.at("historical_software_latch_result") == true, "late successful STOP evidence was discarded instead of retained as history");
}

void review_r6_actual_native_retains_one_shot_candidate_notification() {
  Fixture fixture(true, true); fixture.inject_candidate_notification = true; fixture.drop_dc_feedback = true;
  const auto deadline = steady() + 4000;
  while (steady() < deadline && fixture.received_statuses.empty()) fixture.pump(true);
  expect(fixture.candidate_notification_sent && fixture.received_statuses.size() == 1 &&
      fixture.received_statuses.front().value("issue_code", "") == "vcu_drive_gear_change_moving_or_stale",
      "candidate rejection sent before delayed selection result did not reach native UI queue");
  for (int index=0;index<20;++index) fixture.pump(true);
  expect(fixture.received_statuses.size() == 1, "candidate rejection was delivered repeatedly after confirmation");
}

void review_r7_actual_native_repairs_initial_loss_across_attempt() {
  Fixture fixture(true, true); fixture.drop_initial_feedback = true;
  auto deadline = steady() + 4000;
  while (steady() < deadline && !fixture.lost_initial_feedback.is_object()) fixture.pump(true);
  expect(fixture.lost_initial_feedback.is_object() && !fixture.feedback_fence.negotiated(), "R7 initial selection was not lost before vehicle init");
  fixture.media_attempt = "media-b";
  auto offer = ProtocolMetadata{kProtocolVersion,"transport-vehicle","transport-driver",fixture.session,2,utc()}.to_json();
  offer.update({{"sender","transport-vehicle"},{"recipient","transport-driver"},{"device_token","fixture-device"},{"connection_generation",fixture.generation},
      {"type","webrtc_offer"},{"payload",{{"type","offer"},{"sdp","v=0\r\na=ice-ufrag:second\r\n"},{"media_negotiation_id",fixture.media_attempt},
      {"control_transport_capability",kControlTransportCapability},{"emergency_stop_capability",kEmergencyStopCapability},{"feedback_transport_capability",kFeedbackTransportCapability}}}});
  static_cast<void>(fixture.http.post_json_response(fixture.base+"/signaling/"+fixture.session+"/messages",offer));
  bool new_offer_seen = false; deadline = steady() + 1500;
  while (steady() < deadline && !new_offer_seen) {
    const auto polled = fixture.driver->poll_signaling();
    for (const auto& message : polled.at("messages")) if (message.value("type", "") == "webrtc_offer" &&
        message.at("payload").value("media_negotiation_id", "") == fixture.media_attempt) new_offer_seen = true;
  }
  expect(new_offer_seen, "new scoped media offer was not delivered to actual native signaling");
  fixture.pause_after_feedback_loss = false;
  bool recovered = false; deadline = steady() + 5000;
  while (steady() < deadline) {
    fixture.pump(true); const auto state = fixture.driver->status().at("native_control").at("control_transport").at("feedback_transport");
    if (state.value("media_attempt","") == fixture.media_attempt && state.value("enabled",false) && !state.value("selecting",true)) { recovered = true; break; }
  }
  expect(recovered && fixture.feedback_fence.negotiated(), "actual native remained stuck after exact scoped epoch0 repair");
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
  // Cloud shutdown must not pause the independently healthy driver/DC pump.
  // server.stop() closes real sockets but may join workers for longer than the
  // short observation interval. Joining first manufactured input loss.
  fixture.cloud_online = false; fixture.vehicle_socket.close(); fixture.delayed.clear();
  const auto outage_started = steady();
  std::jthread cloud_shutdown([&] { fixture.server->stop(); });
  bool cloud_socket_closed = false;
  const auto cut_deadline = outage_started + remaining - 100;
  while (steady() < cut_deadline) {
    fixture.pump(true);
    if (!fixture.driver->status().at("native_control").value("websocket_connected", true)) { cloud_socket_closed = true; break; }
  }
  const auto cut_observed_at = steady();
  const auto before_observation = fixture.dc_commands;
  const auto at_cut = fixture.driver->status().at("native_control");
  const auto accepted_at_cut = at_cut.at("vehicle_result_latency").value("last_accepted_command_seq", std::uint64_t{0});
  const auto grant_at_cut = at_cut.at("control_transport").at("direct_authority").value("remaining_ms", std::int64_t{0});
  // Continuity is actual accepted progress inside the existing200ms healthy
  // result budget, not a100ms packet-rate requirement. Never extend the
  // accepted600ms REVIEW grant, including time spent observing socket closure.
  const auto observation_budget = std::max<std::int64_t>(0,
      std::min<std::int64_t>(NativeControlAckRecovery::kFreshAckMs, grant_at_cut - 1));
  const auto outage_deadline = cut_observed_at + observation_budget;
  Json observed_native = at_cut;
  while (steady() < outage_deadline) {
    fixture.pump(true); observed_native = fixture.driver->status().at("native_control");
    const auto& latency = observed_native.at("vehicle_result_latency");
    if (latency.value("last_accepted_command_seq", std::uint64_t{0}) > accepted_at_cut &&
        latency.value("accepted", false) && latency.at("round_trip_ms").is_number() &&
        latency.at("round_trip_ms").get<std::int64_t>() <= NativeControlAckRecovery::kFreshAckMs) break;
  }
  const auto during = observed_native.at("control_transport");
  const auto result_progress = observed_native.at("vehicle_result_latency");
  const auto observed_at = steady();
  cloud_shutdown.join();
  std::cout << "[DIRECT_OUTAGE] " << Json{{"grant_remaining_before_cut_ms", remaining},
      {"grant_remaining_at_cut_ms", grant_at_cut}, {"observation_budget_ms", observation_budget},
      {"cut_wait_ms", cut_observed_at - outage_started}, {"cloud_socket_closed", cloud_socket_closed},
      {"observation_elapsed_ms", observed_at - cut_observed_at}, {"shutdown_join_elapsed_ms", steady() - outage_started},
      {"accepted_seq_at_cut", accepted_at_cut}, {"accepted_seq_after_cut", result_progress.value("last_accepted_command_seq", std::uint64_t{0})},
      {"dc_commands_before", before}, {"dc_commands_at_cut", before_observation}, {"dc_commands_during", fixture.dc_commands}, {"transport", during}}.dump() << '\n';
  expect(cloud_socket_closed && observed_at <= outage_deadline && fixture.dc_commands > before_observation &&
      result_progress.value("last_accepted_command_seq", std::uint64_t{0}) > accepted_at_cut &&
      result_progress.value("accepted", false) && result_progress.at("round_trip_ms").is_number() &&
      result_progress.at("round_trip_ms").get<std::int64_t>() <= NativeControlAckRecovery::kFreshAckMs &&
      during.at("path") == "dc" && during.at("direct_authority").value("valid", false),
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
  // R5: a browser that proved it did not call send retains last_seen=0 and
  // refetches the same publication, not a newly signed command/deadline.
  // The JS courier's 25ms retry tick has separate deterministic coverage. A
  // host sleep here can overshoot the unchanged 100ms native publication and
  // turn a correct expiry into a false "changed bytes" failure on busy runners.
  const auto refetch_started = steady();
  const auto retry = fixture.driver->emergency_stop_bridge(request);
  const auto refetch_evidence = Json{{"elapsed_ms", steady() - refetch_started},
      {"first_valid_for_ms", response.at("valid_for_ms")}, {"retry_valid_for_ms", retry.at("valid_for_ms")},
      {"same_signed_bytes", retry.at("frame_json") == response.at("frame_json")}};
  if (retry.at("frame_json") != response.at("frame_json") ||
      retry.at("valid_for_ms").get<int>() > response.at("valid_for_ms").get<int>())
    throw std::runtime_error("unqueued STOP refetch changed signed bytes or renewed publication window: " + refetch_evidence.dump());
  const auto payload = Json::parse(response.at("frame_json").get<std::string>());
  expect(payload.at("estop") == true && payload.at("brake") == 1.0 && verify_transport_message(payload, fixture.signing_key, false), "native emergency bytes were forged or not stop-only");
  const auto result = fixture.apply(payload, ControlPath::DataChannel); // Production MAC/stop receiver + real common safety service.
  const auto applied_sequence = fixture.result_seq;
  expect(fixture.apply(payload, ControlPath::Wss) == result && fixture.result_seq == applied_sequence,
      "same signed STOP on backup WSS reapplied rather than returning its cached result");
  request["last_seen_seq"] = payload.at("seq"); request["vehicle_result"] = result;
  const auto confirmed = fixture.driver->emergency_stop_bridge(request);
  expect(confirmed.at("software_latched_confirmed") == true && confirmed.at("physical_execution_confirmed") == false &&
      confirmed.at("command_received_path") == "dc" && confirmed.at("feedback_path") == "dc" && !wss_finished.load(),
      "software stop confirmation waited for WSS or mislabeled actual delivery/physical state");
  auto tampered = result; tampered["software_estop_latched"] = false; request["vehicle_result"] = tampered;
  bool rejected = false; try { static_cast<void>(fixture.driver->emergency_stop_bridge(request)); } catch (const std::invalid_argument&) { rejected = true; }
  expect(rejected, "page could forge an emergency software result");
  request.erase("vehicle_result"); request["last_seen_seq"] = 0;
  std::this_thread::sleep_for(std::chrono::milliseconds(retry.at("valid_for_ms").get<int>() + 1));
  expect(!wss_finished.load() && fixture.driver->emergency_stop_bridge(request).at("frame").is_null(),
      "original unqueued publication remained fetchable past100ms or WSS stall precondition ended");
  expect(press_stop().at("accepted") == true, "new explicit STOP intent rejected");
  const auto latest = fixture.driver->emergency_stop_bridge(request);
  expect(latest.at("frame").at("seq").get<std::uint64_t>() > payload.at("seq").get<std::uint64_t>() &&
      latest.at("frame_json") != response.at("frame_json"), "native bridge returned old STOP instead of the new explicit latest publication");
  std::cout << "[STOP_REFETCH] " << refetch_evidence.dump() << '\n';
}
}  // namespace
int main(int argc, char** argv) {
  try { courier_status_does_not_wait_for_signaling_poll(); if (argc > 1 && std::string_view(argv[1]) == "--feedback-only") { actual_feedback_selection_is_independent_and_rejects_retired_statuses(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--native-courier-only") { native_courier_bridge_integration(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--native-dual-video-only") { native_dual_video_bridge_integration(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--native-safety-only") { native_publication_safety_interlocks(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--native-publication-only") { native_publication_bridge_integration(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--outage-only") { actual_native_short_cloud_outage_and_independent_vehicle_expiry(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--product-only") { product_recovery_withdraws_unfetched_motion(); product_wss_reconnect_retires_dc_and_recovers_ack(); product_startup_and_p2p_cloud_recovery(); product_cloud_only_never_trials_dc(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--review-r7-only") { review_r7_actual_native_repairs_initial_loss_across_attempt(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--review-r5-only") { emergency_broker_delivers_after_ordinary_wss_is_already_blocked(); return 0; } if (argc > 1 && std::string_view(argv[1]) == "--review-r4-only") { review_r4_late_stop_result_is_historical_not_current(); return 0; } native_publication_bridge_integration(); native_courier_bridge_integration(); native_publication_safety_interlocks(); native_dual_video_bridge_integration(); integration(); review_r6_actual_native_retains_one_shot_candidate_notification(); review_r7_actual_native_repairs_initial_loss_across_attempt(); actual_feedback_selection_is_independent_and_rejects_retired_statuses(); actual_cloud_explicit_denial_revokes_before_short_deadline(); websocket_upgrade_status_preserves_denial_vs_transient_service_failure(); actual_native_short_cloud_outage_and_independent_vehicle_expiry(); emergency_broker_delivers_after_ordinary_wss_is_already_blocked(); std::cout << "Native/cloud/scoped-byte-courier/shared-service integration passed, including cloud outage, short authority and blocked-WSS emergency; loopback only, no WebRTC or hardware proof\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
