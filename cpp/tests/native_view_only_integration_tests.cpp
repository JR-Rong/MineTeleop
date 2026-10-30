#include "mine_teleop/server.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace mine_teleop;
using namespace std::chrono_literals;

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<typename Callback> void reject(Callback callback, const char* message) {
  try { callback(); } catch (const std::exception& error) {
    require(std::string_view(error.what()) == "control is disabled in receive-only mode", message);
    return;
  }
  throw std::runtime_error(message);
}
}

int main() {
  try {
    SignalingServerConfig config;
    config.driver_passwords = {{"viewer-test", "fixture-password"}};
    config.device_tokens = {{"vehicle-001", "fixture-token"}};
    config.driver_vehicle_permissions = {{"viewer-test", {"vehicle-001"}}};
    auto cloud = std::make_shared<SignalingService>(config);
    std::atomic<unsigned> media_websockets{0}, control_websockets{0};
    SimpleHttpServer server("127.0.0.1", 0,
        [cloud](const HttpRequest& request) { return cloud->handle(request); }, 8 * 1024 * 1024,
        [&](SocketHandle socket, const HttpRequest& request) {
          const auto it = request.query.find("send_only");
          if (it != request.query.end() && it->second == "1") ++control_websockets;
          else ++media_websockets;
          return cloud->handle_websocket(socket, request);
        });
    server.start();
    const auto base = "http://127.0.0.1:" + std::to_string(server.port());
    HttpClient http;
    static_cast<void>(http.post_json_response(base + "/vehicles/online",
        {{"vehicle_id", "vehicle-001"}, {"device_token", "fixture-token"},
         {"connection_id", "viewer-fixture"}, {"native_control_capabilities", kNativeControlCapabilities}}));
    DriverConfig driver_config;
    driver_config.driver_id = "viewer-test";
    driver_config.signaling_url = base;
    driver_config.receive_only = true;
    DriverConsoleRuntime viewer(driver_config, "vehicle-001", "fixture-password");
    const auto session = viewer.connect("vehicle-001", "native-viewer-test");
    require(session.value("connected", false), "viewer session did not connect");
    const auto reconnected = viewer.connect("vehicle-001", "native-viewer-test");
    require(reconnected.value("session_id", "") == session.value("session_id", ""),
        "viewer did not retain its active session on reconnect");
    std::this_thread::sleep_for(700ms);
    const auto status = viewer.status();
    require(media_websockets.load() > 0, "viewer media signaling was not connected");
    require(control_websockets.load() == 0, "viewer opened a control WebSocket");
    require(!status.at("native_control").value("intent_active", true), "viewer installed an input intent");
    require(status.at("native_control").value("commands_sent_total", 1) == 0, "viewer sent control commands");
    const Json empty = Json::object();
    reject([&] { static_cast<void>(viewer.update_control_intent(empty)); }, "viewer accepted control input");
    reject([&] { static_cast<void>(viewer.prepare_control_profile(empty)); }, "viewer prepared a control profile");
    reject([&] { static_cast<void>(viewer.send_control_management(empty)); }, "viewer accepted management input");
    reject([&] { static_cast<void>(viewer.control_transport_bridge(empty)); }, "viewer accepted control bridge input");
    reject([&] { static_cast<void>(viewer.emergency_stop_bridge(empty)); }, "viewer accepted stop bridge input");
    static_cast<void>(viewer.end_session("viewer_test_finished"));
    static_cast<void>(viewer.disconnect("viewer_test_finished"));
    auto deferred_config = driver_config;
    deferred_config.receive_only = false;
    deferred_config.defer_control_bootstrap = true;
    const auto prior_control_websockets = control_websockets.load();
    DriverConsoleRuntime deferred(deferred_config, "vehicle-001", "fixture-password");
    const auto deferred_session = deferred.connect("vehicle-001", "deferred-test");
    std::this_thread::sleep_for(700ms);
    require(control_websockets.load() == prior_control_websockets &&
        deferred.status().at("native_control").value("commands_sent_total", 1) == 0 &&
        !deferred.status().at("native_control").value("intent_active", true),
        "deferred client sent a control command before input");
    const auto input = deferred.update_control_intent({
        {"session_id", deferred_session.at("session_id")},
        {"session_generation", deferred_session.at("control_session_generation")},
        {"ui_instance_id", "deferred-test"}, {"ui_control_epoch", 0}, {"takeover", true},
        {"intent_seq", 1}, {"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0},
        {"estop", false}, {"critical_video_ready", false}, {"critical_video_sample_healthy", false},
        {"critical_video_bad_sample_revision", 0}, {"critical_video_brake_revision", 0}});
    require(input.value("accepted", false), "deferred neutral input rejected");
    const auto deferred_deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deferred_deadline &&
           deferred.status().at("native_control").value("commands_sent_total", 0) == 0)
      std::this_thread::sleep_for(10ms);
    require(deferred.status().at("native_control").value("commands_sent_total", 0) > 0,
        "deferred sender did not start after explicit input");
    static_cast<void>(deferred.end_session("deferred_test_finished"));
    static_cast<void>(deferred.disconnect("deferred_test_finished"));
    auto control_config = driver_config;
    control_config.receive_only = false;
    DriverConsoleRuntime control(control_config, "vehicle-001", "fixture-password");
    require(control.connect("vehicle-001", "control-test").value("connected", false),
        "normal control session did not connect");
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    bool normal_sender_observed = false;
    while (std::chrono::steady_clock::now() < deadline && !normal_sender_observed) {
      normal_sender_observed = control_websockets.load() > 0 &&
          control.status().at("native_control").value("commands_sent_total", 0) > 0;
      if (!normal_sender_observed) std::this_thread::sleep_for(10ms);
    }
    require(normal_sender_observed, "normal control sender or control WebSocket changed");
    static_cast<void>(control.end_session("control_test_finished"));
    std::cout << "PASS receive-only session has signaling, no control connection, sender or API\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
