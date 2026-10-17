#pragma once
#include "common.hpp"

#include <cmath>
#include <cstddef>
#include <limits>

namespace mine_teleop::path_selection_models {
struct StopEvent {
  Authority authority;
  std::uint64_t reset_epoch{}, command_seq{}, intent_seq{};
  Digest digest{};
  bool stop_only{}, fresh{};
  Preconditions prerequisites;
};
struct StopRoute {
  Route route;
  bool available{}, authorized{}, healthy{};
  std::optional<double> actual_stop_result_ms;
};
struct StopDispatch {
  std::array<Route, 2> routes{};
  std::size_t count{};
};
// Returns a bounded plan only. A real dispatcher must enqueue independently;
// calling blocking sends in this order would not prove parallel delivery.
inline StopDispatch stop_dispatch_plan(const StopEvent& event, const Authority& authority,
    std::uint64_t reset_epoch, const std::array<StopRoute, 2>& routes) {
  StopDispatch result;
  if (!authority.valid() || event.authority != authority || event.reset_epoch != reset_epoch ||
      !event.command_seq || !event.intent_seq || !event.stop_only || !event.fresh ||
      !event.prerequisites.satisfied() || routes[0].route == routes[1].route) return result;
  const auto score = [](const StopRoute& route) {
    return route.healthy && route.actual_stop_result_ms && std::isfinite(*route.actual_stop_result_ms) &&
        *route.actual_stop_result_ms >= 0 ? *route.actual_stop_result_ms : std::numeric_limits<double>::infinity();
  };
  const auto first = score(routes[1]) < score(routes[0]) ? 1U : 0U;
  for (const auto index : {first, 1U - first})
    if (routes[index].available && routes[index].authorized) result.routes[result.count++] = routes[index].route;
  return result; // An unmeasured available rescue route is not withheld.
}

template<std::size_t Capacity> class StopLedger {
  static_assert(Capacity > 0);
 public:
  enum class Verdict { ApplyOnce, DuplicatePending, DuplicateCached, Conflict, Replay, Rejected, Full };
  struct Result { bool software_latched; std::uint64_t result_seq; bool operator==(const Result&) const = default; };
  struct Decision { Verdict verdict; std::optional<Result> cached; };
  StopLedger(Authority authority, std::uint64_t reset_epoch)
      : authority_(std::move(authority)), reset_epoch_(reset_epoch) {}
  Decision claim(const StopEvent& event) {
    if (!authority_.valid() || event.authority != authority_ || event.reset_epoch != reset_epoch_ ||
        !event.prerequisites.satisfied() || !event.stop_only || !event.fresh || !event.command_seq || !event.intent_seq)
      return {Verdict::Rejected, {}};
    for (const auto& entry : entries_) if (entry && entry->event.command_seq == event.command_seq) {
      if (entry->event.digest != event.digest || entry->event.intent_seq != event.intent_seq)
        return {Verdict::Conflict, {}};
      return {entry->result ? Verdict::DuplicateCached : Verdict::DuplicatePending, entry->result};
    }
    if (event.command_seq <= highest_seq_) return {Verdict::Replay, {}};
    std::optional<std::size_t> slot;
    for (std::size_t index = 0; index < entries_.size(); ++index) {
      if (!entries_[index]) { slot = index; break; }
      if (entries_[index]->result && (!slot || entries_[index]->event.command_seq < entries_[*slot]->event.command_seq)) slot = index;
    }
    if (!slot) return {Verdict::Full, {}}; // Never evict an in-flight application.
    entries_[*slot] = Entry{event, {}}; highest_seq_ = event.command_seq;
    return {Verdict::ApplyOnce, {}};
  }
  bool commit(std::uint64_t seq, const Digest& digest, Result result) {
    if (!result.result_seq) return false;
    for (auto& entry : entries_) if (entry && entry->event.command_seq == seq && entry->event.digest == digest) {
      if (entry->result) return *entry->result == result;
      entry->result = result; latched_ = latched_ || result.software_latched; return true;
    }
    return false;
  }
  bool software_latched() const { return latched_; }
  std::size_t retained() const { std::size_t count = 0; for (const auto& entry : entries_) if (entry) ++count; return count; }
 private:
  struct Entry { StopEvent event; std::optional<Result> result; };
  Authority authority_;
  std::uint64_t reset_epoch_{}, highest_seq_{};
  bool latched_{};
  std::array<std::optional<Entry>, Capacity> entries_;
};
}  // namespace mine_teleop::path_selection_models
