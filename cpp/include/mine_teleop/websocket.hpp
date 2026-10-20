#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <stop_token>
#include <vector>

#include "mine_teleop/http.hpp"
#include "mine_teleop/platform.hpp"

namespace mine_teleop {

inline constexpr std::string_view kNativeControlCapabilities = "v9-revision-recovery-v1";

enum class WebSocketReceiveStatus {
  Message,
  Timeout,
  Closed,
  Notified,
};

// Preparation exhausted the caller's budget before any backend write or
// deadline-driven socket interruption. The stream remains reusable; callers
// must discard this message, never replay it as a fresh command.
class WebSocketSendNotStarted : public std::runtime_error {
 public:
  WebSocketSendNotStarted() : std::runtime_error("websocket send deadline expired before I/O") {}
};

struct WebSocketReceiveResult {
  WebSocketReceiveStatus status{WebSocketReceiveStatus::Timeout};
  Json message{Json::object()};
  std::int64_t receive_wait_ms{0};
  std::int64_t frame_assembly_ms{0};
  std::int64_t json_parse_ms{0};
  // Byte counts and local steady-clock observations only; never frame content.
  // Application read completion is not a kernel/network arrival timestamp.
  Json read_diagnostics = Json::object();
};

[[nodiscard]] std::string websocket_accept_key(std::string_view client_key);
[[nodiscard]] Json websocket_transport_diagnostics(SocketHandle socket);
namespace detail {
// Owns a duplicate descriptor; never closes or calls curl on the original.
[[nodiscard]] std::function<void()> websocket_socket_interrupter(SocketHandle socket);

// A coalesced, local-only notification that can share a socket poll. It owns
// both endpoints; callers retain it while either notifying or waiting.
class SocketWakeup {
 public:
  SocketWakeup();
  ~SocketWakeup();
  SocketWakeup(const SocketWakeup&) = delete;
  SocketWakeup& operator=(const SocketWakeup&) = delete;
  [[nodiscard]] bool notify() noexcept;
  [[nodiscard]] bool consume() noexcept;
  [[nodiscard]] SocketHandle socket() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Pins exactly one socket endpoint for one wait owner. Waiting does not access CURL or consume data,
// and remains safe while the owning client closes/replaces its original fd.
class SocketReadiness {
 public:
  explicit SocketReadiness(SocketHandle socket);
  ~SocketReadiness();
  [[nodiscard]] bool wait(std::chrono::milliseconds timeout,
      SocketWakeup* wakeup = nullptr) const;
  // The owner retires this endpoint before freeing CURL. On Windows, shutdown
  // of a duplicated socket alone need not interrupt an existing WSAPoll.
  void retire() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}
[[nodiscard]] std::string signaling_websocket_url(
    std::string_view signaling_url,
    std::string_view session_id,
    std::string_view participant,
    std::string_view connection_generation = {});

class ServerWebSocketConnection {
 public:
  ServerWebSocketConnection(SocketHandle socket, std::size_t max_message_bytes);

  [[nodiscard]] WebSocketReceiveResult receive_json(std::chrono::milliseconds timeout,
      detail::SocketWakeup* wakeup = nullptr);
  void send_json(const Json& value) const;
  void send_close(std::uint16_t code = 1000, std::string_view reason = {}) const;
  [[nodiscard]] Json transport_diagnostics() const;

 private:
  void send_frame(std::uint8_t opcode, std::string_view payload) const;

  SocketHandle socket_;
  std::size_t max_message_bytes_;
};

class WebSocketClient {
 public:
  explicit WebSocketClient(std::chrono::milliseconds timeout = std::chrono::seconds(5));
  WebSocketClient(
      std::chrono::milliseconds timeout,
      std::vector<std::string> resolve_entries,
      std::filesystem::path ca_bundle);
  ~WebSocketClient();

  WebSocketClient(const WebSocketClient&) = delete;
  WebSocketClient& operator=(const WebSocketClient&) = delete;

  void connect(std::string_view url);
  void connect(std::string_view url, const HttpHeaders& headers);
  void close();
  [[nodiscard]] bool connected() const;
  [[nodiscard]] const std::string& url() const { return url_; }
  [[nodiscard]] std::string response_header(std::string_view name) const;
  void send_json(const Json& value);
  void send_json(const Json& value, std::chrono::milliseconds timeout);
  void send_json(const Json& value, std::chrono::milliseconds timeout, std::stop_token stop);
  void send_json_until(const Json& value, std::chrono::steady_clock::time_point deadline,
      std::stop_token stop = {});
  // The caller serializes access, including snapshots after a failed send.
  [[nodiscard]] Json last_send_diagnostics() const;
  void enable_read_diagnostics(bool enabled = true);
  // Notification interrupts only receiving; buffered partial frames survive.
  // The caller keeps wakeup alive and retains sole ownership of this client.
  [[nodiscard]] WebSocketReceiveResult receive_json(std::chrono::milliseconds timeout,
      detail::SocketWakeup* wakeup = nullptr);
  // Obtain under the client's ownership lock, then wait without that lock.
  // Only network readiness: TLS/application buffering still needs a bounded
  // receive check. Revalidate the current client/session before consuming.
  [[nodiscard]] std::shared_ptr<const detail::SocketReadiness> read_waiter() const;
  // null means the rate limit skipped this sample; unsupported counters stay absent.
  [[nodiscard]] Json transport_diagnostics(std::chrono::milliseconds minimum_interval = {});

 private:
  struct Impl;
  std::vector<std::string> resolve_entries_;
  std::filesystem::path ca_bundle_;
  std::unique_ptr<Impl> impl_;
  std::chrono::milliseconds timeout_;
  std::string url_;
  bool read_diagnostics_enabled_{false};
};

}  // namespace mine_teleop
