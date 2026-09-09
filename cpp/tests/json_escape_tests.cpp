#include "mine_teleop/detail/json_escape.hpp"

#include <nlohmann/json.hpp>

#include <array>
#include <exception>
#include <iterator>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

using Json = nlohmann::json;

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, std::string_view message) {
  if (!condition)
    throw TestFailure(std::string(message));
}

std::string parse_json_string_contents(std::string_view escaped) {
  const auto parsed = Json::parse("\"" + std::string(escaped) + "\"");
  expect(parsed.is_string(), "escaped content did not parse as a JSON string");
  return parsed.get<std::string>();
}

void test_all_control_characters_are_lossless() {
  std::string controls;
  for (unsigned int character = 0; character < 0x20U; ++character) {
    controls.push_back(static_cast<char>(character));
  }

  const std::string expected =
      "\\u0000\\u0001\\u0002\\u0003\\u0004\\u0005\\u0006\\u0007"
      "\\b\\t\\n\\u000b\\f\\r\\u000e\\u000f"
      "\\u0010\\u0011\\u0012\\u0013\\u0014\\u0015\\u0016\\u0017"
      "\\u0018\\u0019\\u001a\\u001b\\u001c\\u001d\\u001e\\u001f";
  const auto escaped = mine_teleop::detail::json_escape(controls);
  expect(escaped == expected, "control characters did not use the intended JSON escapes");
  expect(parse_json_string_contents(escaped) == controls,
         "control character values changed after JSON parse");
}

void test_quotes_and_backslashes_are_not_double_escaped() {
  const std::string original = "\"\\n";
  const auto escaped = mine_teleop::detail::json_escape(original);
  expect(escaped == "\\\"\\\\n", "quote or backslash escape changed");
  expect(parse_json_string_contents(escaped) == original, "quote or backslash was not restored");
}

void test_empty_and_valid_utf8_values_are_preserved() {
  expect(mine_teleop::detail::json_escape("").empty(), "empty JSON string was changed");

  const std::string utf8 = u8"矿山摄像头🚜";
  const auto escaped = mine_teleop::detail::json_escape(utf8);
  expect(escaped == utf8, "valid UTF-8 was changed");
  expect(parse_json_string_contents(escaped) == utf8, "valid UTF-8 did not survive JSON parse");
}

void test_long_field_is_lossless() {
  std::string field;
  for (int index = 0; index < 4096; ++index)
    field += u8"矿";
  field += "\nend";

  const auto escaped = mine_teleop::detail::json_escape(field);
  expect(parse_json_string_contents(escaped) == field, "long JSON field changed after parsing");
}

void test_utf8_boundaries_are_preserved() {
  const std::array<std::string, 6> valid_utf8_values{
      std::string("\xc2\x80", 2),         std::string("\xe0\xa0\x80", 3),
      std::string("\xed\x9f\xbf", 3),     std::string("\xee\x80\x80", 3),
      std::string("\xf0\x90\x80\x80", 4), std::string("\xf4\x8f\xbf\xbf", 4),
  };

  for (const auto& value : valid_utf8_values) {
    const auto escaped = mine_teleop::detail::json_escape(value);
    expect(escaped == value, "a valid UTF-8 boundary value changed");
    expect(parse_json_string_contents(escaped) == value, "a valid UTF-8 boundary did not parse");
  }
}

void test_invalid_utf8_is_explicitly_replaced() {
  struct InvalidUtf8Case {
    std::string value;
    std::string escaped;
    std::string parsed;
  };
  const std::array<InvalidUtf8Case, 6> invalid_utf8_values{
      InvalidUtf8Case{std::string("\x80", 1), "\\ufffd", std::string("\xef\xbf\xbd", 3)},
      InvalidUtf8Case{std::string("\xc0\xaf", 2), "\\ufffd\\ufffd",
                      std::string("\xef\xbf\xbd\xef\xbf\xbd", 6)},
      InvalidUtf8Case{std::string("\xe0\x80\x80", 3), "\\ufffd\\ufffd\\ufffd",
                      std::string("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd", 9)},
      InvalidUtf8Case{std::string("\xed\xa0\x80", 3), "\\ufffd\\ufffd\\ufffd",
                      std::string("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd", 9)},
      InvalidUtf8Case{std::string("\xf4\x90\x80\x80", 4), "\\ufffd\\ufffd\\ufffd\\ufffd",
                      std::string("\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd\xef\xbf\xbd", 12)},
      InvalidUtf8Case{std::string("\xf0\x28\x8c\xbc", 4), "\\ufffd(\\ufffd\\ufffd",
                      std::string("\xef\xbf\xbd(\xef\xbf\xbd\xef\xbf\xbd", 10)},
  };

  for (const auto& invalid : invalid_utf8_values) {
    const auto escaped = mine_teleop::detail::json_escape(invalid.value);
    expect(escaped == invalid.escaped,
           "invalid UTF-8 was not represented by explicit replacement escapes");
    expect(parse_json_string_contents(escaped) == invalid.parsed,
           "invalid UTF-8 replacement output did not parse to replacement characters");
  }
}

struct TestCase {
  const char* name;
  void (*function)();
};

}  // namespace

int main() {
  const TestCase tests[] = {
      {"all_control_characters_are_lossless", test_all_control_characters_are_lossless},
      {"quotes_and_backslashes_are_not_double_escaped",
       test_quotes_and_backslashes_are_not_double_escaped},
      {"empty_and_valid_utf8_values_are_preserved", test_empty_and_valid_utf8_values_are_preserved},
      {"long_field_is_lossless", test_long_field_is_lossless},
      {"utf8_boundaries_are_preserved", test_utf8_boundaries_are_preserved},
      {"invalid_utf8_is_explicitly_replaced", test_invalid_utf8_is_explicitly_replaced},
  };

  try {
    for (const auto& test : tests)
      test.function();
    std::cout << "json_escape_tests=passed count=" << std::size(tests) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "json_escape_tests=failed error=" << error.what() << '\n';
    return 1;
  }
}
