#pragma once

#include <string>
#include <string_view>

namespace mine_teleop::detail {

// Match ECMAScript String.trim(), which the web login applies to driver IDs.
inline bool native_identity_space(wchar_t ch) {
  return (ch >= 0x0009 && ch <= 0x000d) || ch == 0x0020 || ch == 0x00a0 || ch == 0x1680 ||
      (ch >= 0x2000 && ch <= 0x200a) || ch == 0x2028 || ch == 0x2029 || ch == 0x202f ||
      ch == 0x205f || ch == 0x3000 || ch == 0xfeff;
}

inline std::wstring native_trim_identity(std::wstring_view value) {
  std::size_t first = 0, last = value.size();
  while (first < last && native_identity_space(value[first])) ++first;
  while (first < last && native_identity_space(value[last - 1])) --last;
  return std::wstring(value.substr(first, last - first));
}

}  // namespace mine_teleop::detail
