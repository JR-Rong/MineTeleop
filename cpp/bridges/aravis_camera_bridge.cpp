#include <arv.h>
#include <cstdio>
#include <jpeglib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <setjmp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct Arguments {
  bool list{false};
  bool json{false};
  int device_index{0};
  std::string serial;
  std::string model;
  int width{1280};
  int height{720};
  int fps{30};
  int frames{0};
  int timeout_ms{2000};
  int jpeg_quality{80};
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

Arguments parse_arguments(int argc, char** argv) {
  Arguments arguments;
  for (int index = 1; index < argc; ++index) {
    const std::string option(argv[index]);
    if (option == "--list") arguments.list = true;
    else if (option == "--json") arguments.json = true;
    else if (option == "--device-index") arguments.device_index = integer(required_value(index, argc, argv, option), option);
    else if (option == "--serial") arguments.serial = required_value(index, argc, argv, option);
    else if (option == "--model") arguments.model = required_value(index, argc, argv, option);
    else if (option == "--width") arguments.width = integer(required_value(index, argc, argv, option), option);
    else if (option == "--height") arguments.height = integer(required_value(index, argc, argv, option), option);
    else if (option == "--fps") arguments.fps = integer(required_value(index, argc, argv, option), option);
    else if (option == "--frames") arguments.frames = integer(required_value(index, argc, argv, option), option);
    else if (option == "--timeout-ms") arguments.timeout_ms = integer(required_value(index, argc, argv, option), option);
    else if (option == "--jpeg-quality") arguments.jpeg_quality = integer(required_value(index, argc, argv, option), option);
    else if (option == "--help" || option == "-h") {
      std::cout << "Usage: mine-teleop-aravis-camera [--list --json] "
                   "[--device-index N|--serial S|--model M] [--width W] [--height H] "
                   "[--fps FPS] [--frames N] [--timeout-ms MS] [--jpeg-quality 1..99]\n";
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
  AravisLifecycle() { arv_select_interface("USB3Vision"); }
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
  const auto* data = static_cast<const unsigned char*>(arv_buffer_get_image_data(buffer, &size));
  if (data == nullptr) throw std::runtime_error("Aravis returned an empty image buffer");
  gint x_padding = 0;
  gint y_padding = 0;
  arv_buffer_get_image_padding(buffer, &x_padding, &y_padding);
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
      return encode_jpeg(frame_to_rgb(buffer), width, height, jpeg_quality_);
    }
  }

 private:
  std::unique_ptr<ArvCamera, GObjectUnref> camera_;
  std::unique_ptr<ArvStream, GObjectUnref> stream_;
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
