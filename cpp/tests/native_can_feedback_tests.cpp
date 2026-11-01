#include "mine_teleop/detail/native_can_feedback.hpp"
#include <iostream>
#include <stdexcept>

int main() {
  using mine_teleop::detail::native_can_feedback_text;
  using Json = nlohmann::json;
  try {
    const Json input{{"supported", true}, {"feedback_fresh", true},
        {"speed_valid", true}, {"speed_mps", 12.3}, {"gear_valid", false}, {"gear", 999},
        {"motor_torque_valid", {true, false}}, {"motor_torque_nm", {45.6, 999.0}},
        {"steering_valid", {false}}, {"steering_angle_deg", {999.0}}};
    const auto fresh = native_can_feedback_text(input, true);
    if (fresh.find(L"12.30") == std::wstring::npos || fresh.find(L"45.6") == std::wstring::npos ||
        fresh.find(L"999") != std::wstring::npos) throw std::runtime_error("CAN field validity was not respected");
    if (native_can_feedback_text(input, false).find(L"12.30") != std::wstring::npos)
      throw std::runtime_error("stale telemetry retained CAN values");
    auto unsupported = input; unsupported["supported"] = false;
    if (native_can_feedback_text(unsupported, true).find(L"12.30") != std::wstring::npos)
      throw std::runtime_error("unsupported CAN displayed fabricated values");
    auto stale = input; stale["feedback_fresh"] = false;
    if (native_can_feedback_text(stale, true).find(L"12.30") != std::wstring::npos)
      throw std::runtime_error("stale CAN retained values");
    std::cout << "CAN validity and freshness checks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
