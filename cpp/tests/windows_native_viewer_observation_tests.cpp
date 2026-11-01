#include "mine_teleop/detail/native_viewer_observation.hpp"
#include "mine_teleop/detail/native_operator_readouts.hpp"
#include "mine_teleop/detail/native_identity_input.hpp"
#include "mine_teleop/detail/native_video_layout.hpp"
#include <cstdlib>
#include <limits>

int main() {
  using namespace mine_teleop::detail;
  if (!viewer_draws_advance({2, 3}, {1, 2}, 2)) return EXIT_FAILURE;
  if (viewer_draws_advance({2, 2}, {1, 2}, 2) || viewer_draws_advance({2}, {1}, 2) ||
      viewer_draws_advance({}, {}, 0) || viewer_draws_advance({1, 2}, {}, 2)) return EXIT_FAILURE;
  if (viewer_path("host", "srflx", "udp", "udp") != ViewerPath::direct_udp ||
      viewer_path("relay", "host", "udp", "udp") != ViewerPath::relay ||
      viewer_path("host", "unknown", "udp", "udp") != ViewerPath::unknown ||
      viewer_path("host", "host", "tcp", "tcp") != ViewerPath::other) return EXIT_FAILURE;
  if (viewer_observation_stale(1000, 0) || viewer_observation_stale(3500, 1000) ||
      !viewer_observation_stale(3501, 1000) || !viewer_observation_stale(999, 1000)) return EXIT_FAILURE;
  const auto unknown = native_operator_readouts(std::nullopt, std::nullopt, std::nullopt);
  for (const auto& value : unknown) if (value != L"\u2014") return EXIT_FAILURE;
  const auto operator_values = native_operator_readouts(2.0, 'D', NativeOperatorIntent{'R', -.25, .5, .3});
  if (operator_values != std::array<std::wstring, 6>{L"7.2 km/h", L"D", L"R", L"-0.25", L"0.50", L"0.30"})
    return EXIT_FAILURE;
  const auto invalid = native_operator_readouts(std::numeric_limits<double>::infinity(), 'X',
      NativeOperatorIntent{'P', std::numeric_limits<double>::quiet_NaN(), -1, 2});
  for (const auto& value : invalid) if (value != L"\u2014") return EXIT_FAILURE;
  const auto partial = native_operator_readouts(0.0, 'N', NativeOperatorIntent{'X', 0, .5, 0});
  for (std::size_t i = 2; i < partial.size(); ++i) if (partial[i] != L"\u2014") return EXIT_FAILURE;
  if (native_trim_identity(L"\u00a0 driver-name \ufeff") != L"driver-name" ||
      !native_trim_identity(L"\u3000\n").empty() || native_trim_identity(L"a b") != L"a b") return EXIT_FAILURE;
  const auto tall = native_video_fit(960, 768, 360, 460);
  const auto wide = native_video_fit(960, 768, 1000, 300);
  if (!tall || tall->x != 0 || tall->y != 86 || tall->width != 360 || tall->height != 288 ||
      !wide || wide->x != 312 || wide->y != 0 || wide->width != 375 || wide->height != 300 ||
      native_video_fit(0, 768, 360, 460) || native_video_fit(960, 768, 0, 460)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
