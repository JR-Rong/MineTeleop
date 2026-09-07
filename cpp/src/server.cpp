#include "mine_teleop/server.hpp"
#include "mine_teleop/credentials.hpp"
#include "mine_teleop/control_logic_js.hpp"
#include "mine_teleop/detail/clock_deadline.hpp"
#include "mine_teleop/console_assets.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
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
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace mine_teleop {

class AsyncControlTrace {
 public:
  using Emitter = std::function<void(Json)>;

  explicit AsyncControlTrace(Emitter emitter) : emitter_(std::move(emitter)) {
    if (!emitter_) throw std::invalid_argument("control trace emitter is required");
    accepting_.store(true, std::memory_order_release);
    try {
      worker_ = std::thread([this] { worker_loop(); });
    } catch (...) {
      accepting_.store(false, std::memory_order_release);
      throw;
    }
  }

  ~AsyncControlTrace() { stop(); }

  AsyncControlTrace(const AsyncControlTrace&) = delete;
  AsyncControlTrace& operator=(const AsyncControlTrace&) = delete;

  void enqueue(Json record) noexcept {
    if (!accepting_.load(std::memory_order_acquire)) {
      dropped_total_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    try {
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock.owns_lock() || stop_requested_ || queue_.size() >= kQueueCapacity) {
        dropped_total_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
        return;
      }
      queue_.push_back(std::move(record));
      enqueued_total_.fetch_add(1, std::memory_order_relaxed);
      const bool full_batch = queue_.size() >= kBatchMaxRecords;
      lock.unlock();
      if (full_batch) cv_.notify_one();
    } catch (...) {
      dropped_total_.fetch_add(1, std::memory_order_relaxed);
      cv_.notify_one();
    }
  }

  void stop() noexcept {
    accepting_.store(false, std::memory_order_release);
    bool notify = false;
    {
      std::lock_guard lock(mutex_);
      if (!stop_requested_) {
        stop_requested_ = true;
        notify = true;
      }
    }
    if (notify) cv_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

 private:
  static constexpr std::size_t kQueueCapacity = 512;
  static constexpr std::size_t kBatchMaxRecords = 32;

  void worker_loop() noexcept {
    std::uint64_t batch_seq = 0;
    std::uint64_t emitted_total = 0;
    std::uint64_t reported_dropped_total = 0;
    std::uint64_t output_error_total = 0;
    try {
      for (;;) {
        Json commands = Json::array();
        bool final = false;
        {
          std::unique_lock lock(mutex_);
          cv_.wait_for(
              lock,
              std::chrono::seconds(1),
              [this] {
                return stop_requested_ || queue_.size() >= kBatchMaxRecords;
              });
          const auto count = std::min(queue_.size(), kBatchMaxRecords);
          for (std::size_t index = 0; index < count; ++index) {
            commands.push_back(std::move(queue_.front()));
            queue_.pop_front();
          }
          final = stop_requested_ && queue_.empty();
        }

        const auto command_count = commands.size();
        const auto dropped_total = dropped_total_.load(std::memory_order_relaxed);
        if (command_count > 0 || dropped_total != reported_dropped_total || final) {
          try {
            const auto next_emitted_total = emitted_total + command_count;
            emitter_({
                {"batch_seq", ++batch_seq},
                {"final", final},
                {"queue_capacity", kQueueCapacity},
                {"commands", std::move(commands)},
                {"enqueued_total", enqueued_total_.load(std::memory_order_relaxed)},
                {"emitted_total", next_emitted_total},
                {"dropped_since_last", dropped_total - reported_dropped_total},
                {"dropped_total", dropped_total},
                {"output_error_total", output_error_total},
            });
            emitted_total = next_emitted_total;
            reported_dropped_total = dropped_total;
          } catch (...) {
            ++output_error_total;
            dropped_total_.fetch_add(command_count, std::memory_order_relaxed);
          }
        }
        if (final) return;
      }
    } catch (...) {
      // Diagnostic tracing must never terminate or alter control transport.
    }
  }

  Emitter emitter_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<Json> queue_;
  std::thread worker_;
  bool stop_requested_{false};
  std::atomic<bool> accepting_{false};
  std::atomic<std::uint64_t> enqueued_total_{0};
  std::atomic<std::uint64_t> dropped_total_{0};
};

namespace {

#if defined(_WIN32)
using NativeSocket = SOCKET;
using SocketLength = int;
constexpr int kSendFlags = 0;
constexpr int kShutdownBoth = SD_BOTH;
constexpr short kPollRead = POLLRDNORM;
constexpr short kPollWrite = POLLWRNORM;

int last_socket_error() { return WSAGetLastError(); }
bool socket_error_interrupted(int error) { return error == WSAEINTR; }
bool socket_error_would_block(int error) { return error == WSAEWOULDBLOCK; }
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
constexpr short kPollRead = POLLIN;
constexpr short kPollWrite = POLLOUT;

int last_socket_error() { return errno; }
bool socket_error_interrupted(int error) { return error == EINTR; }
bool socket_error_would_block(int error) { return error == EAGAIN || error == EWOULDBLOCK; }
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

void set_socket_nonblocking(SocketHandle socket, bool enabled) {
#if defined(_WIN32)
  u_long mode = enabled ? 1UL : 0UL;
  if (::ioctlsocket(native_socket(socket), FIONBIO, &mode) != 0) {
    throw std::runtime_error("cannot configure HTTP client socket: " + socket_error_message(last_socket_error()));
  }
#else
  const int flags = ::fcntl(native_socket(socket), F_GETFL, 0);
  if (flags < 0 || ::fcntl(
                       native_socket(socket),
                       F_SETFL,
                       enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) != 0) {
    throw std::runtime_error("cannot configure HTTP client socket: " + socket_error_message(last_socket_error()));
  }
#endif
}

std::chrono::milliseconds remaining_until(std::chrono::steady_clock::time_point deadline) {
  const auto now = std::chrono::steady_clock::now();
  if (now >= deadline) return std::chrono::milliseconds::zero();
  const auto remaining = deadline - now;
  auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
  if (milliseconds < remaining) ++milliseconds;
  return std::max(milliseconds, std::chrono::milliseconds(1));
}

bool wait_socket_until(
    SocketHandle socket,
    short events,
    std::chrono::steady_clock::time_point deadline) {
#if defined(_WIN32)
  WSAPOLLFD descriptor{native_socket(socket), events, 0};
#else
  pollfd descriptor{native_socket(socket), events, 0};
#endif
  while (true) {
    const auto remaining = remaining_until(deadline);
    if (remaining <= std::chrono::milliseconds::zero()) return false;
    const auto timeout = static_cast<int>(std::min<std::int64_t>(
        remaining.count(), static_cast<std::int64_t>(std::numeric_limits<int>::max())));
#if defined(_WIN32)
    const int result = ::WSAPoll(&descriptor, 1, timeout);
#else
    const int result = ::poll(&descriptor, 1, timeout);
#endif
    if (result > 0) return (descriptor.revents & (events | POLLERR | POLLHUP | POLLNVAL)) != 0;
    if (result == 0) return false;
    const int error = last_socket_error();
    if (!socket_error_interrupted(error)) {
      throw std::runtime_error("HTTP socket poll failed: " + socket_error_message(error));
    }
  }
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

class CleansedString final {
 public:
  explicit CleansedString(std::string value) : value_(std::move(value)) {}
  ~CleansedString() { cleanse_secret(value_); }

  CleansedString(const CleansedString&) = delete;
  CleansedString& operator=(const CleansedString&) = delete;

  [[nodiscard]] std::string_view view() const noexcept { return value_; }

 private:
  std::string value_;
};

class ServiceUnavailable final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
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

std::int64_t required_positive_integer_node(
    const YAML::Node& node,
    std::string_view field,
    std::string_view field_display,
    std::int64_t fallback) {
  const std::string name(field);
  if (!node || !node.IsMap() || !node[name]) return fallback;
  std::int64_t value = 0;
  try {
    value = node[name].as<std::int64_t>();
  } catch (const YAML::Exception& error) {
    throw std::invalid_argument(std::string(field_display) + " must be an integer: " + error.what());
  }
  if (value <= 0) throw std::invalid_argument(std::string(field_display) + " must be positive");
  return value;
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

bool valid_legacy_password_removal_date(std::string_view value) {
  if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
  for (const auto index : std::array<std::size_t, 8>{0, 1, 2, 3, 5, 6, 8, 9}) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
      (value[2] - '0') * 10 + (value[3] - '0');
  const unsigned month = static_cast<unsigned>((value[5] - '0') * 10 + (value[6] - '0'));
  const unsigned day = static_cast<unsigned>((value[8] - '0') * 10 + (value[9] - '0'));
  return std::chrono::year_month_day{
      std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}}
      .ok();
}

bool legacy_password_migration_is_active(std::string_view value) {
  if (!valid_legacy_password_removal_date(value)) return false;
  const int year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
      (value[2] - '0') * 10 + (value[3] - '0');
  const unsigned month = static_cast<unsigned>((value[5] - '0') * 10 + (value[6] - '0'));
  const unsigned day = static_cast<unsigned>((value[8] - '0') * 10 + (value[9] - '0'));
  const std::chrono::sys_days removal_day{
      std::chrono::year_month_day{
          std::chrono::year{year}, std::chrono::month{month}, std::chrono::day{day}}};
  const std::chrono::sys_days today =
      std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now());
  return today <= removal_day;
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

std::int64_t control_lease_renew_at(
    MonotonicMillis received_at,
    MonotonicMillis expires_at) {
  if (expires_at.value <= received_at.value) return received_at.value;
  return detail::saturating_deadline_ms(
      received_at.value,
      (expires_at.value - received_at.value) / 3);
}

std::int64_t monotonic_now_ms() {
  return process_monotonic_now_ms().value;
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
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 413: return "Payload Too Large";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Response";
  }
}

class HttpDeadlineExceeded final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

std::size_t receive_until(
    SocketHandle socket,
    char* output,
    std::size_t output_size,
    std::chrono::steady_clock::time_point deadline,
    std::string_view phase) {
  while (true) {
    if (!wait_socket_until(socket, kPollRead, deadline)) {
      throw HttpDeadlineExceeded("HTTP " + std::string(phase) + " timed out");
    }
    const auto result = ::recv(
        native_socket(socket), output, socket_buffer_size(output_size), 0);
    if (result < 0) {
      const int error = last_socket_error();
      if (socket_error_interrupted(error) || socket_error_would_block(error)) continue;
      throw std::runtime_error("recv failed: " + socket_error_message(error));
    }
    return static_cast<std::size_t>(result);
  }
}

void send_all_until(
    SocketHandle socket,
    std::string_view value,
    std::chrono::steady_clock::time_point deadline) {
  std::size_t sent = 0;
  while (sent < value.size()) {
    if (!wait_socket_until(socket, kPollWrite, deadline)) {
      throw HttpDeadlineExceeded("HTTP response write timed out");
    }
    const auto result = ::send(
        native_socket(socket),
        value.data() + sent,
        socket_buffer_size(value.size() - sent),
        kSendFlags);
    if (result < 0) {
      const int error = last_socket_error();
      if (socket_error_interrupted(error) || socket_error_would_block(error)) continue;
      throw std::runtime_error("send failed: " + socket_error_message(error));
    }
    if (result == 0) throw std::runtime_error("connection closed while sending response");
    sent += static_cast<std::size_t>(result);
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

std::string http_response_header(const ServerResponse& response) {
  std::ostringstream header;
  header << "HTTP/1.1 " << response.status << ' ' << status_reason(response.status) << "\r\n"
         << "Content-Type: " << response.content_type << "\r\n"
         << "Content-Length: " << response.body.size() << "\r\n"
         << "Connection: close\r\n";
  for (const auto& [name, value] : response.headers) header << name << ": " << value << "\r\n";
  header << "\r\n";
  return header.str();
}

void send_http_response_until(
    SocketHandle socket,
    const ServerResponse& response,
    std::chrono::steady_clock::time_point deadline) {
  const auto header = http_response_header(response);
  send_all_until(socket, header, deadline);
  send_all_until(socket, response.body, deadline);
}

void send_http_response(SocketHandle socket, const ServerResponse& response) {
  const auto header = http_response_header(response);
  send_all(socket, header);
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

std::size_t parse_content_length(std::string_view value) {
  if (value.empty()) throw std::invalid_argument("invalid Content-Length header");
  std::size_t result = 0;
  for (const unsigned char character : value) {
    if (character < '0' || character > '9') {
      throw std::invalid_argument("invalid Content-Length header");
    }
    const auto digit = static_cast<std::size_t>(character - '0');
    if (result > (std::numeric_limits<std::size_t>::max() - digit) / 10U) {
      throw std::invalid_argument("invalid Content-Length header");
    }
    result = result * 10U + digit;
  }
  return result;
}

HttpRequest parse_request(
    SocketHandle socket,
    std::size_t max_body_bytes,
    std::chrono::milliseconds header_read_timeout,
    std::chrono::milliseconds body_read_timeout) {
  constexpr std::size_t max_headers = 64 * 1024;
  std::string wire;
  std::array<char, 16 * 1024> buffer{};
  std::size_t header_end = std::string::npos;
  const auto header_deadline = std::chrono::steady_clock::now() + header_read_timeout;
  while ((header_end = wire.find("\r\n\r\n")) == std::string::npos) {
    const auto received = receive_until(
        socket, buffer.data(), buffer.size(), header_deadline, "header read");
    if (received == 0) throw std::invalid_argument("client closed before sending HTTP headers");
    wire.append(buffer.data(), received);
    if (wire.size() > max_headers) throw std::invalid_argument("HTTP headers too large");
  }

  std::istringstream headers(wire.substr(0, header_end));
  HttpRequest request;
  std::string request_line;
  if (!std::getline(headers, request_line)) throw std::invalid_argument("missing HTTP request line");
  request_line = trim(std::move(request_line));
  std::istringstream line(request_line);
  std::string version;
  if (!(line >> request.method >> request.target >> version) ||
      (version != "HTTP/1.0" && version != "HTTP/1.1")) {
    throw std::invalid_argument("invalid HTTP request line");
  }
  std::string header;
  while (std::getline(headers, header)) {
    header = trim(std::move(header));
    if (header.empty()) continue;
    const auto separator = header.find(':');
    if (separator == std::string::npos) throw std::invalid_argument("invalid HTTP header");
    const auto name = lower(trim(header.substr(0, separator)));
    if (name.empty()) throw std::invalid_argument("invalid HTTP header");
    if (name == "transfer-encoding") {
      throw std::invalid_argument("Transfer-Encoding is not supported");
    }
    if (name == "content-length" && request.headers.contains(name)) {
      throw std::invalid_argument("duplicate Content-Length header");
    }
    request.headers[name] = trim(header.substr(separator + 1));
  }

  std::size_t content_length = 0;
  if (const auto found = request.headers.find("content-length"); found != request.headers.end()) {
    content_length = parse_content_length(found->second);
  }
  if (content_length > max_body_bytes) throw std::length_error("request body too large");
  const auto body_start = header_end + 4;
  const auto body_deadline = std::chrono::steady_clock::now() + body_read_timeout;
  while (wire.size() - body_start < content_length) {
    const auto received = receive_until(
        socket, buffer.data(), buffer.size(), body_deadline, "body read");
    if (received == 0) throw std::invalid_argument("client closed before sending HTTP body");
    wire.append(buffer.data(), received);
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

bool websocket_upgrade_requested(const HttpRequest& request) {
  const auto upgrade = request.headers.find("upgrade");
  if (upgrade == request.headers.end() || lower(trim(upgrade->second)) != "websocket") return false;
  const auto connection = request.headers.find("connection");
  if (connection == request.headers.end()) return false;
  std::size_t start = 0;
  while (start <= connection->second.size()) {
    const auto end = connection->second.find(',', start);
    const auto token = lower(trim(connection->second.substr(
        start,
        end == std::string::npos ? std::string::npos : end - start)));
    if (token == "upgrade") return true;
    if (end == std::string::npos) break;
    start = end + 1;
  }
  return false;
}

struct LoopbackHttpAuthority {
  std::string host;
  std::uint16_t effective_port{80};
};

std::optional<LoopbackHttpAuthority> loopback_http_authority(std::string authority) {
  authority = lower(trim(std::move(authority)));
  std::string host;
  std::string_view port;
  for (const std::string_view candidate : {"127.0.0.1", "localhost", "[::1]"}) {
    if (authority == candidate) {
      return LoopbackHttpAuthority{std::string(candidate), 80};
    }
    if (authority.starts_with(std::string(candidate) + ":")) {
      host = candidate;
      port = std::string_view(authority).substr(candidate.size() + 1);
      break;
    }
  }
  if (host.empty()) return std::nullopt;
  if (port.empty() || !std::all_of(port.begin(), port.end(), [](unsigned char value) {
        return std::isdigit(value) != 0;
      })) {
    return std::nullopt;
  }
  try {
    const auto parsed = std::stoul(std::string(port));
    if (parsed == 0 || parsed > 65535) return std::nullopt;
    return LoopbackHttpAuthority{std::move(host), static_cast<std::uint16_t>(parsed)};
  } catch (const std::exception&) {
    return std::nullopt;
  }
}

bool application_json_content_type(const HttpRequest& request) {
  const auto found = request.headers.find("content-type");
  if (found == request.headers.end()) return false;
  const auto value = lower(trim(found->second));
  const auto separator = value.find(';');
  return trim(value.substr(0, separator)) == "application/json";
}

bool trusted_local_mutation_request(const HttpRequest& request, std::string_view page_capability) {
  const auto host = request.headers.find("host");
  const auto origin = request.headers.find("origin");
  const auto capability = request.headers.find("x-mine-teleop-page-capability");
  if (host == request.headers.end() || origin == request.headers.end() || capability == request.headers.end()) {
    return false;
  }
  const auto host_authority = loopback_http_authority(host->second);
  auto normalized_origin = lower(trim(origin->second));
  constexpr std::string_view http_scheme = "http://";
  if (!host_authority || !normalized_origin.starts_with(http_scheme)) return false;
  const auto origin_authority = loopback_http_authority(normalized_origin.substr(http_scheme.size()));
  if (!origin_authority || host_authority->host != origin_authority->host ||
      host_authority->effective_port != origin_authority->effective_port ||
      capability->second != page_capability) {
    return false;
  }
  const auto fetch_site = request.headers.find("sec-fetch-site");
  return fetch_site == request.headers.end() || lower(trim(fetch_site->second)) == "same-origin";
}

Json console_config_json(const DriverConfig& config, std::string_view page_capability) {
  return {
      {"page_capability", page_capability},
      {"rate_hz", config.rate_hz},
      {"intent_lease_ms", config.intent_lease_ms},
      {"estop_hold_ms", config.estop_hold_ms},
      {"max_time_sync_uncertainty_ms", config.max_time_sync_uncertainty_ms},
      {"ice_transport_policy", config.ice_transport_policy},
      {"control_trace_commands", config.control_trace_commands},
      {"control_limits",
       {
           {"initial_target_speed_kph", config.control_limits.initial_target_speed_kph},
           {"initial_max_motor_torque_nm", config.control_limits.initial_max_motor_torque_nm},
           {"initial_max_brake_pressure_bar", config.control_limits.initial_max_brake_pressure_bar},
           {"initial_service_brake_pressure_bar", config.control_limits.initial_service_brake_pressure_bar},
           {"initial_hard_brake_pressure_bar", config.control_limits.initial_hard_brake_pressure_bar},
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
}

// Every DriverConsoleHttpApp response is same-origin console material and is
// served from this process only. The strict CSP leaves no inline script or
// style path, and the console/config bodies carry no-store so that refresh and
// navigation never reuse stale capability-bound state from cache. Apply these
// once at the dispatch boundary so an asset route cannot accidentally emit
// duplicate CSP/XFO fields.
void add_console_security_headers(ServerResponse& response) {
  response.headers.emplace_back("X-Frame-Options", "DENY");
  response.headers.emplace_back("X-Content-Type-Options", "nosniff");
  response.headers.emplace_back("Content-Security-Policy",
                                "default-src 'none'; script-src 'self'; style-src 'self'; "
                                "img-src 'self' data:; media-src 'self' blob:; "
                                "connect-src 'self'; frame-ancestors 'none'; "
                                "base-uri 'none'; object-src 'none'; form-action 'none'");
  response.headers.emplace_back("Referrer-Policy", "no-referrer");
  response.headers.emplace_back("Cache-Control", "no-store");
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
  if (auth["allow_legacy_passwords"]) {
    try {
      config.allow_legacy_passwords = auth["allow_legacy_passwords"].as<bool>();
    } catch (const YAML::Exception& error) {
      throw std::invalid_argument(
          "auth.allow_legacy_passwords must be a boolean: " + std::string(error.what()));
    }
  }
  if (config.allow_legacy_passwords) {
    config.legacy_passwords_remove_by = required_yaml_string(
        auth, "legacy_passwords_remove_by", "auth");
  }
  config.driver_passwords.clear();
  config.driver_password_verifiers.clear();
  config.device_tokens.clear();
  config.driver_vehicle_permissions.clear();
  const auto base_path = std::filesystem::absolute(path).parent_path();
  for (std::size_t index = 0; index < drivers.size(); ++index) {
    const auto entry = drivers[index];
    const auto context = "auth.drivers[" + std::to_string(index) + "]";
    const auto driver_id = required_yaml_string(entry, "id", context);
    const bool has_legacy_source = entry["password_file"] || entry["password_env"];
    const bool has_verifier_source = entry["password_hash_file"] || entry["password_hash_env"];
    if (has_legacy_source && has_verifier_source) {
      throw std::invalid_argument(
          context + " must configure either a legacy password source or an Argon2id verifier source, not both");
    }
    if (!has_legacy_source && !has_verifier_source) {
      throw std::invalid_argument(
          context + " must configure exactly one of password_hash_file or password_hash_env");
    }
    if (has_legacy_source) {
      if (!config.allow_legacy_passwords) {
        throw std::invalid_argument(
            context + " uses a legacy plaintext password source but auth.allow_legacy_passwords is not true");
      }
      const auto password = load_identity_secret(
          entry, "password_file", "password_env", base_path, context);
      if (!config.driver_passwords.emplace(driver_id, password).second) {
        throw std::invalid_argument("duplicate driver id: " + driver_id);
      }
    } else {
      const auto verifier = load_identity_secret(
          entry, "password_hash_file", "password_hash_env", base_path, context);
      std::string reason;
      if (!validate_argon2id_verifier(verifier, default_argon2id_policy(), &reason)) {
        throw std::invalid_argument(context + " Argon2id verifier is invalid: " + reason);
      }
      if (!config.driver_password_verifiers.emplace(driver_id, verifier).second) {
        throw std::invalid_argument("duplicate driver id: " + driver_id);
      }
    }
    if (config.driver_passwords.contains(driver_id) &&
        config.driver_password_verifiers.contains(driver_id)) {
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
  const auto limits = root["connection_limits"];
  if (limits) {
    if (!limits.IsMap()) throw std::invalid_argument("connection_limits must be a mapping");
    // Partial mappings inherit every omitted field from the R05
    // default-constructed ConnectionLimits (SignalingServerConfig already
    // carries those defaults), so only explicitly-present keys are validated.
    config.connection_limits.max_active_connections = static_cast<std::size_t>(
        required_positive_integer_node(
            limits,
            "max_active_connections",
            "connection_limits.max_active_connections",
            static_cast<std::int64_t>(config.connection_limits.max_active_connections)));
    config.connection_limits.max_pending_http_connections = static_cast<std::size_t>(
        required_positive_integer_node(
            limits,
            "max_pending_http_connections",
            "connection_limits.max_pending_http_connections",
            static_cast<std::int64_t>(config.connection_limits.max_pending_http_connections)));
    config.connection_limits.max_websocket_connections = static_cast<std::size_t>(
        required_positive_integer_node(
            limits,
            "max_websocket_connections",
            "connection_limits.max_websocket_connections",
            static_cast<std::int64_t>(config.connection_limits.max_websocket_connections)));
    config.connection_limits.max_connections_per_source = static_cast<std::size_t>(
        required_positive_integer_node(
            limits,
            "max_connections_per_source",
            "connection_limits.max_connections_per_source",
            static_cast<std::int64_t>(config.connection_limits.max_connections_per_source)));
    const auto listen_backlog = required_positive_integer_node(
        limits,
        "listen_backlog",
        "connection_limits.listen_backlog",
        config.connection_limits.listen_backlog);
    if (listen_backlog > std::numeric_limits<int>::max()) {
      throw std::invalid_argument("connection_limits.listen_backlog is too large");
    }
    config.connection_limits.listen_backlog = static_cast<int>(listen_backlog);
    config.connection_limits.header_read_timeout = std::chrono::milliseconds(
        required_positive_integer_node(
            limits,
            "header_read_timeout_ms",
            "connection_limits.header_read_timeout_ms",
            config.connection_limits.header_read_timeout.count()));
    config.connection_limits.body_read_timeout = std::chrono::milliseconds(
        required_positive_integer_node(
            limits,
            "body_read_timeout_ms",
            "connection_limits.body_read_timeout_ms",
            config.connection_limits.body_read_timeout.count()));
    config.connection_limits.response_write_timeout = std::chrono::milliseconds(
        required_positive_integer_node(
            limits,
            "response_write_timeout_ms",
            "connection_limits.response_write_timeout_ms",
            config.connection_limits.response_write_timeout.count()));
    config.connection_limits.overload_write_timeout = std::chrono::milliseconds(
        required_positive_integer_node(
            limits,
            "overload_write_timeout_ms",
            "connection_limits.overload_write_timeout_ms",
            config.connection_limits.overload_write_timeout.count()));
    validate_connection_limits(config.connection_limits);
  }
  return config;
}

void validate_connection_limits(const SimpleHttpServer::ConnectionLimits& limits) {
  if (limits.max_active_connections == 0) {
    throw std::invalid_argument("connection_limits.max_active_connections must be positive");
  }
  if (limits.max_pending_http_connections == 0) {
    throw std::invalid_argument("connection_limits.max_pending_http_connections must be positive");
  }
  if (limits.max_connections_per_source == 0) {
    throw std::invalid_argument("connection_limits.max_connections_per_source must be positive");
  }
  if (limits.listen_backlog <= 0) {
    throw std::invalid_argument("connection_limits.listen_backlog must be positive");
  }
  if (limits.header_read_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("connection_limits.header_read_timeout_ms must be positive");
  }
  if (limits.body_read_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("connection_limits.body_read_timeout_ms must be positive");
  }
  if (limits.response_write_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("connection_limits.response_write_timeout_ms must be positive");
  }
  if (limits.overload_write_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("connection_limits.overload_write_timeout_ms must be positive");
  }
  if (limits.max_pending_http_connections > limits.max_active_connections) {
    throw std::invalid_argument(
        "connection_limits.max_pending_http_connections must not exceed "
        "connection_limits.max_active_connections");
  }
  if (limits.max_websocket_connections > limits.max_active_connections) {
    throw std::invalid_argument(
        "connection_limits.max_websocket_connections must not exceed "
        "connection_limits.max_active_connections");
  }
  // The standalone signaling server always installs a WSS handler, so a valid
  // budget must always reserve post-upgrade capacity for WebSocket channels.
  if (limits.max_websocket_connections == 0 ||
      limits.max_pending_http_connections >= limits.max_active_connections) {
    throw std::invalid_argument(
        "connection_limits must reserve active capacity for WebSocket connections "
        "(max_websocket_connections must be positive and max_pending_http_connections "
        "must be less than max_active_connections)");
  }
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
    : SimpleHttpServer(
          std::move(host),
          port,
          std::move(handler),
          max_body_bytes,
          std::move(websocket_handler),
          ConnectionLimits{}) {}

SimpleHttpServer::SimpleHttpServer(
    std::string host,
    std::uint16_t port,
    Handler handler,
    std::size_t max_body_bytes,
    WebSocketHandler websocket_handler,
    ConnectionLimits connection_limits)
    : host_(std::move(host)),
      requested_port_(port),
      handler_(std::move(handler)),
      max_body_bytes_(max_body_bytes),
      websocket_handler_(std::move(websocket_handler)),
      connection_limits_(std::move(connection_limits)) {
  if (host_.empty()) throw std::invalid_argument("HTTP host must not be empty");
  if (!handler_) throw std::invalid_argument("HTTP handler is required");
  if (max_body_bytes_ == 0) throw std::invalid_argument("HTTP max body size must be positive");
  if (connection_limits_.max_active_connections == 0 ||
      connection_limits_.max_pending_http_connections == 0 ||
      connection_limits_.max_connections_per_source == 0 ||
      connection_limits_.listen_backlog <= 0 ||
      connection_limits_.header_read_timeout <= std::chrono::milliseconds::zero() ||
      connection_limits_.body_read_timeout <= std::chrono::milliseconds::zero() ||
      connection_limits_.response_write_timeout <= std::chrono::milliseconds::zero() ||
      connection_limits_.overload_write_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("HTTP connection limits and deadlines must be positive");
  }
  if (connection_limits_.max_pending_http_connections > connection_limits_.max_active_connections ||
      connection_limits_.max_websocket_connections > connection_limits_.max_active_connections) {
    throw std::invalid_argument("HTTP connection sub-limits must not exceed the active connection limit");
  }
  if (websocket_handler_ &&
      (connection_limits_.max_websocket_connections == 0 ||
       connection_limits_.max_pending_http_connections >= connection_limits_.max_active_connections)) {
    throw std::invalid_argument("HTTP limits must reserve active capacity for WebSocket connections");
  }
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
        ::listen(native_socket(listener_fd_), connection_limits_.listen_backlog) == 0) {
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

bool SimpleHttpServer::try_register_client(SocketHandle client_fd, std::string source) {
  std::lock_guard lock(clients_mutex_);
  const auto source_count = connections_by_source_.find(source);
  if (stopping_ || client_sockets_.size() >= connection_limits_.max_active_connections ||
      pending_http_connections_ >= connection_limits_.max_pending_http_connections ||
      (source_count != connections_by_source_.end() &&
       source_count->second >= connection_limits_.max_connections_per_source)) {
    return false;
  }
  const auto [socket, inserted] = client_sockets_.insert(client_fd);
  if (!inserted) return false;
  try {
    const auto [stored_source, source_inserted] = client_sources_.emplace(client_fd, std::move(source));
    if (!source_inserted) {
      client_sockets_.erase(socket);
      return false;
    }
    ++connections_by_source_[stored_source->second];
    ++pending_http_connections_;
    return true;
  } catch (...) {
    client_sources_.erase(client_fd);
    client_sockets_.erase(socket);
    throw;
  }
}

bool SimpleHttpServer::try_promote_client_to_websocket(SocketHandle client_fd) {
  std::lock_guard lock(clients_mutex_);
  if (!client_sockets_.contains(client_fd) || websocket_sockets_.contains(client_fd) ||
      websocket_sockets_.size() >= connection_limits_.max_websocket_connections) {
    return false;
  }
  websocket_sockets_.insert(client_fd);
  if (pending_http_connections_ > 0) --pending_http_connections_;
  return true;
}

bool SimpleHttpServer::try_demote_client_from_websocket(SocketHandle client_fd) {
  std::lock_guard lock(clients_mutex_);
  if (!websocket_sockets_.contains(client_fd) ||
      pending_http_connections_ >= connection_limits_.max_pending_http_connections) {
    return false;
  }
  websocket_sockets_.erase(client_fd);
  ++pending_http_connections_;
  return true;
}

void SimpleHttpServer::unregister_client(SocketHandle client_fd) {
  {
    std::lock_guard lock(clients_mutex_);
    const auto client = client_sockets_.find(client_fd);
    if (client == client_sockets_.end()) return;
    if (websocket_sockets_.erase(client_fd) == 0 && pending_http_connections_ > 0) {
      --pending_http_connections_;
    }
    if (const auto source = client_sources_.find(client_fd); source != client_sources_.end()) {
      if (const auto count = connections_by_source_.find(source->second); count != connections_by_source_.end()) {
        if (count->second > 1) {
          --count->second;
        } else {
          connections_by_source_.erase(count);
        }
      }
      client_sources_.erase(source);
    }
    client_sockets_.erase(client);
  }
  clients_stopped_.notify_all();
}

void SimpleHttpServer::serve_client(SocketHandle client_fd) {
  ServerResponse response;
  bool socket_is_nonblocking = true;
  try {
    auto request = parse_request(
        client_fd,
        max_body_bytes_,
        connection_limits_.header_read_timeout,
        connection_limits_.body_read_timeout);
    request.peer_address = socket_peer_address(client_fd);
    if (websocket_handler_) {
      const bool websocket_upgrade = websocket_upgrade_requested(request);
      if (websocket_upgrade && !try_promote_client_to_websocket(client_fd)) {
        response = ServerResponse::json(503, {{"error", "WebSocket connection budget exhausted"}});
      } else {
        // The established WebSocket implementation expects the blocking socket
        // contract it had before HTTP parsing became deadline-driven.
        set_socket_nonblocking(client_fd, false);
        socket_is_nonblocking = false;
        if (websocket_handler_(client_fd, request)) return;
        set_socket_nonblocking(client_fd, true);
        socket_is_nonblocking = true;
        if (websocket_upgrade && !try_demote_client_from_websocket(client_fd)) {
          // A request that advertised an upgrade consumed the WebSocket slot
          // before the handler decided it was ordinary HTTP.  It must regain
          // the pending-HTTP slot before reaching the normal handler; otherwise
          // an upgrade-shaped request could bypass that bounded pre-auth pool.
          response = ServerResponse::json(503, {{"error", "HTTP connection budget exhausted"}});
        } else {
          response = handler_(request);
        }
      }
    } else {
      response = handler_(request);
    }
  } catch (const HttpDeadlineExceeded& error) {
    response = ServerResponse::json(408, {{"error", error.what()}});
  } catch (const std::length_error& error) {
    response = ServerResponse::json(413, {{"error", error.what()}});
  } catch (const std::invalid_argument& error) {
    response = ServerResponse::json(400, {{"error", error.what()}});
  } catch (const std::exception& error) {
    response = ServerResponse::json(500, {{"error", error.what()}});
  }
  if (!socket_is_nonblocking) {
    try {
      set_socket_nonblocking(client_fd, true);
    } catch (const std::exception&) {
      return;
    }
  }
  try {
    send_http_response_until(
        client_fd,
        response,
        std::chrono::steady_clock::now() + connection_limits_.response_write_timeout);
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
    try {
      set_socket_nonblocking(client, true);
    } catch (const std::exception&) {
      shutdown_socket(client);
      close_socket(client);
      continue;
    }
    if (!try_register_client(client, socket_peer_address(client))) {
      try {
        send_http_response_until(
            client,
            ServerResponse::json(503, {{"error", "HTTP connection budget exhausted"}}),
            std::chrono::steady_clock::now() + connection_limits_.overload_write_timeout);
      } catch (const std::exception&) {
      }
      shutdown_socket(client);
      close_socket(client);
      continue;
    }
    try {
      std::thread([this, client] {
        try {
          serve_client(client);
        } catch (const std::exception&) {
        }
        shutdown_socket(client);
        close_socket(client);
        unregister_client(client);
      }).detach();
    } catch (...) {
      shutdown_socket(client);
      close_socket(client);
      unregister_client(client);
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
    value["control_token_expires_at_utc_ms"] = control_token_expires_at_utc_ms;
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
    std::function<std::int64_t()> audit_clock,
    ClockSampler clock_sampler)
    : config_(std::move(config)),
      service_instance_id_("service-" + random_token(12)),
      audit_clock_(std::move(audit_clock)),
      clock_sampler_(std::move(clock_sampler)) {
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
  if (config_.password_verification_max_concurrency == 0 ||
      config_.password_verification_max_concurrency > 16 ||
      config_.password_verification_retry_after_ms <= 0 ||
      config_.password_verification_retry_after_ms > 60 * 1000) {
    throw std::invalid_argument(
        "password verification concurrency and retry budget are outside the supported range");
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
      config_.max_ice_candidate_bytes == 0 || config_.signaling_message_ttl_ms <= 0 ||
      config_.native_control_message_ttl_ms <= 0 ||
      config_.native_control_message_ttl_ms > 1000 ||
      config_.max_signaling_queue_messages == 0 || config_.max_signaling_queue_bytes == 0 ||
      config_.websocket_rate_limit_messages <= 0 || config_.websocket_rate_limit_bytes == 0 ||
      config_.websocket_rate_limit_window_ms <= 0) {
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
  if (config_.driver_passwords.empty() && config_.driver_password_verifiers.empty()) {
    throw std::invalid_argument("at least one driver credential is required");
  }
  if (!config_.allow_legacy_passwords && !config_.driver_passwords.empty()) {
    throw std::invalid_argument(
        "legacy plaintext driver passwords require allow_legacy_passwords=true");
  }
  if (config_.allow_legacy_passwords && !config_.driver_passwords.empty() &&
      !valid_legacy_password_removal_date(config_.legacy_passwords_remove_by)) {
    throw std::invalid_argument(
        "legacy plaintext driver passwords require a valid YYYY-MM-DD legacy_passwords_remove_by deadline");
  }
  if (config_.allow_legacy_passwords && !config_.driver_passwords.empty() &&
      !legacy_password_migration_is_active(config_.legacy_passwords_remove_by)) {
    throw std::invalid_argument(
        "legacy plaintext driver passwords are past their legacy_passwords_remove_by deadline");
  }
  if (config_.device_tokens.empty()) throw std::invalid_argument("at least one device credential is required");
  for (const auto& [id, password] : config_.driver_passwords) {
    if (id.empty() || password.empty()) throw std::invalid_argument("driver credentials must not be empty");
  }
  for (const auto& [id, verifier] : config_.driver_password_verifiers) {
    if (id.empty() || verifier.empty()) {
      throw std::invalid_argument("driver password verifiers must not be empty");
    }
    if (config_.driver_passwords.contains(id)) {
      throw std::invalid_argument("a driver cannot have both legacy and Argon2id credentials");
    }
    std::string reason;
    if (!validate_argon2id_verifier(verifier, default_argon2id_policy(), &reason)) {
      throw std::invalid_argument("driver Argon2id verifier is invalid: " + reason);
    }
  }
  for (const auto& [id, token] : config_.device_tokens) {
    if (id.empty() || token.empty()) throw std::invalid_argument("device credentials must not be empty");
  }
  for (const auto& [driver_id, vehicles] : config_.driver_vehicle_permissions) {
    if (!configured_driver(driver_id)) {
      throw std::invalid_argument("vehicle permission references an unknown driver");
    }
    for (const auto& vehicle_id : vehicles) {
      if (!config_.device_tokens.contains(vehicle_id)) {
        throw std::invalid_argument("vehicle permission references an unknown vehicle");
      }
    }
  }
  if (config_.native_control_trace_commands && !config_.audit_log_path.empty()) {
    native_control_trace_ = std::make_unique<AsyncControlTrace>(
        [this](Json details) {
          audit("cloud_native_control_trace_batch", std::move(details));
        });
  }
  if (!audit(
          "signaling_service_started",
          {{"runtime", "cpp"},
           {"native_control_trace_commands", config_.native_control_trace_commands},
           {"audit_log_max_bytes", config_.audit_log_max_bytes},
           {"audit_log_files", config_.audit_log_files},
           {"audit_log_rotation_interval_ms", config_.audit_log_rotation_interval_ms},
           {"audit_log_retention_days", config_.audit_log_retention_days}})) {
    throw std::runtime_error("signaling audit log is unavailable at startup");
  }
  connection_reaper_ = std::jthread([this](std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(config_.connection_reaper_interval_ms));
      if (stop_token.stop_requested()) break;
      try {
        const auto now = clock_sample();
        std::lock_guard lock(mutex_);
        cleanup_expired_connections(now);
        connection_reaper_healthy_.store(true);
      } catch (const std::exception& error) {
        connection_reaper_healthy_.store(false);
        connection_reaper_failures_.fetch_add(1);
        std::cerr << "mine-teleop-signaling: connection reaper recovered from error: " << error.what() << '\n';
      } catch (...) {
        connection_reaper_healthy_.store(false);
        connection_reaper_failures_.fetch_add(1);
        std::cerr << "mine-teleop-signaling: connection reaper recovered from unknown error\n";
      }
    }
  });
}

ClockSample SignalingService::clock_sample() const {
  return clock_sampler_ ? clock_sampler_() : ClockSample{utc_now_ms(), process_monotonic_now_ms()};
}

SignalingService::~SignalingService() {
  connection_reaper_.request_stop();
  if (connection_reaper_.joinable()) connection_reaper_.join();
  if (native_control_trace_) native_control_trace_->stop();
}

Json SignalingService::health() const {
  std::size_t active_password_verifications = 0;
  {
    std::lock_guard verification_lock(password_verification_mutex_);
    active_password_verifications = active_password_verifications_;
  }
  std::lock_guard lock(mutex_);
  const auto now = clock_sample();
  const auto active_sessions = std::count_if(sessions_.begin(), sessions_.end(), [](const auto& item) {
    return item.second.state == SessionState::Active || item.second.state == SessionState::Degraded;
  });
  std::uint64_t turn_relay_bytes_total = 0;
  std::size_t queued_signaling_messages = 0;
  std::size_t queued_signaling_bytes = 0;
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
  for (const auto& [key, queue] : messages_) {
    static_cast<void>(key);
    queued_signaling_messages += queue.size();
    for (const auto& message : queue) {
      queued_signaling_bytes = message.serialized_bytes > std::numeric_limits<std::size_t>::max() - queued_signaling_bytes
          ? std::numeric_limits<std::size_t>::max()
          : queued_signaling_bytes + message.serialized_bytes;
    }
  }
  const auto login_locked_buckets = std::count_if(login_failures_.begin(), login_failures_.end(), [&](const auto& item) {
    return item.second.blocked_until_monotonic_ms.has_value() &&
        !detail::monotonic_deadline_reached(
            now.monotonic,
            *item.second.blocked_until_monotonic_ms);
  });
  const bool api_rate_limit_overflow_active =
      api_rate_limit_overflow_.window_started_at_monotonic_ms.has_value() &&
      !detail::monotonic_deadline_reached(
          now.monotonic,
          detail::saturating_deadline_ms(
              *api_rate_limit_overflow_.window_started_at_monotonic_ms,
              config_.api_rate_limit_window_ms));
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
  if (!audit_healthy_.load()) {
    alerts.push_back({
        {"code", "audit_log_unavailable"},
        {"severity", "critical"},
        {"count", audit_write_failures_.load()},
    });
  }
  if (!connection_reaper_healthy_.load()) {
    alerts.push_back({
        {"code", "connection_reaper_unhealthy"},
        {"severity", "critical"},
        {"count", connection_reaper_failures_.load()},
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
      {"password_verification_active", active_password_verifications},
      {"password_verification_capacity", config_.password_verification_max_concurrency},
      {"api_rate_limit_tracked_sources", api_rate_limits_.size()},
      {"api_rate_limit_overflow_active", api_rate_limit_overflow_active},
      {"api_rate_limited_requests", api_rate_limited_requests_},
      {"queued_signaling_messages", queued_signaling_messages},
      {"queued_signaling_bytes", queued_signaling_bytes},
      {"signaling_queue_rejections", signaling_queue_rejections_},
      {"audit_healthy", audit_healthy_.load()},
      {"audit_write_failures", audit_write_failures_.load()},
      {"connection_reaper_healthy", connection_reaper_healthy_.load()},
      {"connection_reaper_failures", connection_reaper_failures_.load()},
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

void SignalingService::validate_driver_token(
    std::string_view driver_id,
    std::string_view token,
    ClockSample now) {
  if (revoked_drivers_.contains(std::string(driver_id))) throw Unauthorized("driver is revoked");
  const auto found = driver_tokens_.find(std::string(token));
  if (token.empty() || found == driver_tokens_.end() || found->second.driver_id != driver_id) {
    throw Unauthorized("invalid driver token");
  }
  if (detail::monotonic_deadline_reached(now.monotonic, found->second.expires_at_monotonic_ms)) {
    throw Unauthorized("driver token expired");
  }
  const auto presence = online_drivers_.find(std::string(driver_id));
  if (presence == online_drivers_.end() || presence->second.generation != found->second.connection_generation) {
    throw Unauthorized("driver connection is no longer current");
  }
  presence->second.last_seen_at_utc_ms = now.utc.value;
  presence->second.last_seen_at_monotonic_ms = now.monotonic.value;
}

void SignalingService::validate_device_token(std::string_view vehicle_id, std::string_view token) const {
  if (revoked_vehicles_.contains(std::string(vehicle_id))) throw Unauthorized("vehicle is revoked");
  const auto found = config_.device_tokens.find(std::string(vehicle_id));
  if (token.empty() || found == config_.device_tokens.end() ||
      !constant_time_equal(found->second, token)) {
    throw Unauthorized("invalid device token");
  }
}

void SignalingService::validate_vehicle_connection(
    std::string_view vehicle_id,
    std::string_view token,
    std::uint64_t connection_generation,
    ClockSample now) {
  validate_device_token(vehicle_id, token);
  const auto found = online_vehicles_.find(std::string(vehicle_id));
  if (found == online_vehicles_.end()) throw Conflict("vehicle is offline", "vehicle_offline");
  if (connection_generation == 0 || found->second.generation != connection_generation) {
    throw Conflict(
        "vehicle connection generation is stale",
        "vehicle_connection_generation_stale");
  }
  found->second.last_seen_at_utc_ms = now.utc.value;
  found->second.last_seen_at_monotonic_ms = now.monotonic.value;
}

void SignalingService::validate_actor_credential(
    const Session& session,
    std::string_view actor,
    const Json& value,
    ClockSample now) {
  if (actor == session.driver_id) {
    validate_driver_token(actor, optional_string(value, "token"), now);
  } else if (actor == session.vehicle_id) {
    validate_vehicle_connection(
        actor,
        optional_string(value, "device_token"),
        required_uint64(value, "connection_generation"),
        now);
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

void SignalingService::cleanup_expired_connections(ClockSample now) {
  prune_expired_signaling_messages(now);
  for (auto token = driver_tokens_.begin(); token != driver_tokens_.end();) {
    if (!detail::monotonic_deadline_reached(now.monotonic, token->second.expires_at_monotonic_ms)) {
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
    if (session.state != SessionState::Closed &&
        detail::monotonic_deadline_reached(
            now.monotonic,
            session.control_token_expires_at_monotonic_ms)) {
      close_session(session, "control_token_expired");
      audit("control_authority_expired", session.to_json());
    }
  }

  for (auto iterator = online_vehicles_.begin(); iterator != online_vehicles_.end();) {
    if (!detail::monotonic_deadline_reached(
            now.monotonic,
            detail::saturating_deadline_ms(
                iterator->second.last_seen_at_monotonic_ms,
                config_.vehicle_heartbeat_timeout_ms))) {
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
    if (!detail::monotonic_deadline_reached(
            now.monotonic,
            detail::saturating_deadline_ms(
                iterator->second.last_seen_at_monotonic_ms,
                config_.driver_heartbeat_timeout_ms))) {
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

void SignalingService::prune_expired_signaling_messages(ClockSample now) {
  for (auto queue = messages_.begin(); queue != messages_.end();) {
    std::erase_if(queue->second, [&](const auto& message) {
      return detail::monotonic_deadline_reached(
          now.monotonic,
          detail::saturating_deadline_ms(
              message.queued_at_monotonic_ms,
              config_.signaling_message_ttl_ms));
    });
    if (queue->second.empty()) {
      queue = messages_.erase(queue);
    } else {
      ++queue;
    }
  }
  for (auto message = latest_control_messages_.begin(); message != latest_control_messages_.end();) {
    if (!detail::monotonic_deadline_reached(
            now.monotonic,
            detail::saturating_deadline_ms(
                message->second.queued_at_monotonic_ms,
                config_.native_control_message_ttl_ms))) {
      ++message;
      continue;
    }
    if (native_control_trace_) {
      const auto& expired = message->second;
      native_control_trace_->enqueue({
          {"stage", "mailbox_expired"},
          {"trace_session_id", expired.metadata.session_id},
          {"vehicle_id", expired.metadata.vehicle_id},
          {"driver_id", expired.metadata.driver_id},
          {"seq", expired.metadata.seq},
          {"intent_seq", expired.payload.value("intent_seq", std::uint64_t{0})},
          {"command_sent_at_utc_ms", expired.metadata.sent_at_utc_ms},
          {"cloud_queued_at_utc_ms", expired.queued_at_utc_ms},
          {"cloud_queued_monotonic_ms", expired.queued_at_monotonic_ms},
          {"cloud_expired_at_utc_ms", now.utc.value},
          {"cloud_expired_monotonic_ms", now.monotonic.value},
          {"cloud_mailbox_age_ms", std::max<std::int64_t>(
                                         0,
                                         now.monotonic.value - expired.queued_at_monotonic_ms)},
          {"delivery_cursor", expired.delivery_cursor},
      });
    }
    message = latest_control_messages_.erase(message);
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
  session.control_token_expires_at_utc_ms = 0;
  session.control_token_expires_at_monotonic_ms = 0;
  messages_.erase(message_key(session.session_id, session.driver_id));
  messages_.erase(message_key(session.session_id, session.vehicle_id));
  latest_control_messages_.erase(message_key(session.session_id, session.driver_id));
  latest_control_messages_.erase(message_key(session.session_id, session.vehicle_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.driver_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.vehicle_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.driver_id) + ":native_control");
  next_delivery_cursors_.erase(message_key(session.session_id, session.driver_id));
  next_delivery_cursors_.erase(message_key(session.session_id, session.vehicle_id));
  if (session.state != SessionState::Stopping) transition_session(session, SessionState::Stopping, reason);
  transition_session(session, SessionState::Closed, reason);
}

bool SignalingService::configured_driver(std::string_view driver_id) const {
  const auto id = std::string(driver_id);
  return config_.driver_passwords.contains(id) ||
      config_.driver_password_verifiers.contains(id);
}

bool SignalingService::try_acquire_password_verification_slot() {
  std::lock_guard lock(password_verification_mutex_);
  if (active_password_verifications_ >= config_.password_verification_max_concurrency) {
    return false;
  }
  ++active_password_verifications_;
  return true;
}

void SignalingService::release_password_verification_slot() noexcept {
  std::lock_guard lock(password_verification_mutex_);
  if (active_password_verifications_ > 0) --active_password_verifications_;
}

SignalingService::LoginFailureReservation SignalingService::reserve_login_failure_locked(
    std::string_view driver_id,
    ClockSample admitted_at) {
  const bool known_driver = configured_driver(driver_id);
  const std::string bucket = known_driver ? "driver:" + std::string(driver_id) : "unknown";
  auto& state = login_failures_[bucket];
  if (state.blocked_until_monotonic_ms.has_value() &&
      !detail::monotonic_deadline_reached(
          admitted_at.monotonic,
          *state.blocked_until_monotonic_ms)) {
    throw TooManyRequests(
        "too many login attempts",
        std::max<std::int64_t>(
            1,
            *state.blocked_until_monotonic_ms - admitted_at.monotonic.value));
  }

  const bool window_expired =
      state.window_started_at_monotonic_ms.has_value() &&
      detail::monotonic_deadline_reached(
          admitted_at.monotonic,
          detail::saturating_deadline_ms(
              *state.window_started_at_monotonic_ms,
              config_.login_failure_window_ms));
  if (state.pending_failures == 0 &&
      (state.blocked_until_monotonic_ms.has_value() || window_expired)) {
    state = LoginFailureState{
        .failures = 0,
        .pending_failures = 0,
        .window_started_at_monotonic_ms = admitted_at.monotonic.value,
        .blocked_until_utc_ms = std::nullopt,
        .blocked_until_monotonic_ms = std::nullopt};
  } else if (state.blocked_until_monotonic_ms.has_value()) {
    // This can only occur when an already-expired lockout still has an
    // in-flight candidate. Keep that candidate in its admission window rather
    // than resetting its state based on a later KDF completion.
    state.blocked_until_utc_ms.reset();
    state.blocked_until_monotonic_ms.reset();
  }
  if (!state.window_started_at_monotonic_ms.has_value()) {
    state.window_started_at_monotonic_ms = admitted_at.monotonic.value;
  }
  if (state.failures >= config_.login_max_failures ||
      state.pending_failures >= config_.login_max_failures - state.failures) {
    throw TooManyRequests("too many login attempts", config_.login_lockout_ms);
  }

  ++state.pending_failures;
  return LoginFailureReservation{
      bucket,
      admitted_at.utc.value,
      admitted_at.monotonic.value};
}

void SignalingService::release_login_failure_reservation_locked(
    const LoginFailureReservation& reservation) {
  const auto found = login_failures_.find(reservation.bucket);
  if (found == login_failures_.end()) return;
  auto& state = found->second;
  if (state.pending_failures <= 0) return;
  --state.pending_failures;
  if (state.pending_failures == 0 && state.failures == 0 &&
      !state.blocked_until_monotonic_ms.has_value()) {
    login_failures_.erase(found);
  }
}

void SignalingService::record_login_failure_locked(
    std::string_view driver_id,
    const LoginFailureReservation& reservation,
    ClockSample settled_at) {
  const bool known_driver = configured_driver(driver_id);
  auto found = login_failures_.find(reservation.bucket);
  if (found == login_failures_.end()) {
    found = login_failures_
                .emplace(
                    reservation.bucket,
                    LoginFailureState{
                        .failures = 0,
                        .pending_failures = 1,
                        .window_started_at_monotonic_ms = reservation.admitted_at_monotonic_ms,
                        .blocked_until_utc_ms = std::nullopt,
                        .blocked_until_monotonic_ms = std::nullopt})
                .first;
  }
  auto& state = found->second;
  if (state.pending_failures <= 0) {
    state.pending_failures = 1;
    state.window_started_at_monotonic_ms = reservation.admitted_at_monotonic_ms;
  }
  --state.pending_failures;
  if (!state.window_started_at_monotonic_ms.has_value()) {
    state.window_started_at_monotonic_ms = reservation.admitted_at_monotonic_ms;
  }
  ++state.failures;
  const bool lock_login = state.failures >= config_.login_max_failures;
  if (lock_login) {
    state.blocked_until_utc_ms = detail::saturating_deadline_ms(
        settled_at.utc.value,
        config_.login_lockout_ms);
    state.blocked_until_monotonic_ms = detail::saturating_deadline_ms(
        settled_at.monotonic.value,
        config_.login_lockout_ms);
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
  limited_details["blocked_until_utc_ms"] = state.blocked_until_utc_ms.value_or(0);
  audit("driver_login_rate_limited", limited_details);
  throw TooManyRequests("too many login attempts", config_.login_lockout_ms);
}

void SignalingService::clear_login_failures_locked(std::string_view driver_id) {
  const auto found = login_failures_.find("driver:" + std::string(driver_id));
  if (found == login_failures_.end()) return;
  if (found->second.pending_failures == 0) {
    login_failures_.erase(found);
    return;
  }
  found->second.failures = 0;
  found->second.blocked_until_utc_ms.reset();
  found->second.blocked_until_monotonic_ms.reset();
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

void SignalingService::cleanup_api_rate_limits(MonotonicMillis now) {
  const auto expired = [&](const ApiRateState& state) {
    return !state.window_started_at_monotonic_ms.has_value() ||
        detail::monotonic_deadline_reached(
            now,
            detail::saturating_deadline_ms(
                *state.window_started_at_monotonic_ms,
                config_.api_rate_limit_window_ms));
  };
  std::erase_if(api_rate_limits_, [&](const auto& item) { return expired(item.second); });
  if (expired(api_rate_limit_overflow_)) api_rate_limit_overflow_ = {};
  api_rate_limit_last_cleanup_monotonic_ms_ = now.value;
}

void SignalingService::enforce_api_rate_limit(const HttpRequest& request, MonotonicMillis now) {
  if (!api_rate_limit_last_cleanup_monotonic_ms_.has_value() ||
      detail::monotonic_deadline_reached(
          now,
          detail::saturating_deadline_ms(
              *api_rate_limit_last_cleanup_monotonic_ms_,
              config_.api_rate_limit_window_ms))) {
    cleanup_api_rate_limits(now);
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

  if (!state->window_started_at_monotonic_ms.has_value() ||
      detail::monotonic_deadline_reached(
          now,
          detail::saturating_deadline_ms(
              *state->window_started_at_monotonic_ms,
              config_.api_rate_limit_window_ms))) {
    *state = ApiRateState{0, now.value, false};
  }
  if (state->requests < std::numeric_limits<std::int64_t>::max()) ++state->requests;
  if (state->requests <= config_.api_rate_limit_requests) return;

  if (api_rate_limited_requests_ < std::numeric_limits<std::uint64_t>::max()) ++api_rate_limited_requests_;
  const auto elapsed = std::max<std::int64_t>(
      0,
      now.value - *state->window_started_at_monotonic_ms);
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

bool SignalingService::audit(std::string_view event, const Json& details) const noexcept {
  if (config_.audit_log_path.empty()) return true;
  try {
    const auto timestamp_ms = audit_clock_ ? audit_clock_() : clock_sample().utc.value;
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
    audit_healthy_.store(true);
    return true;
  } catch (...) {
    audit_healthy_.store(false);
    audit_write_failures_.fetch_add(1);
    try {
      // Do not invoke the injected sampler again here: this noexcept recovery
      // path must remain non-terminating when the sampler itself is faulty.
      const auto now = process_monotonic_now_ms();
      bool report = false;
      {
        std::lock_guard fallback_lock(audit_fallback_mutex_);
        if (!audit_last_fallback_report_has_monotonic_ ||
            detail::monotonic_deadline_reached(
                now,
                detail::saturating_deadline_ms(
                    audit_last_fallback_report_monotonic_ms_,
                    60 * 1000))) {
          audit_last_fallback_report_monotonic_ms_ = now.value;
          audit_last_fallback_report_has_monotonic_ = true;
          report = true;
        }
      }
      if (report) {
        std::cerr << "mine-teleop-signaling: audit log unavailable; new control sessions are disabled\n";
      }
    } catch (...) {
      // Keep the audit failure path noexcept even if a diagnostic sink fails.
    }
    return false;
  }
}

ServerResponse SignalingService::handle(const HttpRequest& request) {
  RequestIdScope request_id("request-" + random_token(12));
  ServerResponse response;
  try {
    const auto now = clock_sample();
    {
      std::lock_guard lock(mutex_);
      enforce_api_rate_limit(request, now.monotonic);
    }
    if (request.method == "GET") {
      response = handle_get(request, now);
    } else if (request.method == "POST") {
      response = handle_post(request, now);
    } else {
      response = ServerResponse::json(405, {{"error", "method not allowed"}});
    }
  } catch (const Unauthorized& error) {
    response = ServerResponse::json(401, {{"error", error.what()}});
  } catch (const TooManyRequests& error) {
    response = too_many_requests_response(error);
  } catch (const ServiceUnavailable& error) {
    response = ServerResponse::json(503, {{"error", error.what()}, {"issue_code", "audit_log_unavailable"}});
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
    const auto now = clock_sample();
    std::lock_guard lock(mutex_);
    enforce_api_rate_limit(request, now.monotonic);
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
  const auto send_only_value = query_value(request, "send_only");
  if (!send_only_value.empty() && send_only_value != "1") {
    return reject(400, "send_only must be 1 when provided");
  }
  const bool send_only = send_only_value == "1";
  const auto requested_types = query_value(request, "types");
  const bool control_receive_only = !requested_types.empty();
  if (control_receive_only && requested_types != "control_command") {
    return reject(400, "WebSocket types must be control_command when provided");
  }
  if (control_receive_only && send_only) {
    return reject(400, "control-command receive WebSocket cannot be send-only");
  }

  Json credentials = {
      {"token", credential_value(request, "token", "x-mine-teleop-driver-token")},
      {"device_token", credential_value(request, "device_token", "x-mine-teleop-device-token")},
      {"connection_generation", query_value(request, "connection_generation")}};
  auto authenticate = [&] {
    const auto now = clock_sample();
    std::lock_guard lock(mutex_);
    cleanup_expired_connections(now);
    const auto& session = require_participant(parts[1], participant);
    validate_actor_credential(session, participant, credentials, now);
    if (send_only && participant != session.driver_id) {
      throw Unauthorized("send-only signaling is restricted to the session driver");
    }
    if (control_receive_only && participant != session.vehicle_id) {
      throw Unauthorized("control-command receive signaling is restricted to the session vehicle");
    }
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
    std::int64_t last_delivery_sent_at_monotonic_ms = 0;
    while (true) {
      Json pending = Json::array();
      std::int64_t pending_control_queued_at_monotonic_ms = 0;
      try {
        const auto now = clock_sample();
        std::lock_guard lock(mutex_);
        cleanup_expired_connections(now);
        const auto& session = require_participant(parts[1], participant);
        validate_actor_credential(session, participant, credentials, now);
        if (!send_only) {
          pending = take_signaling_messages(
              parts[1],
              participant,
              now,
              control_receive_only ? std::string_view("control_command") : std::string_view{},
              false);
          if (control_receive_only && !pending.empty()) {
            const auto queued = latest_control_messages_.find(message_key(parts[1], participant));
            if (queued != latest_control_messages_.end() &&
                queued->second.delivery_cursor ==
                    pending.back().value("delivery_cursor", std::uint64_t{0})) {
              pending_control_queued_at_monotonic_ms = queued->second.queued_at_monotonic_ms;
            }
          }
        }
      } catch (const std::exception& error) {
        connection.send_json({{"error", error.what()}, {"event", "signaling_authority_lost"}});
        connection.send_close(1008, "signaling authority lost");
        return true;
      }
      if (!send_only && !pending.empty()) {
        const auto delivery_cursor = pending.back().value("delivery_cursor", std::uint64_t{0});
        const auto send_clock = clock_sample();
        if (delivery_cursor > last_delivery_cursor_sent ||
            detail::monotonic_deadline_reached(
                send_clock.monotonic,
                detail::saturating_deadline_ms(last_delivery_sent_at_monotonic_ms, 500))) {
          Json delivery_trace;
          const bool trace_delivery = control_receive_only && native_control_trace_;
          if (trace_delivery) {
            const auto& message = pending.back();
            const auto& payload = message.value("payload", Json::object());
            delivery_trace = {
                {"stage", "delivery_send_completed"},
                {"trace_session_id", message.value("session_id", std::string(parts[1]))},
                {"vehicle_id", message.value("vehicle_id", "")},
                {"driver_id", message.value("driver_id", "")},
                {"seq", message.value("seq", std::uint64_t{0})},
                {"intent_seq", payload.value("intent_seq", std::uint64_t{0})},
                {"command_sent_at_utc_ms", message.value("sent_at_utc_ms", std::int64_t{0})},
                {"cloud_queued_at_utc_ms", message.value("queued_at_utc_ms", std::int64_t{0})},
                {"cloud_mailbox_to_send_ms", std::max<std::int64_t>(
                                                   0,
                                                   send_clock.monotonic.value -
                                                       pending_control_queued_at_monotonic_ms)},
                {"delivery_cursor", delivery_cursor},
                {"redelivery", delivery_cursor <= last_delivery_cursor_sent},
            };
          }
          const auto send_started_at_utc_ms = send_clock.utc.value;
          const auto send_started_monotonic_ms = send_clock.monotonic.value;
          try {
            connection.send_json(
                {{"event", "signaling_messages"},
                 {"delivery_cursor", delivery_cursor},
                 {"messages", std::move(pending)}});
          } catch (const std::exception& error) {
            if (trace_delivery) {
              delivery_trace["stage"] = "delivery_send_failed";
              delivery_trace["cloud_delivery_send_started_at_utc_ms"] =
                  send_started_at_utc_ms;
              delivery_trace["cloud_delivery_send_started_monotonic_ms"] =
                  send_started_monotonic_ms;
              const auto failed_at = clock_sample();
              delivery_trace["cloud_delivery_send_failed_at_utc_ms"] = failed_at.utc.value;
              delivery_trace["cloud_delivery_send_failed_monotonic_ms"] =
                  failed_at.monotonic.value;
              delivery_trace["error"] = error.what();
              native_control_trace_->enqueue(std::move(delivery_trace));
            }
            throw;
          }
          const auto send_completed_at = clock_sample();
          const auto send_completed_at_utc_ms = send_completed_at.utc.value;
          const auto send_completed_monotonic_ms = send_completed_at.monotonic.value;
          if (trace_delivery) {
            delivery_trace["cloud_delivery_send_started_at_utc_ms"] =
                send_started_at_utc_ms;
            delivery_trace["cloud_delivery_send_started_monotonic_ms"] =
                send_started_monotonic_ms;
            delivery_trace["cloud_delivery_send_completed_at_utc_ms"] =
                send_completed_at_utc_ms;
            delivery_trace["cloud_delivery_send_completed_monotonic_ms"] =
                send_completed_monotonic_ms;
            delivery_trace["cloud_delivery_send_call_ms"] =
                std::max<std::int64_t>(
                    0,
                    send_completed_monotonic_ms - send_started_monotonic_ms);
            native_control_trace_->enqueue(std::move(delivery_trace));
          }
          last_delivery_cursor_sent = std::max(last_delivery_cursor_sent, delivery_cursor);
          last_delivery_sent_at_monotonic_ms = send_clock.monotonic.value;
        }
      }

      WebSocketReceiveResult received;
      try {
        received = connection.receive_json(
            std::chrono::milliseconds(send_only ? 10 : 50));
      } catch (const std::exception& error) {
        connection.send_json({{"error", error.what()}, {"event", "websocket_protocol_error"}});
        connection.send_close(1002, "websocket protocol error");
        return true;
      }
      if (received.status == WebSocketReceiveStatus::Timeout) continue;
      if (received.status == WebSocketReceiveStatus::Closed) return true;
      try {
        const auto serialized_bytes = received.message.dump().size();
        const auto rate_now = clock_sample();
        std::optional<std::int64_t> retry_after_ms;
        {
          std::lock_guard lock(mutex_);
          cleanup_expired_connections(rate_now);
          const auto& session = require_participant(parts[1], participant);
          validate_actor_credential(session, participant, credentials, rate_now);
          auto& rate = sessions_.at(parts[1]).websocket_rate_by_participant[participant];
          if (!rate.window_started_at_monotonic_ms.has_value() ||
              detail::monotonic_deadline_reached(
                  rate_now.monotonic,
                  detail::saturating_deadline_ms(
                      *rate.window_started_at_monotonic_ms,
                      config_.websocket_rate_limit_window_ms))) {
            rate.window_started_at_monotonic_ms = rate_now.monotonic.value;
            rate.messages = 0;
            rate.bytes = 0;
          }
          const bool message_limit = rate.messages >= config_.websocket_rate_limit_messages;
          const bool byte_limit = serialized_bytes > config_.websocket_rate_limit_bytes -
              std::min(rate.bytes, config_.websocket_rate_limit_bytes);
          if (message_limit || byte_limit) {
            const auto elapsed_ms = std::max<std::int64_t>(
                0,
                rate_now.monotonic.value - *rate.window_started_at_monotonic_ms);
            retry_after_ms = std::max<std::int64_t>(
                1,
                config_.websocket_rate_limit_window_ms - elapsed_ms);
          } else {
            ++rate.messages;
            rate.bytes += serialized_bytes;
          }
        }
        if (retry_after_ms) {
          connection.send_json(
              {{"event", "signaling_rate_limited"},
               {"error", "websocket participant rate limit exceeded"},
               {"retry_after_ms", retry_after_ms.value()}});
          connection.send_close(1008, "signaling rate limit exceeded");
          return true;
        }
        if (!send_only && received.message.value("event", "") == "signaling_delivery_ack") {
          const auto delivery_cursor = required_uint64(received.message, "delivery_cursor");
          const auto reported_trace_session_id =
              received.message.value("trace_session_id", "");
          if (!reported_trace_session_id.empty() &&
              reported_trace_session_id != parts[1]) {
            throw std::invalid_argument(
                "delivery acknowledgement trace session does not match WebSocket session");
          }
          const auto reported_seq =
              received.message.value("seq", std::uint64_t{0});
          const auto reported_intent_seq =
              received.message.value("intent_seq", std::uint64_t{0});
          if (delivery_cursor > last_delivery_cursor_sent) {
            throw std::invalid_argument("delivery acknowledgement exceeds the last delivered cursor");
          }
          const auto ack_received_at = clock_sample();
          const auto ack_received_at_utc_ms = ack_received_at.utc.value;
          const auto ack_received_monotonic_ms = ack_received_at.monotonic.value;
          std::size_t acknowledged = 0;
          Json acknowledgement_trace;
          {
            std::lock_guard lock(mutex_);
            cleanup_expired_connections(ack_received_at);
            const auto& session = require_participant(parts[1], participant);
            validate_actor_credential(session, participant, credentials, ack_received_at);
            if (control_receive_only && native_control_trace_) {
              acknowledgement_trace = {
                  {"stage", "delivery_ack_received"},
                  {"trace_session_id", std::string(parts[1])},
                  {"vehicle_id", session.vehicle_id},
                  {"driver_id", session.driver_id},
                  {"seq", reported_seq},
                  {"intent_seq", reported_intent_seq},
                  {"delivery_cursor", delivery_cursor},
                  {"vehicle_reported_seq", reported_seq},
                  {"vehicle_reported_intent_seq", reported_intent_seq},
                  {"cloud_delivery_ack_received_at_utc_ms", ack_received_at_utc_ms},
                  {"cloud_delivery_ack_received_monotonic_ms", ack_received_monotonic_ms},
              };
              const auto queued = latest_control_messages_.find(
                  message_key(parts[1], participant));
              if (queued != latest_control_messages_.end() &&
                  queued->second.delivery_cursor <= delivery_cursor) {
                acknowledgement_trace["seq"] = queued->second.metadata.seq;
                acknowledgement_trace["intent_seq"] =
                    queued->second.payload.value("intent_seq", std::uint64_t{0});
                acknowledgement_trace["command_sent_at_utc_ms"] =
                    queued->second.metadata.sent_at_utc_ms;
                acknowledgement_trace["cloud_queued_at_utc_ms"] =
                    queued->second.queued_at_utc_ms;
                acknowledgement_trace["cloud_queue_to_vehicle_ack_ms"] =
                    std::max<std::int64_t>(
                        0,
                        ack_received_monotonic_ms - queued->second.queued_at_monotonic_ms);
              }
            }
            acknowledged = acknowledge_signaling_messages(
                parts[1],
                participant,
                delivery_cursor,
                control_receive_only);
          }
          if (control_receive_only && native_control_trace_) {
            acknowledgement_trace["acknowledged"] = acknowledged;
            native_control_trace_->enqueue(std::move(acknowledgement_trace));
          }
          connection.send_json(
              {{"event", "signaling_delivery_acknowledged"},
               {"delivery_cursor", delivery_cursor},
               {"acknowledged", acknowledged}});
          continue;
        }
        if (control_receive_only) {
          throw std::invalid_argument(
              "control-command receive WebSocket accepts delivery acknowledgements only");
        }
        Json acknowledgement;
        {
          const auto received_at = clock_sample();
          std::lock_guard lock(mutex_);
          cleanup_expired_connections(received_at);
          const auto& session = require_participant(parts[1], participant);
          validate_actor_credential(session, participant, credentials, received_at);
          if (send_only && received.message.value("type", "") != "control_command") {
            throw std::invalid_argument(
                "send-only control WebSocket accepts control_command messages only");
          }
          acknowledgement = enqueue_signaling_message(
              parts[1],
              received.message,
              received_at,
              participant);
        }
        connection.send_json(acknowledgement);
      } catch (const TooManyRequests& error) {
        connection.send_json(
            {{"error", error.what()},
             {"event", "signaling_backpressure"},
             {"retry_after_ms", error.retry_after_ms()}});
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
    ClockSample now,
    std::string_view requested_types,
    bool consume) {
  Json values = Json::array();
  prune_expired_signaling_messages(now);
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
  const auto requested = [&](std::string_view type) {
    return requested_types.empty() ||
        std::find(types.begin(), types.end(), type) != types.end();
  };
  const auto recipient_key = message_key(session_id, recipient);
  auto latest_control = latest_control_messages_.find(recipient_key);
  if (latest_control != latest_control_messages_.end() && requested("control_command")) {
    values.push_back(latest_control->second.to_json());
    if (consume) latest_control_messages_.erase(latest_control);
  }

  auto found = messages_.find(recipient_key);
  if (found != messages_.end()) {
    std::vector<Message> remaining;
    for (const auto& message : found->second) {
      if (requested(message.type)) {
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
  }
  std::sort(values.begin(), values.end(), [](const Json& left, const Json& right) {
    return left.value("delivery_cursor", std::uint64_t{0}) <
        right.value("delivery_cursor", std::uint64_t{0});
  });
  return values;
}

std::size_t SignalingService::acknowledge_signaling_messages(
    std::string_view session_id,
    std::string_view recipient,
    std::uint64_t delivery_cursor,
    bool control_only) {
  const auto recipient_key = message_key(session_id, recipient);
  std::size_t acknowledged = 0;
  if (!control_only) {
    auto found = messages_.find(recipient_key);
    if (found != messages_.end()) {
      const auto previous_size = found->second.size();
      std::erase_if(found->second, [&](const auto& message) {
        return message.delivery_cursor <= delivery_cursor;
      });
      acknowledged += previous_size - found->second.size();
      if (found->second.empty()) messages_.erase(found);
    }
  }
  const auto latest_control = latest_control_messages_.find(recipient_key);
  if (latest_control != latest_control_messages_.end() &&
      latest_control->second.delivery_cursor <= delivery_cursor) {
    latest_control_messages_.erase(latest_control);
    ++acknowledged;
  }
  return acknowledged;
}

Json SignalingService::enqueue_signaling_message(
    std::string_view session_id,
    const Json& value,
    ClockSample received_at,
    std::optional<std::string_view> authenticated_actor) {
  const auto cloud_ingress_started_at_utc_ms = received_at.utc.value;
  const auto cloud_ingress_started_monotonic_ms = received_at.monotonic.value;
  const auto sender = required_string(value, "sender");
  const auto recipient = required_string(value, "recipient");
  const auto type = required_string(value, "type");
  static const std::vector<std::string> allowed{
      "webrtc_offer", "webrtc_answer", "ice_candidate", "media_capabilities", "media_fallback",
      "connection_status", "telemetry", "media_status", "session_event", "control_command"};
  if (std::find(allowed.begin(), allowed.end(), type) == allowed.end()) {
    throw std::invalid_argument("unsupported signaling message type");
  }
  const auto& session = require_participant(session_id, sender);
  if (authenticated_actor.has_value()) {
    if (sender != authenticated_actor.value()) {
      throw Unauthorized("sender is not authenticated websocket participant");
    }
  } else {
    validate_actor_credential(session, sender, value, received_at);
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
  if (type == "control_command" && !driver_to_vehicle) {
    throw Unauthorized("control_command route is invalid");
  }
  if (type == "ice_candidate" && !driver_to_vehicle && !vehicle_to_driver) {
    throw Unauthorized("ice_candidate route is invalid");
  }
  const auto payload = value.value("payload", Json::object());
  if (!payload.is_object()) throw std::invalid_argument("payload must be an object");
  const auto serialized_payload = payload.dump();
  if (serialized_payload.size() > config_.max_signaling_payload_bytes) {
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
  std::uint64_t control_intent_seq = 0;
  if (type == "control_command") {
    const auto command = ControlCommand::from_json(payload);
    control_intent_seq = required_uint64(payload, "intent_seq");
    if (control_intent_seq == 0) throw std::invalid_argument("intent_seq must be positive");
    if (!payload.contains("intent_fresh") || !payload.at("intent_fresh").is_boolean()) {
      throw std::invalid_argument("intent_fresh must be a boolean");
    }
    if (command.protocol_version != metadata.protocol_version ||
        command.vehicle_id != metadata.vehicle_id ||
        command.driver_id != metadata.driver_id ||
        command.session_id != metadata.session_id ||
        command.seq != metadata.seq ||
        command.sent_at_utc_ms != metadata.sent_at_utc_ms) {
      throw std::invalid_argument("control command payload metadata does not match signaling wrapper");
    }
    if (!constant_time_equal(session.control_token, command.control_token)) {
      throw Unauthorized("control command token does not match active session");
    }
  }
  const auto sequence_key = message_key(session_id, sender) +
      (type == "control_command" ? ":native_control" : "");
  const auto fingerprint = recipient + "\n" + type + "\n" + metadata.to_json().dump() + "\n" + serialized_payload;
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
      if (type == "control_command" && native_control_trace_) {
        native_control_trace_->enqueue({
            {"stage", "ingress_duplicate_acknowledged"},
            {"trace_session_id", std::string(session_id)},
            {"vehicle_id", metadata.vehicle_id},
            {"driver_id", metadata.driver_id},
            {"seq", metadata.seq},
            {"intent_seq", control_intent_seq},
            {"command_sent_at_utc_ms", metadata.sent_at_utc_ms},
            {"cloud_received_at_utc_ms", cloud_ingress_started_at_utc_ms},
            {"cloud_received_monotonic_ms", cloud_ingress_started_monotonic_ms},
            {"delivery_cursor", acknowledgement.value("delivery_cursor", std::uint64_t{0})},
        });
      }
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
  std::uint64_t replaced_seq = 0;
  std::uint64_t replaced_delivery_cursor = 0;
  std::int64_t replaced_queued_at_utc_ms = 0;
  if (type == "control_command") {
    const auto previous = latest_control_messages_.find(recipient_key);
    if (previous != latest_control_messages_.end()) {
      replaced_seq = previous->second.metadata.seq;
      replaced_delivery_cursor = previous->second.delivery_cursor;
      replaced_queued_at_utc_ms = previous->second.queued_at_utc_ms;
    }
  }
  const auto queued_at = clock_sample();
  const auto cloud_queued_at_utc_ms = queued_at.utc.value;
  const auto cloud_queued_monotonic_ms = queued_at.monotonic.value;
  prune_expired_signaling_messages(queued_at);
  const auto serialized_bytes = value.dump().size();
  std::size_t queue_bytes = 0;
  if (type != "control_command") {
    auto& queue = messages_[recipient_key];
    for (const auto& message : queue) {
      queue_bytes = message.serialized_bytes > std::numeric_limits<std::size_t>::max() - queue_bytes
          ? std::numeric_limits<std::size_t>::max()
          : queue_bytes + message.serialized_bytes;
    }
    const bool message_capacity_reached = queue.size() >= config_.max_signaling_queue_messages;
    const bool byte_capacity_reached = serialized_bytes > config_.max_signaling_queue_bytes -
        std::min(queue_bytes, config_.max_signaling_queue_bytes);
    if (message_capacity_reached || byte_capacity_reached) {
      const auto queued_messages = queue.size();
      if (signaling_queue_rejections_ < std::numeric_limits<std::uint64_t>::max()) {
        ++signaling_queue_rejections_;
      }
      audit(
          "signaling_queue_backpressure",
          {{"session_id", session_id},
           {"sender", sender},
           {"recipient", recipient},
           {"queued_messages", queued_messages},
           {"queued_bytes", queue_bytes},
           {"message_capacity_reached", message_capacity_reached},
           {"byte_capacity_reached", byte_capacity_reached}});
      if (queued_messages == 0) messages_.erase(recipient_key);
      throw TooManyRequests(
          "signaling recipient queue capacity exceeded",
          config_.signaling_message_ttl_ms);
    }
  }
  const auto delivery_cursor = ++next_delivery_cursors_[recipient_key];
  const Message queued_message{
      metadata,
      sender,
      recipient,
      type,
      payload,
      cloud_queued_at_utc_ms,
      cloud_queued_monotonic_ms,
      delivery_cursor,
      serialized_bytes};
  std::size_t queued = 1;
  std::size_t queued_bytes = serialized_bytes;
  if (type == "control_command") {
    latest_control_messages_.insert_or_assign(recipient_key, queued_message);
  } else {
    auto& queue = messages_[recipient_key];
    queue.push_back(queued_message);
    queued = queue.size();
    queued_bytes = queue_bytes + serialized_bytes;
  }
  Json acknowledgement = {
      {"queued", queued},
      {"queued_bytes", queued_bytes},
      {"event", "signaling_ack"},
      {"type", type},
      {"seq", metadata.seq},
      {"message_id", std::string(session_id) + ":" + sender + ":" + std::to_string(metadata.seq)},
      {"delivery_cursor", delivery_cursor},
      {"duplicate", false}};
  if (type == "control_command") {
    acknowledgement["cloud_received_at_utc_ms"] = cloud_ingress_started_at_utc_ms;
    acknowledgement["cloud_queued_at_utc_ms"] = cloud_queued_at_utc_ms;
    acknowledgement["cloud_ingress_processing_ms"] = std::max<std::int64_t>(
        0,
        cloud_queued_monotonic_ms - cloud_ingress_started_monotonic_ms);
  }
  last_accepted_messages_[sequence_key] = AcceptedMessage{metadata.seq, fingerprint, acknowledgement};
  if (type == "control_command" && native_control_trace_) {
    native_control_trace_->enqueue({
        {"stage", "ingress_queued"},
        {"trace_session_id", std::string(session_id)},
        {"vehicle_id", metadata.vehicle_id},
        {"driver_id", metadata.driver_id},
        {"seq", metadata.seq},
        {"intent_seq", control_intent_seq},
        {"command_sent_at_utc_ms", metadata.sent_at_utc_ms},
        {"cloud_received_at_utc_ms", cloud_ingress_started_at_utc_ms},
        {"cloud_received_monotonic_ms", cloud_ingress_started_monotonic_ms},
        {"cloud_queued_at_utc_ms", cloud_queued_at_utc_ms},
        {"cloud_queued_monotonic_ms", cloud_queued_monotonic_ms},
        {"driver_to_cloud_utc_delta_ms",
         cloud_ingress_started_at_utc_ms - metadata.sent_at_utc_ms},
        {"cloud_ingress_processing_ms", std::max<std::int64_t>(
                                             0,
                                             cloud_queued_monotonic_ms -
                                                 cloud_ingress_started_monotonic_ms)},
        {"delivery_cursor", delivery_cursor},
        {"mailbox_replaced_seq", replaced_seq},
        {"mailbox_replaced_delivery_cursor", replaced_delivery_cursor},
        {"mailbox_replaced_age_ms",
         replaced_queued_at_utc_ms > 0
             ? std::max<std::int64_t>(0, cloud_queued_at_utc_ms - replaced_queued_at_utc_ms)
             : 0},
        {"transport", authenticated_actor.has_value() ? "websocket" : "http"},
    });
  }
  if (type != "control_command") {
    audit(
        type,
        {{"session_id", session_id},
         {"vehicle_id", metadata.vehicle_id},
         {"driver_id", metadata.driver_id},
         {"seq", metadata.seq},
         {"sender", sender},
         {"recipient", recipient},
         {"transport", authenticated_actor.has_value() ? "websocket" : "http"}});
  }
  return acknowledgement;
}

ServerResponse SignalingService::handle_get(const HttpRequest& request, ClockSample now) {
  if (request.path == "/health") return ServerResponse::json(200, health());
  if (request.path == "/time") {
    const auto server_receive_ms = now.utc.value;
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
         {"server_send_ms", clock_sample().utc.value}});
  }
  const auto parts = path_parts(request.path);
  std::lock_guard lock(mutex_);
  cleanup_expired_connections(now);
  if (parts.size() == 3 && parts[0] == "drivers" && parts[2] == "vehicles") {
    const auto& driver_id = parts[1];
    validate_driver_token(
        driver_id,
        credential_value(request, "token", "x-mine-teleop-driver-token"),
        now);
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
          credential_value(request, "token", "x-mine-teleop-driver-token"),
          now);
    } else {
      validate_vehicle_connection(
          recipient,
          credential_value(request, "device_token", "x-mine-teleop-device-token"),
          required_uint64(Json{{"connection_generation", query_value(request, "connection_generation")}}, "connection_generation"),
          now);
    }
    return ServerResponse::json(
        200,
        {{"messages", take_signaling_messages(parts[1], recipient, now, query_value(request, "types"))}});
  }
  if (parts.size() == 3 && parts[0] == "vehicles" && parts[2] == "session") {
    const auto& vehicle_id = parts[1];
    validate_vehicle_connection(
        vehicle_id,
        credential_value(request, "device_token", "x-mine-teleop-device-token"),
        required_uint64(Json{{"connection_generation", query_value(request, "connection_generation")}}, "connection_generation"),
        now);
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
             {"control_token_expires_at_utc_ms", session.control_token_expires_at_utc_ms},
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
    validate_actor_credential(session, actor, credentials, now);
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "ice_servers") {
    const auto actor = query_value(request, "actor");
    const auto& session = require_participant(parts[1], actor);
    Json credentials = {
        {"token", credential_value(request, "token", "x-mine-teleop-driver-token")},
        {"device_token", credential_value(request, "device_token", "x-mine-teleop-device-token")},
        {"connection_generation", query_value(request, "connection_generation")}};
    validate_actor_credential(session, actor, credentials, now);
    Json servers = Json::array();
    if (!config_.stun_urls.empty()) servers.push_back({{"urls", config_.stun_urls}});
    std::int64_t expires_at_utc_ms = 0;
    if (!config_.turn_urls.empty()) {
      const auto expires_at_seconds = now.utc.value / 1000 + config_.turn_credential_ttl_seconds;
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

ServerResponse SignalingService::handle_driver_login(Json value, ClockSample admitted_at) {
  const auto driver_id = required_string(value, "driver_id");
  CleansedString password(optional_string(value, "password"));
  if (const auto field = value.find("password"); field != value.end() && field->is_string()) {
    cleanse_secret(field->get_ref<std::string&>());
    value.erase(field);
  }

  LoginCredentialSnapshot credential;
  std::optional<LoginFailureReservation> reservation;
  {
    std::lock_guard lock(mutex_);
    cleanup_expired_connections(admitted_at);
    if (const auto found = config_.driver_password_verifiers.find(driver_id);
        found != config_.driver_password_verifiers.end()) {
      credential.kind = LoginCredentialSnapshot::Kind::Argon2id;
      credential.verifier = found->second;
    } else if (config_.allow_legacy_passwords) {
      if (const auto found = config_.driver_passwords.find(driver_id);
          found != config_.driver_passwords.end()) {
        credential.kind = LoginCredentialSnapshot::Kind::LegacyPlaintext;
        credential.verifier = found->second;
      }
    }
    reservation.emplace(reserve_login_failure_locked(driver_id, admitted_at));
  }

  const auto release_reservation = [&] {
    if (!reservation) return;
    std::lock_guard lock(mutex_);
    release_login_failure_reservation_locked(*reservation);
    reservation.reset();
  };

  bool verification_slot_acquired = false;
  try {
    verification_slot_acquired = try_acquire_password_verification_slot();
  } catch (...) {
    release_reservation();
    throw;
  }
  if (!verification_slot_acquired) {
    release_reservation();
    throw TooManyRequests(
        "password verification capacity is temporarily exhausted",
        config_.password_verification_retry_after_ms);
  }

  bool verified = false;
  try {
    switch (credential.kind) {
      case LoginCredentialSnapshot::Kind::Argon2id:
        verified = verify_argon2id_password(
            credential.verifier,
            password.view(),
            default_argon2id_policy());
        break;
      case LoginCredentialSnapshot::Kind::LegacyPlaintext:
        verified = constant_time_equal(credential.verifier, password.view());
        break;
      case LoginCredentialSnapshot::Kind::Unknown:
        static_cast<void>(verify_argon2id_password(
            dummy_argon2id_verifier(),
            password.view(),
            default_argon2id_policy()));
        break;
    }
  } catch (...) {
    release_password_verification_slot();
    release_reservation();
    throw;
  }
  release_password_verification_slot();

  const auto settled_at = clock_sample();
  std::lock_guard lock(mutex_);
  cleanup_expired_connections(settled_at);
  if (!verified || credential.kind == LoginCredentialSnapshot::Kind::Unknown) {
    const auto failed_reservation = *reservation;
    reservation.reset();
    record_login_failure_locked(driver_id, failed_reservation, settled_at);
    throw Unauthorized("invalid driver credentials");
  }
  release_login_failure_reservation_locked(*reservation);
  reservation.reset();
  clear_login_failures_locked(driver_id);
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
  driver_tokens_[token] = DriverToken{
      driver_id,
      detail::saturating_deadline_ms(settled_at.utc.value, config_.token_ttl_ms),
      detail::saturating_deadline_ms(settled_at.monotonic.value, config_.token_ttl_ms),
      generation};
  online_drivers_[driver_id] = ConnectionPresence{
      "",
      generation,
      settled_at.utc.value,
      settled_at.utc.value,
      settled_at.monotonic.value};
  audit("driver_login", {{"driver_id", driver_id}, {"connection_generation", generation}});
  return ServerResponse::json(
      200,
      {{"token_type", "bearer"},
       {"token", token},
       {"expires_at_ms", driver_tokens_.at(token).expires_at_utc_ms},
       {"connection_generation", generation},
       {"service_instance_id", service_instance_id_}});
}

ServerResponse SignalingService::handle_post(const HttpRequest& request, ClockSample now) {
  auto value = request.json_body();
  if (request.path == "/auth/driver_login") return handle_driver_login(std::move(value), now);
  const auto parts = path_parts(request.path);
  std::lock_guard lock(mutex_);
  cleanup_expired_connections(now);
  if (request.path.starts_with("/admin/")) {
    if (config_.admin_token.empty()) throw Unauthorized("admin API is disabled");
    if (!constant_time_equal(config_.admin_token, optional_string(value, "admin_token"))) {
      throw Unauthorized("invalid admin token");
    }
    const auto object_id = required_string(value, "id");
    if (request.path == "/admin/revoke/driver") {
      if (!configured_driver(object_id)) throw NotFound("unknown driver");
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
      if (!configured_driver(object_id)) throw NotFound("unknown driver");
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
  if (request.path == "/auth/driver_heartbeat") {
    const auto driver_id = required_string(value, "driver_id");
    validate_driver_token(driver_id, optional_string(value, "token"), now);
    const auto& presence = online_drivers_.at(driver_id);
    return ServerResponse::json(
        200,
        {{"driver_id", driver_id},
         {"state", "online"},
         {"connection_generation", presence.generation},
         {"last_seen_at_utc_ms", presence.last_seen_at_utc_ms}});
  }
  if (request.path == "/auth/driver_logout") {
    const auto driver_id = required_string(value, "driver_id");
    const auto token = optional_string(value, "token");
    validate_driver_token(driver_id, token, now);
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
    const auto current = online_vehicles_.find(vehicle_id);
    if (current != online_vehicles_.end() && current->second.connection_id == connection_id) {
      current->second.last_seen_at_utc_ms = now.utc.value;
      current->second.last_seen_at_monotonic_ms = now.monotonic.value;
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
    online_vehicles_[vehicle_id] = ConnectionPresence{
        connection_id,
        generation,
        now.utc.value,
        now.utc.value,
        now.monotonic.value};
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
    validate_vehicle_connection(vehicle_id, optional_string(value, "device_token"), generation, now);
    return ServerResponse::json(
        200,
        {{"vehicle_id", vehicle_id},
         {"state", "online"},
         {"connection_generation", generation},
         {"last_seen_at_utc_ms", online_vehicles_.at(vehicle_id).last_seen_at_utc_ms}});
  }
  if (request.path == "/vehicles/offline") {
    const auto vehicle_id = required_string(value, "vehicle_id");
    const auto generation = required_uint64(value, "connection_generation");
    validate_vehicle_connection(vehicle_id, optional_string(value, "device_token"), generation, now);
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
    validate_driver_token(driver_id, optional_string(value, "token"), now);
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
    if (!audit(
            "control_authority_grant_preflight",
            {{"vehicle_id", vehicle_id}, {"driver_id", driver_id}})) {
      throw ServiceUnavailable("audit log unavailable; new control authority is disabled");
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
        .control_token_expires_at_utc_ms = detail::saturating_deadline_ms(
            now.utc.value,
            config_.control_token_ttl_ms),
        .control_token_expires_at_monotonic_ms = detail::saturating_deadline_ms(
            now.monotonic.value,
            config_.control_token_ttl_ms),
        .last_relay_usage_by_actor = {},
        .websocket_rate_by_participant = {}};
    sessions_[session.session_id] = session;
    auto& stored = sessions_.at(session.session_id);
    audit(
        "session_created",
        {{"session_id", stored.session_id}, {"vehicle_id", stored.vehicle_id}, {"driver_id", stored.driver_id}});
    transition_session(stored, SessionState::Reserved, "control_requested");
    transition_session(stored, SessionState::Connecting, "participants_authenticated");
    transition_session(stored, SessionState::Active, "control_authority_granted");
    if (!audit(
            "control_authority_granted",
            {{"session_id", stored.session_id},
             {"vehicle_id", stored.vehicle_id},
             {"driver_id", stored.driver_id}})) {
      close_session(stored, "audit_log_unavailable");
      throw ServiceUnavailable("audit log unavailable; control authority was not granted");
    }
    return ServerResponse::json(200, stored.to_json(true));
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "renew") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    if (actor != session.driver_id) throw Unauthorized("only the current driver can renew control authority");
    validate_actor_credential(session, actor, value, now);
    const auto previous_expiry_utc_ms = session.control_token_expires_at_utc_ms;
    session.control_token_expires_at_utc_ms = detail::saturating_deadline_ms(
        now.utc.value,
        config_.control_token_ttl_ms);
    session.control_token_expires_at_monotonic_ms = detail::saturating_deadline_ms(
        now.monotonic.value,
        config_.control_token_ttl_ms);
    audit(
        "control_authority_renewed",
        {{"session_id", session.session_id},
         {"vehicle_id", session.vehicle_id},
         {"driver_id", session.driver_id},
         {"previous_expires_at_utc_ms", previous_expiry_utc_ms},
         {"expires_at_utc_ms", session.control_token_expires_at_utc_ms}});
    return ServerResponse::json(200, session.to_json(true));
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "end") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    validate_actor_credential(session, actor, value, now);
    close_session(session, optional_string(value, "reason").empty() ? "session_end" : optional_string(value, "reason"));
    audit("session_ended", session.to_json());
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 4 && parts[0] == "sessions" && parts[2] == "control_authority" && parts[3] == "revoke") {
    const auto actor = required_string(value, "actor");
    auto& session = const_cast<Session&>(require_participant(parts[1], actor));
    validate_actor_credential(session, actor, value, now);
    close_session(
        session,
        optional_string(value, "reason").empty() ? "control_authority_revoked" : optional_string(value, "reason"));
    audit("control_authority_revoked", {{"session_id", session.session_id}, {"reason", optional_string(value, "reason")}});
    return ServerResponse::json(200, session.to_json());
  }
  if (parts.size() == 3 && parts[0] == "sessions" && parts[2] == "webrtc_connection") {
    const auto actor = required_string(value, "actor");
    const auto& session = require_participant(parts[1], actor);
    validate_actor_credential(session, actor, value, now);
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
    validate_actor_credential(session, actor, value, now);
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
    validate_actor_credential(session, actor, value, now);
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
    validate_actor_credential(session, actor, value, now);
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
    validate_actor_credential(session, actor, value, now);
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
    validate_actor_credential(session, actor, value, now);
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
    validate_actor_credential(session, actor, value, now);
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
    return ServerResponse::json(200, enqueue_signaling_message(parts[1], value, now));
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
  if (logging && logging["control_trace_commands"]) {
    config.control_trace_commands = logging["control_trace_commands"].as<bool>();
  }
  if (root["control"] && root["control"].IsMap() && root["control"]["keyboard"].IsDefined()) {
    throw std::invalid_argument(
        "control.keyboard is no longer supported; keyboard bindings are fixed: "
        "Arrow/WASD directions, Space service brake, B hard brake, E ESTOP");
  }
  if (root["control"] && root["control"]["rate_hz"]) config.rate_hz = root["control"]["rate_hz"].as<int>();
  if (root["control"] && root["control"]["intent_lease_ms"]) {
    config.intent_lease_ms = root["control"]["intent_lease_ms"].as<int>();
  }
  if (root["control"] && root["control"]["estop_hold_ms"]) config.estop_hold_ms = root["control"]["estop_hold_ms"].as<int>();
  const auto limits = root["control"] ? root["control"]["limits"] : YAML::Node{};
  if (limits) {
    if (limits["initial_max_throttle"]) {
      throw std::invalid_argument(
          "control.limits.initial_max_throttle is no longer accepted because a ratio cannot be "
          "safely converted without the selected vehicle hard limit; migrate to "
          "initial_target_speed_kph");
    }
    if (limits["initial_target_speed_kph"]) {
      config.control_limits.initial_target_speed_kph =
          limits["initial_target_speed_kph"].as<double>();
    }
    if (limits["initial_max_motor_torque_nm"]) {
      config.control_limits.initial_max_motor_torque_nm =
          limits["initial_max_motor_torque_nm"].as<double>();
    }
    if (limits["initial_max_brake_request"] || limits["initial_service_brake"] ||
        limits["initial_hard_brake"] || limits["initial_max_brake"]) {
      throw std::invalid_argument(
          "normalized control brake configuration is no longer accepted; migrate to "
          "initial_max_brake_pressure_bar, initial_service_brake_pressure_bar, and "
          "initial_hard_brake_pressure_bar");
    }
    if (limits["initial_max_brake_pressure_bar"]) {
      config.control_limits.initial_max_brake_pressure_bar =
          limits["initial_max_brake_pressure_bar"].as<double>();
    }
    if (limits["initial_service_brake_pressure_bar"]) {
      config.control_limits.initial_service_brake_pressure_bar =
          limits["initial_service_brake_pressure_bar"].as<double>();
    }
    if (limits["initial_hard_brake_pressure_bar"]) {
      config.control_limits.initial_hard_brake_pressure_bar =
          limits["initial_hard_brake_pressure_bar"].as<double>();
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
  if (config.driver_id.empty() || config.signaling_url.empty() || config.rate_hz <= 0 ||
      config.intent_lease_ms < 100 || config.intent_lease_ms > 1000 || config.estop_hold_ms < 0 ||
      config.browser_event_log_max_bytes < 1024 || config.browser_event_log_files < 1 ||
      config.browser_event_log_files > 20 ||
      config.max_time_sync_uncertainty_ms < 0 || config.time_sync_interval_ms <= 0 ||
      config.time_sync_samples < 3 || config.time_sync_samples > 15 || config.gamepad.steering_axis < 0 ||
      config.gamepad.throttle_axis < 0 || config.gamepad.brake_axis < 0 || config.gamepad.estop_button < 0 ||
      config.gamepad.axis_deadzone < 0.0 || config.gamepad.axis_deadzone >= 0.5 ||
      config.gamepad.steering_center < -1.0 || config.gamepad.steering_center > 1.0 ||
      config.gamepad.throttle_rest < -1.0 || config.gamepad.throttle_rest > 1.0 ||
      config.gamepad.brake_rest < -1.0 || config.gamepad.brake_rest > 1.0 ||
      !std::isfinite(config.control_limits.initial_target_speed_kph) ||
      config.control_limits.initial_target_speed_kph < 0.0 ||
      config.control_limits.initial_target_speed_kph > 72.0 ||
      !std::isfinite(config.control_limits.initial_max_motor_torque_nm) ||
      config.control_limits.initial_max_motor_torque_nm < 0.0 ||
      config.control_limits.initial_max_motor_torque_nm > 640.0 ||
      !std::isfinite(config.control_limits.initial_max_brake_pressure_bar) ||
      config.control_limits.initial_max_brake_pressure_bar < 0.0 ||
      config.control_limits.initial_max_brake_pressure_bar > 327.6 ||
      !std::isfinite(config.control_limits.initial_service_brake_pressure_bar) ||
      config.control_limits.initial_service_brake_pressure_bar < 0.0 ||
      !std::isfinite(config.control_limits.initial_hard_brake_pressure_bar) ||
      config.control_limits.initial_hard_brake_pressure_bar <
          config.control_limits.initial_service_brake_pressure_bar ||
      config.control_limits.initial_hard_brake_pressure_bar >
          config.control_limits.initial_max_brake_pressure_bar ||
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
      http_(std::chrono::seconds(5), config_.resolve_entries, config_.ca_bundle),
      native_control_intent_(config_.intent_lease_ms) {
  if (vehicle_id_.empty() || password_.empty()) throw std::invalid_argument("vehicle id and driver password are required");
  reset_control_profile_locked();
  if (!signaling_url_is_secure_or_loopback(config_.signaling_url)) {
    throw std::invalid_argument("public signaling URL must use HTTPS or WSS; HTTP/WS is allowed only on loopback");
  }
  if (config_.control_trace_commands && !config_.browser_event_log_path.empty()) {
    native_control_trace_ = std::make_unique<AsyncControlTrace>(
        [this](Json details) {
          std::string trace_session_id;
          const auto& commands = details["commands"];
          if (commands.is_array() && !commands.empty()) {
            trace_session_id = commands.front().value("trace_session_id", "");
          }
          const auto timestamp_ms = now_ms();
          append_driver_log_record({
              {"event", "driver_native_control_trace_batch"},
              {"sent_at_utc_ms", timestamp_ms},
              {"browser_sent_at_utc_ms", timestamp_ms},
              {"driver_id", config_.driver_id},
              {"vehicle_id", vehicle_id_},
              {"session_id", trace_session_id},
              {"details", std::move(details)},
          });
        });
  }
  native_control_sender_ = std::jthread(
      [this](std::stop_token stop_token) { native_control_sender_loop(stop_token); });
  native_control_lease_ = std::jthread(
      [this](std::stop_token stop_token) { native_control_lease_loop(stop_token); });
}

DriverConsoleRuntime::~DriverConsoleRuntime() {
  native_control_sender_.request_stop();
  native_control_lease_.request_stop();
  native_control_cv_.notify_all();
  if (native_control_sender_.joinable()) native_control_sender_.join();
  if (native_control_lease_.joinable()) native_control_lease_.join();
  close_control_signaling_websocket();
  close_signaling_websocket();
  if (native_control_trace_) native_control_trace_->stop();
}

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

bool DriverConsoleRuntime::connect_control_signaling_websocket(
    std::string_view session_id,
    std::string_view token,
    std::uint64_t expected_generation) {
  const auto url = signaling_websocket_url(
                       config_.signaling_url,
                       session_id,
                       config_.driver_id) +
      "&send_only=1";
  std::lock_guard lock(control_signaling_websocket_mutex_);
  if (expected_generation != 0) {
    {
      std::lock_guard state_lock(mutex_);
      if (session_id_ != session_id || driver_token_ != token ||
          control_session_generation_ != expected_generation) {
        return false;
      }
    }
    if (control_signaling_next_connect_monotonic_ms_ > monotonic_now_ms()) {
      return false;
    }
  }
  if (control_signaling_websocket_ && control_signaling_websocket_->connected() &&
      control_signaling_websocket_->url() == url &&
      control_signaling_websocket_session_id_ == session_id) {
    return true;
  }
  auto next = std::make_unique<WebSocketClient>(
      std::chrono::seconds(1), config_.resolve_entries, config_.ca_bundle);
  next->connect(url, {{"X-Mine-Teleop-Driver-Token", std::string(token)}});
  if (control_signaling_websocket_) control_signaling_websocket_->close();
  control_signaling_websocket_ = std::move(next);
  control_signaling_websocket_session_id_ = std::string(session_id);
  control_signaling_last_ack_seq_ = 0;
  control_signaling_ack_window_.reset();
  control_signaling_next_connect_monotonic_ms_ = 0;
  return true;
}

void DriverConsoleRuntime::close_control_signaling_websocket() {
  std::lock_guard lock(control_signaling_websocket_mutex_);
  if (control_signaling_websocket_) control_signaling_websocket_->close();
  control_signaling_websocket_.reset();
  control_signaling_websocket_session_id_.clear();
  control_signaling_last_ack_seq_ = 0;
  control_signaling_ack_window_.reset();
  control_signaling_next_connect_monotonic_ms_ = 0;
  control_signaling_reconnect_delay_ms_ = 100;
}

void DriverConsoleRuntime::reset_native_control_state() {
  std::lock_guard update_lock(native_control_update_mutex_);
  close_control_signaling_websocket();
  native_control_intent_.reset();
  native_control_last_sent_monotonic_ms_.store(0, std::memory_order_relaxed);
  native_control_last_ack_received_at_utc_ms_.store(0, std::memory_order_relaxed);
  native_control_last_ack_cloud_received_at_utc_ms_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    control_sequence_ = 0;
    ++control_session_generation_;
  }
  native_control_estop_wakeup_.store(false);
  native_control_cv_.notify_all();
}

void DriverConsoleRuntime::note_native_control_failure(
    std::string error,
    std::string_view session_id,
    std::uint64_t generation) {
  std::lock_guard update_lock(native_control_update_mutex_);
  {
    std::lock_guard lock(mutex_);
    if (session_id_ != session_id || control_session_generation_ != generation) {
      return;
    }
  }
  ++native_control_send_failures_;
  {
    std::lock_guard lock(native_control_status_mutex_);
    native_control_last_error_ = std::move(error);
  }
  native_control_intent_.invalidate();
  std::lock_guard websocket_lock(control_signaling_websocket_mutex_);
  if (control_signaling_websocket_) control_signaling_websocket_->close();
  control_signaling_websocket_.reset();
  control_signaling_websocket_session_id_.clear();
  control_signaling_last_ack_seq_ = 0;
  control_signaling_ack_window_.reset();
  const auto now_monotonic_ms = monotonic_now_ms();
  control_signaling_next_connect_monotonic_ms_ =
      now_monotonic_ms + control_signaling_reconnect_delay_ms_;
  control_signaling_reconnect_delay_ms_ =
      std::min(control_signaling_reconnect_delay_ms_ * 2, 1000);
}

bool DriverConsoleRuntime::send_native_control_sample() {
  const auto sample_started_at = clock_.sample();
  const auto sample_started_monotonic_ms = sample_started_at.monotonic.value;
  const auto sample_started_at_utc_ms = sample_started_at.utc.value;
  const auto scheduled_at_monotonic_ms =
      native_control_scheduled_at_monotonic_ms_.load(std::memory_order_relaxed);
  NativeControlIntentSample sample;
  std::string session;
  std::string control_token;
  std::string driver_token;
  std::string vehicle;
  std::int64_t control_token_expires_at_monotonic_ms = 0;
  std::uint64_t sequence = 0;
  std::uint64_t generation = 0;
  Json trace_record;
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    sample = native_control_intent_.sample(monotonic_now_ms());
    if (!sample.active) return false;
    {
      std::lock_guard lock(mutex_);
      if (session_id_.empty() || driver_token_.empty() || control_token_.empty()) {
        return false;
      }
      session = session_id_;
      control_token = control_token_;
      driver_token = driver_token_;
      vehicle = vehicle_id_;
      control_token_expires_at_monotonic_ms = control_token_expires_at_monotonic_ms_;
      generation = control_session_generation_;
      sequence = ++control_sequence_;
    }
  }

  try {
    if (native_control_trace_) {
      trace_record = {
          {"stage", "send_started"},
          {"trace_session_id", session},
          {"vehicle_id", vehicle},
          {"driver_id", config_.driver_id},
          {"session_generation", generation},
          {"seq", sequence},
          {"intent_seq", sample.intent.intent_seq},
          {"intent_fresh", sample.fresh && !sample.requires_fresh_input},
          {"intent_requires_fresh_input", sample.requires_fresh_input},
          {"effective_gear", sample.intent.gear},
          {"scheduled_at_monotonic_ms", scheduled_at_monotonic_ms},
          {"sample_started_at_utc_ms", sample_started_at_utc_ms},
          {"sample_started_monotonic_ms", sample_started_monotonic_ms},
          {"sender_wakeup_lag_ms",
           scheduled_at_monotonic_ms > 0
               ? std::max<std::int64_t>(
                     0,
                     sample_started_monotonic_ms - scheduled_at_monotonic_ms)
               : 0},
      };
    }
    if (detail::monotonic_deadline_reached(
            clock_.sample().monotonic,
            control_token_expires_at_monotonic_ms)) {
      throw std::runtime_error("control authority lease expired");
    }

    ControlCommand command;
    command.vehicle_id = vehicle;
    command.driver_id = config_.driver_id;
    command.session_id = session;
    command.seq = sequence;
    command.sent_at_utc_ms = clock_.now_ms();
    command.gear = sample.intent.gear;
    command.steering = sample.intent.steering;
    command.throttle = sample.intent.throttle;
    command.brake = sample.intent.brake;
    command.estop = sample.intent.estop;
    command.control_token = control_token;
    command.validate();
    auto payload = command.to_json();
    payload["intent_seq"] = sample.intent.intent_seq;
    payload["intent_fresh"] = sample.fresh && !sample.requires_fresh_input;
    payload["intent_requires_fresh_input"] = sample.requires_fresh_input;
    auto envelope = ProtocolMetadata{
                        command.protocol_version,
                        command.vehicle_id,
                        command.driver_id,
                        command.session_id,
                        command.seq,
                        command.sent_at_utc_ms}
                        .to_json();
    envelope["sender"] = config_.driver_id;
    envelope["recipient"] = vehicle;
    envelope["type"] = "control_command";
    envelope["payload"] = payload;

    const auto connect_started_at_utc_ms = clock_.now_ms();
    const auto connect_started_monotonic_ms = monotonic_now_ms();
    if (!connect_control_signaling_websocket(
            session,
            driver_token,
            generation)) {
      if (native_control_trace_) {
        trace_record["stage"] = "send_skipped";
        trace_record["reason"] = "session_changed_or_reconnect_backoff";
        trace_record["command_sent_at_utc_ms"] = command.sent_at_utc_ms;
        trace_record["connect_started_at_utc_ms"] = connect_started_at_utc_ms;
        trace_record["connect_started_monotonic_ms"] = connect_started_monotonic_ms;
        trace_record["trace_completed_at_utc_ms"] = clock_.now_ms();
        trace_record["trace_completed_monotonic_ms"] = monotonic_now_ms();
        native_control_trace_->enqueue(std::move(trace_record));
      }
      return false;
    }
    const auto connect_completed_at_utc_ms = clock_.now_ms();
    const auto connect_completed_monotonic_ms = monotonic_now_ms();
    if (native_control_trace_) {
      trace_record["command_sent_at_utc_ms"] = command.sent_at_utc_ms;
      trace_record["connect_started_at_utc_ms"] = connect_started_at_utc_ms;
      trace_record["connect_started_monotonic_ms"] = connect_started_monotonic_ms;
      trace_record["connect_completed_at_utc_ms"] = connect_completed_at_utc_ms;
      trace_record["connect_completed_monotonic_ms"] = connect_completed_monotonic_ms;
      trace_record["connect_processing_ms"] = std::max<std::int64_t>(
          0,
          connect_completed_monotonic_ms - connect_started_monotonic_ms);
    }
    Json drained_acknowledgements = Json::array();
    std::int64_t acknowledgement_drain_started_monotonic_ms = 0;
    std::int64_t acknowledgement_drain_completed_monotonic_ms = 0;
    std::int64_t send_started_at_utc_ms = 0;
    std::int64_t send_started_monotonic_ms = 0;
    std::int64_t send_completed_at_utc_ms = 0;
    std::int64_t send_completed_monotonic_ms = 0;
    std::int64_t unacknowledged_age_ms = 0;
    std::uint64_t last_ack_seq = 0;
    {
      std::lock_guard websocket_lock(control_signaling_websocket_mutex_);
      {
        std::lock_guard lock(mutex_);
        if (session_id_ != session || control_token_ != control_token ||
            control_session_generation_ != generation) {
          return false;
        }
      }
      if (!control_signaling_websocket_ ||
          !control_signaling_websocket_->connected() ||
          control_signaling_websocket_session_id_ != session) {
        throw std::runtime_error(
            "native control signaling websocket is not connected");
      }
      // Drain acknowledgements from earlier packets without making the 20 Hz
      // schedule depend on a cloud round trip. Track the oldest packet that is
      // still pending, rather than accumulating time across an ACK pipeline
      // whose sequence is continuously advancing.
      acknowledgement_drain_started_monotonic_ms = monotonic_now_ms();
      for (int drained = 0; drained < 16; ++drained) {
        const auto received =
            control_signaling_websocket_->receive_json(std::chrono::milliseconds(0));
        if (received.status == WebSocketReceiveStatus::Timeout) break;
        if (received.status == WebSocketReceiveStatus::Closed) {
          throw std::runtime_error("native control signaling websocket closed");
        }
        if (received.message.contains("error")) {
          throw std::runtime_error(received.message.value(
              "error", "native control signaling message was rejected"));
        }
        if (received.message.value("event", "") != "signaling_ack" ||
            received.message.value("type", "") != "control_command") {
          throw std::runtime_error(
              "native control signaling acknowledgement is invalid");
        }
        const auto acknowledged_seq =
            received.message.value("seq", std::uint64_t{0});
        if (acknowledged_seq == 0 || acknowledged_seq > sequence) {
          throw std::runtime_error(
              "native control signaling acknowledgement sequence is invalid");
        }
        control_signaling_last_ack_seq_ =
            std::max(control_signaling_last_ack_seq_, acknowledged_seq);
        control_signaling_ack_window_.acknowledge_through(
            control_signaling_last_ack_seq_);
        const auto acknowledgement_received_at_utc_ms = clock_.now_ms();
        const auto acknowledgement_received_monotonic_ms = monotonic_now_ms();
        native_control_last_ack_received_at_utc_ms_.store(
            acknowledgement_received_at_utc_ms,
            std::memory_order_relaxed);
        const auto cloud_received_at_utc_ms = received.message.value(
            "cloud_received_at_utc_ms",
            std::int64_t{0});
        if (cloud_received_at_utc_ms > 0) {
          native_control_last_ack_cloud_received_at_utc_ms_.store(
              cloud_received_at_utc_ms,
              std::memory_order_relaxed);
        }
        if (native_control_trace_) {
          drained_acknowledgements.push_back({
              {"seq", acknowledged_seq},
              {"ack_received_at_utc_ms", acknowledgement_received_at_utc_ms},
              {"ack_received_monotonic_ms", acknowledgement_received_monotonic_ms},
              {"cloud_received_at_utc_ms", cloud_received_at_utc_ms},
              {"cloud_queued_at_utc_ms", received.message.value(
                                                "cloud_queued_at_utc_ms",
                                                std::int64_t{0})},
              {"cloud_ingress_processing_ms", received.message.value(
                                                   "cloud_ingress_processing_ms",
                                                   std::int64_t{0})},
              {"delivery_cursor", received.message.value(
                                      "delivery_cursor",
                                      std::uint64_t{0})},
          });
        }
        control_signaling_next_connect_monotonic_ms_ = 0;
        control_signaling_reconnect_delay_ms_ = 100;
      }
      acknowledgement_drain_completed_monotonic_ms = monotonic_now_ms();
      if (native_control_trace_) {
        trace_record["acknowledgement_drain_started_monotonic_ms"] =
            acknowledgement_drain_started_monotonic_ms;
        trace_record["acknowledgement_drain_completed_monotonic_ms"] =
            acknowledgement_drain_completed_monotonic_ms;
        trace_record["acknowledgement_drain_ms"] = std::max<std::int64_t>(
            0,
            acknowledgement_drain_completed_monotonic_ms -
                acknowledgement_drain_started_monotonic_ms);
        trace_record["acknowledgements"] = drained_acknowledgements;
      }
      const auto monotonic_ms = monotonic_now_ms();
      if (control_signaling_ack_window_.pending_count() > 0) {
        unacknowledged_age_ms =
            control_signaling_ack_window_.oldest_age_ms(monotonic_ms);
        if (native_control_trace_) {
          trace_record["last_ack_seq"] = control_signaling_last_ack_seq_;
          trace_record["oldest_unacknowledged_seq"] =
              control_signaling_ack_window_.oldest_sequence();
          trace_record["unacknowledged_count"] =
              control_signaling_ack_window_.pending_count();
          trace_record["unacknowledged_age_ms"] = unacknowledged_age_ms;
        }
        if (unacknowledged_age_ms >= 500) {
          throw std::runtime_error(
              "native control signaling acknowledgements stalled for 500ms");
        }
      }
      last_ack_seq = control_signaling_last_ack_seq_;
      send_started_at_utc_ms = clock_.now_ms();
      send_started_monotonic_ms = monotonic_now_ms();
      control_signaling_websocket_->send_json(
          envelope,
          std::chrono::milliseconds(20));
      send_completed_at_utc_ms = clock_.now_ms();
      send_completed_monotonic_ms = monotonic_now_ms();
      control_signaling_ack_window_.note_sent(
          sequence, send_completed_monotonic_ms);
    }

    const auto sent_at_ms = clock_.now_ms();
    const auto previous = native_control_last_sent_at_utc_ms_.exchange(sent_at_ms);
    const auto gap_ms =
        previous > 0 ? std::max<std::int64_t>(0, sent_at_ms - previous) : 0;
    const auto previous_monotonic_ms =
        native_control_last_sent_monotonic_ms_.exchange(send_completed_monotonic_ms);
    const auto monotonic_gap_ms = previous_monotonic_ms > 0
        ? std::max<std::int64_t>(
              0,
              send_completed_monotonic_ms - previous_monotonic_ms)
        : 0;
    native_control_last_gap_ms_.store(gap_ms);
    auto maximum = native_control_max_gap_ms_.load();
    while (gap_ms > maximum &&
           !native_control_max_gap_ms_.compare_exchange_weak(maximum, gap_ms)) {
    }
    ++native_control_commands_sent_;
    native_control_last_seq_.store(sequence);
    {
      std::lock_guard lock(native_control_status_mutex_);
      native_control_last_error_.clear();
    }
    if (native_control_trace_) {
      trace_record["stage"] = "send_completed";
      trace_record["acknowledgement_drain_started_monotonic_ms"] =
          acknowledgement_drain_started_monotonic_ms;
      trace_record["acknowledgement_drain_completed_monotonic_ms"] =
          acknowledgement_drain_completed_monotonic_ms;
      trace_record["acknowledgement_drain_ms"] = std::max<std::int64_t>(
          0,
          acknowledgement_drain_completed_monotonic_ms -
              acknowledgement_drain_started_monotonic_ms);
      trace_record["acknowledgements"] = std::move(drained_acknowledgements);
      trace_record["last_ack_seq"] = last_ack_seq;
      trace_record["unacknowledged_age_ms"] = unacknowledged_age_ms;
      trace_record["send_started_at_utc_ms"] = send_started_at_utc_ms;
      trace_record["send_started_monotonic_ms"] = send_started_monotonic_ms;
      trace_record["send_completed_at_utc_ms"] = send_completed_at_utc_ms;
      trace_record["send_completed_monotonic_ms"] = send_completed_monotonic_ms;
      trace_record["send_call_ms"] = std::max<std::int64_t>(
          0,
          send_completed_monotonic_ms - send_started_monotonic_ms);
      trace_record["successful_send_gap_ms"] = gap_ms;
      trace_record["successful_send_monotonic_gap_ms"] = monotonic_gap_ms;
      trace_record["sample_processing_ms"] = std::max<std::int64_t>(
          0,
          send_completed_monotonic_ms - sample_started_monotonic_ms);
      native_control_trace_->enqueue(std::move(trace_record));
    }
    return true;
  } catch (const std::exception& error) {
    if (native_control_trace_) {
      trace_record["stage"] = "send_failed";
      trace_record["error"] = error.what();
      trace_record["failure_at_utc_ms"] = clock_.now_ms();
      trace_record["failure_monotonic_ms"] = monotonic_now_ms();
      trace_record["sample_processing_ms"] = std::max<std::int64_t>(
          0,
          monotonic_now_ms() - sample_started_monotonic_ms);
      native_control_trace_->enqueue(std::move(trace_record));
    }
    note_native_control_failure(error.what(), session, generation);
    return false;
  }
}

void DriverConsoleRuntime::native_control_sender_loop(std::stop_token stop_token) {
  const auto period = std::chrono::milliseconds(
      std::max(1, static_cast<int>(std::llround(1000.0 / config_.rate_hz))));
  auto next_send = std::chrono::steady_clock::now();
  while (!stop_token.stop_requested()) {
    {
      std::unique_lock lock(native_control_wait_mutex_);
      native_control_cv_.wait_until(lock, next_send, [&] {
        return stop_token.stop_requested() ||
            native_control_estop_wakeup_.exchange(false);
      });
    }
    if (stop_token.stop_requested()) break;
    native_control_scheduled_at_monotonic_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            next_send.time_since_epoch())
            .count(),
        std::memory_order_relaxed);
    static_cast<void>(send_native_control_sample());
    // Never replay missed periods or emit a catch-up burst after a scheduler or
    // network stall. The next vehicle packet is always one fresh sample.
    next_send = std::chrono::steady_clock::now() + period;
  }
}

void DriverConsoleRuntime::native_control_lease_loop(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    {
      std::unique_lock lock(native_control_wait_mutex_);
      native_control_cv_.wait_for(lock, std::chrono::milliseconds(250));
    }
    if (stop_token.stop_requested()) break;
    bool renewal_due = false;
    std::string renewal_session;
    std::uint64_t renewal_generation = 0;
    {
      std::lock_guard lock(mutex_);
      const auto now = clock_.sample();
      renewal_due = !session_id_.empty() && !control_token_.empty() &&
          detail::monotonic_deadline_reached(
              now.monotonic,
              control_token_renew_at_monotonic_ms_);
      renewal_session = session_id_;
      renewal_generation = control_session_generation_;
    }
    if (!renewal_due) continue;
    try {
      static_cast<void>(renew_control_authority());
    } catch (const std::exception& error) {
      note_native_control_failure(
          std::string("control authority renewal failed: ") + error.what(),
          renewal_session,
          renewal_generation);
    }
  }
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
  std::int64_t renew_at_monotonic_ms = 0;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    session = session_id_;
    control_token = control_token_;
    renew_at_monotonic_ms = control_token_renew_at_monotonic_ms_;
  }
  if (token.empty() || session.empty() || control_token.empty()) {
    return {{"renewed", false}, {"reason", "not_connected"}};
  }
  const auto request_started_at = clock_.sample();
  if (!detail::monotonic_deadline_reached(
          request_started_at.monotonic,
          renew_at_monotonic_ms)) {
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
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      if (session_id_ == session) {
        session_id_.clear();
        control_token_.clear();
        control_token_expires_at_utc_ms_ = 0;
        control_token_expires_at_monotonic_ms_ = 0;
        control_token_renew_at_monotonic_ms_ = 0;
        sequence_ = 0;
        reset_control_profile_locked();
        connected_at_utc_ms_ = 0;
      }
    }
    throw;
  }
  if (response.value("session_id", "") != session || response.value("control_token", "") != control_token) {
    throw std::runtime_error("control authority renewal changed the active session or token");
  }
  const auto expires_at_utc_ms = required_int64(response, "control_token_expires_at_utc_ms");
  const auto received_at = clock_.sample();
  const auto expires_at_monotonic_ms = detail::local_monotonic_deadline_from_utc_expiry(
      UtcMillis{expires_at_utc_ms},
      received_at);
  if (detail::monotonic_deadline_reached(received_at.monotonic, expires_at_monotonic_ms.value)) {
    throw std::runtime_error("control authority renewal returned an expired lease");
  }
  {
    std::lock_guard lock(mutex_);
    if (session_id_ != session || control_token_ != control_token) {
      return {{"renewed", false}, {"reason", "session_changed"}};
    }
    control_token_expires_at_utc_ms_ = expires_at_utc_ms;
    control_token_expires_at_monotonic_ms_ = expires_at_monotonic_ms.value;
    control_token_renew_at_monotonic_ms_ = control_lease_renew_at(
        received_at.monotonic,
        expires_at_monotonic_ms);
  }
  return {
      {"renewed", true},
      {"session_id", session},
      {"control_token_expires_at_utc_ms", expires_at_utc_ms},
  };
}

Json DriverConsoleRuntime::login_locked(std::string_view password) {
  if (clock_.refresh_due(config_.time_sync_interval_ms)) static_cast<void>(refresh_time_sync());
  std::string current_token;
  std::int64_t current_expiry_utc_ms = 0;
  std::int64_t current_expiry_monotonic_ms = 0;
  {
    std::lock_guard lock(mutex_);
    current_token = driver_token_;
    current_expiry_utc_ms = driver_token_expires_at_utc_ms_;
    current_expiry_monotonic_ms = driver_token_expires_at_monotonic_ms_;
  }
  if (!current_token.empty() && !detail::monotonic_deadline_reached(
                                    clock_.sample().monotonic,
                                    current_expiry_monotonic_ms)) {
    try {
      auto result = fetch_authorized_vehicles(current_token, current_expiry_utc_ms);
      result["authenticated"] = true;
      return result;
    } catch (const std::exception&) {
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      driver_token_.clear();
      driver_token_expires_at_utc_ms_ = 0;
      driver_token_expires_at_monotonic_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_utc_ms_ = 0;
      control_token_expires_at_monotonic_ms_ = 0;
      control_token_renew_at_monotonic_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_utc_ms_ = 0;
    }
  }
  const auto credential = password.empty() ? password_ : std::string(password);
  if (credential.empty()) throw std::invalid_argument("driver password is required");
  const auto response = http_.post_json_response(
      signaling_http_url_ + "/auth/driver_login",
      {{"driver_id", config_.driver_id}, {"password", credential}});
  const auto expires_at_utc_ms = response.value("expires_at_ms", std::int64_t{0});
  const auto received_at = clock_.sample();
  const auto expires_at_monotonic_ms = detail::local_monotonic_deadline_from_utc_expiry(
      UtcMillis{expires_at_utc_ms},
      received_at);
  if (detail::monotonic_deadline_reached(received_at.monotonic, expires_at_monotonic_ms.value)) {
    throw std::runtime_error("driver login returned an expired token");
  }
  {
    std::lock_guard lock(mutex_);
    password_ = credential;
    driver_token_ = required_string(response, "token");
    driver_token_expires_at_utc_ms_ = expires_at_utc_ms;
    driver_token_expires_at_monotonic_ms_ = expires_at_monotonic_ms.value;
    signaling_service_instance_id_ = required_string(response, "service_instance_id");
    signaling_available_ = true;
  }
  auto result = fetch_authorized_vehicles(
      required_string(response, "token"),
      expires_at_utc_ms);
  result["authenticated"] = true;
  return result;
}

Json DriverConsoleRuntime::login(std::string_view password) {
  std::lock_guard authentication_lock(authentication_mutex_);
  return login_locked(password);
}

Json DriverConsoleRuntime::fetch_authorized_vehicles(
    std::string_view token,
    std::int64_t expires_at_utc_ms) {
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
      {"token_expires_at_utc_ms", expires_at_utc_ms},
      {"service_instance_id", service_instance_id},
      {"vehicles", listed},
  };
}

Json DriverConsoleRuntime::vehicles() {
  std::lock_guard authentication_lock(authentication_mutex_);
  std::string token;
  std::int64_t expires_at_utc_ms = 0;
  std::string service_instance_id;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    expires_at_utc_ms = driver_token_expires_at_utc_ms_;
    service_instance_id = signaling_service_instance_id_;
  }
  if (token.empty()) throw HttpStatusError(401, "driver login is required");
  try {
    return fetch_authorized_vehicles(token, expires_at_utc_ms);
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
    reset_native_control_state();
    {
      std::lock_guard lock(mutex_);
      driver_token_.clear();
      driver_token_expires_at_utc_ms_ = 0;
      driver_token_expires_at_monotonic_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_utc_ms_ = 0;
      control_token_expires_at_monotonic_ms_ = 0;
      control_token_renew_at_monotonic_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_utc_ms_ = 0;
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
        {"token_expires_at_utc_ms", expires_at_utc_ms},
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
        static_cast<void>(
            connect_control_signaling_websocket(current_session, current_token));
        std::lock_guard lock(mutex_);
        return {
            {"runtime", "cpp"},
            {"driver_id", config_.driver_id},
            {"vehicle_id", vehicle_id_},
            {"connected", true},
            {"session_id", session_id_},
            {"control_session_generation", control_session_generation_},
            {"connected_at_ms", connected_at_utc_ms_},
            {"time_sync", clock_.status().to_json()},
        };
      }
    } else {
      validate_target();
      target_validated = true;
      static_cast<void>(end_session("driver_vehicle_switch"));
    }
    if (current_vehicle == target) {
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_utc_ms_ = 0;
      control_token_expires_at_monotonic_ms_ = 0;
      control_token_renew_at_monotonic_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_utc_ms_ = 0;
    }
  }

  if (!target_validated) validate_target();
  reset_native_control_state();
  const auto session = http_.post_json_response(
      signaling_http_url_ + "/sessions",
      {{"driver_id", config_.driver_id}, {"vehicle_id", target}, {"token", current_token}});
  const auto session_id = required_string(session, "session_id");
  const auto control_token = required_string(session, "control_token");
  const auto connected_at = clock_.sample();
  const auto connected_at_utc_ms = connected_at.utc.value;
  const auto control_token_expires_at_utc_ms = required_int64(session, "control_token_expires_at_utc_ms");
  const auto control_token_expires_at_monotonic_ms =
      detail::local_monotonic_deadline_from_utc_expiry(
          UtcMillis{control_token_expires_at_utc_ms},
          connected_at);
  if (detail::monotonic_deadline_reached(
          connected_at.monotonic,
          control_token_expires_at_monotonic_ms.value)) {
    throw std::runtime_error("new control authority lease is already expired");
  }
  std::uint64_t control_session_generation = 0;
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    {
      std::lock_guard lock(mutex_);
      vehicle_id_ = target;
      session_id_ = session_id;
      control_token_ = control_token;
      control_token_expires_at_utc_ms_ = control_token_expires_at_utc_ms;
      control_token_expires_at_monotonic_ms_ = control_token_expires_at_monotonic_ms.value;
      control_token_renew_at_monotonic_ms_ = control_lease_renew_at(
          connected_at.monotonic,
          control_token_expires_at_monotonic_ms);
      sequence_ = 0;
      control_sequence_ = 0;
      control_session_generation = ++control_session_generation_;
      reset_control_profile_locked();
      connected_at_utc_ms_ = connected_at_utc_ms;
    }
    const auto bootstrap = native_control_intent_.update(
        NativeControlIntent{
            "native-session-bootstrap", 1, "N", 0.0, 0.0, 0.0, false},
        monotonic_now_ms());
    if (!bootstrap.accepted) {
      throw std::runtime_error("native control bootstrap intent was rejected");
    }
  }
  native_control_cv_.notify_all();
  try {
    connect_signaling_websocket(session_id, current_token);
    static_cast<void>(
        connect_control_signaling_websocket(session_id, current_token));
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      static_cast<void>(end_session("signaling_websocket_connect_failed"));
    } catch (const std::exception&) {
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      if (session_id_ == session_id) {
        session_id_.clear();
        control_token_.clear();
        control_token_expires_at_utc_ms_ = 0;
        control_token_expires_at_monotonic_ms_ = 0;
        control_token_renew_at_monotonic_ms_ = 0;
        sequence_ = 0;
        reset_control_profile_locked();
        connected_at_utc_ms_ = 0;
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
      {"control_session_generation", control_session_generation},
      {"connected_at_ms", connected_at_utc_ms},
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
    reset_native_control_state();
    std::lock_guard lock(mutex_);
    reset_control_profile_locked();
    return {{"driver_id", config_.driver_id}, {"connected", false}, {"session_id", ""}};
  }
  if (token.empty()) throw std::runtime_error("driver login is required to end the session");
  auto response = http_.post_json_response(
      signaling_http_url_ + "/sessions/" + http_.url_encode(session) + "/end",
      {{"actor", config_.driver_id}, {"token", token}, {"reason", std::string(reason)}});
  close_signaling_websocket();
  reset_native_control_state();
  {
    std::lock_guard lock(mutex_);
    if (session_id_ == session) {
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_utc_ms_ = 0;
      control_token_expires_at_monotonic_ms_ = 0;
      control_token_renew_at_monotonic_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_utc_ms_ = 0;
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
    reset_native_control_state();
    std::lock_guard lock(mutex_);
    session_id_.clear();
    control_token_.clear();
    control_token_expires_at_utc_ms_ = 0;
    control_token_expires_at_monotonic_ms_ = 0;
    control_token_renew_at_monotonic_ms_ = 0;
    sequence_ = 0;
    reset_control_profile_locked();
    connected_at_utc_ms_ = 0;
    authorized_vehicles_ = Json::array();
    return {{"driver_id", config_.driver_id}, {"state", "offline"}, {"session_id", session}};
  }
  auto response = http_.post_json_response(
      signaling_http_url_ + "/auth/driver_logout",
      {{"driver_id", config_.driver_id}, {"token", token}, {"reason", std::string(reason)}});
  close_signaling_websocket();
  reset_native_control_state();
  {
    std::lock_guard lock(mutex_);
    if (driver_token_ == token) {
      driver_token_.clear();
      driver_token_expires_at_utc_ms_ = 0;
      driver_token_expires_at_monotonic_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_utc_ms_ = 0;
      control_token_expires_at_monotonic_ms_ = 0;
      control_token_renew_at_monotonic_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_utc_ms_ = 0;
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
      reset_native_control_state();
      {
        std::lock_guard lock(mutex_);
        signaling_available_ = true;
        if (session_id_ == session) {
          session_id_.clear();
          control_token_.clear();
          control_token_expires_at_utc_ms_ = 0;
          control_token_expires_at_monotonic_ms_ = 0;
          control_token_renew_at_monotonic_ms_ = 0;
          sequence_ = 0;
          reset_control_profile_locked();
          connected_at_utc_ms_ = 0;
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
        reset_native_control_state();
        {
          std::lock_guard lock(mutex_);
          if (session_id_ == session) {
            session_id_.clear();
            control_token_.clear();
            control_token_expires_at_utc_ms_ = 0;
            control_token_expires_at_monotonic_ms_ = 0;
            control_token_renew_at_monotonic_ms_ = 0;
            sequence_ = 0;
            reset_control_profile_locked();
            connected_at_utc_ms_ = 0;
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

Json DriverConsoleRuntime::control_profile_locked() const {
  return {
      {"profile_version", kSessionControlProfileVersion},
      {"target_speed_kph", target_speed_kph_},
      {"max_motor_torque_nm", max_motor_torque_nm_},
      {"max_brake_pressure_bar", max_brake_pressure_bar_},
      {"service_brake_pressure_bar", service_brake_pressure_bar_},
      {"hard_brake_pressure_bar", hard_brake_pressure_bar_},
      {"max_steering_angle_deg", max_steering_angle_deg_},
      {"speed_pid_kp", speed_pid_kp_},
      {"speed_pid_ki", speed_pid_ki_},
      {"speed_pid_kd", speed_pid_kd_},
      {"speed_pid_derivative_filter_tau_ms", speed_pid_derivative_filter_tau_ms_},
      {"speed_pid_max_dt_ms", speed_pid_max_dt_ms_},
      {"motor_torque_rise_rate_nm_per_s", motor_torque_rise_rate_nm_per_s_},
      {"initialized", control_profile_initialized_},
      {"last_prepared_seq", last_control_profile_prepared_seq_},
  };
}

void DriverConsoleRuntime::reset_control_profile_locked() {
  target_speed_kph_ = config_.control_limits.initial_target_speed_kph;
  max_motor_torque_nm_ = config_.control_limits.initial_max_motor_torque_nm;
  max_brake_pressure_bar_ = config_.control_limits.initial_max_brake_pressure_bar;
  service_brake_pressure_bar_ = config_.control_limits.initial_service_brake_pressure_bar;
  hard_brake_pressure_bar_ = config_.control_limits.initial_hard_brake_pressure_bar;
  max_steering_angle_deg_ = config_.control_limits.initial_max_steering_angle_deg;
  speed_pid_kp_ = 0.0;
  speed_pid_ki_ = 0.0;
  speed_pid_kd_ = 0.0;
  speed_pid_derivative_filter_tau_ms_ = 0.0;
  speed_pid_max_dt_ms_ = 0;
  motor_torque_rise_rate_nm_per_s_ = 0.0;
  control_profile_initialized_ = false;
  service_brake_limit_.store(
      max_brake_pressure_bar_ > 0.0 ? service_brake_pressure_bar_ / max_brake_pressure_bar_ : 0.0);
  hard_brake_limit_.store(
      max_brake_pressure_bar_ > 0.0 ? hard_brake_pressure_bar_ / max_brake_pressure_bar_ : 0.0);
  last_control_profile_prepared_seq_ = 0;
}

Json DriverConsoleRuntime::control_profile() const {
  std::lock_guard lock(mutex_);
  return control_profile_locked();
}

Json DriverConsoleRuntime::prepare_control_profile(const Json& input) {
  if (!input.is_object()) {
    throw std::invalid_argument("control profile requires an object");
  }
  for (const auto* field : {
           "target_speed_kph",
           "max_motor_torque_nm",
           "max_brake_pressure_bar",
           "service_brake_pressure_bar",
           "hard_brake_pressure_bar",
           "max_steering_angle_deg",
           "speed_pid_kp",
           "speed_pid_ki",
           "speed_pid_kd",
           "speed_pid_derivative_filter_tau_ms",
           "speed_pid_max_dt_ms",
           "motor_torque_rise_rate_nm_per_s"}) {
    if (!input.contains(field) || !input.at(field).is_number()) {
      throw std::invalid_argument(std::string("control profile requires numeric ") + field);
    }
  }
  if (!input.contains("profile_version") || !input.at("profile_version").is_number_integer() ||
      input.at("profile_version").get<int>() != kSessionControlProfileVersion) {
    throw std::invalid_argument("control profile requires profile_version 3");
  }
  if (input.size() != 13) {
    throw std::invalid_argument("control profile must contain exactly the V3 fields");
  }
  const auto target_speed_kph = input.at("target_speed_kph").get<double>();
  const auto max_motor_torque_nm = input.at("max_motor_torque_nm").get<double>();
  const auto max_brake_pressure_bar = input.at("max_brake_pressure_bar").get<double>();
  const auto service_brake_pressure_bar = input.at("service_brake_pressure_bar").get<double>();
  const auto hard_brake_pressure_bar = input.at("hard_brake_pressure_bar").get<double>();
  const auto max_steering_angle_deg = input.at("max_steering_angle_deg").get<double>();
  const auto speed_pid_kp = input.at("speed_pid_kp").get<double>();
  const auto speed_pid_ki = input.at("speed_pid_ki").get<double>();
  const auto speed_pid_kd = input.at("speed_pid_kd").get<double>();
  const auto speed_pid_derivative_filter_tau_ms =
      input.at("speed_pid_derivative_filter_tau_ms").get<double>();
  if (!input.at("speed_pid_max_dt_ms").is_number_integer()) {
    throw std::invalid_argument("speed_pid_max_dt_ms must be an integer");
  }
  const auto speed_pid_max_dt_ms = input.at("speed_pid_max_dt_ms").get<int>();
  const auto motor_torque_rise_rate_nm_per_s =
      input.at("motor_torque_rise_rate_nm_per_s").get<double>();
  if (!std::isfinite(target_speed_kph) || target_speed_kph < 0.0 ||
      target_speed_kph > 72.0 || !std::isfinite(max_motor_torque_nm) ||
      max_motor_torque_nm < 0.0 || max_motor_torque_nm > 640.0 ||
      !std::isfinite(max_brake_pressure_bar) || max_brake_pressure_bar < 0.0 ||
      max_brake_pressure_bar > 327.6 || !std::isfinite(service_brake_pressure_bar) ||
      !std::isfinite(hard_brake_pressure_bar) || service_brake_pressure_bar < 0.0 ||
      service_brake_pressure_bar > hard_brake_pressure_bar ||
      hard_brake_pressure_bar > max_brake_pressure_bar ||
      !std::isfinite(max_steering_angle_deg) || max_steering_angle_deg < 0.0 ||
      max_steering_angle_deg > 30.0 || !std::isfinite(speed_pid_kp) ||
      speed_pid_kp <= 0.0 || speed_pid_kp > 100.0 ||
      !std::isfinite(speed_pid_ki) || speed_pid_ki < 0.0 || speed_pid_ki > 100.0 ||
      !std::isfinite(speed_pid_kd) || speed_pid_kd < 0.0 || speed_pid_kd > 100.0 ||
      !std::isfinite(speed_pid_derivative_filter_tau_ms) ||
      speed_pid_derivative_filter_tau_ms < 0.0 ||
      speed_pid_derivative_filter_tau_ms > 2000.0 ||
      speed_pid_max_dt_ms < 20 || speed_pid_max_dt_ms > 200 ||
      !std::isfinite(motor_torque_rise_rate_nm_per_s) ||
      motor_torque_rise_rate_nm_per_s < 0.0 ||
      motor_torque_rise_rate_nm_per_s > 32000.0) {
    throw std::invalid_argument(
        "control profile V3 values are outside the controller schema bounds");
  }

  std::lock_guard lock(mutex_);
  if (driver_token_.empty() || session_id_.empty() || control_token_.empty()) {
    throw std::runtime_error("driver console is not connected");
  }
  target_speed_kph_ = target_speed_kph;
  max_motor_torque_nm_ = max_motor_torque_nm;
  max_brake_pressure_bar_ = max_brake_pressure_bar;
  service_brake_pressure_bar_ = service_brake_pressure_bar;
  hard_brake_pressure_bar_ = hard_brake_pressure_bar;
  max_steering_angle_deg_ = max_steering_angle_deg;
  speed_pid_kp_ = speed_pid_kp;
  speed_pid_ki_ = speed_pid_ki;
  speed_pid_kd_ = speed_pid_kd;
  speed_pid_derivative_filter_tau_ms_ = speed_pid_derivative_filter_tau_ms;
  speed_pid_max_dt_ms_ = speed_pid_max_dt_ms;
  motor_torque_rise_rate_nm_per_s_ = motor_torque_rise_rate_nm_per_s;
  control_profile_initialized_ = true;
  service_brake_limit_.store(
      max_brake_pressure_bar > 0.0 ? service_brake_pressure_bar / max_brake_pressure_bar : 0.0);
  hard_brake_limit_.store(
      max_brake_pressure_bar > 0.0 ? hard_brake_pressure_bar / max_brake_pressure_bar : 0.0);
  const auto sequence = ++sequence_;
  last_control_profile_prepared_seq_ = sequence;
  auto request = ProtocolMetadata{
      kProtocolVersion,
      vehicle_id_,
      config_.driver_id,
      session_id_,
      sequence,
      clock_.now_ms()}.to_json();
  request["type"] = "session_control_profile";
  request["control_token"] = control_token_;
  request["profile_version"] = kSessionControlProfileVersion;
  request["target_speed_kph"] = target_speed_kph;
  request["max_motor_torque_nm"] = max_motor_torque_nm;
  request["max_brake_pressure_bar"] = max_brake_pressure_bar;
  request["service_brake_pressure_bar"] = service_brake_pressure_bar;
  request["hard_brake_pressure_bar"] = hard_brake_pressure_bar;
  request["max_steering_angle_deg"] = max_steering_angle_deg;
  request["speed_pid_kp"] = speed_pid_kp;
  request["speed_pid_ki"] = speed_pid_ki;
  request["speed_pid_kd"] = speed_pid_kd;
  request["speed_pid_derivative_filter_tau_ms"] =
      speed_pid_derivative_filter_tau_ms;
  request["speed_pid_max_dt_ms"] = speed_pid_max_dt_ms;
  request["motor_torque_rise_rate_nm_per_s"] = motor_torque_rise_rate_nm_per_s;
  return {
      {"prepared", true},
      {"transport", "webrtc_data_channel"},
      {"delivery_state", "browser_data_channel_pending"},
      {"request", std::move(request)},
      {"control_profile", control_profile_locked()},
  };
}

Json DriverConsoleRuntime::set_control_limits(const Json&) {
  throw std::runtime_error(
      "legacy POST /api/control-limits is retired; use authenticated /api/control-profile");
}

Json DriverConsoleRuntime::send_control(const Json& input) {
  static_cast<void>(input);
  throw std::runtime_error(
      "legacy browser control packet preparation is retired; use /api/control-intent");
}

Json DriverConsoleRuntime::update_control_intent(const Json& input) {
  if (!input.is_object()) throw std::invalid_argument("control intent must be an object");
  const auto expected_session_id = required_string(input, "session_id");
  const auto expected_session_generation = required_uint64(input, "session_generation");
  NativeControlIntent intent;
  intent.ui_instance_id = required_string(input, "ui_instance_id");
  intent.intent_seq = required_uint64(input, "intent_seq");
  intent.gear = required_string(input, "gear");
  try {
    intent.steering = input.at("steering").get<double>();
    intent.throttle = input.at("throttle").get<double>();
    intent.brake = input.at("brake").get<double>();
    intent.estop = input.value("estop", false);
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid control intent: ") + error.what());
  }
  intent.validate();
  NativeControlIntentUpdate result;
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    {
      std::lock_guard lock(mutex_);
      if (session_id_.empty() || driver_token_.empty() || control_token_.empty()) {
        throw std::runtime_error("driver console is not connected");
      }
      if (session_id_ != expected_session_id ||
          control_session_generation_ != expected_session_generation) {
        throw std::runtime_error(
            "control intent belongs to a stale or different native control session");
      }
    }
    result = native_control_intent_.update(intent, monotonic_now_ms());
  }
  if (result.accepted && intent.estop) {
    native_control_estop_wakeup_.store(true);
    native_control_cv_.notify_all();
  }
  return {
      {"accepted", result.accepted},
      {"duplicate", result.duplicate},
      {"requires_fresh_input", result.requires_fresh_input},
      {"reason", result.reason},
      {"intent_seq", intent.intent_seq},
      {"session_id", expected_session_id},
      {"session_generation", expected_session_generation},
      {"lease_ms", native_control_intent_.lease_ms()},
      {"transport", "native_signaling_websocket"},
      {"delivery_state", "native_periodic_sender_owned"},
  };
}

Json DriverConsoleRuntime::status() {
  bool authenticated = false;
  bool control_lease_due = false;
  const auto timestamp = clock_.sample();
  {
    std::lock_guard lock(mutex_);
    authenticated = !driver_token_.empty();
    control_lease_due = !session_id_.empty() && detail::monotonic_deadline_reached(
        timestamp.monotonic,
        control_token_renew_at_monotonic_ms_);
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
  bool native_control_websocket_connected = false;
  std::uint64_t native_control_last_ack_seq = 0;
  std::int64_t native_control_next_connect_monotonic_ms = 0;
  std::int64_t native_control_unacknowledged_age_ms = 0;
  int native_control_reconnect_delay_ms = 0;
  {
    std::lock_guard websocket_lock(control_signaling_websocket_mutex_);
    native_control_websocket_connected =
        control_signaling_websocket_ && control_signaling_websocket_->connected();
    native_control_last_ack_seq = control_signaling_last_ack_seq_;
    native_control_next_connect_monotonic_ms =
        control_signaling_next_connect_monotonic_ms_;
    native_control_unacknowledged_age_ms =
        control_signaling_ack_window_.oldest_age_ms(monotonic_now_ms());
    native_control_reconnect_delay_ms = control_signaling_reconnect_delay_ms_;
  }
  const auto native_sample = native_control_intent_.sample(monotonic_now_ms());
  std::string native_control_last_error;
  {
    std::lock_guard status_lock(native_control_status_mutex_);
    native_control_last_error = native_control_last_error_;
  }
  std::lock_guard lock(mutex_);
  return {
      {"runtime", "cpp"},
      {"driver_id", config_.driver_id},
      {"vehicle_id", vehicle_id_},
      {"authenticated", !driver_token_.empty()},
      {"driver_token_expires_at_utc_ms", driver_token_expires_at_utc_ms_},
      {"signaling_service_instance_id", signaling_service_instance_id_},
      {"signaling_restart_recoveries", signaling_restart_recoveries_},
      {"signaling_available", signaling_available_},
      {"connected", !session_id_.empty()},
      {"session_id", session_id_},
      {"control_session_generation", control_session_generation_},
      {"control_token_expires_at_utc_ms", control_token_expires_at_utc_ms_},
      {"sequence", sequence_},
      {"connected_at_ms", connected_at_utc_ms_},
      {"last_control_prepared_at_utc_ms", last_control_prepared_at_utc_ms_},
      {"control_commands_prepared_total", control_commands_prepared_total_},
      {"signaling_transport", "websocket"},
      {"signaling_websocket_connected", websocket_connected},
      {"signaling_websocket_reconnects", websocket_reconnects},
      {"signaling_delivery_cursor", delivery_cursor},
      {"pending_signaling_deliveries", pending_deliveries},
      {"native_control",
       {{"transport", "native_signaling_websocket"},
        {"session_generation", control_session_generation_},
        {"rate_hz", config_.rate_hz},
        {"intent_lease_ms", native_control_intent_.lease_ms()},
        {"intent_active", native_sample.active},
        {"intent_fresh", native_sample.fresh},
        {"requires_fresh_input", native_sample.requires_fresh_input},
        {"effective_intent_seq", native_sample.intent.intent_seq},
        {"effective_gear", native_sample.intent.gear},
        {"effective_steering", native_sample.intent.steering},
        {"effective_throttle", native_sample.intent.throttle},
        {"effective_brake", native_sample.intent.brake},
        {"effective_estop", native_sample.intent.estop},
        {"websocket_connected", native_control_websocket_connected},
        {"next_connect_in_ms",
         std::max<std::int64_t>(
             0,
             native_control_next_connect_monotonic_ms - monotonic_now_ms())},
        {"reconnect_delay_ms", native_control_reconnect_delay_ms},
        {"commands_sent_total", native_control_commands_sent_.load()},
        {"send_failures_total", native_control_send_failures_.load()},
        {"last_sent_at_utc_ms", native_control_last_sent_at_utc_ms_.load()},
        {"last_sent_monotonic_ms", native_control_last_sent_monotonic_ms_.load()},
        {"last_gap_ms", native_control_last_gap_ms_.load()},
        {"max_gap_ms", native_control_max_gap_ms_.load()},
        {"last_seq", native_control_last_seq_.load()},
        {"last_ack_seq", native_control_last_ack_seq},
        {"unacknowledged_age_ms", native_control_unacknowledged_age_ms},
        {"last_ack_received_at_utc_ms",
         native_control_last_ack_received_at_utc_ms_.load()},
        {"last_ack_cloud_received_at_utc_ms",
         native_control_last_ack_cloud_received_at_utc_ms_.load()},
        {"last_error", native_control_last_error}}},
      {"time_sync", clock_.status().to_json()},
      {"webrtc_metrics", webrtc_metrics_},
      {"last_signaling_messages", signaling_messages_},
      {"authorized_vehicles", authorized_vehicles_},
  };
}

void DriverConsoleRuntime::append_driver_log_record(const Json& record) const {
  if (config_.browser_event_log_path.empty()) return;
  const auto line = record.dump() + "\n";
  if (line.size() > config_.browser_event_log_max_bytes) {
    throw std::invalid_argument("driver event exceeds configured log size");
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
  append_driver_log_record(record);
  return {{"recorded", true}, {"event", event}};
}

DriverConsoleHttpApp::DriverConsoleHttpApp(std::shared_ptr<DriverConsoleRuntime> runtime)
    : runtime_(std::move(runtime)), page_capability_(random_token(32)) {
  if (!runtime_) throw std::invalid_argument("driver console runtime is required");
}

ServerResponse DriverConsoleHttpApp::handle(const HttpRequest& request) const {
  ServerResponse response;
  try {
    if (request.method == "POST") {
      if (!application_json_content_type(request)) {
        response = ServerResponse::json(415, {{"error", "local mutation requests require application/json"}});
      } else if (!trusted_local_mutation_request(request, page_capability_)) {
        response = ServerResponse::json(403, {{"error", "local mutation request origin or capability is invalid"}});
      }
    }
    if (response.status == 200) {
      if (request.method == "GET" && request.path == "/assets/control_logic.js") {
        response = ServerResponse::text(
            200,
            std::string(web::kControlLogicJavaScript),
            "application/javascript; charset=utf-8");
      } else if (request.method == "GET" && request.path == "/assets/control_console.js") {
        response = ServerResponse::text(
            200,
            std::string(web::kControlConsoleJavaScript),
            "application/javascript; charset=utf-8");
      } else if (request.method == "GET" && request.path == "/assets/control_console.css") {
        response = ServerResponse::text(
            200,
            std::string(web::kControlConsoleCss),
            "text/css; charset=utf-8");
      } else if (request.method == "GET" && request.path == "/assets/control_console.html") {
        response = ServerResponse::text(
            200,
            std::string(web::kControlConsoleHtml),
            "text/html; charset=utf-8");
      } else if (request.method == "GET" && request.path == "/health") {
        response = ServerResponse::json(200, {{"status", "ok"}, {"runtime", "cpp"}});
      } else if (request.method == "GET" && request.path == "/api/time") {
        response = ServerResponse::json(200, {{"now_ms", now_ms()}});
      } else if (request.method == "GET" && request.path == "/api/console-config") {
        response = ServerResponse::json(200, console_config_json(runtime_->config(), page_capability_));
      } else if (request.method == "GET" && request.path == "/api/status") {
        response = ServerResponse::json(200, runtime_->status());
      } else if (request.method == "GET" && request.path == "/api/vehicles") {
        response = ServerResponse::json(200, runtime_->vehicles());
      } else if (request.method == "GET" && request.path == "/api/control-limits") {
        response = ServerResponse::json(200, runtime_->control_limits());
      } else if (request.method == "GET" && request.path == "/api/control-profile") {
        response = ServerResponse::json(200, runtime_->control_profile());
      } else if (request.method == "GET" && request.path == "/") {
        response = ServerResponse::text(
            200,
            std::string(web::kControlConsoleHtml),
            "text/html; charset=utf-8");
      } else if (request.method == "POST" && request.path == "/api/login") {
        response = ServerResponse::json(200, runtime_->login(request.json_body().value("password", "")));
      } else if (request.method == "POST" && request.path == "/api/connect") {
        response = ServerResponse::json(200, runtime_->connect(request.json_body().value("vehicle_id", "")));
      } else if (request.method == "POST" && request.path == "/api/end-session") {
        response = ServerResponse::json(
            200,
            runtime_->end_session(request.json_body().value("reason", "driver_session_end")));
      } else if (request.method == "POST" && request.path == "/api/disconnect") {
        response = ServerResponse::json(
            200,
            runtime_->disconnect(request.json_body().value("reason", "driver_console_disconnect")));
      } else if (request.method == "POST" && request.path == "/api/poll-signaling") {
        response = ServerResponse::json(200, runtime_->poll_signaling());
      } else if (request.method == "POST" && request.path == "/api/webrtc/ice-servers") {
        response = ServerResponse::json(200, runtime_->ice_servers());
      } else if (request.method == "POST" && request.path == "/api/webrtc/capabilities") {
        response = ServerResponse::json(200, runtime_->send_media_capabilities(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/webrtc/fallback") {
        response = ServerResponse::json(200, runtime_->send_media_fallback(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/webrtc/answer") {
        response = ServerResponse::json(200, runtime_->send_webrtc_answer(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/webrtc/ice-candidate") {
        response = ServerResponse::json(200, runtime_->send_webrtc_ice_candidate(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/webrtc/metrics") {
        response = ServerResponse::json(200, runtime_->ingest_webrtc_metrics(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/browser-event") {
        response = ServerResponse::json(200, runtime_->record_browser_event(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/control-limits") {
        response = ServerResponse::json(
            410,
            {{"error", "legacy control-limit mutation is retired; use /api/control-profile"}});
      } else if (request.method == "POST" && request.path == "/api/control-profile") {
        response = ServerResponse::json(200, runtime_->prepare_control_profile(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/control-intent") {
        response = ServerResponse::json(200, runtime_->update_control_intent(request.json_body()));
      } else if (request.method == "POST" && request.path == "/api/control") {
        response = ServerResponse::json(
            410,
            {{"error", "legacy browser control packet endpoint is retired; use /api/control-intent"}});
      } else if (request.method == "POST" &&
                 (request.path == "/api/control/keyboard" ||
                  request.path == "/api/control/gamepad")) {
        response = ServerResponse::json(
            410,
            {{"error",
              "legacy specialized control endpoint is retired; use /api/control-intent"}});
      } else {
        response = ServerResponse::json(404, {{"error", "not found"}});
      }
    }
    add_console_security_headers(response);
  } catch (const HttpStatusError& error) {
    const auto status = error.status() >= 400 && error.status() <= 599
        ? static_cast<int>(error.status())
        : 502;
    response = ServerResponse::json(status, {{"error", error.what()}});
    add_console_security_headers(response);
  } catch (const std::invalid_argument& error) {
    response = ServerResponse::json(400, {{"error", error.what()}});
    add_console_security_headers(response);
  } catch (const std::exception& error) {
    response = ServerResponse::json(409, {{"error", error.what()}});
    add_console_security_headers(response);
  }
  return response;
}

}  // namespace mine_teleop
