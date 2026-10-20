#include "mine_teleop/server.hpp"
#include "mine_teleop/session_logs.hpp"
#include "mine_teleop/control_logic_js.hpp"
#include "mine_teleop/detail/console_assets.hpp"
#include "mine_teleop/detail/diagnostic_queue.hpp"
#include "mine_teleop/detail/native_control_dispatch.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <CommonCrypto/CommonHMAC.h>
#include <Security/Security.h>
#include <pthread/qos.h>
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
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace mine_teleop {

enum class ControlObservationPhase { Idle, Sample, Connect, AckDrain, Send, Failure,
  LoopMutex, LoopCheck, DeliverySend, Receive, IngressMutex, IngressCheck, IngressAck, DeliveryAck, TransportSample };

class ControlConnectionObservation {
 public:
  ControlConnectionObservation(SocketHandle socket = kInvalidSocket, std::string session = {},
      std::string actor = {}, std::string role = "driver_sender")
      : socket_(socket), session_(std::move(session)), actor_(std::move(actor)), role_(std::move(role)) {
    started_ = steady_ms();
    begin(ControlObservationPhase::Idle);
  }
  static std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
  }
  void begin(ControlObservationPhase phase, std::uint64_t seq = 0) {
    epoch_.fetch_add(1, std::memory_order_acq_rel);
    phase_started_.store(steady_ms());
    phase_.store(phase);
    if (seq) seq_.store(seq);
    epoch_.fetch_add(1, std::memory_order_release);
  }
  void detach() { std::lock_guard lock(socket_mutex_); socket_ = kInvalidSocket; detached_ = true; }
  [[nodiscard]] std::int64_t started() const { return started_; }
  Json sample(bool include_socket = true) {
    const auto epoch = epoch_.load(std::memory_order_acquire);
    static constexpr std::array names{"idle", "sample", "connect", "ack_drain", "send", "failure_handling",
        "loop_mutex_wait", "loop_authority_check", "vehicle_delivery_send", "receive_frame",
        "ingress_mutex_wait", "ingress_authority_check", "ingress_ack_send", "vehicle_ack_send", "transport_sample"};
    const auto phase = phase_.load();
    const auto phase_started = phase_started_.load();
    const auto seq = seq_.load();
    const bool stable = (epoch % 2 == 0 && epoch == epoch_.load(std::memory_order_acquire));
    Json result = {{"stage", "connection_observer_sample"}, {"role", role_}, {"trace_session_id", session_},
        {"actor", actor_}, {"seq", seq}, {"phase", names.at(static_cast<std::size_t>(phase))},
        {"phase_snapshot_stable", stable}, {"sample_monotonic_ms", steady_ms()},
        {"native_control_capabilities", kNativeControlCapabilities},
        {"connection_started_monotonic_ms", started_}};
    if (stable) {
      result["phase_started_monotonic_ms"] = phase_started;
      result["phase_elapsed_ms"] = std::max<std::int64_t>(0, steady_ms() - phase_started);
    }
    if (include_socket) {
      // Socket lifetime is guarded separately from service/control locks. The
      // handler detaches before the HTTP server can close/reuse this descriptor.
      std::unique_lock lock(socket_mutex_, std::try_to_lock);
      if (lock.owns_lock() && !detached_) result["transport"] = websocket_transport_diagnostics(socket_);
      else result["socket_sample_skipped"] = true;
    }
    return result;
  }
 private:
  std::mutex socket_mutex_;
  SocketHandle socket_;
  bool detached_{false};
  const std::string session_, actor_, role_;
  std::int64_t started_{0};
  std::atomic<std::uint64_t> epoch_{0}, seq_{0};
  std::atomic<ControlObservationPhase> phase_{ControlObservationPhase::Idle};
  std::atomic<std::int64_t> phase_started_{0};
};

class ControlConnectionObserverRegistry {
 public:
  std::shared_ptr<ControlConnectionObservation> observe(SocketHandle socket, std::string session,
      std::string actor, std::string role) {
    std::lock_guard lock(mutex_);
    std::erase_if(states_, [](const auto& state) { return state.expired(); });
    if (states_.size() >= 256) return {};
    auto state = std::make_shared<ControlConnectionObservation>(socket, std::move(session), std::move(actor), std::move(role));
    states_.push_back(state);
    return state;
  }
  Json sample() {
    std::vector<std::shared_ptr<ControlConnectionObservation>> active;
    {
      std::unique_lock lock(mutex_, std::try_to_lock);
      if (!lock.owns_lock()) return Json::array();
      for (const auto& weak : states_) if (auto state = weak.lock()) active.push_back(std::move(state));
    }
    Json result = Json::array();
    for (const auto& state : active) result.push_back(state->sample());
    return result;
  }
 private:
  std::mutex mutex_;
  std::vector<std::weak_ptr<ControlConnectionObservation>> states_;
};

struct ScopedControlConnectionObservation {
  std::shared_ptr<ControlConnectionObservation> state;
  ~ScopedControlConnectionObservation() { if (state) state->detach(); }
};

class AsyncControlTrace {
 public:
  using Emitter = std::function<void(Json)>;
  using Observer = std::function<Json()>;

  explicit AsyncControlTrace(Emitter emitter, Observer observer = {})
      : emitter_(std::move(emitter)), observer_(std::move(observer)) {
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
      if (!queue_.try_push(std::move(record))) {
        dropped_total_.fetch_add(1, std::memory_order_relaxed);
        cv_.notify_one();
        return;
      }
      const bool full_batch = queue_.pending() >= kBatchMaxRecords;
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
        queue_.close();
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
    auto last_observed = std::chrono::steady_clock::time_point{};
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
                return stop_requested_ || queue_.pending() >= kBatchMaxRecords;
              });
          Json record;
          for (std::size_t index = 0; index < kBatchMaxRecords && queue_.try_pop(record); ++index) {
            commands.push_back(std::move(record));
          }
          final = stop_requested_ && queue_.drained();
        }

        const auto command_count = commands.size();
        Json observations = Json::array();
        const auto observer_now = std::chrono::steady_clock::now();
        if (!final && observer_ && observer_now - last_observed >= std::chrono::seconds(1)) {
          try {
            observations = observer_();
            for (auto& observation : observations) {
              observation["observer_heartbeat_gap_ms"] = last_observed == std::chrono::steady_clock::time_point{}
                  ? Json(nullptr) : Json(std::chrono::duration_cast<std::chrono::milliseconds>(observer_now - last_observed).count());
            }
          } catch (...) { observations = Json::array(); }
          last_observed = observer_now;
        }
        const auto dropped_total = dropped_total_.load(std::memory_order_relaxed);
        if (command_count > 0 || !observations.empty() || dropped_total != reported_dropped_total || final) {
          try {
            const auto next_emitted_total = emitted_total + command_count;
            emitter_({
                {"batch_seq", ++batch_seq},
                {"final", final},
                {"queue_capacity", kQueueCapacity},
                {"commands", std::move(commands)},
                {"observations", std::move(observations)},
                {"enqueued_total", queue_.accepted_total()},
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
  Observer observer_;
  std::mutex mutex_;
  std::condition_variable cv_;
  detail::DiagnosticQueue<Json, kQueueCapacity> queue_;
  std::thread worker_;
  bool stop_requested_{false};
  std::atomic<bool> accepting_{false};
  std::atomic<std::uint64_t> dropped_total_{0};
};

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

class ServiceUnavailable final : public std::runtime_error {
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

std::int64_t monotonic_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
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
    case 410: return "Gone";
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

  const auto headers_received_at_utc_ms = now_ms();
  const auto headers_received_at_monotonic_ms = monotonic_now_ms();
  std::istringstream headers(wire.substr(0, header_end));
  HttpRequest request;
  request.received_at_utc_ms = headers_received_at_utc_ms;
  request.received_at_monotonic_ms = headers_received_at_monotonic_ms;
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
    const auto name = lower(trim(header.substr(0, separator)));
    if ((name == "host" || name == "origin" || name == "content-type" || name == "sec-fetch-site") &&
        request.headers.contains(name))
      throw std::invalid_argument("duplicate HTTP security header");
    request.headers[name] = trim(header.substr(separator + 1));
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
    request.received_at_utc_ms = now_ms();
    request.received_at_monotonic_ms = monotonic_now_ms();
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
  request.parsed_at_monotonic_ms = monotonic_now_ms();
  return request;
}

std::string console_html(const DriverConfig& config) {
  const Json page_config = {
      {"diagnostics_revision", "traction-diagnostics-v1"},
      {"console_source_sha256", web::kConsoleSourceSha256},
      {"rate_hz", config.rate_hz},
      {"intent_lease_ms", config.intent_lease_ms},
      {"mock_control_diagnostic", config.mock_control_diagnostic},
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
.visual-stage>#estop-status,.visual-stage>#control-recovery-status{grid-row:1}.estop-banner.recovery-banner{border-color:#fbbf24;background:#422f08;color:#fef3c7}.visual-stage>#cameras{grid-row:2}.visual-stage>#can-feedback-panel{grid-row:3}.visual-stage>#status{grid-row:4}
.visual-stage>.grid{overflow:hidden}
.visual-stage .camera>video{position:absolute;inset:0;width:100%;height:100%;object-fit:contain}
.can-feedback-panel .can-summary{grid-template-columns:repeat(5,minmax(0,1fr))}
@media(max-height:760px){.operator-status-item{padding:4px 6px}.operator-status-item span{font-size:8px}.operator-status-item strong{font-size:14px}}
@media(max-width:900px){body .operator-status-strip{grid-template-columns:repeat(3,minmax(0,1fr))}}
@media(max-width:720px){body .operator-status-strip{position:sticky;top:0;z-index:20;grid-template-columns:repeat(3,minmax(0,1fr))}.workspace>.visual-stage{min-height:0}.visual-stage>.grid{grid-template-columns:1fr;grid-template-rows:none;grid-auto-rows:clamp(190px,56vw,300px);overflow:visible}.can-feedback-panel .can-summary{grid-template-columns:repeat(2,minmax(0,1fr))}}
*{box-sizing:border-box}html,body{height:100%;overflow:hidden}body{background:var(--bg);color:var(--text);font-family:Inter,ui-sans-serif,system-ui,-apple-system,BlinkMacSystemFont,"Segoe UI",sans-serif;margin:0;padding:12px}button,input,select{font:inherit}button,input,select{border:1px solid var(--line);border-radius:7px;background:#0b1422;color:var(--text);padding:8px 10px}button{cursor:pointer}button:hover{border-color:#58708e}button:focus-visible,input:focus-visible,select:focus-visible,main:focus-visible{outline:2px solid var(--accent);outline-offset:2px}.danger{background:#be123c;border-color:#fb7185;color:#fff}.app-shell{height:calc(100svh - 24px);min-height:0;display:grid;grid-template-rows:auto minmax(0,1fr);gap:10px;max-width:1920px;margin:auto}.topbar{display:flex;align-items:center;justify-content:space-between;gap:16px;min-width:0}.brand{display:flex;align-items:baseline;gap:12px;min-width:max-content}.brand h1{font-size:21px;line-height:1.1;margin:0;letter-spacing:-.02em}.brand p{font-size:12px;color:var(--muted);margin:0}.auth{display:flex;align-items:center;justify-content:flex-end;gap:7px;flex-wrap:wrap;margin:0;min-width:0}.auth label{font-size:12px;color:var(--muted)}.auth input{width:170px}.auth select{max-width:220px}.auth strong{font-size:12px;color:var(--ok)}.workspace{display:grid;grid-template-columns:minmax(0,1fr) clamp(300px,25vw,390px);gap:10px;min-height:0}.visual-stage{display:grid;grid-template-rows:auto minmax(0,1fr) auto auto;gap:8px;min-width:0;min-height:0}.grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));grid-template-rows:repeat(2,minmax(0,1fr));gap:8px;min-height:0}.camera{background:#020617;border:1px solid #162338;border-radius:8px;overflow:hidden;position:relative;min-width:0;min-height:0}.camera video{display:block;width:100%;height:100%;object-fit:contain}.label{position:absolute;left:7px;top:7px;background:#020617cc;padding:3px 7px;border-radius:5px;font-size:11px;z-index:2}.empty-stage{grid-column:1/-1;grid-row:1/-1;display:grid;place-items:center;border:1px dashed var(--line);border-radius:8px;color:var(--muted);font-size:13px}.can-feedback-panel{background:var(--surface);border:1px solid var(--line);border-radius:8px;padding:7px;min-width:0}.can-feedback-heading{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:5px}.can-feedback-heading h2{font-size:11px;letter-spacing:.05em;text-transform:uppercase;margin:0}.can-summary{display:grid;grid-template-columns:repeat(7,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:5px;overflow:hidden;margin-bottom:5px}.can-summary-item{background:var(--surface-2);padding:3px 5px;min-width:0}.can-summary-item span{display:block;color:var(--muted);font-size:8px}.can-summary-item strong{display:block;font:700 10px/1.25 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.can-feedback-body{display:grid;grid-template-columns:minmax(0,4fr) minmax(150px,1fr);gap:5px}.wheel-feedback-grid{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:3px}.wheel-feedback,.steering-feedback{background:#0b1422;border:1px solid #1f3047;border-radius:5px;padding:3px 5px;min-width:0}.feedback-title{display:flex;justify-content:space-between;gap:4px;color:#cbd5e1;font-size:8px}.feedback-values{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:3px;margin-top:2px}.feedback-value{min-width:0}.feedback-value span{display:block;color:var(--muted);font-size:7px}.feedback-value strong{display:block;font:700 9px/1.2 ui-monospace,SFMono-Regular,Menlo,monospace;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}.steering-feedback-grid{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:3px}.steering-feedback .feedback-values{grid-template-columns:minmax(0,1fr) auto}.sidebar{min-height:0;overflow:auto;display:flex;flex-direction:column;gap:10px;padding-right:2px}.side-section{background:var(--surface);border:1px solid var(--line);border-radius:9px;padding:10px}.section-heading{display:flex;align-items:center;justify-content:space-between;gap:8px;margin-bottom:8px}.section-heading h2{font-size:13px;letter-spacing:.04em;text-transform:uppercase;margin:0}.status-chip{font-size:11px;color:var(--muted)}.key-help,.gate-copy{font-size:11px;color:var(--muted);margin:0 0 8px;line-height:1.35}.keyboard-grid{display:grid;grid-template-columns:repeat(3,46px);grid-template-rows:repeat(2,40px);justify-content:center;gap:5px;margin:8px 0}.keycap{display:flex;align-items:center;justify-content:center;gap:4px;border:1px solid #41536d;border-radius:7px;background:#0a1321;color:#aebdd0;font:700 15px/1 ui-monospace,SFMono-Regular,Menlo,monospace;transition:background .08s ease,border-color .08s ease,color .08s ease,transform .08s ease}.keycap small{font-size:9px;color:#71839b}.keycap.active{background:#075985;border-color:var(--accent);color:#fff;transform:translateY(1px);box-shadow:0 0 0 2px #38bdf826}.keycap.active small{color:#bae6fd}.key-up{grid-column:2}.key-left{grid-column:1;grid-row:2}.key-down{grid-column:2;grid-row:2}.key-right{grid-column:3;grid-row:2}.brake-keys{display:grid;grid-template-columns:1fr 1fr;gap:5px;margin:7px 0 10px}.brake-key{height:30px;font-size:11px}.last-input{display:block;text-align:center;min-height:17px;font-size:11px;color:var(--accent)}.control-readouts{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:7px;overflow:hidden}.control-readout{background:var(--surface-2);padding:8px}.control-readout span{display:block;font-size:10px;color:var(--muted);margin-bottom:3px}.control-readout strong{display:block;font:700 18px/1.1 ui-monospace,SFMono-Regular,Menlo,monospace;font-variant-numeric:tabular-nums}.control-readout.active strong{color:var(--accent)}.side-actions{display:grid;grid-template-columns:1fr 1fr;gap:6px}.side-actions button{font-size:11px;padding:7px 6px}.limit-inline{display:flex;align-items:center;justify-content:space-between;gap:8px;margin:0 0 7px;color:var(--muted);font-size:10px}.side-section .limit-inline strong{font-size:11px}.metrics{display:grid;grid-template-columns:repeat(2,minmax(0,1fr));gap:1px;background:var(--line);border:1px solid var(--line);border-radius:7px;overflow:hidden}.metric{background:var(--surface-2);padding:7px}.metric span{display:block;color:var(--muted);font-size:9px;margin-bottom:3px}.metric strong{display:block;font-size:12px;line-height:1.25;overflow-wrap:anywhere}.ok{color:var(--ok)}.warn{color:var(--warn)}.critical{color:var(--critical)}.alerts{border-left:3px solid #475569;background:#0b1422;padding:7px 8px;margin:8px 0;font-size:11px;line-height:1.35}.alerts.warn{border-color:#f59e0b}.alerts.critical{border-color:#e11d48}table{width:100%;border-collapse:collapse;background:#0b1422;font-size:9px;table-layout:fixed}th,td{text-align:left;padding:5px 3px;border-bottom:1px solid #26364d;font-variant-numeric:tabular-nums;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}th:first-child,td:first-child{width:30%}.estop-banner{border:1px solid #fb7185;background:#881337;color:#fff;border-radius:7px;padding:7px 10px;margin:0;font-size:12px}.status-line{background:#020617;border:1px solid #162338;border-radius:7px;color:#9fb0c6;font:11px/1.35 ui-monospace,SFMono-Regular,Menlo,monospace;margin:0;padding:7px 9px;min-height:31px;max-height:48px;overflow:auto;white-space:pre-wrap}.muted{color:var(--muted)}[hidden]{display:none!important}dialog{color:var(--text);background:var(--surface);border:1px solid #4b5563;border-radius:10px;max-width:520px;width:calc(100% - 40px);padding:18px}dialog::backdrop{background:#000a}dialog h2{margin:0 0 12px;font-size:18px}.limit-grid{display:grid;grid-template-columns:1fr 1fr;gap:12px}.limit-grid label{display:flex;flex-direction:column;gap:5px;font-size:12px}.limit-warning{border-left:4px solid #f59e0b;padding:8px 10px;background:#1f2937;font-size:12px;line-height:1.45}
@media(max-height:760px){body{padding:8px}.app-shell{height:calc(100svh - 16px);gap:7px}.brand p{display:none}.sidebar{gap:7px}.side-section{padding:7px}.section-heading{margin-bottom:5px}.key-help,.gate-copy{margin-bottom:4px;line-height:1.25}.keyboard-grid{grid-template-columns:repeat(3,40px);grid-template-rows:repeat(2,30px);margin:3px 0}.brake-keys{margin:3px 0}.brake-key{height:26px}.last-input{min-height:14px;font-size:10px}.control-readout{padding:4px 6px}.control-readout span{font-size:9px;margin-bottom:2px}.control-readout strong{font-size:16px}.side-actions button{padding:5px 4px}.limit-inline{margin-bottom:4px}.metric{padding:3px 5px}.metric span{margin-bottom:2px}.metric strong{font-size:11px}.alerts{margin:4px 0;padding:4px 6px;font-size:10px;line-height:1.25}th,td{padding:3px 2px}}
@media(max-width:900px){.workspace{grid-template-columns:minmax(0,1fr) 290px}.brand p{display:none}.auth label,.auth-expiry{display:none}}
@media(max-width:720px){html,body{overflow:auto}.app-shell{height:auto;min-height:100svh}.topbar{align-items:flex-start;flex-direction:column}.auth{justify-content:flex-start}.workspace{grid-template-columns:1fr}.visual-stage{min-height:58svh}.sidebar{overflow:visible}.grid{grid-template-columns:1fr}.can-summary{grid-template-columns:repeat(2,minmax(0,1fr))}.can-feedback-body{grid-template-columns:1fr}.wheel-feedback-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.steering-feedback-grid{grid-template-columns:repeat(4,minmax(0,1fr))}.limit-grid{grid-template-columns:1fr}}</style><link rel="stylesheet" href="/assets/console-layout.css"></head><body><main class="app-shell" tabindex="-1">
<header class="topbar"><div class="brand"><h1>Mine Teleop 控制台</h1><p>方向键 / WASD 控制 · Space 缓刹 · B 急刹 · E 急停</p></div>
<section id="login-panel" class="auth"><label for="driver-id">驾驶员 ID</label><input id="driver-id" type="text" autocomplete="username" maxlength="128" spellcheck="false" placeholder="请输入驾驶员 ID"><label for="password">驾驶员密码</label><input id="password" type="password" autocomplete="current-password"><button id="login">登录并加载车辆</button></section>
<section id="session-panel" class="auth" hidden><label for="vehicle">授权车辆</label><select id="vehicle"></select><button id="connect">连接所选车辆</button><button id="control-limits-open">实车调试限幅</button><button id="export-session-logs">导出会话日志</button><button id="logout">安全退出</button><button id="estop" class="danger">急停</button><strong id="webrtc">未连接</strong><span id="auth-expiry" class="muted auth-expiry"></span></section></header>
<section id="operator-status-strip" class="operator-status-strip" aria-label="关键车辆状态"><article class="operator-status-item"><span>实测车速</span><strong id="operator-speed">—</strong></article><article class="operator-status-item"><span>实际挡位</span><strong id="operator-actual-gear">—</strong></article><article id="operator-readout-gear" class="operator-status-item"><span>指令挡位</span><strong id="operator-control-gear">N</strong></article><article id="operator-readout-steering" class="operator-status-item"><span>转向</span><strong id="operator-control-steering">0.00</strong></article><article id="operator-readout-throttle" class="operator-status-item"><span>目标车速比例</span><strong id="operator-control-throttle">0.00</strong></article><article id="operator-readout-brake" class="operator-status-item"><span>刹车</span><strong id="operator-control-brake">0.00</strong></article></section>
<div class="workspace"><section class="visual-stage"><p id="estop-status" class="estop-banner" hidden>急停请求已锁定；等待车端遥测确认，未确认时请使用车辆物理急停。</p><p id="control-recovery-status" class="estop-banner recovery-banner" role="status" aria-live="polite" hidden></p><section id="cameras" class="grid"><div id="empty-stage" class="empty-stage">登录并连接车辆后显示实时视频</div></section><section id="can-feedback-panel" class="can-feedback-panel" hidden aria-label="CAN 实时反馈"><div class="can-feedback-heading"><h2>CAN 实时反馈 · 测量值</h2><strong id="can-feedback-status" class="status-chip warn">等待车端遥测</strong></div><div class="can-summary"><div class="can-summary-item"><span>车速</span><strong id="can-speed">—</strong></div><div class="can-summary-item"><span>实际挡位</span><strong id="can-gear">—</strong></div><div class="can-summary-item"><span>物理选择器</span><strong id="can-selector">—</strong></div><div class="can-summary-item"><span>电子驻车 1-4</span><strong id="can-epb">—</strong></div><div class="can-summary-item"><span>VCU 状态</span><strong id="can-handshake">—</strong></div><div class="can-summary-item"><span>VMC 故障码</span><strong id="can-vmc-fault">—</strong></div><div class="can-summary-item"><span>物理手刹</span><strong id="can-parking-switch">—</strong></div><div class="can-summary-item"><span>制动踏板</span><strong id="can-brake-pedal">—</strong></div><div class="can-summary-item"><span>紧急开关</span><strong id="can-emergency">—</strong></div><div class="can-summary-item"><span>反馈时效</span><strong id="can-age">—</strong></div></div><div class="can-feedback-body"><div id="wheel-feedback-grid" class="wheel-feedback-grid" aria-label="八路轮端反馈"></div><div id="steering-feedback-grid" class="steering-feedback-grid" aria-label="四路转向反馈"></div></div></section><pre id="status" class="status-line">请先登录</pre></section>
<aside class="sidebar"><section id="keyboard-panel" class="side-section" aria-label="键盘控制状态"><div class="section-heading"><h2>键盘控制</h2><span id="input-readiness" class="status-chip">等待连接</span></div><p class="key-help">方向键与 WASD 等效；D/R 在会话内锁存，松开前进/倒车只将目标车速比例归零。</p><div class="keyboard-grid" aria-label="方向键状态"><span id="key-up" class="keycap key-up" aria-pressed="false">↑<small>W</small></span><span id="key-left" class="keycap key-left" aria-pressed="false">←<small>A</small></span><span id="key-down" class="keycap key-down" aria-pressed="false">↓<small>S</small></span><span id="key-right" class="keycap key-right" aria-pressed="false">→<small>D</small></span></div><div class="brake-keys"><span id="key-service-brake" class="keycap brake-key" aria-pressed="false">SPACE · 缓刹</span><span id="key-hard-brake" class="keycap brake-key" aria-pressed="false">B · 急刹</span></div><span id="last-keyboard-event" class="last-input">等待键盘输入</span><div class="control-readouts"><div id="readout-gear" class="control-readout"><span>挡位</span><strong id="control-gear">N</strong></div><div id="readout-steering" class="control-readout"><span>转向</span><strong id="control-steering">0.00</strong></div><div id="readout-throttle" class="control-readout"><span>目标车速比例</span><strong id="control-throttle">0.00</strong></div><div id="readout-brake" class="control-readout"><span>刹车</span><strong id="control-brake">0.00</strong></div></div></section>
<section id="vcu-panel" class="side-section" hidden><div class="section-heading"><h2>VCU 平行驾驶</h2><strong id="vcu-status" class="status-chip warn">等待车端 VCU 状态</strong></div><p class="limit-inline"><span>当前会话限幅</span><strong id="control-limits-summary" class="warn">等待车端确认会话控制参数</strong></p><p id="vcu-gate" class="gate-copy warn" role="status" aria-live="polite">准入检查：需要车端确认会话控制参数、N 挡、电子驻车、零速及 VCU 人工状态</p><div class="side-actions"><button id="vcu-connect" disabled>开始平行驾驶握手</button><button id="vcu-disconnect" disabled>断开 VCU 握手</button></div></section>
<section id="monitor-panel" class="side-section" hidden><div class="section-heading"><h2>运行监控</h2><span class="status-chip">实时</span></div><div class="metrics"><article class="metric"><span>车辆在线</span><strong id="metric-vehicle">未知</strong></article><article class="metric"><span>当前会话</span><strong id="metric-session">未连接</strong></article><article class="metric"><span>当前控制权</span><strong id="metric-authority">无</strong></article><article class="metric"><span>视频编码 / 后端</span><strong id="metric-video">等待媒体</strong></article><article class="metric"><span>媒体 ICE RTT</span><strong id="metric-rtt">未知</strong></article><article class="metric"><span>云入口 ACK</span><strong id="metric-cloud-ack">尚无入口确认</strong></article><article class="metric"><span>车端软件结果</span><strong id="metric-vehicle-result">尚无关联结果</strong></article><article class="metric"><span>车端最近接受</span><strong id="metric-vehicle-accepted">尚无关联接受结果</strong></article><article class="metric"><span>网络连接</span><strong id="metric-network">未知</strong></article><article class="metric"><span>TURN</span><strong id="metric-turn">未配置</strong></article><article class="metric"><span>时间同步</span><strong id="metric-time">未知</strong></article><article class="metric"><span>录像</span><strong id="metric-recording">未确认</strong></article></div><div id="alerts" class="alerts">尚无媒体指标；控制命令不会在链路未就绪时发送。</div><table><thead><tr><th>camera</th><th>FPS</th><th>kbps</th><th>loss</th><th>latency</th></tr></thead><tbody id="stream-metrics"><tr><td colspan="5" class="muted">等待视频轨道</td></tr></tbody></table></section></aside></div>
<dialog id="control-limits-dialog"><h2>当前会话驾驶与 PID 参数</h2><p class="limit-warning">这些值仅用于当前控制会话，必须由车端精确确认后才生效；车端本地硬上限仍会再次截断。三个制动字段都是每路 EHB 压力请求，单位 bar、分辨率 0.1 bar，不是百分比或整车制动力。急停、物理急停、故障、断开停车和 bridge 本地 watchdog 使用独立安全制动，普通 profile 不能削弱这些安全路径。升扭斜率是会话标定值，不是车型级硬上限；0 表示取消升扭限制，下一次牵引可能在一个控制周期内达到当前会话的单电机最大转矩。修改时立即清零当前输入。</p><fieldset><legend>驾驶参数</legend><div class="limit-grid"><label for="target-speed-kph">目标车速上限（km/h）<input id="target-speed-kph" type="number" min="0" max="72" step="0.1" inputmode="decimal"></label><label for="max-motor-torque-nm">单电机最大驱动转矩（Nm）<input id="max-motor-torque-nm" type="number" min="0" max="640.0" step="0.1" inputmode="decimal"></label><label for="max-brake-pressure-bar">每路 EHB 最大普通压力（bar）<input id="max-brake-pressure-bar" type="number" min="0" max="327.6" step="0.1" inputmode="decimal"></label><label for="service-brake-pressure-bar">每路 EHB 缓刹压力（bar）<input id="service-brake-pressure-bar" type="number" min="0" max="327.6" step="0.1" inputmode="decimal"></label><label for="hard-brake-pressure-bar">每路 EHB 急刹压力（bar）<input id="hard-brake-pressure-bar" type="number" min="0" max="327.6" step="0.1" inputmode="decimal"></label><label for="consecutive-late-frame-limit">连续迟到帧数（1–5 帧，默认 3）<input id="consecutive-late-frame-limit" type="number" min="1" max="5" step="1" value="3" required><span class="muted">过期命令不执行；达到此帧数需释放后重新按下。1 帧为严格模式，收包间隔超过 200 ms 也需重新输入；任意模式下命令中断导致降级都需重新输入。持续无有效控制 800 ms 仍安全停车。</span></label><label for="critical-camera-stale-brake">关键画面无新帧制动（ms）<input id="critical-camera-stale-brake" type="number" min="100" max="3000" step="1" value="500" required><span class="muted">关键画面遮罩在连续 1 秒无新帧后显示；制动仍按此门限执行。</span></label><label for="control-recovery-timeout">ACK 断流重连等待（秒）<input id="control-recovery-timeout" type="number" min="1" max="30" step="0.001" value="5" required><span class="muted">视频、校时或输入暂态异常保留会话制动等待；此值用于 ACK 无推进的传输重连。共同健康 300 ms 后仍需释放旧输入再重按。</span></label><label for="parking-idle-timeout">无操作自动驻车（秒，0=松键即制动停车）<input id="parking-idle-timeout" type="number" min="0" max="1" step="0.001" value="0.5" required></label><label for="max-steering-deg">最大四轴转向角（°）<input id="max-steering-deg" type="number" min="0" max="30" step="0.5" inputmode="decimal"></label></div></fieldset><fieldset><legend>速度 PID 与升扭标定</legend><p class="muted">PID 初始值与可调范围只使用当前车端上报；未收到完整默认值和范围时禁止提交与驾驶。升扭斜率数值越大，转矩建立越快；0 不是禁用驱动，而是直接跟随 PID 输出。</p><div class="limit-grid"><label for="speed-pid-kp">Kp<input id="speed-pid-kp" type="number" min="0" max="100" step="0.01" inputmode="decimal"></label><label for="speed-pid-ki">Ki<input id="speed-pid-ki" type="number" min="0" max="100" step="0.01" inputmode="decimal"></label><label for="speed-pid-kd">Kd<input id="speed-pid-kd" type="number" min="0" max="100" step="0.01" inputmode="decimal"></label><label for="speed-pid-derivative-filter-tau-ms">微分滤波 τ（ms）<input id="speed-pid-derivative-filter-tau-ms" type="number" min="0" max="2000" step="1" inputmode="numeric"></label><label for="speed-pid-max-dt-ms">最大采样周期（ms）<input id="speed-pid-max-dt-ms" type="number" min="20" max="200" step="1" inputmode="numeric"></label><label for="motor-torque-rise-rate">升扭斜率（Nm/s，0=直接跟随 PID 输出）<input id="motor-torque-rise-rate" type="number" min="0" max="32000" step="1" inputmode="decimal" required></label></div></fieldset><p id="vehicle-hard-limits" class="muted">等待车端硬上限与 PID 默认值</p><label><input id="control-limits-confirm" type="checkbox">我已确认车辆处于 N 挡、零速、电子驻车或隔离 mock 台架，并理解 0 表示取消升扭限制、可能单周期达到会话转矩上限</label><div><button id="control-limits-apply" disabled>发送并等待车端确认</button><button id="control-limits-cancel">取消</button></div></dialog></main><script>)HTML" + std::string(web::kControlLogicJavaScript) + R"HTML(</script><script>
MineTeleopSessionLogs.install();
const controlLogic=MineTeleopControlLogic;
const consoleConfig=)HTML" + page_config.dump() + R"HTML(;
const gamepadConfig=consoleConfig.gamepad;
const limitConfig=consoleConfig.control_limits;
const keys=controlLogic.KEY_BINDINGS;
let state=controlLogic.deriveKeyState(controlLogic.createKeySet());
const pressedControlKeys=controlLogic.createKeySet(),blockedControlKeys=controlLogic.createKeySet();
const gamepadState={connected:false,steering:0,throttle:0,brake:0};
const driverActuationDefaults={target_speed_kph:limitConfig.initial_target_speed_kph,max_motor_torque_nm:limitConfig.initial_max_motor_torque_nm,max_brake_pressure_bar:limitConfig.initial_max_brake_pressure_bar,service_brake_pressure_bar:limitConfig.initial_service_brake_pressure_bar,hard_brake_pressure_bar:limitConfig.initial_hard_brake_pressure_bar,max_steering_angle_deg:limitConfig.initial_max_steering_angle_deg};
let controlProfileState={requestedProfile:null,pendingRequestSeq:0,effectiveProfile:null,effectiveRequestSeq:0,effectiveAppliedRevision:0,acknowledged:false,reason:''},pendingControlProfileEnvelope=null,lastControlProfileSendAt=0,controlProfilePrepareInFlight=false,controlProfileGeneration=0;
let vehicleHardLimits={received:false};
const controlTraceEnabled=Boolean(consoleConfig.control_trace_commands),intentRefreshIntervalMs=Math.max(50,Math.floor(Number(consoleConfig.intent_lease_ms||200)/3)),controlTraceBuffer=[],controlTraceErrorScopes=new WeakMap();
let controlTraceSummary=createControlTraceSummary(),controlTraceScope={session_id:'',vehicle_id:''},lastHeartbeatTraceAt=null;
const calibration={steeringCenter:gamepadConfig.steering_center,steeringRange:gamepadConfig.steering_range,throttleRest:gamepadConfig.throttle_rest,throttleRange:gamepadConfig.throttle_range,brakeRest:gamepadConfig.brake_rest,brakeRange:gamepadConfig.brake_range};
const webrtcLabel=document.getElementById('webrtc'),cameraGrid=document.getElementById('cameras'),statusPanel=document.getElementById('status'),loginPanel=document.getElementById('login-panel'),sessionPanel=document.getElementById('session-panel'),driverIdInput=document.getElementById('driver-id'),passwordInput=document.getElementById('password'),vehicleSelect=document.getElementById('vehicle'),connectButton=document.getElementById('connect'),authExpiry=document.getElementById('auth-expiry'),vcuPanel=document.getElementById('vcu-panel'),vcuStatus=document.getElementById('vcu-status'),vcuGate=document.getElementById('vcu-gate'),vcuConnectButton=document.getElementById('vcu-connect'),vcuDisconnectButton=document.getElementById('vcu-disconnect'),controlLimitsOpen=document.getElementById('control-limits-open'),controlLimitsSummary=document.getElementById('control-limits-summary'),controlLimitsDialog=document.getElementById('control-limits-dialog'),targetSpeedKph=document.getElementById('target-speed-kph'),maxMotorTorqueNm=document.getElementById('max-motor-torque-nm'),maxBrakePressureBar=document.getElementById('max-brake-pressure-bar'),serviceBrakePressureBar=document.getElementById('service-brake-pressure-bar'),hardBrakePressureBar=document.getElementById('hard-brake-pressure-bar'),maxSteeringDeg=document.getElementById('max-steering-deg'),speedPidKp=document.getElementById('speed-pid-kp'),speedPidKi=document.getElementById('speed-pid-ki'),speedPidKd=document.getElementById('speed-pid-kd'),speedPidDerivativeFilterTauMs=document.getElementById('speed-pid-derivative-filter-tau-ms'),speedPidMaxDtMs=document.getElementById('speed-pid-max-dt-ms'),parkingIdleTimeout=document.getElementById('parking-idle-timeout'),consecutiveLateFrameLimit=document.getElementById('consecutive-late-frame-limit'),criticalCameraStaleBrake=document.getElementById('critical-camera-stale-brake'),controlRecoveryTimeout=document.getElementById('control-recovery-timeout'),motorTorqueRiseRate=document.getElementById('motor-torque-rise-rate'),vehicleHardLimitsLabel=document.getElementById('vehicle-hard-limits'),controlLimitsConfirm=document.getElementById('control-limits-confirm'),controlLimitsApply=document.getElementById('control-limits-apply'),controlLimitsCancel=document.getElementById('control-limits-cancel'),estopStatus=document.getElementById('estop-status'),controlRecoveryStatus=document.getElementById('control-recovery-status'),monitorPanel=document.getElementById('monitor-panel'),alertsPanel=document.getElementById('alerts'),streamMetrics=document.getElementById('stream-metrics'),inputReadiness=document.getElementById('input-readiness'),lastKeyboardEvent=document.getElementById('last-keyboard-event'),emptyStage=document.getElementById('empty-stage');
const canFeedbackPanel=document.getElementById('can-feedback-panel'),canFeedbackStatus=document.getElementById('can-feedback-status'),canSpeed=document.getElementById('can-speed'),canGear=document.getElementById('can-gear'),canSelector=document.getElementById('can-selector'),canEpb=document.getElementById('can-epb'),canHandshake=document.getElementById('can-handshake'),canVmcFault=document.getElementById('can-vmc-fault'),canParkingSwitch=document.getElementById('can-parking-switch'),canBrakePedal=document.getElementById('can-brake-pedal'),canEmergency=document.getElementById('can-emergency'),canAge=document.getElementById('can-age'),wheelFeedbackGrid=document.getElementById('wheel-feedback-grid'),steeringFeedbackGrid=document.getElementById('steering-feedback-grid');
const operatorSpeed=document.getElementById('operator-speed'),operatorActualGear=document.getElementById('operator-actual-gear');
const keyIndicators={left:document.getElementById('key-left'),right:document.getElementById('key-right'),up:document.getElementById('key-up'),down:document.getElementById('key-down'),service_brake:document.getElementById('key-service-brake'),hard_brake:document.getElementById('key-hard-brake')};
const controlReadouts={gear:document.getElementById('control-gear'),steering:document.getElementById('control-steering'),throttle:document.getElementById('control-throttle'),brake:document.getElementById('control-brake')};
const operatorControlReadouts={gear:document.getElementById('operator-control-gear'),steering:document.getElementById('operator-control-steering'),throttle:document.getElementById('operator-control-throttle'),brake:document.getElementById('operator-control-brake')};
let peer=null,controlChannel=null,pendingIce=[],remoteCameraIds=[],offeredCameraByMid=new Map(),iceServers=[],polling=false,connecting=false,authenticated=false,mediaStatus={lanes:[]},h265FailureSamples=0,h265FallbackSent=false,estopLatched=false,gamepadEstopPressedAt=0,gamepadRequiresNeutral=true,gamepadPreserveBrakeDuringRearm=false,activeGamepadIndex=null,latestMetrics={streams:[]},latestRuntimeStatus={},lastAlertKey='',controlAuthorityLost=false,gearRejectionInhibited=false,signalingGeneration=0,signalingPollAbort=null,vehicleTelemetry=null,lastVehicleSafetyState='',vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null},vcuEverReady=false,selectedGear='N',pendingGearRequest=null,pendingGearTransition=null,gearTransitionGeneration=0,lastControlStatusSeq=0,lastNativeAcceptedControlSeq=0,lastControlPrepareTimeoutLogAt=0,lastControlPrepareExpiredLogAt=0,activeControlRequests=new Map();let vehicleTelemetryReceivedAt=null,lastFeedbackHealthLogAt=null,lastFeedbackHealthKey='';let lastMediaMetricsLogAt=null;const previousStats=new Map(),cameraByMid=new Map(),assignedCameraIds=new Set();
let mediaPeerEpoch=0,mediaOfferScope=null,mediaHasOffer=false,mediaScopeIssue='';
const retiredMediaScopes=new Set();
let gearChangeStationaryEvidence=controlLogic.createGearChangeStationaryEvidence();
const uiInstanceId=(globalThis.crypto?.randomUUID?.()||`ui-${Date.now()}-${Math.random().toString(16).slice(2)}`).replace(/[^A-Za-z0-9_-]/g,'_');
let connectRequestGeneration=0;
let safeLogoutAttempt=null;
let mediaStatusReceivedAt=null,recordingMonitorState={scopeKey:'',incomplete:false};
let nativeIntentSeq=0,lastNativeIntentSnapshot='',nativeControlSessionId='',nativeControlSessionGeneration=0;
let nativeControlUiEpoch=0,nativeControlTakeoverPending=false,nativeControlPageRetired=false;
let controlOutcomeSession={metrics:controlLogic.createControlOutcomeMetrics()};
function responseError(response,body){const error=Error(body.error||response.status);error.status=response.status;error.issue_code=String(body.issue_code||'');return error}
async function post(path,body={},signal=null){const options={method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(body)};if(signal)options.signal=signal;const r=await fetch(path,options);const j=await r.json();if(!r.ok)throw responseError(r,j);return j}
async function get(path,signal=null){const r=await fetch(path,signal?{signal}:undefined);const j=await r.json();if(!r.ok)throw responseError(r,j);return j}
function clientLog(event,details={}){const entry={event,sent_at_utc_ms:Date.now(),details:{...details,page_source_sha256:consoleConfig.console_source_sha256,diagnostics_revision:consoleConfig.diagnostics_revision,ui_instance_id:uiInstanceId}};console.info(JSON.stringify(entry));fetch('/api/browser-event',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(entry),keepalive:true}).catch(()=>{})}
clientLog('driver_page_loaded',{user_agent:navigator.userAgent,ice_transport_policy:consoleConfig.ice_transport_policy,long_task_observer_supported:typeof PerformanceObserver==='function'&&PerformanceObserver.supportedEntryTypes?.includes('longtask')===true});
if(controlTraceEnabled&&typeof PerformanceObserver==='function'&&PerformanceObserver.supportedEntryTypes?.includes('longtask')){const observer=new PerformanceObserver(list=>{if(!polling)return;for(const item of list.getEntries()){controlTraceSummary.long_task_count++;controlTraceSummary.long_task_max_ms=Math.max(controlTraceSummary.long_task_max_ms,item.duration)}});observer.observe({type:'longtask',buffered:false})}
function createControlTraceSummary(){return{interval_started_at_utc_ms:Date.now(),heartbeat_tick_count:0,heartbeat_enqueued_count:0,heartbeat_coalesced_count:0,timer_lag_sample_count:0,timer_lag_total_ms:0,timer_lag_max_ms:0,explicit_send_count:0,intent_response_preempted_count:0,intent_http_sample_count:0,intent_http_elapsed_max_ms:0,intent_http_server_wait_max_ms:0,intent_http_server_processing_max_ms:0,long_task_count:0,long_task_max_ms:0,backpressure_count:0,max_buffered_amount_bytes:0,prepare_timeout_count:0,prepare_expired_count:0,prepare_preempted_by_estop_count:0,queue_unhandled_error_count:0}}
function noteIntentRefresh(now){if(!controlTraceEnabled)return;controlTraceSummary.heartbeat_tick_count++;if(lastHeartbeatTraceAt!==null){const lag=Math.max(0,now-lastHeartbeatTraceAt-intentRefreshIntervalMs);controlTraceSummary.timer_lag_sample_count++;controlTraceSummary.timer_lag_total_ms+=lag;controlTraceSummary.timer_lag_max_ms=Math.max(controlTraceSummary.timer_lag_max_ms,lag)}lastHeartbeatTraceAt=now}
function noteControlBackpressure(bufferedAmount){if(!controlTraceEnabled)return;const bytes=Math.max(0,Number(bufferedAmount)||0);controlTraceSummary.backpressure_count++;controlTraceSummary.max_buffered_amount_bytes=Math.max(controlTraceSummary.max_buffered_amount_bytes,bytes)}
function setControlTraceScope(sessionId='',vehicleId=''){if(!controlTraceEnabled)return;const next={session_id:String(sessionId||''),vehicle_id:String(vehicleId||'')};if(next.session_id===controlTraceScope.session_id&&next.vehicle_id===controlTraceScope.vehicle_id)return;if(controlTraceScope.session_id||controlTraceScope.vehicle_id)flushControlTrace('session_scope_change');controlTraceScope=next}
function sameControlTraceScope(scope){return Boolean(scope)&&String(scope.session_id||'')===controlTraceScope.session_id&&String(scope.vehicle_id||'')===controlTraceScope.vehicle_id}
function emitControlTraceBatch(reason,scope,commands,summary,sampleTransport=true){const samples=summary.timer_lag_sample_count;summary.interval_ended_at_utc_ms=Date.now();summary.timer_lag_average_ms=samples?summary.timer_lag_total_ms/samples:0;summary.peer_connection_state=sampleTransport?(peer?.connectionState||'none'):null;summary.data_channel_ready_state=sampleTransport?(controlChannel?.readyState||'none'):null;summary.data_channel_buffered_amount_bytes=sampleTransport?Math.max(0,Number(controlChannel?.bufferedAmount)||0):null;clientLog('control_trace_batch',{reason,trace_session_id:String(scope&&scope.session_id||''),trace_vehicle_id:String(scope&&scope.vehicle_id||''),commands,summary})}
function noteScopedControlTraceCounter(field,scope){if(!controlTraceEnabled)return;if(sameControlTraceScope(scope)){controlTraceSummary[field]++;return}const summary=createControlTraceSummary();summary[field]=1;emitControlTraceBatch(`late_${field}`,scope,[],summary,false)}
function queueTerminalControlTrace(outcome,command,trace){if(!controlTraceEnabled||!trace)return;const seq=Number(command&&command.seq);if(!Number.isSafeInteger(seq)||seq<=0)return;const scope={session_id:String(command&&command.session_id||trace.scope&&trace.scope.session_id||''),vehicle_id:String(command&&command.vehicle_id||trace.scope&&trace.scope.vehicle_id||'')},item={session_id:scope.session_id,seq,browser_prepare_started_at_utc_ms:trace.browserPrepareStartedAtUtcMs,native_command_sent_at_utc_ms:Number(command&&command.sent_at_utc_ms)||null,prepare_elapsed_ms:trace.prepareElapsedMs,data_channel_send_invoked_at_utc_ms:trace.dataChannelSendInvokedAtUtcMs,buffered_amount_bytes:trace.bufferedAmountBytes,outcome,browser_terminal_at_utc_ms:Date.now(),estop:Boolean(trace.estop)};if(!sameControlTraceScope(scope)){const summary=createControlTraceSummary();summary.interval_started_at_utc_ms=Number(trace.browserPrepareStartedAtUtcMs)||summary.interval_started_at_utc_ms;emitControlTraceBatch('late_terminal',scope,[item],summary,false);return}controlTraceBuffer.push(item);if(controlTraceBuffer.length>=48)flushControlTrace('capacity')}
function flushControlTrace(reason='interval'){if(!controlTraceEnabled)return false;const summary=controlTraceSummary,scope=controlTraceScope,commands=controlTraceBuffer.splice(0,controlTraceBuffer.length),hasActivity=commands.length||summary.intent_response_preempted_count||summary.intent_http_sample_count||summary.long_task_count||summary.heartbeat_tick_count||summary.explicit_send_count||summary.backpressure_count||summary.prepare_timeout_count||summary.prepare_expired_count||summary.prepare_preempted_by_estop_count||summary.queue_unhandled_error_count;controlTraceSummary=createControlTraceSummary();if(!hasActivity)return false;emitControlTraceBatch(reason,scope,commands,summary);return true}
function recordTerminalControlOutcome(outcome,command,session,trace,details={}){recordControlOutcome(outcome,command,session,details);queueTerminalControlTrace(outcome,command,trace)}
function resetControlOutcomeSession(){controlOutcomeSession={metrics:controlLogic.createControlOutcomeMetrics()}}
function recordControlOutcome(outcome,command,session=controlOutcomeSession,details={}){const sequence=Number(command&&command.seq);session.metrics=controlLogic.reduceControlOutcome(session.metrics,outcome,sequence);if(controlLogic.shouldLogControlOutcome(outcome))clientLog('control_command_browser_outcome',{session_id:String(command&&command.session_id||''),vehicle_id:String(command&&command.vehicle_id||''),outcome,control_seq:sequence,...session.metrics,...details})}
function controlPrepareDeadlineMs(){return controlLogic.controlPrepareDeadlineMs(vehicleHardLimits?.read_only_control_safety?.max_command_gap_ms)}
function logControlPrepareFailure(event,details){const now=Date.now(),last=event==='control_prepare_timeout'?lastControlPrepareTimeoutLogAt:lastControlPrepareExpiredLogAt;if(now-last<1000)return;if(event==='control_prepare_timeout')lastControlPrepareTimeoutLogAt=now;else lastControlPrepareExpiredLogAt=now;clientLog(event,details)}
function hasTurnServer(){return iceServers.some(server=>String(server.urls||'').includes('turn:')||(Array.isArray(server.urls)&&server.urls.some(url=>String(url).startsWith('turn:'))))}
function safeIceEndpoint(value){const match=String(value||'').match(/^([a-z]+):(?:\/\/)?(?:[^@]*@)?(\[[^\]]+\]|[^:?/]+)(?::(\d+))?/i);return match?`${match[1].toLowerCase()}:${match[2]}${match[3]?`:${match[3]}`:''}`:'unknown'}
function setMetric(id,text,level=''){const element=document.getElementById(id);element.textContent=text;element.classList.remove('ok','warn','critical');if(level)element.classList.add(level)}
function formatMetric(value,digits=1,suffix=''){return value!==null&&value!==undefined&&Number.isFinite(Number(value))?`${Number(value).toFixed(digits)}${suffix}`:'未知'}
function setCanValue(id,text){const element=document.getElementById(id);if(element)element.textContent=text}
function validCanValue(values,valid,index,digits,unit=''){return Array.isArray(values)&&Array.isArray(valid)&&valid[index]&&Number.isFinite(Number(values[index]))?`${Number(values[index]).toFixed(digits)}${unit}`:'—'}
function ensureCanFeedbackCards(){
  if(!wheelFeedbackGrid.childElementCount)for(let index=0;index<8;index++){const card=document.createElement('article');card.className='wheel-feedback';const title=document.createElement('div');title.className='feedback-title';const name=document.createElement('strong');name.textContent=`轮端 ${index+1}`;const mode=document.createElement('span');mode.id=`can-wheel-mode-${index}`;mode.textContent='M— / B—';title.append(name,mode);const values=document.createElement('div');values.className='feedback-values';for(const [label,key] of [['扭矩','torque'],['转速','speed'],['制动压力','brake']]){const item=document.createElement('div');item.className='feedback-value';const caption=document.createElement('span');caption.textContent=label;const value=document.createElement('strong');value.id=`can-wheel-${key}-${index}`;value.textContent='—';item.append(caption,value);values.appendChild(item)}card.append(title,values);wheelFeedbackGrid.appendChild(card)}
  if(!steeringFeedbackGrid.childElementCount)for(let index=0;index<4;index++){const card=document.createElement('article');card.className='steering-feedback';const title=document.createElement('div');title.className='feedback-title';const name=document.createElement('strong');name.textContent=`转向轴 ${index+1}`;title.appendChild(name);const values=document.createElement('div');values.className='feedback-values';for(const [label,key] of [['转角','angle'],['模式','mode']]){const item=document.createElement('div');item.className='feedback-value';const caption=document.createElement('span');caption.textContent=label;const value=document.createElement('strong');value.id=`can-steering-${key}-${index}`;value.textContent='—';item.append(caption,value);values.appendChild(item)}card.append(title,values);steeringFeedbackGrid.appendChild(card)}
}
function renderCanFeedback(){
  document.dispatchEvent(new CustomEvent('mine-teleop-telemetry',{detail:authenticated?vehicleTelemetry:null}));
  canFeedbackPanel.hidden=false;
  ensureCanFeedbackCards();
  if(!authenticated){
    canFeedbackStatus.textContent='等待车端遥测';
    canFeedbackStatus.className='status-chip warn';
    for(const element of document.querySelectorAll('.can-summary strong,.feedback-value strong'))element.textContent='—';
    for(let index=0;index<8;index++)setCanValue(`can-wheel-mode-${index}`,'M— / B—');
    operatorSpeed.textContent='—';operatorActualGear.textContent='—';
    return;
  }
  const feedback=vehicleTelemetry?.can_feedback;
  const supported=Boolean(feedback?.supported);
  const sync=latestRuntimeStatus.time_sync||{},timeTrusted=(latestRuntimeStatus.signaling_available!==false||latestRuntimeStatus.native_control?.control_transport?.direct_authority?.continuation_available===true)&&sync.synchronized===true&&Number(sync.uncertainty_ms)<=consoleConfig.max_time_sync_uncertainty_ms;
  const freshness=controlLogic.telemetryFreshness(vehicleTelemetry,vehicleTelemetryReceivedAt,performance.now(),timeTrusted?Date.now()+Number(sync.offset_ms||0):null),canFeedbackAge=supported&&typeof feedback.max_feedback_age_ms==='number'&&Number.isFinite(feedback.max_feedback_age_ms)&&feedback.max_feedback_age_ms>=0?feedback.max_feedback_age_ms:null;
  const feedbackFresh=supported&&Boolean(feedback.feedback_fresh)&&freshness.fresh;
  const healthKey=`${Boolean(vehicleTelemetry)}:${feedbackFresh}:${controlChannel?.readyState||'none'}`,healthLogAt=performance.now();
  if(authenticated&&(healthKey!==lastFeedbackHealthKey||lastFeedbackHealthLogAt===null||healthLogAt-lastFeedbackHealthLogAt>=5000)){lastFeedbackHealthKey=healthKey;lastFeedbackHealthLogAt=healthLogAt;clientLog('driver_feedback_health',{telemetry_seq:vehicleTelemetry?.seq??null,control_status_seq:vehicleTelemetry?.control_status_seq??null,telemetry_sent_at_utc_ms:vehicleTelemetry?.sent_at_utc_ms??null,received_age_ms:freshness.received_age_ms,source_age_ms:freshness.source_age_ms,source_clock_synchronized:timeTrusted,can_feedback_age_ms:canFeedbackAge,can_feedback_fresh:Boolean(feedback?.feedback_fresh),feedback_fresh:feedbackFresh,data_channel_ready_state:controlChannel?.readyState||'none',peer_connection_state:peer?.connectionState||'none',vcu_issue_code:vcuHandshake.issue_code||''});}
  canFeedbackStatus.textContent=!vehicleTelemetry?'等待车端遥测':(supported?`序号 ${vehicleTelemetry.seq??'—'}`:'车端桥接包无完整反馈');
  canFeedbackStatus.className=feedbackFresh?'status-chip ok':'status-chip warn';
  const telemetrySpeed=vehicleTelemetry?.speed_mps,measuredSpeed=supported&&feedback.speed_valid?Number(feedback.speed_mps):(telemetrySpeed!==null&&Number.isFinite(Number(telemetrySpeed))?Number(telemetrySpeed):null),speedText=measuredSpeed===null?'—':`${measuredSpeed.toFixed(2)} m/s · ${(measuredSpeed*3.6).toFixed(1)} km/h`;
  const gearText=supported&&feedback.gear_valid?vcuGearLabel(feedback.gear):(typeof vehicleTelemetry?.gear==='string'&&vehicleTelemetry.gear?vehicleTelemetry.gear:'—');
  canSpeed.textContent=speedText;operatorSpeed.textContent=speedText;
  canGear.textContent=gearText;operatorActualGear.textContent=gearText;
  canSelector.textContent=supported&&feedback.driver_gear_request_valid?vcuGearLabel(feedback.driver_gear_request):'—';
  canEpb.textContent=supported&&Array.isArray(feedback.parking_brake_status)?feedback.parking_brake_status.map((value,index)=>feedback.parking_brake_valid?.[index]?vcuEpbLabel(value):'—').join('/'):'—';
  canHandshake.textContent=supported&&feedback.handshake_valid?String(feedback.handshake_status):'—';
  canVmcFault.textContent=supported&&feedback.vmc_fault_code_valid?String(feedback.vmc_fault_code):'—';
  canVmcFault.className=supported&&feedback.vmc_fault_code_valid&&Number(feedback.vmc_fault_code)!==0?'critical':'';
  canParkingSwitch.textContent=supported&&feedback.parking_brake_switch_valid?vcuSwitchLabel(feedback.parking_brake_switch,'已拉起','已松开'):'—';
  canBrakePedal.textContent=supported&&feedback.brake_pedal_switch_valid?vcuSwitchLabel(feedback.brake_pedal_switch,'已踩下','已松开'):'—';
  canEmergency.textContent=supported&&feedback.gear_valid?String(feedback.emergency_switch??'—'):'—';
  canAge.textContent=!supported?'—':(`${canFeedbackAge===null?'CAN 未完整':`CAN ${canFeedbackAge} ms`} · ${freshness.received_age_ms===null?'尚无遥测':`遥测 ${Math.round(freshness.received_age_ms)} ms 前收到`}`);
  canAge.className=feedbackFresh?'ok':'warn';
  for(let index=0;index<8;index++){setCanValue(`can-wheel-torque-${index}`,validCanValue(feedback?.motor_torque_nm,feedback?.motor_torque_valid,index,1,' Nm'));setCanValue(`can-wheel-speed-${index}`,validCanValue(feedback?.motor_speed_rpm,feedback?.motor_speed_valid,index,0,' rpm'));setCanValue(`can-wheel-brake-${index}`,validCanValue(feedback?.brake_pressure_bar,feedback?.brake_valid,index,1,' bar'));const motorMode=feedback?.motor_mode_valid?.[index]?feedback.motor_mode[index]:'—',brakeMode=feedback?.brake_valid?.[index]?feedback.brake_mode[index]:'—';setCanValue(`can-wheel-mode-${index}`,`M${motorMode} / B${brakeMode}`)}
  for(let index=0;index<4;index++){setCanValue(`can-steering-angle-${index}`,validCanValue(feedback?.steering_angle_deg,feedback?.steering_valid,index,1,'°'));setCanValue(`can-steering-mode-${index}`,feedback?.steering_valid?.[index]?String(feedback.steering_mode[index]):'—')}
}
function vcuGearLabel(value){const labels={1:'N',2:'R',3:'D'};return labels[Number(value)]||`不支持(${String(value)})`}
function vcuEpbLabel(value){const labels={0:'保持',1:'释放',2:'已拉起'};return labels[Number(value)]||`异常(${String(value)})`}
function vcuSwitchLabel(value,activeLabel,inactiveLabel){const number=Number(value);if(number===1)return activeLabel;if(number===0)return inactiveLabel;return`异常(${String(value)})`}
function vcuAdapterReady(status,explicit){return controlLogic.adapterReady(status,explicit)}
function vcuDrivingReady(){return controlProfileState.acknowledged&&controlLogic.drivingReady(vcuHandshake)}
function diagnoseVcuHandshake(channelOpen){
  const controlIssue=controlLogic.vehicleControlDiagnostic(vcuHandshake.issue_code,vcuHandshake.control_recovery);if(controlIssue)return controlIssue;
  if(!channelOpen)return{level:'warn',text:'连接步骤未完成：控制 DataChannel 尚未连接。'};
  if(vcuHandshake.handshake_revoked){const vmc=vcuHandshake.vmc_fault_code_valid?String(vcuHandshake.vmc_fault_code):'未知',epb=Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status.map((value,index)=>vcuHandshake.epb_valid?.[index]?vcuEpbLabel(value):'—').join('/'):'未知';return{level:'critical',text:`握手已被 VCU 撤销（状态 5→${String(vcuHandshake.revoked_handshake_status??vcuHandshake.handshake_status??3)}）；VMC 故障码 ${vmc}，电子驻车 ${epb}。车端正在安全退出，完成后请从页面重新申请 VCU 握手。`};}
  if(!controlProfileState.acknowledged)return{level:'warn',text:controlProfileState.pendingRequestSeq?`连接步骤未完成：等待车端确认会话控制参数序号 ${controlProfileState.pendingRequestSeq}。`:'连接步骤未完成：尚未发送或确认会话控制参数。'};
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
function renderVcuHandshake(){const labels={unavailable:'等待车端状态',unsupported:'当前适配器不支持 VCU 握手',closed:'车端适配器已关闭',standby:'待机（未请求）',initial:'启动 1/5 · 低握手帧',wait_parallel_handshake:'启动 2/5 · 智驾状态 5',wait_parking_brake_released:'启动 3/5 · 等待电子驻车释放',wait_gear:'启动 4/5 · 挡位闭环（零牵引）',wait_actuator_modes:'启动 5/5 · 执行器模式（零牵引）',ready:'握手成功（平行驾驶）',disarm_torque:'退出 1/5 · 扭矩归零',disarm_stop:'退出 2/5 · 车辆零速',disarm_neutral:'退出 3/5 · N 挡',disarm_parking_brake:'退出 4/5 · 电子驻车',disarm_manual:'退出 5/5 · 人工状态 3',disarmed:'车端报告退出协议完成',fault:'VCU 通讯故障'};const channelOpen=Boolean(controlChannel&&controlChannel.readyState==='open'),supported=Boolean(vcuHandshake.supported),ready=Boolean(vcuHandshake.ready),disarming=Boolean(vcuHandshake.disarming),requested=Boolean(vcuHandshake.requested),adapterReady=vcuHandshake.adapter_ready===true,diagnostic=diagnoseVcuHandshake(channelOpen),vmcSuffix=vcuHandshake.vmc_fault_code_valid&&Number(vcuHandshake.vmc_fault_code)!==0?` · VMC ${vcuHandshake.vmc_fault_code}`:'';vcuStatus.textContent=(controlLogic.vehicleControlDiagnostic(vcuHandshake.issue_code,vcuHandshake.control_recovery)?.title||labels[vcuHandshake.state]||vcuHandshake.state||'未知')+vmcSuffix;vcuStatus.className=ready&&adapterReady&&controlProfileState.acknowledged?'ok':(diagnostic.level==='critical'?'critical':'warn');vcuGate.textContent=diagnostic.text;vcuGate.className=`gate-copy ${diagnostic.level}`;const selector=vcuHandshake.driver_gear_request_valid?vcuGearLabel(vcuHandshake.driver_gear_request):'未知',speed=vcuHandshake.speed_valid?`${Number(vcuHandshake.speed_mps).toFixed(2)} m/s`:'未知',manual=vcuHandshake.handshake_valid?String(vcuHandshake.handshake_status):'未知',epb=Array.isArray(vcuHandshake.epb_status)?vcuHandshake.epb_status.map(vcuEpbLabel).join('/'):'未知',vmc=vcuHandshake.vmc_fault_code_valid?String(vcuHandshake.vmc_fault_code):'未知',parkingSwitch=vcuHandshake.parking_brake_switch_valid?vcuSwitchLabel(vcuHandshake.parking_brake_switch,'已拉起','已松开'):'未知',brakePedal=vcuHandshake.brake_pedal_switch_valid?vcuSwitchLabel(vcuHandshake.brake_pedal_switch,'已踩下','已松开'):'未知';vcuGate.title=`控制链路 ${channelOpen?'已连接':'未连接'} · 参数 ${controlProfileState.acknowledged?'已确认':'未确认'} · 选择器 ${selector} · 车速 ${speed} · 电子驻车 ${epb} · VCU状态 ${manual} · VMC故障码 ${vmc} · 物理手刹 ${parkingSwitch} · 制动踏板 ${brakePedal}`;vcuConnectButton.disabled=!channelOpen||!controlProfileState.acknowledged||!adapterReady||!supported||!vcuHandshake.parking_ready||requested||ready||disarming;vcuDisconnectButton.disabled=!channelOpen||!adapterReady||!supported||(!requested&&!ready&&!disarming)}
function renderMonitoring(){const estopPresentation=controlLogic.deriveControlStopPresentation(estopLatched,Boolean(vehicleTelemetry&&vehicleTelemetry.estop===true),(vehicleTelemetry?vehicleTelemetry.stop_source:undefined),(vehicleTelemetry?vehicleTelemetry.stop_reason:undefined),vcuHandshake);renderEstopRequest(estopPresentation);const recoveryPresentation=renderControlRecovery(estopPresentation.visible);renderVcuHandshake();renderCanFeedback();renderControlState();const recording=renderRecordingStatus();if(!authenticated){monitorPanel.hidden=true;return}monitorPanel.hidden=false;const runtime=latestRuntimeStatus||{},metrics=latestMetrics||{streams:[]},vehicles=runtime.authorized_vehicles||[],selected=vehicles.find(v=>v.vehicle_id===(runtime.vehicle_id||vehicleSelect.value));setMetric('metric-vehicle',selected?(selected.online?`${selected.vehicle_id} 在线`:`${selected.vehicle_id} 离线`):'未知',(selected&&selected.online)?'ok':'warn');setMetric('metric-session',runtime.connected?`${runtime.session_id||'活动'} · ${metrics.connection_state||'等待媒体'}`:'未连接',runtime.connected?'ok':'warn');const authority=runtime.connected&&!controlAuthorityLost;setMetric('metric-authority',authority?'已获得':'无',authority?'ok':(controlAuthorityLost?'critical':'warn'));const codec=metrics.codec||mediaStatus.codec||'',backend=metrics.backend||mediaStatus.backend||'';setMetric('metric-video',codec||backend?`${codec||'未知'} / ${backend||'未知'}`:'等待媒体',codec?'ok':'warn');const latency=controlLogic.controlLatencyPresentation(metrics,runtime,nativeControlSessionId,nativeControlSessionGeneration);setMetric('metric-rtt',latency.media.text,latency.media.level);setMetric('metric-cloud-ack',latency.cloud.text,latency.cloud.level);setMetric('metric-vehicle-result',latency.vehicle.text,latency.vehicle.level);setMetric('metric-vehicle-accepted',latency.accepted.text,latency.accepted.level);setMetric('metric-network',metrics.connection_method||'未知',metrics.connection_method==='TURN'?'warn':(metrics.connection_method&&metrics.connection_method!=='unknown'?'ok':'warn'));const turnConfigured=Boolean((metrics.turn_configured===null||metrics.turn_configured===undefined?hasTurnServer():metrics.turn_configured));setMetric('metric-turn',metrics.turn_in_use?'正在中继':(turnConfigured?'已配置，未使用':'未配置'),metrics.turn_in_use?'warn':(turnConfigured?'ok':'warn'));const sync=runtime.time_sync||metrics.time_sync||{},timeTrusted=(runtime.signaling_available!==false||runtime.native_control?.control_transport?.direct_authority?.continuation_available===true)&&Boolean(sync.synchronized)&&Number(sync.uncertainty_ms)<=consoleConfig.max_time_sync_uncertainty_ms;setMetric('metric-time',timeTrusted?`可信 ±${sync.uncertainty_ms} ms`:`不可信${Number.isFinite(Number(sync.uncertainty_ms))?` ±${sync.uncertainty_ms} ms`:''}`,timeTrusted?'ok':'critical');streamMetrics.replaceChildren();const streams=metrics.streams||[];if(!streams.length){const row=document.createElement('tr');const cell=document.createElement('td');cell.colSpan=5;cell.className='muted';cell.textContent='等待视频轨道';row.appendChild(cell);streamMetrics.appendChild(row)}for(const stream of streams){const row=document.createElement('tr');const loss=Number(stream.packet_loss_percent||0),fps=Number(stream.fps||0),latency=(stream.estimated_end_to_end_latency_ms===null||stream.estimated_end_to_end_latency_ms===undefined?null:stream.estimated_end_to_end_latency_ms);for(const [text,level] of [[MineTeleopCameraView.cameraLabel(stream.camera_id||stream.mid||'unknown'),''],[formatMetric(fps,1),fps<20?'critical':'ok'],[formatMetric(stream.bitrate_kbps,0,' kbps'),''],[formatMetric(loss,2,'%'),loss>2?'warn':''],[formatMetric(latency,1,' ms'),latency===null?'warn':latency>200?'critical':'ok']]){const cell=document.createElement('td');cell.textContent=text;if(level)cell.className=level;row.appendChild(cell)}streamMetrics.appendChild(row)}const alerts=[];let severity='';if(estopPresentation.visible){alerts.push(estopPresentation.alert);severity=estopPresentation.severity}else if(recoveryPresentation.visible){alerts.push(recoveryPresentation.alert);severity='warn'}const mediaDiagnostic=controlLogic.mediaScopeDiagnostic(mediaStatus.local_scope_issue||mediaStatus.media_negotiation?.issue_code);if(mediaDiagnostic){alerts.push(mediaDiagnostic.text);if(!severity)severity='warn'}if(controlAuthorityLost){alerts.push('控制权或信令已丢失，当前页面不会继续发送驾驶命令');severity='critical'}else if(runtime.connected&&(!controlChannel||controlChannel.readyState!=='open')){alerts.push('控制 DataChannel 尚未就绪');if(!severity)severity='warn'}if((vcuHandshake.supported&&!vcuHandshake.ready)||controlLogic.vehicleControlDiagnostic(vcuHandshake.issue_code,vcuHandshake.control_recovery)){const diagnostic=diagnoseVcuHandshake(Boolean(controlChannel&&controlChannel.readyState==='open'));alerts.push(diagnostic.text);if(diagnostic.level==='critical')severity='critical';else if(!severity)severity='warn'}if(!timeTrusted){alerts.push('时间同步不可信，端到端时延只作参考');severity='critical'}for(const stream of streams){if(Number(stream.estimated_end_to_end_latency_ms)>200){alerts.push(`${stream.camera_id||'视频'} 时延超过 200 ms`);severity='critical'}if(Number(stream.fps)<20){alerts.push(`${stream.camera_id||'视频'} 低于 20 FPS`);severity='critical'}}for(const message of recording.alerts)alerts.push(message);if(recording.alerts.length&&!severity)severity='warn';if(!alerts.length)alerts.push(streams.length?'当前指标在目标范围内':'尚无媒体指标；控制命令不会在链路未就绪时发送');alertsPanel.textContent=alerts.join('；');alertsPanel.className=`alerts ${severity}`.trim();const alertKey=`${severity}:${alerts.join('|')}`;if(alertKey!==lastAlertKey){clientLog('control_monitor_state',{severity:severity||'ok',alerts});lastAlertKey=alertKey}}
async function refreshRuntimeStatus(){if(!authenticated)return;try{latestRuntimeStatus=await get('/api/status');if(connecting&&latestRuntimeStatus.pending_mobile_approval?.request_id){webrtcLabel.textContent='等待手机确认';statusPanel.textContent=`车辆 ${latestRuntimeStatus.pending_mobile_approval.vehicle_id} 等待手机确认，同意后自动连接；可点击安全退出取消。`}if(polling&&!latestRuntimeStatus.connected){closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='控制权丢失'}renderMonitoring()}catch(error){controlAuthorityLost=true;resetControlAuthorityInput();webrtcLabel.textContent='本地状态读取失败';alertsPanel.textContent='无法读取本地运行状态: '+error.message;alertsPanel.className='alerts critical'}}
function clamp(value,min,max){return Math.min(max,Math.max(min,value))}
function resetControlProfileSession(){controlProfileGeneration+=1;controlProfileState={requestedProfile:null,pendingRequestSeq:0,effectiveProfile:null,effectiveRequestSeq:0,effectiveAppliedRevision:0,acknowledged:false,reason:''};pendingControlProfileEnvelope=null;lastControlProfileSendAt=0;controlProfilePrepareInFlight=false;vehicleHardLimits={received:false}}
function effectiveControlLimits(){const profile=controlProfileState.acknowledged?controlProfileState.effectiveProfile:null;if(!profile||!vehicleHardLimits.received)return{maxThrottle:0,maxBrakePressureBar:0,serviceBrakePressureBar:0,hardBrakePressureBar:0,maxSteeringDeg:0};return{maxThrottle:controlLogic.controlProfileThrottleLimit(profile,vehicleHardLimits),maxBrakePressureBar:profile.max_brake_pressure_bar,serviceBrakePressureBar:profile.service_brake_pressure_bar,hardBrakePressureBar:profile.hard_brake_pressure_bar,maxSteeringDeg:Math.min(profile.max_steering_angle_deg,vehicleHardLimits.max_steering_angle_deg)}}
function readOnlyControlSafetyText(safety){const stages=safety.deceleration_profile.map(stage=>`${stage.after_ms}ms:${stage.brake}`).join('/');return`固定安全：upstream rate ${safety.control_rate_hz} Hz · command gap ${safety.max_command_gap_ms} ms · watchdog ${safety.degraded_timeout_ms}/${safety.control_timeout_ms} ms · decel ${stages} · speed feedback ${safety.speed_feedback_timeout_ms} ms · overspeed margin ${safety.hard_overspeed_margin_kph} km/h · gates CAN=${safety.require_can_feedback_before_control}, ESTOP reset=${safety.require_local_estop_reset}, time sync=${safety.require_time_sync} (±${safety.max_time_sync_uncertainty_ms} ms / ${safety.time_sync_interval_ms} ms / ${safety.time_sync_samples} samples) · mode ${safety.commissioning_mode}`}
function renderControlLimits(){const profile=controlProfileState.effectiveProfile;if(controlProfileState.pendingRequestSeq)controlLimitsSummary.textContent=`等待车端确认参数序号 ${controlProfileState.pendingRequestSeq}`;else if(!controlProfileState.acknowledged||!profile)controlLimitsSummary.textContent='未获得车端会话参数确认（需人工打开并发送）';else controlLimitsSummary.textContent=`目标 ${profile.target_speed_kph.toFixed(1)} km/h · 迟到阈值 ${profile.consecutive_late_frame_limit} 帧 · 视频制动 ${profile.critical_camera_stale_brake_ms} ms · 控制恢复等待 ${profile.control_recovery_timeout_ms/1000}s · 自动驻车 ${profile.parking_idle_timeout_ms/1000}s · 单电机 ${profile.max_motor_torque_nm.toFixed(1)} Nm · EHB ${profile.service_brake_pressure_bar.toFixed(1)}/${profile.hard_brake_pressure_bar.toFixed(1)}/${profile.max_brake_pressure_bar.toFixed(1)} bar · 转向 ≤${profile.max_steering_angle_deg.toFixed(1)}° · PID ${profile.speed_pid_kp.toFixed(2)}/${profile.speed_pid_ki.toFixed(2)}/${profile.speed_pid_kd.toFixed(2)} · 升扭 ${profile.motor_torque_rise_rate_nm_per_s.toFixed(0)} Nm/s · rev ${controlProfileState.effectiveAppliedRevision}`;controlLimitsSummary.className=controlProfileState.acknowledged&&!controlProfileState.pendingRequestSeq?'ok':'warn';controlLimitsOpen.disabled=!vehicleHardLimits.received||!controlProfileState.requestedProfile;if(vehicleHardLimits.received){const pid=vehicleHardLimits.speed_pid_limits;targetSpeedKph.max=String(vehicleHardLimits.max_target_speed_kph);maxMotorTorqueNm.max=String(vehicleHardLimits.full_scale_motor_torque_nm);maxBrakePressureBar.max=String(vehicleHardLimits.max_brake_pressure_bar);serviceBrakePressureBar.max=String(vehicleHardLimits.max_brake_pressure_bar);hardBrakePressureBar.max=String(vehicleHardLimits.max_brake_pressure_bar);maxSteeringDeg.max=String(vehicleHardLimits.max_steering_angle_deg);speedPidKp.min=String(pid.kp.min);speedPidKp.max=String(pid.kp.max);speedPidKi.min=String(pid.ki.min);speedPidKi.max=String(pid.ki.max);speedPidKd.min=String(pid.kd.min);speedPidKd.max=String(pid.kd.max);speedPidDerivativeFilterTauMs.min=String(pid.derivative_filter_tau_ms.min);speedPidDerivativeFilterTauMs.max=String(pid.derivative_filter_tau_ms.max);speedPidMaxDtMs.min=String(pid.max_dt_ms.min);speedPidMaxDtMs.max=String(pid.max_dt_ms.max);motorTorqueRiseRate.min=String(vehicleHardLimits.motor_torque_rise_rate_limits_nm_per_s.min);motorTorqueRiseRate.max=String(vehicleHardLimits.motor_torque_rise_rate_limits_nm_per_s.max)}vehicleHardLimitsLabel.textContent=vehicleHardLimits.received?`车端只读硬上限：max speed ${vehicleHardLimits.max_speed_kph.toFixed(1)} km/h × max throttle ${vehicleHardLimits.max_throttle.toFixed(3)} = 目标 ${vehicleHardLimits.max_target_speed_kph.toFixed(1)} km/h · 单电机 ${vehicleHardLimits.full_scale_motor_torque_nm.toFixed(1)} Nm · 每路 EHB 普通压力 ${vehicleHardLimits.max_brake_pressure_bar.toFixed(1)} bar · 转向 ${vehicleHardLimits.max_steering_angle_deg.toFixed(1)}° · 速度反馈超时 ${vehicleHardLimits.speed_feedback_timeout_ms} ms · 硬超速余量 ${vehicleHardLimits.hard_overspeed_margin_kph} km/h。车端 PID 默认 Kp/Ki/Kd=${vehicleHardLimits.default_speed_pid_kp}/${vehicleHardLimits.default_speed_pid_ki}/${vehicleHardLimits.default_speed_pid_kd}，τ=${vehicleHardLimits.default_speed_pid_derivative_filter_tau_ms} ms，max dt=${vehicleHardLimits.default_speed_pid_max_dt_ms} ms，升扭斜率默认 ${vehicleHardLimits.default_motor_torque_rise_rate_nm_per_s} Nm/s。${readOnlyControlSafetyText(vehicleHardLimits.read_only_control_safety)}。以上硬安全制动与 watchdog 参数不可编辑。`:'等待车端完整硬上限、PID 默认值与固定安全参数；普通驾驶保持禁用'}
function sendPendingControlProfile(force=false){if(controlAuthorityLost||!pendingControlProfileEnvelope||!controlProfileState.pendingRequestSeq||controlProfileState.acknowledged||Number(pendingControlProfileEnvelope.seq)!==Number(controlProfileState.pendingRequestSeq)||((!peer||peer.connectionState!=='connected'||!controlChannel||controlChannel.readyState!=='open')&&!controlTransportCourier.negotiated()))return false;const now=Date.now();if(!force&&now-lastControlProfileSendAt<200)return false;sendControlManagement(pendingControlProfileEnvelope).catch(error=>clientLog('control_profile_send_failed',{error:error.message}));lastControlProfileSendAt=now;return true}
async function prepareControlProfile(value,announce=true){if(controlProfilePrepareInFlight)throw Error('已有会话控制参数正在准备');if(!vehicleHardLimits.received)throw Error('尚未收到车端完整硬上限与 PID 默认值');const requested=controlLogic.normalizeControlProfile(value),bounded=controlLogic.mergeControlProfileWithHardLimits(requested,vehicleHardLimits);if(!controlLogic.controlProfilesEqual(requested,bounded))throw Error('请求超出当前车辆硬上限');const activePeer=peer,activeChannel=controlChannel,activeProfileGeneration=controlProfileGeneration;if((!activePeer||activePeer.connectionState!=='connected'||!activeChannel||activeChannel.readyState!=='open')&&!controlTransportCourier.negotiated())throw Error('控制传输尚未连接');clearControlInput(false);controlProfilePrepareInFlight=true;try{const prepared=await post('/api/control-profile',requested),requestSeq=Number(prepared?.request?.seq);if(!Number.isSafeInteger(requestSeq)||requestSeq<=0)throw Error('控制端未生成有效参数序号');if(controlProfileGeneration!==activeProfileGeneration||controlAuthorityLost||peer!==activePeer||controlChannel!==activeChannel||((activePeer?.connectionState!=='connected'||activeChannel?.readyState!=='open')&&!controlTransportCourier.negotiated()))throw Error('准备参数期间控制链路已变化');controlProfileState={...controlProfileState,requestedProfile:requested,pendingRequestSeq:requestSeq,effectiveProfile:null,effectiveRequestSeq:0,effectiveAppliedRevision:0,acknowledged:false,reason:'pending'};pendingControlProfileEnvelope=prepared.request;lastControlProfileSendAt=0;sendPendingControlProfile(true);renderControlLimits();renderMonitoring();if(announce)statusPanel.textContent=`会话控制参数序号 ${requestSeq} 已发送，等待车端确认`;return prepared}finally{if(controlProfileGeneration===activeProfileGeneration)controlProfilePrepareInFlight=false}}
function applyControlProfileStatus(value){const wasAcknowledged=controlProfileState.acknowledged,previousRequestSeq=controlProfileState.effectiveRequestSeq,previousRevision=controlProfileState.effectiveAppliedRevision,next=controlLogic.reduceControlProfileStatus(controlProfileState,value);if(!next.matched)return false;controlProfileState=next;if(!controlProfileState.pendingRequestSeq)pendingControlProfileEnvelope=null;if(!controlProfileState.acknowledged&&(wasAcknowledged||next.invalidated))resetControlAuthorityInput('session_control_profile_invalidated');if(controlProfileState.acknowledged){if(!wasAcknowledged||previousRequestSeq!==controlProfileState.effectiveRequestSeq||previousRevision!==controlProfileState.effectiveAppliedRevision){statusPanel.textContent=`车端已确认会话控制参数序号 ${controlProfileState.effectiveRequestSeq} / revision ${controlProfileState.effectiveAppliedRevision}`;clientLog('session_control_profile_accepted',{request_seq:controlProfileState.effectiveRequestSeq,applied_revision:controlProfileState.effectiveAppliedRevision,effective_profile:controlProfileState.effectiveProfile,reason:controlProfileState.reason})}}else{statusPanel.textContent=`车端会话控制参数无效：${controlProfileState.reason}`;clientLog('session_control_profile_invalidated',{reason:controlProfileState.reason})}renderControlLimits();renderMonitoring();return true}
function controlProfileParkingReady(){const mockBench=vcuMockUnsupported()&&vcuHandshake.adapter_ready===true,parkedStandby=vcuHandshake.parking_ready===true&&(vcuHandshake.state==='standby'||vcuHandshake.state==='disarmed');return mockBench||parkedStandby}
function updateVehicleHardLimits(value){if(!value||typeof value!=='object')return;try{const hard=controlLogic.normalizeVehicleHardLimits(value);vehicleHardLimits={...hard,received:true};if(!controlProfileState.requestedProfile)controlProfileState={...controlProfileState,requestedProfile:controlLogic.controlProfileFromVehicleDefaults(driverActuationDefaults,hard)};renderControlLimits()}catch(error){resetControlProfileSession();resetControlAuthorityInput();renderControlLimits();renderMonitoring();statusPanel.textContent='车端控制参数不完整，驾驶权限已撤销';clientLog('vehicle_hard_limits_invalid',{error:error.message})}}
function openControlLimits(){const requested=controlProfileState.requestedProfile;if(!vehicleHardLimits.received||!requested)throw Error('尚未收到车端完整硬上限与 PID 默认值');targetSpeedKph.value=requested.target_speed_kph.toFixed(1);maxMotorTorqueNm.value=requested.max_motor_torque_nm.toFixed(1);maxBrakePressureBar.value=requested.max_brake_pressure_bar.toFixed(1);serviceBrakePressureBar.value=requested.service_brake_pressure_bar.toFixed(1);hardBrakePressureBar.value=requested.hard_brake_pressure_bar.toFixed(1);maxSteeringDeg.value=requested.max_steering_angle_deg.toFixed(1);speedPidKp.value=requested.speed_pid_kp;speedPidKi.value=requested.speed_pid_ki;speedPidKd.value=requested.speed_pid_kd;speedPidDerivativeFilterTauMs.value=requested.speed_pid_derivative_filter_tau_ms;speedPidMaxDtMs.value=requested.speed_pid_max_dt_ms;motorTorqueRiseRate.value=requested.motor_torque_rise_rate_nm_per_s;parkingIdleTimeout.value=requested.parking_idle_timeout_ms/1000;consecutiveLateFrameLimit.value=requested.consecutive_late_frame_limit;criticalCameraStaleBrake.value=requested.critical_camera_stale_brake_ms;controlRecoveryTimeout.value=requested.control_recovery_timeout_ms/1000;controlLimitsConfirm.checked=false;controlLimitsApply.disabled=true;renderControlLimits();controlLimitsDialog.showModal()}
async function applyControlLimits(){if(!controlLimitsConfirm.checked)throw Error('请先确认停车或隔离台架条件');if(!vehicleHardLimits.received)throw Error('尚未收到车端完整硬上限与 PID 默认值');if(!consecutiveLateFrameLimit.checkValidity())throw Error('连续迟到帧数必须为 1–5 的整数');if(!criticalCameraStaleBrake.checkValidity()||!controlRecoveryTimeout.checkValidity())throw Error('请填写允许范围内的视频制动与恢复等待门限');if(!parkingIdleTimeout.checkValidity())throw Error('自动驻车延时必须在 0–1 秒之间');if(!motorTorqueRiseRate.checkValidity())throw Error('请填写车端允许范围内的升扭斜率；0 表示取消升扭限制');const requested=controlLogic.normalizeControlProfile({profile_version:7,consecutive_late_frame_limit:consecutiveLateFrameLimit.valueAsNumber,critical_camera_stale_brake_ms:criticalCameraStaleBrake.valueAsNumber,control_recovery_timeout_ms:Math.round(controlRecoveryTimeout.valueAsNumber*1000),parking_idle_timeout_ms:Math.round(parkingIdleTimeout.valueAsNumber*1000),target_speed_kph:Number(targetSpeedKph.value),max_motor_torque_nm:Number(maxMotorTorqueNm.value),max_brake_pressure_bar:Number(maxBrakePressureBar.value),service_brake_pressure_bar:Number(serviceBrakePressureBar.value),hard_brake_pressure_bar:Number(hardBrakePressureBar.value),max_steering_angle_deg:Number(maxSteeringDeg.value),speed_pid_kp:Number(speedPidKp.value),speed_pid_ki:Number(speedPidKi.value),speed_pid_kd:Number(speedPidKd.value),speed_pid_derivative_filter_tau_ms:Number(speedPidDerivativeFilterTauMs.value),speed_pid_max_dt_ms:Number(speedPidMaxDtMs.value),motor_torque_rise_rate_nm_per_s:motorTorqueRiseRate.valueAsNumber}),prior=controlProfileState.effectiveProfile||controlProfileState.requestedProfile,pidChanged=!prior||requested.consecutive_late_frame_limit!==prior.consecutive_late_frame_limit||requested.parking_idle_timeout_ms!==prior.parking_idle_timeout_ms||requested.critical_camera_stale_brake_ms!==prior.critical_camera_stale_brake_ms||requested.control_recovery_timeout_ms!==prior.control_recovery_timeout_ms||requested.speed_pid_kp!==prior.speed_pid_kp||requested.speed_pid_ki!==prior.speed_pid_ki||requested.speed_pid_kd!==prior.speed_pid_kd||requested.speed_pid_derivative_filter_tau_ms!==prior.speed_pid_derivative_filter_tau_ms||requested.speed_pid_max_dt_ms!==prior.speed_pid_max_dt_ms||requested.motor_torque_rise_rate_nm_per_s!==prior.motor_torque_rise_rate_nm_per_s,requiresParking=!controlProfileState.effectiveProfile||pidChanged||requested.target_speed_kph>prior.target_speed_kph||requested.max_motor_torque_nm>prior.max_motor_torque_nm||requested.max_brake_pressure_bar!==prior.max_brake_pressure_bar||requested.service_brake_pressure_bar!==prior.service_brake_pressure_bar||requested.hard_brake_pressure_bar!==prior.hard_brake_pressure_bar||requested.max_steering_angle_deg!==prior.max_steering_angle_deg;if(requiresParking&&!controlProfileParkingReady())throw Error('首次应用、连续迟到帧数、视频保护门限、自动驻车延时、任一 PID 或升扭斜率修改、提高目标车速/转矩、修改转向上限或制动压力，都需要 N 挡、零速、电子驻车且 VCU 为 standby/disarmed，或隔离 mock 台架');await prepareControlProfile(requested);controlLimitsDialog.close();clientLog('driver_control_profile_requested',{requested_profile:requested,vehicle_hard_limits:vehicleHardLimits})}
function applyDeadzone(value){const magnitude=Math.abs(value),deadzone=gamepadConfig.axis_deadzone;if(magnitude<=deadzone)return 0;return Math.sign(value)*(magnitude-deadzone)/(1-deadzone)}
)HTML" + R"HTML(
function applyPedalDeadzone(value){const deadzone=gamepadConfig.axis_deadzone;return value<=deadzone?0:(value-deadzone)/(1-deadzone)}
function axisValue(pad,index){return Number.isInteger(index)&&index>=0&&index<pad.axes.length&&Number.isFinite(pad.axes[index])?pad.axes[index]:null}
function buttonValue(pad,index){return Number.isInteger(index)&&index>=0&&index<pad.buttons.length?Number(pad.buttons[index].value||0):0}
function syncControlKeyState(){state=controlLogic.deriveKeyState(pressedControlKeys)}
function vcuMockUnsupported(value=vcuHandshake){return controlLogic.mockUnsupported(value)}
function vcuAllowsGearChange(requestedGear){return controlLogic.allowsGearChange(selectedGear,requestedGear,vcuHandshake)}
function updateSelectedGearFromInput(inputState){const next=controlLogic.deriveGearSelection(selectedGear,inputState,vcuHandshake);if(next.changed&&pendingGearTransition){const requestedGear=next.selectedGear;clearControlInput(false);pendingGearRequest=requestedGear;statusPanel.textContent=`${pendingGearTransition.fromGear}→${pendingGearTransition.toGear} 换挡尚未获得车端反馈；已阻止新的 ${requestedGear} 挡请求，请释放后重新操作`;return{selectedGear,pendingGearRequest,changed:false}}if(next.changed)pendingGearTransition=controlLogic.createGearTransition(selectedGear,next.selectedGear,lastControlStatusSeq,++gearTransitionGeneration);selectedGear=next.selectedGear;pendingGearRequest=next.pendingGearRequest;if(pendingGearRequest)statusPanel.textContent=`${selectedGear}→${pendingGearRequest} 换挡已阻止：需至少 3 帧且持续 200 ms 的新鲜零速反馈；请停车后释放并重新按下方向键`;return next}
function updateSelectedGearFromHeldDirections(){return updateSelectedGearFromInput(state)}
function controlInputDiagnostic(){return{pressed_codes:[...pressedControlKeys],blocked_codes:[...blockedControlKeys],selected_gear:selectedGear,vcu_state:vcuHandshake.state||null,vehicle_safety_state:lastVehicleSafetyState||null,control_profile_acknowledged:Boolean(controlProfileState.acknowledged),gamepad_steering:gamepadState.steering,gamepad_throttle:gamepadState.throttle,gamepad_brake:gamepadState.brake,document_has_focus:document.hasFocus(),document_hidden:document.hidden}}
function clearControlInput(resetGear=true,reason='control_input_reset',preserveBrake=false){const prior=controlInputDiagnostic(),hadInput=prior.pressed_codes.length||prior.gamepad_steering||prior.gamepad_throttle||prior.gamepad_brake;controlLogic.blockAndClearKeys(pressedControlKeys,blockedControlKeys,preserveBrake);syncControlKeyState();gamepadState.steering=0;gamepadState.throttle=0;if(!preserveBrake)gamepadState.brake=0;gamepadRequiresNeutral=true;gamepadPreserveBrakeDuringRearm=preserveBrake;if(resetGear){selectedGear='N';pendingGearRequest=null;pendingGearTransition=null}if(hadInput)clientLog('driver_input_cleared',{reason,reset_gear:resetGear,preserve_brake:preserveBrake,...prior});renderControlState()}
function vcuStateRequiresFreshInput(value=vcuHandshake){return controlLogic.requiresFreshInput(value)}
function vcuStateKeepsHeldInput(value=vcuHandshake){return controlLogic.keepsHeldInput(vcuEverReady,value)}
function acceptControlStatusMessage(message){const decision=controlLogic.reduceStatusSequence(lastControlStatusSeq,message?.control_status_seq);if(!decision.accepted){const sequence=Number(message?.control_status_seq);clientLog('control_status_message_dropped',{event:message?.event||'unknown',control_status_seq:Number.isFinite(sequence)?sequence:null,last_control_status_seq:lastControlStatusSeq});return false}if(decision.gap>0)clientLog('control_status_sequence_gap',{event:message?.event||'unknown',control_status_seq:decision.lastSequence,last_control_status_seq:lastControlStatusSeq,missing_status_count:decision.gap});lastControlStatusSeq=decision.lastSequence;return true}
function resetControlAuthorityInput(reason='control_authority_reset'){vcuEverReady=false;clearControlInput(true,reason)}
function updateVcuHandshakeState(value){const locked=vcuHandshake.issue_code==='control_recovery_timeout'||vcuHandshake.issue_code==='critical_camera_failed';if(locked)value={...value,issue_code:vcuHandshake.issue_code,control_recovery:vcuHandshake.control_recovery,ready:false,state:'fault'};gearChangeStationaryEvidence=controlLogic.updateGearChangeStationaryEvidence(gearChangeStationaryEvidence,value,lastControlStatusSeq,performance.now());value={...value,gear_change_stationary_confirmed:gearChangeStationaryEvidence.confirmed};const transition=controlLogic.transitionVcuState(vcuEverReady,value);vcuHandshake=value;vcuEverReady=transition.everReady;if(transition.resetInput)resetControlAuthorityInput('vcu_state_'+String(value.state||'unavailable'))}
function applyVehicleSafetyState(value,timing){const next=String(value||'');if(next==='DEGRADED'&&controlLogic.degradedInputRearmRequired(timing,lastNativeAcceptedControlSeq)){clearControlInput(false,'vehicle_degraded',true);if(lastVehicleSafetyState!=='DEGRADED'){lastKeyboardEvent.textContent='控制命令短暂中断，输入已清除 · 请释放后重新按下';statusPanel.textContent='车端进入可恢复降级：牵引已清零；请释放控制键后重新按下';clientLog('driver_input_cleared_on_degraded',{previous_safety_state:lastVehicleSafetyState||null})}}lastVehicleSafetyState=next}
function suspendSignalingPoll(){const generation=++signalingGeneration;polling=false;if(signalingPollAbort){signalingPollAbort.abort();signalingPollAbort=null}return generation}
function closeRealtimeSession(){++mediaPeerEpoch;mediaOfferScope=null;mediaHasOffer=false;mediaScopeIssue='';retiredMediaScopes.clear();controlWriteQueue.reset();flushControlTrace('session_close');controlTraceScope={session_id:'',vehicle_id:''};lastHeartbeatTraceAt=null;nativeControlSessionId='';nativeControlSessionGeneration=0;lastNativeIntentSnapshot='';nativeControlUiEpoch=0;nativeControlTakeoverPending=false;nativeControlPageRetired=false;lastNativeAcceptedControlSeq=0;const generation=suspendSignalingPoll();gearRejectionInhibited=false;resetControlAuthorityInput();resetControlProfileSession();lastControlStatusSeq=0;gearChangeStationaryEvidence=controlLogic.createGearChangeStationaryEvidence();resetControlOutcomeSession();if(controlChannel)controlChannel.close();if(peer)peer.close();controlChannel=null;peer=null;vehicleTelemetry=null;vehicleTelemetryReceivedAt=null;lastFeedbackHealthLogAt=null;lastFeedbackHealthKey='';lastVehicleSafetyState='';mediaStatus={lanes:[]};vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};pendingIce=[];remoteCameraIds=[];offeredCameraByMid.clear();cameraByMid.clear();assignedCameraIds.clear();previousStats.clear();lastMediaMetricsLogAt=null;cameraView.reset();cameraGrid.replaceChildren(emptyStage);renderMonitoring();return generation}
function renderEstopRequest(presentation=controlLogic.deriveControlStopPresentation(estopLatched,Boolean(vehicleTelemetry&&vehicleTelemetry.estop===true),(vehicleTelemetry?vehicleTelemetry.stop_source:undefined),(vehicleTelemetry?vehicleTelemetry.stop_reason:undefined),vcuHandshake)){estopStatus.hidden=!presentation.visible;estopStatus.textContent=presentation.banner}
function renderControlRecovery(hardStopVisible){const sync=latestRuntimeStatus.time_sync||{},trusted=sync.synchronized===true&&Number(sync.uncertainty_ms)<=consoleConfig.max_time_sync_uncertainty_ms,freshness=controlLogic.telemetryFreshness(vehicleTelemetry,vehicleTelemetryReceivedAt,performance.now(),trusted?Date.now()+Number(sync.offset_ms||0):null),fresh=freshness.received_age_ms!==null&&freshness.received_age_ms<=500&&(freshness.source_age_ms===null||freshness.source_age_ms<=500),presentation=controlLogic.deriveControlRecoveryPresentation(polling&&!controlAuthorityLost,vehicleTelemetry,fresh,hardStopVisible);controlRecoveryStatus.hidden=!presentation.visible;controlRecoveryStatus.textContent=presentation.banner;return presentation}
function latchEstop(source){if(estopLatched)return false;estopLatched=true;clientLog('control_estop_request_latched',{source});renderEstopRequest();renderMonitoring();return true}
function firstConnectedGamepad(){const pads=navigator.getGamepads?navigator.getGamepads():[];if(activeGamepadIndex!==null&&pads[activeGamepadIndex]?.connected)return pads[activeGamepadIndex];for(const pad of pads)if(pad?.connected){activeGamepadIndex=pad.index;return pad}activeGamepadIndex=null;return null}
function applyGamepadNeutralInterlock(authorityReady,gearRequestPending=false){const next=controlLogic.reduceGamepadNeutralInterlock({requiresNeutral:gamepadRequiresNeutral,authorityReady,throttle:gamepadState.throttle,brake:gamepadState.brake,gearRequestPending,preserveBrakeDuringRearm:gamepadPreserveBrakeDuringRearm,steering:gamepadState.steering});gamepadRequiresNeutral=next.requiresNeutral;if(gamepadPreserveBrakeDuringRearm&&next.requiresNeutral)gamepadState.steering=0;if(!next.requiresNeutral)gamepadPreserveBrakeDuringRearm=false;gamepadState.throttle=next.throttle;gamepadState.brake=next.brake;return next}
function sampleGamepad(){if(!gamepadConfig.enabled||document.hidden||!document.hasFocus()){gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}const pad=firstConnectedGamepad();if(!pad){gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}gamepadState.connected=true;const standard=pad.mapping==='standard';if(standard){const steering=axisValue(pad,0);let steeringValue=steering===null?0:(steering-calibration.steeringCenter)/calibration.steeringRange;if(gamepadConfig.steering_inverted)steeringValue=-steeringValue;gamepadState.steering=clamp(applyDeadzone(steeringValue),-1,1);gamepadState.throttle=clamp(applyPedalDeadzone(buttonValue(pad,7)),0,1);gamepadState.brake=clamp(applyPedalDeadzone(buttonValue(pad,6)),0,1)}else{const steering=axisValue(pad,gamepadConfig.steering_axis),throttle=axisValue(pad,gamepadConfig.throttle_axis),brake=axisValue(pad,gamepadConfig.brake_axis);if(steering===null||throttle===null||brake===null){gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;renderControlState();return}let steeringValue=(steering-calibration.steeringCenter)/calibration.steeringRange;if(gamepadConfig.steering_inverted)steeringValue=-steeringValue;gamepadState.steering=clamp(applyDeadzone(steeringValue),-1,1);const throttleDelta=gamepadConfig.throttle_inverted?calibration.throttleRest-throttle:throttle-calibration.throttleRest;const brakeDelta=gamepadConfig.brake_inverted?calibration.brakeRest-brake:brake-calibration.brakeRest;gamepadState.throttle=clamp(applyPedalDeadzone(throttleDelta/calibration.throttleRange),0,1);gamepadState.brake=clamp(applyPedalDeadzone(brakeDelta/calibration.brakeRange),0,1)}const gamepadAuthorityReady=vcuEverReady||vcuMockUnsupported();applyGamepadNeutralInterlock(gamepadAuthorityReady);if(gamepadState.throttle>0&&selectedGear==='N'){const nextGear=updateSelectedGearFromInput({up:true,down:false});if(nextGear.pendingGearRequest)applyGamepadNeutralInterlock(gamepadAuthorityReady,true)}const estopPressed=buttonValue(pad,gamepadConfig.estop_button)>=0.5;if(estopPressed){if(!gamepadEstopPressedAt)gamepadEstopPressedAt=performance.now();if(performance.now()-gamepadEstopPressedAt>=consoleConfig.estop_hold_ms&&latchEstop('Gamepad'))send({estop:true},false).catch(console.error)}else gamepadEstopPressedAt=0;renderControlState()}
function currentControl(extra={}){const control=controlLogic.deriveControl({keyState:state,gamepad:gamepadState,selectedGear,limits:effectiveControlLimits(),steeringFullScaleDeg:limitConfig.steering_full_scale_deg,estop:estopLatched||Boolean(extra.estop)});if(pendingGearTransition&&!control.estop)control.throttle=0;return control}
function setControlReadout(name,text,active=false){for(const element of [controlReadouts[name],operatorControlReadouts[name]]){element.textContent=text;element.parentElement?.classList.toggle('active',active)}}
function renderControlState(){
  const control=currentControl();
  for(const [name,element] of Object.entries(keyIndicators)){const active=Boolean(state[name]);element.classList.toggle('active',active);element.setAttribute('aria-pressed',String(active))}
  setControlReadout('gear',control.gear);setControlReadout('steering',control.steering.toFixed(2),Math.abs(control.steering)>0.001);setControlReadout('throttle',control.throttle.toFixed(2),control.throttle>0.001);setControlReadout('brake',control.brake.toFixed(2),control.brake>0.001);
  const linkReady=polling&&peer?.connectionState==='connected'&&controlChannel?.readyState==='open',vcuReady=vcuDrivingReady(),ready=linkReady&&vcuReady,retainedWait=vcuHandshake.adapter_ready===true&&vcuEverReady&&vcuHandshake.state==='wait_gear'?'换挡闭环中（输入保持）':(vcuHandshake.adapter_ready===true&&vcuEverReady&&vcuHandshake.state==='wait_actuator_modes'?'执行器闭环中（输入保持）':''),terminalState=vcuStateRequiresFreshInput(vcuHandshake),freshReadyRequired=linkReady&&vcuHandshake.ready&&!vcuEverReady;
  inputReadiness.textContent=estopLatched?'急停请求锁定':(gearRejectionInhibited?'换挡拒绝状态不确定，普通控制已冻结':(pendingGearRequest?`等待有效零速后重新选择 ${pendingGearRequest}`:(ready?'控制已就绪':(retainedWait||(terminalState?'VCU 故障/退出，输入已清除':(freshReadyRequired?'输入已清除，等待新鲜 VCU Ready':(linkReady?'等待 VCU 握手':(polling?'等待控制链路':'等待连接'))))))));
  inputReadiness.className=`status-chip ${estopLatched||gearRejectionInhibited||terminalState?'critical':(ready?'ok':'warn')}`;
  const hardStop=controlLogic.deriveControlStopPresentation(estopLatched,vehicleTelemetry?.estop===true,vehicleTelemetry?.stop_source,vehicleTelemetry?.stop_reason,vcuHandshake);
  const sync=latestRuntimeStatus.time_sync||{},trusted=driverTimeSyncReady(),freshness=controlLogic.telemetryFreshness(vehicleTelemetry,vehicleTelemetryReceivedAt,performance.now(),trusted?Date.now()+Number(sync.offset_ms||0):null),feedbackFresh=freshness.received_age_ms!==null&&freshness.received_age_ms<=500&&(freshness.source_age_ms===null||freshness.source_age_ms<=500),guidance=controlLogic.controlRecoveryGuidance(vehicleTelemetry?.control_timing,feedbackFresh);
  if(!estopLatched&&hardStop.visible){inputReadiness.textContent='当前阻挡：安全控制已锁定 · 下一步：核实车端故障及恢复条件';inputReadiness.className='status-chip critical'}else if(!estopLatched&&nativeControlPageRetired){inputReadiness.textContent='当前阻挡：新页面已接管 · 下一步：请在当前操作页面驾驶';inputReadiness.className='status-chip warn'}else if(!estopLatched&&nativeControlTakeoverPending){inputReadiness.textContent='当前阻挡：页面接管尚未确认 · 下一步：保持零输入，等待接管确认';inputReadiness.className='status-chip warn'}else if(!estopLatched&&polling&&guidance.visible){inputReadiness.textContent=guidance.text;inputReadiness.className='status-chip warn'}else if(!estopLatched&&polling&&(driverVideoBlocksControl()||!driverTimeSyncReady()||vcuHandshake.issue_code==='control_recovery_braking_wait')){inputReadiness.textContent=`当前阻挡：${!driverTimeSyncReady()?'时间同步不可信':driverVideoBlocksControl()?'关键视频未就绪':'恢复状态待确认'} · 下一步：保持方向输入释放，等待连续健康确认`;inputReadiness.className='status-chip warn'}
}
function renderVehicles(vehicles=[]){const previous=vehicleSelect.value,currentVehicle=latestRuntimeStatus.connected?latestRuntimeStatus.vehicle_id:'';vehicleSelect.replaceChildren();let firstSelectable='',previousAvailable=false;const labels={online:'在线可控',offline:'离线',active:'控制中',reserved:'已预留',connecting:'连接中',revoked:'已撤销'};for(const vehicle of vehicles){const option=document.createElement('option'),current=vehicle.vehicle_id===currentVehicle,selectable=vehicle.controllable||current;option.value=vehicle.vehicle_id;const controlIssue=String(vehicle.control_issue_message||(vehicle.native_control_compatible===false?'车端版本不兼容，需更新':''));option.textContent=`${vehicle.vehicle_id} · ${controlIssue||(current?'当前会话':(labels[vehicle.state]||vehicle.state))}`;option.disabled=!selectable;if(selectable&&!firstSelectable)firstSelectable=vehicle.vehicle_id;if(selectable&&vehicle.vehicle_id===previous)previousAvailable=true;vehicleSelect.appendChild(option)}vehicleSelect.value=previousAvailable?previous:firstSelectable;connectButton.disabled=connecting||!vehicleSelect.value}
function renderAuthExpiry(expiresAt){authExpiry.textContent=expiresAt?`认证有效至 ${new Date(expiresAt).toLocaleString()}`:''}
function requireLogin(message){closeRealtimeSession();authenticated=false;controlAuthorityLost=false;connectButton.textContent='连接所选车辆';renderAuthExpiry(0);sessionPanel.hidden=true;vcuPanel.hidden=true;monitorPanel.hidden=true;loginPanel.hidden=false;statusPanel.textContent=message;clientLog('driver_reauthentication_required',{reason:message})}
function handleVehicleRefreshError(error){if(error.status===401){requireLogin('登录已失效，请重新认证: '+error.message);return}statusPanel.textContent='车辆状态刷新失败，当前会话已保留: '+error.message;clientLog('vehicle_list_refresh_failed',{error:error.message})}
async function login(){const driver_id=driverIdInput.value.trim();if(!driver_id)throw Error('请输入驾驶员 ID');const password=passwordInput.value;if(!password)throw Error('请输入驾驶员密码');passwordInput.value='';const result=await post('/api/login',{driver_id,password});authenticated=true;controlAuthorityLost=false;webrtcLabel.textContent='未连接';loginPanel.hidden=true;sessionPanel.hidden=false;vcuPanel.hidden=false;renderVehicles(result.vehicles||[]);renderAuthExpiry(result.token_expires_at_utc_ms);sampleGamepad();latestRuntimeStatus=await get('/api/status');renderMonitoring();statusPanel.textContent=`已登录 ${result.driver_id}，请选择在线车辆`;clientLog('driver_login_succeeded',{driver_id:result.driver_id,authorized_vehicle_count:(result.vehicles||[]).length})}
async function refreshVehicles(){if(!authenticated)return;const result=await get('/api/vehicles');renderVehicles(result.vehicles||[]);renderAuthExpiry(result.token_expires_at_utc_ms);if(result.signaling_available===false&&result.direct_authority_continuation!==true){controlAuthorityLost=true;resetControlAuthorityInput();connectButton.disabled=true;statusPanel.textContent='信令服务暂时不可用；车辆列表为安全快照，禁止建立控制会话';renderMonitoring();return}if(result.signaling_restart_recovered){closeRealtimeSession();controlAuthorityLost=true;latestRuntimeStatus=await get('/api/status');connectButton.textContent='连接所选车辆';webrtcLabel.textContent='服务已恢复，需重新建立控制会话';statusPanel.textContent='信令服务已重启，驾驶员身份已自动恢复；旧控制权未恢复，请重新选择车辆';clientLog('signaling_restart_recovered',{previous_service_instance_id:result.previous_service_instance_id,service_instance_id:result.service_instance_id,control_authority_recovered:false});renderMonitoring()}}
function sendVcuHandshakeCommand(action){if((!controlChannel||controlChannel.readyState!=='open')&&!controlTransportCourier.negotiated())throw Error('控制传输尚未连接');if(action==='connect'&&!controlProfileState.acknowledged)throw Error('会话控制参数尚未获得车端确认');if(vcuHandshake.adapter_ready!==true)throw Error('VCU 适配器尚未就绪');if(!['connect','disconnect'].includes(action))throw Error('VCU 握手命令非法');if(action==='disconnect'){resetControlAuthorityInput();vcuHandshake={...vcuHandshake,ready:false,disarming:true}}else{gearRejectionInhibited=false;vcuHandshake={...vcuHandshake,requested:true}}renderMonitoring();const message={event:'vcu_handshake_command',action,sent_at_utc_ms:Date.now()};const delivery=sendControlManagement(message);clientLog('driver_vcu_handshake_command',{action});statusPanel.textContent=action==='connect'?'已请求开始 VCU 平行驾驶握手':'已请求安全断开 VCU 握手';return delivery}
let lastCriticalVideoHealthKey='',lastCriticalVideoHealthLogAt=null;
function recordCriticalVideoHealth(health,staleMs){
  if(!polling)return health;
  const now=performance.now(),key=JSON.stringify([nativeControlSessionId,nativeControlSessionGeneration,health.ready,health.sample_healthy,health.reason||null,(health.cameras||[]).map(camera=>[camera.camera_id,camera.reason,camera.consecutive_anomalies])]);
  if(key!==lastCriticalVideoHealthKey||(!health.ready&&(lastCriticalVideoHealthLogAt===null||now-lastCriticalVideoHealthLogAt>=1000))){
    lastCriticalVideoHealthKey=key;lastCriticalVideoHealthLogAt=now;
    clientLog('driver_critical_video_health',{...health,session_id:nativeControlSessionId,session_generation:nativeControlSessionGeneration,timestamp_scope:'browser_monotonic_snapshot',snapshot_at_monotonic_ms:now,stale_ms:staleMs,document_hidden:document.hidden,visibility_state:document.visibilityState,metrics_sampled_at_utc_ms:latestMetrics.sampled_at_ms||null,receive_metrics:(latestMetrics.streams||[]).filter(stream=>(health.cameras||[]).some(camera=>camera.camera_id===stream.camera_id)),heartbeat_lag_max_ms:controlTraceSummary.timer_lag_max_ms,long_task_max_ms:controlTraceSummary.long_task_max_ms})
  }
  return health
}
function mockControlDiagnosticReady(){
  return consoleConfig.mock_control_diagnostic===true&&mediaStatus.mock_control_diagnostic===true&&
    mediaStatus.adapter_type==='mock'&&mediaStatus.session_id===nativeControlSessionId&&
    mediaStatus.vehicle_id===latestRuntimeStatus.vehicle_id&&String(mediaStatus.vehicle_id).startsWith('test_bench_')&&
    Array.isArray(mediaStatus.lanes)&&mediaStatus.lanes.length===0&&mediaStatusReceivedAt!==null&&
    performance.now()-mediaStatusReceivedAt>=0&&performance.now()-mediaStatusReceivedAt<3000
}
function driverVideoBlocksControl(){const health=driverCriticalVideoHealth();return health.required!==false&&!health.ready}
function driverCriticalVideoHealth(){
  if(mockControlDiagnosticReady())return recordCriticalVideoHealth({required:false,ready:false,sample_healthy:false,assessed:false,reason:'mock_control_diagnostic_no_video',missing:[],cameras:[],bad_sample_revision:0,brake_sample_revision:0},null);
  const lanes=mediaStatus.lanes||[],ids=lanes.filter(lane=>lane.critical_for_control===true).map(lane=>lane.camera_id);
  if(lanes.length>0&&ids.length===0&&lanes.every(lane=>lane.critical_for_control===false)){cameraView.criticalHealth([],1000);return recordCriticalVideoHealth({ready:true,sample_healthy:true,missing:[],cameras:[]},null)};
  const profile=controlProfileState.effectiveProfile,staleMs=profile?profile.critical_camera_stale_brake_ms:(vehicleHardLimits.default_critical_camera_stale_brake_ms||500);
  const health=cameraView.criticalHealth(ids,staleMs);
  return recordCriticalVideoHealth(ids.length?health:{...health,reason:'critical_configuration_unknown'},staleMs)
}
function renderRecordingStatus(){
  if(mockControlDiagnosticReady()){setMetric('metric-recording','Mock 控制诊断 · 视频未测试','warn');return{assessed:false,text:'视频未测试',alerts:[]}};
  const sync=latestRuntimeStatus.time_sync||{},sourceNow=driverTimeSyncReady()?Date.now()+Number(sync.offset_ms||0):null;
  recordingMonitorState=controlLogic.recordingPresentation(mediaStatus,{session_id:nativeControlSessionId,vehicle_id:latestRuntimeStatus.vehicle_id},mediaStatusReceivedAt,performance.now(),sourceNow,recordingMonitorState);
  setMetric('metric-recording',recordingMonitorState.text,recordingMonitorState.level);
  return recordingMonitorState;
}
function driverTimeSyncReady(){
  const sync=latestRuntimeStatus.time_sync||{},limit=Number(consoleConfig.max_time_sync_uncertainty_ms);
  return sync.synchronized===true&&Number.isFinite(Number(sync.uncertainty_ms))&&
      Number(sync.uncertainty_ms)>=0&&Number(sync.uncertainty_ms)<=(Number.isFinite(limit)?limit:100)
}
function driverControlTimeSyncReady(){
  // The native gate owns three-candidate tolerance and absence deadlines.
  // Raw clock trust still owns UTC displays and source-age calculations.
  return (latestRuntimeStatus.time_sync_readiness||{}).ready===true
}
function nativeIntentEnvelope(outgoing){
  const viewHealth=driverCriticalVideoHealth();
  const normalized={gear:String(outgoing.gear||'N'),steering:Number(outgoing.steering||0),throttle:Number(outgoing.throttle||0),brake:Number(outgoing.brake||0),estop:Boolean(outgoing.estop),critical_video_ready:viewHealth.ready===true,critical_video_sample_healthy:viewHealth.sample_healthy===true,critical_video_bad_sample_revision:Number.isSafeInteger(viewHealth.bad_sample_revision)?viewHealth.bad_sample_revision:0,critical_video_brake_revision:Number.isSafeInteger(viewHealth.brake_sample_revision)?viewHealth.brake_sample_revision:0};
  if(nativeControlTakeoverPending&&!normalized.estop)normalized.steering=normalized.throttle=normalized.brake=0;
  const snapshot=JSON.stringify(normalized);
  if(!nativeIntentSeq||snapshot!==lastNativeIntentSnapshot){nativeIntentSeq++;lastNativeIntentSnapshot=snapshot}
  return{session_id:nativeControlSessionId,session_generation:nativeControlSessionGeneration,ui_instance_id:uiInstanceId,intent_seq:nativeIntentSeq,ui_control_epoch:nativeControlUiEpoch,takeover:nativeControlTakeoverPending&&!normalized.estop,...normalized}
}
async function writeControlIntent(extra,announceUnavailable,requestToken={}){
  const activePeer=peer,activeChannel=controlChannel,estopRequested=estopLatched||Boolean(extra.estop);
  if(safeLogoutAttempt&&!estopRequested){clearControlInput(false,'driver_safe_logout');return{sent:false,reason:'safe_logout_in_progress'}};
  if(nativeControlPageRetired&&!estopRequested){clearControlInput(false,'input_page_not_current');statusPanel.textContent='当前阻挡：操作权已由新页面接管 · 下一步：请在当前操作页面驾驶';return{sent:false,reason:'input_page_not_current'}};
  if(controlAuthorityLost){
    resetControlAuthorityInput();
    if(announceUnavailable)webrtcLabel.textContent='控制权丢失';
    return{sent:false,reason:'control_authority_lost'}
  }
  let blockReason='';
  if((!activePeer||activePeer.connectionState!=='connected'||!activeChannel||activeChannel.readyState!=='open')&&!(typeof controlTransportCourier!=='undefined'&&controlTransportCourier.negotiated()&&controlTransportCourier.activePath()==='wss')){
    blockReason='control_link_unavailable';
    if(announceUnavailable)webrtcLabel.textContent='控制链路中断'
  }else if(gearRejectionInhibited&&!estopRequested){
    blockReason='gear_rejection_unresolved'
  }else if(!estopRequested&&!controlProfileState.acknowledged){
    blockReason='control_profile_not_acknowledged';
    if(announceUnavailable)statusPanel.textContent='会话控制参数尚未获得车端确认，驾驶命令已阻止'
  }
  const retainedWait=vcuHandshake.adapter_ready===true&&vcuStateKeepsHeldInput(vcuHandshake),gearTransitionPending=Boolean(pendingGearTransition);
  if(!blockReason&&estopRequested&&vcuHandshake.adapter_ready===false){
    blockReason='vcu_adapter_unavailable';
    if(announceUnavailable)statusPanel.textContent='VCU 适配器明确不可用，远程急停未发送；请使用车辆物理急停'
  }else if(!blockReason&&!vcuDrivingReady()&&!estopRequested&&!retainedWait){
    blockReason='vcu_handshake_not_ready';
    if(announceUnavailable)statusPanel.textContent='VCU 平行驾驶握手未成功，驾驶命令已阻止'
  }
  if(blockReason)clearControlInput(false,blockReason);
  if(nativeControlTakeoverPending&&!estopRequested)clearControlInput(false,'input_page_takeover_pending');
  const viewHealth=driverCriticalVideoHealth(),videoWaiting=(viewHealth.required!==false&&!viewHealth.ready)||!driverControlTimeSyncReady()||vcuHandshake.issue_code==='control_recovery_braking_wait'||(vehicleTelemetry&&vehicleTelemetry.control_timing&&vehicleTelemetry.control_timing.control_braking_wait===true);
  if(videoWaiting&&!estopRequested)clearControlInput(false,'control_braking_wait',true);
  const outgoing=currentControl(blockReason?{}:extra);
  if((retainedWait||gearTransitionPending)&&!estopRequested)outgoing.throttle=0;
  const outgoingSnapshot=controlLogic.controlSnapshot(outgoing);
  const transitionGeneration=!blockReason&&!estopRequested&&pendingGearTransition?pendingGearTransition.generation:0;
  const intent=nativeIntentEnvelope(outgoing);
  const requestStartedAt=performance.now(),controller=new AbortController(),deadlineMs=Math.max(75,Math.min(150,Math.floor(Number(consoleConfig.intent_lease_ms||200)*0.75))),timer=setTimeout(()=>controller.abort(),deadlineMs);
  const requestState={controller,estop:estopRequested,preemptedReason:null};activeControlRequests.set(requestToken,requestState);
  let accepted;
  try{
    accepted=await post('/api/control-intent',intent,controller.signal)
  }catch(error){
    if(intent.session_id!==nativeControlSessionId||intent.session_generation!==nativeControlSessionGeneration)return{sent:false,reason:'stale_control_intent_response'};
    if(error.name==='AbortError'){
      if(requestState.preemptedReason&&!estopRequested){if(requestState.preemptedReason==='estop')clearControlInput(false,'estop_preemption');if(controlTraceEnabled)controlTraceSummary.intent_response_preempted_count++;return{sent:false,reason:'control_intent_preempted_'+requestState.preemptedReason}};
      // A response deadline cannot tell whether the native store accepted this
      // refresh. Its unchanged lease and fresh-neutral interlock own expiry;
      // clearing here turns a delayed local response into a false key release.
      clientLog('control_intent_update_timeout',{intent_seq:intent.intent_seq,deadline_ms:deadlineMs,request_elapsed_ms:Math.max(0,performance.now()-requestStartedAt),input_retained:true,...controlInputDiagnostic()});
      return{sent:false,reason:'control_intent_update_timeout'}
    }
    if(requestState.preemptedReason||intent.intent_seq<nativeIntentSeq)return{sent:false,reason:'superseded_control_intent_response'};
    if(error.status===400&&error.issue_code==='control_page_metadata_missing'){
      clearControlInput(false,'control_page_metadata_missing');
      statusPanel.textContent='当前阻挡：控车页面数据不完整 · 下一步：刷新页面再确认接管';
      return{sent:false,reason:'control_page_metadata_missing'}
    }
    if([401,403,409].includes(error.status)){controlAuthorityLost=true;resetControlAuthorityInput();resetControlProfileSession()}
    throw error
  }finally{clearTimeout(timer);activeControlRequests.delete(requestToken)}
  if(intent.session_id!==nativeControlSessionId||intent.session_generation!==nativeControlSessionGeneration)return{sent:false,reason:'stale_control_intent_response'};
  if(requestState.preemptedReason||intent.intent_seq<nativeIntentSeq)return{sent:false,reason:'superseded_control_intent_response'};
  if(controlTraceEnabled){controlTraceSummary.intent_http_sample_count++;controlTraceSummary.intent_http_elapsed_max_ms=Math.max(controlTraceSummary.intent_http_elapsed_max_ms,performance.now()-requestStartedAt);const timing=accepted.local_http_timing||{};controlTraceSummary.intent_http_server_wait_max_ms=Math.max(controlTraceSummary.intent_http_server_wait_max_ms,Number(timing.before_handler_ms||0));controlTraceSummary.intent_http_server_processing_max_ms=Math.max(controlTraceSummary.intent_http_server_processing_max_ms,Number(timing.handler_ms||0))}
  if(Number.isSafeInteger(accepted.vehicle_accepted_command_seq)&&accepted.vehicle_accepted_command_seq>lastNativeAcceptedControlSeq)lastNativeAcceptedControlSeq=accepted.vehicle_accepted_command_seq;
  if(Number.isSafeInteger(accepted.rearm_control_status_seq)&&accepted.rearm_control_status_seq>lastControlStatusSeq)lastControlStatusSeq=accepted.rearm_control_status_seq;
  if(!accepted.accepted){
    if(['input_page_not_current','input_page_takeover_conflict','input_page_epoch_mismatch','input_page_epoch_exhausted'].includes(accepted.reason)){
      nativeControlPageRetired=true;nativeControlTakeoverPending=false;clearControlInput(false,accepted.reason);
      statusPanel.textContent='当前阻挡：操作权已由其他页面接管 · 下一步：请在当前操作页面驾驶';
    }
    if(accepted.reason==='fresh_neutral_required'){
      clearControlInput(false,'native_intent_rearm_required',accepted.preserve_brake_during_rearm===true);
      lastNativeIntentSnapshot='';
      lastKeyboardEvent.textContent=accepted.preserve_brake_during_rearm===true?'车端请求重新输入 · 运动输入已清除，持续制动保留':'原生输入租约失效，输入已清除 · 请释放后重新按下';
      statusPanel.textContent=accepted.preserve_brake_during_rearm===true?'链路已恢复：保持当前挡位和持续制动；释放旧方向输入，提供零输入后重新按下即可继续':'原生控制输入需重新激活：已清零并保持当前挡位；请释放控制键后重新按下';
    }
    clientLog('control_intent_rejected',{intent_seq:intent.intent_seq,reason:accepted.reason||'control_intent_rejected',requires_fresh_input:Boolean(accepted.requires_fresh_input),request_elapsed_ms:Math.max(0,performance.now()-requestStartedAt),...controlInputDiagnostic()});
    return{...accepted,sent:false,reason:accepted.reason||'control_intent_rejected'}
  }
  if(intent.takeover&&!intent.estop){
    if(!Number.isSafeInteger(accepted.ui_control_epoch)||![intent.ui_control_epoch,intent.ui_control_epoch+1].includes(accepted.ui_control_epoch)){clearControlInput(false,'input_page_confirmation_invalid');return{sent:false,reason:'input_page_confirmation_invalid'}};
    nativeControlUiEpoch=accepted.ui_control_epoch;nativeControlTakeoverPending=false;
    statusPanel.textContent='本页面已确认接管 · 下一步：满足就绪条件后，释放控制键再重新按下';
  }
  if(transitionGeneration){
    pendingGearTransition=controlLogic.recordForwardedGearCommand(pendingGearTransition,transitionGeneration,accepted.intent_seq,outgoingSnapshot.gear)
  }
  if(!accepted.duplicate)clientLog('control_intent_accepted',{intent_seq:accepted.intent_seq,transport:accepted.transport,gear:outgoingSnapshot.gear,estop:estopRequested});
  return{...accepted,sent:true,blocked_reason:blockReason||null}
}
function reportControlQueueError(error){if(controlTraceEnabled)noteScopedControlTraceCounter('queue_unhandled_error_count',error&&typeof error==='object'?(controlTraceErrorScopes.get(error)||controlTraceScope):controlTraceScope);console.error(error)}
const controlWriteQueue=controlLogic.createLatestControlWriteQueue(writeControlIntent,reportControlQueueError,(token,reason)=>{const active=activeControlRequests.get(token);if(active&&(!active.estop||reason==='session_changed')){active.preemptedReason=reason;active.controller.abort()}});
function enqueueIntentRefresh(){return controlWriteQueue.enqueueHeartbeat()}
async function send(extra={},announceUnavailable=true){if(controlTraceEnabled)controlTraceSummary.explicit_send_count++;return controlWriteQueue.send(extra,announceUnavailable)}
async function refreshControlIntent(){const now=performance.now();if(!polling){lastHeartbeatTraceAt=null;return}noteIntentRefresh(now);sampleGamepad();sendPendingControlProfile();const enqueued=enqueueIntentRefresh();if(controlTraceEnabled){if(enqueued)controlTraceSummary.heartbeat_enqueued_count++;else controlTraceSummary.heartbeat_coalesced_count++}}
function advertisedCodecs(){const caps=RTCRtpReceiver.getCapabilities&&RTCRtpReceiver.getCapabilities('video');const found=new Set(['h264']);for(const c of (caps&&caps.codecs)||[]){const m=(c.mimeType||'').toLowerCase();if(m.includes('h265')||m.includes('hevc'))found.add('h265');if(m.includes('h264')||m.includes('avc'))found.add('h264')}return [...found]}
async function connect(){if(connecting)return;const attempt=++connectRequestGeneration;const target=vehicleSelect.value;if(!target)throw Error('没有可连接的在线车辆');const fromVehicle=latestRuntimeStatus.connected?latestRuntimeStatus.vehicle_id:'';if(polling&&fromVehicle===target){statusPanel.textContent=`车辆 ${target} 已处于当前会话`;return}const changingVehicle=Boolean(fromVehicle)&&fromVehicle!==target;const reconnecting=Boolean(fromVehicle)&&fromVehicle===target;const hadRealtime=polling;let suspendedGeneration=signalingGeneration;if((changingVehicle||reconnecting)&&hadRealtime){suspendedGeneration=suspendSignalingPoll();clearControlInput()}connecting=true;connectButton.disabled=true;if(changingVehicle){webrtcLabel.textContent='正在安全切换车辆';statusPanel.textContent=`正在验证 ${target}，成功后释放 ${fromVehicle}`;clientLog('driver_vehicle_switch_started',{from_vehicle_id:fromVehicle,to_vehicle_id:target})}let session=null,generation=signalingGeneration;try{session=await post('/api/connect',{vehicle_id:target,ui_instance_id:uiInstanceId});if(attempt!==connectRequestGeneration)return;generation=closeRealtimeSession();nativeControlSessionId=String(session.session_id||'');nativeControlSessionGeneration=Number(session.control_session_generation);nativeControlUiEpoch=Number(session.ui_control_epoch);nativeControlTakeoverPending=true;nativeControlPageRetired=false;if(!Number.isSafeInteger(nativeControlUiEpoch)||nativeControlUiEpoch<0)throw Error('Input page epoch is invalid');if(!nativeControlSessionId||!Number.isSafeInteger(nativeControlSessionGeneration)||nativeControlSessionGeneration<=0)throw Error('原生控制会话代次无效');setControlTraceScope(session.session_id,session.vehicle_id);controlAuthorityLost=true;const ice=await post('/api/webrtc/ice-servers');if(attempt!==connectRequestGeneration)return;iceServers=ice.ice_servers||[];await post('/api/webrtc/capabilities',{codecs:advertisedCodecs()});if(attempt!==connectRequestGeneration)return;polling=true;controlAuthorityLost=false;latestRuntimeStatus=await get('/api/status');if(attempt!==connectRequestGeneration)return;webrtcLabel.textContent='等待车端媒体';statusPanel.textContent=`会话 ${session.session_id} · ${session.vehicle_id}`;connectButton.textContent='切换所选车辆';document.querySelector('main').focus();renderMonitoring();clientLog(changingVehicle?'driver_vehicle_switched':(reconnecting?'driver_session_reconnected':'driver_session_connected'),{from_vehicle_id:fromVehicle||undefined,session_id:session.session_id,vehicle_id:session.vehicle_id});pollSignaling(generation)}catch(error){if(attempt!==connectRequestGeneration)return;if(session){const failedScope=pageReleaseScope(session);flushControlTrace('connect_setup_failed');controlTraceScope={session_id:'',vehicle_id:''};nativeControlSessionId='';nativeControlSessionGeneration=0;lastNativeIntentSnapshot='';nativeControlUiEpoch=0;nativeControlTakeoverPending=false;nativeControlPageRetired=false;await post('/api/end-session',safeLogoutReleaseBody(failedScope,'driver_connect_setup_failed')).catch(()=>{})}latestRuntimeStatus=await get('/api/status').catch(()=>({connected:false}));const retained=Boolean(!session&&latestRuntimeStatus.connected&&hadRealtime);if(retained){polling=true;controlAuthorityLost=false;webrtcLabel.textContent=controlChannel&&controlChannel.readyState==='open'?'控制链路已连接':'当前会话已保留';statusPanel.textContent=`切换失败，当前会话已保留: ${error.message}`;clientLog('driver_vehicle_switch_rejected',{from_vehicle_id:fromVehicle,to_vehicle_id:target,error:error.message});pollSignaling(suspendedGeneration)}else{controlAuthorityLost=Boolean(latestRuntimeStatus.connected)}connectButton.textContent=latestRuntimeStatus.connected?'切换所选车辆':'连接所选车辆';renderMonitoring();if(!retained)throw error}finally{if(attempt===connectRequestGeneration){connecting=false;connectButton.disabled=!vehicleSelect.value}}}
function pageReleaseScope(session=null){const source=session||{},native=latestRuntimeStatus.native_control||{},pick=(value,fallback)=>value===null||value===undefined?fallback:value;return{session_id:String(pick(source.session_id,nativeControlSessionId)),session_generation:Number(pick(source.control_session_generation,nativeControlSessionGeneration||latestRuntimeStatus.control_session_generation)),ui_epoch:Number(source.session_id===nativeControlSessionId?nativeControlUiEpoch:pick(source.ui_control_epoch,nativeControlUiEpoch||native.ui_control_epoch)),driver_id:String(source.driver_id||latestRuntimeStatus.driver_id||driverIdInput.value.trim()),connect_cancellation_generation:Number(pick(source.connect_cancellation_generation,latestRuntimeStatus.connect_cancellation_generation))}}
function safeLogoutLocalScopeCurrent(attempt){return connectRequestGeneration===attempt.request_generation&&nativeControlSessionId===attempt.local_session_id&&nativeControlSessionGeneration===attempt.local_session_generation&&nativeControlUiEpoch===attempt.local_ui_epoch}
function safeLogoutRuntimeMatches(attempt,runtime){
  if(runtime.authenticated!==true||String(runtime.driver_id||'')!==attempt.driver_id||String(runtime.session_id||'')!==attempt.session_id)return false;
  if(attempt.session_id){const owner=(runtime.native_control||{}).owner_ui_instance_id,owns=owner===uiInstanceId||(owner===''&&attempt.ui_epoch===0&&runtime.session_creator_ui_instance_id===uiInstanceId);return runtime.connected===true&&Number(runtime.control_session_generation)===attempt.session_generation&&Number((runtime.native_control||{}).ui_control_epoch)===attempt.ui_epoch&&owns}
  return runtime.connected===false;
}
function safeLogoutReleaseBody(attempt,reason){return{reason,expected_session_id:attempt.session_id,expected_control_session_generation:attempt.session_generation,expected_ui_control_epoch:attempt.ui_epoch,ui_instance_id:uiInstanceId,expected_driver_id:attempt.driver_id,expected_connect_cancellation_generation:attempt.connect_cancellation_generation}}
async function safeLogoutRequest(path,body,timeoutMs){
  const controller=new AbortController();let timer;
  try{return await Promise.race([body===null?get(path,controller.signal):post(path,body,controller.signal),new Promise((_,reject)=>{timer=setTimeout(()=>{controller.abort();reject(Error('退出请求响应超时'))},timeoutMs)})])}finally{clearTimeout(timer)}
}
function safeLogoutTransportCurrent(attempt){
  if(attempt.wss_transport)return typeof controlTransportCourier!=='undefined'&&controlTransportCourier.negotiated()&&
    controlTransportCourier.activePath()==='wss'&&controlTransportCourier.epoch()===attempt.transport_epoch;
  return peer===attempt.peer&&controlChannel===attempt.channel&&peer?.connectionState==='connected'&&controlChannel?.readyState==='open'
}
async function waitForSafeLogoutFeedback(attempt){
  for(;;){
    if(!safeLogoutLocalScopeCurrent(attempt))return{confirmed:false,reason:'session_changed'};
    if(!safeLogoutTransportCurrent(attempt))return{confirmed:false,reason:'link_lost'};
    if(performance.now()>=attempt.deadline_monotonic_ms)return{confirmed:false,reason:'timeout'};
    const sync=latestRuntimeStatus.time_sync||{},synchronizedNow=driverTimeSyncReady()?Date.now()+Number(sync.offset_ms||0):null;
    const evidence=controlLogic.safeLogoutFeedback(attempt,vehicleTelemetry,vehicleTelemetryReceivedAt,performance.now(),synchronizedNow);
    if(evidence.confirmed||evidence.reason==='adapter_failed')return evidence;
    await new Promise(resolve=>setTimeout(resolve,Math.min(100,attempt.deadline_monotonic_ms-performance.now())));
  }
}
async function logout(){
  if(safeLogoutAttempt)return safeLogoutAttempt.promise;
  const started=performance.now(),sync=latestRuntimeStatus.time_sync||{};
  const attempt={request_generation:++connectRequestGeneration,local_session_id:nativeControlSessionId,local_session_generation:nativeControlSessionGeneration,local_ui_epoch:nativeControlUiEpoch,
    session_id:nativeControlSessionId,session_generation:nativeControlSessionGeneration,ui_epoch:nativeControlUiEpoch,driver_id:String(latestRuntimeStatus.driver_id||driverIdInput.value.trim()),
    vehicle_id:String(latestRuntimeStatus.vehicle_id||''),status_floor:lastControlStatusSeq,started_at_monotonic_ms:started,deadline_monotonic_ms:started+15000,
    source_floor_utc_ms:driverTimeSyncReady()?Date.now()+Number(sync.offset_ms||0):null,peer,channel:controlChannel,connect_cancellation_generation:null,
    wss_transport:typeof controlTransportCourier!=='undefined'&&controlTransportCourier.negotiated()&&controlTransportCourier.activePath()==='wss'&&controlChannel?.readyState!=='open',
    transport_epoch:typeof controlTransportCourier!=='undefined'?controlTransportCourier.epoch():0};
  safeLogoutAttempt=attempt;connecting=false;connectButton.disabled=true;resetControlAuthorityInput('driver_safe_logout');
  attempt.promise=(async()=>{
    let result={confirmed:false,reason:attempt.session_id?'feedback_missing':'no_active_session'},released=false;
    try{
      const before=await safeLogoutRequest('/api/status',null,1000);
      if(!safeLogoutLocalScopeCurrent(attempt)||!safeLogoutRuntimeMatches(attempt,before)){result={confirmed:false,reason:'session_changed'};return result}
      latestRuntimeStatus=before;
      if(!attempt.session_id){attempt.session_generation=Number(before.control_session_generation);attempt.ui_epoch=Number((before.native_control||{}).ui_control_epoch)}
      attempt.connect_cancellation_generation=Number(before.connect_cancellation_generation);
      if(attempt.session_id&&vcuHandshake.supported===true){
        if(!safeLogoutTransportCurrent(attempt)){result={confirmed:false,reason:'link_lost'}}
        else{
          attempt.status_floor=lastControlStatusSeq;attempt.started_at_monotonic_ms=performance.now();
          attempt.source_floor_utc_ms=driverTimeSyncReady()?Date.now()+Number((latestRuntimeStatus.time_sync||{}).offset_ms||0):null;
          try{await sendVcuHandshakeCommand('disconnect');webrtcLabel.textContent='等待车端退出确认';statusPanel.textContent='已请求车端退出；保留反馈，等待退出协议及最终 CAN 写入确认（最多 15 秒）。';result=await waitForSafeLogoutFeedback(attempt)}
          catch(error){result={confirmed:false,reason:'request_failed'};clientLog('driver_safe_logout_vcu_request_failed',{error:error.message})}
        }
      }else if(attempt.session_id){result={confirmed:false,reason:'unsupported'}}
      if(!safeLogoutLocalScopeCurrent(attempt)){result={confirmed:false,reason:'session_changed'};return result}
      const current=await safeLogoutRequest('/api/status',null,1000);
      if(!safeLogoutLocalScopeCurrent(attempt)||!safeLogoutRuntimeMatches(attempt,current)){result={confirmed:false,reason:'session_changed'};return result}
      if(performance.now()>=attempt.deadline_monotonic_ms)result={...result,confirmed:false,reason:'timeout'};
      webrtcLabel.textContent='正在释放控制权';
      try{await safeLogoutRequest('/api/disconnect',safeLogoutReleaseBody(attempt,'driver_safe_logout'),1500);released=true}catch(error){result={...result,confirmed:false,reason:'release_failed'};clientLog('driver_safe_logout_release_failed',{error:error.message})}
      if(performance.now()>=attempt.deadline_monotonic_ms)result={...result,confirmed:false,reason:'timeout'};
      if(released&&safeLogoutLocalScopeCurrent(attempt)){closeRealtimeSession();authenticated=false;controlAuthorityLost=false;connectButton.textContent='连接所选车辆';renderAuthExpiry(0);sessionPanel.hidden=true;vcuPanel.hidden=true;renderCanFeedback();monitorPanel.hidden=true;loginPanel.hidden=false;webrtcLabel.textContent='未连接'}
      else if(released){result={confirmed:false,reason:'session_changed'}}
      return result;
    }catch(error){result={confirmed:false,reason:'request_failed'};clientLog('driver_safe_logout_request_failed',{error:error.message});return result}
    finally{const presentation=controlLogic.safeLogoutPresentation(result,released,estopLatched);statusPanel.textContent=presentation.text;clientLog('driver_safe_logout',{session_id:attempt.session_id,session_generation:attempt.session_generation,exit_confirmed:presentation.confirmed,reason:result.reason,session_released:released,estop_request_latched:estopLatched,estop_confirmed:result.estop_confirmed===true})}
  })();
  try{return await attempt.promise}finally{if(safeLogoutAttempt===attempt)safeLogoutAttempt=null}
}
addEventListener('pagehide',()=>{const scope=pageReleaseScope();++connectRequestGeneration;flushControlTrace('pagehide');closeRealtimeSession();if(authenticated)fetch('/api/disconnect',{method:'POST',headers:{'content-type':'application/json'},body:JSON.stringify(safeLogoutReleaseBody(scope,'browser_page_closed')),keepalive:true}).catch(()=>{})});
function neutralizeInput(event){clearControlInput(false,event?.type==='blur'?'window_blur':'document_hidden');send({},false).catch(console.error)}
addEventListener('blur',neutralizeInput);document.addEventListener('visibilitychange',()=>{if(document.hidden)neutralizeInput()});
document.querySelector('#login').onclick=()=>login().catch(e=>{statusPanel.textContent='登录失败: '+e.message});
driverIdInput.addEventListener('keydown',e=>{if(e.key==='Enter')passwordInput.focus()});
passwordInput.addEventListener('keydown',e=>{if(e.key==='Enter')login().catch(error=>{statusPanel.textContent='登录失败: '+error.message})});
document.querySelector('#connect').onclick=()=>{if(safeLogoutAttempt)return;connect().catch(e=>{webrtcLabel.textContent='连接失败';statusPanel.textContent=e.message})};
document.querySelector('#logout').onclick=()=>logout().catch(e=>{statusPanel.textContent='退出失败: '+e.message});
document.querySelector('#estop').onclick=()=>{latchEstop('页面按钮');send({estop:true}).catch(alert)};
controlLimitsOpen.onclick=openControlLimits;
controlLimitsConfirm.onchange=()=>{controlLimitsApply.disabled=!controlLimitsConfirm.checked};
controlLimitsApply.onclick=()=>applyControlLimits().catch(error=>{statusPanel.textContent='限幅设置失败: '+error.message});
controlLimitsCancel.onclick=()=>controlLimitsDialog.close();
vcuConnectButton.onclick=async()=>{try{if(safeLogoutAttempt)throw Error('正在退出，不能重新握手');await sendVcuHandshakeCommand('connect')}catch(error){statusPanel.textContent='开始握手失败: '+error.message}};
vcuDisconnectButton.onclick=async()=>{try{await sendVcuHandshakeCommand('disconnect')}catch(error){statusPanel.textContent='断开握手失败: '+error.message}};
const keyNames={left:'左转',right:'右转',up:'前进',down:'倒车',service_brake:'缓刹',hard_brake:'急刹'};
renderControlLimits();
renderControlState();
renderCanFeedback();
addEventListener('gamepadconnected',e=>{activeGamepadIndex=e.gamepad.index;sampleGamepad();clientLog('gamepad_connected',{id:e.gamepad.id,mapping:e.gamepad.mapping,axes:e.gamepad.axes.length,buttons:e.gamepad.buttons.length})});addEventListener('gamepaddisconnected',e=>{if(activeGamepadIndex===e.gamepad.index)activeGamepadIndex=null;gamepadState.connected=false;gamepadState.steering=0;gamepadState.throttle=0;gamepadState.brake=0;gamepadRequiresNeutral=true;renderControlState();clientLog('gamepad_disconnected',{id:e.gamepad.id})});
function editingTarget(target){return ['INPUT','SELECT','TEXTAREA','BUTTON'].includes((target?target.tagName:undefined))||Boolean((target?target.isContentEditable:undefined))}
addEventListener('keydown',e=>{const binding=keys[e.code],estopKey=e.code==='KeyE';if(!binding&&!estopKey)return;if(editingTarget(e.target))return;e.preventDefault();if(binding&&!e.repeat)clientLog('driver_input_keydown',{code:e.code,...controlInputDiagnostic()});if(!polling){if(binding)controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${estopKey?'急停':keyNames[binding]}已截获 · 等待连接`;return}if(estopKey){if(!e.repeat){lastKeyboardEvent.textContent='急停请求已锁定 · E';latchEstop('键盘 E');send({estop:true}).catch(console.error)}return}if(blockedControlKeys.has(e.code)){lastKeyboardEvent.textContent=`${keyNames[binding]}需释放后重新按下 · ${e.code}`;return}if(vcuStateRequiresFreshInput(vcuHandshake)){controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${keyNames[binding]}已阻止 · 等待 VCU 恢复后重新按下`;return}if(!vcuEverReady&&!vcuMockUnsupported()){controlLogic.blockKey(blockedControlKeys,e.code);lastKeyboardEvent.textContent=`${keyNames[binding]}已阻止 · 首次握手完成后请重新按下`;return}const pressed=controlLogic.pressKey(pressedControlKeys,blockedControlKeys,e.code);if(pressed.changed){syncControlKeyState();updateSelectedGearFromHeldDirections();lastKeyboardEvent.textContent=`${keyNames[binding]}按下 · ${e.code}`;renderControlState();send().catch(console.error)}});
addEventListener('keyup',e=>{const binding=keys[e.code];if(!binding)return;if(!editingTarget(e.target))e.preventDefault();clientLog('driver_input_keyup',{code:e.code,...controlInputDiagnostic()});const released=controlLogic.releaseKey(pressedControlKeys,blockedControlKeys,e.code);syncControlKeyState();updateSelectedGearFromHeldDirections();lastKeyboardEvent.textContent=`${keyNames[binding]}释放 · ${e.code}`;renderControlState();if(released.changed&&polling)send().catch(console.error)});
function applyMediaStatus(payload){
  const id=payload?.media_negotiation?.id;
  if(id&&mediaOfferScope?.id&&id!==mediaOfferScope.id)return;
  const localScopeIssue=mediaStatus.local_scope_issue;
  mediaStatus=payload||{lanes:[]};
  if(localScopeIssue)mediaStatus.local_scope_issue=localScopeIssue;
  mediaStatusReceivedAt=performance.now();
  const mediaDiagnostic=controlLogic.mediaScopeDiagnostic(mediaStatus.media_negotiation?.issue_code);
  if(mediaDiagnostic)statusPanel.textContent=mediaDiagnostic.text;
  const diagnostic=controlLogic.vehicleControlDiagnostic(mediaStatus.control_issue_code,mediaStatus.control_recovery);
  if(diagnostic&&diagnostic.level==='critical'){
    updateVcuHandshakeState({...vcuHandshake,issue_code:mediaStatus.control_issue_code,control_recovery:mediaStatus.control_recovery||null,ready:false,state:'fault'});
  }else if(mediaStatus.control_issue_code==='control_recovery_braking_wait'){
    updateVcuHandshakeState({...vcuHandshake,issue_code:mediaStatus.control_issue_code,control_recovery:mediaStatus.control_recovery||null});
  }else if(vcuHandshake.issue_code==='control_recovery_braking_wait'){
    vcuHandshake={...vcuHandshake,issue_code:'',control_recovery:null};
  }
  renderMonitoring();
}
async function pollSignaling(generation){const controller=new AbortController();signalingPollAbort=controller;while(polling&&generation===signalingGeneration){try{const data=await post('/api/poll-signaling',{},controller.signal);if(generation!==signalingGeneration)break;const messages=data.messages||[];let latestOffer=-1,offerCount=0;messages.forEach((message,index)=>{if(message.type==='webrtc_offer'){latestOffer=index;offerCount++}});const latestScope=latestOffer>=0?controlLogic.iceOfferScope(messages[latestOffer].payload||{},!mediaHasOffer&&offerCount===1):null;for(let index=0;index<messages.length;index++){if(generation!==signalingGeneration)break;const message=messages[index];if(message.type==='webrtc_offer'&&index!==latestOffer)continue;if(message.type==='ice_candidate'&&latestScope&&!controlLogic.iceCandidateMatchesOffer(message.payload||{},latestScope))continue;if(message.type==='webrtc_offer')await startFromOffer(message.payload||{});if(message.type==='ice_candidate')await addIce(message.payload||{});if(message.type==='media_status')applyMediaStatus(message.payload);if(message.type==='control_status')applyVehicleControlMessage(message.payload)}}catch(e){if(generation!==signalingGeneration||e.name==='AbortError')break;const failedScope=pageReleaseScope();closeRealtimeSession();controlAuthorityLost=true;webrtcLabel.textContent='控制权或信令中断';statusPanel.textContent='信令轮询失败，已停止驾驶命令: '+e.message;post('/api/end-session',safeLogoutReleaseBody(failedScope,'signaling_poll_failed')).catch(()=>{});clientLog('signaling_poll_failed',{error:e.message});renderMonitoring();break}await new Promise(r=>setTimeout(r,100))}if(signalingPollAbort===controller)signalingPollAbort=null}
function applyVehicleControlMessage(message,feedbackVerified=false){
  const feedback=typeof controlTransportCourier==='undefined'?{}:controlTransportCourier.feedback?.()||{};
  if(feedback.enabled===true){const stamp=message.feedback_transport;if(!feedbackVerified||!stamp||stamp.epoch!==feedback.epoch||stamp.authority_epoch!==feedback.authority_epoch||stamp.authority_issuer!==feedback.authority_issuer||stamp.media_attempt!==feedback.media_attempt||stamp.path!==feedback.path||stamp.status_floor!==feedback.status_floor)return}
  if(message.session_id&&message.session_id!==nativeControlSessionId)return;
        if(!['vehicle_telemetry','vcu_handshake_status','session_control_profile_status','control_command_rejected'].includes(message.event))return;
        if(!acceptControlStatusMessage(message))return;
        const gearRejectionState=message.event==='control_command_rejected'&&message.issue_code==='vcu_drive_gear_change_moving_or_stale'?controlLogic.reduceGearChangeRejection(pendingGearTransition,message,selectedGear):null;
        const gearRejectionMatched=Boolean(gearRejectionState?.matched);
        if(message.event==='control_command_rejected'){
          const rejection=controlLogic.deriveControlCommandRejection(message.issue_code);
          let rollbackFrom=null,rollbackTo=null;
          if(rejection.action==='rollback_gear_change'){
            if(gearRejectionMatched){rollbackFrom=pendingGearTransition.toGear;rollbackTo=pendingGearTransition.fromGear}
            selectedGear=gearRejectionState.selectedGear;pendingGearRequest=gearRejectionState.pendingGearRequest;pendingGearTransition=gearRejectionState.pendingGearTransition;gearRejectionInhibited=gearRejectionState.inhibitOrdinaryControl;clearControlInput(false,'vehicle_gear_change_rejected');
            if(gearRejectionState.sendRollback)send({},false).catch(console.error);
          }else if(rejection.action==='rearm_input'){clearControlInput(false,'vehicle_input_rearm_required',true);send({},false).catch(console.error)}else if(rejection.clearInput)clearControlInput(true,'vehicle_control_rejected');
          statusPanel.textContent=gearRejectionMatched?rejection.text:(rejection.action==='rollback_gear_change'?'换挡拒绝无法关联，普通控制已冻结；请安全断开并重新握手。':rejection.text);
          const commandSeq=Number(message.command_seq);
          clientLog('driver_control_command_rejected',{issue_code:rejection.issueCode,command_seq:Number.isSafeInteger(commandSeq)&&commandSeq>0?commandSeq:null,gear_rejection_matched:gearRejectionMatched,rollback_from:rollbackFrom,rollback_to:rollbackTo});
          renderMonitoring();
          return;
        }
        if(message.event==='session_control_profile_status'){
          applyControlProfileStatus(message);
          updateVehicleHardLimits(message.hard_limits);
          renderMonitoring();
          return;
        }
        if(message.event==='vehicle_telemetry'){
          vehicleTelemetry=message;vehicleTelemetryReceivedAt=performance.now();
          if(controlLogic.telemetryConfirmsGearTransition(pendingGearTransition,message)){const completedTransition=pendingGearTransition;pendingGearTransition=null;pendingGearRequest=null;clientLog('driver_gear_transition_confirmed',{from_gear:completedTransition.fromGear,to_gear:completedTransition.toGear,control_status_seq:Number(message.control_status_seq)})}
          if(message.vcu_handshake){const nextVcuStatus={...message.vcu_handshake,driver_connected:true,adapter_ready:vcuAdapterReady(message.vcu_handshake,message.vehicle_adapter?.opened)};updateVcuHandshakeState(nextVcuStatus)}
          renderEstopRequest();
          applyControlProfileStatus(message.session_control_profile);
          updateVehicleHardLimits(message.control_limits);
          applyVehicleSafetyState(message.safety_state,message.control_timing);
          renderMonitoring();
          return;
        }
        if(message.event!=='vcu_handshake_status')return;
        const nextVcuStatus=message.status||{};
        updateVcuHandshakeState({...nextVcuStatus,issue_code:message.issue_code||'',control_recovery:message.control_recovery||null,driver_connected:Boolean(message.driver_connected),adapter_ready:vcuAdapterReady(nextVcuStatus,message.adapter_ready)});
        updateVehicleHardLimits(message.hard_limits);
        if(message.session_control_profile)applyControlProfileStatus(message.session_control_profile);
        if(controlLogic.vehicleControlDiagnostic(vcuHandshake.issue_code,vcuHandshake.control_recovery))statusPanel.textContent=diagnoseVcuHandshake(true).text;
        else if(vcuHandshake.adapter_ready===false)statusPanel.textContent='VCU 适配器明确不可用；视频保持在线，驾驶命令已阻止';
        else if(vcuHandshake.adapter_ready===null)statusPanel.textContent='VCU 适配器状态未确认；视频保持在线，驾驶命令已阻止';
        else if(message.result==='command_rejected')statusPanel.textContent='开始握手失败：'+diagnoseVcuHandshake(true).text;
        else if(vcuHandshake.handshake_revoked)statusPanel.textContent=diagnoseVcuHandshake(true).text;
        else if(vcuDrivingReady())statusPanel.textContent='VCU 平行驾驶握手成功，可以发送驾驶命令';
        else if(vcuHandshake.state==='disarmed')statusPanel.textContent=safeLogoutAttempt?'车端报告退出协议完成；仍等待本次退出的新鲜完整 CAN 确认':'车端报告 VCU 退出协议完成；停车状态需本地核实';
        clientLog('driver_vcu_handshake_status',{result:message.result,issue_code:vcuHandshake.issue_code,control_recovery:vcuHandshake.control_recovery,adapter_ready:vcuHandshake.adapter_ready,state:vcuHandshake.state,ready:Boolean(vcuHandshake.ready),parking_ready:Boolean(vcuHandshake.parking_ready),handshake_revoked:Boolean(vcuHandshake.handshake_revoked),vmc_fault_code:vcuHandshake.vmc_fault_code_valid?Number(vcuHandshake.vmc_fault_code):null});
        renderMonitoring();

}
function controlManagementScope(){return{source_session_id:nativeControlSessionId,source_control_session_generation:nativeControlSessionGeneration,
  media_negotiation_id:mediaOfferScope?.id||'',ui_instance_id:uiInstanceId,ui_control_epoch:nativeControlUiEpoch}}
function sendControlManagement(message){
  const authority=latestRuntimeStatus.native_control?.control_transport?.direct_authority||{},disconnect=message.event==='vcu_handshake_command'&&message.action==='disconnect';
  if(!controlTransportCourier.negotiated()&&controlChannel?.readyState==='open'&&(!authority.configured||disconnect)){controlChannel.send(JSON.stringify(message));return Promise.resolve()}
  if(!controlTransportCourier.negotiated()&&authority.valid!==true)return Promise.reject(Error('控制传输尚未协商'));
  return post('/api/control-transport/management',{...controlManagementScope(),message})
}
function currentMediaPeer(instance,epoch,session,generation){return peer===instance&&mediaPeerEpoch===epoch&&nativeControlSessionId===session&&nativeControlSessionGeneration===generation}
function showMediaScopeIssue(){if(mediaScopeIssue==='media_ice_scope_missing')return;mediaScopeIssue='media_ice_scope_missing';mediaStatus.local_scope_issue=mediaScopeIssue;statusPanel.textContent=controlLogic.mediaScopeDiagnostic(mediaScopeIssue).text;clientLog('driver_media_scope_rejected',{issue_code:mediaScopeIssue});renderMonitoring()}
function retireMediaOffer(scope){if(!scope)return;for(const key of [scope.id?`id:${scope.id}`:'',...scope.ufrags.map(value=>`ufrag:${value}`)])if(key)retiredMediaScopes.add(key);while(retiredMediaScopes.size>256)retiredMediaScopes.delete(retiredMediaScopes.values().next().value)}
async function addIce(candidate){
  if(!candidate||typeof candidate.candidate!=='string'||!candidate.candidate)return;
  const scope=controlLogic.iceCandidateScope(candidate);
  if((scope.id&&retiredMediaScopes.has(`id:${scope.id}`))||(!scope.id&&scope.ufrag&&retiredMediaScopes.has(`ufrag:${scope.ufrag}`)))return;
  if(!peer||!peer.remoteDescription||!controlLogic.iceCandidateMatchesOffer(candidate,mediaOfferScope)){
    if(!scope.id&&!scope.ufrag&&mediaHasOffer&&!controlLogic.iceCandidateMatchesOffer(candidate,mediaOfferScope)){showMediaScopeIssue();return}
    if(candidate.candidate.length>8192||pendingIce.length>=128)throw Error('ICE candidate buffer exceeds limit');
    pendingIce.push(candidate);return;
  }
  const instance=peer,epoch=mediaPeerEpoch,session=nativeControlSessionId,generation=nativeControlSessionGeneration;
  try{await instance.addIceCandidate({candidate:candidate.candidate,sdpMid:candidate.sdpMid,sdpMLineIndex:candidate.sdpMLineIndex,usernameFragment:candidate.usernameFragment});if(currentMediaPeer(instance,epoch,session,generation))clientLog('webrtc_remote_ice_candidate_applied',controlLogic.iceCandidateSummary(candidate))}
  catch(error){if(currentMediaPeer(instance,epoch,session,generation)){clientLog('webrtc_remote_ice_candidate_error',{...controlLogic.iceCandidateSummary(candidate),error_name:error.name||'Error'});throw error}}
}
function offeredVideoCameraIds(sdp,tracks){const mapping=new Map(),cameraIds=(tracks||[]).map(track=>track.camera_id).filter(Boolean);let cameraIndex=0;for(const section of String(sdp||'').split(/\r?\nm=/).slice(1)){if(!section.startsWith('video '))continue;const match=section.match(/(?:^|\r?\n)a=mid:([^\r\n]+)/),cameraId=cameraIds[cameraIndex++];if(match&&cameraId)mapping.set(match[1],cameraId)}return mapping}
const cameraView=MineTeleopCameraView.mount(cameraGrid);
function attach(cameraId,track){if(emptyStage.isConnected)emptyStage.remove();cameraView.attach(cameraId,track)}
async function startFromOffer(offer){
  const previousPeer=peer,nextScope=controlLogic.iceOfferScope(offer,!mediaHasOffer);
  if(nextScope.id&&(retiredMediaScopes.has(`id:${nextScope.id}`)||mediaOfferScope&&mediaOfferScope.id===nextScope.id))return;
  const queued=pendingIce;
  retireMediaOffer(mediaOfferScope);
  // A repeated ufrag can belong to renegotiation on the same ICE transport.
  // The unique negotiation id, when supplied, remains the stronger fence.
  for(const value of nextScope.ufrags)retiredMediaScopes.delete(`ufrag:${value}`);
  if(nextScope.id)retiredMediaScopes.delete(`id:${nextScope.id}`);
  peer=null;const epoch=++mediaPeerEpoch;
  mediaOfferScope=nextScope;mediaHasOffer=true;mediaScopeIssue='';delete mediaStatus.local_scope_issue;
  if(previousPeer)previousPeer.close();
  controlChannel=null;
  resetControlProfileSession();
  lastControlStatusSeq=0;
  vehicleTelemetry=null;vehicleTelemetryReceivedAt=null;lastFeedbackHealthLogAt=null;lastFeedbackHealthKey='';
  lastVehicleSafetyState='';
  vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};
  resetControlAuthorityInput();
  pendingIce=queued.filter(candidate=>{const scope=controlLogic.iceCandidateScope(candidate);return !(scope.id&&retiredMediaScopes.has(`id:${scope.id}`))&&!(scope.ufrag&&!scope.id&&retiredMediaScopes.has(`ufrag:${scope.ufrag}`))&&Boolean(scope.id||scope.ufrag||controlLogic.iceCandidateMatchesOffer(candidate,nextScope))});
  cameraByMid.clear();
  assignedCameraIds.clear();
  previousStats.clear();
  h265FailureSamples=0;
  h265FallbackSent=false;
  remoteCameraIds=(offer.media_tracks||[]).map(t=>t.camera_id).filter(id=>typeof id==='string'&&id.length);
  cameraView.configure(remoteCameraIds);
  offeredCameraByMid=offeredVideoCameraIds(offer.sdp,offer.media_tracks||[]);
  const nextPeer=new RTCPeerConnection({bundlePolicy:'max-bundle',iceServers,iceTransportPolicy:consoleConfig.ice_transport_policy});
  peer=nextPeer;
  const sourceSession=nativeControlSessionId,sourceGeneration=nativeControlSessionGeneration;
  const current=()=>currentMediaPeer(nextPeer,epoch,sourceSession,sourceGeneration);
  webrtcLabel.textContent=`协商 ${offer.codec||''}/${offer.backend||''}`;
  nextPeer.onconnectionstatechange=()=>{
    if(!current())return;
    const connectionState=nextPeer.connectionState;
    cameraView.setConnectionState(connectionState);
    webrtcLabel.textContent=connectionState;
    if(connectionState==='disconnected'){
      lastVehicleSafetyState='';
      resetControlAuthorityInput();
      resetControlProfileSession();
      clientLog('webrtc_peer_disconnected');
    }
    if(['failed','closed'].includes(connectionState)){
      const terminalChannel=controlChannel;
      flushControlTrace(`peer_${connectionState}`);
      clientLog('webrtc_peer_terminal',{connection_state:connectionState,trace_session_id:controlTraceScope.session_id||null,trace_vehicle_id:controlTraceScope.vehicle_id||null,data_channel_ready_state:terminalChannel?.readyState||'none',buffered_amount_bytes:Math.max(0,Number(terminalChannel?.bufferedAmount)||0)});
      if(controlChannel===terminalChannel)controlChannel=null;
      lastVehicleSafetyState='';
      resetControlAuthorityInput();
      resetControlProfileSession();
    }
    if(connectionState==='connected'&&controlChannel?.readyState==='open')webrtcLabel.textContent='控制链路已连接';
    renderMonitoring();
  };
  nextPeer.onicecandidateerror=e=>{if(current())clientLog('webrtc_ice_candidate_error',{endpoint:safeIceEndpoint(e.url),error_code:Number(e.errorCode||0),...controlLogic.iceServerError(e)})};
  nextPeer.ondatachannel=e=>{
    const channel=e.channel;
    if(!current()){channel.close();return}
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
      if(!current()||!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      gearRejectionInhibited=false;
      webrtcLabel.textContent='控制链路已连接';
      resetControlAuthorityInput();
      resetControlProfileSession();
      lastVehicleSafetyState='';
      gearChangeStationaryEvidence=controlLogic.createGearChangeStationaryEvidence();
      vcuHandshake={supported:false,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:true,adapter_ready:null};
      clientLog('control_datachannel_open');
      renderMonitoring();
    };
    channel.onmessage=async event=>{
      if(!current()||!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      try{
        const message=JSON.parse(event.data);
        if(message.event==='vehicle_control_result'){if(message.emergency_stop)emergencyStopCourier.receive(message);controlTransportCourier.receive(message);return}
        if(message.event==='vehicle_control_status'){controlTransportCourier.receive(message);return}
        applyVehicleControlMessage(message);
      }catch(error){if(current())clientLog('control_datachannel_message_invalid',{error:error.message})}
    };
    channel.onclose=()=>{
      if(!current()||!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      flushControlTrace('datachannel_close');
      controlChannel=null;
      resetControlAuthorityInput();
      if(!controlTransportCourier.negotiated())resetControlProfileSession();
      vehicleTelemetry=null;vehicleTelemetryReceivedAt=null;lastFeedbackHealthLogAt=null;lastFeedbackHealthKey='';
      lastVehicleSafetyState='';
      vcuHandshake={issue_code:vcuHandshake.issue_code,control_recovery:vcuHandshake.control_recovery,supported:controlTransportCourier.negotiated()&&vcuHandshake.supported===true,state:'unavailable',ready:false,requested:false,disarming:false,parking_ready:false,driver_connected:false,adapter_ready:null};
      webrtcLabel.textContent='控制链路中断';
      clientLog('control_datachannel_closed');
      renderMonitoring();
    };
    channel.onerror=()=>{
      if(!current()||!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;
      flushControlTrace('datachannel_error');
      clientLog('control_datachannel_error',{ready_state:channel.readyState,buffered_amount_bytes:Math.max(0,Number(channel.bufferedAmount)||0),peer_connection_state:nextPeer.connectionState});
      resetControlAuthorityInput();
      if(!controlTransportCourier.negotiated())resetControlProfileSession();
      lastVehicleSafetyState='';
      webrtcLabel.textContent='控制链路错误';
      renderMonitoring();
    };
  };
  nextPeer.onicecandidate=e=>{if(!current()||!e.candidate)return;const candidate=e.candidate.toJSON();clientLog('webrtc_local_ice_candidate_discovered',controlLogic.iceCandidateSummary(candidate));post('/api/webrtc/ice-candidate',{source_session_id:sourceSession,source_control_session_generation:sourceGeneration,media_negotiation_id:nextScope.id,candidate}).catch(error=>{if(current())clientLog('webrtc_ice_signal_error',{stage:'candidate',error:error.message})})};
  nextPeer.ontrack=e=>{if(!current()||e.track.kind!=='video')return;const mid=e.transceiver.mid||'',id=offeredCameraByMid.get(mid)||remoteCameraIds.find(cameraId=>!assignedCameraIds.has(cameraId))||mid||e.track.id;assignedCameraIds.add(id);cameraByMid.set(mid,id);attach(id,e.track)};
  try{
    await nextPeer.setRemoteDescription({type:'offer',sdp:offer.sdp});
    if(!current())return;
    const candidates=pendingIce;pendingIce=[];
    for(const candidate of candidates){if(!current())return;if(controlLogic.iceCandidateMatchesOffer(candidate,nextScope))await addIce(candidate);else pendingIce.push(candidate)}
    const answer=await nextPeer.createAnswer();
    if(!current())return;
    await nextPeer.setLocalDescription(answer);
    if(!current())return;
    await post('/api/webrtc/answer',{type:'answer',sdp:nextPeer.localDescription.sdp,media_negotiation_id:nextScope.id,source_session_id:sourceSession,source_control_session_generation:sourceGeneration});
  }catch(error){if(current())throw error}
}
const controlTransportCourier=MineTeleopControlTransport.createCourier({
  scope:()=>latestRuntimeStatus.native_control?.control_transport?.enabled===true&&polling&&!controlAuthorityLost&&nativeControlSessionId&&mediaOfferScope?.id?{
    session:nativeControlSessionId,generation:nativeControlSessionGeneration,ui:uiInstanceId,uiEpoch:nativeControlUiEpoch,
    attempt:mediaOfferScope.id,...MineTeleopControlTransport.dataChannelRoute(peer,controlChannel,latestRuntimeStatus.native_control.control_transport.mode,latestRuntimeStatus.native_control.control_transport.experimental_enabled===true)}:null,
  post,now:()=>performance.now(),onStatus:message=>applyVehicleControlMessage(message,true),
  onPending:isCurrent=>controlTransportScheduler.request(isCurrent),
  onTrace:record=>{if(!controlTraceEnabled)return;const item={...record,browser_event_at_utc_ms:Date.now()};if(record.session_id!==controlTraceScope.session_id){emitControlTraceBatch('late_courier',{session_id:record.session_id},[item],createControlTraceSummary(),false);return}controlTraceBuffer.push(item);if(controlTraceBuffer.length>=48)flushControlTrace('capacity')},
  send:(frame,bytes)=>{const channel=controlChannel;if(channel?.readyState!=='open')return {reason:'channel_not_open'};if(Number(channel.bufferedAmount)!==0)return {reason:'channel_buffer_not_empty'};channel.send(bytes);return true},
  onBoundary:()=>{clearControlInput(false,'control_transport_boundary',true);lastNativeIntentSnapshot=''},
});
const controlTransportScheduler=MineTeleopControlTransport.createTickScheduler({
  tick:()=>controlTransportCourier.tick(),beforeTick:()=>controlTransportCourier.cancelStaleRequest(),now:()=>performance.now(),
  onError:error=>clientLog('control_transport_bridge_error',{error:error.message}),
});
setInterval(()=>controlTransportScheduler.tick(),25);
const emergencyStopCourier=MineTeleopControlTransport.createEmergencyCourier({
  onPending:isCurrent=>emergencyStopScheduler.request(isCurrent),
  scope:()=>latestRuntimeStatus.native_control?.emergency_stop_capability===MineTeleopControlTransport.emergencyProtocol&&polling&&nativeControlSessionId&&mediaOfferScope?.id?{
    session:nativeControlSessionId,generation:nativeControlSessionGeneration,ui:uiInstanceId,uiEpoch:nativeControlUiEpoch,
    attempt:mediaOfferScope.id,ready:Boolean(controlChannel?.readyState==='open')}:null,
  post,now:()=>performance.now(),send:bytes=>{
    const channel=controlChannel;
    if(channel?.readyState!=='open')return;
    const buffered=Number(channel.bufferedAmount),threshold=Number(channel.bufferedAmountLowThreshold);
    if(!Number.isFinite(buffered)||buffered<0||!Number.isFinite(threshold)||threshold<0)return;
    if(buffered>threshold)return MineTeleopControlTransport.emergencyBackpressure;
    channel.send(bytes);return true;
  },
  onConfirmation:seq=>{clientLog('driver_emergency_software_latched',{command_seq:seq,physical_execution_confirmed:false});statusPanel.textContent='车端已锁存急停；执行机构状态仍需查看新鲜 CAN 反馈。'},
});
const emergencyStopScheduler=MineTeleopControlTransport.createTickScheduler({
  tick:()=>emergencyStopCourier.tick(),beforeTick:()=>emergencyStopCourier.cancelStaleRequest(),now:()=>performance.now(),
  onError:error=>clientLog('emergency_stop_bridge_error',{error:error.message}),
});
setInterval(()=>emergencyStopScheduler.tick(),25);

setInterval(()=>cameraView.refresh(),500);
async function collectMetrics(){
  if(!peer)return;
  const instance=peer,epoch=mediaPeerEpoch,sourceSession=nativeControlSessionId,sourceGeneration=nativeControlSessionGeneration,sourceOfferId=mediaOfferScope?.id||'';
  const current=()=>currentMediaPeer(instance,epoch,sourceSession,sourceGeneration);
  try{
  const report=await instance.getStats(),sampledAt=Date.now();
  if(!current())return;
  let rtt=null,connectionMethod='unknown',turnInUse=false;
  const selection=controlLogic.selectedIcePair(report),selectedPair=selection.pair;
  const networkPath=controlLogic.iceNetworkPath(report,selectedPair);
  if(selectedPair){rtt=typeof selectedPair.currentRoundTripTime==='number'&&Number.isFinite(selectedPair.currentRoundTripTime)&&selectedPair.currentRoundTripTime>=0?selectedPair.currentRoundTripTime:null;const local=report.get(selectedPair.localCandidateId),remote=report.get(selectedPair.remoteCandidateId),types=[local?.candidateType,remote?.candidateType];turnInUse=types.includes('relay');connectionMethod=turnInUse?'TURN':networkPath==='unknown'?'unknown':(types.some(type=>type==='srflx'||type==='prflx')?'STUN':'direct')}
  const streams=[];
  for(const s of report.values()){
    if(s.type!=='inbound-rtp'||(s.kind||s.mediaType)!=='video')continue;
    const statsKey=s.mid||String(s.ssrc||s.id),prior=previousStats.get(statsKey),decoded=Number(s.framesDecoded||0),bytesReceived=Number(s.bytesReceived||0),packetsLost=Number(s.packetsLost||0),packetsReceived=Number(s.packetsReceived||0);
    let fps=Number(s.framesPerSecond||0),bitrateKbps=0;
    if(prior){const seconds=(sampledAt-prior.sampledAt)/1000;if(seconds>0){if(!fps)fps=(decoded-prior.framesDecoded)/seconds;bitrateKbps=Math.max(0,(bytesReceived-prior.bytesReceived)*8/seconds/1000)}}
    const packetDelta=prior?Math.max(0,packetsReceived-prior.packetsReceived):null,lostDelta=prior?Math.max(0,packetsLost-prior.packetsLost):null;
    previousStats.set(statsKey,{sampledAt,framesDecoded:decoded,bytesReceived,packetsReceived,packetsLost});

    const cameraId=cameraByMid.get(s.mid||'')||'',lane=(mediaStatus.lanes||[]).find(l=>l.camera_id===cameraId)||{};
    const captureEncodeMs=lane.capture_to_encoded_valid===false||typeof lane.capture_to_encoded_ms!=='number'?null:lane.capture_to_encoded_ms;
    const latency=controlLogic.receiverLatency(s,captureEncodeMs,rtt),{jitterMs,processingMs,latencyMs}=latency;
    streams.push({camera_id:cameraId,mid:s.mid||'',codec_id:s.codecId||'',fps,bitrate_kbps:bitrateKbps,frames_decoded:decoded,key_frames_decoded:s.keyFramesDecoded??null,frames_received:s.framesReceived??null,frames_dropped:Number(s.framesDropped||0),nack_count:s.nackCount??null,pli_count:s.pliCount??null,fir_count:s.firCount??null,decoder_implementation:s.decoderImplementation??null,decode_ms_per_frame:decoded>0&&typeof s.totalDecodeTime==='number'?s.totalDecodeTime*1000/decoded:null,codec_mime_type:report.get(s.codecId)?.mimeType||'',packets_lost:packetsLost,packets_received:packetsReceived,packet_loss_percent:(packetsLost+packetsReceived)>0?100*packetsLost/(packetsLost+packetsReceived):0,jitter_ms:Number(s.jitter||0)*1000,capture_to_encoded_ms:captureEncodeMs,jitter_buffer_ms:jitterMs,processing_ms:processingMs,receiver_latency_method:latency.method,round_trip_ms:rtt===null?null:rtt*1000,interval_packets_received:packetDelta,interval_packets_lost:lostDelta,interval_packet_loss_percent:packetDelta!==null&&packetDelta+lostDelta>0?100*lostDelta/(packetDelta+lostDelta):null,freeze_count:s.freezeCount??null,total_freezes_duration_s:s.totalFreezesDuration??null,last_packet_received_timestamp_ms:s.lastPacketReceivedTimestamp??null,total_decode_time_s:s.totalDecodeTime??null,total_processing_delay_s:s.totalProcessingDelay??null,estimated_end_to_end_latency_ms:latencyMs,passed:fps>=20&&latencyMs!==null&&latencyMs<=200})
  }
  const timeSync=latestRuntimeStatus.time_sync||mediaStatus.time_sync||{},turnConfigured=hasTurnServer();
  const controlOutcomes={...controlOutcomeSession.metrics};
  const metrics={media_negotiation_id:typeof mediaOfferScope==='undefined'?'':mediaOfferScope?.id||'',network_path:networkPath,media_transport_rtt_ms:rtt===null?null:rtt*1000,control_rtt_source:'not_measured_by_media_stats',ice_connectivity:controlLogic.iceConnectivity(report,selectedPair),sampled_at_ms:sampledAt,connection_state:peer.connectionState,codec:mediaStatus.codec||'',backend:mediaStatus.backend||'',control_rtt_ms:null,selected_candidate_pair:selectedPair?{id:selectedPair.id,selection_method:selection.method,state:selectedPair.state,nominated:selectedPair.nominated??null,transport_id:selection.transport?.id??selectedPair.transportId??null,local_candidate_type:report.get(selectedPair.localCandidateId)?.candidateType??null,remote_candidate_type:report.get(selectedPair.remoteCandidateId)?.candidateType??null,protocol:report.get(selectedPair.localCandidateId)?.protocol??null,relay_protocol:report.get(selectedPair.localCandidateId)?.relayProtocol??null,current_round_trip_time_s:selectedPair.currentRoundTripTime??null,available_outgoing_bitrate_bps:selectedPair.availableOutgoingBitrate??null,bytes_sent:selectedPair.bytesSent??null,bytes_received:selectedPair.bytesReceived??null,requests_sent:selectedPair.requestsSent??null,responses_received:selectedPair.responsesReceived??null}:null,connection_method:connectionMethod,turn_configured:turnConfigured,turn_in_use:turnInUse,time_sync:timeSync,clock_uncertainty_ms:Number(timeSync.uncertainty_ms||0),latency_method:'capture-to-encoded + rtt/2 + receiver-processing (buffer + decode fallback)',control_outcomes:controlOutcomes,control_outcomes_balanced:controlLogic.controlOutcomesBalanced(controlOutcomes),streams,passed:streams.length>0&&streams.every(s=>s.passed)};
  const mediaLogAt=performance.now();
  metrics.presentation=cameraView.presentationSnapshot();
  metrics.source_session_id=sourceSession;metrics.source_control_session_generation=sourceGeneration;metrics.media_negotiation_id=sourceOfferId;
  if(lastMediaMetricsLogAt===null||mediaLogAt-lastMediaMetricsLogAt>=5000){lastMediaMetricsLogAt=mediaLogAt;clientLog('driver_media_metrics',metrics);clientLog('driver_ice_connectivity',{ice_transport_policy:consoleConfig.ice_transport_policy,ice_connection_state:peer.iceConnectionState,ice_gathering_state:peer.iceGatheringState,...controlLogic.iceConnectivity(report,selectedPair)})}
  await post('/api/webrtc/metrics',metrics);
  if(!current())return;
  if(metrics.codec==='h265'&&metrics.connection_state==='connected'&&streams.length){h265FailureSamples=streams.some(s=>s.fps<20)?h265FailureSamples+1:0;if(h265FailureSamples>=3&&!h265FallbackSent){h265FallbackSent=true;await post('/api/webrtc/fallback',{codec:'h264',reason:'h265_decode_fps_below_20',media_negotiation_id:mediaOfferScope?.id||'',source_session_id:sourceSession,source_control_session_generation:sourceGeneration})}}else h265FailureSamples=0;
  if(!current())return;
  latestMetrics=metrics;renderMonitoring();statusPanel.textContent=`${metrics.connection_state||'等待连接'} · ${streams.length} 路视频 · 媒体 ICE RTT ${formatMetric(metrics.media_transport_rtt_ms,1,' ms')} · ${metrics.connection_method||'unknown'}`
  }catch(error){if(current())throw error}
}
setInterval(()=>collectMetrics().catch(console.error),1000);
setInterval(()=>refreshRuntimeStatus().catch(console.error),1000);
setInterval(()=>flushControlTrace('interval'),1000);
setInterval(()=>refreshControlIntent().catch(console.error),intentRefreshIntervalMs);
setInterval(()=>{if(authenticated&&!connecting)refreshVehicles().catch(handleVehicleRefreshError)},5000);
</script><script src="/assets/console-layout.js" defer></script></body></html>)HTML";
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
    if (entry["mock_test_login_ttl_ms"]) {
      try {
        config.mock_test_login_ttl_ms.emplace(driver_id, entry["mock_test_login_ttl_ms"].as<std::int64_t>());
      } catch (const YAML::Exception&) {
        throw std::invalid_argument(context + ".mock_test_login_ttl_ms must be an integer");
      }
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
    if (entry["mobile_approval_required"]) {
      try {
        if (entry["mobile_approval_required"].as<bool>()) config.mobile_approval_vehicles.insert(vehicle_id);
      } catch (const YAML::Exception&) {
        throw std::invalid_argument(context + ".mobile_approval_required must be a boolean");
      }
    }
    if (!config.device_tokens.emplace(vehicle_id, token).second) {
      throw std::invalid_argument("duplicate vehicle id: " + vehicle_id);
    }
  }
  if (auth["mobile_approval_timeout_ms"]) {
    try {
      config.mobile_approval_timeout_ms = auth["mobile_approval_timeout_ms"].as<std::int64_t>();
    } catch (const YAML::Exception&) {
      throw std::invalid_argument("auth.mobile_approval_timeout_ms must be an integer");
    }
  }
  if (auth["approver_token_ttl_ms"]) {
    try {
      config.approver_token_ttl_ms = auth["approver_token_ttl_ms"].as<std::int64_t>();
    } catch (const YAML::Exception&) {
      throw std::invalid_argument("auth.approver_token_ttl_ms must be an integer");
    }
  }
  const auto app_password_file = optional_yaml_string(auth, "mobile_app_password_file", "auth");
  const auto default_app_password_file = base_path / "secrets/app-token";
  if (!config.mobile_approval_vehicles.empty() || app_password_file.has_value() ||
      std::filesystem::exists(default_app_password_file)) {
    YAML::Node app_secret;
    app_secret["password_file"] = app_password_file.value_or("secrets/app-token");
    config.mobile_app_password = load_identity_secret(
        app_secret, "password_file", "password_env", base_path, "mobile app");
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

std::string diagnostic_fingerprint(std::string_view value, std::string_view key) {
  if (key.empty() || key.size() > 4096) throw std::invalid_argument("diagnostic fingerprint key is invalid");
  std::array<unsigned char, 32> digest{};
#if defined(__APPLE__)
  CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), value.data(), value.size(), digest.data());
#else
  unsigned int length = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
          reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data(), &length) || length != digest.size())
    throw std::runtime_error("diagnostic fingerprint failed");
#endif
  std::ostringstream output;
  output << std::hex << std::setfill('0');
  for (std::size_t index = 0; index < 16; ++index) output << std::setw(2) << static_cast<int>(digest[index]);
  return output.str();
}

std::string detail::control_transport_mac(std::string_view value, std::string_view key) {
  if (key.size() < 32 || key.size() > 4096) throw std::invalid_argument("control transport signing key unavailable");
  std::array<unsigned char, 32> digest{};
#if defined(__APPLE__)
  CCHmac(kCCHmacAlgSHA256, key.data(), key.size(), value.data(), value.size(), digest.data());
#else
  unsigned int length = 0;
  if (!HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
          reinterpret_cast<const unsigned char*>(value.data()), value.size(), digest.data(), &length) || length != digest.size())
    throw std::runtime_error("control transport signing failed");
#endif
  std::ostringstream output; output << std::hex << std::setfill('0');
  for (auto byte : digest) output << std::setw(2) << static_cast<int>(byte);
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
      diagnostic_fingerprint_key_(random_token()),
      audit_clock_(std::move(audit_clock)) {
  if (config_.experimental_direct_authority_lease_ms < 0)
    throw std::invalid_argument("direct authority lease must be explicitly nonnegative");
  if (config_.experimental_direct_authority_lease_ms > 0 || !config_.experimental_direct_authority_journal.empty()) {
    const auto& path = config_.experimental_direct_authority_journal;
    if (path.empty() || !path.is_absolute() || !std::filesystem::is_directory(path.parent_path()))
      throw std::invalid_argument("direct authority requires an existing absolute journal directory");
    const auto lock_path = path.string() + ".lock";
#if defined(_WIN32)
    const auto handle = CreateFileA(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) throw std::invalid_argument("direct authority journal already owned or unavailable");
    direct_authority_journal_lock_ = std::shared_ptr<void>(handle, [](void* file) { CloseHandle(file); });
#else
    const auto fd = ::open(lock_path.c_str(), O_CREAT | O_RDWR, 0600);
    if (fd < 0) throw std::invalid_argument("direct authority journal lock unavailable");
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) { ::close(fd); throw std::invalid_argument("direct authority journal already owned"); }
    direct_authority_journal_lock_ = std::shared_ptr<void>(new int(fd), [](void* raw) { const auto value = static_cast<int*>(raw); ::close(*value); delete value; });
#endif
    if (std::filesystem::exists(path)) {
      if (std::filesystem::file_size(path) > 1024 * 1024) throw std::invalid_argument("direct authority journal too large");
      std::ifstream stream(path); direct_authority_journal_ = Json::parse(stream);
      if (!direct_authority_journal_.is_object()) throw std::invalid_argument("invalid direct authority journal");
      for (const auto& [vehicle, record] : direct_authority_journal_.items()) {
        if (!config_.device_tokens.contains(vehicle)) throw std::invalid_argument("unknown vehicle in authority journal");
        const auto restored = detail::DirectAuthorityGrant::parse(record);
        const auto duration = restored.duration_ms;
        const auto epoch = restored.epoch;
        if (duration <= 0 || duration > std::numeric_limits<std::int64_t>::max() - monotonic_now_ms())
          throw std::invalid_argument("invalid authority journal duration");
        // Restart/clock-step conservative: wait a WHOLE prior granted duration
        // on this boot's monotonic clock, rather than trusting stale UTC expiry.
        direct_authority_reservations_[vehicle] = monotonic_now_ms() + duration;
        direct_authority_next_epoch_ = std::max(direct_authority_next_epoch_, epoch);
      }
    }
  }
  if (config_.mobile_approval_timeout_ms <= 0 || config_.mobile_approval_timeout_ms > 600000) {
    throw std::invalid_argument("mobile approval timeout must be between 1 and 600000 ms");
  }
  if (!config_.mobile_approval_vehicles.empty() && config_.mobile_app_password.empty()) {
    throw std::invalid_argument("mobile approval requires a non-empty mobile app password");
  }
  for (const auto& vehicle : config_.mobile_approval_vehicles) {
    if (!config_.device_tokens.contains(vehicle)) throw std::invalid_argument("unknown mobile approval vehicle");
  }
  if (config_.approver_token_ttl_ms <= 0 || config_.approver_token_ttl_ms > 7LL * 24 * 60 * 60 * 1000) {
    throw std::invalid_argument("approver token TTL must be between 1ms and 7 days");
  }
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
      config_.max_ice_candidate_bytes == 0 || config_.signaling_message_ttl_ms <= 0 ||
      config_.native_control_message_ttl_ms <= 0 ||
      config_.native_control_message_ttl_ms > 1000) {
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
  for (const auto& [driver_id, ttl_ms] : config_.mock_test_login_ttl_ms) {
    const auto permissions = config_.driver_vehicle_permissions.find(driver_id);
    if (!config_.driver_passwords.contains(driver_id) ||
        permissions == config_.driver_vehicle_permissions.end() ||
        permissions->second != std::unordered_set<std::string>{"test_bench_001"}) {
      throw std::invalid_argument("mock login TTL requires an existing driver authorized only for test_bench_001");
    }
    if (ttl_ms <= 0 || ttl_ms > 4LL * 60 * 60 * 1000) {
      throw std::invalid_argument("mock login TTL must be between 1ms and 4 hours");
    }
  }
  if (config_.native_control_trace_commands && !config_.audit_log_path.empty()) {
    control_connection_observers_ = std::make_shared<ControlConnectionObserverRegistry>();
    native_control_trace_ = std::make_unique<AsyncControlTrace>(
        [this](Json details) {
          audit("cloud_native_control_trace_batch", std::move(details));
        }, [registry = control_connection_observers_] { return registry->sample(); });
  }
  audit(
      "signaling_service_started",
      {{"runtime", "cpp"},
       {"native_control_capabilities", kNativeControlCapabilities},
       {"native_control_trace_commands", config_.native_control_trace_commands},
       {"audit_log_max_bytes", config_.audit_log_max_bytes},
       {"audit_log_files", config_.audit_log_files},
       {"audit_log_rotation_interval_ms", config_.audit_log_rotation_interval_ms},
       {"audit_log_retention_days", config_.audit_log_retention_days}});
  session_logs_ = std::make_unique<SessionLogBroker>(config_.audit_log_path);
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
  if (native_control_trace_) native_control_trace_->stop();
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
      {"native_control_capabilities", kNativeControlCapabilities},
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
  cleanup_mobile_approvals();
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
  session.transport_mac_key.clear();
  session.control_token_expires_at_ms = 0;
  messages_.erase(message_key(session.session_id, session.driver_id));
  messages_.erase(message_key(session.session_id, session.vehicle_id));
  latest_vehicle_control_feedback_.erase(session.session_id);
  latest_vehicle_transport_results_.erase(session.session_id);
  latest_vehicle_transport_status_.erase(session.session_id);
  latest_control_messages_.erase(message_key(session.session_id, session.driver_id));
  latest_control_messages_.erase(message_key(session.session_id, session.vehicle_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.driver_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.vehicle_id));
  last_accepted_messages_.erase(message_key(session.session_id, session.driver_id) + ":native_control");
  next_delivery_cursors_.erase(message_key(session.session_id, session.driver_id));
  next_delivery_cursors_.erase(message_key(session.session_id, session.vehicle_id));
  if (session.state != SessionState::Stopping) transition_session(session, SessionState::Stopping, reason);
  transition_session(session, SessionState::Closed, reason);
  for (const auto& participant : {session.driver_id, session.vehicle_id}) {
    notify_signaling_recipient(session.session_id, participant);
    signaling_wakeups_.erase(message_key(session.session_id, participant));
  }
  remember_log_session(session, now_ms());
}

bool SignalingService::direct_authority_vehicle_reserved(std::string_view vehicle) const {
  const auto found = direct_authority_reservations_.find(std::string(vehicle));
  return found != direct_authority_reservations_.end() && monotonic_now_ms() < found->second;
}

void SignalingService::persist_direct_authority_state(const Json& snapshot) {
  const auto& path = config_.experimental_direct_authority_journal;
  const auto temporary = path.string() + ".tmp";
  const auto data = snapshot.dump();
  FILE* file = std::fopen(temporary.c_str(), "wb");
  if (!file) throw ServiceUnavailable("direct authority journal cannot be written");
  bool saved = std::fwrite(data.data(), 1, data.size(), file) == data.size() && std::fflush(file) == 0;
#if defined(_WIN32)
  saved = saved && ::_commit(::_fileno(file)) == 0;
#else
  saved = saved && ::fsync(::fileno(file)) == 0;
#endif
  saved = std::fclose(file) == 0 && saved;
  if (!saved) throw ServiceUnavailable("direct authority journal flush failed");
#if defined(_WIN32)
  if (!MoveFileExA(temporary.c_str(), path.string().c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    throw ServiceUnavailable("direct authority journal commit failed");
#else
  std::error_code error; std::filesystem::rename(temporary, path, error);
  if (error) throw ServiceUnavailable("direct authority journal commit failed");
  const auto directory = ::open(path.parent_path().c_str(), O_RDONLY);
  if (directory < 0) throw ServiceUnavailable("direct authority journal directory unavailable");
  const auto synced = ::fsync(directory); ::close(directory);
  if (synced != 0) throw ServiceUnavailable("direct authority journal directory flush failed");
#endif
}

void SignalingService::remember_log_session(const Session& session, std::int64_t ended_at) {
  session_logs_->remember({{"key", service_instance_id_ + "_" + session.session_id},
      {"service_instance_id", service_instance_id_}, {"session_id", session.session_id},
      {"driver_id", session.driver_id}, {"vehicle_id", session.vehicle_id},
      {"started_at_utc_ms", session.started_at_utc_ms}, {"ended_at_utc_ms", ended_at}});
}

ServerResponse SignalingService::handle_diagnostics(const HttpRequest& request) {
  if (request.method != "POST") return ServerResponse::json(405, {{"error", "POST required"}});
  const auto value = request.json_body();
  const auto operation = required_string(value, "operation");
  if (request.path == "/sessions/logs/vehicle") {
    const auto vehicle = required_string(value, "vehicle_id");
    bool idle = true;
    {
      std::lock_guard lock(mutex_);
      validate_device_token(vehicle, optional_string(value, "device_token"));
      for (const auto& [id, session] : sessions_) {
        (void)id;
        if (session.vehicle_id == vehicle && session.state != SessionState::Closed) idle = false;
      }
    }
    return ServerResponse::json(200, session_logs_->vehicle(vehicle, value, idle));
  }
  if (request.path != "/sessions/logs/driver") return ServerResponse::json(404, {{"error", "not found"}});
  const auto driver = required_string(value, "driver_id");
  std::unordered_set<std::string> allowed;
  {
    std::lock_guard lock(mutex_);
    validate_driver_token(driver, optional_string(value, "token"));
    const auto permissions = config_.driver_vehicle_permissions.find(driver);
    if (permissions != config_.driver_vehicle_permissions.end()) allowed = permissions->second;
  }
  if (operation == "list") {
    auto records = session_logs_->sessions(driver);
    std::erase_if(records.get_ref<Json::array_t&>(), [&](const Json& r) {
      return !allowed.contains(r.at("vehicle_id").get<std::string>());
    });
    return ServerResponse::json(200, {{"sessions", records}});
  }
  if (operation == "start") {
    const auto record = session_logs_->session(required_string(value, "key"), driver);
    const auto vehicle = record.at("vehicle_id").get<std::string>();
    if (!allowed.contains(vehicle)) throw Unauthorized("vehicle permission was revoked");
    bool online;
    {
      std::lock_guard lock(mutex_);
      cleanup_expired_connections(now_ms());
      online = online_vehicles_.contains(vehicle);
      for (const auto& [id, session] : sessions_) {
        (void)id;
        if ((session.vehicle_id == vehicle || session.driver_id == driver) && session.state != SessionState::Closed)
          throw Conflict("end the active control session before collecting logs");
      }
    }
    return ServerResponse::json(200, session_logs_->start(record, online));
  }
  const auto id = required_string(value, "export_id");
  // Check ownership and current permissions on every download, not only at creation.
  const auto state = session_logs_->status(id, driver);
  if (!allowed.contains(state.at("session").at("vehicle_id").get<std::string>()))
    throw Unauthorized("vehicle permission was revoked");
  if (operation == "status") return ServerResponse::json(200, session_logs_->status(id, driver, value.value("finish_partial", false)));
  if (operation == "chunk") return ServerResponse::json(200, session_logs_->chunk(id, driver, value));
  if (operation == "release") {
    session_logs_->release(id, driver);
    return ServerResponse::json(200, {{"released", true}});
  }
  throw std::invalid_argument("unknown log export operation");
}

std::string SignalingService::approver_login_bucket(const HttpRequest& request, std::int64_t timestamp_ms) {
  if (approver_login_last_cleanup_ms_ == 0 || timestamp_ms < approver_login_last_cleanup_ms_ ||
      timestamp_ms - approver_login_last_cleanup_ms_ >= config_.login_failure_window_ms) {
    std::erase_if(login_failures_, [&](const auto& entry) {
      if (!entry.first.starts_with("approver:")) return false;
      const auto& state = entry.second;
      return state.blocked_until_ms > 0 ? timestamp_ms >= state.blocked_until_ms
          : timestamp_ms - state.window_started_at_ms >= config_.login_failure_window_ms;
    });
    approver_login_last_cleanup_ms_ = timestamp_ms;
  }
  const auto source = request_source(request);
  if (login_failures_.contains("approver:" + source)) return source;
  const auto count = std::count_if(login_failures_.begin(), login_failures_.end(), [](const auto& entry) {
    return entry.first.starts_with("approver:") && entry.first != "approver:<overflow>";
  });
  // Same bounded overflow policy as API rate limiting; existing sources retain their own buckets.
  return count < config_.api_rate_limit_max_sources ? source : "<overflow>";
}

void SignalingService::enforce_login_rate_limit(std::string_view driver_id, std::int64_t timestamp_ms, bool approver) {
  const bool known_driver = !approver && config_.driver_passwords.contains(std::string(driver_id));
  const std::string bucket = approver ? "approver:" + std::string(driver_id) : (known_driver ? "driver:" + std::string(driver_id) : "unknown");
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

void SignalingService::record_login_failure(std::string_view driver_id, std::int64_t timestamp_ms, bool approver) {
  const bool known_driver = !approver && config_.driver_passwords.contains(std::string(driver_id));
  const std::string bucket = approver ? "approver:" + std::string(driver_id) : (known_driver ? "driver:" + std::string(driver_id) : "unknown");
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
  const Json identity = {
      {approver ? "approver_id" : "driver_id", known_driver ? std::string(driver_id) : "<unknown>"},
      {approver ? "recognized_approver" : "recognized_driver", known_driver}};
  auto failed_details = identity;
  if (approver) failed_details["source_address"] = std::string(driver_id);
  failed_details["failure_count"] = state.failures;
  failed_details["failure_limit"] = config_.login_max_failures;
  audit(approver ? "approver_login_failed" : "driver_login_failed", failed_details);
  if (!lock_login) return;

  auto limited_details = identity;
  if (approver) limited_details["source_address"] = std::string(driver_id);
  limited_details["failure_count"] = state.failures;
  limited_details["blocked_until_utc_ms"] = state.blocked_until_ms;
  audit(approver ? "approver_login_rate_limited" : "driver_login_rate_limited", limited_details);
  throw TooManyRequests("too many login attempts", config_.login_lockout_ms);
}

void SignalingService::clear_login_failures(std::string_view driver_id, bool approver) {
  login_failures_.erase((approver ? "approver:" : "driver:") + std::string(driver_id));
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

bool SignalingService::try_audit(std::string_view event, const Json& details) const noexcept {
  try {
    audit(event, details);
    return true;
  } catch (...) {
    return false;
  }
}

Json SignalingService::MobileApproval::to_json() const {
  return {{"request_id", request_id}, {"vehicle_id", vehicle_id}, {"driver_id", driver_id},
          {"state", state}, {"decided_by", decided_by}, {"expires_at_utc_ms", expires_at_ms},
          {"remaining_ms", std::max<std::int64_t>(0, std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()).count())}};
}

void SignalingService::cleanup_mobile_approvals() {
  for (auto& [vehicle_id, approval] : mobile_approvals_) {
    if (approval.state != "pending" && approval.state != "approved" && approval.state != "rejected") continue;
    const auto driver = online_drivers_.find(approval.driver_id);
    const auto vehicle = online_vehicles_.find(vehicle_id);
    std::string next;
    if (driver == online_drivers_.end() || vehicle == online_vehicles_.end() ||
        driver->second.generation != approval.driver_generation ||
        vehicle->second.generation != approval.vehicle_generation ||
        revoked_drivers_.contains(approval.driver_id) || revoked_vehicles_.contains(vehicle_id)) {
      next = "cancelled";
    } else if (std::chrono::steady_clock::now() >= approval.deadline) {
      next = "expired";
    }
    if (!next.empty()) {
      approval.state = next;
      // Revocation must survive an unavailable audit sink, including on the
      // reaper thread. New approvals/grants still require successful auditing.
      try_audit("mobile_approval_" + next, approval.to_json());
    }
  }
  const auto timestamp = now_ms();
  for (auto it = approver_tokens_.begin(); it != approver_tokens_.end();) {
    if (timestamp >= it->second.expires_at_ms) it = approver_tokens_.erase(it);
    else ++it;
  }
}

void SignalingService::require_mobile_approval(
    const std::string& driver_id, const std::string& vehicle_id, std::string_view request_id) {
  if (!config_.mobile_approval_vehicles.contains(vehicle_id)) {
    if (!request_id.empty()) throw Conflict("车辆审批策略已变化，请重新申请连接", "mobile_approval_stale");
    return;
  }
  cleanup_mobile_approvals();
  auto found = mobile_approvals_.find(vehicle_id);
  if (!request_id.empty()) {
    if (found == mobile_approvals_.end() || found->second.request_id != request_id ||
        found->second.driver_id != driver_id) {
      throw Conflict("连接申请已失效，请重新申请", "mobile_approval_stale");
    }
    const auto& approval = found->second;
    if (approval.state != "pending" && approval.state != "approved" && approval.state != "rejected") {
      throw Conflict("手机审批已过期或申请已结束，请重新申请连接", "mobile_approval_ended", approval.to_json());
    }
  }
  if (found != mobile_approvals_.end()) {
    const auto& approval = found->second;
    const bool live = approval.state == "pending" || approval.state == "approved" || approval.state == "rejected";
    if (live && approval.driver_id != driver_id) {
      throw Conflict("该车辆已有手机审批申请，请稍后重试", "mobile_approval_busy");
    }
    if (live && approval.state == "approved") return;
    if (live && approval.state == "rejected") {
      throw Conflict("手机已拒绝本次申请，请在申请过期后重试", "mobile_approval_rejected", approval.to_json());
    }
    if (live) {
      throw Conflict("等待手机审批，同意后将自动连接", "mobile_approval_pending", approval.to_json());
    }
  }
  MobileApproval approval;
  approval.request_id = "approval-" + random_token();
  approval.vehicle_id = vehicle_id;
  approval.driver_id = driver_id;
  approval.driver_generation = online_drivers_.at(driver_id).generation;
  approval.vehicle_generation = online_vehicles_.at(vehicle_id).generation;
  approval.expires_at_ms = now_ms() + config_.mobile_approval_timeout_ms;
  approval.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.mobile_approval_timeout_ms);
  if (!try_audit("mobile_approval_requested", approval.to_json())) {
    throw ServiceUnavailable("audit log unavailable; approval request is disabled");
  }
  mobile_approvals_[vehicle_id] = approval;
  throw Conflict("已发送手机审批申请，同意后将自动连接", "mobile_approval_pending", approval.to_json());
}

ServerResponse SignalingService::handle_mobile_api(const HttpRequest& request) {
  cleanup_mobile_approvals();
  if (request.path == "/mobile/api/login" && request.method == "POST") {
    const auto value = request.json_body();
    const auto timestamp_ms = now_ms();
    const auto source = approver_login_bucket(request, timestamp_ms);
    enforce_login_rate_limit(source, timestamp_ms, true);
    if (config_.mobile_app_password.empty() || config_.mobile_app_password != optional_string(value, "password")) {
      record_login_failure(source, timestamp_ms, true);
      throw Unauthorized("App 密码错误");
    }
    clear_login_failures(source, true);
    // A random login-session label distinguishes concurrent phones in audit, without usernames.
    const auto id = "app-" + random_token(12);
    const auto token = "approver-token-" + random_token();
    const auto expiry = now_ms() + config_.approver_token_ttl_ms;
    approver_tokens_[token] = DriverToken{id, expiry, 0};
    audit("approver_login", {{"approver_id", id}});
    return ServerResponse::json(200, {{"token", token}, {"expires_at_utc_ms", expiry}, {"remaining_ms", std::max<std::int64_t>(0, expiry - now_ms())}});
  }
  // Mobile credentials are header-only, never URL parameters or driver/device tokens.
  const auto header = request.headers.find("x-mine-teleop-approver-token");
  const auto token = header == request.headers.end() ? std::string{} : header->second;
  const auto authenticated = approver_tokens_.find(token);
  if (authenticated == approver_tokens_.end()) throw Unauthorized("审批登录已失效，请重新登录");
  const auto id = authenticated->second.driver_id;
  const auto& permissions = config_.mobile_approval_vehicles;
  if (request.path == "/mobile/api/logout" && request.method == "POST") {
    approver_tokens_.erase(token);
    audit("approver_logout", {{"approver_id", id}});
    return ServerResponse::json(200, {{"state", "logged_out"}});
  }
  if (request.path == "/mobile/api/requests" && request.method == "GET") {
    Json requests = Json::array();
    std::vector<std::string> vehicles(permissions.begin(), permissions.end());
    std::sort(vehicles.begin(), vehicles.end());
    for (const auto& vehicle : vehicles) {
      if (const auto found = mobile_approvals_.find(vehicle); found != mobile_approvals_.end()) {
        requests.push_back(found->second.to_json());
      }
    }
    return ServerResponse::json(200, {{"requests", requests}});
  }
  const auto parts = path_parts(request.path);
  if (parts.size() == 5 && parts[0] == "mobile" && parts[1] == "api" &&
      parts[2] == "requests" && parts[4] == "decision" && request.method == "POST") {
    const auto value = request.json_body();
    const auto decision = required_string(value, "decision");
    if (decision != "approve" && decision != "reject") throw std::invalid_argument("decision must be approve or reject");
    auto found = std::find_if(mobile_approvals_.begin(), mobile_approvals_.end(), [&](const auto& entry) {
      return entry.second.request_id == parts[3] && permissions.contains(entry.first);
    });
    if (found == mobile_approvals_.end()) throw NotFound("审批申请不存在或无权访问");
    auto& approval = found->second;
    const std::string next = decision == "approve" ? "approved" : "rejected";
    // Same actor retry after an uncertain network response is idempotent. Other decisions conflict.
    if (approval.state == next && approval.decided_by == id) return ServerResponse::json(200, approval.to_json());
    if (approval.state != "pending") throw Conflict("申请已处理或失效，请刷新", "mobile_approval_not_pending", approval.to_json());
    auto decided = approval;
    decided.state = next;
    decided.decided_by = id;
    if (!try_audit("mobile_approval_decided", decided.to_json())) {
      throw ServiceUnavailable("audit log unavailable; approval decision was not saved");
    }
    approval = std::move(decided);
    return ServerResponse::json(200, approval.to_json());
  }
  return ServerResponse::json(404, {{"error", "not found"}});
}

ServerResponse SignalingService::handle(const HttpRequest& request) {
  RequestIdScope request_id("request-" + random_token(12));
  ServerResponse response;
  const auto entered_at_utc_ms = now_ms();
  const auto entered_at_monotonic_ms = monotonic_now_ms();
  std::int64_t api_gate_wait_ms = 0;
  try {
    {
      const auto started = monotonic_now_ms();
      std::lock_guard lock(mutex_);
      api_gate_wait_ms = monotonic_now_ms() - started;
      enforce_api_rate_limit(request, now_ms());
    }
    if (request.path.starts_with("/sessions/logs/")) {
      response = handle_diagnostics(request);
      response.headers.emplace_back("Cache-Control", "no-store");
    } else if (request.path.starts_with("/mobile/api/")) {
      std::lock_guard lock(mutex_);
      cleanup_expired_connections(now_ms());
      response = handle_mobile_api(request);
    } else if (request.method == "POST" && request.path.starts_with("/vehicles/") && request.path.ends_with("/direct-authority")) {
      response = handle_direct_authority_post(request);
    } else if (request.method == "GET") {
      response = handle_get(request);
      if (request.path == "/time" && response.status == 200) {
        auto body = Json::parse(response.body);
        body["server_receive_ms"] = request.received_at_utc_ms.value_or(entered_at_utc_ms);
        body["server_send_ms"] = now_ms();
        body["timestamp_scope"] = "http_request_received_to_response_ready";
        body["server_before_handler_ms"] = request.received_at_monotonic_ms
            ? Json(std::max<std::int64_t>(0, entered_at_monotonic_ms - *request.received_at_monotonic_ms)) : Json(nullptr);
        body["server_parse_ms"] = request.received_at_monotonic_ms && request.parsed_at_monotonic_ms
            ? Json(std::max<std::int64_t>(0, *request.parsed_at_monotonic_ms - *request.received_at_monotonic_ms)) : Json(nullptr);
        body["server_processing_monotonic_ms"] = monotonic_now_ms() - request.received_at_monotonic_ms.value_or(entered_at_monotonic_ms);
        body["server_api_gate_wait_ms"] = api_gate_wait_ms;
        response.body = body.dump();
      }
    } else if (request.method == "POST") {
      response = handle_post(request);
    } else {
      response = ServerResponse::json(405, {{"error", "method not allowed"}});
    }
  } catch (const Unauthorized& error) {
    response = ServerResponse::json(401, {{"error", error.what()}});
  } catch (const TooManyRequests& error) {
    response = too_many_requests_response(error);
  } catch (const ServiceUnavailable& error) {
    response = ServerResponse::json(503, {{"error", error.what()}});
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
  if ((request.path == "/auth/driver_login" || request.path.starts_with("/mobile/api/")) &&
      std::none_of(response.headers.begin(), response.headers.end(), [](const auto& header) {
        return lower(header.first) == "cache-control";
      })) {
    response.headers.emplace_back("Cache-Control", "no-store");
  }
  if (request.path.starts_with("/mobile/api/")) {
    response.headers.emplace_back("X-Content-Type-Options", "nosniff");
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
  Json authenticated_authority_scope;
  auto authenticate = [&] {
    std::lock_guard lock(mutex_);
    cleanup_expired_connections(now_ms());
    const auto& session = require_participant(parts[1], participant);
    validate_actor_credential(session, participant, credentials);
    if (session.direct_authority_enabled) authenticated_authority_scope = {
        {"protocol", detail::kDirectAuthorityCapability}, {"issuer", service_instance_id_},
        {"vehicle_id", session.vehicle_id}, {"driver_id", session.driver_id}, {"session_id", session.session_id},
        {"connection_generation", online_vehicles_.at(session.vehicle_id).generation}, {"authority_epoch", 0}};
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
            accept + "\r\nX-Request-ID: " + request_id.value() +
            "\r\nX-Mine-Teleop-Control-Capabilities: " + std::string(kNativeControlCapabilities) + "\r\nX-Mine-Teleop-Transport-Capabilities: " + std::string(detail::kControlTransportCapability) + "\r\n\r\n");
    ServerWebSocketConnection connection(socket, config_.max_signaling_payload_bytes);
    ScopedControlConnectionObservation observation{control_connection_observers_ && (send_only || control_receive_only)
        ? control_connection_observers_->observe(socket, parts[1], participant,
            send_only ? "cloud_control_ingress" : "cloud_control_delivery") : nullptr};
    const auto observe_phase = [&](ControlObservationPhase phase, std::uint64_t seq = 0) {
      if (observation.state) observation.state->begin(phase, seq);
    };
    const bool transport_protocol = query_value(request, "transport_protocol") == detail::kControlTransportCapability;
    std::shared_ptr<detail::SocketWakeup> wakeup;
    if (!send_only || transport_protocol) {
      wakeup = std::make_shared<detail::SocketWakeup>();
      std::lock_guard lock(mutex_);
      static_cast<void>(require_participant(parts[1], participant));
      auto& subscribers = signaling_wakeups_[message_key(parts[1], participant)];
      std::erase_if(subscribers, [](const auto& item) { return item.expired(); });
      subscribers.push_back(wakeup);
    }
    std::uint64_t last_vehicle_result_sent = 0, last_control_status_sent = 0;
    std::uint64_t last_vehicle_result_epoch_sent = 0, last_control_status_epoch_sent = 0;
    std::int64_t last_lease_expiry_sent = 0;
    std::uint64_t last_delivery_cursor_sent = 0;
    std::uint64_t last_control_seq_sent = 0;
    auto last_delivery_sent_at = std::chrono::steady_clock::time_point{};
    std::int64_t last_transport_sample_monotonic_ms = 0;
    std::int64_t last_receive_completed_monotonic_ms = 0;
    const auto socket_connection_started_monotonic_ms = observation.state ? observation.state->started() : monotonic_now_ms();
    while (true) {
      Json pending = Json::array();
      Json vehicle_transport_result;
      Json vehicle_transport_status = Json::array();
      std::int64_t authority_expiry = 0;
      const auto loop_mutex_wait_started = monotonic_now_ms();
      std::int64_t loop_mutex_acquired = 0;
      try {
        observe_phase(ControlObservationPhase::LoopMutex);
        std::lock_guard lock(mutex_);
        observe_phase(ControlObservationPhase::LoopCheck);
        loop_mutex_acquired = monotonic_now_ms();
        cleanup_expired_connections(now_ms());
        const auto& session = require_participant(parts[1], participant);
        validate_actor_credential(session, participant, credentials);
        if (transport_protocol && control_receive_only) authority_expiry = session.control_token_expires_at_ms;
        if (transport_protocol && send_only) {
          const auto latest = latest_vehicle_transport_results_.find(session.session_id);
          if (latest != latest_vehicle_transport_results_.end()) vehicle_transport_result = latest->second;
          const auto statuses = latest_vehicle_transport_status_.find(session.session_id);
          if (statuses != latest_vehicle_transport_status_.end())
            for (const auto& status : statuses->second) vehicle_transport_status.push_back(status);
        }
        if (!send_only) {
          pending = take_signaling_messages(
              parts[1],
              participant,
              control_receive_only ? std::string_view("control_command") : std::string_view{},
              false);
        }
      } catch (const std::exception& error) {
        Json denial = {{"error", error.what()}, {"event", "signaling_authority_lost"}};
        if (authenticated_authority_scope.is_object()) {
          auto scope = authenticated_authority_scope;
          { std::lock_guard lock(mutex_);
            const auto grant = direct_authority_journal_.find(scope.at("vehicle_id").get<std::string>());
            if (grant != direct_authority_journal_.end() && grant->value("issuer", "") == scope.at("issuer").get<std::string>() &&
                grant->value("session_id", "") == scope.at("session_id").get<std::string>() &&
                grant->value("connection_generation", std::uint64_t{0}) == scope.at("connection_generation").get<std::uint64_t>())
              scope["authority_epoch"] = grant->at("authority_epoch"); }
          denial["authority_scope"] = std::move(scope);
        }
        connection.send_json(denial);
        connection.send_close(1008, "signaling authority lost");
        return true;
      }
      if (authority_expiry > 0 && authority_expiry != last_lease_expiry_sent) {
        connection.send_json({{"event", "control_authority_lease"}, {"expires_at_utc_ms", authority_expiry}});
        last_lease_expiry_sent = authority_expiry;
      }
      if (vehicle_transport_result.is_object() &&
          (detail::feedback_transport_epoch(vehicle_transport_result) > last_vehicle_result_epoch_sent ||
           (detail::feedback_transport_epoch(vehicle_transport_result) == last_vehicle_result_epoch_sent &&
            vehicle_transport_result.value("result_seq", std::uint64_t{0}) > last_vehicle_result_sent))) {
        connection.send_json(vehicle_transport_result);
        last_vehicle_result_sent = vehicle_transport_result.at("result_seq").get<std::uint64_t>();
        last_vehicle_result_epoch_sent = detail::feedback_transport_epoch(vehicle_transport_result);
      }
      std::sort(vehicle_transport_status.begin(), vehicle_transport_status.end(), [](const Json& left, const Json& right) {
        return left.at("status").at("control_status_seq").get<std::uint64_t>() < right.at("status").at("control_status_seq").get<std::uint64_t>(); });
      for (const auto& status : vehicle_transport_status) {
        const auto sequence = status.at("status").at("control_status_seq").get<std::uint64_t>();
        const auto feedback_epoch = detail::feedback_transport_epoch(status);
        if (feedback_epoch > last_control_status_epoch_sent || (feedback_epoch == last_control_status_epoch_sent && sequence > last_control_status_sent)) {
          connection.send_json(status); last_control_status_sent = sequence; last_control_status_epoch_sent = feedback_epoch;
        }
      }
      const auto loop_check_completed = monotonic_now_ms();
      if (!send_only && !pending.empty()) {
        const auto delivery_cursor = pending.back().value("delivery_cursor", std::uint64_t{0});
        const auto timestamp = std::chrono::steady_clock::now();
        if (delivery_cursor > last_delivery_cursor_sent ||
            timestamp - last_delivery_sent_at >= std::chrono::milliseconds(500)) {
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
                {"delivery_cursor", delivery_cursor},
                {"redelivery", delivery_cursor <= last_delivery_cursor_sent},
            };
          }
          for (const auto& delivered : pending) {
            if (delivered.value("type", "") == "control_command")
              last_control_seq_sent = std::max(last_control_seq_sent, delivered.value("seq", std::uint64_t{0}));
          }
          const auto diagnostic_transport = [&] {
            auto sample = connection.transport_diagnostics();
            sample["peer_is_loopback"] = sample.contains("peer_address")
                ? Json(is_loopback_bind_address(sample.at("peer_address").get<std::string>())) : Json(nullptr);
            sample.erase("peer_address");
            return sample;
          };
          if (trace_delivery) {
            observe_phase(ControlObservationPhase::TransportSample, last_control_seq_sent);
            const auto sample_started_ms = monotonic_now_ms();
            try { delivery_trace["transport_before_send"] = diagnostic_transport(); }
            catch (...) { delivery_trace["transport_before_send_unavailable"] = true; }
            delivery_trace["transport_before_send_sample_ms"] = monotonic_now_ms() - sample_started_ms;
          }
          const auto send_started_at_utc_ms = now_ms();
          const auto send_started_monotonic_ms = monotonic_now_ms();
          try {
            observe_phase(ControlObservationPhase::DeliverySend, last_control_seq_sent);
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
              delivery_trace["cloud_delivery_send_failed_at_utc_ms"] = now_ms();
              delivery_trace["cloud_delivery_send_failed_monotonic_ms"] =
                  monotonic_now_ms();
              delivery_trace["error"] = error.what();
              native_control_trace_->enqueue(std::move(delivery_trace));
            }
            throw;
          }
          const auto send_completed_at_utc_ms = now_ms();
          const auto send_completed_monotonic_ms = monotonic_now_ms();
          if (trace_delivery) {
            const auto queued_at_utc_ms =
                delivery_trace.value("cloud_queued_at_utc_ms", std::int64_t{0});
            delivery_trace["cloud_delivery_send_started_at_utc_ms"] =
                send_started_at_utc_ms;
            delivery_trace["cloud_delivery_send_started_monotonic_ms"] =
                send_started_monotonic_ms;
            delivery_trace["cloud_delivery_send_completed_at_utc_ms"] =
                send_completed_at_utc_ms;
            delivery_trace["cloud_delivery_send_completed_monotonic_ms"] =
                send_completed_monotonic_ms;
            delivery_trace["cloud_mailbox_to_send_ms"] = queued_at_utc_ms > 0
                ? std::max<std::int64_t>(0, send_started_at_utc_ms - queued_at_utc_ms)
                : 0;
            delivery_trace["cloud_delivery_send_call_ms"] =
                std::max<std::int64_t>(
                    0,
                    send_completed_monotonic_ms - send_started_monotonic_ms);
            observe_phase(ControlObservationPhase::TransportSample, last_control_seq_sent);
            const auto sample_started_ms = monotonic_now_ms();
            try { delivery_trace["transport_after_send"] = diagnostic_transport(); }
            catch (...) { delivery_trace["transport_after_send_unavailable"] = true; }
            delivery_trace["transport_after_send_sample_ms"] = monotonic_now_ms() - sample_started_ms;
            native_control_trace_->enqueue(std::move(delivery_trace));
          }
          last_delivery_cursor_sent = std::max(last_delivery_cursor_sent, delivery_cursor);
          last_delivery_sent_at = timestamp;
        }
      }

      WebSocketReceiveResult received;
      const auto receive_started_monotonic_ms = monotonic_now_ms();
      try {
        observe_phase(ControlObservationPhase::Receive);
        received = connection.receive_json(
            std::chrono::milliseconds(send_only || control_receive_only ? 10 : 50), wakeup.get());
      } catch (const std::exception& error) {
        if (send_only && native_control_trace_) {
          Json failure = {{"stage", "ingress_receive_failed"}, {"trace_session_id", parts[1]},
              {"driver_id", participant}, {"error", error.what()},
              {"cloud_receive_started_monotonic_ms", receive_started_monotonic_ms},
              {"cloud_receive_failed_monotonic_ms", monotonic_now_ms()},
              {"cloud_receive_failed_at_utc_ms", now_ms()}};
          try {
            failure["transport"] = connection.transport_diagnostics();
            failure["transport"]["connection_started_monotonic_ms"] = socket_connection_started_monotonic_ms;
          } catch (const std::exception&) {
            failure["transport_sample_failed"] = true;
          }
          native_control_trace_->enqueue(std::move(failure));
        }
        connection.send_json({{"error", error.what()}, {"event", "websocket_protocol_error"}});
        connection.send_close(1002, "websocket protocol error");
        return true;
      }
      const auto frame_received_monotonic_ms = monotonic_now_ms();
      const auto receive_loop_gap_ms = last_receive_completed_monotonic_ms > 0
          ? std::max<std::int64_t>(0, receive_started_monotonic_ms - last_receive_completed_monotonic_ms) : 0;
      last_receive_completed_monotonic_ms = frame_received_monotonic_ms;
      if (received.status == WebSocketReceiveStatus::Timeout ||
          received.status == WebSocketReceiveStatus::Notified) continue;
      if (received.status == WebSocketReceiveStatus::Closed) return true;
      const auto frame_received_at_utc_ms = now_ms();
      try {
        if (transport_protocol && control_receive_only && received.message.value("event", "") == "vehicle_control_status") {
          std::lock_guard status_lock(mutex_);
          cleanup_expired_connections(now_ms());
          const auto& session = require_participant(parts[1], participant);
          validate_actor_credential(session, participant, credentials);
          if (received.message.contains("feedback_transport") && !detail::verify_transport_message(received.message, session.transport_mac_key, true))
            throw std::invalid_argument("feedback status authentication failed");
          auto status = detail::bounded_vehicle_control_status(received.message, session.session_id, session.vehicle_id, session.driver_id);
          const auto event = status.at("status").at("event").get<std::string>();
          auto& statuses = latest_vehicle_transport_status_[session.session_id];
          if (!statuses.is_object()) statuses = Json::object();
          const auto epoch_of = [](const Json& item) { return item.contains("feedback_transport") ?
              detail::ControlTransportStamp::uint(item.at("feedback_transport"), "epoch") : std::uint64_t{0}; };
          if (!statuses.contains(event) || epoch_of(status) > epoch_of(statuses.at(event)) ||
              (epoch_of(status) == epoch_of(statuses.at(event)) && status.at("status").at("control_status_seq").get<std::uint64_t>() >
              statuses.at(event).at("status").at("control_status_seq").get<std::uint64_t>())) {
            statuses[event] = std::move(status);
            notify_signaling_recipient(session.session_id, session.driver_id);
          }
          continue;
        }
        if (transport_protocol && control_receive_only && received.message.value("event", "") == "vehicle_control_result") {
          std::lock_guard result_lock(mutex_);
          cleanup_expired_connections(now_ms());
          const auto& session = require_participant(parts[1], participant);
          validate_actor_credential(session, participant, credentials);
          if (!detail::verify_transport_message(received.message, session.transport_mac_key, true))
            throw std::invalid_argument("vehicle result authentication failed");
          auto result = detail::bounded_vehicle_control_result(received.message, session.session_id, session.vehicle_id, session.driver_id);
          auto& latest = latest_vehicle_transport_results_[session.session_id];
          if (!latest.is_object() || detail::feedback_transport_epoch(result) > detail::feedback_transport_epoch(latest) ||
              (detail::feedback_transport_epoch(result) == detail::feedback_transport_epoch(latest) &&
               result.at("result_seq").get<std::uint64_t>() > latest.value("result_seq", std::uint64_t{0}))) {
            latest = std::move(result);
            notify_signaling_recipient(session.session_id, session.driver_id);
          }
          continue;
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
          const auto ack_received_at_utc_ms = now_ms();
          const auto ack_received_monotonic_ms = monotonic_now_ms();
          std::size_t acknowledged = 0;
          Json acknowledgement_trace;
          {
            std::lock_guard lock(mutex_);
            cleanup_expired_connections(now_ms());
            const auto& session = require_participant(parts[1], participant);
            validate_actor_credential(session, participant, credentials);
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
                        ack_received_at_utc_ms - queued->second.queued_at_utc_ms);
              }
            }
            acknowledgement_trace["vehicle_feedback_present"] = received.message.contains("control_feedback");
            if (control_receive_only && received.message.contains("control_feedback")) {
              const auto& feedback = received.message.at("control_feedback");
              const auto feedback_seq = required_uint64(feedback, "command_seq");
              if (!feedback.is_object() || participant != session.vehicle_id ||
                  feedback.value("session_id", "") != session.session_id ||
                  feedback.value("vehicle_id", "") != session.vehicle_id ||
                  feedback.value("driver_id", "") != session.driver_id ||
                  feedback_seq == 0 || feedback_seq != reported_seq || feedback_seq > last_control_seq_sent ||
                  required_uint64(feedback, "intent_seq") != reported_intent_seq ||
                  !feedback.at("accepted").is_boolean() || !feedback.at("rearm_required").is_boolean() ||
                  (feedback.at("rearm_required").get<bool>() &&
                   (feedback.at("accepted").get<bool>() || required_uint64(feedback, "control_status_seq") == 0))) {
                throw std::invalid_argument("vehicle control feedback does not match the delivered command/session");
              }
              auto& latest = latest_vehicle_control_feedback_[session.session_id];
              acknowledgement_trace["vehicle_feedback_rearm_required"] = feedback.at("rearm_required");
              acknowledgement_trace["vehicle_feedback_command_seq"] = feedback_seq;
              if (!latest.is_object() || feedback_seq > latest.value("command_seq", std::uint64_t{0})) {
                // Store only bounded fields from the authenticated vehicle. An
                // ingress ACK retains its meaning; this is a separate result.
                latest = {{"session_id", session.session_id}, {"vehicle_id", session.vehicle_id},
                          {"driver_id", session.driver_id}, {"command_seq", feedback_seq},
                          {"intent_seq", reported_intent_seq}, {"accepted", feedback.at("accepted")},
                          {"rearm_required", feedback.at("rearm_required")},
                          {"control_status_seq", required_uint64(feedback, "control_status_seq")}};
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
          observe_phase(ControlObservationPhase::DeliveryAck, reported_seq);
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
        const auto ingress_mutex_wait_started = monotonic_now_ms();
        std::int64_t ingress_mutex_acquired = 0;
        std::int64_t authority_check_completed = 0;
        {
          observe_phase(ControlObservationPhase::IngressMutex, received.message.value("seq", std::uint64_t{0}));
          std::lock_guard lock(mutex_);
          observe_phase(ControlObservationPhase::IngressCheck);
          ingress_mutex_acquired = monotonic_now_ms();
          cleanup_expired_connections(now_ms());
          const auto& session = require_participant(parts[1], participant);
          validate_actor_credential(session, participant, credentials);
          if (send_only && received.message.value("type", "") != "control_command") {
            throw std::invalid_argument(
                "send-only control WebSocket accepts control_command messages only");
          }
          authority_check_completed = monotonic_now_ms();
          acknowledgement = enqueue_signaling_message(parts[1], received.message, participant);
          if (send_only) {
            const auto feedback = latest_vehicle_control_feedback_.find(session.session_id);
            if (feedback != latest_vehicle_control_feedback_.end())
              acknowledgement["vehicle_control_feedback"] = feedback->second;
          }
        }
        Json acknowledgement_trace;
        if (native_control_trace_ && received.message.value("type", "") == "control_command") {
          acknowledgement_trace = {
              {"trace_session_id", parts[1]},
              {"vehicle_id", received.message.value("vehicle_id", "")},
              {"driver_id", received.message.value("driver_id", "")},
              {"seq", acknowledgement.value("seq", std::uint64_t{0})},
              {"intent_seq", received.message.at("payload").value("intent_seq", std::uint64_t{0})},
              {"delivery_cursor", acknowledgement.value("delivery_cursor", std::uint64_t{0})},
              {"cloud_received_at_utc_ms", acknowledgement.value("cloud_received_at_utc_ms", std::int64_t{0})},
              {"cloud_queued_at_utc_ms", acknowledgement.value("cloud_queued_at_utc_ms", std::int64_t{0})},
              {"ack_send_started_at_utc_ms", now_ms()},
              {"ack_send_started_monotonic_ms", monotonic_now_ms()},
              {"cloud_frame_received_at_utc_ms", frame_received_at_utc_ms},
              {"cloud_frame_received_monotonic_ms", frame_received_monotonic_ms},
              {"cloud_receive_started_monotonic_ms", receive_started_monotonic_ms},
              {"cloud_receive_call_ms", std::max<std::int64_t>(0, frame_received_monotonic_ms - receive_started_monotonic_ms)},
              {"cloud_receive_wait_ms", received.receive_wait_ms},
              {"cloud_frame_assembly_ms", received.frame_assembly_ms},
              {"cloud_json_parse_ms", received.json_parse_ms},
              {"cloud_receive_loop_gap_ms", receive_loop_gap_ms},
              {"cloud_loop_mutex_wait_ms", std::max<std::int64_t>(0, loop_mutex_acquired - loop_mutex_wait_started)},
              {"cloud_loop_authority_check_ms", std::max<std::int64_t>(0, loop_check_completed - loop_mutex_acquired)},
              {"cloud_ingress_mutex_wait_ms", std::max<std::int64_t>(0, ingress_mutex_acquired - ingress_mutex_wait_started)},
              {"cloud_ingress_authority_check_ms", std::max<std::int64_t>(0, authority_check_completed - ingress_mutex_acquired)},
          };
          if (frame_received_monotonic_ms - last_transport_sample_monotonic_ms >= 1000) {
            try {
              acknowledgement_trace["transport"] = connection.transport_diagnostics();
              acknowledgement_trace["transport"]["connection_started_monotonic_ms"] = socket_connection_started_monotonic_ms;
              last_transport_sample_monotonic_ms = frame_received_monotonic_ms;
            } catch (const std::exception&) {
              acknowledgement_trace["transport_sample_failed"] = true;
            }
          }
        }
        const auto finish_ack_trace = [&](bool sent, const char* error = nullptr) {
          if (acknowledgement_trace.is_null()) return;
          const auto completed_monotonic_ms = monotonic_now_ms();
          acknowledgement_trace["stage"] = sent
              ? "ingress_ack_send_completed" : "ingress_ack_send_failed";
          acknowledgement_trace["ack_send_completed_at_utc_ms"] = now_ms();
          acknowledgement_trace["ack_send_completed_monotonic_ms"] = completed_monotonic_ms;
          acknowledgement_trace["ack_send_call_ms"] = std::max<std::int64_t>(
              0, completed_monotonic_ms - acknowledgement_trace.at(
                  "ack_send_started_monotonic_ms").get<std::int64_t>());
          if (error) acknowledgement_trace["error"] = error;
          native_control_trace_->enqueue(std::move(acknowledgement_trace));
        };
        try {
          observe_phase(ControlObservationPhase::IngressAck);
          connection.send_json(acknowledgement);
        } catch (const std::exception& error) {
          finish_ack_trace(false, error.what());
          throw;
        }
        finish_ack_trace(true);
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
  const auto timestamp_monotonic_ms = monotonic_now_ms();

  auto latest_control = latest_control_messages_.find(recipient_key);
  if (latest_control != latest_control_messages_.end() &&
      timestamp_monotonic_ms - latest_control->second.queued_at_monotonic_ms >=
          config_.native_control_message_ttl_ms) {
    if (native_control_trace_) {
      const auto& expired = latest_control->second;
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
          {"cloud_expired_at_utc_ms", now_ms()},
          {"cloud_expired_monotonic_ms", timestamp_monotonic_ms},
          {"cloud_mailbox_age_ms", std::max<std::int64_t>(
                                           0,
                                           timestamp_monotonic_ms -
                                               expired.queued_at_monotonic_ms)},
          {"delivery_cursor", expired.delivery_cursor},
      });
    }
    latest_control_messages_.erase(latest_control);
    latest_control = latest_control_messages_.end();
  }
  if (latest_control != latest_control_messages_.end() && requested("control_command")) {
    values.push_back(latest_control->second.to_json());
    if (consume) latest_control_messages_.erase(latest_control);
  }

  auto found = messages_.find(recipient_key);
  if (found != messages_.end()) {
    std::erase_if(found->second, [&](const auto& message) {
      return timestamp_monotonic_ms - message.queued_at_monotonic_ms >=
          config_.signaling_message_ttl_ms;
    });
    if (found->second.empty()) {
      messages_.erase(found);
      found = messages_.end();
    }
  }
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

void SignalingService::notify_signaling_recipient(std::string_view session_id, std::string_view recipient) {
  const auto found = signaling_wakeups_.find(message_key(session_id, recipient));
  if (found == signaling_wakeups_.end()) return;
  std::erase_if(found->second, [&](const auto& subscriber) {
    const auto wakeup = subscriber.lock();
    if (!wakeup) return true;
    // The existing bounded receive timeout remains a fallback if a local
    // notification fails; never roll back or replay an already queued command.
    if (!wakeup->notify()) try_audit("signaling_wakeup_failed",
        {{"session_id", session_id}, {"recipient", recipient}});
    return false;
  });
}

Json SignalingService::enqueue_signaling_message(
    std::string_view session_id,
    const Json& value,
    std::optional<std::string_view> authenticated_actor) {
  const auto cloud_ingress_started_at_utc_ms = now_ms();
  const auto cloud_ingress_started_monotonic_ms = monotonic_now_ms();
  const auto sender = required_string(value, "sender");
  const auto recipient = required_string(value, "recipient");
  const auto type = required_string(value, "type");
  static const std::vector<std::string> allowed{
      "webrtc_offer", "webrtc_answer", "ice_candidate", "media_capabilities", "media_fallback",
      "connection_status", "telemetry", "media_status", "session_event", "control_command", "control_management", "control_status"};
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
  if ((type == "control_management" && !driver_to_vehicle) || (type == "control_status" && !vehicle_to_driver))
    throw Unauthorized("control management/status route is invalid");
  if (type == "control_command" && !driver_to_vehicle) {
    throw Unauthorized("control_command route is invalid");
  }
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
    if (command.control_token != session.control_token) {
      throw Unauthorized("control command token does not match active session");
    }
  }
  const auto sequence_key = message_key(session_id, sender) +
      (type == "control_command" ? ":native_control" : "");
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
  const auto delivery_cursor = ++next_delivery_cursors_[recipient_key];
  const auto cloud_queued_at_utc_ms = now_ms();
  const auto cloud_queued_monotonic_ms = monotonic_now_ms();
  const Message queued_message{
      metadata,
      sender,
      recipient,
      type,
      payload,
      cloud_queued_at_utc_ms,
      cloud_queued_monotonic_ms,
      delivery_cursor};
  std::size_t queued = 1;
  if (type == "control_command") {
    latest_control_messages_.insert_or_assign(recipient_key, queued_message);
  } else {
    auto& queue = messages_[recipient_key];
    queue.push_back(queued_message);
    queued = queue.size();
  }
  Json acknowledgement = {
      {"queued", queued},
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
  notify_signaling_recipient(session_id, recipient);
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
             {"native_control_capabilities", online ? online_vehicles_.at(vehicle_id).native_control_capabilities : ""},
             {"mobile_approval_required", config_.mobile_approval_vehicles.contains(vehicle_id)},
             {"controlled_by", active == nullptr ? "" : active->driver_id},
             {"session_id", active == nullptr ? "" : active->session_id}});
      }
    }
    audit("authorized_vehicles_listed", {{"driver_id", driver_id}, {"vehicle_count", vehicles.size()}});
    return ServerResponse::json(200, {{"driver_id", driver_id}, {"vehicles", std::move(vehicles)},
        {"native_control_capabilities", kNativeControlCapabilities}});
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
             {"transport_mac_key", session.transport_mac_key},
             {"direct_authority_configuration", session.direct_authority_enabled ? Json{
                 {"protocol", detail::kDirectAuthorityCapability}, {"issuer", service_instance_id_},
                 {"maximum_duration_ms", config_.experimental_direct_authority_lease_ms},
                 {"connection_generation", online_vehicles_.at(vehicle_id).generation}} : Json(nullptr)},
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

ServerResponse SignalingService::handle_direct_authority_post(const HttpRequest& request) {
  const auto value = request.json_body();
  const auto parts = path_parts(request.path);
  std::lock_guard journal_lock(direct_authority_write_mutex_);
  std::unique_lock state_lock(mutex_);
  cleanup_expired_connections(now_ms());
  if (parts.size() == 3 && parts[0] == "vehicles" && parts[2] == "direct-authority") {
    const auto vehicle = parts[1];
    const auto generation = required_uint64(value, "connection_generation");
    validate_vehicle_connection(vehicle, optional_string(value, "device_token"), generation);
    if (config_.experimental_direct_authority_lease_ms <= 0) throw Conflict("direct authority is disabled", "direct_authority_disabled");
    const auto session_id = required_string(value, "session_id");
    const auto nonce = required_string(value, "nonce");
    if (nonce.size() > 128) throw std::invalid_argument("authority nonce too long");
    auto& session = require_participant(session_id, vehicle);
    if (session.vehicle_id != vehicle || session.state != SessionState::Active || !session.direct_authority_enabled)
      throw Unauthorized("direct authority session is not active");
    const auto budget = required_int64(value, "maximum_duration_ms");
    const auto duration = config_.experimental_direct_authority_lease_ms;
    if (duration > budget) throw Conflict("direct authority duration exceeds vehicle reviewed cap", "direct_authority_budget_mismatch");
    const auto now = now_ms(), monotonic = monotonic_now_ms();
    auto previous = direct_authority_journal_.value(vehicle, Json::object());
    const bool same = previous.value("issuer", "") == service_instance_id_ && previous.value("session_id", "") == session_id &&
        previous.value("connection_generation", std::uint64_t{0}) == generation;
    const bool live = direct_authority_vehicle_reserved(vehicle);
    if (!same && live) throw Conflict("previous direct authority is still reserved", "direct_authority_reserved");
    if (same && previous.value("nonce", "") == nonce) {
      const auto retry_snapshot = direct_authority_journal_;
      state_lock.unlock(); persist_direct_authority_state(retry_snapshot); state_lock.lock();
      static_cast<void>(require_participant(session_id, vehicle));
      return ServerResponse::json(200, previous); // Retry durability, never renew deadline.
    }
    const auto retired_epoch = value.value("retired_authority_epoch", std::uint64_t{0});
    std::uint64_t epoch = same && live && retired_epoch != previous.value("authority_epoch", std::uint64_t{0})
        ? previous.at("authority_epoch").get<std::uint64_t>() : 0;
    if (!epoch) {
      if (direct_authority_next_epoch_ == std::numeric_limits<std::uint64_t>::max()) throw ServiceUnavailable("authority epoch exhausted");
      epoch = ++direct_authority_next_epoch_;
    }
    const auto old_sequence = same ? previous.value("lease_seq", std::uint64_t{0}) : 0;
    if (old_sequence == std::numeric_limits<std::uint64_t>::max() || now > std::numeric_limits<std::int64_t>::max() - duration ||
        monotonic > std::numeric_limits<std::int64_t>::max() - duration) throw ServiceUnavailable("authority sequence exhausted");
    detail::DirectAuthorityGrant grant{service_instance_id_, vehicle, session.driver_id, session_id, nonce,
        generation, epoch, old_sequence + 1, now, now + duration, duration};
    if (grant.expires_utc_ms > session.control_token_expires_at_ms) throw Unauthorized("session token requires renewal before short authority");
    auto issued = grant.json();
    issued["control_token_expires_at_utc_ms"] = session.control_token_expires_at_ms;
    direct_authority_journal_[vehicle] = issued;
    direct_authority_reservations_[vehicle] = monotonic + duration;
    // Persist before replying. On failure the in-memory reservation remains
    // conservative; no grant is issued and no second owner can be activated.
    const auto durable_snapshot = direct_authority_journal_;
    state_lock.unlock();
    persist_direct_authority_state(durable_snapshot);
    state_lock.lock();
    static_cast<void>(require_participant(session_id, vehicle));
    return ServerResponse::json(200, issued);
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
    const auto mock_ttl = config_.mock_test_login_ttl_ms.find(driver_id);
    const auto login_ttl_ms = mock_ttl == config_.mock_test_login_ttl_ms.end()
        ? config_.token_ttl_ms : mock_ttl->second;
    driver_tokens_[token] = DriverToken{driver_id, timestamp_ms + login_ttl_ms, generation};
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
    const auto capabilities = optional_string(value, "native_control_capabilities");
    if (capabilities.size() > 128) throw std::invalid_argument("vehicle control capability declaration is too long");
    const auto timestamp_ms = now_ms();
    const auto registration_started_monotonic_ms = monotonic_now_ms();
    const auto current = online_vehicles_.find(vehicle_id);
    if (direct_authority_vehicle_reserved(vehicle_id) &&
        (current == online_vehicles_.end() || current->second.connection_id != connection_id || current->second.native_control_capabilities != capabilities))
      throw Conflict("previous vehicle direct authority is still reserved", "direct_authority_reserved");
    std::string connection_fingerprint, previous_fingerprint, source_fingerprint;
    try {
      connection_fingerprint = diagnostic_fingerprint("connection:" + connection_id, diagnostic_fingerprint_key_);
      if (current != online_vehicles_.end())
        previous_fingerprint = diagnostic_fingerprint("connection:" + current->second.connection_id, diagnostic_fingerprint_key_);
      source_fingerprint = diagnostic_fingerprint("source:" + request_source(request), diagnostic_fingerprint_key_);
    } catch (...) { /* Diagnostic allocation/crypto failure cannot reject registration. */ }
    const auto previous_generation = current == online_vehicles_.end() ? std::uint64_t{0} : current->second.generation;
    const auto registration_diagnostic = [&](std::uint64_t generation, std::string_view policy) {
      try {
        if (!native_control_trace_) return;
        // The service mutex is held: use the existing bounded, nonblocking
        // trace queue. Diagnostic disk failures must not change admission.
        native_control_trace_->enqueue({{"stage", "vehicle_registration_observed"}, {"vehicle_id", vehicle_id},
          {"registration_request_id", active_request_id},
          {"connection_fingerprint", connection_fingerprint}, {"previous_connection_fingerprint", previous_fingerprint},
          {"source_fingerprint", source_fingerprint}, {"previous_connection_generation", previous_generation},
          {"connection_generation", generation}, {"duplicate_policy", policy},
          {"registration_observed_monotonic_ms", registration_started_monotonic_ms},
          {"registration_completed_monotonic_ms", monotonic_now_ms()},
          {"native_control_capabilities", capabilities}});
      } catch (...) { /* Diagnostics are best effort. */ }
    };
    if (current != online_vehicles_.end() && current->second.connection_id == connection_id) {
      const bool capabilities_changed = current->second.native_control_capabilities != capabilities;
      if (capabilities_changed) {
        // Reusing an ID cannot retain authority across an implementation
        // downgrade or silently promote a previously unknown vehicle.
        close_sessions_for_vehicle(vehicle_id, "vehicle_control_capabilities_changed");
        current->second.native_control_capabilities = capabilities;
        current->second.generation = ++connection_generation_;
        current->second.connected_at_ms = timestamp_ms;
        audit("vehicle_control_capabilities_changed", {{"vehicle_id", vehicle_id},
            {"connection_generation", current->second.generation}, {"native_control_capabilities", capabilities}});
      }
      current->second.last_seen_at_ms = timestamp_ms;
      registration_diagnostic(current->second.generation,
          capabilities_changed ? "capabilities_changed_new_generation" : "same_connection_refresh");
      return ServerResponse::json(
          200,
          {{"vehicle_id", vehicle_id},
           {"state", "online"},
           {"connection_generation", current->second.generation},
           {"registration_request_id", active_request_id},
           {"connection_fingerprint", connection_fingerprint},
           {"native_control_capabilities", current->second.native_control_capabilities},
           {"duplicate_policy", capabilities_changed ? "capabilities_changed_new_generation" : "same_connection_refresh"}});
    }
    const bool replacing = current != online_vehicles_.end();
    if (replacing) {
      close_sessions_for_vehicle(vehicle_id, "vehicle_connection_replaced");
      audit(
          "vehicle_connection_replaced",
          {{"vehicle_id", vehicle_id}, {"previous_connection_generation", current->second.generation}});
    }
    const auto generation = ++connection_generation_;
    online_vehicles_[vehicle_id] = ConnectionPresence{connection_id, generation, timestamp_ms, timestamp_ms, capabilities};
    audit("vehicle_online", {{"vehicle_id", vehicle_id}, {"connection_generation", generation}});
    registration_diagnostic(generation, replacing ? "replace_previous_connection" : "new_connection");
    return ServerResponse::json(
        200,
        {{"vehicle_id", vehicle_id},
         {"state", "online"},
         {"connection_generation", generation},
         {"registration_request_id", active_request_id},
         {"connection_fingerprint", connection_fingerprint},
         {"native_control_capabilities", capabilities},
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
  if (request.path == "/sessions/approval/cancel") {
    const auto driver_id = required_string(value, "driver_id");
    const auto vehicle_id = required_string(value, "vehicle_id");
    validate_driver_token(driver_id, optional_string(value, "token"));
    const auto request_id = required_string(value, "approval_request_id");
    const auto found = mobile_approvals_.find(vehicle_id);
    if (found == mobile_approvals_.end() || found->second.request_id != request_id ||
        found->second.driver_id != driver_id ||
        found->second.driver_generation != online_drivers_.at(driver_id).generation) {
      throw Conflict("连接申请已失效", "mobile_approval_stale");
    }
    auto& approval = found->second;
    if (!approval.session_id.empty()) {
      const auto session = sessions_.find(approval.session_id);
      if (session != sessions_.end() && session->second.state != SessionState::Closed) {
        close_session(session->second, "mobile_approval_cancelled");
      }
    }
    if (approval.state == "pending" || approval.state == "approved" || approval.state == "consumed") {
      approval.state = "cancelled";
      audit("mobile_approval_cancelled", approval.to_json());
    }
    return ServerResponse::json(200, approval.to_json());
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
    if (direct_authority_vehicle_reserved(vehicle_id))
      throw Conflict("previous vehicle direct authority is still reserved", "direct_authority_reserved");
    const bool direct_requested = value.value("required_direct_authority_capability", "") == detail::kDirectAuthorityCapability;
    if (direct_requested && (config_.experimental_direct_authority_lease_ms <= 0 ||
        value.value("required_control_transport_capability", "") != detail::kControlTransportCapability))
      throw Conflict("direct authority capability unavailable", "direct_authority_disabled");
    const auto required_capabilities = value.contains("required_native_control_capabilities")
        ? required_string(value, "required_native_control_capabilities") : std::string{};
    if (!required_capabilities.empty() &&
        (required_capabilities != kNativeControlCapabilities ||
         online_vehicles_.at(vehicle_id).native_control_capabilities != required_capabilities)) {
      audit("session_rejected", {{"vehicle_id", vehicle_id}, {"driver_id", driver_id},
          {"reason", "vehicle_control_capability_mismatch"},
          {"required_native_control_capabilities", required_capabilities},
          {"vehicle_native_control_capabilities", online_vehicles_.at(vehicle_id).native_control_capabilities}});
      throw Conflict("车辆原生控制能力不匹配或未知，请更新并重新注册车端包", "vehicle_control_capability_mismatch");
    }
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
    require_mobile_approval(driver_id, vehicle_id, optional_string(value, "approval_request_id"));
    if (!try_audit(
            "control_authority_grant_preflight",
            {{"vehicle_id", vehicle_id}, {"driver_id", driver_id}})) {
      throw ServiceUnavailable("audit log unavailable; new control authority is disabled");
    }
    if (config_.mobile_approval_vehicles.contains(vehicle_id)) {
      if (!try_audit("mobile_approval_consume_preflight", mobile_approvals_.at(vehicle_id).to_json())) {
        throw ServiceUnavailable("audit log unavailable; approval cannot be consumed");
      }
      // Synchronous audit I/O may span the approval deadline. Check again before issuing authority.
      require_mobile_approval(driver_id, vehicle_id, optional_string(value, "approval_request_id"));
      mobile_approvals_.at(vehicle_id).state = "consumed";
    }
    ++session_counter_;
    std::ostringstream id;
    id << "session-" << std::setw(6) << std::setfill('0') << session_counter_;
    Session session{
        .session_id = id.str(),
        .vehicle_id = vehicle_id,
        .driver_id = driver_id,
        .started_at_utc_ms = now_ms(),
        .state = SessionState::Online,
        .control_token = "control-token-" + random_token(),
        .transport_mac_key = value.value("required_control_transport_capability", "") == detail::kControlTransportCapability ? random_token(32) : std::string{},
        .direct_authority_enabled = direct_requested,
        .control_token_expires_at_ms = now_ms() + config_.control_token_ttl_ms};
    sessions_[session.session_id] = session;
    auto& stored = sessions_.at(session.session_id);
    remember_log_session(stored);
    if (config_.mobile_approval_vehicles.contains(vehicle_id)) {
      mobile_approvals_.at(vehicle_id).session_id = stored.session_id;
    }
    try {
      audit(
          "session_created",
          {{"session_id", stored.session_id}, {"vehicle_id", stored.vehicle_id}, {"driver_id", stored.driver_id}});
      transition_session(stored, SessionState::Reserved, "control_requested");
      transition_session(stored, SessionState::Connecting, "participants_authenticated");
      transition_session(stored, SessionState::Active, "control_authority_granted");
      if (!try_audit(
              "control_authority_granted",
              {{"session_id", stored.session_id},
               {"vehicle_id", stored.vehicle_id},
               {"driver_id", stored.driver_id},
               {"approval_request_id", config_.mobile_approval_vehicles.contains(vehicle_id)
                   ? mobile_approvals_.at(vehicle_id).request_id : ""}})) {
        close_session(stored, "audit_log_unavailable");
        throw ServiceUnavailable("audit log unavailable; control authority was not granted");
      }
    } catch (...) {
      stored.state = SessionState::Closed;
      throw ServiceUnavailable("audit log unavailable; control authority was not granted");
    }
    auto response = stored.to_json(true);
    if (value.value("required_control_transport_capability", "") == detail::kControlTransportCapability)
      response["transport_mac_key"] = stored.transport_mac_key;
    if (stored.direct_authority_enabled) response["direct_authority_configuration"] = {
        {"protocol", detail::kDirectAuthorityCapability}, {"issuer", service_instance_id_},
        {"maximum_duration_ms", config_.experimental_direct_authority_lease_ms},
        {"connection_generation", online_vehicles_.at(vehicle_id).generation}};
    return ServerResponse::json(200, std::move(response));
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
    notify_signaling_recipient(session.session_id, session.vehicle_id);
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
  const auto base = std::filesystem::absolute(path).parent_path();
  const auto ui = root["ui"];
  config.visual_assets_root = base / "../assets";
  if (!std::filesystem::is_directory(config.visual_assets_root) &&
      std::filesystem::is_directory(base / "../cpp/web/assets")) {
    config.visual_assets_root = base / "../cpp/web/assets";
  }
  if (ui && ui["assets_root"]) {
    config.visual_assets_root = ui["assets_root"].as<std::string>();
    if (config.visual_assets_root.is_relative()) config.visual_assets_root = base / config.visual_assets_root;
  }
  config.visual_assets_root = config.visual_assets_root.lexically_normal();
  if (ui && ui["scene_enabled"]) config.scene_enabled = ui["scene_enabled"].as<bool>();
  if (ui && ui["scene_manifest"]) config.scene_manifest = ui["scene_manifest"].as<std::string>();
  const std::filesystem::path manifest(config.scene_manifest);
  if (manifest.empty() || manifest.is_absolute() || manifest.extension() != ".json" ||
      config.scene_manifest.find_first_of("\\:\r\n?#") != std::string::npos) {
    throw std::invalid_argument("ui.scene_manifest must be an asset-relative JSON path");
  }
  for (const auto& part : manifest) {
    if (part == ".." || part == ".") throw std::invalid_argument("ui.scene_manifest must stay inside assets");
  }
  if (!root["cloud"] || !root["cloud"]["signaling_url"]) throw std::invalid_argument("cloud.signaling_url is required");
  if (root["driver"] && root["driver"]["id"]) config.driver_id = root["driver"]["id"].as<std::string>();
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
  config.browser_event_log_path = std::filesystem::absolute(
      std::filesystem::path(path).parent_path() / "../log/control-browser-events.jsonl").lexically_normal();
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
  if (root["control"] && root["control"]["mock_control_diagnostic"])
    config.mock_control_diagnostic = root["control"]["mock_control_diagnostic"].as<bool>();
  if (root["control"] && root["control"]["transport_mode"])
    config.control_transport_mode = root["control"]["transport_mode"].as<std::string>();
  if (!control_transport_mode_is_valid(config.control_transport_mode))
    throw std::invalid_argument("control.transport_mode must be p2p_preferred or cloud_only");
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
  if (config.signaling_url.empty() || config.rate_hz <= 0 ||
      config.intent_lease_ms < 100 || config.intent_lease_ms > 1000 || config.estop_hold_ms < 0 ||
      config.browser_event_log_max_bytes < 1024 || config.browser_event_log_files < 1 ||
      config.browser_event_log_files > 20 ||
      config.max_time_sync_uncertainty_ms < 0 || config.max_time_sync_uncertainty_ms > 100 ||
      config.time_sync_interval_ms <= 0 ||
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

Json DriverConsoleRuntime::native_control_observation() {
  auto observation = native_control_observation_state_->sample(false);
  {
    std::unique_lock lock(mutex_, std::try_to_lock);
    observation["scope_snapshot_available"] = lock.owns_lock();
    if (lock.owns_lock()) {
      observation["trace_session_id"] = session_id_;
      observation["session_generation"] = control_session_generation_;
    }
  }
  observation["commands_sent_total"] = native_control_commands_sent_.load();
  observation["last_sent_monotonic_ms"] = native_control_last_sent_monotonic_ms_.load();
  {
    std::unique_lock lock(control_signaling_websocket_mutex_, std::try_to_lock);
    observation["control_socket_lock_busy"] = !lock.owns_lock();
    if (lock.owns_lock()) {
      observation["last_ack_seq"] = control_signaling_last_ack_seq_;
      observation["ack_stall_age_ms"] = control_signaling_ack_window_.stall_age_ms(monotonic_now_ms());
      observation["unacknowledged_count"] = control_signaling_ack_window_.pending_count();
      observation["ack_braking_wait"] = control_signaling_ack_recovery_.waiting();
      observation["cloud_capabilities_verified"] = control_signaling_capabilities_verified_;
      if (control_signaling_websocket_) observation["transport"] = control_signaling_websocket_->transport_diagnostics();
    }
  }
  return observation;
}

DriverConsoleRuntime::DriverConsoleRuntime(DriverConfig config, std::string vehicle_id, std::string password)
    : config_(std::move(config)),
      driver_id_(config_.driver_id),
      vehicle_id_(std::move(vehicle_id)),
      password_(std::move(password)),
      signaling_http_url_(normalize_signaling_http_url(config_.signaling_url)),
      http_(std::chrono::seconds(5), config_.resolve_entries, config_.ca_bundle),
      time_sync_http_(std::chrono::milliseconds(500), config_.resolve_entries, config_.ca_bundle),
      native_control_intent_(config_.intent_lease_ms) {
  if (!control_transport_mode_is_valid(config_.control_transport_mode))
    throw std::invalid_argument("control.transport_mode must be p2p_preferred or cloud_only");
  if (vehicle_id_.empty()) throw std::invalid_argument("vehicle id is required");
  reset_control_profile_locked();
  if (!signaling_url_is_secure_or_loopback(config_.signaling_url)) {
    throw std::invalid_argument("public signaling URL must use HTTPS or WSS; HTTP/WS is allowed only on loopback");
  }
  if (config_.control_trace_commands && !config_.browser_event_log_path.empty()) {
    native_control_observation_state_ = std::make_shared<ControlConnectionObservation>();
    native_control_trace_ = std::make_unique<AsyncControlTrace>(
        [this](Json details) {
          std::string trace_session_id;
          const auto& commands = details["commands"];
          if (commands.is_array() && !commands.empty()) {
            trace_session_id = commands.front().value("trace_session_id", "");
          } else if (details.contains("observations") && !details.at("observations").empty()) {
            trace_session_id = details.at("observations").front().value("trace_session_id", "");
          }
          const auto timestamp_ms = now_ms();
          append_driver_log_record({
              {"event", "driver_native_control_trace_batch"},
              {"sent_at_utc_ms", timestamp_ms},
              {"browser_sent_at_utc_ms", timestamp_ms},
              {"driver_id", driver_id()},
              {"vehicle_id", vehicle_id_},
              {"session_id", trace_session_id},
              {"details", std::move(details)},
          });
        }, [this] { return Json::array({native_control_observation()}); });
  }
  native_control_sender_ = std::jthread(
      [this](std::stop_token stop_token) { native_control_sender_loop(stop_token); });
  native_control_lease_ = std::jthread(
      [this](std::stop_token stop_token) { native_control_lease_loop(stop_token); });
  time_sync_worker_ = std::jthread(
      [this](std::stop_token stop_token) { time_sync_loop(stop_token); });
}

DriverConsoleRuntime::~DriverConsoleRuntime() {
  if (log_export_) log_export_->cancel();
  native_control_sender_.request_stop();
  native_control_lease_.request_stop();
  time_sync_worker_.request_stop();
  wake_native_control_sender();
  if (native_control_sender_.joinable()) native_control_sender_.join();
  if (native_control_lease_.joinable()) native_control_lease_.join();
  if (time_sync_worker_.joinable()) time_sync_worker_.join();
  close_control_signaling_websocket();
  close_signaling_websocket();
  if (native_control_trace_) native_control_trace_->stop();
}

void DriverConsoleRuntime::connect_signaling_websocket(std::string_view session_id, std::string_view token) {
  const auto url = signaling_websocket_url(
      config_.signaling_url,
      session_id,
      driver_id());
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
    std::uint64_t expected_generation, bool estop_only) {
  const auto url = signaling_websocket_url(
                       config_.signaling_url,
                       session_id,
                       driver_id()) +
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
      control_signaling_websocket_->url() == url + ("&transport_protocol=" + std::string(detail::kControlTransportCapability)) &&
      control_signaling_websocket_session_id_ == session_id) {
    if (!estop_only && !control_signaling_capabilities_verified_)
      throw std::runtime_error("云端原生控制能力不匹配：缺少恢复反馈或 ACK 制动等待支持，请更新云端包");
    return true;
  }
  auto next = std::make_unique<WebSocketClient>(
      std::chrono::seconds(1), config_.resolve_entries, config_.ca_bundle);
  Json authority_scope;
  { std::lock_guard authority_lock(direct_authority_mutex_); authority_scope = direct_authority_.scope(); }
  try {
    next->connect(url + ("&transport_protocol=" + std::string(detail::kControlTransportCapability)),
        {{"X-Mine-Teleop-Driver-Token", std::string(token)}});
  } catch (const HttpStatusError& error) {
    if (!detail::transient_cloud_failure(error)) {
      std::lock_guard authority_lock(direct_authority_mutex_);
      if (direct_authority_.observe_authenticated_cloud_denial({{"event", "signaling_authority_lost"}}, authority_scope))
        native_control_intent_.invalidate(true);
    }
    throw;
  }
  const bool capabilities_verified = next->response_header("x-mine-teleop-control-capabilities") == kNativeControlCapabilities;
  if (!estop_only && !capabilities_verified)
    throw std::runtime_error("云端原生控制能力不匹配：缺少恢复反馈或 ACK 制动等待支持，请更新云端包");
  if (control_signaling_websocket_) control_signaling_websocket_->close();
  control_signaling_websocket_ = std::move(next);
  control_signaling_websocket_session_id_ = std::string(session_id);
  control_signaling_websocket_authority_scope_ = std::move(authority_scope);
  control_signaling_capabilities_verified_ = capabilities_verified;
  control_transport_cloud_capable_ = control_signaling_websocket_->response_header("x-mine-teleop-transport-capabilities") == detail::kControlTransportCapability;
  control_signaling_last_ack_seq_ = 0;
  control_signaling_ack_window_.reset();
  control_signaling_ack_recovery_.connection_reopened(monotonic_now_ms());
  control_signaling_next_connect_monotonic_ms_ = 0;
  return true;
}

void DriverConsoleRuntime::close_control_signaling_websocket() {
  std::lock_guard lock(control_signaling_websocket_mutex_);
  if (control_signaling_websocket_) control_signaling_websocket_->close();
  control_signaling_websocket_.reset();
  control_signaling_websocket_session_id_.clear();
  control_signaling_websocket_authority_scope_ = Json(nullptr);
  control_signaling_capabilities_verified_ = false;
  control_signaling_last_ack_seq_ = 0;
  control_signaling_ack_window_.reset();
  control_signaling_ack_recovery_.reset();
  control_vehicle_result_latency_.reset();
  control_transport_sender_.reset(); control_transport_cloud_capable_ = false;
  feedback_transport_sender_.reset(); feedback_transport_enabled_ = feedback_transport_dc_is_p2p_ = false;
  { std::lock_guard authority_lock(direct_authority_mutex_); direct_authority_ = {}; }
  control_transport_dc_ready_ = false; control_transport_dc_network_path_ = "unknown"; control_transport_bridge_attempt_.clear();
  control_transport_bridge_owner_.clear(); control_transport_bridge_seen_ms_ = 0;
  control_transport_latest_dc_ = Json(); control_transport_pending_status_ = Json::object(); control_dc_ack_window_.reset(); control_dc_ack_recovery_.reset();
  control_dc_last_ack_seq_ = 0;
  control_source_revision_fence_.reset();
  recovery_bad_sample_revision_ = recovery_brake_revision_ = 0;
  control_signaling_next_connect_monotonic_ms_ = 0;
  control_signaling_reconnect_delay_ms_ = 100;
}

void DriverConsoleRuntime::reset_native_control_state() {
  std::lock_guard update_lock(native_control_update_mutex_);
  close_control_signaling_websocket();
  native_control_intent_.reset();
  native_control_last_feedback_command_seq_ = 0;
  native_control_rearm_status_seq_.store(0);
  native_control_vehicle_accepted_command_seq_.store(0);
  native_control_last_sent_monotonic_ms_.store(0, std::memory_order_relaxed);
  native_control_last_ack_received_at_utc_ms_.store(0, std::memory_order_relaxed);
  native_control_last_ack_received_monotonic_ms_.store(0, std::memory_order_relaxed);
  native_control_last_ack_cloud_received_at_utc_ms_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    control_sequence_ = 0;
    control_transport_vehicle_capable_ = false;
    emergency_stop_vehicle_capable_ = false; feedback_transport_vehicle_capable_ = false;
    control_transport_mac_key_.clear();
    ++control_session_generation_;
  }
  { std::lock_guard lock(emergency_stop_mutex_);
    emergency_stop_latest_frame_ = emergency_stop_last_result_ = Json(nullptr);
    emergency_stop_owner_.clear(); emergency_stop_attempt_.clear(); emergency_stop_ui_epoch_ = emergency_stop_session_generation_ = emergency_stop_reset_epoch_ = 0;
    emergency_stop_published_ms_ = 0; emergency_stop_latency_.reset(); }
  native_control_estop_wakeup_.store(false);
  wake_native_control_sender();
}

void DriverConsoleRuntime::note_native_control_failure(
    std::string error,
    std::string_view session_id,
    std::uint64_t generation) {
  std::unique_lock update_lock(native_control_update_mutex_);
  int recovery_timeout_ms = 5000;
  {
    std::lock_guard lock(mutex_);
    if (session_id_ != session_id || control_session_generation_ != generation) {
      return;
    }
    recovery_timeout_ms = control_recovery_timeout_ms_;
  }
  ++native_control_send_failures_;
  {
    std::lock_guard lock(native_control_status_mutex_);
    native_control_last_error_ = std::move(error);
  }
  const auto frozen_gear = native_control_intent_.sample(monotonic_now_ms()).intent.gear;
  native_control_intent_.invalidate(true);
  update_lock.unlock(); // Never hold input acceptance behind an unrelated WSS operation.
  std::lock_guard websocket_lock(control_signaling_websocket_mutex_);
  { std::lock_guard lock(mutex_);
    if (session_id_ != session_id || control_session_generation_ != generation) return; }
  if (!control_signaling_ack_recovery_.waiting()) control_signaling_recovery_gear_ = frozen_gear;
  control_signaling_ack_recovery_.enter(monotonic_now_ms(), recovery_timeout_ms, control_signaling_last_ack_seq_);
  // Ordinary DC still depends on cloud authority. Its receipts cannot clear
  // the independent WSS ACK fault: select a new, neutral WSS epoch so that a
  // real cloud ACK can recover it. Never resurrect the retired DC publication.
  if (control_transport_sender_.path() == detail::ControlPath::DataChannel)
    select_control_wss_locked(monotonic_now_ms(), recovery_timeout_ms);
  if (control_signaling_websocket_) control_signaling_websocket_->close();
  control_signaling_websocket_.reset();
  control_signaling_websocket_session_id_.clear();
  control_signaling_websocket_authority_scope_ = Json(nullptr);
  control_signaling_capabilities_verified_ = false;
  control_signaling_last_ack_seq_ = 0;
  control_signaling_ack_window_.reset();
  const auto now_monotonic_ms = monotonic_now_ms();
  control_signaling_next_connect_monotonic_ms_ =
      now_monotonic_ms + control_signaling_reconnect_delay_ms_;
  control_signaling_reconnect_delay_ms_ =
      std::min(control_signaling_reconnect_delay_ms_ * 2, 1000);
}

// Caller owns control_signaling_websocket_mutex_: publication and ACK state
// retire atomically with the route, before the browser can fetch another frame.
void DriverConsoleRuntime::retire_control_dc_locked() {
  control_transport_latest_dc_ = Json();
  control_transport_latest_dc_at_ms_ = 0;
  control_dc_ack_window_.reset();
  control_dc_ack_recovery_.reset();
  control_dc_last_ack_seq_ = 0;
}

void DriverConsoleRuntime::select_control_wss_locked(std::int64_t now, int timeout_ms) {
  control_transport_sender_.begin(detail::ControlPath::Wss, {}, now, timeout_ms);
  retire_control_dc_locked();
}

bool DriverConsoleRuntime::consume_vehicle_control_result_locked(const Json& input, std::string_view session,
    std::string_view vehicle, std::string_view driver, std::int64_t now, bool direct_return) {
  std::string signing_key;
  { std::lock_guard lock(mutex_); signing_key = control_transport_mac_key_; }
  if (!detail::verify_transport_message(input, signing_key, true))
    throw std::invalid_argument("vehicle result authentication failed");
  const auto result = detail::bounded_vehicle_control_result(input, session, vehicle, driver);
  if (feedback_transport_enabled_ && feedback_transport_sender_.active() && !result.contains("emergency_stop") &&
      !feedback_transport_sender_.matches_event(result)) return false;
  if (result.contains("emergency_stop")) observe_emergency_stop_result_verified(result, direct_return);
  if (result.contains("emergency_reset_epoch")) {
    std::lock_guard lock(emergency_stop_mutex_);
    emergency_stop_reset_epoch_ = std::max(emergency_stop_reset_epoch_, result.at("emergency_reset_epoch").get<std::uint64_t>());
  }
  if (config_.enable_experimental_direct_authority && result.contains("direct_authority")) {
    std::lock_guard authority_lock(direct_authority_mutex_);
    const auto& authority = result.at("direct_authority");
    const auto before = direct_authority_.rearm_revision();
    if (authority.value("revoked", false)) direct_authority_.revoke();
    if (authority.contains("grant") && authority.at("grant").is_object()) {
      const auto sync = clock_.status();
      if (sync.acceptable(config_.max_time_sync_uncertainty_ms))
        static_cast<void>(direct_authority_.observe_vehicle_accepted(authority.at("grant"), now, clock_.now_ms(), sync.uncertainty_ms));
    }
    if (before != direct_authority_.rearm_revision()) native_control_intent_.invalidate(true);
  }
  if (feedback_transport_enabled_ && feedback_transport_sender_.active() && !result.contains("emergency_stop")) {
    const auto receipt = feedback_transport_sender_.result(result,
        direct_return ? detail::ControlPath::DataChannel : detail::ControlPath::Wss, now);
    if (receipt.boundary) {
      for (const auto& status : feedback_transport_sender_.take_pending_statuses())
        consume_feedback_status_locked(status, session, vehicle, driver,
            detail::FeedbackTransportStamp::parse(status.at("feedback_transport")).path);
      control_transport_sender_.reset_feedback_comparison();
      if (native_control_trace_) native_control_trace_->enqueue({{"stage", "feedback_transport_selected"},
          {"trace_session_id", session}, {"seq", result.at("command_seq")}, {"feedback_transport", result.at("feedback_transport")},
          {"command_received_path", result.value("command_received_path", "unknown")}, {"return_path", direct_return ? "dc" : "wss"},
          {"dc_path_is_p2p", feedback_transport_dc_is_p2p_}});
    }
    if (!receipt.progress && !receipt.command_reconciliation) return false;
  }
  int timeout_ms;
  { std::lock_guard lock(mutex_); timeout_ms = control_recovery_timeout_ms_; }
  const auto previous_path = control_transport_sender_.path();
  const auto receipt = control_transport_sender_.note_result(result, now, timeout_ms);
  // Selection conflicts and slower relay qualification can fall back inside
  // the sender, including while this result is being uploaded by the bridge.
  if (previous_path == detail::ControlPath::DataChannel &&
      control_transport_sender_.path() == detail::ControlPath::Wss) retire_control_dc_locked();
  if (!receipt.matched) {
    // A newer result may have retired the retained send. Preserve the verified
    // observation without treating it as current feedback or refreshing a lease.
    if (native_control_trace_)
      native_control_trace_->enqueue({{"stage", "vehicle_transport_result_unmatched"},
          {"trace_session_id", session}, {"seq", result.at("command_seq")},
          {"intent_seq", result.at("intent_seq")}, {"accepted", result.at("accepted")},
          {"reason", result.at("reason")}, {"transport_admitted", result.at("transport_admitted")},
          {"return_path", direct_return ? "dc" : "wss"}});
    return false;
  }
  const auto seq = result.at("command_seq").get<std::uint64_t>();
  const auto intent = result.at("intent_seq").get<std::uint64_t>();
  const bool accepted = result.at("accepted").get<bool>();
  static_cast<void>(control_vehicle_result_latency_.note_result(seq, intent, now, accepted, false, direct_return));
  bool rearm = receipt.boundary;
  if (seq > native_control_last_feedback_command_seq_) {
    native_control_last_feedback_command_seq_ = seq;
    if (accepted) native_control_vehicle_accepted_command_seq_.store(seq);
    if (result.at("rearm_required").get<bool>() && !accepted) {
      native_control_rearm_status_seq_.store(std::max(native_control_rearm_status_seq_.load(),
          result.at("control_status_seq").get<std::uint64_t>()));
      rearm = true;
    }
  }
  if (receipt.current && result.contains("control_transport") && result.at("control_transport").at("path") == "dc" &&
      result.at("transport_admitted").get<bool>() && seq > control_dc_last_ack_seq_) {
    if (control_dc_ack_window_.acknowledge_through(seq, now)) {
      control_dc_last_ack_seq_ = seq;
      const auto transition = control_dc_ack_recovery_.note_ack_progress(now, seq, receipt.round_trip_ms, timeout_ms);
      rearm = rearm || transition.entered || transition.recovered;
    }
  }
  if (rearm) native_control_intent_.invalidate(true);
  if (native_control_trace_) native_control_trace_->enqueue({{"stage", "vehicle_transport_result_received"},
      {"trace_session_id", session}, {"seq", seq}, {"intent_seq", intent}, {"accepted", accepted},
      {"reason", result.at("reason")}, {"transport_admitted", result.at("transport_admitted")},
      {"transport_selected", result.at("transport_selected")},
      {"round_trip_ms", receipt.round_trip_ms}, {"return_path", direct_return ? "dc" : "wss"}, {"transport", result.value("control_transport", Json(nullptr))},
      {"authority_epoch", result.at("authority_epoch")}, {"feedback_transport", result.value("feedback_transport", Json(nullptr))}, {"requires_fresh_input", rearm}});
  return rearm;
}

Json DriverConsoleRuntime::send_control_management(const Json& input) {
  Json payload;
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    std::lock_guard lock(mutex_);
    validate_media_source_scope_locked(input); validate_media_negotiation_locked(input);
    const auto ownership = native_control_intent_.ownership_scope();
    if (required_string(input, "source_session_id") != session_id_ ||
        required_uint64(input, "source_control_session_generation") != control_session_generation_ ||
        required_string(input, "ui_instance_id") != ownership.first ||
        required_uint64(input, "ui_control_epoch") != ownership.second || ownership.first.empty())
      throw Conflict("management request belongs to a retired page", "control_transport_scope_mismatch");
    auto message = input.at("message");
    if (!message.is_object()) throw std::invalid_argument("control management message must be an object");
    if (message.value("type", "") == "session_control_profile") {
      const auto profile = SessionControlProfileRequest::from_json(message);
      if (profile.session_id != session_id_ || profile.driver_id != driver_id() || profile.vehicle_id != vehicle_id_ ||
          profile.control_token != control_token_ || profile.seq != last_control_profile_prepared_seq_)
        throw std::invalid_argument("profile request was not prepared by this native session");
    } else if (message.value("event", "") == "vcu_handshake_command") {
      const auto action = required_string(message, "action");
      if (action != "connect" && action != "disconnect") throw std::invalid_argument("invalid VCU action");
      message = {{"event", "vcu_handshake_command"}, {"action", action}, {"sent_at_utc_ms", clock_.now_ms()}};
    } else throw std::invalid_argument("unsupported control management message");
    payload = {{"protocol", detail::kControlTransportCapability}, {"session_id", session_id_},
        {"vehicle_id", vehicle_id_}, {"driver_id", driver_id()}, {"control_token", control_token_},
        {"sent_at_utc_ms", clock_.now_ms()}, {"message", std::move(message)}};
  }
  { std::lock_guard lock(control_signaling_websocket_mutex_);
    std::lock_guard authority_lock(direct_authority_mutex_);
    if (!control_transport_cloud_capable_ || (!control_transport_sender_.negotiated() && !direct_authority_.valid(monotonic_now_ms())))
      throw std::invalid_argument("control management fallback was not negotiated"); }
  return send_signaling_message("control_management", payload, &input);
}

Json DriverConsoleRuntime::publish_native_emergency_locked(const NativeControlIntentSample& sample,
    Json payload, std::uint64_t generation) {
  if (!config_.enable_experimental_direct_authority || !sample.intent.estop) return Json(nullptr);
  std::lock_guard state_lock(mutex_);
  if (session_id_.empty() || control_session_generation_ != generation || !emergency_stop_vehicle_capable_ ||
      control_transport_mac_key_.size() < 32 || media_scope_id_.empty()) return Json(nullptr);
  const auto current = native_control_intent_.sample(monotonic_now_ms());
  if (!current.active || !current.intent.estop || current.owner_ui_instance_id != sample.owner_ui_instance_id ||
      current.ui_control_epoch != sample.ui_control_epoch) return Json(nullptr);
  Json authority;
  { std::lock_guard lock(direct_authority_mutex_); authority = direct_authority_.reference(); }
  if (!authority.is_object()) return Json(nullptr);
  if (!payload.is_object()) {
    // Serialize the accepted STOP event through the SAME native sequence
    // allocator as periodic commands. No browser wire command or new worker.
    ControlCommand command;
    command.vehicle_id = vehicle_id_; command.driver_id = driver_id(); command.session_id = session_id_;
    command.control_token = control_token_; command.seq = ++control_sequence_; command.sent_at_utc_ms = clock_.now_ms();
    command.gear = current.intent.gear; command.estop = true; command.brake = 1.0;
    payload = command.to_json(); payload["intent_seq"] = current.intent.intent_seq;
    payload["intent_fresh"] = current.fresh; payload["intent_requires_fresh_input"] = true;
  }
  if (payload.at("session_id") != session_id_ || payload.at("vehicle_id") != vehicle_id_) return Json(nullptr);
  payload["estop"] = true; payload["throttle"] = payload["steering"] = 0.0;
  payload["brake"] = 1.0; payload["operator_active"] = false;
  const auto published = monotonic_now_ms();
  std::lock_guard stop_lock(emergency_stop_mutex_);
  if (emergency_stop_owner_ != sample.owner_ui_instance_id || emergency_stop_ui_epoch_ != sample.ui_control_epoch ||
      emergency_stop_session_generation_ != generation || emergency_stop_attempt_ != media_scope_id_) {
    emergency_stop_latency_.reset(); emergency_stop_last_result_ = Json(nullptr);
  } else if (emergency_stop_latest_frame_.is_object() &&
      payload.at("seq").get<std::uint64_t>() <= emergency_stop_latest_frame_.at("seq").get<std::uint64_t>()) {
    return Json(nullptr); // An earlier periodic sample cannot replace an urgent event.
  }
  payload["emergency_stop"] = {{"protocol", detail::kEmergencyStopCapability}, {"authority", authority},
      {"reset_epoch", emergency_stop_reset_epoch_}, {"media_attempt", media_scope_id_}};
  payload["native_mac"] = detail::transport_message_mac(payload, control_transport_mac_key_, false);
  emergency_stop_latest_frame_ = payload; emergency_stop_published_ms_ = published;
  emergency_stop_owner_ = sample.owner_ui_instance_id; emergency_stop_ui_epoch_ = sample.ui_control_epoch;
  emergency_stop_session_generation_ = generation; emergency_stop_attempt_ = media_scope_id_;
  emergency_stop_latency_.note_sent(payload.at("seq").get<std::uint64_t>(), payload.at("intent_seq").get<std::uint64_t>(), published, true);
  if (native_control_trace_) native_control_trace_->enqueue({{"stage", "emergency_native_published"},
      {"trace_session_id", session_id_}, {"seq", payload.at("seq")}, {"intent_seq", payload.at("intent_seq")},
      {"published_monotonic_ms", published}, {"scope_ui_epoch", sample.ui_control_epoch}});
  return payload;
}

void DriverConsoleRuntime::observe_emergency_stop_result_verified(const Json& result, bool data_channel_return) {
  std::optional<detail::VehicleResultLatencyWindow::Sample> sample;
  {
    std::lock_guard lock(emergency_stop_mutex_);
    if (result.contains("emergency_reset_epoch"))
      emergency_stop_reset_epoch_ = std::max(emergency_stop_reset_epoch_, result.at("emergency_reset_epoch").get<std::uint64_t>());
    sample = emergency_stop_latency_.note_result(result.at("command_seq").get<std::uint64_t>(),
        result.at("intent_seq").get<std::uint64_t>(), monotonic_now_ms(), result.at("accepted").get<bool>(), false, data_channel_return);
    if (sample) emergency_stop_last_result_ = result;
  }
  if (sample && native_control_trace_) native_control_trace_->enqueue({{"stage", "emergency_software_result_received"},
      {"trace_session_id", result.at("session_id")}, {"seq", sample->command_seq}, {"intent_seq", sample->intent_seq},
      {"accepted", sample->accepted}, {"software_estop_latched", result.value("software_estop_latched", false)},
      {"command_received_path", result.value("command_received_path", "unknown")}, {"return_path", data_channel_return ? "dc" : "wss"},
      {"round_trip_ms", sample->round_trip_ms}, {"physical_execution_confirmed", false}});
}

Json DriverConsoleRuntime::emergency_stop_bridge(const Json& input) {
  // No ordinary transport/update mutex: a stuck WSS connect/write must not
  // block fetching an already-published native emergency. No network I/O.
  std::string session, vehicle, driver, key, attempt, owner;
  std::uint64_t generation, ui_epoch;
  bool enabled;
  {
    std::lock_guard lock(mutex_);
    validate_media_source_scope_locked(input); validate_media_negotiation_locked(input);
    const auto ownership = native_control_intent_.ownership_scope();
    session = required_string(input, "source_session_id"); generation = required_uint64(input, "source_control_session_generation");
    owner = required_string(input, "ui_instance_id"); ui_epoch = required_uint64(input, "ui_control_epoch");
    if (session != session_id_ || generation != control_session_generation_ || ownership.first.empty() ||
        owner != ownership.first || ui_epoch != ownership.second)
      throw Conflict("emergency bridge belongs to a retired session/page", "emergency_scope_mismatch");
    attempt = media_scope_id_; vehicle = vehicle_id_; driver = driver_id(); key = control_transport_mac_key_;
    enabled = config_.enable_experimental_direct_authority && emergency_stop_vehicle_capable_ && key.size() >= 32;
  }
  std::optional<Json> result;
  if (input.contains("vehicle_result")) {
    if (!enabled || !detail::verify_transport_message(input.at("vehicle_result"), key, true))
      throw std::invalid_argument("emergency result authentication failed");
    result = detail::bounded_vehicle_control_result(input.at("vehicle_result"), session, vehicle, driver);
    if (!result->contains("emergency_stop") || result->at("emergency_stop").value("protocol", "") != detail::kEmergencyStopCapability)
      throw std::invalid_argument("emergency result identity is missing");
  }
  if (result) observe_emergency_stop_result_verified(*result, true);
  Json current_authority;
  { std::lock_guard lock(direct_authority_mutex_); current_authority = direct_authority_.reference(); }
  const auto now = monotonic_now_ms();
  const auto last_seen = detail::ControlTransportStamp::uint(input, "last_seen_seq");
  std::lock_guard lock(emergency_stop_mutex_);
  const bool scoped = enabled && emergency_stop_latest_frame_.is_object() && emergency_stop_owner_ == owner &&
      emergency_stop_ui_epoch_ == ui_epoch && emergency_stop_session_generation_ == generation && emergency_stop_attempt_ == attempt &&
      current_authority.is_object() && emergency_stop_latest_frame_.at("emergency_stop").at("authority").at("issuer") == current_authority.at("issuer") &&
      emergency_stop_latest_frame_.at("emergency_stop").at("authority").at("authority_epoch") == current_authority.at("authority_epoch");
  const auto age = now - emergency_stop_published_ms_;
  const bool available = scoped && input.value("dc_ready", false) && age >= 0 && age < detail::kNativeBridgePublicationMaxAgeMs &&
      emergency_stop_latest_frame_.at("seq").get<std::uint64_t>() > last_seen;
  const auto timing = emergency_stop_latency_.snapshot();
  // Freshness starts at original native publication, not at a delayed reply's
  // arrival. Keep late successful results as history without a current latch claim.
  const bool result_fresh = timing.last && now >= timing.last->received_monotonic_ms &&
      timing.last->round_trip_ms >= 0 && timing.last->round_trip_ms <= detail::NativeControlAckRecovery::kFreshAckMs &&
      now - timing.last->received_monotonic_ms <= detail::NativeControlAckRecovery::kFreshAckMs - timing.last->round_trip_ms;
  const bool confirmed = scoped && emergency_stop_last_result_.is_object() && result_fresh &&
      emergency_stop_latest_frame_.at("emergency_stop").at("reset_epoch").get<std::uint64_t>() == emergency_stop_reset_epoch_ &&
      emergency_stop_last_result_.at("emergency_stop").at("reset_epoch") == emergency_stop_latest_frame_.at("emergency_stop").at("reset_epoch") &&
      emergency_stop_last_result_.at("emergency_stop").at("authority").at("authority_epoch") == emergency_stop_latest_frame_.at("emergency_stop").at("authority").at("authority_epoch") &&
      emergency_stop_last_result_.at("accepted").get<bool>() && emergency_stop_last_result_.value("software_estop_latched", false);
  return {{"protocol", detail::kEmergencyStopCapability}, {"enabled", enabled},
      {"frame", available ? emergency_stop_latest_frame_ : Json(nullptr)},
      {"frame_json", available ? Json(emergency_stop_latest_frame_.dump()) : Json(nullptr)},
      {"valid_for_ms", available ? detail::kNativeBridgePublicationMaxAgeMs - age : 0},
      {"software_latched_confirmed", confirmed}, {"physical_execution_confirmed", false}, {"covers_all_commands", false},
      {"historical_software_latch_result", !confirmed && emergency_stop_last_result_.is_object() &&
          emergency_stop_last_result_.value("accepted", false) && emergency_stop_last_result_.value("software_estop_latched", false)},
      {"round_trip_ms", timing.last ? Json(timing.last->round_trip_ms) : Json(nullptr)},
      {"dispatch", "native_emergency_publication_and_wss_attempt"},
      {"command_received_path", emergency_stop_last_result_.is_object() ? emergency_stop_last_result_.value("command_received_path", "unknown") : "unknown"},
      {"feedback_path", timing.last ? Json(timing.last->feedback_data_channel ? "dc" : "wss") : Json(nullptr)},
      {"confirmed_command_seq", confirmed ? emergency_stop_last_result_.at("command_seq") : Json(nullptr)}};
}

void DriverConsoleRuntime::consume_feedback_status_locked(const Json& value, std::string_view session,
    std::string_view vehicle, std::string_view driver, detail::ControlPath returned) {
  auto wrapper = detail::bounded_vehicle_control_status(value, session, vehicle, driver);
  auto status = wrapper.at("status");
  if (feedback_transport_enabled_) {
    std::string key; { std::lock_guard lock(mutex_); key = control_transport_mac_key_; }
    if (!detail::verify_transport_message(value, key, true) || !feedback_transport_sender_.status(wrapper, returned)) return;
    status["feedback_transport"] = wrapper.at("feedback_transport");
  } else if (returned != detail::ControlPath::Wss) return;
  const auto event = status.at("event").get<std::string>();
  if (!control_transport_pending_status_.contains(event) || status.at("control_status_seq").get<std::uint64_t>() >
      control_transport_pending_status_.at(event).at("control_status_seq").get<std::uint64_t>())
    control_transport_pending_status_[event] = std::move(status);
}

Json DriverConsoleRuntime::control_transport_bridge(const Json& input, const HttpRequest* request) {
  const auto handler_entered = monotonic_now_ms();
  std::string session, vehicle, driver, attempt, owner;
  std::uint64_t ui_epoch = 0;
  bool capable = false, feedback_capable = false;
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    std::lock_guard lock(mutex_);
    validate_media_source_scope_locked(input); validate_media_negotiation_locked(input);
    if (required_string(input, "source_session_id") != session_id_ ||
        required_uint64(input, "source_control_session_generation") != control_session_generation_)
      throw Conflict("transport bridge belongs to an old session", "control_transport_scope_mismatch");
    const auto ownership = native_control_intent_.ownership_scope();
    owner = required_string(input, "ui_instance_id"); ui_epoch = required_uint64(input, "ui_control_epoch");
    if (ownership.first.empty() || ownership.first != owner || ownership.second != ui_epoch)
      throw Conflict("transport bridge belongs to a retired page", "control_transport_owner_mismatch");
    session = session_id_; vehicle = vehicle_id_; driver = driver_id(); attempt = media_scope_id_;
    capable = control_transport_vehicle_capable_ && control_transport_mac_key_.size() >= 32;
    feedback_capable = config_.enable_experimental_direct_authority && feedback_transport_vehicle_capable_;
  }
  if (!input.at("dc_ready").is_boolean()) throw std::invalid_argument("dc_ready must be boolean");
  const auto transport_lock_started = monotonic_now_ms();
  std::lock_guard transport_lock(control_signaling_websocket_mutex_);
  const auto transport_lock_acquired = monotonic_now_ms();
  { std::lock_guard lock(mutex_);
    const auto ownership = native_control_intent_.ownership_scope();
    if (session_id_ != session || control_session_generation_ != required_uint64(input, "source_control_session_generation") ||
        media_scope_id_ != attempt || ownership.first != owner || ownership.second != ui_epoch)
      throw Conflict("transport bridge scope changed while waiting", "control_transport_scope_mismatch"); }
  const auto now = monotonic_now_ms();
  const auto network_path = input.value("dc_network_path", "unknown");
  if (network_path != "p2p" && network_path != "turn_udp" && network_path != "turn_tcp" && network_path != "turn_tls" && network_path != "turn_unknown" && network_path != "unknown")
    throw std::invalid_argument("invalid DC path classification");
  control_transport_dc_network_path_ = network_path;
  control_transport_dc_ready_ = capable && control_transport_cloud_capable_ && input.at("dc_ready").get<bool>() &&
      config_.control_transport_mode != "cloud_only" &&
      (network_path == "p2p" || config_.enable_experimental_control_transport);
  control_transport_sender_.set_data_channel_is_p2p(network_path == "p2p");
  feedback_transport_dc_is_p2p_ = network_path == "p2p";
  feedback_transport_enabled_ = capable && feedback_capable;
  control_transport_bridge_attempt_ = attempt; control_transport_bridge_owner_ = owner;
  control_transport_bridge_ui_epoch_ = ui_epoch; control_transport_bridge_seen_ms_ = now;
  if (input.contains("vehicle_result")) {
    static_cast<void>(consume_vehicle_control_result_locked(input.at("vehicle_result"), session, vehicle, driver, now, true));
    if (native_control_trace_) native_control_trace_->enqueue({
        {"stage", "dc_result_bridge_received"}, {"trace_session_id", session},
        {"seq", input.at("vehicle_result").at("command_seq")},
        {"result_seq", input.at("vehicle_result").at("result_seq")},
        {"received_at_utc_ms", clock_.now_ms()}, {"consumed_monotonic_ms", now},
        {"http_received_monotonic_ms", request && request->received_at_monotonic_ms ? Json(*request->received_at_monotonic_ms) : Json(nullptr)},
        {"before_handler_ms", request && request->received_at_monotonic_ms ? Json(std::max<std::int64_t>(0, handler_entered - *request->received_at_monotonic_ms)) : Json(nullptr)},
        {"scope_validation_ms", transport_lock_started - handler_entered},
        {"transport_mutex_wait_ms", transport_lock_acquired - transport_lock_started},
        {"scope_recheck_ms", now - transport_lock_acquired},
        {"result_processing_ms", monotonic_now_ms() - now}});
  }
  if (input.contains("vehicle_statuses")) {
    const auto& statuses = input.at("vehicle_statuses");
    if (!statuses.is_array() || statuses.size() > 4) throw std::invalid_argument("feedback status batch exceeds four kinds");
    std::vector<Json> ordered(statuses.begin(), statuses.end());
    std::sort(ordered.begin(), ordered.end(), [](const Json& left, const Json& right) {
      return detail::ControlTransportStamp::uint(left.at("status"), "control_status_seq") <
          detail::ControlTransportStamp::uint(right.at("status"), "control_status_seq"); });
    for (const auto& status : ordered) consume_feedback_status_locked(status, session, vehicle, driver, detail::ControlPath::DataChannel);
  }
  const auto last_seen = required_uint64(input, "last_seen_seq");
  Json frame;
  // Native and vehicle enforce command expiry. The experimental courier also
  // has a conservative local publication allowance, independent of cloud
  // failure detection; it never extends the vehicle's timestamp limit.
  const auto age = now - control_transport_latest_dc_at_ms_;
  const auto allowance = detail::kNativeBridgePublicationMaxAgeMs;
  if (control_transport_dc_ready_ && control_transport_sender_.owns_dc_publication(control_transport_latest_dc_) && age >= 0 && age < allowance &&
      control_transport_latest_dc_.value("seq", std::uint64_t{0}) > last_seen &&
      control_transport_latest_dc_.value("bridge_owner", "") == owner &&
      control_transport_latest_dc_.value("bridge_ui_epoch", std::uint64_t{0}) == ui_epoch) {
    frame = control_transport_latest_dc_;
    frame.erase("bridge_owner"); frame.erase("bridge_ui_epoch");
  }
  Json statuses = Json::array();
  for (const auto& status : control_transport_pending_status_)
    if (!feedback_transport_enabled_ || feedback_transport_sender_.eligible_status(status)) statuses.push_back(status);
  control_transport_pending_status_ = Json::object();
  std::sort(statuses.begin(), statuses.end(), [](const Json& left, const Json& right) {
    return left.at("control_status_seq").get<std::uint64_t>() < right.at("control_status_seq").get<std::uint64_t>(); });
  return {{"protocol", detail::kControlTransportCapability}, {"enabled", capable && control_transport_cloud_capable_},
      {"transport", control_transport_sender_.status()}, {"feedback_transport", feedback_transport_sender_.status()}, {"statuses", std::move(statuses)}, {"frame_json", frame.is_object() ? Json(frame.dump()) : Json(nullptr)}, {"frame", std::move(frame)},
      {"valid_for_ms", age >= 0 && age < allowance ? allowance - age : 0}};
}

bool DriverConsoleRuntime::drain_native_control_acknowledgements_locked(
    std::string_view session, std::string_view identity, std::string_view vehicle,
    Json& drained_acknowledgements) {
  // The caller owns control_signaling_websocket_mutex_ and verifies generation.
  bool requires_rearm = false;
  for (int drained = 0; drained < 16; ++drained) {
    const auto received =
        control_signaling_websocket_->receive_json(std::chrono::milliseconds(0));
    if (received.status == WebSocketReceiveStatus::Timeout) break;
    if (received.status == WebSocketReceiveStatus::Closed) {
      throw std::runtime_error("native control signaling websocket closed");
    }
    if (received.message.value("event", "") == "signaling_authority_lost") {
      std::lock_guard authority_lock(direct_authority_mutex_);
      if (direct_authority_.configured()) {
        if (direct_authority_.observe_authenticated_cloud_denial(received.message, control_signaling_websocket_authority_scope_))
          native_control_intent_.invalidate(true);
        continue;
      }
    }
    if (received.message.contains("error")) {
      { std::lock_guard authority_lock(direct_authority_mutex_);
        auto denial = received.message; denial["event"] = "signaling_authority_lost";
        if (direct_authority_.observe_authenticated_cloud_denial(denial, control_signaling_websocket_authority_scope_))
          native_control_intent_.invalidate(true);
      }
      throw std::invalid_argument(received.message.value(
          "error", "native control signaling message was rejected"));
    }
    if (received.message.value("event", "") == "vehicle_control_status") {
      consume_feedback_status_locked(received.message, session, vehicle, identity, detail::ControlPath::Wss);
      continue;
    }
    if (received.message.value("event", "") == "vehicle_control_result") {
      requires_rearm = consume_vehicle_control_result_locked(received.message, session, vehicle, identity, monotonic_now_ms()) || requires_rearm;
      continue;
    }
    if (received.message.value("event", "") != "signaling_ack" ||
        received.message.value("type", "") != "control_command") {
      throw std::runtime_error(
          "native control signaling acknowledgement is invalid");
    }
    const auto acknowledged_seq =
        received.message.value("seq", std::uint64_t{0});
    const auto acknowledgement_received_at_utc_ms = clock_.now_ms();
    const auto acknowledgement_received_monotonic_ms = monotonic_now_ms();
    const bool ack_advanced = control_signaling_ack_window_.acknowledge_through(
        acknowledged_seq, acknowledgement_received_monotonic_ms);
    std::optional<detail::VehicleResultLatencyWindow::Sample> vehicle_result_sample;
    if (received.message.contains("vehicle_control_feedback") && !feedback_transport_enabled_) {
      const auto& feedback = received.message.at("vehicle_control_feedback");
      const auto feedback_seq = required_uint64(feedback, "command_seq");
      const auto status_seq = required_uint64(feedback, "control_status_seq");
      if (feedback.value("session_id", "") != session ||
          feedback.value("vehicle_id", "") != vehicle || feedback.value("driver_id", "") != identity ||
          feedback_seq == 0 || feedback_seq > acknowledged_seq ||
          // UI intent sequences restart on page replacement. The session
          // command sequence above binds feedback to actual sent traffic.
          required_uint64(feedback, "intent_seq") == 0 ||
          !feedback.at("rearm_required").is_boolean() || !feedback.at("accepted").is_boolean())
        throw std::runtime_error("native vehicle control feedback scope is invalid");
      if (feedback_seq > native_control_last_feedback_command_seq_) {
        vehicle_result_sample = control_vehicle_result_latency_.note_result(
            feedback_seq, required_uint64(feedback, "intent_seq"),
            acknowledgement_received_monotonic_ms, feedback.at("accepted").get<bool>());
        native_control_last_feedback_command_seq_ = feedback_seq;
        if (feedback.at("accepted").get<bool>())
          native_control_vehicle_accepted_command_seq_.store(feedback_seq);
        if (feedback.at("rearm_required").get<bool>() && !feedback.at("accepted").get<bool>() && status_seq > 0) {
          native_control_intent_.invalidate(true);
          native_control_rearm_status_seq_.store(std::max(native_control_rearm_status_seq_.load(), status_seq));
          requires_rearm = true;
          if (native_control_trace_) native_control_trace_->enqueue(
              {{"stage", "vehicle_input_rearm_received"}, {"session_id", session}, {"vehicle_id", vehicle},
               {"command_seq", feedback_seq}, {"control_status_seq", status_seq}, {"intent_seq", feedback.at("intent_seq")}});
        }
      }
    }
    if (ack_advanced) {
      control_signaling_last_ack_seq_ = acknowledged_seq;
      int recovery_timeout_ms = 5000;
      {
        std::lock_guard lock(mutex_);
        recovery_timeout_ms = control_recovery_timeout_ms_;
      }
      const auto transition = control_signaling_ack_recovery_.note_ack_progress(
          acknowledgement_received_monotonic_ms, acknowledged_seq,
          control_signaling_ack_window_.last_ack_packet_age_ms(), recovery_timeout_ms);
      if (transition.entered) {
        control_signaling_recovery_gear_ = native_control_intent_.sample(
            acknowledgement_received_monotonic_ms).intent.gear;
      }
      if (transition.entered || transition.recovered) {
        native_control_intent_.invalidate(true);
        requires_rearm = true;
        if (native_control_trace_) native_control_trace_->enqueue({
            {"stage", transition.entered ? "ack_braking_wait_entered" : "ack_braking_wait_recovered"},
            {"trace_session_id", session}, {"last_ack_seq", acknowledged_seq},
            {"last_ack_packet_age_ms", control_signaling_ack_window_.last_ack_packet_age_ms()},
            {"ack_consecutive_anomalies", control_signaling_ack_recovery_.consecutive_anomalies()},
            {"event_monotonic_ms", acknowledgement_received_monotonic_ms},
            {"action", transition.entered ? "withdraw_motion_keep_socket_request_vehicle_braking"
                : "require_fresh_neutral_keep_session"}});
      }
    }
    native_control_last_ack_received_at_utc_ms_.store(
        acknowledgement_received_at_utc_ms,
        std::memory_order_relaxed);
    native_control_last_ack_received_monotonic_ms_.store(acknowledgement_received_monotonic_ms, std::memory_order_relaxed);
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
          {"vehicle_feedback_present", received.message.contains("vehicle_control_feedback")},
          {"vehicle_feedback_command_seq", received.message.value("vehicle_control_feedback", Json::object()).value("command_seq", std::uint64_t{0})},
          {"vehicle_feedback_rearm_required", received.message.value("vehicle_control_feedback", Json::object()).value("rearm_required", false)},
          {"vehicle_result_round_trip_ms", vehicle_result_sample ? Json(vehicle_result_sample->round_trip_ms) : Json(nullptr)},
          {"vehicle_result_latency_correlated", vehicle_result_sample.has_value()},
          {"seq", acknowledged_seq},
          {"ack_advanced", ack_advanced},
          {"ack_consecutive_anomalies", control_signaling_ack_recovery_.consecutive_anomalies()},
          {"ack_packet_age_ms", ack_advanced ? Json(control_signaling_ack_window_.last_ack_packet_age_ms()) : Json(nullptr)},
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
    if (ack_advanced) {
      control_signaling_next_connect_monotonic_ms_ = 0;
      control_signaling_reconnect_delay_ms_ = 100;
    }
  }
  return requires_rearm;
}

bool DriverConsoleRuntime::direct_control_continuation_available_locked() {
  if (!config_.enable_experimental_direct_authority || control_transport_sender_.path() != detail::ControlPath::DataChannel ||
      control_transport_sender_.holding_motion() || !control_transport_dc_ready_ || control_dc_ack_recovery_.waiting() ||
      !control_dc_ack_recovery_.latest_sample_healthy()) return false;
  const auto now = monotonic_now_ms();
  const auto sample = native_control_intent_.sample(now);
  if (!sample.active || !sample.fresh || sample.requires_fresh_input || !sample.intent.critical_video_ready ||
      !sample.intent.critical_video_sample_healthy || !clock_.status().acceptable(config_.max_time_sync_uncertainty_ms) ||
      control_transport_bridge_owner_ != sample.owner_ui_instance_id || control_transport_bridge_ui_epoch_ != sample.ui_control_epoch ||
      control_transport_bridge_seen_ms_ > now || now - control_transport_bridge_seen_ms_ >= native_control_intent_.lease_ms() ||
      control_dc_last_ack_seq_ == 0 || control_dc_ack_window_.stall_age_ms(now) >= detail::NativeControlAckRecovery::kStallMs) return false;
  std::lock_guard authority_lock(direct_authority_mutex_);
  return direct_authority_.valid(now);
}

bool DriverConsoleRuntime::poll_native_control_acknowledgements() {
  std::string session, identity, vehicle;
  std::uint64_t generation = 0, sequence = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty()) return false;
    session = session_id_; identity = driver_id(); vehicle = vehicle_id_;
    generation = control_session_generation_; sequence = control_sequence_;
  }
  const auto started = monotonic_now_ms();
  try {
    Json acknowledgements = Json::array();
    {
      std::unique_lock websocket_lock(control_signaling_websocket_mutex_, std::try_to_lock);
      if (!websocket_lock.owns_lock() || !control_signaling_websocket_ ||
          !control_signaling_websocket_->connected() || control_signaling_websocket_session_id_ != session) return false;
      {
        std::lock_guard lock(mutex_);
        if (session_id_ != session || control_session_generation_ != generation) return false;
      }
      if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::AckDrain);
      static_cast<void>(drain_native_control_acknowledgements_locked(session, identity, vehicle, acknowledgements));
      if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::Idle);
    }
    if (native_control_trace_ && !acknowledgements.empty()) native_control_trace_->enqueue({
        {"stage", "acknowledgements_polled"}, {"trace_session_id", session}, {"seq", sequence},
        {"session_generation", generation}, {"poll_started_monotonic_ms", started},
        {"timestamp_scope", "native_websocket_ack_drain"},
        {"poll_processing_ms", monotonic_now_ms() - started}, {"acknowledgements", std::move(acknowledgements)}});
    return true;
  } catch (const std::exception& error) {
    {
      std::lock_guard lock(control_signaling_websocket_mutex_);
      if (dynamic_cast<const std::invalid_argument*>(&error) == nullptr && direct_control_continuation_available_locked()) {
        if (control_signaling_websocket_) control_signaling_websocket_->close();
        control_signaling_websocket_.reset(); control_signaling_websocket_session_id_.clear(); return false;
      }
    }
    if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::Failure);
    if (native_control_trace_) {
      Json failure = {{"stage", "ack_poll_failed"}, {"trace_session_id", session},
          {"session_generation", generation}, {"seq", sequence}, {"error", error.what()},
          {"poll_processing_ms", monotonic_now_ms() - started}};
      try {
        std::unique_lock websocket_lock(control_signaling_websocket_mutex_, std::try_to_lock);
        if (websocket_lock.owns_lock() && control_signaling_websocket_)
          failure["failure_transport"] = control_signaling_websocket_->transport_diagnostics();
      } catch (const std::exception&) { failure["transport_sample_failed"] = true; }
      native_control_trace_->enqueue(std::move(failure));
    }
    note_native_control_failure(error.what(), session, generation);
  }
  return false;
}

bool DriverConsoleRuntime::send_native_control_sample(std::stop_token stop) {
  if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::Sample);
  struct FinishPhase {
    std::shared_ptr<ControlConnectionObservation> state;
    ~FinishPhase() { if (state) state->begin(ControlObservationPhase::Idle); }
  } finish_phase{native_control_observation_state_};
  const auto sample_started_monotonic_ms = monotonic_now_ms();
  const auto sample_started_at_utc_ms = clock_.now_ms();
  const auto scheduled_at_monotonic_ms =
      native_control_scheduled_at_monotonic_ms_.load(std::memory_order_relaxed);
  NativeControlIntentSample sample;
  bool wire_intent_fresh = false;
  std::string session;
  std::string identity;
  std::string control_token;
  std::string driver_token;
  std::string vehicle;
  std::int64_t control_token_expires_at_ms = 0;
  std::uint64_t sequence = 0;
  std::uint64_t generation = 0;
  int recovery_timeout_ms = 5000;
  Json trace_record;
  std::int64_t socket_lock_acquired_ms = 0, socket_lock_released_ms = 0;
  bool vehicle_transport_capable = false, vehicle_stop_capable = false;
  std::string current_media_attempt, transport_signing_key;
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    sample = native_control_intent_.sample(monotonic_now_ms());
    if (!sample.active) return false;
    // A vehicle-requested motion interlock can retain live braking. It never
    // marks expired input or a nonzero motion request as fresh.
    wire_intent_fresh = sample.fresh && (!sample.requires_fresh_input ||
        (sample.intent.throttle <= 1e-9 && std::abs(sample.intent.steering) <= 1e-9 &&
         sample.intent.brake > 0.0));
    {
      std::lock_guard lock(mutex_);
      if (session_id_.empty() || driver_token_.empty() || control_token_.empty()) {
        return false;
      }
      session = session_id_;
      identity = driver_id();
      control_token = control_token_;
      driver_token = driver_token_;
      vehicle = vehicle_id_;
      control_token_expires_at_ms = control_token_expires_at_ms_;
      generation = control_session_generation_;
      recovery_timeout_ms = control_recovery_timeout_ms_;
      sequence = ++control_sequence_;
      vehicle_transport_capable = control_transport_vehicle_capable_ && control_transport_mac_key_.size() >= 32;
      transport_signing_key = control_transport_mac_key_;
      vehicle_stop_capable = emergency_stop_vehicle_capable_;
      current_media_attempt = media_scope_id_;
    }
  }

  try {
    if (native_control_trace_) {
      trace_record = {
          {"stage", "send_started"},
          {"trace_session_id", session},
          {"vehicle_id", vehicle},
          {"driver_id", identity},
          {"session_generation", generation},
          {"seq", sequence},
          {"intent_seq", sample.intent.intent_seq},
          {"intent_fresh", wire_intent_fresh},
          {"intent_requires_fresh_input", sample.requires_fresh_input},
          {"intent_age_ms", sample.age_ms},
          {"intent_lease_ms", native_control_intent_.lease_ms()},
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
    if (clock_.now_ms() >= control_token_expires_at_ms) {
      throw std::runtime_error("control authority lease expired");
    }

    ControlCommand command;
    command.vehicle_id = vehicle;
    command.driver_id = identity;
    command.session_id = session;
    command.seq = sequence;
    command.sent_at_utc_ms = clock_.now_ms();
    command.gear = sample.intent.gear;
    command.steering = sample.intent.steering;
    command.throttle = sample.intent.throttle;
    command.brake = sample.intent.brake;
    command.estop = sample.intent.estop;
    command.mock_control_diagnostic = config_.mock_control_diagnostic;
    command.critical_video_ready = sample.intent.critical_video_ready;
    const auto time_readiness = time_sync_readiness_gate_.observe(
        clock_.status(), config_.max_time_sync_uncertainty_ms, monotonic_now_ms(),
        TimeSyncReadinessGate::candidate_timeout_ms(config_.time_sync_interval_ms, config_.time_sync_samples));
    command.control_time_sync_ready = time_readiness.ready;
    command.recovery_sample_healthy = time_readiness.latest_candidate_healthy &&
        sample.refresh_sample_healthy && (config_.mock_control_diagnostic || sample.intent.critical_video_sample_healthy);
    command.operator_input_ready = sample.fresh;
    if (!command.control_time_sync_ready && !command.estop) {
      command.throttle = command.steering = 0.0;
    }
    command.operator_active = wire_intent_fresh &&
        !sample.intent.estop && !sample.intent.is_neutral();
    command.control_token = control_token;
    if (native_control_trace_) {
      trace_record["control_time_sync_ready"] = command.control_time_sync_ready;
      trace_record["operator_input_ready"] = command.operator_input_ready;
      trace_record["mock_control_diagnostic"] = config_.mock_control_diagnostic;
      trace_record["critical_video_ready"] = command.critical_video_ready;
      trace_record["effective_throttle"] = command.throttle;
      trace_record["effective_steering"] = command.steering;
    }
    if (command.estop && config_.enable_experimental_direct_authority && vehicle_stop_capable) {
      command.throttle = command.steering = 0.0; command.brake = 1.0; command.operator_active = false;
    }
    command.validate();
    auto payload = command.to_json();
    payload["intent_seq"] = sample.intent.intent_seq;
    payload["intent_fresh"] = wire_intent_fresh;
    payload["intent_requires_fresh_input"] = sample.requires_fresh_input;
    auto envelope = ProtocolMetadata{
                        command.protocol_version,
                        command.vehicle_id,
                        command.driver_id,
                        command.session_id,
                        command.seq,
                        command.sent_at_utc_ms}
                        .to_json();
    envelope["sender"] = identity;
    envelope["recipient"] = vehicle;
    envelope["type"] = "control_command";
    envelope["payload"] = payload;
    Json emergency_payload;
    if (command.estop && config_.enable_experimental_direct_authority && vehicle_stop_capable) {
      std::lock_guard update_lock(native_control_update_mutex_);
      emergency_payload = publish_native_emergency_locked(sample, payload, generation);
      if (emergency_payload.is_object()) envelope["payload"] = emergency_payload;
    }

    const auto connect_started_at_utc_ms = clock_.now_ms();
    const auto connect_started_monotonic_ms = monotonic_now_ms();
    if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::Connect, sequence);
    bool direct_continuation = false;
    { std::lock_guard lock(control_signaling_websocket_mutex_);
      direct_continuation = !command.estop && direct_control_continuation_available_locked() && command.control_time_sync_ready && command.recovery_sample_healthy; }
    if (!direct_continuation && !connect_control_signaling_websocket(
            session,
            driver_token,
            generation, command.estop)) {
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
    std::int64_t ack_stall_age_ms = 0;
    std::uint64_t last_ack_seq = 0;
    {
      std::lock_guard websocket_lock(control_signaling_websocket_mutex_);
      socket_lock_acquired_ms = monotonic_now_ms();
      struct RecordUnlock {
        std::int64_t& value;
        ~RecordUnlock() { value = monotonic_now_ms(); }
      } record_unlock{socket_lock_released_ms};
      if (native_control_trace_) trace_record["socket_lock_acquired_monotonic_ms"] = socket_lock_acquired_ms;
      {
        std::lock_guard lock(mutex_);
        if (session_id_ != session || control_token_ != control_token ||
            control_session_generation_ != generation) {
          return false;
        }
      }
      if (!direct_continuation && (!control_signaling_websocket_ ||
          !control_signaling_websocket_->connected() ||
          control_signaling_websocket_session_id_ != session)) {
        throw std::runtime_error(
            "native control signaling websocket is not connected");
      }
      // Drain acknowledgements from earlier packets without making the 20 Hz
      // schedule depend on a cloud round trip. A delayed ACK pipeline is live
      // while its cumulative sequence advances; packet age remains diagnostic.
      acknowledgement_drain_started_monotonic_ms = monotonic_now_ms();
      if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::AckDrain);
      bool requires_rearm = false;
      if (control_signaling_websocket_ && control_signaling_websocket_->connected()) {
        try { requires_rearm = drain_native_control_acknowledgements_locked(session, identity, vehicle, drained_acknowledgements); }
        catch (const std::runtime_error&) {
          if (!direct_continuation) throw;
          control_signaling_websocket_->close(); control_signaling_websocket_.reset(); control_signaling_websocket_session_id_.clear();
        }
      }
      if (requires_rearm) {
        // This cycle's payload was built before draining ACKs. Neutralize
        // it too; retain gear and an already latched ESTOP. A zero
        // response may be fresh only while the original UI lease is live.
        if (!command.estop) {
          command.steering = command.throttle = 0.0;
          command.operator_active = sample.fresh && command.brake > 0.0;
          payload["steering"] = 0.0;
          payload["throttle"] = 0.0;
          payload["operator_active"] = command.operator_active;
          payload["intent_fresh"] = sample.fresh;
          payload["intent_requires_fresh_input"] = true;
          envelope["payload"] = payload;
          if (native_control_trace_) {
            trace_record["steering"] = 0.0;
            trace_record["throttle"] = 0.0;
            trace_record["operator_active"] = command.operator_active;
            trace_record["intent_fresh"] = sample.fresh;
            trace_record["intent_requires_fresh_input"] = true;
            trace_record["native_rearm_neutralized"] = true;
          }
        }
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
      const auto previous_transport_revision = control_transport_sender_.revision();
      const bool dc_ready = vehicle_transport_capable && control_transport_cloud_capable_ &&
          control_transport_dc_ready_ && control_transport_bridge_attempt_ == current_media_attempt &&
          control_transport_bridge_owner_ == sample.owner_ui_instance_id &&
          control_transport_bridge_ui_epoch_ == sample.ui_control_epoch &&
          monotonic_ms >= control_transport_bridge_seen_ms_ &&
          monotonic_ms - control_transport_bridge_seen_ms_ < native_control_intent_.lease_ms();
      // Bootstrap a fenced cloud route even if ICE/DC never opens. Neutral
      // selection is admitted before common profile/VCU readiness checks.
      if (vehicle_transport_capable && control_transport_cloud_capable_ && !control_transport_sender_.negotiated())
        select_control_wss_locked(monotonic_ms, recovery_timeout_ms);
      if (!direct_continuation && control_signaling_ack_recovery_.waiting() &&
          control_transport_sender_.path() == detail::ControlPath::DataChannel)
        select_control_wss_locked(monotonic_ms, recovery_timeout_ms);
      if (control_transport_sender_.observe(dc_ready, current_media_attempt, monotonic_ms, recovery_timeout_ms))
        retire_control_dc_locked();
      if (dc_ready && !control_signaling_ack_recovery_.waiting() &&
          control_transport_sender_.consider_dc(current_media_attempt, monotonic_ms, recovery_timeout_ms)) {
        control_dc_ack_window_.reset(); control_dc_ack_recovery_.reset(); control_dc_last_ack_seq_ = 0;
      }
      if (control_transport_sender_.path() == detail::ControlPath::DataChannel) {
        const auto dc_transition = control_dc_ack_recovery_.observe(monotonic_ms, control_dc_ack_window_.pending_count(),
            control_dc_ack_window_.stall_age_ms(monotonic_ms), control_dc_last_ack_seq_,
            control_dc_ack_window_.last_ack_packet_age_ms(), recovery_timeout_ms);
        if (dc_transition.entered || control_dc_ack_recovery_.waiting())
          select_control_wss_locked(monotonic_ms, recovery_timeout_ms);
      }
      if (control_transport_sender_.revision() != previous_transport_revision) native_control_intent_.invalidate(true);
      const bool transport_hold = control_transport_sender_.holding_motion() ||
          control_transport_sender_.revision() != previous_transport_revision;
      if (!command.estop && transport_hold) {
        command.throttle = command.steering = 0.0;
        command.operator_active = sample.fresh && command.brake > 0.0;
        payload["intent_fresh"] = sample.fresh;
        payload["intent_requires_fresh_input"] = true;
      }
      ack_stall_age_ms = control_signaling_ack_window_.stall_age_ms(monotonic_ms);
      unacknowledged_age_ms =
          control_signaling_ack_window_.oldest_age_ms(monotonic_ms);
      const auto recovery_transition = direct_continuation ? detail::NativeControlAckRecovery::Transition{} : control_signaling_ack_recovery_.observe(
          monotonic_ms, control_signaling_ack_window_.pending_count(), ack_stall_age_ms,
          control_signaling_last_ack_seq_, control_signaling_ack_window_.last_ack_packet_age_ms(), recovery_timeout_ms);
      if (recovery_transition.entered) {
        control_signaling_recovery_gear_ = command.gear;
        if (control_transport_sender_.path() == detail::ControlPath::DataChannel)
          select_control_wss_locked(monotonic_ms, recovery_timeout_ms);
      }
      if (recovery_transition.entered || recovery_transition.recovered) {
        // Invalidate at both boundaries: neutral refreshes during the wait
        // cannot arm motion before ACK recovery has actually been confirmed.
        native_control_intent_.invalidate(true);
        if (native_control_trace_) native_control_trace_->enqueue({
            {"stage", recovery_transition.entered ? "ack_braking_wait_entered" : "ack_braking_wait_recovered"},
            {"trace_session_id", session}, {"seq", sequence}, {"intent_seq", sample.intent.intent_seq},
            {"event_monotonic_ms", monotonic_ms}, {"ack_stall_age_ms", ack_stall_age_ms},
            {"last_ack_seq", control_signaling_last_ack_seq_},
            {"last_ack_packet_age_ms", control_signaling_ack_window_.last_ack_packet_age_ms()},
            {"recovery_deadline_monotonic_ms", control_signaling_ack_recovery_.deadline()},
            {"action", recovery_transition.entered ? "withdraw_motion_keep_socket_request_vehicle_braking"
                : "require_fresh_neutral_keep_session"}});
      }
      command.control_link_ready = (direct_continuation || !control_signaling_ack_recovery_.waiting()) && !control_transport_sender_.selecting();
      if (!command.estop && control_transport_sender_.selecting()) command.operator_input_ready = false;
      if (!command.estop && !direct_continuation && (control_signaling_ack_recovery_.waiting() || recovery_transition.recovered)) {
        command.throttle = command.steering = 0.0;
        command.gear = control_signaling_recovery_gear_;
        command.operator_active = sample.fresh && command.brake > 0.0;
        // V7 compatibility: old relays may strip the new cause field, but
        // input-not-ready still requests the same independent safety brake.
        if (control_signaling_ack_recovery_.waiting()) command.operator_input_ready = false;
        payload["intent_fresh"] = sample.fresh;
        payload["intent_requires_fresh_input"] = true;
      }
      payload.update(command.to_json());
      envelope["payload"] = payload;
      if (native_control_trace_) {
        trace_record["ack_braking_wait"] = control_signaling_ack_recovery_.waiting();
        trace_record["ack_recovery_deadline_monotonic_ms"] = control_signaling_ack_recovery_.deadline();
        trace_record["last_ack_packet_age_ms"] = control_signaling_ack_window_.last_ack_packet_age_ms();
        trace_record["control_link_ready"] = command.control_link_ready;
        trace_record["operator_input_ready"] = command.operator_input_ready;
        trace_record["effective_gear"] = command.gear;
        trace_record["effective_throttle"] = command.throttle;
        trace_record["effective_steering"] = command.steering;
        trace_record["intent_fresh"] = payload.at("intent_fresh");
        trace_record["intent_requires_fresh_input"] = payload.at("intent_requires_fresh_input");
        trace_record["last_ack_seq"] = control_signaling_last_ack_seq_;
        trace_record["oldest_unacknowledged_seq"] =
            control_signaling_ack_window_.oldest_sequence();
        trace_record["unacknowledged_count"] =
            control_signaling_ack_window_.pending_count();
        trace_record["unacknowledged_age_ms"] = unacknowledged_age_ms;
        trace_record["ack_stall_age_ms"] = ack_stall_age_ms;
        trace_record["unacknowledged_limit"] =
            detail::NativeControlAcknowledgementWindow::kMaxPendingPackets;
        try {
          auto transport = control_signaling_websocket_ ? control_signaling_websocket_->transport_diagnostics(
              std::chrono::milliseconds(recovery_transition.entered ? 0 : 1000)) : Json(nullptr);
          if (!transport.is_null()) trace_record["transport"] = std::move(transport);
        } catch (const std::exception&) {
          trace_record["transport_sample_failed"] = true;
        }
      }
      if (!command.estop && recovery_transition.reconnect_due)
        throw std::runtime_error("native control ACK recovery deadline exceeded");
      if (control_signaling_ack_window_.pending_count() > 0) {
        if (!command.estop && control_signaling_ack_window_.pending_count() >=
            detail::NativeControlAcknowledgementWindow::kMaxPendingPackets) {
          throw std::runtime_error(
              "native control acknowledgement backlog exceeded 128 packets");
        }
      }
      command.recovery_sample_healthy = command.recovery_sample_healthy &&
          (direct_continuation || control_signaling_ack_recovery_.latest_sample_healthy()) &&
          (control_transport_sender_.path() != detail::ControlPath::DataChannel || control_dc_ack_recovery_.latest_sample_healthy());
      const auto revisions = control_source_revision_fence_.observe({
          {"input", sample.refresh_bad_sample_revision, sample.refresh_brake_revision},
          {"video:" + sample.owner_ui_instance_id, config_.mock_control_diagnostic ? 0 : sample.intent.critical_video_bad_sample_revision,
              config_.mock_control_diagnostic ? 0 : sample.intent.critical_video_brake_revision},
          {"clock", time_readiness.bad_cohort_revision, time_readiness.brake_cohort_revision},
          {"ack", control_signaling_ack_recovery_.bad_sample_revision(),
              control_signaling_ack_recovery_.brake_sample_revision()},
          {"dc_ack", control_dc_ack_recovery_.bad_sample_revision(), control_dc_ack_recovery_.brake_sample_revision()}});
      if (revisions.bad) ++recovery_bad_sample_revision_;
      if (revisions.brake) ++recovery_brake_revision_;
      command.recovery_bad_sample_revision = recovery_bad_sample_revision_;
      command.recovery_brake_revision = recovery_brake_revision_;
      envelope["payload"]["recovery_sample_healthy"] = command.recovery_sample_healthy;
      envelope["payload"]["recovery_bad_sample_revision"] = command.recovery_bad_sample_revision;
      envelope["payload"]["recovery_brake_revision"] = command.recovery_brake_revision;
      last_ack_seq = control_signaling_last_ack_seq_;
      send_started_at_utc_ms = clock_.now_ms();
      send_started_monotonic_ms = monotonic_now_ms();
      if (native_control_trace_) {
        trace_record["send_started_at_utc_ms"] = send_started_at_utc_ms;
        trace_record["send_started_monotonic_ms"] = send_started_monotonic_ms;
        trace_record["send_timeout_ms"] = 20;
      }
      if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::Send);
      if (config_.enable_experimental_direct_authority) {
        std::lock_guard authority_lock(direct_authority_mutex_);
        if (direct_authority_.observe_expiry(send_started_monotonic_ms)) native_control_intent_.invalidate(true);
        envelope["payload"]["direct_authority"] = direct_authority_.reference();
        if (!command.estop && !direct_authority_.valid(send_started_monotonic_ms)) {
          envelope["payload"]["throttle"] = envelope["payload"]["steering"] = 0.0;
          envelope["payload"]["operator_input_ready"] = false; envelope["payload"]["control_link_ready"] = false;
        }
      }
      if (!command.estop) control_transport_sender_.decorate(envelope["payload"]);
      if (!command.estop && feedback_transport_enabled_) {
        Json authority;
        { std::lock_guard lock(direct_authority_mutex_); authority = direct_authority_.reference(); }
        feedback_transport_sender_.observe(dc_ready, feedback_transport_dc_is_p2p_, current_media_attempt, authority,
            control_transport_sender_.path(), control_transport_sender_.status().at("epoch").get<std::uint64_t>(),
            send_started_monotonic_ms, recovery_timeout_ms);
        feedback_transport_sender_.decorate(envelope["payload"]);
      }
      if (envelope.at("payload").contains("control_transport") || envelope.at("payload").contains("feedback_request"))
        envelope["payload"]["native_mac"] = detail::transport_message_mac(envelope.at("payload"), transport_signing_key, false);
      if (emergency_payload.is_object()) envelope["payload"] = emergency_payload; // Exact same signed bytes on both carriers.
      // ACK processing and transport preparation can outlive an input lease.
      // A release, re-arm or newer input must retire this unsent snapshot too.
      const auto dispatch_checked_ms = monotonic_now_ms();
      const auto dispatch_sample = native_control_intent_.sample(dispatch_checked_ms);
      const auto dispatch_rejection = detail::native_control_dispatch_rejection(
          sample, dispatch_sample, dispatch_checked_ms - sample_started_monotonic_ms,
          native_control_intent_.lease_ms());
      if (!dispatch_rejection.empty()) {
        if (native_control_trace_) {
          trace_record["stage"] = "send_skipped";
          trace_record["reason"] = dispatch_rejection;
          trace_record["dispatch_check_monotonic_ms"] = dispatch_checked_ms;
          trace_record["dispatch_intent_seq"] = dispatch_sample.intent.intent_seq;
          trace_record["dispatch_intent_age_ms"] = dispatch_sample.age_ms;
          trace_record["dispatch_intent_fresh"] = dispatch_sample.fresh;
          trace_record["dispatch_requires_fresh_input"] = dispatch_sample.requires_fresh_input;
          trace_record["trace_completed_at_utc_ms"] = clock_.now_ms();
          trace_record["trace_completed_monotonic_ms"] = dispatch_checked_ms;
          native_control_trace_->enqueue(std::move(trace_record));
        }
        return false;
      }
      const auto actual_path = command.estop ? detail::ControlPath::Wss : control_transport_sender_.path();
      if (actual_path == detail::ControlPath::DataChannel) {
        // One latest slot: a blocked page cannot accumulate or retimestamp
        // commands. The scoped local endpoint and vehicle both expire them.
        control_transport_latest_dc_ = envelope.at("payload");
        control_transport_latest_dc_["bridge_owner"] = sample.owner_ui_instance_id;
        control_transport_latest_dc_["bridge_ui_epoch"] = sample.ui_control_epoch;
        control_transport_latest_dc_at_ms_ = send_started_monotonic_ms;
        control_dc_ack_window_.note_sent(sequence, send_started_monotonic_ms);
      } else {
        if (!control_signaling_websocket_ || !control_signaling_websocket_->connected())
          throw std::runtime_error("WSS fallback unavailable after direct path boundary");
        try {
          control_signaling_websocket_->send_json(envelope, std::chrono::milliseconds(20), stop);
        } catch (const WebSocketSendNotStarted& error) {
          // Nothing entered the stream: do not manufacture a link failure or
          // ACK progress. The next cycle samples current intent; the existing
          // input, ACK and vehicle command-gap watchdogs keep their deadlines.
          ++native_control_send_failures_;
          { std::lock_guard lock(native_control_status_mutex_); native_control_last_error_ = error.what(); }
          if (native_control_trace_) {
            trace_record["stage"] = "send_skipped";
            trace_record["reason"] = "preparation_deadline_expired_no_io";
            trace_record["send_io"] = control_signaling_websocket_->last_send_diagnostics();
            trace_record["trace_completed_at_utc_ms"] = clock_.now_ms();
            trace_record["trace_completed_monotonic_ms"] = monotonic_now_ms();
            native_control_trace_->enqueue(std::move(trace_record));
          }
          return false;
        }
        if (native_control_trace_) trace_record["send_io"] = control_signaling_websocket_->last_send_diagnostics();
      }
      send_completed_at_utc_ms = clock_.now_ms();
      send_completed_monotonic_ms = monotonic_now_ms();
      if (!emergency_payload.is_object()) {
        control_transport_sender_.note_sent(envelope.at("payload"), actual_path, send_started_monotonic_ms);
        if (feedback_transport_enabled_) feedback_transport_sender_.sent(envelope.at("payload"), actual_path, send_started_monotonic_ms);
        control_vehicle_result_latency_.note_sent(sequence, sample.intent.intent_seq, send_started_monotonic_ms, actual_path == detail::ControlPath::DataChannel);
      } // Dual stop timing uses its earliest publication and actual car-reported receive carrier, never a fabricated WSS RTT.
      if (actual_path == detail::ControlPath::Wss)
        control_signaling_ack_window_.note_sent(sequence, send_completed_monotonic_ms, command.estop);
      if (native_control_trace_) {
        trace_record["dispatch_path"] = detail::control_path_name(actual_path);
        trace_record["dispatch_kind"] = actual_path == detail::ControlPath::Wss ? "socket_write" : "latest_bridge_publication";
        trace_record["control_transport"] = envelope.at("payload").value("control_transport", Json(nullptr));
      }
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
      trace_record["ack_stall_age_ms"] = ack_stall_age_ms;
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
      trace_record["socket_lock_held_ms"] = socket_lock_released_ms - socket_lock_acquired_ms;
      native_control_trace_->enqueue(std::move(trace_record));
    }
    return true;
  } catch (const std::exception& error) {
    if (native_control_observation_state_) native_control_observation_state_->begin(ControlObservationPhase::Failure);
    if (native_control_trace_) {
      trace_record["stage"] = "send_failed";
      // Observe the socket before failure handling closes it, without waiting
      // for another operation or changing the existing reconnect decision.
      try {
        std::unique_lock websocket_lock(control_signaling_websocket_mutex_, std::try_to_lock);
        if (websocket_lock.owns_lock() && control_signaling_websocket_) {
          trace_record["failure_transport"] = control_signaling_websocket_->transport_diagnostics();
          trace_record["send_io"] = control_signaling_websocket_->last_send_diagnostics();
        }
      } catch (const std::exception&) {
        trace_record["transport_sample_failed"] = true;
      }
      trace_record["error"] = error.what();
      trace_record["failure_at_utc_ms"] = clock_.now_ms();
      trace_record["failure_monotonic_ms"] = monotonic_now_ms();
      if (trace_record.contains("send_started_monotonic_ms"))
        trace_record["send_call_ms"] = monotonic_now_ms() - trace_record.at("send_started_monotonic_ms").get<std::int64_t>();
      if (socket_lock_released_ms > 0)
        trace_record["socket_lock_held_ms"] = socket_lock_released_ms - socket_lock_acquired_ms;
      trace_record["sample_processing_ms"] = std::max<std::int64_t>(
          0,
          monotonic_now_ms() - sample_started_monotonic_ms);
      native_control_trace_->enqueue(std::move(trace_record));
    }
    note_native_control_failure(error.what(), session, generation);
    return false;
  }
}

void DriverConsoleRuntime::wake_native_control_sender() noexcept {
  if (!native_control_sender_wakeup_.notify())
    native_control_sender_wakeup_failures_.fetch_add(1, std::memory_order_relaxed);
  native_control_cv_.notify_all();
}

void DriverConsoleRuntime::native_control_sender_loop(std::stop_token stop_token) {
  std::stop_callback cancellation(stop_token, [this] { wake_native_control_sender(); });
#if defined(__APPLE__)
  // Each short input/ACK exchange is latency-sensitive interactive work. The
  // default pthread QoS can defer it while unrelated desktop work runs; keep
  // the existing wall-clock deadlines even if this scheduling hint fails.
  const auto scheduling_result = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
  qos_class_t effective_qos = QOS_CLASS_UNSPECIFIED;
  int relative_priority = 0;
  const auto scheduling_query_result = pthread_get_qos_class_np(
      pthread_self(), &effective_qos, &relative_priority);
  if (native_control_trace_) native_control_trace_->enqueue({
      {"stage", "sender_thread_scheduling"}, {"platform", "macos"},
      {"requested_qos", static_cast<int>(QOS_CLASS_USER_INTERACTIVE)},
      {"set_result", scheduling_result}, {"query_result", scheduling_query_result},
      {"effective_qos", static_cast<int>(effective_qos)},
      {"relative_priority", relative_priority}});
#endif
  const auto period = std::chrono::milliseconds(
      std::max(1, static_cast<int>(std::llround(1000.0 / config_.rate_hz))));
  auto next_send = std::chrono::steady_clock::now();
  bool socket_wait_allowed = true;
  while (!stop_token.stop_requested()) {
    bool urgent = native_control_estop_wakeup_.exchange(false);
    const auto next_poll = std::min(next_send,
        std::chrono::steady_clock::now() + std::chrono::milliseconds(20));
    bool socket_waited = false;
    if (!urgent && socket_wait_allowed && std::chrono::steady_clock::now() < next_poll) {
      try {
        std::shared_ptr<const detail::SocketReadiness> waiter;
        {
          std::unique_lock lock(control_signaling_websocket_mutex_, std::try_to_lock);
          if (lock.owns_lock() && control_signaling_websocket_)
            waiter = control_signaling_websocket_->read_waiter();
        }
        // Only poll the retained endpoint here: never hold the ownership lock
        // while waiting, and never access CURL from a second thread.
        if (waiter) {
          const auto remaining = std::max(std::chrono::milliseconds::zero(),
              std::chrono::ceil<std::chrono::milliseconds>(next_poll - std::chrono::steady_clock::now()));
          if (waiter->wait(remaining, &native_control_sender_wakeup_))
            native_control_socket_read_wakeups_.fetch_add(1, std::memory_order_relaxed);
          socket_waited = true;
          urgent = native_control_estop_wakeup_.exchange(false);
        }
      } catch (const std::exception&) {
        // This observer is an optimization, not evidence of network progress
        // or failure. Keep the existing bounded receive check on any failure.
        native_control_socket_wait_failures_.fetch_add(1, std::memory_order_relaxed);
      }
    }
    if (!urgent && !socket_waited) {
      std::unique_lock lock(native_control_wait_mutex_);
      urgent = native_control_cv_.wait_until(lock, next_poll, [&] {
        return stop_token.stop_requested() ||
            native_control_estop_wakeup_.exchange(false);
      });
    }
    if (stop_token.stop_requested()) break;
    if (!urgent && std::chrono::steady_clock::now() < next_send) {
      // Record ACK arrival between sends, without increasing command rate or
      // including the sender's idle period in recovery ACK age.
      // If another owner prevented draining a readable socket, back off once
      // on the original timer instead of spinning on its unread bytes.
      socket_wait_allowed = poll_native_control_acknowledgements();
      continue;
    }
    native_control_scheduled_at_monotonic_ms_.store(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            next_send.time_since_epoch())
            .count(),
        std::memory_order_relaxed);
    static_cast<void>(send_native_control_sample(stop_token));
    socket_wait_allowed = true;
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
      renewal_due = !session_id_.empty() && !control_token_.empty() &&
          control_token_renew_at_ms_ <= clock_.now_ms();
      renewal_session = session_id_;
      renewal_generation = control_session_generation_;
    }
    if (!renewal_due) continue;
    try {
      static_cast<void>(renew_control_authority());
    } catch (const std::exception& error) {
      if (detail::transient_cloud_failure(error)) {
        std::lock_guard lock(control_signaling_websocket_mutex_);
        if (direct_control_continuation_available_locked()) continue;
      }
      note_native_control_failure(
          std::string("control authority renewal failed: ") + error.what(),
          renewal_session,
          renewal_generation);
    }
  }
}

void DriverConsoleRuntime::time_sync_loop(std::stop_token stop_token) {
  while (!stop_token.stop_requested()) {
    {
      std::unique_lock lock(native_control_wait_mutex_);
      const auto poll_ms = clock_.status().acceptable(config_.max_time_sync_uncertainty_ms) ? 500 : 100;
      native_control_cv_.wait_for(lock, std::chrono::milliseconds(poll_ms),
                                 [&] { return stop_token.stop_requested(); });
    }
    if (stop_token.stop_requested()) break;
    std::string session;
    {
      std::lock_guard lock(mutex_);
      if (driver_token_.empty()) continue;
      session = session_id_;
    }
    if (!clock_.refresh_due(config_.time_sync_interval_ms)) continue;
    const auto started = monotonic_now_ms();
    std::string error;
    Json http_failure;
    Json sample_failure;
    try {
      static_cast<void>(refresh_time_sync());
    } catch (const std::exception& failure) {
      error = std::string(failure.what()).substr(0, 512);
      if (const auto* transport = dynamic_cast<const HttpTransportError*>(&failure)) http_failure = transport->timing();
      if (const auto* sample = dynamic_cast<const TimeSyncSampleError*>(&failure)) { sample_failure = sample->diagnostics(); http_failure = sample_failure.value("http_failure", Json(nullptr)); }
      if (clock_.status().acceptable(config_.max_time_sync_uncertainty_ms)) clock_.mark_refresh_failed();
      // Keep the transport/session alive, but no previously held motion may
      // become executable merely because a later time sample is good.
      native_control_intent_.invalidate(true);
    }
    if (native_control_trace_) native_control_trace_->enqueue({
        {"stage", "time_sync_refresh"}, {"trace_session_id", session},
        {"duration_ms", monotonic_now_ms() - started},
        {"time_sync", clock_.status().to_json(true)}, {"error", error}, {"http_failure", std::move(http_failure)}, {"sample_failure", std::move(sample_failure)},
        {"safety_action", error.empty() ? "fresh_input_after_recovery" : "brake_with_session_retained"}});
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
          http_.url_encode(driver_id()),
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
  const auto status = clock_.synchronize(time_sync_http_, signaling_http_url_, config_.time_sync_samples,
                                       config_.max_time_sync_uncertainty_ms);
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
  std::string identity;
  std::string control_token;
  std::int64_t renew_at_ms = 0;
  {
    std::lock_guard lock(mutex_);
    token = driver_token_;
    session = session_id_;
    identity = driver_id();
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
        {{"actor", identity}, {"token", token}});
  } catch (const HttpStatusError& error) {
    if (error.status() == 401 || error.status() == 403 || error.status() == 404 || error.status() == 409) {
      close_signaling_websocket();
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      if (session_id_ == session) {
        session_id_.clear();
        control_token_.clear();
        control_token_expires_at_ms_ = 0;
        control_token_renew_at_ms_ = 0;
        sequence_ = 0;
        reset_control_profile_locked();
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
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      driver_token_.clear();
      driver_token_expires_at_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_ms_ = 0;
    }
  }
  const auto credential = password.empty() ? password_ : std::string(password);
  if (credential.empty()) throw std::invalid_argument("driver password is required");
  const auto response = http_.post_json_response(
      signaling_http_url_ + "/auth/driver_login",
      {{"driver_id", driver_id()}, {"password", credential}});
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

std::string DriverConsoleRuntime::driver_id() const {
  std::lock_guard lock(identity_mutex_);
  return driver_id_;
}

Json DriverConsoleRuntime::login(std::string_view password, std::string_view requested_driver_id) {
  std::lock_guard authentication_lock(authentication_mutex_);
  const auto identity = requested_driver_id.empty() ? driver_id() : std::string(requested_driver_id);
  if (identity.empty() || identity.size() > 128 ||
      std::any_of(identity.begin(), identity.end(), [](unsigned char ch) { return std::isspace(ch) || std::iscntrl(ch); })) {
    throw std::invalid_argument("driver ID is required and must not contain whitespace");
  }
  if (identity != driver_id()) {
    // Login, logout and session changes share the authentication lock. A failed
    // logout retains its token for retry and must not allow an identity switch.
    std::lock_guard lock(mutex_);
    if (!driver_token_.empty() || !session_id_.empty()) {
      throw HttpStatusError(409, "log out before switching driver ID");
    }
    if (password.empty()) throw std::invalid_argument("driver password is required");
    std::lock_guard identity_lock(identity_mutex_);
    driver_id_ = identity;
    password_.clear();
  }
  return login_locked(password);
}

Json DriverConsoleRuntime::fetch_authorized_vehicles(
    std::string_view token,
    std::int64_t expires_at_ms) {
  const auto response = http_.get_json(
      signaling_http_url_ + "/drivers/" + http_.url_encode(driver_id()) + "/vehicles",
      {{"X-Mine-Teleop-Driver-Token", std::string(token)}});
  auto listed = response.value("vehicles", Json::array());
  if (!listed.is_array()) throw std::runtime_error("authorized vehicle response is invalid");
  for (auto& vehicle : listed) {
    if (!vehicle.is_object()) continue;
    const bool compatible = vehicle.value("native_control_capabilities", "") == kNativeControlCapabilities;
    vehicle["native_control_compatible"] = compatible;
    if (vehicle.value("online", false) && !compatible) {
      vehicle["controllable"] = false;
      vehicle["control_issue_code"] = "vehicle_control_capability_mismatch";
      vehicle["control_issue_message"] = "车辆原生控制能力不匹配或未知，请更新车端包";
    }
  }
  std::string service_instance_id;
  {
    std::lock_guard lock(mutex_);
    authorized_vehicles_ = listed;
    service_instance_id = signaling_service_instance_id_;
    signaling_available_ = true;
  }
  return {
      {"authenticated", true},
      {"driver_id", driver_id()},
      {"token_expires_at_utc_ms", expires_at_ms},
      {"service_instance_id", service_instance_id},
      {"vehicles", listed},
      {"native_control_capabilities", response.value("native_control_capabilities", "")},
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
    reset_native_control_state();
    {
      std::lock_guard lock(mutex_);
      driver_token_.clear();
      driver_token_expires_at_ms_ = 0;
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
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
    bool direct_continuation;
    { std::lock_guard lock(control_signaling_websocket_mutex_); direct_continuation = direct_control_continuation_available_locked(); }
    return {
        {"direct_authority_continuation", direct_continuation},
        {"authenticated", true},
        {"driver_id", driver_id()},
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

void DriverConsoleRuntime::cancel_pending_connect() {
  ++connect_cancellation_generation_;
  approval_wait_cv_.notify_all();
}

Json DriverConsoleRuntime::await_control_session(
    const std::string& vehicle_id, const std::string& token, std::uint64_t generation) {
  const auto current_driver = driver_id();
  std::string approval_id;
  auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(10);
  try {
    while (true) {
      if (connect_cancellation_generation_.load() != generation) throw std::runtime_error("连接申请已取消");
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("手机审批已超时");
      Json request = {{"driver_id", current_driver}, {"vehicle_id", vehicle_id}, {"token", token},
          {"required_native_control_capabilities", kNativeControlCapabilities}};
      request["required_control_transport_capability"] = detail::kControlTransportCapability;
      if (config_.enable_experimental_direct_authority)
        request["required_direct_authority_capability"] = detail::kDirectAuthorityCapability;
      if (!approval_id.empty()) request["approval_request_id"] = approval_id;
      try {
        return http_.post_json_response(signaling_http_url_ + "/sessions", request);
      } catch (const HttpStatusError& error) {
        if (error.status() != 409 || error.issue_code() != "mobile_approval_pending") throw;
        const auto pending = Json::parse(error.response_body());
        const auto request_id = required_string(pending, "request_id");
        if (!approval_id.empty() && approval_id != request_id) throw std::runtime_error("连接申请已被替换");
        approval_id = request_id;
        const auto remaining_ms = std::clamp<std::int64_t>(required_int64(pending, "remaining_ms"), 0, 600000);
        deadline = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining_ms));
        {
          std::lock_guard lock(mutex_);
          pending_mobile_approval_ = {{"request_id", approval_id}, {"vehicle_id", vehicle_id},
                                     {"expires_at_utc_ms", pending.at("expires_at_utc_ms")}};
        }
      }
      std::unique_lock wait_lock(approval_wait_mutex_);
      approval_wait_cv_.wait_until(wait_lock,
          std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(500)),
          [&] { return connect_cancellation_generation_.load() != generation; });
    }
  } catch (...) {
    if (!approval_id.empty()) {
      try {
        static_cast<void>(http_.post_json_response(signaling_http_url_ + "/sessions/approval/cancel",
            {{"driver_id", current_driver}, {"vehicle_id", vehicle_id}, {"token", token},
             {"approval_request_id", approval_id}}));
      } catch (const std::exception&) {
        // Never retry an uncertain grant automatically; normal presence/lease expiry remains in force.
      }
    }
    throw;
  }
}

Json DriverConsoleRuntime::connect(std::string_view requested_vehicle_id, std::string_view ui_instance_id) {
  if (!ui_instance_id.empty() && (ui_instance_id.size() > 128 ||
      !std::all_of(ui_instance_id.begin(), ui_instance_id.end(), [](unsigned char value) {
        return std::isalnum(value) || value == '-' || value == '_';
      }))) throw std::invalid_argument("connect ui_instance_id is invalid");
  std::unique_lock operation(authentication_mutex_, std::try_to_lock);
  if (!operation.owns_lock()) throw std::runtime_error("已有连接或退出操作正在进行");
  // Capture cancellation before checking shutdown so a concurrent shutdown cannot
  // leave this request waiting on a newer, uncancelled generation.
  const auto generation = connect_cancellation_generation_.fetch_add(1) + 1;
  if (shutting_down_.load()) throw std::runtime_error("控制端正在退出");
  {
    std::lock_guard lock(mutex_);
    pending_connect_generation_ = generation;
    pending_connect_ui_instance_id_ = ui_instance_id;
  }
  try {
    auto result = connect_locked(requested_vehicle_id, generation, ui_instance_id);
    std::lock_guard lock(mutex_);
    pending_mobile_approval_ = Json::object();
    pending_connect_generation_.reset();
    pending_connect_ui_instance_id_.clear();
    result["connect_cancellation_generation"] = generation;
    result["connect_operation_generation"] = generation;
    return result;
  } catch (...) {
    std::lock_guard lock(mutex_);
    pending_mobile_approval_ = Json::object();
    pending_connect_generation_.reset();
    pending_connect_ui_instance_id_.clear();
    throw;
  }
}

Json DriverConsoleRuntime::connect_locked(std::string_view requested_vehicle_id, std::uint64_t generation,
                                         std::string_view ui_instance_id) {
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
  if (config_.mock_control_diagnostic && !target.starts_with("test_bench_"))
    throw std::invalid_argument("mock control diagnostic requires test_bench_ identity");

  auto validate_target = [&](bool require_available = true) {
    const auto catalog = vehicles();
    if (catalog.value("native_control_capabilities", "") != kNativeControlCapabilities)
      throw std::runtime_error("云端原生控制能力不匹配：缺少恢复反馈或 ACK 制动等待支持，请更新云端包");
    const auto available = catalog.at("vehicles");
    const auto selected = std::find_if(available.begin(), available.end(), [&](const auto& value) {
      return value.value("vehicle_id", "") == target;
    });
    if (selected == available.end()) throw std::invalid_argument("vehicle is not authorized for this driver");
    if (selected->value("native_control_capabilities", "") != kNativeControlCapabilities)
      throw std::runtime_error("车辆原生控制能力不匹配或未知，请更新并重新注册车端包");
    if (require_available && !selected->value("controllable", false)) {
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
        // Active catalogue entries are intentionally unavailable to a new
        // owner, but their declared vehicle capability still needs checking.
        validate_target(false);
        connect_signaling_websocket(current_session, current_token);
        static_cast<void>(
            connect_control_signaling_websocket(current_session, current_token));
        std::lock_guard lock(mutex_);
        return {
            {"runtime", "cpp"},
            {"driver_id", driver_id()},
            {"vehicle_id", vehicle_id_},
            {"connected", true},
            {"session_id", session_id_},
            {"control_session_generation", control_session_generation_},
            {"ui_control_epoch", native_control_intent_.sample(monotonic_now_ms()).ui_control_epoch},
            {"connected_at_ms", connected_at_ms_},
            {"time_sync", clock_.status().to_json()},
        };
      }
    } else {
      validate_target();
      target_validated = true;
      static_cast<void>(end_session_locked("driver_vehicle_switch"));
    }
    if (current_vehicle == target) {
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_ms_ = 0;
    }
  }

  if (!target_validated) validate_target();
  reset_native_control_state();
  const auto session = await_control_session(target, current_token, generation);
  const auto session_id = required_string(session, "session_id");
  const auto control_token = required_string(session, "control_token");
  const auto connected_at_ms = clock_.now_ms();
  const auto control_token_expires_at_ms = required_int64(session, "control_token_expires_at_utc_ms");
  if (control_token_expires_at_ms <= connected_at_ms) {
    throw std::runtime_error("new control authority lease is already expired");
  }
  std::uint64_t control_session_generation = 0;
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    {
      std::lock_guard lock(mutex_);
      vehicle_id_ = target;
      session_id_ = session_id;
      session_creator_ui_instance_id_ = ui_instance_id;
      control_token_ = control_token;
      control_transport_mac_key_ = session.value("transport_mac_key", "");
      if (config_.enable_experimental_direct_authority) {
        std::lock_guard authority_lock(direct_authority_mutex_);
        const auto& authority = session.at("direct_authority_configuration");
        direct_authority_.configure(authority.at("issuer").get<std::string>(), vehicle_id_, driver_id(), session_id_,
            authority.at("connection_generation").get<std::uint64_t>(), authority.at("maximum_duration_ms").get<std::int64_t>());
      }
      control_token_expires_at_ms_ = control_token_expires_at_ms;
      control_token_renew_at_ms_ = control_lease_renew_at(connected_at_ms, control_token_expires_at_ms);
      sequence_ = 0;
      control_sequence_ = 0;
      control_session_generation = ++control_session_generation_;
      reset_control_profile_locked();
      connected_at_ms_ = connected_at_ms;
    }
    native_control_intent_.begin_session(monotonic_now_ms());
  }
  wake_native_control_sender();
  try {
    if (connect_cancellation_generation_.load() != generation) throw std::runtime_error("连接申请已取消");
    connect_signaling_websocket(session_id, current_token);
    static_cast<void>(
        connect_control_signaling_websocket(session_id, current_token));
    if (connect_cancellation_generation_.load() != generation) throw std::runtime_error("连接申请已取消");
  } catch (...) {
    const auto failure = std::current_exception();
    try {
      static_cast<void>(end_session_locked("signaling_websocket_connect_failed"));
    } catch (const std::exception&) {
      reset_native_control_state();
      std::lock_guard lock(mutex_);
      if (session_id_ == session_id) {
        session_id_.clear();
        control_token_.clear();
        control_token_expires_at_ms_ = 0;
        control_token_renew_at_ms_ = 0;
        sequence_ = 0;
        reset_control_profile_locked();
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
      {"driver_id", driver_id()},
      {"vehicle_id", target},
      {"connected", true},
      {"session_id", session_id},
      {"control_session_generation", control_session_generation},
      {"ui_control_epoch", std::uint64_t{0}},
      {"connected_at_ms", connected_at_ms},
      {"time_sync", clock_.status().to_json()},
  };
}

Json DriverConsoleRuntime::end_session(std::string_view reason) {
  cancel_pending_connect();
  std::lock_guard operation(authentication_mutex_);
  return end_session_locked(reason);
}

Json DriverConsoleRuntime::end_session_locked(std::string_view reason) {
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
    return {{"driver_id", driver_id()}, {"connected", false}, {"session_id", ""}};
  }
  if (token.empty()) throw std::runtime_error("driver login is required to end the session");
  auto response = http_.post_json_response(
      signaling_http_url_ + "/sessions/" + http_.url_encode(session) + "/end",
      {{"actor", driver_id()}, {"token", token}, {"reason", std::string(reason)}});
  close_signaling_websocket();
  reset_native_control_state();
  {
    std::lock_guard lock(mutex_);
    if (session_id_ == session) {
      session_id_.clear();
      control_token_.clear();
      control_token_expires_at_ms_ = 0;
      control_token_renew_at_ms_ = 0;
      sequence_ = 0;
      reset_control_profile_locked();
      connected_at_ms_ = 0;
    }
  }
  response["driver_id"] = driver_id();
  response["connected"] = false;
  response["session_id"] = session;
  return response;
}

void DriverConsoleRuntime::prepare_shutdown() {
  shutting_down_.store(true);
  {
    std::lock_guard lock(log_export_mutex_);
    if (log_export_) log_export_->cancel();
  }
  // The HTTP server joins active connect handlers before disconnect() runs.
  cancel_pending_connect();
  // Drop the local control generation before any potentially slow logout I/O.
  reset_native_control_state();
}

Json DriverConsoleRuntime::disconnect(std::string_view reason) {
  cancel_pending_connect();
  std::lock_guard operation(authentication_mutex_);
  return disconnect_locked(reason);
}

Json DriverConsoleRuntime::disconnect_scoped(const Json& input) {
  return release_scoped(input, false);
}

Json DriverConsoleRuntime::end_session_scoped(const Json& input) {
  return release_scoped(input, true);
}

Json DriverConsoleRuntime::release_scoped(const Json& input, bool end_session_only) {
  const auto invalid_scope = [] {
    throw Conflict("退出请求范围缺失或格式无效，请刷新当前会话", "driver_disconnect_scope_invalid");
  };
  if (!input.is_object()) invalid_scope();
  const auto scope_string = [&](const char* key, bool allow_empty = false) {
    if (!input.contains(key) || !input.at(key).is_string()) invalid_scope();
    const auto value = input.at(key).get<std::string>();
    if (!allow_empty && value.empty()) invalid_scope();
    return value;
  };
  const auto scope_uint = [&](const char* key) {
    if (!input.contains(key)) invalid_scope();
    const auto& value = input.at(key);
    if (!value.is_number_unsigned() &&
        !(value.is_number_integer() && value.get<std::int64_t>() >= 0)) invalid_scope();
    return value.get<std::uint64_t>();
  };
  const auto expected_session = scope_string("expected_session_id", true);
  const auto expected_driver = scope_string("expected_driver_id");
  const auto ui = scope_string("ui_instance_id");
  const auto expected_generation = scope_uint("expected_control_session_generation");
  const auto expected_epoch = scope_uint("expected_ui_control_epoch");
  std::optional<std::uint64_t> expected_connect_generation;
  if (input.contains("expected_connect_cancellation_generation"))
    expected_connect_generation = scope_uint("expected_connect_cancellation_generation");
  const auto reason = input.value("reason", std::string(end_session_only ? "driver_session_end" : "driver_console_disconnect"));
  // All callers hold native_control_update_mutex_ and mutex_ here. The store
  // read is deliberately const: stale requests cannot expire a live input.
  const auto verify_scope = [&] {
    const auto [owner, epoch] = native_control_intent_.ownership_scope();
    if (driver_id() != expected_driver || session_id_ != expected_session ||
        control_session_generation_ != expected_generation || epoch != expected_epoch ||
        (!owner.empty() && owner != ui) ||
        (!session_id_.empty() && owner.empty() &&
            (session_creator_ui_instance_id_.empty() || session_creator_ui_instance_id_ != ui)) ||
        (expected_connect_generation && *expected_connect_generation != connect_cancellation_generation_.load())) {
      throw Conflict("退出请求属于旧身份、会话或页面；当前会话已保留", "driver_disconnect_scope_mismatch");
    }
  };
  std::unique_lock operation(authentication_mutex_, std::try_to_lock);
  if (!operation.owns_lock()) {
    // Approval waits intentionally own authentication_mutex_. Only the page
    // and exact operation which initiated that wait may wake it for release.
    {
      std::lock_guard update_lock(native_control_update_mutex_);
      std::lock_guard lock(mutex_);
      verify_scope();
      if (!expected_connect_generation || !pending_connect_generation_ ||
          *pending_connect_generation_ != *expected_connect_generation ||
          pending_connect_ui_instance_id_.empty() || pending_connect_ui_instance_id_ != ui ||
          logout_scope_reserved_) {
        throw Conflict("另一页面或操作正在连接／退出，当前请求未取消它", "driver_disconnect_operation_mismatch");
      }
      cancel_pending_connect();
    }
    operation.lock();
    // Cancellation is this request's authorized mutation. Compare its next
    // generation below; a replacement operation must still fail the guard.
    expected_connect_generation = *expected_connect_generation + 1;
  }
  {
    std::lock_guard update_lock(native_control_update_mutex_);
    std::lock_guard lock(mutex_);
    verify_scope();
    if (logout_scope_reserved_)
      throw Conflict("安全退出已在进行中，请等待操作完成", "driver_disconnect_in_progress");
    // The authentication lock prevents a new session; this reservation also
    // prevents a new page taking ownership between validation and cloud I/O.
    logout_scope_reserved_ = true;
  }
  const auto release_reservation = [&] {
    std::lock_guard update_lock(native_control_update_mutex_);
    std::lock_guard lock(mutex_);
    logout_scope_reserved_ = false;
  };
  try {
    auto response = end_session_only ? end_session_locked(reason) : disconnect_locked(reason);
    release_reservation();
    response["release_scope_validated"] = true;
    if (!end_session_only) response["disconnect_scope_validated"] = true;
    return response;
  } catch (...) {
    release_reservation();
    throw;
  }
}

Json DriverConsoleRuntime::disconnect_locked(std::string_view reason) {
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
    control_token_expires_at_ms_ = 0;
    control_token_renew_at_ms_ = 0;
    sequence_ = 0;
    reset_control_profile_locked();
    connected_at_ms_ = 0;
    authorized_vehicles_ = Json::array();
    password_.clear();
    return {{"driver_id", driver_id()}, {"state", "offline"}, {"session_id", session}};
  }
  auto response = http_.post_json_response(
      signaling_http_url_ + "/auth/driver_logout",
      {{"driver_id", driver_id()}, {"token", token}, {"reason", std::string(reason)}});
  close_signaling_websocket();
  reset_native_control_state();
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
      reset_control_profile_locked();
      connected_at_ms_ = 0;
      authorized_vehicles_ = Json::array();
      password_.clear();
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
      if (detail::transient_cloud_failure(error)) {
        std::lock_guard lock(control_signaling_websocket_mutex_);
        if (direct_control_continuation_available_locked()) return {{"messages", Json::array()}, {"direct_authority_continuation", true}};
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
          control_token_expires_at_ms_ = 0;
          control_token_renew_at_ms_ = 0;
          sequence_ = 0;
          reset_control_profile_locked();
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
    if (media_scope_session_ != session) {
      media_scope_session_ = session; media_scope_id_.clear(); media_offer_count_ = 0; media_scope_legacy_allowed_ = true;
    }
    for (const auto& message : messages) {
      if (message.value("type", "") != "webrtc_offer") continue;
      const auto payload = message.value("payload", Json::object());
      const auto id = payload.value("media_negotiation_id", "");
      if (id.empty() || id != media_scope_id_) { ++media_offer_count_; media_scope_id_ = id; }
      media_scope_legacy_allowed_ = payload.value("legacy_bootstrap_allowed", media_offer_count_ == 1);
      control_transport_vehicle_capable_ = payload.value("control_transport_capability", "") == detail::kControlTransportCapability;
      emergency_stop_vehicle_capable_ = payload.value("emergency_stop_capability", "") == detail::kEmergencyStopCapability;
      feedback_transport_vehicle_capable_ = payload.value("feedback_transport_capability", "") == detail::kFeedbackTransportCapability;
    }
  }
  return {{"session_id", session}, {"messages", messages}};
}

void DriverConsoleRuntime::validate_media_source_scope_locked(const Json& input) const {
  if (!input.contains("source_session_id") && !input.contains("source_control_session_generation")) return;
  if (!input.contains("source_session_id") || !input.at("source_session_id").is_string() ||
      !input.contains("source_control_session_generation") ||
      !input.at("source_control_session_generation").is_number_unsigned())
    throw std::invalid_argument("WebRTC source session scope is incomplete");
  if (input.at("source_session_id") != session_id_ ||
      input.at("source_control_session_generation").get<std::uint64_t>() != control_session_generation_)
    throw std::runtime_error("WebRTC source session changed");
}

void DriverConsoleRuntime::validate_media_negotiation_locked(const Json& input) const {
  if (media_scope_session_ != session_id_ || media_offer_count_ == 0) return;
  const auto id = input.value("media_negotiation_id", "");
  if (!id.empty() && !media_scope_id_.empty() && id != media_scope_id_)
    throw std::runtime_error("WebRTC negotiation changed; discard this old media request");
  // A retry requires both peers to identify the offer. Source session alone
  // cannot distinguish delayed responses to two offers in the same session.
  if ((!media_scope_legacy_allowed_ && id.empty()) ||
      (media_offer_count_ > 1 && (id.empty() || media_scope_id_.empty())))
    throw std::runtime_error("媒体信令版本不兼容：重试缺少协商标识。请保持车辆停止，统一更新控制端和车端包，重新打开控制页面并建立新会话。");
}

Json DriverConsoleRuntime::send_signaling_message(
    std::string_view type, const Json& payload, const Json* source_scope) {
  std::lock_guard send_lock(signaling_send_mutex_);
  std::string token;
  std::string session;
  std::string identity;
  std::string vehicle;
  std::uint64_t sequence = 0;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty() || driver_token_.empty()) throw std::runtime_error("driver console is not connected");
    if (source_scope) validate_media_source_scope_locked(*source_scope);
    if (source_scope) validate_media_negotiation_locked(payload);
    token = driver_token_;
    session = session_id_;
    identity = driver_id();
    vehicle = vehicle_id_;
    sequence = ++sequence_;
  }
  const ProtocolMetadata metadata{
      kProtocolVersion, vehicle, identity, session, sequence, clock_.now_ms()};
  auto request = metadata.to_json();
  request["sender"] = identity;
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
            control_token_expires_at_ms_ = 0;
            control_token_renew_at_ms_ = 0;
            sequence_ = 0;
            reset_control_profile_locked();
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
  std::string identity;
  {
    std::lock_guard lock(mutex_);
    if (session_id_.empty() || driver_token_.empty()) throw std::runtime_error("driver console is not connected");
    token = driver_token_;
    session = session_id_;
    identity = driver_id();
  }
  return http_.get_json(
      signaling_http_url_ + "/sessions/" + http_.url_encode(session) + "/ice_servers?actor=" +
          http_.url_encode(identity),
      {{"X-Mine-Teleop-Driver-Token", token}});
}

Json DriverConsoleRuntime::send_media_fallback(const Json& input) {
  if (!input.is_object() || lower(input.value("codec", "")) != "h264") {
    throw std::invalid_argument("media fallback must request H.264");
  }
  Json payload{{"codec", "h264"}, {"reason", input.value("reason", "browser_decode_failure")}};
  if (input.contains("media_negotiation_id")) {
    if (!input["media_negotiation_id"].is_string() || input["media_negotiation_id"].get_ref<const std::string&>().size() > 128)
      throw std::invalid_argument("WebRTC negotiation identity is invalid");
    if (input["media_negotiation_id"] != "") payload["media_negotiation_id"] = input["media_negotiation_id"];
  }
  return send_signaling_message("media_fallback", payload, &input);
}

Json DriverConsoleRuntime::send_webrtc_answer(const Json& input) {
  if (!input.is_object() || input.value("type", "") != "answer" || input.value("sdp", "").empty()) {
    throw std::invalid_argument("WebRTC answer must contain type=answer and SDP");
  }
  Json payload{{"type", "answer"}, {"sdp", input.at("sdp")}};
  if (input.contains("media_negotiation_id")) {
    if (!input["media_negotiation_id"].is_string() || input["media_negotiation_id"].get_ref<const std::string&>().size() > 128)
      throw std::invalid_argument("WebRTC negotiation identity is invalid");
    if (input["media_negotiation_id"] != "") payload["media_negotiation_id"] = input["media_negotiation_id"];
  }
  return send_signaling_message("webrtc_answer", payload, &input);
}

Json DriverConsoleRuntime::send_webrtc_ice_candidate(const Json& input) {
  auto candidate = input.contains("candidate") && input.at("candidate").is_object() ? input.at("candidate") : input;
  if (!candidate.is_object() || candidate.value("candidate", "").empty()) {
    throw std::invalid_argument("WebRTC ICE candidate is required");
  }
  if (input.contains("media_negotiation_id")) {
    if (!input["media_negotiation_id"].is_string() || input["media_negotiation_id"].get_ref<const std::string&>().size() > 128)
      throw std::invalid_argument("WebRTC negotiation identity is invalid");
    if (input["media_negotiation_id"] != "") candidate["media_negotiation_id"] = input["media_negotiation_id"];
  }
  return send_signaling_message("ice_candidate", candidate, &input);
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
    validate_media_source_scope_locked(input);
    validate_media_negotiation_locked(input);
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
        {{"actor", driver_id()},
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
      {"parking_idle_timeout_ms", parking_idle_timeout_ms_},
      {"consecutive_late_frame_limit", consecutive_late_frame_limit_},
      {"critical_camera_stale_brake_ms", critical_camera_stale_brake_ms_},
      {"control_recovery_timeout_ms", control_recovery_timeout_ms_},
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
  parking_idle_timeout_ms_ = 500;
  consecutive_late_frame_limit_ = 3;
  critical_camera_stale_brake_ms_ = 500;
  control_recovery_timeout_ms_ = 5000;
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
           "motor_torque_rise_rate_nm_per_s",
           "parking_idle_timeout_ms",
           "consecutive_late_frame_limit", "critical_camera_stale_brake_ms", "control_recovery_timeout_ms"}) {
    if (!input.contains(field) || !input.at(field).is_number()) {
      throw std::invalid_argument(std::string("control profile requires numeric ") + field);
    }
  }
  if (!input.contains("profile_version") || !input.at("profile_version").is_number_integer() ||
      input.at("profile_version").get<int>() != kSessionControlProfileVersion) {
    throw std::invalid_argument("control profile requires profile_version 7");
  }
  if (input.size() != 17) {
    throw std::invalid_argument("control profile must contain exactly the V7 fields");
  }
  if (!input.at("parking_idle_timeout_ms").is_number_integer() ||
      input.at("parking_idle_timeout_ms") < 0 || input.at("parking_idle_timeout_ms") > 1000)
    throw std::invalid_argument("parking_idle_timeout_ms must be an integer");
  if (!input.at("consecutive_late_frame_limit").is_number_integer() ||
      input.at("consecutive_late_frame_limit") < 1 || input.at("consecutive_late_frame_limit") > 5)
    throw std::invalid_argument("consecutive_late_frame_limit must be an integer in [1, 5]");
  for (const auto field : {"critical_camera_stale_brake_ms", "control_recovery_timeout_ms"})
    if (!input.at(field).is_number_integer()) throw std::invalid_argument(std::string(field) + " must use integer milliseconds");
  if (input.at("critical_camera_stale_brake_ms") < 100 || input.at("critical_camera_stale_brake_ms") > 3000 ||
      input.at("control_recovery_timeout_ms") < 1000 || input.at("control_recovery_timeout_ms") > 30000)
    throw std::invalid_argument("critical camera timing is outside its allowed range");
  const auto stale_brake_ms = input.at("critical_camera_stale_brake_ms").get<int>();
  const auto recovery_timeout_ms = input.at("control_recovery_timeout_ms").get<int>();
  if (stale_brake_ms < 100 || stale_brake_ms > 3000 || recovery_timeout_ms < 1000 || recovery_timeout_ms > 30000)
    throw std::invalid_argument("critical camera timing is outside its allowed range");
  const auto consecutive_late_frame_limit = input.at("consecutive_late_frame_limit").get<int>();
  const auto parking_idle_timeout_ms = input.at("parking_idle_timeout_ms").get<int>();
  if (parking_idle_timeout_ms < 0 || parking_idle_timeout_ms > 1000)
    throw std::invalid_argument("parking_idle_timeout_ms must be in [0, 1000]");
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
        "control profile V4 values are outside the controller schema bounds");
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
  parking_idle_timeout_ms_ = parking_idle_timeout_ms;
  consecutive_late_frame_limit_ = consecutive_late_frame_limit;
  critical_camera_stale_brake_ms_ = stale_brake_ms;
  control_recovery_timeout_ms_ = recovery_timeout_ms;
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
      driver_id(),
      session_id_,
      sequence,
      clock_.now_ms()}.to_json();
  request["type"] = "session_control_profile";
  request["control_token"] = control_token_;
  request["profile_version"] = kSessionControlProfileVersion;
  request["parking_idle_timeout_ms"] = parking_idle_timeout_ms;
  request["consecutive_late_frame_limit"] = consecutive_late_frame_limit;
  request["critical_camera_stale_brake_ms"] = stale_brake_ms;
  request["control_recovery_timeout_ms"] = recovery_timeout_ms;
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

Json DriverConsoleRuntime::update_control_intent(const Json& input, const HttpRequest* request) {
  const auto entered_at = monotonic_now_ms();
  const auto received_at = request ? request->received_at_monotonic_ms.value_or(entered_at) : entered_at;
  std::int64_t update_lock_wait_ms = 0, runtime_lock_wait_ms = 0, applied_at = entered_at;
  if (!input.is_object()) throw std::invalid_argument("control intent must be an object");
  const auto expected_session_id = required_string(input, "session_id");
  const auto expected_session_generation = required_uint64(input, "session_generation");
  std::optional<std::uint64_t> expected_ui_control_epoch;
  if (input.contains("ui_control_epoch")) {
    const auto& epoch = input.at("ui_control_epoch");
    if (!epoch.is_number_unsigned() &&
        !(epoch.is_number_integer() && epoch.get<std::int64_t>() >= 0)) {
      throw std::invalid_argument("ui_control_epoch must be a non-negative integer");
    }
    expected_ui_control_epoch = epoch.get<std::uint64_t>();
  }
  if (input.contains("takeover") && !input.at("takeover").is_boolean()) {
    throw std::invalid_argument("takeover must be a boolean");
  }
  const bool takeover = input.value("takeover", false);
  NativeControlIntent intent;
  intent.ui_instance_id = required_string(input, "ui_instance_id");
  intent.intent_seq = required_uint64(input, "intent_seq");
  intent.gear = required_string(input, "gear");
  try {
    intent.steering = input.at("steering").get<double>();
    intent.throttle = input.at("throttle").get<double>();
    intent.brake = input.at("brake").get<double>();
    intent.estop = input.value("estop", false);
    intent.critical_video_ready = input.value("critical_video_ready", true);
    if (!intent.estop && !input.contains("critical_video_sample_healthy")) {
      throw std::invalid_argument("critical_video_sample_healthy is required; refresh the control page");
    }
    intent.critical_video_sample_healthy = !intent.estop && input.value("critical_video_sample_healthy", false);
    if (!intent.estop) {
      for (const auto* field : {"critical_video_bad_sample_revision", "critical_video_brake_revision"}) {
        const auto& revision = input.at(field);
        if (!revision.is_number_unsigned() &&
            !(revision.is_number_integer() && revision.get<std::int64_t>() >= 0))
          throw std::invalid_argument(std::string(field) + " must be a non-negative integer");
      }
      intent.critical_video_bad_sample_revision = input.at("critical_video_bad_sample_revision").get<std::uint64_t>();
      intent.critical_video_brake_revision = input.at("critical_video_brake_revision").get<std::uint64_t>();
    }
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid control intent: ") + error.what());
  }
  intent.validate();
  NativeControlIntentUpdate result;
  {
    const auto lock_started = monotonic_now_ms();
    std::lock_guard update_lock(native_control_update_mutex_);
    update_lock_wait_ms = monotonic_now_ms() - lock_started;
    {
      const auto runtime_lock_started = monotonic_now_ms();
      std::lock_guard lock(mutex_);
      runtime_lock_wait_ms = monotonic_now_ms() - runtime_lock_started;
      if (logout_scope_reserved_ && !intent.estop) {
        throw Conflict("安全退出正在释放当前会话，禁止更新或接管输入", "driver_disconnect_in_progress");
      }
      if (session_id_.empty() || driver_token_.empty() || control_token_.empty()) {
        throw std::runtime_error("driver console is not connected");
      }
      if (session_id_ != expected_session_id ||
          control_session_generation_ != expected_session_generation) {
        throw std::runtime_error(
            "control intent belongs to a stale or different native control session");
      }
    }
    const auto apply_started_at = monotonic_now_ms();
    result = native_control_intent_.update(
        intent, received_at, expected_ui_control_epoch, takeover, apply_started_at);
    if (result.accepted && !result.duplicate && intent.estop) {
      // The accepted native latch and its DC publication must not wait for a
      // prior ordinary WSS operation on the periodic sender thread.
      static_cast<void>(publish_native_emergency_locked(native_control_intent_.sample(monotonic_now_ms()),
          Json(nullptr), expected_session_generation));
    }
    applied_at = monotonic_now_ms();
  }
  const Json timing = {{"timestamp_scope", "local_http_received_to_intent_applied"},
      {"before_handler_ms", std::max<std::int64_t>(0, entered_at - received_at)},
      {"update_mutex_wait_ms", update_lock_wait_ms}, {"runtime_mutex_wait_ms", runtime_lock_wait_ms},
      {"handler_ms", monotonic_now_ms() - entered_at},
      {"receive_to_apply_ms", std::max<std::int64_t>(0, applied_at - received_at)}};
  if (native_control_trace_) {
    const auto sample = native_control_intent_.sample(monotonic_now_ms());
    native_control_trace_->enqueue({{"stage", "intent_received"}, {"trace_session_id", expected_session_id},
        {"intent_seq", intent.intent_seq}, {"stored_intent_seq", sample.intent.intent_seq}, {"accepted", result.accepted}, {"reason", result.reason},
        {"intent_age_ms", sample.age_ms}, {"intent_lease_ms", native_control_intent_.lease_ms()},
        {"requires_fresh_input", result.requires_fresh_input},
        {"ui_control_epoch", result.ui_control_epoch},
        {"accepted_sample_id", result.accepted_sample_id},
        {"accepted_refresh_sample_id", result.accepted_refresh_sample_id},
        {"refresh_sample_id", result.refresh_sample_id},
        {"consecutive_refresh_anomalies", result.consecutive_refresh_anomalies},
        {"refresh_sample_healthy", result.refresh_sample_healthy},
        {"takeover", takeover}, {"ui_instance_id", intent.ui_instance_id},
        {"critical_video_ready", intent.critical_video_ready},
        {"critical_video_sample_healthy", intent.critical_video_sample_healthy},
        {"received_at_monotonic_ms", received_at}, {"applied_at_monotonic_ms", applied_at},
        {"local_http_timing", timing}});
  }
  if (result.accepted && intent.estop) {
    native_control_estop_wakeup_.store(true);
    wake_native_control_sender();
  }
  return {
      {"accepted", result.accepted},
      {"duplicate", result.duplicate},
      {"requires_fresh_input", result.requires_fresh_input},
      {"preserve_brake_during_rearm", result.preserve_brake_during_rearm},
      {"rearm_control_status_seq", native_control_rearm_status_seq_.load()},
      {"vehicle_accepted_command_seq", native_control_vehicle_accepted_command_seq_.load()},
      {"reason", result.reason},
      {"intent_seq", intent.intent_seq},
      {"session_id", expected_session_id},
      {"session_generation", expected_session_generation},
      {"ui_control_epoch", result.ui_control_epoch},
      {"accepted_sample_id", result.accepted_sample_id},
      {"accepted_refresh_sample_id", result.accepted_refresh_sample_id},
      {"refresh_sample_id", result.refresh_sample_id},
      {"consecutive_refresh_anomalies", result.consecutive_refresh_anomalies},
      {"refresh_sample_healthy", result.refresh_sample_healthy},
      {"lease_ms", native_control_intent_.lease_ms()},
      {"transport", "native_signaling_websocket"},
      {"delivery_state", "native_periodic_sender_owned"},
      {"local_http_timing", timing},
  };
}

Json DriverConsoleRuntime::status() {
  // Monitoring is a local snapshot. Network time samples and authority
  // renewal belong to their workers and must not delay input-lease refresh.
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
  std::int64_t native_control_ack_stall_age_ms = 0;
  std::size_t native_control_unacknowledged_count = 0;
  int native_control_reconnect_delay_ms = 0;
  bool native_control_ack_braking_wait = false;
  bool native_control_capabilities_verified = false;
  std::int64_t native_control_ack_recovery_deadline_ms = 0;
  std::int64_t native_control_last_ack_packet_age_ms = 0;
  Json vehicle_result_latency;
  Json control_transport_status;
  std::string observed_session;
  std::uint64_t observed_generation = 0;
  std::int64_t native_ack_receive_age_ms = -1;
  {
    std::lock_guard websocket_lock(control_signaling_websocket_mutex_);
    { std::lock_guard state_lock(mutex_);
      observed_session = session_id_; observed_generation = control_session_generation_; }
    const auto last_ack_received = native_control_last_ack_received_monotonic_ms_.load();
    if (last_ack_received > 0) native_ack_receive_age_ms = std::max<std::int64_t>(0, monotonic_now_ms() - last_ack_received);
    native_control_websocket_connected =
        control_signaling_websocket_ && control_signaling_websocket_->connected();
    native_control_last_ack_seq = control_signaling_last_ack_seq_;
    native_control_next_connect_monotonic_ms =
        control_signaling_next_connect_monotonic_ms_;
    native_control_unacknowledged_age_ms =
        control_signaling_ack_window_.oldest_age_ms(monotonic_now_ms());
    native_control_ack_stall_age_ms =
        control_signaling_ack_window_.stall_age_ms(monotonic_now_ms());
    native_control_unacknowledged_count = control_signaling_ack_window_.pending_count();
    native_control_reconnect_delay_ms = control_signaling_reconnect_delay_ms_;
    native_control_ack_braking_wait = control_signaling_ack_recovery_.waiting();
    native_control_capabilities_verified = control_signaling_capabilities_verified_;
    native_control_ack_recovery_deadline_ms = control_signaling_ack_recovery_.deadline();
    native_control_last_ack_packet_age_ms = control_signaling_ack_window_.last_ack_packet_age_ms();
    control_transport_status = control_transport_sender_.status();
    control_transport_status["enabled"] = true;
    control_transport_status["mode"] = config_.control_transport_mode;
    control_transport_status["experimental_enabled"] = config_.enable_experimental_control_transport;
    control_transport_status["cloud_capable"] = control_transport_cloud_capable_;
    control_transport_status["dc_network_path"] = control_transport_dc_network_path_;
    control_transport_status["dc_ready"] = control_transport_dc_ready_;
    { std::lock_guard state_lock(mutex_);
      control_transport_status["vehicle_capable"] = control_transport_vehicle_capable_;
      control_transport_status["session_key_present"] = control_transport_mac_key_.size() >= 32; }
    control_transport_status["cloud_authority_required"] = !config_.enable_experimental_direct_authority;
    control_transport_status["feedback_transport"] = feedback_transport_sender_.status();
    { std::lock_guard authority_lock(direct_authority_mutex_);
      control_transport_status["direct_authority"] = direct_authority_.status(monotonic_now_ms()); }
    control_transport_status["direct_authority"]["continuation_available"] = direct_control_continuation_available_locked();
    const auto result = control_vehicle_result_latency_.snapshot();
    const auto now = monotonic_now_ms();
    vehicle_result_latency = {
        {"method", "native_dispatch_to_vehicle_result_received"},
        {"command_path", result.last ? Json(result.last->data_channel ? "dc" : "wss") : Json(nullptr)},
        {"feedback_path", result.last ? Json(result.last->feedback_data_channel ? "dc" : "wss") : Json(nullptr)},
        {"includes_cloud_piggyback_wait", result.last ? Json(result.last->piggyback_return) : Json(nullptr)},
        {"physical_execution_confirmed", false},
        {"last_accepted_command_seq", result.last_accepted ? Json(result.last_accepted->command_seq) : Json(nullptr)},
        {"last_accepted_result_age_ms", result.last_accepted && now >= result.last_accepted->received_monotonic_ms
            ? Json(now - result.last_accepted->received_monotonic_ms) : Json(nullptr)},
        {"command_seq", result.last ? Json(result.last->command_seq) : Json(nullptr)},
        {"intent_seq", result.last ? Json(result.last->intent_seq) : Json(nullptr)},
        {"round_trip_ms", result.last ? Json(result.last->round_trip_ms) : Json(nullptr)},
        {"accepted", result.last ? Json(result.last->accepted) : Json(nullptr)},
        {"result_age_ms", result.last && now >= result.last->received_monotonic_ms
            ? Json(now - result.last->received_monotonic_ms) : Json(nullptr)},
        {"samples_total", result.samples},
        {"retained_sends", result.retained_sends},
        {"retention_limit", detail::VehicleResultLatencyWindow::kCapacity},
        {"evicted_sends_total", result.evicted_sends},
        {"uncorrelated_results_total", result.uncorrelated_results},
        {"invalid_timing_total", result.invalid_timing}};
  }
  const auto native_sample = native_control_intent_.sample(monotonic_now_ms());
  std::string native_control_last_error;
  {
    std::lock_guard status_lock(native_control_status_mutex_);
    native_control_last_error = native_control_last_error_;
  }
  std::lock_guard lock(mutex_);
  if (observed_session != session_id_ || observed_generation != control_session_generation_) {
    // A status request can straddle a session replacement after releasing the
    // socket lock. Do not attach the old measurement to the new generation.
    native_control_last_ack_seq = 0; native_ack_receive_age_ms = -1;
    vehicle_result_latency = nullptr;
  }
  return {
      {"runtime", "cpp"},
      {"driver_id", driver_id()},
      {"vehicle_id", vehicle_id_},
      {"authenticated", !driver_token_.empty()},
      {"driver_token_expires_at_utc_ms", driver_token_expires_at_ms_},
      {"signaling_service_instance_id", signaling_service_instance_id_},
      {"signaling_restart_recoveries", signaling_restart_recoveries_},
      {"signaling_available", signaling_available_},
      {"connected", !session_id_.empty()},
      {"pending_mobile_approval", pending_mobile_approval_},
      {"connect_cancellation_generation", connect_cancellation_generation_.load()},
      {"connect_operation_generation", connect_cancellation_generation_.load()},
      {"pending_connect_ui_instance_id", pending_connect_ui_instance_id_},
      {"disconnect_in_progress", logout_scope_reserved_},
      {"session_id", session_id_},
      {"session_creator_ui_instance_id", session_id_.empty() ? "" : session_creator_ui_instance_id_},
      {"control_session_generation", control_session_generation_},
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
      {"native_control",
       {{"transport", control_transport_status.value("path", "wss") == "dc" ? "native_webrtc_data_channel_bridge" : "native_signaling_websocket"},
        {"cloud_capabilities_verified", native_control_capabilities_verified},
        {"session_generation", control_session_generation_},
        {"rate_hz", config_.rate_hz},
        {"intent_lease_ms", native_control_intent_.lease_ms()},
        {"intent_active", native_sample.active},
        {"intent_fresh", native_sample.fresh},
        {"intent_age_ms", native_sample.age_ms},
        {"ui_control_epoch", native_sample.ui_control_epoch},
        {"owner_ui_instance_id", native_sample.owner_ui_instance_id},
        {"accepted_sample_id", native_sample.accepted_sample_id},
        {"accepted_refresh_sample_id", native_sample.accepted_refresh_sample_id},
        {"refresh_sample_id", native_sample.refresh_sample_id},
        {"consecutive_refresh_anomalies", native_sample.consecutive_refresh_anomalies},
        {"refresh_sample_healthy", native_sample.refresh_sample_healthy},
        {"requires_fresh_input", native_sample.requires_fresh_input},
        {"effective_intent_seq", native_sample.intent.intent_seq},
        {"effective_gear", native_sample.intent.gear},
        {"effective_steering", native_sample.intent.steering},
        {"effective_throttle", native_sample.intent.throttle},
        {"effective_brake", native_sample.intent.brake},
        {"effective_estop", native_sample.intent.estop},
        {"mock_control_diagnostic", config_.mock_control_diagnostic},
        {"critical_video_required", !config_.mock_control_diagnostic},
        {"effective_critical_video_ready", native_sample.intent.critical_video_ready},
        {"effective_critical_video_sample_healthy", native_sample.intent.critical_video_sample_healthy},
        {"websocket_connected", native_control_websocket_connected},
        {"next_connect_in_ms",
         std::max<std::int64_t>(
             0,
             native_control_next_connect_monotonic_ms - monotonic_now_ms())},
        {"reconnect_delay_ms", native_control_reconnect_delay_ms},
        {"commands_sent_total", native_control_commands_sent_.load()},
        {"send_failures_total", native_control_send_failures_.load()},
        {"socket_read_wakeups_total", native_control_socket_read_wakeups_.load()},
        {"socket_wait_failures_total", native_control_socket_wait_failures_.load()},
        {"sender_wakeup_failures_total", native_control_sender_wakeup_failures_.load()},
        {"last_sent_at_utc_ms", native_control_last_sent_at_utc_ms_.load()},
        {"last_sent_monotonic_ms", native_control_last_sent_monotonic_ms_.load()},
        {"last_gap_ms", native_control_last_gap_ms_.load()},
        {"max_gap_ms", native_control_max_gap_ms_.load()},
        {"last_seq", native_control_last_seq_.load()},
        {"last_ack_seq", native_control_last_ack_seq},
        {"unacknowledged_age_ms", native_control_unacknowledged_age_ms},
        {"ack_stall_age_ms", native_control_ack_stall_age_ms},
        {"ack_braking_wait", native_control_ack_braking_wait},
        {"ack_recovery_deadline_monotonic_ms", native_control_ack_recovery_deadline_ms},
        {"last_ack_packet_age_ms", native_control_last_ack_packet_age_ms},
        {"last_ack_receive_age_ms", native_ack_receive_age_ms >= 0 ? Json(native_ack_receive_age_ms) : Json(nullptr)},
        {"vehicle_result_latency", std::move(vehicle_result_latency)},
        {"control_transport", std::move(control_transport_status)},
        {"emergency_stop_capability", emergency_stop_vehicle_capable_ ? Json(detail::kEmergencyStopCapability) : Json(nullptr)},
        {"unacknowledged_count", native_control_unacknowledged_count},
        {"unacknowledged_limit", detail::NativeControlAcknowledgementWindow::kMaxPendingPackets},
        {"last_ack_received_at_utc_ms",
         native_control_last_ack_received_at_utc_ms_.load()},
        {"last_ack_received_monotonic_ms", native_control_last_ack_received_monotonic_ms_.load()},
        {"last_ack_cloud_received_at_utc_ms",
         native_control_last_ack_cloud_received_at_utc_ms_.load()},
        {"last_error", native_control_last_error}}},
      {"time_sync", clock_.status().to_json()},
      {"time_sync_readiness", time_sync_readiness_gate_.observe(clock_.status(),
          config_.max_time_sync_uncertainty_ms, monotonic_now_ms(),
          TimeSyncReadinessGate::candidate_timeout_ms(config_.time_sync_interval_ms,
              config_.time_sync_samples)).to_json()},
      {"webrtc_metrics", webrtc_metrics_},
      {"last_signaling_messages", signaling_messages_},
      {"authorized_vehicles", authorized_vehicles_},
  };
}

Json DriverConsoleRuntime::log_sessions() {
  std::string token;
  { std::lock_guard lock(mutex_); token = driver_token_; }
  if (token.empty()) throw std::invalid_argument("请先登录后查看会话日志");
  HttpClient http(std::chrono::seconds(2), config_.resolve_entries, config_.ca_bundle);
  return http.post_json_response(signaling_http_url_ + "/sessions/logs/driver",
      {{"operation", "list"}, {"driver_id", driver_id()}, {"token", token}});
}

Json DriverConsoleRuntime::start_log_export(std::string_view key) {
  std::lock_guard operation(authentication_mutex_);
  if (shutting_down_.load()) throw std::invalid_argument("控制端正在退出");
  std::string token;
  {
    std::lock_guard lock(mutex_);
    if (!session_id_.empty()) throw std::invalid_argument("请先结束控制会话再导出日志");
    token = driver_token_;
  }
  if (token.empty()) throw std::invalid_argument("请先登录后导出日志");
  std::lock_guard lock(log_export_mutex_);
  if (log_export_ && log_export_->status().at("state") == "collecting")
    throw std::invalid_argument("已有日志导出任务正在进行");
  log_export_ = std::make_unique<ControllerLogExport>(signaling_http_url_, config_.resolve_entries,
      config_.ca_bundle, driver_id(), token, std::string(key), config_.browser_event_log_path);
  log_export_owner_token_ = token;
  return log_export_->status();
}

Json DriverConsoleRuntime::log_export_status() const {
  std::string token;
  { std::lock_guard lock(mutex_); token = driver_token_; }
  std::lock_guard lock(log_export_mutex_);
  if (!log_export_ || token.empty() || token != log_export_owner_token_) return {{"state", "idle"}};
  return log_export_->status();
}

std::string DriverConsoleRuntime::log_export_zip() const {
  std::string token;
  { std::lock_guard lock(mutex_); token = driver_token_; }
  std::lock_guard lock(log_export_mutex_);
  if (!log_export_ || token.empty() || token != log_export_owner_token_)
    throw std::invalid_argument("当前登录无可下载的会话日志");
  return log_export_->zip();
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
      {"runtime_console_source_sha256", web::kConsoleSourceSha256},
      {"runtime_diagnostics_revision", "traction-diagnostics-v1"},
      {"driver_id", driver_id()},
      {"vehicle_id", vehicle},
      {"session_id", session},
      {"details", details},
  };
  if (config_.browser_event_log_path.empty()) return {{"recorded", false}, {"event", event}};
  append_driver_log_record(record);
  return {{"recorded", true}, {"event", event}};
}

DriverConsoleHttpApp::DriverConsoleHttpApp(std::shared_ptr<DriverConsoleRuntime> runtime) : runtime_(std::move(runtime)) {
  if (!runtime_) throw std::invalid_argument("driver console runtime is required");
}

static bool local_control_request_allowed(const HttpRequest& request) {
  const auto header = [&](const char* name) {
    const auto found = request.headers.find(name);
    return found == request.headers.end() ? std::string{} : lower(trim(found->second));
  };
  const auto host = header("host");
  const auto colon = host.starts_with("[") ? host.find(']', 1) : host.find(':');
  const auto address = host.starts_with("[")
      ? (colon == std::string::npos ? std::string{} : host.substr(1, colon - 1))
      : host.substr(0, colon);
  if (!is_loopback_bind_address(address)) return false;
  const auto suffix = colon == std::string::npos ? std::string{}
      : host.substr(colon + (host.starts_with("[") ? 1 : 0));
  if (!suffix.empty()) {
    if (suffix.front() != ':' || suffix.size() < 2 || suffix.size() > 6) return false;
    unsigned port = 0;
    for (const auto digit : std::string_view(suffix).substr(1)) {
      if (digit < '0' || digit > '9') return false;
      port = port * 10 + static_cast<unsigned>(digit - '0');
    }
    if (port == 0 || port > 65535) return false;
  }
  // CLI clients may omit Origin. Browsers must be same-origin; DNS rebinding
  // cannot supply a foreign Host, and simple cross-site forms cannot supply
  // the required JSON media type. This is not authentication of local OS users.
  const auto origin = header("origin");
  if (!origin.empty() && origin != "http://" + host) return false;
  const auto site = header("sec-fetch-site");
  if (!site.empty() && site != "same-origin" && site != "none") return false;
  if (request.method == "POST") {
    const auto type = header("content-type");
    if (trim(type.substr(0, type.find(';'))) != "application/json") return false;
  }
  return true;
}

ServerResponse DriverConsoleHttpApp::handle(const HttpRequest& request) const {
  try {
    if (!local_control_request_allowed(request))
      return ServerResponse::json(403, {{"error", "local control request origin or content type rejected"}});
    if (request.method == "GET" && request.path.starts_with("/assets/")) {
      return serve_console_asset(runtime_->config(), request.path);
    }
    if (request.method == "GET" && request.path == "/api/scene-config") {
      return ServerResponse::json(200, {{"enabled", runtime_->config().scene_enabled},
          {"manifest", "/assets/" + runtime_->config().scene_manifest}});
    }
    if (request.method == "GET" && request.path == "/health") return ServerResponse::json(200, {{"status", "ok"}, {"runtime", "cpp"}});
    if (request.method == "GET" && request.path == "/api/time") return ServerResponse::json(200, {{"now_ms", now_ms()}});
    if (request.method == "GET" && request.path == "/api/status") return ServerResponse::json(200, runtime_->status());
    if (request.method == "GET" && request.path == "/api/vehicles") return ServerResponse::json(200, runtime_->vehicles());
    if (request.method == "GET" && request.path == "/api/log-sessions") return ServerResponse::json(200, runtime_->log_sessions());
    if (request.method == "POST" && request.path == "/api/log-export")
      return ServerResponse::json(202, runtime_->start_log_export(required_string(request.json_body(), "key")));
    if (request.method == "GET" && request.path == "/api/log-export")
      return ServerResponse::json(200, runtime_->log_export_status());
    if (request.method == "GET" && request.path == "/api/log-export/download") {
      auto response = ServerResponse::text(200, runtime_->log_export_zip(), "application/zip");
      response.headers.emplace_back("Content-Disposition", "attachment; filename=mine-teleop-session.zip");
      response.headers.emplace_back("Cache-Control", "no-store");
      return response;
    }
    if (request.method == "GET" && request.path == "/api/control-limits") return ServerResponse::json(200, runtime_->control_limits());
    if (request.method == "GET" && request.path == "/api/control-profile") return ServerResponse::json(200, runtime_->control_profile());
    if (request.method == "GET" && request.path == "/") {
      return ServerResponse::text(200, console_html(runtime_->config()), "text/html; charset=utf-8");
    }
    if (request.method == "POST" && request.path == "/api/login") {
      const auto body = request.json_body();
      return ServerResponse::json(200, runtime_->login(required_string(body, "password"), required_string(body, "driver_id")));
    }
    if (request.method == "POST" && request.path == "/api/connect") {
      const auto input = request.json_body();
      return ServerResponse::json(200, runtime_->connect(input.value("vehicle_id", ""), input.value("ui_instance_id", "")));
    }
    if (request.method == "POST" && request.path == "/api/end-session") {
      return ServerResponse::json(
          200,
          runtime_->end_session_scoped(request.json_body()));
    }
    if (request.method == "POST" && request.path == "/api/disconnect") {
      return ServerResponse::json(
          200,
          runtime_->disconnect_scoped(request.json_body()));
    }
    if (request.method == "POST" && request.path == "/api/poll-signaling") return ServerResponse::json(200, runtime_->poll_signaling());
    if (request.method == "POST" && request.path == "/api/webrtc/ice-servers") return ServerResponse::json(200, runtime_->ice_servers());
    if (request.method == "POST" && request.path == "/api/webrtc/capabilities") return ServerResponse::json(200, runtime_->send_media_capabilities(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/fallback") return ServerResponse::json(200, runtime_->send_media_fallback(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/answer") return ServerResponse::json(200, runtime_->send_webrtc_answer(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/ice-candidate") return ServerResponse::json(200, runtime_->send_webrtc_ice_candidate(request.json_body()));
    if (request.method == "POST" && request.path == "/api/webrtc/metrics") return ServerResponse::json(200, runtime_->ingest_webrtc_metrics(request.json_body()));
    if (request.method == "POST" && request.path == "/api/browser-event") return ServerResponse::json(200, runtime_->record_browser_event(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control-limits") {
      return ServerResponse::json(
          410,
          {{"error", "legacy control-limit mutation is retired; use /api/control-profile"}});
    }
    if (request.method == "POST" && request.path == "/api/control-transport/management") return ServerResponse::json(200, runtime_->send_control_management(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control-transport/emergency") return ServerResponse::json(200, runtime_->emergency_stop_bridge(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control-transport/bridge") return ServerResponse::json(200, runtime_->control_transport_bridge(request.json_body(), &request));
    if (request.method == "POST" && request.path == "/api/control-profile") return ServerResponse::json(200, runtime_->prepare_control_profile(request.json_body()));
    if (request.method == "POST" && request.path == "/api/control-intent") {
      const auto input = request.json_body();
      const bool estop = input.is_object() && input.contains("estop") &&
          input.at("estop").is_boolean() && input.at("estop").get<bool>();
      if (input.is_object() && !estop && (!input.contains("critical_video_sample_healthy") ||
          !input.contains("critical_video_bad_sample_revision") || !input.contains("critical_video_brake_revision"))) {
        return ServerResponse::json(400, {{"error", "控车页面数据不完整，请刷新页面再确认接管"},
            {"issue_code", "control_page_metadata_missing"}});
      }
      return ServerResponse::json(200, runtime_->update_control_intent(input, &request));
    }
    if (request.method == "POST" && request.path == "/api/control") {
      return ServerResponse::json(
          410,
          {{"error", "legacy browser control packet endpoint is retired; use /api/control-intent"}});
    }
    if (request.method == "POST" &&
        (request.path == "/api/control/keyboard" ||
         request.path == "/api/control/gamepad")) {
      return ServerResponse::json(
          410,
          {{"error",
            "legacy specialized control endpoint is retired; use /api/control-intent"}});
    }
    return ServerResponse::json(404, {{"error", "not found"}});
  } catch (const Conflict& error) {
    auto response = error.details();
    response["error"] = error.what();
    response["issue_code"] = error.issue_code();
    return ServerResponse::json(409, response);
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
