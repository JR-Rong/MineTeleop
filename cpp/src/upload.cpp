#include "mine_teleop/upload.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace mine_teleop {
namespace {

Json read_json_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read JSON file: " + path.string());
  try {
    Json value;
    input >> value;
    if (!value.is_object()) throw std::runtime_error("segment metadata must be a JSON object");
    return value;
  } catch (const Json::exception& error) {
    throw std::runtime_error("invalid segment metadata " + path.string() + ": " + error.what());
  }
}

void write_json_atomic(const std::filesystem::path& path, const Json& value) {
  const auto temporary = path.string() + ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("cannot write temporary metadata: " + temporary);
    output << std::setw(2) << value << '\n';
    output.flush();
    if (!output) throw std::runtime_error("cannot flush temporary metadata: " + temporary);
  }
  std::filesystem::rename(temporary, path);
}

void copy_verified_atomic(const std::filesystem::path& source, const std::filesystem::path& destination) {
  std::filesystem::create_directories(destination.parent_path());
  const auto temporary = destination.string() + ".tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
  std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing);
  if (sha256_file(source) != sha256_file(temporary)) {
    std::filesystem::remove(temporary, ignored);
    throw std::runtime_error("archive checksum mismatch: " + source.string());
  }
  if (std::filesystem::exists(destination)) std::filesystem::remove(destination);
  std::filesystem::rename(temporary, destination);
}

std::filesystem::path safe_relative(const std::filesystem::path& root, const std::filesystem::path& path) {
  const auto relative = std::filesystem::relative(path, root);
  if (relative.empty() || relative.is_absolute()) throw std::runtime_error("upload source is outside recording root");
  for (const auto& part : relative) {
    if (part == "..") throw std::runtime_error("upload source is outside recording root");
  }
  return relative;
}

}  // namespace

Json UploadProcessResult::to_json() const {
  Json value = {
      {"event", "vehicle_uploader_process_once"},
      {"runtime", "cpp"},
      {"passed", action != "failed"},
      {"action", action},
      {"segment_id", segment_id},
      {"bytes_uploaded", bytes_uploaded},
  };
  if (!object_path.empty()) value["object_path"] = object_path;
  if (!metadata_object_path.empty()) value["metadata_object_path"] = metadata_object_path;
  if (!error.empty()) value["error"] = error;
  return value;
}

std::string sha256_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot hash file: " + path.string());
  EVP_MD_CTX* context = EVP_MD_CTX_new();
  if (context == nullptr) throw std::runtime_error("EVP_MD_CTX_new failed");
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  try {
    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) throw std::runtime_error("SHA-256 init failed");
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
      input.read(buffer.data(), buffer.size());
      const auto count = input.gcount();
      if (count > 0 && EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(count)) != 1) {
        throw std::runtime_error("SHA-256 update failed");
      }
    }
    if (!input.eof()) throw std::runtime_error("cannot read file while hashing: " + path.string());
    if (EVP_DigestFinal_ex(context, digest.data(), &digest_size) != 1) throw std::runtime_error("SHA-256 final failed");
  } catch (...) {
    EVP_MD_CTX_free(context);
    throw;
  }
  EVP_MD_CTX_free(context);
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < digest_size; ++index) output << std::setw(2) << static_cast<int>(digest[index]);
  return output.str();
}

LocalArchiveUploader::LocalArchiveUploader(
    std::filesystem::path recording_root, std::filesystem::path archive_root, double max_bandwidth_mbps)
    : recording_root_(std::move(recording_root)),
      archive_root_(std::move(archive_root)),
      max_bandwidth_mbps_(max_bandwidth_mbps) {
  if (recording_root_.empty() || archive_root_.empty()) throw std::invalid_argument("recording and archive roots are required");
  if (max_bandwidth_mbps_ < 0.0) throw std::invalid_argument("upload bandwidth limit must be non-negative");
}

UploadProcessResult LocalArchiveUploader::process_once() {
  if (!std::filesystem::exists(recording_root_)) return {};
  std::vector<std::filesystem::path> metadata_files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(recording_root_)) {
    if (entry.is_regular_file() && entry.path().extension() == ".json" && !entry.path().filename().string().ends_with(".tmp")) {
      metadata_files.push_back(entry.path());
    }
  }
  std::sort(metadata_files.begin(), metadata_files.end());
  for (const auto& metadata_path : metadata_files) {
    try {
      const auto metadata = read_json_file(metadata_path);
      if (metadata.value("upload_state", "pending") != "pending") continue;
      return upload_metadata(metadata_path);
    } catch (const std::exception& error) {
      return UploadProcessResult{"failed", metadata_path.stem().string(), "", "", error.what(), 0};
    }
  }
  return {};
}

UploadProcessResult LocalArchiveUploader::upload_metadata(const std::filesystem::path& metadata_path) {
  auto metadata = read_json_file(metadata_path);
  const auto segment_id = metadata.value("segment_id", metadata_path.stem().string());
  if (segment_id.empty()) throw std::runtime_error("segment_id is missing from metadata");
  std::filesystem::path video_path;
  if (metadata.contains("video_file") && metadata["video_file"].is_string()) {
    video_path = metadata_path.parent_path() / metadata["video_file"].get<std::string>();
  } else {
    video_path = metadata_path;
    video_path.replace_extension(".mp4");
  }
  if (!std::filesystem::is_regular_file(video_path)) throw std::runtime_error("segment video file is missing: " + video_path.string());
  const auto video_relative = safe_relative(recording_root_, video_path);
  const auto metadata_relative = safe_relative(recording_root_, metadata_path);
  const auto video_destination = archive_root_ / video_relative;
  const auto metadata_destination = archive_root_ / metadata_relative;
  const auto bytes = std::filesystem::file_size(video_path) + std::filesystem::file_size(metadata_path);
  const auto started = std::chrono::steady_clock::now();
  copy_verified_atomic(video_path, video_destination);

  auto archived_metadata = metadata;
  archived_metadata["upload_state"] = "uploaded";
  std::filesystem::create_directories(metadata_destination.parent_path());
  write_json_atomic(metadata_destination, archived_metadata);
  if (max_bandwidth_mbps_ > 0.0) {
    const auto required_seconds = static_cast<double>(bytes) * 8.0 / (max_bandwidth_mbps_ * 1'000'000.0);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    if (required_seconds > elapsed) std::this_thread::sleep_for(std::chrono::duration<double>(required_seconds - elapsed));
  }
  metadata["upload_state"] = "uploaded";
  write_json_atomic(metadata_path, metadata);
  return {
      "uploaded",
      segment_id,
      video_relative.generic_string(),
      metadata_relative.generic_string(),
      "",
      bytes,
  };
}

Json LocalArchiveUploader::backlog() const {
  std::uint64_t pending_segments = 0;
  std::uint64_t pending_bytes = 0;
  if (std::filesystem::exists(recording_root_)) {
    for (const auto& entry : std::filesystem::recursive_directory_iterator(recording_root_)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json") continue;
      try {
        const auto metadata = read_json_file(entry.path());
        if (metadata.value("upload_state", "pending") != "pending") continue;
        ++pending_segments;
        auto video = entry.path();
        video.replace_extension(".mp4");
        pending_bytes += std::filesystem::file_size(entry.path());
        if (std::filesystem::is_regular_file(video)) pending_bytes += std::filesystem::file_size(video);
      } catch (const std::exception&) {
      }
    }
  }
  return {{"pending_segments", pending_segments}, {"pending_bytes", pending_bytes}};
}

}  // namespace mine_teleop
