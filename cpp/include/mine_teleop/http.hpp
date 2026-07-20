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

  VehicleConfig config_;
  std::string signaling_http_url_;
  std::string device_token_;
  int telemetry_interval_ms_;
  HttpClient http_;
  std::string session_id_;
  std::unique_ptr<VehicleControlService> service_;
  std::uint64_t processed_control_commands_{0};
  std::optional<ControlCommand> last_applied_command_;
  std::vector<Json> control_receive_logs_;
};

}  // namespace mine_teleop
