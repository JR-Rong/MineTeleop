#include "mine_teleop/detail/transient_hold.hpp"
#include <iostream>
#include <stdexcept>
using namespace mine_teleop::detail;
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
TransientHoldConfig enabled() { TransientHoldConfig config; config.enabled = true; return config; }
void resume_within_window() {
  TransientHoldPolicy policy(enabled());
  require(policy.set_cause(TransientCause::ControlGap, true, 1000) == TransientHoldAction::Enter, "gap did not enter hold");
  require(policy.tick(1500) == TransientHoldAction::Hold, "hold ended early");
  require(policy.set_cause(TransientCause::ControlGap, false, 1550) == TransientHoldAction::Hold, "clearing a cause ended the hold");
  require(policy.note_fresh_command(1550) == TransientHoldAction::Hold, "one fresh command resumed");
  require(policy.note_fresh_command(1600) == TransientHoldAction::Resume && !policy.active(), "second fresh command did not resume");
  require(policy.status(1600, true).at("last").at("outcome") == "resumed", "resume outcome not recorded");
}
void cause_reappearing_resets_fresh_count() {
  TransientHoldPolicy policy(enabled());
  policy.set_cause(TransientCause::DriverVideoStale, true, 0);
  policy.set_cause(TransientCause::DriverVideoStale, false, 100);
  require(policy.note_fresh_command(100) == TransientHoldAction::Hold, "first fresh command after clearing");
  policy.set_cause(TransientCause::DriverVideoStale, true, 150);
  policy.set_cause(TransientCause::DriverVideoStale, false, 200);
  require(policy.note_fresh_command(200) == TransientHoldAction::Hold, "fresh count survived a new stall");
  require(policy.note_fresh_command(250) == TransientHoldAction::Resume, "no resume after two consecutive fresh commands");
}
void escalate_at_max_hold() {
  TransientHoldPolicy policy(enabled());
  policy.set_cause(TransientCause::ControlGap, true, 1000);
  require(policy.tick(1699) == TransientHoldAction::Hold, "escalated before 700 ms");
  require(policy.tick(1700) == TransientHoldAction::Escalate && policy.escalated(), "did not escalate at 700 ms");
  require(policy.escalation_reason() == "hold_duration_exceeded", "wrong escalation reason");
  require(policy.set_cause(TransientCause::ControlGap, true, 1800) == TransientHoldAction::None, "escalated policy re-entered a hold");
  policy.escalation_cleared();
  require(policy.set_cause(TransientCause::ControlGap, true, 5000) == TransientHoldAction::Enter, "cleared policy cannot hold again");
}
void budget_count_time_and_window() {
  TransientHoldPolicy count(enabled());
  for (int i = 0; i < 6; ++i) {
    const std::int64_t t = i * 1000;
    require(count.set_cause(TransientCause::LateCommands, true, t) == TransientHoldAction::Enter, "budgeted hold rejected");
    count.set_cause(TransientCause::LateCommands, false, t + 50);
    count.note_fresh_command(t + 50);
    require(count.note_fresh_command(t + 100) == TransientHoldAction::Resume, "budgeted hold did not resume");
  }
  require(count.set_cause(TransientCause::LateCommands, true, 6000) == TransientHoldAction::Escalate &&
      count.escalation_reason() == "budget_count_exceeded", "seventh hold within the window was not escalated");
  // Each hold lasts 650 ms. Four completed holds use 2600 ms of the 3000 ms budget,
  // so the fifth hold must escalate mid-hold (at +450 ms), not at the next entry.
  TransientHoldPolicy time(enabled());
  int escalated_at = -1;
  for (int i = 0; i < 6 && escalated_at < 0; ++i) {
    const std::int64_t t = i * 2000;
    if (time.set_cause(TransientCause::ControlGap, true, t) == TransientHoldAction::Escalate) { escalated_at = i; break; }
    for (std::int64_t step = 50; step <= 650 && escalated_at < 0; step += 50)
      if (time.tick(t + step) == TransientHoldAction::Escalate) escalated_at = i;
    if (escalated_at >= 0) break;
    time.set_cause(TransientCause::ControlGap, false, t + 650);
    time.note_fresh_command(t + 650); time.note_fresh_command(t + 660);
  }
  require(escalated_at == 4 && time.escalation_reason() == "budget_time_exceeded",
      "cumulative hold time budget was not enforced mid-hold on the fifth hold");
  TransientHoldPolicy window(enabled());
  for (int i = 0; i < 6; ++i) {
    const std::int64_t t = i * 1000;
    window.set_cause(TransientCause::LateCommands, true, t);
    window.set_cause(TransientCause::LateCommands, false, t + 10);
    window.note_fresh_command(t + 10); window.note_fresh_command(t + 20);
  }
  require(window.set_cause(TransientCause::LateCommands, true, 66000) == TransientHoldAction::Enter, "budget window did not slide");
}
void fault_clock_and_disabled() {
  TransientHoldPolicy fault(enabled());
  fault.set_cause(TransientCause::VehicleVideoStale, true, 100);
  require(fault.escalate_active(200, "fault_during_hold") == TransientHoldAction::Escalate &&
      fault.escalation_reason() == "fault_during_hold", "fault during hold not escalated");
  TransientHoldPolicy clock(enabled());
  clock.set_cause(TransientCause::ControlGap, true, 100);
  require(clock.tick(50) == TransientHoldAction::Escalate && clock.escalation_reason() == "clock_reversed", "clock reversal did not fail closed");
  TransientHoldPolicy disabled;
  require(disabled.set_cause(TransientCause::ControlGap, true, 0) == TransientHoldAction::None && !disabled.active(), "disabled policy held");
}
void validation_offer_and_lanes() {
  bool threw = false;
  try { auto bad = enabled(); bad.max_hold_ms = 99; TransientHoldPolicy policy(bad); } catch (const std::invalid_argument&) { threw = true; }
  require(threw, "max_hold_ms below range accepted");
  threw = false;
  try { auto bad = enabled(); bad.hold_brake = 0.31; bad.validate(); } catch (const std::invalid_argument&) { threw = true; }
  require(threw, "hold_brake above 0.3 accepted");
  const nlohmann::json offer = {{"transient_hold_capability", "transient-hold-v1"},
      {"transient_hold", {{"control_tolerance_ms", 1000}, {"video_tolerance_ms", 1200}, {"max_hold_ms", 700}}}};
  const auto parsed = parse_transient_hold_offer(offer);
  require(parsed && parsed->control_tolerance_ms == 1000 && parsed->video_tolerance_ms == 1200 && parsed->max_hold_ms == 700, "valid offer rejected");
  auto wide = offer; wide["transient_hold"]["video_tolerance_ms"] = 4500;
  require(parse_transient_hold_offer(wide).has_value(), "maximum video tolerance rejected");
  for (auto broken : {nlohmann::json::object(), nlohmann::json{{"transient_hold_capability", "transient-hold-v2"}}}) require(!parse_transient_hold_offer(broken), "unsupported offer accepted");
  auto low = offer; low["transient_hold"]["control_tolerance_ms"] = 399;
  require(!parse_transient_hold_offer(low), "control tolerance below 400 accepted");
  auto text = offer; text["transient_hold"]["max_hold_ms"] = "700";
  require(!parse_transient_hold_offer(text), "non-integer field accepted");
  require(classify_vehicle_video_lane(false, false, true, false, true, 0, false) == VehicleVideoLaneState::Fresh, "fresh lane misclassified");
  require(classify_vehicle_video_lane(false, false, true, false, false, 0, false) == VehicleVideoLaneState::Stale, "aged lane not stale");
  require(classify_vehicle_video_lane(false, false, true, false, true, 2, false) == VehicleVideoLaneState::Stale, "dropped frames not stale");
  require(classify_vehicle_video_lane(false, false, true, false, true, 0, true) == VehicleVideoLaneState::Stale, "recovering lane not stale");
  require(classify_vehicle_video_lane(false, false, true, true, true, 0, false) == VehicleVideoLaneState::Fault, "invalid samples not a fault");
  require(classify_vehicle_video_lane(true, false, true, false, true, 0, false) == VehicleVideoLaneState::Fault, "disabled lane not a fault");
  require(classify_vehicle_video_lane(false, true, true, false, true, 0, false) == VehicleVideoLaneState::Fault, "faulted lane not a fault");
  require(classify_vehicle_video_lane(false, false, false, false, true, 0, false) == VehicleVideoLaneState::Fault, "missing freshness not a fault");
}
void legacy_lane_decision_is_unchanged() {
  // Without a negotiated hold the per-lane decision must equal the media runtime's
  // original expression for every combination of its inputs.
  for (int bits = 0; bits < (1 << 7); ++bits) {
    const bool disabled = bits & 1, faulted = bits & 2, has = bits & 4, invalid = bits & 8, fresh = bits & 16,
        waiting = bits & 32, enough_frames = bits & 64;
    for (unsigned anomalies = 0; anomalies <= 4; ++anomalies) {
      const bool legacy = disabled || faulted || !has || invalid || anomalies >= 3 || !fresh ||
          (waiting && (anomalies != 0 || !enough_frames));
      const bool awaiting = waiting && !enough_frames;
      const auto legacy_state = classify_vehicle_video_lane(disabled, faulted, has, invalid, fresh,
          effective_lane_anomalies(anomalies, false, waiting), awaiting);
      require(vehicle_video_lane_blocks_control(legacy_state, false) == legacy, "legacy lane decision changed");
      // With a negotiated hold only faults block control; staleness becomes a hold.
      const auto hold_state = classify_vehicle_video_lane(disabled, faulted, has, invalid, fresh,
          effective_lane_anomalies(anomalies, true, waiting), awaiting);
      require(vehicle_video_lane_blocks_control(hold_state, true) == (disabled || faulted || !has || invalid),
          "hold-mode lane decision treated staleness as a fault");
    }
  }
}
}  // namespace
int main() {
  try {
    resume_within_window(); cause_reappearing_resets_fresh_count(); escalate_at_max_hold();
    budget_count_time_and_window(); fault_clock_and_disabled(); validation_offer_and_lanes();
    legacy_lane_decision_is_unchanged();
    std::cout << "PASS transient hold policy: resume, escalation, budget, clock, offer and lane classification\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
