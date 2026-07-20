#include "mine_teleop/media.hpp"

#include "mine_teleop/server.hpp"
#include "mine_teleop/upload.hpp"

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
#include <ctime>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace mine_teleop {
namespace {

bool is_mvs(std::string_view device) {
  return device == "mvs" || device.starts_with("mvs:") || device.starts_with("hikrobot:");
}

bool is_pylon(std::string_view device) {
  return device == "pylon" || device.starts_with("pylon:") || device.starts_with("basler:");
}

std::string environment_or(std::string_view name, std::string fallback) {
  const char* value = std::getenv(std::string(name).c_str());
  return value == nullptr || *value == '\0' ? std::move(fallback) : std::string(value);
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
  if (is_mvs(camera.device)) {
    std::vector<std::string> command{
        environment_or("MINE_TELEOP_MVS_BRIDGE_BIN", "/opt/mine-teleop/bin/mine-teleop-mvs-camera")};
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
  if (is_pylon(camera.device)) {
    std::vector<std::string> command{
        environment_or("MINE_TELEOP_PYLON_BRIDGE_BIN", "/opt/mine-teleop/bin/mine-teleop-pylon-camera")};
    append_camera_selector(command, camera.device);
    command.insert(command.end(), {
                                      "--width", std::to_string(profile.width),
                                      "--height", std::to_string(profile.height),
                                      "--fps", std::to_string(profile.fps),
                                      "--frames", "0",
                                  });
    return command;
  }

  throw std::invalid_argument("camera is not a vendor SDK source: " + camera.device);
}

std::string normalize_http_origin(std::string value) {
  while (!value.empty() && value.back() == '/') value.pop_back();
  if (!value.starts_with("http://") && !value.starts_with("https://")) {
    throw std::invalid_argument("driver console URL must use http or https");
  }
  return value;
}

std::string iso_time(std::int64_t timestamp_ms) {
  const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
  std::tm value{};
  gmtime_r(&seconds, &value);
  std::ostringstream output;
  output << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
         << (timestamp_ms % 1000) << 'Z';
  return output.str();
}

void write_fd_all(int descriptor, std::string_view value) {
  std::size_t offset = 0;
  while (offset < value.size()) {
    const auto written = ::write(descriptor, value.data() + offset, value.size() - offset);
    if (written < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("recorder pipe write failed: ") + std::strerror(errno));
    }
    if (written == 0) throw std::runtime_error("recorder pipe closed");
    offset += static_cast<std::size_t>(written);
  }
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

class FrameSegmentRecorder {
 public:
  FrameSegmentRecorder(
      std::string vehicle_id,
      CameraConfig camera,
      MediaProfile profile,
      std::filesystem::path root,
      std::string ffmpeg_binary)
      : vehicle_id_(std::move(vehicle_id)),
        camera_(std::move(camera)),
        profile_(std::move(profile)),
        root_(std::move(root)),
        ffmpeg_binary_(std::move(ffmpeg_binary)) {}

  ~FrameSegmentRecorder() {
    try {
      finish();
    } catch (const std::exception& error) {
      std::cerr << Json({{"event", "segment_recorder_finalize_failed"}, {"camera_id", camera_.id}, {"error", error.what()}}).dump() << '\n';
    }
  }

  void write_frame(const EncodedFrame& frame) {
    if (frame.codec != "mjpeg" && frame.codec != "jpeg") throw std::runtime_error("segment recorder expects JPEG frames");
    if (child_pid_ > 0 && frame.captured_at_ms - started_at_ms_ >= profile_.segment_seconds * 1000LL) finish();
    if (child_pid_ <= 0) start(frame.captured_at_ms);
    write_fd_all(stdin_fd_, frame.payload);
    ended_at_ms_ = frame.encoded_at_ms;
    ++frames_;
  }

 private:
  void start(std::int64_t timestamp_ms) {
    const auto directory = root_ / vehicle_id_ / "teleop-session" / camera_.id;
    std::filesystem::create_directories(directory);
    ++segment_sequence_;
    segment_id_ = std::to_string(timestamp_ms) + "_" + camera_.id + "_" + std::to_string(segment_sequence_);
    final_path_ = directory / (segment_id_ + ".mp4");
    temporary_path_ = directory / (segment_id_ + ".mp4.tmp");
    int input_pipe[2]{};
    if (::pipe(input_pipe) != 0) throw std::runtime_error(std::string("cannot create recorder pipe: ") + std::strerror(errno));
    const pid_t child = ::fork();
    if (child < 0) {
      ::close(input_pipe[0]);
      ::close(input_pipe[1]);
      throw std::runtime_error(std::string("cannot fork recorder: ") + std::strerror(errno));
    }
    if (child == 0) {
      ::close(input_pipe[1]);
      if (::dup2(input_pipe[0], STDIN_FILENO) < 0) _exit(126);
      ::close(input_pipe[0]);
      const int width = profile_.width > 0 ? profile_.width : camera_.capture_width;
      const int height = profile_.height > 0 ? profile_.height : camera_.capture_height;
      const int fps = profile_.fps > 0 ? profile_.fps : camera_.capture_fps;
      std::vector<std::string> command{
          ffmpeg_binary_, "-hide_banner", "-loglevel", "error", "-f", "mjpeg", "-framerate", std::to_string(fps),
          "-i", "pipe:0", "-an", "-vf", "scale=" + std::to_string(width) + ":" + std::to_string(height),
          "-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency", "-pix_fmt", "yuv420p",
          "-b:v", std::to_string(profile_.bitrate_kbps) + "k", "-movflags", "+faststart", "-f", "mp4", temporary_path_.string()};
      std::vector<char*> arguments;
      for (auto& item : command) arguments.push_back(item.data());
      arguments.push_back(nullptr);
      ::execvp(arguments.front(), arguments.data());
      _exit(errno == ENOENT ? 127 : 126);
    }
    ::close(input_pipe[0]);
    stdin_fd_ = input_pipe[1];
    child_pid_ = child;
    started_at_ms_ = timestamp_ms;
    ended_at_ms_ = timestamp_ms;
    frames_ = 0;
    ::signal(SIGPIPE, SIG_IGN);
  }

  void finish() {
    if (child_pid_ <= 0) return;
    if (stdin_fd_ >= 0) {
      ::close(stdin_fd_);
      stdin_fd_ = -1;
    }
    int status = 0;
    while (::waitpid(child_pid_, &status, 0) < 0) {
      if (errno == EINTR) continue;
      throw std::runtime_error(std::string("recorder wait failed: ") + std::strerror(errno));
    }
    child_pid_ = -1;
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
      std::error_code ignored;
      std::filesystem::remove(temporary_path_, ignored);
      throw std::runtime_error("FFmpeg segment recorder failed for camera " + camera_.id);
    }
    if (frames_ == 0 || !std::filesystem::is_regular_file(temporary_path_)) return;
    std::filesystem::rename(temporary_path_, final_path_);
    const auto metadata_path = final_path_.parent_path() / (segment_id_ + ".json");
    const auto temporary_metadata = metadata_path.string() + ".tmp";
    const int width = profile_.width > 0 ? profile_.width : camera_.capture_width;
    const int height = profile_.height > 0 ? profile_.height : camera_.capture_height;
    const int fps = profile_.fps > 0 ? profile_.fps : camera_.capture_fps;
    const Json metadata = {
        {"vehicle_id", vehicle_id_},
        {"session_id", "teleop-session"},
        {"camera_id", camera_.id},
        {"segment_id", segment_id_},
        {"started_at", iso_time(started_at_ms_)},
        {"ended_at", iso_time(ended_at_ms_)},
        {"codec", "h264"},
        {"encoder", "ffmpeg-libx264"},
        {"width", width},
        {"height", height},
        {"fps", fps},
        {"upload_state", "pending"},
        {"video_file", final_path_.filename().string()},
        {"file_size_bytes", std::filesystem::file_size(final_path_)},
        {"video_sha256", sha256_file(final_path_)},
    };
    {
      std::ofstream output(temporary_metadata, std::ios::trunc);
      if (!output) throw std::runtime_error("cannot write segment metadata");
      output << std::setw(2) << metadata << '\n';
    }
    std::filesystem::rename(temporary_metadata, metadata_path);
  }

  std::string vehicle_id_;
  CameraConfig camera_;
  MediaProfile profile_;
  std::filesystem::path root_;
  std::string ffmpeg_binary_;
  int stdin_fd_{-1};
  int child_pid_{-1};
  std::int64_t started_at_ms_{0};
  std::int64_t ended_at_ms_{0};
  std::uint64_t frames_{0};
  std::uint64_t segment_sequence_{0};
  std::string segment_id_;
  std::filesystem::path temporary_path_;
  std::filesystem::path final_path_;
};

}  // namespace

Json EncodedFrame::post_payload(std::int64_t sent_at_ms) const {
  return {
      {"camera_id", camera_id},
      {"codec", codec},
      {"payload_base64", base64_encode(payload)},
      {"captured_at_ms", captured_at_ms},
      {"encoded_at_ms", encoded_at_ms},
      {"sent_at_ms", sent_at_ms},
      {"seq", seq},
      {"width", width},
      {"height", height},
      {"fps", fps},
      {"bitrate_kbps", bitrate_kbps},
  };
}

CameraFrameSource::CameraFrameSource(CameraConfig camera, MediaProfile profile, int frame_timeout_ms)
    : camera_(std::move(camera)), profile_(std::move(profile)), frame_timeout_ms_(frame_timeout_ms) {
  if (camera_.id.empty() || profile_.width <= 0 || profile_.height <= 0 || profile_.fps <= 0) {
    throw std::invalid_argument("camera media source configuration is invalid");
  }
  if (frame_timeout_ms_ <= 0) throw std::invalid_argument("frame timeout must be positive");
  if (profile_.codec != "mjpeg" && profile_.codec != "jpeg") {
    throw std::invalid_argument("native camera acquisition requires an mjpeg realtime profile");
  }
  output_width_ = profile_.width;
  output_height_ = profile_.height;
  if (camera_.device == "testsrc") {
    mode_ = Mode::TestSource;
  } else if (is_mvs(camera_.device) || is_pylon(camera_.device)) {
    mode_ = Mode::VendorBridge;
    command_ = build_vendor_bridge_command(camera_, profile_);
  } else {
    mode_ = Mode::V4l2;
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
  device_fd_ = ::open(camera_.device.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
  if (device_fd_ < 0) {
    throw std::runtime_error("cannot open V4L2 camera " + camera_.device + ": " + std::strerror(errno));
  }
  try {
    v4l2_capability capability{};
    if (ioctl_retry(device_fd_, VIDIOC_QUERYCAP, &capability) < 0) {
      throw std::runtime_error("VIDIOC_QUERYCAP failed for " + camera_.device + ": " + std::strerror(errno));
    }
    const auto capabilities = (capability.capabilities & V4L2_CAP_DEVICE_CAPS) != 0
                                  ? capability.device_caps
                                  : capability.capabilities;
    if ((capabilities & V4L2_CAP_VIDEO_CAPTURE) == 0 || (capabilities & V4L2_CAP_STREAMING) == 0) {
      throw std::runtime_error("camera must support V4L2 capture and streaming: " + camera_.device);
    }

    v4l2_format format{};
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = static_cast<__u32>(profile_.width);
    format.fmt.pix.height = static_cast<__u32>(profile_.height);
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    format.fmt.pix.field = V4L2_FIELD_ANY;
    if (ioctl_retry(device_fd_, VIDIOC_S_FMT, &format) < 0) {
      throw std::runtime_error("VIDIOC_S_FMT MJPEG failed for " + camera_.device + ": " + std::strerror(errno));
    }
    if (format.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
      throw std::runtime_error("camera does not provide native MJPEG: " + camera_.device);
    }
    output_width_ = static_cast<int>(format.fmt.pix.width);
    output_height_ = static_cast<int>(format.fmt.pix.height);

    v4l2_streamparm parameters{};
    parameters.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parameters.parm.capture.timeperframe.numerator = 1;
    parameters.parm.capture.timeperframe.denominator = static_cast<__u32>(profile_.fps);
    if (ioctl_retry(device_fd_, VIDIOC_S_PARM, &parameters) < 0 && errno != EINVAL) {
      throw std::runtime_error("VIDIOC_S_PARM failed for " + camera_.device + ": " + std::strerror(errno));
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
  if (device_fd_ >= 0) {
    ::close(device_fd_);
    device_fd_ = -1;
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
      if (errno == EAGAIN) continue;
      throw std::runtime_error("VIDIOC_DQBUF failed for " + camera_.device + ": " + std::strerror(errno));
    }
    if (buffer.index >= mapped_buffers_.size() || buffer.bytesused > mapped_buffers_[buffer.index].length) {
      throw std::runtime_error("V4L2 returned an invalid capture buffer for " + camera_.device);
    }
    std::string frame(
        static_cast<const char*>(mapped_buffers_[buffer.index].address),
        static_cast<std::size_t>(buffer.bytesused));
    if (ioctl_retry(device_fd_, VIDIOC_QBUF, &buffer) < 0) {
      throw std::runtime_error("VIDIOC_QBUF failed for " + camera_.device + ": " + std::strerror(errno));
    }
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
  const auto captured = now_ms();
  std::string payload;
  switch (mode_) {
    case Mode::TestSource:
      payload = generate_test_jpeg(sequence);
      break;
    case Mode::V4l2:
      payload = read_v4l2_jpeg();
      break;
    case Mode::VendorBridge:
      payload = read_vendor_jpeg();
      break;
  }
  require_jpeg(payload, camera_.id);
  return {
      camera_.id,
      sequence,
      "mjpeg",
      std::move(payload),
      captured,
      now_ms(),
      output_width_,
      output_height_,
      profile_.fps,
      profile_.bitrate_kbps,
  };
}

VehicleMediaRuntime::VehicleMediaRuntime(
    VehicleConfig config,
    std::string driver_console_url,
    int frame_timeout_ms,
    std::filesystem::path recording_root)
    : config_(std::move(config)),
      driver_console_url_(normalize_http_origin(std::move(driver_console_url))),
      frame_timeout_ms_(frame_timeout_ms),
      recording_root_(std::move(recording_root)) {
  if (frame_timeout_ms_ <= 0) throw std::invalid_argument("frame timeout must be positive");
}

Json VehicleMediaRuntime::run(int frame_count, int duration_ms, int capture_interval_ms) {
  const bool continuous = frame_count == 0 && duration_ms == 0;
  if (!continuous && frame_count <= 0 && duration_ms < 0) throw std::invalid_argument("frame_count or duration_ms is required");
  if (capture_interval_ms < 0) throw std::invalid_argument("capture interval must be non-negative");
  const auto cameras = config_.enabled_cameras();
  if (cameras.empty()) throw std::runtime_error("at least one enabled camera is required");
  std::vector<std::unique_ptr<CameraFrameSource>> sources;
  std::vector<std::unique_ptr<FrameSegmentRecorder>> recorders;
  for (const auto& camera : cameras) {
    sources.push_back(std::make_unique<CameraFrameSource>(
        camera, config_.realtime_profile(camera.realtime_profile), frame_timeout_ms_));
    if (!recording_root_.empty() && !camera.record_profile.empty()) {
      recorders.push_back(std::make_unique<FrameSegmentRecorder>(
          config_.vehicle_id, camera, config_.record_profile(camera.record_profile), recording_root_, config_.hardware.ffmpeg_binary));
    } else {
      recorders.push_back(nullptr);
    }
  }
  const auto started = now_ms();
  Json errors = Json::array();
  std::uint64_t sent = 0;
  std::uint64_t sequence = 0;
  while ((continuous || frame_count <= 0 || sequence < static_cast<std::uint64_t>(frame_count)) &&
         (continuous || duration_ms < 0 || now_ms() - started < duration_ms)) {
    ++sequence;
    for (std::size_t index = 0; index < sources.size(); ++index) {
      auto& source = sources[index];
      try {
        const auto frame = source->next(sequence);
        if (recorders[index]) recorders[index]->write_frame(frame);
        const auto response = http_.post_json_response(
            driver_console_url_ + "/api/media/frame", frame.post_payload(now_ms()));
        if (!response.value("accepted", false)) errors.push_back({{"camera_id", frame.camera_id}, {"error", "frame rejected"}});
        ++sent;
      } catch (const std::exception& error) {
        errors.push_back({{"camera_id", source->camera_id()}, {"error", error.what()}});
      }
    }
    if (!errors.empty()) break;
    if (capture_interval_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(capture_interval_ms));
  }
  const auto elapsed = std::max<std::int64_t>(1, now_ms() - started);
  return {
      {"event", "vehicle_media_stream_summary"},
      {"runtime", "cpp"},
      {"passed", errors.empty() && sent > 0},
      {"vehicle_id", config_.vehicle_id},
      {"camera_count", cameras.size()},
      {"recording_enabled", !recording_root_.empty()},
      {"recording_root", recording_root_.string()},
      {"sent_frames", sent},
      {"duration_ms", elapsed},
      {"achieved_fps", sent * 1000.0 / static_cast<double>(elapsed)},
      {"errors", std::move(errors)},
  };
}

}  // namespace mine_teleop
