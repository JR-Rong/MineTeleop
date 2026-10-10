#pragma once
#include "mine_teleop/core.hpp"
#include <filesystem>
#include <mutex>
namespace mine_teleop {
// The server owns ledger.json; the privileged local reaper owns status.json.
// Client requests can revoke a lease but can never attest to its reclamation.
class RelayBudget {
public:
  explicit RelayBudget(std::filesystem::path directory,
                       double capacity_bps = 8000000, double copies = 2);
  Json request(std::string session, std::string attempt, std::string profile,
               std::int64_t now);
  Json confirm(std::string session, const Json &, std::int64_t now);
  Json renew(std::string session, const Json &, std::int64_t now);
  void revoke(std::string session, std::string attempt, std::int64_t now);
  void record_username(std::string session, std::string attempt,
                       std::string actor, std::string username,
                       std::int64_t now);
  Json credentials(std::string session, std::string attempt, std::int64_t now);

private:
  void persist();
  Json reconcile(std::int64_t now);
  Json agent(std::int64_t now) const;
  Json public_lease(const Json &) const;
  std::filesystem::path directory_;
  std::string instance_;
  Json leases_ = Json::object();
  double capacity_, copies_;
  std::mutex mutex_;
};
} // namespace mine_teleop
