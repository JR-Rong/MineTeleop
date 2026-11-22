#pragma once

#include <cstdint>

namespace mine_teleop::detail {

// Once invalidated, a profile cannot become authoritative again from a late
// acknowledgement. A new request must be explicitly initiated by the user.
class NativeMockProfileState {
 public:
  enum class Result { ignored, accepted, invalidated };
  bool begin(std::uint64_t request, bool manual) {
    if (!request || request > 9007199254740991ULL || request <= request_ || (invalidated_ && !manual)) return false;
    request_ = request; pending_ = true; invalidated_ = false;
    return true;
  }
  Result observe(std::uint64_t status, std::uint64_t request, bool valid) {
    if (!status || status > 9007199254740991ULL || status <= status_) return Result::ignored;
    status_ = status;
    if (invalidated_ || !request_) return Result::ignored;
    if (pending_ && request != request_) return Result::ignored;
    if (request != request_ || !valid) {
      pending_ = false; invalidated_ = true;
      return Result::invalidated;
    }
    pending_ = false;
    return Result::accepted;
  }
 private:
  std::uint64_t request_{0}, status_{0};
  bool pending_{false}, invalidated_{false};
};
}  // namespace mine_teleop::detail
