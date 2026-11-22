#include "mine_teleop/core.hpp"
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using mine_teleop::ControlCommand;
using mine_teleop::ControlRecoveryCause;
using mine_teleop::Json;
using mine_teleop::MockVehicleAdapter;
using mine_teleop::SafetyState;
using mine_teleop::VehicleControlService;
void require(bool value, const std::string& why) { if (!value) throw std::runtime_error(why); }

mine_teleop::SessionControlProfileRequest profile_request(std::int64_t now) {
  mine_teleop::SessionControlProfileRequest request;
  request.vehicle_id = "vehicle-001"; request.driver_id = "driver-001"; request.session_id = "session-001";
  request.seq = 1; request.sent_at_utc_ms = now; request.control_token = "token";
  request.profile.profile_version = mine_teleop::kSessionControlProfileVersion;
  request.profile.consecutive_late_frame_limit = 3;
  request.profile.target_speed_kph = 20.0; request.profile.max_motor_torque_nm = 100.0;
  request.profile.max_brake_pressure_bar = 100.0; request.profile.service_brake_pressure_bar = 30.0;
  request.profile.hard_brake_pressure_bar = 100.0; request.profile.max_steering_angle_deg = 3.0;
  request.profile.speed_pid_kp = 1.0; request.profile.speed_pid_ki = 0.2; request.profile.speed_pid_kd = 0.0;
  request.profile.speed_pid_derivative_filter_tau_ms = 100.0; request.profile.speed_pid_max_dt_ms = 100;
  return request;
}

struct Harness {
  std::int64_t mono{1000}, utc{100000};
  MockVehicleAdapter* vehicle{nullptr};
  std::unique_ptr<VehicleControlService> service;
  std::uint64_t seq{0};
  explicit Harness(bool enabled = true) {
    auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
    config.control.control_timeout_ms = 1200;
    config.control.transient_hold.enabled = enabled;
    auto adapter = std::make_unique<MockVehicleAdapter>(); vehicle = adapter.get();
    service = std::make_unique<VehicleControlService>(config, "driver-001", "session-001", "token",
        std::move(adapter), 100, [this] { return mono; });
    service->start(utc);
    require(service->receive_session_profile(profile_request(utc), utc).accepted, "profile rejected");
  }
  mine_teleop::ReceiveResult send(double throttle, double steering = 0.0, std::int64_t age = 20, bool capable = true,
                                  bool video_ready = true, bool video_transient = false, bool link_ready = true, bool estop = false) {
    ControlCommand value;
    value.vehicle_id = "vehicle-001"; value.driver_id = "driver-001"; value.session_id = "session-001";
    value.seq = ++seq; value.sent_at_utc_ms = utc - age; value.gear = "D"; value.throttle = throttle;
    value.steering = steering; value.control_token = "token"; value.estop = estop;
    value.operator_active = throttle > 0.0 || steering != 0.0;
    value.critical_video_ready = video_ready; value.critical_video_transient = video_transient;
    // A control-link wait must also withdraw input readiness (command validation rule).
    value.control_link_ready = link_ready; value.operator_input_ready = link_ready;
    value.transient_hold_capable = capable;
    return service->receive_command(value, utc);
  }
  void wait(std::int64_t ms) { for (std::int64_t step = 0; step < ms; step += 50) { mono += 50; utc += 50; service->tick(utc); } }
  Json hold() const { return service->control_timing(utc).at("transient_hold"); }
  Json timing() const { return service->control_timing(utc); }
  mine_teleop::VehicleTelemetry output() { return vehicle->read_telemetry(); }
};

void short_gap_does_not_hold() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(250);
  require(!h.hold().at("active").get<bool>() && h.output().throttle_feedback > 0.0, "250 ms gap entered a hold");
}
void gap_holds_with_light_brake_and_resumes_without_rearm() {
  Harness h; require(h.send(0.5, 0.05).accepted, "initial command rejected");
  h.wait(500);
  require(h.hold().at("active").get<bool>() && h.hold().at("causes").at(0) == "control_gap", "500 ms gap did not hold");
  auto out = h.output();
  require(out.throttle_feedback == 0.0 && out.brake_feedback >= 0.15 && out.steering_feedback > 0.0, "hold output wrong");
  require(!h.timing().at("rearm_required").get<bool>() && !h.timing().at("control_braking_wait").get<bool>(),
      "hold demanded rearm or braking wait");
  const auto first = h.send(0.5, 0.05);
  require(first.accepted && h.hold().at("active").get<bool>(), "first fresh command ended the hold");
  bool warned = false; for (const auto& warning : first.warnings) warned = warned || warning == "transient_hold_traction_withheld";
  require(warned, "traction-withheld warning missing");
  // Until the hold resumes, traction stays withdrawn with the light brake, and the held
  // gear/steering reference is still the last fresh pre-hold command (seq 1), not seq 2.
  require(h.output().throttle_feedback == 0.0 && h.output().brake_feedback >= 0.15,
      "hold released traction or brake while fresh commands were still being counted");
  require(h.timing().at("last_valid_command_seq") == 1, "hold followed a fresh command's gear and steering");
  require(h.send(0.5, 0.05).accepted && !h.hold().at("active").get<bool>(), "second fresh command did not resume");
  require(h.output().throttle_feedback > 0.0, "resumed command did not apply traction");
  require(h.timing().at("last_valid_command_seq") == 3, "resume did not adopt the driver's current command");
  require(h.hold().at("last").at("outcome") == "resumed", "resume not recorded");
}
void hold_keeps_the_drivers_brake_above_the_floor() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  // Full session brake is 100 bar of the vehicle's 327.6 bar (about 0.305 normalized): the hold
  // must keep it instead of releasing the brake down to the 0.15 floor.
  ControlCommand braking;
  braking.vehicle_id = "vehicle-001"; braking.driver_id = "driver-001"; braking.session_id = "session-001";
  braking.seq = ++h.seq; braking.sent_at_utc_ms = h.utc - 20; braking.gear = "D"; braking.control_token = "token";
  braking.brake = 1.0; braking.transient_hold_capable = true;
  require(h.service->receive_command(braking, h.utc).accepted, "braking command rejected");
  h.wait(500);
  require(h.hold().at("active").get<bool>(), "gap after a braking command did not hold");
  require(h.output().brake_feedback >= 0.30, "hold released the operator brake");
}
void long_gap_escalates_on_watchdog_tick() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(1050);
  require(h.timing().at("control_braking_wait").get<bool>() && h.output().brake_feedback >= 1.0, "1000 ms gap did not escalate");
  require(h.hold().at("last").at("escalation_reason") == "hold_duration_exceeded", "wrong escalation reason");
  require(!h.send(0.5).accepted, "motion accepted without rearm after escalation");
}
void late_commands_hold_then_resume() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  for (int i = 0; i < 3; ++i) { h.wait(50); static_cast<void>(h.send(0.5, 0.0, 250)); }
  require(h.hold().at("active").get<bool>(), "three late commands did not hold");
  require(!h.timing().at("control_braking_wait").get<bool>(), "late commands caused braking wait");
  h.wait(50); static_cast<void>(h.send(0.5)); h.wait(50);
  require(h.send(0.5).accepted && !h.hold().at("active").get<bool>(), "fresh commands did not resume after late ones");
}
void driver_video_transient_holds_and_fault_brakes() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  static_cast<void>(h.send(0.5, 0.0, 20, true, false, true));
  require(h.hold().at("active").get<bool>() && !h.timing().at("control_braking_wait").get<bool>(), "video stall did not hold");
  static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
  require(!h.hold().at("active").get<bool>(), "video recovery did not resume");
  static_cast<void>(h.send(0.5, 0.0, 20, true, false, false));
  require(h.timing().at("control_braking_wait").get<bool>(), "video fault did not brake immediately");
}
void vehicle_video_transient_holds() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.service->observe_vehicle_video_transient(true, h.utc);
  require(h.hold().at("active").get<bool>(), "vehicle video stall did not hold");
  static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
  require(h.hold().at("active").get<bool>(), "hold resumed while vehicle video was still stale");
  h.service->observe_vehicle_video_transient(false, h.utc);
  static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
  require(!h.hold().at("active").get<bool>(), "vehicle video recovery did not resume");
}
void link_not_ready_is_a_fault() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  static_cast<void>(h.send(0.0, 0.0, 20, true, true, false, false));
  require(h.timing().at("control_braking_wait").get<bool>(), "control_link_ready=false did not brake");
}
void estop_during_hold() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(400);
  require(h.hold().at("active").get<bool>(), "gap did not hold before the estop");
  static_cast<void>(h.send(0.0, 0.0, 20, true, true, false, true, true));
  require(h.service->safety_state() == SafetyState::Estop, "estop during hold not latched");
  require(!h.hold().at("active").get<bool>() && h.hold().at("last").at("escalation_reason") == "estop",
      "estop did not end the hold record");
}
void brake_floor_survives_hard_timeout(bool enabled) {
  Harness h(enabled); require(h.send(0.5).accepted, "initial command rejected");
  h.service->set_control_braking_wait(true, h.utc, ControlRecoveryCause::ControlLink);
  h.wait(1250);
  require(h.service->safety_state() == SafetyState::TimeoutBrake, "hard timeout not reached");
  require(h.output().brake_feedback >= 1.0, "brake dropped below the braking-wait floor at hard timeout");
}
void hard_timeout_boundary() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(1150);
  require(h.service->safety_state() != SafetyState::TimeoutBrake, "hard timeout before 1200 ms");
  h.wait(50);
  require(h.service->safety_state() == SafetyState::TimeoutBrake, "hard timeout not at 1200 ms");
}
void hard_timeout_while_ticks_were_stalled_ends_the_hold() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(400);
  require(h.hold().at("active").get<bool>(), "gap did not hold");
  // The watchdog thread stalls past the hard timeout: the hold must not linger as "active".
  h.mono += 900; h.utc += 900; h.service->tick(h.utc);
  require(h.service->safety_state() == SafetyState::TimeoutBrake, "stalled watchdog did not reach the hard timeout");
  require(!h.hold().at("active").get<bool>() && h.hold().at("last").at("escalation_reason") == "hard_timeout",
      "hard timeout left the hold record active or unexplained");
}
void legacy_without_capability() {
  Harness h; require(h.send(0.5, 0.0, 20, false).accepted, "initial command rejected");
  h.wait(500);
  require(!h.hold().at("active").get<bool>(), "legacy session entered a hold");
  static_cast<void>(h.send(0.5, 0.0, 20, false));
  require(h.timing().at("control_braking_wait").get<bool>(), "legacy gap behavior changed");
}
void disabled_vehicle_ignores_the_capability() {
  Harness h(false); require(h.send(0.5).accepted, "initial command rejected");
  h.wait(500);
  require(!h.hold().at("active").get<bool>() && !h.hold().at("enabled").get<bool>(), "disabled vehicle entered a hold");
  static_cast<void>(h.send(0.5));
  require(h.timing().at("control_braking_wait").get<bool>(), "disabled vehicle changed its gap behavior");
}
void capability_withdrawn_mid_hold_clears_the_hold() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(400);
  require(h.hold().at("active").get<bool>(), "gap did not hold");
  static_cast<void>(h.send(0.0, 0.0, 20, false));
  require(!h.hold().at("active").get<bool>(), "withdrawn capability left a stale hold");
}
void budget_escalation() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  for (int i = 0; i < 6; ++i) {
    h.wait(350);
    require(h.hold().at("active").get<bool>(), "budgeted gap did not hold");
    static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
    require(!h.hold().at("active").get<bool>(), "budgeted hold did not resume");
  }
  h.wait(350);
  require(h.timing().at("control_braking_wait").get<bool>() &&
      h.hold().at("last").at("escalation_reason") == "budget_count_exceeded", "seventh hold in a minute did not escalate");
}
// set_control_braking_wait(false) forces DEGRADED so the next neutral command re-arms the input. That is not a
// command gap, and must not look like one to the hold.
void braking_wait_release_does_not_fake_a_gap() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.service->set_control_braking_wait(true, h.utc, ControlRecoveryCause::ControlLink);
  for (int i = 0; i < 3; ++i) { h.wait(50); static_cast<void>(h.send(0.0)); }  // The driver keeps sending neutral commands.
  h.service->set_control_braking_wait(false, h.utc, ControlRecoveryCause::ControlLink);
  h.wait(50);
  require(!h.hold().at("active").get<bool>() && h.hold().at("holds_in_window") == 0,
      "releasing a braking wait started a control-gap hold: " + h.hold().dump());
  require(h.send(0.0).accepted && !h.timing().at("rearm_required").get<bool>(), "a neutral command did not re-arm after the release");
  require(h.send(0.5).accepted && h.output().throttle_feedback > 0.0, "motion was not restored after the re-arm");
  require(h.hold().at("holds_in_window") == 0 && h.hold().at("last").is_null(), "the release left a hold record behind");
  // A genuine gap after the release still holds.
  h.wait(400);
  require(h.hold().at("active").get<bool>() && h.hold().at("causes").at(0) == "control_gap", "a real gap after the release did not hold");
}
void epoch_change_escalates_with_its_own_reason() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(400);
  require(h.hold().at("active").get<bool>(), "gap did not hold");
  h.service->note_transport_epoch_changed(h.utc);
  require(h.timing().at("control_braking_wait").get<bool>() &&
      h.hold().at("last").at("escalation_reason") == "transport_epoch_changed", "epoch change did not escalate with its reason");
}
void events_are_drained_once() {
  Harness h; require(h.send(0.5).accepted, "initial command rejected");
  h.wait(400);
  static_cast<void>(h.send(0.5)); static_cast<void>(h.send(0.5));
  const auto events = h.service->drain_transient_hold_events();
  require(events.size() == 2 && events.at(0).at("event") == "vehicle_transient_hold_entered" &&
      events.at(1).at("event") == "vehicle_transient_hold_resumed", "hold events missing or out of order");
  require(h.service->drain_transient_hold_events().empty(), "hold events were not drained");
}
}  // namespace

int main() {
  try {
    short_gap_does_not_hold(); gap_holds_with_light_brake_and_resumes_without_rearm();
    hold_keeps_the_drivers_brake_above_the_floor(); long_gap_escalates_on_watchdog_tick();
    late_commands_hold_then_resume(); driver_video_transient_holds_and_fault_brakes(); vehicle_video_transient_holds();
    link_not_ready_is_a_fault(); estop_during_hold(); brake_floor_survives_hard_timeout(true); brake_floor_survives_hard_timeout(false);
    hard_timeout_boundary(); hard_timeout_while_ticks_were_stalled_ends_the_hold(); legacy_without_capability();
    disabled_vehicle_ignores_the_capability(); capability_withdrawn_mid_hold_clears_the_hold(); budget_escalation();
    epoch_change_escalates_with_its_own_reason(); events_are_drained_once(); braking_wait_release_does_not_fake_a_gap();
    std::cout << "PASS vehicle transient hold: gap/late/video holds, light brake, resume, escalation, faults, floor, legacy\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
