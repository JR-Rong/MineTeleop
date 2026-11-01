#pragma once

#include <array>
#include <cmath>
#include <cwchar>
#include <iterator>
#include <optional>
#include <string>

namespace mine_teleop::detail {

// Values supplied here have already passed the caller's freshness checks.
// Missing or malformed evidence is displayed as unknown, never as zero.
struct NativeOperatorIntent {
  char gear{'N'};
  double steering{}, throttle{}, brake{};
};

inline std::array<std::wstring, 6> native_operator_readouts(
    std::optional<double> speed_mps, std::optional<char> actual_gear,
    std::optional<NativeOperatorIntent> intent) {
  std::array<std::wstring, 6> values{L"\u2014", L"\u2014", L"\u2014", L"\u2014", L"\u2014", L"\u2014"};
  const auto number = [](double value, const wchar_t* format) {
    wchar_t text[64]{};
    return std::swprintf(text, std::size(text), format, value) > 0 ? std::wstring(text) : std::wstring(L"\u2014");
  };
  if (speed_mps && std::isfinite(*speed_mps) && std::isfinite(*speed_mps * 3.6))
    values[0] = number(*speed_mps * 3.6, L"%.1f km/h");
  if (actual_gear && (*actual_gear == 'N' || *actual_gear == 'D' || *actual_gear == 'R' || *actual_gear == 'P'))
    values[1] = std::wstring(1, static_cast<wchar_t>(*actual_gear));
  if (!intent) return values;
  if (intent->gear == 'N' || intent->gear == 'D' || intent->gear == 'R')
    values[2] = std::wstring(1, static_cast<wchar_t>(intent->gear));
  if (std::isfinite(intent->steering) && std::abs(intent->steering) <= 1)
    values[3] = number(intent->steering, L"%.2f");
  if (std::isfinite(intent->throttle) && intent->throttle >= 0 && intent->throttle <= 1)
    values[4] = number(intent->throttle, L"%.2f");
  if (std::isfinite(intent->brake) && intent->brake >= 0 && intent->brake <= 1)
    values[5] = number(intent->brake, L"%.2f");
  return values;
}

}  // namespace mine_teleop::detail
