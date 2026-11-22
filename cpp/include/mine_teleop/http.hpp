#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
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
  // One refresh cohort, including a failed one, is one new observation.
  // Repeated status polls and mark_refresh_failed calls do not create samples.
  std::uint64_t candidate_generation{0};
  std::int64_t candidate_observed_steady_ms{0};
  // Producer-owned event history survives a later normal cohort and latest-
  // snapshot coalescing. Only current consecutive anomalies clear on normal.
  std::uint64_t bad_cohort_revision{0};
  std::uint64_t brake_cohort_revision{0};
  int candidate_consecutive_anomalies{0};
  int candidate_classification_max_uncertainty_ms{-1};
  bool has_accepted_anchor{false};
  std::int64_t accepted_anchor_uncertainty_ms{0};
  std::int64_t accepted_anchor_age_ms{0};
  // Share immutable diagnostics so control-loop status reads stay lightweight.
  std::shared_ptr<const Json> samples;

  [[nodiscard]] Json to_json(bool include_samples = false) const;
  [[nodiscard]] bool acceptable(int max_uncertainty_ms) const;
};

struct TimeSyncReadinessState {
  bool ready{false};
  bool latest_candidate_healthy{false};
  bool has_trusted_anchor{false};
  bool candidate_timed_out{true};
  bool anchor_timed_out{true};
  int consecutive_anomalies{0};
  std::uint64_t candidate_generation{0};
  std::uint64_t bad_cohort_revision{0};
  std::uint64_t brake_cohort_revision{0};
  bool classification_policy_matches{false};
  std::int64_t last_candidate_steady_ms{0};
  std::int64_t no_new_candidate_timeout_ms{0};
  [[nodiscard]] Json to_json() const;
};

class TimeSyncReadinessGate {
 public:
  // Independent absence deadline derived from the configured refresh schedule
  // plus one complete bounded cohort and its maximum recovery poll interval.
  // This is a scheduling budget, not permission to change UTC uncertainty.
  [[nodiscard]] static std::int64_t candidate_timeout_ms(
      int refresh_interval_ms, int sample_count, int http_timeout_ms = 500, int poll_max_ms = 500);
  TimeSyncReadinessState observe(const TimeSyncStatus& candidate, int max_uncertainty_ms,
                                std::int64_t now_steady_ms, std::int64_t no_new_candidate_timeout_ms);
  void reset();

 private:
  std::mutex mutex_;
  TimeSyncReadinessState state_;
  bool has_observed_candidate_{false};
};

// Dependency injection keeps clock-step regressions deterministic without
// changing the host clock. The production default uses system/steady clocks.
struct TimeSyncClockSource {
  std::function<std::int64_t()> wall_now_ms;
  std::function<std::chrono::steady_clock::time_point()> steady_now;
};

class SynchronizedClock {
 public:
  explicit SynchronizedClock(TimeSyncClockSource source = {});
  using TimeExchange = std::function<HttpResponse(std::int64_t client_send_ms)>;
  TimeSyncStatus synchronize(const HttpClient& http, std::string_view signaling_origin, int sample_count = 7,
                             std::optional<int> max_uncertainty_ms = std::nullopt);
  TimeSyncStatus synchronize(const TimeExchange& exchange, int sample_count = 7,
                             std::optional<int> max_uncertainty_ms = std::nullopt);
  void mark_refresh_failed();
  [[nodiscard]] std::int64_t now_ms() const;
  [[nodiscard]] std::int64_t from_steady_time(std::chrono::steady_clock::time_point capture_time) const;
  // Legacy wall-clock timestamps refer to the wall domain at the accepted
  // anchor. A wall-only historical value cannot identify which side of a
  // system clock step it came from; capture code should use from_steady_time.
  [[nodiscard]] std::int64_t from_local_system_ms(std::int64_t local_time_ms) const;
  [[nodiscard]] TimeSyncStatus status() const;
  [[nodiscard]] bool refresh_due(int interval_ms) const;

 private:
  TimeSyncClockSource source_;
  std::mutex synchronize_mutex_;
  mutable std::mutex mutex_;
  TimeSyncStatus status_;
  std::chrono::steady_clock::time_point steady_anchor_{};
  std::int64_t synchronized_anchor_ms_{0};
  bool has_accepted_anchor_{false};
  std::int64_t accepted_offset_ms_{0};
};

std::string normalize_signaling_http_url(std::string_view url);

}  // namespace mine_teleop
