#include "feedback_model.hpp"
#include "test_helpers.hpp"

#include <iostream>
#include <vector>

namespace {
using namespace mine_teleop::path_selection_models;
FeedbackSelection selection(std::uint64_t epoch, std::uint64_t floor, Route route) {
  return {fixture_authority(), epoch - 1, epoch, floor, route, route == Route::Dc ? "media-a" : "", {true, true}};
}
Status status(std::uint64_t epoch, std::uint64_t seq, unsigned kind, Route route) {
  return {fixture_authority(), epoch, seq, route, route == Route::Dc ? "media-a" : "", kind, {true, true}};
}
PairedReturnProbe::Reply reply(std::uint64_t nonce, Route route) {
  return {fixture_authority(), 1, nonce, 9, fixture_digest(1), route, "media-a", {true, true}};
}
void epoch_cas_idempotence_and_old_high_sequence() {
  FeedbackFence fence(fixture_authority());
  require(fence.select(selection(1, 10, Route::Wss)) && fence.accept(status(1, 11, 0, Route::Wss)), "initial feedback route failed");
  require(fence.select(selection(2, 11, Route::Dc)), "feedback CAS failed");
  require(!fence.accept(status(1, 999999, 0, Route::Wss)) && fence.watermark() == 11, "old high seq poisoned new feedback watermark");
  require(fence.accept(status(2, 12, 0, Route::Dc)), "new feedback result rejected");
  require(fence.select(selection(2, 11, Route::Dc)) && fence.watermark() == 12, "duplicate selection reset progress");
  auto invalid = selection(4, 12, Route::Wss);
  require(!fence.select(invalid) && fence.epoch() == 2, "selection skipped current epoch");
  invalid = selection(3, 11, Route::Wss);
  require(!fence.select(invalid), "feedback floor went backwards");
  auto message = status(2, 13, 0, Route::Dc); message.media_attempt = "retired";
  require(!fence.accept(message), "old media attempt accepted");
  message = status(2, 13, 4, Route::Dc); require(!fence.accept(message), "fifth status kind introduced actuation");
}
void four_kind_snapshot_filters_scope_before_coalescing() {
  FeedbackFence fence(fixture_authority()); require(fence.select(selection(1, 20, Route::Dc)), "snapshot selection failed");
  std::vector<Status> incoming{status(1, 24, 3, Route::Dc), status(1, 21, 0, Route::Dc), status(1, 23, 2, Route::Dc), status(1, 22, 1, Route::Dc),
      status(1, 25, 0, Route::Dc), status(0, 9999, 2, Route::Dc)};
  auto forged = status(1, 8888, 1, Route::Dc); forged.prerequisites.verified = false; incoming.push_back(forged);
  const auto batch = latest_status_batch(fence, incoming); require(batch.has_value(), "bounded snapshot rejected");
  std::array<bool, 4> seen{};
  for (const auto& message : *batch) { require(message && fence.accept(*message), "snapshot not ordered before global watermark"); seen[message->kind] = true; }
  require(std::all_of(seen.begin(), seen.end(), [](bool value) { return value; }) && fence.watermark() == 25, "status kind lost or forged watermark accepted");
  std::vector<Status> oversized(17, status(1, 26, 0, Route::Dc));
  require(!latest_status_batch(fence, oversized) && fence.watermark() == 25, "snapshot processing unbounded or changed state on rejection");
}
void paired_return_ranking_cancels_common_forward_delay() {
  for (const auto forward_delay : {0, 200}) {
    PairedReturnProbe probe(fixture_authority(), 1);
    require(probe.begin(1, 100, 1000, "media-a"), "return probe rejected");
    require(!probe.receive(reply(1, Route::Dc), 100 + forward_delay + 20), "single return fabricated a pair");
    const auto sample = probe.receive(reply(1, Route::Wss), 100 + forward_delay + 60);
    require(sample && sample->dc_minus_wss_ms == -40, "common forward jitter changed relative return ranking");
    require(probe.completed() == 1, "complete sample counted incorrectly");
    require(!probe.receive(reply(1, Route::Wss), 400) && probe.completed() == 1, "duplicate created progress");
  }
  PairedReturnProbe reverse(fixture_authority(), 1); reverse.begin(1, 100, 1000, "media-a");
  reverse.receive(reply(1, Route::Wss), 120);
  require(reverse.receive(reply(1, Route::Dc), 170)->dc_minus_wss_ms == 50, "feedback selector hardcoded DC preference");
}
void mismatched_events_and_clock_regression_never_pair() {
  PairedReturnProbe probe(fixture_authority(), 1); require(probe.begin(1, 100, 300, "media-a"), "probe start failed");
  require(!probe.receive(reply(1, Route::Dc), 150), "first response not pending");
  for (unsigned scenario = 0; scenario < 7; ++scenario) {
    auto invalid = reply(1, Route::Wss);
    if (scenario == 0) invalid.produced_seq = 10;
    if (scenario == 1) invalid.same_produced_event = fixture_digest(2);
    if (scenario == 2) ++invalid.cohort;
    if (scenario == 3) invalid.authority.driver = "other";
    if (scenario == 4) invalid.media_attempt = "retired";
    if (scenario == 5) invalid.prerequisites.verified = false;
    if (scenario == 6) invalid.prerequisites.caller_authorized = false;
    require(!probe.receive(invalid, 160) && probe.completed() == 0, "mismatched reply manufactured progress");
  }
  require(!probe.receive(reply(1, Route::Wss), 149), "clock regression created valid zero/negative return age");
  require(probe.receive(reply(1, Route::Wss), 170).has_value(), "invalid samples consumed a later valid pair");
  require(!probe.begin(2, 169, 400, "media-a"), "new probe clock went backwards");
}
void single_outstanding_deadline_and_attempt_scope() {
  PairedReturnProbe probe(fixture_authority(), 1); require(probe.begin(1, 0, 20, "media-a"), "zero monotonic origin rejected");
  require(!probe.begin(2, 1, 30, "media-a"), "unbounded parallel probes allowed");
  require(!probe.receive(reply(1, Route::Dc), 21), "expired probe renewed health");
  require(probe.begin(2, 21, 40, "media-a"), "expired probe slot not reusable");
  require(!probe.receive(reply(1, Route::Wss), 22), "retired nonce joined a new pair");
  require(!probe.begin(2, 41, 60, "media-a"), "reused nonce accepted after expiry");
  PairedReturnProbe replacement(fixture_authority(), 2); replacement.begin(1, 0, 100, "media-b");
  require(!replacement.receive(reply(1, Route::Dc), 10), "retired cohort/attempt reply accepted by replacement");
}
}
int main() {
  try {
    epoch_cas_idempotence_and_old_high_sequence(); four_kind_snapshot_filters_scope_before_coalescing(); paired_return_ranking_cancels_common_forward_delay();
    mismatched_events_and_clock_regression_never_pair(); single_outstanding_deadline_and_attempt_scope();
    std::cout << "5 feedback model cases passed; relative paired return timing, no absolute one-way claim\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
