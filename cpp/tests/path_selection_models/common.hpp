#pragma once

// Test-only protocol models. No network, authority timer, actuator or runtime
// hook exists here. Verification/authorization inputs stand for prerequisites
// that a future production adapter must actually prove; they are not crypto.
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace mine_teleop::path_selection_models {
enum class Route { Dc, Wss };
struct Authority {
  std::string session, vehicle, driver;
  std::uint64_t generation{}, epoch{};
  bool operator==(const Authority&) const = default;
  bool valid() const {
    return !session.empty() && !vehicle.empty() && !driver.empty() &&
        session.size() <= 128 && vehicle.size() <= 128 && driver.size() <= 128 && generation && epoch;
  }
};
using Digest = std::array<std::uint8_t, 32>; // Public fixture identity, not a hash implementation.
struct Preconditions {
  bool verified{}, caller_authorized{};
  bool satisfied() const { return verified && caller_authorized; }
};
}  // namespace mine_teleop::path_selection_models
