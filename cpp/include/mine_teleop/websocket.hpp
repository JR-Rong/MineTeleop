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

namespace mine_teleop {

enum class WebSocketReceiveStatus {
  Message,
  Timeout,
  Closed,
};

struct WebSocketReceiveResult {
  WebSocketReceiveStatus status{WebSocketReceiveStatus::Timeout};
  Json message{Json::object()};
};

[[nodiscard]] std::string websocket_accept_key(std::string_view client_key);
[[nodiscard]] std::string signaling_websocket_url(
    std::string_view signaling_url,
    std::string_view session_id,
    std::string_view participant,
    std::string_view connection_generation = {});

class ServerWebSocketConnection {
 public:
  ServerWebSocketConnection(int socket, std::size_t max_message_bytes);

  [[nodiscard]] WebSocketReceiveResult receive_json(std::chrono::milliseconds timeout);
  void send_json(const Json& value) const;
  void send_close(std::uint16_t code = 1000, std::string_view reason = {}) const;

 private:
  void send_frame(std::uint8_t opcode, std::string_view payload) const;

  int socket_;
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
  void send_json(const Json& value);
  [[nodiscard]] WebSocketReceiveResult receive_json(std::chrono::milliseconds timeout);

 private:
  struct Impl;
  std::vector<std::string> resolve_entries_;
  std::filesystem::path ca_bundle_;
  std::unique_ptr<Impl> impl_;
  std::chrono::milliseconds timeout_;
  std::string url_;
};

}  // namespace mine_teleop
