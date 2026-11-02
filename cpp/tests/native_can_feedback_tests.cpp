#include "mine_teleop/detail/native_can_feedback.hpp"
#include <iostream>
#include <stdexcept>

int main() {
  using mine_teleop::detail::native_can_feedback_text;
  using mine_teleop::detail::native_feedback_display_gear;
  using mine_teleop::detail::native_feedback_display_speed;
  using mine_teleop::detail::native_feedback_confirmed_gear;
  using Json = nlohmann::json;
  try {
    const Json input{{"supported", true}, {"feedback_fresh", true},
        {"speed_valid", true}, {"speed_mps", 12.3}, {"gear_valid", false}, {"gear", 999},
        {"motor_torque_valid", {true, false}}, {"motor_torque_nm", {45.6, 999.0}},
        {"steering_valid", {false}}, {"steering_angle_deg", {999.0}}};
    const auto fresh = native_can_feedback_text(input, true);
    if (fresh.find(L"12.30") == std::wstring::npos || fresh.find(L"45.6") == std::wstring::npos ||
        fresh.find(L"999") != std::wstring::npos) throw std::runtime_error("CAN field validity was not respected");
    auto valid_gear = input; valid_gear["gear_valid"] = true; valid_gear["gear"] = 3;
    if (native_can_feedback_text(valid_gear, true).find(L"D") == std::wstring::npos)
      throw std::runtime_error("valid drive gear was not displayed");
    if (native_can_feedback_text(input, false).find(L"12.30") != std::wstring::npos)
      throw std::runtime_error("stale telemetry retained CAN values");
    auto parking = input;
    parking["parking_brake_status"] = {0, 1, 2, 9};
    parking["parking_brake_valid"] = {true, true, true, false};
    if (native_can_feedback_text(parking, true).find(L"驻车状态：保持 / 释放 / 已拉起 / —") == std::wstring::npos)
      throw std::runtime_error("four parking channels or their validity were not displayed");
    parking["parking_brake_valid"][3] = true;
    if (native_can_feedback_text(parking, true).find(L"驻车状态：保持 / 释放 / 已拉起 / 异常(9)") == std::wstring::npos)
      throw std::runtime_error("fourth parking channel was not displayed");
    auto unsupported = input; unsupported["supported"] = false;
    if (native_can_feedback_text(unsupported, true).find(L"12.30") != std::wstring::npos)
      throw std::runtime_error("unsupported CAN displayed fabricated values");
    auto stale = input; stale["feedback_fresh"] = false;
    if (native_can_feedback_text(stale, true).find(L"12.30") != std::wstring::npos)
      throw std::runtime_error("stale CAN retained values");
    Json measured{{"speed_mps", 1.0}, {"can_feedback", input}};
    if (native_feedback_display_speed(measured) != 12.3)
      throw std::runtime_error("valid CAN speed was not preferred for display");
    measured["can_feedback"]["speed_valid"] = false;
    if (native_feedback_display_speed(measured) != 1.0)
      throw std::runtime_error("telemetry speed fallback was not displayed");
    measured["can_feedback"]["feedback_fresh"] = false;
    if (native_feedback_display_speed(measured))
      throw std::runtime_error("stale supported CAN speed was displayed");
    measured["can_feedback"] = {{"supported", false}};
    if (native_feedback_display_speed(measured) != 1.0)
      throw std::runtime_error("mock telemetry speed was not displayed");
    measured["gear"] = "R";
    measured["can_feedback"] = valid_gear;
    if (native_feedback_display_gear(measured) != 'D')
      throw std::runtime_error("valid CAN gear was not preferred for display");
    measured["can_feedback"]["gear_valid"] = false;
    if (native_feedback_display_gear(measured) != 'R')
      throw std::runtime_error("telemetry gear fallback was not displayed");
    measured["can_feedback"]["feedback_fresh"] = false;
    if (native_feedback_display_gear(measured))
      throw std::runtime_error("stale supported CAN gear was displayed");
    measured["can_feedback"] = {{"supported", false}};
    measured["gear"] = "P";
    if (native_feedback_display_gear(measured) != 'P')
      throw std::runtime_error("parked telemetry gear was not displayed");
    Json telemetry{{"gear", "R"}, {"can_feedback", {{"supported", false}}}};
    if (native_feedback_confirmed_gear(telemetry) != 'R')
      throw std::runtime_error("mock gear feedback was not admitted");
    telemetry["can_feedback"] = valid_gear;
    if (native_feedback_confirmed_gear(telemetry) != 'D')
      throw std::runtime_error("fresh valid CAN gear was not selected over mock gear");
    telemetry["can_feedback"]["feedback_fresh"] = false;
    if (native_feedback_confirmed_gear(telemetry)) throw std::runtime_error("stale CAN gear admitted");
    telemetry["can_feedback"]["feedback_fresh"] = true;
    telemetry["can_feedback"]["gear_valid"] = false;
    if (native_feedback_confirmed_gear(telemetry)) throw std::runtime_error("invalid CAN gear admitted");
    telemetry["can_feedback"]["gear_valid"] = true;
    telemetry["can_feedback"]["gear"] = "3";
    if (native_feedback_confirmed_gear(telemetry)) throw std::runtime_error("malformed CAN gear admitted");
    telemetry["can_feedback"] = {{"supported", nullptr}};
    if (native_feedback_confirmed_gear(telemetry)) throw std::runtime_error("unknown CAN capability admitted");
    std::cout << "CAN validity and freshness checks passed\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
