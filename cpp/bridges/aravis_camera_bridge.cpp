#include <arv.h>
#include <cstdio>
#include <jpeglib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <setjmp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

struct Arguments {
  bool list{false};
  bool json{false};
  bool self_test{false};
  bool auto_exposure{false};
  bool auto_gain{false};
  int device_index{0};
  std::string serial;
  std::string model;
  int width{1280};
  int height{720};
  int fps{30};
  int frames{0};
  int timeout_ms{2000};
  int jpeg_quality{80};
  int target_luma{80};
  int luma_deadband{8};
  double exposure_min_us{100.0};
  double exposure_max_us{12000.0};
  double gain_min_fraction{0.0};
  double gain_max_fraction{0.35};
  int update_interval_frames{6};
  std::string metering{"full"};
};

struct DeviceDescription {
  std::string id;
  std::string vendor;
  std::string model;
  std::string serial;
  std::string protocol;
};

std::string required_value(int& index, int argc, char** argv, std::string_view option) {
  if (index + 1 >= argc) throw std::invalid_argument(std::string(option) + " requires a value");
  return argv[++index];
}

int integer(std::string_view value, std::string_view option) {
  std::size_t consumed = 0;
  int result = 0;
  try {
    result = std::stoi(std::string(value), &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(option) + " must be an integer");
  }
  if (consumed != value.size()) throw std::invalid_argument(std::string(option) + " must be an integer");
  return result;
}

double floating_point(std::string_view value, std::string_view option) {
  std::size_t consumed = 0;
  double result = 0.0;
  try {
    result = std::stod(std::string(value), &consumed);
  } catch (const std::exception&) {
    throw std::invalid_argument(std::string(option) + " must be a number");
  }
  if (consumed != value.size() || !std::isfinite(result)) {
    throw std::invalid_argument(std::string(option) + " must be a finite number");
  }
  return result;
}

Arguments parse_arguments(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    if (option == "--list") arguments.list = true;
    else if (option == "--json") arguments.json = true;
    else if (option == "--self-test") arguments.self_test = true;
    else if (option == "--auto-exposure") arguments.auto_exposure = true;
    else if (option == "--auto-gain") arguments.auto_gain = true;
    else if (option == "--device-index") arguments.device_index = integer(required_value(index, argc, argv, option), option);
    else if (option == "--serial") arguments.serial = required_value(index, argc, argv, option);
    else if (option == "--model") arguments.model = required_value(index, argc, argv, option);
    else if (option == "--width") arguments.width = integer(required_value(index, argc, argv, option), option);
    else if (option == "--height") arguments.height = integer(required_value(index, argc, argv, option), option);
    else if (option == "--fps") arguments.fps = integer(required_value(index, argc, argv, option), option);
    else if (option == "--frames") arguments.frames = integer(required_value(index, argc, argv, option), option);
    else if (option == "--timeout-ms") arguments.timeout_ms = integer(required_value(index, argc, argv, option), option);
    else if (option == "--jpeg-quality") arguments.jpeg_quality = integer(required_value(index, argc, argv, option), option);
    else if (option == "--target-luma") arguments.target_luma = integer(required_value(index, argc, argv, option), option);
    else if (option == "--luma-deadband") arguments.luma_deadband = integer(required_value(index, argc, argv, option), option);
    else if (option == "--exposure-min-us") {
      arguments.exposure_min_us = floating_point(required_value(index, argc, argv, option), option);
    } else if (option == "--exposure-max-us") {
      arguments.exposure_max_us = floating_point(required_value(index, argc, argv, option), option);
    } else if (option == "--gain-min-fraction") {
      arguments.gain_min_fraction = floating_point(required_value(index, argc, argv, option), option);
    } else if (option == "--gain-max-fraction") {
      arguments.gain_max_fraction = floating_point(required_value(index, argc, argv, option), option);
    } else if (option == "--update-interval-frames") {
      arguments.update_interval_frames = integer(required_value(index, argc, argv, option), option);
    } else if (option == "--metering") {
      arguments.metering = required_value(index, argc, argv, option);
    }
    else if (option == "--help" || option == "-h") {
      std::cout << "Usage: mine-teleop-aravis-camera [--list --json] "
                   "[--device-index N|--serial S|--model M] [--width W] [--height H] "
                   "[--fps FPS] [--frames N] [--timeout-ms MS] [--jpeg-quality 1..99] "
                   "[--auto-exposure] [--auto-gain] [--target-luma 1..254] "
                   "[--luma-deadband 0..64] [--exposure-min-us US] [--exposure-max-us US] "
                   "[--gain-min-fraction 0..1] [--gain-max-fraction 0..1] "
                   "[--update-interval-frames N] [--metering center|full]\n";
      std::exit(0);
    } else {
      throw std::invalid_argument("unknown option: " + option);
    }
  }
  if (arguments.device_index < 0 || arguments.width <= 0 || arguments.height <= 0 || arguments.fps <= 0 ||
      arguments.frames < 0 || arguments.timeout_ms <= 0 || arguments.jpeg_quality < 1 ||
      arguments.jpeg_quality > 99) {
    throw std::invalid_argument("Aravis camera numeric option is out of range");
  }
  if (arguments.auto_exposure || arguments.auto_gain) {
    if (arguments.target_luma < 1 || arguments.target_luma > 254 ||
        arguments.luma_deadband < 0 || arguments.luma_deadband > 64 ||
        arguments.target_luma - arguments.luma_deadband < 0 ||
        arguments.target_luma + arguments.luma_deadband > 255) {
      throw std::invalid_argument("Aravis imaging target luma/deadband is out of range");
    }
    if (arguments.update_interval_frames <= 0 || arguments.update_interval_frames > arguments.fps * 10) {
      throw std::invalid_argument("Aravis update interval is out of range");
    }
    if (arguments.metering != "center" && arguments.metering != "full") {
      throw std::invalid_argument("Aravis metering must be center or full");
    }
  }
  if (arguments.auto_exposure) {
    const auto frame_period_us = 1000000.0 / static_cast<double>(arguments.fps);
    if (arguments.exposure_min_us <= 0.0 ||
        arguments.exposure_max_us < arguments.exposure_min_us ||
        arguments.exposure_max_us > frame_period_us * 0.9) {
      throw std::invalid_argument("Aravis exposure bounds do not preserve the requested frame rate");
    }
  }
  if (arguments.auto_gain &&
      (arguments.gain_min_fraction < 0.0 || arguments.gain_max_fraction > 1.0 ||
       arguments.gain_max_fraction < arguments.gain_min_fraction)) {
    throw std::invalid_argument("Aravis gain fractions must satisfy 0 <= min <= max <= 1");
  }
  return arguments;
}

std::string text(const char* value) {
  return value == nullptr ? std::string() : std::string(value);
}

std::string json_escape(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    switch (character) {
      case '\\': result += "\\\\"; break;
      case '"': result += "\\\""; break;
      case '\n': result += "\\n"; break;
      case '\r': result += "\\r"; break;
      case '\t': result += "\\t"; break;
      default:
        if (character >= 0x20) result.push_back(static_cast<char>(character));
        break;
    }
  }
  return result;
}

[[noreturn]] void throw_aravis(std::string_view operation, GError* error) {
  const std::string detail = error == nullptr ? "unknown Aravis error" : text(error->message);
  if (error != nullptr) g_error_free(error);
  throw std::runtime_error(std::string(operation) + " failed: " + detail);
}

void check_error(GError* error, std::string_view operation) {
  if (error != nullptr) throw_aravis(operation, error);
}

template <typename T>
T* check_pointer(T* value, GError* error, std::string_view operation) {
  if (error != nullptr) throw_aravis(operation, error);
  if (value == nullptr) throw std::runtime_error(std::string(operation) + " failed without an error message");
  return value;
}

struct AravisLifecycle {
  AravisLifecycle() {
    const auto interface_count = arv_get_n_interfaces();
    for (unsigned int index = 0; index < interface_count; ++index) {
      const auto* interface_id = arv_get_interface_id(index);
      if (interface_id == nullptr) continue;
      if (std::string_view(interface_id) == "USB3Vision") {
        arv_enable_interface(interface_id);
      } else {
        arv_disable_interface(interface_id);
      }
    }
  }
  ~AravisLifecycle() { arv_shutdown(); }
};

struct GObjectUnref {
  template <typename T>
  void operator()(T* value) const {
    if (value != nullptr) g_object_unref(value);
  }
};

std::vector<DeviceDescription> enumerate_devices() {
  arv_update_device_list();
  std::vector<DeviceDescription> devices;
  const auto count = arv_get_n_devices();
  devices.reserve(count);
  for (unsigned int index = 0; index < count; ++index) {
    const auto protocol = text(arv_get_device_protocol(index));
    if (protocol != "USB3Vision") continue;
    devices.push_back({
        text(arv_get_device_id(index)),
        text(arv_get_device_vendor(index)),
        text(arv_get_device_model(index)),
        text(arv_get_device_serial_nbr(index)),
        protocol,
    });
  }
  return devices;
}

void list_devices(const std::vector<DeviceDescription>& devices, bool json) {
  if (json) std::cout << "{\"device_count\":" << devices.size() << ",\"devices\":[";
  for (std::size_t index = 0; index < devices.size(); ++index) {
    const auto& device = devices[index];
    if (json) {
      if (index > 0) std::cout << ',';
      std::cout << "{\"index\":" << index << ",\"id\":\"" << json_escape(device.id)
                << "\",\"vendor\":\"" << json_escape(device.vendor) << "\",\"model\":\""
                << json_escape(device.model) << "\",\"serial\":\"" << json_escape(device.serial)
                << "\",\"type\":\"usb3vision\"}";
    } else {
      std::cout << index << ": " << device.vendor << ' ' << device.model << " serial=" << device.serial
                << " id=" << device.id << " type=usb3vision\n";
    }
  }
  if (json) std::cout << "]}\n";
}

const DeviceDescription& select_device(const std::vector<DeviceDescription>& devices, const Arguments& arguments) {
  if (devices.empty()) throw std::runtime_error("no USB3 Vision camera found through Aravis");
  if (!arguments.serial.empty() || !arguments.model.empty()) {
    const auto match = std::find_if(devices.begin(), devices.end(), [&](const auto& device) {
      return (!arguments.serial.empty() && device.serial == arguments.serial) ||
             (!arguments.model.empty() && device.model == arguments.model);
    });
    if (match == devices.end()) {
      throw std::runtime_error(arguments.serial.empty() ? "Aravis camera model not found: " + arguments.model
                                                        : "Aravis camera serial not found: " + arguments.serial);
    }
    return *match;
  }
  if (arguments.device_index >= static_cast<int>(devices.size())) {
    throw std::runtime_error("Aravis camera index out of range");
  }
  return devices.at(static_cast<std::size_t>(arguments.device_index));
}

std::string select_pixel_format(ArvCamera* camera) {
  GError* error = nullptr;
  guint count = 0;
  const char** raw_formats = arv_camera_dup_available_pixel_formats_as_strings(camera, &count, &error);
  check_error(error, "query available pixel formats");
  std::vector<std::string> formats;
  formats.reserve(count);
  for (guint index = 0; index < count; ++index) formats.push_back(text(raw_formats[index]));
  g_free(raw_formats);

  constexpr std::array<std::string_view, 6> preferred{
      "RGB8", "BayerRG8", "BayerBG8", "BayerGR8", "BayerGB8", "Mono8"};
  for (const auto candidate : preferred) {
    if (std::find(formats.begin(), formats.end(), candidate) == formats.end()) continue;
    error = nullptr;
    arv_camera_set_pixel_format_from_string(camera, std::string(candidate).c_str(), &error);
    check_error(error, "set pixel format " + std::string(candidate));
    return std::string(candidate);
  }

  std::string available;
  for (const auto& format : formats) {
    if (!available.empty()) available += ',';
    available += format;
  }
  throw std::runtime_error("camera has no supported 8-bit RGB/Bayer/Mono pixel format; available=" + available);
}

struct ImagingBounds {
  double exposure_min_us{0.0};
  double exposure_max_us{0.0};
  double gain_min{0.0};
  double gain_max{0.0};
  bool exposure_available{false};
  bool gain_available{false};
};

struct ImagingState {
  double exposure_us{0.0};
  double gain{0.0};
};

struct ImagingAdjustment {
  ImagingState state;
  bool exposure_changed{false};
  bool gain_changed{false};
};

ImagingAdjustment next_imaging_adjustment(
    const Arguments& arguments,
    int luma,
    const ImagingBounds& bounds,
    ImagingState current) {
  ImagingAdjustment result{current};
  const auto dark = luma < arguments.target_luma - arguments.luma_deadband;
  const auto bright = luma > arguments.target_luma + arguments.luma_deadband;
  if (!dark && !bright) return result;

  if (dark) {
    if (bounds.exposure_available && current.exposure_us < bounds.exposure_max_us - 0.5) {
      const auto error_fraction =
          static_cast<double>(arguments.target_luma - luma) /
          static_cast<double>(std::max(arguments.target_luma, 1));
      const auto factor = 1.0 + std::clamp(error_fraction * 0.5, 0.05, 0.25);
      result.state.exposure_us =
          std::clamp(current.exposure_us * factor, bounds.exposure_min_us, bounds.exposure_max_us);
      result.exposure_changed = std::abs(result.state.exposure_us - current.exposure_us) >= 0.5;
    } else if (bounds.gain_available && current.gain < bounds.gain_max - 1e-6) {
      const auto step = std::max((bounds.gain_max - bounds.gain_min) * 0.05, 0.01);
      result.state.gain = std::clamp(current.gain + step, bounds.gain_min, bounds.gain_max);
      result.gain_changed = std::abs(result.state.gain - current.gain) >= 1e-6;
    }
    return result;
  }

  if (bounds.gain_available && current.gain > bounds.gain_min + 1e-6) {
    const auto step = std::max((bounds.gain_max - bounds.gain_min) * 0.05, 0.01);
    result.state.gain = std::clamp(current.gain - step, bounds.gain_min, bounds.gain_max);
    result.gain_changed = std::abs(result.state.gain - current.gain) >= 1e-6;
  } else if (bounds.exposure_available && current.exposure_us > bounds.exposure_min_us + 0.5) {
    const auto error_fraction =
        static_cast<double>(luma - arguments.target_luma) /
        static_cast<double>(std::max(255 - arguments.target_luma, 1));
    const auto factor = 1.0 - std::clamp(error_fraction * 0.5, 0.05, 0.2);
    result.state.exposure_us =
        std::clamp(current.exposure_us * factor, bounds.exposure_min_us, bounds.exposure_max_us);
    result.exposure_changed = std::abs(result.state.exposure_us - current.exposure_us) >= 0.5;
  }
  return result;
}

int sampled_luma(
    const std::vector<unsigned char>& rgb,
    int width,
    int height,
    std::string_view metering) {
  if (width <= 0 || height <= 0 ||
      rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U) {
    throw std::invalid_argument("cannot meter an invalid RGB frame");
  }
  const int x_begin = metering == "center" ? width / 5 : 0;
  const int x_end = metering == "center" ? width - width / 5 : width;
  const int y_begin = metering == "center" ? height / 5 : 0;
  const int y_end = metering == "center" ? height - height / 5 : height;
  const int step = std::max(1, std::min(width, height) / 160);
  std::array<std::size_t, 256> histogram{};
  std::size_t samples = 0;
  for (int y = y_begin; y < y_end; y += step) {
    for (int x = x_begin; x < x_end; x += step) {
      const auto offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
           static_cast<std::size_t>(x)) *
          3U;
      const auto luma =
          (54U * rgb[offset] + 183U * rgb[offset + 1] + 19U * rgb[offset + 2] + 128U) >> 8U;
      ++histogram.at(luma);
      ++samples;
    }
  }
  if (samples == 0) throw std::runtime_error("Aravis imaging metering selected no samples");
  const auto midpoint = (samples + 1U) / 2U;
  std::size_t cumulative = 0;
  for (std::size_t value = 0; value < histogram.size(); ++value) {
    cumulative += histogram[value];
    if (cumulative >= midpoint) return static_cast<int>(value);
  }
  return 255;
}

void run_imaging_self_test() {
  Arguments arguments;
  arguments.auto_exposure = true;
  arguments.auto_gain = true;
  ImagingBounds bounds{100.0, 12000.0, 0.0, 12.0, true, true};

  const auto dark = next_imaging_adjustment(arguments, 20, bounds, {1000.0, 0.0});
  if (!dark.exposure_changed || dark.gain_changed || dark.state.exposure_us <= 1000.0) {
    throw std::runtime_error("dark-frame self-test did not prioritize exposure");
  }
  const auto gain = next_imaging_adjustment(arguments, 20, bounds, {12000.0, 0.0});
  if (gain.exposure_changed || !gain.gain_changed || gain.state.gain <= 0.0) {
    throw std::runtime_error("dark-frame self-test did not use bounded gain after exposure");
  }
  const auto bright = next_imaging_adjustment(arguments, 180, bounds, {12000.0, 6.0});
  if (bright.exposure_changed || !bright.gain_changed || bright.state.gain >= 6.0) {
    throw std::runtime_error("bright-frame self-test did not reduce gain first");
  }
  const auto stable = next_imaging_adjustment(arguments, 80, bounds, {6000.0, 2.0});
  if (stable.exposure_changed || stable.gain_changed) {
    throw std::runtime_error("deadband self-test changed a stable image");
  }

  std::vector<unsigned char> rgb(9U * 3U, 0U);
  for (std::size_t pixel = 0; pixel < 9U; ++pixel) {
    rgb[pixel * 3U] = 80U;
    rgb[pixel * 3U + 1U] = 80U;
    rgb[pixel * 3U + 2U] = 80U;
  }
  if (sampled_luma(rgb, 3, 3, "full") != 80) {
    throw std::runtime_error("luma metering self-test changed a neutral frame");
  }
  std::vector<unsigned char> center_rgb(25U * 3U, 10U);
  for (int y = 1; y < 4; ++y) {
    for (int x = 1; x < 4; ++x) {
      const auto offset = (static_cast<std::size_t>(y) * 5U + static_cast<std::size_t>(x)) * 3U;
      center_rgb[offset] = 90U;
      center_rgb[offset + 1U] = 90U;
      center_rgb[offset + 2U] = 90U;
    }
  }
  if (sampled_luma(center_rgb, 5, 5, "center") != 90) {
    throw std::runtime_error("center metering self-test included the frame border");
  }
  std::cout << "aravis_imaging_self_test=passed\n";
}

enum class BayerColor { Red, Green, Blue };

BayerColor bayer_color(std::string_view format, int x, int y) {
  const bool even_x = (x & 1) == 0;
  const bool even_y = (y & 1) == 0;
  if (format == "BayerRG8") {
    if (even_y) return even_x ? BayerColor::Red : BayerColor::Green;
    return even_x ? BayerColor::Green : BayerColor::Blue;
  }
  if (format == "BayerBG8") {
    if (even_y) return even_x ? BayerColor::Blue : BayerColor::Green;
    return even_x ? BayerColor::Green : BayerColor::Red;
  }
  if (format == "BayerGR8") {
    if (even_y) return even_x ? BayerColor::Green : BayerColor::Red;
    return even_x ? BayerColor::Blue : BayerColor::Green;
  }
  if (even_y) return even_x ? BayerColor::Green : BayerColor::Blue;
  return even_x ? BayerColor::Red : BayerColor::Green;
}

unsigned char average(unsigned int a, unsigned int b) {
  return static_cast<unsigned char>((a + b + 1U) / 2U);
}

unsigned char average(unsigned int a, unsigned int b, unsigned int c, unsigned int d) {
  return static_cast<unsigned char>((a + b + c + d + 2U) / 4U);
}

std::vector<unsigned char> bayer_to_rgb(
    const unsigned char* input, int width, int height, std::size_t stride, std::string_view format) {
  std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
  const auto sample = [&](int x, int y) -> unsigned char {
    x = std::clamp(x, 0, width - 1);
    y = std::clamp(y, 0, height - 1);
    return input[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)];
  };

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      unsigned char red = 0;
      unsigned char green = 0;
      unsigned char blue = 0;
      const auto center = sample(x, y);
      const auto color = bayer_color(format, x, y);
      if (color == BayerColor::Red) {
        red = center;
        green = average(sample(x - 1, y), sample(x + 1, y), sample(x, y - 1), sample(x, y + 1));
        blue = average(sample(x - 1, y - 1), sample(x + 1, y - 1), sample(x - 1, y + 1), sample(x + 1, y + 1));
      } else if (color == BayerColor::Blue) {
        blue = center;
        green = average(sample(x - 1, y), sample(x + 1, y), sample(x, y - 1), sample(x, y + 1));
        red = average(sample(x - 1, y - 1), sample(x + 1, y - 1), sample(x - 1, y + 1), sample(x + 1, y + 1));
      } else {
        green = center;
        if (bayer_color(format, x ^ 1, y) == BayerColor::Red) {
          red = average(sample(x - 1, y), sample(x + 1, y));
          blue = average(sample(x, y - 1), sample(x, y + 1));
        } else {
          blue = average(sample(x - 1, y), sample(x + 1, y));
          red = average(sample(x, y - 1), sample(x, y + 1));
        }
      }
      const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)) * 3U;
      rgb[offset] = red;
      rgb[offset + 1] = green;
      rgb[offset + 2] = blue;
    }
  }
  return rgb;
}

std::vector<unsigned char> frame_to_rgb(ArvBuffer* buffer) {
  const int width = arv_buffer_get_image_width(buffer);
  const int height = arv_buffer_get_image_height(buffer);
  const auto pixel_format = arv_buffer_get_image_pixel_format(buffer);
  if (width <= 0 || height <= 0) throw std::runtime_error("Aravis returned invalid image dimensions");

  size_t size = 0;
#if ARAVIS_CHECK_VERSION(0, 8, 25)
  const auto* data = static_cast<const unsigned char*>(arv_buffer_get_image_data(buffer, &size));
#else
  const auto* data = static_cast<const unsigned char*>(arv_buffer_get_data(buffer, &size));
#endif
  if (data == nullptr) throw std::runtime_error("Aravis returned an empty image buffer");
  gint x_padding = 0;
  gint y_padding = 0;
#if ARAVIS_CHECK_VERSION(0, 8, 23)
  arv_buffer_get_image_padding(buffer, &x_padding, &y_padding);
#endif
  if (x_padding < 0 || y_padding < 0) throw std::runtime_error("Aravis returned invalid image padding");

  if (pixel_format == ARV_PIXEL_FORMAT_RGB_8_PACKED) {
    const auto stride = static_cast<std::size_t>(width) * 3U + static_cast<std::size_t>(x_padding);
    if (size < stride * static_cast<std::size_t>(height)) throw std::runtime_error("short RGB8 image buffer");
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
    for (int y = 0; y < height; ++y) {
      std::memcpy(
          rgb.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width) * 3U,
          data + static_cast<std::size_t>(y) * stride,
          static_cast<std::size_t>(width) * 3U);
    }
    return rgb;
  }

  const auto stride = static_cast<std::size_t>(width) + static_cast<std::size_t>(x_padding);
  if (size < stride * static_cast<std::size_t>(height)) throw std::runtime_error("short 8-bit image buffer");
  if (pixel_format == ARV_PIXEL_FORMAT_MONO_8) {
    std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
    for (int y = 0; y < height; ++y) {
      for (int x = 0; x < width; ++x) {
        const auto value = data[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x)];
        const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                             static_cast<std::size_t>(x)) * 3U;
        rgb[offset] = value;
        rgb[offset + 1] = value;
        rgb[offset + 2] = value;
      }
    }
    return rgb;
  }

  std::string format;
  if (pixel_format == ARV_PIXEL_FORMAT_BAYER_RG_8) format = "BayerRG8";
  else if (pixel_format == ARV_PIXEL_FORMAT_BAYER_BG_8) format = "BayerBG8";
  else if (pixel_format == ARV_PIXEL_FORMAT_BAYER_GR_8) format = "BayerGR8";
  else if (pixel_format == ARV_PIXEL_FORMAT_BAYER_GB_8) format = "BayerGB8";
  else throw std::runtime_error("Aravis returned an unsupported pixel format");
  return bayer_to_rgb(data, width, height, stride, format);
}

struct JpegErrorManager {
  jpeg_error_mgr base;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX]{};
};

void jpeg_error_exit(j_common_ptr compressor) {
  auto* error = reinterpret_cast<JpegErrorManager*>(compressor->err);
  (*compressor->err->format_message)(compressor, error->message);
  longjmp(error->jump, 1);
}

std::vector<unsigned char> encode_jpeg(
    const std::vector<unsigned char>& rgb, int width, int height, int quality) {
  jpeg_compress_struct compressor{};
  JpegErrorManager error{};
  compressor.err = jpeg_std_error(&error.base);
  error.base.error_exit = jpeg_error_exit;
  unsigned char* encoded = nullptr;
  unsigned long encoded_size = 0;
  if (setjmp(error.jump) != 0) {
    jpeg_destroy_compress(&compressor);
    std::free(encoded);
    throw std::runtime_error(std::string("Aravis JPEG conversion failed: ") + error.message);
  }
  jpeg_create_compress(&compressor);
  jpeg_mem_dest(&compressor, &encoded, &encoded_size);
  compressor.image_width = static_cast<JDIMENSION>(width);
  compressor.image_height = static_cast<JDIMENSION>(height);
  compressor.input_components = 3;
  compressor.in_color_space = JCS_RGB;
  jpeg_set_defaults(&compressor);
  jpeg_set_quality(&compressor, quality, TRUE);
  jpeg_start_compress(&compressor, TRUE);
  const auto stride = static_cast<std::size_t>(width) * 3U;
  while (compressor.next_scanline < compressor.image_height) {
    auto* row = const_cast<unsigned char*>(rgb.data() + compressor.next_scanline * stride);
    jpeg_write_scanlines(&compressor, &row, 1);
  }
  jpeg_finish_compress(&compressor);
  std::vector<unsigned char> output(encoded, encoded + encoded_size);
  jpeg_destroy_compress(&compressor);
  std::free(encoded);
  return output;
}

void discard_error(GError*& error) {
  if (error != nullptr) {
    g_error_free(error);
    error = nullptr;
  }
}

bool feature_available(ArvCamera* camera, const char* feature) {
  GError* error = nullptr;
  const auto available = arv_camera_is_feature_available(camera, feature, &error);
  discard_error(error);
  return available;
}

bool set_numeric_feature(ArvCamera* camera, const char* feature, double value) {
  if (!feature_available(camera, feature)) return false;
  GError* error = nullptr;
  arv_camera_set_float(camera, feature, value, &error);
  if (error == nullptr) return true;
  discard_error(error);
  arv_camera_set_integer(camera, feature, static_cast<gint64>(std::llround(value)), &error);
  if (error == nullptr) return true;
  discard_error(error);
  return false;
}

template <std::size_t Size>
bool set_first_numeric_feature(
    ArvCamera* camera,
    const std::array<const char*, Size>& features,
    double value) {
  for (const auto* feature : features) {
    if (set_numeric_feature(camera, feature, value)) return true;
  }
  return false;
}

bool set_normalized_feature(ArvCamera* camera, const char* feature, double normalized) {
  if (!feature_available(camera, feature)) return false;
  normalized = std::clamp(normalized, 0.0, 1.0);
  GError* error = nullptr;
  double float_min = 0.0;
  double float_max = 0.0;
  arv_camera_get_float_bounds(camera, feature, &float_min, &float_max, &error);
  if (error == nullptr && float_max >= float_min) {
    arv_camera_set_float(camera, feature, float_min + normalized * (float_max - float_min), &error);
    if (error == nullptr) return true;
  }
  discard_error(error);
  gint64 integer_min = 0;
  gint64 integer_max = 0;
  arv_camera_get_integer_bounds(camera, feature, &integer_min, &integer_max, &error);
  if (error == nullptr && integer_max >= integer_min) {
    const auto value =
        integer_min + static_cast<gint64>(std::llround(normalized * static_cast<double>(integer_max - integer_min)));
    arv_camera_set_integer(camera, feature, value, &error);
    if (error == nullptr) return true;
  }
  discard_error(error);
  return false;
}

bool set_auto_target(ArvCamera* camera, int target_luma) {
  constexpr std::array<const char*, 3> features{
      "AutoTargetBrightness",
      "AutoTargetValue",
      "AutoTargetGreyValue",
  };
  for (const auto* feature : features) {
    if (set_normalized_feature(camera, feature, static_cast<double>(target_luma) / 255.0)) return true;
  }
  return false;
}

bool exposure_auto_available(ArvCamera* camera) {
  GError* error = nullptr;
  const auto available = arv_camera_is_exposure_auto_available(camera, &error);
  discard_error(error);
  return available;
}

bool gain_auto_available(ArvCamera* camera) {
  GError* error = nullptr;
  const auto available = arv_camera_is_gain_auto_available(camera, &error);
  discard_error(error);
  return available;
}

bool set_exposure_auto(ArvCamera* camera, ArvAuto mode) {
  if (!exposure_auto_available(camera)) return false;
  GError* error = nullptr;
  arv_camera_set_exposure_time_auto(camera, mode, &error);
  if (error == nullptr) return true;
  discard_error(error);
  return false;
}

bool set_gain_auto(ArvCamera* camera, ArvAuto mode) {
  if (!gain_auto_available(camera)) return false;
  GError* error = nullptr;
  arv_camera_set_gain_auto(camera, mode, &error);
  if (error == nullptr) return true;
  discard_error(error);
  return false;
}

std::optional<std::pair<double, double>> exposure_bounds(ArvCamera* camera) {
  GError* error = nullptr;
  if (!arv_camera_is_exposure_time_available(camera, &error) || error != nullptr) {
    discard_error(error);
    return std::nullopt;
  }
  double minimum = 0.0;
  double maximum = 0.0;
  arv_camera_get_exposure_time_bounds(camera, &minimum, &maximum, &error);
  if (error != nullptr || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum < minimum) {
    discard_error(error);
    return std::nullopt;
  }
  return std::pair{minimum, maximum};
}

std::optional<std::pair<double, double>> gain_bounds(ArvCamera* camera) {
  GError* error = nullptr;
  if (!arv_camera_is_gain_available(camera, &error) || error != nullptr) {
    discard_error(error);
    return std::nullopt;
  }
  double minimum = 0.0;
  double maximum = 0.0;
  arv_camera_get_gain_bounds(camera, &minimum, &maximum, &error);
  if (error != nullptr || !std::isfinite(minimum) || !std::isfinite(maximum) || maximum < minimum) {
    discard_error(error);
    return std::nullopt;
  }
  return std::pair{minimum, maximum};
}

std::optional<double> current_exposure(ArvCamera* camera) {
  GError* error = nullptr;
  const auto value = arv_camera_get_exposure_time(camera, &error);
  if (error != nullptr || !std::isfinite(value)) {
    discard_error(error);
    return std::nullopt;
  }
  return value;
}

std::optional<double> current_gain(ArvCamera* camera) {
  GError* error = nullptr;
  const auto value = arv_camera_get_gain(camera, &error);
  if (error != nullptr || !std::isfinite(value)) {
    discard_error(error);
    return std::nullopt;
  }
  return value;
}

bool set_exposure(ArvCamera* camera, double value) {
  GError* error = nullptr;
  arv_camera_set_exposure_time(camera, value, &error);
  if (error == nullptr) return true;
  discard_error(error);
  return false;
}

bool set_gain(ArvCamera* camera, double value) {
  GError* error = nullptr;
  arv_camera_set_gain(camera, value, &error);
  if (error == nullptr) return true;
  discard_error(error);
  return false;
}

class Camera {
 public:
  Camera(const DeviceDescription& device, const Arguments& arguments)
      : timeout_ms_(arguments.timeout_ms), jpeg_quality_(arguments.jpeg_quality) {
    GError* error = nullptr;
    auto* camera = arv_camera_new(device.id.c_str(), &error);
    camera_.reset(check_pointer(camera, error, "open camera " + device.id));

    error = nullptr;
    arv_camera_set_region(camera_.get(), 0, 0, arguments.width, arguments.height, &error);
    check_error(error, "set camera region");

    gint actual_width = 0;
    gint actual_height = 0;
    error = nullptr;
    arv_camera_get_region(camera_.get(), nullptr, nullptr, &actual_width, &actual_height, &error);
    check_error(error, "read camera region");
    if (actual_width != arguments.width || actual_height != arguments.height) {
      throw std::runtime_error(
          "camera rejected requested region " + std::to_string(arguments.width) + "x" +
          std::to_string(arguments.height) + "; actual=" + std::to_string(actual_width) + "x" +
          std::to_string(actual_height));
    }
    width_ = actual_width;
    height_ = actual_height;

    pixel_format_ = select_pixel_format(camera_.get());
    error = nullptr;
    arv_camera_set_frame_rate(camera_.get(), static_cast<double>(arguments.fps), &error);
    check_error(error, "set acquisition frame rate");
    arv_camera_set_acquisition_mode(camera_.get(), ARV_ACQUISITION_MODE_CONTINUOUS, nullptr);
    configure_imaging(arguments);
    if (arv_camera_is_uv_device(camera_.get())) {
      arv_camera_uv_set_usb_mode(camera_.get(), ARV_UV_USB_MODE_ASYNC);
    }

    error = nullptr;
    const auto payload_size = arv_camera_get_payload(camera_.get(), &error);
    check_error(error, "read camera payload size");
    if (payload_size <= 0) throw std::runtime_error("camera reported an invalid payload size");

    error = nullptr;
    auto* stream = arv_camera_create_stream(camera_.get(), nullptr, nullptr, &error);
    stream_.reset(check_pointer(stream, error, "create camera stream"));
    for (int index = 0; index < 8; ++index) {
      arv_stream_push_buffer(stream_.get(), arv_buffer_new(static_cast<std::size_t>(payload_size), nullptr));
    }

    error = nullptr;
    arv_camera_start_acquisition(camera_.get(), &error);
    check_error(error, "start camera acquisition");
    acquiring_ = true;
  }

  ~Camera() {
    if (acquiring_) {
      GError* error = nullptr;
      arv_camera_stop_acquisition(camera_.get(), &error);
      if (error != nullptr) g_error_free(error);
    }
  }

  std::vector<unsigned char> jpeg() {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms_);
    while (true) {
      const auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                                 deadline - std::chrono::steady_clock::now())
                                 .count();
      if (remaining <= 0) throw std::runtime_error("timed out waiting for an Aravis camera frame");
      ArvBuffer* buffer = arv_stream_timeout_pop_buffer(stream_.get(), static_cast<guint64>(remaining));
      if (buffer == nullptr) throw std::runtime_error("timed out waiting for an Aravis camera frame");
      struct ReturnBuffer {
        ArvStream* stream;
        ArvBuffer* buffer;
        ~ReturnBuffer() { arv_stream_push_buffer(stream, buffer); }
      } return_buffer{stream_.get(), buffer};
      if (arv_buffer_get_status(buffer) != ARV_BUFFER_STATUS_SUCCESS) continue;
      const int width = arv_buffer_get_image_width(buffer);
      const int height = arv_buffer_get_image_height(buffer);
      if (width != width_ || height != height_) throw std::runtime_error("camera frame dimensions changed while streaming");
      const auto rgb = frame_to_rgb(buffer);
      update_imaging(rgb);
      return encode_jpeg(rgb, width, height, jpeg_quality_);
    }
  }

 private:
  enum class ImagingMode { Disabled, Native, Software };

  void configure_imaging(const Arguments& arguments) {
    imaging_arguments_ = arguments;
    diagnostics_interval_frames_ = std::max(arguments.fps * 5, arguments.update_interval_frames);
    if (!arguments.auto_exposure && !arguments.auto_gain) return;

    const auto device_exposure_bounds = exposure_bounds(camera_.get());
    if (arguments.auto_exposure && device_exposure_bounds) {
      imaging_bounds_.exposure_min_us =
          std::max(arguments.exposure_min_us, device_exposure_bounds->first);
      imaging_bounds_.exposure_max_us =
          std::min(arguments.exposure_max_us, device_exposure_bounds->second);
      imaging_bounds_.exposure_available =
          imaging_bounds_.exposure_max_us >= imaging_bounds_.exposure_min_us;
    }

    const auto device_gain_bounds = gain_bounds(camera_.get());
    if (arguments.auto_gain && device_gain_bounds) {
      const auto range = device_gain_bounds->second - device_gain_bounds->first;
      imaging_bounds_.gain_min =
          device_gain_bounds->first + arguments.gain_min_fraction * range;
      imaging_bounds_.gain_max =
          device_gain_bounds->first + arguments.gain_max_fraction * range;
      imaging_bounds_.gain_available = imaging_bounds_.gain_max >= imaging_bounds_.gain_min;
    }

    constexpr std::array<const char*, 2> exposure_lower_features{
        "AutoExposureTimeLowerLimit",
        "AutoExposureTimeAbsLowerLimit",
    };
    constexpr std::array<const char*, 2> exposure_upper_features{
        "AutoExposureTimeUpperLimit",
        "AutoExposureTimeAbsUpperLimit",
    };
    constexpr std::array<const char*, 2> gain_lower_features{
        "AutoGainLowerLimit",
        "AutoGainRawLowerLimit",
    };
    constexpr std::array<const char*, 2> gain_upper_features{
        "AutoGainUpperLimit",
        "AutoGainRawUpperLimit",
    };

    // Generic camera auto-exposure uses the camera's full-frame metering
    // defaults. Keep center metering in the software loop so the configured
    // mode is never silently ignored.
    bool native_ready =
        arguments.metering == "full" && set_auto_target(camera_.get(), arguments.target_luma);
    if (arguments.auto_exposure) {
      native_ready =
          native_ready && imaging_bounds_.exposure_available &&
          set_first_numeric_feature(
              camera_.get(), exposure_lower_features, imaging_bounds_.exposure_min_us) &&
          set_first_numeric_feature(
              camera_.get(), exposure_upper_features, imaging_bounds_.exposure_max_us);
    }
    if (arguments.auto_gain) {
      native_ready =
          native_ready && imaging_bounds_.gain_available &&
          set_first_numeric_feature(camera_.get(), gain_lower_features, imaging_bounds_.gain_min) &&
          set_first_numeric_feature(camera_.get(), gain_upper_features, imaging_bounds_.gain_max);
    }

    bool native_exposure_enabled = false;
    bool native_gain_enabled = false;
    if (native_ready) {
      native_exposure_enabled =
          !arguments.auto_exposure || set_exposure_auto(camera_.get(), ARV_AUTO_CONTINUOUS);
      native_gain_enabled = !arguments.auto_gain || set_gain_auto(camera_.get(), ARV_AUTO_CONTINUOUS);
      if (native_exposure_enabled && native_gain_enabled) {
        imaging_mode_ = ImagingMode::Native;
        log_imaging_state("aravis_imaging_configured", -1);
        return;
      }
    }

    if (arguments.auto_exposure) static_cast<void>(set_exposure_auto(camera_.get(), ARV_AUTO_OFF));
    if (arguments.auto_gain) static_cast<void>(set_gain_auto(camera_.get(), ARV_AUTO_OFF));

    if (imaging_bounds_.exposure_available) {
      const auto initial = current_exposure(camera_.get()).value_or(imaging_bounds_.exposure_min_us);
      imaging_state_.exposure_us =
          std::clamp(initial, imaging_bounds_.exposure_min_us, imaging_bounds_.exposure_max_us);
      if (!set_exposure(camera_.get(), imaging_state_.exposure_us)) {
        imaging_bounds_.exposure_available = false;
      }
    }
    if (imaging_bounds_.gain_available) {
      const auto initial = current_gain(camera_.get()).value_or(imaging_bounds_.gain_min);
      imaging_state_.gain = std::clamp(initial, imaging_bounds_.gain_min, imaging_bounds_.gain_max);
      if (!set_gain(camera_.get(), imaging_state_.gain)) imaging_bounds_.gain_available = false;
    }

    imaging_mode_ =
        imaging_bounds_.exposure_available || imaging_bounds_.gain_available
        ? ImagingMode::Software
        : ImagingMode::Disabled;
    log_imaging_state(
        imaging_mode_ == ImagingMode::Software
            ? "aravis_imaging_software_fallback"
            : "aravis_imaging_unavailable",
        -1);
  }

  void update_imaging(const std::vector<unsigned char>& rgb) {
    ++imaging_frame_count_;
    if (imaging_mode_ == ImagingMode::Disabled) return;
    const bool adjustment_due =
        imaging_frame_count_ % static_cast<std::uint64_t>(imaging_arguments_.update_interval_frames) == 0;
    const bool diagnostics_due =
        imaging_frame_count_ % static_cast<std::uint64_t>(diagnostics_interval_frames_) == 0;
    if (!adjustment_due && !diagnostics_due) return;

    const auto luma = sampled_luma(rgb, width_, height_, imaging_arguments_.metering);
    if (imaging_mode_ == ImagingMode::Software && adjustment_due) {
      const auto adjustment =
          next_imaging_adjustment(imaging_arguments_, luma, imaging_bounds_, imaging_state_);
      if (adjustment.exposure_changed) {
        if (set_exposure(camera_.get(), adjustment.state.exposure_us)) {
          imaging_state_.exposure_us = adjustment.state.exposure_us;
        } else {
          imaging_bounds_.exposure_available = false;
        }
      }
      if (adjustment.gain_changed) {
        if (set_gain(camera_.get(), adjustment.state.gain)) {
          imaging_state_.gain = adjustment.state.gain;
        } else {
          imaging_bounds_.gain_available = false;
        }
      }
      if (!imaging_bounds_.exposure_available && !imaging_bounds_.gain_available) {
        imaging_mode_ = ImagingMode::Disabled;
        log_imaging_state("aravis_imaging_disabled_after_error", luma);
        return;
      }
    }
    if (diagnostics_due) log_imaging_state("aravis_imaging_status", luma);
  }

  void log_imaging_state(std::string_view event, int luma) {
    auto exposure = imaging_state_.exposure_us;
    auto gain = imaging_state_.gain;
    if (imaging_mode_ == ImagingMode::Native) {
      exposure = current_exposure(camera_.get()).value_or(exposure);
      gain = current_gain(camera_.get()).value_or(gain);
    }
    const char* mode = "disabled";
    if (imaging_mode_ == ImagingMode::Native) mode = "native";
    else if (imaging_mode_ == ImagingMode::Software) mode = "software";
    std::cerr << std::fixed << std::setprecision(2)
              << "{\"event\":\"" << event
              << "\",\"mode\":\"" << mode
              << "\",\"target_luma\":" << imaging_arguments_.target_luma
              << ",\"metering\":\"" << imaging_arguments_.metering << '"'
              << ",\"luma\":" << luma
              << ",\"exposure_us\":" << exposure
              << ",\"exposure_min_us\":" << imaging_bounds_.exposure_min_us
              << ",\"exposure_max_us\":" << imaging_bounds_.exposure_max_us
              << ",\"gain\":" << gain
              << ",\"gain_min\":" << imaging_bounds_.gain_min
              << ",\"gain_max\":" << imaging_bounds_.gain_max
              << "}\n";
  }

  std::unique_ptr<ArvCamera, GObjectUnref> camera_;
  std::unique_ptr<ArvStream, GObjectUnref> stream_;
  Arguments imaging_arguments_;
  ImagingBounds imaging_bounds_;
  ImagingState imaging_state_;
  ImagingMode imaging_mode_{ImagingMode::Disabled};
  std::uint64_t imaging_frame_count_{0};
  int diagnostics_interval_frames_{150};
  int timeout_ms_{0};
  int jpeg_quality_{80};
  int width_{0};
  int height_{0};
  std::string pixel_format_;
  bool acquiring_{false};
};

}  // namespace

int main(int argc, char** argv) {
  try {
    const auto arguments = parse_arguments(argc, argv);
    if (arguments.self_test) {
      run_imaging_self_test();
      return 0;
    }
    AravisLifecycle lifecycle;
    const auto devices = enumerate_devices();
    if (arguments.list) {
      list_devices(devices, arguments.json);
      return 0;
    }
    Camera camera(select_device(devices, arguments), arguments);
    int emitted = 0;
    while (arguments.frames == 0 || emitted < arguments.frames) {
      const auto jpeg = camera.jpeg();
      std::cout.write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
      std::cout.flush();
      ++emitted;
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "mine-teleop-aravis-camera: " << error.what() << '\n';
    return 2;
  }
}
