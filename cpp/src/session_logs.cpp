#include "mine_teleop/session_logs.hpp"
#include "mine_teleop/platform.hpp"
#include "mine_teleop/server.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <deque>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

namespace mine_teleop {
namespace {
namespace fs = std::filesystem;
constexpr std::uint64_t kFileLimit = 64 * 1024 * 1024;
constexpr std::uint64_t kScanLimit = 512 * 1024 * 1024;
constexpr std::size_t kChunkBytes = 256 * 1024;
constexpr std::int64_t kRetentionMs = 7LL * 24 * 60 * 60 * 1000;
constexpr std::int64_t kVehicleIdleTimeoutMs = 180000;
constexpr std::int64_t kExportTimeoutMs = 10 * 60 * 1000;

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
std::string redact_credentials(std::string_view text, bool bearer) {
  const auto word = [](unsigned char c) { return std::isalnum(c) || c == '_'; };
  const auto space = [](unsigned char c) { return std::isspace(c); };
  std::string result;
  std::size_t copied = 0;
  for (std::size_t pos = 0; pos < text.size();) {
    if (!word(text[pos]) || (pos && word(text[pos - 1]))) { ++pos; continue; }
    const auto begin = pos;
    while (pos < text.size() && word(text[pos])) ++pos;
    std::string key(text.substr(begin, pos - begin));
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool token = key.ends_with("token") && std::all_of(key.begin(), key.end(), [](unsigned char c) {
      return (c >= 'a' && c <= 'z') || c == '_';
    });
    if (bearer ? key != "bearer" : !token && key != "password" && key != "authorization" &&
        key != "credential" && key != "secret" && key != "api_key") continue;
    auto value = pos;
    if (bearer) {
      if (value == text.size() || !space(text[value])) continue;
    } else if (value < text.size() && (text[value] == '"' || text[value] == '\'')) ++value;
    while (value < text.size() && space(text[value])) ++value;
    if (!bearer) {
      if (value == text.size() || (text[value] != '=' && text[value] != ':')) continue;
      ++value;
      while (value < text.size() && space(text[value])) ++value;
      if (value < text.size() && (text[value] == '"' || text[value] == '\'')) ++value;
    }
    auto end = value;
    while (end < text.size()) {
      const auto c = static_cast<unsigned char>(text[end]);
      if (bearer ? !(std::isalnum(c) || std::string_view("._~+/=-").find(c) != std::string_view::npos)
                 : space(c) || std::string_view("&\"',}").find(c) != std::string_view::npos) break;
      ++end;
    }
    if (end == value) continue;
    result.append(text.substr(copied, value - copied));
    result += "[redacted]";
    copied = end;
    pos = end;
  }
  result.append(text.substr(copied));
  return result;
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
  // libstdc++ regex recursion can exhaust the stack on long log strings.
  // Scan each word/value iteratively while preserving the same redaction rules.
  return redact_credentials(redact_credentials(text, true), false);
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
  for (const auto* key : {"logged_at_utc_ms", "event_at_utc_ms", "sent_at_utc_ms", "received_at_utc_ms", "reported_at_utc_ms"})
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
std::optional<bool> select_trace_commands(Json& value, std::string_view session_id,
                                         std::int64_t begin, std::int64_t end) {
  const auto event = value.value("event", "");
  if (event != "cloud_native_control_trace_batch" &&
      event != "vehicle_control_trace_batch" &&
      event != "driver_native_control_trace_batch") return std::nullopt;
  auto& payload = event == "vehicle_control_trace_batch" ? value : value["details"];
  if (!payload.is_object() || !payload.contains("commands") || !payload["commands"].is_array()) return std::nullopt;
  auto& commands = payload["commands"].get_ref<Json::array_t&>();
  std::erase_if(commands, [&](const Json& command) {
    if (record_session(command) != session_id) return true;
    auto timestamp = record_time(command);
    if (!timestamp && command.contains("command_sent_at_utc_ms") && command["command_sent_at_utc_ms"].is_number_integer())
      timestamp = command["command_sent_at_utc_ms"].get<std::int64_t>();
    if (!timestamp) timestamp = record_time(value);
    return !timestamp || timestamp < begin || timestamp > end;
  });
  // A cloud batch can contain multiple sessions. Never include its unrelated
  // commands merely because one entry belongs to the requested session.
  payload["trace_session_id"] = commands.empty() ? "" : std::string(session_id);
  return !commands.empty();
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
  const auto session_started = session.at("started_at_utc_ms").get<std::int64_t>();
  const auto session_ended = session.at("ended_at_utc_ms").get<std::int64_t>();
  Json result = {{"status", "available"}, {"files", Json::array()},
      {"selection", "session_id and UTC window; late runtime recording completion requires an attempt-start anchor inside the session"},
      {"window_start_utc_ms", begin}, {"window_end_utc_ms", end},
      {"redacted", true}, {"file_limit_bytes", kFileLimit}};
  for (const auto& source : sources) {
    require(safe_name(source.name), "invalid log source name");
    Json report = {{"name", source.name}, {"status", "available"}, {"bytes", 0},
                   {"unscoped_records", 0}, {"unattributed_lines", 0}, {"source_files", Json::array()}};
    std::uint64_t scanned = 0, written = 0, unscoped = 0, unattributed = 0;
    std::int64_t first = 0, last = 0;
    bool limited = false, failed = false, dropped = false;
    std::unordered_set<std::string> recording_attempts;
    std::vector<std::pair<std::string, std::string>> late_recording;
    std::size_t late_recording_bytes = 0;
    constexpr std::size_t kAttemptLimit = 4096, kLateRecordingLimit = 4 * 1024 * 1024;
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
        const auto trace_batch = select_trace_commands(value, id, begin, end);
        const auto record_id = record_session(value);
        if (timestamp) { if (!first || timestamp < first) first = timestamp; last = std::max(last, timestamp); }
        if (trace_batch && !*trace_batch) continue;
        if (!record_id.empty() && record_id != id) continue;
        // Recorder draining can finish after the ordinary two-second log tail.
        // Preserve only its exactly scoped completion record beyond that tail;
        // unrelated runtime lines still obey the normal UTC/session selection.
        const bool recording_scope = source.name == "runtime.log" && record_id == id &&
            value.contains("vehicle_id") && value.at("vehicle_id") == session.at("vehicle_id");
        const auto kind = value.value("event", "");
        const std::string attempt = value.contains("recording_attempt_id") && value.at("recording_attempt_id").is_string()
            ? value.at("recording_attempt_id").get<std::string>() : "";
        const bool recording_completion = recording_scope && kind == "vehicle_recording_attempt_finished" && safe_name(attempt);
        if (source.require_session_id && record_id != id) continue;
        if (source.require_session_id && value.contains("service_instance_id") &&
            value["service_instance_id"] != session.at("service_instance_id")) continue;
        // SIDs can repeat after a cloud restart. Only the producer's start
        // timestamp may anchor an attempt inside the actual session. Launcher
        // logged_at is relay time and can be late or on a different clock.
        const auto reported = value.find("reported_at_utc_ms");
        if (recording_scope && kind == "vehicle_recording_attempt_started" && safe_name(attempt) &&
            reported != value.end() && reported->is_number_integer() &&
            (!reported->is_number_unsigned() || reported->get<std::uint64_t>() <= INT64_MAX)) {
          const auto produced_at = reported->get<std::int64_t>();
          if (produced_at >= session_started && produced_at <= session_ended) {
            recording_attempts.insert(attempt);
            if (recording_attempts.size() > kAttemptLimit) { limited = true; break; }
          }
        }
        if (!trace_batch && timestamp && (timestamp < begin || (timestamp > end && !recording_completion))) continue;
        if (!timestamp) { ++unattributed; continue; }
        if (record_id.empty()) ++unscoped;
        if (value.value("event", "") == "desktop_log_dropped" ||
            (value.contains("details") && value["details"].is_object() &&
             value["details"].value("dropped_since_last", 0LL) > 0)) dropped = true;
        line = redact(std::move(value), secrets).dump(-1, ' ', false, Json::error_handler_t::replace) + '\n';
        if (recording_completion && timestamp > end) {
          if (late_recording.size() >= kAttemptLimit || late_recording_bytes + line.size() > kLateRecordingLimit) {
            limited = true; break;
          }
          late_recording_bytes += line.size();
          late_recording.emplace_back(attempt, std::move(line));
          continue;
        }
        if (written + line.size() > kFileLimit) { limited = true; break; }
        output << line; written += line.size();
      }
      if (!input.eof() && !limited && !stop.stop_requested()) failed = true;
      if (limited || stop.stop_requested()) break;
    }
    // Rotation order may expose completion before its anchor. Match only
    // after scanning, without expanding the original file/scan bounds.
    for (const auto& [attempt, line] : late_recording) {
      if (stop.stop_requested()) break;
      if (!recording_attempts.contains(attempt)) continue;
      if (written + line.size() > kFileLimit) { limited = true; break; }
      output << line; written += line.size();
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

Json session_recording_evidence(const Json& summary, const Json& session, const Json& attempt_fence) {
  const auto unknown = [](std::string_view issue) {
    return Json{{"included", false}, {"evidence_status", "unknown"}, {"complete", nullptr},
        {"source", "recording-session.status"}, {"issues", Json::array({issue})}};
  };
  if (!summary.is_object()) return unknown("recording_summary_unavailable");
  try {
    const auto boolean = [](const Json& object, const char* key) {
      require(object.contains(key) && object.at(key).is_boolean(), "invalid recording flag");
      return object.at(key).get<bool>();
    };
    const auto number = [](const Json& object, const char* key) {
      require(object.contains(key) && object.at(key).is_number_integer() &&
          (object.at(key).is_number_unsigned() || object.at(key).get<std::int64_t>() >= 0), "invalid recording counter");
      return object.at(key).get<std::uint64_t>();
    };
    const auto text = [](const Json& object, const char* key, std::size_t limit = 180) {
      require(object.contains(key) && object.at(key).is_string() && object.at(key).get_ref<const std::string&>().size() <= limit,
              "invalid recording text");
      return object.at(key).get<std::string>();
    };
    require(number(summary, "schema_version") == 1 && text(summary, "kind") == "recording_session_summary" &&
        text(summary, "upload_state") == "not_applicable", "unsupported recording summary");
    if (text(summary, "vehicle_id") != text(session, "vehicle_id") || text(summary, "session_id") != text(session, "session_id") ||
        number(summary, "reported_at_utc_ms") < session.at("started_at_utc_ms").get<std::uint64_t>())
      return unknown("recording_summary_scope_mismatch");
    require(!text(summary, "recording_attempt_id").empty(), "missing recording attempt");
    const auto configured = boolean(summary, "configured_enabled"), incomplete = boolean(summary, "incomplete");
    const auto final_eos = boolean(summary, "final_eos_received");
    const auto history_known = boolean(summary, "session_history_known"), producer_attempt_complete = boolean(summary, "attempt_complete");
    const auto completeness_scope = text(summary, "completeness_scope");
    require(completeness_scope == "current_runtime_known_attempts" || completeness_scope == "full_session", "unsupported recording completeness scope");
    const auto revision = number(summary, "summary_revision");
    const auto state = text(summary, "state");
    require(summary.at("expected_cameras").is_array() && summary.at("expected_cameras").size() <= 64 &&
        summary.at("cameras").is_array() && summary.at("cameras").size() <= 64, "invalid recording camera list");
    std::vector<std::string> expected, observed;
    for (const auto& camera : summary.at("expected_cameras")) {
      require(camera.is_string(), "invalid recording camera");
      const auto id = camera.get<std::string>();
      require(safe_name(id) && std::find(expected.begin(), expected.end(), id) == expected.end(), "invalid recording camera");
      expected.push_back(id);
    }
    bool complete = configured && !incomplete && state == "finalized" && final_eos && revision > 0 && !expected.empty();
    bool gap = incomplete || state == "failed" || (attempt_fence.is_object() && attempt_fence.value("incomplete", false));
    Json cameras = Json::array();
    for (const auto& entry : summary.at("cameras")) {
      const auto camera = text(entry, "camera_id");
      require(std::find(expected.begin(), expected.end(), camera) != expected.end() &&
          std::find(observed.begin(), observed.end(), camera) == observed.end(), "unexpected recording camera");
      observed.push_back(camera);
      const auto& recording = entry.at("recording");
      require(number(recording, "schema_version") == 1, "unsupported recording lane");
      const auto lane_state = text(recording, "state");
      const auto enabled = boolean(recording, "configured_enabled"), active = boolean(recording, "active");
      const auto lane_gap = boolean(recording, "incomplete"), eos = boolean(recording, "final_eos_received");
      const auto cleanup = boolean(recording, "cleanup_pending");
      const auto accepted = number(recording, "accepted_access_units"), input = number(recording, "worker_input_access_units");
      const auto finalized = number(recording, "finalized_segments"), unfinished = number(recording, "unfinished_segments");
      const auto lane_revision = number(recording, "status_revision");
      const auto attempt = text(recording, "recording_attempt_id");
      require(!attempt.empty() && attempt == text(summary, "recording_attempt_id"), "recording lane attempt mismatch");
      gap = gap || lane_gap || lane_state == "failed";
      complete = complete && enabled && !active && !lane_gap && !cleanup && eos && lane_state == "finalized" &&
          accepted > 0 && input == accepted && finalized > 0 && unfinished == 0 && lane_revision > 0;
      cameras.push_back({{"camera_id", camera}, {"recording", {{"schema_version", 1}, {"state", lane_state},
          {"configured_enabled", enabled}, {"active", active}, {"incomplete", lane_gap}, {"final_eos_received", eos},
          {"cleanup_pending", cleanup}, {"accepted_access_units", accepted}, {"worker_input_access_units", input},
          {"finalized_segments", finalized}, {"unfinished_segments", unfinished}, {"status_revision", lane_revision},
          {"recording_attempt_id", attempt}, {"issue_code", text(recording, "issue_code")},
          {"next_action", text(recording, "next_action", 2048)}}}});
    }
    complete = complete && observed.size() == expected.size();
    const bool fence_valid = attempt_fence.is_object() && attempt_fence.value("source_complete", false) &&
        attempt_fence.value("recording_attempt_id", "") == text(summary, "recording_attempt_id") &&
        attempt_fence.value("reported_at_utc_ms", std::uint64_t{0}) >= session.at("started_at_utc_ms").get<std::uint64_t>() &&
        number(summary, "reported_at_utc_ms") >= attempt_fence.value("reported_at_utc_ms", std::uint64_t{0}) &&
        !(attempt_fence.value("kind", "") == "status" && !attempt_fence.value("summary_persisted", false) &&
          attempt_fence.value("reported_at_utc_ms", std::uint64_t{0}) >= number(summary, "reported_at_utc_ms"));
    complete = complete && !gap && producer_attempt_complete && fence_valid &&
        number(summary, "reported_at_utc_ms") >= session.at("ended_at_utc_ms").get<std::uint64_t>();
    if (complete) {
      require(summary.at("completed_at_utc_ms").is_number_integer() &&
          summary.at("completed_at_utc_ms").get<std::int64_t>() >= session.at("started_at_utc_ms").get<std::int64_t>() &&
          summary.at("completed_at_utc_ms").get<std::uint64_t>() <= number(summary, "reported_at_utc_ms"), "invalid recording completion time");
    }
    Json canonical = {{"schema_version", 1}, {"kind", "recording_session_summary"}, {"upload_state", "not_applicable"},
        {"vehicle_id", text(summary, "vehicle_id")}, {"session_id", text(summary, "session_id")},
        {"recording_attempt_id", text(summary, "recording_attempt_id")}, {"configured_enabled", configured}, {"state", state},
        {"incomplete", gap}, {"expected_cameras", expected}, {"final_eos_received", final_eos},
        {"session_history_known", history_known}, {"completeness_scope", completeness_scope}, {"attempt_complete", complete},
        {"summary_revision", revision}, {"reported_at_utc_ms", number(summary, "reported_at_utc_ms")},
        {"completed_at_utc_ms", complete ? summary.at("completed_at_utc_ms") : Json(nullptr)}, {"cameras", cameras}};
    const bool disabled = !configured && state == "disabled" && expected.empty() && cameras.empty() && !gap;
    const bool session_complete = complete && history_known && completeness_scope == "full_session";
    Json fence = Json();
    if (attempt_fence.is_object()) fence = {{"recording_attempt_id", attempt_fence.value("recording_attempt_id", "")},
        {"reported_at_utc_ms", attempt_fence.value("reported_at_utc_ms", std::uint64_t{0})}, {"source_complete", attempt_fence.value("source_complete", false)},
        {"kind", attempt_fence.value("kind", "")}, {"summary_persisted", attempt_fence.value("summary_persisted", false)},
        {"incomplete", attempt_fence.value("incomplete", false)}};
    return {{"included", false}, {"source", "recording-session.status"},
        {"evidence_status", gap ? "incomplete" : disabled ? "not_enabled" : session_complete ? "finalized" : complete ? "attempt_finalized" : "unknown"},
        {"complete", gap ? Json(false) : session_complete ? Json(true) : Json(nullptr)}, {"attempt_complete", complete},
        {"summary", canonical}, {"attempt_fence", fence},
        {"issues", gap ? Json::array({"recording_incomplete"}) : disabled || session_complete ? Json::array() :
          complete ? Json::array({"recording_session_history_unconfirmed"}) : Json::array({"recording_finalization_unconfirmed"})}};
  } catch (const std::exception&) { return unknown("recording_summary_invalid"); }
}

Json collect_session_recording_evidence(const fs::path& recording_root, const Json& session,
                                       const std::vector<std::string>& secrets, const fs::path& runtime_snapshot,
                                       bool runtime_source_complete) {
  const auto unavailable = [](std::string_view issue) {
    return Json{{"included", false}, {"evidence_status", "unknown"}, {"complete", nullptr},
        {"source", "recording-session.status"}, {"issues", Json::array({issue})}};
  };
  try {
    const auto vehicle = session.at("vehicle_id").get<std::string>(), id = session.at("session_id").get<std::string>();
    if (recording_root.empty() || !safe_name(vehicle) || !safe_name(id)) return unavailable("recording_summary_path_invalid");
    const auto directory = recording_root / vehicle / id, path = directory / "recording-session.status";
    if (fs::is_symlink(recording_root / vehicle) || fs::is_symlink(directory) || fs::is_symlink(path) || !fs::is_regular_file(path))
      return unavailable("recording_summary_unavailable");
    // The exact small summary is independent of the runtime log UTC window.
    // Never enumerate/upload recordings or infer completion from MP4 existence.
    constexpr std::size_t limit = 64 * 1024;
    std::ifstream input(path, std::ios::binary); std::string bytes(limit + 1, '\0');
    input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (input.bad() || input.gcount() > static_cast<std::streamsize>(limit)) return unavailable("recording_summary_read_failed");
    bytes.resize(static_cast<std::size_t>(input.gcount()));
    const auto summary = Json::parse(bytes, nullptr, false);
    Json fence;
    if (runtime_source_complete && !runtime_snapshot.empty() && !fs::is_symlink(runtime_snapshot)) {
      std::ifstream logs(runtime_snapshot, std::ios::binary); std::vector<char> line(1024 * 1024);
      std::uint64_t latest = 0; bool gap = false, ambiguous = false;
      while (logs.getline(line.data(), static_cast<std::streamsize>(line.size()))) {
        const auto event = Json::parse(line.data(), nullptr, false);
        if (!event.is_object() || event.value("session_id", "") != id || event.value("vehicle_id", "") != vehicle) continue;
        const auto kind = event.value("event", "");
        std::string attempt; std::uint64_t reported = 0; bool persisted = false;
        if (kind == "vehicle_recording_attempt_started") {
          attempt = event.value("recording_attempt_id", ""); reported = event.value("reported_at_utc_ms", std::uint64_t{0});
        } else if (kind == "vehicle_recording_attempt_finished") {
          // A terminal failure can be newer than the last persisted worker
          // summary. Success logs alone never supply positive EOS/file proof.
          if (!event.value("incomplete", false)) continue;
          attempt = event.value("recording_attempt_id", ""); reported = event.value("reported_at_utc_ms", std::uint64_t{0});
        } else if (kind == "vehicle_media_status" && event.contains("status") && event.at("status").contains("lanes")) {
          persisted = true;
          for (const auto& lane : event.at("status").at("lanes")) {
            if (!lane.contains("recording") || !lane.at("recording").is_object()) continue;
            const auto& recording = lane.at("recording");
            if (!recording.value("configured_enabled", false)) continue;
            const auto candidate = recording.value("recording_attempt_id", "");
            if (!attempt.empty() && candidate != attempt) ambiguous = true;
            attempt = candidate; reported = std::max(reported, recording.value("reported_at_utc_ms", std::uint64_t{0}));
            persisted = persisted && recording.value("summary_persisted", false);
            gap = gap || recording.value("incomplete", false) || recording.value("state", "") == "failed";
          }
        } else continue;
        if (attempt.empty() || reported < session.at("started_at_utc_ms").get<std::uint64_t>()) continue;
        if (kind == "vehicle_recording_attempt_finished") gap = true;
        if (reported == latest && fence.is_object() && fence.value("recording_attempt_id", "") != attempt) ambiguous = true;
        if (reported >= latest) {
          latest = reported; fence = {{"recording_attempt_id", attempt}, {"reported_at_utc_ms", reported},
              {"source_complete", !ambiguous}, {"kind", kind == "vehicle_recording_attempt_started" ? "started" :
                  kind == "vehicle_recording_attempt_finished" ? "finished" : "status"},
              {"summary_persisted", persisted}, {"incomplete", gap}};
        }
      }
      if (!logs.eof()) fence = Json();
      if (fence.is_object()) {fence["incomplete"] = gap; fence["source_complete"] = !ambiguous;}
    }
    // Logs only fence the latest attempt and carry negative evidence. The
    // positive End/EOS/closed evidence still comes exclusively from the summary.
    return session_recording_evidence(redact(summary, secrets), session, fence);
  } catch (const std::exception&) { return unavailable("recording_summary_read_failed"); }
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

void append_session_log_chunk(const fs::path& directory, const Json& chunk, bool allow_retry) {
  const auto name = chunk.at("name").get<std::string>();
  require(safe_name(name), "invalid upload file");
  const auto offset = chunk.at("offset").get<std::uint64_t>();
  const auto data = unhex(chunk.at("data_hex"));
  require(offset <= kFileLimit && data.size() <= kFileLimit - offset, "snapshot exceeds limit");
  private_directory(directory);
  const auto path = directory / name;
  std::error_code error;
  const auto size = fs::exists(path) ? fs::file_size(path, error) : 0;
  require(!error && !fs::is_symlink(path), "invalid snapshot file");
  if (allow_retry && fs::exists(path) && offset <= size && data.size() <= size - offset) {
    std::ifstream input(path, std::ios::binary);
    input.seekg(static_cast<std::streamoff>(offset));
    std::string existing(data.size(), '\0');
    input.read(existing.data(), static_cast<std::streamsize>(existing.size()));
    require(input && existing == data, "retried snapshot chunk differs");
    return; // The preceding append succeeded, but its HTTP response was lost.
  }
  require(size == offset, "snapshot offset mismatch");
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
    std::int64_t vehicle_activity{created};
    bool vehicle_claimed{false};
    std::uint64_t vehicle_bytes{0};
    std::jthread worker;

    void fail_vehicle(std::string_view reason) {
      vehicle_manifest = missing(reason);
      for (const auto* name : {"runtime.log", "vcu-can.jsonl"}) {
        const auto path = temporary.path / "vehicle" / name;
        if (fs::is_regular_file(path) && !fs::is_symlink(path)) {
          vehicle_manifest["files"].push_back({{"name", name}, {"bytes", fs::file_size(path)},
              {"status", "partial"}, {"issues", {"incomplete_upload"}}});
        }
      }
    }
    void expire_vehicle(bool forced = false) {
      if (!vehicle_manifest.is_null()) return;
      if (forced || now_ms() - created > kExportTimeoutMs || now_ms() - vehicle_activity > kVehicleIdleTimeoutMs)
        fail_vehicle(vehicle_claimed ? "vehicle_upload_timeout" : "vehicle_worker_unresponsive");
    }
  };
  fs::path audit, catalog;
  TemporaryDirectory fallback;
  mutable std::mutex mutex;
  std::map<std::string, Json> records;
  struct Receipt { std::string id, vehicle; Json manifest; std::int64_t created; };
  std::deque<Receipt> released_exports;
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
    job.expire_vehicle();
    if (!job.cloud.is_null() && !job.vehicle_manifest.is_null() && now_ms() - job.created > kExportTimeoutMs)
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
  job.expire_vehicle(finish_partial);
  return {{"export_id", job.id}, {"session", job.session},
          {"ready", !job.cloud.is_null() && !job.vehicle_manifest.is_null()},
          {"cloud", job.cloud}, {"vehicle", job.vehicle_manifest},
          {"vehicle_progress", {{"claimed", job.vehicle_claimed}, {"bytes_received", job.vehicle_bytes}}}};
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
  // The controller can finish downloading before the vehicle retries a lost
  // finish response. Keep a bounded receipt while immediately freeing files.
  auto& receipts = impl_->released_exports;
  while (!receipts.empty() && (receipts.size() >= 16 || now_ms() - receipts.front().created > kVehicleIdleTimeoutMs))
    receipts.pop_front();
  receipts.push_back({job.id, job.vehicle, job.vehicle_manifest, now_ms()});
  impl_->jobs.erase(std::string(id));
}
Json SessionLogBroker::vehicle(std::string_view vehicle, const Json& request, bool idle) {
  std::lock_guard lock(impl_->mutex);
  const auto operation = request.at("operation").get<std::string>();
  if (operation == "poll") {
    if (idle) for (auto& [id, job] : impl_->jobs) {
      job->expire_vehicle();
      if (job->vehicle == vehicle && job->vehicle_manifest.is_null()) {
        if (!job->vehicle_claimed) { job->vehicle_claimed = true; job->vehicle_activity = now_ms(); }
        return {{"export_id", id}, {"session", job->session}};
      }
    }
    return Json::object();
  }
  const auto export_id = request.at("export_id").get<std::string>();
  const auto found = impl_->jobs.find(export_id);
  if (found == impl_->jobs.end() && operation == "finish") {
    for (const auto& receipt : impl_->released_exports)
      if (receipt.id == export_id && receipt.vehicle == vehicle &&
          now_ms() - receipt.created <= kVehicleIdleTimeoutMs && receipt.manifest == request.at("manifest"))
        return {{"accepted", true}};
  }
  require(found != impl_->jobs.end() && found->second->vehicle == vehicle, "export not found for vehicle");
  auto& job = *found->second;
  job.expire_vehicle();
  if (operation == "finish" && !job.vehicle_manifest.is_null() && job.vehicle_manifest == request.at("manifest"))
    return {{"accepted", true}};
  if (!job.vehicle_manifest.is_null() && job.vehicle_manifest.contains("reason")) return {{"cancelled", true}};
  require(job.vehicle_manifest.is_null(), "vehicle snapshot is already closed");
  if (!idle) { job.fail_vehicle("new_control_session_started"); return {{"cancelled", true}}; }
  if (operation == "failed") {
    const auto stage = request.at("stage").get<std::string>();
    require(stage == "collection" || stage == "upload" || stage == "finish", "invalid export failure stage");
    job.fail_vehicle("vehicle_" + stage + "_failed");
    return {{"accepted", true}};
  }
  if (operation == "append") {
    const auto& chunk = request.at("chunk");
    require(allowed_vehicle_file(chunk.at("name")), "unapproved vehicle log file");
    const auto path = job.temporary.path / "vehicle" / chunk.at("name").get<std::string>();
    const auto before = fs::exists(path) ? fs::file_size(path) : 0;
    append_session_log_chunk(job.temporary.path / "vehicle", chunk, true);
    job.vehicle_bytes += fs::file_size(path) - before;
    job.vehicle_activity = now_ms();
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
    HttpClient http(std::chrono::seconds(5), config.cloud.resolve_entries, config.cloud.ca_bundle);
    const auto url = normalize_signaling_http_url(config.cloud.signaling_url) + "/sessions/logs/vehicle";
    std::string last_attempt;
    std::int64_t last_poll_error_log_ms = 0;
    while (pause(stop, 3000)) {
      std::string id, stage = "poll";
      const auto call = [&](Json request) {
        request["vehicle_id"] = config.vehicle_id; request["device_token"] = token;
        for (int attempt = 0; ; ++attempt) {
          try { return http.post_json_response(url, request); }
          catch (const HttpStatusError& error) {
            if (error.status() != 429 && error.status() < 500) throw;
            if (attempt >= 2 || !pause(stop, 500 * (attempt + 1))) throw;
          } catch (const HttpTransportError&) {
            if (attempt >= 2 || !pause(stop, 500 * (attempt + 1))) throw;
          }
        }
      };
      try {
        const auto task = call({{"operation", "poll"}});
        if (!task.contains("export_id")) continue;
        id = task.at("export_id").get<std::string>();
        if (id == last_attempt) continue;
        stage = "collection";
        TemporaryDirectory snapshot;
        auto manifest = collect_session_logs({{runtime, "runtime.log"}, {can, "vcu-can.jsonl"}},
                                             task.at("session"), snapshot.path, {token}, stop);
        const bool recording_fence_complete = manifest.at("files").at(0).at("status") == "available";
        manifest["recording"] = collect_session_recording_evidence(config.recording.root_dir, task.at("session"), {token},
                                                                  snapshot.path / "runtime.log", recording_fence_complete);
        bool cancelled = false;
        stage = "upload";
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
        stage = "finish";
        if (!cancelled && !stop.stop_requested()) call({{"operation", "finish"}, {"export_id", id}, {"manifest", manifest}});
        last_attempt = id;
      } catch (const std::exception& error) {
        // Never print HTTP bodies: they can contain credentials. Report the
        // bounded stage and preserve any chunks already received by the cloud.
        const auto* http_error = dynamic_cast<const HttpStatusError*>(&error);
        const auto* transport_error = dynamic_cast<const HttpTransportError*>(&error);
        if (stage != "poll" || now_ms() - last_poll_error_log_ms >= 60000) {
          std::cerr << Json({{"event", "vehicle_log_export_failed"}, {"event_at_utc_ms", now_ms()},
                            {"stage", stage}, {"export_id", id},
                            {"http_status", http_error ? http_error->status() : 0},
                            {"issue_code", http_error ? "http_status_error" :
                                transport_error ? "http_transport_error" : "log_processing_failed"}}).dump() << '\n';
          if (stage == "poll") last_poll_error_log_ms = now_ms();
        }
        if (!id.empty() && !stop.stop_requested()) {
          try { call({{"operation", "failed"}, {"export_id", id}, {"stage", stage}}); last_attempt = id; }
          catch (const std::exception&) { last_attempt = id; /* Cloud progress timeout retains partial data. */ }
        }
      }
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
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kExportTimeoutMs + 5000);
      while (!stop.stop_requested()) {
        remote = call("status", {{"export_id", id}, {"finish_partial", std::chrono::steady_clock::now() >= deadline}});
        {
          std::lock_guard lock(impl_->mutex);
          impl_->progress["vehicle_progress"] = remote.value("vehicle_progress", Json::object());
        }
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
      const auto vehicle_recording = remote.at("vehicle").value("recording", Json());
      const auto recording = session_recording_evidence(vehicle_recording.is_object() ? vehicle_recording.value("summary", Json()) : Json(), session,
                                                       vehicle_recording.is_object() ? vehicle_recording.value("attempt_fence", Json()) : Json());
      // The downloaded snapshot now belongs to this controller. Free the
      // cloud slot so successive exports do not wait for retention expiry.
      static_cast<void>(call("release", {{"export_id", id}}));
      const Json manifest = {{"schema_version", 1}, {"session", session}, {"exported_at_utc_ms", now_ms()},
          {"complete", complete}, {"logs_complete", complete}, {"complete_scope", "logs_only"}, {"recording", recording},
          {"controller", controller}, {"cloud", remote.at("cloud")}, {"vehicle", remote.at("vehicle")},
          {"notes", "UTC window includes 2s before/after. CAN and raw runtime lines may be time-correlated rather than session-tagged. Credentials are redacted. Rotated-away, disabled or unavailable logs cannot be reconstructed; consult each file's issues. Videos and system journals are not included."}};
      write_json(root / "manifest.json", manifest);
      write_session_zip(root, impl_->temporary.path / "session.zip");
      std::lock_guard lock(impl_->mutex);
      impl_->progress = {{"state", "ready"}, {"complete", complete}, {"logs_complete", complete}, {"complete_scope", "logs_only"},
                        {"recording", recording}, {"session", session},
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
