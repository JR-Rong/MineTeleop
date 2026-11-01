#include "mine_teleop/detail/native_web_control.hpp"
#include <iomanip>
#include <iostream>

int main() {
  using Control = mine_teleop::detail::NativeWebControl;
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
