#pragma once

#include <nlohmann/json.hpp>
#include <cmath>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace mine_teleop::detail {

// Prefer the same measured CAN speed as the web console when it is supported
// and valid. A stale supported CAN feed must not be presented as current.
inline std::optional<double> native_feedback_display_speed(const nlohmann::json& telemetry) {
  if (!telemetry.is_object()) return std::nullopt;
  const auto finite = [](const nlohmann::json& value) -> std::optional<double> {
    if (!value.is_number()) return std::nullopt;
    const auto number = value.get<double>();
    return std::isfinite(number) ? std::optional<double>(number) : std::nullopt;
  };
  if (telemetry.contains("can_feedback") && telemetry.at("can_feedback").is_object()) {
    const auto& feedback = telemetry.at("can_feedback");
    const auto flag = [&](const char* key) {
      return feedback.contains(key) && feedback.at(key).is_boolean() && feedback.at(key).get<bool>();
    };
    if (flag("supported")) {
      if (!flag("feedback_fresh")) return std::nullopt;
      if (flag("speed_valid") && feedback.contains("speed_mps")) return finite(feedback.at("speed_mps"));
    }
  }
  return telemetry.contains("speed_mps") ? finite(telemetry.at("speed_mps")) : std::nullopt;
}

// Display the fresh measured CAN gear first; unlike control authorization,
// the display may also show an explicitly reported parked gear.
inline std::optional<char> native_feedback_display_gear(const nlohmann::json& telemetry) {
  if (!telemetry.is_object()) return std::nullopt;
  if (telemetry.contains("can_feedback") && telemetry.at("can_feedback").is_object()) {
    const auto& feedback = telemetry.at("can_feedback");
    const auto flag = [&](const char* key) {
      return feedback.contains(key) && feedback.at(key).is_boolean() && feedback.at(key).get<bool>();
    };
    if (flag("supported")) {
      if (!flag("feedback_fresh")) return std::nullopt;
      if (flag("gear_valid")) {
        if (!feedback.contains("gear") || !feedback.at("gear").is_number_integer()) return std::nullopt;
        if (feedback.at("gear") == 1) return 'N';
        if (feedback.at("gear") == 2) return 'R';
        if (feedback.at("gear") == 3) return 'D';
        return std::nullopt;
      }
    }
  }
  if (!telemetry.contains("gear") || !telemetry.at("gear").is_string()) return std::nullopt;
  const auto gear = telemetry.at("gear").get<std::string>();
  return gear == "N" || gear == "D" || gear == "R" || gear == "P"
      ? std::optional<char>(gear.front()) : std::nullopt;
}

// Match the web gear confirmation contract: use fresh valid CAN feedback
// when supported, otherwise use explicit mock telemetry only.
inline std::optional<char> native_feedback_confirmed_gear(const nlohmann::json& telemetry) {
  using Json = nlohmann::json;
  if (!telemetry.is_object() || !telemetry.contains("can_feedback") || !telemetry.at("can_feedback").is_object())
    return std::nullopt;
  const auto& feedback = telemetry.at("can_feedback");
  if (!feedback.contains("supported") || !feedback.at("supported").is_boolean()) return std::nullopt;
  if (!feedback.at("supported").get<bool>()) {
    if (!telemetry.contains("gear") || !telemetry.at("gear").is_string()) return std::nullopt;
    const auto gear = telemetry.at("gear").get<std::string>();
    if (gear == "N" || gear == "D" || gear == "R") return gear.front();
    return std::nullopt;
  }
  const auto flag = [&](const char* key) {
    return feedback.contains(key) && feedback.at(key).is_boolean() && feedback.at(key).get<bool>();
  };
  if (!flag("feedback_fresh") || !flag("gear_valid") || !feedback.contains("gear") ||
      !feedback.at("gear").is_number_integer()) return std::nullopt;
  const Json& gear = feedback.at("gear");
  if (gear == 1) return 'N';
  if (gear == 2) return 'R';
  if (gear == 3) return 'D';
  return std::nullopt;
}

// Missing, invalid and stale data is deliberately not presented as a value.
inline std::wstring native_can_feedback_text(const nlohmann::json& feedback, bool telemetry_fresh) {
  using Json = nlohmann::json;
  const auto flag = [](const Json& value, const char* name) {
    return value.is_object() && value.contains(name) && value.at(name).is_boolean() && value.at(name).get<bool>();
  };
  if (!telemetry_fresh || !feedback.is_object()) return L"CAN 反馈未知或遥测已过期";
  if (!flag(feedback, "supported")) return L"车端桥接包不提供完整 CAN 反馈";
  if (!flag(feedback, "feedback_fresh")) return L"CAN 反馈已过期";
  const auto format = [](const Json& value, int precision) -> std::wstring {
    if (!value.is_number()) return L"—";
    const double number = value.get<double>();
    if (!std::isfinite(number)) return L"—";
    std::wostringstream out; out << std::fixed << std::setprecision(precision) << number;
    return out.str();
  };
  const auto scalar = [&](const char* name, const char* valid, int precision = 0) {
    return flag(feedback, valid) && feedback.contains(name) ? format(feedback.at(name), precision) : std::wstring(L"—");
  };
  const auto array = [&](const char* name, const char* valid, std::size_t index, int precision = 0) -> std::wstring {
    if (!feedback.contains(name) || !feedback.at(name).is_array() || index >= feedback.at(name).size() ||
        !feedback.contains(valid) || !feedback.at(valid).is_array() || index >= feedback.at(valid).size() ||
        !feedback.at(valid)[index].is_boolean() || !feedback.at(valid)[index].get<bool>()) return L"—";
    return format(feedback.at(name)[index], precision);
  };
  const auto gear_label = [](const std::wstring& value) -> std::wstring {
    if (value == L"1") return L"N";
    if (value == L"2") return L"R";
    if (value == L"3") return L"D";
    return value == L"—" ? value : L"不支持(" + value + L")";
  };
  const auto switch_label = [](const std::wstring& value, const wchar_t* on, const wchar_t* off) -> std::wstring {
    if (value == L"1") return on;
    if (value == L"0") return off;
    return value == L"—" ? value : L"异常(" + value + L")";
  };
  const auto parking_label = [](const std::wstring& value) -> std::wstring {
    if (value == L"0") return L"保持";
    if (value == L"1") return L"释放";
    if (value == L"2") return L"已拉起";
    return value == L"—" ? value : L"异常(" + value + L")";
  };
  std::wostringstream out;
  out << L"CAN 反馈（有效且新鲜）";
  if (feedback.contains("max_feedback_age_ms") && feedback.at("max_feedback_age_ms").is_number() &&
      feedback.at("max_feedback_age_ms").get<double>() >= 0)
    out << L"  " << format(feedback.at("max_feedback_age_ms"), 0) << L" ms";
  out << L"\r\n"
      << L"车速 m/s：" << scalar("speed_mps", "speed_valid", 2)
      << L"   挡位：" << gear_label(scalar("gear", "gear_valid")) << L"\r\n"
      << L"挡位请求：" << gear_label(scalar("driver_gear_request", "driver_gear_request_valid"))
      << L"   握手：" << scalar("handshake_status", "handshake_valid") << L"\r\n"
      << L"故障码：" << scalar("vmc_fault_code", "vmc_fault_code_valid")
      << L"   急停：" << scalar("emergency_switch", "gear_valid") << L"\r\n"
      << L"驻车开关：" << switch_label(scalar("parking_brake_switch", "parking_brake_switch_valid"), L"已拉起", L"已松开")
      << L"   刹车踏板：" << switch_label(scalar("brake_pedal_switch", "brake_pedal_switch_valid"), L"已踩下", L"已松开") << L"\r\n";
  for (std::size_t i = 0; i < 8; ++i)
    out << L"轮 " << i + 1 << L"：扭矩 " << array("motor_torque_nm", "motor_torque_valid", i, 1)
        << L" Nm，转速 " << array("motor_speed_rpm", "motor_speed_valid", i)
        << L" rpm，制动 " << array("brake_pressure_bar", "brake_valid", i, 1)
        << L" bar，模式 M" << array("motor_mode", "motor_mode_valid", i)
        << L" / B" << array("brake_mode", "brake_valid", i) << L"\r\n";
  for (std::size_t i = 0; i < 4; ++i)
    out << L"转向 " << i + 1 << L"：" << array("steering_angle_deg", "steering_valid", i, 1)
        << L"°，模式 " << array("steering_mode", "steering_valid", i) << L"\r\n";
  out << L"驻车状态：";
  for (std::size_t i = 0; i < 4; ++i) {
    if (i != 0) out << L" / ";
    out << parking_label(array("parking_brake_status", "parking_brake_valid", i));
  }
  return out.str();
}

}  // namespace mine_teleop::detail
