#include "mine_teleop/http.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

void expect(bool value, const std::string& message) {
  if (!value) throw std::runtime_error(message);
}

// Every test drives the complete production SynchronizedClock instance. Only
// its clock readings and exchange transport are substituted; no host clock,
// public listener, credentials, or real endpoint are involved.
struct SimulatedTime {
  static constexpr std::int64_t base_utc_ms = 1791943484483;
  std::int64_t steady_ms{1000};
  std::int64_t wall_step_ms{0};
  int wall_reads{0};
  int pause_on_wall_read{0};
  int pause_ms{0};
  int step_on_wall_read{0};
  int scheduled_step_ms{0};

  Clock::time_point steady() const { return Clock::time_point(std::chrono::milliseconds(steady_ms)); }
  std::int64_t utc() const { return base_utc_ms + steady_ms; }
  mine_teleop::TimeSyncClockSource source() {
    return {[this] {
      ++wall_reads;
      if (wall_reads == step_on_wall_read) wall_step_ms += scheduled_step_ms;
      const auto result = utc() + wall_step_ms;
      if (wall_reads == pause_on_wall_read) steady_ms += pause_ms;
      return result;
    }, [this] { return steady(); }};
  }
  mine_teleop::HttpResponse reply(std::int64_t sent, int exchange_ms = 2, int server_offset_ms = 0,
                                int server_processing_ms = 0, int end_wall_step_ms = 0) {
    const auto receive = utc() + (exchange_ms - server_processing_ms) / 2 + server_offset_ms;
    steady_ms += exchange_ms;
    wall_step_ms += end_wall_step_ms;
    return {200, mine_teleop::Json{{"client_send_ms", sent}, {"server_receive_ms", receive},
        {"server_send_ms", receive + server_processing_ms}}.dump(), {}};
  }
  void clear_read_faults() {
    wall_reads = 0;
    pause_on_wall_read = step_on_wall_read = 0;
    pause_ms = scheduled_step_ms = 0;
  }
};

mine_teleop::TimeSyncStatus establish(SimulatedTime& time, mine_teleop::SynchronizedClock& clock) {
  const auto status = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25);
  expect(status.acceptable(25) && status.anchor_accepted && status.has_accepted_anchor,
      "a symmetric 2ms exchange did not establish a trusted anchor");
  expect(clock.now_ms() == time.utc(), "complete clock produced incorrect UTC for symmetric exchange");
  time.clear_read_faults();
  return status;
}

void expect_failed_cohort_preserved(SimulatedTime& time, mine_teleop::SynchronizedClock& clock,
                                   const mine_teleop::TimeSyncStatus& previous,
                                   const mine_teleop::SynchronizedClock::TimeExchange& exchange,
                                   int sample_count = 3) {
  bool detailed_failure = false;
  try { static_cast<void>(clock.synchronize(exchange, sample_count, 25)); }
  catch (const mine_teleop::TimeSyncSampleError& error) {
    detailed_failure = error.diagnostics().contains("completed_samples") &&
        error.diagnostics().contains("failed_sample_index");
  }
  expect(detailed_failure, "clock fault did not reject the cohort with diagnostics");
  const auto status = clock.status();
  expect(!status.acceptable(25) && !status.anchor_accepted && status.has_accepted_anchor,
      "failed candidate appeared trusted or discarded the accepted anchor");
  expect(status.accepted_anchor_local_ms == previous.accepted_anchor_local_ms,
      "failed cohort replaced the old anchor");
  expect(status.candidate_generation == previous.candidate_generation + 1,
      "one failed cohort did not produce exactly one new observation");
  expect(clock.now_ms() == time.utc(), "failed refresh shifted the retained UTC/steady mapping");
  expect(clock.refresh_due(30000), "failed cohort postponed retry for the normal interval");
}

void stable_mapping_and_capture_conversion() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  establish(time, clock);
  const auto capture_steady = time.steady();
  const auto capture_utc = time.utc();
  const auto legacy = clock.from_local_system_ms(capture_utc);
  time.wall_step_ms = 1000;
  time.steady_ms += 17;
  expect(clock.now_ms() == time.utc(), "accepted clock followed a system wall-clock step");
  expect(clock.from_steady_time(capture_steady) == capture_utc,
      "historical capture steady timestamp was replaced with callback arrival time");
  expect(clock.from_local_system_ms(capture_utc) == legacy,
      "legacy historical wall conversion changed without a new accepted anchor");
}

void between_sample_wall_jump_is_rejected() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto previous = establish(time, clock);
  // First three fast samples are complete before the wall jumps at the fourth
  // send boundary. Old fastest-three selection accepted a +1000ms bad anchor.
  time.step_on_wall_read = 7;
  time.scheduled_step_ms = 1000;
  expect_failed_cohort_preserved(time, clock, previous, [&](auto sent) { return time.reply(sent); }, 5);
  expect(clock.status().samples->size() == 3, "fixture did not inject a step between completed samples");
}

void final_wall_steady_pair_pause_is_rejected() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto previous = establish(time, clock);
  // Three call/receive pairs are six wall reads. Pause after the seventh wall
  // read but before its second steady reading, reproducing the final anchor gap.
  time.pause_on_wall_read = 7;
  time.pause_ms = 300;
  expect_failed_cohort_preserved(time, clock, previous, [&](auto sent) { return time.reply(sent); });
  expect(clock.status().samples->size() == 3, "final-pair fault did not occur after the complete sample group");
}

void repeated_small_wall_steps_do_not_shorten_rtt() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto previous = establish(time, clock);
  expect_failed_cohort_preserved(time, clock, previous,
      [&](auto sent) { return time.reply(sent, 28, 0, 0, -20); });
  expect(clock.status().samples->front().at("round_trip_ms") == 28,
      "backward wall movement falsely narrowed 28ms real exchange to 8ms");
}

void permitted_small_step_is_inside_the_error_budget() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  int sample = 0;
  const auto status = clock.synchronize([&](auto sent) { return time.reply(sent, 20, 0, 0, sample++ == 0 ? -10 : 0); }, 3, 25);
  expect(status.acceptable(25) && status.uncertainty_ms >= 10,
      "permitted wall movement was absent from the reported error budget");
  expect(std::abs(clock.now_ms() - time.utc()) <= status.uncertainty_ms,
      "accepted UTC mapping exceeded its reported uncertainty");
}

void poor_and_failed_cohorts_are_distinct_observations() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto previous = establish(time, clock);
  const auto old_capture = clock.from_steady_time(time.steady());
  const auto old_steady = time.steady();
  const auto poor = clock.synchronize([&](auto sent) { return time.reply(sent, 80, 10000); }, 3, 25);
  expect(!poor.acceptable(25) && !poor.anchor_accepted && poor.has_accepted_anchor,
      "poor cohort replaced accepted anchor or bypassed the 25ms budget");
  expect(poor.candidate_generation == previous.candidate_generation + 1,
      "completed poor cohort did not advance one generation");
  expect(clock.now_ms() == time.utc() && clock.from_steady_time(old_steady) == old_capture,
      "poor network estimate moved command/capture UTC");
  time.steady_ms += 100;
  const auto age = clock.status().accepted_anchor_age_ms;
  for (int count = 0; count < 100; ++count) static_cast<void>(clock.status());
  clock.mark_refresh_failed();
  expect(clock.status().candidate_generation == poor.candidate_generation,
      "polling/duplicate failure notification was counted as a new cohort");
  expect(clock.status().accepted_anchor_age_ms == age && age >= 340,
      "old anchor age did not advance independently of polling");
  try {
    static_cast<void>(clock.synchronize([&](auto) -> mine_teleop::HttpResponse {
      time.steady_ms += 500;
      throw mine_teleop::HttpTransportError("injected exchange timeout", {{"total_ms", 500}});
    }, 3, 25));
    throw std::runtime_error("injected transport failure was accepted");
  } catch (const mine_teleop::TimeSyncSampleError& error) {
    expect(error.diagnostics().at("http_failure").at("total_ms") == 500,
        "failed exchange lost phase evidence");
  }
  expect(clock.status().candidate_generation == poor.candidate_generation + 1 && clock.now_ms() == time.utc(),
      "transport failure lost the mapping or did not advance one cohort generation");
  const auto recovered = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25);
  expect(recovered.acceptable(25) && recovered.anchor_accepted && recovered.accepted_anchor_age_ms == 0,
      "new normal cohort did not restore trust and reset accepted-anchor age");
}

void initial_58ms_cohort_is_rejected_with_safe_complete_diagnostics() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto rejected = clock.synchronize([&](auto sent) {
    auto response = time.reply(sent, 108);
    auto body = mine_teleop::Json::parse(response.body);
    body["device_token"] = "diagnostic-secret-canary";
    body["timestamp_scope"] = "Bearer diagnostic-secret-canary";
    body["server_api_gate_wait_ms"] = {{"password", "diagnostic-secret-canary"}};
    body["server_before_handler_ms"] = 2;
    body["server_parse_ms"] = 1;
    body["server_processing_monotonic_ms"] = 0;
    response.body = body.dump();
    response.timing = {{"dns_ms", 1.0}, {"connect_ms", 2.0}, {"tls_ms", 3.0},
        {"first_byte_ms", 107.0}, {"total_ms", 108.0}, {"requests_sent", 1}};
    return response;
  }, 7, 25);
  expect(rejected.uncertainty_ms == 58 && !rejected.acceptable(25) &&
      !rejected.anchor_accepted && !rejected.has_accepted_anchor &&
      rejected.candidate_classification_max_uncertainty_ms == 25,
      "initial 58ms cohort established an anchor under the production 25ms policy");
  const auto diagnostic = rejected.to_json(true);
  expect(diagnostic.at("samples").size() == 7, "rejected startup cohort lost samples");
  expect(diagnostic.dump().find("diagnostic-secret-canary") == std::string::npos,
      "remote response text or objects leaked credentials into timing diagnostics");
  for (const auto& sample : diagnostic.at("samples")) {
    expect(sample.at("round_trip_ms") == 108 && sample.at("valid") == true &&
        sample.at("server_timestamp_scope") == "unrecognized" &&
        sample.at("server_api_gate_wait_ms").is_null() &&
        sample.at("server_before_handler_ms") == 2 && sample.at("server_parse_ms") == 1 &&
        sample.at("server_processing_monotonic_ms") == 0 &&
        sample.at("http").at("total_ms") == 108.0 &&
        sample.at("http").contains("dns_ms") && sample.at("http").contains("connect_ms") &&
        sample.at("http").contains("tls_ms") && sample.at("http").contains("first_byte_ms"),
        "safe sample metadata or existing HTTP phase timings were discarded");
  }
  const auto accepted = clock.synchronize([&](auto sent) { return time.reply(sent); }, 7, 25);
  expect(accepted.acceptable(25) && accepted.anchor_accepted && accepted.has_accepted_anchor &&
      accepted.to_json(true).at("samples").size() == 7,
      "normal initial samples did not establish a trusted anchor after a rejected cohort");
}

void server_processing_and_median_uncertainty_are_bounded() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  int sample = 0;
  const auto status = clock.synchronize([&](auto sent) {
    const int index = sample++;
    return time.reply(sent, index == 0 ? 20 : index == 1 ? 200 : 250, index == 0 ? 0 : 500);
  }, 3, 25);
  expect(status.offset_ms == 500 && status.uncertainty_ms >= 510 && !status.anchor_accepted,
      "median estimate falsely narrowed the best sample's uncertainty interval");
  SimulatedTime other;
  mine_teleop::SynchronizedClock invalid(other.source());
  bool rejected = false;
  try {
    static_cast<void>(invalid.synchronize([&](auto sent) { return other.reply(sent, 2, 0, 500); }, 3, 25));
  } catch (const mine_teleop::TimeSyncSampleError&) { rejected = true; }
  expect(rejected && !invalid.status().has_accepted_anchor,
      "impossible server duration fabricated a trusted initial anchor");
}

void paired_wire_boundaries_exclude_client_queue() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto status = clock.synchronize([&](auto sent) {
    time.steady_ms += 150;  // client mutex / pre-wire queue
    const auto wire_sent = time.steady_ms;
    const auto wire_sent_wall = time.utc();
    auto response = time.reply(sent);
    const auto wire_received = time.steady_ms;
    const auto wire_received_wall = time.utc();
    time.steady_ms += 80;  // post-body processing, outside wire scope
    response.timing = {{"request_sent_at_utc_ms", wire_sent_wall}, {"body_received_at_utc_ms", wire_received_wall},
        {"request_sent_at_monotonic_ns", wire_sent * 1000000}, {"body_received_at_monotonic_ns", wire_received * 1000000},
        {"request_timestamp_uncertainty_ms", 1}, {"body_timestamp_uncertainty_ms", 1}};
    return response;
  }, 3, 25);
  expect(status.acceptable(25) && status.round_trip_ms == 2 && clock.now_ms() == time.utc(),
      "pre-wire queue or post-body processing contaminated the UTC estimate");
  expect(status.samples->front().at("timestamp_scope") == "http_header_out_to_body_received",
      "paired wire test fell back to call timing");
}

void send_receive_pair_uncertainty_cannot_be_hidden() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto previous = establish(time, clock);
  time.pause_on_wall_read = 1;
  time.pause_ms = 300;
  expect_failed_cohort_preserved(time, clock, previous, [&](auto sent) { return time.reply(sent); });
}

void slower_samples_cannot_hide_a_server_utc_step() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto previous = establish(time, clock);
  int sample = 0;
  const auto status = clock.synchronize([&](auto sent) {
    const auto index = sample++;
    return time.reply(sent, index < 3 ? 2 : 20, index < 3 ? 0 : 1000);
  }, 5, 25);
  expect(!status.synchronized && !status.anchor_accepted && status.has_accepted_anchor,
      "unselected slower samples concealed a changed server UTC domain");
  expect(status.accepted_anchor_local_ms == previous.accepted_anchor_local_ms && clock.now_ms() == time.utc(),
      "inconsistent server cohort moved the old accepted mapping");
  expect(!status.samples->front().at("cohort_utc_intervals_consistent").get<bool>(),
      "cohort disagreement was absent from diagnostics");
}

void readiness_counts_new_cohorts_and_requires_new_normal_data() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto good = establish(time, clock);
  mine_teleop::TimeSyncReadinessGate gate;
  const auto budget = gate.candidate_timeout_ms(30000, 3);
  expect(budget == 32000, "absence deadline changed its documented scheduling source");
  auto ready = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(ready.ready && ready.latest_candidate_healthy && ready.consecutive_anomalies == 0,
      "new trusted cohort did not establish readiness");
  for (int anomaly = 1; anomaly <= 3; ++anomaly) {
    static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent, 80); }, 3, 25));
    ready = gate.observe(clock.status(), 25, time.steady_ms, budget);
    expect(ready.consecutive_anomalies == anomaly && ready.ready == (anomaly < 3) && !ready.latest_candidate_healthy,
        "time readiness did not apply exactly three new anomalous cohorts");
    for (int poll = 0; poll < 100; ++poll) ready = gate.observe(clock.status(), 25, time.steady_ms, budget);
    expect(ready.consecutive_anomalies == anomaly, "repeated status polls counted as new time samples");
    ready = gate.observe(good, 25, time.steady_ms, budget);
    expect(ready.consecutive_anomalies == anomaly && !ready.latest_candidate_healthy,
        "an old normal cohort cleared a new fault counter");
  }
  static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25));
  ready = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(ready.ready && ready.latest_candidate_healthy && ready.consecutive_anomalies == 0,
      "a new valid normal cohort did not clear only the time-sync counter");
  // This class reports a readiness condition. Motion permission still belongs
  // to the common healthy window and release/repress state machine.
}

void readiness_absence_expiry_is_independent_of_polling_and_failure_count() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  mine_teleop::TimeSyncReadinessGate gate;
  const auto budget = gate.candidate_timeout_ms(30000, 3);
  expect(!gate.observe(clock.status(), 25, time.steady_ms, budget).ready,
      "clock without any accepted anchor became ready");
  establish(time, clock);
  expect(gate.observe(clock.status(), 25, time.steady_ms, budget).ready, "trusted anchor was not ready");
  time.steady_ms += budget + 1;
  auto expired = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(!expired.ready && expired.candidate_timed_out && expired.anchor_timed_out && expired.consecutive_anomalies == 0,
      "silence expiry depended on accumulating three artificial polling anomalies");
  // A fresh bad group is genuine data, but cannot renew an expired accepted
  // anchor or claim that the latest candidate is healthy.
  static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent, 80); }, 3, 25));
  expired = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(!expired.ready && !expired.candidate_timed_out && expired.anchor_timed_out && expired.consecutive_anomalies == 1,
      "fresh failed cohort renewed the old expired UTC mapping");
  static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25));
  expect(gate.observe(clock.status(), 25, time.steady_ms, budget).ready,
      "valid refresh could not recover an expired anchor");
}

void readiness_expired_normal_does_not_count_or_clear_anomalies() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  mine_teleop::TimeSyncReadinessGate gate;
  const auto budget = gate.candidate_timeout_ms(30000, 3);
  establish(time, clock);
  static_cast<void>(gate.observe(clock.status(), 25, time.steady_ms, budget));
  static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent, 80); }, 3, 25));
  const auto consumed_bad = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(consumed_bad.consecutive_anomalies == 1, "fixture did not produce one genuine bad cohort");
  const auto normal = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25);
  expect(normal.candidate_consecutive_anomalies == 0 && normal.bad_cohort_revision == consumed_bad.bad_cohort_revision,
      "fixture did not produce a normal group without a new bad event");
  // Do not forge timestamp metadata: the real producer finishes normally, but
  // the consumer misses it and only reads after its independent absence budget.
  time.steady_ms += budget + 1;
  auto current = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 1 && !current.latest_candidate_healthy &&
      !current.ready && current.candidate_timed_out && current.anchor_timed_out &&
      current.bad_cohort_revision == consumed_bad.bad_cohort_revision,
      "late normal snapshot manufactured an anomaly or cleared the counter instead of independent expiry");
  for (int poll = 0; poll < 100; ++poll) current = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 1, "repeated expired reads counted as new calibration samples");
  // Replaying this generation with repaired metadata is still a duplicate,
  // not evidence that a new exchange succeeded after the deadline.
  auto repaired = clock.status();
  repaired.candidate_observed_steady_ms = time.steady_ms;
  repaired.accepted_anchor_age_ms = 0;
  current = gate.observe(repaired, 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 1 && !current.latest_candidate_healthy && !current.ready,
      "same-generation repaired metadata was promoted to a new normal sample");
  static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25));
  current = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 0 && current.latest_candidate_healthy && current.ready,
      "new timely normal data could not clear the anomaly counter after absence");
}

void readiness_reordered_normal_does_not_count_or_clear_anomalies() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  mine_teleop::TimeSyncReadinessGate gate;
  const auto budget = gate.candidate_timeout_ms(30000, 3);
  const auto earlier_normal = establish(time, clock);
  static_cast<void>(gate.observe(earlier_normal, 25, time.steady_ms, budget));
  static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent, 80); }, 3, 25));
  const auto actual_bad = clock.status();
  auto current = gate.observe(actual_bad, 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 1, "fixture did not produce one genuine bad cohort");
  current = gate.observe(earlier_normal, 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 1 && !current.latest_candidate_healthy &&
      current.candidate_generation == actual_bad.candidate_generation &&
      current.bad_cohort_revision == actual_bad.bad_cohort_revision,
      "reordered old normal snapshot counted a new anomaly, cleared the counter, or reverted event history");
  const auto timely_normal = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25);
  // Boundary robustness: a future or reordered timestamp for a newer identity
  // is incompatible data, not a producer-classified bad exchange. A production
  // steady source cannot generate this metadata; keep that limit explicit.
  auto incompatible = timely_normal;
  incompatible.candidate_observed_steady_ms = time.steady_ms + 1;
  current = gate.observe(incompatible, 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 1 && !current.latest_candidate_healthy &&
      current.bad_cohort_revision == actual_bad.bad_cohort_revision,
      "incompatible new metadata manufactured a producer anomaly or cleared the counter");
  current = gate.observe(timely_normal, 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 1 && !current.latest_candidate_healthy,
      "same-generation repaired snapshot cleared the counter");
  static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25));
  current = gate.observe(clock.status(), 25, time.steady_ms, budget);
  expect(current.consecutive_anomalies == 0 && current.latest_candidate_healthy,
      "a genuinely new timely normal group did not clear the counter");
}

void readiness_rejects_an_anchor_accepted_under_a_larger_budget() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  mine_teleop::TimeSyncReadinessGate gate;
  const auto status = clock.synchronize([&](auto sent) { return time.reply(sent, 80); }, 3);
  expect(status.anchor_accepted && status.accepted_anchor_uncertainty_ms > 25,
      "fixture did not establish a CLI-style unbounded anchor");
  const auto readiness = gate.observe(status, 25, time.steady_ms, gate.candidate_timeout_ms(30000, 3));
  expect(!readiness.ready && !readiness.has_trusted_anchor,
      "anchor accepted with a larger budget bypassed the production 25ms gate");
}

void producer_bad_history_survives_a_later_normal_and_multiple_consumers() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto initial = establish(time, clock);
  const auto budget = mine_teleop::TimeSyncReadinessGate::candidate_timeout_ms(30000, 3);
  mine_teleop::TimeSyncReadinessGate delayed, every_group, other_consumer;
  static_cast<void>(delayed.observe(initial, 25, time.steady_ms, budget));
  static_cast<void>(every_group.observe(initial, 25, time.steady_ms, budget));
  static_cast<void>(other_consumer.observe(initial, 25, time.steady_ms, budget));
  int sample = 0;
  const auto bad = clock.synchronize([&](auto sent) { return time.reply(sent, 2, sample++ == 0 ? 0 : 100); }, 3, 25);
  expect(!bad.acceptable(25) && bad.candidate_consecutive_anomalies == 1 &&
      bad.bad_cohort_revision == 1 && bad.brake_cohort_revision == 0,
      "producer did not classify exactly one true bad cohort");
  static_cast<void>(every_group.observe(bad, 25, time.steady_ms, budget));
  const auto normal = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25);
  expect(normal.candidate_consecutive_anomalies == 0 && normal.bad_cohort_revision == 1 &&
      normal.brake_cohort_revision == 0,
      "normal cleared persistent bad history or did not clear current anomalies");
  const auto delayed_state = delayed.observe(normal, 25, time.steady_ms, budget);
  const auto regular_state = every_group.observe(normal, 25, time.steady_ms, budget);
  const auto independent_state = other_consumer.observe(clock.status(), 25, time.steady_ms, budget);
  expect(delayed_state.ready && delayed_state.latest_candidate_healthy && delayed_state.consecutive_anomalies == 0 &&
      delayed_state.bad_cohort_revision == 1 && regular_state.bad_cohort_revision == 1 &&
      independent_state.bad_cohort_revision == 1,
      "delayed/independent consumers lost the bad revision behind a normal snapshot");
  for (int poll = 0; poll < 100; ++poll) {
    const auto repeated = delayed.observe(clock.status(), 25, time.steady_ms, budget);
    expect(repeated.bad_cohort_revision == 1 && repeated.brake_cohort_revision == 0 && repeated.consecutive_anomalies == 0,
        "polling consumed, cleared, or duplicated producer event history");
  }
  const auto old = delayed.observe(initial, 25, time.steady_ms, budget);
  expect(old.bad_cohort_revision == 1, "out-of-order normal reverted the persistent bad revision");
}

void producer_third_anomaly_history_survives_normal_and_rearms_after_normal() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  const auto initial = establish(time, clock);
  mine_teleop::TimeSyncReadinessGate delayed;
  const auto budget = mine_teleop::TimeSyncReadinessGate::candidate_timeout_ms(30000, 3);
  static_cast<void>(delayed.observe(initial, 25, time.steady_ms, budget));
  for (int anomaly = 1; anomaly <= 4; ++anomaly) {
    if (anomaly == 3) {
      try {
        static_cast<void>(clock.synchronize([&](auto) -> mine_teleop::HttpResponse {
          throw mine_teleop::HttpTransportError("injected cohort transport failure");
        }, 3, 25));
        throw std::runtime_error("transport failure was accepted");
      } catch (const mine_teleop::TimeSyncSampleError&) {}
    } else {
      int sample = 0;
      static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent, 2, sample++ == 0 ? 0 : 100); }, 3, 25));
    }
    const auto current = clock.status();
    expect(current.candidate_consecutive_anomalies == std::min(3, anomaly) &&
        current.bad_cohort_revision == static_cast<std::uint64_t>(anomaly) &&
        current.brake_cohort_revision == static_cast<std::uint64_t>(anomaly >= 3 ? 1 : 0),
        "producer failed to retain exactly one third-anomaly threshold crossing");
    clock.mark_refresh_failed();
    clock.mark_refresh_failed();
    expect(clock.status().candidate_generation == current.candidate_generation &&
        clock.status().bad_cohort_revision == current.bad_cohort_revision &&
        clock.status().brake_cohort_revision == current.brake_cohort_revision,
        "duplicate failure notifications generated fresh cohort events");
  }
  const auto normal = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25);
  const auto latest_only = delayed.observe(normal, 25, time.steady_ms, budget);
  expect(normal.candidate_consecutive_anomalies == 0 && normal.bad_cohort_revision == 4 && normal.brake_cohort_revision == 1 &&
      latest_only.ready && latest_only.latest_candidate_healthy && latest_only.consecutive_anomalies == 0 &&
      latest_only.bad_cohort_revision == 4 && latest_only.brake_cohort_revision == 1,
      "latest normal lost the producer's past third-anomaly threshold");
  // A normal clears only the present run. Three new bad cohorts can cross a
  // new threshold while the previous brake revision remains inspectable.
  for (int anomaly = 1; anomaly <= 3; ++anomaly) {
    int sample = 0;
    static_cast<void>(clock.synchronize([&](auto sent) { return time.reply(sent, 2, sample++ == 0 ? 0 : 100); }, 3, 25));
  }
  const auto second_run = delayed.observe(clock.status(), 25, time.steady_ms, budget);
  expect(second_run.consecutive_anomalies == 3 && !second_run.ready && second_run.bad_cohort_revision == 7 &&
      second_run.brake_cohort_revision == 2,
      "normal did not permit a new independent third-anomaly threshold crossing");
}

void gate_requires_matching_producer_classification_budget() {
  SimulatedTime time;
  mine_teleop::SynchronizedClock clock(time.source());
  mine_teleop::TimeSyncReadinessGate gate;
  const auto budget = mine_teleop::TimeSyncReadinessGate::candidate_timeout_ms(30000, 3);
  const auto unbounded = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3);
  expect(unbounded.anchor_accepted && unbounded.uncertainty_ms <= 25 &&
      unbounded.candidate_classification_max_uncertainty_ms == -1,
      "fixture did not produce a precise but unbounded-policy cohort");
  const auto mismatch = gate.observe(unbounded, 25, time.steady_ms, budget);
  expect(!mismatch.classification_policy_matches && !mismatch.ready && mismatch.consecutive_anomalies == 0 &&
      mismatch.bad_cohort_revision == 0,
      "different policy was silently reused or manufactured an anomaly absent from the producer");
  const auto normal = clock.synchronize([&](auto sent) { return time.reply(sent); }, 3, 25);
  const auto matching = gate.observe(normal, 25, time.steady_ms, budget);
  expect(matching.classification_policy_matches && matching.ready && matching.consecutive_anomalies == 0,
      "a new matching-budget cohort did not restore the classification contract");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"stable_mapping_and_capture_conversion", stable_mapping_and_capture_conversion},
      {"between_sample_wall_jump_is_rejected", between_sample_wall_jump_is_rejected},
      {"final_wall_steady_pair_pause_is_rejected", final_wall_steady_pair_pause_is_rejected},
      {"repeated_small_wall_steps_do_not_shorten_rtt", repeated_small_wall_steps_do_not_shorten_rtt},
      {"permitted_small_step_is_inside_the_error_budget", permitted_small_step_is_inside_the_error_budget},
      {"poor_and_failed_cohorts_are_distinct_observations", poor_and_failed_cohorts_are_distinct_observations},
      {"initial_58ms_cohort_is_rejected_with_safe_complete_diagnostics", initial_58ms_cohort_is_rejected_with_safe_complete_diagnostics},
      {"server_processing_and_median_uncertainty_are_bounded", server_processing_and_median_uncertainty_are_bounded},
      {"paired_wire_boundaries_exclude_client_queue", paired_wire_boundaries_exclude_client_queue},
      {"send_receive_pair_uncertainty_cannot_be_hidden", send_receive_pair_uncertainty_cannot_be_hidden},
      {"slower_samples_cannot_hide_a_server_utc_step", slower_samples_cannot_hide_a_server_utc_step},
      {"readiness_counts_new_cohorts_and_requires_new_normal_data", readiness_counts_new_cohorts_and_requires_new_normal_data},
      {"readiness_absence_expiry_is_independent_of_polling_and_failure_count", readiness_absence_expiry_is_independent_of_polling_and_failure_count},
      {"readiness_expired_normal_does_not_count_or_clear_anomalies", readiness_expired_normal_does_not_count_or_clear_anomalies},
      {"readiness_reordered_normal_does_not_count_or_clear_anomalies", readiness_reordered_normal_does_not_count_or_clear_anomalies},
      {"readiness_rejects_an_anchor_accepted_under_a_larger_budget", readiness_rejects_an_anchor_accepted_under_a_larger_budget},
      {"producer_bad_history_survives_a_later_normal_and_multiple_consumers", producer_bad_history_survives_a_later_normal_and_multiple_consumers},
      {"producer_third_anomaly_history_survives_normal_and_rearms_after_normal", producer_third_anomaly_history_survives_normal_and_rearms_after_normal},
      {"gate_requires_matching_producer_classification_budget", gate_requires_matching_producer_classification_budget}};
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try { test(); std::cout << "PASS " << name << '\n'; }
    catch (const std::exception& error) { ++failures; std::cerr << "FAIL " << name << ": " << error.what() << '\n'; }
  }
  std::cout << tests.size() << " tests, " << failures << " failures\n";
  return failures == 0 ? 0 : 1;
}
