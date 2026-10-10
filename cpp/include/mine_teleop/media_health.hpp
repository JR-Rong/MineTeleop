#pragma once
#include "mine_teleop/core.hpp"
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
namespace mine_teleop {
struct SourceFrameIdentity {
  std::string camera_id;
  std::uint64_t sequence{};
  std::int64_t captured_steady_ms{};
  bool valid{};
  std::uint64_t source_generation{};
  std::string time_quality{"unspecified"};
  std::int64_t read_started_steady_ms{}, read_finished_steady_ms{};
};
// Clock values are local monotonic milliseconds. Output metadata is associated
// with the encoder's actual PTS, rather than with its latest input buffer.
class MediaHealth {
public:
  void reset(const std::vector<std::string> &critical, std::int64_t started);
  bool captured(const SourceFrameIdentity &, std::int64_t now);
  bool composed(std::uint64_t output, const std::vector<SourceFrameIdentity> &,
                std::int64_t now, int max_age, int max_skew);
  bool encoded(std::uint64_t output, const std::vector<SourceFrameIdentity> &,
               std::int64_t now, bool healthy);
  std::string stale(std::int64_t now, int timeout, bool require_frames) const;
  Json snapshot(std::int64_t now) const;

private:
  struct State {
    std::uint64_t captured_seq{}, used_seq{};
    bool seen{}, used{};
    std::int64_t captured{}, consumed{}, encoded{};
    std::uint64_t generation{};
  };
  mutable std::mutex mutex_;
  std::map<std::string, State> states_;
  std::map<std::uint64_t, std::int64_t> outputs_;
  std::int64_t started_{};
};
} // namespace mine_teleop
