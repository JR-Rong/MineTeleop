#include "mine_teleop/core.hpp"
#include "mine_teleop/detail/native_control_acknowledgement_window.hpp"
#include "mine_teleop/detail/native_control_dispatch.hpp"

#include <cstdint>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace {

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

mine_teleop::NativeControlIntent neutral_intent(
    std::uint64_t sequence,
    std::string gear = "D") {
  mine_teleop::NativeControlIntent intent;
  intent.ui_instance_id = "portable-test-ui";
  intent.intent_seq = sequence;
  intent.gear = std::move(gear);
  return intent;
}

void expect_zero_actuation(
    const mine_teleop::NativeControlIntent& intent,
    std::string_view message) {
  expect(
      intent.steering == 0.0 && intent.throttle == 0.0 && intent.brake == 0.0,
      message);
}

void test_lease_expiry_neutralizes_and_preserves_gear() {
  mine_teleop::NativeControlIntentStore store(150);
  auto neutral = neutral_intent(1, "R");
  expect(store.update(neutral, 100).accepted, "initial neutral intent was rejected");

  auto drive = neutral;
  drive.intent_seq = 2;
  drive.steering = -0.4;
  drive.throttle = 0.6;
  drive.brake = 0.2;
  expect(store.update(drive, 110).accepted, "drive intent was rejected after neutral arming");

  const auto fresh = store.sample(259);
  expect(
      fresh.active && fresh.fresh && !fresh.requires_fresh_input &&
          fresh.intent.gear == "R" && fresh.intent.throttle == 0.6,
      "fresh drive intent changed before the lease boundary");

  const auto expired = store.sample(260);
  expect(
      expired.active && !expired.fresh && expired.requires_fresh_input,
      "lease expiry did not enter the fresh-input interlock");
  expect(expired.intent.gear == "R", "lease expiry did not preserve the selected gear");
  expect_zero_actuation(expired.intent, "lease expiry did not zero all actuation");
}

void test_invalidate_does_not_restore_old_throttle() {
  mine_teleop::NativeControlIntentStore store(150);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 10).accepted, "initial neutral intent was rejected");

  auto drive = neutral;
  drive.intent_seq = 2;
  drive.throttle = 0.7;
  expect(store.update(drive, 20).accepted, "drive intent was rejected");

  store.invalidate();
  auto sample = store.sample(21);
  expect(sample.requires_fresh_input, "invalidate did not engage the fresh-input interlock");
  expect_zero_actuation(sample.intent, "invalidate did not immediately neutralize actuation");

  const auto replay = store.update(drive, 22);
  expect(
      !replay.accepted && replay.reason == "fresh_neutral_required",
      "invalidate allowed the prior throttle intent to be replayed");

  auto newer_drive = drive;
  newer_drive.intent_seq = 3;
  const auto newer = store.update(newer_drive, 23);
  expect(
      !newer.accepted && newer.reason == "fresh_neutral_required",
      "invalidate allowed a non-neutral sequence update without neutral re-arming");
  sample = store.sample(24);
  expect_zero_actuation(sample.intent, "rejected input restored throttle after invalidate");
}

void test_delayed_refresh_cannot_bypass_expiry_between_sender_ticks() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 0).accepted, "neutral rejected");
  auto held = neutral; held.intent_seq = 2; held.throttle = 0.5;
  expect(store.update(held, 10).accepted, "held input rejected");
  expect(store.update(held, 210).reason == "fresh_neutral_required",
      "late refresh revived motion without a periodic expiry sample");
  expect_zero_actuation(store.sample(211).intent, "late refresh restored traction");
  neutral.intent_seq = 3;
  expect(store.update(neutral, 212).accepted, "neutral recovery rejected");
  held.intent_seq = 4;
  expect(store.update(held, 213).accepted, "release/repress did not recover in the same store");
}

void test_out_of_order_local_refresh_cannot_rewind_lease_or_restore_released_motion() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 0).accepted, "neutral rejected");
  auto motion = neutral; motion.intent_seq = 2; motion.throttle = 0.5;
  expect(store.update(motion, 20).accepted && store.update(motion, 80).accepted, "refresh rejected");
  expect(store.update(motion, 40).reason == "old_intent_arrival", "old refresh rewound lease");
  expect(store.sample(100).age_ms == 20, "old refresh changed authoritative receive time");
  neutral.intent_seq = 3;
  expect(store.update(neutral, 110).accepted, "release rejected");
  expect(!store.update(motion, 120).accepted, "old held request overwrote release");
  expect_zero_actuation(store.sample(121).intent, "out of order held input restored traction");
  auto estop = neutral; estop.intent_seq = 4; estop.estop = true;
  expect(store.update(estop, 100).accepted && store.sample(122).intent.estop,
      "late ESTOP was rejected because its local receive timestamp was older");
}

void test_confirmed_page_takeover_retires_late_neutral_without_retiring_estop() {
  mine_teleop::NativeControlIntentStore store(200);
  auto old_neutral = neutral_intent(1);
  expect(store.update(old_neutral, 100).ui_control_epoch == 1, "first page did not acquire epoch 1");
  auto new_neutral = neutral_intent(1);
  new_neutral.ui_instance_id = "new-page";
  const auto claim = store.update(new_neutral, 200, 1, true);
  expect(claim.accepted && claim.ui_control_epoch == 2, "new page neutral confirmation did not transfer ownership");
  auto held = new_neutral; held.intent_seq = 2; held.throttle = 0.5;
  expect(store.update(held, 210, 2).accepted, "confirmed page could not operate");
  old_neutral.intent_seq = 2;
  expect(store.update(old_neutral, 190, 1).reason == "old_intent_arrival",
      "old-page late neutral overwrote the new page's motion");
  expect(store.update(old_neutral, 220, 1).reason == "input_page_not_current",
      "a retired page refreshed ordinary input");
  expect(store.update(old_neutral, 220, 1, true).reason == "input_page_takeover_conflict",
      "a delayed retired-page handshake reacquired ownership");
  const auto continued = store.update(held, 230, 2);
  const auto active = store.sample(231);
  expect(continued.accepted && continued.duplicate && active.intent.throttle == 0.5 &&
      !active.requires_fresh_input && active.age_ms == 1 && active.owner_ui_instance_id == "new-page",
      "rejecting retired requests interrupted the current page or rewound its lease");
  auto retired_estop = old_neutral; retired_estop.intent_seq = 3; retired_estop.estop = true;
  expect(store.update(retired_estop, 180, 1).accepted, "legal retired-page ESTOP lost priority");
  const auto stopped = store.sample(232);
  expect(stopped.intent.estop && stopped.owner_ui_instance_id == "new-page" &&
      stopped.ui_control_epoch == 2 && stopped.intent.intent_seq == held.intent_seq,
      "retired-page ESTOP transferred ordinary ownership or regressed the current page's sequence");
  expect_zero_actuation(stopped.intent, "retired-page ESTOP retained ordinary actuation");
}

void test_page_takeover_requires_confirmation_and_preserves_current_input_on_rejection() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 10).accepted, "initial neutral rejected");
  auto held = neutral; held.intent_seq = 2; held.throttle = 0.4;
  expect(store.update(held, 20, 1).accepted, "initial motion rejected");
  auto other = neutral; other.ui_instance_id = "other-page";
  expect(store.update(other, 30).reason == "input_page_not_current", "ordinary neutral implicitly took over");
  expect(store.update(other, 30, 0, true).reason == "input_page_takeover_conflict", "stale epoch took over");
  other.throttle = 0.2;
  expect(store.update(other, 30, 1, true).reason == "fresh_neutral_required", "non-neutral input took over");
  const auto active = store.sample(31);
  expect(active.intent.throttle == 0.4 && !active.requires_fresh_input && active.ui_control_epoch == 1,
      "a rejected takeover changed current input or its interlock");
}

void test_lost_takeover_response_is_retryable_without_epoch_growth_or_motion() {
  mine_teleop::NativeControlIntentStore store(200);
  store.begin_session(10);
  const auto bootstrap = store.sample(11);
  expect(bootstrap.owner_ui_instance_id.empty() && bootstrap.ui_control_epoch == 0 &&
      bootstrap.accepted_refresh_sample_id == 0 && bootstrap.requires_fresh_input,
      "native bootstrap claimed browser ownership or counted as UI input");
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 20, 0, true).ui_control_epoch == 1, "initial handshake failed");
  expect(store.update(neutral, 30, 0, true).accepted, "lost-response identical handshake could not retry");
  neutral.intent_seq = 2; neutral.critical_video_ready = false;
  expect(store.update(neutral, 40, 0, true).accepted, "new neutral readiness sample could not confirm a lost response");
  auto drive = neutral; drive.intent_seq = 3; drive.throttle = 0.5;
  expect(store.update(drive, 50, 0).reason == "input_page_epoch_mismatch", "motion used an unconfirmed epoch");
  expect(store.sample(51).ui_control_epoch == 1, "retry grew the page epoch");
  expect_zero_actuation(store.sample(51).intent, "handshake retry allowed motion");
  expect(store.update(drive, 52, 1).accepted, "confirmed epoch did not permit fresh input");
  store.reset();
  expect(!store.sample(53).active, "session reset retained ownership or input");
  neutral.ui_instance_id = "next-session-page";
  expect(store.update(neutral, 54, 0, true).ui_control_epoch == 1, "new session inherited retired epochs");
}

void test_refresh_sample_ids_distinguish_real_refresh_replay_and_sender_sampling() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  const auto initial = store.update(neutral, 10);
  expect(initial.accepted_sample_id == 1 && initial.accepted_refresh_sample_id == 1,
      "initial UI data did not create sample IDs");
  const auto heartbeat = store.update(neutral, 76, 1);
  expect(heartbeat.duplicate && heartbeat.accepted_sample_id == 1 && heartbeat.accepted_refresh_sample_id == 2,
      "new HTTP lease data was confused with an operation change");
  expect(store.update(neutral, 76, 1).accepted_refresh_sample_id == 2,
      "same receive timestamp replay created a new refresh sample");
  expect(store.update(neutral, 40, 1).reason == "old_intent_arrival", "old arrival accepted");
  auto conflict = neutral; conflict.throttle = 0.2;
  expect(store.update(conflict, 90, 1).reason == "intent_seq_conflict", "same-sequence conflict accepted");
  for (std::int64_t now = 80; now < 150; ++now) {
    expect(store.sample(now).accepted_refresh_sample_id == 2, "native polling forged UI refresh data");
  }
  auto held = neutral; held.intent_seq = 2; held.throttle = 0.5;
  expect(store.update(held, 150, 1).accepted, "new motion rejected");
  expect(store.sample(349).fresh && !store.sample(350).fresh,
      "timer lag widened the unchanged 200ms input lease");
  expect(store.update(held, 351, 1).reason == "fresh_neutral_required",
      "post-stall refresh resurrected held motion");
  expect(store.sample(352).accepted_refresh_sample_id == 3,
      "rejected stale held input advanced the valid refresh sample");
}

void test_ingress_anomaly_samples_are_distinct_and_timeout_is_independent() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 10).accepted, "neutral rejected");
  auto held = neutral; held.intent_seq = 2; held.throttle = 0.4;
  expect(store.update(held, 20, 1).accepted, "motion rejected");
  const auto first = store.update(held, 30, 1, false, 230);
  expect(!first.accepted && first.reason == "local_input_request_expired" &&
      first.consecutive_refresh_anomalies == 1 && first.refresh_sample_id == 3 &&
      !first.refresh_sample_healthy, "first real overage refresh did not record one anomaly");
  expect(!store.sample(230).fresh && store.sample(230).requires_fresh_input,
      "independent 200ms no-data timeout waited for three anomalies");
  expect(store.update(held, 30, 1, false, 231).consecutive_refresh_anomalies == 1,
      "same overage request polling counted twice");
  expect(store.update(held, 25, 1, false, 231).reason == "old_intent_arrival",
      "out-of-order refresh escaped the sample fence");
  auto retired = held; retired.ui_instance_id = "unconfirmed-page";
  expect(store.update(retired, 40, 0, false, 240).reason == "input_page_not_current",
      "retired page joined current input sampling");
  auto conflict = held; conflict.throttle = 0.2;
  expect(store.update(conflict, 40, 1, false, 240).reason == "intent_seq_conflict",
      "conflicting value was counted as a legitimate refresh anomaly");
  expect(store.sample(241).consecutive_refresh_anomalies == 1,
      "rejected input reset or advanced the input anomaly count");
  auto newer = held; newer.intent_seq = 3;
  expect(store.update(newer, 40, 1, false, 240).consecutive_refresh_anomalies == 2,
      "second distinct legitimate overage refresh did not count");
  newer.intent_seq = 4;
  expect(store.update(newer, 50, 1, false, 250).consecutive_refresh_anomalies == 3,
      "three distinct legitimate overage refreshes did not reach their own threshold");
  neutral.intent_seq = 3;
  expect(store.update(neutral, 251, 1, false, 251).reason == "old_intent_seq" &&
      store.sample(251).consecutive_refresh_anomalies == 3,
      "a late normal value older than an anomalous sample cleared the counter");
  neutral.intent_seq = 5;
  const auto recovered = store.update(neutral, 252, 1, false, 252);
  expect(recovered.accepted && recovered.consecutive_refresh_anomalies == 0 &&
      recovered.refresh_sample_healthy && recovered.refresh_sample_id == 6,
      "new accepted fresh neutral did not clear the input-only counter");
  expect(!store.update(held, 253, 1).accepted, "normal recovery resurrected the old held input");
}

void test_raw_video_sample_is_preserved_separately_from_input_and_qualified_video() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1); neutral.critical_video_sample_healthy = true;
  expect(store.update(neutral, 0).accepted, "neutral rejected");
  auto drive = neutral; drive.intent_seq = 2; drive.throttle = 0.4;
  expect(store.update(drive, 10, 1).accepted, "initial motion rejected");
  drive.intent_seq = 3; drive.critical_video_sample_healthy = false;
  expect(store.update(drive, 20, 1).accepted, "tolerated raw video sample was rejected as input");
  const auto sample = store.sample(21);
  expect(sample.intent.critical_video_ready && !sample.intent.critical_video_sample_healthy &&
      sample.intent.throttle == 0.4 && sample.fresh && sample.refresh_sample_healthy &&
      sample.consecutive_refresh_anomalies == 0,
      "raw video health was overwritten or mixed with input readiness");
  expect(!store.sample(220).fresh, "raw video metadata widened the independent input lease");
}

void test_vehicle_rearm_preserves_fresh_braking_without_restoring_motion() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 0).accepted, "initial neutral rejected");
  auto held = neutral;
  held.intent_seq = 2;
  held.throttle = 0.3;
  held.steering = -0.2;
  held.brake = 0.6;
  expect(store.update(held, 10).accepted, "initial braking intent rejected");
  store.invalidate(true);
  const auto sample = store.sample(11);
  expect(sample.requires_fresh_input && sample.intent.throttle == 0.0 &&
      sample.intent.steering == 0.0 && sample.intent.brake == 0.6,
      "vehicle re-arm released continuously held braking");
  expect(!store.update(held, 12).accepted, "old held motion was restored");
  auto brake_only = held;
  brake_only.intent_seq = 3;
  brake_only.throttle = brake_only.steering = 0.0;
  for (std::int64_t time = 20; time <= 1000; time += 50) {
    const auto update = store.update(brake_only, time);
    expect(update.accepted && update.requires_fresh_input && update.preserve_brake_during_rearm,
        "live brake-only refresh cleared motion interlock or was rejected");
    expect(store.sample(time + 49).intent.brake == 0.6, "ongoing braking faded during re-arm");
  }
  expect_zero_actuation(store.sample(1170).intent, "real lease expiry retained stale ordinary braking");
  const auto expired = store.update(brake_only, 1171);
  expect(!expired.accepted && !expired.preserve_brake_during_rearm,
      "old braking bypassed the expired input interlock");
  neutral.intent_seq = 4;
  expect(store.update(neutral, 1172).accepted, "neutral after brake release did not re-arm");
  held.intent_seq = 5;
  expect(store.update(held, 1173).accepted, "fresh motion did not resume after release/repress");
  store.invalidate();
  expect_zero_actuation(store.sample(1174).intent, "transport failure inherited vehicle-only brake preservation");
}

void test_held_input_refreshes_for_a_minute_but_real_expiry_still_requires_neutral() {
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 0).accepted, "initial neutral intent was rejected");
  auto held = neutral;
  held.intent_seq = 2;
  held.throttle = 0.5;
  held.steering = -1.0;
  std::int64_t last_refresh = 0;
  for (std::int64_t time = 66; time <= 61000; time += 66) {
    expect(store.update(held, time).accepted, "unchanged held intent refresh was rejected");
    const auto sample = store.sample(time + 60);
    expect(sample.fresh && !sample.requires_fresh_input &&
               sample.intent.throttle == 0.5 && sample.intent.steering == -1.0,
           "a continuously refreshed long hold lost actuation");
    last_refresh = time;
  }
  const auto expired = store.sample(last_refresh + 200);
  expect(!expired.fresh && expired.requires_fresh_input,
         "the unchanged 200ms lease failed to expire");
  expect_zero_actuation(expired.intent, "real expiry retained actuation");
  held.intent_seq = 3;
  expect(store.update(held, last_refresh + 201).reason == "fresh_neutral_required",
         "a held key resumed traction after real expiry without neutral re-arm");
  neutral.intent_seq = 4;
  expect(store.update(neutral, last_refresh + 202).accepted,
         "fresh neutral re-arm was rejected");
  held.intent_seq = 5;
  expect(store.update(held, last_refresh + 203).accepted,
         "fresh input after neutral re-arm was rejected");
}

void test_fresh_neutral_interlock() {
  mine_teleop::NativeControlIntentStore store(150);
  auto drive = neutral_intent(1);
  drive.throttle = 0.3;
  auto update = store.update(drive, 10);
  expect(
      !update.accepted && update.requires_fresh_input &&
          update.reason == "fresh_neutral_required",
      "an initial non-neutral intent bypassed the fresh-neutral interlock");

  auto neutral = neutral_intent(2);
  update = store.update(neutral, 11);
  expect(
      update.accepted && !update.requires_fresh_input,
      "a fresh neutral intent did not release the interlock");

  drive.intent_seq = 3;
  update = store.update(drive, 12);
  expect(update.accepted, "fresh drive input was rejected after neutral re-arming");
  const auto sample = store.sample(13);
  expect(
      sample.fresh && !sample.requires_fresh_input && sample.intent.throttle == 0.3,
      "re-armed fresh drive input was not exposed to the native sender");
}

void test_estop_is_sticky() {
  mine_teleop::NativeControlIntentStore store(150);
  auto estop = neutral_intent(1, "R");
  estop.steering = 0.4;
  estop.throttle = 0.8;
  estop.estop = true;
  expect(store.update(estop, 10).accepted, "ESTOP was rejected by the initial interlock");
  const auto latched = store.sample(11);
  expect(latched.intent.estop, "accepted ESTOP was not sampled as active");
  expect_zero_actuation(latched.intent, "ESTOP did not normalize ordinary actuation");

  auto clear_attempt = neutral_intent(2, "R");
  expect(store.update(clear_attempt, 20).accepted, "neutral post-ESTOP intent was rejected");
  expect(store.sample(21).intent.estop, "ordinary neutral input cleared the ESTOP latch");
  const auto duplicate_clear = store.update(clear_attempt, 22);
  expect(
      duplicate_clear.accepted && duplicate_clear.duplicate,
      "sticky ESTOP turned an otherwise identical intent refresh into a sequence conflict");
  auto owner_estop = clear_attempt; owner_estop.intent_seq = 3; owner_estop.estop = true;
  expect(store.update(owner_estop, 23).accepted, "current-page new ESTOP was rejected");
  expect(store.update(clear_attempt, 24).reason == "old_intent_seq",
      "pre-ESTOP observation watermark regressed the current owner's ESTOP sequence");

  auto replacement_drive = neutral_intent(1, "R");
  replacement_drive.ui_instance_id = "replacement-test-ui";
  replacement_drive.throttle = 0.2;
  const auto replacement = store.update(replacement_drive, 25);
  expect(
      !replacement.accepted && !replacement.requires_fresh_input &&
          replacement.reason == "input_page_not_current",
      "replacement UI was not rejected without disturbing the current owner");
  expect(
      store.sample(26).intent.estop,
      "rejected replacement UI input cleared the ESTOP latch");

  store.invalidate();
  const auto invalidated = store.sample(1000);
  expect(
      !invalidated.fresh && invalidated.intent.estop,
      "lease expiry or invalidation cleared the ESTOP latch");
}

template <typename Function>
void expect_throws(Function&& function, std::string_view message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw TestFailure(std::string(message));
}

void test_native_control_ack_progress_replays_session31_and_bounds_backlog() {
  using Window = mine_teleop::detail::NativeControlAcknowledgementWindow;
  Window window;
  // Relative monotonic times from session-000031, without wall-clock sleeps.
  const std::int64_t sent_times[] = {0, 61, 124, 188, 251, 315, 376, 440, 501, 566};
  std::uint64_t sequence = 1807;
  for (const auto sent_time : sent_times) {
    window.note_sent(sequence++, sent_time);
  }
  expect(window.acknowledge_through(1807, 440), "first delayed ACK did not advance");
  expect(window.acknowledge_through(1808, 565), "second delayed ACK did not advance");
  expect(
      window.oldest_sequence() == 1809 && window.pending_count() == 8 &&
          window.oldest_age_ms(628) == 504 && window.stall_age_ms(628) == 63,
      "session31's live ACK pipeline was mistaken for a 500ms connection stall");
  expect(
      !window.acknowledge_through(1808, 700) &&
          !window.acknowledge_through(1807, 900) &&
          window.stall_age_ms(1064) == 499 && window.stall_age_ms(1065) == 500,
      "duplicate or regressing ACKs refreshed the 500ms liveness deadline");
  expect_throws([&] { window.acknowledge_through(1817, 1060); },
                "an ACK for an unsent packet was accepted as progress");
  expect(window.stall_age_ms(1065) == 500, "invalid ACK refreshed liveness");
  window.acknowledge_through(1816, 1100);
  expect(window.stall_age_ms(4900) == 0, "an empty ACK window timed out");
  window.note_sent(1817, 5000);
  expect(window.stall_age_ms(5499) == 499 && window.stall_age_ms(5500) == 500,
         "new traffic inherited an idle ACK deadline or lost the first-ACK timeout");
  window.reset();
  expect(window.stall_age_ms(6000) == 0, "reconnect retained the old progress deadline");
  window.note_sent(1, 0);
  window.note_sent(3, 60);
  expect_throws([&] { window.acknowledge_through(2, 90); },
                "an ACK for a skipped sequence was accepted as progress");
  expect_throws([&] { window.acknowledge_through(0, 90); },
                "a zero-sequence ACK was accepted as progress");
  expect(window.pending_count() == 2 && window.stall_age_ms(500) == 500,
         "an unsent-sequence ACK changed the pending window or its deadline");
  window.reset();
  for (std::size_t seq = 1; seq <= Window::kMaxPendingPackets; ++seq) {
    window.note_sent(seq, static_cast<std::int64_t>(seq));
  }
  expect_throws([&] { window.note_sent(Window::kMaxPendingPackets + 1, 200); },
                "advancing but slow ACKs can grow the pending queue without a bound");
  expect(window.pending_count() == Window::kMaxPendingPackets,
         "the pending-packet bound was exceeded");
}

void test_ack_stall_brakes_without_reconnect_and_requires_recent_stable_progress() {
  using Recovery = mine_teleop::detail::NativeControlAckRecovery;
  Recovery recovery;
  recovery.note_ack_progress(0, 10, 10, 5000);
  expect(!recovery.observe(499, 10, 499, 10, 10, 5000).entered, "ACK wait entered before 500ms");
  auto transition = recovery.observe(500, 10, 500, 10, 10, 5000);
  expect(transition.entered && recovery.waiting() && !transition.reconnect_due && recovery.deadline() == 5500,
      "500ms ACK stall closed a recoverable connection");
  expect(!recovery.observe(1000, 5, 0, 11, 800, 30000).recovered && recovery.deadline() == 6000 &&
      recovery.consecutive_anomalies() == 1 && !recovery.latest_sample_healthy(),
      "late new ACK cleared the wait, or a changed configuration replaced the frozen no-progress budget");
  // Actual new return data extends only the absence deadline. A duplicate or
  // regressing ACK cannot extend it, clear the count or become a healthy sample.
  expect(!recovery.observe(1100, 4, 100, 11, 30, 5000).recovered &&
      !recovery.observe(1101, 4, 101, 10, 30, 5000).recovered && recovery.deadline() == 6000 &&
      recovery.consecutive_anomalies() == 1 && recovery.sample_sequence() == 11,
      "duplicate/regressing ACK recovered or renewed a stalled stream");
  expect(recovery.observe(1200, 3, 0, 12, 30, 5000).recovered && !recovery.waiting() &&
      recovery.consecutive_anomalies() == 0 && recovery.latest_sample_healthy(),
      "fresh new ACK failed to restore its own category for the vehicle's common 300ms/rearm gate");
  expect(!recovery.observe(1300, 3, 0, 13, 201, 5000).entered &&
      !recovery.observe(1400, 3, 0, 14, 201, 5000).entered && recovery.consecutive_anomalies() == 2,
      "one or two distinct late ACKs bypassed the confirmed three-sample tolerance");
  expect(!recovery.observe(1401, 3, 1, 14, 10, 5000).recovered &&
      recovery.consecutive_anomalies() == 2 && !recovery.latest_sample_healthy(),
      "duplicate apparently normal ACK cleared late sample history");
  expect(recovery.observe(1500, 3, 0, 15, 201, 5000).entered && recovery.waiting() &&
      recovery.consecutive_anomalies() == 3, "third distinct late ACK did not withdraw readiness");
  for (std::uint64_t sequence = 16; sequence <= 65; ++sequence) {
    expect(!recovery.observe(static_cast<std::int64_t>(sequence * 100), 3, 0,
        sequence, 800, 30000).reconnect_due && recovery.waiting(),
        "continuing late new ACKs reconnected instead of preserving the braking session");
  }
  expect(!recovery.observe(6501, 3, 0, 66, 800, 30000).reconnect_due && recovery.waiting() &&
      recovery.deadline() == 11501,
      "continuing abnormal ACK traffic forced a new connection merely after the old shared 5s budget");
  expect(recovery.observe(6700, 3, 0, 67, 200, 5000).recovered && !recovery.waiting(),
      "fresh new ACK at the production age boundary did not clear only its own category");
  recovery.enter(20000, 1000, 67);
  expect(!recovery.observe(20999, 10, 999, 67, 30, 30000).reconnect_due && recovery.deadline() == 21000,
      "bounded ACK wait reconnected too early");
  expect(recovery.observe(21000, 10, 1000, 67, 30, 30000).reconnect_due,
      "persistent ACK loss never requested a bounded reconnect");
  recovery.connection_reopened(21100);
  expect(recovery.waiting() && recovery.deadline() == 22100 && !recovery.latest_sample_healthy() &&
      !recovery.observe(21101, 0, 0, 0, 0, 5000).recovered, "new socket silently restored motion without an ACK");
  expect(recovery.observe(21200, 2, 0, 30, 20, 5000).recovered && !recovery.waiting() &&
      recovery.latest_sample_healthy(), "fresh ACK readiness after actual reconnect deadlocked");

  mine_teleop::detail::NativeControlAcknowledgementWindow window;
  window.note_sent(1, 100);
  expect(window.acknowledge_through(1, 850) && window.last_ack_packet_age_ms() == 750,
      "ACK packet age was not tied to its actual send");
  expect(!window.acknowledge_through(1, 900) && window.last_ack_packet_age_ms() == 750,
      "duplicate ACK forged a recent packet");
}

struct TestCase {
  const char* name;
  void (*function)();
};

void test_session016_recorded_ack_drain_preserves_brake_and_fresh_neutral() {
  using namespace mine_teleop::detail;
  NativeControlAcknowledgementWindow window; NativeControlAckRecovery recovery;
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral=neutral_intent(1);expect(store.update(neutral,6504100).accepted,"field neutral failed");
  auto held=neutral;held.intent_seq=2;held.throttle=0.5;
  expect(store.update(held,6504110).accepted,"field held input failed");
  // Session016 browser464 commands[23], native monotonic timestamps. These
  // are advancing sent identities, not repeated samples of the same ACK.
  const std::int64_t ages[]={499,437,374,309,246,183,126,62};
  for(std::uint64_t index=0;index<8;++index) {
    const auto received=6504122+(index>=6?1:0);
    window.note_sent(976+index,received-ages[index]);
  }
  unsigned entered=0,recovered=0;
  for(std::uint64_t index=0;index<8;++index) {
    const auto received=6504122+(index>=6?1:0);
    const auto seq=976+index;
    expect(window.acknowledge_through(seq,received),"recorded ACK did not advance");
    const auto transition=recovery.note_ack_progress(received,seq,window.last_ack_packet_age_ms(),5000);
    if(transition.entered){++entered;expect(seq==978,"field wait entered at wrong ACK");}
    if(transition.recovered){++recovered;expect(seq==981,"field wait recovered at wrong ACK");}
    if(transition.entered||transition.recovered)store.invalidate(true);
  }
  expect(entered==1 && recovered==1 && !recovery.waiting() &&
    recovery.bad_sample_revision()==5 && recovery.brake_sample_revision()==1,
    "same-drain fresh ACK erased distinct late samples or their brake event");
  expect(store.sample(6504123).fresh && store.sample(6504123).intent.throttle==0 &&
    store.sample(6504123).requires_fresh_input,"fresh ACK replayed old motion");
  held.intent_seq=3;expect(store.update(held,6504124).reason=="fresh_neutral_required","old hold resumed");
  neutral.intent_seq=4;expect(store.update(neutral,6504125).accepted,"field neutral rearm failed");
  held.intent_seq=5;expect(store.update(held,6504126).accepted,"field new press failed");
}

void test_dispatch_revalidates_input_after_preparation() {
  using mine_teleop::detail::native_control_dispatch_rejection;
  mine_teleop::NativeControlIntentStore store(200);
  auto neutral = neutral_intent(1);
  expect(store.update(neutral, 3936388760).accepted, "neutral rejected");
  auto motion = neutral; motion.intent_seq = 2; motion.throttle = 0.5;
  expect(store.update(motion, 3936388774).accepted, "motion rejected");
  const auto prepared = store.sample(3936388880);
  expect(prepared.fresh && prepared.age_ms == 106, "field preparation did not begin fresh");
  const auto expired = store.sample(3936389027);
  expect(!native_control_dispatch_rejection(prepared, expired, 147, 200).empty(),
      "session37 ACK delay dispatched input whose 200ms lease had expired");
  expect(expired.requires_fresh_input && expired.intent.throttle == 0,
      "dispatch recheck did not retain the existing expiry interlock");
  expect(native_control_dispatch_rejection(expired, store.sample(3936389030), 3, 200).empty(),
      "current fail-closed neutral heartbeat was blocked");

  store.reset();
  expect(store.update(neutral, 100).accepted && store.update(motion, 110).accepted,
      "fresh motion setup failed");
  const auto fresh = store.sample(120);
  expect(store.update(motion, 160).accepted, "same input renewal rejected");
  expect(native_control_dispatch_rejection(fresh, store.sample(180), 60, 200).empty(),
      "ordinary duplicate input renewal retired a valid frame");
  expect(store.update(motion, 300).accepted, "later renewal rejected");
  expect(native_control_dispatch_rejection(fresh, store.sample(320), 200, 200) ==
      "prepared_input_expired_no_io", "new refresh retimestamped an old prepared frame");
  neutral.intent_seq = 3;
  expect(store.update(neutral, 321).accepted, "release rejected");
  expect(!native_control_dispatch_rejection(fresh, store.sample(322), 60, 200).empty(),
      "released throttle was dispatched from a prior snapshot");

  auto other = neutral_intent(1); other.ui_instance_id = "replacement-page";
  const auto released = store.sample(322);
  expect(store.update(other, 323, released.ui_control_epoch, true).accepted, "takeover failed");
  expect(!native_control_dispatch_rejection(released, store.sample(324), 2, 200).empty(),
      "retired page snapshot survived takeover");
  auto estop = other; estop.intent_seq = 2; estop.estop = true;
  const auto before_stop = store.sample(324);
  expect(store.update(estop, 325).accepted, "ESTOP rejected");
  const auto stopped = store.sample(326);
  expect(!native_control_dispatch_rejection(before_stop, stopped, 2, 200).empty(),
      "ordinary frame was dispatched after ESTOP preemption");
  expect(native_control_dispatch_rejection(stopped, store.sample(1000), 674, 200).empty(),
      "latched ESTOP lost priority due to ordinary input expiry");
}

}  // namespace

int main() {
  const TestCase tests[] = {
      {"dispatch_revalidates_input_after_preparation", test_dispatch_revalidates_input_after_preparation},
      {"session016_recorded_ack_drain",test_session016_recorded_ack_drain_preserves_brake_and_fresh_neutral},
      {"raw_video_sample_is_preserved_separately_from_input_and_qualified_video", test_raw_video_sample_is_preserved_separately_from_input_and_qualified_video},
      {"ingress_anomaly_samples_are_distinct_and_timeout_is_independent", test_ingress_anomaly_samples_are_distinct_and_timeout_is_independent},
      {"confirmed_page_takeover_retires_late_neutral_without_retiring_estop", test_confirmed_page_takeover_retires_late_neutral_without_retiring_estop},
      {"page_takeover_requires_confirmation_and_preserves_current_input_on_rejection", test_page_takeover_requires_confirmation_and_preserves_current_input_on_rejection},
      {"lost_takeover_response_is_retryable_without_epoch_growth_or_motion", test_lost_takeover_response_is_retryable_without_epoch_growth_or_motion},
      {"refresh_sample_ids_distinguish_real_refresh_replay_and_sender_sampling", test_refresh_sample_ids_distinguish_real_refresh_replay_and_sender_sampling},
      {"out_of_order_local_refresh_cannot_rewind_lease_or_restore_released_motion", test_out_of_order_local_refresh_cannot_rewind_lease_or_restore_released_motion},
      {"ack_stall_brakes_without_reconnect_and_requires_recent_stable_progress", test_ack_stall_brakes_without_reconnect_and_requires_recent_stable_progress},
      {"delayed_refresh_cannot_bypass_expiry_between_sender_ticks", test_delayed_refresh_cannot_bypass_expiry_between_sender_ticks},
      {"vehicle_rearm_preserves_fresh_braking_without_restoring_motion",
       test_vehicle_rearm_preserves_fresh_braking_without_restoring_motion},
      {"lease_expiry_neutralizes_and_preserves_gear",
       test_lease_expiry_neutralizes_and_preserves_gear},
      {"invalidate_does_not_restore_old_throttle",
       test_invalidate_does_not_restore_old_throttle},
      {"held_input_refreshes_for_a_minute_but_real_expiry_still_requires_neutral",
       test_held_input_refreshes_for_a_minute_but_real_expiry_still_requires_neutral},
      {"fresh_neutral_interlock", test_fresh_neutral_interlock},
      {"estop_is_sticky", test_estop_is_sticky},
      {"native_control_ack_progress_replays_session31_and_bounds_backlog",
       test_native_control_ack_progress_replays_session31_and_bounds_backlog},
  };

  int failures = 0;
  for (const auto& test : tests) {
    try {
      test.function();
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    }
  }
  if (failures != 0) {
    std::cerr << failures << " portable native control safety test(s) failed\n";
    return 1;
  }
  std::cout << "All portable native control safety tests passed\n";
  return 0;
}
