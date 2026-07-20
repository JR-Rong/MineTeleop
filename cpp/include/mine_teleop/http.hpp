#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "mine_teleop/core.hpp"

namespace mine_teleop {

struct HttpResponse {
  long status{0};
  std::string body;
};

class HttpClient {
 public:
  explicit HttpClient(std::chrono::milliseconds timeout = std::chrono::seconds(5));

  [[nodiscard]] HttpResponse get(std::string_view url) const;
  [[nodiscard]] HttpResponse post_json(std::string_view url, const Json& payload) const;
  [[nodiscard]] Json get_json(std::string_view url) const;
  [[nodiscard]] Json post_json_response(std::string_view url, const Json& payload) const;
  [[nodiscard]] std::string url_encode(std::string_view value) const;

 private:
  [[nodiscard]] HttpResponse request(std::string_view method, std::string_view url, std::string_view body) const;

  std::chrono::milliseconds timeout_;
};

struct TimeSyncStatus {
  bool synchronized{false};
  std::int64_t offset_ms{0};
  std::int64_t round_trip_ms{0};
  std::int64_t uncertainty_ms{0};
  std::int64_t synchronized_at_local_ms{0};
  int sample_count{0};

  [[nodiscard]] Json to_json() const;
  [[nodiscard]] bool acceptable(int max_uncertainty_ms) const;
};

class SynchronizedClock {
 public:
  TimeSyncStatus synchronize(const HttpClient& http, std::string_view signaling_origin, int sample_count = 7);
  [[nodiscard]] std::int64_t now_ms() const;
  [[nodiscard]] std::int64_t from_local_system_ms(std::int64_t local_time_ms) const;
  [[nodiscard]] TimeSyncStatus status() const;
  [[nodiscard]] bool refresh_due(int interval_ms) const;

 private:
  mutable std::mutex mutex_;
  TimeSyncStatus status_;
  std::chrono::steady_clock::time_point steady_anchor_{};
  std::int64_t synchronized_anchor_ms_{0};
};

std::string normalize_signaling_http_url(std::string_view url);

class VehicleTeleopRuntime {
 public:
  VehicleTeleopRuntime(VehicleConfig config, std::string signaling_url, std::string device_token, int telemetry_interval_ms = 100);
  ~VehicleTeleopRuntime();

  Json register_online();
  Json register_offline();
  bool discover_session(std::int64_t timestamp_ms);
  Json poll_and_execute(std::int64_t timestamp_ms);
  Json run(int duration_ms, int poll_interval_ms, int session_wait_ms, bool log_controls);
  [[nodiscard]] Json summary() const;

 private:
  void start_session(std::string session_id, std::int64_t timestamp_ms);
  TimeSyncStatus refresh_time_sync();

  VehicleConfig config_;
  std::string signaling_http_url_;
  std::string device_token_;
  int telemetry_interval_ms_;
  HttpClient http_;
  SynchronizedClock clock_;
  std::string session_id_;
  std::unique_ptr<VehicleControlService> service_;
  std::uint64_t processed_control_commands_{0};
  std::optional<ControlCommand> last_applied_command_;
  std::vector<Json> control_receive_logs_;
};

}  // namespace mine_teleop
