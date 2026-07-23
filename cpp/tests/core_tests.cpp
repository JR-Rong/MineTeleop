#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/media.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/upload.hpp"
#include "mine_teleop/video.hpp"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using mine_teleop::ControlCommand;

class TestFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

void expect_near(double actual, double expected, double epsilon, std::string_view message) {
  if (std::abs(actual - expected) > epsilon) {
    throw TestFailure(std::string(message) + ": expected " + std::to_string(expected) + ", got " +
                      std::to_string(actual));
  }
}

template <typename Function>
void expect_throws(Function&& function, std::string_view message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw TestFailure(std::string(message));
}

mine_teleop::Json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw TestFailure("cannot open JSON test vector: " + path.string());
  mine_teleop::Json value;
  input >> value;
  return value;
}

ControlCommand command(std::uint64_t seq = 1, std::int64_t timestamp_ms = 0) {
  ControlCommand value;
  value.vehicle_id = "vehicle-001";
  value.driver_id = "driver-001";
  value.session_id = "session-001";
  value.seq = seq;
  value.sent_at_utc_ms = timestamp_ms;
  value.control_token = "token";
  value.gear = "D";
  value.steering = 0.25;
  value.throttle = 0.5;
  value.brake = 0.0;
  return value;
}

mine_teleop::Json signaling_request(
    std::string_view vehicle_id,
    std::string_view driver_id,
    std::string_view session_id,
    std::uint64_t seq,
    std::int64_t sent_at_utc_ms,
    std::string_view sender,
    std::string_view recipient,
    std::string_view credential_name,
    std::string_view credential,
    std::string_view type,
    const mine_teleop::Json& payload) {
  auto value = mine_teleop::ProtocolMetadata{
                   mine_teleop::kProtocolVersion,
                   std::string(vehicle_id),
                   std::string(driver_id),
                   std::string(session_id),
                   seq,
                   sent_at_utc_ms}
                   .to_json();
  value["sender"] = sender;
  value["recipient"] = recipient;
  value[std::string(credential_name)] = std::string(credential);
  value["type"] = type;
  value["payload"] = payload;
  return value;
}

class NoFeedbackAdapter final : public mine_teleop::VehicleAdapter {
 public:
  void open() override { opened = true; }
  void close() override { opened = false; }
  void apply_control(const ControlCommand&) override { ++applied_commands; }
  void apply_safe_stop(const mine_teleop::ControlOutput& output) override {
    last_safe_output = output;
    ++safe_stops;
  }
  bool poll_feedback() override { return false; }
  [[nodiscard]] bool feedback_ready() const override { return false; }
  [[nodiscard]] mine_teleop::VehicleTelemetry read_telemetry() override { return {}; }
  [[nodiscard]] mine_teleop::VehicleAdapterStatus status() const override {
    return {"no_feedback", opened, true, "can0", "", applied_commands, safe_stops, "", false};
  }

  bool opened{false};
  std::uint64_t applied_commands{0};
  std::uint64_t safe_stops{0};
  mine_teleop::ControlOutput last_safe_output;
};

void test_config_loads_current_vehicle_yaml() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  expect(config.vehicle_id == "vehicle-001", "vehicle id mismatch");
  expect(config.control.control_timeout_ms == 800, "control timeout mismatch");
  expect(config.enabled_cameras().size() == 1, "enabled camera count mismatch");
  expect(config.realtime_profile("realtime_720p").fps == 30, "profile fps mismatch");
  expect(config.vehicle_adapter.type == "mock", "adapter type mismatch");
}

void test_bench_config_drives_unified_vehicle_runtime() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.bench.yaml");
  expect(config.runtime.control_enabled, "bench runtime control is disabled");
  expect(config.runtime.media_enabled, "bench runtime media is disabled");
  expect(config.runtime.control_log_commands, "bench runtime control logging is disabled");
  expect(config.runtime.teleop_poll_interval_ms == 50, "bench teleop poll interval changed");
  expect(config.runtime.media_frame_timeout_ms == 3000, "bench media frame timeout changed");
  expect(config.recording.enabled, "bench recording is disabled");
  expect(
      config.cloud.device_token_file == std::filesystem::path("configs/device-token"),
      "relative device token file was not resolved from the config directory");
}

void test_field_config_pins_tls_route_without_system_dns() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.three-machine.field.yaml");
  expect(
      config.cloud.signaling_url == "wss://teleop-field.internal:6000/signaling",
      "field vehicle signaling URL does not use the private TLS name");
  expect(
      config.cloud.resolve_entries ==
          std::vector<std::string>{"teleop-field.internal:6000:60.205.213.254"},
      "field vehicle resolver override changed");
  expect(
      config.cloud.ca_bundle == std::filesystem::path("configs/mine-teleop-field-root.crt"),
      "field vehicle CA path was not resolved relative to its config");
  expect(std::filesystem::is_regular_file(config.cloud.ca_bundle), "field vehicle CA bundle is missing");
  expect(
      config.runtime.teleop_poll_interval_ms == 500,
      "field vehicle session discovery interval is not rate-limit safe");
  expect(config.cloud.ice_transport_policy == "all", "field vehicle ICE policy is not the safe default");
  expect(mine_teleop::ice_transport_policy_is_valid("all"), "all ICE policy was rejected");
  expect(mine_teleop::ice_transport_policy_is_valid("relay"), "relay ICE policy was rejected");
  expect(!mine_teleop::ice_transport_policy_is_valid("direct"), "unknown ICE policy was accepted");
  expect(
      config.field_safety.max_time_sync_uncertainty_ms == 25,
      "field vehicle time synchronization limit is not 25ms");
}

void test_control_command_json_round_trip_and_validation() {
  const auto original = command(7, 1234);
  const auto parsed = ControlCommand::from_json(original.to_json());
  expect(parsed.seq == 7, "sequence did not round trip");
  expect_near(parsed.steering, 0.25, 1e-9, "steering did not round trip");
  auto invalid = original;
  invalid.throttle = 1.5;
  expect_throws([&] { invalid.validate(); }, "invalid throttle was accepted");
}

void test_shared_protocol_v1_vectors_and_session_states() {
  const auto valid_path = std::filesystem::path("protocol/v1/control-command.valid.json");
  const auto valid = read_json(valid_path);
  const auto parsed = ControlCommand::from_json(valid);
  expect(parsed.protocol_version == mine_teleop::kProtocolVersion, "protocol version changed");
  expect(parsed.driver_id == "driver-001", "driver id did not parse from shared vector");
  expect(parsed.sent_at_utc_ms == 1780000000000, "UTC timestamp did not parse from shared vector");
  expect(!parsed.to_json().contains("future_extension"), "unknown extension leaked into the command model");

  const std::vector<std::string> required_metadata{
      "protocol_version", "vehicle_id", "driver_id", "session_id", "seq", "sent_at_utc_ms"};
  for (const auto& field : required_metadata) {
    auto missing = valid;
    missing.erase(field);
    expect_throws(
        [&] { static_cast<void>(ControlCommand::from_json(missing)); },
        "missing required protocol metadata was accepted: " + field);
  }
  auto missing_token = valid;
  missing_token.erase("control_token");
  expect_throws(
      [&] { static_cast<void>(ControlCommand::from_json(missing_token)); },
      "control command without control_token was accepted");
  auto wrong_version = valid;
  wrong_version["protocol_version"] = mine_teleop::kProtocolVersion + 1;
  expect_throws(
      [&] { static_cast<void>(ControlCommand::from_json(wrong_version)); },
      "incompatible protocol version was accepted");
  expect_throws(
      [&] {
        static_cast<void>(ControlCommand::from_json(
            read_json("protocol/v1/control-command.invalid-missing-driver-id.json")));
      },
      "shared negative protocol vector was accepted");

  const std::vector<mine_teleop::SessionState> states{
      mine_teleop::SessionState::Offline,
      mine_teleop::SessionState::Online,
      mine_teleop::SessionState::Reserved,
      mine_teleop::SessionState::Connecting,
      mine_teleop::SessionState::Active,
      mine_teleop::SessionState::Degraded,
      mine_teleop::SessionState::Stopping,
      mine_teleop::SessionState::Closed,
  };
  for (const auto state : states) {
    expect(
        mine_teleop::session_state_from_string(mine_teleop::to_string(state)) == state,
        "session state did not round trip");
  }
  expect_throws(
      [] { static_cast<void>(mine_teleop::session_state_from_string("SESSION_ACTIVE")); },
      "legacy session state was silently accepted");
}

void test_control_receiver_enforces_token_sequence_and_gap() {
  mine_teleop::ControlReceiver receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto first = command(1, 0);
  first.control_token = "wrong";
  expect(receiver.accept(first, 0).reason == "control_token_invalid", "wrong token was accepted");
  first.control_token = "token";
  first.driver_id = "driver-other";
  expect(receiver.accept(first, 0).reason == "wrong_driver", "wrong driver was accepted");
  first.driver_id = "driver-001";
  expect(receiver.accept(first, 0).accepted, "first command was rejected");
  expect(receiver.accept(first, 50).reason == "old_seq", "old sequence was accepted");
  auto late = command(2, 500);
  late.control_token = "token";
  expect(receiver.accept(late, 500).reason == "command_gap_exceeded", "large command gap was accepted");
  late.estop = true;
  expect(receiver.accept(late, 500).accepted, "estop should bypass command gap rejection");

  mine_teleop::ControlReceiver recovery_receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto recovery_first = command(1, 0);
  recovery_first.control_token = "token";
  expect(recovery_receiver.accept(recovery_first, 0).accepted, "recovery first command was rejected");
  auto recovery_gap = command(2, 500);
  recovery_gap.control_token = "token";
  expect(recovery_receiver.accept(recovery_gap, 500).reason == "command_gap_exceeded", "recovery gap was not detected");
  auto recovery_next = command(3, 550);
  recovery_next.control_token = "token";
  expect(recovery_receiver.accept(recovery_next, 550).accepted, "receiver did not recover after command gap");

  mine_teleop::ControlReceiver synchronized_receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto stale = command(1, 0);
  stale.control_token = "token";
  expect(synchronized_receiver.accept(stale, 201).reason == "command_age_exceeded", "stale command was accepted");
  stale.sent_at_utc_ms = 201;
  stale.estop = true;
  expect(synchronized_receiver.accept(stale, 500).accepted, "stale estop should remain acceptable");

  mine_teleop::ControlReceiver future_receiver("vehicle-001", "driver-001", "session-001", 200, 1, true, "token");
  auto future = command(1, 201);
  future.control_token = "token";
  expect(future_receiver.accept(future, 0).reason == "command_timestamp_in_future", "future command was accepted");
}

void test_mailbox_keeps_only_latest_command() {
  mine_teleop::LatestControlCommandMailbox mailbox;
  mailbox.publish(command(1));
  mailbox.publish(command(2));
  expect(mailbox.pending_count() == 1, "mailbox should contain one command");
  expect(mailbox.dropped_count() == 1, "mailbox should count overwritten command");
  expect(mailbox.pop_latest()->seq == 2, "mailbox did not preserve latest command");
}

void test_safety_timeout_profile_and_estop_latch() {
  mine_teleop::SafetyStateMachine safety(
      300,
      800,
      {{1500, 1.0}, {0, 0.3}, {500, 0.6}});
  safety.mark_ready(0);
  auto value = command(1, 0);
  safety.on_valid_command(value, 0);
  safety.tick(300);
  expect(safety.state() == mine_teleop::SafetyState::Degraded, "degraded state not entered");
  safety.tick(800);
  expect(safety.state() == mine_teleop::SafetyState::TimeoutBrake, "timeout state not entered");
  expect_near(safety.current_output(800).brake, 0.3, 1e-9, "initial timeout brake mismatch");
  expect_near(safety.current_output(1300).brake, 0.6, 1e-9, "second timeout brake mismatch");
  expect_near(safety.current_output(2300).brake, 1.0, 1e-9, "maximum timeout brake mismatch");

  value.seq = 2;
  value.estop = true;
  safety.on_valid_command(value, 2400);
  expect(safety.state() == mine_teleop::SafetyState::Estop, "estop did not latch");
  value.seq = 3;
  value.estop = false;
  safety.on_valid_command(value, 2450);
  expect(safety.state() == mine_teleop::SafetyState::Estop, "drive command cleared estop latch");
  expect(!safety.reset_estop(false, "operator", 2500), "estop reset without local confirmation");
  expect(safety.reset_estop(true, "operator", 2500), "confirmed estop reset failed");
}

void test_control_service_reports_safe_stop_output_after_timeout() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);
  expect(service.receive_command(command(1, 0), 0).accepted, "control command was rejected");
  service.tick(800);
  service.tick(1300);
  expect(service.safety_state() == mine_teleop::SafetyState::TimeoutBrake, "service did not enter timeout");
  const auto telemetry = adapter_view->read_telemetry();
  expect_near(telemetry.throttle_feedback, 0.0, 1e-9, "timeout telemetry retained stale throttle");
  expect_near(telemetry.brake_feedback, 0.6, 1e-9, "timeout telemetry did not report safe brake");
  service.close();
}

void test_control_service_bounds_telemetry_history() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  mine_teleop::VehicleControlService service(
      config,
      "driver-001",
      "session-001",
      "token",
      std::make_unique<mine_teleop::MockVehicleAdapter>(),
      1);
  service.start(0);
  constexpr std::size_t total_samples = mine_teleop::kMaxVehicleTelemetryHistory * 2 + 1;
  for (std::size_t sample = 0; sample < total_samples; ++sample) {
    service.tick(static_cast<std::int64_t>(sample));
  }
  const auto& history = service.telemetry_history();
  expect(
      history.size() == mine_teleop::kMaxVehicleTelemetryHistory,
      "vehicle telemetry history grew beyond its retention bound");
  expect(
      history.front().at("seq").get<std::uint64_t>() == total_samples - history.size() + 1,
      "vehicle telemetry history did not discard its oldest sample");
  expect(
      history.back().at("seq").get<std::uint64_t>() == total_samples,
      "vehicle telemetry history did not retain its newest sample");
  const auto summary = service.summary();
  expect(summary.at("telemetry_count").get<std::uint64_t>() == total_samples, "telemetry total count was truncated");
  expect(
      summary.at("telemetry_retained_count").get<std::size_t>() == mine_teleop::kMaxVehicleTelemetryHistory,
      "telemetry retained count does not match the bounded history");
  service.close();
}

void test_control_service_requires_feedback_before_actuation_but_allows_estop() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  config.field_safety.require_can_feedback_before_control = true;
  auto adapter = std::make_unique<NoFeedbackAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 100);
  service.start(0);

  const auto rejected = service.receive_command(command(1, 0), 0);
  expect(!rejected.accepted && rejected.reason == "can_feedback_missing", "control was not gated on CAN feedback");
  expect(adapter_view->applied_commands == 0, "control reached chassis without CAN feedback");
  expect(adapter_view->safe_stops == 1, "missing feedback did not issue a safe stop");
  expect_near(adapter_view->last_safe_output.brake, 1.0, 1e-9, "feedback gate did not use full brake");

  auto estop = command(2, 10);
  estop.estop = true;
  const auto accepted_estop = service.receive_command(estop, 10);
  expect(accepted_estop.accepted, "estop must bypass the feedback gate");
  expect(service.safety_state() == mine_teleop::SafetyState::Estop, "estop did not latch without feedback");
  service.close();
}

void test_fault_output_fails_safe() {
  mine_teleop::SafetyStateMachine safety(300, 800, {{0, 0.3}});
  safety.mark_ready(0);
  safety.mark_fault();
  const auto output = safety.current_output(0);
  expect_near(output.brake, 1.0, 1e-9, "fault output must command full brake");
  expect_near(output.throttle, 0.0, 1e-9, "fault output must clear throttle");
}

void test_native_signaling_webrtc_message_isolation() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-1", "secret"}, {"driver-2", "secret-2"}};
  config.device_tokens = {{"vehicle-001", "device-secret"}};
  config.driver_vehicle_permissions = {
      {"driver-1", {"vehicle-001"}},
      {"driver-2", {"vehicle-001"}}};
  config.max_sdp_bytes = 64;
  config.max_ice_candidate_bytes = 32;
  config.signaling_message_ttl_ms = 50;
  const auto audit_path = std::filesystem::path("/tmp") / ("mine-teleop-audit-" + mine_teleop::random_token(6) + ".jsonl");
  config.audit_log_path = audit_path.string();
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;

  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "device-secret"},
       {"connection_id", "vehicle-test-connection"}});
  expect(online.value("state", "") == "online", "vehicle did not register online");
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
  const auto login = http.post_json_response(
      base + "/auth/driver_login", {{"driver_id", "driver-1"}, {"password", "secret"}});
  const auto driver_token = login.at("token").get<std::string>();
  const auto session = http.post_json_response(
      base + "/sessions", {{"driver_id", "driver-1"}, {"vehicle_id", "vehicle-001"}, {"token", driver_token}});
  expect(session.value("state", "") == "active", "new session did not become active");
  const auto vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=device-secret&connection_generation=" +
      std::to_string(vehicle_generation));
  expect(
      vehicle_session.value("control_token", "") == session.value("control_token", ""),
      "vehicle did not receive the current control token");

  const auto second_login = http.post_json_response(
      base + "/auth/driver_login", {{"driver_id", "driver-2"}, {"password", "secret-2"}});
  const auto second_driver_token = second_login.at("token").get<std::string>();
  const auto conflict = http.post_json(
      base + "/sessions",
      {{"driver_id", "driver-2"}, {"vehicle_id", "vehicle-001"}, {"token", second_driver_token}});
  expect(conflict.status == 409, "second driver stole an active vehicle session");

  const auto session_id = session.at("session_id").get<std::string>();
  auto missing_metadata = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "driver-1",
      "vehicle-001",
      "token",
      driver_token,
      "media_capabilities",
      {{"codecs", {"h264", "h265"}}});
  missing_metadata.erase("driver_id");
  expect(
      http.post_json(base + "/signaling/" + session_id + "/messages", missing_metadata).status == 400,
      "signaling message without required protocol metadata was accepted");

  auto wrong_version = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "driver-1",
      "vehicle-001",
      "token",
      driver_token,
      "media_capabilities",
      {{"codecs", {"h264", "h265"}}});
  wrong_version["protocol_version"] = mine_teleop::kProtocolVersion + 1;
  expect(
      http.post_json(base + "/signaling/" + session_id + "/messages", wrong_version).status == 400,
      "signaling message with an unsupported protocol version was accepted");

  auto media_request = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "driver-1",
      "vehicle-001",
      "token",
      driver_token,
      "media_capabilities",
      {{"codecs", {"h264", "h265"}}});
  media_request["future_extension"] = {{"safe_to_ignore", true}};
  const auto media = http.post_json_response(base + "/signaling/" + session_id + "/messages", media_request);
  expect(media.value("queued", 0) == 1, "media capabilities were not queued");
  const auto duplicate_media =
      http.post_json_response(base + "/signaling/" + session_id + "/messages", media_request);
  expect(
      duplicate_media.value("duplicate", false) &&
          duplicate_media.value("message_id", "") == media.value("message_id", "") &&
          duplicate_media.value("delivery_cursor", std::uint64_t{0}) ==
              media.value("delivery_cursor", std::uint64_t{0}),
      "identical signaling retry did not return its stable acknowledgement");
  auto conflicting_media = media_request;
  conflicting_media["payload"] = {{"codecs", {"h264"}}};
  expect(
      http.post_json(base + "/signaling/" + session_id + "/messages", conflicting_media).status == 409,
      "signaling sequence reuse with different content was accepted");

  expect(
      http.post_json(
              base + "/signaling/" + session_id + "/messages",
              signaling_request(
                  "vehicle-001",
                  "driver-1",
                  session_id,
                  2,
                  mine_teleop::now_ms(),
                  "driver-1",
                  "vehicle-001",
                  "token",
                  driver_token,
                  "webrtc_answer",
                  {{"type", "answer"}, {"sdp", std::string(65, 's')}}))
              .status == 400,
      "oversized WebRTC SDP was accepted");
  expect(
      http.post_json(
              base + "/signaling/" + session_id + "/messages",
              signaling_request(
                  "vehicle-001",
                  "driver-1",
                  session_id,
                  2,
                  mine_teleop::now_ms(),
                  "driver-1",
                  "vehicle-001",
                  "token",
                  driver_token,
                  "ice_candidate",
                  {{"candidate", std::string(33, 'c')}}))
              .status == 400,
      "oversized WebRTC ICE candidate was accepted");

  auto control = command(2, mine_teleop::now_ms());
  control.session_id = session.at("session_id").get<std::string>();
  control.driver_id = "driver-1";
  control.control_token = session.at("control_token").get<std::string>();
  const auto rejected_control_route = http.post_json(
      base + "/signaling/" + control.session_id + "/messages",
      signaling_request(
          control.vehicle_id,
          control.driver_id,
          control.session_id,
          control.seq,
          control.sent_at_utc_ms,
          "driver-1",
          "vehicle-001",
          "token",
          driver_token,
          "control_command",
          control.to_json()));
  expect(rejected_control_route.status == 400, "signaling server accepted a DataChannel control command");

  const auto media_messages = http.get_json(
      base + "/signaling/" + control.session_id +
      "/messages?recipient=vehicle-001&device_token=device-secret&connection_generation=" +
      std::to_string(vehicle_generation) + "&types=media_capabilities");
  expect(media_messages.at("messages").size() == 1, "control polling consumed WebRTC capabilities");
  expect(media_messages.at("messages").at(0).value("type", "") == "media_capabilities", "media message type changed");
  auto stale_offer = signaling_request(
      "vehicle-001",
      "driver-1",
      session_id,
      1,
      mine_teleop::now_ms(),
      "vehicle-001",
      "driver-1",
      "device_token",
      "device-secret",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0"}});
  stale_offer["connection_generation"] = vehicle_generation;
  expect(
      http.post_json_response(base + "/signaling/" + session_id + "/messages", stale_offer).value("queued", 0) == 1,
      "fresh WebRTC offer was not queued");
  std::this_thread::sleep_for(std::chrono::milliseconds(70));
  const auto expired_offers = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=driver-1&token=" +
      http.url_encode(driver_token) + "&types=webrtc_offer");
  expect(expired_offers.at("messages").empty(), "expired WebRTC offer was redelivered after reconnect polling");
  const auto fallback = http.post_json_response(
      base + "/signaling/" + control.session_id + "/messages",
      signaling_request(
          "vehicle-001",
          "driver-1",
          control.session_id,
          3,
          mine_teleop::now_ms(),
          "driver-1",
          "vehicle-001",
          "token",
          driver_token,
          "media_fallback",
          {{"codec", "h264"}, {"reason", "decode_fps_below_20"}}));
  expect(fallback.value("queued", 0) == 1, "media fallback was not queued");
  const auto fallback_messages = http.get_json(
      base + "/signaling/" + control.session_id +
      "/messages?recipient=vehicle-001&device_token=device-secret&connection_generation=" +
      std::to_string(vehicle_generation) + "&types=media_fallback");
  expect(fallback_messages.at("messages").size() == 1, "vehicle did not receive media fallback");
  expect(
      fallback_messages.at("messages").at(0).value("type", "") == "media_fallback",
      "media fallback message type changed");
  const auto ended = http.post_json_response(
      base + "/sessions/" + control.session_id + "/end",
      {{"actor", "driver-1"}, {"token", driver_token}, {"reason", "test_complete"}});
  expect(ended.value("state", "") == "closed", "ended session did not reach closed state");
  expect(!ended.contains("control_token"), "closed session exposed its old control token");

  const auto replacement = http.post_json_response(
      base + "/sessions",
      {{"driver_id", "driver-2"}, {"vehicle_id", "vehicle-001"}, {"token", second_driver_token}});
  expect(replacement.value("state", "") == "active", "replacement session did not become active");
  expect(
      replacement.value("control_token", "") != control.control_token,
      "replacement session reused the previous control token");
  auto old_token_command = command(1, mine_teleop::now_ms());
  old_token_command.session_id = replacement.at("session_id").get<std::string>();
  old_token_command.driver_id = "driver-2";
  old_token_command.control_token = control.control_token;
  mine_teleop::ControlReceiver replacement_receiver(
      "vehicle-001",
      "driver-2",
      old_token_command.session_id,
      200,
      mine_teleop::kProtocolVersion,
      true,
      replacement.at("control_token").get<std::string>());
  const auto old_token_replay = replacement_receiver.accept(old_token_command, mine_teleop::now_ms());
  expect(
      !old_token_replay.accepted && old_token_replay.reason == "control_token_invalid",
      "replacement vehicle receiver accepted the previous control token");
  server.stop();

  std::ifstream audit_input(audit_path);
  const std::string audit_log((std::istreambuf_iterator<char>(audit_input)), std::istreambuf_iterator<char>());
  for (const std::string state : {"reserved", "connecting", "active", "stopping", "closed"}) {
    expect(audit_log.find("\"to\":\"" + state + "\"") != std::string::npos, "missing audited session state: " + state);
  }
  expect(
      audit_log.find("\"event\":\"signaling_retry_acknowledged\"") != std::string::npos,
      "idempotent signaling retry was not audited");
  expect(audit_log.find("control-token-") == std::string::npos, "audit log leaked a control token");
  std::filesystem::remove(audit_path);
}

void test_signaling_presence_generation_and_automatic_release() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-1", "secret-1"}, {"driver-2", "secret-2"}};
  config.device_tokens = {{"vehicle-1", "device-1"}, {"vehicle-2", "device-2"}};
  config.driver_vehicle_permissions = {
      {"driver-1", {"vehicle-1"}},
      {"driver-2", {"vehicle-2"}}};
  config.admin_token = "test-admin-token";
  config.control_token_ttl_ms = 200;
  config.vehicle_heartbeat_timeout_ms = 500;
  config.driver_heartbeat_timeout_ms = 500;
  config.connection_reaper_interval_ms = 5;
  config.stun_urls = {"stun:turn.example.test:3478"};
  config.turn_urls = {
      "turn:turn.example.test:3478?transport=udp",
      "turns:turn.example.test:5349?transport=tcp"};
  config.turn_realm = "teleop.example.test";
  config.turn_static_auth_secret = "test-static-auth-secret";
  config.turn_credential_ttl_seconds = 60;
  const auto audit_path = std::filesystem::path("/tmp") /
      ("mine-teleop-presence-audit-" + mine_teleop::random_token(6) + ".jsonl");
  config.audit_log_path = audit_path.string();
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;

  const auto register_vehicle = [&](std::string_view vehicle_id, std::string_view token, std::string_view connection_id) {
    return http.post_json_response(
        base + "/vehicles/online",
        {{"vehicle_id", vehicle_id}, {"device_token", token}, {"connection_id", connection_id}});
  };
  const auto login_driver = [&](std::string_view driver_id, std::string_view password) {
    return http.post_json_response(
        base + "/auth/driver_login", {{"driver_id", driver_id}, {"password", password}});
  };
  const auto create_session = [&](std::string_view driver_id, std::string_view vehicle_id, std::string_view token) {
    return http.post_json_response(
        base + "/sessions", {{"driver_id", driver_id}, {"vehicle_id", vehicle_id}, {"token", token}});
  };

  const auto vehicle_1 = register_vehicle("vehicle-1", "device-1", "vehicle-1-runtime-a");
  const auto vehicle_1_generation = vehicle_1.at("connection_generation").get<std::uint64_t>();
  const auto vehicle_1_refresh = register_vehicle("vehicle-1", "device-1", "vehicle-1-runtime-a");
  expect(
      vehicle_1_refresh.at("connection_generation").get<std::uint64_t>() == vehicle_1_generation,
      "same vehicle connection retry created a new generation");
  expect(
      vehicle_1_refresh.value("duplicate_policy", "") == "same_connection_refresh",
      "same vehicle connection retry policy is not explicit");
  const auto vehicle_2 = register_vehicle("vehicle-2", "device-2", "vehicle-2-runtime-a");
  const auto vehicle_2_generation = vehicle_2.at("connection_generation").get<std::uint64_t>();

  const auto driver_1 = login_driver("driver-1", "secret-1");
  const auto driver_2 = login_driver("driver-2", "secret-2");
  const auto driver_1_token = driver_1.at("token").get<std::string>();
  auto driver_2_token = driver_2.at("token").get<std::string>();
  const auto session_1 = create_session("driver-1", "vehicle-1", driver_1_token);
  expect(
      http.post_json(
              base + "/sessions",
              {{"driver_id", "driver-1"}, {"vehicle_id", "vehicle-2"}, {"token", driver_1_token}})
              .status == 401,
      "driver created a session for a vehicle outside its permission list");
  const auto session_2 = create_session("driver-2", "vehicle-2", driver_2_token);
  expect(session_1.value("state", "") == "active" && session_2.value("state", "") == "active", "2x2 sessions did not become active");
  expect(
      http.post_json(
              base + "/auth/driver_login",
              {{"driver_id", "driver-1"}, {"password", "secret-1"}})
              .status == 409,
      "password-only duplicate driver login replaced an active connection");
  const auto health_2x2 = http.get_json(base + "/health");
  expect(health_2x2.value("online_vehicles", 0) == 2, "two vehicles were not simultaneously online");
  expect(health_2x2.value("online_drivers", 0) == 2, "two drivers were not simultaneously online");
  expect(health_2x2.value("active_sessions", 0) == 2, "two independent sessions were not simultaneously active");

  const auto driver_ice = http.get_json(
      base + "/sessions/" + session_1.at("session_id").get<std::string>() +
      "/ice_servers?actor=driver-1&token=" + http.url_encode(driver_1_token));
  const auto vehicle_ice = http.get_json(
      base + "/sessions/" + session_1.at("session_id").get<std::string>() +
      "/ice_servers?actor=vehicle-1&device_token=device-1&connection_generation=" +
      std::to_string(vehicle_1_generation));
  expect(driver_ice.at("ice_servers").size() == 2, "driver did not receive STUN and TURN ICE entries");
  expect(vehicle_ice.at("ice_servers").size() == 2, "vehicle did not receive STUN and TURN ICE entries");
  expect(
      driver_ice.at("ice_servers").at(0).at("urls") == vehicle_ice.at("ice_servers").at(0).at("urls") &&
          driver_ice.at("ice_servers").at(1).at("urls") == vehicle_ice.at("ice_servers").at(1).at("urls"),
      "driver and vehicle received different ICE endpoints");
  const auto driver_turn_username = driver_ice.at("ice_servers").at(1).at("username").get<std::string>();
  const auto vehicle_turn_username = vehicle_ice.at("ice_servers").at(1).at("username").get<std::string>();
  const auto driver_turn_credential = driver_ice.at("ice_servers").at(1).at("credential").get<std::string>();
  expect(
      driver_turn_username.find(session_1.at("session_id").get<std::string>()) != std::string::npos &&
          driver_turn_username.ends_with(":driver-1"),
      "driver TURN username is not bound to its session and actor");
  expect(
      vehicle_turn_username.ends_with(":vehicle-1") && vehicle_turn_username != driver_turn_username,
      "vehicle TURN username is not independently actor-bound");
  expect(!driver_turn_credential.empty(), "TURN REST credential is empty");
  expect(
      driver_ice.value("expires_at_utc_ms", 0LL) > mine_teleop::now_ms(),
      "TURN REST credential is not short-lived");

  const auto cross_vehicle = http.post_json(
      base + "/signaling/" + session_2.at("session_id").get<std::string>() + "/messages",
      signaling_request(
          "vehicle-2",
          "driver-2",
          session_2.at("session_id").get<std::string>(),
          1,
          mine_teleop::now_ms(),
          "driver-1",
          "vehicle-2",
          "token",
          driver_1_token,
          "media_capabilities",
          {{"codecs", {"h264"}}}));
  expect(cross_vehicle.status == 401, "driver from another session crossed the vehicle boundary");

  const auto revoked_driver = http.post_json_response(
      base + "/admin/revoke/driver",
      {{"admin_token", "test-admin-token"}, {"id", "driver-2"}});
  expect(revoked_driver.value("state", "") == "revoked", "admin did not revoke one driver");
  const auto health_after_driver_revoke = http.get_json(base + "/health");
  expect(
      health_after_driver_revoke.value("online_drivers", 0) == 1 &&
          health_after_driver_revoke.value("active_sessions", 0) == 1,
      "revoking driver-2 affected driver-1 or failed to close driver-2 session");
  expect(
      http.post_json(
              base + "/auth/driver_heartbeat",
              {{"driver_id", "driver-2"}, {"token", driver_2_token}})
              .status == 401,
      "revoked driver token remained valid");
  static_cast<void>(http.post_json_response(
      base + "/admin/restore/driver",
      {{"admin_token", "test-admin-token"}, {"id", "driver-2"}}));
  driver_2_token = login_driver("driver-2", "secret-2").at("token").get<std::string>();
  const auto restored_driver_session = create_session("driver-2", "vehicle-2", driver_2_token);
  expect(restored_driver_session.value("state", "") == "active", "restored driver could not regain permitted control");

  const auto vehicle_1_replacement = register_vehicle("vehicle-1", "device-1", "vehicle-1-runtime-b");
  const auto replacement_generation = vehicle_1_replacement.at("connection_generation").get<std::uint64_t>();
  expect(replacement_generation != vehicle_1_generation, "replacement vehicle connection reused its old generation");
  expect(
      vehicle_1_replacement.value("duplicate_policy", "") == "replace_previous_connection",
      "replacement vehicle connection policy is not explicit");
  expect(
      http.post_json(
              base + "/vehicles/heartbeat",
              {{"vehicle_id", "vehicle-1"},
               {"device_token", "device-1"},
               {"connection_generation", vehicle_1_generation}})
              .status == 409,
      "stale vehicle generation refreshed the replacement connection");
  expect(
      http.get(
              base + "/vehicles/vehicle-1/session?device_token=device-1&connection_generation=" +
              std::to_string(vehicle_1_generation))
              .status == 409,
      "stale vehicle generation discovered a replacement session");

  const auto replacement_session = create_session("driver-1", "vehicle-1", driver_1_token);
  expect(
      replacement_session.value("control_token", "") != session_1.value("control_token", ""),
      "replacement session reused the closed session control token");

  const auto logout = http.post_json_response(
      base + "/auth/driver_logout",
      {{"driver_id", "driver-2"}, {"token", driver_2_token}, {"reason", "browser_page_closed"}});
  expect(logout.value("state", "") == "offline", "driver logout did not transition offline");
  const auto vehicle_2_session = http.get_json(
      base + "/vehicles/vehicle-2/session?device_token=device-2&connection_generation=" +
      std::to_string(vehicle_2_generation));
  expect(vehicle_2_session.value("session_id", "").empty(), "driver logout did not release vehicle control authority");
  expect(
      http.post_json(
              base + "/auth/driver_heartbeat",
              {{"driver_id", "driver-2"}, {"token", driver_2_token}})
              .status == 401,
      "logged-out driver token remained valid");
  const auto revoked_vehicle = http.post_json_response(
      base + "/admin/revoke/vehicle",
      {{"admin_token", "test-admin-token"}, {"id", "vehicle-2"}});
  expect(revoked_vehicle.value("state", "") == "revoked", "admin did not revoke one vehicle");
  const auto health_after_vehicle_revoke = http.get_json(base + "/health");
  expect(
      health_after_vehicle_revoke.value("online_vehicles", 0) == 1,
      "revoking vehicle-2 affected vehicle-1");
  expect(
      http.post_json(
              base + "/vehicles/online",
              {{"vehicle_id", "vehicle-2"},
               {"device_token", "device-2"},
               {"connection_id", "vehicle-2-runtime-b"}})
              .status == 401,
      "revoked vehicle unexpectedly registered online");
  static_cast<void>(http.post_json_response(
      base + "/admin/restore/vehicle",
      {{"admin_token", "test-admin-token"}, {"id", "vehicle-2"}}));

  const auto control_expiry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(600);
  mine_teleop::Json after_control_expiry;
  do {
    after_control_expiry = http.get_json(base + "/health");
    if (after_control_expiry.value("active_sessions", 0) == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < control_expiry_deadline);
  expect(after_control_expiry.value("active_sessions", 0) == 0, "short-lived control token did not release its session");

  const auto vehicle_1_after_expiry = http.get_json(
      base + "/vehicles/vehicle-1/session?device_token=device-1&connection_generation=" +
      std::to_string(replacement_generation));
  expect(vehicle_1_after_expiry.value("session_id", "").empty(), "expired control token still exposed an active session");
  static_cast<void>(http.post_json_response(
      base + "/auth/driver_heartbeat", {{"driver_id", "driver-1"}, {"token", driver_1_token}}));

  const auto heartbeat_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(700);
  mine_teleop::Json after_heartbeat_timeout;
  do {
    after_heartbeat_timeout = http.get_json(base + "/health");
    if (after_heartbeat_timeout.value("online_vehicles", 0) == 0 &&
        after_heartbeat_timeout.value("online_drivers", 0) == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < heartbeat_deadline);
  expect(after_heartbeat_timeout.value("online_vehicles", 0) == 0, "vehicle heartbeat timeout did not transition offline");
  expect(after_heartbeat_timeout.value("online_drivers", 0) == 0, "driver heartbeat timeout did not transition offline");
  expect(
      http.get(
              base + "/vehicles/vehicle-1/session?device_token=device-1&connection_generation=" +
              std::to_string(replacement_generation))
              .status == 409,
      "offline vehicle connection was reported as an authentication failure instead of a recoverable conflict");
  server.stop();

  std::ifstream audit_input(audit_path);
  const std::string audit_log((std::istreambuf_iterator<char>(audit_input)), std::istreambuf_iterator<char>());
  for (const std::string event : {
           "vehicle_connection_replaced",
           "driver_logout",
           "driver_revoked",
           "driver_restored",
           "vehicle_revoked",
           "vehicle_restored",
           "control_authority_expired",
           "ice_servers_issued",
           "vehicle_offline",
           "driver_offline"}) {
    expect(audit_log.find("\"event\":\"" + event + "\"") != std::string::npos, "missing presence audit event: " + event);
  }
  expect(audit_log.find("driver-token-") == std::string::npos, "presence audit leaked a driver token");
  expect(audit_log.find("control-token-") == std::string::npos, "presence audit leaked a control token");
  expect(audit_log.find(driver_turn_credential) == std::string::npos, "presence audit leaked a TURN credential");
  std::filesystem::remove(audit_path);
}

void test_signaling_time_sync_common_domain() {
  auto service = std::make_shared<mine_teleop::SignalingService>(mine_teleop::SignalingServerConfig{});
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  mine_teleop::SynchronizedClock clock;
  const auto status = clock.synchronize(http, base, 7);
  expect(status.synchronized, "clock did not synchronize to signaling time");
  expect(status.sample_count == 7, "clock synchronization sample count changed");
  expect(status.acceptable(25), "local signaling clock uncertainty exceeded 25ms");
  const auto first = clock.now_ms();
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  const auto second = clock.now_ms();
  expect(second >= first, "synchronized clock moved backwards");
  const auto direct = http.get_json(base + "/time?client_send_ms=12345");
  expect(direct.value("client_send_ms", 0) == 12345, "time endpoint did not echo the client timestamp");
  expect(direct.value("time_domain", "") == "signaling_server", "time endpoint domain changed");
  server.stop();
}

void test_driver_config_and_hardware_encoder_priority() {
  const auto config = mine_teleop::load_driver_config("configs/driver-console.dev.yaml");
  expect(config.driver_id == "driver-console-001", "driver config id mismatch");
  const auto vehicle = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  expect(vehicle.hardware.preferred_encoder == "nvenc", "NVIDIA is not the preferred encoder");
  expect(vehicle.hardware.fallback_encoder == "vaapi", "Intel VAAPI is not the fallback encoder");
  expect(vehicle.hardware.preferred_codec == "h265", "H.265 is not the preferred codec");
  const auto candidates = mine_teleop::encoder_candidate_order(vehicle.hardware, mine_teleop::VideoCodec::H265);
  expect(candidates.size() == 2, "hardware encoder fallback candidate is missing");
  expect(candidates.at(0).backend == mine_teleop::EncoderBackend::Nvenc, "NVENC priority changed");
  expect(candidates.at(1).backend == mine_teleop::EncoderBackend::Vaapi, "VAAPI fallback priority changed");
  expect(candidates.at(0).codec == mine_teleop::VideoCodec::H265, "preferred candidate codec changed");
}

void test_native_testsrc_acquisition_does_not_spawn_ffmpeg() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  const auto camera = config.enabled_cameras().front();
  auto capture_profile = config.realtime_profile(camera.realtime_profile);
  capture_profile.codec = "mjpeg";
  capture_profile.encoder = "native";
  mine_teleop::CameraFrameSource source(camera, capture_profile);
  expect(source.command().empty(), "native test source unexpectedly configured an external media process");
  const auto frame = source.next(1);
  expect(frame.codec == "mjpeg", "native test source did not produce MJPEG");
  expect(frame.payload.size() > 100, "native test source produced an unexpectedly small JPEG");
  expect(
      static_cast<unsigned char>(frame.payload.front()) == 0xFF &&
          static_cast<unsigned char>(frame.payload.at(1)) == 0xD8 &&
          static_cast<unsigned char>(frame.payload.at(frame.payload.size() - 2)) == 0xFF &&
          static_cast<unsigned char>(frame.payload.back()) == 0xD9,
      "native test source payload is not a complete JPEG");
}

void test_basler_camera_uses_minimal_aravis_bridge() {
  const auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto camera = config.enabled_cameras().front();
  camera.device = "basler:serial=25192546";
  auto capture_profile = config.realtime_profile(camera.realtime_profile);
  capture_profile.codec = "mjpeg";
  capture_profile.encoder = "native";
  mine_teleop::CameraFrameSource source(camera, capture_profile);
  const auto& command = source.command();
  expect(!command.empty(), "Basler camera did not configure an Aravis bridge");
  expect(
      std::filesystem::path(command.front()).filename() == "mine-teleop-aravis-camera",
      "Basler camera selected a non-Aravis bridge");
  expect(
      std::find(command.begin(), command.end(), "--serial") != command.end() &&
          std::find(command.begin(), command.end(), "25192546") != command.end(),
      "Aravis bridge did not preserve the Basler serial selector");
  expect(
      std::find(command.begin(), command.end(), "--jpeg-quality") != command.end(),
      "Aravis bridge did not receive the bounded JPEG quality setting");
}

void test_native_driver_to_vehicle_data_channel_payload() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "dev-device-secret"}};
  signaling_config.control_token_ttl_ms = 100;
  signaling_config.connection_reaper_interval_ms = 5;
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());

  auto vehicle_config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "dev-device-secret"},
       {"connection_id", "data-channel-test-vehicle"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
  auto driver_config = mine_teleop::load_driver_config("configs/driver-console.dev.yaml");
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connection = driver.connect();
  expect(connection.value("connected", false), "driver failed to connect");
  const auto reconnected = driver.connect();
  expect(
      reconnected.value("session_id", "") == connection.value("session_id", ""),
      "driver reconnect did not reuse the active session");
  const auto vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=dev-device-secret&connection_generation=" +
      std::to_string(vehicle_generation));
  const auto prepared = driver.send_control({{"gear", "D"}, {"steering", 0.1}, {"throttle", 0.2}, {"brake", 0.0}});
  expect(prepared.value("prepared", false), "driver did not prepare a DataChannel command");
  expect(prepared.value("transport", "") == "webrtc_data_channel", "driver selected the wrong control transport");
  const auto control = mine_teleop::ControlCommand::from_json(prepared.at("command"));
  expect(control.session_id == connection.value("session_id", ""), "DataChannel command used the wrong session");
  expect(
      control.control_token == vehicle_session.value("control_token", ""),
      "DataChannel command did not use the server-issued control token");
  mine_teleop::VehicleControlService receiver(
      vehicle_config,
      "driver-console-001",
      control.session_id,
      vehicle_session.at("control_token").get<std::string>(),
      std::make_unique<mine_teleop::MockVehicleAdapter>());
  const auto received_at_ms = mine_teleop::now_ms();
  receiver.start(received_at_ms);
  const auto applied = receiver.receive_command(control, received_at_ms);
  expect(applied.accepted, "vehicle receiver did not accept the DataChannel command payload");
  const auto duplicate = receiver.receive_command(control, received_at_ms + 1);
  expect(!duplicate.accepted && duplicate.reason == "old_seq", "vehicle receiver accepted a duplicate command");
  receiver.close();
  const auto expiry_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1000);
  mine_teleop::Json after_expiry;
  do {
    after_expiry = http.get_json(base + "/health");
    if (after_expiry.value("active_sessions", 0) == 0) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  } while (std::chrono::steady_clock::now() < expiry_deadline);
  expect(after_expiry.value("active_sessions", 0) == 0, "server did not expire the short-lived control session");
  const auto renewed_connection = driver.connect();
  expect(
      renewed_connection.value("session_id", "") != connection.value("session_id", ""),
      "driver console reused a server-expired local session");
  const auto disconnected = driver.disconnect("test_disconnect");
  expect(disconnected.value("state", "") == "offline", "driver console disconnect did not release authority");
  const auto released_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=dev-device-secret&connection_generation=" +
      std::to_string(vehicle_generation));
  expect(released_session.value("session_id", "").empty(), "vehicle still discovered a session after driver disconnect");
  server.stop();
}

void test_driver_login_lists_only_authorized_vehicles() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {
      {"driver-console-001", "dev-password"},
      {"driver-console-002", "other-password"},
  };
  signaling_config.device_tokens = {
      {"vehicle-001", "vehicle-secret-1"},
      {"vehicle-002", "vehicle-secret-2"},
  };
  signaling_config.driver_vehicle_permissions = {
      {"driver-console-001", {"vehicle-001"}},
      {"driver-console-002", {"vehicle-002"}},
  };
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "authorized-list-vehicle-1"}}));
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-002"},
       {"device_token", "vehicle-secret-2"},
       {"connection_id", "authorized-list-vehicle-2"}}));

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto login = driver.login("dev-password");
  expect(login.value("authenticated", false), "driver login did not authenticate");
  expect(!login.contains("token"), "local page response exposed the driver token");
  const auto vehicles = login.at("vehicles");
  expect(vehicles.size() == 1, "driver saw a vehicle outside its allowlist");
  expect(vehicles.at(0).value("vehicle_id", "") == "vehicle-001", "authorized vehicle list returned the wrong vehicle");
  expect(vehicles.at(0).value("online", false), "authorized online vehicle was reported offline");
  expect(vehicles.at(0).value("controllable", false), "authorized idle vehicle was not controllable");
  bool unauthorized_rejected = false;
  try {
    static_cast<void>(driver.connect("vehicle-002"));
  } catch (const std::invalid_argument&) {
    unauthorized_rejected = true;
  }
  expect(unauthorized_rejected, "driver selected a vehicle outside its allowlist");
  static_cast<void>(driver.disconnect("authorized_list_test"));
  server.stop();
}

void test_driver_console_page_keeps_waiting_state_during_background_safety_ticks() {
  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001";
  config.signaling_url = "http://127.0.0.1:1";
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "dev-password");
  mine_teleop::DriverConsoleHttpApp app(runtime);
  mine_teleop::HttpRequest request;
  request.method = "GET";
  request.path = "/";
  const auto response = app.handle(request);
  expect(response.status == 200, "driver console page did not load");
  expect(
      response.body.find("async function send(extra={},announceUnavailable=true)") != std::string::npos,
      "driver console page cannot distinguish background safety ticks from user control attempts");
  expect(
      response.body.find("await send({},false)") != std::string::npos,
      "background safety tick still announces a control fault while waiting for media");
  expect(
      response.body.find("webrtcLabel.textContent='等待车端媒体'") != std::string::npos,
      "driver console page does not expose the pending vehicle-media state");
}

void test_local_archive_uploader_is_atomic_and_resumable() {
  const auto root = std::filesystem::path("/tmp") / ("mine-teleop-upload-test-" + mine_teleop::random_token(6));
  const auto recordings = root / "recordings";
  const auto archive = root / "archive";
  const auto segment_dir = recordings / "vehicle-001" / "session-001" / "front";
  std::filesystem::create_directories(segment_dir);
  const auto video = segment_dir / "segment-001.mp4";
  const auto metadata = segment_dir / "segment-001.json";
  {
    std::ofstream output(video, std::ios::binary);
    output << "native-segment-payload";
  }
  {
    std::ofstream output(metadata);
    output << mine_teleop::Json({
        {"vehicle_id", "vehicle-001"},
        {"session_id", "session-001"},
        {"camera_id", "front"},
        {"segment_id", "segment-001"},
        {"upload_state", "pending"},
    }).dump();
  }
  mine_teleop::LocalArchiveUploader uploader(recordings, archive);
  const auto result = uploader.process_once();
  expect(result.action == "uploaded", "pending segment was not archived");
  expect(std::filesystem::is_regular_file(archive / result.object_path), "archived video is missing");
  expect(mine_teleop::sha256_file(video) == mine_teleop::sha256_file(archive / result.object_path), "archive hash mismatch");
  expect(uploader.process_once().action == "idle", "uploaded segment was processed twice");
  std::filesystem::remove_all(root);
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"config_loads_current_vehicle_yaml", test_config_loads_current_vehicle_yaml},
      {"bench_config_drives_unified_vehicle_runtime", test_bench_config_drives_unified_vehicle_runtime},
      {"field_config_pins_tls_route_without_system_dns", test_field_config_pins_tls_route_without_system_dns},
      {"control_command_json_round_trip_and_validation", test_control_command_json_round_trip_and_validation},
      {"shared_protocol_v1_vectors_and_session_states", test_shared_protocol_v1_vectors_and_session_states},
      {"control_receiver_enforces_token_sequence_and_gap", test_control_receiver_enforces_token_sequence_and_gap},
      {"mailbox_keeps_only_latest_command", test_mailbox_keeps_only_latest_command},
      {"safety_timeout_profile_and_estop_latch", test_safety_timeout_profile_and_estop_latch},
      {"control_service_reports_safe_stop_output_after_timeout", test_control_service_reports_safe_stop_output_after_timeout},
      {"control_service_bounds_telemetry_history", test_control_service_bounds_telemetry_history},
      {"control_service_requires_feedback_before_actuation_but_allows_estop", test_control_service_requires_feedback_before_actuation_but_allows_estop},
      {"fault_output_fails_safe", test_fault_output_fails_safe},
      {"native_signaling_webrtc_message_isolation", test_native_signaling_webrtc_message_isolation},
      {"signaling_presence_generation_and_automatic_release", test_signaling_presence_generation_and_automatic_release},
      {"signaling_time_sync_common_domain", test_signaling_time_sync_common_domain},
      {"driver_config_and_hardware_encoder_priority", test_driver_config_and_hardware_encoder_priority},
      {"native_testsrc_acquisition_does_not_spawn_ffmpeg", test_native_testsrc_acquisition_does_not_spawn_ffmpeg},
      {"basler_camera_uses_minimal_aravis_bridge", test_basler_camera_uses_minimal_aravis_bridge},
      {"native_driver_to_vehicle_data_channel_payload", test_native_driver_to_vehicle_data_channel_payload},
      {"driver_console_page_keeps_waiting_state_during_background_safety_ticks", test_driver_console_page_keeps_waiting_state_during_background_safety_ticks},
      {"driver_login_lists_only_authorized_vehicles", test_driver_login_lists_only_authorized_vehicles},
      {"local_archive_uploader_is_atomic_and_resumable", test_local_archive_uploader_is_atomic_and_resumable},
  };
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << "SUMMARY passed=" << (tests.size() - static_cast<std::size_t>(failures))
            << " failed=" << failures << '\n';
  return failures == 0 ? 0 : 1;
}
