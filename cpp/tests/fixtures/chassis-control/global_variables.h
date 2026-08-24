#pragma once

// Minimal ChassisControl ABI fixture for the unprivileged bridge smoke target.
#include <array>
#include <string>
#include <vector>

struct WheelParam {
  std::string feture_name;
  float mu{0.0F};
  float slip_threshold{0.0F};
  float wheel_width{0.0F};
  float wheel_radius{0.0F};
  float wheel_pressure{0.0F};
  float max_electric_torque{0.0F};
  float max_genera_torque{0.0F};
  std::array<float, 3> wheel_position{};
};

struct WheelState {};

struct VehicleParam {
  struct {
    int wheel_count{0};
  } vechicle_static_param;
  struct {
    float mass{0.0F};
    std::array<float, 3> cog_position{};
    std::vector<WheelParam> wheel_params;
  } vechicle_dynamic_param;
};

struct VehicleState {
  float cur_velocity{0.0F};
  std::array<float, 2> target_velocity{};
  std::array<float, 2> target_acceleration{};
  int target_gear{0};
  std::array<float, 3> target_position{};
  std::array<float, 3> vehicle_posture{};
  std::array<float, 3> vehicle_position{};
  std::vector<float> target_steering_angle;
  std::vector<WheelState> tier_state;
};

struct ControlInfo {
  double wheel_speed{0.0};
  double wheel_torque{0.0};
  double ehb_brk_pres_req{0.0};
  double eps_ang_req{0.0};
  double eps_ang_spd_req{0.0};
};

const std::vector<ControlInfo>& GetControlInfo();
bool UpdateVehicleState(const VehicleState& state);
