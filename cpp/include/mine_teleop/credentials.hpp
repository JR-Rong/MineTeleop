#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace mine_teleop {

// Password-verifier policy is deliberately bounded before Argon2 is invoked:
// a malformed configuration must not turn startup or a login attempt into an
// unbounded memory/CPU allocation. Values are expressed in the Argon2 PHC
// units (KiB, iterations, lanes), not as a control-loop timing budget.
struct AuthenticationCostPolicy {
  std::uint32_t memory_kib{64U * 1024U};
  std::uint32_t time_cost{3};
  std::uint32_t parallelism{1};
  std::uint32_t hash_length{32};
  std::uint32_t minimum_memory_kib{19U * 1024U};
  std::uint32_t maximum_memory_kib{256U * 1024U};
  std::uint32_t minimum_time_cost{1};
  std::uint32_t maximum_time_cost{10};
  std::uint32_t minimum_parallelism{1};
  std::uint32_t maximum_parallelism{4};
  std::size_t maximum_encoded_bytes{1024};
};

// Preserve the public provisioning type name while all verification paths use
// one selected authentication-cost policy.
using Argon2idPolicy = AuthenticationCostPolicy;

[[nodiscard]] const AuthenticationCostPolicy& default_authentication_cost_policy();
[[nodiscard]] const AuthenticationCostPolicy& default_argon2id_policy();

[[nodiscard]] bool validate_authentication_cost_policy(
    const AuthenticationCostPolicy& policy,
    std::string* reason = nullptr);

// Compares equal-length high-entropy secrets with the platform crypto primitive
// (CRYPTO_memcmp on OpenSSL targets). This does not claim that the surrounding
// HTTP/request handling has constant timing; callers must validate expected
// length/format before calling it.
[[nodiscard]] bool constant_time_equal(
    std::string_view expected,
    std::string_view actual) noexcept;

// Parses only the supported Argon2id PHC form and applies the bounded policy
// before the password KDF runs. `reason` is suitable for configuration
// diagnostics and must never be returned to an unauthenticated client.
[[nodiscard]] bool validate_argon2id_verifier(
    std::string_view encoded,
    const AuthenticationCostPolicy& policy,
    std::string* reason = nullptr);

// Returns false for a password mismatch. Invalid configured verifier data is
// a startup error and should have been rejected by validate_argon2id_verifier.
[[nodiscard]] bool verify_argon2id_password(
    std::string_view encoded,
    std::string_view password,
    const AuthenticationCostPolicy& policy);

// Intended for provisioning and deterministic tests. The caller owns the
// plaintext and must clear it after use; this function never logs it.
[[nodiscard]] std::string hash_argon2id_password(
    std::string_view password,
    std::span<const std::uint8_t> salt,
    const AuthenticationCostPolicy& policy = default_authentication_cost_policy());

// A valid fixed verifier used for unknown-account work so that a missing ID
// does not bypass Argon2 altogether. It is not a credential and is never
// emitted to clients or logs.
[[nodiscard]] const std::string& dummy_argon2id_verifier(
    const AuthenticationCostPolicy& policy = default_authentication_cost_policy());

void cleanse_secret(std::string& value) noexcept;

}  // namespace mine_teleop
