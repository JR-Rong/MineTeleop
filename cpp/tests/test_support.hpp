#pragma once

#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mine_teleop::test {

struct SourceLocation {
  const char* file;
  int line;
};

class TestFailure : public std::runtime_error {
 public:
  TestFailure(SourceLocation location, std::string_view message)
      : std::runtime_error(
            std::string(location.file) + ":" + std::to_string(location.line) + ": " +
            std::string(message)) {}
};

inline void expect(bool condition, std::string_view message, SourceLocation location) {
  if (!condition) throw TestFailure(location, message);
}

template <typename Function>
void expect_throws(Function&& function, std::string_view message, SourceLocation location) {
  try {
    std::forward<Function>(function)();
  } catch (const std::exception&) {
    return;
  }
  throw TestFailure(location, message);
}

}  // namespace mine_teleop::test

// C++17 has no std::source_location. These macros preserve the current
// expect/expect_throws style while attaching the call site to a failure.
#define MINE_TELEOP_EXPECT(condition, message) \
  ::mine_teleop::test::expect( \
      static_cast<bool>(condition), (message), {__FILE__, __LINE__})

#define MINE_TELEOP_EXPECT_THROWS(function, message) \
  ::mine_teleop::test::expect_throws( \
      (function), (message), {__FILE__, __LINE__})
