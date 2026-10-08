#include "mine_teleop/session_logs.hpp"
#include "mine_teleop/platform.hpp"
#include "mine_teleop/server.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>

namespace mine_teleop {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t kFileLimit = 64 * 1024 * 1024;
constexpr std::uint64_t kScanLimit = 512 * 1024 * 1024;
constexpr std::size_t kChunkBytes = 256 * 1024;
constexpr std::int64_t kRetentionMs = 7LL * 24 * 60 * 60 * 1000;

void require(bool condition, const char* message) {
  if (!condition) throw std::invalid_argument(message);
}
bool safe_name(std::string_view name) {
  return !name.empty() && name.size() < 180 && name != "." && name != ".." &&
      std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '_' || c == '.';
      });
}
void private_directory(const fs::path& path) {
  fs::create_directories(path);
  fs::permissions(path, fs::perms::owner_all, fs::perm_options::replace);
}
void write_json(const fs::path& path, const Json& value) {
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  output << value.dump(2, ' ', false, Json::error_handler_t::replace) << '\n';
  if (!output) throw std::runtime_error("cannot write diagnostic metadata");
}
Json read_json(const fs::path& path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("diagnostic metadata is unavailable");
  return Json::parse(input);
}
struct TemporaryDirectory {
  fs::path path = fs::temp_directory_path() / ("mine-teleop-session-" + random_token(12));
  TemporaryDirectory() { private_directory(path); }
  ~TemporaryDirectory() { std::error_code error; fs::remove_all(path, error); }
};
bool pause(std::stop_token stop, int milliseconds) {
  for (int elapsed = 0; elapsed < milliseconds && !stop.stop_requested(); elapsed += 50)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  return !stop.stop_requested();
}
std::string redact_text(std::string text, const std::vector<std::string>& secrets) {
  for (const auto& secret : secrets) {
    if (secret.empty()) continue;
    std::size_t pos = 0;
    while ((pos = text.find(secret, pos)) != std::string::npos) {
      text.replace(pos, secret.size(), "[redacted]"); pos += 10;
    }
  }
  // Raw stdout can contain an HTTP header inside a JSON string. Redact the
  // complete Bearer value before the generic key=value rule sees whitespace.
  static const std::regex bearer(R"(\bBearer\s+[A-Za-z0-9._~+/=-]+)", std::regex::icase);
  text = std::regex_replace(text, bearer, "Bearer [redacted]");
  static const std::regex credential(
      R"((([?&]|\b)(?:[a-z_]*token|password|authorization|credential|secret|api_key)["']?\s*[=:]\s*["']?)[^\s&"',}]+)",
      std::regex::icase);
  return std::regex_replace(text, credential, "$1[redacted]");
}
Json redact(Json value, const std::vector<std::string>& secrets, int depth = 0) {
  if (depth > 64) return "[depth-limited]";
  if (value.is_object()) {
    for (auto it = value.begin(); it != value.end(); ++it) {
      auto key = it.key();
      std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
      const bool sensitive = key.find("password") != std::string::npos || key.find("token") != std::string::npos ||
          key.find("secret") != std::string::npos || key.find("credential") != std::string::npos ||
          key.find("authorization") != std::string::npos || key.find("cookie") != std::string::npos ||
          key.find("private_key") != std::string::npos || key.find("api_key") != std::string::npos;
      it.value() = sensitive ? Json("[redacted]") : redact(it.value(), secrets, depth + 1);
    }
  } else if (value.is_array()) {
    for (auto& item : value) item = redact(item, secrets, depth + 1);
  } else if (value.is_string()) value = redact_text(value.get<std::string>(), secrets);
  return value;
}
std::int64_t iso_time(const std::string& text) {
  if (text.size() < 20 || text.back() != 'Z') return 0;
  std::tm tm{}; std::istringstream input(text.substr(0, 19));
  input >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
  if (input.fail()) return 0;
  using namespace std::chrono;
  const year_month_day day{year{tm.tm_year + 1900}, month{static_cast<unsigned>(tm.tm_mon + 1)},
                           std::chrono::day{static_cast<unsigned>(tm.tm_mday)}};
  if (!day.ok()) return 0;
  auto result = duration_cast<milliseconds>(sys_days(day).time_since_epoch() + hours(tm.tm_hour) +
                                           minutes(tm.tm_min) + seconds(tm.tm_sec)).count();
  if (text.size() >= 24 && text[19] == '.' && std::all_of(text.begin() + 20, text.begin() + 23, ::isdigit))
    result += std::stoi(text.substr(20, 3));
  return result;
}
std::int64_t record_time(const Json& value) {
  for (const auto* key : {"logged_at_utc_ms", "event_at_utc_ms", "sent_at_utc_ms", "received_at_utc_ms"})
    if (value.contains(key) && value[key].is_number_integer()) return value[key].get<std::int64_t>();
  if (value.contains("ts") && value["ts"].is_string()) return iso_time(value["ts"]);
  if (value.contains("at") && value["at"].is_string()) return iso_time(value["at"]);
  return 0;
}
std::string record_session(const Json& value) {
  if (!value.is_object()) return {};
  for (const auto* key : {"session_id", "trace_session_id"})
    if (value.contains(key) && value[key].is_string() && !value[key].get<std::string>().empty()) return value[key];
  if (value.contains("details")) return record_session(value["details"]);
  return {};
}
Json missing(std::string_view reason) {
  return {{"status", "partial"}, {"reason", reason}, {"files", Json::array()}};
}
std::vector<fs::path> log_family(const fs::path& path) {
  std::vector<fs::path> result;
  if (path.empty()) return result;
  const auto parent = path.has_parent_path() ? path.parent_path() : fs::path(".");
  std::error_code error;
  for (const auto& item : fs::directory_iterator(parent, error)) {
    if (!fs::is_regular_file(item.symlink_status())) continue;
    const auto name = item.path().filename().string(), base = path.filename().string();
    const auto prefix = path.stem().string() + ".", suffix = path.extension().string();
    const bool period = name.starts_with(prefix) && name.ends_with(suffix) &&
        name.size() > prefix.size() + suffix.size() && std::regex_match(
            name.substr(prefix.size(), name.size() - prefix.size() - suffix.size()),
            std::regex("[0-9]{8}T[0-9]{6}Z\\.part[0-9]+"));
    if (name == base || period || (name.starts_with(base + ".") &&
        std::all_of(name.begin() + base.size() + 1, name.end(), [](unsigned char c) {
          return std::isdigit(c) || c == '-' || c == '.';
        }))) result.push_back(item.path());
  }
  std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
    return fs::last_write_time(a) < fs::last_write_time(b);
  });
  return result;
}
void prioritize_session_files(std::vector<fs::path>& paths, std::int64_t begin,
                              std::int64_t end, std::uint64_t& scanned, std::stop_token stop) {
  // Sample both ends before spending the scan budget on unrelated rotations.
  // Samples are hints, never grounds to exclude a file: clocks can jump and
  // asynchronous writers can interleave records. Unknown files remain eligible.
  constexpr std::size_t sample_bytes = 64 * 1024;
  constexpr std::uint64_t probe_limit = 8 * 1024 * 1024;
  struct Candidate { fs::path path; int priority; };
  std::vector<Candidate> candidates;
  for (const auto& path : paths) {
    int priority = 1;
    if (!stop.stop_requested() && scanned + 2 * sample_bytes <= probe_limit) {
      std::ifstream input(path, std::ios::binary | std::ios::ate);
      const auto size = input ? static_cast<std::streamoff>(input.tellg()) : 0;
      std::int64_t first = 0, last = 0;
      bool head_timestamp = false, tail_timestamp = false;
      for (int edge = 0; size > 0 && edge < 2; ++edge) {
        const auto offset = edge == 0 ? 0 : std::max<std::streamoff>(0, size - sample_bytes);
        input.clear(); input.seekg(offset);
        std::string sample(static_cast<std::size_t>(std::min<std::streamoff>(sample_bytes, size - offset)), '\0');
        input.read(sample.data(), static_cast<std::streamsize>(sample.size()));
        sample.resize(static_cast<std::size_t>(input.gcount())); scanned += sample.size();
        std::size_t pos = 0;
        if (offset > 0) { // Discard a possibly partial first record in the tail.
          const auto newline = sample.find('\n');
          pos = newline == std::string::npos ? sample.size() : newline + 1;
        }
        while (pos < sample.size()) {
          const auto newline = sample.find('\n', pos);
          if (newline == std::string::npos && offset + static_cast<std::streamoff>(sample.size()) < size) break;
          const auto line_end = newline == std::string::npos ? sample.size() : newline;
          const auto value = Json::parse(sample.substr(pos, line_end - pos), nullptr, false);
          const auto timestamp = value.is_object() ? record_time(value) : 0;
          if (timestamp) {
            (edge == 0 ? head_timestamp : tail_timestamp) = true;
            if (!first || timestamp < first) first = timestamp;
            last = std::max(last, timestamp);
            if (timestamp >= begin && timestamp <= end) priority = 0;
          }
          pos = line_end + 1;
        }
      }
      if (priority != 0 && head_timestamp && tail_timestamp)
        priority = first <= end && last >= begin ? 0 : 2;
    }
    candidates.push_back({path, priority});
  }
  // Keep the original chronological file order within each priority group.
  std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
    return a.priority < b.priority;
  });
  for (std::size_t i = 0; i < paths.size(); ++i) paths[i] = std::move(candidates[i].path);
}
std::string hex(std::string_view bytes) {
  constexpr char digits[] = "0123456789abcdef";
  std::string result; result.reserve(bytes.size() * 2);
  for (unsigned char byte : bytes) { result += digits[byte >> 4]; result += digits[byte & 15]; }
  return result;
}
std::string unhex(const std::string& text) {
  require(text.size() <= kChunkBytes * 2 && text.size() % 2 == 0, "invalid log chunk size");
  const auto digit = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    throw std::invalid_argument("invalid log chunk encoding");
  };
  std::string bytes; bytes.reserve(text.size() / 2);
  for (std::size_t i = 0; i < text.size(); i += 2) bytes += static_cast<char>((digit(text[i]) << 4) | digit(text[i+1]));
  return bytes;
}
bool allowed_vehicle_file(const std::string& name) { return name == "runtime.log" || name == "vcu-can.jsonl"; }
}

Json collect_session_logs(const std::vector<SessionLogSource>& sources, const Json& session,
                          const fs::path& destination, const std::vector<std::string>& secrets,
                          std::stop_token stop) {
  private_directory(destination);
  const auto begin = session.at("started_at_utc_ms").get<std::int64_t>() - 2000;
  const auto end = session.at("ended_at_utc_ms").get<std::int64_t>() + 2000;
  const auto id = session.at("session_id").get<std::string>();
  Json result = {{"status", "available"}, {"files", Json::array()},
      {"selection", "session_id and UTC window; unscoped records use UTC window"},
      {"window_start_utc_ms", begin}, {"window_end_utc_ms", end},
      {"redacted", true}, {"file_limit_bytes", kFileLimit}};
  for (const auto& source : sources) {
    require(safe_name(source.name), "invalid log source name");
    Json report = {{"name", source.name}, {"status", "available"}, {"bytes", 0},
                   {"unscoped_records", 0}, {"unattributed_lines", 0}, {"source_files", Json::array()}};
    std::uint64_t scanned = 0, written = 0, unscoped = 0, unattributed = 0;
    std::int64_t first = 0, last = 0;
    bool limited = false, failed = false, dropped = false;
    std::ofstream output(destination / source.name, std::ios::binary | std::ios::trunc);
    if (!output) throw std::runtime_error("cannot create log snapshot");
    auto paths = log_family(source.path);
    prioritize_session_files(paths, begin, end, scanned, stop);
    report["probe_bytes"] = scanned;
    report["scan_order"] = "UTC-window candidates, unknown ranges, other rotations";
    for (const auto& path : paths) {
      report["source_files"].push_back(path.filename().string());
      std::ifstream input(path, std::ios::binary);
      if (!input) { failed = true; continue; }
      // A damaged or malicious log writer cannot allocate an unbounded line.
      std::vector<char> buffer(1024 * 1024);
      while (!stop.stop_requested() && input.getline(buffer.data(), buffer.size())) {
        std::string line(buffer.data()); scanned += line.size() + 1;
        if (scanned > kScanLimit) { limited = true; break; }
        auto value = Json::parse(line, nullptr, false);
        if (!value.is_object()) { ++unattributed; continue; }
        const auto timestamp = record_time(value);
        const auto record_id = record_session(value);
        if (timestamp) { if (!first || timestamp < first) first = timestamp; last = std::max(last, timestamp); }
        if (!record_id.empty() && record_id != id) continue;
        if (timestamp && (timestamp < begin || timestamp > end)) continue;
        if (!timestamp) { ++unattributed; continue; }
        if (source.require_session_id && record_id != id) continue;
        if (record_id.empty()) ++unscoped;
        if (source.require_session_id && value.contains("service_instance_id") &&
            value["service_instance_id"] != session.at("service_instance_id")) continue;
        if (value.value("event", "") == "desktop_log_dropped" ||
            (value.contains("details") && value["details"].is_object() &&
             value["details"].value("dropped_since_last", 0LL) > 0)) dropped = true;
        line = redact(std::move(value), secrets).dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
        if (written + line.size() > kFileLimit) { limited = true; break; }
        output << line; written += line.size();
      }
      if (!input.eof() && !limited && !stop.stop_requested()) failed = true;
      if (limited || stop.stop_requested()) break;
    }
    output.flush(); if (!output) throw std::runtime_error("cannot finish log snapshot");
    report["bytes"] = written; report["unscoped_records"] = unscoped;
    report["unattributed_lines"] = unattributed; report["first_available_utc_ms"] = first;
    report["last_available_utc_ms"] = last;
    // A retained file is not evidence of complete coverage; state every gap.
    Json issues = Json::array();
    if (paths.empty() && !source.optional) issues.push_back("log_missing_or_disabled");
    if (limited) issues.push_back("size_or_scan_limit");
    if (failed) issues.push_back("read_error_or_overlong_line");
    if (stop.stop_requested()) issues.push_back("cancelled");
    if (unattributed) issues.push_back("records_without_UTC_timestamp");
    if (!written && !source.optional) issues.push_back("no_matching_records");
    if (first > session.at("started_at_utc_ms").get<std::int64_t>()) issues.push_back("start_not_covered_possible_rotation");
    if (last && last + 2000 < session.at("ended_at_utc_ms").get<std::int64_t>()) issues.push_back("end_not_covered_possible_logging_gap");
    if (dropped) issues.push_back("source_reported_log_queue_drops");
    report["issues"] = issues;
    report["optional"] = source.optional;
    if (!issues.empty()) { report["status"] = "partial"; result["status"] = "partial"; }
    result["files"].push_back(std::move(report));
  }
  return result;
}

Json session_log_chunk(const fs::path& directory, const Json& manifest,
                       std::string_view name, std::uint64_t offset) {
  require(safe_name(name), "invalid snapshot file");
  const auto& files = manifest.at("files");
  const auto entry = std::find_if(files.begin(), files.end(), [&](const Json& f) {
    return f.at("name").get_ref<const std::string&>() == name;
  });
  require(entry != files.end(), "snapshot file is not in manifest");
  const auto size = entry->at("bytes").get<std::uint64_t>();
  require(offset <= size && size <= kFileLimit, "invalid snapshot offset");
  std::ifstream input(directory / name, std::ios::binary);
  require(static_cast<bool>(input), "snapshot is unavailable");
  input.seekg(static_cast<std::streamoff>(offset));
  std::string data(static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, size - offset)), '\0');
  input.read(data.data(), static_cast<std::streamsize>(data.size()));
  require(static_cast<std::size_t>(input.gcount()) == data.size(), "snapshot changed during export");
  return {{"name", name}, {"offset", offset}, {"data_hex", hex(data)},
          {"next_offset", offset + data.size()}, {"eof", offset + data.size() == size}};
}

void append_session_log_chunk(const fs::path& directory, const Json& chunk) {
  const auto name = chunk.at("name").get<std::string>();
  require(safe_name(name), "invalid upload file");
  const auto offset = chunk.at("offset").get<std::uint64_t>();
  const auto data = unhex(chunk.at("data_hex"));
  require(offset <= kFileLimit && data.size() <= kFileLimit - offset, "snapshot exceeds limit");
  private_directory(directory);
  const auto path = directory / name;
  std::error_code error;
  const auto size = fs::exists(path) ? fs::file_size(path, error) : 0;
  require(!error && size == offset && !fs::is_symlink(path), "snapshot offset mismatch");
  std::ofstream output(path, std::ios::binary | std::ios::app); output.write(data.data(), data.size());
  require(static_cast<bool>(output), "cannot append log snapshot");
}

void write_session_zip(const fs::path& directory, const fs::path& destination) {
  struct Entry { std::string name; fs::path path; std::uint32_t size, crc, offset; };
  std::vector<Entry> entries;
  static const auto table = [] {
    std::array<std::uint32_t, 256> values{};
    for (std::uint32_t i = 0; i < 256; ++i) {
      auto v = i; for (int bit = 0; bit < 8; ++bit) v = (v >> 1) ^ ((v & 1) ? 0xedb88320U : 0);
      values[i] = v;
    }
    return values;
  }();
  for (const auto& item : fs::recursive_directory_iterator(directory)) {
    if (!fs::is_regular_file(item.symlink_status())) continue;
    const auto size = fs::file_size(item.path()); require(size <= kFileLimit, "ZIP file exceeds limit");
    std::uint32_t crc = 0xffffffffU;
    std::ifstream input(item.path(), std::ios::binary); std::array<char, 65536> buffer{};
    while (input.read(buffer.data(), buffer.size()) || input.gcount())
      for (std::streamsize i = 0; i < input.gcount(); ++i) crc = (crc >> 8) ^ table[(crc ^ static_cast<unsigned char>(buffer[i])) & 255];
    require(input.eof(), "cannot read ZIP source");
    entries.push_back({fs::relative(item.path(), directory).generic_string(), item.path(),
                       static_cast<std::uint32_t>(size), crc ^ 0xffffffffU, 0});
  }
  require(entries.size() <= 32, "too many ZIP files");
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return a.name < b.name; });
  std::ofstream output(destination, std::ios::binary | std::ios::trunc);
  const auto put = [&](std::uint32_t value, int bytes) { for (int i=0;i<bytes;++i) output.put(static_cast<char>(value >> (8*i))); };
  for (auto& entry : entries) {
    entry.offset = static_cast<std::uint32_t>(output.tellp());
    put(0x04034b50,4); put(20,2); put(0x800,2); put(0,2); put(0,2); put(0x21,2);
    put(entry.crc,4); put(entry.size,4); put(entry.size,4); put(static_cast<std::uint32_t>(entry.name.size()),2); put(0,2);
    output << entry.name; std::ifstream input(entry.path, std::ios::binary);
    if (entry.size) output << input.rdbuf();
  }
  const auto central = static_cast<std::uint32_t>(output.tellp());
  for (const auto& entry : entries) {
    put(0x02014b50,4); put(20,2); put(20,2); put(0x800,2); put(0,2); put(0,2); put(0x21,2);
    put(entry.crc,4); put(entry.size,4); put(entry.size,4); put(static_cast<std::uint32_t>(entry.name.size()),2);
    put(0,2); put(0,2); put(0,2); put(0,2); put(0,4); put(entry.offset,4); output << entry.name;
  }
  const auto central_size = static_cast<std::uint32_t>(output.tellp()) - central;
  put(0x06054b50,4); put(0,2); put(0,2);
  put(static_cast<std::uint32_t>(entries.size()),2); put(static_cast<std::uint32_t>(entries.size()),2);
  put(central_size,4); put(central,4); put(0,2); output.flush();
  require(static_cast<bool>(output), "cannot finish ZIP");
}

struct SessionLogBroker::Impl {
  struct Job {
    std::string id, driver, vehicle;
    Json session, cloud, vehicle_manifest;
    TemporaryDirectory temporary;
    std::int64_t created{now_ms()};
    std::jthread worker;
  };
  fs::path audit, catalog;
  TemporaryDirectory fallback;
  mutable std::mutex mutex;
  std::map<std::string, Json> records;
  std::map<std::string, std::unique_ptr<Job>> jobs;

  explicit Impl(fs::path path) : audit(std::move(path)) {
    catalog = audit.empty() ? fallback.path / "catalog" : audit.parent_path() / "session-log-catalog";
    private_directory(catalog);
    for (const auto& item : fs::directory_iterator(catalog)) {
      if (!fs::is_regular_file(item.symlink_status()) || item.path().extension() != ".json" ||
          fs::file_size(item.path()) > 16384) continue;
      try {
        const auto record = read_json(item.path());
        const auto key = record.at("key").get<std::string>();
        if (!safe_name(key) || record.at("started_at_utc_ms").get<std::int64_t>() < now_ms() - kRetentionMs) continue;
        records[key] = record;
      } catch (const std::exception&) { /* Corrupt entries cannot grant access. */ }
    }
  }
  Job& job(std::string_view id, std::string_view driver) {
    const auto found = jobs.find(std::string(id));
    require(found != jobs.end() && found->second->driver == driver, "export not found for this driver");
    return *found->second;
  }
};

SessionLogBroker::SessionLogBroker(fs::path path) : impl_(std::make_unique<Impl>(std::move(path))) {}
SessionLogBroker::~SessionLogBroker() {
  // Workers publish under mutex; never join them while holding that mutex.
  for (auto& [id, job] : impl_->jobs) { (void)id; job->worker.request_stop(); }
  for (auto& [id, job] : impl_->jobs) { (void)id; if (job->worker.joinable()) job->worker.join(); }
}
void SessionLogBroker::remember(const Json& session) noexcept {
  try {
    const auto key = session.at("key").get<std::string>(); require(safe_name(key), "invalid session key");
    std::lock_guard lock(impl_->mutex);
    const auto path = impl_->catalog / (key + ".json");
    write_json(path.string() + ".tmp", session); fs::rename(path.string() + ".tmp", path);
    impl_->records[key] = session;
    for (auto it = impl_->records.begin(); it != impl_->records.end();) {
      if (it->second.at("started_at_utc_ms").get<std::int64_t>() < now_ms() - kRetentionMs) {
        std::error_code error; fs::remove(impl_->catalog / (it->first + ".json"), error);
        it = impl_->records.erase(it);
      } else ++it;
    }
  } catch (const std::exception&) { /* Log export must never prevent safe session teardown. */ }
}
Json SessionLogBroker::sessions(std::string_view driver) const {
  std::lock_guard lock(impl_->mutex);
  Json result = Json::array();
  for (const auto& [key, record] : impl_->records) {
    (void)key;
    if (record.at("driver_id").get_ref<const std::string&>() == driver && record.value("ended_at_utc_ms", 0LL) > 0) result.push_back(record);
  }
  std::sort(result.begin(), result.end(), [](const Json& a, const Json& b) {
    return a.at("started_at_utc_ms").template get<std::int64_t>() > b.at("started_at_utc_ms").template get<std::int64_t>();
  });
  if (result.size() > 200) result.erase(result.begin() + 200, result.end());
  return result;
}
Json SessionLogBroker::session(std::string_view key, std::string_view driver) const {
  std::lock_guard lock(impl_->mutex);
  const auto found = impl_->records.find(std::string(key));
  require(found != impl_->records.end() && found->second.at("driver_id").get_ref<const std::string&>() == driver,
          "session not found for this driver");
  require(found->second.value("ended_at_utc_ms", 0LL) > 0, "end the session before exporting logs");
  return found->second;
}
Json SessionLogBroker::start(const Json& session, bool vehicle_online) {
  std::lock_guard lock(impl_->mutex);
  // Only finished jobs can be discarded without joining a live worker here.
  for (auto it = impl_->jobs.begin(); it != impl_->jobs.end();) {
    auto& job = *it->second;
    if (job.vehicle_manifest.is_null() && now_ms() - job.created > 180000)
      job.vehicle_manifest = missing("vehicle_timeout_offline_or_unsupported_version");
    if (!job.cloud.is_null() && now_ms() - job.created > 10 * 60 * 1000)
      it = impl_->jobs.erase(it);
    else ++it;
  }
  require(impl_->jobs.size() < 2, "two exports are retained; retry after ten minutes");
  for (const auto& [id, job] : impl_->jobs) {
    (void)id;
    require(job->driver != session.at("driver_id").get<std::string>() || (!job->cloud.is_null() && !job->vehicle_manifest.is_null()),
            "an export is already running");
  }
  auto next = std::make_unique<Impl::Job>(); auto* job = next.get();
  job->id = "export-" + random_token(12); job->driver = session.at("driver_id");
  job->vehicle = session.at("vehicle_id"); job->session = session;
  if (!vehicle_online) job->vehicle_manifest = missing("vehicle_offline");
  const auto id = job->id;
  impl_->jobs.emplace(id, std::move(next));
  job->worker = std::jthread([this, job](std::stop_token stop) {
    Json result;
    try {
      if (!pause(stop, 2100)) return;
      result = collect_session_logs({{impl_->audit, "signaling-audit.jsonl", true}}, job->session,
                                    job->temporary.path / "cloud", {}, stop);
    } catch (const std::exception&) { result = missing("cloud_log_read_failed"); }
    std::lock_guard guard(impl_->mutex); job->cloud = std::move(result);
  });
  return {{"export_id", id}, {"session", session}};
}
Json SessionLogBroker::status(std::string_view id, std::string_view driver, bool finish_partial) {
  std::lock_guard lock(impl_->mutex); auto& job = impl_->job(id, driver);
  if ((finish_partial || now_ms() - job.created > 180000) && job.vehicle_manifest.is_null())
    job.vehicle_manifest = missing("vehicle_timeout_offline_or_unsupported_version");
  return {{"export_id", job.id}, {"session", job.session},
          {"ready", !job.cloud.is_null() && !job.vehicle_manifest.is_null()},
          {"cloud", job.cloud}, {"vehicle", job.vehicle_manifest}};
}
Json SessionLogBroker::chunk(std::string_view id, std::string_view driver, const Json& request) {
  std::lock_guard lock(impl_->mutex); auto& job = impl_->job(id, driver);
  const auto source = request.at("source").get<std::string>();
  require(source == "cloud" || source == "vehicle", "invalid export source");
  const auto& manifest = source == "cloud" ? job.cloud : job.vehicle_manifest;
  require(!manifest.is_null(), "source is still collecting");
  return session_log_chunk(job.temporary.path / source, manifest, request.at("name").get<std::string>(),
                           request.at("offset").get<std::uint64_t>());
}
void SessionLogBroker::release(std::string_view id, std::string_view driver) {
  std::lock_guard lock(impl_->mutex); auto& job = impl_->job(id, driver);
  require(!job.cloud.is_null() && !job.vehicle_manifest.is_null(), "export is still collecting");
  impl_->jobs.erase(std::string(id));
}
Json SessionLogBroker::vehicle(std::string_view vehicle, const Json& request, bool idle) {
  std::lock_guard lock(impl_->mutex);
  const auto operation = request.at("operation").get<std::string>();
  if (operation == "poll") {
    if (idle) for (auto& [id, job] : impl_->jobs) {
      if (job->vehicle == vehicle && job->vehicle_manifest.is_null() && now_ms() - job->created < 180000)
        return {{"export_id", id}, {"session", job->session}};
    }
    return Json::object();
  }
  const auto found = impl_->jobs.find(request.at("export_id").get<std::string>());
  require(found != impl_->jobs.end() && found->second->vehicle == vehicle, "export not found for vehicle");
  auto& job = *found->second;
  require(job.vehicle_manifest.is_null(), "vehicle snapshot is already closed");
  if (!idle) { job.vehicle_manifest = missing("new_control_session_started"); return {{"cancelled", true}}; }
  if (operation == "append") {
    const auto& chunk = request.at("chunk");
    require(allowed_vehicle_file(chunk.at("name")), "unapproved vehicle log file");
    append_session_log_chunk(job.temporary.path / "vehicle", chunk);
    return {{"accepted", true}};
  }
  if (operation == "finish") {
    auto manifest = request.at("manifest");
    require(manifest.is_object() && manifest.at("files").is_array() && manifest.at("files").size() == 2,
            "invalid vehicle manifest");
    std::vector<std::string> names;
    for (const auto& file : manifest.at("files")) {
      const auto name = file.at("name").get<std::string>();
      require(allowed_vehicle_file(name) && std::find(names.begin(), names.end(), name) == names.end(), "invalid vehicle manifest file");
      names.push_back(name);
      const auto bytes = file.at("bytes").get<std::uint64_t>();
      require(bytes <= kFileLimit && fs::file_size(job.temporary.path / "vehicle" / name) == bytes, "incomplete upload");
    }
    require(manifest.value("status", "") == "partial" || manifest.value("status", "") == "available", "invalid source status");
    job.vehicle_manifest = std::move(manifest); return {{"accepted", true}};
  }
  throw std::invalid_argument("unknown vehicle log operation");
}

std::jthread start_vehicle_log_worker(const VehicleConfig& config, std::string token) {
  const auto log_path = [](const char* key, const char* fallback) {
    const auto* value = std::getenv(key); return fs::path(value && *value ? value : fallback);
  };
  const auto runtime = log_path("MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH", "/var/log/mine-teleop/vehicle-runtime.log");
  const auto can = log_path("MINE_TELEOP_VCU_LOG_PATH", "/var/log/mine-teleop/vcu-can.jsonl");
  return std::jthread([config, token = std::move(token), runtime, can](std::stop_token stop) {
    HttpClient http(std::chrono::seconds(2), config.cloud.resolve_entries, config.cloud.ca_bundle);
    const auto url = normalize_signaling_http_url(config.cloud.signaling_url) + "/sessions/logs/vehicle";
    std::string last_attempt;
    while (pause(stop, 3000)) {
      try {
        const auto call = [&](Json request) {
          request["vehicle_id"] = config.vehicle_id; request["device_token"] = token;
          return http.post_json_response(url, request);
        };
        const auto task = call({{"operation", "poll"}});
        if (!task.contains("export_id")) continue;
        const auto id = task.at("export_id").get<std::string>();
        if (id == last_attempt) continue; // Lost/partial transfers are reported, never duplicated.
        last_attempt = id;
        TemporaryDirectory snapshot;
        auto manifest = collect_session_logs({{runtime, "runtime.log"}, {can, "vcu-can.jsonl"}},
                                             task.at("session"), snapshot.path, {token}, stop);
        bool cancelled = false;
        for (const auto& file : manifest.at("files")) {
          std::uint64_t offset = 0;
          do {
            if (stop.stop_requested()) return;
            const auto chunk = session_log_chunk(snapshot.path, manifest, file.at("name").get<std::string>(), offset);
            const auto response = call({{"operation", "append"}, {"export_id", id}, {"chunk", chunk}});
            if (response.value("cancelled", false)) { cancelled = true; break; }
            offset = chunk.at("next_offset");
            if (chunk.at("eof").get<bool>()) break;
            if (!pause(stop, 250)) return;
          } while (true);
          if (cancelled) break;
        }
        if (!cancelled && !stop.stop_requested()) call({{"operation", "finish"}, {"export_id", id}, {"manifest", manifest}});
      } catch (const std::exception&) { /* Retry polling; the cloud marks incomplete transfers after timeout. */ }
    }
  });
}

struct ControllerLogExport::Impl {
  TemporaryDirectory temporary;
  mutable std::mutex mutex;
  Json progress = {{"state", "collecting"}, {"message", "正在收集三端日志"}};
  std::jthread worker;
};
ControllerLogExport::ControllerLogExport(std::string origin, std::vector<std::string> resolve,
    fs::path ca, std::string driver, std::string token, std::string key, fs::path browser_log)
    : impl_(std::make_unique<Impl>()) {
  impl_->worker = std::jthread([this, origin = std::move(origin), resolve = std::move(resolve), ca = std::move(ca),
      driver = std::move(driver), token = std::move(token), key = std::move(key), browser_log = std::move(browser_log)](std::stop_token stop) {
    try {
      HttpClient http(std::chrono::seconds(2), resolve, ca);
      const auto call = [&](std::string_view operation, Json body = Json::object()) {
        body["operation"] = operation; body["driver_id"] = driver; body["token"] = token;
        return http.post_json_response(origin + "/sessions/logs/driver", body);
      };
      const auto started = call("start", {{"key", key}});
      const auto id = started.at("export_id").get<std::string>();
      const auto session = started.at("session");
      if (!pause(stop, 2100)) return;
      const auto root = impl_->temporary.path / "bundle";
      auto controller = collect_session_logs({{browser_log, "control-browser-events.jsonl"},
          {browser_log.parent_path() / "desktop-events.jsonl", "desktop-events.jsonl", false, true}},
          session, root / "controller", {token}, stop);
      // Desktop events are optional; their absence is still explicitly represented.
      Json remote;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(185);
      while (!stop.stop_requested()) {
        remote = call("status", {{"export_id", id}, {"finish_partial", std::chrono::steady_clock::now() >= deadline}});
        if (remote.at("ready").get<bool>()) break;
        if (!pause(stop, 1000)) return;
      }
      if (stop.stop_requested()) return;
      for (const auto* source : {"cloud", "vehicle"}) {
        for (const auto& file : remote.at(source).at("files")) {
          std::uint64_t offset = 0;
          do {
            if (stop.stop_requested()) return;
            const auto chunk = call("chunk", {{"export_id", id}, {"source", source},
                {"name", file.at("name")}, {"offset", offset}});
            append_session_log_chunk(root / source, chunk);
            const auto next = chunk.at("next_offset").get<std::uint64_t>();
            require(next > offset || chunk.at("eof").get<bool>(), "log download made no progress"); offset = next;
            if (chunk.at("eof").get<bool>()) break;
            if (!pause(stop, 150)) return;
          } while (true);
          require(offset == file.at("bytes").get<std::uint64_t>(), "log download size mismatch");
        }
      }
      const bool complete = controller.at("status") == "available" && remote.at("cloud").at("status") == "available" &&
                            remote.at("vehicle").at("status") == "available";
      // The downloaded snapshot now belongs to this controller. Free the
      // cloud slot so successive exports do not wait for retention expiry.
      static_cast<void>(call("release", {{"export_id", id}}));
      const Json manifest = {{"schema_version", 1}, {"session", session}, {"exported_at_utc_ms", now_ms()},
          {"complete", complete}, {"controller", controller}, {"cloud", remote.at("cloud")}, {"vehicle", remote.at("vehicle")},
          {"notes", "UTC window includes 2s before/after. CAN and raw runtime lines may be time-correlated rather than session-tagged. Credentials are redacted. Rotated-away, disabled or unavailable logs cannot be reconstructed; consult each file's issues. Videos and system journals are not included."}};
      write_json(root / "manifest.json", manifest);
      write_session_zip(root, impl_->temporary.path / "session.zip");
      std::lock_guard lock(impl_->mutex);
      impl_->progress = {{"state", "ready"}, {"complete", complete}, {"session", session},
                        {"filename", key + ".zip"}, {"manifest", manifest}};
    } catch (const std::exception&) {
      std::lock_guard lock(impl_->mutex);
      impl_->progress = {{"state", "failed"}, {"message", "日志导出失败，请确认已登录、会话已结束且三端版本支持导出，然后重试。"}};
    }
  });
}
ControllerLogExport::~ControllerLogExport() { cancel(); if (impl_->worker.joinable()) impl_->worker.join(); }
void ControllerLogExport::cancel() { impl_->worker.request_stop(); }
Json ControllerLogExport::status() const { std::lock_guard lock(impl_->mutex); return impl_->progress; }
std::string ControllerLogExport::zip() const {
  std::lock_guard lock(impl_->mutex);
  require(impl_->progress.at("state") == "ready", "export is not ready");
  std::ifstream input(impl_->temporary.path / "session.zip", std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

}  // namespace mine_teleop
