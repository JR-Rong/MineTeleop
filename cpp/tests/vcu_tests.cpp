#include "mine_teleop/vcu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using mine_teleop::vcu::CanFrame;
using mine_teleop::vcu::Command;
using mine_teleop::vcu::ParallelController;
using mine_teleop::vcu::State;

constexpr std::array<std::uint32_t, 8> kMotorStatus01Ids{
    0x18A0F4D0U,
    0x18A3F4D0U,
    0x18A6F4D0U,
    0x18A9F4D0U,
    0x18ACF4D0U,
    0x18AFF4D0U,
    0x18B2F4D0U,
    0x18B5F4D0U,
};

constexpr std::array<std::uint32_t, 8> kMotorStatus02Ids{
    0x18A1F4D0U,
    0x18A4F4D0U,
    0x18A7F4D0U,
    0x18AAF4D0U,
    0x18ADF4D0U,
    0x18B0F4D0U,
    0x18B3F4D0U,
    0x18B6F4D0U,
};

constexpr std::array<std::uint32_t, 4> kSteeringStatusIds{
    0x18C0F4D0U,
    0x18C1F4D0U,
    0x18C2F4D0U,
    0x18C3F4D0U,
};

constexpr std::array<std::uint32_t, 4> kBrakeStatusIds{
    0x18C8F4D0U,
    0x18C9F4D0U,
    0x18CAF4D0U,
    0x18CBF4D0U,
};

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

const CanFrame& find_frame(
    const std::vector<CanFrame>& frames,
    std::uint32_t id) {
  const auto found = std::find_if(frames.begin(), frames.end(), [&](const auto& frame) {
    return frame.id == id;
  });
  if (found == frames.end()) throw std::runtime_error("CAN frame was not generated");
  return *found;
}

std::uint64_t payload(const CanFrame& frame) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < frame.data.size(); ++index) {
    value |= static_cast<std::uint64_t>(frame.data[index]) << (index * 8U);
  }
  return value;
}

std::uint64_t signal(
    const CanFrame& frame,
    unsigned start,
    unsigned length) {
  const auto mask = (std::uint64_t{1} << length) - 1U;
  return (payload(frame) >> start) & mask;
}

CanFrame handshake_feedback(int status) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuHandshake};
  frame.data[1] = static_cast<std::uint8_t>(status);
  return frame;
}

CanFrame parking_brake_feedback(int status) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuParkingBrake};
  frame.data[0] = static_cast<std::uint8_t>(status);
  frame.data[2] = static_cast<std::uint8_t>(status);
  frame.data[4] = static_cast<std::uint8_t>(status);
  frame.data[6] = static_cast<std::uint8_t>(status);
  return frame;
}

CanFrame gear_feedback(int gear) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuVehicleStatus};
  frame.data[0] = static_cast<std::uint8_t>(gear << 2);
  return frame;
}

CanFrame speed_feedback(double speed_kph) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuVehicleSpeed};
  const auto raw = static_cast<std::uint16_t>(std::llround((speed_kph + 500.0) / 0.1));
  frame.data[0] = static_cast<std::uint8_t>(raw & 0xFFU);
  frame.data[1] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
  return frame;
}

CanFrame driver_gear_request_feedback(int gear) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuDriverIntention};
  frame.data[7] = static_cast<std::uint8_t>((gear & 0x07) << 1);
  return frame;
}

CanFrame motor_mode_feedback(std::size_t motor, int mode) {
  CanFrame frame{kMotorStatus02Ids.at(motor)};
  frame.data[0] = static_cast<std::uint8_t>(mode << 4);
  return frame;
}

CanFrame motor_torque_feedback(
    std::size_t motor,
    double torque_nm,
    double speed_rpm = 0.0,
    bool speed_valid = true) {
  CanFrame frame{kMotorStatus01Ids.at(motor)};
  const auto speed_raw =
      static_cast<std::uint16_t>(std::llround(speed_rpm + 8000.0));
  frame.data[0] = static_cast<std::uint8_t>(speed_raw & 0xFFU);
  frame.data[1] = static_cast<std::uint8_t>(
      ((speed_raw >> 8U) & 0x3FU) | (speed_valid ? 0x80U : 0U));
  const auto raw = static_cast<std::uint16_t>(std::llround((torque_nm + 800.0) / 0.1));
  frame.data[2] = static_cast<std::uint8_t>(raw & 0xFFU);
  frame.data[3] = static_cast<std::uint8_t>((raw >> 8U) & 0x3FU);
  return frame;
}

CanFrame steering_feedback(std::size_t axis, int mode, double angle_deg = 0.0) {
  CanFrame frame{kSteeringStatusIds.at(axis)};
  const auto raw = static_cast<std::uint16_t>(std::llround((angle_deg + 1575.0) / 0.1));
  frame.data[0] = static_cast<std::uint8_t>(mode);
  frame.data[1] = static_cast<std::uint8_t>(raw & 0xFFU);
  frame.data[2] = static_cast<std::uint8_t>((raw >> 8U) & 0xFFU);
  return frame;
}

CanFrame brake_feedback(std::size_t pair, int mode, double pressure_bar = 0.0) {
  CanFrame frame{kBrakeStatusIds.at(pair)};
  const auto raw = static_cast<std::uint16_t>(std::llround(pressure_bar / 0.1));
  frame.data[0] = static_cast<std::uint8_t>(mode | ((raw & 0x0FU) << 4U));
  frame.data[1] = static_cast<std::uint8_t>((raw >> 4U) & 0xFFU);
  frame.data[4] = frame.data[0];
  frame.data[5] = frame.data[1];
  return frame;
}

void send_mode_feedback(ParallelController& controller) {
  for (std::size_t index = 0; index < kMotorStatus02Ids.size(); ++index) {
    expect(controller.ingest(motor_mode_feedback(index, 1)), "motor mode feedback was rejected");
    expect(controller.ingest(motor_torque_feedback(index, 0.0)), "motor torque feedback was rejected");
  }
  for (std::size_t index = 0; index < kSteeringStatusIds.size(); ++index) {
    expect(controller.ingest(steering_feedback(index, 1)), "steering feedback was rejected");
  }
  for (std::size_t index = 0; index < kBrakeStatusIds.size(); ++index) {
    expect(controller.ingest(brake_feedback(index, 1)), "brake feedback was rejected");
  }
}

void prepare_parking_gate(ParallelController& controller, int driver_gear = 1) {
  expect(controller.ingest(handshake_feedback(3)), "manual handshake feedback was rejected");
  expect(controller.ingest(parking_brake_feedback(2)), "parked EPB feedback was rejected");
  expect(controller.ingest(speed_feedback(0.0)), "zero speed feedback was rejected");
  expect(
      controller.ingest(driver_gear_request_feedback(driver_gear)),
      "driver gear request feedback was rejected");
}

void advance_to_ready(ParallelController& controller, int gear = 3) {
  Command command;
  command.gear = gear;
  prepare_parking_gate(controller);
  expect(
      controller.request_parallel_handshake(),
      "explicit parallel handshake request was rejected in N with EPB parked");
  expect(controller.set_command(command), "valid command was rejected");
  for (int index = 0; index < 6; ++index) static_cast<void>(controller.tick());
  expect(
      controller.state() == State::WaitParallelHandshake,
      "controller did not establish the five-frame handshake low period");

  controller.ingest(handshake_feedback(5));
  static_cast<void>(controller.tick());
  expect(
      controller.state() == State::WaitParkingBrakeReleased,
      "intelligent-driving handshake status 5 was not accepted");

  controller.ingest(parking_brake_feedback(1));
  static_cast<void>(controller.tick());
  expect(controller.state() == State::WaitGear, "EPB release status 1 was not accepted");

  controller.ingest(gear_feedback(gear));
  static_cast<void>(controller.tick());
  expect(controller.state() == State::WaitActuatorModes, "gear feedback was not accepted");

  send_mode_feedback(controller);
  static_cast<void>(controller.tick());
  expect(controller.state() == State::Ready, "actuator mode feedback did not arm control");
}

void test_protocol_frames_reuse_intelligent_handshake_and_physical_zero_encoding() {
  ParallelController controller;
  const auto initial = controller.tick();
  expect(initial.size() == mine_teleop::vcu::kTransmitFrameCount, "not all 20 ms ADU frames were generated");
  expect(
      std::all_of(initial.begin(), initial.end(), [](const auto& frame) {
        return frame.extended && frame.dlc == 8;
      }),
      "ADU frames are not 29-bit eight-byte frames");

  const auto& initial_motor = find_frame(initial, mine_teleop::vcu::ids::kAduMcu01);
  expect(signal(initial_motor, 0, 3) == 0, "initial motor command is enabled");
  expect(signal(initial_motor, 8, 14) == 8000, "zero torque was not encoded with its -800 offset");
  expect(signal(initial_motor, 24, 14) == 8000, "zero speed was not encoded with its -8000 offset");
  const auto& standby_shake = find_frame(initial, mine_teleop::vcu::ids::kAduShake);
  expect(
      signal(standby_shake, 0, 8) == 0 &&
          signal(standby_shake, 16, 8) == 0,
      "standby asserted a handshake without a driver request");

  prepare_parking_gate(controller);
  expect(controller.request_parallel_handshake(), "explicit N/EPB-gated handshake request failed");
  std::vector<CanFrame> handshake_frames;
  for (int index = 0; index < 6; ++index) handshake_frames = controller.tick();
  const auto& shake = find_frame(handshake_frames, mine_teleop::vcu::ids::kAduShake);
  expect(signal(shake, 0, 8) == 2, "intelligent-driving ShakeReq was not asserted");
  expect(signal(shake, 16, 8) == 0, "CloudShakeReq was not kept clear");
}

void test_arming_uses_current_epb_semantics_and_gates_control() {
  ParallelController controller;
  Command command;
  command.gear = 3;
  command.motor_torque_nm.fill(120.0);
  command.steering_angle_deg.fill(12.0);
  command.steering_speed_degps.fill(20.0);
  command.brake_pressure_bar.fill(4.5);

  prepare_parking_gate(controller);
  expect(controller.request_parallel_handshake(), "explicit N/EPB-gated handshake request failed");
  expect(controller.set_command(command), "valid command was rejected");
  for (int index = 0; index < 6; ++index) static_cast<void>(controller.tick());
  controller.ingest(handshake_feedback(5));
  static_cast<void>(controller.tick());

  controller.ingest(parking_brake_feedback(2));
  auto frames = controller.tick();
  expect(
      controller.state() == State::WaitParkingBrakeReleased,
      "EPB parked status 2 was incorrectly treated as released");
  const auto& held_motor = find_frame(frames, mine_teleop::vcu::ids::kAduMcu01);
  expect(signal(held_motor, 3, 3) == 0, "motor mode escaped the arming gate");

  controller.ingest(parking_brake_feedback(1));
  static_cast<void>(controller.tick());
  controller.ingest(gear_feedback(3));
  static_cast<void>(controller.tick());
  send_mode_feedback(controller);
  frames = controller.tick();
  expect(controller.ready(), "controller did not reach ready");

  const auto& motor = find_frame(frames, mine_teleop::vcu::ids::kAduMcu01);
  expect(signal(motor, 0, 3) == 1 && signal(motor, 3, 3) == 1, "motor torque mode is not enabled");
  expect(signal(motor, 8, 14) == 9200, "120 Nm torque encoded incorrectly");

  const auto& steering = find_frame(frames, mine_teleop::vcu::ids::kAduEps01);
  expect(signal(steering, 0, 8) == 1, "EPS by-wire mode is not enabled");
  expect(signal(steering, 8, 16) == 15870, "12 degree steering angle encoded incorrectly");

  const auto& brake = find_frame(frames, mine_teleop::vcu::ids::kAduEhb01);
  expect(signal(brake, 0, 4) == 1, "EHB by-wire mode is not enabled");
  expect(signal(brake, 4, 12) == 45, "4.5 bar brake pressure encoded incorrectly");
}

void test_vehicle_speed_request_is_ready_only_and_keeps_torque_mode() {
  ParallelController controller;
  Command command;
  command.gear = 3;
  command.vehicle_speed_request_kph = 12.6;
  command.vehicle_speed_request_valid = true;

  expect(controller.set_command(command), "valid vehicle speed request was rejected");
  auto frames = controller.tick();
  const auto& standby_speed =
      find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
  expect(
      signal(standby_speed, 0, 8) == 0 && signal(standby_speed, 8, 8) == 0,
      "standby exposed a valid vehicle speed request");

  advance_to_ready(controller);
  expect(controller.set_command(command), "ready vehicle speed request was rejected");
  frames = controller.tick();
  const auto& ready_speed =
      find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
  expect(
      signal(ready_speed, 0, 8) == 12,
      "vehicle speed request was not quantized down to its 1 km/h field");
  expect(signal(ready_speed, 8, 8) == 1, "ready vehicle speed request was not marked valid");

  const auto& motor = find_frame(frames, mine_teleop::vcu::ids::kAduMcu01);
  expect(
      signal(motor, 0, 3) == 1 && signal(motor, 3, 3) == 1,
      "vehicle speed request changed the motor out of torque mode");

  for (const auto& [request_kph, expected_raw] :
       std::array<std::pair<double, std::uint64_t>, 2>{{{0.0, 0U}, {255.0, 255U}}}) {
    command.vehicle_speed_request_kph = request_kph;
    expect(controller.set_command(command), "vehicle speed encoding boundary was rejected");
    frames = controller.tick();
    const auto& boundary_speed =
        find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
    expect(
        signal(boundary_speed, 0, 8) == expected_raw &&
            signal(boundary_speed, 8, 8) == 1,
        "vehicle speed encoding boundary was incorrect");
  }

  command.vehicle_speed_request_kph = 12.6;
  command.vehicle_speed_request_valid = false;
  expect(controller.set_command(command), "disabled vehicle speed request was rejected");
  frames = controller.tick();
  const auto& disabled_speed =
      find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
  expect(
      signal(disabled_speed, 0, 8) == 0 && signal(disabled_speed, 8, 8) == 0,
      "explicitly invalid vehicle speed request was exposed as valid");
}

void test_motor_torque_resolution_preserves_quantized_ceiling() {
  ParallelController controller;
  advance_to_ready(controller);

  Command command;
  command.gear = 3;
  for (const double requested_torque_nm : {4.1, -4.1}) {
    command.motor_torque_nm.fill(requested_torque_nm);
    expect(controller.set_command(command), "quantized motor torque was rejected");
    const auto frames = controller.tick();
    const auto& motor = find_frame(frames, mine_teleop::vcu::ids::kAduMcu01);
    const double decoded_torque_nm =
        static_cast<double>(signal(motor, 8, 14)) * 0.1 - 800.0;
    expect(
        std::abs(decoded_torque_nm - requested_torque_nm) < 1e-9,
        "0.1 Nm motor torque ceiling changed during CAN encoding");
  }
}

void test_vehicle_speed_request_is_invalidated_by_safety_and_disarm_states() {
  ParallelController controller;
  advance_to_ready(controller);

  Command command;
  command.gear = 3;
  command.vehicle_speed_request_kph = 20.0;
  command.vehicle_speed_request_valid = true;
  expect(controller.set_command(command), "ready vehicle speed request was rejected");

  controller.emergency_stop();
  auto frames = controller.tick();
  const auto& emergency_speed =
      find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
  expect(
      signal(emergency_speed, 0, 8) == 0 && signal(emergency_speed, 8, 8) == 0,
      "emergency stop left the vehicle speed request valid");

  controller.clear_emergency_stop();
  auto emergency_switch = gear_feedback(3);
  emergency_switch.data[0] |= 1U;
  expect(controller.ingest(emergency_switch), "VCU emergency switch feedback was rejected");
  frames = controller.tick();
  const auto& emergency_switch_speed =
      find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
  expect(
      signal(emergency_switch_speed, 0, 8) == 0 &&
          signal(emergency_switch_speed, 8, 8) == 0,
      "VCU emergency switch left the vehicle speed request valid");

  expect(controller.ingest(gear_feedback(3)), "VCU emergency switch did not clear");
  controller.request_disarm();
  frames = controller.tick();
  const auto& disarm_speed =
      find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
  expect(
      signal(disarm_speed, 0, 8) == 0 && signal(disarm_speed, 8, 8) == 0,
      "disarm left the vehicle speed request valid");

  ParallelController faulted;
  advance_to_ready(faulted);
  expect(faulted.set_command(command), "fault-path vehicle speed request was rejected");
  faulted.transport_fault();
  frames = faulted.tick();
  const auto& fault_speed =
      find_frame(frames, mine_teleop::vcu::ids::kAduVehicleSpeed);
  expect(
      signal(fault_speed, 0, 8) == 0 && signal(fault_speed, 8, 8) == 0,
      "transport fault left the vehicle speed request valid");
}

void test_vehicle_speed_request_validation_is_finite_and_bounded() {
  ParallelController controller;
  Command command;

  for (const double boundary : {0.0, 255.0}) {
    command.vehicle_speed_request_kph = boundary;
    command.vehicle_speed_request_valid = true;
    expect(controller.set_command(command), "vehicle speed boundary was rejected");
  }

  for (const double invalid : {
           -0.1,
           255.1,
           std::numeric_limits<double>::quiet_NaN(),
           std::numeric_limits<double>::infinity()}) {
    command.vehicle_speed_request_kph = invalid;
    expect(!controller.set_command(command), "invalid vehicle speed request was accepted");
  }

  command.vehicle_speed_request_valid = false;
  command.vehicle_speed_request_kph = std::numeric_limits<double>::quiet_NaN();
  expect(
      !controller.set_command(command),
      "non-finite vehicle speed request bypassed validation while marked invalid");
}

void test_feedback_decoding_uses_si_units_and_complete_snapshot() {
  ParallelController controller;
  expect(controller.ingest(speed_feedback(36.0)), "speed feedback was rejected");
  expect(
      std::abs(controller.feedback().speed_mps - 10.0) < 1e-9,
      "VCU kph feedback was not converted to m/s");
  expect(!controller.feedback_complete(), "one CAN frame was treated as a complete feedback snapshot");

  expect(
      controller.ingest(motor_torque_feedback(0, 123.4, 1450.0)),
      "motor torque and speed feedback was rejected");
  expect(
      std::abs(controller.feedback().motor_torque_nm[0] - 123.4) < 1e-9,
      "motor torque feedback was not decoded in Nm");
  expect(
      controller.feedback().motor_speed_valid[0] &&
          std::abs(controller.feedback().motor_speed_rpm[0] - 1450.0) < 1e-9,
      "valid motor speed feedback was not decoded in rpm");
  expect(
      controller.ingest(motor_torque_feedback(1, 0.0, -25.0, false)) &&
          !controller.feedback().motor_speed_valid[1],
      "invalid motor speed quality was exposed as valid");

  controller.ingest(handshake_feedback(5));
  controller.ingest(parking_brake_feedback(1));
  controller.ingest(gear_feedback(3));
  controller.ingest(driver_gear_request_feedback(1));
  send_mode_feedback(controller);
  expect(controller.feedback_complete(), "complete safety feedback snapshot was not recognized");

  CanFrame short_frame = handshake_feedback(5);
  short_frame.dlc = 7;
  expect(!controller.ingest(short_frame), "short CAN feedback frame was accepted");
}

void test_arming_requires_fresh_feedback_after_each_request() {
  ParallelController controller;
  prepare_parking_gate(controller);
  expect(controller.request_parallel_handshake(), "explicit N/EPB-gated handshake request failed");
  controller.ingest(handshake_feedback(5));
  for (int index = 0; index < 6; ++index) static_cast<void>(controller.tick());
  expect(
      controller.state() == State::WaitParallelHandshake,
      "a handshake status received before ShakeReq armed the controller");
  static_cast<void>(controller.tick());
  expect(
      controller.state() == State::WaitParallelHandshake,
      "stale parallel handshake feedback was reused");

  controller.ingest(handshake_feedback(6));
  static_cast<void>(controller.tick());
  expect(
      controller.state() == State::WaitParallelHandshake,
      "legacy parallel-driving status 6 was accepted");

  controller.ingest(handshake_feedback(5));
  static_cast<void>(controller.tick());
  expect(
      controller.state() == State::WaitParkingBrakeReleased,
      "fresh intelligent-driving handshake feedback was not accepted");
}

void test_handshake_requires_neutral_and_electronic_parking_brake() {
  ParallelController controller;
  for (int index = 0; index < 10; ++index) static_cast<void>(controller.tick());
  expect(
      controller.state() == State::Standby,
      "controller left standby without an explicit handshake request");

  for (const int driver_gear : {2, 3, 4}) {
    prepare_parking_gate(controller, driver_gear);
    expect(
        !controller.request_parallel_handshake(),
        "parallel handshake started while the driver selector was not N");
    expect(
        controller.state() == State::Standby,
        "rejected non-N request changed controller state");
  }

  controller.ingest(driver_gear_request_feedback(1));
  controller.ingest(parking_brake_feedback(1));
  expect(
      !controller.request_parallel_handshake(),
      "parallel handshake started while the electronic parking brake was released");
  expect(
      controller.state() == State::Standby,
      "rejected electronic parking brake request changed controller state");

  controller.ingest(parking_brake_feedback(2));
  expect(
      controller.parking_ready(),
      "valid N/electronic-parking/manual/zero-speed gate was not recognized");
  expect(controller.request_parallel_handshake(), "valid explicit handshake request failed");
  expect(controller.state() == State::Initial, "accepted request did not start low-frame phase");
}

void test_disconnect_during_handshake_clears_request_and_confirms_manual() {
  ParallelController controller;
  prepare_parking_gate(controller);
  expect(controller.request_parallel_handshake(), "explicit N/EPB-gated handshake request failed");
  for (int index = 0; index < 6; ++index) static_cast<void>(controller.tick());
  expect(
      controller.state() == State::WaitParallelHandshake,
      "controller did not reach the asserted parallel handshake phase");

  controller.request_disarm();
  auto frames = controller.tick();
  expect(
      controller.state() == State::DisarmManual,
      "disconnect did not wait for VCU manual state after ShakeReq was asserted");
  const auto& shake = find_frame(frames, mine_teleop::vcu::ids::kAduShake);
  expect(signal(shake, 0, 8) == 0, "disconnect did not clear ShakeReq");
  expect(signal(shake, 16, 8) == 0, "disconnect asserted CloudShakeReq");

  controller.ingest(handshake_feedback(3));
  static_cast<void>(controller.tick());
  expect(controller.disarmed(), "fresh manual status did not confirm early disconnect");
}

void test_handshake_loss_forces_zero_torque_and_calibrated_brake() {
  ParallelController controller;
  advance_to_ready(controller);

  Command command;
  command.gear = 3;
  command.motor_torque_nm.fill(80.0);
  expect(controller.set_command(command), "ready command was rejected");
  Command emergency;
  emergency.gear = 1;
  emergency.brake_pressure_bar.fill(25.0);
  expect(controller.set_emergency_command(emergency), "emergency command was rejected");

  controller.ingest(handshake_feedback(3));
  const auto frames = controller.tick();
  expect(controller.state() == State::Fault, "parallel handshake loss did not latch a fault");
  const auto& motor = find_frame(frames, mine_teleop::vcu::ids::kAduMcu01);
  expect(signal(motor, 8, 14) == 8000, "fault output did not encode zero torque");
  const auto& brake = find_frame(frames, mine_teleop::vcu::ids::kAduEhb01);
  expect(signal(brake, 4, 12) == 250, "fault output did not use calibrated brake pressure");
}

void test_disarm_waits_for_torque_stop_neutral_park_and_manual() {
  ParallelController controller;
  advance_to_ready(controller);

  Command emergency;
  emergency.gear = 1;
  emergency.brake_pressure_bar.fill(25.0);
  expect(controller.set_emergency_command(emergency), "calibrated emergency command was rejected");
  controller.request_disarm();

  auto frames = controller.tick();
  expect(controller.state() == State::DisarmTorque, "disarm skipped torque feedback");
  const auto& braking = find_frame(frames, mine_teleop::vcu::ids::kAduEhb01);
  expect(signal(braking, 4, 12) == 250, "disarm did not apply calibrated brake pressure");

  for (std::size_t index = 0; index < kMotorStatus01Ids.size(); ++index) {
    controller.ingest(motor_torque_feedback(index, 0.0));
  }
  static_cast<void>(controller.tick());
  expect(controller.state() == State::DisarmStop, "zero torque feedback was not required");
  controller.request_disarm();
  expect(
      controller.state() == State::DisarmStop,
      "a repeated park/disarm request restarted the shutdown sequence");

  controller.ingest(speed_feedback(0.0));
  static_cast<void>(controller.tick());
  expect(controller.state() == State::DisarmNeutral, "zero vehicle speed was not required");

  controller.ingest(gear_feedback(1));
  frames = controller.tick();
  expect(controller.state() == State::DisarmParkingBrake, "neutral feedback was not required");
  const auto& epb = find_frame(frames, mine_teleop::vcu::ids::kAduEpb);
  expect(
      signal(epb, 0, 2) == 2 && signal(epb, 16, 2) == 2,
      "disarm did not request EPB parked value 2");

  controller.ingest(parking_brake_feedback(2));
  frames = controller.tick();
  expect(controller.state() == State::DisarmManual, "parked EPB feedback was not required");
  const auto& shake = find_frame(frames, mine_teleop::vcu::ids::kAduShake);
  expect(signal(shake, 0, 8) == 0, "intelligent-driving handshake was not cleared after parking");
  expect(signal(shake, 16, 8) == 0, "CloudShakeReq was asserted during disarm");

  controller.ingest(handshake_feedback(3));
  static_cast<void>(controller.tick());
  expect(controller.disarmed(), "manual handshake status 3 did not complete disarm");
  expect(
      controller.request_parallel_handshake(),
      "a fully disarmed N/park/manual controller could not start a new explicit handshake");
  expect(
      controller.state() == State::Initial,
      "reconnect did not restart the five-frame low-handshake phase");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"protocol_frames_reuse_intelligent_handshake_and_physical_zero_encoding",
       test_protocol_frames_reuse_intelligent_handshake_and_physical_zero_encoding},
      {"arming_uses_current_epb_semantics_and_gates_control",
       test_arming_uses_current_epb_semantics_and_gates_control},
      {"vehicle_speed_request_is_ready_only_and_keeps_torque_mode",
       test_vehicle_speed_request_is_ready_only_and_keeps_torque_mode},
      {"motor_torque_resolution_preserves_quantized_ceiling",
       test_motor_torque_resolution_preserves_quantized_ceiling},
      {"vehicle_speed_request_is_invalidated_by_safety_and_disarm_states",
       test_vehicle_speed_request_is_invalidated_by_safety_and_disarm_states},
      {"vehicle_speed_request_validation_is_finite_and_bounded",
       test_vehicle_speed_request_validation_is_finite_and_bounded},
      {"feedback_decoding_uses_si_units_and_complete_snapshot",
       test_feedback_decoding_uses_si_units_and_complete_snapshot},
      {"arming_requires_fresh_feedback_after_each_request",
       test_arming_requires_fresh_feedback_after_each_request},
      {"handshake_requires_neutral_and_electronic_parking_brake",
       test_handshake_requires_neutral_and_electronic_parking_brake},
      {"disconnect_during_handshake_clears_request_and_confirms_manual",
       test_disconnect_during_handshake_clears_request_and_confirms_manual},
      {"handshake_loss_forces_zero_torque_and_calibrated_brake",
       test_handshake_loss_forces_zero_torque_and_calibrated_brake},
      {"disarm_waits_for_torque_stop_neutral_park_and_manual",
       test_disarm_waits_for_torque_stop_neutral_park_and_manual},
  };

  int failed = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  return failed == 0 ? 0 : 1;
}
