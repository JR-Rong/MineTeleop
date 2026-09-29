#pragma once

#include "mine_teleop/http.hpp"
#include <filesystem>
#include <memory>
#include <stop_token>
#include <thread>

namespace mine_teleop {

struct SessionLogSource {
  std::filesystem::path path;
  std::string name;
  bool require_session_id{false};
  bool optional{false};
};

// Reads only named log families (including rotations), never arbitrary paths
// supplied by a remote caller. Output files contain redacted session records.
Json collect_session_logs(const std::vector<SessionLogSource>& sources,
                          const Json& session, const std::filesystem::path& destination,
                          const std::vector<std::string>& secrets = {},
                          std::stop_token stop = {});
Json session_log_chunk(const std::filesystem::path& directory, const Json& manifest,
                       std::string_view name, std::uint64_t offset);
void append_session_log_chunk(const std::filesystem::path& directory, const Json& chunk);
void write_session_zip(const std::filesystem::path& directory,
                       const std::filesystem::path& destination);

class SessionLogBroker {
 public:
  explicit SessionLogBroker(std::filesystem::path audit_path);
  ~SessionLogBroker();
  void remember(const Json& session) noexcept;
  Json sessions(std::string_view driver) const;
  Json session(std::string_view key, std::string_view driver) const;
  Json start(const Json& session, bool vehicle_online);
  Json status(std::string_view id, std::string_view driver, bool finish_partial = false);
  Json chunk(std::string_view id, std::string_view driver, const Json& request);
  void release(std::string_view id, std::string_view driver);
  Json vehicle(std::string_view vehicle, const Json& request, bool idle);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Lives in the vehicle supervisor, after fork. No I/O on CAN/media threads.
std::jthread start_vehicle_log_worker(const VehicleConfig& config, std::string token);

class ControllerLogExport {
 public:
  ControllerLogExport(std::string origin, std::vector<std::string> resolve,
                      std::filesystem::path ca, std::string driver, std::string token,
                      std::string session_key, std::filesystem::path browser_log);
  ~ControllerLogExport();
  Json status() const;
  std::string zip() const;
  void cancel();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mine_teleop
