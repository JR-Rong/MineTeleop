#include "stop_model.hpp"
#include "test_helpers.hpp"

#include <algorithm>
#include <iostream>
#include <vector>

namespace {
using namespace mine_teleop::path_selection_models;
using Ledger = StopLedger<4>;
StopEvent event(std::uint64_t seq = 1) { return {fixture_authority(), 3, seq, 9, fixture_digest(1), true, true, {true, true}}; }
void fastest_route_and_bounded_rescue() {
  std::array<StopRoute, 2> routes{{{Route::Dc, true, true, true, 60.0}, {Route::Wss, true, true, true, 10.0}}};
  auto plan = stop_dispatch_plan(event(), fixture_authority(), 3, routes);
  require(plan.count == 2 && plan.routes[0] == Route::Wss && plan.routes[1] == Route::Dc, "topology preference overrode actual stop latency");
  routes[0].actual_stop_result_ms = 5;
  require(stop_dispatch_plan(event(), fixture_authority(), 3, routes).routes[0] == Route::Dc, "faster measured DC not first");
  routes[1].healthy = false; routes[1].actual_stop_result_ms.reset();
  require(stop_dispatch_plan(event(), fixture_authority(), 3, routes).count == 2, "unmeasured rescue route was withheld");
  routes[1].authorized = false;
  require(stop_dispatch_plan(event(), fixture_authority(), 3, routes).count == 1, "unauthorized second path included");
  routes[1].route = Route::Dc;
  require(stop_dispatch_plan(event(), fixture_authority(), 3, routes).count == 0, "duplicate route duplicated a send");
}
void prerequisites_cannot_be_inferred_from_path_health() {
  const std::array<StopRoute, 2> routes{{{Route::Dc, true, true, true, 1.0}, {Route::Wss, true, true, true, 2.0}}};
  for (unsigned scenario = 0; scenario < 8; ++scenario) {
    auto value = event();
    if (scenario == 0) value.authority.driver = "other";
    if (scenario == 1) ++value.authority.epoch;
    if (scenario == 2) ++value.reset_epoch;
    if (scenario == 3) value.stop_only = false;
    if (scenario == 4) value.fresh = false;
    if (scenario == 5) value.prerequisites.verified = false;
    if (scenario == 6) value.prerequisites.caller_authorized = false;
    if (scenario == 7) value.command_seq = 0;
    Ledger ledger(fixture_authority(), 3);
    require(stop_dispatch_plan(value, fixture_authority(), 3, routes).count == 0, "fast path bypassed a required prerequisite");
    require(ledger.claim(value).verdict == Ledger::Verdict::Rejected, "receiver granted authority or ignored stop-only scope");
  }
}
void reserve_apply_commit_and_duplicate_cache() {
  Ledger ledger(fixture_authority(), 3); auto value = event();
  require(ledger.claim(value).verdict == Ledger::Verdict::ApplyOnce, "first valid stop lost");
  require(ledger.claim(value).verdict == Ledger::Verdict::DuplicatePending, "parallel duplicate could execute twice");
  auto conflict = value; conflict.digest = fixture_digest(2);
  require(ledger.claim(conflict).verdict == Ledger::Verdict::Conflict, "same seq conflicting body accepted");
  require(!ledger.commit(value.command_seq, conflict.digest, {true, 1}), "conflicting completion changed result");
  require(ledger.commit(value.command_seq, value.digest, {true, 1}), "result commit failed");
  const auto duplicate = ledger.claim(value);
  require(duplicate.verdict == Ledger::Verdict::DuplicateCached && duplicate.cached == Ledger::Result{true, 1}, "duplicate did not return immutable result");
  require(!ledger.commit(value.command_seq, value.digest, {false, 2}) && ledger.software_latched(), "duplicate completion cleared stop latch");
  require(ledger.claim(event(2)).verdict == Ledger::Verdict::ApplyOnce, "new stop seq rejected");
  require(ledger.claim(value).verdict == Ledger::Verdict::DuplicateCached, "older duplicate required re-application after newer seq");
}
void storage_is_bounded_and_eviction_never_reapplies() {
  Ledger ledger(fixture_authority(), 3);
  for (std::uint64_t seq = 1; seq <= 10000; ++seq) {
    auto value = event(seq);
    require(ledger.claim(value).verdict == Ledger::Verdict::ApplyOnce, "advancing stop lost under bounded cache");
    require(ledger.commit(seq, value.digest, {true, seq}), "result commit lost");
    require(ledger.retained() <= 4, "stop cache grew without bound");
  }
  require(ledger.claim(event(1)).verdict == Ledger::Verdict::Replay, "evicted stop executed again");
  Ledger pending(fixture_authority(), 3);
  for (std::uint64_t seq = 1; seq <= 4; ++seq) require(pending.claim(event(seq)).verdict == Ledger::Verdict::ApplyOnce, "reservation failed");
  require(pending.claim(event(5)).verdict == Ledger::Verdict::Full, "in-flight application evicted");
  require(pending.commit(1, event(1).digest, {true, 1}), "overflow invalidated a pending application");
  require(pending.claim(event(5)).verdict == Ledger::Verdict::ApplyOnce, "completed slot never reusable");
}
void old_authority_and_reset_events_cannot_affect_new_scope() {
  auto authority = fixture_authority(); ++authority.epoch;
  Ledger replacement(authority, 3);
  require(replacement.claim(event()).verdict == Ledger::Verdict::Rejected, "retired driver stop affected replacement authority");
  Ledger after_local_reset(fixture_authority(), 4);
  require(after_local_reset.claim(event()).verdict == Ledger::Verdict::Rejected, "pre-reset stop relatched new reset epoch");
}
void simulated_parallel_delivery_does_not_wait_for_blackhole() {
  // A discrete event simulation only: no assertion about real socket threads.
  const std::array<StopRoute, 2> routes{{{Route::Dc, true, true, true, 5.0}, {Route::Wss, true, true, true, 20.0}}};
  for (const auto blocked : {Route::Dc, Route::Wss}) {
    const auto plan = stop_dispatch_plan(event(), fixture_authority(), 3, routes);
    std::vector<std::pair<int, Route>> arrivals;
    for (std::size_t index = 0; index < plan.count; ++index) arrivals.emplace_back(plan.routes[index] == blocked ? 5000 : 8, plan.routes[index]);
    std::sort(arrivals.begin(), arrivals.end()); Ledger ledger(fixture_authority(), 3);
    int applications = 0, first_latch_at = -1;
    for (const auto& arrival : arrivals) if (ledger.claim(event()).verdict == Ledger::Verdict::ApplyOnce) {
      ++applications; first_latch_at = arrival.first; require(ledger.commit(1, event().digest, {true, 1}), "simulated stop did not latch");
    }
    require(first_latch_at == 8 && applications == 1, "serial wait or duplicate application in delivery model");
  }
}
}
int main() {
  try {
    fastest_route_and_bounded_rescue(); prerequisites_cannot_be_inferred_from_path_health(); reserve_apply_commit_and_duplicate_cache();
    storage_is_bounded_and_eviction_never_reapplies(); old_authority_and_reset_events_cannot_affect_new_scope(); simulated_parallel_delivery_does_not_wait_for_blackhole();
    std::cout << "6 stop model cases passed; simulated delivery only, no crypto/IO/physical stop proof\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
