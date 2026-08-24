#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"

namespace mine_teleop {

enum class CameraSourceKind { TestSource, Mvs, Aravis, V4l2, Ccg2 };

[[nodiscard]] CameraSourceKind classify_camera_source(std::string_view device);
[[nodiscard]] CameraSourceKind classify_camera_source(const CameraConfig& camera);
[[nodiscard]] std::string_view camera_source_kind_name(CameraSourceKind kind);

inline constexpr std::uint64_t kCameraAppSrcMaxBuffers = 2;

struct CameraInputSpec {
  std::string codec{"mjpeg"};
  int width{0};
  int height{0};
  int fps{0};
};

// Existing sources retain their realtime-profile capture behavior.  CCG2 is
// explicit because its V4L2 driver reports YUYV while the DMA bytes are UYVY.
[[nodiscard]] CameraInputSpec camera_input_spec(
    const CameraConfig& camera,
    const MediaProfile& realtime_profile);
[[nodiscard]] std::string pack_uyvy_rows(
    std::string_view frame,
    int width,
    int height,
    std::size_t bytes_per_line);
[[nodiscard]] std::uint64_t v4l2_sequence_gap(
    std::optional<std::uint32_t> previous_sequence,
    std::uint32_t current_sequence);
[[nodiscard]] std::string build_camera_input_pipeline(
    std::string_view source_name,
    const CameraInputSpec& input,
    const MediaProfile& output_profile);

struct CameraIssue {
  std::string_view code;
  std::string_view stage;
  std::string_view action;
  bool retryable;
};

[[nodiscard]] CameraIssue classify_camera_issue(std::string_view error);

enum class CameraFailureAction { ReopenLane, DisableLane };

struct CameraFailureDecision {
  bool inhibit_control{false};
  CameraFailureAction lane_action{CameraFailureAction::DisableLane};
};

[[nodiscard]] CameraFailureDecision camera_failure_decision(
    const CameraConfig& camera,
    int failures_in_media_attempt,
    bool retryable);

enum class MediaSignalingErrorKind {
  SessionEnded,
  ConnectionRefresh,
  ConnectionStale,
  SequenceConflict,
  ServiceUnavailable,
  Fatal,
};

[[nodiscard]] MediaSignalingErrorKind classify_media_signaling_error(
    const HttpStatusError& error);

class MediaSignalingSequence {
 public:
  [[nodiscard]] std::uint64_t next(
      std::uint64_t connection_generation,
      std::string_view session_id);
  [[nodiscard]] std::uint64_t current() const;

 private:
  mutable std::mutex mutex_;
  std::uint64_t connection_generation_{0};
  std::string session_id_;
  std::uint64_t value_{0};
};

// A critical-camera fault revokes control for the remainder of the current
// cloud session.  The media service may reconstruct VehicleMediaRuntime while
// that session is still active, so this latch must outlive one runtime object.
class CriticalCameraControlLatch {
 public:
  // Selects the active non-empty session.  Entering the same session preserves
  // its latch; entering a different session starts with control uninhibited.
  [[nodiscard]] bool enter_session(std::string_view session_id);

  // Returns true only for the first inhibition in the selected session.
  // Callers must enter the session before reporting a fault.
  [[nodiscard]] bool inhibit(std::string_view session_id);
  [[nodiscard]] bool inhibited_for(std::string_view session_id) const;

 private:
  mutable std::mutex mutex_;
  std::string session_id_;
  bool inhibited_{false};
};

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
  std::size_t source_bytes_per_line{0};
  std::size_t source_size_image{0};
  std::size_t source_bytes_used{0};
  bool source_sequence_valid{false};
  std::uint64_t source_sequence{0};
  std::uint64_t source_sequence_gap{0};
  std::uint32_t source_timeperframe_numerator{0};
  std::uint32_t source_timeperframe_denominator{0};

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
  [[nodiscard]] std::optional<std::uint32_t> last_v4l2_sequence() const {
    return last_dequeued_v4l2_sequence_;
  }
  [[nodiscard]] std::uint64_t last_v4l2_sequence_gap() const {
    return last_dequeued_v4l2_sequence_gap_;
  }

 private:
  enum class Mode { TestSource, V4l2, Ccg2, VendorBridge };
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
  [[nodiscard]] std::string read_v4l2_uyvy();
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
  std::size_t v4l2_bytes_per_line_{0};
  std::size_t v4l2_size_image_{0};
  std::size_t last_v4l2_bytes_used_{0};
  std::uint32_t v4l2_timeperframe_numerator_{0};
  std::uint32_t v4l2_timeperframe_denominator_{0};
  std::optional<std::uint32_t> last_delivered_v4l2_sequence_;
  std::optional<std::uint32_t> last_dequeued_v4l2_sequence_;
  std::uint64_t last_dequeued_v4l2_sequence_gap_{0};
  std::vector<MappedBuffer> mapped_buffers_;
};

class VehicleMediaRuntime {
 public:
  VehicleMediaRuntime(
      VehicleConfig config,
      std::string signaling_url,
      std::string device_token,
      int frame_timeout_ms = 3000,
      std::filesystem::path recording_root = {},
      std::optional<std::string> forced_codec = std::nullopt,
      int simulate_primary_failure_after_frames = 0,
      std::string connection_id = {},
      std::shared_ptr<MediaSignalingSequence> signaling_sequence = {},
      std::shared_ptr<CriticalCameraControlLatch> critical_camera_control_latch = {});
  ~VehicleMediaRuntime();

  VehicleMediaRuntime(const VehicleMediaRuntime&) = delete;
  VehicleMediaRuntime& operator=(const VehicleMediaRuntime&) = delete;

  [[nodiscard]] Json run(int frame_count, int duration_ms = -1, int capture_interval_ms = 0);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mine_teleop
