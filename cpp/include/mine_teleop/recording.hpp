#pragma once

#include "mine_teleop/core.hpp"
#include "mine_teleop/detail/recording_transport.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

typedef struct _GstBuffer GstBuffer;
typedef struct _GstSegment GstSegment;

namespace mine_teleop {

struct RecordingLaneSettings {
  std::string camera_id, codec, encoder;
  int fps{20}, segment_seconds{2};
};
struct RecordingSessionSettings {
  std::string vehicle_id, session_id, attempt_id;
  std::filesystem::path root;
  std::vector<RecordingLaneSettings> lanes;
  std::filesystem::path worker_executable; // Empty resolves a packaged sibling, never PATH/a shell.
  bool incomplete_before{false};
  bool session_history_known{false}; // Attempt EOS cannot prove pre-process/session recording history.
};

class IsolatedRecordingSession {
 public:
  explicit IsolatedRecordingSession(RecordingSessionSettings settings);
  ~IsolatedRecordingSession();
  IsolatedRecordingSession(const IsolatedRecordingSession&) = delete;
  IsolatedRecordingSession& operator=(const IsolatedRecordingSession&) = delete;
  // Called only by each lane's serialized streaming task. Always nonblocking
  // with respect to IPC/worker/filesystem; copies complete AU into owned memory.
  void admit(std::size_t lane, GstBuffer* buffer, const GstSegment& segment) noexcept;
  [[nodiscard]] bool can_admit(std::size_t lane, std::size_t bytes) const noexcept;
  void admit_bytes(std::size_t lane, detail::RecordingWireHeader header, std::span<const std::uint8_t> bytes) noexcept;
  void reject_admission(std::size_t lane, bool overflow) noexcept;
  void note_mapping_duration(std::size_t lane, std::uint64_t duration_us) noexcept;
  void end_lane(std::size_t lane) noexcept;
  void request_stop() noexcept;
  void finish(); // Bounded recorder-only drain/cleanup, after media producer EOS/NULL.
  void abort(std::string_view issue) noexcept;
  [[nodiscard]] Json lane_status(std::size_t lane) const;
  [[nodiscard]] Json session_status() const;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Internal worker entry point; no vehicle config, network or CAN is loaded.
// Hooks are supplied only by the offline test executable, never the shipped CLI.
struct RecordingWorkerTestHooks {
  int stall_write_ms{0};
  int stall_finalize_ms{0};
  bool disk_full{false};
  std::uint64_t exit_after_access_units{0};
  bool emit_file_diagnostics{false};
};
int run_recording_worker(int input_fd, int status_fd, RecordingWorkerTestHooks hooks = {});

} // namespace mine_teleop
