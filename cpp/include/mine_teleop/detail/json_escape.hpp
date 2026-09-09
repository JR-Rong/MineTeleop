#ifndef MINE_TELEOP_DETAIL_JSON_ESCAPE_HPP
#define MINE_TELEOP_DETAIL_JSON_ESCAPE_HPP

#include <cstddef>
#include <string>
#include <string_view>

namespace mine_teleop::detail {
namespace json_escape_detail {

inline bool is_utf8_continuation(unsigned char character) noexcept {
  return (character & 0xc0U) == 0x80U;
}

inline std::size_t valid_utf8_sequence_length(std::string_view value, std::size_t index) noexcept {
  const auto first = static_cast<unsigned char>(value[index]);
  const auto remaining = value.size() - index;
  const auto at = [&](std::size_t offset) {
    return static_cast<unsigned char>(value[index + offset]);
  };

  if (first >= 0xc2U && first <= 0xdfU) {
    return remaining >= 2U && is_utf8_continuation(at(1U)) ? 2U : 0U;
  }
  if (first == 0xe0U) {
    return remaining >= 3U && at(1U) >= 0xa0U && at(1U) <= 0xbfU && is_utf8_continuation(at(2U))
               ? 3U
               : 0U;
  }
  if ((first >= 0xe1U && first <= 0xecU) || (first >= 0xeeU && first <= 0xefU)) {
    return remaining >= 3U && is_utf8_continuation(at(1U)) && is_utf8_continuation(at(2U)) ? 3U
                                                                                           : 0U;
  }
  if (first == 0xedU) {
    return remaining >= 3U && at(1U) >= 0x80U && at(1U) <= 0x9fU && is_utf8_continuation(at(2U))
               ? 3U
               : 0U;
  }
  if (first == 0xf0U) {
    return remaining >= 4U && at(1U) >= 0x90U && at(1U) <= 0xbfU && is_utf8_continuation(at(2U)) &&
                   is_utf8_continuation(at(3U))
               ? 4U
               : 0U;
  }
  if (first >= 0xf1U && first <= 0xf3U) {
    return remaining >= 4U && is_utf8_continuation(at(1U)) && is_utf8_continuation(at(2U)) &&
                   is_utf8_continuation(at(3U))
               ? 4U
               : 0U;
  }
  if (first == 0xf4U) {
    return remaining >= 4U && at(1U) >= 0x80U && at(1U) <= 0x8fU && is_utf8_continuation(at(2U)) &&
                   is_utf8_continuation(at(3U))
               ? 4U
               : 0U;
  }
  return 0U;
}

inline void append_control_escape(std::string& escaped, unsigned char character) {
  constexpr char kHex[] = "0123456789abcdef";
  escaped += "\\u00";
  escaped += kHex[(character >> 4U) & 0x0fU];
  escaped += kHex[character & 0x0fU];
}

}  // namespace json_escape_detail

// Returns JSON string contents without the surrounding quotes. Valid UTF-8 is
// copied unchanged; each malformed UTF-8 byte becomes the explicit JSON
// replacement escape \\ufffd so callers never emit an invalid JSON string.
inline std::string json_escape(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());

  for (std::size_t index = 0; index < value.size();) {
    const auto character = static_cast<unsigned char>(value[index]);
    switch (character) {
      case '\"':
        escaped += "\\\"";
        ++index;
        break;
      case '\\':
        escaped += "\\\\";
        ++index;
        break;
      case '\b':
        escaped += "\\b";
        ++index;
        break;
      case '\f':
        escaped += "\\f";
        ++index;
        break;
      case '\n':
        escaped += "\\n";
        ++index;
        break;
      case '\r':
        escaped += "\\r";
        ++index;
        break;
      case '\t':
        escaped += "\\t";
        ++index;
        break;
      default:
        if (character < 0x20U) {
          json_escape_detail::append_control_escape(escaped, character);
          ++index;
        } else if (character < 0x80U) {
          escaped += static_cast<char>(character);
          ++index;
        } else {
          const auto sequence_length = json_escape_detail::valid_utf8_sequence_length(value, index);
          if (sequence_length == 0U) {
            escaped += "\\ufffd";
            ++index;
          } else {
            escaped.append(value.data() + index, sequence_length);
            index += sequence_length;
          }
        }
        break;
    }
  }
  return escaped;
}

}  // namespace mine_teleop::detail

#endif  // MINE_TELEOP_DETAIL_JSON_ESCAPE_HPP
