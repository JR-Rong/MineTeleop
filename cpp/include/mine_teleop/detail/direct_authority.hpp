#pragma once

#include "mine_teleop/detail/control_transport.hpp"
#include "mine_teleop/http.hpp"

#include <algorithm>
#include <limits>

namespace mine_teleop::detail {
inline constexpr std::string_view kDirectAuthorityCapability = "native-direct-authority-v1";
inline bool transient_cloud_failure(const std::exception& error) {
  if (dynamic_cast<const HttpTransportError*>(&error)) return true;
  if (const auto* response = dynamic_cast<const HttpStatusError*>(&error))
    return response->status() == 408 || response->status() == 429 || response->status() >= 500;
  return false; // Authentication/session denials and malformed protocols stop.
}

// Public, non-secret grant. The vehicle accepts it ONLY from the authenticated
// cloud RPC response bound to its own pending nonce, never from a DC message.
struct DirectAuthorityGrant {
  std::string issuer, vehicle, driver, session, nonce;
  std::uint64_t connection_generation{}, epoch{}, lease_seq{};
  std::int64_t issued_utc_ms{}, expires_utc_ms{}, duration_ms{};
  static DirectAuthorityGrant parse(const Json& value) {
    if (value.value("protocol", "") != kDirectAuthorityCapability) throw std::invalid_argument("unsupported direct authority");
    DirectAuthorityGrant grant;
    for (auto field : {"issuer", "vehicle_id", "driver_id", "session_id", "nonce"})
      if (!value.at(field).is_string() || value.at(field).get_ref<const std::string&>().empty() || value.at(field).get_ref<const std::string&>().size() > 128)
        throw std::invalid_argument("invalid direct authority identity");
    grant.issuer = value.at("issuer"); grant.vehicle = value.at("vehicle_id"); grant.driver = value.at("driver_id");
    grant.session = value.at("session_id"); grant.nonce = value.at("nonce");
    grant.connection_generation = ControlTransportStamp::uint(value, "connection_generation");
    grant.epoch = ControlTransportStamp::uint(value, "authority_epoch"); grant.lease_seq = ControlTransportStamp::uint(value, "lease_seq");
    for (auto field : {"issued_at_utc_ms", "expires_at_utc_ms", "duration_ms"})
      if (!value.at(field).is_number_integer() || (value.at(field).is_number_unsigned() &&
          value.at(field).get<std::uint64_t>() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())))
        throw std::invalid_argument("direct authority times must be bounded integers");
    grant.issued_utc_ms = value.at("issued_at_utc_ms").get<std::int64_t>();
    grant.expires_utc_ms = value.at("expires_at_utc_ms").get<std::int64_t>();
    grant.duration_ms = value.at("duration_ms").get<std::int64_t>();
    if (!grant.connection_generation || !grant.epoch || !grant.lease_seq || grant.issued_utc_ms < 0 || grant.duration_ms <= 0 ||
        grant.issued_utc_ms > std::numeric_limits<std::int64_t>::max() - grant.duration_ms ||
        grant.expires_utc_ms != grant.issued_utc_ms + grant.duration_ms)
      throw std::invalid_argument("invalid direct authority lifetime");
    return grant;
  }
  Json json() const {
    return {{"protocol", kDirectAuthorityCapability}, {"issuer", issuer}, {"vehicle_id", vehicle}, {"driver_id", driver},
        {"session_id", session}, {"nonce", nonce}, {"connection_generation", connection_generation},
        {"authority_epoch", epoch}, {"lease_seq", lease_seq}, {"issued_at_utc_ms", issued_utc_ms},
        {"expires_at_utc_ms", expires_utc_ms}, {"duration_ms", duration_ms}};
  }
};

class DirectAuthorityLease {
 public:
  void configure(std::string issuer, std::string vehicle, std::string driver, std::string session,
      std::uint64_t connection_generation, std::int64_t maximum_duration_ms) {
    *this = {}; issuer_ = std::move(issuer); vehicle_ = std::move(vehicle); driver_ = std::move(driver);
    session_ = std::move(session); connection_generation_ = connection_generation; maximum_duration_ms_ = maximum_duration_ms;
  }
  bool configured() const { return maximum_duration_ms_ > 0 && !issuer_.empty() && !session_.empty() && connection_generation_ > 0; }
  void begin_request(std::string nonce, std::int64_t started) {
    if (!configured() || revoked_ || nonce.empty() || nonce.size() > 128 || started < 0 || started < last_observed_ms_)
      throw std::invalid_argument("direct authority request is invalid");
    pending_nonce_ = std::move(nonce); pending_started_ms_ = started; last_observed_ms_ = started;
  }
  // now_utc + uncertainty is a conservative upper bound on cloud time. The
  // request-start cap prevents a delayed response granting a fresh whole TTL.
  bool accept_authenticated_cloud(const Json& input, std::int64_t now, std::int64_t now_utc, std::int64_t uncertainty, bool require_new_epoch_after_expiry = true) {
    const auto grant = DirectAuthorityGrant::parse(input);
    if (!configured() || revoked_ || pending_nonce_.empty() || grant.nonce != pending_nonce_ ||
        grant.issuer != issuer_ || grant.vehicle != vehicle_ || grant.driver != driver_ || grant.session != session_ ||
        grant.connection_generation != connection_generation_ || grant.duration_ms > maximum_duration_ms_ ||
        uncertainty < 0 || now < last_observed_ms_ || now_utc < 0 ||
        now_utc > std::numeric_limits<std::int64_t>::max() - uncertainty) return false;
    if (grant_ && (grant.epoch < grant_->epoch || (grant.epoch == grant_->epoch && grant.lease_seq <= grant_->lease_seq))) return false;
    if (require_new_epoch_after_expiry && grant_ && now >= deadline_ms_ && grant.epoch <= grant_->epoch) return false;
    const auto remaining = grant.expires_utc_ms - (now_utc + uncertainty);
    if (remaining <= 0 || pending_started_ms_ > std::numeric_limits<std::int64_t>::max() - grant.duration_ms ||
        now > std::numeric_limits<std::int64_t>::max() - remaining) return false;
    const auto deadline = std::min(pending_started_ms_ + grant.duration_ms, now + remaining);
    if (deadline <= now) return false;
    const bool boundary = !grant_ || grant.epoch != grant_->epoch || expired_observed_ || now >= deadline_ms_;
    if (boundary) ++rearm_revision_;
    grant_ = grant; deadline_ms_ = deadline; renew_at_ms_ = pending_started_ms_ + grant.duration_ms / 3;
    pending_nonce_.clear(); last_observed_ms_ = now; expired_observed_ = false; return true;
  }
  // Native observes a vehicle-authenticated accepted grant, never a raw page
  // or cloud-ingress ACK. It cannot grant the vehicle any authority itself.
  bool observe_vehicle_accepted(const Json& input, std::int64_t now, std::int64_t now_utc, std::int64_t uncertainty) {
    const auto incoming = DirectAuthorityGrant::parse(input);
    if (grant_ && incoming.epoch == grant_->epoch && incoming.lease_seq == grant_->lease_seq) return false;
    pending_nonce_ = incoming.nonce; pending_started_ms_ = now;
    return accept_authenticated_cloud(input, now, now_utc, uncertainty, false);
  }
  bool valid(std::int64_t now) const { return configured() && grant_ && !revoked_ && now >= last_observed_ms_ && now < deadline_ms_; }
  bool observe_expiry(std::int64_t now) {
    if (!configured() || !grant_ || revoked_ || expired_observed_ || (now >= last_observed_ms_ && now < deadline_ms_)) return false;
    expired_observed_ = true; ++rearm_revision_; return true;
  }
  bool allows(const Json& reference, std::int64_t now) const {
    return valid(now) && reference.is_object() && reference.value("issuer", "") == issuer_ &&
        reference.value("authority_epoch", std::uint64_t{0}) == grant_->epoch &&
        reference.value("lease_seq", std::uint64_t{0}) > 0 && reference.value("lease_seq", std::uint64_t{0}) <= grant_->lease_seq;
  }
  Json reference() const { return grant_ ? Json{{"issuer", issuer_}, {"authority_epoch", grant_->epoch}, {"lease_seq", grant_->lease_seq}} : Json(nullptr); }
  Json status(std::int64_t now) const {
    return {{"protocol", kDirectAuthorityCapability}, {"configured", configured()}, {"valid", valid(now)},
        {"revoked", revoked_}, {"rearm_revision", rearm_revision_}, {"grant", grant_ ? grant_->json() : Json(nullptr)},
        {"remaining_ms", valid(now) ? deadline_ms_ - now : 0}};
  }
  void revoke() { if (!revoked_) { revoked_ = true; ++rearm_revision_; } }
  std::uint64_t rearm_revision() const { return rearm_revision_; }
  bool renewal_due(std::int64_t now) const { return configured() && !revoked_ && (!grant_ || now >= renew_at_ms_); }
 private:
  std::string issuer_, vehicle_, driver_, session_, pending_nonce_;
  std::uint64_t connection_generation_{}, rearm_revision_{};
  std::int64_t maximum_duration_ms_{}, pending_started_ms_{}, deadline_ms_{}, renew_at_ms_{}, last_observed_ms_{};
  bool revoked_{}, expired_observed_{};
  std::optional<DirectAuthorityGrant> grant_;
};
}  // namespace mine_teleop::detail
