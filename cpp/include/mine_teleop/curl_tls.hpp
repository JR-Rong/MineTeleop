#pragma once

#include <curl/curl.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#if defined(_WIN32) && LIBCURL_VERSION_NUM < 0x081100
#error "MineTeleop Windows WSS requires libcurl 8.17.0 or newer"
#endif

namespace mine_teleop {

// The default deliberately uses the TLS backend's system trust store.  It is
// distinct from the legacy environment mode so a vehicle process cannot have
// its trust roots silently redirected by a service-manager environment.
enum class CurlTlsTrustMode {
  SystemTrust,
  ProtectedCaBundle,
  LegacyEnvironment,
};

// Validates filesystem protections and parses every PEM certificate before a
// private trust bundle is accepted.  Its implementation stays in the runtime
// library so this public policy header does not add a new crypto dependency to
// every consumer.
[[nodiscard]] std::filesystem::path validate_protected_ca_bundle(std::filesystem::path ca_bundle);

class CurlTlsTrustPolicy {
 public:
  [[nodiscard]] static CurlTlsTrustPolicy system() {
    return CurlTlsTrustPolicy(CurlTlsTrustMode::SystemTrust, {});
  }

  [[nodiscard]] static CurlTlsTrustPolicy protected_ca_bundle(std::filesystem::path ca_bundle) {
    return CurlTlsTrustPolicy(
        CurlTlsTrustMode::ProtectedCaBundle,
        validate_protected_ca_bundle(std::move(ca_bundle)));
  }

  // This preserves the historical CURL_CA_BUNDLE/SSL_CERT_FILE behavior only
  // for callers that opt into it deliberately.
  [[nodiscard]] static CurlTlsTrustPolicy legacy_environment() {
    return CurlTlsTrustPolicy(CurlTlsTrustMode::LegacyEnvironment, {});
  }

  // Compatibility bridge for the original HttpClient/WebSocketClient
  // constructors: an empty path used system trust, while a non-empty path was
  // an app-supplied CA bundle.
  [[nodiscard]] static CurlTlsTrustPolicy from_optional_ca_bundle(std::filesystem::path ca_bundle) {
    if (ca_bundle.empty()) return system();
    return protected_ca_bundle(std::move(ca_bundle));
  }

  [[nodiscard]] CurlTlsTrustMode mode() const noexcept { return mode_; }
  [[nodiscard]] const std::filesystem::path& ca_bundle() const noexcept { return ca_bundle_; }

 private:
  CurlTlsTrustPolicy(CurlTlsTrustMode mode, std::filesystem::path ca_bundle)
      : mode_(mode), ca_bundle_(std::move(ca_bundle)) {}

  CurlTlsTrustMode mode_;
  std::filesystem::path ca_bundle_;
};

struct CurlTlsTrustConfiguration {
  CurlTlsTrustMode mode{CurlTlsTrustMode::SystemTrust};
  std::optional<std::filesystem::path> ca_bundle;
  bool verify_peer{true};
  bool verify_hostname{true};
};

namespace detail {

class CurlCaInfoPath {
 public:
  explicit CurlCaInfoPath(const std::filesystem::path& ca_bundle)
#if defined(_WIN32)
      : storage_(copy_utf8_bytes(ca_bundle.u8string())) {}
#else
      : storage_(ca_bundle.string()) {}
#endif

  explicit CurlCaInfoPath(std::u8string_view utf8_path) : storage_(copy_utf8_bytes(utf8_path)) {}

  [[nodiscard]] const char* c_str() const noexcept { return storage_.c_str(); }
  [[nodiscard]] const std::string& string() const noexcept { return storage_; }

 private:
  [[nodiscard]] static std::string copy_utf8_bytes(std::u8string_view utf8_path) {
    std::string value(utf8_path.size(), '\0');
    if (!utf8_path.empty()) {
      std::memcpy(value.data(), utf8_path.data(), utf8_path.size());
    }
    return value;
  }

  std::string storage_;
};

}  // namespace detail

[[nodiscard]] inline CurlTlsTrustConfiguration resolve_curl_tls_trust_policy(
    const CurlTlsTrustPolicy& policy,
    const char* legacy_curl_ca_bundle = nullptr,
    const char* legacy_ssl_cert_file = nullptr) {
  CurlTlsTrustConfiguration configuration;
  configuration.mode = policy.mode();
  switch (policy.mode()) {
    case CurlTlsTrustMode::SystemTrust:
      // Do not examine the environment in system-trust mode.
      return configuration;
    case CurlTlsTrustMode::ProtectedCaBundle:
      configuration.ca_bundle = validate_protected_ca_bundle(policy.ca_bundle());
      return configuration;
    case CurlTlsTrustMode::LegacyEnvironment:
      if (legacy_curl_ca_bundle != nullptr && *legacy_curl_ca_bundle != '\0') {
        configuration.ca_bundle = validate_protected_ca_bundle(legacy_curl_ca_bundle);
      } else if (legacy_ssl_cert_file != nullptr && *legacy_ssl_cert_file != '\0') {
        configuration.ca_bundle = validate_protected_ca_bundle(legacy_ssl_cert_file);
      }
      return configuration;
  }
  throw std::logic_error("unknown curl TLS trust mode");
}

inline void configure_curl_custom_ca(CURL* curl, const detail::CurlCaInfoPath& ca_bundle) {
  curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle.c_str());
#if defined(_WIN32) && LIBCURL_VERSION_NUM >= 0x074600
  // Private PKI certificates may intentionally omit public CRL/OCSP endpoints.
  // Schannel still validates the CA chain and hostname, but accepts an unknown
  // revocation status when no distribution point is available.
  curl_easy_setopt(
      curl,
      CURLOPT_SSL_OPTIONS,
      static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));
#endif
}

inline void configure_curl_tls_trust_policy(CURL* curl, const CurlTlsTrustPolicy& policy) {
  if (curl == nullptr) throw std::invalid_argument("curl TLS policy requires a handle");

  const bool allow_legacy_environment = policy.mode() == CurlTlsTrustMode::LegacyEnvironment;
  const auto configuration = resolve_curl_tls_trust_policy(
      policy,
      allow_legacy_environment ? std::getenv("CURL_CA_BUNDLE") : nullptr,
      allow_legacy_environment ? std::getenv("SSL_CERT_FILE") : nullptr);
  if (!configuration.verify_peer || !configuration.verify_hostname) {
    throw std::logic_error("TLS policy cannot disable peer or hostname verification");
  }

  // These are deliberate every-request settings, rather than backend defaults:
  // HTTPS/WSS must never continue after a certificate or hostname failure.
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  if (configuration.ca_bundle.has_value()) {
    const detail::CurlCaInfoPath ca_bundle(*configuration.ca_bundle);
    configure_curl_custom_ca(curl, ca_bundle);
  }
  // Do not set CURLOPT_CAINFO when no explicit bundle was selected.  Leaving
  // libcurl's option untouched preserves its compiled-in default CA location
  // on Unix-like backends and the Schannel system-root store on Windows.
}

}  // namespace mine_teleop
