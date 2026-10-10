#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mine_teleop::surround {

// All matrices are row-major. OpenCV camera axes: right/down/forward;
// vehicle axes: forward/left/up. Translations and ground coordinates are
// metres.
struct Camera {
  std::array<double, 9> K{};
  double xi{};
  std::array<double, 4> D{};
  std::array<double, 16> vehicle_from_camera{};
  std::array<double, 9> runtime_from_calibration{};
  int width{}, height{};
};
struct Point {
  double x{}, y{}, z{};
};
struct Pixel {
  double u{}, v{};
  bool valid{};
};
struct Region {
  double x_min{}, x_max{}, y_min{}, y_max{};
};
enum class Format { Rgba, Uyvy };
struct Image {
  const std::uint8_t *data{};
  int width{}, height{};
  std::size_t stride{};
  Format format{Format::Rgba};
};

Pixel project(const Camera &, Point);
Point ground_point(const Camera &, Pixel);
std::array<double, 9> image_mapping(int sensor_width, int sensor_height,
                                    std::array<int, 4> crop, int rotation,
                                    bool mirror_x, bool mirror_y, int width,
                                    int height);
void resize_into(const Image &, std::uint8_t *rgba, int width, int height,
                 std::size_t stride);

class Renderer {
public:
  Renderer(std::array<Camera, 4> cameras, Region region, double vehicle_length,
           double vehicle_width, int width, int height);
  void render(const std::array<Image, 4> &images, unsigned valid_mask,
              std::uint8_t *rgba);
  void effective_mask(std::uint8_t *mask) const;
  int width() const { return width_; }
  int height() const { return height_; }
  double metres_per_pixel() const { return metres_per_pixel_; }
  const Camera &camera(unsigned index) const { return cameras_.at(index); }

private:
  struct Sample {
    float u{}, v{}, weight{};
    std::uint8_t camera{};
  };
  struct Mapping {
    std::array<Sample, 2> samples{};
    bool body{};
  };
  std::array<Camera, 4> cameras_;
  std::vector<Mapping> map_;
  std::array<float, 4> gain_{1, 1, 1, 1};
  int width_, height_;
  double metres_per_pixel_;
};
} // namespace mine_teleop::surround

// Standalone C ABI also built with Emscripten. Each camera is 41 doubles:
// K(9), xi(1), D(4), T_vehicle_from_camera(16), A(9), width,height(2).
extern "C" {
void *mt_surround_create(const double *cameras, const double *region,
                         double vehicle_length, double vehicle_width, int width,
                         int height);
void mt_surround_destroy(void *renderer);
int mt_surround_render(void *renderer, const std::uint8_t *front,
                       const std::uint8_t *rear, const std::uint8_t *left,
                       const std::uint8_t *right, unsigned valid_mask,
                       std::uint8_t *output);
int mt_surround_effective_mask(void *renderer, std::uint8_t *output);
}
