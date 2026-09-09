#include "mine_teleop/credentials.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <tuple>
#include <vector>

#if MINE_TELEOP_HAS_OPENSSL_CRYPTO
#include <openssl/crypto.h>
#endif

#if MINE_TELEOP_HAS_ARGON2ID
#include <argon2.h>
#endif

namespace mine_teleop {
namespace {

void set_reason(std::string* reason, std::string value) {
  if (reason != nullptr) *reason = std::move(value);
}

[[nodiscard]] bool decimal_parameter(
    std::string_view text,
    std::string_view name,
    std::uint32_t* value) {
  if (!text.starts_with(name) || text.size() <= name.size() ||
      text[name.size()] != '=') {
    return false;
  }
  const auto digits = text.substr(name.size() + 1);
  if (std::any_of(digits.begin(), digits.end(), [](unsigned char character) {
        return character < '0' || character > '9';
      })) {
    return false;
  }
  std::uint32_t parsed = 0;
  const auto [end, error] = std::from_chars(
      digits.data(), digits.data() + digits.size(), parsed);
  if (error != std::errc{} || end != digits.data() + digits.size()) return false;
  *value = parsed;
  return true;
}

[[nodiscard]] bool phc_base64_component(std::string_view value) {
  return !value.empty() && std::all_of(
             value.begin(), value.end(), [](unsigned char character) {
               return (character >= 'A' && character <= 'Z') ||
                   (character >= 'a' && character <= 'z') ||
                   (character >= '0' && character <= '9') || character == '+' ||
                   character == '/';
             });
}

[[nodiscard]] bool supports_argon2id() {
#if MINE_TELEOP_HAS_ARGON2ID
  return true;
#else
  return false;
#endif
}

void require_argon2id() {
  if (!supports_argon2id()) {
    throw std::runtime_error(
        "Argon2id support is not compiled into this target; install the packaged Argon2 development dependency");
  }
}

[[nodiscard]] auto policy_key(const AuthenticationCostPolicy& policy) {
  return std::tie(
      policy.memory_kib,
      policy.time_cost,
      policy.parallelism,
      policy.hash_length,
      policy.minimum_memory_kib,
      policy.maximum_memory_kib,
      policy.minimum_time_cost,
      policy.maximum_time_cost,
      policy.minimum_parallelism,
      policy.maximum_parallelism,
      policy.maximum_encoded_bytes);
}

struct AuthenticationCostPolicyLess {
  [[nodiscard]] bool operator()(
      const AuthenticationCostPolicy& left,
      const AuthenticationCostPolicy& right) const {
    return policy_key(left) < policy_key(right);
  }
};

}  // namespace

const AuthenticationCostPolicy& default_authentication_cost_policy() {
  static constexpr AuthenticationCostPolicy policy{};
  return policy;
}

const AuthenticationCostPolicy& default_argon2id_policy() {
  return default_authentication_cost_policy();
}

bool validate_authentication_cost_policy(
    const AuthenticationCostPolicy& policy,
    std::string* reason) {
  if (policy.minimum_memory_kib == 0 || policy.maximum_memory_kib == 0 ||
      policy.minimum_time_cost == 0 || policy.maximum_time_cost == 0 ||
      policy.minimum_parallelism == 0 || policy.maximum_parallelism == 0) {
    set_reason(reason, "authentication cost policy bounds must be positive");
    return false;
  }
  if (policy.minimum_memory_kib > policy.maximum_memory_kib ||
      policy.minimum_time_cost > policy.maximum_time_cost ||
      policy.minimum_parallelism > policy.maximum_parallelism) {
    set_reason(reason, "authentication cost policy minimum exceeds maximum");
    return false;
  }
  if (policy.memory_kib < policy.minimum_memory_kib ||
      policy.memory_kib > policy.maximum_memory_kib ||
      policy.time_cost < policy.minimum_time_cost ||
      policy.time_cost > policy.maximum_time_cost ||
      policy.parallelism < policy.minimum_parallelism ||
      policy.parallelism > policy.maximum_parallelism) {
    set_reason(reason, "authentication cost policy selected cost is outside its bounds");
    return false;
  }
  if (policy.hash_length < 16 || policy.hash_length > 128) {
    set_reason(reason, "authentication cost policy hash length must be between 16 and 128 bytes");
    return false;
  }
  if (policy.maximum_encoded_bytes < 96 || policy.maximum_encoded_bytes > 4096) {
    set_reason(reason, "authentication cost policy encoded PHC budget must be between 96 and 4096 bytes");
    return false;
  }
#if MINE_TELEOP_HAS_ARGON2ID
  const auto encoded_length = argon2_encodedlen(
      policy.time_cost,
      policy.memory_kib,
      policy.parallelism,
      16,
      policy.hash_length,
      Argon2_id);
  if (encoded_length == 0 || encoded_length > policy.maximum_encoded_bytes + 1) {
    set_reason(reason, "authentication cost policy cannot encode its selected Argon2id verifier");
    return false;
  }
#endif
  return true;
}

bool constant_time_equal(std::string_view expected, std::string_view actual) noexcept {
  if (expected.size() != actual.size()) return false;
  if (expected.empty()) return true;
#if MINE_TELEOP_HAS_OPENSSL_CRYPTO
  return CRYPTO_memcmp(expected.data(), actual.data(), expected.size()) == 0;
#else
  // The Apple runtime uses Security/CommonCrypto rather than OpenSSL. Keep a
  // non-short-circuiting, volatile accumulator for the same equal-length
  // contract without adding a second crypto runtime to control-only packages.
  volatile unsigned char difference = 0;
  for (std::size_t index = 0; index < expected.size(); ++index) {
    difference |= static_cast<unsigned char>(expected[index]) ^
        static_cast<unsigned char>(actual[index]);
  }
  return difference == 0;
#endif
}

bool validate_argon2id_verifier(
    std::string_view encoded,
    const AuthenticationCostPolicy& policy,
    std::string* reason) {
  if (!validate_authentication_cost_policy(policy, reason)) return false;
  if (encoded.empty() || encoded.size() > policy.maximum_encoded_bytes) {
    set_reason(reason, "encoded verifier length is outside policy");
    return false;
  }
  std::vector<std::string_view> fields;
  std::size_t begin = 0;
  while (begin <= encoded.size()) {
    const auto separator = encoded.find('$', begin);
    fields.push_back(encoded.substr(
        begin,
        separator == std::string_view::npos ? std::string_view::npos : separator - begin));
    if (separator == std::string_view::npos) break;
    begin = separator + 1;
  }
  if (fields.size() != 6 || fields[0] != "" || fields[1] != "argon2id" ||
      fields[2] != "v=19") {
    set_reason(reason, "verifier must use the supported $argon2id$v=19 PHC form");
    return false;
  }
  const auto params = fields[3];
  const auto first = params.find(',');
  const auto second = first == std::string_view::npos
      ? std::string_view::npos
      : params.find(',', first + 1);
  if (first == std::string_view::npos || second == std::string_view::npos ||
      params.find(',', second + 1) != std::string_view::npos) {
    set_reason(reason, "Argon2id verifier parameters must be m,t,p exactly once");
    return false;
  }
  std::uint32_t memory_kib = 0;
  std::uint32_t time_cost = 0;
  std::uint32_t parallelism = 0;
  if (!decimal_parameter(params.substr(0, first), "m", &memory_kib) ||
      !decimal_parameter(params.substr(first + 1, second - first - 1), "t", &time_cost) ||
      !decimal_parameter(params.substr(second + 1), "p", &parallelism)) {
    set_reason(reason, "Argon2id verifier parameters are malformed");
    return false;
  }
  if (memory_kib < policy.minimum_memory_kib || memory_kib > policy.maximum_memory_kib ||
      time_cost < policy.minimum_time_cost || time_cost > policy.maximum_time_cost ||
      parallelism < policy.minimum_parallelism || parallelism > policy.maximum_parallelism) {
    set_reason(reason, "Argon2id verifier cost is outside configured policy");
    return false;
  }
  if (fields[4].size() < 11 || fields[5].size() < 22 ||
      !phc_base64_component(fields[4]) || !phc_base64_component(fields[5])) {
    set_reason(reason, "Argon2id verifier salt or digest is malformed");
    return false;
  }
  return true;
}

std::string hash_argon2id_password(
    std::string_view password,
    std::span<const std::uint8_t> salt,
    const AuthenticationCostPolicy& policy) {
  require_argon2id();
  if (salt.size() < 8 || salt.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("Argon2id salt length is invalid");
  }
  if (password.size() > std::numeric_limits<std::uint32_t>::max()) {
    throw std::invalid_argument("Argon2id password length is invalid");
  }
  std::string policy_reason;
  if (!validate_authentication_cost_policy(policy, &policy_reason)) {
    throw std::invalid_argument("Argon2id hash policy is invalid: " + policy_reason);
  }
#if MINE_TELEOP_HAS_ARGON2ID
  const auto encoded_length = argon2_encodedlen(
      policy.time_cost,
      policy.memory_kib,
      policy.parallelism,
      salt.size(),
      policy.hash_length,
      Argon2_id);
  if (encoded_length == 0 || encoded_length > policy.maximum_encoded_bytes + 1) {
    throw std::invalid_argument("Argon2id encoded verifier length is outside policy");
  }
  std::string encoded(encoded_length, '\0');
  const int result = argon2id_hash_encoded(
      policy.time_cost,
      policy.memory_kib,
      policy.parallelism,
      password.data(),
      password.size(),
      salt.data(),
      salt.size(),
      policy.hash_length,
      encoded.data(),
      encoded.size());
  if (result != ARGON2_OK) {
    throw std::runtime_error(
        std::string("Argon2id password hashing failed: ") + argon2_error_message(result));
  }
  encoded.resize(std::char_traits<char>::length(encoded.c_str()));
  std::string reason;
  if (!validate_argon2id_verifier(encoded, policy, &reason)) {
    throw std::runtime_error("Argon2id library produced a verifier outside policy: " + reason);
  }
  return encoded;
#else
  static_cast<void>(password);
  static_cast<void>(salt);
  static_cast<void>(policy);
  throw std::logic_error("unreachable Argon2id-disabled hash path");
#endif
}

bool verify_argon2id_password(
    std::string_view encoded,
    std::string_view password,
    const AuthenticationCostPolicy& policy) {
  require_argon2id();
  std::string reason;
  if (!validate_argon2id_verifier(encoded, policy, &reason)) {
    throw std::invalid_argument("Argon2id verifier is invalid: " + reason);
  }
#if MINE_TELEOP_HAS_ARGON2ID
  // The C library consumes a NUL-terminated PHC string. Copy the public
  // string_view so callers cannot make it read past a non-terminated slice.
  const std::string encoded_copy(encoded);
  const int result =
      argon2id_verify(encoded_copy.c_str(), password.data(), password.size());
  if (result == ARGON2_OK) return true;
  if (result == ARGON2_VERIFY_MISMATCH) return false;
  throw std::runtime_error(
      std::string("Argon2id password verification failed: ") + argon2_error_message(result));
#else
  static_cast<void>(encoded);
  static_cast<void>(password);
  static_cast<void>(policy);
  throw std::logic_error("unreachable Argon2id-disabled verify path");
#endif
}

const std::string& dummy_argon2id_verifier(const AuthenticationCostPolicy& policy) {
  std::string reason;
  if (!validate_authentication_cost_policy(policy, &reason)) {
    throw std::invalid_argument("dummy Argon2id verifier policy is invalid: " + reason);
  }
  static std::mutex mutex;
  static std::map<AuthenticationCostPolicy, std::string, AuthenticationCostPolicyLess> cache;
  {
    std::lock_guard lock(mutex);
    if (const auto found = cache.find(policy); found != cache.end()) return found->second;
  }
  constexpr std::array<std::uint8_t, 16> salt{
      0x6d, 0x69, 0x6e, 0x65, 0x2d, 0x74, 0x65, 0x6c,
      0x65, 0x6f, 0x70, 0x2d, 0x64, 0x75, 0x6d, 0x6d};
  auto verifier = hash_argon2id_password(
      "mine-teleop-unknown-driver-verifier",
      salt,
      policy);
  std::lock_guard lock(mutex);
  const auto [inserted, unused] = cache.emplace(policy, std::move(verifier));
  static_cast<void>(unused);
  return inserted->second;
}

void cleanse_secret(std::string& value) noexcept {
  if (!value.empty()) {
#if MINE_TELEOP_HAS_OPENSSL_CRYPTO
    OPENSSL_cleanse(value.data(), value.size());
#else
    volatile char* bytes = value.data();
    for (std::size_t index = 0; index < value.size(); ++index) bytes[index] = '\0';
#endif
  }
  value.clear();
}

}  // namespace mine_teleop
