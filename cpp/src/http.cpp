#include "mine_teleop/http.hpp"
#include "mine_teleop/curl_tls.hpp"
#include "mine_teleop/platform.hpp"
#include "mine_teleop/websocket.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace mine_teleop {
namespace {

class CurlGlobal {
 public:
  CurlGlobal() {
    initialize_network_process();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("curl_global_init failed");
    }
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_curl_global() {
  static CurlGlobal global;
  static_cast<void>(global);
}

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* output) {
  const auto bytes = size * count;
  static_cast<std::string*>(output)->append(data, bytes);
  return bytes;
}

struct PairedTime {
  std::int64_t wall_ms;
  std::chrono::steady_clock::time_point steady;
  std::int64_t uncertainty_ms;
};

PairedTime read_paired_time(const TimeSyncClockSource& source) {
  const auto before = source.steady_now();
  const auto wall = source.wall_now_ms();
  const auto after = source.steady_now();
  if (after < before) throw std::runtime_error("time sync monotonic clock moved backwards");
  const auto width_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(after - before).count();
  // The wall read happened somewhere inside this bracket. Include one ms for
  // the wall timestamp's integer quantization, even if both reads were fast.
  return {wall, before + (after - before) / 2, (width_ns + 1999999) / 2000000 + 1};
}

const TimeSyncClockSource& process_clock_source() {
  static const TimeSyncClockSource source{
      [] { return mine_teleop::now_ms(); }, [] { return std::chrono::steady_clock::now(); }};
  return source;
}

std::int64_t steady_ns(std::chrono::steady_clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
}

void record_candidate_history(TimeSyncStatus& next, const TimeSyncStatus& previous,
                              std::optional<int> max_uncertainty_ms) {
  const auto policy = max_uncertainty_ms.value_or(-1);
  const bool healthy = next.synchronized && next.anchor_accepted &&
      (!max_uncertainty_ms || next.acceptable(*max_uncertainty_ms));
  // The configured production policy is constant. If a caller changes it,
  // previous classifications cannot be reused under the new error budget.
  const auto previous_count = previous.candidate_classification_max_uncertainty_ms == policy
      ? previous.candidate_consecutive_anomalies : 0;
  next.candidate_classification_max_uncertainty_ms = policy;
  next.candidate_consecutive_anomalies = healthy ? 0 : std::min(3, previous_count + 1);
  next.bad_cohort_revision = previous.bad_cohort_revision + (healthy ? 0 : 1);
  next.brake_cohort_revision = previous.brake_cohort_revision +
      (!healthy && previous_count < 3 && next.candidate_consecutive_anomalies == 3 ? 1 : 0);
}

struct TimeProbeWire {
  CURL* handle{nullptr};
  std::string* body{nullptr};
  std::int64_t sent_utc_ms{0}, received_utc_ms{0};
  std::chrono::steady_clock::time_point sent_steady{}, received_steady{};
  std::int64_t sent_pair_uncertainty_ms{0}, received_pair_uncertainty_ms{0};
  int requests_sent{0};
  Json tcp_before_send, tcp_at_receive;
};

Json time_probe_tcp(CURL* handle) {
  curl_socket_t socket = CURL_SOCKET_BAD;
  if (curl_easy_getinfo(handle, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK || socket == CURL_SOCKET_BAD)
    return {{"socket_available", false}, {"tcp_info_available", false}};
  return websocket_transport_diagnostics(static_cast<SocketHandle>(socket));
}

int observe_time_request(CURL* handle, curl_infotype type, char*, std::size_t, void* context) noexcept {
  if (type != CURLINFO_HEADER_OUT || context == nullptr) return 0;
  auto& wire = *static_cast<TimeProbeWire*>(context);
  // Ignore internal curl handles, TLS records, headers and content. No payload is logged.
  if (handle != wire.handle) return 0;
  const auto sent = read_paired_time(process_clock_source());
  wire.sent_utc_ms = sent.wall_ms;
  wire.sent_steady = sent.steady;
  wire.sent_pair_uncertainty_ms = sent.uncertainty_ms;
  wire.received_utc_ms = 0;
  ++wire.requests_sent;
  try { wire.tcp_before_send = time_probe_tcp(handle); } catch (...) {}
  return 0;
}

std::size_t receive_time_body(char* data, std::size_t size, std::size_t count, void* context) noexcept {
  auto& wire = *static_cast<TimeProbeWire*>(context);
  const auto received = read_paired_time(process_clock_source());
  wire.received_utc_ms = received.wall_ms;
  wire.received_steady = received.steady;
  wire.received_pair_uncertainty_ms = received.uncertainty_ms;
  try {
    wire.tcp_at_receive = time_probe_tcp(wire.handle);
    wire.body->append(data, size * count);
    return size * count;
  } catch (...) { return 0; }
}

Json decode_json_response(const HttpResponse& response) {
  if (response.status < 200 || response.status >= 300) {
    std::string issue_code;
    try {
      const auto body = Json::parse(response.body);
      if (body.is_object()) issue_code = body.value("issue_code", "");
    } catch (const Json::exception&) {
      // Preserve the original response below.  Error bodies from proxies or
      // older servers are not required to be JSON.
    }
    throw HttpStatusError(
        response.status,
        "HTTP request failed with status " + std::to_string(response.status) + ": " +
            response.body.substr(0, 512),
        std::move(issue_code),
        response.body.substr(0, 4096));
  }
  try {
    auto value = Json::parse(response.body);
    if (!value.is_object()) throw std::runtime_error("expected JSON object response");
    return value;
  } catch (const Json::exception& error) {
    throw std::runtime_error(std::string("invalid JSON response: ") + error.what());
  }
}

}  // namespace

struct HttpClient::Impl {
  CURL* curl{nullptr};
  curl_slist* resolve_entries{nullptr};
  std::filesystem::path ca_bundle;
  std::mutex mutex;

  Impl(std::vector<std::string> entries, std::filesystem::path next_ca_bundle)
      : ca_bundle(std::move(next_ca_bundle)) {
    curl = curl_easy_init();
    if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");
    try {
      for (const auto& entry : entries) {
        if (entry.empty() || entry.find_first_of("\r\n") != std::string::npos) {
          throw std::invalid_argument("HTTP resolve entry is invalid");
        }
        auto* next = curl_slist_append(resolve_entries, entry.c_str());
        if (next == nullptr) throw std::runtime_error("cannot allocate HTTP resolve entries");
        resolve_entries = next;
      }
    } catch (...) {
      curl_easy_cleanup(curl);
      curl = nullptr;
      if (resolve_entries != nullptr) curl_slist_free_all(resolve_entries);
      resolve_entries = nullptr;
      throw;
    }
  }

  ~Impl() {
    if (curl != nullptr) curl_easy_cleanup(curl);
    if (resolve_entries != nullptr) curl_slist_free_all(resolve_entries);
  }
};

HttpClient::HttpClient(std::chrono::milliseconds timeout)
    : HttpClient(timeout, {}, {}) {}

HttpClient::HttpClient(
    std::chrono::milliseconds timeout,
    std::vector<std::string> resolve_entries,
    std::filesystem::path ca_bundle)
    : timeout_(timeout) {
  if (timeout_.count() <= 0) throw std::invalid_argument("HTTP timeout must be positive");
  ensure_curl_global();
  impl_ = std::make_shared<Impl>(std::move(resolve_entries), std::move(ca_bundle));
}

HttpResponse HttpClient::get(std::string_view url) const { return request("GET", url, "", {}); }

HttpResponse HttpClient::get(std::string_view url, const HttpHeaders& headers) const {
  return request("GET", url, "", headers);
}

HttpResponse HttpClient::post_json(std::string_view url, const Json& payload) const {
  return request("POST", url, payload.dump(), {});
}

Json HttpClient::get_json(std::string_view url) const { return decode_json_response(get(url)); }

Json HttpClient::get_json(std::string_view url, const HttpHeaders& headers) const {
  return decode_json_response(get(url, headers));
}

Json HttpClient::post_json_response(std::string_view url, const Json& payload) const {
  return decode_json_response(post_json(url, payload));
}

std::string HttpClient::url_encode(std::string_view value) const {
  std::lock_guard lock(impl_->mutex);
  char* encoded = curl_easy_escape(impl_->curl, value.data(), static_cast<int>(value.size()));
  if (encoded == nullptr) {
    throw std::runtime_error("curl_easy_escape failed");
  }
  std::string result(encoded);
  curl_free(encoded);
  return result;
}

HttpResponse HttpClient::request(
    std::string_view method,
    std::string_view url,
    std::string_view body,
    const HttpHeaders& request_headers) const {
  if (!url.starts_with("http://") && !url.starts_with("https://")) {
    throw std::invalid_argument("HTTP URL must use http or https");
  }
  const auto waiting_started = std::chrono::steady_clock::now();
  std::lock_guard lock(impl_->mutex);
  const auto acquired = std::chrono::steady_clock::now();
  CURL* curl = impl_->curl;
  curl_easy_reset(curl);
  HttpResponse response;
  const bool time_probe = method == "GET" && url.find("/time?client_send_ms=") != std::string_view::npos;
  TimeProbeWire wire;
  wire.handle = curl;
  wire.body = &response.body;
  curl_slist* headers = nullptr;
  const std::string request_url(url);
  const std::string ca_bundle = impl_->ca_bundle.string();
  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  try {
    curl_easy_setopt(curl, CURLOPT_URL, request_url.c_str());
    // Signaling is a safety-critical direct path. Do not inherit desktop or
    // service proxy variables; explicit app-local resolution is handled below.
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_.count()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<std::int64_t>(timeout_.count(), 3000)));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer.data());
    if (!ca_bundle.empty()) {
      configure_curl_custom_ca(curl, ca_bundle.c_str());
    } else if (const auto* ca_bundle = std::getenv("CURL_CA_BUNDLE"); ca_bundle != nullptr && *ca_bundle != '\0') {
      curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
    } else if (const auto* ca_file = std::getenv("SSL_CERT_FILE"); ca_file != nullptr && *ca_file != '\0') {
      curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    }
    if (impl_->resolve_entries != nullptr) curl_easy_setopt(curl, CURLOPT_RESOLVE, impl_->resolve_entries);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mine-teleop-cpp/0.2");
    if (time_probe) {
      curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);
      curl_easy_setopt(curl, CURLOPT_DEBUGFUNCTION, observe_time_request);
      curl_easy_setopt(curl, CURLOPT_DEBUGDATA, &wire);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, receive_time_body);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wire);
    }
    for (const auto& [name, value] : request_headers) {
      if (name.empty() || name.find_first_of(":\r\n") != std::string::npos ||
          value.find_first_of("\r\n") != std::string::npos) {
        throw std::invalid_argument("HTTP header contains unsupported characters");
      }
      const auto line = name + ": " + value;
      auto* next = curl_slist_append(headers, line.c_str());
      if (next == nullptr) throw std::runtime_error("cannot allocate HTTP request headers");
      headers = next;
    }
    if (method == "POST") {
      auto* next = curl_slist_append(headers, "Content-Type: application/json");
      if (next == nullptr) throw std::runtime_error("cannot allocate HTTP request headers");
      headers = next;
    }
    if (headers != nullptr) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (method == "POST") {
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    const auto result = curl_easy_perform(curl);
    if (time_probe) {
      response.timing = {{"client_mutex_wait_ms", std::chrono::duration_cast<std::chrono::milliseconds>(acquired - waiting_started).count()},
                         {"curl_result", static_cast<int>(result)}, {"requests_sent", wire.requests_sent},
                         {"request_sent_at_utc_ms", wire.sent_utc_ms > 0 ? Json(wire.sent_utc_ms) : Json(nullptr)},
                         {"body_received_at_utc_ms", wire.received_utc_ms > 0 ? Json(wire.received_utc_ms) : Json(nullptr)},
                         {"request_sent_at_monotonic_ns", wire.sent_utc_ms > 0 ? Json(steady_ns(wire.sent_steady)) : Json(nullptr)},
                         {"body_received_at_monotonic_ns", wire.received_utc_ms > 0 ? Json(steady_ns(wire.received_steady)) : Json(nullptr)},
                         {"request_timestamp_uncertainty_ms", wire.sent_pair_uncertainty_ms},
                         {"body_timestamp_uncertainty_ms", wire.received_pair_uncertainty_ms},
                         {"exchange_monotonic_ms", wire.sent_utc_ms > 0 && wire.received_utc_ms > 0
                             ? Json(std::chrono::duration_cast<std::chrono::milliseconds>(wire.received_steady - wire.sent_steady).count()) : Json(nullptr)},
                         {"tcp_before_send", wire.tcp_before_send}, {"tcp_at_receive", wire.tcp_at_receive}};
      for (const auto& [name, info] : std::initializer_list<std::pair<const char*, CURLINFO>>{
          {"dns_ms", CURLINFO_NAMELOOKUP_TIME}, {"connect_ms", CURLINFO_CONNECT_TIME},
          {"tls_ms", CURLINFO_APPCONNECT_TIME}, {"first_byte_ms", CURLINFO_STARTTRANSFER_TIME}, {"total_ms", CURLINFO_TOTAL_TIME}}) {
        double seconds = 0;
        if (curl_easy_getinfo(curl, info, &seconds) == CURLE_OK) response.timing[name] = seconds * 1000.0;
      }
      char* peer = nullptr;
      if (curl_easy_getinfo(curl, CURLINFO_PRIMARY_IP, &peer) == CURLE_OK && peer) response.timing["peer_address"] = peer;
      long connections = 0;
      if (curl_easy_getinfo(curl, CURLINFO_NUM_CONNECTS, &connections) == CURLE_OK) response.timing["new_connections"] = connections;
    }
    if (result != CURLE_OK) {
      const auto detail = error_buffer.front() == '\0' ? curl_easy_strerror(result) : error_buffer.data();
      throw HttpTransportError(std::string("HTTP request failed: ") + detail, std::move(response.timing));
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  } catch (...) {
    curl_easy_reset(curl);
    if (headers != nullptr) curl_slist_free_all(headers);
    throw;
  }
  // Clear request-owned callback/header/error-buffer pointers before their
  // storage dies. Reset preserves curl's live connections and DNS/TLS caches.
  curl_easy_reset(curl);
  if (headers != nullptr) curl_slist_free_all(headers);
  return response;
}

Json TimeSyncStatus::to_json(bool include_samples) const {
  Json result = {
      {"time_domain", "signaling_server"},
      {"synchronized", synchronized},
      {"offset_ms", offset_ms},
      {"round_trip_ms", round_trip_ms},
      {"uncertainty_ms", uncertainty_ms},
      {"synchronized_at_local_ms", synchronized_at_local_ms},
      {"sample_count", sample_count},
      {"anchor_accepted", anchor_accepted},
      {"accepted_anchor_local_ms", accepted_anchor_local_ms},
      {"candidate_generation", candidate_generation},
      {"candidate_observed_steady_ms", candidate_observed_steady_ms},
      {"bad_cohort_revision", bad_cohort_revision},
      {"brake_cohort_revision", brake_cohort_revision},
      {"candidate_consecutive_anomalies", candidate_consecutive_anomalies},
      {"candidate_classification_max_uncertainty_ms", candidate_classification_max_uncertainty_ms},
      {"has_accepted_anchor", has_accepted_anchor},
      {"accepted_anchor_uncertainty_ms", accepted_anchor_uncertainty_ms},
      {"accepted_anchor_age_ms", accepted_anchor_age_ms},
  };
  if (include_samples) result["samples"] = samples ? *samples : Json::array();
  return result;
}

bool TimeSyncStatus::acceptable(int max_uncertainty_ms) const {
  return max_uncertainty_ms >= 0 && synchronized && uncertainty_ms <= max_uncertainty_ms;
}

Json TimeSyncReadinessState::to_json() const {
  return {{"ready", ready}, {"latest_candidate_healthy", latest_candidate_healthy},
      {"has_trusted_anchor", has_trusted_anchor}, {"candidate_timed_out", candidate_timed_out},
      {"anchor_timed_out", anchor_timed_out}, {"consecutive_anomalies", consecutive_anomalies},
      {"candidate_generation", candidate_generation}, {"last_candidate_steady_ms", last_candidate_steady_ms},
      {"bad_cohort_revision", bad_cohort_revision}, {"brake_cohort_revision", brake_cohort_revision},
      {"classification_policy_matches", classification_policy_matches},
      {"no_new_candidate_timeout_ms", no_new_candidate_timeout_ms}};
}

std::int64_t TimeSyncReadinessGate::candidate_timeout_ms(
    int refresh_interval_ms, int sample_count, int http_timeout_ms, int poll_max_ms) {
  if (refresh_interval_ms <= 0 || sample_count < 3 || sample_count > 15 || http_timeout_ms <= 0 || poll_max_ms <= 0)
    throw std::invalid_argument("invalid time sync absence budget inputs");
  return static_cast<std::int64_t>(refresh_interval_ms) + static_cast<std::int64_t>(sample_count) * http_timeout_ms + poll_max_ms;
}

TimeSyncReadinessState TimeSyncReadinessGate::observe(
    const TimeSyncStatus& candidate, int max_uncertainty_ms, std::int64_t now_steady_ms,
    std::int64_t no_new_candidate_timeout_ms) {
  if (max_uncertainty_ms < 0 || no_new_candidate_timeout_ms <= 0)
    throw std::invalid_argument("invalid time sync readiness budget");
  std::lock_guard lock(mutex_);
  state_.no_new_candidate_timeout_ms = no_new_candidate_timeout_ms;
  state_.classification_policy_matches = candidate.candidate_classification_max_uncertainty_ms == max_uncertainty_ms;
  if (candidate.candidate_generation > state_.candidate_generation) {
    const bool timestamp_valid = candidate.candidate_observed_steady_ms <= now_steady_ms &&
        (!has_observed_candidate_ || candidate.candidate_observed_steady_ms >= state_.last_candidate_steady_ms) &&
        now_steady_ms - candidate.candidate_observed_steady_ms <= no_new_candidate_timeout_ms;
    const bool healthy = timestamp_valid && state_.classification_policy_matches &&
        candidate.anchor_accepted && candidate.has_accepted_anchor &&
        candidate.acceptable(max_uncertainty_ms) && candidate.accepted_anchor_uncertainty_ms <= max_uncertainty_ms;
    state_.candidate_generation = candidate.candidate_generation;
    state_.latest_candidate_healthy = healthy;
    if (timestamp_valid && state_.classification_policy_matches &&
        candidate.candidate_consecutive_anomalies >= 0 && candidate.candidate_consecutive_anomalies <= 3) {
      state_.consecutive_anomalies = candidate.candidate_consecutive_anomalies;
    }
    // A late, reordered, or incompatible snapshot is not a fresh calibration
    // observation. It neither counts nor clears current anomalies; independent
    // absence/anchor expiry and policy mismatch still prohibit readiness.
    state_.bad_cohort_revision = std::max(state_.bad_cohort_revision, candidate.bad_cohort_revision);
    state_.brake_cohort_revision = std::max(state_.brake_cohort_revision, candidate.brake_cohort_revision);
    if (timestamp_valid) {
      state_.last_candidate_steady_ms = candidate.candidate_observed_steady_ms;
      has_observed_candidate_ = true;
    }
  }
  state_.has_trusted_anchor = candidate.has_accepted_anchor &&
      candidate.accepted_anchor_uncertainty_ms >= 0 && candidate.accepted_anchor_uncertainty_ms <= max_uncertainty_ms;
  state_.candidate_timed_out = !has_observed_candidate_ || now_steady_ms < state_.last_candidate_steady_ms ||
      now_steady_ms - state_.last_candidate_steady_ms > no_new_candidate_timeout_ms;
  // Fresh failed candidates must not indefinitely extend a previously accepted
  // mapping. Both absence and accepted-anchor age have independent deadlines.
  state_.anchor_timed_out = !state_.has_trusted_anchor || candidate.accepted_anchor_age_ms < 0 ||
      candidate.accepted_anchor_age_ms > no_new_candidate_timeout_ms;
  state_.ready = state_.classification_policy_matches && state_.has_trusted_anchor &&
      !state_.candidate_timed_out && !state_.anchor_timed_out &&
      state_.consecutive_anomalies < 3;
  return state_;
}

void TimeSyncReadinessGate::reset() {
  std::lock_guard lock(mutex_);
  state_ = {};
  has_observed_candidate_ = false;
}

SynchronizedClock::SynchronizedClock(TimeSyncClockSource source) : source_(std::move(source)) {
  if (!source_.wall_now_ms) source_.wall_now_ms = [] { return mine_teleop::now_ms(); };
  if (!source_.steady_now) source_.steady_now = [] { return std::chrono::steady_clock::now(); };
}

TimeSyncStatus SynchronizedClock::synchronize(
    const HttpClient& http, std::string_view signaling_origin, int sample_count,
    std::optional<int> max_uncertainty_ms) {
  const auto origin = normalize_signaling_http_url(signaling_origin);
  return synchronize([&](std::int64_t sent) {
    return http.get(origin + "/time?client_send_ms=" + std::to_string(sent));
  }, sample_count, max_uncertainty_ms);
}

TimeSyncStatus SynchronizedClock::synchronize(
    const TimeExchange& exchange, int sample_count, std::optional<int> max_uncertainty_ms) {
  if (sample_count < 3 || sample_count > 15) throw std::invalid_argument("time sync sample count must be between 3 and 15");
  if (!exchange) throw std::invalid_argument("time sync exchange must be provided");
  // Serialize refreshes without blocking lightweight status or clock reads.
  std::lock_guard refresh_lock(synchronize_mutex_);
  struct Sample {
    std::int64_t utc_at_receive_ms;
    std::chrono::steady_clock::time_point received_steady;
    std::int64_t round_trip_ms;
    std::int64_t uncertainty_ms;
  };
  std::vector<Sample> samples;
  Json diagnostics = Json::array();
  samples.reserve(static_cast<std::size_t>(sample_count));
  std::optional<std::int64_t> cohort_min_wall_offset, cohort_max_wall_offset;
  const auto clock_step_limit_ms = std::max(1, max_uncertainty_ms.value_or(25));
  auto observe_pair = [&](const PairedTime& pair) {
    const auto wall_offset = pair.wall_ms - steady_ns(pair.steady) / 1000000;
    cohort_min_wall_offset = std::min(cohort_min_wall_offset.value_or(wall_offset), wall_offset);
    cohort_max_wall_offset = std::max(cohort_max_wall_offset.value_or(wall_offset), wall_offset);
    if (*cohort_max_wall_offset - *cohort_min_wall_offset > clock_step_limit_ms) {
      throw std::runtime_error("time sync cohort has inconsistent wall/monotonic clock continuity");
    }
  };
  HttpResponse http_response;
  int index = 0;
  try {
    for (; index < sample_count; ++index) {
      const auto call_sent = read_paired_time(source_);
      observe_pair(call_sent);
      http_response = exchange(call_sent.wall_ms);
      const auto call_received = read_paired_time(source_);
      const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(call_received.steady - call_sent.steady).count();
      const auto response = decode_json_response(http_response);
      const auto echoed_client_send_ms = response.at("client_send_ms").get<std::int64_t>();
      const auto server_receive_ms = response.at("server_receive_ms").get<std::int64_t>();
      const auto server_send_ms = response.at("server_send_ms").get<std::int64_t>();
      if (echoed_client_send_ms != call_sent.wall_ms || server_send_ms < server_receive_ms) {
        throw std::runtime_error("signaling time endpoint returned an invalid four-timestamp sample");
      }
      const auto& timing = http_response.timing;
      const bool wire_timestamps = timing.contains("request_sent_at_utc_ms") && timing.at("request_sent_at_utc_ms").is_number_integer() &&
          timing.contains("body_received_at_utc_ms") && timing.at("body_received_at_utc_ms").is_number_integer() &&
          timing.contains("request_sent_at_monotonic_ns") && timing.at("request_sent_at_monotonic_ns").is_number_integer() &&
          timing.contains("body_received_at_monotonic_ns") && timing.at("body_received_at_monotonic_ns").is_number_integer();
      auto sent = call_sent;
      auto received = call_received;
      if (wire_timestamps) {
        sent = {timing.at("request_sent_at_utc_ms").get<std::int64_t>(),
            std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(timing.at("request_sent_at_monotonic_ns").get<std::int64_t>()))),
            timing.value("request_timestamp_uncertainty_ms", std::int64_t{1})};
        received = {timing.at("body_received_at_utc_ms").get<std::int64_t>(),
            std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                std::chrono::nanoseconds(timing.at("body_received_at_monotonic_ns").get<std::int64_t>()))),
            timing.value("body_timestamp_uncertainty_ms", std::int64_t{1})};
      }
      const auto wall_exchange_ms = received.wall_ms - sent.wall_ms;
      const auto mono_exchange_ms = std::chrono::duration_cast<std::chrono::milliseconds>(received.steady - sent.steady).count();
      const auto server_processing_ms = server_send_ms - server_receive_ms;
      // A wall step must not shorten the network delay or move the UTC anchor.
      const auto round_trip_ms = std::max<std::int64_t>(0, mono_exchange_ms - server_processing_ms);
      const auto offset_ms = ((server_receive_ms - sent.wall_ms) + (server_send_ms - received.wall_ms)) / 2;
      diagnostics.push_back({{"index", index}, {"round_trip_ms", round_trip_ms}, {"offset_ms", offset_ms},
          {"server_processing_ms", server_processing_ms}, {"monotonic_elapsed_ms", elapsed_ms},
          {"wall_clock_elapsed_ms", call_received.wall_ms - call_sent.wall_ms},
          {"wall_clock_step_ms", call_received.wall_ms - call_sent.wall_ms - elapsed_ms},
          {"server_api_gate_wait_ms", response.value("server_api_gate_wait_ms", Json(nullptr))}, {"http", http_response.timing}});
      diagnostics.back()["timestamp_scope"] = wire_timestamps ? "http_header_out_to_body_received" : "call_to_return_fallback";
      diagnostics.back()["server_timestamp_scope"] = response.value("timestamp_scope", "legacy_handler_boundary");
      diagnostics.back()["server_before_handler_ms"] = response.value("server_before_handler_ms", Json(nullptr));
      diagnostics.back()["server_parse_ms"] = response.value("server_parse_ms", Json(nullptr));
      diagnostics.back()["server_processing_monotonic_ms"] = response.value("server_processing_monotonic_ms", Json(nullptr));
      diagnostics.back()["exchange_wall_clock_step_ms"] = wall_exchange_ms - mono_exchange_ms;
      diagnostics.back()["send_timestamp_uncertainty_ms"] = sent.uncertainty_ms;
      diagnostics.back()["receive_timestamp_uncertainty_ms"] = received.uncertainty_ms;
      diagnostics.back()["valid"] = false;
      if (mono_exchange_ms < 0 || wall_exchange_ms < 0 || sent.uncertainty_ms < 0 || received.uncertainty_ms < 0 ||
          std::abs(wall_exchange_ms - mono_exchange_ms) > clock_step_limit_ms ||
          server_processing_ms > mono_exchange_ms + sent.uncertainty_ms + received.uncertainty_ms) {
        throw std::runtime_error("four-timestamp sample has inconsistent wall/monotonic elapsed time or server processing duration");
      }
      observe_pair(sent);
      observe_pair(received);
      observe_pair(call_received);
      diagnostics.back()["valid"] = true;
      // Express NTP's estimate at the paired steady receive boundary, so later
      // projection needs no new wall reading and its possible clock steps.
      const auto utc_at_receive_ms = server_receive_ms + (server_processing_ms + mono_exchange_ms) / 2;
      const auto uncertainty_ms = (round_trip_ms + 1) / 2 + sent.uncertainty_ms + received.uncertainty_ms + 1;
      samples.push_back({utc_at_receive_ms, received.steady, round_trip_ms, uncertainty_ms});
    }
    const auto anchor = read_paired_time(source_);
    observe_pair(anchor);
    std::stable_sort(samples.begin(), samples.end(), [](const auto& left, const auto& right) {
      return left.round_trip_ms < right.round_trip_ms;
    });
    const auto selected_count = std::min<std::size_t>(3, samples.size());
    auto utc_at_anchor = [&](const Sample& sample) {
      return sample.utc_at_receive_ms + std::chrono::duration_cast<std::chrono::milliseconds>(anchor.steady - sample.received_steady).count();
    };
    std::vector<std::int64_t> estimates;
    for (std::size_t selected = 0; selected < selected_count; ++selected) estimates.push_back(utc_at_anchor(samples[selected]));
    std::sort(estimates.begin(), estimates.end());
    const auto selected_utc_ms = estimates[estimates.size() / 2];
    std::int64_t estimate_spread_ms = 0;
    for (const auto estimate : estimates) estimate_spread_ms = std::max(estimate_spread_ms, std::abs(estimate - selected_utc_ms));
    auto cohort_lower_ms = utc_at_anchor(samples.front()) - samples.front().uncertainty_ms;
    auto cohort_upper_ms = utc_at_anchor(samples.front()) + samples.front().uncertainty_ms;
    for (const auto& sample : samples) {
      cohort_lower_ms = std::max(cohort_lower_ms, utc_at_anchor(sample) - sample.uncertainty_ms);
      cohort_upper_ms = std::min(cohort_upper_ms, utc_at_anchor(sample) + sample.uncertainty_ms);
    }
    // Do not hide a server-domain step in the slower, unselected samples. All
    // valid network intervals must admit one common UTC/steady mapping.
    const bool cohort_consistent = cohort_lower_ms <= cohort_upper_ms;
    const auto cohort_span_ms = *cohort_max_wall_offset - *cohort_min_wall_offset;
    TimeSyncStatus next;
    next.synchronized = cohort_consistent;
    next.offset_ms = selected_utc_ms - anchor.wall_ms;
    next.round_trip_ms = samples.front().round_trip_ms;
    next.uncertainty_ms = std::max(samples.front().uncertainty_ms + std::abs(selected_utc_ms - utc_at_anchor(samples.front())),
        estimate_spread_ms) + anchor.uncertainty_ms + cohort_span_ms;
    next.synchronized_at_local_ms = anchor.wall_ms;
    next.sample_count = static_cast<int>(samples.size());
    for (auto& diagnostic : diagnostics) {
      diagnostic["cohort_wall_clock_span_ms"] = cohort_span_ms;
      diagnostic["anchor_timestamp_uncertainty_ms"] = anchor.uncertainty_ms;
      diagnostic["cohort_utc_intervals_consistent"] = cohort_consistent;
    }
    next.samples = std::make_shared<const Json>(std::move(diagnostics));
    std::lock_guard lock(mutex_);
    next.candidate_generation = status_.candidate_generation + 1;
    next.candidate_observed_steady_ms = steady_ns(source_.steady_now()) / 1000000;
    // A rejected cohort changes readiness but never the last accepted mapping.
    if (next.synchronized && (!max_uncertainty_ms || next.acceptable(*max_uncertainty_ms))) {
      synchronized_anchor_ms_ = selected_utc_ms;
      steady_anchor_ = anchor.steady;
      has_accepted_anchor_ = true;
      accepted_offset_ms_ = next.offset_ms;
      next.anchor_accepted = true;
      next.accepted_anchor_local_ms = anchor.wall_ms;
    } else {
      next.accepted_anchor_local_ms = status_.accepted_anchor_local_ms;
    }
    next.has_accepted_anchor = has_accepted_anchor_;
    next.accepted_anchor_uncertainty_ms = next.anchor_accepted ? next.uncertainty_ms : status_.accepted_anchor_uncertainty_ms;
    next.accepted_anchor_age_ms = has_accepted_anchor_ ? std::chrono::duration_cast<std::chrono::milliseconds>(source_.steady_now() - steady_anchor_).count() : 0;
    record_candidate_history(next, status_, max_uncertainty_ms);
    status_ = next;
    return status_;
  } catch (const std::exception& error) {
    Json detail = {{"failed_sample_index", index}, {"completed_samples", diagnostics}, {"http", http_response.timing}};
    if (const auto* transport = dynamic_cast<const HttpTransportError*>(&error)) detail["http_failure"] = transport->timing();
    {
      std::lock_guard lock(mutex_);
      auto failed = status_;
      failed.synchronized = false;
      failed.anchor_accepted = false;
      ++failed.candidate_generation;
      failed.candidate_observed_steady_ms = steady_ns(source_.steady_now()) / 1000000;
      failed.has_accepted_anchor = has_accepted_anchor_;
      failed.samples = std::make_shared<const Json>(diagnostics);
      record_candidate_history(failed, status_, max_uncertainty_ms);
      status_ = std::move(failed);
    }
    throw TimeSyncSampleError(error.what(), std::move(detail));
  }
}

void SynchronizedClock::mark_refresh_failed() {
  std::lock_guard lock(mutex_);
  status_.synchronized = false;
  status_.anchor_accepted = false;
}

std::int64_t SynchronizedClock::now_ms() const {
  std::lock_guard lock(mutex_);
  if (!has_accepted_anchor_) return source_.wall_now_ms();
  return synchronized_anchor_ms_ +
      std::chrono::duration_cast<std::chrono::milliseconds>(source_.steady_now() - steady_anchor_).count();
}

std::int64_t SynchronizedClock::from_steady_time(std::chrono::steady_clock::time_point capture_time) const {
  std::lock_guard lock(mutex_);
  if (!has_accepted_anchor_) {
    const auto local = read_paired_time(source_);
    return local.wall_ms + std::chrono::duration_cast<std::chrono::milliseconds>(capture_time - local.steady).count();
  }
  return synchronized_anchor_ms_ + std::chrono::duration_cast<std::chrono::milliseconds>(capture_time - steady_anchor_).count();
}

std::int64_t SynchronizedClock::from_local_system_ms(std::int64_t local_time_ms) const {
  std::lock_guard lock(mutex_);
  // Capture timestamps also feed encoder PTS. A rejected network sample must
  // not jump that timeline while actuation is safely waiting for recovery.
  return has_accepted_anchor_ ? local_time_ms + accepted_offset_ms_ : local_time_ms;
}

TimeSyncStatus SynchronizedClock::status() const {
  std::lock_guard lock(mutex_);
  auto current = status_;
  current.has_accepted_anchor = has_accepted_anchor_;
  current.accepted_anchor_age_ms = has_accepted_anchor_ ?
      std::chrono::duration_cast<std::chrono::milliseconds>(source_.steady_now() - steady_anchor_).count() : 0;
  return current;
}

bool SynchronizedClock::refresh_due(int interval_ms) const {
  if (interval_ms <= 0) throw std::invalid_argument("time sync refresh interval must be positive");
  std::lock_guard lock(mutex_);
  return !status_.synchronized || !status_.anchor_accepted ||
      std::chrono::duration_cast<std::chrono::milliseconds>(source_.steady_now() - steady_anchor_).count() >=
          interval_ms;
}

std::string normalize_signaling_http_url(std::string_view url) {
  std::string value(url);
  if (value.starts_with("ws://")) value.replace(0, 5, "http://");
  if (value.starts_with("wss://")) value.replace(0, 6, "https://");
  if (value.ends_with("/signaling")) value.resize(value.size() - std::string_view("/signaling").size());
  while (!value.empty() && value.back() == '/') value.pop_back();
  if (!value.starts_with("http://") && !value.starts_with("https://")) {
    throw std::invalid_argument("signaling URL must use ws, wss, http, or https");
  }
  return value;
}

}  // namespace mine_teleop
