#include "mine_teleop/media.hpp"
#include "mine_teleop/detail/media_freshness.hpp"

#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <jpeglib.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <setjmp.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace mine_teleop {

CameraSourceKind classify_camera_source(std::string_view device) {
  if (device == "testsrc") return CameraSourceKind::TestSource;
  if (device == "mvs" || device.starts_with("mvs:") ||
      device == "hikrobot" || device.starts_with("hikrobot:")) {
    return CameraSourceKind::Mvs;
  }
  if (device == "aravis" || device.starts_with("aravis:") ||
      device == "basler" || device.starts_with("basler:") ||
      device == "pylon" || device.starts_with("pylon:")) {
    return CameraSourceKind::Aravis;
  }
  return CameraSourceKind::V4l2;
}

CameraSourceKind classify_camera_source(const CameraConfig& camera) {
  if (camera.backend == "auto") return classify_camera_source(camera.device);
  if (camera.backend == "v4l2_uyvy") return CameraSourceKind::V4l2Uyvy;
  if (camera.backend == "ccg2") return CameraSourceKind::Ccg2;
  throw std::invalid_argument("unsupported camera backend: " + camera.backend);
}

std::string_view camera_source_kind_name(CameraSourceKind kind) {
  switch (kind) {
    case CameraSourceKind::TestSource:
      return "testsrc";
    case CameraSourceKind::Mvs:
      return "vendor_sdk";
    case CameraSourceKind::Aravis:
      return "aravis";
    case CameraSourceKind::V4l2:
      return "v4l2";
    case CameraSourceKind::Ccg2:
      return "ccg2";
    case CameraSourceKind::V4l2Uyvy:
      return "v4l2_uyvy";
  }
  throw std::invalid_argument("unknown camera source kind");
}

CameraInputSpec camera_input_spec(
    const CameraConfig& camera,
    const MediaProfile& realtime_profile) {
  CameraInputSpec result{
      "mjpeg",
      realtime_profile.width,
      realtime_profile.height,
      realtime_profile.fps,
  };
  const auto kind = classify_camera_source(camera);
  if (kind != CameraSourceKind::Ccg2 && kind != CameraSourceKind::V4l2Uyvy) return result;
  if (camera.capture_width <= 0 || camera.capture_height <= 0 || camera.capture_fps <= 0 ||
      camera.capture_width % 2 != 0) {
    throw std::invalid_argument(
        "UYVY capture requires positive dimensions/FPS and an even width: " + camera.id);
  }
  result.codec = "uyvy";
  result.width = camera.capture_width;
  result.height = camera.capture_height;
  result.fps = camera.capture_fps;
  return result;
}

std::string pack_uyvy_rows(
    std::string_view frame,
    int width,
    int height,
    std::size_t bytes_per_line) {
  if (width <= 0 || height <= 0 || width % 2 != 0) {
    throw std::invalid_argument("UYVY frame requires positive dimensions and an even width");
  }
  const auto row_bytes = static_cast<std::size_t>(width) * 2U;
  const auto stride = bytes_per_line == 0 ? row_bytes : bytes_per_line;
  if (stride < row_bytes) {
    throw std::runtime_error("UYVY bytesperline is smaller than the visible row");
  }
  const auto rows_before_last = static_cast<std::size_t>(height - 1);
  if (rows_before_last > (std::numeric_limits<std::size_t>::max() - row_bytes) / stride) {
    throw std::overflow_error("UYVY frame layout overflows address space");
  }
  const auto required_bytes = rows_before_last * stride + row_bytes;
  if (frame.size() < required_bytes) {
    throw std::runtime_error(
        "UYVY frame is shorter than the negotiated visible image: bytesused=" +
        std::to_string(frame.size()) + ", required=" + std::to_string(required_bytes) +
        ", bytesperline=" + std::to_string(stride));
  }
  if (static_cast<std::size_t>(height) >
      std::numeric_limits<std::size_t>::max() / row_bytes) {
    throw std::overflow_error("UYVY packed frame size overflows address space");
  }
  std::string packed(static_cast<std::size_t>(height) * row_bytes, '\0');
  for (int row = 0; row < height; ++row) {
    std::memcpy(
        packed.data() + static_cast<std::size_t>(row) * row_bytes,
        frame.data() + static_cast<std::size_t>(row) * stride,
        row_bytes);
  }
  return packed;
}

std::uint64_t v4l2_sequence_gap(
    std::optional<std::uint32_t> previous_sequence,
    std::uint32_t current_sequence) {
  if (!previous_sequence) return 0;
  const auto distance = static_cast<std::uint32_t>(current_sequence - *previous_sequence);
  return distance == 0 ? 0 : static_cast<std::uint64_t>(distance - 1U);
}

std::string build_camera_input_pipeline(
    std::string_view source_name,
    const CameraInputSpec& input,
    const MediaProfile& output_profile) {
  if (source_name.empty() || input.width <= 0 || input.height <= 0 || input.fps <= 0 ||
      output_profile.width <= 0 || output_profile.height <= 0 || output_profile.fps <= 0) {
    throw std::invalid_argument("camera input pipeline configuration is invalid");
  }
  const bool uyvy = input.codec == "uyvy";
  if (!uyvy && input.codec != "mjpeg" && input.codec != "jpeg") {
    throw std::invalid_argument("camera input pipeline codec is unsupported: " + input.codec);
  }

  std::ostringstream pipeline;
  pipeline << "appsrc name=" << source_name
           << " is-live=true format=time do-timestamp=false emit-signals=false block=false max-buffers="
           << kCameraAppSrcMaxBuffers
           << (uyvy ? " max-bytes=0" : " max-bytes=524288")
           << " max-time=0 leaky-type=downstream ";
  if (uyvy) {
    pipeline << "caps=video/x-raw,format=UYVY,width=" << input.width
             << ",height=" << input.height << ",framerate=" << input.fps << "/1 ";
  } else {
    pipeline << "caps=image/jpeg,width=" << input.width << ",height=" << input.height
             << ",framerate=" << input.fps << "/1 ";
  }
  pipeline << "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ";
  if (!uyvy) pipeline << "! jpegdec ";
  // Device rate negotiation is advisory on some drivers. Apply a drop-only
  // raw-frame ceiling even when declared input FPS equals the target: actual
  // delivery may be faster. This never duplicates an old frame to fill a gap
  // and never discards an encoded reference access unit.
  pipeline << "! videorate drop-only=true max-rate=" << output_profile.fps
           << " ! videoconvert ! videoscale ";
  pipeline << "! video/x-raw,format=NV12,width=" << output_profile.width
           << ",height=" << output_profile.height << ",framerate=" << output_profile.fps << "/1 "
           << "! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream ";
  return pipeline.str();
}

std::string build_encoded_frame_queue(std::string_view name, int max_buffers) {
  if (name.empty() || max_buffers <= 0) {
    throw std::invalid_argument("encoded frame queue configuration is invalid");
  }
  // H.264/H.265 access units can reference earlier frames. Backpressure must
  // reach the raw-input queues, where dropping old frames is independent.
  return "queue name=" + std::string(name) + " max-size-buffers=" +
      std::to_string(max_buffers) +
      " max-size-bytes=0 max-size-time=0 leaky=no ";
}

namespace {

std::string environment_or(std::string_view name, std::string fallback) {
  const char* value = std::getenv(std::string(name).c_str());
  return value == nullptr || *value == '\0' ? std::move(fallback) : std::string(value);
}

std::string bundled_executable(std::string_view name) {
  if (const char* configured = std::getenv("MINE_TELEOP_EXECUTABLE_PATH"); configured != nullptr && *configured != '\0') {
    const std::filesystem::path executable(configured);
    if (executable.has_parent_path()) return (executable.parent_path() / name).string();
  }
  std::array<char, 4096> path{};
  const auto length = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (length > 0) {
    path[static_cast<std::size_t>(length)] = '\0';
    return (std::filesystem::path(path.data()).parent_path() / name).string();
  }
  return std::string(name);
}

void append_camera_selector(std::vector<std::string>& command, std::string_view device) {
  auto separator = device.find(':');
  auto selector = separator == std::string_view::npos ? std::string("0") : std::string(device.substr(separator + 1));
  if (selector.starts_with("index=")) {
    command.insert(command.end(), {"--device-index", selector.substr(6)});
  } else if (selector.starts_with("serial=")) {
    command.insert(command.end(), {"--serial", selector.substr(7)});
  } else if (selector.starts_with("model=")) {
    command.insert(command.end(), {"--model", selector.substr(6)});
  } else if (!selector.empty() && std::all_of(selector.begin(), selector.end(), [](unsigned char value) { return std::isdigit(value); })) {
    command.insert(command.end(), {"--device-index", selector});
  } else {
    command.insert(command.end(), {"--serial", selector});
  }
}

std::vector<std::string> build_vendor_bridge_command(const CameraConfig& camera, const MediaProfile& profile) {
  if (classify_camera_source(camera.device) == CameraSourceKind::Mvs) {
    std::vector<std::string> command{
        environment_or("MINE_TELEOP_MVS_BRIDGE_BIN", bundled_executable("mine-teleop-mvs-camera"))};
    append_camera_selector(command, camera.device);
    command.insert(command.end(), {
                                      "--width", std::to_string(profile.width),
                                      "--height", std::to_string(profile.height),
                                      "--fps", std::to_string(profile.fps),
                                      "--frames", "0",
                                      "--jpeg-quality", environment_or("MINE_TELEOP_MVS_JPEG_QUALITY", "80"),
                                  });
    return command;
  }
  if (classify_camera_source(camera.device) == CameraSourceKind::Aravis) {
    std::vector<std::string> command{
        environment_or("MINE_TELEOP_ARAVIS_BRIDGE_BIN", bundled_executable("mine-teleop-aravis-camera"))};
    append_camera_selector(command, camera.device);
    command.insert(command.end(), {
                                      "--width", std::to_string(profile.width),
                                      "--height", std::to_string(profile.height),
                                      "--fps", std::to_string(profile.fps),
                                      "--frames", "0",
                                      "--jpeg-quality", environment_or("MINE_TELEOP_ARAVIS_JPEG_QUALITY", "80"),
                                  });
    return command;
  }

  throw std::invalid_argument("camera is not a vendor SDK source: " + camera.device);
}

int ioctl_retry(int descriptor, unsigned long request, void* argument) {
  int result = 0;
  do {
    result = ::ioctl(descriptor, request, argument);
  } while (result < 0 && errno == EINTR);
  return result;
}

void require_jpeg(std::string_view payload, std::string_view camera_id) {
  if (payload.size() < 4 || static_cast<unsigned char>(payload[0]) != 0xFF ||
      static_cast<unsigned char>(payload[1]) != 0xD8 ||
      static_cast<unsigned char>(payload[payload.size() - 2]) != 0xFF ||
      static_cast<unsigned char>(payload[payload.size() - 1]) != 0xD9) {
    throw std::runtime_error("camera returned an invalid MJPEG frame: " + std::string(camera_id));
  }
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

std::string encode_rgb_jpeg(const std::vector<unsigned char>& rgb, int width, int height, int quality) {
  jpeg_compress_struct compressor{};
  JpegErrorManager error{};
  compressor.err = jpeg_std_error(&error.base);
  error.base.error_exit = jpeg_error_exit;
  unsigned char* encoded = nullptr;
  unsigned long encoded_size = 0;
  if (setjmp(error.jump) != 0) {
    jpeg_destroy_compress(&compressor);
    std::free(encoded);
    throw std::runtime_error(std::string("native JPEG encoder failed: ") + error.message);
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
  const std::size_t stride = static_cast<std::size_t>(width) * 3U;
  while (compressor.next_scanline < compressor.image_height) {
    auto* row = const_cast<unsigned char*>(rgb.data() + compressor.next_scanline * stride);
    jpeg_write_scanlines(&compressor, &row, 1);
  }
  jpeg_finish_compress(&compressor);
  std::string result(reinterpret_cast<char*>(encoded), encoded_size);
  jpeg_destroy_compress(&compressor);
  std::free(encoded);
  return result;
}

}  // namespace

CameraFrameSource::CameraFrameSource(CameraConfig camera, MediaProfile profile, int frame_timeout_ms)
    : CameraFrameSource(std::move(camera), std::move(profile), frame_timeout_ms, BenchPreopenedV4l2Fd{}) {}

CameraFrameSource::CameraFrameSource(CameraConfig camera, MediaProfile profile, int frame_timeout_ms, BenchPreopenedV4l2Fd bench)
    : camera_(std::move(camera)), profile_(std::move(profile)), frame_timeout_ms_(frame_timeout_ms), bench_preopened_(std::move(bench)) {
  if (camera_.id.empty() || profile_.width <= 0 || profile_.height <= 0 || profile_.fps <= 0) {
    throw std::invalid_argument("camera media source configuration is invalid");
  }
  if (frame_timeout_ms_ <= 0) throw std::invalid_argument("frame timeout must be positive");
  const auto source_kind = classify_camera_source(camera_);
  const bool raw_uyvy = source_kind == CameraSourceKind::Ccg2 || source_kind == CameraSourceKind::V4l2Uyvy;
  if (raw_uyvy && profile_.codec != "uyvy") {
    throw std::invalid_argument("UYVY camera acquisition requires a uyvy capture profile");
  }
  if (!raw_uyvy &&
      profile_.codec != "mjpeg" && profile_.codec != "jpeg") {
    throw std::invalid_argument("native camera acquisition requires an mjpeg realtime profile");
  }
  if (bench_preopened_.value >= 0 && (source_kind != CameraSourceKind::Ccg2 ||
      (camera_.device != "/dev/ccg2-channel-6" && camera_.device != "/dev/ccg2-channel-7")))
    throw std::invalid_argument("bench preopened FD is limited to explicit CCG2 channels 6/7");
  output_width_ = profile_.width;
  output_height_ = profile_.height;
  switch (source_kind) {
    case CameraSourceKind::TestSource:
      mode_ = Mode::TestSource;
      break;
    case CameraSourceKind::Mvs:
    case CameraSourceKind::Aravis:
      mode_ = Mode::VendorBridge;
      command_ = build_vendor_bridge_command(camera_, profile_);
      break;
    case CameraSourceKind::V4l2:
      mode_ = Mode::V4l2;
      break;
    case CameraSourceKind::Ccg2:
      mode_ = Mode::Ccg2;
      break;
    case CameraSourceKind::V4l2Uyvy:
      mode_ = Mode::V4l2Uyvy;
      break;
  }
}

CameraFrameSource::~CameraFrameSource() {
  stop_vendor_bridge();
  stop_v4l2();
}

void CameraFrameSource::start_vendor_bridge() {
  if (child_pid_ > 0) return;
  int stdout_pipe[2]{};
  if (::pipe(stdout_pipe) != 0) throw std::runtime_error(std::string("cannot create media pipe: ") + std::strerror(errno));
  const pid_t child = ::fork();
  if (child < 0) {
    ::close(stdout_pipe[0]);
    ::close(stdout_pipe[1]);
    throw std::runtime_error(std::string("cannot fork media process: ") + std::strerror(errno));
  }
  if (child == 0) {
    ::close(stdout_pipe[0]);
    if (::dup2(stdout_pipe[1], STDOUT_FILENO) < 0) _exit(126);
    ::close(stdout_pipe[1]);
    std::vector<char*> arguments;
    arguments.reserve(command_.size() + 1);
    for (auto& item : command_) arguments.push_back(item.data());
    arguments.push_back(nullptr);
    ::execvp(arguments.front(), arguments.data());
    _exit(errno == ENOENT ? 127 : 126);
  }
  ::close(stdout_pipe[1]);
  stdout_fd_ = stdout_pipe[0];
  child_pid_ = child;
}

void CameraFrameSource::stop_vendor_bridge() {
  if (child_pid_ > 0) ::kill(child_pid_, SIGTERM);
  if (stdout_fd_ >= 0) {
    ::close(stdout_fd_);
    stdout_fd_ = -1;
  }
  if (child_pid_ > 0) {
    for (int attempt = 0; attempt < 20; ++attempt) {
      int status = 0;
      const auto result = ::waitpid(child_pid_, &status, WNOHANG);
      if (result == child_pid_ || (result < 0 && errno == ECHILD)) {
        child_pid_ = -1;
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ::kill(child_pid_, SIGKILL);
    ::waitpid(child_pid_, nullptr, 0);
    child_pid_ = -1;
  }
}

std::string CameraFrameSource::read_vendor_jpeg() {
  start_vendor_bridge();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(frame_timeout_ms_);
  std::array<char, 256 * 1024> chunk{};
  while (true) {
    auto start_marker = buffer_.find("\xFF\xD8");
    if (start_marker != std::string::npos) {
      if (start_marker > 0) buffer_.erase(0, start_marker);
      const auto end_marker = buffer_.find("\xFF\xD9", 2);
      if (end_marker != std::string::npos) {
        const auto frame_end = end_marker + 2;
        auto frame = buffer_.substr(0, frame_end);
        buffer_.erase(0, frame_end);
        return frame;
      }
    } else if (buffer_.size() > 1) {
      buffer_.erase(0, buffer_.size() - 1);
    }
    if (buffer_.size() > 16 * 1024 * 1024) throw std::runtime_error("MJPEG frame exceeded 16 MiB");
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
    if (remaining <= 0) throw std::runtime_error("timed out waiting for camera frame: " + camera_.id);
    pollfd descriptor{stdout_fd_, POLLIN | POLLHUP, 0};
    int polled = 0;
    do {
      polled = ::poll(&descriptor, 1, static_cast<int>(remaining));
    } while (polled < 0 && errno == EINTR);
    if (polled == 0) throw std::runtime_error("timed out waiting for camera frame: " + camera_.id);
    if (polled < 0) throw std::runtime_error(std::string("media poll failed: ") + std::strerror(errno));
    const auto bytes = ::read(stdout_fd_, chunk.data(), chunk.size());
    if (bytes < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("media read failed: ") + std::strerror(errno));
    }
    if (bytes == 0) {
      int status = 0;
      const auto result = ::waitpid(child_pid_, &status, WNOHANG);
      if (result == child_pid_) child_pid_ = -1;
      throw std::runtime_error("camera process exited before producing a complete JPEG frame: " + camera_.id);
    }
    buffer_.append(chunk.data(), static_cast<std::size_t>(bytes));
  }
}

void CameraFrameSource::start_v4l2() {
  if (device_fd_ >= 0) return;
  device_fd_ = bench_preopened_.value >= 0 ? ::fcntl(bench_preopened_.value, F_DUPFD_CLOEXEC, 5) :
      ::open(camera_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (device_fd_ < 0) {
    throw std::runtime_error("cannot open V4L2 camera " + camera_.device + ": " + std::strerror(errno));
  }
  try {
    v4l2_capability capability{};
    if (ioctl_retry(device_fd_, VIDIOC_QUERYCAP, &capability) < 0) {
      throw std::runtime_error("VIDIOC_QUERYCAP failed for " + camera_.device + ": " + std::strerror(errno));
    }
    const auto capability_text = [](const auto& text) {
      return std::string(reinterpret_cast<const char*>(text),
                         strnlen(reinterpret_cast<const char*>(text), sizeof(text)));
    };
    v4l2_driver_ = capability_text(capability.driver);
    v4l2_card_ = capability_text(capability.card);
    v4l2_bus_info_ = capability_text(capability.bus_info);
    const auto capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                                  ? capability.device_caps
                                  : capability.capabilities;
    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 || (capabilities & V4L2_CAP_STREAMING) == 0) {
      throw std::runtime_error("camera must support V4L2 capture and streaming: " + camera_.device);
    }

    const bool ccg2 = mode_ == Mode::Ccg2;
    const bool raw_uyvy = ccg2 || mode_ == Mode::V4l2Uyvy;
    const std::string raw_label = ccg2 ? "CCG2" : "V4L2 UYVY";
    const auto requested_format = ccg2 ? V4L2_PIX_FMT_YUYV :
        (raw_uyvy ? V4L2_PIX_FMT_UYVY : V4L2_PIX_FMT_MJPEG);
    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<__u32>(profile_.width);
    format.fmt.pix.height = static_cast<__u32>(profile_.height);
    format.fmt.pix.pixelformat = requested_format;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    const auto format_result = ioctl_retry(device_fd_, VIDIOC_S_FMT, &format);
    const auto format_errno = format_result < 0 ? errno : 0;
    if (bench_preopened_.observe_ioctl) bench_preopened_.observe_ioctl("VIDIOC_S_FMT", format_result, format_errno,
        {{"width", format.fmt.pix.width}, {"height", format.fmt.pix.height}, {"pixelformat", format.fmt.pix.pixelformat}, {"field", format.fmt.pix.field}, {"bytesperline", format.fmt.pix.bytesperline}, {"sizeimage", format.fmt.pix.sizeimage}, {"colorspace", format.fmt.pix.colorspace}, {"priv", format.fmt.pix.priv}, {"flags", format.fmt.pix.flags}, {"ycbcr_enc", format.fmt.pix.ycbcr_enc}, {"quantization", format.fmt.pix.quantization}, {"xfer_func", format.fmt.pix.xfer_func}});
    errno = format_errno;
    if (format_result < 0) {
      if (ccg2) {
        throw std::runtime_error(
            "VIDIOC_S_FMT CCG2 YUYV failed for " + camera_.device + ": " + std::strerror(errno));
      }
      if (raw_uyvy) throw std::runtime_error("VIDIOC_S_FMT UYVY failed for " + camera_.device + ": " + std::strerror(errno));
      throw std::runtime_error(
          "VIDIOC_S_FMT MJPEG failed for " + camera_.device + ": " + std::strerror(errno));
    }
    if (format.fmt.pix.pixelformat != requested_format) {
      throw std::runtime_error(
          ccg2 ? "CCG2 driver did not negotiate reported YUYV: " + camera_.device
               : (raw_uyvy ? "V4L2 UYVY driver did not negotiate UYVY: " + camera_.device
                           : "camera does not provide native MJPEG: " + camera_.device));
    }
    output_width_ = static_cast<int>(format.fmt.pix.width);
    output_height_ = static_cast<int>(format.fmt.pix.height);
    if (raw_uyvy && (output_width_ != profile_.width || output_height_ != profile_.height)) {
      throw std::runtime_error(
          raw_label + " driver negotiated unexpected dimensions for " + camera_.device + ": requested " +
          std::to_string(profile_.width) + "x" + std::to_string(profile_.height) + ", got " +
          std::to_string(output_width_) + "x" + std::to_string(output_height_));
    }
    if (raw_uyvy) {
      if (output_width_ <= 0 || output_height_ <= 0 || output_width_ % 2 != 0) {
        throw std::runtime_error(raw_label + " driver returned invalid UYVY dimensions for " + camera_.device);
      }
      const auto row_bytes = static_cast<std::size_t>(output_width_) * 2U;
      v4l2_bytes_per_line_ = format.fmt.pix.bytesperline == 0
                                 ? row_bytes
                                 : static_cast<std::size_t>(format.fmt.pix.bytesperline);
      v4l2_size_image_ = static_cast<std::size_t>(format.fmt.pix.sizeimage);
      if (v4l2_bytes_per_line_ < row_bytes) {
        throw std::runtime_error(
            raw_label + " driver returned bytesperline smaller than the visible UYVY row for " +
            camera_.device + ": bytesperline=" + std::to_string(v4l2_bytes_per_line_) +
            ", row_bytes=" + std::to_string(row_bytes));
      }
      const auto rows_before_last = static_cast<std::size_t>(output_height_ - 1);
      if (rows_before_last >
          (std::numeric_limits<std::size_t>::max() - row_bytes) / v4l2_bytes_per_line_) {
        throw std::runtime_error(raw_label + " driver returned an overflowing UYVY layout for " + camera_.device);
      }
      const auto visible_bytes = rows_before_last * v4l2_bytes_per_line_ + row_bytes;
      if (v4l2_size_image_ != 0 && v4l2_size_image_ < visible_bytes) {
        throw std::runtime_error(
            raw_label + " driver returned sizeimage smaller than the visible UYVY image for " +
            camera_.device + ": sizeimage=" + std::to_string(v4l2_size_image_) +
            ", visible_bytes=" + std::to_string(visible_bytes));
      }
    }

    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = static_cast<__u32>(profile_.fps);
    const auto set_parameters_result = ioctl_retry(device_fd_, VIDIOC_S_PARM, &parameters);
    const auto parameters_errno = set_parameters_result < 0 ? errno : 0;
    if (bench_preopened_.observe_ioctl) bench_preopened_.observe_ioctl("VIDIOC_S_PARM", set_parameters_result, parameters_errno,
        {{"capturemode", parameters.parm.capture.capturemode}, {"capability", parameters.parm.capture.capability}, {"numerator", parameters.parm.capture.timeperframe.numerator}, {"denominator", parameters.parm.capture.timeperframe.denominator}, {"readbuffers", parameters.parm.capture.readbuffers}, {"extendedmode", parameters.parm.capture.extendedmode}});
    errno = parameters_errno;
    v4l2_timeperframe_set_accepted_ = set_parameters_result == 0;
    v4l2_set_timeperframe_numerator_ = set_parameters_result == 0 ? parameters.parm.capture.timeperframe.numerator : 0;
    v4l2_set_timeperframe_denominator_ = set_parameters_result == 0 ? parameters.parm.capture.timeperframe.denominator : 0;
    v4l2_timeperframe_supported_ = set_parameters_result == 0 &&
        (parameters.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) != 0;
    if (set_parameters_result < 0 && errno != EINVAL && errno != ENOTTY) {
      throw std::runtime_error("VIDIOC_S_PARM failed for " + camera_.device + ": " + std::strerror(errno));
    }
    // S_PARM's returned value is not evidence of actual acquisition cadence.
    // Read the driver's current nominal setting separately when supported.
    v4l2_streamparm readback{};
    readback.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    const auto readback_result = ioctl_retry(device_fd_, VIDIOC_G_PARM, &readback);
    const auto readback_errno = readback_result < 0 ? errno : 0;
    if (bench_preopened_.observe_ioctl) bench_preopened_.observe_ioctl("VIDIOC_G_PARM", readback_result, readback_errno,
        {{"capturemode", readback.parm.capture.capturemode}, {"capability", readback.parm.capture.capability}, {"numerator", readback.parm.capture.timeperframe.numerator}, {"denominator", readback.parm.capture.timeperframe.denominator}, {"readbuffers", readback.parm.capture.readbuffers}, {"extendedmode", readback.parm.capture.extendedmode}});
    v4l2_timeperframe_readback_ = readback_result == 0;
    if (v4l2_timeperframe_readback_) parameters = readback;
    if (set_parameters_result == 0 || v4l2_timeperframe_readback_) {
      v4l2_timeperframe_numerator_ = parameters.parm.capture.timeperframe.numerator;
      v4l2_timeperframe_denominator_ = parameters.parm.capture.timeperframe.denominator;
    }
    if (ccg2) {
      const auto numerator = parameters.parm.capture.timeperframe.numerator;
      const auto denominator = parameters.parm.capture.timeperframe.denominator;
      if (numerator == 0 || denominator == 0) {
        throw std::runtime_error(
            "CCG2 driver returned invalid timeperframe for " + camera_.device +
            ": numerator=" + std::to_string(numerator) +
            ", denominator=" + std::to_string(denominator));
      }
      // Unsupported nominal cadence does not invalidate a correctly negotiated
      // pixel layout. Keep video available with the raw drop-only ceiling and
      // report the driver's setting; actual delivery must still be measured.
      // Do not copy the requested S_PARM fields into a "negotiated" diagnostic
      // when both setting and readback are unsupported.
    }

    v4l2_requestbuffers request{};
    request.count = 4;
    request.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    request.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(device_fd_, VIDIOC_REQBUFS, &request) < 0 || request.count < 2) {
      throw std::runtime_error("V4L2 mmap buffers are unavailable for " + camera_.device);
    }
    mapped_buffers_.reserve(request.count);
    for (std::uint32_t index = 0; index < request.count; ++index) {
      v4l2_buffer buffer{};
      buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
      buffer.memory = V4L2_MEMORY_MMAP;
      buffer.index = index;
      if (ioctl_retry(device_fd_, VIDIOC_QUERYBUF, &buffer) < 0) {
        throw std::runtime_error("VIDIOC_QUERYBUF failed for " + camera_.device + ": " + std::strerror(errno));
      }
      void* address = ::mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, device_fd_, buffer.m.offset);
      if (address == MAP_FAILED) {
        throw std::runtime_error("mmap failed for " + camera_.device + ": " + std::strerror(errno));
      }
      mapped_buffers_.push_back({address, buffer.length});
      if (ioctl_retry(device_fd_, VIDIOC_QBUF, &buffer) < 0) {
        throw std::runtime_error("VIDIOC_QBUF failed for " + camera_.device + ": " + std::strerror(errno));
      }
    }
    auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl_retry(device_fd_, VIDIOC_STREAMON, &type) < 0) {
      throw std::runtime_error("VIDIOC_STREAMON failed for " + camera_.device + ": " + std::strerror(errno));
    }
    streaming_ = true;
  } catch (...) {
    stop_v4l2();
    throw;
  }
}

void CameraFrameSource::stop_v4l2() {
  if (device_fd_ >= 0 && streaming_) {
    auto type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ioctl_retry(device_fd_, VIDIOC_STREAMOFF, &type);
  }
  streaming_ = false;
  for (const auto& buffer : mapped_buffers_) {
    if (buffer.address != nullptr && buffer.address != MAP_FAILED) ::munmap(buffer.address, buffer.length);
  }
  mapped_buffers_.clear();
  v4l2_bytes_per_line_ = 0;
  v4l2_size_image_ = 0;
  last_v4l2_bytes_used_ = 0;
  v4l2_timeperframe_numerator_ = 0;
  v4l2_timeperframe_denominator_ = 0;
  v4l2_timeperframe_supported_ = false;
  v4l2_timeperframe_readback_ = false;
  v4l2_timeperframe_set_accepted_ = false;
  v4l2_set_timeperframe_numerator_ = 0;
  v4l2_set_timeperframe_denominator_ = 0;
  last_capture_used_device_timestamp_ = false;
  last_delivered_v4l2_sequence_.reset();
  last_dequeued_v4l2_sequence_.reset();
  last_dequeued_v4l2_sequence_gap_ = 0;
  last_v4l2_buffer_valid_ = false;
  if (device_fd_ >= 0) {
    ::close(device_fd_);
    device_fd_ = -1;
  }
}

std::string CameraFrameSource::read_v4l2_uyvy() {
  start_v4l2();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(frame_timeout_ms_);
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) throw std::runtime_error("timed out waiting for V4L2 frame: " + camera_.id);
    pollfd descriptor{device_fd_, POLLIN | POLLERR, 0};
    int polled = 0;
    do {
      polled = ::poll(&descriptor, 1, static_cast<int>(remaining));
    } while (polled < 0 && errno == EINTR);
    last_v4l2_poll_revents_ = descriptor.revents;
    if (polled == 0) throw std::runtime_error("timed out waiting for V4L2 frame: " + camera_.id);
    if (polled < 0) throw std::runtime_error("V4L2 poll failed for " + camera_.id + ": " + std::strerror(errno));

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(device_fd_, VIDIOC_DQBUF, &buffer) < 0) {
      if (errno == EAGAIN) { ++v4l2_dequeue_eagain_count_; continue; }
      throw std::runtime_error("VIDIOC_DQBUF failed for " + camera_.device + ": " + std::strerror(errno));
    }
    if (buffer.index >= mapped_buffers_.size()) {
      throw std::runtime_error("V4L2 returned an invalid capture buffer for " + camera_.device);
    }
    // Some drivers clear or rewrite buffer.sequence during VIDIOC_QBUF. Keep
    // the DQBUF value before returning the buffer so gap diagnostics describe
    // the frame that was actually delivered.
    const auto dequeued_sequence = buffer.sequence;
    last_v4l2_buffer_valid_ = true;
    last_v4l2_buffer_flags_ = buffer.flags;
    last_v4l2_buffer_index_ = buffer.index;
    last_v4l2_timestamp_us_ = detail::checked_device_timestamp_us(
        buffer.timestamp.tv_sec, buffer.timestamp.tv_usec).value_or(0);
    last_v4l2_bytes_used_ = buffer.bytesused;
    last_dequeued_v4l2_sequence_ = dequeued_sequence;
    last_dequeued_v4l2_sequence_gap_ =
        v4l2_sequence_gap(last_delivered_v4l2_sequence_, dequeued_sequence);
    if ((buffer.flags & V4L2_BUF_FLAG_ERROR) != 0) {
      if (ioctl_retry(device_fd_, VIDIOC_QBUF, &buffer) < 0) {
        throw std::runtime_error("VIDIOC_QBUF failed for " + camera_.device + ": " + std::strerror(errno));
      }
      throw std::runtime_error(
          std::string(mode_ == Mode::Ccg2 ? "CCG2" : "V4L2 UYVY") + " V4L2 buffer flagged error for " + camera_.device +
          ": sequence=" + std::to_string(dequeued_sequence) +
          ", gap_from_last_delivered=" + std::to_string(last_dequeued_v4l2_sequence_gap_));
    }
    if (buffer.bytesused > mapped_buffers_[buffer.index].length) {
      throw std::runtime_error("V4L2 returned an invalid capture buffer for " + camera_.device);
    }
    std::string frame;
    std::exception_ptr packing_error;
    try {
      frame = pack_uyvy_rows(
          std::string_view(
              static_cast<const char*>(mapped_buffers_[buffer.index].address),
              static_cast<std::size_t>(buffer.bytesused)),
          output_width_,
          output_height_,
          v4l2_bytes_per_line_);
    } catch (const std::exception& error) {
      // Retain existing CCG2 diagnostic codes while sharing the raw layout checker.
      packing_error = mode_ == Mode::Ccg2
          ? std::make_exception_ptr(std::runtime_error(std::string("CCG2 ") + error.what()))
          : std::current_exception();
    } catch (...) {
      packing_error = std::current_exception();
    }
    if (ioctl_retry(device_fd_, VIDIOC_QBUF, &buffer) < 0) {
      throw std::runtime_error("VIDIOC_QBUF failed for " + camera_.device + ": " + std::strerror(errno));
    }
    if (packing_error) std::rethrow_exception(packing_error);
    last_delivered_v4l2_sequence_ = dequeued_sequence;
    return frame;
  }
}

std::string CameraFrameSource::read_v4l2_jpeg() {
  start_v4l2();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(frame_timeout_ms_);
  while (true) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now())
                               .count();
    if (remaining <= 0) throw std::runtime_error("timed out waiting for V4L2 frame: " + camera_.id);
    pollfd descriptor{device_fd_, POLLIN | POLLERR, 0};
    int polled = 0;
    do {
      polled = ::poll(&descriptor, 1, static_cast<int>(remaining));
    } while (polled < 0 && errno == EINTR);
    if (polled == 0) throw std::runtime_error("timed out waiting for V4L2 frame: " + camera_.id);
    if (polled < 0) throw std::runtime_error("V4L2 poll failed for " + camera_.id + ": " + std::strerror(errno));

    v4l2_buffer buffer{};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    if (ioctl_retry(device_fd_, VIDIOC_DQBUF, &buffer) < 0) {
      if (errno == EAGAIN) { ++v4l2_dequeue_eagain_count_; continue; }
      throw std::runtime_error("VIDIOC_DQBUF failed for " + camera_.device + ": " + std::strerror(errno));
    }
    if (buffer.index >= mapped_buffers_.size() || buffer.bytesused > mapped_buffers_[buffer.index].length) {
      throw std::runtime_error("V4L2 returned an invalid capture buffer for " + camera_.device);
    }
    const auto dequeued_sequence = buffer.sequence;
    last_v4l2_buffer_valid_ = true;
    last_v4l2_buffer_flags_ = buffer.flags;
    last_v4l2_buffer_index_ = buffer.index;
    last_v4l2_timestamp_us_ = detail::checked_device_timestamp_us(
        buffer.timestamp.tv_sec, buffer.timestamp.tv_usec).value_or(0);
    last_v4l2_bytes_used_ = buffer.bytesused;
    last_dequeued_v4l2_sequence_ = dequeued_sequence;
    last_dequeued_v4l2_sequence_gap_ = v4l2_sequence_gap(last_delivered_v4l2_sequence_, dequeued_sequence);
    std::string frame(
        static_cast<const char*>(mapped_buffers_[buffer.index].address),
        static_cast<std::size_t>(buffer.bytesused));
    if (ioctl_retry(device_fd_, VIDIOC_QBUF, &buffer) < 0) {
      throw std::runtime_error("VIDIOC_QBUF failed for " + camera_.device + ": " + std::strerror(errno));
    }
    last_delivered_v4l2_sequence_ = dequeued_sequence;
    return frame;
  }
}

std::string CameraFrameSource::generate_test_jpeg(std::uint64_t sequence) const {
  const int width = profile_.width;
  const int height = profile_.height;
  std::vector<unsigned char> rgb(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const auto offset = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                           static_cast<std::size_t>(x)) *
                          3U;
      rgb[offset] = static_cast<unsigned char>((x + sequence * 7U) % 256U);
      rgb[offset + 1] = static_cast<unsigned char>((y + sequence * 13U) % 256U);
      rgb[offset + 2] = static_cast<unsigned char>(((x / 32 + y / 32) % 2 == 0) ? 224 : 32);
    }
  }
  return encode_rgb_jpeg(rgb, width, height, 80);
}

EncodedFrame CameraFrameSource::next(std::uint64_t sequence) {
  const auto read_started = std::chrono::steady_clock::now();
  const auto captured = now_ms();
  auto captured_steady_at = read_started;
  last_capture_used_device_timestamp_ = false;
  std::string payload;
  std::string codec{"mjpeg"};
  try {
    switch (mode_) {
      case Mode::TestSource:
        payload = generate_test_jpeg(sequence);
        break;
      case Mode::V4l2:
        payload = read_v4l2_jpeg();
        break;
      case Mode::Ccg2:
      case Mode::V4l2Uyvy:
        payload = read_v4l2_uyvy();
        codec = "uyvy";
        break;
      case Mode::VendorBridge:
        payload = read_vendor_jpeg();
        break;
    }
    if (codec == "mjpeg") require_jpeg(payload, camera_.id);
  } catch (...) {
    last_read_duration_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - read_started).count();
    throw;
  }
  const auto read_finished = std::chrono::steady_clock::now();
  last_read_duration_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
      read_finished - read_started).count();
  const auto device_monotonic_us = (mode_ == Mode::V4l2 || mode_ == Mode::Ccg2 || mode_ == Mode::V4l2Uyvy) &&
      (last_v4l2_buffer_flags_ & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
          ? std::optional<std::int64_t>(last_v4l2_timestamp_us_) : std::nullopt;
  const auto capture_time = detail::select_capture_steady_time(read_started, read_finished, device_monotonic_us);
  captured_steady_at = capture_time.value;
  last_capture_used_device_timestamp_ = capture_time.used_device_timestamp;
  return {
      camera_.id,
      sequence,
      std::move(codec),
      std::move(payload),
      captured,
      now_ms(),
      output_width_,
      output_height_,
      profile_.fps,
      profile_.bitrate_kbps,
      v4l2_bytes_per_line_,
      v4l2_size_image_,
      last_v4l2_bytes_used_,
      last_dequeued_v4l2_sequence_.has_value(),
      last_dequeued_v4l2_sequence_.value_or(0),
      last_dequeued_v4l2_sequence_gap_,
      v4l2_timeperframe_numerator_,
      v4l2_timeperframe_denominator_,
      captured_steady_at,
  };
}

Json CameraFrameSource::capture_diagnostics() const {
  return {{"read_duration_ms", last_read_duration_ms_},
          {"requested_capture_fps", profile_.fps},
          {"driver_timeperframe_supported", v4l2_timeperframe_supported_},
          {"driver_timeperframe_readback", v4l2_timeperframe_readback_},
          {"driver_rate_request_accepted", v4l2_timeperframe_set_accepted_},
          {"driver_set_timeperframe_numerator", v4l2_set_timeperframe_numerator_},
          {"driver_set_timeperframe_denominator", v4l2_set_timeperframe_denominator_},
          {"driver_nominal_fps_matches_request", v4l2_timeperframe_numerator_ > 0
              ? Json(static_cast<std::uint64_t>(v4l2_timeperframe_denominator_) ==
                  static_cast<std::uint64_t>(profile_.fps) * v4l2_timeperframe_numerator_)
              : Json(nullptr)},
          {"capture_timestamp_scope", last_capture_used_device_timestamp_
              ? "driver_monotonic" : "host_acquisition_start_not_sensor_verification"},
          {"driver_timeperframe_numerator", v4l2_timeperframe_numerator_},
          {"driver_timeperframe_denominator", v4l2_timeperframe_denominator_},
          {"driver_negotiated_fps", v4l2_timeperframe_numerator_ > 0
              ? Json(static_cast<double>(v4l2_timeperframe_denominator_) / v4l2_timeperframe_numerator_) : Json(nullptr)},
          {"frame_rate_scope", "driver_delivery_not_sensor_verification"},
          {"buffer_metadata_scope", "last_dequeued_buffer"},
          {"driver", v4l2_driver_}, {"card", v4l2_card_}, {"bus_info", v4l2_bus_info_},
          {"buffer_valid", last_v4l2_buffer_valid_},
          {"buffer_flags", last_v4l2_buffer_flags_},
          {"buffer_index", last_v4l2_buffer_index_},
          {"device_timestamp_us", last_v4l2_timestamp_us_},
          {"poll_revents", last_v4l2_poll_revents_},
          {"dequeue_eagain_count", v4l2_dequeue_eagain_count_},
          {"bytes_used", last_v4l2_bytes_used_},
          {"bytes_per_line", v4l2_bytes_per_line_}, {"size_image", v4l2_size_image_}};
}

}  // namespace mine_teleop
