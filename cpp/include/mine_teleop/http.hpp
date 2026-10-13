#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "mine_teleop/core.hpp"

namespace mine_teleop {

struct HttpResponse {
  long status{0};
  std::string body;
  Json timing;
};

using HttpHeaders = std::vector<std::pair<std::string, std::string>>;

class HttpStatusError : public std::runtime_error {
 public:
  HttpStatusError(
      long status,
      std::string message,
      std::string issue_code = {},
      std::string response_body = {})
      : std::runtime_error(std::move(message)),
        status_(status),
        issue_code_(std::move(issue_code)),
        response_body_(std::move(response_body)) {}

  [[nodiscard]] long status() const noexcept { return status_; }
  [[nodiscard]] const std::string& issue_code() const noexcept { return issue_code_; }
  [[nodiscard]] const std::string& response_body() const noexcept { return response_body_; }

 private:
  long status_;
  std::string issue_code_;
  std::string response_body_;
};

class HttpTransportError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
  HttpTransportError(std::string message, Json timing)
      : std::runtime_error(std::move(message)), timing_(std::move(timing)) {}
  [[nodiscard]] const Json& timing() const { return timing_; }
 private:
  Json timing_;
};

class TimeSyncSampleError : public std::runtime_error {
 public:
  TimeSyncSampleError(std::string message, Json diagnostics)
      : std::runtime_error(std::move(message)), diagnostics_(std::move(diagnostics)) {}
  [[nodiscard]] const Json& diagnostics() const { return diagnostics_; }
 private:
  Json diagnostics_;
};

class HttpClient {
 public:
  explicit HttpClient(std::chrono::milliseconds timeout = std::chrono::seconds(5));
  HttpClient(
      std::chrono::milliseconds timeout,
      std::vector<std::string> resolve_entries,
      std::filesystem::path ca_bundle);

  [[nodiscard]] HttpResponse get(std::string_view url) const;
  [[nodiscard]] HttpResponse get(std::string_view url, const HttpHeaders& headers) const;
  [[nodiscard]] HttpResponse post_json(std::string_view url, const Json& payload) const;
  [[nodiscard]] Json get_json(std::string_view url) const;
  [[nodiscard]] Json get_json(std::string_view url, const HttpHeaders& headers) const;
  [[nodiscard]] Json post_json_response(std::string_view url, const Json& payload) const;
  [[nodiscard]] std::string url_encode(std::string_view value) const;

 private:
  struct Impl;
  [[nodiscard]] HttpResponse request(
      std::string_view method,
      std::string_view url,
      std::string_view body,
      const HttpHeaders& headers) const;

  std::chrono::milliseconds timeout_;
  std::shared_ptr<Impl> impl_;
};

struct TimeSyncStatus {
  bool synchronized{false};
  std::int64_t offset_ms{0};
  std::int64_t round_trip_ms{0};
  std::int64_t uncertainty_ms{0};
  std::int64_t synchronized_at_local_ms{0};
  int sample_count{0};
  bool anchor_accepted{false};
  std::int64_t accepted_anchor_local_ms{0};
  // Share immutable diagnostics so control-loop status reads stay lightweight.
  std::shared_ptr<const Json> samples;

  [[nodiscard]] Json to_json(bool include_samples = false) const;
  [[nodiscard]] bool acceptable(int max_uncertainty_ms) const;
};

class SynchronizedClock {
 public:
  TimeSyncStatus synchronize(const HttpClient& http, std::string_view signaling_origin, int sample_count = 7,
                             std::optional<int> max_uncertainty_ms = std::nullopt);
  void mark_refresh_failed();
  [[nodiscard]] std::int64_t now_ms() const;
  [[nodiscard]] std::int64_t from_local_system_ms(std::int64_t local_time_ms) const;
  [[nodiscard]] TimeSyncStatus status() const;
  [[nodiscard]] bool refresh_due(int interval_ms) const;

 private:
  mutable std::mutex mutex_;
  TimeSyncStatus status_;
  std::chrono::steady_clock::time_point steady_anchor_{};
  std::int64_t synchronized_anchor_ms_{0};
  bool has_accepted_anchor_{false};
  std::int64_t accepted_offset_ms_{0};
};

std::string normalize_signaling_http_url(std::string_view url);

}  // namespace mine_teleop
