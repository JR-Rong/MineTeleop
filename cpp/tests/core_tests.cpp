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

ControlCommand command(std::uint64_t seq = 1, std::int64_t timestamp_ms = 0) {
  ControlCommand value;
  value.vehicle_id = "vehicle-001";
  value.session_id = "session-001";
  value.seq = seq;
  value.ts_ms = timestamp_ms;
  value.gear = "D";
  value.steering = 0.25;
  value.throttle = 0.5;
  value.brake = 0.0;
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

void test_control_command_json_round_trip_and_validation() {
  const auto original = command(7, 1234);
  const auto parsed = ControlCommand::from_json(original.to_json());
  expect(parsed.seq == 7, "sequence did not round trip");
  expect_near(parsed.steering, 0.25, 1e-9, "steering did not round trip");
  auto invalid = original;
  invalid.throttle = 1.5;
  expect_throws([&] { invalid.validate(); }, "invalid throttle was accepted");
}

void test_control_receiver_enforces_token_sequence_and_gap() {
  mine_teleop::ControlReceiver receiver("vehicle-001", "session-001", 200, 1, true, "token");
  auto first = command(1, 0);
  first.authority_token = "wrong";
  expect(receiver.accept(first, 0).reason == "control_token_invalid", "wrong token was accepted");
  first.authority_token = "token";
  expect(receiver.accept(first, 0).accepted, "first command was rejected");
  expect(receiver.accept(first, 50).reason == "old_seq", "old sequence was accepted");
  auto late = command(2, 500);
  late.authority_token = "token";
  expect(receiver.accept(late, 500).reason == "command_gap_exceeded", "large command gap was accepted");
  late.estop = true;
  expect(receiver.accept(late, 500).accepted, "estop should bypass command gap rejection");

  mine_teleop::ControlReceiver recovery_receiver("vehicle-001", "session-001", 200, 1, true, "token");
  auto recovery_first = command(1, 0);
  recovery_first.authority_token = "token";
  expect(recovery_receiver.accept(recovery_first, 0).accepted, "recovery first command was rejected");
  auto recovery_gap = command(2, 500);
  recovery_gap.authority_token = "token";
  expect(recovery_receiver.accept(recovery_gap, 500).reason == "command_gap_exceeded", "recovery gap was not detected");
  auto recovery_next = command(3, 550);
  recovery_next.authority_token = "token";
  expect(recovery_receiver.accept(recovery_next, 550).accepted, "receiver did not recover after command gap");

  mine_teleop::ControlReceiver synchronized_receiver("vehicle-001", "session-001", 200, 1, true, "token");
  auto stale = command(1, 0);
  stale.authority_token = "token";
  expect(synchronized_receiver.accept(stale, 201).reason == "command_age_exceeded", "stale command was accepted");
  stale.ts_ms = 201;
  stale.estop = true;
  expect(synchronized_receiver.accept(stale, 500).accepted, "stale estop should remain acceptable");

  mine_teleop::ControlReceiver future_receiver("vehicle-001", "session-001", 200, 1, true, "token");
  auto future = command(1, 201);
  future.authority_token = "token";
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
  mine_teleop::VehicleControlService service(config, "session-001", "", std::move(adapter), 100);
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

void test_control_service_requires_feedback_before_actuation_but_allows_estop() {
  auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  config.field_safety.require_can_feedback_before_control = true;
  auto adapter = std::make_unique<NoFeedbackAdapter>();
  auto* adapter_view = adapter.get();
  mine_teleop::VehicleControlService service(config, "session-001", "", std::move(adapter), 100);
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

void test_native_signaling_http_control_round_trip() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-1", "secret"}};
  config.device_tokens = {{"vehicle-001", "device-secret"}};
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1", 0, [service](const auto& request) { return service->handle(request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;

  const auto online = http.post_json_response(
      base + "/vehicles/online", {{"vehicle_id", "vehicle-001"}, {"device_token", "device-secret"}});
  expect(online.value("state", "") == "online", "vehicle did not register online");
  const auto login = http.post_json_response(
      base + "/auth/driver_login", {{"driver_id", "driver-1"}, {"password", "secret"}});
  const auto driver_token = login.at("token").get<std::string>();
  const auto session = http.post_json_response(
      base + "/sessions", {{"driver_id", "driver-1"}, {"vehicle_id", "vehicle-001"}, {"token", driver_token}});

  const auto media = http.post_json_response(
      base + "/signaling/" + session.at("session_id").get<std::string>() + "/messages",
      {{"sender", "driver-1"},
       {"recipient", "vehicle-001"},
       {"token", driver_token},
       {"type", "media_capabilities"},
       {"payload", {{"codecs", {"h264", "h265"}}}}});
  expect(media.value("queued", 0) == 1, "media capabilities were not queued");

  auto control = command(1, mine_teleop::now_ms());
  control.session_id = session.at("session_id").get<std::string>();
  control.authority_token = session.at("control_token").get<std::string>();
  const auto queued = http.post_json_response(
      base + "/signaling/" + control.session_id + "/messages",
      {{"sender", "driver-1"},
       {"recipient", "vehicle-001"},
       {"token", driver_token},
       {"type", "control_command"},
       {"payload", control.to_json()}});
  expect(queued.value("queued", 0) == 2, "control command was not queued behind media capabilities");

  const auto messages = http.get_json(
      base + "/signaling/" + control.session_id +
      "/messages?recipient=vehicle-001&device_token=device-secret&types=control_command");
  expect(messages.at("messages").size() == 1, "vehicle did not receive queued control command");
  expect(messages.at("messages").at(0).value("type", "") == "control_command", "message type changed");
  const auto media_messages = http.get_json(
      base + "/signaling/" + control.session_id +
      "/messages?recipient=vehicle-001&device_token=device-secret&types=media_capabilities");
  expect(media_messages.at("messages").size() == 1, "control polling consumed WebRTC capabilities");
  expect(media_messages.at("messages").at(0).value("type", "") == "media_capabilities", "media message type changed");
  const auto fallback = http.post_json_response(
      base + "/signaling/" + control.session_id + "/messages",
      {{"sender", "driver-1"},
       {"recipient", "vehicle-001"},
       {"token", driver_token},
       {"type", "media_fallback"},
       {"payload", {{"codec", "h264"}, {"reason", "decode_fps_below_20"}}}});
  expect(fallback.value("queued", 0) == 1, "media fallback was not queued");
  const auto fallback_messages = http.get_json(
      base + "/signaling/" + control.session_id +
      "/messages?recipient=vehicle-001&device_token=device-secret&types=media_fallback");
  expect(fallback_messages.at("messages").size() == 1, "vehicle did not receive media fallback");
  expect(
      fallback_messages.at("messages").at(0).value("type", "") == "media_fallback",
      "media fallback message type changed");
  server.stop();
}

void test_signaling_time_sync_common_domain() {
  auto service = std::make_shared<mine_teleop::SignalingService>(mine_teleop::SignalingServerConfig{});
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1", 0, [service](const auto& request) { return service->handle(request); });
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

void test_native_driver_to_vehicle_closed_loop() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "dev-device-secret"}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1", 0, [signaling](const auto& request) { return signaling->handle(request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());

  auto vehicle_config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  mine_teleop::VehicleTeleopRuntime vehicle(vehicle_config, base, "dev-device-secret");
  vehicle.register_online();
  auto driver_config = mine_teleop::load_driver_config("configs/driver-console.dev.yaml");
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connection = driver.connect();
  expect(connection.value("connected", false), "driver failed to connect");
  const auto reconnected = driver.connect();
  expect(
      reconnected.value("session_id", "") == connection.value("session_id", ""),
      "driver reconnect did not reuse the active session");
  const auto queued = driver.send_control({{"gear", "D"}, {"steering", 0.1}, {"throttle", 0.2}, {"brake", 0.0}});
  expect(queued.value("queued", 0) == 1, "driver control command was not queued");
  expect(vehicle.discover_session(mine_teleop::now_ms()), "vehicle did not discover native session");
  const auto applied = vehicle.poll_and_execute(mine_teleop::now_ms());
  expect(applied.value("applied_control_commands", 0) == 1, "vehicle did not apply native driver command");
  expect(vehicle.summary().value("processed_control_commands", 0) == 1, "closed-loop summary command count mismatch");
  server.stop();
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
      {"control_command_json_round_trip_and_validation", test_control_command_json_round_trip_and_validation},
      {"control_receiver_enforces_token_sequence_and_gap", test_control_receiver_enforces_token_sequence_and_gap},
      {"mailbox_keeps_only_latest_command", test_mailbox_keeps_only_latest_command},
      {"safety_timeout_profile_and_estop_latch", test_safety_timeout_profile_and_estop_latch},
      {"control_service_reports_safe_stop_output_after_timeout", test_control_service_reports_safe_stop_output_after_timeout},
      {"control_service_requires_feedback_before_actuation_but_allows_estop", test_control_service_requires_feedback_before_actuation_but_allows_estop},
      {"fault_output_fails_safe", test_fault_output_fails_safe},
      {"native_signaling_http_control_round_trip", test_native_signaling_http_control_round_trip},
      {"signaling_time_sync_common_domain", test_signaling_time_sync_common_domain},
      {"driver_config_and_hardware_encoder_priority", test_driver_config_and_hardware_encoder_priority},
      {"native_testsrc_acquisition_does_not_spawn_ffmpeg", test_native_testsrc_acquisition_does_not_spawn_ffmpeg},
      {"basler_camera_uses_minimal_aravis_bridge", test_basler_camera_uses_minimal_aravis_bridge},
      {"native_driver_to_vehicle_closed_loop", test_native_driver_to_vehicle_closed_loop},
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
