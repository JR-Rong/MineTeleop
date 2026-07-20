#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"

namespace mine_teleop {

struct HttpRequest {
  std::string method;
  std::string target;
  std::string path;
  std::unordered_map<std::string, std::string> query;
  std::unordered_map<std::string, std::string> headers;
  std::string body;

  [[nodiscard]] Json json_body() const;
};

struct ServerResponse {
  int status{200};
  std::string content_type{"application/json; charset=utf-8"};
  std::string body{"{}"};
  std::vector<std::pair<std::string, std::string>> headers;

  static ServerResponse json(int status, const Json& value);
  static ServerResponse text(int status, std::string body, std::string content_type = "text/plain; charset=utf-8");
};

class SimpleHttpServer {
 public:
  using Handler = std::function<ServerResponse(const HttpRequest&)>;

  SimpleHttpServer(std::string host, std::uint16_t port, Handler handler, std::size_t max_body_bytes = 8 * 1024 * 1024);
  ~SimpleHttpServer();

  SimpleHttpServer(const SimpleHttpServer&) = delete;
  SimpleHttpServer& operator=(const SimpleHttpServer&) = delete;

  void serve_forever();
  void start();
  void stop();
  [[nodiscard]] std::uint16_t port() const { return bound_port_; }

 private:
  void open_listener();
  void serve_client(int client_fd) const;

  std::string host_;
  std::uint16_t requested_port_;
  Handler handler_;
  std::size_t max_body_bytes_;
  std::atomic<bool> stopping_{false};
  int listener_fd_{-1};
  std::uint16_t bound_port_{0};
  std::thread thread_;
};

struct SignalingServerConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{8765};
  std::unordered_map<std::string, std::string> driver_passwords{{"driver-console-001", "dev-password"}};
  std::unordered_map<std::string, std::string> device_tokens{{"vehicle-001", "dev-device-secret"}};
  std::int64_t token_ttl_ms{30 * 60 * 1000};
  std::string audit_log_path;
};

class SignalingService {
 public:
  explicit SignalingService(SignalingServerConfig config);

  [[nodiscard]] ServerResponse handle(const HttpRequest& request);
  [[nodiscard]] Json health() const;

 private:
  struct DriverToken {
    std::string driver_id;
    std::int64_t expires_at_ms{0};
  };
  struct Session {
    std::string session_id;
    std::string vehicle_id;
    std::string driver_id;
    std::string state{"SESSION_ACTIVE"};
    std::string control_token;

    [[nodiscard]] Json to_json() const;
  };
  struct Message {
    std::string session_id;
    std::string sender;
    std::string recipient;
    std::string type;
    Json payload;

    [[nodiscard]] Json to_json() const;
  };

  [[nodiscard]] ServerResponse handle_get(const HttpRequest& request);
  [[nodiscard]] ServerResponse handle_post(const HttpRequest& request);
  [[nodiscard]] const Session& require_active_session(std::string_view session_id) const;
  [[nodiscard]] const Session& require_participant(std::string_view session_id, std::string_view participant) const;
  void validate_driver_token(std::string_view driver_id, std::string_view token) const;
  void validate_device_token(std::string_view vehicle_id, std::string_view token) const;
  void validate_actor_credential(const Session& session, std::string_view actor, const Json& value) const;
  void audit(std::string_view event, const Json& details = Json::object()) const;

  SignalingServerConfig config_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, DriverToken> driver_tokens_;
  std::unordered_map<std::string, bool> online_vehicles_;
  std::unordered_map<std::string, Session> sessions_;
  std::unordered_map<std::string, std::vector<Message>> messages_;
  std::uint64_t session_counter_{0};
};

struct DriverConfig {
  std::string driver_id;
  std::string signaling_url;
  int rate_hz{20};
  int estop_hold_ms{500};
};

DriverConfig load_driver_config(const std::string& path);

class DriverConsoleRuntime {
 public:
  DriverConsoleRuntime(DriverConfig config, std::string vehicle_id, std::string password);

  [[nodiscard]] Json connect();
  [[nodiscard]] Json poll_signaling();
  [[nodiscard]] Json send_control(const Json& input);
  [[nodiscard]] Json status() const;
  [[nodiscard]] Json ingest_frame(const Json& payload);
  [[nodiscard]] std::optional<std::pair<std::string, std::string>> frame(std::string_view camera_id) const;

 private:
  struct FrameRecord {
    std::string codec;
    std::string content_type;
    std::string bytes;
    int width{0};
    int height{0};
    std::int64_t captured_at_ms{0};
    std::uint64_t frame_count{0};
  };

  DriverConfig config_;
  std::string vehicle_id_;
  std::string password_;
  std::string signaling_http_url_;
  HttpClient http_;
  mutable std::mutex mutex_;
  std::string driver_token_;
  std::string session_id_;
  std::string control_token_;
  std::uint64_t sequence_{0};
  std::int64_t connected_at_ms_{0};
  std::int64_t last_control_sent_ms_{0};
  std::unordered_map<std::string, FrameRecord> frames_;
  Json signaling_messages_{Json::array()};
};

class DriverConsoleHttpApp {
 public:
  explicit DriverConsoleHttpApp(std::shared_ptr<DriverConsoleRuntime> runtime);
  [[nodiscard]] ServerResponse handle(const HttpRequest& request) const;

 private:
  std::shared_ptr<DriverConsoleRuntime> runtime_;
};

std::string random_token(std::size_t bytes = 24);
std::string base64_encode(std::string_view value);
std::string base64_decode(std::string_view value);

}  // namespace mine_teleop
