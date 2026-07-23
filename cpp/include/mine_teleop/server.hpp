#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/websocket.hpp"

namespace mine_teleop {

struct HttpRequest {
  std::string method;
  std::string target;
  std::string peer_address;
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
  using WebSocketHandler = std::function<bool(int, const HttpRequest&)>;

  SimpleHttpServer(
      std::string host,
      std::uint16_t port,
      Handler handler,
      std::size_t max_body_bytes = 8 * 1024 * 1024,
      WebSocketHandler websocket_handler = {});
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
  WebSocketHandler websocket_handler_;
  std::atomic<bool> stopping_{false};
  mutable std::mutex clients_mutex_;
  mutable std::condition_variable clients_stopped_;
  std::unordered_set<int> client_sockets_;
  std::atomic<int> listener_fd_{-1};
  std::uint16_t bound_port_{0};
  std::thread thread_;
};

struct SignalingServerConfig {
  std::string host{"127.0.0.1"};
  std::uint16_t port{8765};
  std::unordered_map<std::string, std::string> driver_passwords{{"driver-console-001", "dev-password"}};
  std::unordered_map<std::string, std::string> device_tokens{{"vehicle-001", "dev-device-secret"}};
  std::unordered_map<std::string, std::unordered_set<std::string>> driver_vehicle_permissions{
      {"driver-console-001", {"vehicle-001"}}};
  std::string admin_token;
  std::int64_t token_ttl_ms{30 * 60 * 1000};
  std::int64_t control_token_ttl_ms{5 * 60 * 1000};
  std::int64_t vehicle_heartbeat_timeout_ms{15 * 1000};
  std::int64_t driver_heartbeat_timeout_ms{15 * 1000};
  std::int64_t connection_reaper_interval_ms{250};
  std::int64_t login_max_failures{5};
  std::int64_t login_failure_window_ms{60 * 1000};
  std::int64_t login_lockout_ms{5 * 60 * 1000};
  std::int64_t api_rate_limit_requests{600};
  std::int64_t api_rate_limit_window_ms{60 * 1000};
  std::int64_t api_rate_limit_max_sources{4096};
  std::vector<std::string> trusted_proxy_addresses{"127.0.0.1", "::1"};
  std::vector<std::string> stun_urls{"stun:127.0.0.1:3478"};
  std::vector<std::string> turn_urls;
  std::string turn_realm;
  std::string turn_static_auth_secret;
  std::int64_t turn_credential_ttl_seconds{600};
  std::size_t max_signaling_payload_bytes{512 * 1024};
  std::size_t max_sdp_bytes{256 * 1024};
  std::size_t max_ice_candidate_bytes{8 * 1024};
  std::int64_t signaling_message_ttl_ms{15 * 1000};
  std::string audit_log_path;
  std::int64_t audit_log_max_bytes{64 * 1024 * 1024};
  std::int64_t audit_log_files{5};
  std::int64_t audit_log_rotation_interval_ms{60 * 60 * 1000};
  std::int64_t audit_log_retention_days{7};
};

SignalingServerConfig load_signaling_identity_config(const std::filesystem::path& path);

class SignalingService {
 public:
  explicit SignalingService(
      SignalingServerConfig config,
      std::function<std::int64_t()> audit_clock = {});
  ~SignalingService();

  [[nodiscard]] ServerResponse handle(const HttpRequest& request);
  [[nodiscard]] bool handle_websocket(int socket, const HttpRequest& request);
  [[nodiscard]] Json health() const;

 private:
  struct DriverToken {
    std::string driver_id;
    std::int64_t expires_at_ms{0};
    std::uint64_t connection_generation{0};
  };
  struct ConnectionPresence {
    std::string connection_id;
    std::uint64_t generation{0};
    std::int64_t connected_at_ms{0};
    std::int64_t last_seen_at_ms{0};
  };
  struct RelayUsageSample {
    std::uint64_t sequence{0};
    std::uint64_t bytes_sent{0};
    std::uint64_t bytes_received{0};
    std::uint64_t duration_ms{0};
  };
  struct Session {
    std::string session_id;
    std::string vehicle_id;
    std::string driver_id;
    SessionState state{SessionState::Online};
    std::string control_token;
    std::int64_t control_token_expires_at_ms{0};
    std::uint64_t relay_bytes_sent{0};
    std::uint64_t relay_bytes_received{0};
    std::uint64_t relay_duration_ms{0};
    std::uint64_t relay_usage_samples{0};
    double last_relay_bitrate_kbps{0.0};
    std::unordered_map<std::string, RelayUsageSample> last_relay_usage_by_actor;

    [[nodiscard]] Json to_json(bool include_control_token = false) const;
  };
  struct Message {
    ProtocolMetadata metadata;
    std::string sender;
    std::string recipient;
    std::string type;
    Json payload;
    std::int64_t queued_at_utc_ms{0};
    std::uint64_t delivery_cursor{0};

    [[nodiscard]] Json to_json() const;
  };
  struct AcceptedMessage {
    std::uint64_t sequence{0};
    std::string fingerprint;
    Json acknowledgement;
  };
  struct LoginFailureState {
    std::int64_t failures{0};
    std::int64_t window_started_at_ms{0};
    std::int64_t blocked_until_ms{0};
  };
  struct ApiRateState {
    std::int64_t requests{0};
    std::int64_t window_started_at_ms{0};
    bool limit_audited{false};
  };

  [[nodiscard]] ServerResponse handle_get(const HttpRequest& request);
  [[nodiscard]] ServerResponse handle_post(const HttpRequest& request);
  [[nodiscard]] Json enqueue_signaling_message(
      std::string_view session_id,
      const Json& value,
      std::optional<std::string_view> authenticated_actor = std::nullopt);
  [[nodiscard]] Json take_signaling_messages(
      std::string_view session_id,
      std::string_view recipient,
      std::string_view requested_types = {},
      bool consume = true);
  [[nodiscard]] std::size_t acknowledge_signaling_messages(
      std::string_view session_id,
      std::string_view recipient,
      std::uint64_t delivery_cursor);
  [[nodiscard]] const Session& require_active_session(std::string_view session_id) const;
  [[nodiscard]] const Session& require_participant(std::string_view session_id, std::string_view participant) const;
  void validate_driver_token(std::string_view driver_id, std::string_view token);
  void validate_device_token(std::string_view vehicle_id, std::string_view token) const;
  void validate_vehicle_connection(
      std::string_view vehicle_id,
      std::string_view token,
      std::uint64_t connection_generation);
  void validate_actor_credential(const Session& session, std::string_view actor, const Json& value);
  void validate_message_metadata(
      const Session& session,
      const ProtocolMetadata& metadata);
  void cleanup_expired_connections(std::int64_t timestamp_ms);
  void close_sessions_for_vehicle(std::string_view vehicle_id, std::string_view reason);
  void close_sessions_for_driver(std::string_view driver_id, std::string_view reason);
  void transition_session(Session& session, SessionState next, std::string_view reason);
  void close_session(Session& session, std::string_view reason);
  void enforce_login_rate_limit(std::string_view driver_id, std::int64_t timestamp_ms);
  void record_login_failure(std::string_view driver_id, std::int64_t timestamp_ms);
  void clear_login_failures(std::string_view driver_id);
  [[nodiscard]] std::string request_source(const HttpRequest& request) const;
  void cleanup_api_rate_limits(std::int64_t timestamp_ms);
  void enforce_api_rate_limit(const HttpRequest& request, std::int64_t timestamp_ms);
  void audit(std::string_view event, const Json& details = Json::object()) const;

  SignalingServerConfig config_;
  std::string service_instance_id_;
  std::function<std::int64_t()> audit_clock_;
  mutable std::mutex mutex_;
  mutable std::mutex audit_log_mutex_;
  mutable std::int64_t audit_log_period_start_ms_{-1};
  mutable std::int64_t audit_log_last_retention_period_ms_{-1};
  std::unordered_map<std::string, DriverToken> driver_tokens_;
  std::unordered_map<std::string, ConnectionPresence> online_vehicles_;
  std::unordered_map<std::string, ConnectionPresence> online_drivers_;
  std::unordered_set<std::string> revoked_vehicles_;
  std::unordered_set<std::string> revoked_drivers_;
  std::unordered_map<std::string, Session> sessions_;
  std::unordered_map<std::string, std::vector<Message>> messages_;
  std::unordered_map<std::string, AcceptedMessage> last_accepted_messages_;
  std::unordered_map<std::string, std::uint64_t> next_delivery_cursors_;
  std::unordered_map<std::string, LoginFailureState> login_failures_;
  std::unordered_set<std::string> trusted_proxy_addresses_;
  std::unordered_map<std::string, ApiRateState> api_rate_limits_;
  ApiRateState api_rate_limit_overflow_;
  std::int64_t api_rate_limit_last_cleanup_ms_{0};
  std::uint64_t api_rate_limited_requests_{0};
  std::uint64_t session_counter_{0};
  std::uint64_t connection_generation_{0};
  std::jthread connection_reaper_;
};

struct GamepadConfig {
  bool enabled{true};
  int steering_axis{0};
  int throttle_axis{2};
  int brake_axis{5};
  double axis_deadzone{0.05};
  bool steering_inverted{false};
  bool throttle_inverted{true};
  bool brake_inverted{true};
  double steering_center{0.0};
  double steering_range{1.0};
  double throttle_rest{1.0};
  double throttle_range{2.0};
  double brake_rest{1.0};
  double brake_range{2.0};
  int estop_button{0};
};

struct DriverConfig {
  std::string driver_id;
  std::string signaling_url;
  std::vector<std::string> resolve_entries;
  std::filesystem::path ca_bundle;
  std::string ice_transport_policy{"all"};
  std::filesystem::path browser_event_log_path;
  std::uint64_t browser_event_log_max_bytes{2 * 1024 * 1024};
  int browser_event_log_files{3};
  int rate_hz{20};
  int estop_hold_ms{500};
  int max_time_sync_uncertainty_ms{25};
  int time_sync_interval_ms{30000};
  int time_sync_samples{7};
  GamepadConfig gamepad;
};

DriverConfig load_driver_config(const std::string& path);

class DriverConsoleRuntime {
 public:
  DriverConsoleRuntime(DriverConfig config, std::string vehicle_id, std::string password);
  ~DriverConsoleRuntime();

  [[nodiscard]] Json login(std::string_view password = {});
  [[nodiscard]] Json vehicles();
  [[nodiscard]] Json connect(std::string_view vehicle_id = {});
  [[nodiscard]] Json end_session(std::string_view reason = "driver_session_end");
  [[nodiscard]] Json disconnect(std::string_view reason = "driver_console_disconnect");
  [[nodiscard]] Json poll_signaling();
  [[nodiscard]] Json send_media_capabilities(const Json& input);
  [[nodiscard]] Json ice_servers();
  [[nodiscard]] Json send_media_fallback(const Json& input);
  [[nodiscard]] Json send_webrtc_answer(const Json& input);
  [[nodiscard]] Json send_webrtc_ice_candidate(const Json& input);
  [[nodiscard]] Json ingest_webrtc_metrics(const Json& input);
  [[nodiscard]] Json send_control(const Json& input);
  [[nodiscard]] Json record_browser_event(const Json& input);
  [[nodiscard]] Json status();
  [[nodiscard]] const DriverConfig& config() const { return config_; }

 private:
  [[nodiscard]] Json login_locked(std::string_view password);
  [[nodiscard]] Json fetch_authorized_vehicles(std::string_view token, std::int64_t expires_at_ms);
  [[nodiscard]] Json send_signaling_message(std::string_view type, const Json& payload);
  void connect_signaling_websocket(std::string_view session_id, std::string_view token);
  void close_signaling_websocket();
  void append_websocket_messages(const Json& envelope);
  [[nodiscard]] bool remote_session_is_active(std::string_view session_id, std::string_view token) const;
  TimeSyncStatus refresh_time_sync();
  Json renew_control_authority();

  DriverConfig config_;
  std::string vehicle_id_;
  std::string password_;
  std::string signaling_http_url_;
  HttpClient http_;
  SynchronizedClock clock_;
  mutable std::mutex mutex_;
  mutable std::mutex browser_event_log_mutex_;
  mutable std::mutex time_sync_mutex_;
  mutable std::mutex control_lease_mutex_;
  mutable std::mutex authentication_mutex_;
  mutable std::mutex signaling_send_mutex_;
  mutable std::mutex signaling_websocket_mutex_;
  std::string driver_token_;
  std::int64_t driver_token_expires_at_ms_{0};
  std::string signaling_service_instance_id_;
  std::uint64_t signaling_restart_recoveries_{0};
  bool signaling_available_{false};
  std::string session_id_;
  std::string control_token_;
  std::int64_t control_token_expires_at_ms_{0};
  std::int64_t control_token_renew_at_ms_{0};
  std::uint64_t sequence_{0};
  std::int64_t connected_at_ms_{0};
  std::int64_t last_control_sent_ms_{0};
  Json signaling_messages_ = Json::array();
  Json pending_websocket_messages_ = Json::array();
  std::unique_ptr<WebSocketClient> signaling_websocket_;
  std::string signaling_websocket_session_id_;
  std::uint64_t signaling_delivery_cursor_{0};
  std::uint64_t signaling_websocket_reconnects_{0};
  Json webrtc_metrics_ = Json::object();
  std::string last_webrtc_audit_key_;
  Json authorized_vehicles_ = Json::array();
};

class DriverConsoleHttpApp {
 public:
  explicit DriverConsoleHttpApp(std::shared_ptr<DriverConsoleRuntime> runtime);
  [[nodiscard]] ServerResponse handle(const HttpRequest& request) const;

 private:
  std::shared_ptr<DriverConsoleRuntime> runtime_;
};

std::string random_token(std::size_t bytes = 24);

}  // namespace mine_teleop
