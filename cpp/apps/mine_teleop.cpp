#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/media.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/upload.hpp"
#include "mine_teleop/video.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

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

int environment_integer(std::string_view name, int fallback) {
  const auto value = environment(name);
  if (value.empty()) return fallback;
  std::size_t consumed = 0;
  const int parsed = std::stoi(value, &consumed);
  if (consumed != value.size()) throw std::invalid_argument(std::string(name) + " must be an integer");
  return parsed;
}

int configured_integer(
    const Arguments& arguments,
    std::string_view argument_name,
    std::string_view environment_name,
    int fallback) {
  if (arguments.has(argument_name)) return arguments.integer(argument_name, fallback);
  return environment_integer(environment_name, fallback);
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::vector<std::string> comma_separated(std::string value) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto end = value.find(',', start);
    auto item = trim(value.substr(start, end == std::string::npos ? value.size() - start : end - start));
    if (!item.empty()) result.push_back(std::move(item));
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return result;
}

std::string read_secret(const std::filesystem::path& path, std::string_view label) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read " + std::string(label) + " file");
  auto value = trim(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
  if (value.empty()) throw std::runtime_error(std::string(label) + " file is empty");
  return value;
}

void apply_ice_transport_policy_override(const Arguments& arguments, std::string& policy) {
  const auto value = arguments.value("--ice-transport-policy");
  if (value.empty()) return;
  if (!mine_teleop::ice_transport_policy_is_valid(value)) {
    throw std::invalid_argument("--ice-transport-policy must be all or relay");
  }
  policy = value;
}

std::string device_token(const Arguments& arguments, const VehicleConfig& config) {
  auto token = arguments.value("--device-token", environment("MINE_TELEOP_DEVICE_TOKEN"));
  if (!token.empty()) return token;
  if (config.cloud.device_token_file.empty()) {
    throw std::invalid_argument(
        "device token is required: configure cloud.device_token_file, --device-token, or MINE_TELEOP_DEVICE_TOKEN");
  }
  std::ifstream input(config.cloud.device_token_file);
  if (!input) {
    throw std::runtime_error("cannot read device token file: " + config.cloud.device_token_file.string());
  }
  token = trim(std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()));
  if (token.empty()) throw std::runtime_error("device token file is empty: " + config.cloud.device_token_file.string());
  return token;
}

void print_help() {
  std::cout << R"(Mine Teleop native C++ runtime

Usage:
  mine-teleop version
  mine-teleop config-check [--config PATH]
  mine-teleop vehicle-agent [options]
  mine-teleop vehicle-media-agent [options]
  mine-teleop vehicle-runtime [options]
  mine-teleop vehicle-uploader [options]
  mine-teleop signaling-server [options]
  mine-teleop driver-console [options]
  mine-teleop media-probe
  mine-teleop time-sync --signaling-http-url URL [--samples N] [--max-uncertainty-ms N]
  mine-teleop http-health --url URL
  mine-teleop vehicle-online [options]
  mine-teleop control-smoke [options]

Vehicle options:
  --config PATH                 vehicle YAML (default configs/vehicle-agent.dev.yaml)
  --preflight                   validate local runtime/device prerequisites
  --adapter-status              open adapter, print status, close
  --run-loop                    run deterministic local control-loop smoke
  --service                     run selected production agent until terminated
  --device-token TOKEN          or set MINE_TELEOP_DEVICE_TOKEN
  --signaling-http-url URL      defaults to cloud.signaling_url
  --ice-transport-policy P      all (default) or relay (forced TURN)
  --duration-ms N               local smoke duration (default 1500)
  --disconnect-at-ms N          stop local commands at N (default 500)

Signaling server options:
  --host ADDRESS                bind address (default 127.0.0.1)
  --port N                      bind port (default 8765)
  --driver-id ID                configured driver (default driver-console-001)
  --driver-password PASSWORD    or set MINE_TELEOP_DRIVER_PASSWORD
  --vehicle-id ID               configured vehicle (default vehicle-001)
  --device-token TOKEN          or set MINE_TELEOP_DEVICE_TOKEN
  --audit-log PATH              append native JSONL audit records
  --audit-log-max-bytes N       active-hour part size limit (default 67108864)
  --audit-log-files N           active-hour size part count (default 5)
  --audit-log-retention-days N  hourly archive retention (default 7)
  --driver-token-ttl-ms N       driver bearer token lifetime (default 1800000)
  --control-token-ttl-ms N      control authority token lifetime (default 300000)
  --vehicle-heartbeat-ms N      vehicle offline timeout (default 15000)
  --driver-heartbeat-ms N       driver offline timeout (default 15000)
  --login-max-failures N        failures before login lockout (default 5)
  --login-failure-window-ms N   failure counting window (default 60000)
  --login-lockout-ms N          login lockout duration (default 300000)
  --api-rate-limit-requests N   requests per source/window (default 600)
  --api-rate-limit-window-ms N  API quota window (default 60000)
  --api-rate-limit-max-sources N
                                explicit source-table capacity (default 4096)
  --trusted-proxy-addresses CSV direct proxy IPs allowed to supply X-Forwarded-For
  --stun-urls CSV               STUN URLs (default stun:127.0.0.1:3478)
  --turn-urls CSV               TURN/TURNS URLs
  --turn-realm NAME             coturn realm or MINE_TELEOP_TURN_REALM
  --turn-static-auth-secret-file PATH
                                coturn REST secret file (preferred)
  --turn-credential-ttl-seconds N
                                TURN credential lifetime (default 600)

Driver console options:
  --config PATH                 driver YAML (default configs/driver-console.dev.yaml)
  --host ADDRESS                bind address (default 127.0.0.1)
  --port N                      bind port (default 8080)
  --vehicle-id ID               target vehicle (default vehicle-001)
  --driver-password PASSWORD    or set MINE_TELEOP_DRIVER_PASSWORD
  --signaling-http-url URL      override driver configuration

Vehicle media options:
  --config PATH                 vehicle YAML (default configs/vehicle-agent.dev.yaml)
  --signaling-http-url URL      WebRTC signaling origin (defaults to config)
  --device-token TOKEN          or set MINE_TELEOP_DEVICE_TOKEN
  --codec h265|h264             force codec instead of browser negotiation
  --ice-transport-policy P      all (default) or relay (forced TURN)
  --frames N                    frames per camera (default 30)
  --duration-ms N               optional duration limit
  --capture-interval-ms N       optional interval between capture rounds
  --frame-timeout-ms N          native camera timeout (default 3000)
  --simulate-primary-failure-after-frames N  bench-only NVENC failover injection
  --record                      reuse encoded H.264/H.265 packets for MP4 segments
  --recording-root PATH         recording destination (defaults to config)

Unified vehicle runtime:
  vehicle-runtime reads control/media/recording settings and the device-token
  file from the vehicle YAML, then supervises both foreground services.

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
                                camera.device == "aravis" || camera.device.starts_with("aravis:") ||
                                camera.device == "pylon" || camera.device.starts_with("pylon:") ||
                                camera.device == "basler" || camera.device.starts_with("basler:");
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
  constexpr std::string_view driver_id = "driver-001";
  constexpr std::string_view control_token = "bench-control-token";
  mine_teleop::VehicleControlService service(
      config,
      std::string(driver_id),
      "session-001",
      std::string(control_token),
      mine_teleop::create_vehicle_adapter(config),
      100);
  service.start(0);
  for (int timestamp_ms = 0; timestamp_ms <= duration_ms; timestamp_ms += 50) {
    if (timestamp_ms < disconnect_at_ms) {
      ControlCommand command;
      command.vehicle_id = config.vehicle_id;
      command.driver_id = driver_id;
      command.session_id = "session-001";
      command.seq = static_cast<std::uint64_t>(timestamp_ms / 50 + 1);
      command.sent_at_utc_ms = timestamp_ms;
      command.control_token = control_token;
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

int run_vehicle_agent(const Arguments& arguments) {
  const auto config_path = arguments.value("--config", "configs/vehicle-agent.dev.yaml");
  auto config = mine_teleop::load_vehicle_config(config_path);
  apply_ice_transport_policy_override(arguments, config.cloud.ice_transport_policy);
  std::cout << config.redacted_summary().dump() << '\n';
  if (arguments.has("--preflight")) {
    const auto result = preflight(config);
    std::cout << result.dump() << '\n';
    return result["ready"].get<bool>() ? 0 : 2;
  }
  if (arguments.has("--adapter-status")) return run_adapter_status(config);
  if (arguments.has("--run-loop")) return run_loop(config, arguments);
  throw std::invalid_argument("vehicle-agent requires --preflight, --adapter-status, or --run-loop");
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
  config.driver_vehicle_permissions = {{driver_id, {vehicle_id}}};
  config.admin_token = environment("MINE_TELEOP_ADMIN_TOKEN");
  config.token_ttl_ms = arguments.integer("--driver-token-ttl-ms", 30 * 60 * 1000);
  config.control_token_ttl_ms = arguments.integer("--control-token-ttl-ms", 5 * 60 * 1000);
  config.vehicle_heartbeat_timeout_ms = arguments.integer("--vehicle-heartbeat-ms", 15 * 1000);
  config.driver_heartbeat_timeout_ms = arguments.integer("--driver-heartbeat-ms", 15 * 1000);
  config.login_max_failures = configured_integer(
      arguments,
      "--login-max-failures",
      "MINE_TELEOP_LOGIN_MAX_FAILURES",
      5);
  config.login_failure_window_ms = configured_integer(
      arguments,
      "--login-failure-window-ms",
      "MINE_TELEOP_LOGIN_FAILURE_WINDOW_MS",
      60 * 1000);
  config.login_lockout_ms = configured_integer(
      arguments,
      "--login-lockout-ms",
      "MINE_TELEOP_LOGIN_LOCKOUT_MS",
      5 * 60 * 1000);
  config.api_rate_limit_requests = configured_integer(
      arguments,
      "--api-rate-limit-requests",
      "MINE_TELEOP_API_RATE_LIMIT_REQUESTS",
      600);
  config.api_rate_limit_window_ms = configured_integer(
      arguments,
      "--api-rate-limit-window-ms",
      "MINE_TELEOP_API_RATE_LIMIT_WINDOW_MS",
      60 * 1000);
  config.api_rate_limit_max_sources = configured_integer(
      arguments,
      "--api-rate-limit-max-sources",
      "MINE_TELEOP_API_RATE_LIMIT_MAX_SOURCES",
      4096);
  config.trusted_proxy_addresses = comma_separated(arguments.value(
      "--trusted-proxy-addresses",
      environment("MINE_TELEOP_TRUSTED_PROXY_ADDRESSES").empty()
          ? "127.0.0.1,::1"
          : environment("MINE_TELEOP_TRUSTED_PROXY_ADDRESSES")));
  const auto configured_stun_urls = arguments.value(
      "--stun-urls",
      environment("MINE_TELEOP_STUN_URLS").empty()
          ? "stun:127.0.0.1:3478"
          : environment("MINE_TELEOP_STUN_URLS"));
  config.stun_urls = comma_separated(configured_stun_urls);
  config.turn_urls = comma_separated(arguments.value("--turn-urls", environment("MINE_TELEOP_TURN_URLS")));
  config.turn_realm = arguments.value("--turn-realm", environment("MINE_TELEOP_TURN_REALM"));
  const auto turn_secret_file = arguments.value(
      "--turn-static-auth-secret-file",
      environment("MINE_TELEOP_TURN_STATIC_AUTH_SECRET_FILE"));
  config.turn_static_auth_secret = turn_secret_file.empty()
      ? environment("MINE_TELEOP_TURN_STATIC_AUTH_SECRET")
      : read_secret(turn_secret_file, "TURN static auth secret");
  config.turn_credential_ttl_seconds = arguments.integer("--turn-credential-ttl-seconds", 600);
  config.audit_log_path = arguments.value("--audit-log");
  config.audit_log_max_bytes = arguments.integer("--audit-log-max-bytes", 64 * 1024 * 1024);
  config.audit_log_files = arguments.integer("--audit-log-files", 5);
  config.audit_log_retention_days = arguments.integer("--audit-log-retention-days", 7);
  const auto stun_url_count = config.stun_urls.size();
  const auto turn_url_count = config.turn_urls.size();
  const auto login_max_failures = config.login_max_failures;
  const auto login_failure_window_ms = config.login_failure_window_ms;
  const auto login_lockout_ms = config.login_lockout_ms;
  const auto api_rate_limit_requests = config.api_rate_limit_requests;
  const auto api_rate_limit_window_ms = config.api_rate_limit_window_ms;
  const auto api_rate_limit_max_sources = config.api_rate_limit_max_sources;
  const auto trusted_proxy_count = config.trusted_proxy_addresses.size();
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      arguments.value("--host", "127.0.0.1"),
      port_option(arguments, "--port", 8765),
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  std::cout << Json({
                   {"event", "signaling_server_started"},
                   {"runtime", "cpp"},
                   {"host", arguments.value("--host", "127.0.0.1")},
                   {"port", port_option(arguments, "--port", 8765)},
                   {"driver_id", driver_id},
                   {"vehicle_id", vehicle_id},
                   {"stun_url_count", stun_url_count},
                   {"turn_url_count", turn_url_count},
                   {"login_max_failures", login_max_failures},
                   {"login_failure_window_ms", login_failure_window_ms},
                   {"login_lockout_ms", login_lockout_ms},
                   {"api_rate_limit_requests", api_rate_limit_requests},
                   {"api_rate_limit_window_ms", api_rate_limit_window_ms},
                   {"api_rate_limit_max_sources", api_rate_limit_max_sources},
                   {"trusted_proxy_count", trusted_proxy_count},
               }).dump()
            << std::endl;
  server.serve_forever();
  return 0;
}

int run_driver_console(const Arguments& arguments) {
  auto config = mine_teleop::load_driver_config(arguments.value("--config", "configs/driver-console.dev.yaml"));
  const auto signaling_override = arguments.value("--signaling-http-url");
  if (!signaling_override.empty()) config.signaling_url = signaling_override;
  apply_ice_transport_policy_override(arguments, config.ice_transport_policy);
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

struct VehicleMediaLaunch {
  mine_teleop::VehicleConfig config;
  std::string signaling_url;
  std::string device_token;
  int frame_timeout_ms{3000};
  std::filesystem::path recording_root;
  std::optional<std::string> forced_codec;
  int simulate_primary_failure_after_frames{0};
  std::string connection_id;
  int frame_count{0};
  int duration_ms{0};
  int capture_interval_ms{0};
};

int run_vehicle_media_loop(const VehicleMediaLaunch& launch, bool service) {
  constexpr auto kSessionRestartDelay = std::chrono::milliseconds(250);
  constexpr auto kTransportRestartDelay = std::chrono::seconds(1);
  while (true) {
    try {
      mine_teleop::VehicleMediaRuntime runtime(
          launch.config,
          launch.signaling_url,
          launch.device_token,
          launch.frame_timeout_ms,
          launch.recording_root,
          launch.forced_codec,
          launch.simulate_primary_failure_after_frames,
          launch.connection_id);
      const auto summary = runtime.run(launch.frame_count, launch.duration_ms, launch.capture_interval_ms);
      std::cout << summary.dump() << '\n';
      if (!service) return summary.value("passed", false) ? 0 : 2;
      std::cout << Json({
                       {"event", "vehicle_media_service_restart"},
                       {"reason", "runtime_completed"},
                       {"retry_after_ms", std::chrono::duration_cast<std::chrono::milliseconds>(kTransportRestartDelay).count()},
                   }).dump()
                << std::endl;
      std::this_thread::sleep_for(kTransportRestartDelay);
    } catch (const mine_teleop::HttpStatusError& error) {
      const bool session_ended = error.status() == 404 || error.status() == 409;
      const bool signaling_unavailable = error.status() >= 500 && error.status() < 600;
      if (!service || (!session_ended && !signaling_unavailable)) throw;
      const auto retry_delay = session_ended ? kSessionRestartDelay : kTransportRestartDelay;
      std::cout << Json({
                       {"event", session_ended ? "vehicle_media_session_ended" : "vehicle_media_signaling_retry"},
                       {"http_status", error.status()},
                       {"safety_action", "local_full_stop"},
                       {"retry_after_ms", std::chrono::duration_cast<std::chrono::milliseconds>(retry_delay).count()},
                   }).dump()
                << std::endl;
      std::this_thread::sleep_for(retry_delay);
    } catch (const mine_teleop::HttpTransportError& error) {
      if (!service) throw;
      std::cout << Json({
                       {"event", "vehicle_media_signaling_retry"},
                       {"error", error.what()},
                       {"retry_after_ms", std::chrono::duration_cast<std::chrono::milliseconds>(kTransportRestartDelay).count()},
                   }).dump()
                << std::endl;
      std::this_thread::sleep_for(kTransportRestartDelay);
    }
  }
}

int run_vehicle_media_agent(const Arguments& arguments) {
  auto config = mine_teleop::load_vehicle_config(arguments.value("--config", "configs/vehicle-agent.dev.yaml"));
  apply_ice_transport_policy_override(arguments, config.cloud.ice_transport_policy);
  const auto token = device_token(arguments, config);
  const auto recording_root =
      arguments.has("--record") || config.recording.enabled
          ? std::filesystem::path(arguments.value("--recording-root", config.recording.root_dir.string()))
          : std::filesystem::path{};
  std::optional<std::string> forced_codec;
  if (arguments.has("--codec")) forced_codec = arguments.value("--codec");
  const bool service = arguments.has("--service");
  return run_vehicle_media_loop(
      {
          config,
          arguments.value("--signaling-http-url", config.cloud.signaling_url),
          token,
          arguments.integer("--frame-timeout-ms", config.runtime.media_frame_timeout_ms),
          recording_root,
          forced_codec,
          arguments.integer("--simulate-primary-failure-after-frames", 0),
          "vehicle-media-" + mine_teleop::random_token(12),
          service ? 0 : arguments.integer("--frames", arguments.has("--duration-ms") ? 0 : 30),
          service ? 0 : arguments.integer("--duration-ms", -1),
          arguments.integer("--capture-interval-ms", config.runtime.media_capture_interval_ms),
      },
      service);
}

volatile std::sig_atomic_t termination_signal = 0;

void handle_termination_signal(int signal) { termination_signal = signal; }

struct ChildService {
  std::string name;
  pid_t pid{-1};
  bool running{false};
};

pid_t spawn_service(std::string_view name, const std::function<int()>& run) {
  std::cout.flush();
  std::cerr.flush();
  const auto pid = fork();
  if (pid < 0) throw std::runtime_error("cannot start " + std::string(name) + ": " + std::strerror(errno));
  if (pid != 0) return pid;
  std::signal(SIGINT, SIG_DFL);
  std::signal(SIGTERM, SIG_DFL);
  int result = 2;
  try {
    result = run();
  } catch (const std::exception& error) {
    std::cerr << Json({
                     {"event", "vehicle_runtime_child_error"},
                     {"service", std::string(name)},
                     {"error", error.what()},
                 }).dump()
              << '\n';
  }
  std::cout.flush();
  std::cerr.flush();
  _exit(result);
}

int wait_status_code(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 2;
}

int run_vehicle_runtime(const Arguments& arguments) {
  const auto config_path = arguments.value("--config", "config/vehicle-agent.yaml");
  auto config = mine_teleop::load_vehicle_config(config_path);
  const auto signaling_override = arguments.value("--signaling-http-url");
  if (!signaling_override.empty()) config.cloud.signaling_url = signaling_override;
  apply_ice_transport_policy_override(arguments, config.cloud.ice_transport_policy);
  const auto token = device_token(arguments, config);
  const auto connection_id = "vehicle-runtime-" + mine_teleop::random_token(12);
  std::vector<ChildService> children;
  auto stop_children = [&] {
    for (auto& child : children) {
      if (child.running) kill(child.pid, SIGTERM);
    }
    for (auto& child : children) {
      if (!child.running) continue;
      int status = 0;
      while (waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
      }
      child.running = false;
    }
  };

  termination_signal = 0;
  const auto previous_int = std::signal(SIGINT, handle_termination_signal);
  const auto previous_term = std::signal(SIGTERM, handle_termination_signal);

  try {
    if (config.runtime.control_enabled && !config.runtime.media_enabled) {
      throw std::invalid_argument(
          "vehicle control requires the WebRTC media runtime because commands use its DataChannel");
    }
    if (config.runtime.media_enabled) {
      const auto pid = spawn_service("media", [config, token, connection_id] {
        const auto recording_root = config.recording.enabled ? config.recording.root_dir : std::filesystem::path{};
        return run_vehicle_media_loop(
            {
                config,
                config.cloud.signaling_url,
                token,
                config.runtime.media_frame_timeout_ms,
                recording_root,
                std::nullopt,
                0,
                connection_id,
                0,
                0,
                config.runtime.media_capture_interval_ms,
            },
            true);
      });
      children.push_back({"media", pid, true});
    }
  } catch (...) {
    stop_children();
    std::signal(SIGINT, previous_int);
    std::signal(SIGTERM, previous_term);
    throw;
  }

  std::cout << Json({
                   {"event", "vehicle_runtime_started"},
                   {"vehicle_id", config.vehicle_id},
                   {"config", config_path},
                   {"control_enabled", config.runtime.control_enabled},
                   {"control_transport", config.runtime.control_enabled ? "webrtc_data_channel" : "disabled"},
                   {"media_enabled", config.runtime.media_enabled},
                   {"recording_enabled", config.recording.enabled},
               }).dump()
            << std::endl;

  int first_status = 0;
  bool child_exited = false;
  while (termination_signal == 0) {
    const auto pid = waitpid(-1, &first_status, WNOHANG);
    if (pid > 0) {
      for (auto& child : children) {
        if (child.pid == pid) child.running = false;
      }
      child_exited = true;
      break;
    }
    if (pid < 0 && errno != EINTR) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  stop_children();
  std::signal(SIGINT, previous_int);
  std::signal(SIGTERM, previous_term);
  if (termination_signal != 0) return 128 + termination_signal;
  if (!child_exited) return 2;
  const auto code = wait_status_code(first_status);
  return code == 0 ? 1 : code;
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

int run_time_sync(const Arguments& arguments) {
  const auto signaling = arguments.value("--signaling-http-url", "http://127.0.0.1:8765");
  const auto samples = arguments.integer("--samples", 7);
  const auto max_uncertainty_ms = arguments.integer("--max-uncertainty-ms", 25);
  mine_teleop::HttpClient http;
  mine_teleop::SynchronizedClock clock;
  const auto status = clock.synchronize(http, signaling, samples);
  auto result = status.to_json();
  result["event"] = "time_sync_probe";
  result["logical_now_ms"] = clock.now_ms();
  result["max_uncertainty_ms"] = max_uncertainty_ms;
  result["passed"] = status.acceptable(max_uncertainty_ms);
  std::cout << result.dump() << '\n';
  return result.at("passed").get<bool>() ? 0 : 2;
}

int run_vehicle_online(const Arguments& arguments) {
  const auto signaling = mine_teleop::normalize_signaling_http_url(
      arguments.value("--signaling-http-url", "http://127.0.0.1:8765"));
  const auto vehicle_id = arguments.value("--vehicle-id", "vehicle-001");
  const auto device_token = arguments.value(
      "--device-token", environment("MINE_TELEOP_DEVICE_TOKEN").empty() ? "dev-device-secret" : environment("MINE_TELEOP_DEVICE_TOKEN"));
  mine_teleop::HttpClient http;
  const auto response = http.post_json_response(
      signaling + "/vehicles/online",
      {{"vehicle_id", vehicle_id},
       {"device_token", device_token},
       {"connection_id", "vehicle-online-cli-" + mine_teleop::random_token(12)}});
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
      signaling + "/vehicles/online",
      {{"vehicle_id", vehicle_id},
       {"device_token", device_token},
       {"connection_id", "control-smoke-" + mine_teleop::random_token(12)}});
  const auto connection_generation = online.at("connection_generation").get<std::uint64_t>();
  const auto connection = http.post_json_response(console + "/api/connect", Json::object());
  const auto control = http.post_json_response(
      console + "/api/control", {{"gear", "D"}, {"steering", 0.125}, {"throttle", 0.25}, {"brake", 0.0}, {"estop", false}});
  const auto session = http.get_json(
      signaling + "/vehicles/" + http.url_encode(vehicle_id) + "/session?connection_generation=" +
          std::to_string(connection_generation),
      {{"X-Mine-Teleop-Device-Token", device_token}});
  const auto command = mine_teleop::ControlCommand::from_json(control.at("command"));
  const auto capabilities = http.post_json_response(
      console + "/api/webrtc/capabilities", {{"codecs", {"h264", "h265"}}});
  const auto console_status = http.get_json(console + "/api/status");
  const bool passed = online.value("state", "") == "online" && connection.value("connected", false) &&
                      control.value("prepared", false) && control.value("transport", "") == "webrtc_data_channel" &&
                      command.vehicle_id == vehicle_id && command.session_id == session.value("session_id", "") &&
                      command.control_token == session.value("control_token", "") &&
                      capabilities.value("queued", 0) == 1 && console_status.value("connected", false);
  std::cout << Json({
                   {"event", "native_control_plane_smoke"},
                   {"runtime", "cpp"},
                   {"passed", passed},
                   {"session_id", session.value("session_id", "")},
                   {"control_transport", control.value("transport", "")},
                   {"control_command_prepared", control.value("prepared", false)},
                   {"media_capabilities", capabilities},
               }).dump()
            << '\n';
  return passed ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc > 0 && argv[0] != nullptr && *argv[0] != '\0') {
      std::error_code path_error;
      const auto executable = std::filesystem::absolute(argv[0], path_error);
      if (!path_error && std::filesystem::is_regular_file(executable, path_error) && !path_error) {
        ::setenv("MINE_TELEOP_EXECUTABLE_PATH", executable.c_str(), 1);
      }
    }
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
    if (command == "vehicle-runtime") return run_vehicle_runtime(arguments);
    if (command == "vehicle-uploader") return run_vehicle_uploader(arguments);
    if (command == "http-health") return run_http_health(arguments);
    if (command == "time-sync") return run_time_sync(arguments);
    if (command == "vehicle-online") return run_vehicle_online(arguments);
    if (command == "control-smoke") return run_control_smoke(arguments);
    if (command == "signaling-server") return run_signaling_server(arguments);
    if (command == "driver-console") return run_driver_console(arguments);
    if (command == "media-probe") {
      std::cout << mine_teleop::probe_video_encoders().dump() << '\n';
      return 0;
    }
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
