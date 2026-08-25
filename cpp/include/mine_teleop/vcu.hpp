#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mine_teleop::vcu {

constexpr std::size_t kMotorCount = 8;
constexpr std::size_t kSteeringAxisCount = 4;
constexpr std::size_t kBrakeCount = 8;
constexpr std::size_t kParkingBrakeCount = 4;
constexpr std::size_t kTransmitFrameCount = 16;
constexpr int kTransmitPeriodMs = 20;

namespace ids {

constexpr std::uint32_t kAduMcu01 = 0x18F0D0F5U;
constexpr std::uint32_t kAduEps01 = 0x18F8D0F5U;
constexpr std::uint32_t kAduEps03 = 0x18F9D0F5U;
constexpr std::uint32_t kAduEhb01 = 0x18FFD0F5U;
constexpr std::uint32_t kAduEhb02 = 0x18FAD0F5U;
constexpr std::uint32_t kAduEpb = 0x18FBD0F5U;
constexpr std::uint32_t kAduShake = 0x18FCD0F5U;
constexpr std::uint32_t kAduBody = 0x18FDD0F5U;
constexpr std::uint32_t kAduVehicleSpeed = 0x18FED0F5U;

constexpr std::uint32_t kWvcuHandshake = 0x18F0F5D0U;
constexpr std::uint32_t kWvcuVehicleStatus = 0x18F2F5D0U;
constexpr std::uint32_t kWvcuVehicleSpeed = 0x18F3F5D0U;
constexpr std::uint32_t kWvcuDriverIntention = 0x18F5F5D0U;
constexpr std::uint32_t kWvcuParkingBrake = 0x18CFF4D0U;

}  // namespace ids

struct CanFrame {
  std::uint32_t id{0};
  std::uint8_t dlc{8};
  std::array<std::uint8_t, 8> data{};
  bool extended{true};
};

struct Command {
  int gear{1};
  double vehicle_speed_request_kph{0.0};
  bool vehicle_speed_request_valid{false};
  std::array<double, kMotorCount> motor_torque_nm{};
  std::array<double, kMotorCount> motor_speed_rpm{};
  std::array<double, kSteeringAxisCount> steering_angle_deg{};
  std::array<double, kSteeringAxisCount> steering_speed_degps{};
  std::array<double, kBrakeCount> brake_pressure_bar{};
  bool fault_reset{false};
};

struct Feedback {
  int handshake_status{0};
  bool handshake_valid{false};

  int gear{0};
  bool gear_valid{false};
  int emergency_switch{0};

  int driver_gear_request{0};
  bool driver_gear_request_valid{false};

  double speed_mps{0.0};
  bool speed_valid{false};

  std::array<int, kParkingBrakeCount> parking_brake_status{};
  std::array<bool, kParkingBrakeCount> parking_brake_valid{};

  std::array<int, kMotorCount> motor_mode{};
  std::array<bool, kMotorCount> motor_mode_valid{};
  std::array<double, kMotorCount> motor_torque_nm{};
  std::array<bool, kMotorCount> motor_torque_valid{};
  std::array<double, kMotorCount> motor_speed_rpm{};
  std::array<bool, kMotorCount> motor_speed_valid{};

  std::array<int, kSteeringAxisCount> steering_mode{};
  std::array<bool, kSteeringAxisCount> steering_valid{};
  std::array<double, kSteeringAxisCount> steering_angle_deg{};

  std::array<int, kBrakeCount> brake_mode{};
  std::array<bool, kBrakeCount> brake_valid{};
  std::array<double, kBrakeCount> brake_pressure_bar{};
};

enum class State {
  Standby,
  Initial,
  WaitParallelHandshake,
  WaitParkingBrakeReleased,
  WaitGear,
  WaitActuatorModes,
  Ready,
  DisarmTorque,
  DisarmStop,
  DisarmNeutral,
  DisarmParkingBrake,
  DisarmManual,
  Disarmed,
  Fault,
};

class ParallelController {
 public:
  ParallelController();

  void reset();
  bool set_command(const Command& command);
  bool set_emergency_command(const Command& command);
  void emergency_stop();
  void clear_emergency_stop();
  bool request_parallel_handshake();
  void request_disarm();
  void transport_fault();

  bool ingest(const CanFrame& frame);
  [[nodiscard]] std::vector<CanFrame> tick();

  [[nodiscard]] State state() const;
  [[nodiscard]] bool ready() const;
  [[nodiscard]] bool disarmed() const;
  [[nodiscard]] bool handshake_requested() const;
  [[nodiscard]] bool parking_ready() const;
  [[nodiscard]] bool physical_emergency_latched() const;
  [[nodiscard]] bool feedback_complete() const;
  [[nodiscard]] const Feedback& feedback() const;

 private:
  void advance_state();
  bool begin_arming_emergency_disarm();
  void enter(State state);

  Command desired_{};
  Command emergency_{};
  Feedback feedback_{};
  State state_{State::Standby};
  int initial_frame_count_{0};
  bool emergency_stop_{false};
  bool physical_emergency_latched_{false};
  std::uint64_t receive_generation_{0};
  std::uint64_t state_entry_generation_{0};
  std::uint64_t handshake_generation_{0};
  std::uint64_t gear_generation_{0};
  std::uint64_t speed_generation_{0};
  std::uint64_t parking_brake_generation_{0};
  std::array<std::uint64_t, kMotorCount> motor_mode_generation_{};
  std::array<std::uint64_t, kMotorCount> motor_torque_generation_{};
  std::array<std::uint64_t, kSteeringAxisCount> steering_generation_{};
  std::array<std::uint64_t, kSteeringAxisCount> brake_generation_{};
};

[[nodiscard]] const char* state_name(State state);

}  // namespace mine_teleop::vcu
