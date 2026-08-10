#pragma once

#include <curl/curl.h>

namespace mine_teleop {

inline void configure_curl_custom_ca(CURL* curl, const char* ca_bundle) {
  curl_easy_setopt(curl, CURLOPT_CAINFO, ca_bundle);
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

}  // namespace mine_teleop
