#include "mine_teleop/vcu.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace mine_teleop::vcu {
namespace {

constexpr int kNoRequest = 0;
constexpr int kNeutralGear = 1;
constexpr int kIntelligentHandshakeRequest = 2;
constexpr int kIntelligentHandshakeStatus = 5;
constexpr int kManualHandshakeStatus = 3;
constexpr int kNeutralGearRequest = 1;
constexpr int kParkingBrakeHold = 0;
constexpr int kParkingBrakeRelease = 1;
constexpr int kParkingBrakePark = 2;
constexpr int kByWireMode = 1;
constexpr int kMotorEnable = 1;
constexpr int kMotorTorqueMode = 1;
constexpr double kTorqueZeroToleranceNm = 2.0;
constexpr double kStoppedSpeedToleranceMps = 0.1;

constexpr std::array<std::uint32_t, kMotorCount> kMotorCommandIds{
    0x18F0D0F5U,
    0x18F1D0F5U,
    0x18F2D0F5U,
    0x18F3D0F5U,
    0x18F4D0F5U,
    0x18F5D0F5U,
    0x18F6D0F5U,
    0x18F7D0F5U,
};

constexpr std::array<std::uint32_t, kMotorCount> kMotorStatus01Ids{
    0x18A0F4D0U,
    0x18A3F4D0U,
    0x18A6F4D0U,
    0x18A9F4D0U,
    0x18ACF4D0U,
    0x18AFF4D0U,
    0x18B2F4D0U,
    0x18B5F4D0U,
};

constexpr std::array<std::uint32_t, kMotorCount> kMotorStatus02Ids{
    0x18A1F4D0U,
    0x18A4F4D0U,
    0x18A7F4D0U,
    0x18AAF4D0U,
    0x18ADF4D0U,
    0x18B0F4D0U,
    0x18B3F4D0U,
    0x18B6F4D0U,
};

constexpr std::array<std::uint32_t, kSteeringAxisCount> kSteeringStatusIds{
    0x18C0F4D0U,
    0x18C1F4D0U,
    0x18C2F4D0U,
    0x18C3F4D0U,
};

constexpr std::array<std::uint32_t, kSteeringAxisCount> kBrakeStatusIds{
    0x18C8F4D0U,
    0x18C9F4D0U,
    0x18CAF4D0U,
    0x18CBF4D0U,
};

std::uint64_t signal_mask(unsigned length) {
  if (length >= 64U) return std::numeric_limits<std::uint64_t>::max();
  return (std::uint64_t{1} << length) - 1U;
}

std::uint64_t load_little_endian(const std::array<std::uint8_t, 8>& data) {
  std::uint64_t value = 0;
  for (std::size_t index = 0; index < data.size(); ++index) {
    value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
  }
  return value;
}

void store_little_endian(std::array<std::uint8_t, 8>& data, std::uint64_t value) {
  for (std::size_t index = 0; index < data.size(); ++index) {
    data[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
  }
}

std::uint64_t extract_signal(const CanFrame& frame, unsigned start, unsigned length) {
  return (load_little_endian(frame.data) >> start) & signal_mask(length);
}

void insert_signal(CanFrame& frame, unsigned start, unsigned length, std::uint64_t raw) {
  auto payload = load_little_endian(frame.data);
  const auto mask = signal_mask(length) << start;
  payload = (payload & ~mask) | ((raw << start) & mask);
  store_little_endian(frame.data, payload);
}

std::uint64_t encode_physical(
    double value,
    double factor,
    double offset,
    unsigned length,
    double minimum,
    double maximum) {
  const auto bounded = std::clamp(value, minimum, maximum);
  const auto raw = std::llround((bounded - offset) / factor);
  return std::min<std::uint64_t>(
      static_cast<std::uint64_t>(std::max<long long>(raw, 0)),
      signal_mask(length));
}

double decode_physical(std::uint64_t raw, double factor, double offset) {
  return static_cast<double>(raw) * factor + offset;
}

template <typename T, std::size_t Size>
bool all_equal_valid(
    const std::array<T, Size>& values,
    const std::array<bool, Size>& valid,
    T expected) {
  for (std::size_t index = 0; index < Size; ++index) {
    if (!valid[index] || values[index] != expected) return false;
  }
  return true;
}

template <std::size_t Size>
bool all_valid(const std::array<bool, Size>& values) {
  return std::all_of(values.begin(), values.end(), [](bool value) { return value; });
}

template <std::size_t Size>
bool all_newer_than(
    const std::array<std::uint64_t, Size>& values,
    std::uint64_t generation) {
  return std::all_of(values.begin(), values.end(), [&](std::uint64_t value) {
    return value > generation;
  });
}

template <std::size_t Size>
bool all_finite_in_range(
    const std::array<double, Size>& values,
    double minimum,
    double maximum) {
  return std::all_of(values.begin(), values.end(), [&](double value) {
    return std::isfinite(value) && value >= minimum && value <= maximum;
  });
}

bool command_valid(const Command& command) {
  return command.gear >= 1 && command.gear <= 3 &&
         all_finite_in_range(command.motor_torque_nm, -800.0, 838.3) &&
         all_finite_in_range(command.motor_speed_rpm, -8000.0, 8383.0) &&
         all_finite_in_range(command.steering_angle_deg, -30.0, 30.0) &&
         all_finite_in_range(command.steering_speed_degps, 0.0, 255.0) &&
         all_finite_in_range(command.brake_pressure_bar, 0.0, 409.5);
}

CanFrame make_motor_frame(
    std::size_t index,
    int command,
    int mode,
    double torque_nm,
    double speed_rpm) {
  CanFrame frame{kMotorCommandIds[index]};
  insert_signal(frame, 0, 3, static_cast<std::uint64_t>(command));
  insert_signal(frame, 3, 3, static_cast<std::uint64_t>(mode));
  insert_signal(frame, 8, 14, encode_physical(torque_nm, 0.1, -800.0, 14, -800.0, 838.3));
  insert_signal(frame, 24, 14, encode_physical(speed_rpm, 1.0, -8000.0, 14, -8000.0, 8383.0));
  return frame;
}

CanFrame make_steering_frame(
    std::uint32_t id,
    std::size_t first_axis,
    const std::array<int, kSteeringAxisCount>& mode,
    const std::array<double, kSteeringAxisCount>& angle_deg,
    const std::array<double, kSteeringAxisCount>& speed_degps) {
  CanFrame frame{id};
  for (std::size_t local_axis = 0; local_axis < 2; ++local_axis) {
    const auto axis = first_axis + local_axis;
    const auto base = static_cast<unsigned>(local_axis * 32U);
    insert_signal(frame, base, 8, static_cast<std::uint64_t>(mode[axis]));
    insert_signal(
        frame,
        base + 8U,
        16,
        encode_physical(angle_deg[axis], 0.1, -1575.0, 16, -1575.0, 1575.0));
    insert_signal(
        frame,
        base + 24U,
        8,
        encode_physical(speed_degps[axis], 1.0, 0.0, 8, 0.0, 255.0));
  }
  return frame;
}

CanFrame make_brake_frame(
    std::uint32_t id,
    std::size_t first_brake,
    const std::array<int, kBrakeCount>& mode,
    const std::array<double, kBrakeCount>& pressure_bar) {
  CanFrame frame{id};
  for (std::size_t local_brake = 0; local_brake < 4; ++local_brake) {
    const auto brake = first_brake + local_brake;
    const auto base = static_cast<unsigned>(local_brake * 16U);
    insert_signal(frame, base, 4, static_cast<std::uint64_t>(mode[brake]));
    insert_signal(
        frame,
        base + 4U,
        12,
        encode_physical(pressure_bar[brake], 0.1, 0.0, 12, 0.0, 409.5));
  }
  return frame;
}

CanFrame make_epb_frame(
    const std::array<int, kParkingBrakeCount>& requests,
    int steering_mode) {
  CanFrame frame{ids::kAduEpb};
  for (std::size_t index = 0; index < requests.size(); ++index) {
    insert_signal(
        frame,
        static_cast<unsigned>(index * 8U),
        2,
        static_cast<std::uint64_t>(requests[index]));
  }
  insert_signal(frame, 32, 3, static_cast<std::uint64_t>(steering_mode));
  return frame;
}

CanFrame make_shake_frame(int gear, int handshake_request, bool fault_reset) {
  CanFrame frame{ids::kAduShake};
  insert_signal(frame, 0, 8, static_cast<std::uint64_t>(handshake_request));
  insert_signal(frame, 8, 8, static_cast<std::uint64_t>(gear));
  insert_signal(frame, 16, 8, kNoRequest);
  insert_signal(frame, 24, 1, fault_reset ? 1U : 0U);
  return frame;
}

std::size_t find_id(
    const std::uint32_t id,
    const std::array<std::uint32_t, kMotorCount>& ids_to_search) {
  const auto found = std::find(ids_to_search.begin(), ids_to_search.end(), id);
  return found == ids_to_search.end()
      ? ids_to_search.size()
      : static_cast<std::size_t>(std::distance(ids_to_search.begin(), found));
}

std::size_t find_axis_id(
    const std::uint32_t id,
    const std::array<std::uint32_t, kSteeringAxisCount>& ids_to_search) {
  const auto found = std::find(ids_to_search.begin(), ids_to_search.end(), id);
  return found == ids_to_search.end()
      ? ids_to_search.size()
      : static_cast<std::size_t>(std::distance(ids_to_search.begin(), found));
}

}  // namespace

ParallelController::ParallelController() { reset(); }

void ParallelController::reset() {
  desired_ = Command{};
  emergency_ = Command{};
  emergency_.gear = kNeutralGear;
  emergency_.brake_pressure_bar.fill(409.5);
  feedback_ = Feedback{};
  state_ = State::Standby;
  initial_frame_count_ = 0;
  emergency_stop_ = false;
  receive_generation_ = 0;
  state_entry_generation_ = 0;
  handshake_generation_ = 0;
  gear_generation_ = 0;
  speed_generation_ = 0;
  parking_brake_generation_ = 0;
  motor_mode_generation_.fill(0);
  motor_torque_generation_.fill(0);
  steering_generation_.fill(0);
  brake_generation_.fill(0);
}

bool ParallelController::set_command(const Command& command) {
  if (!command_valid(command) ||
      state_ == State::DisarmTorque ||
      state_ == State::DisarmStop ||
      state_ == State::DisarmNeutral ||
      state_ == State::DisarmParkingBrake ||
      state_ == State::DisarmManual ||
      state_ == State::Fault) {
    return false;
  }
  const bool gear_changed = desired_.gear != command.gear;
  desired_ = command;
  emergency_stop_ = false;
  if (gear_changed && state_ == State::Ready) enter(State::WaitGear);
  return true;
}

bool ParallelController::set_emergency_command(const Command& command) {
  if (!command_valid(command) || command.gear != kNeutralGear) return false;
  emergency_ = command;
  emergency_.motor_torque_nm.fill(0.0);
  emergency_.motor_speed_rpm.fill(0.0);
  return true;
}

void ParallelController::emergency_stop() { emergency_stop_ = true; }

void ParallelController::clear_emergency_stop() { emergency_stop_ = false; }

bool ParallelController::request_parallel_handshake() {
  if ((state_ != State::Standby && state_ != State::Disarmed) ||
      !parking_ready()) {
    return false;
  }
  desired_ = Command{};
  desired_.gear = kNeutralGear;
  emergency_stop_ = false;
  initial_frame_count_ = 0;
  enter(State::Initial);
  return true;
}

void ParallelController::request_disarm() {
  if (state_ == State::Standby || state_ == State::Disarmed) return;
  emergency_stop_ = true;
  if (state_ == State::Initial) {
    enter(State::Disarmed);
    return;
  }
  if (state_ == State::WaitParallelHandshake) {
    enter(State::DisarmManual);
    return;
  }
  if (state_ == State::DisarmTorque ||
      state_ == State::DisarmStop ||
      state_ == State::DisarmNeutral ||
      state_ == State::DisarmParkingBrake ||
      state_ == State::DisarmManual ||
      state_ == State::Disarmed) {
    return;
  }
  enter(State::DisarmTorque);
}

void ParallelController::transport_fault() {
  emergency_stop_ = true;
  enter(State::Fault);
}

bool ParallelController::ingest(const CanFrame& frame) {
  if (!frame.extended || frame.dlc < 8) return false;

  if (frame.id == ids::kWvcuHandshake) {
    handshake_generation_ = ++receive_generation_;
    feedback_.handshake_status = static_cast<int>(extract_signal(frame, 8, 8));
    feedback_.handshake_valid = true;
    return true;
  }
  if (frame.id == ids::kWvcuVehicleStatus) {
    gear_generation_ = ++receive_generation_;
    feedback_.emergency_switch = static_cast<int>(extract_signal(frame, 0, 2));
    feedback_.gear = static_cast<int>(extract_signal(frame, 2, 2));
    feedback_.gear_valid = true;
    return true;
  }
  if (frame.id == ids::kWvcuVehicleSpeed) {
    speed_generation_ = ++receive_generation_;
    const auto speed_kph = decode_physical(extract_signal(frame, 0, 16), 0.1, -500.0);
    feedback_.speed_mps = speed_kph / 3.6;
    feedback_.speed_valid = true;
    return true;
  }
  if (frame.id == ids::kWvcuDriverIntention) {
    ++receive_generation_;
    feedback_.driver_gear_request =
        static_cast<int>(extract_signal(frame, 57, 3));
    feedback_.driver_gear_request_valid = true;
    return true;
  }
  if (frame.id == ids::kWvcuParkingBrake) {
    parking_brake_generation_ = ++receive_generation_;
    for (std::size_t index = 0; index < kParkingBrakeCount; ++index) {
      feedback_.parking_brake_status[index] =
          static_cast<int>(extract_signal(frame, static_cast<unsigned>(index * 16U), 2));
      feedback_.parking_brake_valid[index] = true;
    }
    return true;
  }

  const auto motor_status01_index = find_id(frame.id, kMotorStatus01Ids);
  if (motor_status01_index < kMotorCount) {
    motor_torque_generation_[motor_status01_index] = ++receive_generation_;
    feedback_.motor_speed_rpm[motor_status01_index] =
        decode_physical(extract_signal(frame, 0, 14), 1.0, -8000.0);
    feedback_.motor_speed_valid[motor_status01_index] =
        extract_signal(frame, 15, 1) == 1U;
    feedback_.motor_torque_nm[motor_status01_index] =
        decode_physical(extract_signal(frame, 16, 14), 0.1, -800.0);
    feedback_.motor_torque_valid[motor_status01_index] = true;
    return true;
  }

  const auto motor_status02_index = find_id(frame.id, kMotorStatus02Ids);
  if (motor_status02_index < kMotorCount) {
    motor_mode_generation_[motor_status02_index] = ++receive_generation_;
    feedback_.motor_mode[motor_status02_index] =
        static_cast<int>(extract_signal(frame, 4, 4));
    feedback_.motor_mode_valid[motor_status02_index] = true;
    return true;
  }

  const auto steering_index = find_axis_id(frame.id, kSteeringStatusIds);
  if (steering_index < kSteeringAxisCount) {
    steering_generation_[steering_index] = ++receive_generation_;
    feedback_.steering_mode[steering_index] =
        static_cast<int>(extract_signal(frame, 0, 8));
    feedback_.steering_angle_deg[steering_index] =
        decode_physical(extract_signal(frame, 8, 16), 0.1, -1575.0);
    feedback_.steering_valid[steering_index] = true;
    return true;
  }

  const auto brake_frame_index = find_axis_id(frame.id, kBrakeStatusIds);
  if (brake_frame_index < kSteeringAxisCount) {
    brake_generation_[brake_frame_index] = ++receive_generation_;
    for (std::size_t local_brake = 0; local_brake < 2; ++local_brake) {
      const auto brake_index = brake_frame_index * 2U + local_brake;
      const auto base = static_cast<unsigned>(local_brake * 32U);
      feedback_.brake_mode[brake_index] =
          static_cast<int>(extract_signal(frame, base, 4));
      feedback_.brake_pressure_bar[brake_index] =
          decode_physical(extract_signal(frame, base + 4U, 12), 0.1, 0.0);
      feedback_.brake_valid[brake_index] = true;
    }
    return true;
  }
  return false;
}

void ParallelController::enter(State state) {
  state_ = state;
  state_entry_generation_ = receive_generation_;
}

void ParallelController::advance_state() {
  if (state_ == State::Ready && feedback_.handshake_valid &&
      feedback_.handshake_status != kIntelligentHandshakeStatus) {
    transport_fault();
    return;
  }

  switch (state_) {
    case State::Standby:
      break;
    case State::Initial:
      if (initial_frame_count_ >= 5) {
        enter(State::WaitParallelHandshake);
      } else {
        ++initial_frame_count_;
      }
      break;
    case State::WaitParallelHandshake:
      if (feedback_.handshake_valid &&
          handshake_generation_ > state_entry_generation_ &&
          feedback_.handshake_status == kIntelligentHandshakeStatus) {
        enter(State::WaitParkingBrakeReleased);
      }
      break;
    case State::WaitParkingBrakeReleased:
      if (parking_brake_generation_ > state_entry_generation_ &&
          all_equal_valid(
              feedback_.parking_brake_status,
              feedback_.parking_brake_valid,
              kParkingBrakeRelease)) {
        enter(State::WaitGear);
      }
      break;
    case State::WaitGear:
      if (feedback_.gear_valid && gear_generation_ > state_entry_generation_ &&
          feedback_.gear == desired_.gear) {
        enter(State::WaitActuatorModes);
      }
      break;
    case State::WaitActuatorModes:
      if (all_newer_than(motor_mode_generation_, state_entry_generation_) &&
          all_newer_than(motor_torque_generation_, state_entry_generation_) &&
          all_newer_than(steering_generation_, state_entry_generation_) &&
          all_newer_than(brake_generation_, state_entry_generation_) &&
          all_equal_valid(feedback_.motor_mode, feedback_.motor_mode_valid, kMotorTorqueMode) &&
          all_valid(feedback_.motor_torque_valid) &&
          all_equal_valid(feedback_.steering_mode, feedback_.steering_valid, kByWireMode) &&
          all_equal_valid(feedback_.brake_mode, feedback_.brake_valid, kByWireMode)) {
        enter(State::Ready);
      }
      break;
    case State::Ready:
      break;
    case State::DisarmTorque: {
      bool torque_zero =
          all_valid(feedback_.motor_torque_valid) &&
          all_newer_than(motor_torque_generation_, state_entry_generation_);
      for (const auto torque : feedback_.motor_torque_nm) {
        torque_zero = torque_zero && std::abs(torque) <= kTorqueZeroToleranceNm;
      }
      if (torque_zero) enter(State::DisarmStop);
      break;
    }
    case State::DisarmStop:
      if (feedback_.speed_valid && speed_generation_ > state_entry_generation_ &&
          std::abs(feedback_.speed_mps) <= kStoppedSpeedToleranceMps) {
        enter(State::DisarmNeutral);
      }
      break;
    case State::DisarmNeutral:
      if (feedback_.gear_valid && gear_generation_ > state_entry_generation_ &&
          feedback_.gear == kNeutralGear) {
        enter(State::DisarmParkingBrake);
      }
      break;
    case State::DisarmParkingBrake:
      if (parking_brake_generation_ > state_entry_generation_ &&
          all_equal_valid(
              feedback_.parking_brake_status,
              feedback_.parking_brake_valid,
              kParkingBrakePark)) {
        enter(State::DisarmManual);
      }
      break;
    case State::DisarmManual:
      if (feedback_.handshake_valid &&
          handshake_generation_ > state_entry_generation_ &&
          feedback_.handshake_status == kManualHandshakeStatus) {
        enter(State::Disarmed);
      }
      break;
    case State::Disarmed:
    case State::Fault:
      break;
  }
}

std::vector<CanFrame> ParallelController::tick() {
  advance_state();

  int gear = kNoRequest;
  int handshake_request = kNoRequest;
  int steering_mode = kNoRequest;
  std::array<int, kParkingBrakeCount> parking_brake{};
  parking_brake.fill(kParkingBrakeHold);
  std::array<int, kMotorCount> motor_command{};
  std::array<int, kMotorCount> motor_mode{};
  std::array<int, kSteeringAxisCount> steering_axis_mode{};
  std::array<int, kBrakeCount> brake_mode{};
  std::array<double, kMotorCount> motor_torque{};
  std::array<double, kMotorCount> motor_speed{};
  std::array<double, kSteeringAxisCount> steering_angle{};
  std::array<double, kSteeringAxisCount> steering_speed{};
  std::array<double, kBrakeCount> brake_pressure{};
  bool fault_reset = false;

  const bool driving_authority_requested =
      state_ != State::Standby && state_ != State::Initial &&
      state_ != State::Disarmed &&
      state_ != State::DisarmManual;
  if (driving_authority_requested) {
    handshake_request = kIntelligentHandshakeRequest;
  }

  if (state_ == State::WaitParkingBrakeReleased ||
      state_ == State::WaitGear ||
      state_ == State::WaitActuatorModes ||
      state_ == State::Ready ||
      state_ == State::DisarmTorque ||
      state_ == State::DisarmStop ||
      state_ == State::DisarmNeutral) {
    parking_brake.fill(kParkingBrakeRelease);
    steering_mode = kByWireMode;
  }

  if (state_ == State::WaitGear ||
      state_ == State::WaitActuatorModes ||
      state_ == State::Ready ||
      state_ == State::DisarmTorque ||
      state_ == State::DisarmStop) {
    gear = desired_.gear;
    steering_axis_mode.fill(kByWireMode);
    brake_mode.fill(kByWireMode);
  }

  if (state_ == State::WaitActuatorModes ||
      state_ == State::Ready ||
      state_ == State::DisarmTorque ||
      state_ == State::DisarmStop) {
    motor_command.fill(kMotorEnable);
    motor_mode.fill(kMotorTorqueMode);
  }

  if (state_ == State::Ready) {
    motor_torque = desired_.motor_torque_nm;
    motor_speed = desired_.motor_speed_rpm;
    steering_angle = desired_.steering_angle_deg;
    steering_speed = desired_.steering_speed_degps;
    brake_pressure = desired_.brake_pressure_bar;
    fault_reset = desired_.fault_reset;
  }

  const bool safety_brake =
      emergency_stop_ || feedback_.emergency_switch != 0 ||
      state_ == State::Fault ||
      state_ == State::DisarmTorque ||
      state_ == State::DisarmStop;
  if (safety_brake) {
    motor_torque.fill(0.0);
    motor_speed.fill(0.0);
    brake_pressure = emergency_.brake_pressure_bar;
    if (state_ == State::Ready || state_ == State::Fault ||
        state_ == State::DisarmTorque || state_ == State::DisarmStop) {
      motor_command.fill(kMotorEnable);
      motor_mode.fill(kMotorTorqueMode);
      steering_axis_mode.fill(kByWireMode);
      brake_mode.fill(kByWireMode);
      steering_angle = desired_.steering_angle_deg;
      steering_speed = desired_.steering_speed_degps;
    }
  }

  if (state_ == State::DisarmNeutral) {
    gear = kNeutralGear;
    motor_command.fill(kMotorEnable);
    motor_mode.fill(kMotorTorqueMode);
    steering_axis_mode.fill(kByWireMode);
    brake_mode.fill(kByWireMode);
    brake_pressure = emergency_.brake_pressure_bar;
  } else if (state_ == State::DisarmParkingBrake) {
    gear = kNeutralGear;
    parking_brake.fill(kParkingBrakePark);
  } else if (state_ == State::DisarmManual) {
    gear = kNeutralGear;
    handshake_request = kNoRequest;
    parking_brake.fill(kParkingBrakePark);
  }

  std::vector<CanFrame> frames;
  frames.reserve(kTransmitFrameCount);
  for (std::size_t index = 0; index < kMotorCount; ++index) {
    frames.push_back(make_motor_frame(
        index,
        motor_command[index],
        motor_mode[index],
        motor_torque[index],
        motor_speed[index]));
  }
  frames.push_back(make_steering_frame(
      ids::kAduEps01, 0, steering_axis_mode, steering_angle, steering_speed));
  frames.push_back(make_steering_frame(
      ids::kAduEps03, 2, steering_axis_mode, steering_angle, steering_speed));
  frames.push_back(make_brake_frame(
      ids::kAduEhb01, 0, brake_mode, brake_pressure));
  frames.push_back(make_brake_frame(
      ids::kAduEhb02, 4, brake_mode, brake_pressure));
  frames.push_back(make_epb_frame(parking_brake, steering_mode));
  frames.push_back(make_shake_frame(gear, handshake_request, fault_reset));
  frames.push_back(CanFrame{ids::kAduBody});

  CanFrame vehicle_speed{ids::kAduVehicleSpeed};
  insert_signal(vehicle_speed, 0, 8, 0);
  insert_signal(vehicle_speed, 8, 8, 0);
  frames.push_back(vehicle_speed);
  return frames;
}

State ParallelController::state() const { return state_; }

bool ParallelController::ready() const { return state_ == State::Ready; }

bool ParallelController::disarmed() const {
  return state_ == State::Standby || state_ == State::Disarmed;
}

bool ParallelController::handshake_requested() const {
  return state_ != State::Standby && state_ != State::Disarmed &&
         state_ != State::Fault;
}

bool ParallelController::parking_ready() const {
  return feedback_.driver_gear_request_valid &&
         feedback_.driver_gear_request == kNeutralGearRequest &&
         feedback_.handshake_valid &&
         feedback_.handshake_status == kManualHandshakeStatus &&
         feedback_.speed_valid &&
         std::abs(feedback_.speed_mps) <= kStoppedSpeedToleranceMps &&
         all_equal_valid(
             feedback_.parking_brake_status,
             feedback_.parking_brake_valid,
             kParkingBrakePark);
}

bool ParallelController::feedback_complete() const {
  return feedback_.handshake_valid && feedback_.gear_valid &&
         feedback_.driver_gear_request_valid && feedback_.speed_valid &&
         all_valid(feedback_.parking_brake_valid) &&
         all_valid(feedback_.motor_mode_valid) &&
         all_valid(feedback_.motor_torque_valid) &&
         all_valid(feedback_.steering_valid) &&
         all_valid(feedback_.brake_valid);
}

const Feedback& ParallelController::feedback() const { return feedback_; }

const char* state_name(State state) {
  switch (state) {
    case State::Standby:
      return "standby";
    case State::Initial:
      return "initial";
    case State::WaitParallelHandshake:
      return "wait_parallel_handshake";
    case State::WaitParkingBrakeReleased:
      return "wait_parking_brake_released";
    case State::WaitGear:
      return "wait_gear";
    case State::WaitActuatorModes:
      return "wait_actuator_modes";
    case State::Ready:
      return "ready";
    case State::DisarmTorque:
      return "disarm_torque";
    case State::DisarmStop:
      return "disarm_stop";
    case State::DisarmNeutral:
      return "disarm_neutral";
    case State::DisarmParkingBrake:
      return "disarm_parking_brake";
    case State::DisarmManual:
      return "disarm_manual";
    case State::Disarmed:
      return "disarmed";
    case State::Fault:
      return "fault";
  }
  return "unknown";
}

}  // namespace mine_teleop::vcu
