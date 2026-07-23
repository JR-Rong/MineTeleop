#include "mine_teleop/platform.hpp"
#include "mine_teleop/server.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace {

using mine_teleop::Json;

class Arguments {
 public:
  Arguments(int argc, char** argv) {
    for (int index = 1; index < argc; ++index) {
      std::string token(argv[index]);
      if (!token.starts_with("--")) throw std::invalid_argument("unexpected argument: " + token);
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
  static void require_known(std::string_view key) {
    static const std::unordered_set<std::string> known{
        "--config",
        "--port",
        "--vehicle-id",
        "--driver-password",
        "--signaling-url",
        "--ice-transport-policy",
        "--no-open-browser",
        "--help",
        "--version",
    };
    if (!known.contains(std::string(key))) throw std::invalid_argument("unknown option: " + std::string(key));
  }

  std::unordered_map<std::string, std::string> values_;
  std::unordered_set<std::string> flags_;
};

std::string environment(std::string_view key) {
  const char* value = std::getenv(std::string(key).c_str());
  return value == nullptr ? "" : value;
}

std::filesystem::path default_config_path(const char* executable) {
  const auto explicit_path = environment("MINE_TELEOP_CONFIG");
  if (!explicit_path.empty()) return explicit_path;
  const auto bundle_root = std::filesystem::absolute(executable).parent_path().parent_path();
  const auto bundled = bundle_root / "config/driver-console.yaml";
  if (std::filesystem::is_regular_file(bundled)) return bundled;
  const auto installed = bundle_root / "share/mine-teleop/configs/driver-console.dev.yaml";
  if (std::filesystem::is_regular_file(installed)) return installed;
  const std::filesystem::path packaged = "config/driver-console.yaml";
  if (std::filesystem::is_regular_file(packaged)) return packaged;
  return "configs/driver-console.dev.yaml";
}

std::uint16_t port_option(const Arguments& arguments) {
  const int port = arguments.integer("--port", 8080);
  if (port < 0 || port > 65535) throw std::invalid_argument("--port must be between 0 and 65535");
  return static_cast<std::uint16_t>(port);
}

void print_help() {
  std::cout << R"HELP(Mine Teleop portable control client

Usage:
  mine-teleop-control [options]

Options:
  --config PATH              shared driver YAML (or MINE_TELEOP_CONFIG)
  --port N                   loopback HTTP port (default 8080; 0 selects a free port)
  --vehicle-id ID            initial vehicle fallback before browser selection
  --driver-password VALUE    development override (prefer MINE_TELEOP_DRIVER_PASSWORD)
  --signaling-url URL        override cloud.signaling_url
  --ice-transport-policy P   all (default) or relay (forced TURN)
  --no-open-browser          do not open the default browser
  --help                     show this help

The control page always binds to a loopback address and is never published as
a public driving page.
)HELP";
}

volatile std::sig_atomic_t termination_signal = 0;

void handle_signal(int signal) { termination_signal = signal; }

int run(const Arguments& arguments, const char* executable) {
  const std::string host = "127.0.0.1";
  if (!mine_teleop::is_loopback_bind_address(host)) throw std::logic_error("control client loopback policy failed");
  const auto config_path = arguments.value("--config", default_config_path(executable).string());
  auto config = mine_teleop::load_driver_config(config_path);
  const auto signaling_override = arguments.value("--signaling-url");
  if (!signaling_override.empty()) config.signaling_url = signaling_override;
  const auto ice_transport_policy = arguments.value("--ice-transport-policy");
  if (!ice_transport_policy.empty()) {
    if (!mine_teleop::ice_transport_policy_is_valid(ice_transport_policy)) {
      throw std::invalid_argument("--ice-transport-policy must be all or relay");
    }
    config.ice_transport_policy = ice_transport_policy;
  }
  const auto configured_password = environment("MINE_TELEOP_DRIVER_PASSWORD");
  const auto password = arguments.value(
      "--driver-password", configured_password.empty() ? "dev-password" : configured_password);
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(
      std::move(config), arguments.value("--vehicle-id", "vehicle-001"), password);
  auto app = std::make_shared<mine_teleop::DriverConsoleHttpApp>(runtime);
  mine_teleop::SimpleHttpServer server(
      host,
      port_option(arguments),
      [app](const auto& request) { return app->handle(request); });

  try {
    server.start();
  } catch (const std::exception& error) {
    throw std::runtime_error(
        "cannot start the local control page on " + host + ":" +
        std::to_string(port_option(arguments)) + ": " + error.what());
  }
  const auto url = "http://127.0.0.1:" + std::to_string(server.port()) + "/";
  std::string browser_error;
  const bool browser_opened = arguments.has("--no-open-browser") ? false : mine_teleop::open_default_browser(url, browser_error);
  std::cout << Json({
                   {"event", "control_client_started"},
                   {"sent_at_utc_ms", mine_teleop::now_ms()},
                   {"runtime", "cpp"},
                   {"platform", mine_teleop::platform_name()},
                   {"host", host},
                   {"port", server.port()},
                   {"url", url},
                   {"browser_opened", browser_opened},
                   {"config", config_path},
               }).dump()
            << std::endl;
  if (!arguments.has("--no-open-browser") && !browser_opened) {
    std::cerr << Json({
                     {"event", "control_browser_open_failed"},
                     {"sent_at_utc_ms", mine_teleop::now_ms()},
                     {"url", url},
                     {"error", browser_error},
                 }).dump()
              << std::endl;
  }

  termination_signal = 0;
  const auto previous_int = std::signal(SIGINT, handle_signal);
  const auto previous_term = std::signal(SIGTERM, handle_signal);
  while (termination_signal == 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  server.stop();
  try {
    static_cast<void>(runtime->disconnect("control_client_shutdown"));
  } catch (const std::exception& error) {
    std::cerr << Json({
                     {"event", "control_client_disconnect_failed"},
                     {"sent_at_utc_ms", mine_teleop::now_ms()},
                     {"error", error.what()},
                 }).dump()
              << std::endl;
  }
  std::signal(SIGINT, previous_int);
  std::signal(SIGTERM, previous_term);
  std::cout << Json({
                   {"event", "control_client_stopped"},
                   {"sent_at_utc_ms", mine_teleop::now_ms()},
                   {"signal", termination_signal},
               }).dump()
            << std::endl;
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    Arguments arguments(argc, argv);
    if (arguments.has("--help")) {
      print_help();
      return 0;
    }
    if (arguments.has("--version")) {
      std::cout << "mine-teleop-control 0.2.0 " << mine_teleop::platform_name() << '\n';
      return 0;
    }
    return run(arguments, argc > 0 && argv[0] != nullptr ? argv[0] : "mine-teleop-control");
  } catch (const std::exception& error) {
    std::cerr << Json({
                    {"event", "control_client_error"},
                    {"sent_at_utc_ms", mine_teleop::now_ms()},
                    {"runtime", "cpp"},
                    {"platform", mine_teleop::platform_name()},
                    {"error", error.what()},
                }).dump()
              << '\n';
    return 2;
  }
}
