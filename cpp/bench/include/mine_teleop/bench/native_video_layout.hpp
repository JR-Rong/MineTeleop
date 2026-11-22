#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace mine_teleop::detail {
struct NativeVideoRect {
  int x{}, y{}, width{}, height{};
};

// Fit inside the destination while keeping the source aspect ratio. Integer
// arithmetic keeps rounding deterministic and prevents a crop at the edges.
inline std::optional<NativeVideoRect> native_video_fit(int source_width, int source_height,
                                                       int width, int height) {
  if (source_width <= 0 || source_height <= 0 || width <= 0 || height <= 0) return std::nullopt;
  int fitted_width = width, fitted_height = height;
  if (std::int64_t(width) * source_height > std::int64_t(height) * source_width) {
    fitted_width = (std::max)(1, static_cast<int>(std::int64_t(height) * source_width / source_height));
  } else {
    fitted_height = (std::max)(1, static_cast<int>(std::int64_t(width) * source_height / source_width));
  }
  return NativeVideoRect{(width - fitted_width) / 2, (height - fitted_height) / 2,
                         fitted_width, fitted_height};
}
}  // namespace mine_teleop::detail
