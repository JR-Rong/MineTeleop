#include "mine_teleop/detail/native_viewer_observation.hpp"
#include "mine_teleop/detail/native_video_layout.hpp"
#include <cstdlib>

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
  const auto tall = native_video_fit(960, 768, 360, 460);
  const auto wide = native_video_fit(960, 768, 1000, 300);
  if (!tall || tall->x != 0 || tall->y != 86 || tall->width != 360 || tall->height != 288 ||
      !wide || wide->x != 312 || wide->y != 0 || wide->width != 375 || wide->height != 300 ||
      native_video_fit(0, 768, 360, 460) || native_video_fit(960, 768, 0, 460)) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
