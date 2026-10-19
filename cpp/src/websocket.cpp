#include "mine_teleop/websocket.hpp"
#include "mine_teleop/curl_tls.hpp"
#include "mine_teleop/platform.hpp"
#include "mine_teleop/detail/websocket_send_deadline.hpp"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <processthreadsapi.h>
#else
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <curl/curl.h>

#if defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#include <Security/Security.h>
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace mine_teleop {
namespace {

constexpr std::string_view kWebSocketGuid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr auto kWebSocketFrameAssemblyTimeout = std::chrono::seconds(5);

std::optional<std::int64_t> thread_cpu_us() {
#if defined(_WIN32)
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user)) return std::nullopt;
  const auto ticks = [](FILETIME value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32) | value.dwLowDateTime;
  };
  return static_cast<std::int64_t>((ticks(kernel) + ticks(user)) / 10);
#elif defined(CLOCK_THREAD_CPUTIME_ID)
  timespec value{};
  if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &value) != 0) return std::nullopt;
  return static_cast<std::int64_t>(value.tv_sec) * 1000000 + value.tv_nsec / 1000;
#else
  return std::nullopt;
#endif
}

#if defined(_WIN32)
using NativeSocket = SOCKET;
constexpr short kPollRead = POLLRDNORM;
constexpr short kPollWrite = POLLWRNORM;
constexpr int kSendFlags = 0;

int last_socket_error() { return WSAGetLastError(); }
bool socket_error_interrupted(int error) { return error == WSAEINTR; }
std::string socket_error_message(int error) {
  return "Windows socket error " + std::to_string(error);
}
#else
using NativeSocket = int;
constexpr short kPollRead = POLLIN;
constexpr short kPollWrite = POLLOUT;
constexpr int kSendFlags = MSG_NOSIGNAL;

int last_socket_error() { return errno; }
bool socket_error_interrupted(int error) { return error == EINTR; }
std::string socket_error_message(int error) { return std::strerror(error); }
#endif

NativeSocket native_socket(SocketHandle socket) {
  return static_cast<NativeSocket>(socket);
}

// Retain our own descriptor for exactly this TCP endpoint. Even if libcurl
// retires its original descriptor on an error, a delayed observer cannot act
// on a descriptor number/handle already reused by another connection.
struct ShutdownSocket {
  NativeSocket value;
  explicit ShutdownSocket(NativeSocket original) {
#if defined(_WIN32)
    WSAPROTOCOL_INFOW info{};
    if (::WSADuplicateSocketW(original, ::GetCurrentProcessId(), &info) != 0)
      throw std::runtime_error("cannot retain websocket interruption socket");
    value = ::WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &info, 0,
        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
    if (value == INVALID_SOCKET) throw std::runtime_error("cannot retain websocket interruption socket");
#else
    value = ::fcntl(original, F_DUPFD_CLOEXEC, 0);
    if (value < 0) throw std::runtime_error("cannot retain websocket interruption socket");
#endif
  }
  ~ShutdownSocket() {
#if defined(_WIN32)
    ::closesocket(value);
#else
    ::close(value);
#endif
  }
  void interrupt() const noexcept {
#if defined(_WIN32)
    static_cast<void>(::shutdown(value, SD_BOTH));
#else
    static_cast<void>(::shutdown(value, SHUT_RDWR));
#endif
  }
};

Json socket_transport_diagnostics(SocketHandle socket) {
  Json result = {{"sample_monotonic_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()}, {"platform", platform_name()},
      {"tcp_info_available", false}};
  if (socket == kInvalidSocket) {
    result["socket_available"] = false;
    return result;
  }
  result["socket_available"] = true;
  sockaddr_storage peer{};
#if defined(_WIN32)
  int peer_size = sizeof(peer);
  u_long pending = 0;
  if (::ioctlsocket(native_socket(socket), FIONREAD, &pending) == 0)
    result["socket_receive_pending_bytes"] = pending;
#else
  socklen_t peer_size = sizeof(peer);
  int pending = 0;
  if (::ioctl(native_socket(socket), FIONREAD, &pending) == 0)
    result["socket_receive_pending_bytes"] = pending;
#endif
  if (::getpeername(native_socket(socket), reinterpret_cast<sockaddr*>(&peer), &peer_size) == 0) {
    std::array<char, NI_MAXHOST> host{};
    std::array<char, NI_MAXSERV> port{};
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&peer), peer_size, host.data(), host.size(),
            port.data(), port.size(), NI_NUMERICHOST | NI_NUMERICSERV) == 0) {
      result["peer_address"] = host.data();
      result["peer_port"] = port.data();
    }
  }
  // The ephemeral port joins this exact connection to a host-scoped ss/TCP
  // observation. Do not emit the local address or a reusable descriptor.
  sockaddr_storage local{};
  auto local_size = static_cast<decltype(peer_size)>(sizeof(local));
  if (::getsockname(native_socket(socket), reinterpret_cast<sockaddr*>(&local), &local_size) == 0) {
    std::array<char, NI_MAXSERV> port{};
    if (::getnameinfo(reinterpret_cast<sockaddr*>(&local), local_size, nullptr, 0,
            port.data(), port.size(), NI_NUMERICSERV) == 0) result["local_port"] = port.data();
  }
#if defined(_WIN32)
  // Documented SIO_TCP_INFO v0 ABI. Keep the existing Win7 compile baseline;
  // querying this optional Win10+ IOCTL imports no newer Windows function.
  struct WindowsTcpInfoV0 {
    ULONG state, mss;
    ULONG64 connection_time_ms;
    BOOLEAN timestamps_enabled;
    ULONG rtt_us, min_rtt_us, bytes_in_flight, cwnd, snd_wnd, rcv_wnd, rcv_buf;
    ULONG64 bytes_out, bytes_in;
    ULONG bytes_reordered, bytes_retrans, fast_retrans, dup_acks_in, timeout_episodes;
    UCHAR syn_retrans;
  } info{};
  static_assert(sizeof(WindowsTcpInfoV0) == 88);
  DWORD version = 0, returned = 0;
  if (::WSAIoctl(native_socket(socket), _WSAIORW(IOC_VENDOR, 39), &version, sizeof(version),
          &info, sizeof(info), &returned, nullptr, nullptr) == 0 && returned >= sizeof(info)) {
    result.update({{"tcp_info_available", true}, {"tcp_state", info.state}, {"tcp_rtt_us", info.rtt_us},
        {"tcp_bytes_in_flight", info.bytes_in_flight}, {"tcp_retransmitted_bytes_total", info.bytes_retrans},
        {"tcp_retransmission_timeout_episodes_total", info.timeout_episodes},
        {"tcp_bytes_sent_total", info.bytes_out}, {"tcp_bytes_received_total", info.bytes_in},
        {"tcp_send_window_bytes", info.snd_wnd}});
  } else result["tcp_info_error_code"] = WSAGetLastError();
#elif defined(__APPLE__)
  tcp_connection_info info{};
  socklen_t size = sizeof(info);
  if (::getsockopt(native_socket(socket), IPPROTO_TCP, TCP_CONNECTION_INFO, &info, &size) == 0) {
    result.update({{"tcp_info_available", true}, {"tcp_state", info.tcpi_state},
        {"tcp_rtt_us", static_cast<std::uint64_t>(info.tcpi_srtt) * 1000},
        {"tcp_rto_us", static_cast<std::uint64_t>(info.tcpi_rto) * 1000},
        {"tcp_send_queue_bytes", info.tcpi_snd_sbbytes},
        {"tcp_retransmitted_bytes_total", info.tcpi_txretransmitbytes},
        {"tcp_bytes_sent_total", info.tcpi_txbytes}, {"tcp_bytes_received_total", info.tcpi_rxbytes},
        {"tcp_send_window_bytes", info.tcpi_snd_wnd}});
  } else result["tcp_info_error_code"] = errno;
#elif defined(__linux__)
  tcp_info info{};
  socklen_t size = sizeof(info);
  if (::getsockopt(native_socket(socket), IPPROTO_TCP, TCP_INFO, &info, &size) == 0) {
    result.update({{"tcp_info_available", true}, {"tcp_state", info.tcpi_state}, {"tcp_rtt_us", info.tcpi_rtt},
        {"tcp_rto_us", info.tcpi_rto}, {"tcp_unacknowledged_segments", info.tcpi_unacked},
        {"tcp_retransmitted_segments_total", info.tcpi_total_retrans}, {"tcp_current_retransmits", info.tcpi_retransmits}});
  } else result["tcp_info_error_code"] = errno;
#ifdef TIOCOUTQ
  int queued = 0;
  if (::ioctl(native_socket(socket), TIOCOUTQ, &queued) == 0) result["tcp_send_queue_bytes"] = queued;
#endif
#endif
  return result;
}

void enable_tcp_nodelay_best_effort(NativeSocket socket) noexcept {
  int enabled = 1;
#if defined(_WIN32)
  ::setsockopt(
      socket,
      IPPROTO_TCP,
      TCP_NODELAY,
      reinterpret_cast<const char*>(&enabled),
      static_cast<int>(sizeof(enabled)));
#else
  ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled));
#endif
}

int socket_buffer_size(std::size_t size) {
  return static_cast<int>(std::min<std::size_t>(
      size,
      static_cast<std::size_t>(std::numeric_limits<int>::max())));
}

void ensure_curl_global() {
  static std::once_flag initialized;
  std::call_once(initialized, [] {
    initialize_network_process();
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("curl_global_init failed");
    }
  });
}

std::string lower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

std::string trim(std::string value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) return {};
  const auto last = value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1);
}

bool valid_utf8(std::string_view value) {
  std::size_t index = 0;
  const auto continuation = [](unsigned char byte) { return byte >= 0x80U && byte <= 0xbfU; };
  while (index < value.size()) {
    const auto first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7fU) {
      ++index;
      continue;
    }
    if (first >= 0xc2U && first <= 0xdfU) {
      if (index + 1 >= value.size() || !continuation(static_cast<unsigned char>(value[index + 1]))) return false;
      index += 2;
      continue;
    }
    if (first >= 0xe0U && first <= 0xefU) {
      if (index + 2 >= value.size()) return false;
      const auto second = static_cast<unsigned char>(value[index + 1]);
      const auto third = static_cast<unsigned char>(value[index + 2]);
      if (!continuation(third) ||
          (first == 0xe0U && (second < 0xa0U || second > 0xbfU)) ||
          (first == 0xedU && (second < 0x80U || second > 0x9fU)) ||
          ((first != 0xe0U && first != 0xedU) && !continuation(second))) {
        return false;
      }
      index += 3;
      continue;
    }
    if (first >= 0xf0U && first <= 0xf4U) {
      if (index + 3 >= value.size()) return false;
      const auto second = static_cast<unsigned char>(value[index + 1]);
      const auto third = static_cast<unsigned char>(value[index + 2]);
      const auto fourth = static_cast<unsigned char>(value[index + 3]);
      if (!continuation(third) || !continuation(fourth) ||
          (first == 0xf0U && (second < 0x90U || second > 0xbfU)) ||
          (first == 0xf4U && (second < 0x80U || second > 0x8fU)) ||
          ((first != 0xf0U && first != 0xf4U) && !continuation(second))) {
        return false;
      }
      index += 4;
      continue;
    }
    return false;
  }
  return true;
}

bool valid_websocket_close_code(std::uint16_t code) {
  if (code >= 3000U && code <= 4999U) return true;
  if (code < 1000U || code > 1014U) return false;
  return code != 1004U && code != 1005U && code != 1006U;
}

void validate_websocket_close_payload(std::string_view payload) {
  if (payload.size() == 1) throw std::invalid_argument("websocket close payload must be empty or include a status code");
  if (payload.size() < 2) return;
  const auto code = static_cast<std::uint16_t>(
      (static_cast<std::uint16_t>(static_cast<unsigned char>(payload[0])) << 8U) |
      static_cast<unsigned char>(payload[1]));
  if (!valid_websocket_close_code(code)) throw std::invalid_argument("invalid websocket close status code");
  if (!valid_utf8(payload.substr(2))) throw std::invalid_argument("websocket close reason must be valid UTF-8");
}

bool supported_websocket_opcode(std::uint8_t opcode) {
  return opcode == 0x1U || opcode == 0x8U || opcode == 0x9U || opcode == 0xaU;
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

int base64_digit(unsigned char value) {
  if (value >= 'A' && value <= 'Z') return value - 'A';
  if (value >= 'a' && value <= 'z') return value - 'a' + 26;
  if (value >= '0' && value <= '9') return value - '0' + 52;
  if (value == '+') return 62;
  if (value == '/') return 63;
  return -1;
}

std::vector<unsigned char> base64_decode(std::string_view encoded) {
  if (encoded.empty() || encoded.size() % 4 != 0) throw std::invalid_argument("invalid base64");
  std::vector<unsigned char> decoded;
  decoded.reserve(encoded.size() / 4 * 3);
  for (std::size_t index = 0; index < encoded.size(); index += 4) {
    const bool last = index + 4 == encoded.size();
    const bool third_padding = encoded[index + 2] == '=';
    const bool fourth_padding = encoded[index + 3] == '=';
    if (encoded[index] == '=' || encoded[index + 1] == '=' || (third_padding && !fourth_padding) ||
        (!last && (third_padding || fourth_padding))) {
      throw std::invalid_argument("invalid base64");
    }
    const int first = base64_digit(static_cast<unsigned char>(encoded[index]));
    const int second = base64_digit(static_cast<unsigned char>(encoded[index + 1]));
    const int third = third_padding ? 0 : base64_digit(static_cast<unsigned char>(encoded[index + 2]));
    const int fourth = fourth_padding ? 0 : base64_digit(static_cast<unsigned char>(encoded[index + 3]));
    if (first < 0 || second < 0 || third < 0 || fourth < 0) throw std::invalid_argument("invalid base64");
    const auto block = static_cast<std::uint32_t>(
        (first << 18U) | (second << 12U) | (third << 6U) | fourth);
    decoded.push_back(static_cast<unsigned char>((block >> 16U) & 0xffU));
    if (!third_padding) decoded.push_back(static_cast<unsigned char>((block >> 8U) & 0xffU));
    if (!fourth_padding) decoded.push_back(static_cast<unsigned char>(block & 0xffU));
  }
  return decoded;
}

std::array<unsigned char, 20> sha1(std::string_view value) {
  std::array<unsigned char, 20> digest{};
#if defined(__APPLE__)
  if (CC_SHA1(value.data(), static_cast<CC_LONG>(value.size()), digest.data()) == nullptr) {
    throw std::runtime_error("CommonCrypto SHA-1 failed");
  }
#else
  unsigned int digest_size = 0;
  if (EVP_Digest(
          value.data(),
          value.size(),
          digest.data(),
          &digest_size,
          EVP_sha1(),
          nullptr) != 1 ||
      digest_size != digest.size()) {
    throw std::runtime_error("OpenSSL SHA-1 failed");
  }
#endif
  return digest;
}

void random_bytes(unsigned char* output, std::size_t size) {
#if defined(__APPLE__)
  if (SecRandomCopyBytes(kSecRandomDefault, size, output) != errSecSuccess) {
    throw std::runtime_error("Security SecRandomCopyBytes failed");
  }
#else
  if (size > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
      RAND_bytes(output, static_cast<int>(size)) != 1) {
    throw std::runtime_error("OpenSSL RAND_bytes failed");
  }
#endif
}

bool wait_socket(SocketHandle socket, short events, std::chrono::milliseconds timeout,
    std::uint64_t* interrupted_count = nullptr, detail::SocketWakeup* wakeup = nullptr,
    bool* notified = nullptr) {
#if defined(_WIN32)
  std::array<WSAPOLLFD, 2> descriptors{{{native_socket(socket), events, 0}, {}}};
#else
  std::array<pollfd, 2> descriptors{{{native_socket(socket), events, 0}, {}}};
#endif
  if (wakeup) descriptors[1] = {native_socket(wakeup->socket()), kPollRead, 0};
  const auto bounded = std::clamp<std::int64_t>(timeout.count(), 0, std::numeric_limits<int>::max());
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(bounded);
  auto remaining = static_cast<int>(bounded);
  while (true) {
#if defined(_WIN32)
    const int result = ::WSAPoll(descriptors.data(), wakeup ? 2 : 1, remaining);
#else
    const int result = ::poll(descriptors.data(), wakeup ? 2 : 1, remaining);
#endif
    if (result > 0) {
      // Drain an already-readable network frame first. Leave the notification
      // pending so simultaneous mailbox updates cannot be lost or starve ACKs.
      if (descriptors[0].revents & (events | POLLERR | POLLHUP)) return true;
      if (descriptors[0].revents & POLLNVAL) throw std::runtime_error("invalid websocket poll socket");
      if (wakeup && descriptors[1].revents) {
        if (!wakeup->consume()) throw std::runtime_error("websocket wakeup receive failed");
        if (notified) *notified = true;
      }
      return false;
    }
    if (result == 0) return false;
    const int error = last_socket_error();
    if (!socket_error_interrupted(error)) {
      throw std::runtime_error("poll failed: " + socket_error_message(error));
    }
    if (interrupted_count) ++*interrupted_count;
    // An interrupted wait must not renew the caller's original time budget.
    remaining = static_cast<int>(std::clamp<std::int64_t>(
        std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count(),
        0, bounded));
    if (remaining == 0) return false;
  }
}

void socket_send_all(SocketHandle socket, std::string_view value) {
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
      throw std::runtime_error("websocket send failed: " + socket_error_message(error));
    }
    if (result == 0) throw std::runtime_error("websocket closed while sending");
    sent += static_cast<std::size_t>(result);
  }
}

bool socket_receive_exact(
    SocketHandle socket,
    char* output,
    std::size_t size,
    std::chrono::steady_clock::time_point deadline) {
  std::size_t received = 0;
  while (received < size) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline || !wait_socket(
            socket,
            kPollRead,
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now))) {
      throw std::runtime_error("websocket frame timed out");
    }
    const auto result = ::recv(
        native_socket(socket),
        output + received,
        socket_buffer_size(size - received),
        0);
    if (result < 0) {
      const int error = last_socket_error();
      if (socket_error_interrupted(error)) continue;
      throw std::runtime_error("websocket receive failed: " + socket_error_message(error));
    }
    if (result == 0) return false;
    received += static_cast<std::size_t>(result);
  }
  return true;
}

std::string url_encode(std::string_view value) {
  static constexpr char hex[] = "0123456789ABCDEF";
  std::string encoded;
  for (const unsigned char character : value) {
    if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') {
      encoded.push_back(static_cast<char>(character));
    } else {
      encoded.push_back('%');
      encoded.push_back(hex[character >> 4U]);
      encoded.push_back(hex[character & 0x0fU]);
    }
  }
  return encoded;
}

std::string curl_url_part(CURLU* url, CURLUPart part, unsigned int flags = 0) {
  char* value = nullptr;
  const auto result = curl_url_get(url, part, &value, flags);
  if (result != CURLUE_OK || value == nullptr) return {};
  std::string output(value);
  curl_free(value);
  return output;
}

std::unordered_map<std::string, std::string> parse_http_headers(std::string_view response) {
  std::unordered_map<std::string, std::string> headers;
  const auto first_end = response.find("\r\n");
  std::size_t start = first_end == std::string_view::npos ? response.size() : first_end + 2;
  while (start < response.size()) {
    const auto end = response.find("\r\n", start);
    if (end == std::string_view::npos || end == start) break;
    const auto separator = response.find(':', start);
    if (separator != std::string_view::npos && separator < end) {
      headers[lower(trim(std::string(response.substr(start, separator - start))))] =
          trim(std::string(response.substr(separator + 1, end - separator - 1)));
    }
    start = end + 2;
  }
  return headers;
}

}  // namespace

struct detail::SocketWakeup::Impl {
  std::array<SocketHandle, 2> sockets{kInvalidSocket, kInvalidSocket};
  std::mutex mutex;
  bool pending{false};
  ~Impl() {
    for (auto socket : sockets) {
      if (socket == kInvalidSocket) continue;
#if defined(_WIN32)
      ::closesocket(native_socket(socket));
#else
      ::close(native_socket(socket));
#endif
    }
  }
};

detail::SocketWakeup::SocketWakeup() : impl_(std::make_unique<Impl>()) {
  initialize_network_process();
  std::array<sockaddr_in, 2> addresses{};
  for (std::size_t i = 0; i < impl_->sockets.size(); ++i) {
#if defined(_WIN32)
    const auto socket = ::WSASocketW(AF_INET, SOCK_DGRAM, IPPROTO_UDP, nullptr, 0,
        WSA_FLAG_NO_HANDLE_INHERIT);
    if (socket == INVALID_SOCKET) throw std::runtime_error("cannot create websocket wakeup socket");
#else
    const auto socket = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (socket < 0) throw std::runtime_error("cannot create websocket wakeup socket");
#endif
    impl_->sockets[i] = static_cast<SocketHandle>(socket);
    addresses[i].sin_family = AF_INET;
    addresses[i].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(socket, reinterpret_cast<const sockaddr*>(&addresses[i]), sizeof(sockaddr_in)) != 0)
      throw std::runtime_error("cannot bind websocket wakeup socket");
#if defined(_WIN32)
    int size = sizeof(sockaddr_in);
#else
    socklen_t size = sizeof(sockaddr_in);
#endif
    if (::getsockname(socket, reinterpret_cast<sockaddr*>(&addresses[i]), &size) != 0)
      throw std::runtime_error("cannot locate websocket wakeup socket");
#if defined(_WIN32)
    u_long nonblocking = 1;
    if (::ioctlsocket(socket, FIONBIO, &nonblocking) != 0)
      throw std::runtime_error("cannot make websocket wakeup nonblocking");
#else
    if (::fcntl(socket, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(socket, F_SETFL, O_NONBLOCK) < 0)
      throw std::runtime_error("cannot make websocket wakeup nonblocking");
#endif
  }
  for (std::size_t i = 0; i < impl_->sockets.size(); ++i) {
    if (::connect(native_socket(impl_->sockets[i]),
            reinterpret_cast<const sockaddr*>(&addresses[1 - i]), sizeof(sockaddr_in)) != 0)
      throw std::runtime_error("cannot connect websocket wakeup sockets");
  }
}

detail::SocketWakeup::~SocketWakeup() = default;
SocketHandle detail::SocketWakeup::socket() const noexcept { return impl_->sockets[0]; }

bool detail::SocketWakeup::notify() noexcept {
  std::lock_guard lock(impl_->mutex);
  if (impl_->pending) return true;
  int result;
  do { result = static_cast<int>(::send(native_socket(impl_->sockets[1]), "w", 1, kSendFlags)); }
  while (result < 0 && socket_error_interrupted(last_socket_error()));
  impl_->pending = result == 1;
  return impl_->pending;
}

bool detail::SocketWakeup::consume() noexcept {
  std::lock_guard lock(impl_->mutex);
  char byte;
  int result;
  do { result = static_cast<int>(::recv(native_socket(impl_->sockets[0]), &byte, 1, 0)); }
  while (result < 0 && socket_error_interrupted(last_socket_error()));
  if (result != 1) return false;
  impl_->pending = false;
  return true;
}

std::string websocket_accept_key(std::string_view client_key) {
  try {
    if (base64_decode(client_key).size() != 16) throw std::invalid_argument("invalid key length");
  } catch (const std::exception&) {
    throw std::invalid_argument("Sec-WebSocket-Key must be a base64-encoded 16-byte value");
  }
  const auto digest = sha1(std::string(client_key) + std::string(kWebSocketGuid));
  return base64_encode(digest.data(), digest.size());
}

Json websocket_transport_diagnostics(SocketHandle socket) { return socket_transport_diagnostics(socket); }

std::string signaling_websocket_url(
    std::string_view signaling_url,
    std::string_view session_id,
    std::string_view participant,
    std::string_view connection_generation) {
  if (session_id.empty() || participant.empty()) {
    throw std::invalid_argument("websocket signaling identity is required");
  }
  std::string origin(signaling_url);
  if (origin.starts_with("http://")) origin.replace(0, 7, "ws://");
  if (origin.starts_with("https://")) origin.replace(0, 8, "wss://");
  if (!origin.starts_with("ws://") && !origin.starts_with("wss://")) {
    throw std::invalid_argument("websocket signaling URL must use ws, wss, http, or https");
  }
  if (origin.ends_with("/signaling")) origin.resize(origin.size() - std::string_view("/signaling").size());
  while (!origin.empty() && origin.back() == '/') origin.pop_back();
  auto url = origin + "/signaling/" + url_encode(session_id) + "/ws?participant=" +
      url_encode(participant);
  if (!connection_generation.empty()) {
    url += "&connection_generation=" + url_encode(connection_generation);
  }
  return url;
}

ServerWebSocketConnection::ServerWebSocketConnection(SocketHandle socket, std::size_t max_message_bytes)
    : socket_(socket), max_message_bytes_(max_message_bytes) {
  if (socket_ < 0 || max_message_bytes_ == 0) throw std::invalid_argument("invalid server websocket connection");
  // ACKs and control deliveries are small latency-sensitive frames. Apply the
  // same policy as the client to the accepted TCP socket; non-TCP test sockets
  // continue to work when this optional socket option is unsupported.
  enable_tcp_nodelay_best_effort(native_socket(socket_));
}

void ServerWebSocketConnection::send_frame(std::uint8_t opcode, std::string_view payload) const {
  std::string frame;
  frame.push_back(static_cast<char>(0x80U | (opcode & 0x0fU)));
  if (payload.size() < 126) {
    frame.push_back(static_cast<char>(payload.size()));
  } else if (payload.size() <= 0xffffU) {
    frame.push_back(126);
    frame.push_back(static_cast<char>((payload.size() >> 8U) & 0xffU));
    frame.push_back(static_cast<char>(payload.size() & 0xffU));
  } else {
    frame.push_back(127);
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xffU));
    }
  }
  frame.append(payload);
  socket_send_all(socket_, frame);
}

void ServerWebSocketConnection::send_json(const Json& value) const { send_frame(0x1, value.dump()); }

Json ServerWebSocketConnection::transport_diagnostics() const { return socket_transport_diagnostics(socket_); }

void ServerWebSocketConnection::send_close(std::uint16_t code, std::string_view reason) const {
  if (!valid_websocket_close_code(code)) throw std::invalid_argument("invalid websocket close status code");
  if (reason.size() > 123) throw std::invalid_argument("websocket close reason is too long");
  if (!valid_utf8(reason)) throw std::invalid_argument("websocket close reason must be valid UTF-8");
  std::string payload;
  payload.push_back(static_cast<char>((code >> 8U) & 0xffU));
  payload.push_back(static_cast<char>(code & 0xffU));
  payload.append(reason);
  send_frame(0x8, payload);
}

WebSocketReceiveResult ServerWebSocketConnection::receive_json(std::chrono::milliseconds timeout,
    detail::SocketWakeup* wakeup) {
  const auto receive_started = std::chrono::steady_clock::now();
  while (true) {
    bool notified = false;
    if (!wait_socket(socket_, kPollRead, timeout, nullptr, wakeup, &notified))
      return {notified ? WebSocketReceiveStatus::Notified : WebSocketReceiveStatus::Timeout, Json::object()};
    // The caller's short timeout is an idle poll interval. Once any part of a
    // frame is readable, allow a separate bounded window for the rest of that
    // valid TCP frame to arrive instead of consuming the poll's remaining few
    // milliseconds as the frame-assembly deadline.
    const auto frame_started = std::chrono::steady_clock::now();
    const auto frame_deadline = frame_started + kWebSocketFrameAssemblyTimeout;
    std::array<char, 2> header{};
    if (!socket_receive_exact(socket_, header.data(), header.size(), frame_deadline)) {
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    const auto first = static_cast<unsigned char>(header[0]);
    const auto second = static_cast<unsigned char>(header[1]);
    const bool final = (first & 0x80U) != 0;
    const auto opcode = static_cast<std::uint8_t>(first & 0x0fU);
    if ((first & 0x70U) != 0) throw std::invalid_argument("websocket reserved bits are not supported");
    if ((second & 0x80U) == 0) throw std::invalid_argument("client websocket frames must be masked");
    if (!supported_websocket_opcode(opcode)) throw std::invalid_argument("unsupported websocket opcode");
    std::uint64_t length = second & 0x7fU;
    if (length == 126) {
      std::array<unsigned char, 2> extended{};
      if (!socket_receive_exact(socket_, reinterpret_cast<char*>(extended.data()), extended.size(), frame_deadline)) {
        return {WebSocketReceiveStatus::Closed, Json::object()};
      }
      length = (static_cast<std::uint64_t>(extended[0]) << 8U) | extended[1];
      if (length < 126U) throw std::invalid_argument("websocket frame length is not minimally encoded");
    } else if (length == 127) {
      std::array<unsigned char, 8> extended{};
      if (!socket_receive_exact(socket_, reinterpret_cast<char*>(extended.data()), extended.size(), frame_deadline)) {
        return {WebSocketReceiveStatus::Closed, Json::object()};
      }
      if ((extended[0] & 0x80U) != 0) throw std::invalid_argument("invalid websocket frame length");
      length = 0;
      for (const auto byte : extended) length = (length << 8U) | byte;
      if (length <= 0xffffU) throw std::invalid_argument("websocket frame length is not minimally encoded");
    }
    const bool control = opcode == 0x8 || opcode == 0x9 || opcode == 0xa;
    if (control && (!final || length > 125)) {
      throw std::invalid_argument("websocket control frames must be complete and no longer than 125 bytes");
    }
    if (length > max_message_bytes_) throw std::invalid_argument("websocket message too large");
    std::array<unsigned char, 4> mask{};
    if (!socket_receive_exact(socket_, reinterpret_cast<char*>(mask.data()), mask.size(), frame_deadline)) {
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    std::string payload(static_cast<std::size_t>(length), '\0');
    if (length > 0 && !socket_receive_exact(socket_, payload.data(), payload.size(), frame_deadline)) {
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    const auto frame_completed = std::chrono::steady_clock::now();
    for (std::size_t index = 0; index < payload.size(); ++index) {
      payload[index] = static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]);
    }
    if (opcode == 0x8) {
      validate_websocket_close_payload(payload);
      send_frame(0x8, payload);
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    if (opcode == 0x9) {
      send_frame(0xa, payload);
      continue;
    }
    if (opcode == 0xa) continue;
    if (!final) throw std::invalid_argument("fragmented websocket messages are not supported");
    if (opcode != 0x1) throw std::invalid_argument("only text websocket frames are supported");
    try {
      const auto parse_started = std::chrono::steady_clock::now();
      auto value = Json::parse(payload);
      if (!value.is_object()) throw std::invalid_argument("websocket message must be a JSON object");
      return {WebSocketReceiveStatus::Message, std::move(value),
          std::chrono::duration_cast<std::chrono::milliseconds>(frame_started - receive_started).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(frame_completed - frame_started).count(),
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - parse_started).count()};
    } catch (const Json::exception& error) {
      throw std::invalid_argument(std::string("invalid websocket JSON: ") + error.what());
    }
  }
}

std::function<void()> detail::websocket_socket_interrupter(SocketHandle socket) {
  auto retained = std::make_shared<ShutdownSocket>(native_socket(socket));
  return [retained]() noexcept { retained->interrupt(); };
}

struct WebSocketClient::Impl {
  struct ReadSpan {
    std::uint64_t begin, end;
    std::int64_t started_ms, completed_ms;
  };
  CURL* curl{nullptr};
  curl_slist* resolve_entries{nullptr};
  std::filesystem::path ca_bundle;
  curl_socket_t socket{CURL_SOCKET_BAD};
  std::string buffered;
  std::unordered_map<std::string, std::string> handshake_headers;
  std::int64_t last_transport_sample_monotonic_ms{0};
  std::uint64_t partial_frame_polls{0};
  std::uint64_t received_bytes{0}, consumed_bytes{0}, receive_calls{0}, receive_again{0}, idle_timeouts{0};
  std::uint64_t read_poll_calls{0};
  std::uint64_t read_poll_interrupted{0};
  bool read_diagnostics_enabled{false};
  std::int64_t read_poll_wait_ms{0}, longest_receive_call_ms{0}, longest_read_poll_ms{0};
  std::deque<ReadSpan> read_spans;
  const std::int64_t connection_started_monotonic_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  std::size_t max_message_bytes{8 * 1024 * 1024};
  bool peer_closed{false};
  bool send_failed{false};
  Json last_send = Json::object();
  std::function<void()> socket_interrupt;
  std::unique_ptr<detail::WebSocketSendDeadline> send_deadline;
  std::optional<std::chrono::steady_clock::time_point> active_send_deadline;
  std::stop_token active_send_stop;
  std::optional<std::chrono::steady_clock::time_point> last_send_progress;
  std::uint64_t send_progress_calls{0};
  std::int64_t longest_send_progress_gap_ms{0};

  static int send_progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
    auto& owner = *static_cast<Impl*>(context);
    if (owner.send_failed) return 1;  // remains cancelled through owner cleanup
    if (!owner.active_send_deadline) return 0;
    const auto now = std::chrono::steady_clock::now();
    if (owner.last_send_progress) owner.longest_send_progress_gap_ms = std::max(
        owner.longest_send_progress_gap_ms,
        std::chrono::duration_cast<std::chrono::milliseconds>(now - *owner.last_send_progress).count());
    owner.last_send_progress = now;
    ++owner.send_progress_calls;
    return owner.active_send_stop.stop_requested() || now >= *owner.active_send_deadline ? 1 : 0;
  }

  static std::int64_t steady_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  }

  Json read_snapshot() const {
    Json result = {{"timestamp_scope", "application_decrypted_read_completion"},
        {"connection_started_monotonic_ms", connection_started_monotonic_ms},
        {"buffered_receive_bytes", buffered.size()}, {"received_bytes_total", received_bytes},
        {"consumed_bytes_total", consumed_bytes}, {"curl_receive_calls_total", receive_calls},
        {"curl_again_total", receive_again}, {"idle_timeouts_total", idle_timeouts},
        {"partial_frame_polls_total", partial_frame_polls}, {"read_poll_calls_total", read_poll_calls},
        {"read_poll_interrupted_total", read_poll_interrupted},
        {"read_poll_wait_ms_total", read_poll_wait_ms}, {"longest_curl_receive_call_ms", longest_receive_call_ms},
        {"longest_read_poll_ms", longest_read_poll_ms}};
    if (!read_spans.empty()) {
      result["buffer_head_first_observed_monotonic_ms"] = read_spans.front().begin <= consumed_bytes
          ? Json(read_spans.front().completed_ms) : Json(nullptr);
      result["last_read_completed_monotonic_ms"] = read_spans.back().completed_ms;
    }
    return result;
  }

  Json frame_snapshot(std::size_t frame_bytes) const {
    auto result = read_snapshot();
    result["frame_stream_begin"] = consumed_bytes;
    result["frame_bytes"] = frame_bytes;
    const auto end = consumed_bytes + frame_bytes;
    const ReadSpan* first = nullptr;
    const ReadSpan* last = nullptr;
    for (const auto& span : read_spans) {
      if (span.begin <= consumed_bytes && consumed_bytes < span.end) first = &span;
      if (span.begin < end && end <= span.end) last = &span;
    }
    result["frame_read_timing_complete"] = first != nullptr && last != nullptr;
    if (first && last) {
      result["frame_first_read_started_monotonic_ms"] = first->started_ms;
      result["frame_first_observed_monotonic_ms"] = first->completed_ms;
      result["frame_last_observed_monotonic_ms"] = last->completed_ms;
      result["frame_observed_span_ms"] = std::max<std::int64_t>(0, last->completed_ms - first->completed_ms);
    }
    return result;
  }

  void consume(std::size_t size) {
    buffered.erase(0, size);
    consumed_bytes += size;
    while (!read_spans.empty() && read_spans.front().end <= consumed_bytes) read_spans.pop_front();
  }

  Impl(const std::vector<std::string>& entries, std::filesystem::path next_ca_bundle)
      : ca_bundle(std::move(next_ca_bundle)) {
    try {
      for (const auto& entry : entries) {
        if (entry.empty() || entry.find_first_of("\r\n") != std::string::npos) {
          throw std::invalid_argument("websocket resolve entry is invalid");
        }
        auto* next = curl_slist_append(resolve_entries, entry.c_str());
        if (next == nullptr) throw std::runtime_error("cannot allocate websocket resolve entries");
        resolve_entries = next;
      }
    } catch (...) {
      if (resolve_entries != nullptr) curl_slist_free_all(resolve_entries);
      resolve_entries = nullptr;
      throw;
    }
  }

  ~Impl() {
    // The observer never touches CURL. Join it before curl can free the socket
    // so a late deadline cannot shut down a subsequently reused descriptor.
    // Do not let TLS close-notify cleanup start another unbounded network
    // wait after the WebSocket close attempt has already ended.
    send_failed = true;
    if (socket_interrupt) socket_interrupt();
    send_deadline.reset();
    if (curl != nullptr) curl_easy_cleanup(curl);
    socket_interrupt = {};
    if (resolve_entries != nullptr) curl_slist_free_all(resolve_entries);
  }

  void send_all(std::string_view value, std::chrono::steady_clock::time_point deadline,
      std::stop_token stop = {}, Json preparation = Json::object()) {
    last_send = std::move(preparation);
    last_send["io_started_monotonic_ms"] = steady_ms();
    last_send["deadline_monotonic_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(deadline.time_since_epoch()).count();
    last_send["frame_bytes"] = value.size();
    detail::WebSocketWriteObservation observation;
    int last_curl_result = CURLE_OK;
    std::int64_t curl_wall_total_us = 0, curl_wall_max_us = 0, curl_cpu_total_us = 0;
    bool curl_cpu_available = true;
    const auto record = [&] {
      last_send["io_completed_monotonic_ms"] = steady_ms();
      last_send["curl_send_calls"] = observation.calls;
      last_send["curl_again_count"] = observation.again;
      last_send["longest_send_step_ms"] = observation.longest_call_ms;
      last_send["longest_curl_send_call_ms"] = curl_wall_max_us / 1000;
      last_send["curl_send_call_total_us"] = curl_wall_total_us;
      last_send["curl_send_call_max_us"] = curl_wall_max_us;
      last_send["curl_send_thread_cpu_us"] = curl_cpu_available && observation.calls
          ? Json(curl_cpu_total_us) : Json(nullptr);
      last_send["curl_last_result"] = last_curl_result;
      last_send["poll_calls"] = observation.polls;
      last_send["poll_wait_ms"] = observation.poll_ms;
      last_send["curl_reported_bytes"] = observation.bytes;
      last_send["cancel_requested"] = stop.stop_requested();
      last_send["socket_interrupted"] = send_deadline && send_deadline->interrupted();
      last_send["progress_callback_calls"] = send_progress_calls;
      last_send["longest_progress_callback_gap_ms"] = longest_send_progress_gap_ms;
    };
    // No TLS/WebSocket bytes have been handed to the backend yet. Expiring
    // serialization/frame preparation cannot corrupt this connection. Do not
    // arm a timer in the past and turn a local scheduling delay into a reconnect.
    if (!stop.stop_requested() && std::chrono::steady_clock::now() >= deadline) {
      send_progress_calls = 0;
      longest_send_progress_gap_ms = 0;
      last_send["completed"] = false;
      last_send["timed_out"] = true;
      last_send["not_started"] = true;
      record();
      throw WebSocketSendNotStarted();
    }
    if (!send_deadline) {
      socket_interrupt = detail::websocket_socket_interrupter(static_cast<SocketHandle>(socket));
      send_deadline = std::make_unique<detail::WebSocketSendDeadline>(socket_interrupt);
    }
    send_deadline->arm(deadline);
    active_send_deadline = deadline;
    active_send_stop = stop;
    last_send_progress.reset();
    send_progress_calls = 0;
    longest_send_progress_gap_ms = 0;
    struct Disarm {
      Impl& owner;
      ~Disarm() {
        owner.active_send_deadline.reset();
        owner.active_send_stop = {};
        owner.send_deadline->disarm();
      }
    } disarm{*this};
    std::stop_callback cancellation(stop, [this] { send_deadline->interrupt(); });
    try {
      detail::send_websocket_bytes(value, deadline, stop,
          [&](std::string_view remaining) {
            std::size_t count = 0;
            const auto cpu_started = thread_cpu_us();
            const auto call_started = std::chrono::steady_clock::now();
            const auto result = curl_easy_send(curl, remaining.data(), remaining.size(), &count);
            const auto call_finished = std::chrono::steady_clock::now();
            const auto cpu_finished = thread_cpu_us();
            const auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(call_finished - call_started).count();
            curl_wall_total_us += elapsed_us;
            curl_wall_max_us = std::max(curl_wall_max_us, elapsed_us);
            if (cpu_started && cpu_finished) curl_cpu_total_us += std::max<std::int64_t>(0, *cpu_finished - *cpu_started);
            else curl_cpu_available = false;
            // Keep JSON allocation outside the timed backend step. Wall time
            // still includes OS preemption; CPU time distinguishes CPU work
            // from time spent blocked or not scheduled, without changing SLA.
            last_curl_result = static_cast<int>(result);
            if (result != CURLE_OK && result != CURLE_AGAIN)
              throw std::runtime_error(std::string("websocket send failed: ") + curl_easy_strerror(result));
            return detail::WebSocketWriteProgress{count, result == CURLE_AGAIN};
          }, [this](auto remaining) { return wait_socket(static_cast<SocketHandle>(socket), kPollWrite, remaining); },
          [] { return std::chrono::steady_clock::now(); },
          [this] { return send_deadline->interrupted(); }, observation);
      if (!send_deadline->finish()) throw std::runtime_error("websocket send timed out");
      last_send["completed"] = true;
      record();
    } catch (...) {
      // Partial WebSocket/TLS records cannot carry the next command. Retire
      // the stream; the existing owner performs cleanup/reconnect, no replay.
      send_failed = true;
      last_send["timed_out"] = !stop.stop_requested() &&
          (std::chrono::steady_clock::now() >= deadline || send_deadline->interrupted());
      send_deadline->interrupt();
      last_send["completed"] = false;
      record();
      throw;
    }
  }

  bool read_more(std::chrono::steady_clock::time_point deadline,
      detail::SocketWakeup* wakeup = nullptr, bool* notified = nullptr) {
    while (true) {
      std::array<char, 16 * 1024> buffer{};
      std::size_t received = 0;
      const auto read_started_ms = read_diagnostics_enabled ? steady_ms() : 0;
      if (read_diagnostics_enabled) ++receive_calls;
      const auto result = curl_easy_recv(curl, buffer.data(), buffer.size(), &received);
      const auto read_completed_ms = read_diagnostics_enabled ? steady_ms() : 0;
      if (read_diagnostics_enabled)
        longest_receive_call_ms = std::max(longest_receive_call_ms, read_completed_ms - read_started_ms);
      if (result == CURLE_OK) {
        if (received == 0) {
          peer_closed = true;
          return false;
        }
        buffered.append(buffer.data(), received);
        const auto span_begin = received_bytes;
        received_bytes += received;
        if (read_diagnostics_enabled) {
          try { read_spans.push_back({span_begin, received_bytes, read_started_ms, read_completed_ms}); }
          catch (...) { read_spans.clear(); }
        }
        // A malicious byte-at-a-time peer must not grow diagnostic storage.
        // Lost timing coverage is explicit; protocol bytes remain untouched.
        if (read_spans.size() > 128) read_spans.pop_front();
        return true;
      }
      if (result != CURLE_AGAIN) throw std::runtime_error(std::string("websocket receive failed: ") + curl_easy_strerror(result));
      if (read_diagnostics_enabled) ++receive_again;
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline) return false;
      const auto poll_started_ms = read_diagnostics_enabled ? steady_ms() : 0;
      if (read_diagnostics_enabled) ++read_poll_calls;
      const auto ready = wait_socket(
              static_cast<SocketHandle>(socket),
              kPollRead,
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now),
              read_diagnostics_enabled ? &read_poll_interrupted : nullptr,
              wakeup, notified);
      if (read_diagnostics_enabled) {
        const auto poll_ms = steady_ms() - poll_started_ms;
        read_poll_wait_ms += poll_ms;
        longest_read_poll_ms = std::max(longest_read_poll_ms, poll_ms);
      }
      if (!ready) return false;
    }
  }

  bool ensure_bytes(std::size_t size, std::chrono::steady_clock::time_point deadline,
      detail::SocketWakeup* wakeup = nullptr, bool* notified = nullptr) {
    while (buffered.size() < size) {
      if (!read_more(deadline, wakeup, notified)) return false;
    }
    return true;
  }

  void send_frame(std::uint8_t opcode, std::string_view payload,
      std::chrono::steady_clock::time_point deadline, std::stop_token stop = {}, std::int64_t serialization_ms = 0) {
    const auto frame_started = steady_ms();
    std::array<unsigned char, 4> mask{};
    random_bytes(mask.data(), mask.size());
    std::string frame;
    frame.push_back(static_cast<char>(0x80U | (opcode & 0x0fU)));
    if (payload.size() < 126) {
      frame.push_back(static_cast<char>(0x80U | payload.size()));
    } else if (payload.size() <= 0xffffU) {
      frame.push_back(static_cast<char>(0x80U | 126U));
      frame.push_back(static_cast<char>((payload.size() >> 8U) & 0xffU));
      frame.push_back(static_cast<char>(payload.size() & 0xffU));
    } else {
      frame.push_back(static_cast<char>(0x80U | 127U));
      for (int shift = 56; shift >= 0; shift -= 8) {
        frame.push_back(static_cast<char>((static_cast<std::uint64_t>(payload.size()) >> shift) & 0xffU));
      }
    }
    frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
    for (std::size_t index = 0; index < payload.size(); ++index) {
      frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]));
    }
    send_all(frame, deadline, stop, {{"serialization_ms", serialization_ms},
        {"frame_build_ms", steady_ms() - frame_started}});
  }
};

WebSocketClient::WebSocketClient(std::chrono::milliseconds timeout)
    : WebSocketClient(timeout, {}, {}) {}

WebSocketClient::WebSocketClient(
    std::chrono::milliseconds timeout,
    std::vector<std::string> resolve_entries,
    std::filesystem::path ca_bundle)
    : resolve_entries_(std::move(resolve_entries)),
      ca_bundle_(std::move(ca_bundle)),
      impl_(nullptr),
      timeout_(timeout) {
  if (timeout_.count() <= 0) throw std::invalid_argument("websocket timeout must be positive");
  ensure_curl_global();
  impl_ = std::make_unique<Impl>(resolve_entries_, ca_bundle_);
}

WebSocketClient::~WebSocketClient() { close(); }

void WebSocketClient::connect(std::string_view url) { connect(url, {}); }

void WebSocketClient::connect(std::string_view url, const HttpHeaders& request_headers) {
  close();
  if (!url.starts_with("ws://") && !url.starts_with("wss://")) {
    throw std::invalid_argument("websocket URL must use ws or wss");
  }
  url_ = std::string(url);
  std::string transport_url(url);
  if (transport_url.starts_with("ws://")) transport_url.replace(0, 5, "http://");
  if (transport_url.starts_with("wss://")) transport_url.replace(0, 6, "https://");

  CURLU* parsed = curl_url();
  if (parsed == nullptr) throw std::runtime_error("curl_url failed");
  if (curl_url_set(parsed, CURLUPART_URL, transport_url.c_str(), 0) != CURLUE_OK) {
    curl_url_cleanup(parsed);
    throw std::invalid_argument("invalid websocket URL");
  }
  const auto host = curl_url_part(parsed, CURLUPART_HOST);
  const auto port = curl_url_part(parsed, CURLUPART_PORT);
  auto path = curl_url_part(parsed, CURLUPART_PATH);
  const auto query = curl_url_part(parsed, CURLUPART_QUERY);
  if (path.empty()) path = "/";
  curl_url_cleanup(parsed);
  if (host.empty()) throw std::invalid_argument("websocket URL host is required");
  const auto authority_host = host.find(':') == std::string::npos ? host : "[" + host + "]";
  const auto authority = port.empty() ? authority_host : authority_host + ":" + port;

  impl_ = std::make_unique<Impl>(resolve_entries_, ca_bundle_);
  impl_->read_diagnostics_enabled = read_diagnostics_enabled_;
  impl_->curl = curl_easy_init();
  if (impl_->curl == nullptr) throw std::runtime_error("curl_easy_init failed");
  curl_easy_setopt(impl_->curl, CURLOPT_URL, transport_url.c_str());
  // Keep WSS on the same direct path as HTTPS even when the host environment
  // has HTTP(S)/ALL_PROXY configured.
  curl_easy_setopt(impl_->curl, CURLOPT_PROXY, "");
  curl_easy_setopt(impl_->curl, CURLOPT_CONNECT_ONLY, 1L);
  curl_easy_setopt(impl_->curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(impl_->curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(timeout_.count()));
  curl_easy_setopt(impl_->curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_.count()));
  curl_easy_setopt(impl_->curl, CURLOPT_NOSIGNAL, 1L);
  // The pinned Windows Schannel overlay polls this callback even while
  // flushing one TLS record. CURL and these fields stay on the owner thread.
  curl_easy_setopt(impl_->curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(impl_->curl, CURLOPT_XFERINFOFUNCTION, &Impl::send_progress);
  curl_easy_setopt(impl_->curl, CURLOPT_XFERINFODATA, impl_.get());
  curl_easy_setopt(impl_->curl, CURLOPT_USERAGENT, "mine-teleop-websocket/0.2");
  const auto ca_bundle = impl_->ca_bundle.string();
  if (!ca_bundle.empty()) {
    configure_curl_custom_ca(impl_->curl, ca_bundle.c_str());
  } else if (const auto* ca_bundle = std::getenv("CURL_CA_BUNDLE"); ca_bundle != nullptr && *ca_bundle != '\0') {
    curl_easy_setopt(impl_->curl, CURLOPT_CAINFO, ca_bundle);
  } else if (const auto* ca_file = std::getenv("SSL_CERT_FILE"); ca_file != nullptr && *ca_file != '\0') {
    curl_easy_setopt(impl_->curl, CURLOPT_CAINFO, ca_file);
  }
  if (impl_->resolve_entries != nullptr) curl_easy_setopt(impl_->curl, CURLOPT_RESOLVE, impl_->resolve_entries);
  const auto result = curl_easy_perform(impl_->curl);
  if (result != CURLE_OK) {
    close();
    throw std::runtime_error(std::string("websocket connect failed: ") + curl_easy_strerror(result));
  }
  if (curl_easy_getinfo(impl_->curl, CURLINFO_ACTIVESOCKET, &impl_->socket) != CURLE_OK ||
      impl_->socket == CURL_SOCKET_BAD) {
    close();
    throw std::runtime_error("websocket connection has no active socket");
  }
  enable_tcp_nodelay_best_effort(impl_->socket);

  std::array<unsigned char, 16> nonce{};
  random_bytes(nonce.data(), nonce.size());
  const auto key = base64_encode(nonce.data(), nonce.size());
  const auto target = path + (query.empty() ? "" : "?" + query);
  std::string request =
      "GET " + target + " HTTP/1.1\r\nHost: " + authority +
      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key +
      "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: mine-teleop-websocket/0.2\r\n";
  std::unordered_set<std::string> header_names;
  static const std::unordered_set<std::string> reserved_headers{
      "host",
      "upgrade",
      "connection",
      "sec-websocket-key",
      "sec-websocket-version",
      "user-agent",
  };
  for (const auto& [name, value] : request_headers) {
    const auto canonical_name = lower(name);
    if (name.empty() || name.find_first_of(":\r\n") != std::string::npos ||
        value.find_first_of("\r\n") != std::string::npos ||
        reserved_headers.contains(canonical_name) ||
        !header_names.insert(canonical_name).second) {
      close();
      throw std::invalid_argument("websocket header is invalid or reserved");
    }
    request += name + ": " + value + "\r\n";
  }
  request += "\r\n";
  impl_->send_all(request, std::chrono::steady_clock::now() + timeout_);
  const auto deadline = std::chrono::steady_clock::now() + timeout_;
  while (impl_->buffered.find("\r\n\r\n") == std::string::npos) {
    if (impl_->buffered.size() > 64 * 1024) {
      close();
      throw std::runtime_error("websocket handshake headers are too large");
    }
    if (!impl_->read_more(deadline)) {
      close();
      throw std::runtime_error("websocket handshake timed out");
    }
  }
  const auto end = impl_->buffered.find("\r\n\r\n") + 4;
  const auto response = impl_->buffered.substr(0, end);
  impl_->consume(end);
  const auto line_end = response.find("\r\n");
  if (line_end == std::string::npos || response.substr(0, line_end).find(" 101 ") == std::string::npos) {
    const auto status_line = response.substr(0, line_end);
    close();
    std::istringstream status(status_line); std::string version; long code = 0;
    if (status >> version >> code && version.starts_with("HTTP/") && code >= 100 && code <= 599)
      throw HttpStatusError(code, "websocket upgrade failed: " + status_line);
    throw std::runtime_error("websocket upgrade failed: " + status_line);
  }
  const auto headers = parse_http_headers(response);
  impl_->handshake_headers = headers;
  const auto upgrade = headers.find("upgrade");
  const auto connection = headers.find("connection");
  const auto accept = headers.find("sec-websocket-accept");
  if (upgrade == headers.end() || lower(upgrade->second) != "websocket" || connection == headers.end() ||
      lower(connection->second).find("upgrade") == std::string::npos || accept == headers.end() ||
      accept->second != websocket_accept_key(key)) {
    close();
    throw std::runtime_error("websocket upgrade response is invalid");
  }
}

void WebSocketClient::close() {
  if (connected()) {
    try {
      impl_->send_frame(0x8, std::string("\x03\xe8", 2), std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
    } catch (const std::exception&) {
    }
  }
  impl_.reset();
  url_.clear();
}

bool WebSocketClient::connected() const {
  return impl_ && impl_->curl != nullptr && impl_->socket != CURL_SOCKET_BAD && !impl_->send_failed;
}

std::string WebSocketClient::response_header(std::string_view name) const {
  if (!impl_) return {};
  const auto found = impl_->handshake_headers.find(lower(std::string(name)));
  return found == impl_->handshake_headers.end() ? std::string{} : found->second;
}

void WebSocketClient::send_json(const Json& value) {
  send_json(value, timeout_);
}

void WebSocketClient::send_json(const Json& value, std::chrono::milliseconds timeout) {
  send_json(value, timeout, {});
}

void WebSocketClient::send_json(const Json& value, std::chrono::milliseconds timeout, std::stop_token stop) {
  if (timeout.count() <= 0) throw std::invalid_argument("websocket send timeout must be positive");
  send_json_until(value, std::chrono::steady_clock::now() + timeout, stop);
}

void WebSocketClient::send_json_until(const Json& value,
    std::chrono::steady_clock::time_point deadline, std::stop_token stop) {
  if (!connected()) throw std::runtime_error("websocket is not connected");
  if (!value.is_object()) throw std::invalid_argument("websocket JSON message must be an object");
  const auto started = std::chrono::steady_clock::now();
  const auto bytes = value.dump();
  impl_->send_frame(0x1, bytes, deadline, stop,
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count());
}

Json WebSocketClient::last_send_diagnostics() const {
  return impl_ ? impl_->last_send : Json(nullptr);
}

void WebSocketClient::enable_read_diagnostics(bool enabled) {
  read_diagnostics_enabled_ = enabled;
  if (impl_) impl_->read_diagnostics_enabled = enabled;
}

WebSocketReceiveResult WebSocketClient::receive_json(std::chrono::milliseconds timeout,
    detail::SocketWakeup* wakeup) {
  if (!connected()) throw std::runtime_error("websocket is not connected");
  const auto receive_started_ms = Impl::steady_ms();
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  bool notified = false;
  const auto incomplete = [&]() -> WebSocketReceiveResult {
    if (impl_->peer_closed) {
      close();
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    if (!impl_->buffered.empty()) ++impl_->partial_frame_polls;
    if (impl_->read_diagnostics_enabled && !notified) ++impl_->idle_timeouts;
    // TCP/TLS may split any header or payload. Retain every byte for the next
    // poll; a short/zero idle deadline says nothing about protocol validity.
    WebSocketReceiveResult result{notified ? WebSocketReceiveStatus::Notified :
        WebSocketReceiveStatus::Timeout, Json::object()};
    if (impl_->read_diagnostics_enabled) {
      try {
        result.read_diagnostics = impl_->read_snapshot();
        result.read_diagnostics["receive_started_monotonic_ms"] = receive_started_ms;
        result.read_diagnostics["receive_completed_monotonic_ms"] = Impl::steady_ms();
      } catch (...) { result.read_diagnostics = nullptr; }
    }
    return result;
  };
  while (true) {
    if (!impl_->ensure_bytes(2, deadline, wakeup, &notified)) {
      return incomplete();
    }
    const auto first = static_cast<unsigned char>(impl_->buffered[0]);
    const auto second = static_cast<unsigned char>(impl_->buffered[1]);
    const bool final = (first & 0x80U) != 0;
    const auto opcode = static_cast<std::uint8_t>(first & 0x0fU);
    if ((first & 0x70U) != 0) throw std::runtime_error("websocket server used unsupported reserved bits");
    if ((second & 0x80U) != 0) throw std::runtime_error("websocket server frame must not be masked");
    if (!supported_websocket_opcode(opcode)) throw std::runtime_error("websocket server used an unsupported opcode");
    std::uint64_t length = second & 0x7fU;
    std::size_t header_size = 2;
    if (length == 126) {
      if (!impl_->ensure_bytes(4, deadline, wakeup, &notified)) return incomplete();
      length = (static_cast<std::uint64_t>(static_cast<unsigned char>(impl_->buffered[2])) << 8U) |
          static_cast<unsigned char>(impl_->buffered[3]);
      if (length < 126U) throw std::runtime_error("websocket frame length is not minimally encoded");
      header_size = 4;
    } else if (length == 127) {
      if (!impl_->ensure_bytes(10, deadline, wakeup, &notified)) return incomplete();
      if ((static_cast<unsigned char>(impl_->buffered[2]) & 0x80U) != 0) {
        throw std::runtime_error("invalid websocket frame length");
      }
      length = 0;
      for (std::size_t index = 2; index < 10; ++index) {
        length = (length << 8U) | static_cast<unsigned char>(impl_->buffered[index]);
      }
      if (length <= 0xffffU) throw std::runtime_error("websocket frame length is not minimally encoded");
      header_size = 10;
    }
    const bool control = opcode == 0x8 || opcode == 0x9 || opcode == 0xa;
    if (control && (!final || length > 125)) throw std::runtime_error("invalid websocket control frame");
    if (length > impl_->max_message_bytes) throw std::runtime_error("websocket message too large");
    if (length > std::numeric_limits<std::size_t>::max() - header_size) {
      throw std::runtime_error("websocket frame length is unsupported");
    }
    if (!impl_->ensure_bytes(header_size + static_cast<std::size_t>(length), deadline,
                            wakeup, &notified)) {
      return incomplete();
    }
    const auto frame_bytes = header_size + static_cast<std::size_t>(length);
    Json read_diagnostics = nullptr;
    if (impl_->read_diagnostics_enabled) {
      try { read_diagnostics = impl_->frame_snapshot(frame_bytes); }
      catch (...) { /* Missing diagnostic coverage must not discard protocol bytes. */ }
    }
    const auto payload = impl_->buffered.substr(header_size, static_cast<std::size_t>(length));
    impl_->consume(frame_bytes);
    if (opcode == 0x8) {
      try {
        validate_websocket_close_payload(payload);
      } catch (const std::invalid_argument& error) {
        throw std::runtime_error(error.what());
      }
      close();
      return {WebSocketReceiveStatus::Closed, Json::object()};
    }
    if (opcode == 0x9) {
      impl_->send_frame(0xa, payload, std::chrono::steady_clock::now() + timeout_);
      continue;
    }
    if (opcode == 0xa) continue;
    if (!final) throw std::runtime_error("fragmented websocket messages are not supported");
    if (opcode != 0x1) throw std::runtime_error("only text websocket frames are supported");
    try {
      const auto parse_started_ms = impl_->read_diagnostics_enabled ? Impl::steady_ms() : 0;
      auto value = Json::parse(payload);
      if (!value.is_object()) throw std::runtime_error("websocket message must be a JSON object");
      WebSocketReceiveResult result{WebSocketReceiveStatus::Message, std::move(value)};
      if (impl_->read_diagnostics_enabled && read_diagnostics.is_object()) try {
        result.json_parse_ms = Impl::steady_ms() - parse_started_ms;
        if (read_diagnostics.value("frame_read_timing_complete", false))
          read_diagnostics["frame_last_observed_to_parse_ms"] = std::max<std::int64_t>(
              0, parse_started_ms - read_diagnostics.at("frame_last_observed_monotonic_ms").get<std::int64_t>());
        read_diagnostics["receive_started_monotonic_ms"] = receive_started_ms;
        read_diagnostics["receive_completed_monotonic_ms"] = Impl::steady_ms();
      } catch (...) { read_diagnostics = nullptr; }
      result.read_diagnostics = std::move(read_diagnostics);
      return result;
    } catch (const Json::exception& error) {
      throw std::runtime_error(std::string("invalid websocket JSON: ") + error.what());
    }
  }
}

Json WebSocketClient::transport_diagnostics(std::chrono::milliseconds minimum_interval) {
  if (!impl_) return {{"socket_available", false}, {"tcp_info_available", false}};
  const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  if (impl_->last_transport_sample_monotonic_ms > 0 &&
      timestamp - impl_->last_transport_sample_monotonic_ms < minimum_interval.count()) return nullptr;
  impl_->last_transport_sample_monotonic_ms = timestamp;
  auto result = socket_transport_diagnostics(static_cast<SocketHandle>(impl_->socket));
  result["buffered_receive_bytes"] = impl_->buffered.size();
  result["partial_frame_polls_total"] = impl_->partial_frame_polls;
  result["connection_started_monotonic_ms"] = impl_->connection_started_monotonic_ms;
  if (const auto* version = curl_version_info(CURLVERSION_NOW)) {
    result["curl_runtime_version"] = version->version ? version->version : "unknown";
    result["curl_tls_backend"] = version->ssl_version ? version->ssl_version : "unknown";
  }
  // MultiSSL's version string can include inactive backends in parentheses.
  // Record the established connection's enum separately; never expose its TLS pointer.
  curl_tlssessioninfo* tls = nullptr;
  result["curl_active_tls_backend_id"] = impl_->curl != nullptr &&
      curl_easy_getinfo(impl_->curl, CURLINFO_TLS_SSL_PTR, &tls) == CURLE_OK && tls != nullptr
      ? Json(static_cast<int>(tls->backend)) : Json(nullptr);
  return result;
}

}  // namespace mine_teleop
