#include "mine_teleop/surround_math.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace mine_teleop::surround {
namespace {
std::array<float, 3> sample(const Image &image, double u, double v) {
  const auto at = [&](int x, int y) -> std::array<float, 3> {
    x = std::clamp(x, 0, image.width - 1);
    y = std::clamp(y, 0, image.height - 1);
    const auto *row = image.data + y * image.stride;
    if (image.format == Format::Rgba) {
      const auto *p = row + x * 4;
      return {float(p[0]), float(p[1]), float(p[2])};
    }
    const auto *p = row + (x & ~1) * 2;
    const float Y = std::max(0, int(p[(x & 1) ? 3 : 1]) - 16);
    const float U = int(p[0]) - 128, V = int(p[2]) - 128;
    return {1.164f * Y + 1.596f * V, 1.164f * Y - .392f * U - .813f * V,
            1.164f * Y + 2.017f * U};
  };
  const int x = int(std::floor(u)), y = int(std::floor(v));
  const float a = float(u - x), b = float(v - y);
  const auto p = at(x, y), q = at(x + 1, y), r = at(x, y + 1),
             s = at(x + 1, y + 1);
  std::array<float, 3> result{};
  for (int i = 0; i < 3; ++i)
    result[i] =
        (1 - b) * ((1 - a) * p[i] + a * q[i]) + b * ((1 - a) * r[i] + a * s[i]);
  return result;
}
void check_image(const Image &image) {
  if (!image.data || image.width <= 0 || image.height <= 0 ||
      image.stride <
          std::size_t(image.width) * (image.format == Format::Rgba ? 4 : 2) ||
      (image.format == Format::Uyvy && image.width % 2))
    throw std::invalid_argument("invalid surround image layout");
}
} // namespace

Pixel project(const Camera &c, Point p) {
  const auto &t = c.vehicle_from_camera;
  p.x -= t[3];
  p.y -= t[7];
  p.z -= t[11];
  const double X = t[0] * p.x + t[4] * p.y + t[8] * p.z;
  const double Y = t[1] * p.x + t[5] * p.y + t[9] * p.z;
  const double Z = t[2] * p.x + t[6] * p.y + t[10] * p.z;
  const double denominator = Z + c.xi * std::sqrt(X * X + Y * Y + Z * Z);
  if (!(denominator > 1e-10))
    return {};
  const double x = X / denominator, y = Y / denominator, r2 = x * x + y * y;
  const double radial = 1 + c.D[0] * r2 + c.D[1] * r2 * r2;
  const double xd = x * radial + 2 * c.D[2] * x * y + c.D[3] * (r2 + 2 * x * x);
  const double yd = y * radial + c.D[2] * (r2 + 2 * y * y) + 2 * c.D[3] * x * y;
  const double u = c.K[0] * xd + c.K[1] * yd + c.K[2], v = c.K[4] * yd + c.K[5];
  const auto &a = c.runtime_from_calibration;
  const double w = a[6] * u + a[7] * v + a[8];
  if (std::abs(w) < 1e-12)
    return {};
  const double ru = (a[0] * u + a[1] * v + a[2]) / w,
               rv = (a[3] * u + a[4] * v + a[5]) / w;
  return {ru, rv,
          std::isfinite(ru) && std::isfinite(rv) && ru >= 0 && rv >= 0 &&
              ru < c.width - 1 && rv < c.height - 1};
}

Point ground_point(const Camera &c, Pixel pixel) {
  const auto &a = c.runtime_from_calibration;
  const double u = pixel.u, v = pixel.v;
  const double B00 = a[0] - u * a[6], B01 = a[1] - u * a[7],
               B10 = a[3] - v * a[6], B11 = a[4] - v * a[7];
  const double det = B00 * B11 - B01 * B10;
  if (std::abs(det) < 1e-12)
    throw std::invalid_argument("singular image transform");
  const double bu = u * a[8] - a[2], bv = v * a[8] - a[5];
  const double cu = (bu * B11 - B01 * bv) / det,
               cv = (B00 * bv - bu * B10) / det;
  const double yd = (cv - c.K[5]) / c.K[4],
               xd = (cu - c.K[2] - c.K[1] * yd) / c.K[0];
  double x = xd, y = yd;
  for (int i = 0; i < 20; ++i) {
    const double r2 = x * x + y * y,
                 radial = 1 + c.D[0] * r2 + c.D[1] * r2 * r2;
    if (std::abs(radial) < 1e-12)
      throw std::invalid_argument("invalid distortion inverse");
    x = (xd - 2 * c.D[2] * x * y - c.D[3] * (r2 + 2 * x * x)) / radial;
    y = (yd - c.D[2] * (r2 + 2 * y * y) - 2 * c.D[3] * x * y) / radial;
  }
  const double r2 = x * x + y * y, disc = 1 + (1 - c.xi * c.xi) * r2;
  if (disc < 0)
    throw std::invalid_argument("pixel outside omnidir domain");
  const double lambda = (c.xi + std::sqrt(disc)) / (1 + r2);
  const Point ray{lambda * x, lambda * y, lambda - c.xi};
  const auto &t = c.vehicle_from_camera;
  const double dz = t[8] * ray.x + t[9] * ray.y + t[10] * ray.z;
  if (std::abs(dz) < 1e-10 || -t[11] / dz <= 0)
    throw std::invalid_argument("pixel does not intersect ground ahead");
  const double k = -t[11] / dz;
  return {t[3] + k * (t[0] * ray.x + t[1] * ray.y + t[2] * ray.z),
          t[7] + k * (t[4] * ray.x + t[5] * ray.y + t[6] * ray.z), 0};
}

std::array<double, 9> image_mapping(int sw, int sh, std::array<int, 4> crop,
                                    int rotation, bool mx, bool my, int width,
                                    int height) {
  if (sw <= 0 || sh <= 0 || crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 ||
      crop[3] <= 0 || crop[0] > sw - crop[2] || crop[1] > sh - crop[3] ||
      width <= 0 || height <= 0)
    throw std::invalid_argument("invalid sensor crop/resize");
  auto point = [&](double x, double y) {
    x -= crop[0];
    y -= crop[1];
    const auto ox = x, oy = y;
    int cw = crop[2], ch = crop[3];
    if (rotation == 90) {
      x = ch - 1 - oy;
      y = ox;
      std::swap(cw, ch);
    } else if (rotation == 180) {
      x = cw - 1 - ox;
      y = ch - 1 - oy;
    } else if (rotation == 270) {
      x = oy;
      y = cw - 1 - ox;
      std::swap(cw, ch);
    } else if (rotation != 0)
      throw std::invalid_argument("unsupported camera rotation");
    if (mx)
      x = cw - 1 - x;
    if (my)
      y = ch - 1 - y;
    return std::array<double, 2>{(x + .5) * width / cw - .5,
                                 (y + .5) * height / ch - .5};
  };
  const auto o = point(0, 0), x = point(1, 0), y = point(0, 1);
  return {x[0] - o[0], y[0] - o[0], o[0], x[1] - o[1], y[1] - o[1], o[1],
          0,           0,           1};
}

void resize_into(const Image &image, std::uint8_t *rgba, int width, int height,
                 std::size_t stride) {
  check_image(image);
  if (!rgba || width <= 0 || height <= 0 || stride < std::size_t(width) * 4)
    throw std::invalid_argument("invalid mosaic output");
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) {
      auto rgb = sample(image, (x + .5) * image.width / width - .5,
                        (y + .5) * image.height / height - .5);
      auto *out = rgba + y * stride + x * 4;
      for (int k = 0; k < 3; ++k)
        out[k] = std::uint8_t(std::clamp(rgb[k], 0.f, 255.f));
      out[3] = 255;
    }
}

Renderer::Renderer(std::array<Camera, 4> cameras, Region r, double length,
                   double width, int w, int h)
    : cameras_(cameras), width_(w), height_(h) {
  if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || !(length > 0) ||
      !(width > 0) || !std::isfinite(length) || !std::isfinite(width) ||
      !std::isfinite(r.x_min) || !std::isfinite(r.x_max) ||
      !std::isfinite(r.y_min) || !std::isfinite(r.y_max) ||
      !(r.x_max > r.x_min) || !(r.y_max > r.y_min))
    throw std::invalid_argument("invalid surround geometry");
  for (const auto &c : cameras_) {
    auto finite = [](const auto &v) {
      return std::all_of(v.begin(), v.end(),
                         [](auto n) { return std::isfinite(n); });
    };
    if (c.width <= 1 || c.height <= 1 || c.width > 8192 || c.height > 8192 ||
        !std::isfinite(c.xi) || c.xi < 0 || !finite(c.K) || !finite(c.D) ||
        !finite(c.vehicle_from_camera) || !finite(c.runtime_from_calibration) ||
        c.K[0] <= 0 || c.K[4] <= 0 || c.K[8] != 1)
      throw std::invalid_argument("invalid surround camera");
  }
  metres_per_pixel_ =
      std::max((r.y_max - r.y_min) / w, (r.x_max - r.x_min) / h);
  map_.resize(std::size_t(w) * h);
  const double xc = (r.x_min + r.x_max) / 2, yc = (r.y_min + r.y_max) / 2;
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      const Point p{xc + (h / 2. - y - .5) * metres_per_pixel_,
                    yc + (w / 2. - x - .5) * metres_per_pixel_, 0};
      auto &m = map_[std::size_t(y) * w + x];
      if (p.x < r.x_min || p.x > r.x_max || p.y < r.y_min || p.y > r.y_max)
        continue;
      m.body = std::abs(p.x) <= length / 2 && std::abs(p.y) <= width / 2;
      if (m.body)
        continue;
      for (unsigned i = 0; i < 4; ++i) {
        const auto uv = project(cameras[i], p);
        if (!uv.valid)
          continue;
        const auto &c = cameras[i];
        const double edge = std::min({uv.u / c.width, 1 - uv.u / c.width,
                                      uv.v / c.height, 1 - uv.v / c.height});
        const double dx = p.x - c.vehicle_from_camera[3],
                     dy = p.y - c.vehicle_from_camera[7];
        const float weight = float(edge / std::pow(.25 + dx * dx + dy * dy, 2));
        Sample s{float(uv.u), float(uv.v), weight, std::uint8_t(i)};
        if (weight > m.samples[0].weight) {
          m.samples[1] = m.samples[0];
          m.samples[0] = s;
        } else if (weight > m.samples[1].weight)
          m.samples[1] = s;
      }
    }
}

void Renderer::effective_mask(std::uint8_t *out) const {
  if (!out)
    throw std::invalid_argument("missing effective mask");
  for (std::size_t i = 0; i < map_.size(); ++i)
    out[i] = !map_[i].body && map_[i].samples[0].weight > 0 ? 255 : 0;
}

void Renderer::render(const std::array<Image, 4> &images, unsigned valid,
                      std::uint8_t *rgba) {
  if (!rgba)
    throw std::invalid_argument("missing surround output");
  for (unsigned i = 0; i < 4; ++i)
    if (valid & (1u << i)) {
      check_image(images[i]);
      if (images[i].width != cameras_[i].width ||
          images[i].height != cameras_[i].height)
        throw std::invalid_argument(
            "surround input differs from calibration runtime size");
    }
  // Brightness adapts only across overlap samples and is bounded to avoid
  // amplifying noise or changing colour on every exposure adjustment.
  std::array<double, 4> luminance{}, reference{};
  std::array<int, 4> count{};
  for (std::size_t i = 0; i < map_.size(); i += 257) {
    const auto &a = map_[i].samples[0];
    const auto &b = map_[i].samples[1];
    if (a.weight <= 0 || b.weight <= 0 || !(valid & (1u << a.camera)) ||
        !(valid & (1u << b.camera)))
      continue;
    auto av = sample(images[a.camera], a.u, a.v),
         bv = sample(images[b.camera], b.u, b.v);
    const double al = (av[0] + av[1] + av[2]) / 3,
                 bl = (bv[0] + bv[1] + bv[2]) / 3;
    if (al < 16 || bl < 16)
      continue;
    const double mean = (al + bl) / 2;
    luminance[a.camera] += al;
    reference[a.camera] += mean;
    ++count[a.camera];
    luminance[b.camera] += bl;
    reference[b.camera] += mean;
    ++count[b.camera];
  }
  for (unsigned i = 0; i < 4; ++i)
    if (count[i] >= 4)
      gain_[i] = .98f * gain_[i] +
                 .02f * float(std::clamp(reference[i] / luminance[i], .8, 1.2));
  for (std::size_t i = 0; i < map_.size(); ++i) {
    const auto &m = map_[i];
    std::array<float, 3> rgb{};
    float sum = 0;
    for (const auto &s : m.samples) {
      if (s.weight <= 0)
        continue;
      // A missing primary camera masks the pixel. Never let the secondary
      // image silently substitute for a failed source at a seam.
      if (!(valid & (1u << s.camera))) {
        sum = 0;
        break;
      }
      const auto colour = sample(images[s.camera], s.u, s.v);
      for (int k = 0; k < 3; ++k)
        rgb[k] += colour[k] * s.weight * gain_[s.camera];
      sum += s.weight;
    }
    for (int k = 0; k < 3; ++k)
      rgba[i * 4 + k] = m.body ? std::uint8_t(34)
                        : sum > 0
                            ? std::uint8_t(std::clamp(rgb[k] / sum, 0.f, 255.f))
                            : std::uint8_t(12);
    rgba[i * 4 + 3] = 255;
  }
}
} // namespace mine_teleop::surround

extern "C" void *mt_surround_create(const double *params, const double *r,
                                    double l, double w, int width, int height) {
  try {
    if (!params || !r)
      return nullptr;
    std::array<mine_teleop::surround::Camera, 4> cameras;
    for (auto &c : cameras) {
      std::copy_n(params, 9, c.K.begin());
      params += 9;
      c.xi = *params++;
      std::copy_n(params, 4, c.D.begin());
      params += 4;
      std::copy_n(params, 16, c.vehicle_from_camera.begin());
      params += 16;
      std::copy_n(params, 9, c.runtime_from_calibration.begin());
      params += 9;
      const auto cw = *params++, ch = *params++;
      if (!std::isfinite(cw) || !std::isfinite(ch) || cw < 2 || ch < 2 ||
          cw > 8192 || ch > 8192 || cw != std::floor(cw) ||
          ch != std::floor(ch))
        return nullptr;
      c.width = int(cw);
      c.height = int(ch);
    }
    return new mine_teleop::surround::Renderer(
        cameras, {r[0], r[1], r[2], r[3]}, l, w, width, height);
  } catch (...) {
    return nullptr;
  }
}
extern "C" void mt_surround_destroy(void *p) {
  delete static_cast<mine_teleop::surround::Renderer *>(p);
}
extern "C" int mt_surround_effective_mask(void *p, std::uint8_t *out) {
  try {
    if (!p || !out)
      return -1;
    static_cast<mine_teleop::surround::Renderer *>(p)->effective_mask(out);
    return 0;
  } catch (...) {
    return -1;
  }
}
extern "C" int mt_surround_render(void *p, const std::uint8_t *a,
                                  const std::uint8_t *b, const std::uint8_t *c,
                                  const std::uint8_t *d, unsigned valid,
                                  std::uint8_t *out) {
  try {
    if (!p || !out || (valid & ~15u))
      return -1;
    auto &renderer = *static_cast<mine_teleop::surround::Renderer *>(p);
    const std::array<const std::uint8_t *, 4> data{a, b, c, d};
    std::array<mine_teleop::surround::Image, 4> images;
    for (unsigned i = 0; i < 4; ++i) {
      const auto &camera = renderer.camera(i);
      images[i] = {data[i], camera.width, camera.height,
                   std::size_t(camera.width) * 4,
                   mine_teleop::surround::Format::Rgba};
    }
    renderer.render(images, valid, out);
    return 0;
  } catch (...) {
    return -1;
  }
}
