#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/media.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/upload.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

using mine_teleop::ControlCommand;
using mine_teleop::Json;
using mine_teleop::VehicleConfig;

class Arguments {
 public:
  Arguments(int argc, char** argv, int start) {
    for (int index = start; index < argc; ++index) {
      std::string token(argv[index]);
      if (!token.starts_with("--")) {
        positional_.push_back(std::move(token));
        continue;
      }
      const auto equal = token.find('=');
      if (equal != std::string::npos) {
        values_[token.substr(0, equal)] = token.substr(equal + 1);
        continue;
      }
      if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--")) {
        values_[token] = argv[++index];
      } else {
        flags_.insert(std::move(token));
      }
    }
  }

  [[nodiscard]] bool has(std::string_view key) const {
    return flags_.contains(std::string(key)) || values_.contains(std::string(key));
  }

  [[nodiscard]] std::string value(std::string_view key, std::string fallback = {}) const {
    const auto found = values_.find(std::string(key));
    return found == values_.end() ? std::move(fallback) : found->second;
  }

  [[nodiscard]] int integer(std::string_view key, int fallback) const {
    const auto raw = value(key);
    if (raw.empty()) return fallback;
    std::size_t consumed = 0;
    const int parsed = std::stoi(raw, &consumed);
    if (consumed != raw.size()) throw std::invalid_argument(std::string(key) + " must be an integer");
    return parsed;
  }

 private:
  std::unordered_map<std::string, std::string> values_;
  std::unordered_set<std::string> flags_;
  std::vector<std::string> positional_;
};

std::string environment(std::string_view key) {
  const char* value = std::getenv(std::string(key).c_str());
  return value == nullptr ? "" : value;
}

void print_help() {
  std::cout << R"(Mine Teleop native C++ runtime

Usage:
  mine-teleop version
  mine-teleop config-check [--config PATH]
  mine-teleop vehicle-agent [options]
  mine-teleop vehicle-media-agent [options]
  mine-teleop vehicle-uploader [options]
  mine-teleop signaling-server [options]
  mine-teleop driver-console [options]
  mine-teleop http-health --url URL
  mine-teleop vehicle-online [options]
  mine-teleop control-smoke [options]

Vehicle options:
  --config PATH                 vehicle YAML (default configs/vehicle-agent.dev.yaml)
  --preflight                   validate local runtime/device prerequisites
  --adapter-status              open adapter, print status, close
  --run-loop                    run deterministic local control-loop smoke
  --teleop                      poll signaling and execute remote control commands
  --service                     run selected production agent until terminated
  --device-token TOKEN          or set MINE_TELEOP_DEVICE_TOKEN
  --signaling-http-url URL      defaults to cloud.signaling_url
  --duration-ms N               local smoke duration (default 1500)
  --disconnect-at-ms N          stop local commands at N (default 500)
  --teleop-duration-ms N        live teleop duration (default 5000)
  --teleop-poll-interval-ms N   signaling poll period (default 50)
  --teleop-session-wait-ms N    session discovery timeout (default 5000)
  --teleop-log-controls         emit accepted commands as JSONL

Signaling server options:
  --host ADDRESS                bind address (default 127.0.0.1)
  --port N                      bind port (default 8765)
  --driver-id ID                configured driver (default driver-console-001)
  --driver-password PASSWORD    or set MINE_TELEOP_DRIVER_PASSWORD
  --vehicle-id ID               configured vehicle (default vehicle-001)
  --device-token TOKEN          or set MINE_TELEOP_DEVICE_TOKEN
  --audit-log PATH              append native JSONL audit records

Driver console options:
  --config PATH                 driver YAML (default configs/driver-console.dev.yaml)
  --host ADDRESS                bind address (default 127.0.0.1)
  --port N                      bind port (default 8080)
  --vehicle-id ID               target vehicle (default vehicle-001)
  --driver-password PASSWORD    or set MINE_TELEOP_DRIVER_PASSWORD
  --signaling-http-url URL      override driver configuration

Vehicle media options:
  --config PATH                 vehicle YAML (default configs/vehicle-agent.dev.yaml)
  --driver-console-url URL      frame sink (default http://127.0.0.1:8080)
  --frames N                    frames per camera (default 30)
  --duration-ms N               optional duration limit
  --capture-interval-ms N       optional interval between capture rounds
  --frame-timeout-ms N          camera/ffmpeg timeout (default 3000)
  --record                      record H.264 MP4 segments while streaming
  --recording-root PATH         recording destination (defaults to config)

Vehicle uploader options:
  --config PATH                 vehicle YAML (default configs/vehicle-agent.dev.yaml)
  --recording-root PATH         override configured recording root
  --archive-root PATH           local archive destination (default .local/archive)
  --service                     keep scanning until terminated
  --poll-interval-ms N          service scan interval (default 5000)

Native smoke options:
  --signaling-http-url URL      signaling origin (default http://127.0.0.1:8765)
  --driver-console-url URL      console origin (default http://127.0.0.1:8080)
  --vehicle-id ID               vehicle identity (default vehicle-001)
  --device-token TOKEN          device credential
)";
}

Json preflight(const VehicleConfig& config) {
  Json checks = Json::array();
  auto add = [&](std::string name, bool passed, std::string detail) {
    checks.push_back({{"event", "vehicle_preflight_check"}, {"name", std::move(name)}, {"passed", passed}, {"detail", std::move(detail)}});
  };

  if (config.vehicle_adapter.type == "mock") {
    add("vehicle_adapter", true, "mock adapter does not require CAN hardware");
  } else {
    add(
        "chassis_bridge_library",
        std::filesystem::is_regular_file(config.vehicle_adapter.bridge_library_path),
        config.vehicle_adapter.bridge_library_path.string());
    const auto interface_path = std::filesystem::path("/sys/class/net") / config.hardware.can_interface;
    add("can_interface", std::filesystem::exists(interface_path), interface_path.string());
  }

  for (const auto& camera : config.enabled_cameras()) {
    const bool virtual_source = camera.device == "testsrc" || camera.device == "mvs" ||
                                camera.device.starts_with("mvs:") || camera.device.starts_with("hikrobot:") ||
                                camera.device == "pylon" || camera.device.starts_with("pylon:") ||
                                camera.device.starts_with("basler:");
    add("camera:" + camera.id, virtual_source || std::filesystem::exists(camera.device), camera.device);
  }

  const bool ready = std::all_of(checks.begin(), checks.end(), [](const auto& check) {
    return check["passed"].template get<bool>();
  });
  return {
      {"event", "vehicle_preflight"},
      {"runtime", "cpp"},
      {"vehicle_id", config.vehicle_id},
      {"ready", ready},
      {"check_count", checks.size()},
      {"checks", std::move(checks)},
  };
}

int run_adapter_status(const VehicleConfig& config) {
  auto adapter = mine_teleop::create_vehicle_adapter(config);
  try {
    adapter->open();
    const auto status = adapter->status();
    std::cout << Json({
                     {"event", "vehicle_adapter_status"},
                     {"runtime", "cpp"},
                     {"vehicle_id", config.vehicle_id},
                     {"ready", status.opened && status.healthy},
                     {"status", status.to_json()},
                 }).dump()
              << '\n';
    adapter->close();
    return status.opened && status.healthy ? 0 : 2;
  } catch (const std::exception& error) {
    std::cout << Json({
                     {"event", "vehicle_adapter_status"},
                     {"runtime", "cpp"},
                     {"vehicle_id", config.vehicle_id},
                     {"ready", false},
                     {"error", error.what()},
                     {"status", adapter->status().to_json()},
                 }).dump()
              << '\n';
    return 2;
  }
}

int run_loop(const VehicleConfig& config, const Arguments& arguments) {
  const int duration_ms = arguments.integer("--duration-ms", 1500);
  const int disconnect_at_ms = arguments.integer("--disconnect-at-ms", 500);
  if (duration_ms < 0 || disconnect_at_ms < 0) throw std::invalid_argument("loop timing must be non-negative");
  mine_teleop::VehicleControlService service(
      config, "session-001", "", mine_teleop::create_vehicle_adapter(config), 100);
  service.start(0);
  for (int timestamp_ms = 0; timestamp_ms <= duration_ms; timestamp_ms += 50) {
    if (timestamp_ms < disconnect_at_ms) {
      ControlCommand command;
      command.vehicle_id = config.vehicle_id;
      command.session_id = "session-001";
      command.seq = static_cast<std::uint64_t>(timestamp_ms / 50 + 1);
      command.ts_ms = timestamp_ms;
      command.gear = "D";
      command.throttle = 0.25;
      service.receive_command(command, timestamp_ms);
    }
    service.tick(timestamp_ms);
  }
  std::cout << service.summary().dump() << '\n';
  service.close();
  return 0;
}

int run_teleop(const VehicleConfig& config, const Arguments& arguments) {
  std::string token = arguments.value("--device-token", environment("MINE_TELEOP_DEVICE_TOKEN"));
  if (token.empty()) throw std::invalid_argument("--device-token or MINE_TELEOP_DEVICE_TOKEN is required");
  const std::string signaling_url = arguments.value("--signaling-http-url", config.cloud.signaling_url);
  mine_teleop::VehicleTeleopRuntime runtime(config, signaling_url, std::move(token));
  const auto result = runtime.run(
      arguments.has("--service") ? 0 : arguments.integer("--teleop-duration-ms", 5000),
      arguments.integer("--teleop-poll-interval-ms", 50),
      arguments.has("--service") ? 0 : arguments.integer("--teleop-session-wait-ms", 5000),
      arguments.has("--teleop-log-controls"));
  std::cout << result.dump() << '\n';
  return result.value("session_discovered", false) ? 0 : 2;
}

int run_vehicle_agent(const Arguments& arguments) {
  const auto config_path = arguments.value("--config", "configs/vehicle-agent.dev.yaml");
  const auto config = mine_teleop::load_vehicle_config(config_path);
  std::cout << config.redacted_summary().dump() << '\n';
  if (arguments.has("--preflight")) {
    const auto result = preflight(config);
    std::cout << result.dump() << '\n';
    return result["ready"].get<bool>() ? 0 : 2;
  }
  if (arguments.has("--adapter-status")) return run_adapter_status(config);
  if (arguments.has("--run-loop")) return run_loop(config, arguments);
  if (arguments.has("--teleop")) return run_teleop(config, arguments);
  throw std::invalid_argument("vehicle-agent requires --preflight, --adapter-status, --run-loop, or --teleop");
}

std::uint16_t port_option(const Arguments& arguments, std::string_view key, int fallback) {
  const int value = arguments.integer(key, fallback);
  if (value < 0 || value > 65535) throw std::invalid_argument(std::string(key) + " must be between 0 and 65535");
  return static_cast<std::uint16_t>(value);
}

int run_signaling_server(const Arguments& arguments) {
  mine_teleop::SignalingServerConfig config;
  config.host = arguments.value("--host", "127.0.0.1");
  config.port = port_option(arguments, "--port", 8765);
  const auto driver_id = arguments.value("--driver-id", "driver-console-001");
  const auto driver_password = arguments.value(
      "--driver-password", environment("MINE_TELEOP_DRIVER_PASSWORD").empty() ? "dev-password" : environment("MINE_TELEOP_DRIVER_PASSWORD"));
  const auto vehicle_id = arguments.value("--vehicle-id", "vehicle-001");
  const auto device_token = arguments.value(
      "--device-token", environment("MINE_TELEOP_DEVICE_TOKEN").empty() ? "dev-device-secret" : environment("MINE_TELEOP_DEVICE_TOKEN"));
  config.driver_passwords = {{driver_id, driver_password}};
  config.device_tokens = {{vehicle_id, device_token}};
  config.audit_log_path = arguments.value("--audit-log");
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      arguments.value("--host", "127.0.0.1"),
      port_option(arguments, "--port", 8765),
      [service](const auto& request) { return service->handle(request); });
  std::cout << Json({
                   {"event", "signaling_server_started"},
                   {"runtime", "cpp"},
                   {"host", arguments.value("--host", "127.0.0.1")},
                   {"port", port_option(arguments, "--port", 8765)},
                   {"driver_id", driver_id},
                   {"vehicle_id", vehicle_id},
               }).dump()
            << std::endl;
  server.serve_forever();
  return 0;
}

int run_driver_console(const Arguments& arguments) {
  auto config = mine_teleop::load_driver_config(arguments.value("--config", "configs/driver-console.dev.yaml"));
  const auto signaling_override = arguments.value("--signaling-http-url");
  if (!signaling_override.empty()) config.signaling_url = signaling_override;
  const auto password = arguments.value(
      "--driver-password", environment("MINE_TELEOP_DRIVER_PASSWORD").empty() ? "dev-password" : environment("MINE_TELEOP_DRIVER_PASSWORD"));
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(
      std::move(config), arguments.value("--vehicle-id", "vehicle-001"), password);
  auto app = std::make_shared<mine_teleop::DriverConsoleHttpApp>(runtime);
  mine_teleop::SimpleHttpServer server(
      arguments.value("--host", "127.0.0.1"),
      port_option(arguments, "--port", 8080),
      [app](const auto& request) { return app->handle(request); });
  std::cout << Json({
                   {"event", "driver_console_started"},
                   {"runtime", "cpp"},
                   {"host", arguments.value("--host", "127.0.0.1")},
                   {"port", port_option(arguments, "--port", 8080)},
               }).dump()
            << std::endl;
  server.serve_forever();
  return 0;
}

int run_vehicle_media_agent(const Arguments& arguments) {
  auto config = mine_teleop::load_vehicle_config(arguments.value("--config", "configs/vehicle-agent.dev.yaml"));
  const auto recording_root = arguments.has("--record")
                                  ? std::filesystem::path(arguments.value("--recording-root", config.recording.root_dir.string()))
                                  : std::filesystem::path{};
  mine_teleop::VehicleMediaRuntime runtime(
      std::move(config),
      arguments.value("--driver-console-url", "http://127.0.0.1:8080"),
      arguments.integer("--frame-timeout-ms", 3000),
      recording_root);
  const auto summary = runtime.run(
      arguments.has("--service") ? 0 : arguments.integer("--frames", 30),
      arguments.has("--service") ? 0 : arguments.integer("--duration-ms", -1),
      arguments.integer("--capture-interval-ms", 0));
  std::cout << summary.dump() << '\n';
  return summary.value("passed", false) ? 0 : 2;
}

int run_vehicle_uploader(const Arguments& arguments) {
  const auto config = mine_teleop::load_vehicle_config(arguments.value("--config", "configs/vehicle-agent.dev.yaml"));
  mine_teleop::LocalArchiveUploader uploader(
      arguments.value("--recording-root", config.recording.root_dir.string()),
      arguments.value("--archive-root", ".local/archive"),
      config.upload.max_bandwidth_mbps);
  const bool service = arguments.has("--service") || arguments.has("--service-mode");
  const int poll_interval_ms = arguments.integer("--poll-interval-ms", 5000);
  if (poll_interval_ms <= 0) throw std::invalid_argument("--poll-interval-ms must be positive");
  do {
    const auto result = uploader.process_once();
    auto record = result.to_json();
    record["backlog"] = uploader.backlog();
    std::cout << record.dump() << std::endl;
    if (!service) return result.action == "failed" ? 2 : 0;
    if (result.action == "idle") std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
  } while (true);
}

int run_http_health(const Arguments& arguments) {
  const auto url = arguments.value("--url");
  if (url.empty()) throw std::invalid_argument("--url is required");
  mine_teleop::HttpClient http;
  const auto response = http.get_json(url);
  std::cout << Json({{"event", "http_health"}, {"passed", true}, {"url", url}, {"response", response}}).dump() << '\n';
  return 0;
}

int run_vehicle_online(const Arguments& arguments) {
  const auto signaling = mine_teleop::normalize_signaling_http_url(
      arguments.value("--signaling-http-url", "http://127.0.0.1:8765"));
  const auto vehicle_id = arguments.value("--vehicle-id", "vehicle-001");
  const auto device_token = arguments.value(
      "--device-token", environment("MINE_TELEOP_DEVICE_TOKEN").empty() ? "dev-device-secret" : environment("MINE_TELEOP_DEVICE_TOKEN"));
  mine_teleop::HttpClient http;
  const auto response = http.post_json_response(
      signaling + "/vehicles/online", {{"vehicle_id", vehicle_id}, {"device_token", device_token}});
  std::cout << Json({{"event", "vehicle_online_cli"}, {"passed", true}, {"response", response}}).dump() << '\n';
  return 0;
}

int run_control_smoke(const Arguments& arguments) {
  const auto signaling = mine_teleop::normalize_signaling_http_url(
      arguments.value("--signaling-http-url", "http://127.0.0.1:8765"));
  std::string console = arguments.value("--driver-console-url", "http://127.0.0.1:8080");
  while (!console.empty() && console.back() == '/') console.pop_back();
  const auto vehicle_id = arguments.value("--vehicle-id", "vehicle-001");
  const auto device_token = arguments.value(
      "--device-token", environment("MINE_TELEOP_DEVICE_TOKEN").empty() ? "dev-device-secret" : environment("MINE_TELEOP_DEVICE_TOKEN"));
  mine_teleop::HttpClient http(std::chrono::seconds(10));
  const auto online = http.post_json_response(
      signaling + "/vehicles/online", {{"vehicle_id", vehicle_id}, {"device_token", device_token}});
  const auto connection = http.post_json_response(console + "/api/connect", Json::object());
  const auto control = http.post_json_response(
      console + "/api/control", {{"gear", "D"}, {"steering", 0.125}, {"throttle", 0.25}, {"brake", 0.0}, {"estop", false}});
  const auto session = http.get_json(
      signaling + "/vehicles/" + http.url_encode(vehicle_id) + "/session?device_token=" + http.url_encode(device_token));
  const auto messages = http.get_json(
      signaling + "/signaling/" + http.url_encode(session.at("session_id").get<std::string>()) +
      "/messages?recipient=" + http.url_encode(vehicle_id) + "&device_token=" + http.url_encode(device_token));
  auto vehicle_config = mine_teleop::load_vehicle_config(arguments.value("--config", "configs/vehicle-agent.dev.yaml"));
  mine_teleop::VehicleMediaRuntime media(std::move(vehicle_config), console, 5000);
  const auto media_summary = media.run(1);
  const auto console_status = http.get_json(console + "/api/status");
  const bool passed = online.value("state", "") == "online" && connection.value("connected", false) &&
                      control.value("queued", 0) == 1 && messages.at("messages").size() == 1 &&
                      media_summary.value("passed", false) && console_status.at("cameras").contains("front");
  std::cout << Json({
                   {"event", "native_control_plane_smoke"},
                   {"runtime", "cpp"},
                   {"passed", passed},
                   {"session_id", session.value("session_id", "")},
                   {"control_messages", messages.at("messages").size()},
                   {"media", media_summary},
               }).dump()
            << '\n';
  return passed ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 2 || std::string_view(argv[1]) == "--help" || std::string_view(argv[1]) == "help") {
      print_help();
      return 0;
    }
    const std::string command(argv[1]);
    Arguments arguments(argc, argv, 2);
    if (command == "version" || command == "--version") {
      std::cout << "mine-teleop 0.2.0 cpp ubuntu22.04\n";
      return 0;
    }
    if (command == "config-check") {
      const auto config = mine_teleop::load_vehicle_config(
          arguments.value("--config", "configs/vehicle-agent.dev.yaml"));
      auto result = config.redacted_summary();
      result["event"] = "vehicle_config_check";
      result["passed"] = true;
      std::cout << result.dump() << '\n';
      return 0;
    }
    if (command == "vehicle-agent") return run_vehicle_agent(arguments);
    if (command == "vehicle-media-agent") return run_vehicle_media_agent(arguments);
    if (command == "vehicle-uploader") return run_vehicle_uploader(arguments);
    if (command == "http-health") return run_http_health(arguments);
    if (command == "vehicle-online") return run_vehicle_online(arguments);
    if (command == "control-smoke") return run_control_smoke(arguments);
    if (command == "signaling-server") return run_signaling_server(arguments);
    if (command == "driver-console") return run_driver_console(arguments);
    throw std::invalid_argument("unknown command: " + command);
  } catch (const std::exception& error) {
    std::cerr << Json({
                    {"event", "mine_teleop_error"},
                    {"runtime", "cpp"},
                    {"error", error.what()},
                }).dump()
              << '\n';
    return 2;
  }
}
