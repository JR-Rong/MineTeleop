#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace mine_teleop::detail {

// Only a changed input or safety/scope field advances the sequence. Identical
// neutral heartbeats renew the real lease without retiring an in-flight send.
class NativeMockInputSequence {
 public:
  std::uint64_t next(const nlohmann::json& input_without_sequence) {
    if (!input_without_sequence.is_object() || input_without_sequence.contains("intent_seq"))
      throw std::invalid_argument("mock input identity must be an object without a sequence");
    if (!previous_ || *previous_ != input_without_sequence) {
      if (sequence_ == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("mock input sequence exhausted");
      ++sequence_;
      previous_ = input_without_sequence;
    }
    return sequence_;
  }
 private:
  std::optional<nlohmann::json> previous_;
  std::uint64_t sequence_{};
};
}  // namespace mine_teleop::detail
