#include "mine_teleop/media.hpp"
#include "mine_teleop/detail/native_control_acknowledgement_window.hpp"
#include "mine_teleop/detail/media_freshness.hpp"
#include "control_time_fixture.hpp"

#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using namespace mine_teleop;
using Phase = ControlRecoveryPhase;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
ControlCommand command(std::uint64_t seq, std::int64_t now, bool motion = false) {
  ControlCommand c;
  c.vehicle_id = "vehicle-001"; c.driver_id = "driver-001";
  c.session_id = "session-001"; c.control_token = "token";
  c.seq = seq; c.sent_at_utc_ms = now; c.gear = "D";
  c.throttle = motion ? 0.5 : 0.0;
  return c;
}
SessionControlProfileRequest profile() {
  SessionControlProfileRequest p;
  p.vehicle_id = "vehicle-001"; p.driver_id = "driver-001";
  p.session_id = "session-001"; p.control_token = "token"; p.seq = 1;
  return p;
}
void test_wait_retains_session_beyond_legacy_deadline() {
  SessionControlRecoveryLatch latch;
  expect(!latch.enter_session("session"), "new session inhibited");
  expect(latch.observe_recovery("session", false, 0, 5000) == Phase::BrakingWait, "fault did not brake");
  for (std::int64_t t : {4999, 5000, 30000, 60000}) {
    expect(latch.observe_recovery("session", false, t, 5000, ControlRecoveryCause::InputLease) == Phase::BrakingWait,
        "elapsed readiness wait revoked session");
    expect(!latch.enter_session("session") && latch.braking_wait_for("session"), "rebuild lost braking state");
  }
  expect(!latch.deadline_for("session") && latch.recovery_status_for("session").at("session_retained") == true,
      "obsolete hard deadline was advertised");
  expect(latch.observe_recovery("session", true, 60001, 5000) == Phase::BrakingWait, "one healthy observation released brake");
  expect(latch.observe_recovery("session", true, 60300, 5000) == Phase::BrakingWait, "window ended before 300ms");
  expect(latch.observe_recovery("session", true, 60301, 5000) == Phase::Recovered, "same session did not recover");
}
void test_any_required_failure_resets_shared_healthy_window() {
  SessionControlRecoveryLatch latch;
  expect(!latch.enter_session("session"), "unexpected inhibition");
  expect(latch.observe_recovery("session", false, 0, 5000) == Phase::BrakingWait, "missing wait");
  expect(latch.observe_recovery("session", true, 1, 5000) == Phase::BrakingWait, "missing stable wait");
  expect(latch.observe_recovery("session", false, 299, 5000, ControlRecoveryCause::ControlLink) == Phase::BrakingWait,
      "another category released wait");
  expect(latch.observe_recovery("session", true, 300, 5000) == Phase::BrakingWait, "one sample was release");
  expect(latch.observe_recovery("session", true, 599, 5000) == Phase::BrakingWait, "prior window leaked across failure");
  expect(latch.observe_recovery("session", true, 600, 5000) == Phase::Recovered, "reset window did not end");
  expect(latch.inhibit("session") && latch.observe_recovery("session", true, 10000, 5000) == Phase::Locked,
      "health unlocked explicit safety failure");
}
void test_real_vehicle_brake_heartbeats_release_then_new_press() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<MockVehicleAdapter>();
  test::LogicalTimeControlService service(config, "driver-001", "session-001", "token", std::move(adapter));
  service.start(0);
  expect(service.receive_session_profile(profile(), 0).accepted, "profile rejected");
  expect(service.receive_command(command(1, 0, true), 0).accepted, "initial motion rejected");
  std::uint64_t seq = 2;
  for (std::int64_t now = 50; now <= 6000; now += 50) {
    auto c = command(seq++, now); c.operator_input_ready = false; c.recovery_sample_healthy = false;
    const auto result = service.receive_command(c, now);
    expect(result.accepted && result.command && result.command->brake == 1, "readiness wait lost independent brake/heartbeat");
    expect(service.session_control_profile().at("active") == true, "readiness duration revoked profile");
  }
  // A neutral heartbeat while waiting is neither healthy-window completion
  // nor a post-recovery release event.
  SessionControlRecoveryLatch latch;
  expect(!latch.enter_session("session-001"), "unexpected session inhibition");
  expect(latch.observe_recovery("session-001", false, 6000, 5000) == Phase::BrakingWait, "receive wait was not coordinated");
  for (std::int64_t now = 6050; now <= 6350; now += 50) {
    expect(service.receive_command(command(seq++, now), now).accepted, "healthy neutral heartbeat failed");
    const bool healthy = service.driver_critical_video_ready() && service.operator_input_ready() &&
        service.driver_control_link_ready() && service.driver_time_sync_ready() && service.recovery_samples_healthy();
    const auto phase = latch.observe_recovery("session-001", healthy, now, 5000);
    if (now < 6350) expect(phase == Phase::BrakingWait, "shared healthy window ended early");
    else expect(phase == Phase::Recovered, "shared healthy window did not recover");
    service.set_control_braking_wait(phase == Phase::BrakingWait, now);
  }
  expect(!service.receive_command(command(seq++, 6351, true), 6351).accepted,
      "old held motion resumed immediately on health recovery");
  expect(service.receive_command(command(seq++, 6352), 6352).accepted, "post-recovery release rejected");
  expect(service.receive_command(command(seq++, 6353, true), 6353).accepted, "new press did not resume");
  service.tick(6353 + config.control.control_timeout_ms);
  expect(service.safety_state() == SafetyState::TimeoutBrake && service.session_control_profile().at("active") == false,
      "genuine control absence was suppressed by session retention");
  service.close();
}
void test_raw_bad_sample_is_distinct_from_tolerated_gate() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<MockVehicleAdapter>();
  test::LogicalTimeControlService service(config, "driver-001", "session-001", "token", std::move(adapter));
  service.start(0); expect(service.receive_session_profile(profile(), 0).accepted, "profile rejected");
  auto c = command(1, 0); c.recovery_sample_healthy = false;
  expect(service.receive_command(c, 0).accepted && !service.control_braking_wait(), "single tolerated raw anomaly started brake");
  expect(!service.recovery_samples_healthy(), "raw anomaly was advertised as healthy window evidence");
  c = command(2, 1); expect(service.receive_command(c, 1).accepted && service.recovery_samples_healthy(), "valid normal did not replace raw sample");
  service.close();
}
void test_new_wire_requires_recovery_evidence_estop_remains_compatible() {
  auto wire = command(1, 0).to_json();
  expect(ControlCommand::from_json(wire).recovery_sample_healthy, "new wire did not round trip");
  wire.erase("recovery_sample_healthy");
  bool rejected = false;
  try { static_cast<void>(ControlCommand::from_json(wire)); } catch (const std::exception&) { rejected = true; }
  expect(rejected, "old peer silently stripped recovery window evidence");
  wire["estop"] = true;
  expect(ControlCommand::from_json(wire).estop, "legacy legal ESTOP depended on recovery evidence");
}
void test_raw_sample_wire_false_and_wrong_type_are_not_promoted_to_health() {
  auto wire = command(1, 0).to_json();
  wire["recovery_sample_healthy"] = false;
  expect(!ControlCommand::from_json(wire).recovery_sample_healthy,
      "raw false sample was promoted to healthy by wire parsing");
  for (const auto& invalid : std::vector<Json>{Json(nullptr), Json(0), Json(1), Json("true"), Json::array(), Json::object()}) {
    wire["estop"] = false;
    wire["recovery_sample_healthy"] = invalid;
    bool rejected = false;
    try { static_cast<void>(ControlCommand::from_json(wire)); } catch (const std::exception&) { rejected = true; }
    expect(rejected, "ordinary command accepted wrong-type recovery health evidence");
    wire["estop"] = true;
    expect(ControlCommand::from_json(wire).estop,
        "legal ESTOP was blocked by newer recovery metadata type");
  }
  wire["control_token"] = "";
  bool invalid_estop_rejected = false;
  try { static_cast<void>(ControlCommand::from_json(wire)); } catch (const std::exception&) { invalid_estop_rejected = true; }
  expect(invalid_estop_rejected, "new-schema ESTOP exception removed existing credential validation");
}

// Drive each production producer, deliberately omit the intermediate bad
// snapshot, then feed the same revision fence used by the media enforcer.
struct RecoveryProducers {
  std::int64_t steady_ms{1000};
  SynchronizedClock clock{TimeSyncClockSource{
      [this] { return 1791943484483LL + steady_ms; },
      [this] { return std::chrono::steady_clock::time_point(std::chrono::milliseconds(steady_ms)); }}};
  TimeSyncReadinessGate time_gate;
  NativeControlIntentStore input{200};
  NativeControlIntent neutral;
  detail::NativeControlAckRecovery ack;
  detail::MediaSampleAnomalies video;
  std::uint64_t ack_seq{1}, video_seq{1};
  std::int64_t input_received_ms{1};
  RecoveryProducers() {
    neutral.ui_instance_id = "current-page"; neutral.intent_seq = 1;
    neutral.critical_video_sample_healthy = true;
    expect(input.update(neutral, input_received_ms).accepted, "producer neutral input failed");
    static_cast<void>(ack.note_ack_progress(1, ack_seq, 1, 5000));
    expect(video.observe(video_seq, true), "producer video baseline failed");
    sync(2);
  }
  void sync(int duration_ms) {
    static_cast<void>(clock.synchronize([&](std::int64_t sent) {
      const auto receive = 1791943484483LL + steady_ms + duration_ms / 2;
      steady_ms += duration_ms;
      return HttpResponse{200, Json{{"client_send_ms", sent}, {"server_receive_ms", receive},
          {"server_send_ms", receive}}.dump(), {}};
    }, 3, 25));
  }
  void bad_then_normal(int category, int bad_count) {
    for (int index = 0; index < bad_count; ++index) {
      if (category == 0) {
        input_received_ms += 300;
        expect(!input.update(neutral, input_received_ms, 1, false, input_received_ms + 210).accepted,
            "aged refresh was treated as valid");
      } else if (category == 1) {
        static_cast<void>(ack.note_ack_progress(100 + index, ++ack_seq, 300, 5000));
      } else if (category == 2) {
        expect(video.observe(++video_seq, false), "new bad video was ignored");
      } else sync(80);
    }
    if (category == 0) {
      input_received_ms += 300;
      expect(input.update(neutral, input_received_ms, 1).accepted, "new normal refresh did not clear its category");
    } else if (category == 1) static_cast<void>(ack.note_ack_progress(200, ++ack_seq, 1, 5000));
    else if (category == 2) expect(video.observe(++video_seq, true), "normal video was ignored");
    else sync(2);
  }
  std::vector<ReadinessSampleRevision> revisions() {
    const auto latest_input = input.sample(input_received_ms);
    const auto latest_time = time_gate.observe(clock.status(), 25, steady_ms, 100000);
    expect(latest_input.refresh_sample_healthy && ack.latest_sample_healthy() &&
        video.consecutive() == 0 && latest_time.latest_candidate_healthy,
        "fixture final snapshots were not actually normal");
    return {{"input", latest_input.refresh_bad_sample_revision, latest_input.refresh_brake_revision},
        {"ack", ack.bad_sample_revision(), ack.brake_sample_revision()},
        {"video", video.bad_sample_revision(), video.brake_sample_revision()},
        {"clock", latest_time.bad_cohort_revision, latest_time.brake_cohort_revision}};
  }
};
void test_four_producer_histories_reset_once_without_combining_categories() {
  for (int category = 0; category < 4; ++category) {
    RecoveryProducers producer;
    SessionControlRecoveryLatch latch;
    expect(!latch.enter_session("scope"), "new scope inhibited");
    auto revisions = producer.revisions();
    expect(latch.observe_readiness("scope", false, 0, 5000, ControlRecoveryCause::InputLease, revisions) == Phase::BrakingWait,
        "missing initial wait");
    expect(latch.observe_readiness("scope", true, 1, 5000, ControlRecoveryCause::InputLease, revisions) == Phase::BrakingWait,
        "missing initial healthy window");
    producer.bad_then_normal(category, 1);
    revisions = producer.revisions();
    expect(revisions[category].bad == 1 && revisions[category].brake == 0,
        "single-category anomaly became a third-sample brake");
    expect(latch.observe_readiness("scope", true, 299, 5000, ControlRecoveryCause::InputLease, revisions) == Phase::BrakingWait,
        "coalesced bad-to-normal snapshot released old healthy window");
    expect(latch.observe_readiness("scope", true, 598, 5000, ControlRecoveryCause::InputLease, revisions) == Phase::BrakingWait,
        "reset window ended before 300ms");
    expect(latch.observe_readiness("scope", true, 599, 5000, ControlRecoveryCause::InputLease, revisions) == Phase::Recovered,
        "unchanged revision reset the healthy window twice");
  }
  RecoveryProducers producer;
  ReadinessRevisionFence fence;
  static_cast<void>(fence.observe(producer.revisions()));
  for (int category = 0; category < 4; ++category) producer.bad_then_normal(category, 1);
  expect(!fence.observe(producer.revisions()).brake, "four independent single faults were combined into a third fault");
}
void test_coalesced_third_sample_still_withdraws_and_wire_revisions_are_required() {
  RecoveryProducers producer;
  SessionControlRecoveryLatch latch;
  expect(!latch.enter_session("scope"), "new scope inhibited");
  expect(latch.observe_readiness("scope", true, 0, 5000, ControlRecoveryCause::ControlLink,
      producer.revisions()) == Phase::Ready, "normal baseline was not ready");
  producer.bad_then_normal(1, 3);
  const auto revisions = producer.revisions();
  expect(revisions[1].bad == 3 && revisions[1].brake == 1, "third ACK history was lost");
  expect(latch.observe_readiness("scope", true, 10, 5000, ControlRecoveryCause::ControlLink, revisions) == Phase::BrakingWait,
      "third bad sample disappeared behind its newer normal snapshot");
  expect(latch.observe_readiness("scope", true, 11, 5000, ControlRecoveryCause::ControlLink, revisions) == Phase::BrakingWait,
      "one normal observation released brake");
  expect(latch.observe_readiness("scope", true, 311, 5000, ControlRecoveryCause::ControlLink, revisions) == Phase::Recovered,
      "retained historical third sample blocked normal recovery forever");
  for (const auto* field : {"recovery_bad_sample_revision", "recovery_brake_revision"}) {
    auto wire = command(1, 0).to_json(); wire.erase(field);
    bool rejected = false;
    try { static_cast<void>(ControlCommand::from_json(wire)); } catch (const std::exception&) { rejected = true; }
    expect(rejected, "ordinary wire silently lost revision history");
    wire["estop"] = true;
    expect(ControlCommand::from_json(wire).estop, "legal ESTOP depended on newer revision history");
    for (const auto& invalid : std::vector<Json>{Json(-1), Json(0.5), Json(true), Json("0"), Json(nullptr)}) {
      wire = command(1, 0).to_json(); wire[field] = invalid;
      rejected = false;
      try { static_cast<void>(ControlCommand::from_json(wire)); } catch (const std::exception&) { rejected = true; }
      expect(rejected, "wrong-type revision was accepted as safety evidence");
    }
  }
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  test::LogicalTimeControlService service(config, "driver-001", "session-001", "token", std::make_unique<MockVehicleAdapter>());
  service.start(0); expect(service.receive_session_profile(profile(), 0).accepted, "profile rejected");
  expect(service.receive_command(command(1, 0, true), 0).accepted, "initial motion rejected");
  auto c = command(2, 50); c.recovery_bad_sample_revision = 3; c.recovery_brake_revision = 1;
  const auto result = service.receive_command(c, 50);
  expect(result.accepted && service.control_braking_wait() && result.command && result.command->brake == 1,
      "actual service failed to brake on coalesced third-sample evidence");
  expect(service.session_control_profile().at("active") == true, "historical readiness event revoked profile");
  service.close();
}
}
int main() {
  const std::vector<std::pair<const char*, std::function<void()>>> tests = {
    {"wait_retains_session", test_wait_retains_session_beyond_legacy_deadline},
    {"required_failure_resets_shared_window", test_any_required_failure_resets_shared_healthy_window},
    {"real_vehicle_brake_and_release", test_real_vehicle_brake_heartbeats_release_then_new_press},
    {"raw_sample_vs_gate", test_raw_bad_sample_is_distinct_from_tolerated_gate},
    {"new_wire_and_legacy_estop", test_new_wire_requires_recovery_evidence_estop_remains_compatible},
    {"raw_wire_type_and_false", test_raw_sample_wire_false_and_wrong_type_are_not_promoted_to_health},
    {"four_producer_history_window", test_four_producer_histories_reset_once_without_combining_categories},
    {"coalesced_third_and_required_revisions", test_coalesced_third_sample_still_withdraws_and_wire_revisions_are_required},
  };
  int failures = 0;
  for (const auto& [name, run] : tests) {
    try { run(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
  }
  std::cout << tests.size() << " tests, " << failures << " failures\n";
  return failures ? 1 : 0;
}
