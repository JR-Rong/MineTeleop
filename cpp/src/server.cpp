#include "mine_teleop/server.hpp"
#include "mine_teleop/control_logic_js.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <CommonCrypto/CommonHMAC.h>
#include <Security/Security.h>
#else
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace mine_teleop {
namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr int kSendFlags = 0;
constexpr int kShutdownBoth = SD_BOTH;

int last_socket_error() { return WSAGetLastError(); }
bool socket_error_interrupted(int error) { return error == WSAEINTR; }
bool socket_error_closed(int error) {
  return error == WSAENOTSOCK || error == WSAEINVAL;
}
std::string socket_error_message(int error) {
  return "Windows socket error " + std::to_string(error);
}
std::string address_error_message(int error) {
  return "Windows address resolution error " + std::to_string(error);
}
#else
using NativeSocket = int;
using SocketLength = socklen_t;
constexpr int kSendFlags = MSG_NOSIGNAL;
constexpr int kShutdownBoth = SHUT_RDWR;

int last_socket_error() { return errno; }
bool socket_error_interrupted(int error) { return error == EINTR; }
bool socket_error_closed(int error) { return error == EBADF || error == EINVAL; }
std::string socket_error_message(int error) { return std::strerror(error); }
std::string address_error_message(int error) { return ::gai_strerror(error); }
#endif

NativeSocket native_socket(SocketHandle socket) {
  return static_cast<NativeSocket>(socket);
}

int socket_buffer_size(std::size_t size) {
  return static_cast<int>(std::min<std::size_t>(
      size,
      static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

void close_socket(SocketHandle socket) {
  if (socket == kInvalidSocket) return;
#if defined(_WIN32)
  ::closesocket(native_socket(socket));
#else
  ::close(native_socket(socket));
#endif
}

void shutdown_socket(SocketHandle socket) {
  if (socket != kInvalidSocket) {
    ::shutdown(native_socket(socket), kShutdownBoth);
  }
}

int socket_inet_pton(int family, const char* source, void* destination) {
#if defined(_WIN32)
  return ::InetPtonA(family, source, destination);
#else
  return ::inet_pton(family, source, destination);
#endif
}

const char* socket_inet_ntop(
    int family,
    const void* source,
    char* destination,
    std::size_t destination_size) {
#if defined(_WIN32)
  return ::InetNtopA(
      family,
      const_cast<void*>(source),
      destination,
      static_cast<DWORD>(destination_size));
#else
  return ::inet_ntop(family, source, destination, destination_size);
#endif
}

void configure_listener_socket(SocketHandle socket) {
  int enabled = 1;
#if defined(_WIN32)
  ::setsockopt(
      native_socket(socket),
      SOL_SOCKET,
      SO_EXCLUSIVEADDRUSE,
      reinterpret_cast<const char*>(&enabled),
      sizeof(enabled));
#else
  ::setsockopt(native_socket(socket), SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled));
#endif
}

class Unauthorized final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class Conflict final : public std::runtime_error {
 public:
  explicit Conflict(std::string message, std::string issue_code = "conflict", Json details = Json::object())
      : std::runtime_error(std::move(message)),
        issue_code_(std::move(issue_code)),
        details_(std::move(details)) {}

  [[nodiscard]] const std::string& issue_code() const noexcept { return issue_code_; }
  [[nodiscard]] const Json& details() const noexcept { return details_; }

 private:
  std::string issue_code_;
  Json details_;
};

class NotFound final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class TooManyRequests final : public std::runtime_error {
 public:
  TooManyRequests(std::string message, std::int64_t retry_after_ms)
      : std::runtime_error(std::move(message)), retry_after_ms_(retry_after_ms) {}

  [[nodiscard]] std::int64_t retry_after_ms() const { return retry_after_ms_; }

 private:
  std::int64_t retry_after_ms_;
};

class SignalingRejected final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

thread_local std::string active_request_id;

class RequestIdScope final {
 public:
  explicit RequestIdScope(std::string request_id)
      : previous_(std::move(active_request_id)) {
    active_request_id = std::move(request_id);
  }

  ~RequestIdScope() { active_request_id = std::move(previous_); }

  RequestIdScope(const RequestIdScope&) = delete;
  RequestIdScope& operator=(const RequestIdScope&) = delete;

  [[nodiscard]] const std::string& value() const { return active_request_id; }

 private:
  std::string previous_;
};

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char value) {
    return static_cast<char>(std::tolower(value));
  });
  return value;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

std::optional<std::string> canonical_ip_address(std::string value) {
  value = trim(std::move(value));
  if (value.empty()) return std::nullopt;

  std::array<unsigned char, sizeof(in6_addr)> binary{};
  std::array<char, INET6_ADDRSTRLEN> text{};
  if (socket_inet_pton(AF_INET, value.c_str(), binary.data()) == 1) {
    if (socket_inet_ntop(AF_INET, binary.data(), text.data(), text.size()) == nullptr) return std::nullopt;
    return std::string(text.data());
  }
  if (socket_inet_pton(AF_INET6, value.c_str(), binary.data()) == 1) {
    if (socket_inet_ntop(AF_INET6, binary.data(), text.data(), text.size()) == nullptr) return std::nullopt;
    return std::string(text.data());
  }
  return std::nullopt;
}

std::string socket_peer_address(SocketHandle socket) {
  sockaddr_storage peer{};
  SocketLength peer_size = sizeof(peer);
  if (::getpeername(native_socket(socket), reinterpret_cast<sockaddr*>(&peer), &peer_size) != 0) {
    return "unknown";
  }

  std::array<char, INET6_ADDRSTRLEN> text{};
  const void* address = nullptr;
  if (peer.ss_family == AF_INET) {
    address = &reinterpret_cast<const sockaddr_in*>(&peer)->sin_addr;
  } else if (peer.ss_family == AF_INET6) {
    address = &reinterpret_cast<const sockaddr_in6*>(&peer)->sin6_addr;
  } else {
    return "unknown";
  }
  if (socket_inet_ntop(peer.ss_family, address, text.data(), text.size()) == nullptr) return "unknown";
  return std::string(text.data());
}

bool signaling_url_is_secure_or_loopback(std::string value) {
  value = lower(trim(std::move(value)));
  const auto scheme_end = value.find("://");
  if (scheme_end == std::string::npos) return false;
  const auto scheme = value.substr(0, scheme_end);
  if (scheme != "http" && scheme != "ws" && scheme != "https" && scheme != "wss") return false;
  const auto authority_start = scheme_end + 3;
  const auto authority_end = value.find_first_of("/?#", authority_start);
  auto authority = value.substr(
      authority_start,
      authority_end == std::string::npos ? std::string::npos : authority_end - authority_start);
  if (authority.empty() || authority.find('@') != std::string::npos) return false;
  std::string host;
  if (authority.front() == '[') {
    const auto close = authority.find(']');
    if (close == std::string::npos) return false;
    if (close + 1 < authority.size() && authority[close + 1] != ':') return false;
    host = authority.substr(1, close - 1);
  } else {
    const auto colon = authority.find(':');
    host = authority.substr(0, colon);
  }
  if (host.empty()) return false;
  if (scheme == "https" || scheme == "wss") return true;
  return host == "127.0.0.1" || host == "localhost" || host == "::1";
}

int hex_digit(char value) {
  if (value >= '0' && value <= '9') return value - '0';
  if (value >= 'a' && value <= 'f') return value - 'a' + 10;
  if (value >= 'A' && value <= 'F') return value - 'A' + 10;
  return -1;
}

std::string url_decode(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (std::size_t index = 0; index < value.size(); ++index) {
    if (value[index] == '+') {
      result.push_back(' ');
    } else if (value[index] == '%' && index + 2 < value.size()) {
      const int high = hex_digit(value[index + 1]);
      const int low = hex_digit(value[index + 2]);
      if (high < 0 || low < 0) throw std::invalid_argument("invalid URL encoding");
      result.push_back(static_cast<char>((high << 4) | low));
      index += 2;
    } else {
      result.push_back(value[index]);
    }
  }
  return result;
}

std::vector<std::string> path_parts(std::string_view path) {
  std::vector<std::string> result;
  std::size_t start = 0;
  while (start < path.size()) {
    while (start < path.size() && path[start] == '/') ++start;
    if (start >= path.size()) break;
    const auto end = path.find('/', start);
    result.push_back(url_decode(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start)));
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  return result;
}

std::string query_value(const HttpRequest& request, std::string_view key) {
  const auto found = request.query.find(std::string(key));
  return found == request.query.end() ? "" : found->second;
}

std::string credential_value(
    const HttpRequest& request,
    std::string_view query_key,
    std::string_view header_name) {
  const auto header = request.headers.find(std::string(header_name));
  if (header != request.headers.end() && !header->second.empty()) return header->second;
  return query_value(request, query_key);
}

std::string required_string(const Json& value, std::string_view key) {
  const std::string name(key);
  if (!value.contains(name) || !value.at(name).is_string() || value.at(name).get_ref<const std::string&>().empty()) {
    throw std::invalid_argument(std::string(key) + " must be a non-empty string");
  }
  return value.at(name).get<std::string>();
}

std::string optional_string(const Json& value, std::string_view key) {
  const std::string name(key);
  if (!value.contains(name) || value.at(name).is_null()) return {};
  if (!value.at(name).is_string()) throw std::invalid_argument(std::string(key) + " must be a string");
  return value.at(name).get<std::string>();
}

std::string required_yaml_string(const YAML::Node& node, std::string_view key, std::string_view context) {
  const std::string name(key);
  if (!node || !node.IsMap() || !node[name]) {
    throw std::invalid_argument(std::string(context) + "." + name + " is required");
  }
  try {
    auto value = node[name].as<std::string>();
    if (value.empty()) throw std::invalid_argument(std::string(context) + "." + name + " must not be empty");
    return value;
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument(std::string(context) + "." + name + " must be a string: " + error.what());
  }
}

std::optional<std::string> optional_yaml_string(
    const YAML::Node& node,
    std::string_view key,
    std::string_view context) {
  const std::string name(key);
  if (!node || !node.IsMap() || !node[name]) return std::nullopt;
  try {
    auto value = node[name].as<std::string>();
    if (value.empty()) throw std::invalid_argument(std::string(context) + "." + name + " must not be empty");
    return value;
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument(std::string(context) + "." + name + " must be a string: " + error.what());
  }
}

std::string read_identity_secret_file(const std::filesystem::path& path, std::string_view context) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + std::string(context) + " secret file: " + path.string());
  std::string value((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  while (!value.empty() && (value.back() == '\r' || value.back() == '\n')) value.pop_back();
  if (value.empty()) throw std::invalid_argument(std::string(context) + " secret must not be empty");
  return value;
}

std::string load_identity_secret(
    const YAML::Node& node,
    std::string_view file_key,
    std::string_view environment_key,
    const std::filesystem::path& base_path,
    std::string_view context) {
  const auto configured_file = optional_yaml_string(node, file_key, context);
  const auto configured_environment = optional_yaml_string(node, environment_key, context);
  if (configured_file.has_value() == configured_environment.has_value()) {
    throw std::invalid_argument(
        std::string(context) + " must configure exactly one of " + std::string(file_key) + " or " +
        std::string(environment_key));
  }
  if (configured_file.has_value()) {
    auto path = std::filesystem::path(*configured_file);
    if (path.is_relative()) path = base_path / path;
    return read_identity_secret_file(path.lexically_normal(), context);
  }
  const auto* value = std::getenv(configured_environment->c_str());
  if (value == nullptr || std::string_view(value).empty()) {
    throw std::runtime_error(
        std::string(context) + " environment variable is unset or empty: " + *configured_environment);
  }
  return value;
}

std::uint64_t required_uint64(const Json& value, std::string_view key) {
  const std::string name(key);
  if (!value.contains(name)) throw std::invalid_argument(name + " is required");
  if (value.at(name).is_number_unsigned()) return value.at(name).get<std::uint64_t>();
  if (value.at(name).is_number_integer()) {
    const auto parsed = value.at(name).get<std::int64_t>();
    if (parsed > 0) return static_cast<std::uint64_t>(parsed);
  }
  if (value.at(name).is_string()) {
    const auto text = value.at(name).get<std::string>();
    std::size_t consumed = 0;
    try {
      const auto parsed = std::stoull(text, &consumed);
      if (parsed > 0 && consumed == text.size()) return parsed;
    } catch (const std::exception&) {
    }
  }
  throw std::invalid_argument(name + " must be a positive integer");
}

std::int64_t required_int64(const Json& value, std::string_view key) {
  if (!value.contains(std::string(key)) || !value.at(std::string(key)).is_number_integer()) {
    throw std::invalid_argument(std::string(key) + " must be an integer");
  }
  return value.at(std::string(key)).get<std::int64_t>();
}

std::uint64_t required_nonnegative_uint64(const Json& value, std::string_view key) {
  const std::string name(key);
  if (!value.contains(name) || !value.at(name).is_number_integer()) {
    throw std::invalid_argument(name + " must be a non-negative integer");
  }
  if (value.at(name).is_number_unsigned()) return value.at(name).get<std::uint64_t>();
  const auto parsed = value.at(name).get<std::int64_t>();
  if (parsed < 0) throw std::invalid_argument(name + " must be a non-negative integer");
  return static_cast<std::uint64_t>(parsed);
}

double required_nonnegative_number(const Json& value, std::string_view key) {
  const std::string name(key);
  if (!value.contains(name) || !value.at(name).is_number()) {
    throw std::invalid_argument(name + " must be a non-negative number");
  }
  const auto parsed = value.at(name).get<double>();
  if (!std::isfinite(parsed) || parsed < 0.0) {
    throw std::invalid_argument(name + " must be a non-negative number");
  }
  return parsed;
}

std::int64_t control_lease_renew_at(std::int64_t now_ms, std::int64_t expires_at_ms) {
  if (expires_at_ms <= now_ms) return now_ms;
  return now_ms + (expires_at_ms - now_ms) / 3;
}

std::string base64_encode(const unsigned char* data, std::size_t size) {
  static constexpr std::string_view alphabet =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string encoded;
  encoded.reserve(4 * ((size + 2) / 3));
  for (std::size_t index = 0; index < size; index += 3) {
    const auto remaining = size - index;
    const std::uint32_t block =
        (static_cast<std::uint32_t>(data[index]) << 16U) |
        (remaining > 1 ? static_cast<std::uint32_t>(data[index + 1]) << 8U : 0U) |
        (remaining > 2 ? static_cast<std::uint32_t>(data[index + 2]) : 0U);
    encoded.push_back(alphabet[(block >> 18U) & 0x3fU]);
    encoded.push_back(alphabet[(block >> 12U) & 0x3fU]);
    encoded.push_back(remaining > 1 ? alphabet[(block >> 6U) & 0x3fU] : '=');
    encoded.push_back(remaining > 2 ? alphabet[block & 0x3fU] : '=');
  }
  return encoded;
}

bool sensitive_log_key(std::string key) {
  key = lower(std::move(key));
  return key.find("password") != std::string::npos || key.find("token") != std::string::npos ||
      key.find("secret") != std::string::npos || key.find("credential") != std::string::npos ||
      key.find("authorization") != std::string::npos || key.find("cookie") != std::string::npos ||
      key.find("private_key") != std::string::npos || key.find("api_key") != std::string::npos;
}

Json sanitize_log_value(const Json& value, int depth = 0) {
  if (depth > 6) return "[depth-limited]";
  if (value.is_object()) {
    Json sanitized = Json::object();
    std::size_t count = 0;
    for (auto iterator = value.begin(); iterator != value.end() && count < 64; ++iterator, ++count) {
      sanitized[iterator.key()] = sensitive_log_key(iterator.key())
          ? Json("[redacted]")
          : sanitize_log_value(iterator.value(), depth + 1);
    }
    return sanitized;
  }
  if (value.is_array()) {
    Json sanitized = Json::array();
    for (std::size_t index = 0; index < value.size() && index < 64; ++index) {
      sanitized.push_back(sanitize_log_value(value.at(index), depth + 1));
    }
    return sanitized;
  }
  if (value.is_string()) {
    auto text = value.get<std::string>();
    if (text.size() > 1024) text.resize(1024);
    return text;
  }
  return value;
}

void rotate_jsonl_log(const std::filesystem::path& path, std::uint64_t max_bytes, int file_count, std::size_t incoming_bytes) {
  std::error_code error;
  const auto current_size = std::filesystem::file_size(path, error);
  if (error || (current_size <= max_bytes && incoming_bytes <= max_bytes - current_size)) return;
  if (file_count <= 1) {
    std::filesystem::remove(path, error);
    if (error) throw std::runtime_error("cannot rotate JSONL log: " + error.message());
    return;
  }
  for (int index = file_count - 1; index >= 1; --index) {
    const auto source = index == 1 ? path : std::filesystem::path(path.string() + "." + std::to_string(index - 1));
    const auto destination = std::filesystem::path(path.string() + "." + std::to_string(index));
    std::filesystem::remove(destination, error);
    if (error) throw std::runtime_error("cannot remove expired JSONL log: " + error.message());
    if (!std::filesystem::exists(source, error)) {
      if (error) throw std::runtime_error("cannot inspect JSONL log: " + error.message());
      continue;
    }
    std::filesystem::rename(source, destination, error);
    if (error) throw std::runtime_error("cannot rotate JSONL log: " + error.message());
  }
}

std::int64_t log_period_start(std::int64_t timestamp_ms, std::int64_t interval_ms) {
  if (timestamp_ms < 0 || interval_ms <= 0) {
    throw std::invalid_argument("audit log timestamp and rotation interval must be positive");
  }
  return timestamp_ms - timestamp_ms % interval_ms;
}

std::string utc_log_period_key(std::int64_t period_start_ms) {
  const auto seconds = static_cast<std::time_t>(period_start_ms / 1000);
  std::tm value{};
#if defined(_WIN32)
  gmtime_s(&value, &seconds);
#else
  gmtime_r(&seconds, &value);
#endif
  std::ostringstream output;
  output << std::put_time(&value, "%Y%m%dT%H%M%SZ");
  return output.str();
}

std::filesystem::path audit_archive_path(
    const std::filesystem::path& active_path,
    std::string_view period_key,
    int part) {
  const auto stem = active_path.stem().string();
  const auto extension = active_path.extension().string();
  std::ostringstream filename;
  filename << stem << '.' << period_key << ".part" << std::setw(2) << std::setfill('0') << part
           << extension;
  return active_path.parent_path() / filename.str();
}

std::int64_t existing_log_period(
    const std::filesystem::path& path,
    std::int64_t interval_ms,
    std::int64_t fallback_period_ms) {
  std::error_code error;
  if (!std::filesystem::is_regular_file(path, error)) {
    if (error == std::errc::no_such_file_or_directory) return fallback_period_ms;
    if (error) throw std::runtime_error("cannot inspect signaling audit log: " + error.message());
    return fallback_period_ms;
  }
  const auto modified = std::filesystem::last_write_time(path, error);
  if (error) throw std::runtime_error("cannot inspect signaling audit log time: " + error.message());
  const auto system_modified =
      std::chrono::system_clock::now() +
      std::chrono::duration_cast<std::chrono::system_clock::duration>(
          modified - std::filesystem::file_time_type::clock::now());
  const auto modified_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               system_modified.time_since_epoch())
                               .count();
  return log_period_start(std::max<std::int64_t>(0, modified_ms), interval_ms);
}

void archive_jsonl_period(
    const std::filesystem::path& active_path,
    std::int64_t period_start_ms,
    int file_count) {
  const auto period_key = utc_log_period_key(period_start_ms);
  int archive_part = 0;
  for (int index = file_count - 1; index >= 0; --index) {
    const auto source = index == 0
        ? active_path
        : std::filesystem::path(active_path.string() + "." + std::to_string(index));
    std::error_code error;
    if (!std::filesystem::exists(source, error)) {
      if (error) throw std::runtime_error("cannot inspect signaling audit slice: " + error.message());
      continue;
    }
    auto destination = audit_archive_path(active_path, period_key, archive_part++);
    while (std::filesystem::exists(destination, error)) {
      if (error) throw std::runtime_error("cannot inspect signaling audit archive: " + error.message());
      destination = audit_archive_path(active_path, period_key, archive_part++);
    }
    std::filesystem::rename(source, destination, error);
    if (error) throw std::runtime_error("cannot archive signaling audit slice: " + error.message());
  }
}

void prune_jsonl_periods(
    const std::filesystem::path& active_path,
    std::int64_t current_period_start_ms,
    std::int64_t retention_days) {
  const auto retention_ms = retention_days * 24 * 60 * 60 * std::int64_t{1000};
  const auto cutoff_ms = std::max<std::int64_t>(0, current_period_start_ms - retention_ms);
  const auto cutoff_key = utc_log_period_key(cutoff_ms);
  const auto prefix = active_path.stem().string() + ".";
  const auto extension = active_path.extension().string();
  const auto parent = active_path.parent_path().empty()
      ? std::filesystem::path(".")
      : active_path.parent_path();
  std::error_code error;
  std::filesystem::directory_iterator entries(parent, error);
  if (error) throw std::runtime_error("cannot inspect signaling audit retention: " + error.message());
  for (const auto& entry : entries) {
    if (!entry.is_regular_file(error)) {
      if (error) throw std::runtime_error("cannot inspect signaling audit archive: " + error.message());
      continue;
    }
    const auto filename = entry.path().filename().string();
    if (!filename.starts_with(prefix) || !filename.ends_with(extension)) continue;
    const auto period = filename.substr(prefix.size(), 16);
    if (period.size() != 16 || period[8] != 'T' || period[15] != 'Z') continue;
    if (period >= cutoff_key) continue;
    std::filesystem::remove(entry.path(), error);
    if (error) throw std::runtime_error("cannot remove expired signaling audit slice: " + error.message());
  }
}

std::string turn_rest_credential(std::string_view secret, std::string_view username) {
#if defined(__APPLE__)
  std::array<unsigned char, CC_SHA1_DIGEST_LENGTH> digest{};
  CCHmac(
      kCCHmacAlgSHA1,
      secret.data(),
      secret.size(),
      username.data(),
      username.size(),
      digest.data());
  return base64_encode(digest.data(), digest.size());
#else
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (HMAC(
          EVP_sha1(),
          secret.data(),
          static_cast<int>(secret.size()),
          reinterpret_cast<const unsigned char*>(username.data()),
          username.size(),
          digest.data(),
          &digest_size) == nullptr) {
    throw std::runtime_error("TURN REST HMAC-SHA1 failed");
  }
  return base64_encode(digest.data(), digest_size);
#endif
}

bool valid_ice_url(std::string_view value, bool turn) {
  if (turn) return value.starts_with("turn:") || value.starts_with("turns:");
  return value.starts_with("stun:") || value.starts_with("stuns:");
}

std::string message_key(std::string_view session_id, std::string_view recipient) {
  return std::string(session_id) + "\x1f" + std::string(recipient);
}

std::string status_reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    default: return "Response";
  }
}

void send_all(SocketHandle socket, std::string_view value) {
  std::size_t sent = 0;
  while (sent < value.size()) {
    const auto result = ::send(
        native_socket(socket),
        value.data() + sent,
        socket_buffer_size(value.size() - sent),
        kSendFlags);
    if (result < 0) {
      const int error = last_socket_error();
      if (socket_error_interrupted(error)) continue;
      throw std::runtime_error("send failed: " + socket_error_message(error));
    }
    if (result == 0) throw std::runtime_error("connection closed while sending response");
    sent += static_cast<std::size_t>(result);
  }
}

void send_http_response(SocketHandle socket, const ServerResponse& response) {
  std::ostringstream header;
  header << "HTTP/1.1 " << response.status << ' ' << status_reason(response.status) << "\r\n"
         << "Content-Type: " << response.content_type << "\r\n"
         << "Content-Length: " << response.body.size() << "\r\n"
         << "Connection: close\r\n";
  for (const auto& [name, value] : response.headers) header << name << ": " << value << "\r\n";
  header << "\r\n";
  send_all(socket, header.str());
  send_all(socket, response.body);
}

ServerResponse too_many_requests_response(const TooManyRequests& error) {
  auto response = ServerResponse::json(
      429,
      {{"error", error.what()}, {"retry_after_ms", error.retry_after_ms()}});
  const auto retry_after_seconds = error.retry_after_ms() / 1000 +
      (error.retry_after_ms() % 1000 == 0 ? 0 : 1);
  response.headers.emplace_back(
      "Retry-After",
      std::to_string(std::max<std::int64_t>(1, retry_after_seconds)));
  response.headers.emplace_back("Cache-Control", "no-store");
  return response;
}

void add_request_id_header(ServerResponse& response, std::string_view request_id) {
  response.headers.emplace_back("X-Request-ID", request_id);
}

HttpRequest parse_request(SocketHandle socket, std::size_t max_body_bytes) {
  constexpr std::size_t max_headers = 64 * 1024;
  std::string wire;
  std::array<char, 16 * 1024> buffer{};
  std::size_t header_end = std::string::npos;
  while ((header_end = wire.find("\r\n\r\n")) == std::string::npos) {
    const auto received = ::recv(
        native_socket(socket), buffer.data(), socket_buffer_size(buffer.size()), 0);
    if (received < 0) {
      const int error = last_socket_error();
      if (socket_error_interrupted(error)) continue;
      throw std::runtime_error("recv failed: " + socket_error_message(error));
    }
    if (received == 0) throw std::invalid_argument("client closed before sending HTTP headers");
    wire.append(buffer.data(), static_cast<std::size_t>(received));
    if (wire.size() > max_headers) throw std::invalid_argument("HTTP headers too large");
  }

  std::istringstream headers(wire.substr(0, header_end));
  HttpRequest request;
  std::string request_line;
  if (!std::getline(headers, request_line)) throw std::invalid_argument("missing HTTP request line");
  request_line = trim(std::move(request_line));
  std::istringstream line(request_line);
  std::string version;
  if (!(line >> request.method >> request.target >> version) || !version.starts_with("HTTP/1.")) {
    throw std::invalid_argument("invalid HTTP request line");
  }
  std::string header;
  while (std::getline(headers, header)) {
    header = trim(std::move(header));
    if (header.empty()) continue;
    const auto separator = header.find(':');
    if (separator == std::string::npos) throw std::invalid_argument("invalid HTTP header");
    request.headers[lower(trim(header.substr(0, separator)))] = trim(header.substr(separator + 1));
  }

  std::size_t content_length = 0;
  if (const auto found = request.headers.find("content-length"); found != request.headers.end()) {
    std::size_t consumed = 0;
    try {
      content_length = std::stoull(found->second, &consumed);
    } catch (const std::exception&) {
      throw std::invalid_argument("invalid Content-Length header");
    }
    if (consumed != found->second.size()) throw std::invalid_argument("invalid Content-Length header");
  }
  if (content_length > max_body_bytes) throw std::length_error("request body too large");
  const auto body_start = header_end + 4;
  while (wire.size() - body_start < content_length) {
    const auto received = ::recv(
        native_socket(socket), buffer.data(), socket_buffer_size(buffer.size()), 0);
    if (received < 0) {
      const int error = last_socket_error();
      if (socket_error_interrupted(error)) continue;
      throw std::runtime_error("recv failed: " + socket_error_message(error));
    }
    if (received == 0) throw std::invalid_argument("client closed before sending HTTP body");
    wire.append(buffer.data(), static_cast<std::size_t>(received));
  }
  request.body = wire.substr(body_start, content_length);

  const auto question = request.target.find('?');
  request.path = url_decode(request.target.substr(0, question));
  if (question != std::string::npos) {
    const std::string_view query(request.target.data() + question + 1, request.target.size() - question - 1);
    std::size_t start = 0;
    while (start <= query.size()) {
      const auto end = query.find('&', start);
      const auto item = query.substr(start, end == std::string_view::npos ? query.size() - start : end - start);
      if (!item.empty()) {
        const auto equal = item.find('=');
        request.query[url_decode(item.substr(0, equal))] = equal == std::string_view::npos ? "" : url_decode(item.substr(equal + 1));
      }
      if (end == std::string_view::npos) break;
      start = end + 1;
    }
  }
  return request;
}

std::string console_html(const DriverConfig& config) {
  const Json page_config = {
      {"rate_hz", config.rate_hz},
      {"estop_hold_ms", config.estop_hold_ms},
      {"max_time_sync_uncertainty_ms", config.max_time_sync_uncertainty_ms},
      {"ice_transport_policy", config.ice_transport_policy},
      {"control_limits",
       {
           {"initial_max_throttle", config.control_limits.initial_max_throttle},
           {"initial_service_brake", config.control_limits.initial_service_brake},
           {"initial_hard_brake", config.control_limits.initial_hard_brake},
           {"initial_max_steering_angle_deg",
            config.control_limits.initial_max_steering_angle_deg},
           {"steering_full_scale_deg", 30.0},
       }},
      {"gamepad",
       {
           {"enabled", config.gamepad.enabled},
           {"steering_axis", config.gamepad.steering_axis},
           {"throttle_axis", config.gamepad.throttle_axis},
           {"brake_axis", config.gamepad.brake_axis},
           {"axis_deadzone", config.gamepad.axis_deadzone},
           {"steering_inverted", config.gamepad.steering_inverted},
           {"throttle_inverted", config.gamepad.throttle_inverted},
           {"brake_inverted", config.gamepad.brake_inverted},
           {"steering_center", config.gamepad.steering_center},
           {"steering_range", config.gamepad.steering_range},
           {"throttle_rest", config.gamepad.throttle_rest},
           {"throttle_range", config.gamepad.throttle_range},
           {"brake_rest", config.gamepad.brake_rest},
           {"brake_range", config.gamepad.brake_range},
           {"estop_button", config.gamepad.estop_button},
       }},
  };
  return R"HTML(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Mine Teleop WebRTC Console</title><link rel="icon" href="data:," type="image/x-icon"><style>
:root{color-scheme:dark;--bg:#08101d;--surface:#0f1929;--surface-2:#111f32;--line:#26364d;--text:#e7edf6;--muted:#8ea0b8;--accent:#38bdf8;--ok:#34d399;--warn:#fbbf24;--critical:#fb7185}
body .app-shell{grid-template-rows:auto auto minmax(0,1fr)}
.operator-status-strip{display:grid;grid-template-columns:minmax(170px,1.6fr) repeat(5,minmax(72px,1fr));gap:1px;overflow:hidden;border:1px solid var(--line);border-radius:8px;background:var(--line);position:relative;z-index:3}
.operator-status-item{background:var(--surface-2);min-width:0;padding:6px 8px}
.operator-status-item span{display:block;color:var(--muted);font-size:9px;margin-bottom:2px}
.operator-status-item strong{display:block;font:700 16px/1.15 ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.operator-status-item.active strong{color:var(--accent)}
.visual-stage>#estop-status{grid-row:1}.visual-stage>#cameras{grid-row:2}.visual-stage>#can-feedback-panel{grid-row:3}.visual-stage>#status{grid-row:4}
.visual-stage>.grid{overflow:hidden}
.visual-stage .camera>video{position:absolute;inset:0;width:100%;height:100%;object-fit:contain}
.can-feedback-panel .can-summary{grid-template-columns:repeat(5,minmax(0,1fr))}
@media(max-height:760px){.operator-status-item{padding:4px 6px}.operator-status-item span{font-size:8px}.operator-status-item strong{font-size:14px}}
@media(max-width:900px){body .operator-status-strip{grid-template-columns:repeat(3,minmax(0,1fr))}}
@media(max-width:720px){body .operator-status-strip{position:sticky;top:0;z-index:20;grid-template-columns:repeat(3,minmax(0,1fr))}.workspace>.visual-stage{min-height:0}.visual-stage>.grid{grid-template-columns:1fr;grid-template-rows:none;grid-auto-rows:clamp(190px,56vw,300px);overflow:visible}.can-feedback-panel .can-summary{grid-template-columns:repeat(2,minmax(0,1fr))}}
*{box-sizing:border-box}html,body{height:100%;overflow:hidden}body{background:var(--bg);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;padding:12px}button,input,select{font:inherit}button,input,select{border:1px solid var(--line);border-radius:7px;background:#0b1422;color:var(--text);padding:8px 10px}button{cursor:pointer}button:hover{border-color:#58708e}button:focus-visible,input:focus-visible,select:focus-visible,main:focus-visible{outline:2px solid var(--accent);outline-offset:2px}.danger{background:#be123c;border-color:#fb7185;color:#fff}.app-shell{height:calc(100svh - 24px);min-height:0;display:grid;grid-template-rows:auto minmax(0,1fr);gap:10px;max-width:1920px;margin:auto}.topbar{display:flex;align-items:center;justify-content:space-between;gap:16px;min-width:0}.brand{display:flex;align-items:baseline;gap:12px;min-width:max-content}.brand h1{font-size:21px;line-height:1.1;margin:0;letter-spacing:-.02em}.brand p{font-size:12px;color:var(--muted);margin:0}.auth{display:flex;align-items:center;justify-content:flex-end;gap:7px;flex-wrap:wrap;margin:0;min-width:0}.auth label{font-size:12px;color:var(--muted)}.auth input{width:170px}.auth select{max-width:220px}.auth strong{font-size:12px;color:var(--ok)}.workspace{display:grid;grid-template-columns:minmax(0,1fr) clamp(300px,25vw,390px);gap:10px;min-height:0}.visual-stage{display:grid;grid-template-rows:auto minmax(0,1fr) auto auto;gap:8px;min-width:0;min-height:0}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));grid-template-rows:repeat(2,minmax(0,1fr));gap:8px;min-height:0}.camera{background:#020617;border:1px solid #162338;border-radius:8px;overflow:hidden;position:relative;min-width:0;min-height:0}.camera video{display:block;width:100%;height:100%;object-fit:contain}.label{position:absolute;left:7px;top:7px;background:#020617cc;padding:3px 7px;border-radius:5px;font-size:11px;z-index:2}.empty-stage{grid-column:1/-1;grid-row:1/-1;display:grid;place-items:center;border:1px dashed var(--line);border-radius:8px;color:var(--muted);font-size:13px}.can-feedback-panel{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:7px;min-width:0}.can-feedback-heading{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:5px}.can-feedback-heading h2{font-size:11px;letter-spacing:.05em;text-transform:uppercase;margin:0}.can-summary{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:5px;overflow:hidden;margin-bottom:5px}.can-summary-item{background:var(--surface-2);padding:3px 5px;min-width:0}.can-summary-item span{display:block;color:var(--muted);font-size:8px}.can-summary-item strong{display:block;font:700 10px/1.25 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.can-feedback-body{display:grid;grid-template-columns:minmax(0,4fr) minmax(150px,1fr);gap:5px}.wheel-feedback-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:3px}.wheel-feedback,.steering-feedback{background:#0b1422;border:1px solid #1f3047;border-radius:5px;padding:3px 5px;min-width:0}.feedback-title{display:flex;justify-content:space-between;gap:4px;color:#cbd5e1;font-size:8px}.feedback-values{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:3px;margin-top:2px}.feedback-value{min-width:0}.feedback-value span{display:block;color:var(--muted);font-size:7px}.feedback-value strong{display:block;font:700 9px/1.2 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.steering-feedback-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:3px}.steering-feedback .feedback-values{grid-template-columns:minmax(0,1fr) auto}.sidebar{min-height:0;overflow:auto;display:flex;flex-direction:column;gap:10px;padding-right:2px}.side-section{background:var(--surface);border:1px solid var(--line);border-radius:9px;padding:10px}.section-heading{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:8px}.section-heading h2{font-size:13px;letter-spacing:.04em;text-transform:uppercase;margin:0}.status-chip{font-size:11px;color:var(--muted)}.key-help,.gate-copy{font-size:11px;color:var(--muted);margin:0 0 8px;line-height:1.35}.keyboard-grid{display:grid;grid-template-columns:repeat(3,46px);grid-template-rows:repeat(2,40px);justify-content:center;gap:5px;margin:8px 0}.keycap{display:flex;align-items:center;justify-content:center;gap:4px;border:1px solid #41536d;border-radius:7px;background:#0a1321;color:#aebdd0;font:700 15px/1 ui-monospace,SFMono-Regular,Menlo,monospace;transition:background .08s ease,border-color .08s ease,color .08s ease,transform .08s ease}.keycap small{font-size:9px;color:#71839b}.keycap.active{background:#075985;border-color:var(--accent);color:#fff;transform:translateY(1px);box-shadow:0 0 0 2px #38bdf826}.keycap.active small{color:#bae6fd}.key-up{grid-column:2}.key-left{grid-column:1;grid-row:2}.key-down{grid-column:2;grid-row:2}.key-right{grid-column:3;grid-row:2}.brake-keys{display:grid;grid-template-columns:1fr 1fr;gap:5px;margin:7px 0 10px}.brake-key{height:30px;font-size:11px}.last-input{display:block;text-align:center;min-height:17px;font-size:11px;color:var(--accent)}.control-readouts{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:7px;overflow:hidden}.control-readout{background:var(--surface-2);padding:8px}.control-readout span{display:block;font-size:10px;color:var(--muted);margin-bottom:3px}.control-readout strong{display:block;font:700 18px/1.1 ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums}.control-readout.active strong{color:var(--accent)}.side-actions{display:grid;grid-template-columns:1fr 1fr;gap:6px}.side-actions button{font-size:11px;padding:7px 6px}.limit-inline{display:flex;align-items:center;justify-content:space-between;gap:8px;margin:0 0 7px;color:var(--muted);font-size:10px}.side-section .limit-inline strong{font-size:11px}.metrics{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:7px;overflow:hidden}.metric{background:var(--surface-2);padding:7px}.metric span{display:block;color:var(--muted);font-size:9px;margin-bottom:3px}.metric strong{display:block;font-size:12px;line-height:1.25;overflow-wrap:anywhere}.ok{color:var(--ok)}.warn{color:var(--warn)}.critical{color:var(--critical)}.alerts{border-left:3px solid #475569;background:#0b1422;padding:7px 8px;margin:8px 0;font-size:11px;line-height:1.35}.alerts.warn{border-color:#f59e0b}.alerts.critical{border-color:#e11d48}table{width:100%;border-collapse:collapse;background:#0b1422;font-size:9px;table-layout:fixed}th,td{text-align:left;padding:5px 3px;border-bottom:1px solid #26364d;font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}th:first-child,td:first-child{width:30%}.estop-banner{border:1px solid #fb7185;background:#881337;color:#fff;border-radius:7px;padding:7px 10px;margin:0;font-size:12px}.status-line{background:#020617;border:1px solid #162338;border-radius:7px;color:#9fb0c6;font:11px/1.35 ui-monospace,SFMono-Regular,Menlo,monospace;margin:0;padding:7px 9px;min-height:31px;max-height:48px;overflow:auto;white-space:pre-wrap}.muted{color:var(--muted)}[hidden]{display:none!important}dialog{color:var(--text);background:var(--surface);border:1px solid #4b5563;border-radius:10px;max-width:520px;width:calc(100% - 40px);padding:18px}dialog::backdrop{background:#000a}dialog h2{margin:0 0 12px;font-size:18px}.limit-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.limit-grid label{display:flex;flex-direction:column;gap:5px;font-size:12px}.limit-warning{border-left:4px solid #f59e0b;padding:8px 10px;background:#1f2937;font-size:12px;line-height:1.45}
@media(max-height:760px){body{padding:8px}.app-shell{height:calc(100svh - 16px);gap:7px}.brand p{display:none}.sidebar{gap:7px}.side-section{padding:7px}.section-heading{margin-bottom:5px}.key-help,.gate-copy{margin-bottom:4px;line-height:1.25}.keyboard-grid{grid-template-columns:repeat(3,40px);grid-template-rows:repeat(2,30px);margin:3px 0}.brake-keys{margin:3px 0}.brake-key{height:26px}.last-input{min-height:14px;font-size:10px}.control-readout{padding:4px 6px}.control-readout span{font-size:9px;margin-bottom:2px}.control-readout strong{font-size:16px}.side-actions button{padding:5px 4px}.limit-inline{margin-bottom:4px}.metric{padding:3px 5px}.metric span{margin-bottom:2px}.metric strong{font-size:11px}.alerts{margin:4px 0;padding:4px 6px;font-size:10px;line-height:1.25}th,td{padding:3px 2px}}
@media(max-width:900px){.workspace{grid-template-columns:minmax(0,1fr) 290px}.brand p{display:none}.auth label,.auth-expiry{display:none}}
@media(max-width:720px){html,body{overflow:auto}.app-shell{height:auto;min-height:100svh}.topbar{align-items:flex-start;flex-direction:column}.auth{justify-content:flex-start}.workspace{grid-template-columns:1fr}.visual-stage{min-height:58svh}.sidebar{overflow:visible}.grid{grid-template-columns:1fr}.can-summary{grid-template-columns:repeat(2,minmax(0,1fr))}.can-feedback-body{grid-template-columns:1fr}.wheel-feedback-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.steering-feedback-grid{grid-template-columns:repeat(4,minmax(0,1fr))}.limit-grid{grid-template-columns:1fr}}</style></head><body><main class="app-shell" tabindex="-1">
<header class="topbar"><div class="brand"><h1>Mine Teleop 控制台</h1><p>方向键 / WASD 控制 · Space 缓刹 · B 急刹 · E 急停</p></div>
<section id="login-panel" class="auth"><label for="password">驾驶员密码</label><input id="password" type="password" autocomplete="current-password"><button id="login">登录并加载车辆</button></section>
<section id="session-panel" class="auth" hidden><label for="vehicle">授权车辆</label><select id="vehicle"></select><button id="connect">连接所选车辆</button><button id="control-limits-open">实车调试限幅</button><button id="logout">安全退出</button><button id="estop" class="danger">急停</button><strong id="webrtc">未连接</strong><span id="auth-expiry" class="muted auth-expiry"></span></section></header>
<section id="operator-status-strip" class="operator-status-strip" aria-label="关键车辆状态"><article class="operator-status-item"><span>实测车速</span><strong id="operator-speed">—</strong></article><article class="operator-status-item"><span>实际挡位</span><strong id="operator-actual-gear">—</strong></article><article id="operator-readout-gear" class="operator-status-item"><span>指令挡位</span><strong id="operator-control-gear">N</strong></article><article id="operator-readout-steering" class="operator-status-item"><span>转向</span><strong id="operator-control-steering">0.00</strong></article><article id="operator-readout-throttle" class="operator-status-item"><span>目标车速比例</span><strong id="operator-control-throttle">0.00</strong></article><article id="operator-readout-brake" class="operator-status-item"><span>刹车</span><strong id="operator-control-brake">0.00</strong></article></section>
<div class="workspace"><section class="visual-stage"><p id="estop-status" class="estop-banner" hidden>急停请求已锁定；等待车端遥测确认，未确认时请使用车辆物理急停。</p><section id="cameras" class="grid"><div id="empty-stage" class="empty-stage">登录并连接车辆后显示实时视频</div></section><section id="can-feedback-panel" class="can-feedback-panel" hidden aria-label="CAN 实时反馈"><div class="can-feedback-heading"><h2>CAN 实时反馈 · 测量值</h2><strong id="can-feedback-status" class="status-chip warn">等待车端遥测</strong></div><div class="can-summary"><div class="can-summary-item"><span>车速</span><strong id="can-speed">—</strong></div><div class="can-summary-item"><span>实际挡位</span><strong id="can-gear">—</strong></div><div class="can-summary-item"><span>物理选择器</span><strong id="can-selector">—</strong></div><div class="can-summary-item"><span>电子驻车 1-4</span><strong id="can-epb">—</strong></div><div class="can-summary-item"><span>VCU 状态</span><strong id="can-handshake">—</strong></div><div class="can-summary-item"><span>紧急开关</span><strong id="can-emergency">—</strong></div><div class="can-summary-item"><span>反馈时效</span><strong id="can-age">—</strong></div></div><div class="can-feedback-body"><div id="wheel-feedback-grid" class="wheel-feedback-grid" aria-label="八路轮端反馈"></div><div id="steering-feedback-grid" class="steering-feedback-grid" aria-label="四路转向反馈"></div></div></section><pre id="status" class="status-line">请先登录</pre></section>
<aside class="sidebar"><section id="keyboard-panel" class="side-section" aria-label="键盘控制状态"><div class="section-heading"><h2>键盘控制</h2><span id="input-readiness" class="status-chip">等待连接</span></div><p class="key-help">方向键与 WASD 等效；D/R 在会话内锁存，松开前进/倒车只将目标车速比例归零。</p><div class="keyboard-grid" aria-label="方向键状态"><span id="key-up" class="keycap key-up" aria-pressed="false">↑<small>W</small></span><span id="key-left" class="keycap key-left" aria-pressed="false">←<small>A</small></span><span id="key-down" class="keycap key-down" aria-pressed="false">↓<small>S</small></span><span id="key-right" class="keycap key-right" aria-pressed="false">→<small>D</small></span></div><div class="brake-keys"><span id="key-service-brake" class="keycap brake-key" aria-pressed="false">SPACE · 缓刹</span><span id="key-hard-brake" class="keycap brake-key" aria-pressed="false">B · 急刹</span></div><span id="last-keyboard-event" class="last-input">等待键盘输入</span><div class="control-readouts"><div id="readout-gear" class="control-readout"><span>挡位</span><strong id="control-gear">N</strong></div><div id="readout-steering" class="control-readout"><span>转向</span><strong id="control-steering">0.00</strong></div><div id="readout-throttle" class="control-readout"><span>目标车速比例</span><strong id="control-throttle">0.00</strong></div><div id="readout-brake" class="control-readout"><span>刹车</span><strong id="control-brake">0.00</strong></div></div></section>
<section id="vcu-panel" class="side-section" hidden><div class="section-heading"><h2>VCU 平行驾驶</h2><strong id="vcu-status" class="status-chip warn">等待车端 VCU 状态</strong></div><p class="limit-inline"><span>当前会话限幅</span><strong id="control-limits-summary" class="warn">目标车速比例 ≤5% · 转向 ≤3.0°</strong></p><p id="vcu-gate" class="gate-copy warn" role="status" aria-live="polite">准入检查：需要 N 挡、电子驻车已拉起、车辆零速及 VCU 人工状态</p><div class="side-actions"><button id="vcu-connect" disabled>开始平行驾驶握手</button><button id="vcu-disconnect" disabled>断开 VCU 握手</button></div></section>
<section id="monitor-panel" class="side-section" hidden><div class="section-heading"><h2>运行监控</h2><span class="status-chip">实时</span></div><div class="metrics"><article class="metric"><span>车辆在线</span><strong id="metric-vehicle">未知</strong></article><article class="metric"><span>当前会话</span><strong id="metric-session">未连接</strong></article><article class="metric"><span>当前控制权</span><strong id="metric-authority">无</strong></article><article class="metric"><span>视频编码 / 后端</span><strong id="metric-video">等待媒体</strong></article><article class="metric"><span>控制 RTT</span><strong id="metric-rtt">未知</strong></article><article class="metric"><span>网络连接</span><strong id="metric-network">未知</strong></article><article class="metric"><span>TURN</span><strong id="metric-turn">未配置</strong></article><article class="metric"><span>时间同步</span><strong id="metric-time">未知</strong></article></div><div id="alerts" class="alerts">尚无媒体指标；控制命令不会在链路未就绪时发送。</div><table><thead><tr><th>camera</th><th>FPS</th><th>kbps</th><th>loss</th><th>latency</th></tr></thead><tbody id="stream-metrics"><tr><td colspan="5" class="muted">等待视频轨道</td></tr></tbody></table></section></aside></div>
<dialog id="control-limits-dialog"><h2>实车调试限幅</h2><p class="limit-warning">这是控制端普通驾驶会话限幅；车端 <code>field_safety</code> 硬上限仍会再次截断。缓刹必须不大于急刹。急停、超时、故障和断开停车不受人工刹车限幅削弱。修改时立即清零当前输入。</p><div class="limit-grid"><label for="max-throttle-percent">最大目标车速比例（%）<input id="max-throttle-percent" type="number" min="0" max="100" step="1" inputmode="decimal"></label><label for="service-brake-percent">缓刹请求（%）<input id="service-brake-percent" type="number" min="0" max="100" step="1" inputmode="decimal"></label><label for="hard-brake-percent">急刹请求（%）<input id="hard-brake-percent" type="number" min="0" max="100" step="1" inputmode="decimal"></label><label for="max-steering-deg">最大四轴转向角（°）<input id="max-steering-deg" type="number" min="0" max="30" step="0.5" inputmode="decimal"></label></div><p id="vehicle-hard-limits" class="muted">等待车端硬上限</p><label><input id="control-limits-confirm" type="checkbox">我已确认车辆处于隔离台架，并理解四轴当前使用同方向转角</label><div><button id="control-limits-apply" disabled>应用限幅</button><button id="control-limits-cancel">取消</button></div></dialog></main><script>)HTML" + std::string(web::kControlLogicJavaScript) + R"HTML(</script><script>
const controlLogic=MineTeleopControlLogic;
const consoleConfig=)HTML" + page_config.dump() + R"HTML(;
const gamepadConfig=consoleConfig.gamepad;
const limitConfig=consoleConfig.control_limits;
const keys=controlLogic.KEY_BINDINGS;
let state=controlLogic.deriveKeyState(controlLogic.createKeySet());
const pressedControlKeys=controlLogic.createKeySet(),blockedControlKeys=controlLogic.createKeySet();
const gamepadState={connected:false,steering:0,throttle:0,brake:0};
const controlLimits={maxThrottle:limitConfig.initial_max_throttle,serviceBrake:limitConfig.initial_service_brake,hardBrake:limitConfig.initial_hard_brake,maxSteeringDeg:limitConfig.initial_max_steering_angle_deg};
let vehicleHardLimits={max_throttle:1,max_brake:1,max_steering_angle_deg:limitConfig.steering_full_scale_deg,max_speed_kph:null,received:false};
const calibration={steeringCenter:gamepadConfig.steering_center,steeringRange:gamepadConfig.steering_range,throttleRest:gamepadConfig.throttle_rest,throttleRange:gamepadConfig.throttle_range,brakeRest:gamepadConfig.brake_rest,brakeRange:gamepadConfig.brake_range};
const webrtcLabel=document.getElementById('webrtc'),cameraGrid=document.getElementById('cameras'),statusPanel=document.getElementById('status'),loginPanel=document.getElementById('login-panel'),sessionPanel=document.getElementById('session-panel'),passwordInput=document.getElementById('password'),vehicleSelect=document.getElementById('vehicle'),connectButton=document.getElementById('connect'),authExpiry=document.getElementById('auth-expiry'),vcuPanel=document.getElementById('vcu-panel'),vcuStatus=document.getElementById('vcu-status'),vcuGate=document.getElementById('vcu-gate'),vcuConnectButton=document.getElementById('vcu-connect'),vcuDisconnectButton=document.getElementById('vcu-disconnect'),controlLimitsOpen=document.getElementById('control-limits-open'),controlLimitsSummary=document.getElementById('control-limits-summary'),controlLimitsDialog=document.getElementById('control-limits-dialog'),maxThrottlePercent=document.getElementById('max-throttle-percent'),serviceBrakePercent=document.getElementById('service-brake-percent'),hardBrakePercent=document.getElementById('hard-brake-percent'),maxSteeringDeg=document.getElementById('max-steering-deg'),vehicleHardLimitsLabel=document.getElementById('vehicle-hard-limits'),controlLimitsConfirm=document.getElementById('control-limits-confirm'),controlLimitsApply=document.getElementById('control-limits-apply'),controlLimitsCancel=document.getElementById('control-limits-cancel'),estopStatus=document.getElementById('estop-status'),monitorPanel=document.getElementById('monitor-panel'),alertsPanel=document.getElementById('alerts'),streamMetrics=document.getElementById('stream-metrics'),inputReadiness=document.getElementById('input-readiness'),lastKeyboardEvent=document.getElementById('last-keyboard-event'),emptyStage=document.getElementById('empty-stage');
const canFeedbackPanel=document.getElementById('can-feedback-panel'),canFeedbackStatus=document.getElementById('can-feedback-status'),canSpeed=document.getElementById('can-speed'),canGear=document.getElementById('can-gear'),canSelector=document.getElementById('can-selector'),canEpb=document.getElementById('can-epb'),canHandshake=document.getElementById('can-handshake'),canEmergency=document.getElementById('can-emergency'),canAge=document.getElementById('can-age'),wheelFeedbackGrid=document.getElementById('wheel-feedback-grid'),steeringFeedbackGrid=document.getElementById('steering-feedback-grid');
const operatorSpeed=document.getElementById('operator-speed'),operatorActualGear=document.getElementById('operator-actual-gear');
const keyIndicators={left:document.getElementById('key-left'),right:document.getElementById('key-right'),up:document.getElementById('key-up'),down:document.getElementById('key-down'),service_brake:document.getElementById('key-service-brake'),hard_brake:document.getElementById('key-hard-brake')};
const controlReadouts={gear:document.getElementById('control-gear'),steering:document.getElementById('control-steering'),throttle:document.getElementById('control-throttle'),brake:document.getElementById('control-brake')};
const operatorControlReadouts={gear:document.getElementById('operator-control-gear'),steering:document.getElementById('operator-control-steering'),throttle:document.getElementById('operator-control-throttle'),brake:document.getElementById('operator-control-brake')};
let peer=null,controlChannel=null,pendingIce=[],remoteCameraIds=[],offeredCameraByMid=new Map(),iceServers=[],polling=false,connecting=false,authenticated=false,heartbeatInFlight=false,mediaStatus={lanes:[]},h265FailureSamples=0,h265FallbackSent=false,estopLatched=false,gamepadEstopPressedAt=0,gamepadRequiresNeutral=true,activeGamepadIndex=null,latestMetrics={streams:[]},latestRuntimeStatus={},lastAlertKey='',controlAuthorityLost=false,signalingGeneration=0,signalingPollAbort=null,vehicleTelemetry=null,vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null},vcuEverReady=false,selectedGear='N',pendingGearRequest=null,controlWriteActive=false,pendingControlWrite=null,lastControlStatusSeq=0;const previousStats=new Map(),cameraByMid=new Map(),assignedCameraIds=new Set();
let controlOutcomeSession={metrics:controlLogic.createControlOutcomeMetrics()};
function responseError(response,body){const error=Error(body.error||response.status);error.status=response.status;return error}
async function post(path,body={},signal=null){const options={method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)};if(signal)options.signal=signal;const r=await fetch(path,options);const j=await r.json();if(!r.ok)throw responseError(r,j);return j}
async function get(path){const r=await fetch(path);const j=await r.json();if(!r.ok)throw responseError(r,j);return j}
function clientLog(event,details={}){const entry={event,sent_at_utc_ms:Date.now(),details};console.info(JSON.stringify(entry));fetch('/api/browser-event',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(entry),keepalive:true}).catch(()=>{})}
function resetControlOutcomeSession(){controlOutcomeSession={metrics:controlLogic.createControlOutcomeMetrics()}}
function recordControlOutcome(outcome,command,session=controlOutcomeSession){const sequence=Number(command&&command.seq);session.metrics=controlLogic.reduceControlOutcome(session.metrics,outcome,sequence);if(controlLogic.shouldLogControlOutcome(outcome))clientLog('control_command_browser_outcome',{session_id:String(command&&command.session_id||''),vehicle_id:String(command&&command.vehicle_id||''),outcome,control_seq:sequence,...session.metrics})}
function hasTurnServer(){return iceServers.some(server=>String(server.urls||'').includes('turn:')||(Array.isArray(server.urls)&&server.urls.some(url=>String(url).startsWith('turn:'))))}
function safeIceEndpoint(value){const match=String(value||'').match(/^([a-z]+):(?:\/\/)?(?:[^@]*@)?(\[[^\]]+\]|[^:?/]+)(?::(\d+))?/i);return match?`${match[1].toLowerCase()}:${match[2]}${match[3]?`:${match[3]}`:''}`:'unknown'}
function setMetric(id,text,level=''){const element=document.getElementById(id);element.textContent=text;element.classList.remove('ok','warn','critical');if(level)element.classList.add(level)}
function formatMetric(value,digits=1,suffix=''){return Number.isFinite(Number(value))?`${Number(value).toFixed(digits)}${suffix}`:'未知'}
function setCanValue(id,text){const element=document.getElementById(id);if(element)element.textContent=text}
function validCanValue(values,valid,index,digits,unit=''){return Array.isArray(values)&&Array.isArray(valid)&&valid[index]&&Number.isFinite(Number(values[index]))?`${Number(values[index]).toFixed(digits)}${unit}`:'—'}
function ensureCanFeedbackCards(){
  if(!wheelFeedbackGrid.childElementCount)for(let index=0;index<8;index++){const card=document.createElement('article');card.className='wheel-feedback';const title=document.createElement('div');title.className='feedback-title';const name=document.createElement('strong');name.textContent=`轮端 ${index+1}`;const mode=document.createElement('span');mode.id=`can-wheel-mode-${index}`;mode.textContent='M— / B—';title.append(name,mode);const values=document.createElement('div');values.className='feedback-values';for(const [label,key] of [['扭矩','torque'],['转速','speed'],['制动压力','brake']]){const item=document.createElement('div');item.className='feedback-value';const caption=document.createElement('span');caption.textContent=label;const value=document.createElement('strong');value.id=`can-wheel-${key}-${index}`;value.textContent='—';item.append(caption,value);values.appendChild(item)}card.append(title,values);wheelFeedbackGrid.appendChild(card)}
  if(!steeringFeedbackGrid.childElementCount)for(let index=0;index<4;index++){const card=document.createElement('article');card.className='steering-feedback';const title=document.createElement('div');title.className='feedback-title';const name=document.createElement('strong');name.textContent=`转向轴 ${index+1}`;title.appendChild(name);const values=document.createElement('div');values.className='feedback-values';for(const [label,key] of [['转角','angle'],['模式','mode']]){const item=document.createElement('div');item.className='feedback-value';const caption=document.createElement('span');caption.textContent=label;const value=document.createElement('strong');value.id=`can-steering-${key}-${index}`;value.textContent='—';item.append(caption,value);values.appendChild(item)}card.append(title,values);steeringFeedbackGrid.appendChild(card)}
}
function renderCanFeedback(){
  canFeedbackPanel.hidden=!authenticated;
  if(!authenticated)return;
  ensureCanFeedbackCards();
  const feedback=vehicleTelemetry?.can_feedback;
  const supported=Boolean(feedback?.supported);
  const transportAge=vehicleTelemetry&&Number.isFinite(Number(vehicleTelemetry.sent_at_utc_ms))?Math.max(0,Date.now()-Number(vehicleTelemetry.sent_at_utc_ms)):null,canFeedbackAge=supported&&Number(feedback.max_feedback_age_ms)>=0?Number(feedback.max_feedback_age_ms):null;
  canFeedbackStatus.textContent=!vehicleTelemetry?'等待车端遥测':(supported?`序号 ${vehicleTelemetry.seq??'—'}`:'车端桥接包无完整反馈');
  canFeedbackStatus.className=supported&&Boolean(feedback.feedback_fresh)&&transportAge!==null&&transportAge<=500?'status-chip ok':'status-chip warn';
  const telemetrySpeed=vehicleTelemetry?.speed_mps,measuredSpeed=supported&&feedback.speed_valid?Number(feedback.speed_mps):(telemetrySpeed!==null&&Number.isFinite(Number(telemetrySpeed))?Number(telemetrySpeed):null),speedText=measuredSpeed===null?'—':`${measuredSpeed.toFixed(2)} m/s · ${(measuredSpeed*3.6).toFixed(1)} km/h`;
  const gearText=supported&&feedback.gear_valid?vcuGearLabel(feedback.gear):(typeof vehicleTelemetry?.gear==='string'&&vehicleTelemetry.gear?vehicleTelemetry.gear:'—');
  canSpeed.textContent=speedText;operatorSpeed.textContent=speedText;
  canGear.textContent=gearText;operatorActualGear.textContent=gearText;
  canSelector.textContent=supported&&feedback.driver_gear_request_valid?vcuGearLabel(feedback.driver_gear_request):'—';
  canEpb.textContent=supported&&Array.isArray(feedback.parking_brake_status)?feedback.parking_brake_status.map((value,index)=>feedback.parking_brake_valid?.[index]?vcuEpbLabel(value):'—').join('/'):'—';
  canHandshake.textContent=supported&&feedback.handshake_valid?String(feedback.handshake_status):'—';
  canEmergency.textContent=supported&&feedback.gear_valid?String(feedback.emergency_switch??'—'):'—';
  canAge.textContent=!supported?'—':(canFeedbackAge===null?'CAN 未完整':`${canFeedbackAge} ms CAN · ${transportAge??'—'} ms 链路`);
  canAge.className=supported&&Boolean(feedback.feedback_fresh)&&transportAge!==null&&transportAge<=500?'ok':'warn';
  for(let index=0;index<8;index++){setCanValue(`can-wheel-torque-${index}`,validCanValue(feedback?.motor_torque_nm,feedback?.motor_torque_valid,index,1,' Nm'));setCanValue(`can-wheel-speed-${index}`,validCanValue(feedback?.motor_speed_rpm,feedback?.motor_speed_valid,index,0,' rpm'));setCanValue(`can-wheel-brake-${index}`,validCanValue(feedback?.brake_pressure_bar,feedback?.brake_valid,index,1,' bar'));const motorMode=feedback?.motor_mode_valid?.[index]?feedback.motor_mode[index]:'—',brakeMode=feedback?.brake_valid?.[index]?feedback.brake_mode[index]:'—';setCanValue(`can-wheel-mode-${index}`,`M${motorMode} / B${brakeMode}`)}
  for(let index=0;index<4;index++){setCanValue(`can-steering-angle-${index}`,validCanValue(feedback?.steering_angle_deg,feedback?.steering_valid,index,1,'°'));setCanValue(`can-steering-mode-${index}`,feedback?.steering_valid?.[index]?String(feedback.steering_mode[index]):'—')}
}
function vcuGearLabel(value){const labels={1:'N',2:'R',3:'D'};return labels[Number(value)]||`不支持(${String(value)})`}
function vcuEpbLabel(value){const labels={0:'保持',1:'释放',2:'已拉起'};return labels[Number(value)]||`异常(${String(value)})`}
function vcuAdapterReady(status,explicit){return controlLogic.adapterReady(status,explicit)}
function vcuDrivingReady(){return controlLogic.drivingReady(vcuHandshake)}
function diagnoseVcuHandshake(channelOpen){
  if(!channelOpen)return{level:'warn',text:'连接步骤未完成：控制 DataChannel 尚未连接。'};
  if(!vcuHandshake.supported){
    if(vcuHandshake.state==='unsupported')return{level:'ok',text:'当前适配器不需要 VCU 平行驾驶握手。'};
    return{level:'warn',text:'连接步骤未完成：尚未收到车端 VCU 状态。'};
  }
  const stateName=vcuHandshake.state||'unavailable';
  if(stateName==='closed')return{level:'critical',text:'连接步骤失败：车端 VCU 适配器已关闭，请检查 CAN bridge。'};
  if(stateName==='fault')return{level:'critical',text:'运行阶段失败：VCU 握手状态丢失或 CAN/I/O 故障，驾驶命令已阻止；请查看车端 VCU 日志 issue_code。'};
  if((stateName==='standby'||stateName==='disarmed')&&!vcuHandshake.requested&&!vcuHandshake.disarming&&!vcuHandshake.ready){
    if(!vcuHandshake.driver_gear_request_valid)return{level:'warn',text:'准入第 1 步失败：未收到物理挡位反馈；必须确认在 N 挡。'};
    const selector=vcuGearLabel(vcuHandshake.driver_gear_request);
    if(Number(vcuHandshake.driver_gear_request)!==1)return{level:'critical',text:`准入第 1 步失败：当前为 ${selector} 挡；只有 N 挡允许进入平行驾驶。`};
    const epbStatus=Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status:[];
    const epbValid=Array.isArray(vcuHandshake.epb_valid)?vcuHandshake.epb_valid:[];
    if(epbValid.length!==4||epbValid.some(value=>!value))return{level:'warn',text:'准入第 2 步失败：电子驻车反馈不完整；需确认四路电子驻车均已拉起。'};
    if(epbStatus.length!==4||epbStatus.some(value=>Number(value)!==2))return{level:'critical',text:`准入第 2 步失败：电子驻车未全部拉起；当前 ${epbStatus.map(vcuEpbLabel).join('/')}。`};
    if(!vcuHandshake.speed_valid)return{level:'warn',text:'准入第 3 步失败：未收到有效车速反馈；必须确认车辆静止。'};
    if(Math.abs(Number(vcuHandshake.speed_mps))>0.1)return{level:'critical',text:`准入第 3 步失败：当前车速 ${Number(vcuHandshake.speed_mps).toFixed(2)} m/s，高于 0.10 m/s。`};
    if(!vcuHandshake.handshake_valid)return{level:'warn',text:'准入第 4 步失败：未收到 VCU 握手状态；需 VCU 处于人工状态 3。'};
    if(Number(vcuHandshake.handshake_status)!==3)return{level:'critical',text:`准入第 4 步失败：VCU 当前状态 ${String(vcuHandshake.handshake_status)}，需要人工状态 3。`};
    if(!vcuHandshake.parking_ready)return{level:'warn',text:'准入第 5 步失败：N 挡、电子驻车、零速和人工状态值已满足，但反馈已过期；请检查最近 500 ms 的 CAN 更新。'};
    return{level:'ok',text:'准入检查通过：N 挡、电子驻车已拉起、车辆零速、VCU 人工状态及反馈新鲜度均满足。'};
  }
  if(stateName==='standby'&&vcuHandshake.requested){
    return{level:'warn',text:'握手请求已发送：等待车端确认并进入启动第 1/5 步。'};
  }
  const stages={
    initial:'启动第 1/5 步：正在发送 5 个周期低握手帧。',
    wait_parallel_handshake:`启动第 2/5 步未完成：复用智驾握手，等待 VCU 状态 5；当前 ${vcuHandshake.handshake_valid?String(vcuHandshake.handshake_status):'无有效反馈'}。`,
    wait_parking_brake_released:`启动第 3/5 步未完成：等待四路电子驻车释放反馈为 1；当前 ${Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status.map(vcuEpbLabel).join('/'):'无有效反馈'}。`,
    wait_gear:'启动第 4/5 步未完成：等待 N/R/D 目标挡位闭环反馈。',
    wait_actuator_modes:'启动第 5/5 步未完成：等待 MCU/EPS/EHB 全部进入线控模式。',
    ready:'启动完成：平行驾驶已就绪，可以发送驾驶命令。',
    disarm_torque:'退出第 1/5 步未完成：等待八路驱动扭矩归零。',
    disarm_stop:'退出第 2/5 步未完成：正在制动并等待车辆零速。',
    disarm_neutral:'退出第 3/5 步未完成：等待挡位回到 N。',
    disarm_parking_brake:'退出第 4/5 步未完成：等待四路电子驻车全部拉起。',
    disarm_manual:'退出第 5/5 步未完成：等待 VCU 回到人工状态 3。'
  };
  return{level:stateName==='ready'?'ok':'warn',text:stages[stateName]||`VCU 状态无法识别：${stateName}。`};
}
function renderVcuHandshake(){const labels={unavailable:'等待车端状态',unsupported:'当前适配器不支持 VCU 握手',closed:'车端适配器已关闭',standby:'待机（未请求）',initial:'启动 1/5 · 低握手帧',wait_parallel_handshake:'启动 2/5 · 智驾状态 5',wait_parking_brake_released:'启动 3/5 · 电子驻车释放',wait_gear:'启动 4/5 · 挡位闭环',wait_actuator_modes:'启动 5/5 · 执行器模式',ready:'握手成功（平行驾驶）',disarm_torque:'退出 1/5 · 扭矩归零',disarm_stop:'退出 2/5 · 车辆零速',disarm_neutral:'退出 3/5 · N 挡',disarm_parking_brake:'退出 4/5 · 电子驻车',disarm_manual:'退出 5/5 · 人工状态 3',disarmed:'已安全断开',fault:'VCU 通讯故障'};const channelOpen=Boolean(controlChannel&&controlChannel.readyState==='open'),supported=Boolean(vcuHandshake.supported),ready=Boolean(vcuHandshake.ready),disarming=Boolean(vcuHandshake.disarming),requested=Boolean(vcuHandshake.requested),adapterReady=vcuHandshake.adapter_ready===true,diagnostic=diagnoseVcuHandshake(channelOpen);vcuStatus.textContent=labels[vcuHandshake.state]||vcuHandshake.state||'未知';vcuStatus.className=ready&&adapterReady?'ok':(diagnostic.level==='critical'?'critical':'warn');vcuGate.textContent=diagnostic.text;vcuGate.className=`gate-copy ${diagnostic.level}`;const selector=vcuHandshake.driver_gear_request_valid?vcuGearLabel(vcuHandshake.driver_gear_request):'未知',speed=vcuHandshake.speed_valid?`${Number(vcuHandshake.speed_mps).toFixed(2)} m/s`:'未知',manual=vcuHandshake.handshake_valid?String(vcuHandshake.handshake_status):'未知',epb=Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status.map(vcuEpbLabel).join('/'):'未知';vcuGate.title=`控制链路 ${channelOpen?'已连接':'未连接'} · 选择器 ${selector} · 车速 ${speed} · 电子驻车 ${epb} · VCU状态 ${manual}`;vcuConnectButton.disabled=!channelOpen||!adapterReady||!supported||!vcuHandshake.parking_ready||requested||ready||disarming;vcuDisconnectButton.disabled=!channelOpen||!adapterReady||!supported||(!requested&&!ready&&!disarming)}
function renderMonitoring(){const estopPresentation=controlLogic.deriveEstopPresentation(estopLatched,vehicleTelemetry?.estop===true);renderEstopRequest(estopPresentation);renderVcuHandshake();renderCanFeedback();renderControlState();if(!authenticated){monitorPanel.hidden=true;return}monitorPanel.hidden=false;const runtime=latestRuntimeStatus||{},metrics=latestMetrics||{streams:[]},vehicles=runtime.authorized_vehicles||[],selected=vehicles.find(v=>v.vehicle_id===(runtime.vehicle_id||vehicleSelect.value));setMetric('metric-vehicle',selected?(selected.online?`${selected.vehicle_id} 在线`:`${selected.vehicle_id} 离线`):'未知',selected?.online?'ok':'warn');setMetric('metric-session',runtime.connected?`${runtime.session_id||'活动'} · ${metrics.connection_state||'等待媒体'}`:'未连接',runtime.connected?'ok':'warn');const authority=runtime.connected&&!controlAuthorityLost;setMetric('metric-authority',authority?'已获得':'无',authority?'ok':(controlAuthorityLost?'critical':'warn'));const codec=metrics.codec||mediaStatus.codec||'',backend=metrics.backend||mediaStatus.backend||'';setMetric('metric-video',codec||backend?`${codec||'未知'} / ${backend||'未知'}`:'等待媒体',codec?'ok':'warn');setMetric('metric-rtt',formatMetric(metrics.control_rtt_ms,1,' ms'),Number(metrics.control_rtt_ms)>200?'critical':(Number.isFinite(Number(metrics.control_rtt_ms))?'ok':'warn'));setMetric('metric-network',metrics.connection_method||'未知',metrics.connection_method==='TURN'?'warn':(metrics.connection_method&&metrics.connection_method!=='unknown'?'ok':'warn'));const turnConfigured=Boolean(metrics.turn_configured??hasTurnServer());setMetric('metric-turn',metrics.turn_in_use?'正在中继':(turnConfigured?'已配置，未使用':'未配置'),metrics.turn_in_use?'warn':(turnConfigured?'ok':'warn'));const sync=runtime.time_sync||metrics.time_sync||{},timeTrusted=runtime.signaling_available!==false&&Boolean(sync.synchronized)&&Number(sync.uncertainty_ms)<=consoleConfig.max_time_sync_uncertainty_ms;setMetric('metric-time',timeTrusted?`可信 ±${sync.uncertainty_ms} ms`:`不可信${Number.isFinite(Number(sync.uncertainty_ms))?` ±${sync.uncertainty_ms} ms`:''}`,timeTrusted?'ok':'critical');streamMetrics.replaceChildren();const streams=metrics.streams||[];if(!streams.length){const row=document.createElement('tr');const cell=document.createElement('td');cell.colSpan=5;cell.className='muted';cell.textContent='等待视频轨道';row.appendChild(cell);streamMetrics.appendChild(row)}for(const stream of streams){const row=document.createElement('tr');const loss=Number(stream.packet_loss_percent||0),fps=Number(stream.fps||0),latency=Number(stream.estimated_end_to_end_latency_ms||0);for(const [text,level] of [[stream.camera_id||stream.mid||'unknown',''],[formatMetric(fps,1),fps<20?'critical':'ok'],[formatMetric(stream.bitrate_kbps,0,' kbps'),''],[formatMetric(loss,2,'%'),loss>2?'warn':''],[formatMetric(latency,1,' ms'),latency>200?'critical':'ok']]){const cell=document.createElement('td');cell.textContent=text;if(level)cell.className=level;row.appendChild(cell)}streamMetrics.appendChild(row)}const alerts=[];let severity='';if(estopPresentation.visible){alerts.push(estopPresentation.alert);severity=estopPresentation.severity}if(controlAuthorityLost){alerts.push('控制权或信令已丢失，当前页面不会继续发送驾驶命令');severity='critical'}else if(runtime.connected&&(!controlChannel||controlChannel.readyState!=='open')){alerts.push('控制 DataChannel 尚未就绪');if(!severity)severity='warn'}if(vcuHandshake.supported&&!vcuHandshake.ready){const diagnostic=diagnoseVcuHandshake(Boolean(controlChannel&&controlChannel.readyState==='open'));alerts.push(diagnostic.text);if(diagnostic.level==='critical')severity='critical';else if(!severity)severity='warn'}if(!timeTrusted){alerts.push('时间同步不可信，端到端时延只作参考');severity='critical'}for(const stream of streams){if(Number(stream.estimated_end_to_end_latency_ms)>200){alerts.push(`${stream.camera_id||'视频'} 时延超过 200 ms`);severity='critical'}if(Number(stream.fps)<20){alerts.push(`${stream.camera_id||'视频'} 低于 20 FPS`);severity='critical'}}if(!alerts.length)alerts.push(streams.length?'当前指标在目标范围内':'尚无媒体指标；控制命令不会在链路未就绪时发送');alertsPanel.textContent=alerts.join('；');alertsPanel.className=`alerts ${severity}`.trim();const alertKey=`${severity}:${alerts.join('|')}`;if(alertKey!==lastAlertKey){clientLog('control_monitor_state',{severity:severity||'ok',alerts});lastAlertKey=alertKey}}
async function refreshRuntimeStatus(){if(!authenticated)return;try{latestRuntimeStatus=await get('/api/status');if(polling&&!latestRuntimeStatus.connected){closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='控制权丢失'}renderMonitoring()}catch(error){controlAuthorityLost=true;resetControlAuthorityInput();webrtcLabel.textContent='本地状态读取失败';alertsPanel.textContent='无法读取本地运行状态: '+error.message;alertsPanel.className='alerts critical'}}
function clamp(value,min,max){return Math.min(max,Math.max(min,value))}
function effectiveControlLimits(){return{maxThrottle:Math.min(controlLimits.maxThrottle,vehicleHardLimits.max_throttle),serviceBrake:Math.min(controlLimits.serviceBrake,vehicleHardLimits.max_brake),hardBrake:Math.min(controlLimits.hardBrake,vehicleHardLimits.max_brake),maxSteeringDeg:Math.min(controlLimits.maxSteeringDeg,vehicleHardLimits.max_steering_angle_deg)}}
function renderControlLimits(){const effective=effectiveControlLimits();controlLimitsSummary.textContent=`目标车速比例 ≤${(effective.maxThrottle*100).toFixed(0)}% · 缓刹 ${(effective.serviceBrake*100).toFixed(0)}% · 急刹 ${(effective.hardBrake*100).toFixed(0)}% · 转向 ≤${effective.maxSteeringDeg.toFixed(1)}°`;controlLimitsSummary.className=effective.maxThrottle<=0.1&&effective.serviceBrake<=effective.hardBrake&&effective.hardBrake<=1&&effective.maxSteeringDeg<=5?'ok':'warn';maxThrottlePercent.max=String(vehicleHardLimits.max_throttle*100);serviceBrakePercent.max=String(vehicleHardLimits.max_brake*100);hardBrakePercent.max=String(vehicleHardLimits.max_brake*100);maxSteeringDeg.max=String(vehicleHardLimits.max_steering_angle_deg);vehicleHardLimitsLabel.textContent=vehicleHardLimits.received?`车端硬上限：目标车速比例 ${(vehicleHardLimits.max_throttle*100).toFixed(0)}% · 人工刹车 ${(vehicleHardLimits.max_brake*100).toFixed(0)}% · 转向 ${vehicleHardLimits.max_steering_angle_deg.toFixed(1)}° · 配置速度 ${Number(vehicleHardLimits.max_speed_kph).toFixed(1)} km/h`:'等待车端硬上限；应用值仍会被车端再次截断'}
async function updateVehicleHardLimits(value){if(!value||typeof value!=='object')return;const throttle=Number(value.max_throttle),brake=value.max_brake===undefined?1:Number(value.max_brake),steering=Number(value.max_steering_angle_deg),speed=Number(value.max_speed_kph);if(!Number.isFinite(throttle)||throttle<0||throttle>1||!Number.isFinite(brake)||brake<0||brake>1||!Number.isFinite(steering)||steering<0||steering>limitConfig.steering_full_scale_deg||!Number.isFinite(speed)||speed<0)return;vehicleHardLimits={max_throttle:throttle,max_brake:brake,max_steering_angle_deg:steering,max_speed_kph:speed,received:true};const previous={...controlLimits};controlLimits.maxThrottle=Math.min(controlLimits.maxThrottle,throttle);controlLimits.serviceBrake=Math.min(controlLimits.serviceBrake,brake);controlLimits.hardBrake=Math.min(controlLimits.hardBrake,brake);controlLimits.maxSteeringDeg=Math.min(controlLimits.maxSteeringDeg,steering);if(previous.maxThrottle!==controlLimits.maxThrottle||previous.serviceBrake!==controlLimits.serviceBrake||previous.hardBrake!==controlLimits.hardBrake||previous.maxSteeringDeg!==controlLimits.maxSteeringDeg){clearControlInput(false);if(previous.serviceBrake!==controlLimits.serviceBrake||previous.hardBrake!==controlLimits.hardBrake)await post('/api/control-limits',{service_brake:controlLimits.serviceBrake,hard_brake:controlLimits.hardBrake});clientLog('control_limits_reduced_by_vehicle',{requested_max_throttle:previous.maxThrottle,requested_service_brake:previous.serviceBrake,requested_hard_brake:previous.hardBrake,requested_max_steering_angle_deg:previous.maxSteeringDeg,effective_max_throttle:controlLimits.maxThrottle,effective_service_brake:controlLimits.serviceBrake,effective_hard_brake:controlLimits.hardBrake,effective_max_steering_angle_deg:controlLimits.maxSteeringDeg})}renderControlLimits()}
function openControlLimits(){maxThrottlePercent.value=(controlLimits.maxThrottle*100).toFixed(0);serviceBrakePercent.value=(controlLimits.serviceBrake*100).toFixed(0);hardBrakePercent.value=(controlLimits.hardBrake*100).toFixed(0);maxSteeringDeg.value=controlLimits.maxSteeringDeg.toFixed(1);controlLimitsConfirm.checked=false;controlLimitsApply.disabled=true;renderControlLimits();controlLimitsDialog.showModal()}
async function applyControlLimits(){if(!controlLimitsConfirm.checked)throw Error('请先确认隔离台架条件');if(vcuHandshake.supported&&(!vcuHandshake.speed_valid||Math.abs(Number(vcuHandshake.speed_mps))>0.1))throw Error('VCU 车速未确认归零，禁止修改实车限幅');const throttlePercent=Number(maxThrottlePercent.value),servicePercent=Number(serviceBrakePercent.value),hardPercent=Number(hardBrakePercent.value),steeringDeg=Number(maxSteeringDeg.value);if(!Number.isFinite(throttlePercent)||throttlePercent<0||throttlePercent>vehicleHardLimits.max_throttle*100)throw Error('目标车速比例上限超出车端硬上限');if(!Number.isFinite(servicePercent)||servicePercent<0||servicePercent>vehicleHardLimits.max_brake*100)throw Error('缓刹请求超出车端硬上限');if(!Number.isFinite(hardPercent)||hardPercent<0||hardPercent>vehicleHardLimits.max_brake*100)throw Error('急刹请求超出车端硬上限');if(servicePercent>hardPercent)throw Error('缓刹请求不能大于急刹请求');if(!Number.isFinite(steeringDeg)||steeringDeg<0||steeringDeg>vehicleHardLimits.max_steering_angle_deg)throw Error('转向上限超出车端硬上限');clearControlInput(false);const applied=await post('/api/control-limits',{service_brake:servicePercent/100,hard_brake:hardPercent/100});controlLimits.maxThrottle=throttlePercent/100;controlLimits.serviceBrake=Number(applied.service_brake);controlLimits.hardBrake=Number(applied.hard_brake);controlLimits.maxSteeringDeg=steeringDeg;controlLimitsDialog.close();renderControlLimits();clientLog('driver_control_limits_applied',{max_throttle:controlLimits.maxThrottle,service_brake:controlLimits.serviceBrake,hard_brake:controlLimits.hardBrake,max_steering_angle_deg:controlLimits.maxSteeringDeg,vehicle_hard_limits:vehicleHardLimits});statusPanel.textContent=`调试限幅已应用：目标车速比例 ${throttlePercent.toFixed(0)}%，缓刹 ${servicePercent.toFixed(0)}%，急刹 ${hardPercent.toFixed(0)}%，转向 ${steeringDeg.toFixed(1)}°`;send({},false).catch(console.error)}
function applyDeadzone(value){const magnitude=Math.abs(value),deadzone=gamepadConfig.axis_deadzone;if(magnitude<=deadzone)return 0;return Math.sign(value)*(magnitude-deadzone)/(1-deadzone)}
)HTML" + R"HTML(
function applyPedalDeadzone(value){const deadzone=gamepadConfig.axis_deadzone;return value<=deadzone?0:(value-deadzone)/(1-deadzone)}
function axisValue(pad,index){return Number.isInteger(index)&&index>=0&&index<pad.axes.length&&Number.isFinite(pad.axes[index])?pad.axes[index]:null}
function buttonValue(pad,index){return Number.isInteger(index)&&index>=0&&index<pad.buttons.length?Number(pad.buttons[index].value||0):0}
function syncControlKeyState(){state=controlLogic.deriveKeyState(pressedControlKeys)}
function vcuMockUnsupported(value=vcuHandshake){return controlLogic.mockUnsupported(value)}
function vcuAllowsGearChange(requestedGear){return controlLogic.allowsGearChange(selectedGear,requestedGear,vcuHandshake)}
function updateSelectedGearFromInput(inputState){const next=controlLogic.deriveGearSelection(selectedGear,inputState,vcuHandshake);selectedGear=next.selectedGear;pendingGearRequest=next.pendingGearRequest;if(pendingGearRequest)statusPanel.textContent=`${selectedGear}→${pendingGearRequest} 换挡已阻止：需有效零速反馈；请停车后释放并重新按下方向键`;return next}
function updateSelectedGearFromHeldDirections(){return updateSelectedGearFromInput(state)}
function clearControlInput(resetGear=true){controlLogic.blockAndClearKeys(pressedControlKeys,blockedControlKeys);syncControlKeyState();gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;gamepadRequiresNeutral=true;if(resetGear){selectedGear='N';pendingGearRequest=null}renderControlState()}
function vcuStateRequiresFreshInput(value=vcuHandshake){return controlLogic.requiresFreshInput(value)}
function vcuStateKeepsHeldInput(value=vcuHandshake){return controlLogic.keepsHeldInput(vcuEverReady,value)}
function acceptControlStatusMessage(message){const decision=controlLogic.reduceStatusSequence(lastControlStatusSeq,message?.control_status_seq);if(!decision.accepted){const sequence=Number(message?.control_status_seq);clientLog('control_status_message_dropped',{event:message?.event||'unknown',control_status_seq:Number.isFinite(sequence)?sequence:null,last_control_status_seq:lastControlStatusSeq});return false}if(decision.gap>0)clientLog('control_status_sequence_gap',{event:message?.event||'unknown',control_status_seq:decision.lastSequence,last_control_status_seq:lastControlStatusSeq,missing_status_count:decision.gap});lastControlStatusSeq=decision.lastSequence;return true}
function resetControlAuthorityInput(){vcuEverReady=false;clearControlInput()}
function updateVcuHandshakeState(value){const transition=controlLogic.transitionVcuState(vcuEverReady,value);vcuHandshake=value;vcuEverReady=transition.everReady;if(transition.resetInput)resetControlAuthorityInput()}
function suspendSignalingPoll(){const generation=++signalingGeneration;polling=false;if(signalingPollAbort){signalingPollAbort.abort();signalingPollAbort=null}return generation}
function closeRealtimeSession(){const generation=suspendSignalingPoll();resetControlAuthorityInput();lastControlStatusSeq=0;resetControlOutcomeSession();if(controlChannel)controlChannel.close();if(peer)peer.close();controlChannel=null;peer=null;vehicleTelemetry=null;vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};pendingIce=[];remoteCameraIds=[];offeredCameraByMid.clear();cameraByMid.clear();assignedCameraIds.clear();previousStats.clear();cameraGrid.replaceChildren(emptyStage);renderMonitoring();return generation}
function renderEstopRequest(presentation=controlLogic.deriveEstopPresentation(estopLatched,vehicleTelemetry?.estop===true)){estopStatus.hidden=!presentation.visible;estopStatus.textContent=presentation.banner}
function latchEstop(source){if(estopLatched)return;estopLatched=true;clientLog('control_estop_request_latched',{source});renderEstopRequest();renderMonitoring()}
function firstConnectedGamepad(){const pads=navigator.getGamepads?navigator.getGamepads():[];if(activeGamepadIndex!==null&&pads[activeGamepadIndex]?.connected)return pads[activeGamepadIndex];for(const pad of pads)if(pad?.connected){activeGamepadIndex=pad.index;return pad}activeGamepadIndex=null;return null}
function applyGamepadNeutralInterlock(authorityReady,gearRequestPending=false){const next=controlLogic.reduceGamepadNeutralInterlock({requiresNeutral:gamepadRequiresNeutral,authorityReady,throttle:gamepadState.throttle,brake:gamepadState.brake,gearRequestPending});gamepadRequiresNeutral=next.requiresNeutral;gamepadState.throttle=next.throttle;gamepadState.brake=next.brake;return next}
function sampleGamepad(){if(!gamepadConfig.enabled||document.hidden||!document.hasFocus()){gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}const pad=firstConnectedGamepad();if(!pad){gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}gamepadState.connected=true;const standard=pad.mapping==='standard';if(standard){const steering=axisValue(pad,0);let steeringValue=steering===null?0:(steering-calibration.steeringCenter)/calibration.steeringRange;if(gamepadConfig.steering_inverted)steeringValue=-steeringValue;gamepadState.steering=clamp(applyDeadzone(steeringValue),-1,1);gamepadState.throttle=clamp(applyPedalDeadzone(buttonValue(pad,7)),0,1);gamepadState.brake=clamp(applyPedalDeadzone(buttonValue(pad,6)),0,1)}else{const steering=axisValue(pad,gamepadConfig.steering_axis),throttle=axisValue(pad,gamepadConfig.throttle_axis),brake=axisValue(pad,gamepadConfig.brake_axis);if(steering===null||throttle===null||brake===null){gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}let steeringValue=(steering-calibration.steeringCenter)/calibration.steeringRange;if(gamepadConfig.steering_inverted)steeringValue=-steeringValue;gamepadState.steering=clamp(applyDeadzone(steeringValue),-1,1);const throttleDelta=gamepadConfig.throttle_inverted?calibration.throttleRest-throttle:throttle-calibration.throttleRest;const brakeDelta=gamepadConfig.brake_inverted?calibration.brakeRest-brake:brake-calibration.brakeRest;gamepadState.throttle=clamp(applyPedalDeadzone(throttleDelta/calibration.throttleRange),0,1);gamepadState.brake=clamp(applyPedalDeadzone(brakeDelta/calibration.brakeRange),0,1)}const gamepadAuthorityReady=vcuEverReady||vcuMockUnsupported();applyGamepadNeutralInterlock(gamepadAuthorityReady);if(gamepadState.throttle>0&&selectedGear==='N'){const nextGear=updateSelectedGearFromInput({up:true,down:false});if(nextGear.pendingGearRequest)applyGamepadNeutralInterlock(gamepadAuthorityReady,true)}const estopPressed=buttonValue(pad,gamepadConfig.estop_button)>=0.5;if(estopPressed){if(!gamepadEstopPressedAt)gamepadEstopPressedAt=performance.now();if(performance.now()-gamepadEstopPressedAt>=consoleConfig.estop_hold_ms)latchEstop('Gamepad')}else gamepadEstopPressedAt=0;renderControlState()}
function currentControl(extra={}){return controlLogic.deriveControl({keyState:state,gamepad:gamepadState,selectedGear,limits:effectiveControlLimits(),steeringFullScaleDeg:limitConfig.steering_full_scale_deg,estop:estopLatched||Boolean(extra.estop)})}
function setControlReadout(name,text,active=false){for(const element of [controlReadouts[name],operatorControlReadouts[name]]){element.textContent=text;element.parentElement?.classList.toggle('active',active)}}
function renderControlState(){const control=currentControl();for(const [name,element] of Object.entries(keyIndicators)){const active=Boolean(state[name]);element.classList.toggle('active',active);element.setAttribute('aria-pressed',String(active))}setControlReadout('gear',control.gear);setControlReadout('steering',control.steering.toFixed(2),Math.abs(control.steering)>0.001);setControlReadout('throttle',control.throttle.toFixed(2),control.throttle>0.001);setControlReadout('brake',control.brake.toFixed(2),control.brake>0.001);const linkReady=polling&&peer?.connectionState==='connected'&&controlChannel?.readyState==='open',vcuReady=vcuDrivingReady(),ready=linkReady&&vcuReady,retainedWait=vcuHandshake.adapter_ready===true&&vcuEverReady&&vcuHandshake.state==='wait_gear'?'换挡闭环中（输入保持）':(vcuHandshake.adapter_ready===true&&vcuEverReady&&vcuHandshake.state==='wait_actuator_modes'?'执行器闭环中（输入保持）':''),terminalState=vcuStateRequiresFreshInput(vcuHandshake),freshReadyRequired=linkReady&&vcuHandshake.ready&&!vcuEverReady;inputReadiness.textContent=estopLatched?'急停请求锁定':(pendingGearRequest?`等待有效零速后重新选择 ${pendingGearRequest}`:(ready?'控制已就绪':(retainedWait||(terminalState?'VCU 故障/退出，输入已清除':(freshReadyRequired?'输入已清除，等待新鲜 VCU Ready':(linkReady?'等待 VCU 握手':(polling?'等待控制链路':'等待连接')))))));inputReadiness.className=`status-chip ${estopLatched||terminalState?'critical':(ready?'ok':'warn')}`}
function renderVehicles(vehicles=[]){const previous=vehicleSelect.value,currentVehicle=latestRuntimeStatus.connected?latestRuntimeStatus.vehicle_id:'';vehicleSelect.replaceChildren();let firstSelectable='',previousAvailable=false;const labels={online:'在线可控',offline:'离线',active:'控制中',reserved:'已预留',connecting:'连接中',revoked:'已撤销'};for(const vehicle of vehicles){const option=document.createElement('option'),current=vehicle.vehicle_id===currentVehicle,selectable=vehicle.controllable||current;option.value=vehicle.vehicle_id;option.textContent=`${vehicle.vehicle_id} · ${current?'当前会话':(labels[vehicle.state]||vehicle.state)}`;option.disabled=!selectable;if(selectable&&!firstSelectable)firstSelectable=vehicle.vehicle_id;if(selectable&&vehicle.vehicle_id===previous)previousAvailable=true;vehicleSelect.appendChild(option)}vehicleSelect.value=previousAvailable?previous:firstSelectable;connectButton.disabled=connecting||!vehicleSelect.value}
function renderAuthExpiry(expiresAt){authExpiry.textContent=expiresAt?`认证有效至 ${new Date(expiresAt).toLocaleString()}`:''}
function requireLogin(message){closeRealtimeSession();authenticated=false;controlAuthorityLost=false;connectButton.textContent='连接所选车辆';renderAuthExpiry(0);sessionPanel.hidden=true;vcuPanel.hidden=true;monitorPanel.hidden=true;loginPanel.hidden=false;statusPanel.textContent=message;clientLog('driver_reauthentication_required',{reason:message})}
function handleVehicleRefreshError(error){if(error.status===401){requireLogin('登录已失效，请重新认证: '+error.message);return}statusPanel.textContent='车辆状态刷新失败，当前会话已保留: '+error.message;clientLog('vehicle_list_refresh_failed',{error:error.message})}
async function login(){const password=passwordInput.value;if(!password)throw Error('请输入驾驶员密码');passwordInput.value='';const result=await post('/api/login',{password});authenticated=true;controlAuthorityLost=false;webrtcLabel.textContent='未连接';loginPanel.hidden=true;sessionPanel.hidden=false;vcuPanel.hidden=false;renderVehicles(result.vehicles||[]);renderAuthExpiry(result.token_expires_at_utc_ms);sampleGamepad();latestRuntimeStatus=await get('/api/status');renderMonitoring();statusPanel.textContent=`已登录 ${result.driver_id}，请选择在线车辆`;clientLog('driver_login_succeeded',{driver_id:result.driver_id,authorized_vehicle_count:(result.vehicles||[]).length})}
async function refreshVehicles(){if(!authenticated)return;const result=await get('/api/vehicles');renderVehicles(result.vehicles||[]);renderAuthExpiry(result.token_expires_at_utc_ms);if(result.signaling_available===false){controlAuthorityLost=true;resetControlAuthorityInput();connectButton.disabled=true;statusPanel.textContent='信令服务暂时不可用；车辆列表为安全快照，禁止建立控制会话';renderMonitoring();return}if(result.signaling_restart_recovered){closeRealtimeSession();controlAuthorityLost=true;latestRuntimeStatus=await get('/api/status');connectButton.textContent='连接所选车辆';webrtcLabel.textContent='服务已恢复，需重新建立控制会话';statusPanel.textContent='信令服务已重启，驾驶员身份已自动恢复；旧控制权未恢复，请重新选择车辆';clientLog('signaling_restart_recovered',{previous_service_instance_id:result.previous_service_instance_id,service_instance_id:result.service_instance_id,control_authority_recovered:false});renderMonitoring()}}
function sendVcuHandshakeCommand(action){if(!controlChannel||controlChannel.readyState!=='open')throw Error('控制 DataChannel 尚未连接');if(vcuHandshake.adapter_ready!==true)throw Error('VCU 适配器尚未就绪');if(!['connect','disconnect'].includes(action))throw Error('VCU 握手命令非法');if(action==='disconnect'){resetControlAuthorityInput();vcuHandshake={...vcuHandshake,ready:false,disarming:true}}else vcuHandshake={...vcuHandshake,requested:true};renderMonitoring();controlChannel.send(JSON.stringify({event:'vcu_handshake_command',action,sent_at_utc_ms:Date.now()}));clientLog('driver_vcu_handshake_command',{action});statusPanel.textContent=action==='connect'?'已请求开始 VCU 平行驾驶握手':'已请求安全断开 VCU 握手'}
async function writeControl(extra,announceUnavailable){
  const activePeer=peer,activeChannel=controlChannel,outcomeSession=controlOutcomeSession;
  if(controlAuthorityLost||!activePeer||activePeer.connectionState!=='connected'||!activeChannel||activeChannel.readyState!=='open'){
    resetControlAuthorityInput();
    if(announceUnavailable)webrtcLabel.textContent=controlAuthorityLost?'控制权丢失':'控制链路中断';
    return{sent:false,reason:controlAuthorityLost?'control_authority_lost':'control_link_unavailable'};
  }
  const estopRequested=estopLatched||Boolean(extra.estop);
  const retainedWait=vcuHandshake.adapter_ready===true&&vcuStateKeepsHeldInput(vcuHandshake);
  if(estopRequested&&vcuHandshake.adapter_ready===false){
    resetControlAuthorityInput();
    if(announceUnavailable)statusPanel.textContent='VCU 适配器明确不可用，远程急停未发送；请使用车辆物理急停';
    return{sent:false,reason:'vcu_adapter_unavailable'};
  }
  if(!vcuDrivingReady()&&!estopRequested){
    if(!retainedWait){
      if(vcuHandshake.adapter_ready!==true||vcuStateRequiresFreshInput(vcuHandshake))resetControlAuthorityInput();
      if(announceUnavailable)statusPanel.textContent='VCU 平行驾驶握手未成功，驾驶命令已阻止';
      return{sent:false,reason:'vcu_handshake_not_ready'};
    }
  }
  if(activeChannel.bufferedAmount>4096&&!estopRequested){
    resetControlAuthorityInput();
    webrtcLabel.textContent='控制链路拥塞，输入已清除';
    return{sent:false,reason:'buffered_amount_limit'};
  }
  const outgoing=currentControl(extra);
  if(retainedWait&&!estopRequested)outgoing.throttle=0;
  const outgoingSnapshot=controlLogic.controlSnapshot(outgoing);
  let prepared;
  try{prepared=await post('/api/control',outgoing)}catch(error){
    if([401,403,409].includes(error.status)){controlAuthorityLost=true;resetControlAuthorityInput()}
    throw error;
  }
  recordControlOutcome('prepared',prepared.command,outcomeSession);
  if(controlAuthorityLost||peer!==activePeer||controlChannel!==activeChannel||activePeer.connectionState!=='connected'||activeChannel.readyState!=='open'){
    recordControlOutcome('post_prepare_link_changed',prepared.command,outcomeSession);
    resetControlAuthorityInput();
    return{sent:false,reason:'control_link_changed'};
  }
  if(estopRequested&&vcuHandshake.adapter_ready===false){
    recordControlOutcome('post_prepare_vcu_not_ready',prepared.command,outcomeSession);
    resetControlAuthorityInput();
    if(announceUnavailable)statusPanel.textContent='VCU 适配器明确不可用，远程急停未发送；请使用车辆物理急停';
    return{sent:false,reason:'vcu_adapter_unavailable'};
  }
  const stillVcuReady=vcuDrivingReady(),stillRetainedWait=vcuHandshake.adapter_ready===true&&vcuStateKeepsHeldInput(vcuHandshake);
  if(!stillVcuReady&&!estopRequested){
    if(!stillRetainedWait||!retainedWait){
      recordControlOutcome('post_prepare_vcu_not_ready',prepared.command,outcomeSession);
      if(vcuHandshake.adapter_ready!==true||vcuStateRequiresFreshInput(vcuHandshake))resetControlAuthorityInput();
      return{sent:false,reason:'vcu_handshake_not_ready'};
    }
  }
  const latestOutgoing=currentControl(extra);
  if(stillRetainedWait&&!estopRequested)latestOutgoing.throttle=0;
  if(controlLogic.controlIntentSuperseded(outgoingSnapshot,latestOutgoing)){
    recordControlOutcome('superseded',prepared.command,outcomeSession);
    return{sent:false,reason:'control_intent_superseded'};
  }
  try{activeChannel.send(JSON.stringify(prepared.command))}catch(error){
    recordControlOutcome('post_prepare_link_changed',prepared.command,outcomeSession);
    resetControlAuthorityInput();
    throw error;
  }
  recordControlOutcome('forwarded',prepared.command,outcomeSession);
  if(webrtcLabel.textContent==='控制链路拥塞，输入已清除')webrtcLabel.textContent='控制链路已连接';
  return{...prepared,delivery_state:'browser_data_channel_send_invoked'};
}
function enqueueControlWrite(extra,announceUnavailable){return new Promise((resolve,reject)=>{if(pendingControlWrite){pendingControlWrite.extra={...pendingControlWrite.extra,...extra,estop:Boolean(pendingControlWrite.extra.estop||extra.estop)};pendingControlWrite.announceUnavailable=pendingControlWrite.announceUnavailable||announceUnavailable;pendingControlWrite.waiters.push({resolve,reject})}else pendingControlWrite={extra:{...extra},announceUnavailable,waiters:[{resolve,reject}]};drainControlWrites().catch(console.error)})}
async function drainControlWrites(){if(controlWriteActive)return;controlWriteActive=true;try{while(pendingControlWrite){const request=pendingControlWrite;pendingControlWrite=null;try{const result=await writeControl(request.extra,request.announceUnavailable);for(const waiter of request.waiters)waiter.resolve(result)}catch(error){for(const waiter of request.waiters)waiter.reject(error)}}}finally{controlWriteActive=false;if(pendingControlWrite)drainControlWrites().catch(console.error)}}
async function send(extra={},announceUnavailable=true){return enqueueControlWrite(extra,announceUnavailable)}
async function heartbeat(){if(!polling||heartbeatInFlight)return;heartbeatInFlight=true;try{sampleGamepad();await send({},false)}finally{heartbeatInFlight=false}}
function advertisedCodecs(){const caps=RTCRtpReceiver.getCapabilities&&RTCRtpReceiver.getCapabilities('video');const found=new Set(['h264']);for(const c of (caps&&caps.codecs)||[]){const m=(c.mimeType||'').toLowerCase();if(m.includes('h265')||m.includes('hevc'))found.add('h265');if(m.includes('h264')||m.includes('avc'))found.add('h264')}return [...found]}
async function connect(){if(connecting)return;const target=vehicleSelect.value;if(!target)throw Error('没有可连接的在线车辆');const fromVehicle=latestRuntimeStatus.connected?latestRuntimeStatus.vehicle_id:'';if(polling&&fromVehicle===target){statusPanel.textContent=`车辆 ${target} 已处于当前会话`;return}const changingVehicle=Boolean(fromVehicle)&&fromVehicle!==target;const reconnecting=Boolean(fromVehicle)&&fromVehicle===target;const hadRealtime=polling;let suspendedGeneration=signalingGeneration;if((changingVehicle||reconnecting)&&hadRealtime){suspendedGeneration=suspendSignalingPoll();clearControlInput()}connecting=true;connectButton.disabled=true;if(changingVehicle){webrtcLabel.textContent='正在安全切换车辆';statusPanel.textContent=`正在验证 ${target}，成功后释放 ${fromVehicle}`;clientLog('driver_vehicle_switch_started',{from_vehicle_id:fromVehicle,to_vehicle_id:target})}let session=null,generation=signalingGeneration;try{session=await post('/api/connect',{vehicle_id:target});generation=closeRealtimeSession();controlAuthorityLost=true;const ice=await post('/api/webrtc/ice-servers');iceServers=ice.ice_servers||[];await post('/api/webrtc/capabilities',{codecs:advertisedCodecs()});polling=true;controlAuthorityLost=false;latestRuntimeStatus=await get('/api/status');webrtcLabel.textContent='等待车端媒体';statusPanel.textContent=`会话 ${session.session_id} · ${session.vehicle_id}`;connectButton.textContent='切换所选车辆';document.querySelector('main').focus();renderMonitoring();clientLog(changingVehicle?'driver_vehicle_switched':(reconnecting?'driver_session_reconnected':'driver_session_connected'),{from_vehicle_id:fromVehicle||undefined,session_id:session.session_id,vehicle_id:session.vehicle_id});pollSignaling(generation)}catch(error){if(session)await post('/api/end-session',{reason:'driver_connect_setup_failed'}).catch(()=>{});latestRuntimeStatus=await get('/api/status').catch(()=>({connected:false}));const retained=Boolean(!session&&latestRuntimeStatus.connected&&hadRealtime);if(retained){polling=true;controlAuthorityLost=false;webrtcLabel.textContent=controlChannel&&controlChannel.readyState==='open'?'控制链路已连接':'当前会话已保留';statusPanel.textContent=`切换失败，当前会话已保留: ${error.message}`;clientLog('driver_vehicle_switch_rejected',{from_vehicle_id:fromVehicle,to_vehicle_id:target,error:error.message});pollSignaling(suspendedGeneration)}else{controlAuthorityLost=Boolean(latestRuntimeStatus.connected)}connectButton.textContent=latestRuntimeStatus.connected?'切换所选车辆':'连接所选车辆';renderMonitoring();if(!retained)throw error}finally{connecting=false;connectButton.disabled=!vehicleSelect.value}}
async function logout(){const estopConfirmed=vehicleTelemetry?.estop===true;closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='正在释放控制权';await post('/api/disconnect',{reason:'driver_safe_logout'});authenticated=false;controlAuthorityLost=false;connectButton.textContent='连接所选车辆';renderAuthExpiry(0);sessionPanel.hidden=true;vcuPanel.hidden=true;canFeedbackPanel.hidden=true;monitorPanel.hidden=true;loginPanel.hidden=false;webrtcLabel.textContent='未连接';statusPanel.textContent=estopLatched?(estopConfirmed?'已安全退出；车辆急停已确认，仍需本地确认复位':'已安全退出；急停请求未获车端确认，请在车辆本地核实'):'已安全退出';clientLog('driver_safe_logout',{estop_request_latched:estopLatched,estop_confirmed:estopConfirmed})}
addEventListener('pagehide',()=>{closeRealtimeSession();if(authenticated)fetch('/api/disconnect',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify({reason:'browser_page_closed'}),keepalive:true}).catch(()=>{})});
function neutralizeInput(){clearControlInput(false);send({},false).catch(console.error)}
addEventListener('blur',neutralizeInput);document.addEventListener('visibilitychange',()=>{if(document.hidden)neutralizeInput()});
document.querySelector('#login').onclick=()=>login().catch(e=>{statusPanel.textContent='登录失败: '+e.message});
passwordInput.addEventListener('keydown',e=>{if(e.key==='Enter')login().catch(error=>{statusPanel.textContent='登录失败: '+error.message})});
document.querySelector('#connect').onclick=()=>connect().catch(e=>{webrtcLabel.textContent='连接失败';statusPanel.textContent=e.message});
document.querySelector('#logout').onclick=()=>logout().catch(e=>{statusPanel.textContent='退出失败: '+e.message});
document.querySelector('#estop').onclick=()=>{latchEstop('页面按钮');send({estop:true}).catch(alert)};
controlLimitsOpen.onclick=openControlLimits;
controlLimitsConfirm.onchange=()=>{controlLimitsApply.disabled=!controlLimitsConfirm.checked};
controlLimitsApply.onclick=()=>applyControlLimits().catch(error=>{statusPanel.textContent='限幅设置失败: '+error.message});
controlLimitsCancel.onclick=()=>controlLimitsDialog.close();
vcuConnectButton.onclick=()=>{try{sendVcuHandshakeCommand('connect')}catch(error){statusPanel.textContent='开始握手失败: '+error.message}};
vcuDisconnectButton.onclick=()=>{try{sendVcuHandshakeCommand('disconnect')}catch(error){statusPanel.textContent='断开握手失败: '+error.message}};
const keyNames={left:'左转',right:'右转',up:'前进',down:'倒车',service_brake:'缓刹',hard_brake:'急刹'};
renderControlLimits();
get('/api/control-limits').then(value=>{const hardBrake=Number(value.hard_brake??value.max_brake),serviceBrake=Number(value.service_brake??Math.min(controlLimits.serviceBrake,hardBrake));if(Number.isFinite(serviceBrake)&&Number.isFinite(hardBrake)&&serviceBrake>=0&&serviceBrake<=hardBrake&&hardBrake<=1){controlLimits.serviceBrake=serviceBrake;controlLimits.hardBrake=hardBrake;renderControlLimits()}}).catch(console.error);
renderControlState();
addEventListener('gamepadconnected',e=>{activeGamepadIndex=e.gamepad.index;sampleGamepad();clientLog('gamepad_connected',{id:e.gamepad.id,mapping:e.gamepad.mapping,axes:e.gamepad.axes.length,buttons:e.gamepad.buttons.length})});addEventListener('gamepaddisconnected',e=>{if(activeGamepadIndex===e.gamepad.index)activeGamepadIndex=null;gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;gamepadRequiresNeutral=true;renderControlState();clientLog('gamepad_disconnected',{id:e.gamepad.id})});
function editingTarget(target){return ['INPUT','SELECT','TEXTAREA','BUTTON'].includes(target?.tagName)||Boolean(target?.isContentEditable)}
addEventListener('keydown',e=>{const binding=keys[e.code],estopKey=e.code==='KeyE';if(!binding&&!estopKey)return;if(editingTarget(e.target))return;e.preventDefault();if(!polling){if(binding)controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${estopKey?'急停':keyNames[binding]}已截获 · 等待连接`;return}if(estopKey){if(!e.repeat){lastKeyboardEvent.textContent='急停请求已锁定 · E';latchEstop('键盘 E');send({estop:true}).catch(console.error)}return}if(blockedControlKeys.has(e.code)){lastKeyboardEvent.textContent=`${keyNames[binding]}需释放后重新按下 · ${e.code}`;return}if(vcuStateRequiresFreshInput(vcuHandshake)){controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${keyNames[binding]}已阻止 · 等待 VCU 恢复后重新按下`;return}if(!vcuEverReady&&!vcuMockUnsupported()){controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${keyNames[binding]}已阻止 · 首次握手完成后请重新按下`;return}const pressed=controlLogic.pressKey(pressedControlKeys,blockedControlKeys,e.code);if(pressed.changed){syncControlKeyState();updateSelectedGearFromHeldDirections();lastKeyboardEvent.textContent=`${keyNames[binding]}按下 · ${e.code}`;renderControlState();send().catch(console.error)}});
addEventListener('keyup',e=>{const binding=keys[e.code];if(!binding)return;if(!editingTarget(e.target))e.preventDefault();const released=controlLogic.releaseKey(pressedControlKeys,blockedControlKeys,e.code);syncControlKeyState();updateSelectedGearFromHeldDirections();lastKeyboardEvent.textContent=`${keyNames[binding]}释放 · ${e.code}`;renderControlState();if(released.changed&&polling)send().catch(console.error)});
async function pollSignaling(generation){const controller=new AbortController();signalingPollAbort=controller;while(polling&&generation===signalingGeneration){try{const data=await post('/api/poll-signaling',{},controller.signal);if(generation!==signalingGeneration)break;for(const message of data.messages||[]){if(message.type==='webrtc_offer')await startFromOffer(message.payload||{});if(message.type==='ice_candidate')await addIce(message.payload||{});if(message.type==='media_status'){mediaStatus=message.payload||{lanes:[]};renderMonitoring()}}}catch(e){if(generation!==signalingGeneration||e.name==='AbortError')break;closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='控制权或信令中断';statusPanel.textContent='信令轮询失败，已停止驾驶命令: '+e.message;post('/api/end-session',{reason:'signaling_poll_failed'}).catch(()=>{});clientLog('signaling_poll_failed',{error:e.message});renderMonitoring();break}await new Promise(r=>setTimeout(r,100))}if(signalingPollAbort===controller)signalingPollAbort=null}
async function addIce(candidate){if(!candidate.candidate)return;if(!peer||!peer.remoteDescription){pendingIce.push(candidate);return}await peer.addIceCandidate(candidate)}
function offeredVideoCameraIds(sdp,tracks){const mapping=new Map(),cameraIds=(tracks||[]).map(track=>track.camera_id).filter(Boolean);let cameraIndex=0;for(const section of String(sdp||'').split(/\r?\nm=/).slice(1)){if(!section.startsWith('video '))continue;const match=section.match(/(?:^|\r?\n)a=mid:([^\r\n]+)/),cameraId=cameraIds[cameraIndex++];if(match&&cameraId)mapping.set(match[1],cameraId)}return mapping}
function attach(cameraId,track){if(emptyStage.isConnected)emptyStage.remove();let box=document.getElementById('camera-'+cameraId);if(!box){box=document.createElement('article');box.id='camera-'+cameraId;box.className='camera';box.innerHTML='<span class="label"></span><video autoplay playsinline muted></video>';box.querySelector('.label').textContent=cameraId;cameraGrid.appendChild(box)}box.querySelector('video').srcObject=new MediaStream([track])}
async function startFromOffer(offer){
  if(peer)peer.close();
  controlChannel=null;
  lastControlStatusSeq=0;
  vehicleTelemetry=null;
  vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};
  resetControlAuthorityInput();
  cameraGrid.replaceChildren();
  pendingIce=[];
  cameraByMid.clear();
  assignedCameraIds.clear();
  previousStats.clear();
  h265FailureSamples=0;
  h265FallbackSent=false;
  remoteCameraIds=(offer.media_tracks||[]).map(t=>t.camera_id);
  offeredCameraByMid=offeredVideoCameraIds(offer.sdp,offer.media_tracks||[]);
  const nextPeer=new RTCPeerConnection({bundlePolicy:'max-bundle',iceServers,iceTransportPolicy:consoleConfig.ice_transport_policy});
  peer=nextPeer;
  webrtcLabel.textContent=`协商 ${offer.codec||''}/${offer.backend||''}`;
  nextPeer.onconnectionstatechange=()=>{
    if(peer!==nextPeer)return;
    const connectionState=nextPeer.connectionState;
    webrtcLabel.textContent=connectionState;
    if(connectionState==='disconnected'){
      resetControlAuthorityInput();
      clientLog('webrtc_peer_disconnected');
    }
    if(['failed','closed'].includes(connectionState)){
      controlChannel=null;
      resetControlAuthorityInput();
    }
    if(connectionState==='connected'&&controlChannel?.readyState==='open')webrtcLabel.textContent='控制链路已连接';
    renderMonitoring();
  };
  nextPeer.onicecandidateerror=e=>clientLog('webrtc_ice_candidate_error',{endpoint:safeIceEndpoint(e.url),error_code:Number(e.errorCode||0)});
  nextPeer.ondatachannel=e=>{
    const channel=e.channel;
    if(!controlLogic.isCurrentPeer(peer,nextPeer)){channel.close();return}
    if(channel.label!=='control'||channel.protocol!=='mine-teleop-control-v1'||channel.ordered||channel.maxRetransmits!==0){
      channel.close();
      webrtcLabel.textContent='控制通道参数非法';
      clientLog('control_datachannel_rejected',{label:channel.label,protocol:channel.protocol,ordered:channel.ordered,max_retransmits:channel.maxRetransmits});
      return;
    }
    lastControlStatusSeq=0;
    controlChannel=channel;
    channel.bufferedAmountLowThreshold=1024;
    channel.onopen=()=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      webrtcLabel.textContent='控制链路已连接';
      resetControlAuthorityInput();
      vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:true,adapter_ready:null};
      clientLog('control_datachannel_open');
      renderMonitoring();
    };
    channel.onmessage=async event=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      try{
        const message=JSON.parse(event.data);
        if(!['vehicle_telemetry','vcu_handshake_status'].includes(message.event))return;
        if(!acceptControlStatusMessage(message))return;
        if(message.event==='vehicle_telemetry'){
          vehicleTelemetry=message;
          if(message.vcu_handshake){const nextVcuStatus={...message.vcu_handshake,driver_connected:true,adapter_ready:vcuAdapterReady(message.vcu_handshake,message.vehicle_adapter?.opened)};updateVcuHandshakeState(nextVcuStatus)}
          renderEstopRequest();
          await updateVehicleHardLimits(message.control_limits);
          renderMonitoring();
          return;
        }
        if(message.event!=='vcu_handshake_status')return;
        const nextVcuStatus=message.status||{};
        updateVcuHandshakeState({...nextVcuStatus,driver_connected:Boolean(message.driver_connected),adapter_ready:vcuAdapterReady(nextVcuStatus,message.adapter_ready)});
        await updateVehicleHardLimits(message.hard_limits);
        if(vcuHandshake.adapter_ready===false)statusPanel.textContent='VCU 适配器明确不可用；视频保持在线，驾驶命令已阻止';
        else if(vcuHandshake.adapter_ready===null)statusPanel.textContent='VCU 适配器状态未确认；视频保持在线，驾驶命令已阻止';
        else if(message.result==='command_rejected')statusPanel.textContent='开始握手失败：'+diagnoseVcuHandshake(true).text;
        else if(vcuDrivingReady())statusPanel.textContent='VCU 平行驾驶握手成功，可以发送驾驶命令';
        else if(vcuHandshake.state==='disarmed')statusPanel.textContent='VCU 握手已安全断开';
        clientLog('driver_vcu_handshake_status',{result:message.result,state:vcuHandshake.state,ready:Boolean(vcuHandshake.ready),parking_ready:Boolean(vcuHandshake.parking_ready)});
        renderMonitoring();
      }catch(error){clientLog('control_datachannel_message_invalid',{error:error.message})}
    };
    channel.onclose=()=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      controlChannel=null;
      resetControlAuthorityInput();
      vehicleTelemetry=null;
      vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};
      webrtcLabel.textContent='控制链路中断';
      clientLog('control_datachannel_closed');
      renderMonitoring();
    };
    channel.onerror=()=>{
      if(!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      resetControlAuthorityInput();
      webrtcLabel.textContent='控制链路错误';
      renderMonitoring();
    };
  };
  nextPeer.onicecandidate=e=>{if(e.candidate)post('/api/webrtc/ice-candidate',{candidate:e.candidate.toJSON()}).catch(console.error)};
  nextPeer.ontrack=e=>{const mid=e.transceiver.mid||'',id=offeredCameraByMid.get(mid)||remoteCameraIds.find(cameraId=>!assignedCameraIds.has(cameraId))||mid||e.track.id;assignedCameraIds.add(id);cameraByMid.set(mid,id);attach(id,e.track)};
  await nextPeer.setRemoteDescription({type:'offer',sdp:offer.sdp});
  while(pendingIce.length)await addIce(pendingIce.shift());
  const answer=await nextPeer.createAnswer();
  await nextPeer.setLocalDescription(answer);
  await post('/api/webrtc/answer',{type:'answer',sdp:nextPeer.localDescription.sdp});
}
async function collectMetrics(){
  if(!peer)return;
  const report=await peer.getStats(),sampledAt=Date.now();
  let rtt=0,connectionMethod='unknown',turnInUse=false,selectedPair=null;
  for(const s of report.values())if(s.type==='candidate-pair'&&s.state==='succeeded'&&(s.nominated||!selectedPair))selectedPair=s;
  if(selectedPair){rtt=Number(selectedPair.currentRoundTripTime||0);const local=report.get(selectedPair.localCandidateId),remote=report.get(selectedPair.remoteCandidateId),types=[local?.candidateType,remote?.candidateType];turnInUse=types.includes('relay');connectionMethod=turnInUse?'TURN':(types.some(type=>type==='srflx'||type==='prflx')?'STUN':'direct')}
  const streams=[];
  for(const s of report.values()){
    if(s.type!=='inbound-rtp'||(s.kind||s.mediaType)!=='video')continue;
    const statsKey=s.mid||String(s.ssrc||s.id),prior=previousStats.get(statsKey),decoded=Number(s.framesDecoded||0),bytesReceived=Number(s.bytesReceived||0),packetsLost=Number(s.packetsLost||0),packetsReceived=Number(s.packetsReceived||0);
    let fps=Number(s.framesPerSecond||0),bitrateKbps=0;
    if(prior){const seconds=(sampledAt-prior.sampledAt)/1000;if(seconds>0){if(!fps)fps=(decoded-prior.framesDecoded)/seconds;bitrateKbps=Math.max(0,(bytesReceived-prior.bytesReceived)*8/seconds/1000)}}
    previousStats.set(statsKey,{sampledAt,framesDecoded:decoded,bytesReceived});
    const jitterMs=Number(s.jitterBufferEmittedCount||0)>0?Number(s.jitterBufferDelay||0)*1000/Number(s.jitterBufferEmittedCount):0;
    const processingMs=decoded>0?Number(s.totalProcessingDelay||0)*1000/decoded:0;
    const cameraId=cameraByMid.get(s.mid||'')||'',lane=(mediaStatus.lanes||[]).find(l=>l.camera_id===cameraId)||{};
    const captureEncodeMs=Number(lane.capture_to_encoded_ms||0),latencyMs=captureEncodeMs+rtt*500+jitterMs+processingMs;
    streams.push({camera_id:cameraId,mid:s.mid||'',codec_id:s.codecId||'',fps,bitrate_kbps:bitrateKbps,frames_decoded:decoded,frames_dropped:Number(s.framesDropped||0),packets_lost:packetsLost,packets_received:packetsReceived,packet_loss_percent:(packetsLost+packetsReceived)>0?100*packetsLost/(packetsLost+packetsReceived):0,jitter_ms:Number(s.jitter||0)*1000,capture_to_encoded_ms:captureEncodeMs,jitter_buffer_ms:jitterMs,processing_ms:processingMs,round_trip_ms:rtt*1000,estimated_end_to_end_latency_ms:latencyMs,passed:fps>=20&&latencyMs<=200})
  }
  const timeSync=latestRuntimeStatus.time_sync||mediaStatus.time_sync||{},turnConfigured=hasTurnServer();
  const controlOutcomes={...controlOutcomeSession.metrics};
  const metrics={sampled_at_ms:sampledAt,connection_state:peer.connectionState,codec:mediaStatus.codec||'',backend:mediaStatus.backend||'',control_rtt_ms:rtt*1000,connection_method:connectionMethod,turn_configured:turnConfigured,turn_in_use:turnInUse,time_sync:timeSync,clock_uncertainty_ms:Number(timeSync.uncertainty_ms||0),latency_method:'capture-to-encoded + rtt/2 + jitter-buffer + browser-processing',control_outcomes:controlOutcomes,control_outcomes_balanced:controlLogic.controlOutcomesBalanced(controlOutcomes),streams,passed:streams.length>0&&streams.every(s=>s.passed)};
  await post('/api/webrtc/metrics',metrics);
  if(metrics.codec==='h265'&&metrics.connection_state==='connected'&&streams.length){h265FailureSamples=streams.some(s=>s.fps<20)?h265FailureSamples+1:0;if(h265FailureSamples>=3&&!h265FallbackSent){h265FallbackSent=true;await post('/api/webrtc/fallback',{codec:'h264',reason:'h265_decode_fps_below_20'})}}else h265FailureSamples=0;
  latestMetrics=metrics;renderMonitoring();statusPanel.textContent=`${metrics.connection_state||'等待连接'} · ${streams.length} 路视频 · RTT ${formatMetric(metrics.control_rtt_ms,1,' ms')} · ${metrics.connection_method||'unknown'}`
}
setInterval(()=>collectMetrics().catch(console.error),1000);
setInterval(()=>refreshRuntimeStatus().catch(console.error),1000);
setInterval(()=>heartbeat().catch(console.error),50);
setInterval(()=>{if(authenticated&&!connecting)refreshVehicles().catch(handleVehicleRefreshError)},5000);
</script></body></html>)HTML";
}

Json keyboard_to_control(const Json& payload, double service_brake_limit, double hard_brake_limit) {
  if (!payload.is_object()) throw std::invalid_argument("keyboard control payload must be an object");
  const auto flag = [&](std::string_view name) {
    const auto key = std::string(name);
    if (!payload.contains(key)) return false;
    if (!payload.at(key).is_boolean()) {
      throw std::invalid_argument("keyboard control " + key + " must be a boolean");
    }
    return payload.at(key).get<bool>();
  };
  const bool left = flag("left");
  const bool right = flag("right");
  const bool up = flag("up");
  const bool down = flag("down");
  const bool service_brake = flag("service_brake");
  const bool hard_brake_key = flag("hard_brake");
  const bool legacy_brake = flag("brake");
  const bool hard_brake = hard_brake_key || legacy_brake;
  const bool braking = service_brake || hard_brake;
  const bool driving = up != down;
  return {
      {"gear", driving ? (up ? "D" : "R") : "N"},
      {"steering", left == right ? 0.0 : (left ? -1.0 : 1.0)},
      {"throttle", driving && !braking ? 0.35 : 0.0},
      {"brake", hard_brake ? hard_brake_limit : (service_brake ? service_brake_limit : 0.0)},
      {"estop", flag("estop")},
  };
}

}  // namespace

SignalingServerConfig load_signaling_identity_config(const std::filesystem::path& path) {
  if (path.empty()) throw std::invalid_argument("signaling identity configuration path is required");
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument("cannot load signaling identity configuration: " + std::string(error.what()));
  }
  if (!root || !root.IsMap()) throw std::invalid_argument("signaling identity configuration must be a mapping");
  const auto auth = root["auth"];
  if (!auth || !auth.IsMap()) throw std::invalid_argument("auth mapping is required");
  const auto drivers = auth["drivers"];
  const auto vehicles = auth["vehicles"];
  if (!drivers || !drivers.IsSequence() || drivers.size() == 0) {
    throw std::invalid_argument("auth.drivers must be a non-empty sequence");
  }
  if (!vehicles || !vehicles.IsSequence() || vehicles.size() == 0) {
    throw std::invalid_argument("auth.vehicles must be a non-empty sequence");
  }

  SignalingServerConfig config;
  config.driver_passwords.clear();
  config.device_tokens.clear();
  config.driver_vehicle_permissions.clear();
  const auto base_path = std::filesystem::absolute(path).parent_path();
  for (std::size_t index = 0; index < drivers.size(); ++index) {
    const auto entry = drivers[index];
    const auto context = "auth.drivers[" + std::to_string(index) + "]";
    const auto driver_id = required_yaml_string(entry, "id", context);
    const auto password = load_identity_secret(
        entry, "password_file", "password_env", base_path, context);
    if (!config.driver_passwords.emplace(driver_id, password).second) {
      throw std::invalid_argument("duplicate driver id: " + driver_id);
    }
    const auto allowed = entry["vehicles"];
    if (!allowed || !allowed.IsSequence() || allowed.size() == 0) {
      throw std::invalid_argument(context + ".vehicles must be a non-empty sequence");
    }
    auto& permissions = config.driver_vehicle_permissions[driver_id];
    for (std::size_t permission_index = 0; permission_index < allowed.size(); ++permission_index) {
      std::string vehicle_id;
      try {
        vehicle_id = allowed[permission_index].as<std::string>();
      } catch (const YAML::Exception& error) {
        throw std::invalid_argument(context + ".vehicles entries must be strings: " + error.what());
      }
      if (vehicle_id.empty()) throw std::invalid_argument(context + ".vehicles entries must not be empty");
      if (!permissions.insert(vehicle_id).second) {
        throw std::invalid_argument("duplicate vehicle permission for driver " + driver_id + ": " + vehicle_id);
      }
    }
  }
  for (std::size_t index = 0; index < vehicles.size(); ++index) {
    const auto entry = vehicles[index];
    const auto context = "auth.vehicles[" + std::to_string(index) + "]";
    const auto vehicle_id = required_yaml_string(entry, "id", context);
    const auto token = load_identity_secret(
        entry, "device_token_file", "device_token_env", base_path, context);
    if (!config.device_tokens.emplace(vehicle_id, token).second) {
      throw std::invalid_argument("duplicate vehicle id: " + vehicle_id);
    }
  }
  for (const auto& [driver_id, permissions] : config.driver_vehicle_permissions) {
    for (const auto& vehicle_id : permissions) {
      if (!config.device_tokens.contains(vehicle_id)) {
        throw std::invalid_argument(
            "driver " + driver_id + " references an unknown vehicle: " + vehicle_id);
      }
    }
  }
  return config;
}

Json HttpRequest::json_body() const {
  if (body.empty()) return Json::object();
  try {
    auto value = Json::parse(body);
    if (!value.is_object()) throw std::invalid_argument("JSON body must be an object");
    return value;
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid JSON body: ") + error.what());
  }
}

ServerResponse ServerResponse::json(int status, const Json& value) {
  return ServerResponse{status, "application/json; charset=utf-8", value.dump(), {}};
}

ServerResponse ServerResponse::text(int status, std::string body, std::string content_type) {
  return ServerResponse{status, std::move(content_type), std::move(body), {}};
}

SimpleHttpServer::SimpleHttpServer(
    std::string host,
    std::uint16_t port,
    Handler handler,
    std::size_t max_body_bytes,
    WebSocketHandler websocket_handler)
    : host_(std::move(host)),
      requested_port_(port),
      handler_(std::move(handler)),
      max_body_bytes_(max_body_bytes),
      websocket_handler_(std::move(websocket_handler)) {
  if (host_.empty()) throw std::invalid_argument("HTTP host must not be empty");
  if (!handler_) throw std::invalid_argument("HTTP handler is required");
  if (max_body_bytes_ == 0) throw std::invalid_argument("HTTP max body size must be positive");
}

SimpleHttpServer::~SimpleHttpServer() { stop(); }

void SimpleHttpServer::open_listener() {
  if (listener_fd_ != kInvalidSocket) return;
  initialize_network_process();
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  const auto service = std::to_string(requested_port_);
  const int resolve = ::getaddrinfo(host_.c_str(), service.c_str(), &hints, &addresses);
  if (resolve != 0) {
    throw std::runtime_error("cannot resolve HTTP bind address: " + address_error_message(resolve));
  }
  int saved_error = 0;
  for (auto* address = addresses; address != nullptr; address = address->ai_next) {
    const auto candidate = ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
#if defined(_WIN32)
    if (candidate == INVALID_SOCKET) {
#else
    if (candidate < 0) {
#endif
      saved_error = last_socket_error();
      continue;
    }
    listener_fd_ = static_cast<SocketHandle>(candidate);
    configure_listener_socket(listener_fd_);
    if (::bind(native_socket(listener_fd_), address->ai_addr, address->ai_addrlen) == 0 &&
        ::listen(native_socket(listener_fd_), 64) == 0) {
      break;
    }
    saved_error = last_socket_error();
    close_socket(listener_fd_);
    listener_fd_ = kInvalidSocket;
  }
  ::freeaddrinfo(addresses);
  if (listener_fd_ == kInvalidSocket) {
    throw std::runtime_error(
        "cannot bind HTTP listener: " + socket_error_message(saved_error));
  }

  sockaddr_storage bound{};
  SocketLength length = sizeof(bound);
  if (::getsockname(native_socket(listener_fd_), reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
    throw std::runtime_error(
        "getsockname failed: " + socket_error_message(last_socket_error()));
  }
  if (bound.ss_family == AF_INET) bound_port_ = ntohs(reinterpret_cast<sockaddr_in*>(&bound)->sin_port);
  if (bound.ss_family == AF_INET6) bound_port_ = ntohs(reinterpret_cast<sockaddr_in6*>(&bound)->sin6_port);
}

void SimpleHttpServer::serve_client(SocketHandle client_fd) const {
  ServerResponse response;
  try {
    auto request = parse_request(client_fd, max_body_bytes_);
    request.peer_address = socket_peer_address(client_fd);
    if (websocket_handler_ && websocket_handler_(client_fd, request)) return;
    response = handler_(request);
  } catch (const std::length_error& error) {
    response = ServerResponse::json(413, {{"error", error.what()}});
  } catch (const std::invalid_argument& error) {
    response = ServerResponse::json(400, {{"error", error.what()}});
  } catch (const std::exception& error) {
    response = ServerResponse::json(500, {{"error", error.what()}});
  }
  try {
    send_http_response(client_fd, response);
  } catch (const std::exception&) {
  }
}

void SimpleHttpServer::serve_forever() {
  open_listener();
  while (!stopping_) {
    const auto accepted = ::accept(native_socket(listener_fd_), nullptr, nullptr);
#if defined(_WIN32)
    if (accepted == INVALID_SOCKET) {
#else
    if (accepted < 0) {
#endif
      const int error = last_socket_error();
      if (socket_error_interrupted(error)) continue;
      if (stopping_ || socket_error_closed(error)) break;
      continue;
    }
    const auto client = static_cast<SocketHandle>(accepted);
    {
      std::lock_guard lock(clients_mutex_);
      client_sockets_.insert(client);
    }
    try {
      std::thread([this, client] {
        serve_client(client);
        shutdown_socket(client);
        close_socket(client);
        {
          std::lock_guard lock(clients_mutex_);
          client_sockets_.erase(client);
        }
        clients_stopped_.notify_all();
      }).detach();
    } catch (...) {
      {
        std::lock_guard lock(clients_mutex_);
        client_sockets_.erase(client);
      }
      shutdown_socket(client);
      close_socket(client);
      throw;
    }
  }
}

void SimpleHttpServer::start() {
  if (thread_.joinable()) throw std::runtime_error("HTTP server is already running");
  stopping_ = false;
  open_listener();
  thread_ = std::thread([this] { serve_forever(); });
}

void SimpleHttpServer::stop() {
  stopping_ = true;
  if (listener_fd_ != kInvalidSocket) {
    shutdown_socket(listener_fd_);
    close_socket(listener_fd_);
    listener_fd_ = kInvalidSocket;
  }
  if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) thread_.join();
  {
    std::lock_guard lock(clients_mutex_);
    for (const auto client : client_sockets_) shutdown_socket(client);
  }
  std::unique_lock lock(clients_mutex_);
  clients_stopped_.wait(lock, [this] { return client_sockets_.empty(); });
}

std::string random_token(std::size_t bytes) {
  if (bytes == 0 || bytes > 1024) throw std::invalid_argument("random token byte count is invalid");
  std::vector<unsigned char> value(bytes);
#if defined(__APPLE__)
  if (SecRandomCopyBytes(kSecRandomDefault, value.size(), value.data()) != errSecSuccess) {
    throw std::runtime_error("Security SecRandomCopyBytes failed");
  }
#else
  if (RAND_bytes(value.data(), static_cast<int>(value.size())) != 1) {
    throw std::runtime_error("OpenSSL RAND_bytes failed");
  }
#endif
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (const auto byte : value) output << std::setw(2) << static_cast<int>(byte);
  return output.str();
}

Json SignalingService::Session::to_json(bool include_control_token) const {
  Json value = {
      {"session_id", session_id},
      {"vehicle_id", vehicle_id},
      {"driver_id", driver_id},
      {"state", to_string(state)},
      {"turn_usage",
       {{"bytes_sent", relay_bytes_sent},
        {"bytes_received", relay_bytes_received},
        {"relay_bytes_total", relay_bytes_sent + relay_bytes_received},
        {"duration_ms", relay_duration_ms},
        {"sample_count", relay_usage_samples},
        {"last_bitrate_kbps", last_relay_bitrate_kbps}}},
  };
  if (include_control_token && !control_token.empty()) {
    value["control_token"] = control_token;
    value["control_token_expires_at_utc_ms"] = control_token_expires_at_ms;
  }
  return value;
}

Json SignalingService::Message::to_json() const {
  auto value = metadata.to_json();
  value["sender"] = sender;
  value["recipient"] = recipient;
  value["type"] = type;
  value["payload"] = payload;
  value["queued_at_utc_ms"] = queued_at_utc_ms;
  value["delivery_cursor"] = delivery_cursor;
  return value;
}

SignalingService::SignalingService(
    SignalingServerConfig config,
    std::function<std::int64_t()> audit_clock)
    : config_(std::move(config)),
      service_instance_id_("service-" + random_token(12)),
      audit_clock_(std::move(audit_clock)) {
  if (config_.token_ttl_ms <= 0) throw std::invalid_argument("driver token TTL must be positive");
  if (config_.control_token_ttl_ms <= 0) throw std::invalid_argument("control token TTL must be positive");
  if (config_.vehicle_heartbeat_timeout_ms <= 0 || config_.driver_heartbeat_timeout_ms <= 0 ||
      config_.connection_reaper_interval_ms <= 0) {
    throw std::invalid_argument("connection heartbeat and reaper intervals must be positive");
  }
  if (config_.login_max_failures <= 0 || config_.login_failure_window_ms <= 0 ||
      config_.login_lockout_ms <= 0) {
    throw std::invalid_argument("login failure limit, window, and lockout must be positive");
  }
  if (config_.api_rate_limit_requests <= 0 || config_.api_rate_limit_window_ms <= 0 ||
      config_.api_rate_limit_max_sources <= 0) {
    throw std::invalid_argument("API rate limit, window, and source capacity must be positive");
  }
  if (static_cast<std::uint64_t>(config_.api_rate_limit_max_sources) >
      std::numeric_limits<std::size_t>::max()) {
    throw std::invalid_argument("API rate-limit source capacity is too large");
  }
  if (config_.audit_log_max_bytes < 1024 || config_.audit_log_files < 1 || config_.audit_log_files > 20) {
    throw std::invalid_argument("audit log max bytes must be at least 1024 and files must be between 1 and 20");
  }
  if (config_.audit_log_rotation_interval_ms <= 0 ||
      config_.audit_log_rotation_interval_ms > 24 * 60 * 60 * std::int64_t{1000}) {
    throw std::invalid_argument("audit log rotation interval must be between 1ms and 24 hours");
  }
  if (config_.audit_log_retention_days < 1 || config_.audit_log_retention_days > 365) {
    throw std::invalid_argument("audit log retention must be between 1 and 365 days");
  }
  for (const auto& address : config_.trusted_proxy_addresses) {
    const auto canonical = canonical_ip_address(address);
    if (!canonical.has_value()) throw std::invalid_argument("trusted proxy address must be an IP address");
    trusted_proxy_addresses_.insert(*canonical);
  }
  if (config_.turn_credential_ttl_seconds <= 0) {
    throw std::invalid_argument("TURN credential TTL must be positive");
  }
  if (config_.max_signaling_payload_bytes == 0 || config_.max_sdp_bytes == 0 ||
      config_.max_ice_candidate_bytes == 0 || config_.signaling_message_ttl_ms <= 0) {
    throw std::invalid_argument("signaling limits and message TTL must be positive");
  }
  if (config_.stun_urls.empty() && config_.turn_urls.empty()) {
    throw std::invalid_argument("at least one STUN or TURN URL is required");
  }
  for (const auto& url : config_.stun_urls) {
    if (!valid_ice_url(url, false)) throw std::invalid_argument("invalid STUN URL");
  }
  for (const auto& url : config_.turn_urls) {
    if (!valid_ice_url(url, true)) throw std::invalid_argument("invalid TURN URL");
  }
  if (!config_.turn_urls.empty() &&
      (config_.turn_realm.empty() || config_.turn_static_auth_secret.empty())) {
    throw std::invalid_argument("TURN URLs require a realm and static auth secret");
  }
  if (config_.driver_passwords.empty()) throw std::invalid_argument("at least one driver credential is required");
  if (config_.device_tokens.empty()) throw std::invalid_argument("at least one device credential is required");
  for (const auto& [id, password] : config_.driver_passwords) {
    if (id.empty() || password.empty()) throw std::invalid_argument("driver credentials must not be empty");
  }
  for (const auto& [id, token] : config_.device_tokens) {
    if (id.empty() || token.empty()) throw std::invalid_argument("device credentials must not be empty");
  }
  for (const auto& [driver_id, vehicles] : config_.driver_vehicle_permissions) {
    if (!config_.driver_passwords.contains(driver_id)) {
      throw std::invalid_argument("vehicle permission references an unknown driver");
    }
    for (const auto& vehicle_id : vehicles) {
      if (!config_.device_tokens.contains(vehicle_id)) {
        throw std::invalid_argument("vehicle permission references an unknown vehicle");
      }
    }
  }
  audit(
      "signaling_service_started",
      {{"runtime", "cpp"},
       {"audit_log_max_bytes", config_.audit_log_max_bytes},
       {"audit_log_files", config_.audit_log_files},
       {"audit_log_rotation_interval_ms", config_.audit_log_rotation_interval_ms},
       {"audit_log_retention_days", config_.audit_log_retention_days}});
  connection_reaper_ = std::jthread([this](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(config_.connection_reaper_interval_ms));
      if (stop_token.stop_requested()) break;
      std::lock_guard lock(mutex_);
      cleanup_expired_connections(now_ms());
    }
  });
}

SignalingService::~SignalingService() {
  connection_reaper_.request_stop();
  if (connection_reaper_.joinable()) connection_reaper_.join();
}

Json SignalingService::health() const {
  std::lock_guard lock(mutex_);
  const auto timestamp_ms = now_ms();
  const auto active_sessions = std::count_if(sessions_.begin(), sessions_.end(), [](const auto& item) {
    return item.second.state == SessionState::Active || item.second.state == SessionState::Degraded;
  });
  std::uint64_t turn_relay_bytes_total = 0;
  std::size_t turn_usage_sessions = 0;
  for (const auto& [id, session] : sessions_) {
    static_cast<void>(id);
    if (session.relay_usage_samples == 0) continue;
    ++turn_usage_sessions;
    const auto session_total = session.relay_bytes_sent + session.relay_bytes_received;
    turn_relay_bytes_total = session_total > std::numeric_limits<std::uint64_t>::max() - turn_relay_bytes_total
        ? std::numeric_limits<std::uint64_t>::max()
        : turn_relay_bytes_total + session_total;
  }
  const auto login_locked_buckets = std::count_if(login_failures_.begin(), login_failures_.end(), [&](const auto& item) {
    return item.second.blocked_until_ms > timestamp_ms;
  });
  const bool api_rate_limit_overflow_active =
      api_rate_limit_overflow_.window_started_at_ms > 0 &&
      timestamp_ms - api_rate_limit_overflow_.window_started_at_ms < config_.api_rate_limit_window_ms;
  Json alerts = Json::array();
  if (login_locked_buckets > 0) {
    alerts.push_back({
        {"code", "login_lockout_active"},
        {"severity", "warning"},
        {"count", login_locked_buckets},
    });
  }
  if (api_rate_limit_overflow_active) {
    alerts.push_back({
        {"code", "api_rate_limit_source_capacity"},
        {"severity", "warning"},
        {"count", 1},
    });
  }
  const auto alert_count = alerts.size();
  return {
      {"status", alerts.empty() ? "ok" : "degraded"},
      {"runtime", "cpp"},
      {"service_instance_id", service_instance_id_},
      {"alerts", std::move(alerts)},
      {"alert_count", alert_count},
      {"online_vehicles", online_vehicles_.size()},
      {"online_drivers", online_drivers_.size()},
      {"active_sessions", active_sessions},
      {"sessions", sessions_.size()},
      {"revoked_vehicles", revoked_vehicles_.size()},
      {"revoked_drivers", revoked_drivers_.size()},
      {"login_locked_buckets", login_locked_buckets},
      {"api_rate_limit_tracked_sources", api_rate_limits_.size()},
      {"api_rate_limit_overflow_active", api_rate_limit_overflow_active},
      {"api_rate_limited_requests", api_rate_limited_requests_},
      {"turn_usage_sessions", turn_usage_sessions},
      {"turn_relay_bytes_total", turn_relay_bytes_total},
  };
}

const SignalingService::Session& SignalingService::require_active_session(std::string_view session_id) const {
  const auto found = sessions_.find(std::string(session_id));
  if (found == sessions_.end()) throw NotFound("unknown session");
  if (found->second.state != SessionState::Active) {
    throw Conflict("session is not active", "session_not_active");
  }
  return found->second;
}

const SignalingService::Session& SignalingService::require_participant(
    std::string_view session_id, std::string_view participant) const {
  const auto& session = require_active_session(session_id);
  if (participant != session.driver_id && participant != session.vehicle_id) {
    throw Unauthorized("actor is not current session participant");
  }
  return session;
}

void SignalingService::validate_driver_token(std::string_view driver_id, std::string_view token) {
  if (revoked_drivers_.contains(std::string(driver_id))) throw Unauthorized("driver is revoked");
  const auto found = driver_tokens_.find(std::string(token));
  if (token.empty() || found == driver_tokens_.end() || found->second.driver_id != driver_id) {
    throw Unauthorized("invalid driver token");
  }
  if (now_ms() >= found->second.expires_at_ms) throw Unauthorized("driver token expired");
  const auto presence = online_drivers_.find(std::string(driver_id));
  if (presence == online_drivers_.end() || presence->second.generation != found->second.connection_generation) {
    throw Unauthorized("driver connection is no longer current");
  }
  presence->second.last_seen_at_ms = now_ms();
}

void SignalingService::validate_device_token(std::string_view vehicle_id, std::string_view token) const {
  if (revoked_vehicles_.contains(std::string(vehicle_id))) throw Unauthorized("vehicle is revoked");
  const auto found = config_.device_tokens.find(std::string(vehicle_id));
  if (token.empty() || found == config_.device_tokens.end() || found->second != token) {
    throw Unauthorized("invalid device token");
  }
}

void SignalingService::validate_vehicle_connection(
    std::string_view vehicle_id,
    std::string_view token,
    std::uint64_t connection_generation) {
  validate_device_token(vehicle_id, token);
  const auto found = online_vehicles_.find(std::string(vehicle_id));
  if (found == online_vehicles_.end()) throw Conflict("vehicle is offline", "vehicle_offline");
  if (connection_generation == 0 || found->second.generation != connection_generation) {
    throw Conflict(
        "vehicle connection generation is stale",
        "vehicle_connection_generation_stale");
  }
  found->second.last_seen_at_ms = now_ms();
}

void SignalingService::validate_actor_credential(const Session& session, std::string_view actor, const Json& value) {
  if (actor == session.driver_id) {
    validate_driver_token(actor, optional_string(value, "token"));
  } else if (actor == session.vehicle_id) {
    validate_vehicle_connection(
        actor,
        optional_string(value, "device_token"),
        required_uint64(value, "connection_generation"));
  } else {
    throw Unauthorized("actor is not current session participant");
  }
}

void SignalingService::close_sessions_for_vehicle(std::string_view vehicle_id, std::string_view reason) {
  for (auto& [id, session] : sessions_) {
    static_cast<void>(id);
    if (session.vehicle_id == vehicle_id && session.state != SessionState::Closed) close_session(session, reason);
  }
}

void SignalingService::close_sessions_for_driver(std::string_view driver_id, std::string_view reason) {
  for (auto& [id, session] : sessions_) {
    static_cast<void>(id);
    if (session.driver_id == driver_id && session.state != SessionState::Closed) close_session(session, reason);
  }
}

void SignalingService::cleanup_expired_connections(std::int64_t timestamp_ms) {
  for (auto token = driver_tokens_.begin(); token != driver_tokens_.end();) {
    if (timestamp_ms < token->second.expires_at_ms) {
      ++token;
      continue;
    }
    const auto driver_id = token->second.driver_id;
    const auto generation = token->second.connection_generation;
    token = driver_tokens_.erase(token);
    const auto presence = online_drivers_.find(driver_id);
    if (presence == online_drivers_.end() || presence->second.generation != generation) continue;
    online_drivers_.erase(presence);
    close_sessions_for_driver(driver_id, "driver_token_expired");
    audit(
        "driver_offline",
        {{"driver_id", driver_id},
         {"connection_generation", generation},
         {"reason", "token_expired"}});
  }

  for (auto& [id, session] : sessions_) {
    static_cast<void>(id);
    if (session.state != SessionState::Closed && session.control_token_expires_at_ms > 0 &&
        timestamp_ms >= session.control_token_expires_at_ms) {
      close_session(session, "control_token_expired");
      audit("control_authority_expired", session.to_json());
    }
  }

  for (auto iterator = online_vehicles_.begin(); iterator != online_vehicles_.end();) {
    if (timestamp_ms - iterator->second.last_seen_at_ms < config_.vehicle_heartbeat_timeout_ms) {
      ++iterator;
      continue;
    }
    const auto vehicle_id = iterator->first;
    const auto generation = iterator->second.generation;
    iterator = online_vehicles_.erase(iterator);
    close_sessions_for_vehicle(vehicle_id, "vehicle_heartbeat_timeout");
    audit(
        "vehicle_offline",
        {{"vehicle_id", vehicle_id},
         {"connection_generation", generation},
         {"reason", "heartbeat_timeout"}});
  }

  for (auto iterator = online_drivers_.begin(); iterator != online_drivers_.end();) {
    if (timestamp_ms - iterator->second.last_seen_at_ms < config_.driver_heartbeat_timeout_ms) {
      ++iterator;
      continue;
    }
    const auto driver_id = iterator->first;
    const auto generation = iterator->second.generation;
    iterator = online_drivers_.erase(iterator);
    for (auto token = driver_tokens_.begin(); token != driver_tokens_.end();) {
      if (token->second.driver_id == driver_id && token->second.connection_generation == generation) {
        token = driver_tokens_.erase(token);
      } else {
        ++token;
      }
    }
    close_sessions_for_driver(driver_id, "driver_heartbeat_timeout");
    audit(
        "driver_offline",
        {{"driver_id", driver_id},
         {"connection_generation", generation},
         {"reason", "heartbeat_timeout"}});
  }
}

void SignalingService::validate_message_metadata(
    const Session& session,
    const ProtocolMetadata& metadata) {
  metadata.validate();
  if (metadata.vehicle_id != session.vehicle_id || metadata.driver_id != session.driver_id ||
      metadata.session_id != session.session_id) {
    throw Unauthorized("protocol metadata does not match current session");
  }
}

void SignalingService::transition_session(Session& session, SessionState next, std::string_view reason) {
  const auto previous = session.state;
  if (previous == next) return;
  const bool allowed =
      (previous == SessionState::Online && next == SessionState::Reserved) ||
      (previous == SessionState::Reserved &&
       (next == SessionState::Connecting || next == SessionState::Stopping || next == SessionState::Closed)) ||
      (previous == SessionState::Connecting &&
       (next == SessionState::Active || next == SessionState::Degraded || next == SessionState::Stopping ||
        next == SessionState::Closed)) ||
      (previous == SessionState::Active &&
       (next == SessionState::Degraded || next == SessionState::Stopping || next == SessionState::Closed)) ||
      (previous == SessionState::Degraded &&
       (next == SessionState::Active || next == SessionState::Stopping || next == SessionState::Closed)) ||
      (previous == SessionState::Stopping && next == SessionState::Closed);
  if (!allowed) {
    throw std::logic_error(
        "invalid session transition " + std::string(to_string(previous)) + " -> " + std::string(to_string(next)));
  }
  session.state = next;
  audit(
      "session_state_changed",
      {{"session_id", session.session_id},
       {"vehicle_id", session.vehicle_id},
       {"driver_id", session.driver_id},
       {"from", to_string(previous)},
       {"to", to_string(next)},
       {"reason", reason}});
}

void SignalingService::close_session(Session& session, std::string_view reason) {
  if (session.state == SessionState::Closed) return;
  session.control_token.clear();
  session.control_token_expires_at_ms = 0;
  messages_.erase(message_key(session.session_id, session.driver_id));
  messages_.erase(message_key(session.session_id, session.vehicle_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.driver_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.vehicle_id));
  next_delivery_cursors_.erase(message_key(session.session_id, session.driver_id));
  next_delivery_cursors_.erase(message_key(session.session_id, session.vehicle_id));
  if (session.state != SessionState::Stopping) transition_session(session, SessionState::Stopping, reason);
  transition_session(session, SessionState::Closed, reason);
}

void SignalingService::enforce_login_rate_limit(std::string_view driver_id, std::int64_t timestamp_ms) {
  const bool known_driver = config_.driver_passwords.contains(std::string(driver_id));
  const std::string bucket = known_driver ? "driver:" + std::string(driver_id) : "unknown";
  const auto found = login_failures_.find(bucket);
  if (found == login_failures_.end()) return;

  auto& state = found->second;
  if (state.blocked_until_ms > timestamp_ms) {
    throw TooManyRequests("too many login attempts", state.blocked_until_ms - timestamp_ms);
  }
  if (state.blocked_until_ms > 0 || timestamp_ms - state.window_started_at_ms >= config_.login_failure_window_ms) {
    login_failures_.erase(found);
  }
}

void SignalingService::record_login_failure(std::string_view driver_id, std::int64_t timestamp_ms) {
  const bool known_driver = config_.driver_passwords.contains(std::string(driver_id));
  const std::string bucket = known_driver ? "driver:" + std::string(driver_id) : "unknown";
  auto& state = login_failures_[bucket];
  if (state.window_started_at_ms == 0 ||
      timestamp_ms - state.window_started_at_ms >= config_.login_failure_window_ms) {
    state = LoginFailureState{0, timestamp_ms, 0};
  }
  ++state.failures;
  const bool lock_login = state.failures >= config_.login_max_failures;
  if (lock_login) {
    state.blocked_until_ms = config_.login_lockout_ms > std::numeric_limits<std::int64_t>::max() - timestamp_ms
        ? std::numeric_limits<std::int64_t>::max()
        : timestamp_ms + config_.login_lockout_ms;
  }
  const Json identity = known_driver
      ? Json{{"driver_id", std::string(driver_id)}, {"recognized_driver", true}}
      : Json{{"driver_id", "<unknown>"}, {"recognized_driver", false}};
  auto failed_details = identity;
  failed_details["failure_count"] = state.failures;
  failed_details["failure_limit"] = config_.login_max_failures;
  audit("driver_login_failed", failed_details);
  if (!lock_login) return;

  auto limited_details = identity;
  limited_details["failure_count"] = state.failures;
  limited_details["blocked_until_utc_ms"] = state.blocked_until_ms;
  audit("driver_login_rate_limited", limited_details);
  throw TooManyRequests("too many login attempts", config_.login_lockout_ms);
}

void SignalingService::clear_login_failures(std::string_view driver_id) {
  login_failures_.erase("driver:" + std::string(driver_id));
}

std::string SignalingService::request_source(const HttpRequest& request) const {
  const auto canonical_peer = canonical_ip_address(request.peer_address);
  const std::string peer = canonical_peer.value_or("unknown");
  if (!trusted_proxy_addresses_.contains(peer)) return peer;

  const auto forwarded = request.headers.find("x-forwarded-for");
  if (forwarded == request.headers.end()) return peer;
  const auto separator = forwarded->second.rfind(',');
  const auto candidate = separator == std::string::npos
      ? forwarded->second
      : forwarded->second.substr(separator + 1);
  return canonical_ip_address(candidate).value_or(peer);
}

void SignalingService::cleanup_api_rate_limits(std::int64_t timestamp_ms) {
  const auto expired = [&](const ApiRateState& state) {
    return state.window_started_at_ms == 0 || timestamp_ms < state.window_started_at_ms ||
        timestamp_ms - state.window_started_at_ms >= config_.api_rate_limit_window_ms;
  };
  std::erase_if(api_rate_limits_, [&](const auto& item) { return expired(item.second); });
  if (expired(api_rate_limit_overflow_)) api_rate_limit_overflow_ = {};
  api_rate_limit_last_cleanup_ms_ = timestamp_ms;
}

void SignalingService::enforce_api_rate_limit(const HttpRequest& request, std::int64_t timestamp_ms) {
  if (api_rate_limit_last_cleanup_ms_ == 0 || timestamp_ms < api_rate_limit_last_cleanup_ms_ ||
      timestamp_ms - api_rate_limit_last_cleanup_ms_ >= config_.api_rate_limit_window_ms) {
    cleanup_api_rate_limits(timestamp_ms);
  }

  const auto source = request_source(request);
  auto found = api_rate_limits_.find(source);
  const bool overflow = found == api_rate_limits_.end() &&
      api_rate_limits_.size() >= static_cast<std::size_t>(config_.api_rate_limit_max_sources);
  ApiRateState* state = nullptr;
  if (overflow) {
    state = &api_rate_limit_overflow_;
  } else if (found != api_rate_limits_.end()) {
    state = &found->second;
  } else {
    state = &api_rate_limits_.try_emplace(source).first->second;
  }

  if (state->window_started_at_ms == 0 || timestamp_ms < state->window_started_at_ms ||
      timestamp_ms - state->window_started_at_ms >= config_.api_rate_limit_window_ms) {
    *state = ApiRateState{0, timestamp_ms, false};
  }
  if (state->requests < std::numeric_limits<std::int64_t>::max()) ++state->requests;
  if (state->requests <= config_.api_rate_limit_requests) return;

  if (api_rate_limited_requests_ < std::numeric_limits<std::uint64_t>::max()) ++api_rate_limited_requests_;
  const auto elapsed = std::max<std::int64_t>(0, timestamp_ms - state->window_started_at_ms);
  const auto retry_after_ms = std::max<std::int64_t>(1, config_.api_rate_limit_window_ms - elapsed);
  if (!state->limit_audited) {
    state->limit_audited = true;
    audit(
        "api_rate_limited",
        {{"source_address", overflow ? "<overflow>" : source},
         {"request_limit", config_.api_rate_limit_requests},
         {"window_ms", config_.api_rate_limit_window_ms},
         {"overflow_bucket", overflow}});
  }
  throw TooManyRequests("API request rate limit exceeded", retry_after_ms);
}

void SignalingService::audit(std::string_view event, const Json& details) const {
  if (config_.audit_log_path.empty()) return;
  const auto timestamp_ms = audit_clock_ ? audit_clock_() : now_ms();
  const auto max_bytes = static_cast<std::uint64_t>(config_.audit_log_max_bytes);
  Json record = {
      {"event", event},
      {"sent_at_utc_ms", timestamp_ms},
      {"service_instance_id", service_instance_id_},
      {"details", sanitize_log_value(details)}};
  if (!active_request_id.empty()) record["request_id"] = active_request_id;
  const auto line = record.dump();
  if (static_cast<std::uint64_t>(line.size()) >= max_bytes) {
    throw std::runtime_error("signaling audit record exceeds configured maximum size");
  }
  std::lock_guard log_lock(audit_log_mutex_);
  const auto current_period =
      log_period_start(timestamp_ms, config_.audit_log_rotation_interval_ms);
  if (audit_log_period_start_ms_ < 0) {
    audit_log_period_start_ms_ = existing_log_period(
        config_.audit_log_path,
        config_.audit_log_rotation_interval_ms,
        current_period);
  }
  if (audit_log_period_start_ms_ != current_period) {
    archive_jsonl_period(
        config_.audit_log_path,
        audit_log_period_start_ms_,
        static_cast<int>(config_.audit_log_files));
    audit_log_period_start_ms_ = current_period;
  }
  if (audit_log_last_retention_period_ms_ != current_period) {
    prune_jsonl_periods(
        config_.audit_log_path,
        current_period,
        config_.audit_log_retention_days);
    audit_log_last_retention_period_ms_ = current_period;
  }
  rotate_jsonl_log(
      config_.audit_log_path,
      max_bytes,
      static_cast<int>(config_.audit_log_files),
      line.size() + 1);
  std::ofstream output(config_.audit_log_path, std::ios::app);
  if (!output) throw std::runtime_error("cannot append signaling audit log");
  output << line << '\n';
  output.flush();
  if (!output) throw std::runtime_error("cannot append signaling audit log");
}

ServerResponse SignalingService::handle(const HttpRequest& request) {
  RequestIdScope request_id("request-" + random_token(12));
  ServerResponse response;
  try {
    {
      std::lock_guard lock(mutex_);
      enforce_api_rate_limit(request, now_ms());
    }
    if (request.method == "GET") {
      response = handle_get(request);
    } else if (request.method == "POST") {
      response = handle_post(request);
    } else {
      response = ServerResponse::json(405, {{"error", "method not allowed"}});
    }
  } catch (const Unauthorized& error) {
    response = ServerResponse::json(401, {{"error", error.what()}});
  } catch (const TooManyRequests& error) {
    response = too_many_requests_response(error);
  } catch (const NotFound& error) {
    response = ServerResponse::json(404, {{"error", error.what()}});
  } catch (const Conflict& error) {
    Json body = {{"error", error.what()}, {"issue_code", error.issue_code()}};
    body.update(error.details());
    response = ServerResponse::json(409, std::move(body));
  } catch (const std::invalid_argument& error) {
    response = ServerResponse::json(400, {{"error", error.what()}});
  } catch (const Json::exception& error) {
    response = ServerResponse::json(400, {{"error", error.what()}});
  }
  if (request.path == "/auth/driver_login" &&
      std::none_of(response.headers.begin(), response.headers.end(), [](const auto& header) {
        return lower(header.first) == "cache-control";
      })) {
    response.headers.emplace_back("Cache-Control", "no-store");
  }
  add_request_id_header(response, request_id.value());
  return response;
}

bool SignalingService::handle_websocket(SocketHandle socket, const HttpRequest& request) {
  const auto parts = path_parts(request.path);
  if (parts.size() != 3 || parts[0] != "signaling" || parts[2] != "ws") return false;
  RequestIdScope request_id("request-" + random_token(12));
  auto reject = [&](int status, std::string message) {
    try {
      auto response = ServerResponse::json(status, {{"error", std::move(message)}});
      add_request_id_header(response, request_id.value());
      send_http_response(socket, response);
    } catch (const std::exception&) {
    }
    return true;
  };
  try {
    std::lock_guard lock(mutex_);
    enforce_api_rate_limit(request, now_ms());
  } catch (const TooManyRequests& error) {
    try {
      auto response = too_many_requests_response(error);
      add_request_id_header(response, request_id.value());
      send_http_response(socket, response);
    } catch (const std::exception&) {
    }
    return true;
  }
  if (request.method != "GET") return reject(405, "WebSocket endpoint requires GET");
  const auto participant = query_value(request, "participant");
  if (participant.empty()) return reject(400, "participant is required");

  Json credentials = {
      {"token", credential_value(request, "token", "x-mine-teleop-driver-token")},
      {"device_token", credential_value(request, "device_token", "x-mine-teleop-device-token")},
      {"connection_generation", query_value(request, "connection_generation")}};
  auto authenticate = [&] {
    std::lock_guard lock(mutex_);
    cleanup_expired_connections(now_ms());
    const auto& session = require_participant(parts[1], participant);
    validate_actor_credential(session, participant, credentials);
  };
  try {
    authenticate();
  } catch (const Unauthorized& error) {
    return reject(401, error.what());
  } catch (const NotFound& error) {
    return reject(404, error.what());
  } catch (const Conflict& error) {
    return reject(409, error.what());
  } catch (const std::exception& error) {
    return reject(400, error.what());
  }

  const auto upgrade = request.headers.find("upgrade");
  if (upgrade == request.headers.end() || lower(trim(upgrade->second)) != "websocket") {
    return reject(400, "WebSocket Upgrade header is required");
  }
  const auto connection = request.headers.find("connection");
  bool connection_upgrade = false;
  if (connection != request.headers.end()) {
    std::size_t start = 0;
    while (start <= connection->second.size()) {
      const auto end = connection->second.find(',', start);
      const auto token = lower(trim(connection->second.substr(
          start,
          end == std::string::npos ? std::string::npos : end - start)));
      if (token == "upgrade") connection_upgrade = true;
      if (end == std::string::npos) break;
      start = end + 1;
    }
  }
  if (!connection_upgrade) return reject(400, "Connection: Upgrade header is required");
  const auto version = request.headers.find("sec-websocket-version");
  if (version == request.headers.end() || trim(version->second) != "13") {
    return reject(400, "Sec-WebSocket-Version must be 13");
  }
  const auto key = request.headers.find("sec-websocket-key");
  if (key == request.headers.end() || trim(key->second).empty()) {
    return reject(400, "Sec-WebSocket-Key is required");
  }
  std::string accept;
  try {
    accept = websocket_accept_key(trim(key->second));
  } catch (const std::exception& error) {
    return reject(400, error.what());
  }

  try {
    send_all(
        socket,
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: " +
            accept + "\r\nX-Request-ID: " + request_id.value() + "\r\n\r\n");
    ServerWebSocketConnection connection(socket, config_.max_signaling_payload_bytes);
    std::uint64_t last_delivery_cursor_sent = 0;
    auto last_delivery_sent_at = std::chrono::steady_clock::time_point{};
    while (true) {
      Json pending = Json::array();
      try {
        std::lock_guard lock(mutex_);
        cleanup_expired_connections(now_ms());
        const auto& session = require_participant(parts[1], participant);
        validate_actor_credential(session, participant, credentials);
        pending = take_signaling_messages(parts[1], participant, {}, false);
      } catch (const std::exception& error) {
        connection.send_json({{"error", error.what()}, {"event", "signaling_authority_lost"}});
        connection.send_close(1008, "signaling authority lost");
        return true;
      }
      if (!pending.empty()) {
        const auto delivery_cursor = pending.back().value("delivery_cursor", std::uint64_t{0});
        const auto timestamp = std::chrono::steady_clock::now();
        if (delivery_cursor > last_delivery_cursor_sent ||
            timestamp - last_delivery_sent_at >= std::chrono::milliseconds(500)) {
          connection.send_json(
              {{"event", "signaling_messages"},
               {"delivery_cursor", delivery_cursor},
               {"messages", std::move(pending)}});
          last_delivery_cursor_sent = std::max(last_delivery_cursor_sent, delivery_cursor);
          last_delivery_sent_at = timestamp;
        }
      }

      WebSocketReceiveResult received;
      try {
        received = connection.receive_json(std::chrono::milliseconds(50));
      } catch (const std::exception& error) {
        connection.send_json({{"error", error.what()}, {"event", "websocket_protocol_error"}});
        connection.send_close(1002, "websocket protocol error");
        return true;
      }
      if (received.status == WebSocketReceiveStatus::Timeout) continue;
      if (received.status == WebSocketReceiveStatus::Closed) return true;
      try {
        if (received.message.value("event", "") == "signaling_delivery_ack") {
          const auto delivery_cursor = required_uint64(received.message, "delivery_cursor");
          if (delivery_cursor > last_delivery_cursor_sent) {
            throw std::invalid_argument("delivery acknowledgement exceeds the last delivered cursor");
          }
          std::size_t acknowledged = 0;
          {
            std::lock_guard lock(mutex_);
            cleanup_expired_connections(now_ms());
            const auto& session = require_participant(parts[1], participant);
            validate_actor_credential(session, participant, credentials);
            acknowledged = acknowledge_signaling_messages(parts[1], participant, delivery_cursor);
          }
          connection.send_json(
              {{"event", "signaling_delivery_acknowledged"},
               {"delivery_cursor", delivery_cursor},
               {"acknowledged", acknowledged}});
          continue;
        }
        Json acknowledgement;
        {
          std::lock_guard lock(mutex_);
          cleanup_expired_connections(now_ms());
          const auto& session = require_participant(parts[1], participant);
          validate_actor_credential(session, participant, credentials);
          acknowledgement = enqueue_signaling_message(parts[1], received.message, participant);
        }
        connection.send_json(acknowledgement);
      } catch (const std::exception& error) {
        connection.send_json({{"error", error.what()}, {"event", "signaling_message_rejected"}});
      }
    }
  } catch (const std::exception&) {
    return true;
  }
}

Json SignalingService::take_signaling_messages(
    std::string_view session_id,
    std::string_view recipient,
    std::string_view requested_types,
    bool consume) {
  Json values = Json::array();
  auto found = messages_.find(message_key(session_id, recipient));
  if (found != messages_.end()) {
    const auto timestamp_ms = now_ms();
    std::erase_if(found->second, [&](const auto& message) {
      return timestamp_ms - message.queued_at_utc_ms >= config_.signaling_message_ttl_ms;
    });
    if (found->second.empty()) {
      messages_.erase(found);
      found = messages_.end();
    }
  }
  if (found == messages_.end()) return values;
  if (requested_types.empty()) {
    for (const auto& message : found->second) values.push_back(message.to_json());
    if (consume) messages_.erase(found);
    return values;
  }

  std::vector<std::string> types;
  std::size_t start = 0;
  while (start <= requested_types.size()) {
    const auto end = requested_types.find(',', start);
    const auto value = trim(std::string(requested_types.substr(
        start,
        end == std::string_view::npos ? requested_types.size() - start : end - start)));
    if (!value.empty()) types.push_back(value);
    if (end == std::string_view::npos) break;
    start = end + 1;
  }
  std::vector<Message> remaining;
  for (const auto& message : found->second) {
    if (std::find(types.begin(), types.end(), message.type) != types.end()) {
      values.push_back(message.to_json());
    } else if (consume) {
      remaining.push_back(message);
    }
  }
  if (consume) {
    if (remaining.empty()) {
      messages_.erase(found);
    } else {
      found->second = std::move(remaining);
    }
  }
  return values;
}

std::size_t SignalingService::acknowledge_signaling_messages(
    std::string_view session_id,
    std::string_view recipient,
    std::uint64_t delivery_cursor) {
  auto found = messages_.find(message_key(session_id, recipient));
  if (found == messages_.end()) return 0;
  const auto previous_size = found->second.size();
  std::erase_if(found->second, [&](const auto& message) {
    return message.delivery_cursor <= delivery_cursor;
  });
  const auto acknowledged = previous_size - found->second.size();
  if (found->second.empty()) messages_.erase(found);
  return acknowledged;
}

Json SignalingService::enqueue_signaling_message(
    std::string_view session_id,
    const Json& value,
    std::optional<std::string_view> authenticated_actor) {
  const auto sender = required_string(value, "sender");
  const auto recipient = required_string(value, "recipient");
  const auto type = required_string(value, "type");
  static const std::vector<std::string> allowed{
      "webrtc_offer", "webrtc_answer", "ice_candidate", "media_capabilities", "media_fallback",
      "connection_status", "telemetry", "media_status", "session_event"};
  if (std::find(allowed.begin(), allowed.end(), type) == allowed.end()) {
    throw std::invalid_argument("unsupported signaling message type");
  }
  const auto& session = require_participant(session_id, sender);
  if (authenticated_actor.has_value()) {
    if (sender != authenticated_actor.value()) {
      throw Unauthorized("sender is not authenticated websocket participant");
    }
  } else {
    validate_actor_credential(session, sender, value);
  }
  const auto metadata = ProtocolMetadata::from_json(value);
  validate_message_metadata(session, metadata);
  if (recipient != session.driver_id && recipient != session.vehicle_id) {
    throw Unauthorized("recipient is not current session participant");
  }
  if (sender == recipient) throw Unauthorized("signaling messages must target the other session participant");
  const bool driver_to_vehicle = sender == session.driver_id && recipient == session.vehicle_id;
  const bool vehicle_to_driver = sender == session.vehicle_id && recipient == session.driver_id;
  if ((type == "media_capabilities" || type == "media_fallback" || type == "webrtc_answer") &&
      !driver_to_vehicle) {
    throw Unauthorized(type + " route is invalid");
  }
  if (type == "webrtc_offer" && !vehicle_to_driver) throw Unauthorized("webrtc_offer route is invalid");
  if (type == "ice_candidate" && !driver_to_vehicle && !vehicle_to_driver) {
    throw Unauthorized("ice_candidate route is invalid");
  }
  const auto payload = value.value("payload", Json::object());
  if (!payload.is_object()) throw std::invalid_argument("payload must be an object");
  if (payload.dump().size() > config_.max_signaling_payload_bytes) {
    throw std::invalid_argument("signaling payload exceeds configured limit");
  }
  if (type == "webrtc_offer" || type == "webrtc_answer") {
    const auto sdp = required_string(payload, "sdp");
    if (sdp.size() > config_.max_sdp_bytes) throw std::invalid_argument("WebRTC SDP exceeds configured limit");
    const auto expected_description = type == "webrtc_offer" ? "offer" : "answer";
    if (payload.value("type", "") != expected_description) {
      throw std::invalid_argument("WebRTC SDP type does not match signaling message type");
    }
  }
  if (type == "ice_candidate") {
    const auto candidate = required_string(payload, "candidate");
    if (candidate.size() > config_.max_ice_candidate_bytes) {
      throw std::invalid_argument("WebRTC ICE candidate exceeds configured limit");
    }
  }
  const auto sequence_key = message_key(session_id, sender);
  const auto fingerprint = recipient + "\n" + type + "\n" + metadata.to_json().dump() + "\n" + payload.dump();
  if (const auto accepted = last_accepted_messages_.find(sequence_key); accepted != last_accepted_messages_.end()) {
    if (metadata.seq < accepted->second.sequence) {
      throw Conflict(
          "signaling message sequence is older than the previous message",
          "signaling_sequence_older",
          {{"received_seq", metadata.seq}, {"last_accepted_seq", accepted->second.sequence}});
    }
    if (metadata.seq == accepted->second.sequence) {
      if (fingerprint != accepted->second.fingerprint) {
        throw Conflict(
            "signaling message sequence was reused with different content",
            "signaling_sequence_reused",
            {{"received_seq", metadata.seq}, {"last_accepted_seq", accepted->second.sequence}});
      }
      auto acknowledgement = accepted->second.acknowledgement;
      acknowledgement["duplicate"] = true;
      audit(
          "signaling_retry_acknowledged",
          {{"session_id", session_id},
           {"vehicle_id", metadata.vehicle_id},
           {"driver_id", metadata.driver_id},
           {"seq", metadata.seq},
           {"sender", sender},
           {"recipient", recipient},
           {"message_id", acknowledgement.value("message_id", "")}});
      return acknowledgement;
    }
  }
  const auto recipient_key = message_key(session_id, recipient);
  const auto delivery_cursor = ++next_delivery_cursors_[recipient_key];
  auto& queue = messages_[recipient_key];
  queue.push_back(Message{metadata, sender, recipient, type, payload, now_ms(), delivery_cursor});
  Json acknowledgement = {
      {"queued", queue.size()},
      {"event", "signaling_ack"},
      {"type", type},
      {"seq", metadata.seq},
      {"message_id", std::string(session_id) + ":" + sender + ":" + std::to_string(metadata.seq)},
      {"delivery_cursor", delivery_cursor},
      {"duplicate", false}};
  last_accepted_messages_[sequence_key] = AcceptedMessage{metadata.seq, fingerprint, acknowledgement};
  audit(
      type,
      {{"session_id", session_id},
       {"vehicle_id", metadata.vehicle_id},
       {"driver_id", metadata.driver_id},
       {"seq", metadata.seq},
       {"sender", sender},
       {"recipient", recipient},
       {"transport", authenticated_actor.has_value() ? "websocket" : "http"}});
  return acknowledgement;
}

ServerResponse SignalingService::handle_get(const HttpRequest& request) {
  if (request.path == "/health") return ServerResponse::json(200, health());
  if (request.path == "/time") {
    const auto server_receive_ms = now_ms();
    const auto encoded_client_send_ms = query_value(request, "client_send_ms");
    if (encoded_client_send_ms.empty()) throw std::invalid_argument("client_send_ms is required");
    std::size_t consumed = 0;
    std::int64_t client_send_ms = 0;
    try {
      client_send_ms = std::stoll(encoded_client_send_ms, &consumed);
    } catch (const std::exception&) {
      throw std::invalid_argument("client_send_ms must be an integer");
    }
    if (consumed != encoded_client_send_ms.size()) {
      throw std::invalid_argument("client_send_ms must be an integer");
    }
    return ServerResponse::json(
        200,
        {{"time_domain", "signaling_server"},
         {"client_send_ms", client_send_ms},
         {"server_receive_ms", server_receive_ms},
         {"server_send_ms", now_ms()}});
  }
  const auto parts = path_parts(request.path);
  std::lock_guard lock(mutex_);
  cleanup_expired_connections(now_ms());
  if (parts.size() == 3 && parts[0] == "drivers" && parts[2] == "vehicles") {
    const auto& driver_id = parts[1];
    validate_driver_token(
        driver_id,
        credential_value(request, "token", "x-mine-teleop-driver-token"));
    const auto permission = config_.driver_vehicle_permissions.find(driver_id);
    Json vehicles = Json::array();
    if (permission != config_.driver_vehicle_permissions.end()) {
      std::vector<std::string> vehicle_ids(permission->second.begin(), permission->second.end());
      std::sort(vehicle_ids.begin(), vehicle_ids.end());
      for (const auto& vehicle_id : vehicle_ids) {
        const bool revoked = revoked_vehicles_.contains(vehicle_id);
        const bool online = !revoked && online_vehicles_.contains(vehicle_id);
        const Session* active = nullptr;
        for (const auto& [session_id, session] : sessions_) {
          static_cast<void>(session_id);
          if (session.vehicle_id == vehicle_id && session.state != SessionState::Closed) {
            active = &session;
            break;
          }
        }
        vehicles.push_back(
            {{"vehicle_id", vehicle_id},
             {"state", revoked ? "revoked" : (active == nullptr ? (online ? "online" : "offline") : to_string(active->state))},
             {"online", online},
             {"controllable", online && active == nullptr},
             {"controlled_by", active == nullptr ? "" : active->driver_id},
             {"session_id", active == nullptr ? "" : active->session_id}});
      }
    }
    audit("authorized_vehicles_listed", {{"driver_id", driver_id}, {"vehicle_count", vehicles.size()}});
    return ServerResponse::json(200, {{"driver_id", driver_id}, {"vehicles", std::move(vehicles)}});
  }
  if (parts.size() == 3 && parts[0] == "signaling" && parts[2] == "messages") {
    const auto recipient = query_value(request, "recipient");
    if (recipient.empty()) throw std::invalid_argument("recipient is required");
    const auto& session = require_participant(parts[1], recipient);
    if (recipient == session.driver_id) {
      validate_driver_token(
          recipient,
          credential_value(request, "token", "x-mine-teleop-driver-token"));
    } else {
      validate_vehicle_connection(
          recipient,
          credential_value(request, "device_token", "x-mine-teleop-device-token"),
          required_uint64(Json{{"connection_generation", query_value(request, "connection_generation")}}, "connection_generation"));
    }
    return ServerResponse::json(
        200,
        {{"messages", take_signaling_messages(parts[1], recipient, query_value(request, "types"))}});
  }
  if (parts.size() == 3 && parts[0] == "vehicles" && parts[2] == "session") {
    const auto& vehicle_id = parts[1];
    validate_vehicle_connection(
        vehicle_id,
        credential_value(request, "device_token", "x-mine-teleop-device-token"),
        required_uint64(Json{{"connection_generation", query_value(request, "connection_generation")}}, "connection_generation"));
    for (const auto& [id, session] : sessions_) {
      static_cast<void>(id);
      if (session.vehicle_id == vehicle_id && session.state == SessionState::Active) {
        return ServerResponse::json(
            200,
            {{"vehicle_id", vehicle_id},
             {"session_id", session.session_id},
             {"driver_id", session.driver_id},
             {"state", to_string(session.state)},
             {"control_token", session.control_token},
             {"control_token_expires_at_utc_ms", session.control_token_expires_at_ms},
             {"connection_generation", online_vehicles_.at(vehicle_id).generation}});
      }
    }
    return ServerResponse::json(
        200,
        {{"vehicle_id", vehicle_id},
         {"session_id", ""},
         {"state", "online"},
         {"connection_generation", online_vehicles_.at(vehicle_id).generation}});
  }
  if (parts.size() == 2 && parts[0] == "sessions") {
    const auto actor = query_value(request, "actor");
    const auto& session = require_participant(parts[1], actor);
    Json credentials = {
        {"token", credential_value(request, "token", "x-mine-teleop-driver-token")},
        {"device_token", credential_value(request, "device_token", "x-mine-teleop-device-token")},
        {"connection_generation", query_value(request, "connection_generation")}};
    validate_actor_credential(session, actor, credentials);
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "ice_servers") {
    const auto actor = query_value(request, "actor");
    const auto& session = require_participant(parts[1], actor);
    Json credentials = {
        {"token", credential_value(request, "token", "x-mine-teleop-driver-token")},
        {"device_token", credential_value(request, "device_token", "x-mine-teleop-device-token")},
        {"connection_generation", query_value(request, "connection_generation")}};
    validate_actor_credential(session, actor, credentials);
    Json servers = Json::array();
    if (!config_.stun_urls.empty()) servers.push_back({{"urls", config_.stun_urls}});
    std::int64_t expires_at_utc_ms = 0;
    if (!config_.turn_urls.empty()) {
      const auto expires_at_seconds = now_ms() / 1000 + config_.turn_credential_ttl_seconds;
      const auto username = std::to_string(expires_at_seconds) + ":" + config_.turn_realm + ":" +
          session.session_id + ":" + std::string(actor);
      servers.push_back(
          {{"urls", config_.turn_urls},
           {"username", username},
           {"credential", turn_rest_credential(config_.turn_static_auth_secret, username)},
           {"credentialType", "password"}});
      expires_at_utc_ms = expires_at_seconds * 1000;
    }
    audit(
        "ice_servers_issued",
        {{"session_id", session.session_id},
         {"vehicle_id", session.vehicle_id},
         {"driver_id", session.driver_id},
         {"actor", actor},
         {"ice_server_count", servers.size()},
         {"turn_server_count", config_.turn_urls.size()},
         {"expires_at_utc_ms", expires_at_utc_ms}});
    return ServerResponse::json(
        200,
        {{"session_id", session.session_id},
         {"ice_servers", std::move(servers)},
         {"expires_at_utc_ms", expires_at_utc_ms}});
  }
  return ServerResponse::json(404, {{"error", "not found"}});
}

ServerResponse SignalingService::handle_post(const HttpRequest& request) {
  const auto value = request.json_body();
  const auto parts = path_parts(request.path);
  std::lock_guard lock(mutex_);
  cleanup_expired_connections(now_ms());
  if (request.path.starts_with("/admin/")) {
    if (config_.admin_token.empty()) throw Unauthorized("admin API is disabled");
    if (optional_string(value, "admin_token") != config_.admin_token) throw Unauthorized("invalid admin token");
    const auto object_id = required_string(value, "id");
    if (request.path == "/admin/revoke/driver") {
      if (!config_.driver_passwords.contains(object_id)) throw NotFound("unknown driver");
      revoked_drivers_.insert(object_id);
      close_sessions_for_driver(object_id, "driver_revoked");
      online_drivers_.erase(object_id);
      for (auto token = driver_tokens_.begin(); token != driver_tokens_.end();) {
        if (token->second.driver_id == object_id) {
          token = driver_tokens_.erase(token);
        } else {
          ++token;
        }
      }
      audit("driver_revoked", {{"driver_id", object_id}});
      return ServerResponse::json(200, {{"driver_id", object_id}, {"state", "revoked"}});
    }
    if (request.path == "/admin/restore/driver") {
      if (!config_.driver_passwords.contains(object_id)) throw NotFound("unknown driver");
      revoked_drivers_.erase(object_id);
      audit("driver_restored", {{"driver_id", object_id}});
      return ServerResponse::json(200, {{"driver_id", object_id}, {"state", "offline"}});
    }
    if (request.path == "/admin/revoke/vehicle") {
      if (!config_.device_tokens.contains(object_id)) throw NotFound("unknown vehicle");
      revoked_vehicles_.insert(object_id);
      close_sessions_for_vehicle(object_id, "vehicle_revoked");
      online_vehicles_.erase(object_id);
      audit("vehicle_revoked", {{"vehicle_id", object_id}});
      return ServerResponse::json(200, {{"vehicle_id", object_id}, {"state", "revoked"}});
    }
    if (request.path == "/admin/restore/vehicle") {
      if (!config_.device_tokens.contains(object_id)) throw NotFound("unknown vehicle");
      revoked_vehicles_.erase(object_id);
      audit("vehicle_restored", {{"vehicle_id", object_id}});
      return ServerResponse::json(200, {{"vehicle_id", object_id}, {"state", "offline"}});
    }
    return ServerResponse::json(404, {{"error", "not found"}});
  }
  if (request.path == "/auth/driver_login") {
    const auto driver_id = required_string(value, "driver_id");
    const auto password = optional_string(value, "password");
    const auto timestamp_ms = now_ms();
    enforce_login_rate_limit(driver_id, timestamp_ms);
    const auto found = config_.driver_passwords.find(driver_id);
    if (found == config_.driver_passwords.end() || found->second != password) {
      record_login_failure(driver_id, timestamp_ms);
      throw Unauthorized("invalid driver credentials");
    }
    clear_login_failures(driver_id);
    if (revoked_drivers_.contains(driver_id)) {
      audit("driver_login_rejected", {{"driver_id", driver_id}, {"reason", "driver_revoked"}});
      throw Unauthorized("driver is revoked");
    }
    if (online_drivers_.contains(driver_id)) {
      audit("driver_login_rejected", {{"driver_id", driver_id}, {"reason", "driver_already_online"}});
      throw Conflict("driver is already online");
    }
    const auto generation = ++connection_generation_;
    const std::string token = "driver-token-" + random_token();
    driver_tokens_[token] = DriverToken{driver_id, timestamp_ms + config_.token_ttl_ms, generation};
    online_drivers_[driver_id] = ConnectionPresence{"", generation, timestamp_ms, timestamp_ms};
    audit("driver_login", {{"driver_id", driver_id}, {"connection_generation", generation}});
    return ServerResponse::json(
        200,
        {{"token_type", "bearer"},
         {"token", token},
         {"expires_at_ms", driver_tokens_.at(token).expires_at_ms},
         {"connection_generation", generation},
         {"service_instance_id", service_instance_id_}});
  }
  if (request.path == "/auth/driver_heartbeat") {
    const auto driver_id = required_string(value, "driver_id");
    validate_driver_token(driver_id, optional_string(value, "token"));
    const auto& presence = online_drivers_.at(driver_id);
    return ServerResponse::json(
        200,
        {{"driver_id", driver_id},
         {"state", "online"},
         {"connection_generation", presence.generation},
         {"last_seen_at_utc_ms", presence.last_seen_at_ms}});
  }
  if (request.path == "/auth/driver_logout") {
    const auto driver_id = required_string(value, "driver_id");
    const auto token = optional_string(value, "token");
    validate_driver_token(driver_id, token);
    const auto generation = online_drivers_.at(driver_id).generation;
    close_sessions_for_driver(driver_id, "driver_logout");
    driver_tokens_.erase(token);
    online_drivers_.erase(driver_id);
    audit(
        "driver_logout",
        {{"driver_id", driver_id},
         {"connection_generation", generation},
         {"reason", optional_string(value, "reason")}});
    return ServerResponse::json(200, {{"driver_id", driver_id}, {"state", "offline"}});
  }
  if (request.path == "/vehicles/online") {
    const auto vehicle_id = required_string(value, "vehicle_id");
    validate_device_token(vehicle_id, optional_string(value, "device_token"));
    const auto connection_id = required_string(value, "connection_id");
    const auto timestamp_ms = now_ms();
    const auto current = online_vehicles_.find(vehicle_id);
    if (current != online_vehicles_.end() && current->second.connection_id == connection_id) {
      current->second.last_seen_at_ms = timestamp_ms;
      return ServerResponse::json(
          200,
          {{"vehicle_id", vehicle_id},
           {"state", "online"},
           {"connection_generation", current->second.generation},
           {"duplicate_policy", "same_connection_refresh"}});
    }
    const bool replacing = current != online_vehicles_.end();
    if (replacing) {
      close_sessions_for_vehicle(vehicle_id, "vehicle_connection_replaced");
      audit(
          "vehicle_connection_replaced",
          {{"vehicle_id", vehicle_id}, {"previous_connection_generation", current->second.generation}});
    }
    const auto generation = ++connection_generation_;
    online_vehicles_[vehicle_id] = ConnectionPresence{connection_id, generation, timestamp_ms, timestamp_ms};
    audit("vehicle_online", {{"vehicle_id", vehicle_id}, {"connection_generation", generation}});
    return ServerResponse::json(
        200,
        {{"vehicle_id", vehicle_id},
         {"state", "online"},
         {"connection_generation", generation},
         {"duplicate_policy", replacing ? "replace_previous_connection" : "new_connection"}});
  }
  if (request.path == "/vehicles/heartbeat") {
    const auto vehicle_id = required_string(value, "vehicle_id");
    const auto generation = required_uint64(value, "connection_generation");
    validate_vehicle_connection(vehicle_id, optional_string(value, "device_token"), generation);
    return ServerResponse::json(
        200,
        {{"vehicle_id", vehicle_id},
         {"state", "online"},
         {"connection_generation", generation},
         {"last_seen_at_utc_ms", online_vehicles_.at(vehicle_id).last_seen_at_ms}});
  }
  if (request.path == "/vehicles/offline") {
    const auto vehicle_id = required_string(value, "vehicle_id");
    const auto generation = required_uint64(value, "connection_generation");
    validate_vehicle_connection(vehicle_id, optional_string(value, "device_token"), generation);
    online_vehicles_.erase(vehicle_id);
    close_sessions_for_vehicle(vehicle_id, "vehicle_offline");
    audit(
        "vehicle_offline",
        {{"vehicle_id", vehicle_id},
         {"connection_generation", generation},
         {"reason", optional_string(value, "reason")}});
    return ServerResponse::json(200, {{"vehicle_id", vehicle_id}, {"state", "offline"}});
  }
  if (request.path == "/sessions") {
    const auto driver_id = required_string(value, "driver_id");
    const auto vehicle_id = required_string(value, "vehicle_id");
    validate_driver_token(driver_id, optional_string(value, "token"));
    const auto permissions = config_.driver_vehicle_permissions.find(driver_id);
    if (permissions == config_.driver_vehicle_permissions.end() || !permissions->second.contains(vehicle_id)) {
      audit(
          "session_rejected",
          {{"vehicle_id", vehicle_id}, {"driver_id", driver_id}, {"reason", "vehicle_not_permitted"}});
      throw Unauthorized("driver is not permitted to control this vehicle");
    }
    if (!online_vehicles_.contains(vehicle_id)) throw Conflict("vehicle is not online");
    for (const auto& [id, session] : sessions_) {
      static_cast<void>(id);
      if (session.vehicle_id == vehicle_id && session.state != SessionState::Closed) {
        audit(
            "session_rejected",
            {{"vehicle_id", vehicle_id},
             {"driver_id", driver_id},
             {"reason", "control_authority_already_granted"},
             {"active_session_id", session.session_id}});
        throw Conflict("control authority already granted");
      }
    }
    ++session_counter_;
    std::ostringstream id;
    id << "session-" << std::setw(6) << std::setfill('0') << session_counter_;
    Session session{
        .session_id = id.str(),
        .vehicle_id = vehicle_id,
        .driver_id = driver_id,
        .state = SessionState::Online,
        .control_token = "control-token-" + random_token(),
        .control_token_expires_at_ms = now_ms() + config_.control_token_ttl_ms};
    sessions_[session.session_id] = session;
    auto& stored = sessions_.at(session.session_id);
    audit(
        "session_created",
        {{"session_id", stored.session_id}, {"vehicle_id", stored.vehicle_id}, {"driver_id", stored.driver_id}});
    transition_session(stored, SessionState::Reserved, "control_requested");
    transition_session(stored, SessionState::Connecting, "participants_authenticated");
    transition_session(stored, SessionState::Active, "control_authority_granted");
    audit(
        "control_authority_granted",
        {{"session_id", stored.session_id}, {"vehicle_id", stored.vehicle_id}, {"driver_id", stored.driver_id}});
    return ServerResponse::json(200, stored.to_json(true));
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "renew") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    if (actor != session.driver_id) throw Unauthorized("only the current driver can renew control authority");
    validate_actor_credential(session, actor, value);
    const auto renewed_at_ms = now_ms();
    const auto previous_expiry_ms = session.control_token_expires_at_ms;
    session.control_token_expires_at_ms =
        config_.control_token_ttl_ms > std::numeric_limits<std::int64_t>::max() - renewed_at_ms
        ? std::numeric_limits<std::int64_t>::max()
        : renewed_at_ms + config_.control_token_ttl_ms;
    audit(
        "control_authority_renewed",
        {{"session_id", session.session_id},
         {"vehicle_id", session.vehicle_id},
         {"driver_id", session.driver_id},
         {"previous_expires_at_utc_ms", previous_expiry_ms},
         {"expires_at_utc_ms", session.control_token_expires_at_ms}});
    return ServerResponse::json(200, session.to_json(true));
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "end") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    validate_actor_credential(session, actor, value);
    close_session(session, optional_string(value, "reason").empty() ? "session_end" : optional_string(value, "reason"));
    audit("session_ended", session.to_json());
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 4 && parts[0] == "sessions" && parts[2] == "control_authority" && parts[3] == "revoke") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    validate_actor_credential(session, actor, value);
    close_session(
        session,
        optional_string(value, "reason").empty() ? "control_authority_revoked" : optional_string(value, "reason"));
    audit("control_authority_revoked", {{"session_id", session.session_id}, {"reason", optional_string(value, "reason")}});
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "webrtc_connection") {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value);
    const auto connection_state = required_string(value, "connection_state");
    const auto connection_method = required_string(value, "connection_method");
    static const std::unordered_set<std::string> allowed_states{
        "new", "connecting", "connected", "disconnected", "failed", "closed"};
    static const std::unordered_set<std::string> allowed_methods{"unknown", "direct", "STUN", "TURN"};
    if (!allowed_states.contains(connection_state)) throw std::invalid_argument("invalid WebRTC connection state");
    if (!allowed_methods.contains(connection_method)) throw std::invalid_argument("invalid WebRTC connection method");
    const auto required_boolean = [&](std::string_view key) {
      const std::string name(key);
      if (!value.contains(name) || !value.at(name).is_boolean()) {
        throw std::invalid_argument(name + " must be a boolean");
      }
      return value.at(name).get<bool>();
    };
    const auto turn_in_use = required_boolean("turn_in_use");
    const auto time_sync_synchronized = required_boolean("time_sync_synchronized");
    const auto time_sync_acceptable = required_boolean("time_sync_acceptable");
    const auto uncertainty_ms = required_int64(value, "time_sync_uncertainty_ms");
    const auto sampled_at_utc_ms = required_int64(value, "sampled_at_utc_ms");
    if (uncertainty_ms < 0) throw std::invalid_argument("time_sync_uncertainty_ms must be non-negative");
    if (sampled_at_utc_ms <= 0) throw std::invalid_argument("sampled_at_utc_ms must be positive");
    if (turn_in_use != (connection_method == "TURN")) {
      throw std::invalid_argument("TURN usage does not match the WebRTC connection method");
    }
    const Json details = {
        {"session_id", session.session_id},
        {"vehicle_id", session.vehicle_id},
        {"driver_id", session.driver_id},
        {"actor", actor},
        {"connection_state", connection_state},
        {"connection_method", connection_method},
        {"turn_in_use", turn_in_use},
        {"time_sync_synchronized", time_sync_synchronized},
        {"time_sync_acceptable", time_sync_acceptable},
        {"time_sync_uncertainty_ms", uncertainty_ms},
        {"sampled_at_utc_ms", sampled_at_utc_ms}};
    const auto event = connection_state == "connected"
        ? "webrtc_connection_succeeded"
        : (connection_state == "failed" ? "webrtc_connection_failed" : "webrtc_connection_state");
    audit(event, details);
    if (!time_sync_acceptable) audit("time_sync_anomaly", details);
    return ServerResponse::json(200, {{"event", event}, {"session_id", session.session_id}});
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "abnormal_disconnect") {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value);
    const auto reason = required_string(value, "reason");
    const auto detected_by = required_string(value, "detected_by");
    audit(
        "abnormal_disconnect",
        {{"session_id", session.session_id},
         {"vehicle_id", session.vehicle_id},
         {"driver_id", session.driver_id},
         {"actor", actor},
         {"reason", reason},
         {"detected_by", detected_by}});
    return ServerResponse::json(200, {{"event", "abnormal_disconnect"}, {"session_id", session.session_id}});
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "diagnostics") {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value);
    const auto component = required_string(value, "component");
    const auto rtt_ms = required_nonnegative_uint64(value, "rtt_ms");
    const auto packet_loss_percent = required_nonnegative_number(value, "packet_loss_percent");
    const auto jitter_ms = required_nonnegative_uint64(value, "jitter_ms");
    const auto video_latency_ms = required_nonnegative_uint64(value, "video_latency_ms");
    const auto control_rate_hz = required_nonnegative_number(value, "control_rate_hz");
    if (packet_loss_percent > 100.0) throw std::invalid_argument("packet_loss_percent must not exceed 100");
    const Json details = {
        {"session_id", session.session_id},
        {"vehicle_id", session.vehicle_id},
        {"driver_id", session.driver_id},
        {"actor", actor},
        {"component", component},
        {"rtt_ms", rtt_ms},
        {"packet_loss_percent", packet_loss_percent},
        {"jitter_ms", jitter_ms},
        {"video_latency_ms", video_latency_ms},
        {"control_rate_hz", control_rate_hz}};
    audit("realtime_diagnostics", details);
    return ServerResponse::json(200, {{"event", "realtime_diagnostics"}, {"session_id", session.session_id}});
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "control_timeout") {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value);
    if (actor != session.vehicle_id) throw Unauthorized("only the vehicle may report a control timeout");
    const auto last_valid_control_at_utc_ms = required_nonnegative_uint64(value, "last_valid_control_at_utc_ms");
    const auto braking_at_utc_ms = required_nonnegative_uint64(value, "braking_at_utc_ms");
    const auto control_timeout_ms = required_nonnegative_uint64(value, "control_timeout_ms");
    if (control_timeout_ms == 0) throw std::invalid_argument("control_timeout_ms must be positive");
    if (braking_at_utc_ms < last_valid_control_at_utc_ms) {
      throw std::invalid_argument("braking_at_utc_ms must not precede the last valid control time");
    }
    audit(
        "control_timeout",
        {{"session_id", session.session_id},
         {"vehicle_id", session.vehicle_id},
         {"driver_id", session.driver_id},
         {"actor", actor},
         {"last_valid_control_at_utc_ms", last_valid_control_at_utc_ms},
         {"braking_at_utc_ms", braking_at_utc_ms},
         {"control_timeout_ms", control_timeout_ms}});
    return ServerResponse::json(200, {{"event", "control_timeout"}, {"session_id", session.session_id}});
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "estop") {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value);
    const auto reason = required_string(value, "reason");
    const auto control_seq = required_nonnegative_uint64(value, "control_seq");
    audit(
        "estop",
        {{"session_id", session.session_id},
         {"vehicle_id", session.vehicle_id},
         {"driver_id", session.driver_id},
         {"actor", actor},
         {"reason", reason},
         {"control_seq", control_seq}});
    return ServerResponse::json(200, {{"event", "estop"}, {"session_id", session.session_id}});
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "turn_relay") {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value);
    const auto turn_url = required_string(value, "turn_url");
    const auto relay_candidate = required_string(value, "relay_candidate");
    const auto selected_pair = required_string(value, "selected_pair");
    audit(
        "turn_relay_enabled",
        {{"session_id", session.session_id},
         {"vehicle_id", session.vehicle_id},
         {"driver_id", session.driver_id},
         {"actor", actor},
         {"turn_url", turn_url},
         {"relay_candidate", relay_candidate},
         {"selected_pair", selected_pair}});
    return ServerResponse::json(200, {{"event", "turn_relay_enabled"}, {"session_id", session.session_id}});
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "turn_usage") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    validate_actor_credential(session, actor, value);
    const auto sample_sequence = required_nonnegative_uint64(value, "sample_seq");
    if (sample_sequence == 0) throw std::invalid_argument("sample_seq must be positive");
    const auto bytes_sent = required_nonnegative_uint64(value, "bytes_sent");
    const auto bytes_received = required_nonnegative_uint64(value, "bytes_received");
    const auto duration_ms = required_nonnegative_uint64(value, "duration_ms");
    if (duration_ms == 0) throw std::invalid_argument("duration_ms must be positive");
    const RelayUsageSample sample{sample_sequence, bytes_sent, bytes_received, duration_ms};
    if (const auto previous = session.last_relay_usage_by_actor.find(actor);
        previous != session.last_relay_usage_by_actor.end()) {
      if (sample_sequence < previous->second.sequence) {
        throw Conflict("stale TURN usage sample sequence");
      }
      if (sample_sequence == previous->second.sequence) {
        if (bytes_sent != previous->second.bytes_sent || bytes_received != previous->second.bytes_received ||
            duration_ms != previous->second.duration_ms) {
          throw Conflict("TURN usage sample sequence was reused with different content");
        }
        return ServerResponse::json(
            200,
            {{"event", "turn_relay_usage"},
             {"duplicate", true},
             {"sample_seq", sample_sequence},
             {"turn_usage", session.to_json().at("turn_usage")}});
      }
    }
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    if (bytes_sent > maximum - bytes_received) throw std::invalid_argument("TURN usage sample bytes overflow");
    const auto sample_bytes = bytes_sent + bytes_received;
    if (session.relay_bytes_sent > maximum - session.relay_bytes_received ||
        session.relay_bytes_sent + session.relay_bytes_received > maximum - sample_bytes ||
        session.relay_duration_ms > maximum - duration_ms || session.relay_usage_samples == maximum) {
      throw std::invalid_argument("TURN usage total overflow");
    }
    const auto relay_bytes_sent = session.relay_bytes_sent + bytes_sent;
    const auto relay_bytes_received = session.relay_bytes_received + bytes_received;
    const auto relay_duration_ms = session.relay_duration_ms + duration_ms;
    const auto relay_usage_samples = session.relay_usage_samples + 1;
    const auto last_relay_bitrate_kbps =
        static_cast<double>(sample_bytes) * 8.0 / static_cast<double>(duration_ms);
    const auto relay_bytes_total = relay_bytes_sent + relay_bytes_received;
    const Json details = {
        {"session_id", session.session_id},
        {"vehicle_id", session.vehicle_id},
        {"driver_id", session.driver_id},
        {"actor", actor},
        {"sample_seq", sample_sequence},
        {"bytes_sent", bytes_sent},
        {"bytes_received", bytes_received},
        {"duration_ms", duration_ms},
        {"relay_bytes_total", relay_bytes_total},
        {"sample_count", relay_usage_samples},
        {"last_bitrate_kbps", last_relay_bitrate_kbps}};
    audit("turn_relay_usage", details);
    session.relay_bytes_sent = relay_bytes_sent;
    session.relay_bytes_received = relay_bytes_received;
    session.relay_duration_ms = relay_duration_ms;
    session.relay_usage_samples = relay_usage_samples;
    session.last_relay_bitrate_kbps = last_relay_bitrate_kbps;
    session.last_relay_usage_by_actor[actor] = sample;
    return ServerResponse::json(
        200,
        {{"event", "turn_relay_usage"},
         {"duplicate", false},
         {"sample_seq", sample_sequence},
         {"turn_usage", session.to_json().at("turn_usage")}});
  }
  if (parts.size() == 3 && parts[0] == "signaling" && parts[2] == "messages") {
    return ServerResponse::json(200, enqueue_signaling_message(parts[1], value));
  }
  return ServerResponse::json(404, {{"error", "not found"}});
}

DriverConfig load_driver_config(const std::string& path) {
  const auto root = YAML::LoadFile(path);
  DriverConfig config;
  if (!root["driver"] || !root["driver"]["id"]) throw std::invalid_argument("driver.id is required");
  if (!root["cloud"] || !root["cloud"]["signaling_url"]) throw std::invalid_argument("cloud.signaling_url is required");
  config.driver_id = root["driver"]["id"].as<std::string>();
  config.signaling_url = root["cloud"]["signaling_url"].as<std::string>();
  const auto cloud = root["cloud"];
  if (cloud["resolve"]) config.resolve_entries = cloud["resolve"].as<std::vector<std::string>>();
  if (cloud["ca_bundle"]) {
    config.ca_bundle = cloud["ca_bundle"].as<std::string>();
    if (config.ca_bundle.is_relative()) {
      config.ca_bundle = (std::filesystem::absolute(path).parent_path() / config.ca_bundle).lexically_normal();
    }
  }
  if (cloud["ice_transport_policy"]) {
    config.ice_transport_policy = cloud["ice_transport_policy"].as<std::string>();
  }
  for (const auto& entry : config.resolve_entries) {
    if (entry.empty() || entry.find_first_of("\r\n") != std::string::npos) {
      throw std::invalid_argument("cloud.resolve contains an invalid entry");
    }
  }
  if (!ice_transport_policy_is_valid(config.ice_transport_policy)) {
    throw std::invalid_argument("cloud.ice_transport_policy must be all or relay");
  }
  const auto logging = root["logging"];
  if (logging && logging["browser_event_log"]) {
    auto event_log_path = std::filesystem::path(logging["browser_event_log"].as<std::string>());
    if (event_log_path.is_relative()) {
      event_log_path = std::filesystem::absolute(path).parent_path() / event_log_path;
    }
    config.browser_event_log_path = event_log_path.lexically_normal();
  }
  if (logging && logging["browser_event_log_max_bytes"]) {
    config.browser_event_log_max_bytes = logging["browser_event_log_max_bytes"].as<std::uint64_t>();
  }
  if (logging && logging["browser_event_log_files"]) {
    config.browser_event_log_files = logging["browser_event_log_files"].as<int>();
  }
  if (root["control"] && root["control"].IsMap() && root["control"]["keyboard"].IsDefined()) {
    throw std::invalid_argument(
        "control.keyboard is no longer supported; keyboard bindings are fixed: "
        "Arrow/WASD directions, Space service brake, B hard brake, E ESTOP");
  }
  if (root["control"] && root["control"]["rate_hz"]) config.rate_hz = root["control"]["rate_hz"].as<int>();
  if (root["control"] && root["control"]["estop_hold_ms"]) config.estop_hold_ms = root["control"]["estop_hold_ms"].as<int>();
  const auto limits = root["control"] ? root["control"]["limits"] : YAML::Node{};
  if (limits) {
    if (limits["initial_max_throttle"]) {
      config.control_limits.initial_max_throttle =
          limits["initial_max_throttle"].as<double>();
    }
    const auto service_brake_explicit = static_cast<bool>(limits["initial_service_brake"]);
    if (service_brake_explicit) {
      config.control_limits.initial_service_brake =
          limits["initial_service_brake"].as<double>();
    }
    if (limits["initial_hard_brake"]) {
      config.control_limits.initial_hard_brake =
          limits["initial_hard_brake"].as<double>();
    } else if (limits["initial_max_brake"]) {
      config.control_limits.initial_hard_brake =
          limits["initial_max_brake"].as<double>();
    }
    if (!service_brake_explicit) {
      config.control_limits.initial_service_brake = std::min(
          config.control_limits.initial_service_brake,
          config.control_limits.initial_hard_brake);
    }
    if (limits["initial_max_steering_angle_deg"]) {
      config.control_limits.initial_max_steering_angle_deg =
          limits["initial_max_steering_angle_deg"].as<double>();
    }
  }
  const auto gamepad = root["control"] ? root["control"]["gamepad"] : YAML::Node{};
  if (gamepad) {
    if (gamepad["enabled"]) config.gamepad.enabled = gamepad["enabled"].as<bool>();
    if (gamepad["steering_axis"]) config.gamepad.steering_axis = gamepad["steering_axis"].as<int>();
    if (gamepad["throttle_axis"]) config.gamepad.throttle_axis = gamepad["throttle_axis"].as<int>();
    if (gamepad["brake_axis"]) config.gamepad.brake_axis = gamepad["brake_axis"].as<int>();
    if (gamepad["axis_deadzone"]) config.gamepad.axis_deadzone = gamepad["axis_deadzone"].as<double>();
    if (gamepad["steering_inverted"]) config.gamepad.steering_inverted = gamepad["steering_inverted"].as<bool>();
    if (gamepad["throttle_inverted"]) config.gamepad.throttle_inverted = gamepad["throttle_inverted"].as<bool>();
    if (gamepad["brake_inverted"]) config.gamepad.brake_inverted = gamepad["brake_inverted"].as<bool>();
    if (gamepad["steering_center"]) config.gamepad.steering_center = gamepad["steering_center"].as<double>();
    if (gamepad["steering_range"]) config.gamepad.steering_range = gamepad["steering_range"].as<double>();
    if (gamepad["throttle_rest"]) config.gamepad.throttle_rest = gamepad["throttle_rest"].as<double>();
    if (gamepad["throttle_range"]) config.gamepad.throttle_range = gamepad["throttle_range"].as<double>();
    if (gamepad["brake_rest"]) config.gamepad.brake_rest = gamepad["brake_rest"].as<double>();
    if (gamepad["brake_range"]) config.gamepad.brake_range = gamepad["brake_range"].as<double>();
    if (gamepad["estop_button"]) config.gamepad.estop_button = gamepad["estop_button"].as<int>();
  }
  const auto time_sync = root["time_sync"];
  if (time_sync && time_sync["max_uncertainty_ms"]) {
    config.max_time_sync_uncertainty_ms = time_sync["max_uncertainty_ms"].as<int>();
  }
  if (time_sync && time_sync["interval_ms"]) config.time_sync_interval_ms = time_sync["interval_ms"].as<int>();
  if (time_sync && time_sync["samples"]) config.time_sync_samples = time_sync["samples"].as<int>();
  if (config.driver_id.empty() || config.signaling_url.empty() || config.rate_hz <= 0 || config.estop_hold_ms < 0 ||
      config.browser_event_log_max_bytes < 1024 || config.browser_event_log_files < 1 ||
      config.browser_event_log_files > 20 ||
      config.max_time_sync_uncertainty_ms < 0 || config.time_sync_interval_ms <= 0 ||
      config.time_sync_samples < 3 || config.time_sync_samples > 15 || config.gamepad.steering_axis < 0 ||
      config.gamepad.throttle_axis < 0 || config.gamepad.brake_axis < 0 || config.gamepad.estop_button < 0 ||
      config.gamepad.axis_deadzone < 0.0 || config.gamepad.axis_deadzone >= 0.5 ||
      config.gamepad.steering_center < -1.0 || config.gamepad.steering_center > 1.0 ||
      config.gamepad.throttle_rest < -1.0 || config.gamepad.throttle_rest > 1.0 ||
      config.gamepad.brake_rest < -1.0 || config.gamepad.brake_rest > 1.0 ||
      !std::isfinite(config.control_limits.initial_max_throttle) ||
      config.control_limits.initial_max_throttle < 0.0 ||
      config.control_limits.initial_max_throttle > 1.0 ||
      !std::isfinite(config.control_limits.initial_service_brake) ||
      config.control_limits.initial_service_brake < 0.0 ||
      !std::isfinite(config.control_limits.initial_hard_brake) ||
      config.control_limits.initial_hard_brake < config.control_limits.initial_service_brake ||
      config.control_limits.initial_hard_brake > 1.0 ||
      !std::isfinite(config.control_limits.initial_max_steering_angle_deg) ||
      config.control_limits.initial_max_steering_angle_deg < 0.0 ||
      config.control_limits.initial_max_steering_angle_deg > 30.0 ||
      config.gamepad.steering_range <= 0.0 || config.gamepad.steering_range > 2.0 ||
      config.gamepad.throttle_range <= 0.0 || config.gamepad.throttle_range > 2.0 ||
      config.gamepad.brake_range <= 0.0 || config.gamepad.brake_range > 2.0) {
    throw std::invalid_argument("driver configuration is invalid");
  }
  return config;
}

DriverConsoleRuntime::DriverConsoleRuntime(DriverConfig config, std::string vehicle_id, std::string password)
    : config_(std::move(config)),
      vehicle_id_(std::move(vehicle_id)),
      password_(std::move(password)),
      signaling_http_url_(normalize_signaling_http_url(config_.signaling_url)),
      http_(std::chrono::seconds(5), config_.resolve_entries, config_.ca_bundle) {
  if (vehicle_id_.empty() || password_.empty()) throw std::invalid_argument("vehicle id and driver password are required");
  service_brake_limit_.store(config_.control_limits.initial_service_brake);
  hard_brake_limit_.store(config_.control_limits.initial_hard_brake);
  if (!signaling_url_is_secure_or_loopback(config_.signaling_url)) {
    throw std::invalid_argument("public signaling URL must use HTTPS or WSS; HTTP/WS is allowed only on loopback");
  }
}

DriverConsoleRuntime::~DriverConsoleRuntime() { close_signaling_websocket(); }

void DriverConsoleRuntime::connect_signaling_websocket(std::string_view session_id, std::string_view token) {
  const auto url = signaling_websocket_url(
      config_.signaling_url,
      session_id,
      config_.driver_id);
  std::lock_guard lock(signaling_websocket_mutex_);
  const bool same_session = signaling_websocket_session_id_ == session_id;
  if (signaling_websocket_ && signaling_websocket_->connected() && signaling_websocket_->url() == url) {
    const auto probe = signaling_websocket_->receive_json(std::chrono::milliseconds(0));
    if (probe.status == WebSocketReceiveStatus::Timeout) return;
    if (probe.status == WebSocketReceiveStatus::Message) {
      if (probe.message.contains("error")) {
        signaling_websocket_->close();
        throw std::runtime_error(probe.message.value("error", "websocket signaling rejected"));
      }
      if (probe.message.value("event", "") == "signaling_messages") {
        append_websocket_messages(probe.message);
      }
      return;
    }
  }
  const bool reconnecting = same_session && !signaling_websocket_session_id_.empty();
  auto next = std::make_unique<WebSocketClient>(
      std::chrono::seconds(5), config_.resolve_entries, config_.ca_bundle);
  next->connect(url, {{"X-Mine-Teleop-Driver-Token", std::string(token)}});
  if (signaling_websocket_) signaling_websocket_->close();
  signaling_websocket_ = std::move(next);
  signaling_websocket_session_id_ = std::string(session_id);
  if (!same_session) {
    pending_websocket_messages_ = Json::array();
    signaling_delivery_cursor_ = 0;
  }
  if (reconnecting) ++signaling_websocket_reconnects_;
}

void DriverConsoleRuntime::close_signaling_websocket() {
  std::lock_guard lock(signaling_websocket_mutex_);
  if (signaling_websocket_) signaling_websocket_->close();
  signaling_websocket_.reset();
  signaling_websocket_session_id_.clear();
  pending_websocket_messages_ = Json::array();
  signaling_delivery_cursor_ = 0;
}

void DriverConsoleRuntime::append_websocket_messages(const Json& envelope) {
  const auto messages = envelope.value("messages", Json::array());
  if (!messages.is_array()) throw std::runtime_error("websocket signaling envelope is invalid");
  const bool cursor_delivery = envelope.contains("delivery_cursor");
  const auto envelope_cursor = cursor_delivery ? required_uint64(envelope, "delivery_cursor") : 0;
  for (const auto& message : messages) {
    if (!message.is_object()) throw std::runtime_error("websocket signaling message is invalid");
    if (!cursor_delivery) {
      pending_websocket_messages_.push_back(message);
      continue;
    }
    const auto message_cursor = required_uint64(message, "delivery_cursor");
    if (message_cursor > envelope_cursor) {
      throw std::runtime_error("websocket signaling message exceeds its delivery cursor");
    }
    if (message_cursor <= signaling_delivery_cursor_) continue;
    pending_websocket_messages_.push_back(message);
    signaling_delivery_cursor_ = message_cursor;
  }
  if (cursor_delivery) {
    if (signaling_delivery_cursor_ < envelope_cursor) {
      throw std::runtime_error("websocket signaling envelope has a missing delivery cursor");
    }
    if (!signaling_websocket_ || !signaling_websocket_->connected()) {
      throw std::runtime_error("websocket signaling closed before delivery acknowledgement");
    }
    signaling_websocket_->send_json(
        {{"event", "signaling_delivery_ack"}, {"delivery_cursor", envelope_cursor}});
  }
}

bool DriverConsoleRuntime::remote_session_is_active(
    std::string_view session_id,
    std::string_view token) const {
  const auto response = http_.get(
      signaling_http_url_ + "/sessions/" + http_.url_encode(session_id) + "?actor=" +
          http_.url_encode(config_.driver_id),
      {{"X-Mine-Teleop-Driver-Token", std::string(token)}});
  if (response.status == 401 || response.status == 403 || response.status == 404 || response.status == 409) {
    return false;
  }
  if (response.status < 200 || response.status >= 300) {
    throw std::runtime_error(
        "session authority check failed with HTTP status " + std::to_string(response.status));
  }
  const auto value = Json::parse(response.body);
  return value.is_object() && value.value("session_id", "") == session_id && value.value("state", "") == "active";
}

TimeSyncStatus DriverConsoleRuntime::refresh_time_sync() {
  std::lock_guard refresh_lock(time_sync_mutex_);
  if (!clock_.refresh_due(config_.time_sync_interval_ms)) return clock_.status();
  const auto status = clock_.synchronize(http_, signaling_http_url_, config_.time_sync_samples);
  if (!status.acceptable(config_.max_time_sync_uncertainty_ms)) {
    throw std::runtime_error(
        "driver time synchronization uncertainty " + std::to_string(status.uncertainty_ms) +
        "ms exceeds limit " + std::to_string(config_.max_time_sync_uncertainty_ms) + "ms");
  }
  return status;
}

Json DriverConsoleRuntime::renew_control_authority() {
  std::lock_guard renewal_lock(control_lease_mutex_);
  std::string token;
  std::string session;
  std::string control_token;
  std::int64_t renew_at_ms = 0;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    session = session_id_;
    control_token = control_token_;
    renew_at_ms = control_token_renew_at_ms_;
  }
  if (token.empty() || session.empty() || control_token.empty()) {
    return {{"renewed", false}, {"reason", "not_connected"}};
  }
  const auto now = clock_.now_ms();
  if (renew_at_ms > now) {
    return {{"renewed", false}, {"reason", "not_due"}};
  }
  Json response;
  try {
    response = http_.post_json_response(
        signaling_http_url_ + "/sessions/" + http_.url_encode(session) + "/renew",
        {{"actor", config_.driver_id}, {"token", token}});
  } catch (const HttpStatusError& error) {
    if (error.status() == 401 || error.status() == 403 || error.status() == 404 || error.status() == 409) {
      close_signaling_websocket();
      std::lock_guard lock(mutex_);
      if (session_id_ == session) {
        session_id_.clear();
        control_token_.clear();
        control_token_expires_at_ms_ = 0;
        control_token_renew_at_ms_ = 0;
        sequence_ = 0;
        connected_at_ms_ = 0;
      }
    }
    throw;
  }
  if (response.value("session_id", "") != session || response.value("control_token", "") != control_token) {
    throw std::runtime_error("control authority renewal changed the active session or token");
  }
  const auto expires_at_ms = required_int64(response, "control_token_expires_at_utc_ms");
  if (expires_at_ms <= now) throw std::runtime_error("control authority renewal returned an expired lease");
  {
    std::lock_guard lock(mutex_);
    if (session_id_ != session || control_token_ != control_token) {
      return {{"renewed", false}, {"reason", "session_changed"}};
    }
    control_token_expires_at_ms_ = expires_at_ms;
    control_token_renew_at_ms_ = control_lease_renew_at(now, expires_at_ms);
  }
  return {
      {"renewed", true},
      {"session_id", session},
      {"control_token_expires_at_utc_ms", expires_at_ms},
  };
}

Json DriverConsoleRuntime::login_locked(std::string_view password) {
  if (clock_.refresh_due(config_.time_sync_interval_ms)) static_cast<void>(refresh_time_sync());
  std::string current_token;
  std::int64_t current_expiry = 0;
  {
    std::lock_guard lock(mutex_);
    current_token = driver_token_;
    current_expiry = driver_token_expires_at_ms_;
  }
  if (!current_token.empty() && clock_.now_ms() < current_expiry) {
    try {
      auto result = fetch_authorized_vehicles(current_token, current_expiry);
      result["authenticated"] = true;
      return result;
    } catch (const std::exception&) {
      std::lock_guard lock(mutex_);
      driver_token_.clear();
      driver_token_expires_at_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      connected_at_ms_ = 0;
    }
  }
  const auto credential = password.empty() ? password_ : std::string(password);
  if (credential.empty()) throw std::invalid_argument("driver password is required");
  const auto response = http_.post_json_response(
      signaling_http_url_ + "/auth/driver_login",
      {{"driver_id", config_.driver_id}, {"password", credential}});
  {
    std::lock_guard lock(mutex_);
    password_ = credential;
    driver_token_ = required_string(response, "token");
    driver_token_expires_at_ms_ = response.value("expires_at_ms", std::int64_t{0});
    signaling_service_instance_id_ = required_string(response, "service_instance_id");
    signaling_available_ = true;
  }
  auto result = fetch_authorized_vehicles(
      required_string(response, "token"),
      response.value("expires_at_ms", std::int64_t{0}));
  result["authenticated"] = true;
  return result;
}

Json DriverConsoleRuntime::login(std::string_view password) {
  std::lock_guard authentication_lock(authentication_mutex_);
  return login_locked(password);
}

Json DriverConsoleRuntime::fetch_authorized_vehicles(
    std::string_view token,
    std::int64_t expires_at_ms) {
  const auto response = http_.get_json(
      signaling_http_url_ + "/drivers/" + http_.url_encode(config_.driver_id) + "/vehicles",
      {{"X-Mine-Teleop-Driver-Token", std::string(token)}});
  const auto listed = response.value("vehicles", Json::array());
  if (!listed.is_array()) throw std::runtime_error("authorized vehicle response is invalid");
  std::string service_instance_id;
  {
    std::lock_guard lock(mutex_);
    authorized_vehicles_ = listed;
    service_instance_id = signaling_service_instance_id_;
    signaling_available_ = true;
  }
  return {
      {"authenticated", true},
      {"driver_id", config_.driver_id},
      {"token_expires_at_utc_ms", expires_at_ms},
      {"service_instance_id", service_instance_id},
      {"vehicles", listed},
  };
}

Json DriverConsoleRuntime::vehicles() {
  std::lock_guard authentication_lock(authentication_mutex_);
  std::string token;
  std::int64_t expires_at_ms = 0;
  std::string service_instance_id;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    expires_at_ms = driver_token_expires_at_ms_;
    service_instance_id = signaling_service_instance_id_;
  }
  if (token.empty()) throw HttpStatusError(401, "driver login is required");
  try {
    return fetch_authorized_vehicles(token, expires_at_ms);
  } catch (const HttpStatusError& error) {
    {
      std::lock_guard lock(mutex_);
      signaling_available_ = true;
    }
    if (error.status() != 401 && error.status() != 403) throw;
    const auto health = http_.get_json(signaling_http_url_ + "/health");
    const auto observed_instance_id = health.value("service_instance_id", "");
    if (service_instance_id.empty() || observed_instance_id.empty() || observed_instance_id == service_instance_id) {
      throw;
    }

    close_signaling_websocket();
    {
      std::lock_guard lock(mutex_);
      driver_token_.clear();
      driver_token_expires_at_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      connected_at_ms_ = 0;
      authorized_vehicles_ = Json::array();
    }
    auto recovered = login_locked({});
    {
      std::lock_guard lock(mutex_);
      ++signaling_restart_recoveries_;
    }
    recovered["signaling_restart_recovered"] = true;
    recovered["previous_service_instance_id"] = service_instance_id;
    recovered["control_authority_recovered"] = false;
    return recovered;
  } catch (const HttpTransportError&) {
    Json stale_vehicles;
    {
      std::lock_guard lock(mutex_);
      signaling_available_ = false;
      stale_vehicles = authorized_vehicles_;
    }
    for (auto& vehicle : stale_vehicles) {
      if (!vehicle.is_object()) continue;
      vehicle["online"] = false;
      vehicle["controllable"] = false;
      vehicle["controlled_by"] = "";
      vehicle["session_id"] = "";
      vehicle["state"] = "signaling_unavailable";
    }
    return {
        {"authenticated", true},
        {"driver_id", config_.driver_id},
        {"token_expires_at_utc_ms", expires_at_ms},
        {"service_instance_id", service_instance_id},
        {"signaling_available", false},
        {"stale", true},
        {"vehicles", std::move(stale_vehicles)},
    };
  } catch (const std::exception&) {
    std::lock_guard lock(mutex_);
    signaling_available_ = false;
    throw;
  }
}

Json DriverConsoleRuntime::connect(std::string_view requested_vehicle_id) {
  if (clock_.refresh_due(config_.time_sync_interval_ms)) static_cast<void>(refresh_time_sync());
  std::string current_token;
  std::string current_session;
  std::string current_vehicle;
  {
    std::lock_guard lock(mutex_);
    current_token = driver_token_;
    current_session = session_id_;
    current_vehicle = vehicle_id_;
  }
  if (current_token.empty()) {
    static_cast<void>(login());
    std::lock_guard lock(mutex_);
    current_token = driver_token_;
  }
  const std::string target = requested_vehicle_id.empty() ? current_vehicle : std::string(requested_vehicle_id);
  if (target.empty()) throw std::invalid_argument("vehicle_id is required");

  auto validate_target = [&] {
    const auto available = vehicles().at("vehicles");
    const auto selected = std::find_if(available.begin(), available.end(), [&](const auto& value) {
      return value.value("vehicle_id", "") == target;
    });
    if (selected == available.end()) throw std::invalid_argument("vehicle is not authorized for this driver");
    if (!selected->value("controllable", false)) {
      throw std::runtime_error("vehicle is not controllable: " + selected->value("state", "unknown"));
    }
  };
  bool target_validated = false;

  if (!current_session.empty()) {
    if (current_vehicle == target) {
      bool session_is_active = false;
      try {
        session_is_active = remote_session_is_active(current_session, current_token);
      } catch (const std::exception& error) {
        throw std::runtime_error(
            std::string("cannot verify the existing driver session; local authority was retained: ") + error.what());
      }
      if (session_is_active) {
        connect_signaling_websocket(current_session, current_token);
        std::lock_guard lock(mutex_);
        return {
            {"runtime", "cpp"},
            {"driver_id", config_.driver_id},
            {"vehicle_id", vehicle_id_},
            {"connected", true},
            {"session_id", session_id_},
            {"connected_at_ms", connected_at_ms_},
            {"time_sync", clock_.status().to_json()},
        };
      }
    } else {
      validate_target();
      target_validated = true;
      static_cast<void>(end_session("driver_vehicle_switch"));
    }
    if (current_vehicle == target) {
      std::lock_guard lock(mutex_);
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      connected_at_ms_ = 0;
    }
  }

  if (!target_validated) validate_target();
  const auto session = http_.post_json_response(
      signaling_http_url_ + "/sessions",
      {{"driver_id", config_.driver_id}, {"vehicle_id", target}, {"token", current_token}});
  const auto session_id = required_string(session, "session_id");
  const auto control_token = required_string(session, "control_token");
  const auto connected_at_ms = clock_.now_ms();
  const auto control_token_expires_at_ms = required_int64(session, "control_token_expires_at_utc_ms");
  if (control_token_expires_at_ms <= connected_at_ms) {
    throw std::runtime_error("new control authority lease is already expired");
  }
  {
    std::lock_guard lock(mutex_);
    vehicle_id_ = target;
    session_id_ = session_id;
    control_token_ = control_token;
    control_token_expires_at_ms_ = control_token_expires_at_ms;
    control_token_renew_at_ms_ = control_lease_renew_at(connected_at_ms, control_token_expires_at_ms);
    sequence_ = 0;
    connected_at_ms_ = connected_at_ms;
  }
  try {
    connect_signaling_websocket(session_id, current_token);
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      static_cast<void>(end_session("signaling_websocket_connect_failed"));
    } catch (const std::exception&) {
      std::lock_guard lock(mutex_);
      if (session_id_ == session_id) {
        session_id_.clear();
        control_token_.clear();
        control_token_expires_at_ms_ = 0;
        control_token_renew_at_ms_ = 0;
        sequence_ = 0;
        connected_at_ms_ = 0;
      }
    }
    std::rethrow_exception(failure);
  }
  try {
    static_cast<void>(vehicles());
  } catch (const std::exception&) {
    // Session creation is authoritative; a later status refresh will retry the vehicle-list update.
  }
  return {
      {"runtime", "cpp"},
      {"driver_id", config_.driver_id},
      {"vehicle_id", target},
      {"connected", true},
      {"session_id", session_id},
      {"connected_at_ms", connected_at_ms},
      {"time_sync", clock_.status().to_json()},
  };
}

Json DriverConsoleRuntime::end_session(std::string_view reason) {
  std::string token;
  std::string session;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    session = session_id_;
  }
  if (session.empty()) {
    return {{"driver_id", config_.driver_id}, {"connected", false}, {"session_id", ""}};
  }
  if (token.empty()) throw std::runtime_error("driver login is required to end the session");
  auto response = http_.post_json_response(
      signaling_http_url_ + "/sessions/" + http_.url_encode(session) + "/end",
      {{"actor", config_.driver_id}, {"token", token}, {"reason", std::string(reason)}});
  close_signaling_websocket();
  {
    std::lock_guard lock(mutex_);
    if (session_id_ == session) {
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      connected_at_ms_ = 0;
    }
  }
  response["driver_id"] = config_.driver_id;
  response["connected"] = false;
  response["session_id"] = session;
  return response;
}

Json DriverConsoleRuntime::disconnect(std::string_view reason) {
  std::string token;
  std::string session;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    session = session_id_;
  }
  if (token.empty()) {
    close_signaling_websocket();
    std::lock_guard lock(mutex_);
    session_id_.clear();
    control_token_.clear();
    control_token_expires_at_ms_ = 0;
    control_token_renew_at_ms_ = 0;
    sequence_ = 0;
    connected_at_ms_ = 0;
    authorized_vehicles_ = Json::array();
    return {{"driver_id", config_.driver_id}, {"state", "offline"}, {"session_id", session}};
  }
  auto response = http_.post_json_response(
      signaling_http_url_ + "/auth/driver_logout",
      {{"driver_id", config_.driver_id}, {"token", token}, {"reason", std::string(reason)}});
  close_signaling_websocket();
  {
    std::lock_guard lock(mutex_);
    if (driver_token_ == token) {
      driver_token_.clear();
      driver_token_expires_at_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      connected_at_ms_ = 0;
      authorized_vehicles_ = Json::array();
    }
  }
  response["session_id"] = session;
  return response;
}

Json DriverConsoleRuntime::poll_signaling() {
  std::string token;
  std::string session;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    session = session_id_;
  }
  if (token.empty() || session.empty()) throw std::runtime_error("driver console is not connected");
  auto drain = [&](std::chrono::milliseconds first_wait) {
    std::lock_guard websocket_lock(signaling_websocket_mutex_);
    if (!signaling_websocket_ || !signaling_websocket_->connected()) {
      throw std::runtime_error("websocket signaling is not connected");
    }
    auto wait = pending_websocket_messages_.empty() ? first_wait : std::chrono::milliseconds(0);
    while (true) {
      const auto received = signaling_websocket_->receive_json(wait);
      if (received.status == WebSocketReceiveStatus::Timeout) break;
      if (received.status == WebSocketReceiveStatus::Closed) {
        throw std::runtime_error("websocket signaling connection closed");
      }
      if (received.message.contains("error")) {
        throw std::runtime_error(received.message.value("error", "websocket signaling rejected"));
      }
      if (received.message.value("event", "") == "signaling_messages") {
        append_websocket_messages(received.message);
      }
      wait = std::chrono::milliseconds(0);
    }
    Json messages = std::move(pending_websocket_messages_);
    pending_websocket_messages_ = Json::array();
    return messages;
  };

  Json messages;
  try {
    messages = drain(std::chrono::milliseconds(150));
  } catch (const std::exception&) {
    {
      std::lock_guard websocket_lock(signaling_websocket_mutex_);
      if (signaling_websocket_) signaling_websocket_->close();
      signaling_websocket_.reset();
    }
    bool session_is_active = false;
    try {
      session_is_active = remote_session_is_active(session, token);
    } catch (const std::exception& error) {
      {
        std::lock_guard lock(mutex_);
        signaling_available_ = false;
      }
      throw std::runtime_error(
          std::string("signaling reconnect is unavailable; local session is retained pending verification: ") +
          error.what());
    }
    if (!session_is_active) {
      {
        std::lock_guard lock(mutex_);
        signaling_available_ = true;
        if (session_id_ == session) {
          session_id_.clear();
          control_token_.clear();
          control_token_expires_at_ms_ = 0;
          control_token_renew_at_ms_ = 0;
          sequence_ = 0;
          connected_at_ms_ = 0;
        }
      }
      close_signaling_websocket();
      throw std::runtime_error("signaling authority is no longer valid");
    }
    connect_signaling_websocket(session, token);
    messages = drain(std::chrono::milliseconds(150));
  }
  {
    std::lock_guard lock(mutex_);
    if (session_id_ != session) throw std::runtime_error("driver session changed during signaling poll");
    signaling_messages_ = messages;
    signaling_available_ = true;
  }
  return {{"session_id", session}, {"messages", messages}};
}

Json DriverConsoleRuntime::send_signaling_message(std::string_view type, const Json& payload) {
  std::lock_guard send_lock(signaling_send_mutex_);
  std::string token;
  std::string session;
  std::string vehicle;
  std::uint64_t sequence = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty() || driver_token_.empty()) throw std::runtime_error("driver console is not connected");
    token = driver_token_;
    session = session_id_;
    vehicle = vehicle_id_;
    sequence = ++sequence_;
  }
  const ProtocolMetadata metadata{
      kProtocolVersion, vehicle, config_.driver_id, session, sequence, clock_.now_ms()};
  auto request = metadata.to_json();
  request["sender"] = config_.driver_id;
  request["recipient"] = vehicle;
  request["token"] = token;
  request["type"] = type;
  request["payload"] = payload;
  auto exchange = [&]() -> Json {
    std::lock_guard websocket_lock(signaling_websocket_mutex_);
    if (!signaling_websocket_ || !signaling_websocket_->connected()) {
      throw std::runtime_error("websocket signaling is not connected");
    }
    signaling_websocket_->send_json(request);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - std::chrono::steady_clock::now());
      const auto received = signaling_websocket_->receive_json(remaining);
      if (received.status == WebSocketReceiveStatus::Timeout) break;
      if (received.status == WebSocketReceiveStatus::Closed) {
        throw std::runtime_error("websocket signaling connection closed before acknowledgement");
      }
      if (received.message.contains("error")) {
        throw SignalingRejected(received.message.value("error", "websocket signaling rejected"));
      }
      if (received.message.value("event", "") == "signaling_messages") {
        append_websocket_messages(received.message);
        continue;
      }
      if (received.message.value("event", "") == "signaling_ack" &&
          received.message.value("type", "") == type &&
          (!received.message.contains("seq") || received.message.value("seq", std::uint64_t{0}) == sequence)) {
        return {
            {"queued", received.message.value("queued", 0)},
            {"transport", "websocket"},
            {"type", type},
            {"seq", sequence},
            {"message_id", received.message.value("message_id", "")},
            {"delivery_cursor", received.message.value("delivery_cursor", std::uint64_t{0})},
            {"duplicate", received.message.value("duplicate", false)},
            {"session_id", session}};
      }
    }
    throw std::runtime_error("websocket signaling acknowledgement timed out");
  };

  std::string first_failure;
  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      return exchange();
    } catch (const SignalingRejected& error) {
      throw std::runtime_error(error.what());
    } catch (const std::exception& error) {
      if (attempt != 0) {
        throw std::runtime_error(
            "websocket signaling retry failed after " + first_failure + ": " + error.what());
      }
      first_failure = error.what();
      {
        std::lock_guard websocket_lock(signaling_websocket_mutex_);
        if (signaling_websocket_) signaling_websocket_->close();
        signaling_websocket_.reset();
      }
      bool session_is_active = false;
      try {
        session_is_active = remote_session_is_active(session, token);
      } catch (const std::exception& authority_error) {
        throw std::runtime_error(
            std::string("signaling retry is unavailable; local session is retained pending verification: ") +
            authority_error.what());
      }
      if (!session_is_active) {
        {
          std::lock_guard lock(mutex_);
          if (session_id_ == session) {
            session_id_.clear();
            control_token_.clear();
            control_token_expires_at_ms_ = 0;
            control_token_renew_at_ms_ = 0;
            sequence_ = 0;
            connected_at_ms_ = 0;
          }
        }
        close_signaling_websocket();
        throw std::runtime_error("signaling authority is no longer valid");
      }
      connect_signaling_websocket(session, token);
    }
  }
  throw std::logic_error("unreachable websocket signaling retry state");
}

Json DriverConsoleRuntime::send_media_capabilities(const Json& input) {
  if (!input.is_object() || !input.contains("codecs") || !input.at("codecs").is_array()) {
    throw std::invalid_argument("media capabilities must contain a codecs array");
  }
  Json codecs = Json::array();
  for (const auto& value : input.at("codecs")) {
    if (!value.is_string()) throw std::invalid_argument("media codec capability must be a string");
    auto codec = lower(value.get<std::string>());
    if (codec == "h265" || codec == "hevc" || codec == "h264" || codec == "avc") codecs.push_back(codec);
  }
  if (codecs.empty()) codecs.push_back("h264");
  return send_signaling_message("media_capabilities", {{"codecs", std::move(codecs)}});
}

Json DriverConsoleRuntime::ice_servers() {
  std::string token;
  std::string session;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty() || driver_token_.empty()) throw std::runtime_error("driver console is not connected");
    token = driver_token_;
    session = session_id_;
  }
  return http_.get_json(
      signaling_http_url_ + "/sessions/" + http_.url_encode(session) + "/ice_servers?actor=" +
          http_.url_encode(config_.driver_id),
      {{"X-Mine-Teleop-Driver-Token", token}});
}

Json DriverConsoleRuntime::send_media_fallback(const Json& input) {
  if (!input.is_object() || lower(input.value("codec", "")) != "h264") {
    throw std::invalid_argument("media fallback must request H.264");
  }
  return send_signaling_message(
      "media_fallback", {{"codec", "h264"}, {"reason", input.value("reason", "browser_decode_failure")}});
}

Json DriverConsoleRuntime::send_webrtc_answer(const Json& input) {
  if (!input.is_object() || input.value("type", "") != "answer" || input.value("sdp", "").empty()) {
    throw std::invalid_argument("WebRTC answer must contain type=answer and SDP");
  }
  return send_signaling_message("webrtc_answer", {{"type", "answer"}, {"sdp", input.at("sdp")}});
}

Json DriverConsoleRuntime::send_webrtc_ice_candidate(const Json& input) {
  const auto candidate = input.contains("candidate") && input.at("candidate").is_object() ? input.at("candidate") : input;
  if (!candidate.is_object() || candidate.value("candidate", "").empty()) {
    throw std::invalid_argument("WebRTC ICE candidate is required");
  }
  return send_signaling_message("ice_candidate", candidate);
}

Json DriverConsoleRuntime::ingest_webrtc_metrics(const Json& input) {
  if (!input.is_object()) throw std::invalid_argument("WebRTC metrics must be an object");
  const auto connection_state = required_string(input, "connection_state");
  const auto connection_method = required_string(input, "connection_method");
  static const std::unordered_set<std::string> allowed_states{
      "new", "connecting", "connected", "disconnected", "failed", "closed"};
  static const std::unordered_set<std::string> allowed_methods{"unknown", "direct", "STUN", "TURN"};
  if (!allowed_states.contains(connection_state)) throw std::invalid_argument("invalid WebRTC connection state");
  if (!allowed_methods.contains(connection_method)) throw std::invalid_argument("invalid WebRTC connection method");
  if (!input.contains("turn_in_use") || !input.at("turn_in_use").is_boolean()) {
    throw std::invalid_argument("turn_in_use must be a boolean");
  }
  const auto turn_in_use = input.at("turn_in_use").get<bool>();
  if (turn_in_use != (connection_method == "TURN")) {
    throw std::invalid_argument("TURN usage does not match the WebRTC connection method");
  }
  const auto time_sync = input.value("time_sync", Json::object());
  if (!time_sync.is_object()) throw std::invalid_argument("time_sync must be an object");
  const auto time_sync_synchronized = time_sync.value("synchronized", false);
  const auto uncertainty_ms = time_sync.value("uncertainty_ms", std::int64_t{0});
  if (uncertainty_ms < 0) throw std::invalid_argument("time_sync uncertainty must be non-negative");
  const auto time_sync_acceptable = time_sync_synchronized && uncertainty_ms <= config_.max_time_sync_uncertainty_ms;
  const auto report_key = connection_state + "\n" + connection_method + "\n" +
      (turn_in_use ? "turn" : "not-turn") + "\n" + (time_sync_acceptable ? "time-ok" : "time-anomaly");
  const auto received_at_ms = now_ms();
  std::string session;
  std::string token;
  std::string audit_key;
  bool should_report = false;
  {
    std::lock_guard lock(mutex_);
    webrtc_metrics_ = input;
    webrtc_metrics_["received_at_ms"] = received_at_ms;
    audit_key = session_id_ + "\n" + report_key;
    if (!session_id_.empty() && !driver_token_.empty() && audit_key != last_webrtc_audit_key_) {
      session = session_id_;
      token = driver_token_;
      last_webrtc_audit_key_ = audit_key;
      should_report = true;
    }
  }
  Json response = {{"accepted", true}, {"received_at_ms", received_at_ms}, {"reported", false}};
  if (!should_report) return response;
  try {
    const auto reported = http_.post_json_response(
        signaling_http_url_ + "/sessions/" + http_.url_encode(session) + "/webrtc_connection",
        {{"actor", config_.driver_id},
         {"token", token},
         {"connection_state", connection_state},
         {"connection_method", connection_method},
         {"turn_in_use", turn_in_use},
         {"time_sync_synchronized", time_sync_synchronized},
         {"time_sync_acceptable", time_sync_acceptable},
         {"time_sync_uncertainty_ms", uncertainty_ms},
         {"sampled_at_utc_ms", received_at_ms}});
    response["reported"] = true;
    response["audit_event"] = reported.value("event", "");
  } catch (const std::exception& error) {
    {
      std::lock_guard lock(mutex_);
      if (last_webrtc_audit_key_ == audit_key) last_webrtc_audit_key_.clear();
    }
    response["report_error"] = error.what();
  }
  return response;
}

Json DriverConsoleRuntime::control_limits() const {
  std::lock_guard lock(mutex_);
  const auto service_brake = service_brake_limit_.load();
  const auto hard_brake = hard_brake_limit_.load();
  return {
      {"service_brake", service_brake},
      {"hard_brake", hard_brake},
      {"max_brake", hard_brake},
  };
}

Json DriverConsoleRuntime::set_control_limits(const Json& input) {
  if (!input.is_object()) {
    throw std::invalid_argument("control limits require an object");
  }
  std::lock_guard lock(mutex_);
  double service_brake = service_brake_limit_.load();
  double hard_brake = hard_brake_limit_.load();
  if (input.contains("service_brake") || input.contains("hard_brake")) {
    if (!input.contains("service_brake") || !input.at("service_brake").is_number() ||
        !input.contains("hard_brake") || !input.at("hard_brake").is_number()) {
      throw std::invalid_argument(
          "control limits require numeric service_brake and hard_brake");
    }
    service_brake = input.at("service_brake").get<double>();
    hard_brake = input.at("hard_brake").get<double>();
  } else if (input.contains("max_brake") && input.at("max_brake").is_number()) {
    hard_brake = input.at("max_brake").get<double>();
    service_brake = std::min(service_brake, hard_brake);
  } else {
    throw std::invalid_argument(
        "control limits require numeric service_brake and hard_brake");
  }
  if (!std::isfinite(service_brake) || !std::isfinite(hard_brake) ||
      service_brake < 0.0 || service_brake > hard_brake || hard_brake > 1.0) {
    throw std::invalid_argument(
        "control brake limits must satisfy 0 <= service_brake <= hard_brake <= 1");
  }
  hard_brake_limit_.store(hard_brake);
  service_brake_limit_.store(service_brake);
  return {
      {"service_brake", service_brake},
      {"hard_brake", hard_brake},
      {"max_brake", hard_brake},
  };
}

Json DriverConsoleRuntime::send_control(const Json& input) {
  std::string session;
  std::string control_token;
  std::string vehicle;
  std::uint64_t sequence = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty() || driver_token_.empty() || control_token_.empty()) throw std::runtime_error("driver console is not connected");
    session = session_id_;
    control_token = control_token_;
    vehicle = vehicle_id_;
    sequence = ++sequence_;
  }
  ControlCommand command;
  command.vehicle_id = vehicle;
  command.driver_id = config_.driver_id;
  command.session_id = session;
  command.seq = sequence;
  command.sent_at_utc_ms = clock_.now_ms();
  command.gear = input.value("gear", "N");
  command.steering = input.value("steering", 0.0);
  command.throttle = input.value("throttle", 0.0);
  command.brake = input.value("brake", 0.0);
  command.estop = input.value("estop", false);
  command.control_token = control_token;
  command.validate();
  if (!command.estop) {
    command.brake = std::min(command.brake, hard_brake_limit_.load());
  }
  {
    std::lock_guard lock(mutex_);
    last_control_prepared_at_utc_ms_ = command.sent_at_utc_ms;
    ++control_commands_prepared_total_;
  }
  return {
      {"prepared", true},
      {"transport", "webrtc_data_channel"},
      {"delivery_state", "browser_data_channel_pending"},
      {"command", command.to_json()},
  };
}

Json DriverConsoleRuntime::status() {
  bool authenticated = false;
  bool control_lease_due = false;
  const auto timestamp_ms = clock_.now_ms();
  {
    std::lock_guard lock(mutex_);
    authenticated = !driver_token_.empty();
    control_lease_due = !session_id_.empty() && control_token_renew_at_ms_ <= timestamp_ms;
  }
  if (control_lease_due) {
    try {
      static_cast<void>(renew_control_authority());
    } catch (const std::exception&) {
      // Status remains locally readable while the browser has stopped realtime control.
    }
  }
  if (authenticated && clock_.refresh_due(config_.time_sync_interval_ms)) {
    try {
      static_cast<void>(refresh_time_sync());
    } catch (const std::exception&) {
      // Vehicle-list refresh owns restart recovery; monitoring must not become unavailable with signaling.
    }
  }
  bool websocket_connected = false;
  std::uint64_t websocket_reconnects = 0;
  std::uint64_t delivery_cursor = 0;
  std::size_t pending_deliveries = 0;
  {
    std::lock_guard websocket_lock(signaling_websocket_mutex_);
    websocket_connected = signaling_websocket_ && signaling_websocket_->connected();
    websocket_reconnects = signaling_websocket_reconnects_;
    delivery_cursor = signaling_delivery_cursor_;
    pending_deliveries = pending_websocket_messages_.size();
  }
  std::lock_guard lock(mutex_);
  return {
      {"runtime", "cpp"},
      {"driver_id", config_.driver_id},
      {"vehicle_id", vehicle_id_},
      {"authenticated", !driver_token_.empty()},
      {"driver_token_expires_at_utc_ms", driver_token_expires_at_ms_},
      {"signaling_service_instance_id", signaling_service_instance_id_},
      {"signaling_restart_recoveries", signaling_restart_recoveries_},
      {"signaling_available", signaling_available_},
      {"connected", !session_id_.empty()},
      {"session_id", session_id_},
      {"control_token_expires_at_utc_ms", control_token_expires_at_ms_},
      {"sequence", sequence_},
      {"connected_at_ms", connected_at_ms_},
      {"last_control_prepared_at_utc_ms", last_control_prepared_at_utc_ms_},
      {"control_commands_prepared_total", control_commands_prepared_total_},
      {"signaling_transport", "websocket"},
      {"signaling_websocket_connected", websocket_connected},
      {"signaling_websocket_reconnects", websocket_reconnects},
      {"signaling_delivery_cursor", delivery_cursor},
      {"pending_signaling_deliveries", pending_deliveries},
      {"time_sync", clock_.status().to_json()},
      {"webrtc_metrics", webrtc_metrics_},
      {"last_signaling_messages", signaling_messages_},
      {"authorized_vehicles", authorized_vehicles_},
  };
}

Json DriverConsoleRuntime::record_browser_event(const Json& input) {
  if (!input.is_object()) throw std::invalid_argument("browser event must be an object");
  const auto event = input.value("event", "");
  if (event.empty() || event.size() > 128 || !std::all_of(event.begin(), event.end(), [](unsigned char value) {
        return std::isalnum(value) || value == '_' || value == '-' || value == '.';
      })) {
    throw std::invalid_argument("browser event name is invalid");
  }
  const auto details = input.contains("details") ? sanitize_log_value(input.at("details")) : Json::object();
  if (!details.is_object()) throw std::invalid_argument("browser event details must be an object");
  const auto received_at = now_ms();
  auto browser_sent_at = received_at;
  if (input.contains("sent_at_utc_ms")) {
    const auto& timestamp = input.at("sent_at_utc_ms");
    if (timestamp.is_number_unsigned()) {
      const auto value = timestamp.get<std::uint64_t>();
      if (value > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw std::invalid_argument("browser event timestamp is invalid");
      }
      browser_sent_at = static_cast<std::int64_t>(value);
    } else if (timestamp.is_number_integer()) {
      browser_sent_at = timestamp.get<std::int64_t>();
    } else {
      throw std::invalid_argument("browser event timestamp is invalid");
    }
    if (browser_sent_at < 0) throw std::invalid_argument("browser event timestamp is invalid");
  }
  std::string session;
  std::string vehicle;
  {
    std::lock_guard lock(mutex_);
    session = session_id_;
    vehicle = vehicle_id_;
  }
  Json record = {
      {"event", event},
      {"sent_at_utc_ms", received_at},
      {"browser_sent_at_utc_ms", browser_sent_at},
      {"driver_id", config_.driver_id},
      {"vehicle_id", vehicle},
      {"session_id", session},
      {"details", details},
  };
  if (config_.browser_event_log_path.empty()) return {{"recorded", false}, {"event", event}};
  const auto line = record.dump() + "\n";
  if (line.size() > config_.browser_event_log_max_bytes) {
    throw std::invalid_argument("browser event exceeds configured log size");
  }
  std::lock_guard log_lock(browser_event_log_mutex_);
  const auto parent = config_.browser_event_log_path.parent_path();
  std::error_code error;
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error) throw std::runtime_error("cannot create browser event log directory: " + error.message());
  }
  rotate_jsonl_log(
      config_.browser_event_log_path,
      config_.browser_event_log_max_bytes,
      config_.browser_event_log_files,
      line.size());
  std::ofstream output(config_.browser_event_log_path, std::ios::app);
  if (!output) throw std::runtime_error("cannot append browser event log");
  output << line;
  if (!output) throw std::runtime_error("cannot flush browser event log");
  return {{"recorded", true}, {"event", event}};
}

DriverConsoleHttpApp::DriverConsoleHttpApp(std::shared_ptr<DriverConsoleRuntime> runtime) : runtime_(std::move(runtime)) {
  if (!runtime_) throw std::invalid_argument("driver console runtime is required");
}

ServerResponse DriverConsoleHttpApp::handle(const HttpRequest& request) const {
  try {
    if (request.method == "GET" && request.path == "/health") return ServerResponse::json(200, {{"status", "ok"}, {"runtime", "cpp"}});
    if (request.method == "GET" && request.path == "/api/time") return ServerResponse::json(200, {{"now_ms", now_ms()}});
    if (request.method == "GET" && request.path == "/api/status") return ServerResponse::json(200, runtime_->status());
    if (request.method == "GET" && request.path == "/api/vehicles") return ServerResponse::json(200, runtime_->vehicles());
    if (request.method == "GET" && request.path == "/api/control-limits") return ServerResponse::json(200, runtime_->control_limits());
    if (request.method == "GET" && request.path == "/") {
      return ServerResponse::text(200, console_html(runtime_->config()), "text/html; charset=utf-8");
    }
    if (request.method == "POST" && request.path == "/api/login") {
      return ServerResponse::json(200, runtime_->login(request.json_body().value("password", "")));
    }
    if (request.method == "POST" && request.path == "/api/connect") {
      return ServerResponse::json(200, runtime_->connect(request.json_body().value("vehicle_id", "")));
    }
    if (request.method == "POST" && request.path == "/api/end-session") {
      return ServerResponse::json(
          200,
          runtime_->end_session(request.json_body().value("reason", "driver_session_end")));
    }
    if (request.method == "POST" && request.path == "/api/disconnect") {
      return ServerResponse::json(
          200,
          runtime_->disconnect(request.json_body().value("reason", "driver_console_disconnect")));
    }
    if (request.method == "POST" && request.path == "/api/poll-signaling") return ServerResponse::json(200, runtime_->poll_signaling());
    if (request.method == "POST" && request.path == "/api/webrtc/ice-servers") return ServerResponse::json(200, runtime_->ice_servers());
    if (request.method == "POST" && request.path == "/api/webrtc/capabilities") return ServerResponse::json(200, runtime_->send_media_capabilities(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/fallback") return ServerResponse::json(200, runtime_->send_media_fallback(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/answer") return ServerResponse::json(200, runtime_->send_webrtc_answer(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/ice-candidate") return ServerResponse::json(200, runtime_->send_webrtc_ice_candidate(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/metrics") return ServerResponse::json(200, runtime_->ingest_webrtc_metrics(request.json_body()));
    if (request.method == "POST" && request.path == "/api/browser-event") return ServerResponse::json(200, runtime_->record_browser_event(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control-limits") return ServerResponse::json(200, runtime_->set_control_limits(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control") return ServerResponse::json(200, runtime_->send_control(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control/keyboard") {
      const auto limits = runtime_->control_limits();
      return ServerResponse::json(
          200,
          runtime_->send_control(keyboard_to_control(
              request.json_body(),
              limits.at("service_brake").get<double>(),
              limits.at("hard_brake").get<double>())));
    }
    if (request.method == "POST" && request.path == "/api/control/gamepad") {
      const auto input = request.json_body();
      Json control = {
          {"gear", input.value("gear", "D")},
          {"steering", input.value("steering", 0.0)},
          {"throttle", input.value("throttle", 0.0)},
          {"brake", input.value("brake", 0.0)},
          {"estop", input.value("estop", false)},
      };
      return ServerResponse::json(200, runtime_->send_control(control));
    }
    return ServerResponse::json(404, {{"error", "not found"}});
  } catch (const HttpStatusError& error) {
    const auto status = error.status() >= 400 && error.status() <= 599
        ? static_cast<int>(error.status())
        : 502;
    return ServerResponse::json(status, {{"error", error.what()}});
  } catch (const std::invalid_argument& error) {
    return ServerResponse::json(400, {{"error", error.what()}});
  } catch (const std::exception& error) {
    return ServerResponse::json(409, {{"error", error.what()}});
  }
}

}  // namespace mine_teleop
