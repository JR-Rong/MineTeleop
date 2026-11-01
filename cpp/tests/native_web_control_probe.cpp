#include "mine_teleop/detail/native_web_control.hpp"
#include "mine_teleop/detail/native_mock_gear_transition.hpp"
#include <iomanip>
#include <iostream>
#include <string_view>

int main(int argc, char** argv) {
  using Control = mine_teleop::detail::NativeWebControl;
  if (argc == 2 && std::string_view(argv[1]) == "--gear-transition") {
    using Gear = mine_teleop::detail::NativeMockGearTransition;
    std::uint64_t status, rejection;
    char actual;
    bool forwarded, fresh;
    while (std::cin >> status >> forwarded >> actual >> fresh >> rejection) {
      Gear gear;
      static_cast<void>(gear.observe_status(10));
      const auto generation = gear.begin('N', 'D');
      if (forwarded && generation) gear.forwarded(*generation, 20, 'D');
      static_cast<void>(gear.observe_status(status));
      const bool rejected = gear.reject(status, rejection) == Gear::Rejection::matched;
      const bool confirmed = gear.confirm(status, actual, fresh);
      std::cout << confirmed << ' ' << rejected << '\n';
    }
    return 0;
  }
  if (argc == 2 && std::string_view(argv[1]) == "--standard-gamepad") {
    double axis, throttle, brake, center, range, deadzone;
    bool inverted;
    std::cout << std::setprecision(17);
    while (std::cin >> axis >> throttle >> brake >> center >> range >> deadzone >> inverted) {
      const auto pad = Control::standard_gamepad(axis, throttle, brake, center, range, deadzone, inverted);
      std::cout << pad.steering << ' ' << pad.throttle << ' ' << pad.brake << '\n';
    }
    return 0;
  }
  Control::Keys keys;
  Control::Gamepad pad;
  Control::Limits limits;
  char gear;
  double scale;
  bool estop;
  std::cout << std::setprecision(17);
  while (std::cin >> keys.up >> keys.down >> keys.left >> keys.right >> keys.service_brake >> keys.hard_brake
         >> pad.steering >> pad.throttle >> pad.brake >> gear >> limits.throttle >> limits.brake
         >> limits.service_brake >> limits.hard_brake >> limits.steering >> scale >> estop) {
    const auto result = Control::derive(keys, pad, gear, limits, scale, estop);
    std::cout << result.gear << ' ' << result.steering << ' ' << result.throttle << ' '
              << result.brake << ' ' << result.estop << '\n';
  }
}
