#include "mine_teleop/http.hpp"
#include "mine_teleop/platform.hpp"

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

Json decode_json_response(const HttpResponse& response) {
  if (response.status < 200 || response.status >= 300) {
    throw HttpStatusError(
        response.status,
        "HTTP request failed with status " + std::to_string(response.status) + ": " +
            response.body.substr(0, 512));
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
  std::lock_guard lock(impl_->mutex);
  CURL* curl = impl_->curl;
  curl_easy_reset(curl);
  HttpResponse response;
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
      curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
    } else if (const auto* ca_bundle = std::getenv("CURL_CA_BUNDLE"); ca_bundle != nullptr && *ca_bundle != '\0') {
      curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
    } else if (const auto* ca_file = std::getenv("SSL_CERT_FILE"); ca_file != nullptr && *ca_file != '\0') {
      curl_easy_setopt(curl, CURLOPT_CAINFO, ca_file);
    }
    if (impl_->resolve_entries != nullptr) curl_easy_setopt(curl, CURLOPT_RESOLVE, impl_->resolve_entries);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mine-teleop-cpp/0.2");
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
    if (result != CURLE_OK) {
      const auto detail = error_buffer.front() == '\0' ? curl_easy_strerror(result) : error_buffer.data();
      throw HttpTransportError(std::string("HTTP request failed: ") + detail);
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  } catch (...) {
    if (headers != nullptr) curl_slist_free_all(headers);
    throw;
  }
  if (headers != nullptr) curl_slist_free_all(headers);
  return response;
}

Json TimeSyncStatus::to_json() const {
  return {
      {"time_domain", "signaling_server"},
      {"synchronized", synchronized},
      {"offset_ms", offset_ms},
      {"round_trip_ms", round_trip_ms},
      {"uncertainty_ms", uncertainty_ms},
      {"synchronized_at_local_ms", synchronized_at_local_ms},
      {"sample_count", sample_count},
  };
}

bool TimeSyncStatus::acceptable(int max_uncertainty_ms) const {
  return max_uncertainty_ms >= 0 && synchronized && uncertainty_ms <= max_uncertainty_ms;
}

TimeSyncStatus SynchronizedClock::synchronize(
    const HttpClient& http, std::string_view signaling_origin, int sample_count) {
  if (sample_count < 3 || sample_count > 15) throw std::invalid_argument("time sync sample count must be between 3 and 15");
  const auto origin = normalize_signaling_http_url(signaling_origin);
  struct Sample {
    std::int64_t offset_ms;
    std::int64_t round_trip_ms;
  };
  std::vector<Sample> samples;
  samples.reserve(static_cast<std::size_t>(sample_count));
  for (int index = 0; index < sample_count; ++index) {
    const auto client_send_ms = mine_teleop::now_ms();
    const auto response = http.get_json(
        origin + "/time?client_send_ms=" + std::to_string(client_send_ms));
    const auto client_receive_ms = mine_teleop::now_ms();
    const auto echoed_client_send_ms = response.at("client_send_ms").get<std::int64_t>();
    const auto server_receive_ms = response.at("server_receive_ms").get<std::int64_t>();
    const auto server_send_ms = response.at("server_send_ms").get<std::int64_t>();
    if (echoed_client_send_ms != client_send_ms || server_send_ms < server_receive_ms) {
      throw std::runtime_error("signaling time endpoint returned an invalid four-timestamp sample");
    }
    const auto server_processing_ms = server_send_ms - server_receive_ms;
    const auto round_trip_ms = std::max<std::int64_t>(0, client_receive_ms - client_send_ms - server_processing_ms);
    const auto offset_ms = ((server_receive_ms - client_send_ms) + (server_send_ms - client_receive_ms)) / 2;
    samples.push_back({offset_ms, round_trip_ms});
  }
  std::sort(samples.begin(), samples.end(), [](const auto& left, const auto& right) {
    return left.round_trip_ms < right.round_trip_ms;
  });
  const auto selected_count = std::min<std::size_t>(3, samples.size());
  std::vector<std::int64_t> offsets;
  offsets.reserve(selected_count);
  for (std::size_t index = 0; index < selected_count; ++index) offsets.push_back(samples[index].offset_ms);
  std::sort(offsets.begin(), offsets.end());
  const auto selected_offset_ms = offsets[offsets.size() / 2];
  std::int64_t offset_spread_ms = 0;
  for (const auto offset_ms : offsets) {
    offset_spread_ms = std::max(offset_spread_ms, std::abs(offset_ms - selected_offset_ms));
  }
  TimeSyncStatus next{
      true,
      selected_offset_ms,
      samples.front().round_trip_ms,
      std::max<std::int64_t>((samples.front().round_trip_ms + 1) / 2, offset_spread_ms),
      mine_teleop::now_ms(),
      static_cast<int>(samples.size()),
  };

  const auto next_steady_anchor = std::chrono::steady_clock::now();
  const auto proposed_synchronized_ms = next.synchronized_at_local_ms + next.offset_ms;
  std::lock_guard lock(mutex_);
  synchronized_anchor_ms_ = proposed_synchronized_ms;
  steady_anchor_ = next_steady_anchor;
  status_ = next;
  return status_;
}

std::int64_t SynchronizedClock::now_ms() const {
  std::lock_guard lock(mutex_);
  if (!status_.synchronized) return mine_teleop::now_ms();
  return synchronized_anchor_ms_ +
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - steady_anchor_).count();
}

std::int64_t SynchronizedClock::from_local_system_ms(std::int64_t local_time_ms) const {
  std::lock_guard lock(mutex_);
  return status_.synchronized ? local_time_ms + status_.offset_ms : local_time_ms;
}

TimeSyncStatus SynchronizedClock::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

bool SynchronizedClock::refresh_due(int interval_ms) const {
  if (interval_ms <= 0) throw std::invalid_argument("time sync refresh interval must be positive");
  std::lock_guard lock(mutex_);
  return !status_.synchronized ||
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - steady_anchor_).count() >=
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
