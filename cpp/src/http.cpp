#include "mine_teleop/http.hpp"
#include "mine_teleop/curl_tls.hpp"
#include "mine_teleop/platform.hpp"

#include <curl/curl.h>

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <Security/SecCertificate.h>
#include <Security/SecCertificateOIDs.h>
#include <Security/SecImportExport.h>
#else
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <thread>

namespace mine_teleop {
namespace {

#if defined(__APPLE__)
[[nodiscard]] bool apple_ca_property_is_yes(CFTypeRef value) {
  return value != nullptr && CFGetTypeID(value) == CFStringGetTypeID() &&
      CFStringCompare(
          static_cast<CFStringRef>(value),
          CFSTR("Yes"),
          kCFCompareCaseInsensitive) == kCFCompareEqualTo;
}

[[nodiscard]] bool is_apple_ca_certificate(SecCertificateRef certificate) {
  const void* keys[] = {kSecOIDBasicConstraints};
  CFArrayRef requested_keys = CFArrayCreate(
      kCFAllocatorDefault,
      keys,
      static_cast<CFIndex>(std::size(keys)),
      &kCFTypeArrayCallBacks);
  if (requested_keys == nullptr) return false;

  CFErrorRef error = nullptr;
  CFDictionaryRef values = SecCertificateCopyValues(certificate, requested_keys, &error);
  CFRelease(requested_keys);
  if (error != nullptr) CFRelease(error);
  if (values == nullptr) return false;

  const auto* basic_constraints = static_cast<CFDictionaryRef>(
      CFDictionaryGetValue(values, kSecOIDBasicConstraints));
  if (basic_constraints == nullptr) {
    CFRelease(values);
    return false;
  }
  const auto* properties = static_cast<CFArrayRef>(
      CFDictionaryGetValue(basic_constraints, kSecPropertyKeyValue));
  if (properties == nullptr) {
    CFRelease(values);
    return false;
  }

  bool is_ca = false;
  for (CFIndex index = 0; index < CFArrayGetCount(properties); ++index) {
    const auto* property = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(properties, index));
    if (property == nullptr) continue;
    const auto* label = static_cast<CFStringRef>(
        CFDictionaryGetValue(property, kSecPropertyKeyLabel));
    if (label == nullptr ||
        CFStringCompare(label, CFSTR("Certificate Authority"), 0) != kCFCompareEqualTo) {
      continue;
    }
    is_ca = apple_ca_property_is_yes(CFDictionaryGetValue(property, kSecPropertyKeyValue));
    break;
  }
  CFRelease(values);
  return is_ca;
}

void validate_pem_ca_bundle_contents(const std::filesystem::path& ca_bundle) {
  std::ifstream input(ca_bundle, std::ios::binary);
  const std::string contents{
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>()};
  CFDataRef data = CFDataCreate(
      kCFAllocatorDefault,
      reinterpret_cast<const UInt8*>(contents.data()),
      static_cast<CFIndex>(contents.size()));
  if (data == nullptr) {
    throw std::invalid_argument(
        "TLS CA bundle cannot allocate PEM validation data: " + ca_bundle.string());
  }

  SecExternalFormat input_format = kSecFormatPEMSequence;
  SecExternalItemType item_type = kSecItemTypeAggregate;
  CFArrayRef items = nullptr;
  const OSStatus status = SecItemImport(
      data,
      nullptr,
      &input_format,
      &item_type,
      0,
      nullptr,
      nullptr,
      &items);
  CFRelease(data);
  if (status != errSecSuccess || items == nullptr || CFArrayGetCount(items) == 0) {
    if (items != nullptr) CFRelease(items);
    throw std::invalid_argument(
        "TLS CA bundle contains invalid PEM certificate data: " + ca_bundle.string() +
        " (Security status " + std::to_string(status) + ")");
  }

  bool contains_only_ca_certificates = true;
  for (CFIndex index = 0; index < CFArrayGetCount(items); ++index) {
    const auto* item = CFArrayGetValueAtIndex(items, index);
    if (item == nullptr || CFGetTypeID(item) != SecCertificateGetTypeID()) {
      contains_only_ca_certificates = false;
      break;
    }
    const auto certificate = reinterpret_cast<SecCertificateRef>(const_cast<void*>(item));
    if (!is_apple_ca_certificate(certificate)) {
      contains_only_ca_certificates = false;
      break;
    }
  }
  CFRelease(items);
  if (!contains_only_ca_certificates) {
    throw std::invalid_argument(
        "TLS CA bundle must contain only PEM CA certificates: " + ca_bundle.string());
  }
}
#else
[[nodiscard]] std::string openssl_pem_error() {
  const auto error = ERR_peek_last_error();
  if (error == 0) return {};
  std::array<char, 256> detail{};
  ERR_error_string_n(error, detail.data(), detail.size());
  return detail.data();
}

void validate_pem_ca_bundle_contents(const std::filesystem::path& ca_bundle) {
  ERR_clear_error();
  BIO* input = BIO_new_file(ca_bundle.string().c_str(), "rb");
  if (input == nullptr) {
    throw std::invalid_argument("TLS CA bundle is unreadable: " + ca_bundle.string());
  }
  STACK_OF(X509_INFO)* entries = PEM_X509_INFO_read_bio(input, nullptr, nullptr, nullptr);
  BIO_free(input);
  const auto parse_error = openssl_pem_error();
  if (entries == nullptr || !parse_error.empty()) {
    if (entries != nullptr) sk_X509_INFO_pop_free(entries, X509_INFO_free);
    throw std::invalid_argument(
        "TLS CA bundle contains invalid PEM certificate data: " + ca_bundle.string() +
        (parse_error.empty() ? std::string{} : ": " + parse_error));
  }

  bool contains_ca_certificate = false;
  bool contains_non_ca_entry = false;
  for (int index = 0; index < sk_X509_INFO_num(entries); ++index) {
    const X509_INFO* entry = sk_X509_INFO_value(entries, index);
    if (entry == nullptr || entry->x509 == nullptr || X509_check_ca(entry->x509) <= 0) {
      contains_non_ca_entry = true;
      break;
    }
    contains_ca_certificate = true;
  }
  sk_X509_INFO_pop_free(entries, X509_INFO_free);
  if (!contains_ca_certificate) {
    throw std::invalid_argument(
        "TLS CA bundle must contain one or more PEM CA certificates: " + ca_bundle.string());
  }
  if (contains_non_ca_entry) {
    throw std::invalid_argument(
        "TLS CA bundle must contain only PEM CA certificates: " + ca_bundle.string());
  }
}
#endif

}  // namespace

std::filesystem::path validate_protected_ca_bundle(std::filesystem::path ca_bundle) {
  if (ca_bundle.empty()) throw std::invalid_argument("TLS CA bundle path is empty");

  std::error_code error;
  const auto link_status = std::filesystem::symlink_status(ca_bundle, error);
  if (error) {
    if (error == std::errc::no_such_file_or_directory) {
      throw std::invalid_argument("TLS CA bundle does not exist: " + ca_bundle.string());
    }
    if (error == std::errc::permission_denied) {
      throw std::invalid_argument("TLS CA bundle is unreadable: " + ca_bundle.string());
    }
    throw std::invalid_argument(
        "TLS CA bundle cannot be inspected: " + ca_bundle.string() + ": " + error.message());
  }
  if (!std::filesystem::exists(link_status)) {
    throw std::invalid_argument("TLS CA bundle does not exist: " + ca_bundle.string());
  }
  if (std::filesystem::is_symlink(link_status)) {
    throw std::invalid_argument("TLS CA bundle must not be a symbolic link: " + ca_bundle.string());
  }
  if (!std::filesystem::is_regular_file(link_status)) {
    throw std::invalid_argument("TLS CA bundle must be a regular file: " + ca_bundle.string());
  }

  constexpr auto kReadPermissions =
      std::filesystem::perms::owner_read |
      std::filesystem::perms::group_read |
      std::filesystem::perms::others_read;
  if (link_status.permissions() != std::filesystem::perms::unknown &&
      (link_status.permissions() & kReadPermissions) == std::filesystem::perms::none) {
    throw std::invalid_argument("TLS CA bundle is unreadable: " + ca_bundle.string());
  }

  const auto size = std::filesystem::file_size(ca_bundle, error);
  if (error) throw std::invalid_argument("TLS CA bundle is unreadable: " + ca_bundle.string());
  if (size == 0) throw std::invalid_argument("TLS CA bundle is empty: " + ca_bundle.string());

  std::ifstream input(ca_bundle, std::ios::binary);
  if (!input) throw std::invalid_argument("TLS CA bundle is unreadable: " + ca_bundle.string());

  const auto canonical = std::filesystem::canonical(ca_bundle, error);
  if (error) {
    throw std::invalid_argument(
        "TLS CA bundle cannot be canonicalized: " + ca_bundle.string() + ": " + error.message());
  }
  validate_pem_ca_bundle_contents(canonical);
  return canonical;
}

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
  CurlTlsTrustPolicy tls_trust_policy;
  std::mutex mutex;

  Impl(std::vector<std::string> entries, CurlTlsTrustPolicy next_tls_trust_policy)
      : tls_trust_policy(std::move(next_tls_trust_policy)) {
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
    : HttpClient(timeout, {}, CurlTlsTrustPolicy::system()) {}

HttpClient::HttpClient(
    std::chrono::milliseconds timeout,
    std::vector<std::string> resolve_entries,
    std::filesystem::path ca_bundle)
    : HttpClient(
          timeout,
          std::move(resolve_entries),
          CurlTlsTrustPolicy::from_optional_ca_bundle(std::move(ca_bundle))) {}

HttpClient::HttpClient(
    std::chrono::milliseconds timeout,
    std::vector<std::string> resolve_entries,
    CurlTlsTrustPolicy tls_trust_policy)
    : timeout_(timeout) {
  if (timeout_.count() <= 0) throw std::invalid_argument("HTTP timeout must be positive");
  ensure_curl_global();
  impl_ = std::make_shared<Impl>(std::move(resolve_entries), std::move(tls_trust_policy));
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
    configure_curl_tls_trust_policy(curl, impl_->tls_trust_policy);
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
    const auto client_send_ms = mine_teleop::utc_now_ms().value;
    const auto client_send_monotonic_ms = mine_teleop::process_monotonic_now_ms().value;
    const auto response = http.get_json(
        origin + "/time?client_send_ms=" + std::to_string(client_send_ms));
    const auto client_receive_ms = mine_teleop::utc_now_ms().value;
    const auto client_receive_monotonic_ms = mine_teleop::process_monotonic_now_ms().value;
    const auto echoed_client_send_ms = response.at("client_send_ms").get<std::int64_t>();
    const auto server_receive_ms = response.at("server_receive_ms").get<std::int64_t>();
    const auto server_send_ms = response.at("server_send_ms").get<std::int64_t>();
    if (echoed_client_send_ms != client_send_ms || server_send_ms < server_receive_ms) {
      throw std::runtime_error("signaling time endpoint returned an invalid four-timestamp sample");
    }
    const auto wall_elapsed_ms = client_receive_ms - client_send_ms;
    const auto monotonic_elapsed_ms =
        client_receive_monotonic_ms - client_send_monotonic_ms;
    // A system-clock step while measuring the request would corrupt both RTT
    // and offset.  Reject that sample rather than treating it as an ordinary
    // long network request; the caller will retain/retry its prior sync state.
    if (std::llabs(wall_elapsed_ms - monotonic_elapsed_ms) > 1000) {
      throw std::runtime_error("local wall clock changed during signaling time synchronization");
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

ClockSample SynchronizedClock::sample() const {
  const auto monotonic = mine_teleop::process_monotonic_now_ms();
  std::lock_guard lock(mutex_);
  if (!status_.synchronized) {
    return {mine_teleop::utc_now_ms(), monotonic};
  }
  return {
      UtcMillis{synchronized_anchor_ms_ +
          std::chrono::duration_cast<std::chrono::milliseconds>(
              std::chrono::steady_clock::now() - steady_anchor_).count()},
      monotonic};
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
