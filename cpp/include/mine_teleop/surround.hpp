#pragma once
#include "mine_teleop/core.hpp"
#include "mine_teleop/surround_math.hpp"
#include <array>
#include <filesystem>
#include <string>
namespace mine_teleop {
std::string sha256_text(std::string_view text);
struct SurroundCalibration {
  std::array<surround::Camera, 4> cameras;
  surround::Region region;
  double vehicle_length{}, vehicle_width{};
  std::string hash;
  Json document;
};
SurroundCalibration load_surround_calibration(const std::filesystem::path &,
                                              const VehicleConfig &,
                                              bool require_acceptance = true);
} // namespace mine_teleop
