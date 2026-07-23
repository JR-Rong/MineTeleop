#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "mine_teleop/core.hpp"

namespace mine_teleop {

struct UploadProcessResult {
  std::string action{"idle"};
  std::string segment_id;
  std::string object_path;
  std::string metadata_object_path;
  std::string error;
  std::uint64_t bytes_uploaded{0};

  [[nodiscard]] Json to_json() const;
};

class LocalArchiveUploader {
 public:
  LocalArchiveUploader(
      std::filesystem::path recording_root,
      std::filesystem::path archive_root,
      double max_bandwidth_mbps = 0.0);

  [[nodiscard]] UploadProcessResult process_once();
  [[nodiscard]] Json backlog() const;

 private:
  [[nodiscard]] UploadProcessResult upload_metadata(const std::filesystem::path& metadata_path);

  std::filesystem::path recording_root_;
  std::filesystem::path archive_root_;
  double max_bandwidth_mbps_;
};

std::string sha256_file(const std::filesystem::path& path);

}  // namespace mine_teleop
