#pragma once
#include "common.hpp"

#include <stdexcept>

namespace mine_teleop::path_selection_models {
inline Authority fixture_authority() { return {"fixture-session", "fixture-vehicle", "fixture-driver", 7, 11}; }
inline Digest fixture_digest(std::uint8_t value) { Digest digest{}; digest[0] = value; return digest; }
inline void require(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
template<class Action> inline void rejects(Action action) {
  bool rejected = false;
  try { action(); } catch (const std::invalid_argument&) { rejected = true; }
  require(rejected, "invalid model configuration was accepted");
}
}  // namespace mine_teleop::path_selection_models
