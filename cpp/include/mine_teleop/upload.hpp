#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>

#include "mine_teleop/core.hpp"

namespace mine_teleop {

struct UploadProcessResult {
  std::string action{"idle"};
  std::string segment_id;
  std::string object_path;
  std::string metadata_object_path;
  std::string error;
  std::uint64_t bytes_uploaded{0};
  std::int64_t retry_after_ms{0};
  std::uint64_t deferred_failures{0};

  [[nodiscard]] Json to_json() const;
};

class LocalArchiveUploader {
 public:
  LocalArchiveUploader(
      std::filesystem::path recording_root,
      std::filesystem::path archive_root,
      double max_bandwidth_mbps = 0.0,
      int retry_initial_seconds = 10,
      int retry_max_seconds = 600);

  [[nodiscard]] UploadProcessResult process_once();
  [[nodiscard]] Json backlog() const;

 private:
  [[nodiscard]] UploadProcessResult upload_metadata(const std::filesystem::path& metadata_path);

  std::filesystem::path recording_root_;
  std::filesystem::path archive_root_;
  double max_bandwidth_mbps_;
  int retry_initial_seconds_;
  int retry_max_seconds_;
  struct RetryState {
    std::uint32_t attempts{0};
    std::chrono::steady_clock::time_point next_attempt_at{};
  };
  std::unordered_map<std::string, RetryState> retries_;
};

struct RecordingStorageResult {
  bool recording_allowed{true};
  std::uint64_t available_bytes{0};
  std::uint64_t required_free_bytes{0};
  std::uint64_t removed_uploaded_segments{0};
  std::uint64_t removed_unuploaded_segments{0};
  std::uint64_t removed_bytes{0};

  [[nodiscard]] Json to_json() const;
};

[[nodiscard]] RecordingStorageResult enforce_recording_storage_policy(
    const std::filesystem::path& recording_root,
    const RecordingConfig& config);

std::string sha256_file(const std::filesystem::path& path);

}  // namespace mine_teleop
