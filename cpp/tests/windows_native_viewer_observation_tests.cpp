#include "mine_teleop/detail/native_viewer_observation.hpp"
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
  return EXIT_SUCCESS;
}
