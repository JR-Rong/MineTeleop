#include "mine_teleop/relay.hpp"
#include "mine_teleop/server.hpp"
#include <cmath>
#include <fstream>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
namespace mine_teleop {
RelayBudget::RelayBudget(std::filesystem::path path, double cap, double copies)
    : directory_(std::move(path)), instance_(random_token(16)), capacity_(cap),
      copies_(copies) {
  if (directory_.empty() || !std::isfinite(cap) || cap <= 0 || cap > 8000000 ||
      !std::isfinite(copies) || copies < 1 || copies > 4)
    throw std::invalid_argument("invalid relay capacity/copy multiplier");
  std::filesystem::create_directories(directory_);
  std::filesystem::permissions(directory_, std::filesystem::perms::owner_all);
  std::ifstream old(directory_ / "ledger.json");
  if (old) {
    Json document;
    old >> document;
    leases_ = document.at("leases");
    for (auto &lease : leases_)
      if (lease.at("state") != "released")
        lease["state"] = "revoking";
  }
  persist();
}
void RelayBudget::persist() {
  const auto file = directory_ / "ledger.json",
             temp = directory_ / "ledger.tmp";
  {
    std::ofstream out(temp, std::ios::trunc);
    out << Json{{"schema", 1},
                {"instance", instance_},
                {"capacity_bps", capacity_},
                {"leases", leases_}}
               .dump();
    out.flush();
    if (!out)
      throw std::runtime_error("relay ledger write failed");
  }
  std::filesystem::permissions(temp, std::filesystem::perms::owner_read |
                                         std::filesystem::perms::owner_write);
#ifndef _WIN32
  int fd = ::open(temp.c_str(), O_RDONLY);
  if (fd < 0 || ::fsync(fd) != 0) {
    if (fd >= 0)
      ::close(fd);
    throw std::runtime_error("relay ledger fsync failed");
  }
  ::close(fd);
#endif
  std::filesystem::rename(temp, file);
#ifndef _WIN32
  int dir = ::open(directory_.c_str(), O_RDONLY | O_DIRECTORY);
  if (dir < 0 || ::fsync(dir) != 0) {
    if (dir >= 0)
      ::close(dir);
    throw std::runtime_error("relay directory fsync failed");
  }
  ::close(dir);
#endif
}
Json RelayBudget::agent(std::int64_t now) const {
  std::ifstream in(directory_ / "status.json");
  if (!in)
    return Json::object();
  Json status;
  try {
    in >> status;
  } catch (...) {
    return Json::object();
  }
  const auto stamp = status.value("at_ms", std::int64_t{0});
  if (!status.value("healthy", false) ||
      status.value("instance", "") != instance_ || stamp > now ||
      now - stamp > 3000)
    return Json::object();
  return status;
}
Json RelayBudget::public_lease(const Json &lease) const {
  auto out = lease;
  out.erase("secret");
  out.erase("usernames");
  return out;
}
Json RelayBudget::reconcile(std::int64_t now) {
  // Caller serializes operations with mutex_. Retain quota through failures.
  const auto status = agent(now);
  bool changed = false;
  for (auto &lease : leases_) {
    const auto state = lease.value("state", "");
    if (state == "released")
      continue;
    if (lease.at("expires_at_ms").get<std::int64_t>() <= now &&
        state != "revoking") {
      lease["state"] = "revoking";
      changed = true;
    }
    const auto report =
        status.value("leases", Json::object())
            .value(lease.at("lease_id").get<std::string>(), Json::object());
    if (lease.at("state") == "revoking" &&
        report.value("secret_deleted", false) &&
        report.value("credential_rejected", false) &&
        report.value("allocations", -1) == 0 &&
        report.value("empty_for_ms", 0) >= 5000) {
      lease["state"] = "released";
      lease.erase("secret");
      changed = true;
    }
    if (lease.at("state") == "confirmed" && report.value("activated", false)) {
      lease["state"] = "active";
      changed = true;
    }
  }
  if (changed)
    persist();
  return status;
}
Json RelayBudget::request(std::string session, std::string attempt,
                          std::string profile, std::int64_t now) {
  std::lock_guard lock(mutex_);
  const auto status = reconcile(now);
  if (attempt.empty() || attempt.size() > 64)
    throw std::invalid_argument("relay media attempt required");
  double video = 0;
  if (profile == "two-720p")
    video = 4000000;
  else if (profile == "two-540p")
    video = 2200000;
  else if (profile == "full")
    video = 9000000;
  else
    throw std::invalid_argument("unknown relay profile");
  const auto requested = video * 1.25 * copies_;
  double used = 0;
  for (const auto &lease : leases_) {
    if (lease.at("session_id") == session &&
        lease.at("media_attempt_id") == attempt) {
      if (lease.at("state") == "revoking" || lease.at("state") == "released")
        return {{"approved", false}, {"reason", "relay_attempt_revoked"}};
      return public_lease(lease);
    }
    if (lease.at("state") != "released")
      used += lease.at("egress_bps").get<double>();
  }
  if (leases_.size() >= 4096)
    return {{"approved", false},
            {"reason", "relay_ledger_maintenance_required"}};
  if (status.empty() || used + requested > capacity_)
    return {{"approved", false},
            {"reason", status.empty() ? "relay_manager_unavailable"
                                      : "relay_capacity_exhausted"}};
  const auto id = random_token(16);
  Json lease = {{"approved", true},
                {"lease_id", id},
                {"session_id", session},
                {"media_attempt_id", attempt},
                {"policy_version", 1},
                {"profile", profile},
                {"video_bps", video},
                {"peak_video_bps", video},
                {"egress_bps", requested},
                {"state", "reserved"},
                {"expires_at_ms", now + 15000},
                {"secret", random_token(32)},
                {"usernames", Json::object()}};
  leases_[id] = lease;
  persist();
  return public_lease(lease);
}
Json RelayBudget::confirm(std::string session, const Json &request,
                          std::int64_t now) {
  std::lock_guard lock(mutex_);
  reconcile(now);
  auto &lease = leases_.at(request.at("lease_id").get<std::string>());
  if (lease.at("session_id") != session ||
      lease.at("media_attempt_id") != request.at("media_attempt_id") ||
      lease.at("policy_version") != request.at("policy_version") ||
      lease.at("state") != "reserved" ||
      lease.at("expires_at_ms").get<std::int64_t>() <= now ||
      !request.value("healthy_encoded_frames", false) ||
      request.at("applied_video_bps") != lease.at("video_bps"))
    throw std::invalid_argument("relay encoder policy confirmation mismatch");
  lease["state"] = "confirmed";
  persist();
  return public_lease(lease);
}
Json RelayBudget::renew(std::string session, const Json &request,
                        std::int64_t now) {
  std::lock_guard lock(mutex_);
  const auto status = reconcile(now);
  auto &lease = leases_.at(request.at("lease_id").get<std::string>());
  if (lease.at("session_id") != session ||
      lease.at("media_attempt_id") != request.at("media_attempt_id") ||
      status.empty() || lease.at("state") == "revoking" ||
      lease.at("state") == "released")
    throw std::invalid_argument("relay lease cannot renew");
  lease["expires_at_ms"] = now + 15000;
  persist();
  auto out = public_lease(lease);
  out["usage"] =
      status.value("leases", Json::object())
          .value(lease.at("lease_id").get<std::string>(), Json::object());
  return out;
}
void RelayBudget::revoke(std::string session, std::string attempt,
                         std::int64_t now) {
  std::lock_guard lock(mutex_);
  reconcile(now);
  for (auto &lease : leases_)
    if (lease.at("session_id") == session &&
        (attempt.empty() || lease.at("media_attempt_id") == attempt) &&
        lease.at("state") != "released")
      lease["state"] = "revoking";
  persist();
}
void RelayBudget::record_username(std::string session, std::string attempt,
                                  std::string actor, std::string username,
                                  std::int64_t now) {
  std::lock_guard lock(mutex_);
  reconcile(now);
  for (auto &lease : leases_)
    if (lease.at("session_id") == session &&
        lease.at("media_attempt_id") == attempt &&
        lease.at("state") == "active" &&
        lease.at("expires_at_ms").get<std::int64_t>() > now) {
      lease["usernames"][actor] = username;
      persist();
      return;
    }
  throw std::invalid_argument("relay credentials revoked before issuance");
}
Json RelayBudget::credentials(std::string session, std::string attempt,
                              std::int64_t now) {
  std::lock_guard lock(mutex_);
  const auto status = reconcile(now);
  if (status.empty())
    return Json::object();
  for (auto &lease : leases_)
    if (lease.at("session_id") == session &&
        lease.at("media_attempt_id") == attempt &&
        lease.at("state") == "active" &&
        lease.at("expires_at_ms").get<std::int64_t>() > now)
      return lease;
  return Json::object();
}
} // namespace mine_teleop
