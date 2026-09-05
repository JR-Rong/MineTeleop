#include "mine_teleop/upload.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <vector>

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

void copy_verified_atomic(
    const std::filesystem::path& source,
    const std::filesystem::path& destination,
    std::string_view expected_sha256) {
  std::filesystem::create_directories(destination.parent_path());
  const auto temporary = destination.string() + ".tmp";
  std::error_code ignored;
  std::filesystem::remove(temporary, ignored);
  std::filesystem::copy_file(source, temporary, std::filesystem::copy_options::overwrite_existing);
  if (sha256_file(temporary) != expected_sha256) {
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

std::uint64_t gigabytes_to_bytes(double gigabytes) {
  if (!std::isfinite(gigabytes) || gigabytes < 0.0) {
    throw std::invalid_argument("recording free-space threshold must be finite and non-negative");
  }
  constexpr double kBytesPerGigabyte = 1'000'000'000.0;
  if (gigabytes >= static_cast<double>(std::numeric_limits<std::uint64_t>::max()) / kBytesPerGigabyte) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return static_cast<std::uint64_t>(gigabytes * kBytesPerGigabyte);
}

struct RecordingCleanupCandidate {
  std::filesystem::path metadata_path;
  std::filesystem::path video_path;
  std::filesystem::file_time_type modified_at;
  bool uploaded{false};
};

bool plain_regular_file(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(std::filesystem::symlink_status(path, error)) && !error;
}

std::filesystem::path sidecar_video_path(
    const std::filesystem::path& metadata_path,
    const Json& metadata) {
  if (!metadata.contains("video_file")) {
    auto video_path = metadata_path;
    video_path.replace_extension(".mp4");
    return video_path;
  }
  if (!metadata["video_file"].is_string()) {
    throw std::runtime_error("video_file must be a filename in the sidecar directory");
  }
  const std::filesystem::path filename(metadata["video_file"].get<std::string>());
  if (filename.empty() || filename.is_absolute() || filename.has_parent_path()) {
    throw std::runtime_error("video_file must be a filename in the sidecar directory");
  }
  return metadata_path.parent_path() / filename;
}

std::uint64_t remove_recording_candidate(const RecordingCleanupCandidate& candidate) {
  std::uint64_t removed_bytes = 0;
  std::error_code error;
  if (plain_regular_file(candidate.video_path)) {
    const auto size = std::filesystem::file_size(candidate.video_path, error);
    if (error || !std::filesystem::remove(candidate.video_path, error) || error) return 0;
    removed_bytes += size;
  }
  error.clear();
  if (plain_regular_file(candidate.metadata_path)) {
    const auto size = std::filesystem::file_size(candidate.metadata_path, error);
    if (!error && std::filesystem::remove(candidate.metadata_path, error) && !error) removed_bytes += size;
  }
  return removed_bytes;
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
  if (retry_after_ms > 0) value["retry_after_ms"] = retry_after_ms;
  if (deferred_failures > 0) value["deferred_failures"] = deferred_failures;
  return value;
}

Json RecordingStorageResult::to_json() const {
  return {
      {"recording_allowed", recording_allowed},
      {"available_bytes", available_bytes},
      {"required_free_bytes", required_free_bytes},
      {"removed_uploaded_segments", removed_uploaded_segments},
      {"removed_unuploaded_segments", removed_unuploaded_segments},
      {"removed_bytes", removed_bytes},
  };
}

RecordingStorageResult enforce_recording_storage_policy(
    const std::filesystem::path& recording_root,
    const RecordingConfig& config) {
  if (recording_root.empty()) throw std::invalid_argument("recording root is required");
  std::filesystem::create_directories(recording_root);
  RecordingStorageResult result;
  result.required_free_bytes = gigabytes_to_bytes(config.min_free_gb);
  const auto uploaded_cleanup_threshold = gigabytes_to_bytes(config.delete_uploaded_when_below_free_gb);
  auto space = std::filesystem::space(recording_root);
  result.available_bytes = space.available;
  if (result.available_bytes >= result.required_free_bytes) return result;

  std::vector<RecordingCleanupCandidate> uploaded;
  std::vector<RecordingCleanupCandidate> unuploaded;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           recording_root,
           std::filesystem::directory_options::skip_permission_denied)) {
    if (!plain_regular_file(entry.path()) || entry.path().extension() != ".json" ||
        entry.path().filename().string().ends_with(".tmp")) {
      continue;
    }
    try {
      const auto metadata = read_json_file(entry.path());
      auto video_path = sidecar_video_path(entry.path(), metadata);
      std::error_code modified_error;
      const auto modified_at = std::filesystem::last_write_time(entry.path(), modified_error);
      if (modified_error) continue;
      RecordingCleanupCandidate candidate{
          entry.path(),
          std::move(video_path),
          modified_at,
          metadata.value("upload_state", "pending") == "uploaded"};
      (candidate.uploaded ? uploaded : unuploaded).push_back(std::move(candidate));
    } catch (const std::exception&) {
    }
  }
  const auto oldest_first = [](const auto& left, const auto& right) {
    return left.modified_at < right.modified_at;
  };
  std::sort(uploaded.begin(), uploaded.end(), oldest_first);
  std::sort(unuploaded.begin(), unuploaded.end(), oldest_first);
  const auto remove_until_safe = [&](const auto& candidates, bool uploaded_state) {
    for (const auto& candidate : candidates) {
      if (result.available_bytes >= result.required_free_bytes) break;
      const auto removed = remove_recording_candidate(candidate);
      if (removed == 0) continue;
      result.removed_bytes += removed;
      if (uploaded_state) {
        ++result.removed_uploaded_segments;
      } else {
        ++result.removed_unuploaded_segments;
      }
      space = std::filesystem::space(recording_root);
      result.available_bytes = space.available;
    }
  };
  if (uploaded_cleanup_threshold > 0 && result.available_bytes < uploaded_cleanup_threshold) {
    remove_until_safe(uploaded, true);
  }
  if (config.delete_unuploaded_when_below_free_gb && result.available_bytes < result.required_free_bytes) {
    remove_until_safe(unuploaded, false);
  }
  result.recording_allowed = result.available_bytes >= result.required_free_bytes;
  return result;
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
    std::filesystem::path recording_root,
    std::filesystem::path archive_root,
    double max_bandwidth_mbps,
    int retry_initial_seconds,
    int retry_max_seconds)
    : recording_root_(std::move(recording_root)),
      archive_root_(std::move(archive_root)),
      max_bandwidth_mbps_(max_bandwidth_mbps),
      retry_initial_seconds_(retry_initial_seconds),
      retry_max_seconds_(retry_max_seconds) {
  if (recording_root_.empty() || archive_root_.empty()) throw std::invalid_argument("recording and archive roots are required");
  if (!std::isfinite(max_bandwidth_mbps_) || max_bandwidth_mbps_ < 0.0) {
    throw std::invalid_argument("upload bandwidth limit must be finite and non-negative");
  }
  if (retry_initial_seconds_ <= 0 || retry_max_seconds_ < retry_initial_seconds_) {
    throw std::invalid_argument("upload retry bounds are invalid");
  }
}

UploadProcessResult LocalArchiveUploader::process_once() {
  if (!std::filesystem::exists(recording_root_)) return {};
  std::vector<std::filesystem::path> metadata_files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(
           recording_root_,
           std::filesystem::directory_options::skip_permission_denied)) {
    if (plain_regular_file(entry.path()) && entry.path().extension() == ".json" &&
        !entry.path().filename().string().ends_with(".tmp")) {
      metadata_files.push_back(entry.path());
    }
  }
  std::sort(metadata_files.begin(), metadata_files.end());
  std::unordered_set<std::string> current_retry_keys;
  current_retry_keys.reserve(metadata_files.size());
  for (const auto& metadata_path : metadata_files) {
    current_retry_keys.insert(metadata_path.generic_string());
  }
  for (auto retry = retries_.begin(); retry != retries_.end();) {
    if (!current_retry_keys.contains(retry->first)) {
      retry = retries_.erase(retry);
    } else {
      ++retry;
    }
  }
  std::optional<UploadProcessResult> first_failure;
  std::int64_t shortest_retry_after_ms = 0;
  std::uint64_t deferred_failures = 0;
  for (const auto& metadata_path : metadata_files) {
    const auto retry_key = metadata_path.generic_string();
    if (const auto retry = retries_.find(retry_key); retry != retries_.end()) {
      const auto now = std::chrono::steady_clock::now();
      if (now < retry->second.next_attempt_at) {
        const auto remaining = std::max<std::int64_t>(
            1,
            std::chrono::duration_cast<std::chrono::milliseconds>(retry->second.next_attempt_at - now).count());
        shortest_retry_after_ms = shortest_retry_after_ms == 0
            ? remaining
            : std::min(shortest_retry_after_ms, remaining);
        ++deferred_failures;
        continue;
      }
    }
    try {
      const auto metadata = read_json_file(metadata_path);
      if (metadata.value("upload_state", "pending") != "pending") {
        retries_.erase(retry_key);
        continue;
      }
      auto result = upload_metadata(metadata_path);
      retries_.erase(retry_key);
      result.deferred_failures = deferred_failures;
      return result;
    } catch (const std::exception& error) {
      auto& retry = retries_[retry_key];
      retry.attempts = std::min<std::uint32_t>(retry.attempts + 1, 31);
      const auto exponent = std::min<std::uint32_t>(retry.attempts - 1, 30);
      const auto multiplier = std::uint64_t{1} << exponent;
      const auto delay_seconds = std::min<std::uint64_t>(
          static_cast<std::uint64_t>(retry_max_seconds_),
          static_cast<std::uint64_t>(retry_initial_seconds_) * multiplier);
      retry.next_attempt_at = std::chrono::steady_clock::now() + std::chrono::seconds(delay_seconds);
      const auto retry_after_ms = static_cast<std::int64_t>(delay_seconds * 1000);
      shortest_retry_after_ms = shortest_retry_after_ms == 0
          ? retry_after_ms
          : std::min(shortest_retry_after_ms, retry_after_ms);
      ++deferred_failures;
      if (!first_failure.has_value()) {
        first_failure = UploadProcessResult{
            "failed",
            metadata_path.stem().string(),
            "",
            "",
            error.what(),
            0,
            retry_after_ms,
            deferred_failures};
      }
    }
  }
  if (first_failure.has_value()) {
    first_failure->deferred_failures = deferred_failures;
    return *first_failure;
  }
  if (deferred_failures > 0) {
    UploadProcessResult result;
    result.action = "retry_wait";
    result.retry_after_ms = shortest_retry_after_ms;
    result.deferred_failures = deferred_failures;
    return result;
  }
  return {};
}

UploadProcessResult LocalArchiveUploader::upload_metadata(const std::filesystem::path& metadata_path) {
  if (!plain_regular_file(metadata_path)) {
    throw std::runtime_error("segment metadata must be a regular non-symlink file: " + metadata_path.string());
  }
  auto metadata = read_json_file(metadata_path);
  const auto segment_id = metadata.value("segment_id", metadata_path.stem().string());
  if (segment_id.empty()) throw std::runtime_error("segment_id is missing from metadata");
  const auto video_path = sidecar_video_path(metadata_path, metadata);
  if (!plain_regular_file(video_path)) {
    throw std::runtime_error("segment video must be a regular non-symlink file: " + video_path.string());
  }
  if (!metadata.contains("video_sha256") || !metadata["video_sha256"].is_string()) {
    throw std::runtime_error("video_sha256 is required; legacy sidecar lacks an integrity proof");
  }
  const auto expected_sha256 = metadata["video_sha256"].get<std::string>();
  if (expected_sha256.size() != 64 ||
      !std::all_of(expected_sha256.begin(), expected_sha256.end(), [](unsigned char value) {
        return std::isxdigit(value) != 0;
      })) {
    throw std::runtime_error("video_sha256 is invalid in segment metadata");
  }
  const auto source_sha256 = sha256_file(video_path);
  if (source_sha256 != expected_sha256) {
    throw std::runtime_error("recorded video checksum no longer matches its sidecar: " + video_path.string());
  }
  const auto video_relative = safe_relative(recording_root_, video_path);
  const auto metadata_relative = safe_relative(recording_root_, metadata_path);
  const auto video_destination = archive_root_ / video_relative;
  const auto metadata_destination = archive_root_ / metadata_relative;
  const auto bytes = std::filesystem::file_size(video_path) + std::filesystem::file_size(metadata_path);
  const auto started = std::chrono::steady_clock::now();
  copy_verified_atomic(video_path, video_destination, expected_sha256);

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
    for (const auto& entry : std::filesystem::recursive_directory_iterator(
             recording_root_,
             std::filesystem::directory_options::skip_permission_denied)) {
      if (!plain_regular_file(entry.path()) || entry.path().extension() != ".json") continue;
      try {
        const auto metadata = read_json_file(entry.path());
        if (metadata.value("upload_state", "pending") != "pending") continue;
        ++pending_segments;
        const auto video = sidecar_video_path(entry.path(), metadata);
        pending_bytes += std::filesystem::file_size(entry.path());
        if (plain_regular_file(video)) pending_bytes += std::filesystem::file_size(video);
      } catch (const std::exception&) {
      }
    }
  }
  return {{"pending_segments", pending_segments}, {"pending_bytes", pending_bytes}};
}

}  // namespace mine_teleop
