#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"

namespace mine_teleop {

enum class CameraSourceKind { TestSource, Mvs, Aravis, V4l2, Ccg2, V4l2Uyvy };

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
// v4l2_uyvy requests real UYVY and shares the bounded raw capture path.
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
[[nodiscard]] std::string build_encoded_frame_queue(
    std::string_view name, int max_buffers);

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

// Recoverable readiness faults retain the session and its independent brake.
// Explicit inhibition remains reserved for non-recoverable safety failures.
inline constexpr int kCriticalCameraStartupTimeoutMs = 15000;
inline constexpr int kControlRecoveryStableMs = 300;
enum class ControlRecoveryPhase { Ready, BrakingWait, Recovered, Locked };
enum class NeutralHeartbeatGate { Accepted, InactiveState, NativeGap };
[[nodiscard]] NeutralHeartbeatGate neutral_heartbeat_gate(
    SafetyState state, bool braking_wait, std::int64_t native_gap_ms, int degraded_timeout_ms);
[[nodiscard]] bool camera_encoded_frame_fresh(
    std::int64_t last_encoded_ms, std::int64_t now_ms, int timeout_ms);

struct ReadinessSampleRevision {
  std::string source;
  std::uint64_t bad{0};
  std::uint64_t brake{0};
};
struct ReadinessRevisionChanges { bool bad{false}; bool brake{false}; };
// Persistent producer revisions survive latest-value coalescing. The current
// bounded source set replaces the previous set; no historical source list grows.
class ReadinessRevisionFence {
 public:
  [[nodiscard]] ReadinessRevisionChanges observe(const std::vector<ReadinessSampleRevision>& sources);
  void reset() { initialized_ = false; sources_.clear(); }
 private:
  bool initialized_{false};
  std::vector<ReadinessSampleRevision> sources_;
};

class SessionControlRecoveryLatch {
 public:
  // Selects the active non-empty session.  Entering the same session preserves
  // its latch; entering a different session starts with control uninhibited.
  [[nodiscard]] bool enter_session(std::string_view session_id);

  // Returns true only for the first inhibition in the selected session.
  // Callers must enter the session before reporting a fault.
  [[nodiscard]] bool inhibit(std::string_view session_id);
  [[nodiscard]] bool inhibited_for(std::string_view session_id) const;
  // Grace is only for admission before the first adapter start. Both its
  // deadline and the armed state survive same-session media reconstruction.
  [[nodiscard]] bool startup_grace_active(std::string_view session_id, std::int64_t now_ms);
  [[nodiscard]] bool arm_for_control(std::string_view session_id);
  [[nodiscard]] ControlRecoveryPhase observe_recovery(
      std::string_view session_id, bool ready, std::int64_t now_ms, int recovery_timeout_ms,
      ControlRecoveryCause cause = ControlRecoveryCause::CriticalVideo);
  [[nodiscard]] ControlRecoveryPhase observe_readiness(
      std::string_view session_id, bool ready, std::int64_t now_ms, int recovery_timeout_ms,
      ControlRecoveryCause cause, const std::vector<ReadinessSampleRevision>& sources);
  [[nodiscard]] bool braking_wait_for(std::string_view session_id) const;
  [[nodiscard]] ControlRecoveryCause cause_for(std::string_view session_id) const;
  [[nodiscard]] std::optional<std::int64_t> deadline_for(std::string_view session_id) const;
  [[nodiscard]] Json recovery_status_for(std::string_view session_id) const;

 private:
  [[nodiscard]] ControlRecoveryPhase observe_recovery_locked(
      bool ready, std::int64_t now_ms, int recovery_timeout_ms, ControlRecoveryCause cause);
  mutable std::mutex mutex_;
  ReadinessRevisionFence revisions_;
  std::string session_id_;
  bool inhibited_{false};
  bool armed_{false};
  std::optional<std::int64_t> startup_started_ms_;
  std::optional<std::int64_t> braking_started_ms_;
  std::optional<std::int64_t> recovered_started_ms_;
  int braking_timeout_ms_{0};
  ControlRecoveryCause cause_{ControlRecoveryCause::CriticalVideo};
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
  // Host monotonic acquisition time; device timestamp is used only when the
  // driver explicitly labels it monotonic. This is independent of wall jumps.
  std::chrono::steady_clock::time_point captured_steady_at{};

};

// Explicit opt-in for the supervised bench utility only. The source borrows
// this descriptor until lazy startup then owns a CLOEXEC duplicate of the same
// open file-description. The supervisor retains the original through restore.
struct BenchPreopenedV4l2Fd {
  int value{-1};
  std::function<void(std::string_view, int, int, const Json&)> observe_ioctl;
};

class CameraFrameSource {
 public:
  CameraFrameSource(CameraConfig camera, MediaProfile profile, int frame_timeout_ms = 3000);
  CameraFrameSource(CameraConfig camera, MediaProfile profile, int frame_timeout_ms, BenchPreopenedV4l2Fd bench);
  ~CameraFrameSource();

  CameraFrameSource(const CameraFrameSource&) = delete;
  CameraFrameSource& operator=(const CameraFrameSource&) = delete;

  [[nodiscard]] EncodedFrame next(std::uint64_t sequence);
  [[nodiscard]] const std::string& camera_id() const { return camera_.id; }
  [[nodiscard]] const std::vector<std::string>& command() const { return command_; }
  // Read on the capture thread, including immediately after a failed read.
  [[nodiscard]] Json capture_diagnostics() const;
  [[nodiscard]] std::optional<std::uint32_t> last_v4l2_sequence() const {
    return last_dequeued_v4l2_sequence_;
  }
  [[nodiscard]] std::uint64_t last_v4l2_sequence_gap() const {
    return last_dequeued_v4l2_sequence_gap_;
  }

 private:
  enum class Mode { TestSource, V4l2, Ccg2, V4l2Uyvy, VendorBridge };
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
  BenchPreopenedV4l2Fd bench_preopened_;
  bool streaming_{false};
  int output_width_{0};
  int output_height_{0};
  std::size_t v4l2_bytes_per_line_{0};
  std::size_t v4l2_size_image_{0};
  std::size_t last_v4l2_bytes_used_{0};
  std::uint32_t v4l2_timeperframe_numerator_{0};
  std::uint32_t v4l2_timeperframe_denominator_{0};
  bool v4l2_timeperframe_supported_{false};
  std::optional<std::uint32_t> last_delivered_v4l2_sequence_;
  std::optional<std::uint32_t> last_dequeued_v4l2_sequence_;
  std::uint64_t last_dequeued_v4l2_sequence_gap_{0};
  std::string v4l2_driver_, v4l2_card_, v4l2_bus_info_;
  bool last_v4l2_buffer_valid_{false};
  std::uint32_t last_v4l2_buffer_flags_{0}, last_v4l2_buffer_index_{0};
  std::int64_t last_v4l2_timestamp_us_{0};
  int last_v4l2_poll_revents_{0};
  std::uint64_t v4l2_dequeue_eagain_count_{0};
  std::int64_t last_read_duration_ms_{0};
  bool last_capture_used_device_timestamp_{false};
  bool v4l2_timeperframe_readback_{false};
  bool v4l2_timeperframe_set_accepted_{false};
  std::uint32_t v4l2_set_timeperframe_numerator_{0};
  std::uint32_t v4l2_set_timeperframe_denominator_{0};
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
      std::shared_ptr<SessionControlRecoveryLatch> control_recovery_latch = {});
  ~VehicleMediaRuntime();

  VehicleMediaRuntime(const VehicleMediaRuntime&) = delete;
  VehicleMediaRuntime& operator=(const VehicleMediaRuntime&) = delete;

  [[nodiscard]] Json run(int frame_count, int duration_ms = -1, int capture_interval_ms = 0,
                         std::stop_token stop_token = {});

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mine_teleop
