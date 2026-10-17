#include "mine_teleop/detail/lowest_latency_path_policy.hpp"
#include "mine_teleop/detail/vehicle_result_latency_window.hpp"

#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using namespace mine_teleop::detail;
using Policy = LowestLatencyPathPolicy;
using Metric = PathMeasurement;
using Path = NetworkPath;
void expect(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

void exact_vehicle_results() {
  VehicleResultLatencyWindow window;
  expect(!window.snapshot().last, "unmeasured result must be null");
  window.note_sent(3564, 80, 1000);
  window.note_sent(3565, 81, 1050);
  // Independent cloud ingress bookkeeping can already have retired both.
  auto result = window.note_result(3564, 80, 4702, false);
  expect(result && result->round_trip_ms == 3702 && !result->accepted, "rejected result lost its actual round trip");
  result = window.note_result(3565, 81, 4711, true);
  expect(result && result->round_trip_ms == 3661 && result->accepted, "cloud ACK age replaced end-to-end age");
  expect(!window.note_result(3565, 81, 4800, true), "duplicate renewed result progress");
  expect(!window.note_result(3564, 80, 4900, true), "old result renewed progress");
  expect(window.snapshot().last->received_monotonic_ms == 4711 && window.snapshot().samples == 2,
      "duplicate changed last sample");
  window.note_sent(3566,82,4800);
  expect(window.note_result(3566,82,5000,false).has_value(),"new rejection missing");
  const auto state=window.snapshot();
  expect(state.last->command_seq==3566 && !state.last->accepted && state.last_accepted &&
      state.last_accepted->command_seq==3565 && state.last_accepted->received_monotonic_ms==4711,
      "fresh rejection pretended that last software acceptance was also fresh");
  window.reset(); expect(!window.snapshot().last_accepted,"new session retained accepted result");
}

void bounded_unknown_results() {
  VehicleResultLatencyWindow window;
  for (std::uint64_t seq = 1; seq <= 1000; ++seq) window.note_sent(seq * 2, seq, seq);
  auto state = window.snapshot();
  expect(state.retained_sends == 128 && state.evicted_sends == 872, "diagnostic storage is not bounded");
  expect(!window.note_result(2, 1, 1001, true), "evicted identity acquired invented timing");
  expect(!window.note_result(1999, 1000, 1001, true), "unsent hole acquired timing");
  expect(!window.note_result(2002, 1001, 1001, true), "unsent future acquired timing");
  expect(!window.note_result(2000, 999, 1001, true), "mismatched intent acquired timing");
  expect(!window.note_result(2000, 1000, 999, true), "backward clock became a healthy zero");
  expect(window.snapshot().uncorrelated_results == 4 && window.snapshot().invalid_timing == 1,
      "unknown results were not counted separately");
  expect(window.note_result(2000, 1000, 1010, true)->round_trip_ms == 10, "valid latest result was lost");
  window.reset();
  expect(!window.snapshot().last && window.snapshot().retained_sends == 0, "session reset retained old timing");
  window.note_sent(1, 1, 0);
  expect(window.note_result(1, 1, 0, true)->round_trip_ms == 0, "legitimate zero round trip lost");
}

Policy policy() {
  // TEST VALUES ONLY: no deployed product or safety threshold uses these.
  return Policy({Metric::ControlVehicleResultRoundTrip, 200, 3, 5, 0.10, 100, 300});
}
Policy::Observation observation(Path path, double latency, std::int64_t now) {
  return {path, 7, Metric::ControlVehicleResultRoundTrip, true, true, 1, 3, now, latency};
}

void lowest_healthy_path_not_topology_preference() {
  auto selector = policy(); selector.reset(7);
  std::array paths{observation(Path::P2p, 70, 1000), observation(Path::TurnUdp, 40, 1000),
      observation(Path::TurnTcp, 55, 1000), observation(Path::CloudWss, 20, 1000)};
  expect(selector.recommend(1000, paths).path == Path::CloudWss, "direct topology overrode lower measured latency");
  paths[3].healthy = false;
  expect(selector.recommend(1000, paths).path == Path::TurnUdp, "unhealthy fastest path was selected");
  paths[1].selectable = false;
  expect(selector.recommend(1000, paths).path == Path::TurnTcp, "unselectable ICE pair was selected");
}

void incomparable_and_stale_samples_cannot_select() {
  auto selector = policy(); selector.reset(7);
  std::array paths{observation(Path::P2p, 1, 1000), observation(Path::CloudWss, 30, 1000)};
  paths[0].measurement = Metric::IceRoundTrip;
  expect(selector.recommend(1000, paths).path == Path::CloudWss, "media RTT was used as control latency");
  paths[1].measurement = Metric::CloudIngressRoundTrip;
  expect(!selector.recommend(1000, paths).path, "cloud ingress ACK was used as vehicle result");
  paths[0] = observation(Path::P2p, 1, 799);
  paths[1] = observation(Path::CloudWss, 30, 1001);
  expect(!selector.recommend(1000, paths).path, "stale or future sample selected");
  paths[0] = observation(Path::P2p, 1, 1000); paths[0].session_generation = 6;
  paths[1] = observation(Path::CloudWss, 30, 1000); paths[1].samples = 2;
  expect(!selector.recommend(1000, paths).path, "old generation or undersampled path selected");
  paths[0].session_generation = 7; paths[0].latency_ms = std::numeric_limits<double>::quiet_NaN();
  expect(!selector.recommend(1000, paths).path, "nonfinite latency selected");
}

void switch_hysteresis_requires_new_progress_and_confirmation() {
  auto selector = policy(); selector.reset(7); selector.confirm_selected(Path::CloudWss, 7, 1, 1000);
  std::array paths{observation(Path::CloudWss, 50, 1050), observation(Path::P2p, 20, 1050)};
  expect(selector.recommend(1050, paths).reason == Policy::Reason::Stabilizing, "switch ignored debounce");
  for (auto& value : paths) value.progress_monotonic_ms = 1150;
  expect(selector.recommend(1150, paths).path == Path::CloudWss, "switch ignored minimum dwell");
  for (auto& value : paths) value.progress_monotonic_ms = 1300;
  expect(selector.recommend(1300, paths).path == Path::P2p, "stable faster route was never proposed");
  // A recommendation alone cannot mark the route active or start its dwell.
  paths[1].healthy = false;
  expect(selector.recommend(1301, paths).path == Path::CloudWss, "recommendation changed actual selection");
  selector.confirm_selected(Path::P2p, 7, 1, 1302);
  expect(selector.recommend(1303, paths).reason == Policy::Reason::UnhealthyFallback,
      "hard failure waited for dwell/debounce");
  selector.reset(8);
  expect(!selector.recommend(1304, paths).path, "new session accepted prior route observations");
}

void repeated_snapshot_and_small_improvement_do_not_flap() {
  auto selector = policy(); selector.reset(7); selector.confirm_selected(Path::CloudWss, 7, 1, 0);
  std::array paths{observation(Path::CloudWss, 50, 1000), observation(Path::P2p, 48, 1000)};
  expect(selector.recommend(1000, paths).reason == Policy::Reason::KeepCurrent, "small improvement flapped route");
  paths[1].latency_ms = 20;
  selector.recommend(1000, paths);
  expect(selector.recommend(1150, paths).reason == Policy::Reason::Stabilizing, "same sample counted as new progress");
  paths[1].progress_monotonic_ms = 1151; paths[0].progress_monotonic_ms = 1151;
  expect(selector.recommend(1151, paths).reason == Policy::Reason::BetterPath, "new progress did not permit proposal");
  expect(selector.recommend(1150, paths).reason == Policy::Reason::InvalidSnapshot, "clock regression ignored");
  paths[1] = paths[0];
  expect(selector.recommend(1152, paths).reason == Policy::Reason::InvalidSnapshot, "duplicate route snapshot ambiguous");
}

void attempt_and_session_confirmation_are_fenced() {
  auto selector = policy(); selector.reset(7); selector.confirm_selected(Path::CloudWss, 7, 1, 0);
  std::array paths{observation(Path::CloudWss, 50, 1000), observation(Path::P2p, 20, 1000)};
  selector.recommend(1000, paths);
  for (auto& value : paths) value.progress_monotonic_ms = 1100;
  paths[1].transport_attempt = 2;
  expect(selector.recommend(1100, paths).reason == Policy::Reason::Stabilizing,
      "replacement attempt inherited prior stability");
  for (auto& value : paths) value.progress_monotonic_ms = 1200;
  expect(selector.recommend(1200, paths).reason == Policy::Reason::BetterPath, "new attempt never stabilized");
  selector.reset(8);
  expect(!selector.confirm_selected(Path::P2p, 7, 2, 1200), "old session confirmation selected a path");
  expect(!selector.confirm_selected(Path::P2p, 8, 0, 1200), "missing attempt confirmation selected a path");
  expect(!selector.recommend(1201, paths).path, "old observations survived session reset");
}

void explicit_budgets_required() {
  for (auto metric : {Metric::IceRoundTrip, Metric::CloudIngressRoundTrip}) {
    bool rejected = false;
    try { Policy value({metric, 200, 3, 5, 0.1, 100, 300}); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "non-end-to-end policy accepted");
  }
  const Policy::Config valid{Metric::ControlVehicleResultRoundTrip, 200, 3, 5, 0.1, 100, 300};
  for (int field = 0; field < 7; ++field) {
    auto config = valid;
    if (field == 0) config.max_sample_age_ms = 0;
    if (field == 1) config.min_samples = 0;
    if (field == 2) config.improvement_ms = -1;
    if (field == 3) config.improvement_fraction = 1.1;
    if (field == 4) config.stable_for_ms = -1;
    if (field == 5) config.min_dwell_ms = -1;
    if (field == 6) config.improvement_ms = std::numeric_limits<double>::quiet_NaN();
    bool rejected = false;
    try { Policy value(config); } catch (const std::invalid_argument&) { rejected = true; }
    expect(rejected, "invalid budget accepted");
  }
  auto selector = policy();
  std::array paths{observation(Path::P2p, 20, 1000)};
  expect(selector.recommend(1000, paths).reason == Policy::Reason::InvalidSnapshot, "unbound session selected");
}
}  // namespace

int main() {
  try {
    exact_vehicle_results();
    bounded_unknown_results();
    lowest_healthy_path_not_topology_preference();
    incomparable_and_stale_samples_cannot_select();
    switch_hysteresis_requires_new_progress_and_confirmation();
    repeated_snapshot_and_small_improvement_do_not_flap();
    explicit_budgets_required();
    attempt_and_session_confirmation_are_fenced();
    std::cout << "8 latency/path cases passed; pure offline, no sockets or actuation\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
