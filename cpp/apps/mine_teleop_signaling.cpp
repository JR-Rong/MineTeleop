#include "mine_teleop/platform.hpp"
#include "mine_teleop/server.hpp"

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

std::atomic<bool> stopping{false};

void stop_handler(int) { stopping = true; }

class Arguments {
 public:
  Arguments(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
      std::string token(argv[index]);
      if (!token.starts_with("--")) throw std::invalid_argument("unexpected positional argument: " + token);
      const auto equal = token.find('=');
      if (equal != std::string::npos) {
        const auto key = token.substr(0, equal);
        require_known(key);
        values_[key] = token.substr(equal + 1);
      } else if (index + 1 < argc && !std::string_view(argv[index + 1]).starts_with("--")) {
        require_known(token);
        values_[token] = argv[++index];
      } else {
        require_known(token);
        values_[token] = "true";
      }
    }
  }

  [[nodiscard]] bool has(std::string_view key) const { return values_.contains(std::string(key)); }

  [[nodiscard]] std::string value(std::string_view key, std::string fallback = {}) const {
    const auto found = values_.find(std::string(key));
    return found == values_.end() ? std::move(fallback) : found->second;
  }

  [[nodiscard]] std::int64_t integer(std::string_view key, std::int64_t fallback) const {
    const auto text = value(key);
    if (text.empty()) return fallback;
    std::int64_t parsed = 0;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (error != std::errc{} || end != text.data() + text.size()) {
      throw std::invalid_argument(std::string(key) + " must be an integer");
    }
    return parsed;
  }

 private:
  static void require_known(std::string_view key) {
    static const std::unordered_set<std::string> known{
        "--allow-insecure-nonloopback-dev",
        "--api-rate-limit-max-sources",
        "--api-rate-limit-requests",
        "--api-rate-limit-window-ms",
        "--audit-log",
        "--audit-log-files",
        "--audit-log-max-bytes",
        "--audit-log-retention-days",
        "--config",
        "--control-token-ttl-ms",
        "--device-token",
        "--driver-heartbeat-ms",
        "--driver-id",
        "--driver-password",
        "--driver-token-ttl-ms",
        "--help",
        "--host",
        "--login-failure-window-ms",
        "--login-lockout-ms",
        "--login-max-failures",
        "--port",
        "--stun-urls",
        "--trusted-proxy-addresses",
        "--turn-credential-ttl-seconds",
        "--turn-realm",
        "--turn-static-auth-secret-file",
        "--turn-urls",
        "--validate-config",
        "--vehicle-heartbeat-ms",
        "--vehicle-id",
        "--version",
    };
    if (!known.contains(std::string(key))) throw std::invalid_argument("unknown option: " + std::string(key));
  }

  std::unordered_map<std::string, std::string> values_;
};

std::string environment(std::string_view name) {
  const auto* value = std::getenv(std::string(name).c_str());
  return value == nullptr ? "" : value;
}

std::int64_t environment_integer(std::string_view name, std::int64_t fallback) {
  const auto value = environment(name);
  if (value.empty()) return fallback;
  std::int64_t parsed = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (error != std::errc{} || end != value.data() + value.size()) {
    throw std::invalid_argument(std::string(name) + " must be an integer");
  }
  return parsed;
}

std::int64_t configured_integer(
    const Arguments& arguments,
    std::string_view argument_name,
    std::string_view environment_name,
    std::int64_t fallback) {
  if (arguments.has(argument_name)) return arguments.integer(argument_name, fallback);
  return environment_integer(environment_name, fallback);
}

std::string read_secret(std::string_view path, std::string_view label) {
  std::ifstream input{std::string(path)};
  if (!input) throw std::runtime_error("cannot read " + std::string(label) + " file");
  std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
  if (value.empty()) throw std::invalid_argument(std::string(label) + " must not be empty");
  return value;
}

std::vector<std::string> comma_separated(std::string_view input) {
  std::vector<std::string> values;
  std::size_t start = 0;
  while (start <= input.size()) {
    const auto end = input.find(',', start);
    auto value = std::string(input.substr(
        start,
        end == std::string_view::npos ? input.size() - start : end - start));
    const auto first = value.find_first_not_of(" \t\r\n");
    const auto last = value.find_last_not_of(" \t\r\n");
    if (first != std::string::npos) values.push_back(value.substr(first, last - first + 1));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return values;
}

std::uint16_t port(const Arguments& arguments) {
  const auto value = arguments.integer("--port", 8765);
  if (value < 0 || value > 65535) throw std::invalid_argument("--port must be between 0 and 65535");
  return static_cast<std::uint16_t>(value);
}

void print_help() {
  std::cout << R"HELP(Mine Teleop standalone signaling server

Usage:
  mine-teleop-signaling-server [options]

Identity and listener:
  --config PATH                          multi-identity YAML (or MINE_TELEOP_SIGNALING_CONFIG)
  --host ADDRESS                         loopback bind address (default 127.0.0.1)
  --port N                               HTTP/WSS port (default 8765; 0 selects a free port)
  --driver-id ID                         configured driver identity
  --driver-password VALUE                development driver credential
  --vehicle-id ID                        configured vehicle identity
  --device-token VALUE                   development vehicle credential
  --allow-insecure-nonloopback-dev        permit an isolated non-loopback development bind

Lease and presence:
  --driver-token-ttl-ms N
  --control-token-ttl-ms N
  --vehicle-heartbeat-ms N
  --driver-heartbeat-ms N

Abuse controls and proxy trust:
  --login-max-failures N
  --login-failure-window-ms N
  --login-lockout-ms N
  --api-rate-limit-requests N
  --api-rate-limit-window-ms N
  --api-rate-limit-max-sources N
  --trusted-proxy-addresses CSV

ICE and audit:
  --stun-urls CSV
  --turn-urls CSV
  --turn-realm VALUE
  --turn-static-auth-secret-file PATH
  --turn-credential-ttl-seconds N
  --audit-log PATH
  --audit-log-max-bytes N
  --audit-log-files N                    size-limited parts within the active hour
  --audit-log-retention-days N           hourly archive retention (default 7)

Other:
  --validate-config                        load and validate configuration without listening
  --help                                  show this help without starting a listener
  --version                               show the version
)HELP";
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const Arguments arguments(argc, argv);
    if (arguments.has("--help")) {
      print_help();
      return 0;
    }
    if (arguments.has("--version")) {
      std::cout << "mine-teleop-signaling-server 0.2.0 " << mine_teleop::platform_name() << '\n';
      return 0;
    }
    const auto host = arguments.value("--host", "127.0.0.1");
    if (!mine_teleop::is_loopback_bind_address(host) && !arguments.has("--allow-insecure-nonloopback-dev")) {
      throw std::invalid_argument(
          "signaling backend must bind loopback behind the TLS proxy; "
          "use --allow-insecure-nonloopback-dev only for an isolated development tunnel");
    }
    const auto identity_config_path = arguments.value(
        "--config", environment("MINE_TELEOP_SIGNALING_CONFIG"));
    mine_teleop::SignalingServerConfig config;
    if (!identity_config_path.empty()) {
      if (arguments.has("--driver-id") || arguments.has("--driver-password") ||
          arguments.has("--vehicle-id") || arguments.has("--device-token") ||
          !environment("MINE_TELEOP_DRIVER_PASSWORD").empty() ||
          !environment("MINE_TELEOP_DEVICE_TOKEN").empty()) {
        throw std::invalid_argument(
            "--config cannot be combined with legacy single-identity arguments or secret environment variables");
      }
      config = mine_teleop::load_signaling_identity_config(identity_config_path);
    } else {
      const auto driver_id = arguments.value("--driver-id", "driver-console-001");
      const auto vehicle_id = arguments.value("--vehicle-id", "vehicle-001");
      const auto driver_password = arguments.value(
          "--driver-password",
          environment("MINE_TELEOP_DRIVER_PASSWORD").empty()
              ? "dev-password"
              : environment("MINE_TELEOP_DRIVER_PASSWORD"));
      const auto device_token = arguments.value(
          "--device-token",
          environment("MINE_TELEOP_DEVICE_TOKEN").empty()
              ? "dev-device-secret"
              : environment("MINE_TELEOP_DEVICE_TOKEN"));
      config.driver_passwords = {{driver_id, driver_password}};
      config.device_tokens = {{vehicle_id, device_token}};
      config.driver_vehicle_permissions = {{driver_id, {vehicle_id}}};
    }
    config.host = host;
    config.port = port(arguments);
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
    config.stun_urls = comma_separated(arguments.value(
        "--stun-urls",
        environment("MINE_TELEOP_STUN_URLS").empty()
            ? "stun:127.0.0.1:3478"
            : environment("MINE_TELEOP_STUN_URLS")));
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
    config.audit_log_max_bytes = configured_integer(
        arguments,
        "--audit-log-max-bytes",
        "MINE_TELEOP_AUDIT_LOG_MAX_BYTES",
        64 * 1024 * 1024);
    config.audit_log_files = configured_integer(
        arguments,
        "--audit-log-files",
        "MINE_TELEOP_AUDIT_LOG_FILES",
        5);
    config.audit_log_retention_days = configured_integer(
        arguments,
        "--audit-log-retention-days",
        "MINE_TELEOP_AUDIT_LOG_RETENTION_DAYS",
        7);
    const auto stun_count = config.stun_urls.size();
    const auto turn_count = config.turn_urls.size();
    const auto login_max_failures = config.login_max_failures;
    const auto login_failure_window_ms = config.login_failure_window_ms;
    const auto login_lockout_ms = config.login_lockout_ms;
    const auto api_rate_limit_requests = config.api_rate_limit_requests;
    const auto api_rate_limit_window_ms = config.api_rate_limit_window_ms;
    const auto api_rate_limit_max_sources = config.api_rate_limit_max_sources;
    const auto trusted_proxy_count = config.trusted_proxy_addresses.size();
    const auto audit_log_max_bytes = config.audit_log_max_bytes;
    const auto audit_log_files = config.audit_log_files;
    const auto audit_log_rotation_interval_ms = config.audit_log_rotation_interval_ms;
    const auto audit_log_retention_days = config.audit_log_retention_days;
    const auto driver_count = config.driver_passwords.size();
    const auto vehicle_count = config.device_tokens.size();
    std::size_t permission_count = 0;
    for (const auto& entry : config.driver_vehicle_permissions) permission_count += entry.second.size();

    if (arguments.has("--validate-config")) {
      config.audit_log_path.clear();
      mine_teleop::SignalingService validation(std::move(config));
      std::cout << mine_teleop::Json({
                       {"event", "signaling_config_valid"},
                       {"driver_count", driver_count},
                       {"vehicle_count", vehicle_count},
                       {"permission_count", permission_count},
                   }).dump()
                << std::endl;
      return 0;
    }

    auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
    mine_teleop::SimpleHttpServer server(
        host,
        port(arguments),
        [service](const auto& request) { return service->handle(request); },
        8 * 1024 * 1024,
        [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
    server.start();
    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    std::cout << mine_teleop::Json({
                     {"event", "signaling_server_started"},
                     {"runtime", "cpp"},
                     {"host", host},
                     {"port", server.port()},
                     {"tls_termination", "external"},
                     {"websocket", true},
                     {"stun_url_count", stun_count},
                     {"turn_url_count", turn_count},
                     {"login_max_failures", login_max_failures},
                     {"login_failure_window_ms", login_failure_window_ms},
                     {"login_lockout_ms", login_lockout_ms},
                     {"api_rate_limit_requests", api_rate_limit_requests},
                     {"api_rate_limit_window_ms", api_rate_limit_window_ms},
                     {"api_rate_limit_max_sources", api_rate_limit_max_sources},
                     {"trusted_proxy_count", trusted_proxy_count},
                     {"audit_log_max_bytes", audit_log_max_bytes},
                     {"audit_log_files", audit_log_files},
                     {"audit_log_rotation_interval_ms", audit_log_rotation_interval_ms},
                     {"audit_log_retention_days", audit_log_retention_days},
                     {"driver_count", driver_count},
                     {"vehicle_count", vehicle_count},
                     {"permission_count", permission_count},
                 }).dump()
              << std::endl;
    while (!stopping) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    server.stop();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "mine-teleop-signaling-server: " << error.what() << '\n';
    return 1;
  }
}
