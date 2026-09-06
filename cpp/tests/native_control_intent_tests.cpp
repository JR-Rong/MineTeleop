#include "mine_teleop/core.hpp"

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

  auto replacement_drive = neutral_intent(1, "R");
  replacement_drive.ui_instance_id = "replacement-test-ui";
  replacement_drive.throttle = 0.2;
  const auto replacement = store.update(replacement_drive, 23);
  expect(
      !replacement.accepted && replacement.requires_fresh_input &&
          replacement.reason == "fresh_neutral_required",
      "sticky ESTOP allowed a replacement UI to bypass the fresh-neutral interlock");
  expect(
      store.sample(24).intent.estop,
      "rejected replacement UI input cleared the ESTOP latch");

  store.invalidate();
  const auto invalidated = store.sample(1000);
  expect(
      !invalidated.fresh && invalidated.intent.estop,
      "lease expiry or invalidation cleared the ESTOP latch");
}

struct TestCase {
  const char* name;
  void (*function)();
};

}  // namespace

int main() {
  const TestCase tests[] = {
      {"lease_expiry_neutralizes_and_preserves_gear",
       test_lease_expiry_neutralizes_and_preserves_gear},
      {"invalidate_does_not_restore_old_throttle",
       test_invalidate_does_not_restore_old_throttle},
      {"fresh_neutral_interlock", test_fresh_neutral_interlock},
      {"estop_is_sticky", test_estop_is_sticky},
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
