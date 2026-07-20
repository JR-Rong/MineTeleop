#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"

namespace mine_teleop {

struct EncodedFrame {
  std::string camera_id;
  std::uint64_t seq{0};
  std::string codec{"mjpeg"};
  std::string payload;
  std::int64_t captured_at_ms{0};
  std::int64_t encoded_at_ms{0};
  int width{0};
  int height{0};
  int fps{0};
  int bitrate_kbps{0};

  [[nodiscard]] Json post_payload(std::int64_t sent_at_ms) const;
};

class CameraFrameSource {
 public:
  CameraFrameSource(CameraConfig camera, MediaProfile profile, int frame_timeout_ms = 3000);
  ~CameraFrameSource();

  CameraFrameSource(const CameraFrameSource&) = delete;
  CameraFrameSource& operator=(const CameraFrameSource&) = delete;

  [[nodiscard]] EncodedFrame next(std::uint64_t sequence);
  [[nodiscard]] const std::string& camera_id() const { return camera_.id; }
  [[nodiscard]] const std::vector<std::string>& command() const { return command_; }

 private:
  enum class Mode { TestSource, V4l2, VendorBridge };
  struct MappedBuffer {
    void* address{nullptr};
    std::size_t length{0};
  };

  void start_vendor_bridge();
  void stop_vendor_bridge();
  [[nodiscard]] std::string read_vendor_jpeg();
  void start_v4l2();
  void stop_v4l2();
  [[nodiscard]] std::string read_v4l2_jpeg();
  [[nodiscard]] std::string generate_test_jpeg(std::uint64_t sequence) const;

  CameraConfig camera_;
  MediaProfile profile_;
  int frame_timeout_ms_;
  Mode mode_{Mode::TestSource};
  std::vector<std::string> command_;
  int stdout_fd_{-1};
  int child_pid_{-1};
  std::string buffer_;
  int device_fd_{-1};
  bool streaming_{false};
  int output_width_{0};
  int output_height_{0};
  std::vector<MappedBuffer> mapped_buffers_;
};

class VehicleMediaRuntime {
 public:
  VehicleMediaRuntime(
      VehicleConfig config,
      std::string driver_console_url,
      int frame_timeout_ms = 3000,
      std::filesystem::path recording_root = {});

  [[nodiscard]] Json run(int frame_count, int duration_ms = -1, int capture_interval_ms = 0);

 private:
  VehicleConfig config_;
  std::string driver_console_url_;
  int frame_timeout_ms_;
  std::filesystem::path recording_root_;
  HttpClient http_{std::chrono::seconds(10)};
};

}  // namespace mine_teleop
