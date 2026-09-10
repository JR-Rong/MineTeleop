#include "mine_teleop/platform.hpp"
#include "mine_teleop/server.hpp"

#include <curl/curl.h>

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

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

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
        "--desktop-managed",
        "--browser-event-log",
        "--dependency-info",
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
  --desktop-managed          exit when the owning desktop pipe closes
  --browser-event-log PATH   write browser events outside the application bundle
  --dependency-info          show linked dependency versions as JSON
  --help                     show this help
  --version                  show the program and linked curl versions

The control page always binds to a loopback address and is never published as
a public driving page.
)HELP";
}

Json dependency_info() {
  const auto* curl = curl_version_info(CURLVERSION_NOW);
  if (curl == nullptr) throw std::runtime_error("curl_version_info failed");
  return Json({
      {"curl_compile_version", LIBCURL_VERSION},
      {"curl_compile_version_num", static_cast<std::uint64_t>(LIBCURL_VERSION_NUM)},
      {"curl_linkage", MINE_TELEOP_CURL_LINKAGE},
      {"curl_runtime_version", curl->version == nullptr ? "" : curl->version},
      {"curl_runtime_version_num", static_cast<std::uint64_t>(curl->version_num)},
      {"curl_tls_backend", curl->ssl_version == nullptr ? "" : curl->ssl_version},
      {"mine_teleop_version", "0.2.0"},
      {"platform", mine_teleop::platform_name()},
  });
}

volatile std::sig_atomic_t termination_signal = 0;

bool desktop_owner_requested_shutdown() {
  static std::string pending;
  char bytes[256];
  std::size_t count = 0;
#if defined(_WIN32)
  const auto pipe = GetStdHandle(STD_INPUT_HANDLE);
  DWORD available = 0;
  if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) return true;
  if (available == 0) return false;
  DWORD received = 0;
  if (!ReadFile(pipe, bytes, sizeof(bytes), &received, nullptr) || received == 0) return true;
  count = received;
#else
  pollfd input{STDIN_FILENO, POLLIN, 0};
  if (::poll(&input, 1, 0) <= 0) return false;
  if ((input.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) == 0) return false;
  const auto received = ::read(STDIN_FILENO, bytes, sizeof(bytes));
  if (received <= 0) return true;
  count = static_cast<std::size_t>(received);
#endif
  pending.append(bytes, count);
  if (pending.size() > 4096) return true;
  for (auto end = pending.find('\n'); end != std::string::npos; end = pending.find('\n')) {
    const auto command = pending.substr(0, end);
    pending.erase(0, end + 1);
    if (command == "shutdown" || command == "shutdown\r") return true;
  }
  return false;
}

void handle_signal(int signal) { termination_signal = signal; }

int run(const Arguments& arguments, const char* executable) {
  const std::string host = "127.0.0.1";
  if (!mine_teleop::is_loopback_bind_address(host)) throw std::logic_error("control client loopback policy failed");
  const auto config_path = arguments.value("--config", default_config_path(executable).string());
  auto config = mine_teleop::load_driver_config(config_path);
  if (arguments.has("--browser-event-log")) {
    config.browser_event_log_path = arguments.value("--browser-event-log");
  }
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
      "--driver-password", configured_password);
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
  const bool open_browser = !arguments.has("--no-open-browser") && !arguments.has("--desktop-managed");
  const bool browser_opened = open_browser ? mine_teleop::open_default_browser(url, browser_error) : false;
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
  if (open_browser && !browser_opened) {
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
  // The owner is the only writer. EOF also covers an owner crash. Polling avoids
  // a blocked stdin reader during signal shutdown and needs no public HTTP API.
  while (termination_signal == 0) {
    if (arguments.has("--desktop-managed") && desktop_owner_requested_shutdown()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  runtime->prepare_shutdown();
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
    if (arguments.has("--dependency-info")) {
      std::cout << dependency_info().dump() << '\n';
      return 0;
    }
    if (arguments.has("--version")) {
      const auto dependencies = dependency_info();
      std::cout << "mine-teleop-control 0.2.0 " << mine_teleop::platform_name()
                << " libcurl/" << dependencies.at("curl_runtime_version").get<std::string>()
                << " " << dependencies.at("curl_tls_backend").get<std::string>()
                << " curl-linkage/" << dependencies.at("curl_linkage").get<std::string>() << '\n';
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
