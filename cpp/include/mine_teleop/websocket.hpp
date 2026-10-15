#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mine_teleop/http.hpp"
#include "mine_teleop/platform.hpp"

namespace mine_teleop {

inline constexpr std::string_view kNativeControlCapabilities = "v9-revision-recovery-v1";

enum class WebSocketReceiveStatus {
  Message,
  Timeout,
  Closed,
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
[[nodiscard]] std::string signaling_websocket_url(
    std::string_view signaling_url,
    std::string_view session_id,
    std::string_view participant,
    std::string_view connection_generation = {});

class ServerWebSocketConnection {
 public:
  ServerWebSocketConnection(SocketHandle socket, std::size_t max_message_bytes);

  [[nodiscard]] WebSocketReceiveResult receive_json(std::chrono::milliseconds timeout);
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
  void enable_read_diagnostics(bool enabled = true);
  [[nodiscard]] WebSocketReceiveResult receive_json(std::chrono::milliseconds timeout);
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
