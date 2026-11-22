#include "switch_model.hpp"
#include "test_helpers.hpp"

#include <iostream>

namespace {
using namespace mine_teleop::path_selection_models;
using Model = SwitchModel;
using Path = Model::Path;
using Policy = mine_teleop::detail::LowestLatencyPathPolicy;
using Metric = mine_teleop::detail::PathMeasurement;
Model::Proposal proposal(Model::Axis axis = Model::Axis::Control, Path path = Path::P2p, bool failed = false) {
  return {fixture_authority(), axis, path, 2, 1, true, true, failed, {true, true}};
}
Model::Barrier barrier(std::uint64_t ticket, Model::Axis axis = Model::Axis::Control, Path path = Path::P2p) {
  return {fixture_authority(), ticket, 1, 2, axis, path, {true, true}};
}
Model::Result result(std::uint64_t ticket, std::uint64_t seq, Path path = Path::P2p) {
  return {fixture_authority(), ticket, seq, 100 + seq, 2, 1, path, Model::Evidence::VehicleControlResult, true, true, true, true, {true, true}};
}
Policy selector() {
  // Explicit fixture values only; no product/safety defaults are introduced.
  return Policy({Metric::ControlVehicleResultRoundTrip, 200, 3, 5, 0.1, 100, 300});
}
Policy::Observation observation(Path path, double latency, std::int64_t now) {
  return {path, fixture_authority().generation, Metric::ControlVehicleResultRoundTrip, true, true, path == Path::CloudWss ? 1U : 2U, 3, now, latency};
}
void continuous_improvement_waits_for_neutral_and_true_results() {
  auto policy = selector(); policy.reset(7); policy.confirm_selected(Path::CloudWss, 7, 1, 0);
  std::array paths{observation(Path::CloudWss, 50, 1000), observation(Path::P2p, 20, 1000)};
  require(policy.recommend(1000, paths).reason == Policy::Reason::Stabilizing, "performance change ignored stability");
  for (auto& path : paths) path.progress_monotonic_ms = 1100;
  const auto recommendation = policy.recommend(1100, paths);
  require(recommendation.reason == Policy::Reason::BetterPath && recommendation.path == Path::P2p, "continuous faster path never proposed");
  Model model(fixture_authority(), 3);
  const auto ticket = model.propose(proposal(), false); require(ticket.has_value(), "model rejected a valid recommendation");
  require(model.phase() == Model::Phase::WaitingNeutral && !model.withdraw_motion(), "healthy moving path interrupted for a performance trial");
  require(!model.barrier(barrier(*ticket)), "selection confirmation skipped natural neutral");
  require(model.neutral_boundary(*ticket) && model.withdraw_motion(), "neutral switch did not withdraw old motion");
  require(model.barrier(barrier(*ticket)), "fenced selection result rejected");
  for (const auto evidence : {Model::Evidence::Probe, Model::Evidence::CloudIngress, Model::Evidence::IceRtt}) {
    auto invalid = result(*ticket, 1); invalid.evidence = evidence;
    require(!model.result(invalid) && model.confirmed_path(Model::Axis::Control) == Path::CloudWss, "probe/ingress/ICE result qualified control");
  }
  require(!model.result(result(*ticket, 1)), "one result qualified control");
  require(!model.result(result(*ticket, 1)), "duplicate qualified control");
  auto stale = result(*ticket, 2); stale.fresh = false;
  require(!model.result(stale), "stale result qualified control");
  require(!model.result(result(*ticket, 3)) && !model.result(result(*ticket, 4)), "bad sample did not reset consecutive qualification");
  require(model.result(result(*ticket, 5)) && model.confirmed_path(Model::Axis::Control) == Path::P2p, "actual accepted neutral results never qualified candidate");
  // Production still must enforce existing input recovery/release-repress.
}
void feedback_switch_is_independent_and_invalidates_control_cohort() {
  Model model(fixture_authority(), 2);
  const auto ticket = model.propose(proposal(Model::Axis::Feedback), false);
  require(ticket && model.phase() == Model::Phase::AwaitBarrier && !model.withdraw_motion(), "gap-free feedback change unnecessarily withdrew healthy motion");
  require(!model.propose(proposal(), true), "two axes could switch concurrently");
  require(model.barrier(barrier(*ticket, Model::Axis::Feedback)), "feedback barrier rejected");
  auto first = result(*ticket, 1); first.neutral = false;
  require(!model.result(first), "feedback route qualified on one result");
  first.command_seq = 2; first.intent_seq = 102;
  require(model.result(first), "healthy real feedback required a needless operator neutral");
  require(model.confirmed_path(Model::Axis::Control) == Path::CloudWss && model.confirmed_path(Model::Axis::Feedback) == Path::P2p,
      "feedback selection silently changed command path");
  require(model.feedback_cohort() == 2 && !model.propose(proposal(), true), "old mixed-return cohort proposed a control change");
  auto fresh = proposal(); fresh.feedback_cohort = 2;
  require(model.propose(fresh, true).has_value(), "fresh fixed-return control cohort never usable");
}
void hard_failure_bypasses_performance_dwell_and_safety_interrupt_retires_ticket() {
  auto policy = selector(); policy.reset(7); policy.confirm_selected(Path::P2p, 7, 2, 1000);
  std::array paths{observation(Path::P2p, 10, 1001), observation(Path::CloudWss, 50, 1001)};
  paths[0].healthy = false;
  require(policy.recommend(1001, paths).reason == Policy::Reason::UnhealthyFallback, "hard failure waited for dwell");
  Model model(fixture_authority(), 3);
  const auto ticket = model.propose(proposal(Model::Axis::Control, Path::TurnTcp, true), false);
  require(ticket && model.phase() == Model::Phase::AwaitBarrier && model.withdraw_motion(), "hard failure deferred withdrawal until operator neutral");
  model.interrupt_for_safety();
  require(model.phase() == Model::Phase::SafetyHold && model.withdraw_motion(), "safety interruption auto-resumed old path");
  require(!model.barrier(barrier(*ticket, Model::Axis::Control, Path::TurnTcp)) && !model.result(result(*ticket, 1, Path::TurnTcp)),
      "retired switch confirmation resumed motion");
  require(!model.propose(proposal(Model::Axis::Feedback), true), "safety hold silently started another axis without reconciliation");
  paths[1].healthy = false;
  require(!policy.recommend(1002, paths).path, "all unhealthy paths produced a route");
}
void scope_selectability_and_authentication_are_external_prerequisites() {
  for (unsigned scenario = 0; scenario < 6; ++scenario) {
    Model model(fixture_authority(), 3); auto invalid = proposal();
    if (scenario == 0) invalid.authority.generation = 6;
    if (scenario == 1) invalid.prerequisites.verified = false;
    if (scenario == 2) invalid.prerequisites.caller_authorized = false;
    if (scenario == 3) invalid.selectable = false;
    if (scenario == 4) invalid.candidate_healthy = false;
    if (scenario == 5) invalid.attempt = 0;
    require(!model.propose(invalid, true) && model.phase() == Model::Phase::Idle, "path model invented eligibility or authority");
  }
  Model model(fixture_authority(), 1); const auto ticket = *model.propose(proposal(), true);
  auto invalid_barrier = barrier(ticket); invalid_barrier.authority.epoch++;
  require(!model.barrier(invalid_barrier), "wrong authority barrier accepted");
  invalid_barrier = barrier(ticket); invalid_barrier.epoch = 2;
  require(!model.barrier(invalid_barrier), "selection epoch skipped");
  require(model.barrier(barrier(ticket)), "valid barrier lost after invalid ones");
  for (unsigned scenario = 0; scenario < 5; ++scenario) {
    auto invalid = result(ticket, 1);
    if (scenario == 0) invalid.authority.session = "retired";
    if (scenario == 1) invalid.matched_native_send = false;
    if (scenario == 2) invalid.attempt = 1;
    if (scenario == 3) invalid.feedback_cohort = 0;
    if (scenario == 4) invalid.prerequisites.verified = false;
    require(!model.result(invalid), "foreign/unmatched/retired result qualified path");
  }
  require(model.result(result(ticket, 1)), "invalid results poisoned subsequent valid exact result");
  rejects([] { Model invalid(fixture_authority(), 0); });
}
void small_improvement_and_repeated_probes_do_not_flap() {
  auto policy = selector(); policy.reset(7); policy.confirm_selected(Path::CloudWss, 7, 1, 0);
  std::array paths{observation(Path::CloudWss, 50, 1000), observation(Path::P2p, 48, 1000)};
  require(policy.recommend(1000, paths).reason == Policy::Reason::KeepCurrent, "noise triggered a switch");
  paths[1].latency_ms = 20; policy.recommend(1000, paths);
  require(policy.recommend(1100, paths).reason == Policy::Reason::Stabilizing, "same sample reread counted as progress");
  paths[1].transport_attempt = 3; paths[1].progress_monotonic_ms = 1101; paths[0].progress_monotonic_ms = 1101;
  require(policy.recommend(1101, paths).reason == Policy::Reason::Stabilizing, "new attempt inherited old stability");
  paths[1].measurement = Metric::IceRoundTrip;
  require(policy.recommend(1102, paths).path == Path::CloudWss, "ICE check won a control-performance ranking");
  paths[1].measurement = Metric::ControlVehicleResultRoundTrip; paths[1].selectable = false;
  require(policy.recommend(1103, paths).path == Path::CloudWss, "unselectable ICE pair was ranked as selectable");
}
}
int main() {
  try {
    continuous_improvement_waits_for_neutral_and_true_results(); feedback_switch_is_independent_and_invalidates_control_cohort();
    hard_failure_bypasses_performance_dwell_and_safety_interrupt_retires_ticket(); scope_selectability_and_authentication_are_external_prerequisites();
    small_improvement_and_repeated_probes_do_not_flap();
    std::cout << "5 switch model cases passed; advisory policy and deterministic state machine only\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
