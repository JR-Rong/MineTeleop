#pragma once

extern "C" int mine_teleop_chassis_test_transition_deadline_reset(void);
extern "C" int mine_teleop_chassis_test_transition_deadline_ms(int state_value,
                                                               std::int64_t* deadline_ms);
extern "C" int mine_teleop_chassis_test_transition_deadline_observe(int state_value,
                                                                    std::uint64_t transition_epoch,
                                                                    std::int64_t monotonic_ms,
                                                                    std::int64_t* elapsed_ms,
                                                                    std::int64_t* limit_ms);

void test_transition_deadline_supervisor_boundaries() {
  using State = mine_teleop::vcu::State;
  const auto deadline_for = [](State state) {
    std::int64_t deadline_ms = -1;
    expect(mine_teleop_chassis_test_transition_deadline_ms(static_cast<int>(state), &deadline_ms) ==
                   1 &&
               deadline_ms > 0,
           "transition supervisor did not expose a deadline for a monitored state");
    return deadline_ms;
  };
  const auto observe = [](State state, std::uint64_t epoch, std::int64_t now_ms,
                          std::int64_t* elapsed_ms = nullptr, std::int64_t* limit_ms = nullptr) {
    std::int64_t local_elapsed = -1;
    std::int64_t local_limit = -1;
    const int result = mine_teleop_chassis_test_transition_deadline_observe(
        static_cast<int>(state), epoch, now_ms, elapsed_ms == nullptr ? &local_elapsed : elapsed_ms,
        limit_ms == nullptr ? &local_limit : limit_ms);
    return result;
  };

  const auto wait_handshake_limit = deadline_for(State::WaitParallelHandshake);
  const auto wait_epb_limit = deadline_for(State::WaitParkingBrakeReleased);
  const auto wait_gear_limit = deadline_for(State::WaitGear);
  const auto wait_modes_limit = deadline_for(State::WaitActuatorModes);
  const auto torque_limit = deadline_for(State::DisarmTorque);
  const auto stop_limit = deadline_for(State::DisarmStop);
  const auto neutral_limit = deadline_for(State::DisarmNeutral);
  const auto park_limit = deadline_for(State::DisarmParkingBrake);
  const auto manual_limit = deadline_for(State::DisarmManual);
  expect(wait_handshake_limit == 4000 && wait_epb_limit == 5000 && wait_gear_limit == 3000 &&
             wait_modes_limit == 4000 && torque_limit == 3000 && stop_limit == 10000 &&
             neutral_limit == 3000 && park_limit == 5000 && manual_limit == 4000,
         "transition policy did not provide the documented bounded deadline for every phase");
  expect(wait_gear_limit != 500 && stop_limit > wait_gear_limit,
         "transition policy reused the 500 ms feedback freshness watchdog for every phase");

  // No first handshake feedback must still have its own bounded progress
  // deadline; this does not depend on a freshness watchdog having a frame to
  // age out.
  expect(mine_teleop_chassis_test_transition_deadline_reset() == 0 &&
             observe(State::WaitParallelHandshake, 40, 500) == 0 &&
             observe(State::WaitParallelHandshake, 40, 500 + wait_handshake_limit) == 1,
         "missing first handshake feedback did not have a bounded phase deadline");

  expect(mine_teleop_chassis_test_transition_deadline_reset() == 0 &&
             observe(State::WaitGear, 41, 1000) == 0 &&
             observe(State::WaitGear, 41, 1000 + wait_gear_limit - 1) == 0,
         "transition deadline expired before its inclusive boundary");
  std::int64_t elapsed_ms = -1;
  std::int64_t limit_ms = -1;
  expect(observe(State::WaitGear, 41, 1000 + wait_gear_limit, &elapsed_ms, &limit_ms) == 1 &&
             elapsed_ms == wait_gear_limit && limit_ms == wait_gear_limit,
         "transition deadline did not expire exactly at its monotonic boundary");
  expect(observe(State::WaitGear, 41, 1000 + wait_gear_limit + 1) == 0,
         "one phase timeout was emitted repeatedly without a new transition");

  expect(mine_teleop_chassis_test_transition_deadline_reset() == 0 &&
             observe(State::WaitGear, 42, 2000) == 0 &&
             observe(State::WaitGear, 42, 2000 + wait_gear_limit - 1) == 0 &&
             // A duplicate page request or fresh-but-unmatched feedback retains
             // the same epoch and therefore cannot renew the deadline.
             observe(State::WaitGear, 42, 2000 + wait_gear_limit - 1) == 0 &&
             observe(State::WaitGear, 42, 2000 + wait_gear_limit) == 1,
         "same-state activity renewed a phase-progress deadline");

  const auto replacement_entry = 3000 + wait_gear_limit - 1;
  expect(mine_teleop_chassis_test_transition_deadline_reset() == 0 &&
             observe(State::WaitGear, 43, 3000) == 0 &&
             observe(State::WaitGear, 44, replacement_entry) == 0 &&
             observe(State::WaitGear, 44, replacement_entry + wait_gear_limit - 1) == 0 &&
             observe(State::WaitGear, 44, replacement_entry + wait_gear_limit) == 1,
         "a new state-transition epoch did not start one new deadline");

  expect(mine_teleop_chassis_test_transition_deadline_reset() == 0 &&
             observe(State::DisarmStop, 45, 7000) == 0 &&
             observe(State::DisarmStop, 45, 6990) == 0 &&
             observe(State::DisarmStop, 45, 7000 + stop_limit - 1) == 0 &&
             observe(State::DisarmStop, 45, 7000 + stop_limit) == 1,
         "a backward injected clock sample extended the disarm-stop deadline");

  std::int64_t ignored_deadline = -1;
  expect(mine_teleop_chassis_test_transition_deadline_ms(static_cast<int>(State::Ready),
                                                         &ignored_deadline) == 0 &&
             mine_teleop_chassis_test_transition_deadline_reset() == 0 &&
             observe(State::Ready, 46, 9000) == 0,
         "the supervisor incorrectly imposed a deadline on Ready");
}

void test_wait_gear_transition_timeout_preserves_staged_disarm() {
  int transport[2]{-1, -1};
  expect(::socketpair(AF_UNIX, SOCK_DGRAM, 0, transport) == 0,
         "transition-timeout adopted transport could not be created");
  const auto log_path =
      std::filesystem::path("/tmp/mine-teleop-vcu-transition-timeout-smoke.jsonl");
  std::error_code error;
  std::filesystem::remove(log_path, error);
  const auto adopted_fd = std::to_string(transport[0]);
  ::setenv("MINE_TELEOP_VCU_LOG_PATH", log_path.c_str(), 1);
  ::setenv("MINE_TELEOP_CHASSIS_TEST_FD", adopted_fd.c_str(), 1);
  ::setenv("MINE_TELEOP_CHASSIS_TEST_TRANSITION_DEADLINE_MS", "80", 1);

  bool opened = false;
  const auto cleanup = [&] {
    if (opened)
      static_cast<void>(mine_teleop_chassis_close());
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_TRANSITION_DEADLINE_MS");
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_FD");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");
    if (transport[0] >= 0)
      ::close(transport[0]);
    if (transport[1] >= 0)
      ::close(transport[1]);
  };

  try {
    // The test-only SocketCan adopter intentionally accepts only this fixed
    // interface name; the FD provides the isolated transport.
    auto config = valid_v2_config("mt-test", 800);
    expect(mine_teleop_chassis_open_v2(&config) == 0,
           "transition-timeout bridge did not adopt the test transport");
    opened = true;
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_TRANSITION_DEADLINE_MS");
    ::unsetenv("MINE_TELEOP_CHASSIS_TEST_FD");
    ::unsetenv("MINE_TELEOP_VCU_LOG_PATH");

    auto feedback = runtime_feedback(3, 1, 2, 0.0);
    MineTeleopChassisRuntimeControlResultV1 profile_result{};
    // V2 has no ordinary physical-pressure capability; the full EHB request
    // asserted below comes from the independent safe-disarm path.
    auto profile = valid_runtime_control_config(1, 5.0, 41.25, 0.0);
    expect(mine_teleop_chassis_update_feedback(&feedback) == 0 &&
               mine_teleop_chassis_configure_runtime_control_v2(&profile, &profile_result) == 0 &&
               mine_teleop_chassis_request_parallel_handshake() == 0,
           "transition-timeout runtime could not enter an explicitly authorized handshake");

    const std::array<double, 4> steering{};
    expect(mine_teleop_chassis_apply_state(3, 5.0, 0.0, steering.data(), steering.size()) == 0,
           "transition-timeout runtime did not retain a D target for WaitGear");
    expect(wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
           "transition-timeout runtime did not reach WaitParallelHandshake");

    // Keep every critical frame fresh while retaining actual N.  This proves
    // the phase deadline is independent of the 500 ms freshness watchdog.
    feedback = runtime_feedback(5, 1, 2, 0.0);
    expect(mine_teleop_chassis_update_feedback(&feedback) == 0 &&
               wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARKING_BRAKE_RELEASED),
           "transition-timeout runtime did not accept the intelligent handshake");
    feedback = runtime_feedback(5, 1, 1, 0.0);
    expect(mine_teleop_chassis_update_feedback(&feedback) == 0 &&
               wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_GEAR),
           "transition-timeout runtime did not reach fresh-but-mismatched WaitGear");

    bool entered_disarm_torque = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
      expect(mine_teleop_chassis_update_feedback(&feedback) == 0,
             "fresh mismatch feedback injection failed before transition timeout");
      MineTeleopChassisHandshakeStatus status{};
      expect(mine_teleop_chassis_read_handshake_status(&status) == 0,
             "transition-timeout handshake status could not be read");
      if (status.state == MINE_TELEOP_VCU_DISARM_TORQUE) {
        entered_disarm_torque = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    expect(entered_disarm_torque,
           "fresh-but-wrong gear feedback did not enter the torque-zero disarm phase");
    MineTeleopChassisTelemetry transition_timeout_telemetry{};
    expect(
        mine_teleop_chassis_read_telemetry(&transition_timeout_telemetry) == 0 &&
            transition_timeout_telemetry.estop == 1 &&
            transition_timeout_telemetry.stop_source == MINE_TELEOP_CHASSIS_STOP_SOURCE_WATCHDOG &&
            transition_timeout_telemetry.stop_reason ==
                MINE_TELEOP_CHASSIS_STOP_REASON_VCU_TRANSITION_TIMEOUT,
        "transition deadline expiry did not expose distinct watchdog provenance");

    const auto stop_frames = drain_can_frames(transport[1], 50);
    expect_all_motor_torque_raw(stop_frames, 8000,
                                "transition timeout did not withdraw all traction before disarm");
    expect_all_brake_pressure_raw(
        stop_frames, 4095, "transition timeout did not preserve EHB safety braking during disarm");

    // Finish the existing reverse sequence with fresh data.  It may reach
    // Disarmed, but it must never auto-start another handshake or clear the
    // timeout/profile latch from these old feedback values alone.
    feedback = runtime_feedback(3, 1, 2, 0.0);
    for (int index = 0; index < 6; ++index) {
      expect(mine_teleop_chassis_update_feedback(&feedback) == 0,
             "fresh staged-disarm feedback injection failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    expect(wait_for_handshake_state(MINE_TELEOP_VCU_DISARMED),
           "transition timeout did not retain the existing N/EPB/manual disarm sequence");
    for (int index = 0; index < 3; ++index) {
      expect(mine_teleop_chassis_update_feedback(&feedback) == 0,
             "post-disarm stale-input injection failed");
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    MineTeleopChassisHandshakeStatus status{};
    expect(mine_teleop_chassis_read_handshake_status(&status) == 0 &&
               status.state == MINE_TELEOP_VCU_DISARMED && status.ready == 0,
           "post-timeout feedback automatically restored driving authority");

    // Old feedback alone cannot clear the timeout: an explicit current profile
    // followed by a new handshake is required before the bridge can leave
    // Disarmed again.
    expect(mine_teleop_chassis_request_parallel_handshake() == -2,
           "transition timeout recovered before an explicit current profile");
    profile.profile_revision = 2;
    expect(mine_teleop_chassis_configure_runtime_control_v2(&profile, &profile_result) == 0 &&
               mine_teleop_chassis_request_parallel_handshake() == 0 &&
               wait_for_handshake_state(MINE_TELEOP_VCU_WAIT_PARALLEL_HANDSHAKE),
           "transition timeout did not require profile reapply plus a new handshake");
    expect(mine_teleop_chassis_disconnect_parallel_handshake() == 0 &&
               mine_teleop_chassis_update_feedback(&feedback) == 0 &&
               wait_for_handshake_state(MINE_TELEOP_VCU_DISARMED),
           "explicit post-timeout handshake could not return through safe manual disarm");

    expect(mine_teleop_chassis_close() == 0, "transition-timeout bridge did not close");
    opened = false;
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();

  const auto events = read_json_lines(log_path);
  const auto timed_out = std::find_if(events.begin(), events.end(), [](const auto& event) {
    return event.value("name", "") == "transition_timeout" &&
           event.value("issue_code", "") == "vcu_transition_timeout";
  });
  expect(timed_out != events.end(), "transition timeout did not write its stable diagnostic");
  expect(timed_out->value("stage", "") == "vcu_transition_wait_gear" &&
             timed_out->value("elapsed_ms", 0) >= timed_out->value("limit_ms", 1) &&
             timed_out->value("state_transition_epoch", 0ULL) > 0ULL &&
             timed_out->value("state_entry_generation", 0ULL) > 0ULL &&
             timed_out->value("stop_source", "") == "watchdog" &&
             timed_out->value("stop_reason", "") == "vcu_transition_timeout" &&
             timed_out->value("stop_reason_id", 0U) ==
                 MINE_TELEOP_CHASSIS_STOP_REASON_VCU_TRANSITION_TIMEOUT &&
             timed_out->at("expected").value("gear", 0) == 3 &&
             timed_out->at("observed").value("gear", 0) == 1 &&
             std::find(timed_out->at("missing_or_mismatched").begin(),
                       timed_out->at("missing_or_mismatched").end(),
                       Json("gear")) != timed_out->at("missing_or_mismatched").end(),
         "transition-timeout diagnostic omitted the phase, boundary, or mismatch evidence");
  expect(std::count_if(events.begin(), events.end(),
                       [](const auto& event) {
                         return event.value("name", "") == "transition_timeout";
                       }) == 1 &&
             std::none_of(events.begin(), events.end(),
                          [](const auto& event) {
                            return event.value("name", "") == "arming_feedback_timeout";
                          }),
         "fresh mismatch feedback was misclassified as a repeated or freshness timeout");
  expect(std::any_of(events.begin(), events.end(),
                     [](const auto& event) {
                       return event.value("name", "") == "runtime_control_profile_cleared" &&
                              event.value("stage", "") == "vcu_transition_timeout";
                     }),
         "transition timeout did not revoke the active control profile");
  expect(std::any_of(events.begin(), events.end(),
                     [](const auto& event) {
                       return event.value("name", "") == "transition_timeout_recovered" &&
                              event.value("stage", "") == "runtime_control_config";
                     }),
         "transition timeout recovery did not require the explicit profile reapply");
  std::filesystem::remove(log_path, error);
}
