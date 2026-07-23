#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/platform.hpp"
#include "mine_teleop/server.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void expect(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

std::string response_header(const mine_teleop::ServerResponse& response, std::string_view name) {
  const auto found = std::find_if(response.headers.begin(), response.headers.end(), [&](const auto& header) {
    return header.first == name;
  });
  return found == response.headers.end() ? "" : found->second;
}

template <typename Function>
void expect_throws(Function&& function, std::string_view message) {
  try {
    function();
  } catch (const std::exception&) {
    return;
  }
  throw std::runtime_error(std::string(message));
}

void raw_send_all(int socket, std::string_view value) {
  std::size_t sent = 0;
  while (sent < value.size()) {
    const auto count = ::send(socket, value.data() + sent, value.size() - sent, MSG_NOSIGNAL);
    if (count <= 0) throw std::runtime_error("raw websocket test send failed");
    sent += static_cast<std::size_t>(count);
  }
}

std::string raw_receive_exact(int socket, std::size_t size) {
  std::string value(size, '\0');
  std::size_t received = 0;
  while (received < size) {
    const auto count = ::recv(socket, value.data() + received, size - received, 0);
    if (count <= 0) throw std::runtime_error("raw websocket test connection closed");
    received += static_cast<std::size_t>(count);
  }
  return value;
}

int raw_websocket_connect(
    std::uint16_t port,
    std::string_view target,
    std::string* response_headers = nullptr,
    std::string_view additional_headers = {}) {
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) throw std::runtime_error("raw websocket test socket failed");
  timeval timeout{2, 0};
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(socket);
    throw std::runtime_error("raw websocket test connect failed");
  }
  raw_send_all(
      socket,
      "GET " + std::string(target) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n" +
          std::string(additional_headers) + "\r\n");
  std::string headers;
  while (headers.find("\r\n\r\n") == std::string::npos) {
    headers += raw_receive_exact(socket, 1);
    if (headers.size() > 64 * 1024) throw std::runtime_error("raw websocket handshake is too large");
  }
  if (headers.find("HTTP/1.1 101 ") != 0) {
    ::close(socket);
    throw std::runtime_error("raw websocket upgrade failed");
  }
  if (response_headers != nullptr) *response_headers = headers;
  return socket;
}

std::string raw_http_exchange(std::uint16_t port, std::string_view request) {
  const int socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket < 0) throw std::runtime_error("raw HTTP test socket failed");
  try {
    timeval timeout{2, 0};
    ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(socket, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
      throw std::runtime_error("raw HTTP test connect failed");
    }
    raw_send_all(socket, request);
    std::string response;
    std::array<char, 4096> buffer{};
    while (true) {
      const auto received = ::recv(socket, buffer.data(), buffer.size(), 0);
      if (received == 0) break;
      if (received < 0) throw std::runtime_error("raw HTTP test receive failed");
      response.append(buffer.data(), static_cast<std::size_t>(received));
    }
    ::close(socket);
    return response;
  } catch (...) {
    ::close(socket);
    throw;
  }
}

struct RawWebSocketFrame {
  bool final{false};
  std::uint8_t opcode{0};
  std::string payload;
};

RawWebSocketFrame raw_receive_websocket_frame(int socket) {
  const auto header = raw_receive_exact(socket, 2);
  const auto first = static_cast<unsigned char>(header[0]);
  const auto second = static_cast<unsigned char>(header[1]);
  if ((second & 0x80U) != 0) throw std::runtime_error("raw websocket test received a masked server frame");
  std::uint64_t length = second & 0x7fU;
  if (length == 126) {
    const auto extended = raw_receive_exact(socket, 2);
    length = (static_cast<std::uint64_t>(static_cast<unsigned char>(extended[0])) << 8U) |
        static_cast<unsigned char>(extended[1]);
  } else if (length == 127) {
    const auto extended = raw_receive_exact(socket, 8);
    length = 0;
    for (const unsigned char byte : extended) length = (length << 8U) | byte;
  }
  if (length > 1024 * 1024) throw std::runtime_error("raw websocket test frame is unexpectedly large");
  return {
      (first & 0x80U) != 0,
      static_cast<std::uint8_t>(first & 0x0fU),
      raw_receive_exact(socket, static_cast<std::size_t>(length))};
}

mine_teleop::Json raw_receive_websocket_json(int socket) {
  const auto frame = raw_receive_websocket_frame(socket);
  if (!frame.final || frame.opcode != 0x1U) throw std::runtime_error("raw websocket test expected a text frame");
  return mine_teleop::Json::parse(frame.payload);
}

std::string raw_masked_websocket_header(
    std::uint8_t first,
    std::uint64_t advertised_length,
    int extended_length_bytes) {
  std::string frame(1, static_cast<char>(first));
  if (extended_length_bytes == 0) {
    if (advertised_length > 125U) throw std::invalid_argument("short websocket test length is too large");
    frame.push_back(static_cast<char>(0x80U | advertised_length));
  } else if (extended_length_bytes == 2) {
    if (advertised_length > 0xffffU) throw std::invalid_argument("16-bit websocket test length is too large");
    frame.push_back(static_cast<char>(0x80U | 126U));
    frame.push_back(static_cast<char>((advertised_length >> 8U) & 0xffU));
    frame.push_back(static_cast<char>(advertised_length & 0xffU));
  } else if (extended_length_bytes == 8) {
    frame.push_back(static_cast<char>(0x80U | 127U));
    for (int shift = 56; shift >= 0; shift -= 8) {
      frame.push_back(static_cast<char>((advertised_length >> shift) & 0xffU));
    }
  } else {
    throw std::invalid_argument("invalid websocket test length encoding");
  }
  return frame;
}

std::string raw_masked_websocket_frame(std::uint8_t first, std::string_view payload) {
  const int extended_length_bytes = payload.size() < 126U ? 0 : (payload.size() <= 0xffffU ? 2 : 8);
  std::string frame = raw_masked_websocket_header(first, payload.size(), extended_length_bytes);
  constexpr std::array<unsigned char, 4> mask{0x11U, 0x22U, 0x33U, 0x44U};
  frame.append(reinterpret_cast<const char*>(mask.data()), mask.size());
  for (std::size_t index = 0; index < payload.size(); ++index) {
    frame.push_back(static_cast<char>(static_cast<unsigned char>(payload[index]) ^ mask[index % mask.size()]));
  }
  return frame;
}

mine_teleop::Json signaling_request_for(
    std::string_view session_id,
    std::uint64_t sequence,
    std::string_view vehicle_id,
    std::string_view driver_id,
    std::string_view sender,
    std::string_view recipient,
    std::string_view credential_name,
    std::string_view credential,
    std::string_view type,
    const mine_teleop::Json& payload) {
  auto value = mine_teleop::ProtocolMetadata{
                   mine_teleop::kProtocolVersion,
                   std::string(vehicle_id),
                   std::string(driver_id),
                   std::string(session_id),
                   sequence,
                   mine_teleop::now_ms()}
                   .to_json();
  value["sender"] = sender;
  value["recipient"] = recipient;
  if (!credential_name.empty()) value[std::string(credential_name)] = credential;
  value["type"] = type;
  value["payload"] = payload;
  return value;
}

mine_teleop::Json signaling_request(
    std::string_view session_id,
    std::uint64_t sequence,
    std::string_view sender,
    std::string_view recipient,
    std::string_view credential_name,
    std::string_view credential,
    std::string_view type,
    const mine_teleop::Json& payload) {
  return signaling_request_for(
      session_id,
      sequence,
      "vehicle-001",
      "driver-console-001",
      sender,
      recipient,
      credential_name,
      credential,
      type,
      payload);
}

void test_shared_control_protocol_vector() {
  std::ifstream input("protocol/v1/control-command.valid.json");
  expect(input.good(), "shared protocol vector is missing");
  auto json = mine_teleop::Json::parse(input);
  json["future_field"] = "ignored";
  const auto command = mine_teleop::ControlCommand::from_json(json);
  expect(command.protocol_version == 1, "protocol vector version changed");
  expect(command.vehicle_id == "vehicle-001", "protocol vector vehicle changed");
  expect(command.driver_id == "driver-001", "protocol vector driver changed");
  expect(command.session_id == "session-001", "protocol vector session changed");
  expect(command.seq == 1024, "protocol vector sequence changed");
}

void test_loopback_http_server_and_port_conflict() {
  auto handler = [](const mine_teleop::HttpRequest& request) {
    if (request.path == "/health") {
      return mine_teleop::ServerResponse::json(
          200,
          {{"status", "ok"}, {"peer_address", request.peer_address}});
    }
    return mine_teleop::ServerResponse::json(404, {{"error", "not found"}});
  };
  mine_teleop::SimpleHttpServer first("127.0.0.1", 0, handler);
  first.start();
  mine_teleop::HttpClient http;
  const auto health = http.get_json("http://127.0.0.1:" + std::to_string(first.port()) + "/health");
  expect(health.value("status", "") == "ok", "loopback HTTP server is unreachable");
  expect(health.value("peer_address", "") == "127.0.0.1", "HTTP server did not retain the TCP peer address");

  bool conflict_reported = false;
  try {
    mine_teleop::SimpleHttpServer second("127.0.0.1", first.port(), handler);
    second.start();
    second.stop();
  } catch (const std::exception& error) {
    conflict_reported = std::string(error.what()).find("cannot bind HTTP listener") != std::string::npos;
  }
  first.stop();
  expect(conflict_reported, "occupied control port did not produce a clear bind error");
}

void test_signaling_time_sync_applies_backward_utc_correction() {
  std::atomic<std::int64_t> server_offset_ms{80};
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [&server_offset_ms](const mine_teleop::HttpRequest& request) {
        const auto client_send = request.query.find("client_send_ms");
        if (request.path != "/time" || client_send == request.query.end()) {
          return mine_teleop::ServerResponse::json(404, {{"error", "not found"}});
        }
        const auto server_time = mine_teleop::now_ms() + server_offset_ms.load();
        return mine_teleop::ServerResponse::json(
            200,
            {{"time_domain", "signaling_server"},
             {"client_send_ms", std::stoll(client_send->second)},
             {"server_receive_ms", server_time},
             {"server_send_ms", server_time}});
      });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  mine_teleop::SynchronizedClock clock;
  static_cast<void>(clock.synchronize(http, base, 7));
  const auto initial_lead_ms = clock.now_ms() - mine_teleop::now_ms();
  expect(initial_lead_ms >= 60, "synthetic positive UTC offset was not applied");

  server_offset_ms = 0;
  static_cast<void>(clock.synchronize(http, base, 7));
  const auto corrected_lead_ms = clock.now_ms() - mine_teleop::now_ms();
  expect(
      std::abs(corrected_lead_ms) <= 10,
      "backward UTC correction was permanently hidden by a monotonic clock clamp");
  server.stop();
}

void test_control_page_contract() {
  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001";
  config.signaling_url = "https://signal.example.test";
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "dev-password");
  mine_teleop::DriverConsoleHttpApp app(runtime);
  mine_teleop::HttpRequest request;
  request.method = "GET";
  request.path = "/";
  const auto response = app.handle(request);
  expect(response.status == 200, "control page did not load");
  expect(response.body.find("Mine Teleop WebRTC 控制台") != std::string::npos, "control page identity is missing");
  expect(response.body.find("rel=\"icon\" href=\"data:,\"") != std::string::npos, "control page triggers a favicon 404");
  expect(response.body.find("登录并加载车辆") != std::string::npos, "driver login UI is missing");
  expect(response.body.find("授权车辆") != std::string::npos, "authorized vehicle selector is missing");
  expect(response.body.find("认证有效至") != std::string::npos, "driver token expiry is not visible");
  expect(response.body.find("登录已失效，请重新认证") != std::string::npos, "driver re-authentication UX is missing");
  expect(response.body.find("post('/api/login'") != std::string::npos, "driver credential is not handled by the local runtime");
  expect(
      response.body.find("passwordInput.value='';const result=await post('/api/login'") != std::string::npos,
      "driver credential is not cleared before the login request can fail");
  expect(
      response.body.find("controlAuthorityLost=false;webrtcLabel.textContent='未连接'") != std::string::npos,
      "successful reauthentication leaves a stale lost-authority label visible");
  expect(response.body.find("navigator.getGamepads") != std::string::npos, "Gamepad discovery is missing");
  expect(response.body.find("开始量程校准") != std::string::npos, "Gamepad range calibration is missing");
  expect(response.body.find("post('/api/control',currentControl(extra))") != std::string::npos, "control inputs do not share one normalized path");
  expect(response.body.find("driver_vehicle_switch_started") != std::string::npos, "safe vehicle switching UI is missing");
  const auto switch_request = response.body.find("session=await post('/api/connect'");
  const auto realtime_close = response.body.find("generation=closeRealtimeSession()", switch_request);
  expect(
      switch_request != std::string::npos && realtime_close != std::string::npos && switch_request < realtime_close,
      "vehicle switch closes the current realtime path before the target is accepted");
  expect(
      response.body.find("driver_vehicle_switch_rejected") != std::string::npos &&
          response.body.find("pollSignaling(suspendedGeneration)") != std::string::npos,
      "rejected vehicle switch cannot resume the retained realtime session");
  expect(
      response.body.find("suspendSignalingPoll()") != std::string::npos &&
          response.body.find("post('/api/poll-signaling',{},controller.signal)") != std::string::npos,
      "vehicle switching cannot cancel a stale browser signaling request without closing realtime media");
  expect(
      response.body.find("current=vehicle.vehicle_id===currentVehicle,selectable=vehicle.controllable||current") !=
          std::string::npos,
      "the active vehicle cannot remain selected while other switch targets refresh");
  expect(
      response.body.find("if(authenticated&&!connecting)refreshVehicles()") != std::string::npos,
      "vehicle availability does not refresh during an active session");
  expect(
      response.body.find("if(result.signaling_restart_recovered)") != std::string::npos &&
          response.body.find("旧控制权未恢复，请重新选择车辆") != std::string::npos &&
          response.body.find("control_authority_recovered:false") != std::string::npos,
      "signaling restart recovery does not preserve the no-authority operator state");
  expect(
      response.body.find("if(result.signaling_available===false)") != std::string::npos &&
          response.body.find("车辆列表为安全快照，禁止建立控制会话") != std::string::npos &&
          response.body.find("connectButton.disabled=true") != std::string::npos,
      "signaling outage vehicle refresh is not fail-safe and locally readable");
  expect(
      response.body.find("if(error.status===401){requireLogin") != std::string::npos &&
          response.body.find("车辆状态刷新失败，当前会话已保留") != std::string::npos,
      "a transient vehicle-list refresh failure can discard current authority");
  expect(response.body.find("post('/api/end-session'") != std::string::npos, "failed connection setup cannot release only its session");
  expect(response.body.find("pollSignaling(generation)") != std::string::npos, "stale signaling pollers are not isolated across vehicle switches");
  expect(response.body.find("Gamepad 已断开（输出已归零）") != std::string::npos, "Gamepad disconnect does not expose the safe-zero state");
  expect(response.body.find("车辆必须本地确认后才能复位") != std::string::npos, "ESTOP latch feedback is missing");
  expect(response.body.find("运行监控") != std::string::npos, "operator monitoring panel is missing");
  expect(response.body.find("控制 RTT") != std::string::npos, "control RTT display is missing");
  expect(response.body.find("packet_loss_percent") != std::string::npos, "per-stream packet-loss metric is missing");
  expect(response.body.find("connection_method") != std::string::npos, "ICE path classification is missing");
  expect(
      response.body.find("iceTransportPolicy:consoleConfig.ice_transport_policy") != std::string::npos,
      "browser RTCPeerConnection does not apply the configured ICE transport policy");
  expect(
      response.body.find("webrtc_ice_candidate_error") != std::string::npos,
      "browser does not record credential-free ICE candidate errors");
  expect(
      response.body.find("if(connectionState==='disconnected')") != std::string::npos &&
          response.body.find("if(['failed','closed'].includes(connectionState))") != std::string::npos &&
          response.body.find("peer!==nextPeer") != std::string::npos,
      "a transient or stale peer state change can discard a recoverable control DataChannel");
  expect(
      response.body.find("peer.connectionState!=='connected'") != std::string::npos,
      "control commands can be sent while the current WebRTC peer is disconnected");
  expect(
      response.body.find("webrtcLabel.textContent==='控制链路拥塞'") != std::string::npos,
      "the operator label cannot recover after DataChannel backpressure clears");
  expect(response.body.find("时延超过 200 ms") != std::string::npos, "latency threshold alarm is missing");
  expect(response.body.find("低于 20 FPS") != std::string::npos, "FPS threshold alarm is missing");
  expect(response.body.find("sent_at_utc_ms:Date.now()") != std::string::npos, "browser-local logs do not use UTC milliseconds");
  expect(response.body.find("fetch('/api/browser-event'") != std::string::npos, "browser events are not persisted by the local runtime");
  expect(response.body.find("dev-password") == std::string::npos, "driver credential leaked into the control page");
  expect(response.body.find("await send({},false)") != std::string::npos, "background safety ticks can override waiting state");
  expect(response.body.find("addEventListener('pagehide'") != std::string::npos, "page close does not release the session");
  const auto initial_status = runtime->status();
  expect(
      initial_status.at("last_signaling_messages").is_array() &&
          initial_status.at("last_signaling_messages").empty(),
      "initial signaling messages are not an empty array");
  expect(
      initial_status.at("webrtc_metrics").is_object() && initial_status.at("webrtc_metrics").empty(),
      "initial WebRTC metrics are not an empty object");
  expect(
      initial_status.at("authorized_vehicles").is_array() && initial_status.at("authorized_vehicles").empty(),
      "initial authorized vehicles are not an empty array");
}

void test_driver_gamepad_config() {
  const auto config = mine_teleop::load_driver_config("configs/driver-console.dev.yaml");
  expect(config.gamepad.enabled, "Gamepad was disabled unexpectedly");
  expect(config.gamepad.steering_axis == 0, "Gamepad steering axis changed");
  expect(config.gamepad.throttle_axis == 2, "Gamepad throttle axis changed");
  expect(config.gamepad.brake_axis == 5, "Gamepad brake axis changed");
  expect(config.gamepad.axis_deadzone == 0.05, "Gamepad deadzone changed");
  expect(config.gamepad.steering_range == 1.0, "Gamepad steering range changed");
  expect(config.gamepad.throttle_range == 2.0, "Gamepad throttle range changed");
  expect(config.gamepad.brake_range == 2.0, "Gamepad brake range changed");
  expect(config.browser_event_log_path.is_absolute(), "browser event log path was not resolved absolutely");
  expect(
      config.browser_event_log_path.filename() == "control-browser-events.jsonl",
      "browser event log filename changed");
  expect(config.browser_event_log_max_bytes == 2097152, "browser event log rotation size changed");
  expect(config.browser_event_log_files == 3, "browser event log retention count changed");

  const auto field = mine_teleop::load_driver_config("configs/driver-console.three-machine.dev.yaml");
  expect(
      field.signaling_url == "wss://teleop-field.internal:6000/signaling",
      "field driver signaling URL does not use the private TLS name");
  expect(
      field.resolve_entries ==
          std::vector<std::string>{"teleop-field.internal:6000:60.205.213.254"},
      "field driver resolver override changed");
  expect(
      field.ca_bundle == std::filesystem::absolute("configs/mine-teleop-field-root.crt").lexically_normal(),
      "field driver CA path was not resolved relative to its config");
  expect(std::filesystem::is_regular_file(field.ca_bundle), "field driver CA bundle is missing");
  expect(field.ice_transport_policy == "all", "field driver ICE policy is not the safe default");
  expect(field.max_time_sync_uncertainty_ms == 25, "field driver time synchronization limit is not 25ms");
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  expect(input.good(), "expected log file is missing: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void write_text_file(const std::filesystem::path& path, std::string_view value) {
  std::ofstream output(path);
  expect(output.good(), "cannot create test file: " + path.string());
  output << value;
  expect(output.good(), "cannot write test file: " + path.string());
}

void test_signaling_multi_identity_config() {
  const auto root = std::filesystem::temp_directory_path() /
      ("mine-teleop-signaling-identities-" + mine_teleop::random_token(6));
  std::filesystem::create_directories(root);
  write_text_file(root / "driver-1.password", "driver-password-1\n");
  write_text_file(root / "driver-2.password", "driver-password-2\n");
  write_text_file(root / "vehicle-1.token", "vehicle-token-1\n");
  write_text_file(root / "vehicle-2.token", "vehicle-token-2\n");
  const auto valid_path = root / "valid.yaml";
  write_text_file(
      valid_path,
      R"YAML(auth:
  drivers:
    - id: driver-console-001
      password_file: driver-1.password
      vehicles: [vehicle-001]
    - id: driver-console-002
      password_file: driver-2.password
      vehicles: [vehicle-002]
  vehicles:
    - id: vehicle-001
      device_token_file: vehicle-1.token
    - id: vehicle-002
      device_token_file: vehicle-2.token
)YAML");
  const auto config = mine_teleop::load_signaling_identity_config(valid_path);
  expect(config.driver_passwords.size() == 2, "multi-identity config lost a driver");
  expect(config.device_tokens.size() == 2, "multi-identity config lost a vehicle");
  expect(
      config.driver_passwords.at("driver-console-001") == "driver-password-1",
      "relative driver secret file was not loaded");
  expect(
      config.device_tokens.at("vehicle-002") == "vehicle-token-2",
      "relative vehicle secret file was not loaded");
  expect(
      config.driver_vehicle_permissions.at("driver-console-001") ==
          std::unordered_set<std::string>{"vehicle-001"},
      "first driver permission changed");
  expect(
      config.driver_vehicle_permissions.at("driver-console-002") ==
          std::unordered_set<std::string>{"vehicle-002"},
      "second driver permission changed");
  {
    mine_teleop::SignalingService service(config);
    expect(service.health().value("status", "") == "ok", "loaded multi-identity service did not initialize");
  }

  const auto duplicate_path = root / "duplicate.yaml";
  write_text_file(
      duplicate_path,
      R"YAML(auth:
  drivers:
    - id: driver-console-001
      password_file: driver-1.password
      vehicles: [vehicle-001]
    - id: driver-console-001
      password_file: driver-2.password
      vehicles: [vehicle-001]
  vehicles:
    - id: vehicle-001
      device_token_file: vehicle-1.token
)YAML");
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_signaling_identity_config(duplicate_path)); },
      "duplicate driver identity was accepted");

  const auto empty_permissions_path = root / "empty-permissions.yaml";
  write_text_file(
      empty_permissions_path,
      R"YAML(auth:
  drivers:
    - id: driver-console-001
      password_file: driver-1.password
      vehicles: []
  vehicles:
    - id: vehicle-001
      device_token_file: vehicle-1.token
)YAML");
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_signaling_identity_config(empty_permissions_path)); },
      "driver with no vehicle permissions was accepted");

  const auto unknown_vehicle_path = root / "unknown-vehicle.yaml";
  write_text_file(
      unknown_vehicle_path,
      R"YAML(auth:
  drivers:
    - id: driver-console-001
      password_file: driver-1.password
      vehicles: [vehicle-999]
  vehicles:
    - id: vehicle-001
      device_token_file: vehicle-1.token
)YAML");
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_signaling_identity_config(unknown_vehicle_path)); },
      "permission for an unknown vehicle was accepted");

  const auto ambiguous_secret_path = root / "ambiguous-secret.yaml";
  write_text_file(
      ambiguous_secret_path,
      R"YAML(auth:
  drivers:
    - id: driver-console-001
      password_file: driver-1.password
      password_env: MINE_TELEOP_TEST_DRIVER_PASSWORD
      vehicles: [vehicle-001]
  vehicles:
    - id: vehicle-001
      device_token_file: vehicle-1.token
)YAML");
  expect_throws(
      [&] { static_cast<void>(mine_teleop::load_signaling_identity_config(ambiguous_secret_path)); },
      "identity with two secret sources was accepted");
  std::filesystem::remove_all(root);
}

void test_credential_purpose_separation_and_stale_control_replay() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-purpose", "driver-purpose-password"}};
  config.device_tokens = {{"vehicle-purpose", "vehicle-purpose-device-token"}};
  config.driver_vehicle_permissions = {{"driver-purpose", {"vehicle-purpose"}}};
  config.turn_urls = {"turn:127.0.0.1:3478?transport=udp"};
  config.turn_realm = "purpose.test";
  config.turn_static_auth_secret = "turn-purpose-static-secret";
  auto service = std::make_shared<mine_teleop::SignalingService>(std::move(config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;

  expect(
      http.post_json(
              base + "/auth/driver_login",
              {{"driver_id", "driver-purpose"}, {"password", "vehicle-purpose-device-token"}})
              .status == 401,
      "device credential was accepted as a driver password");
  expect(
      http.post_json(
              base + "/vehicles/online",
              {{"vehicle_id", "vehicle-purpose"},
               {"device_token", "driver-purpose-password"},
               {"connection_id", "purpose-invalid-device"}})
              .status == 401,
      "driver credential was accepted as a device credential");

  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-purpose"},
       {"device_token", "vehicle-purpose-device-token"},
       {"connection_id", "purpose-device-runtime"}});
  const auto generation = online.at("connection_generation").get<std::uint64_t>();
  const auto login = http.post_json_response(
      base + "/auth/driver_login",
      {{"driver_id", "driver-purpose"}, {"password", "driver-purpose-password"}});
  const auto driver_token = login.at("token").get<std::string>();
  const auto first_session = http.post_json_response(
      base + "/sessions",
      {{"driver_id", "driver-purpose"}, {"vehicle_id", "vehicle-purpose"}, {"token", driver_token}});
  const auto first_session_id = first_session.at("session_id").get<std::string>();
  const auto first_control_token = first_session.at("control_token").get<std::string>();

  expect(
      http.get(
              base + "/sessions/" + first_session_id + "?actor=driver-purpose&token=" +
              http.url_encode("vehicle-purpose-device-token"))
              .status == 401,
      "device credential was accepted as a driver token");
  expect(
      http.get(
              base + "/sessions/" + first_session_id + "?actor=vehicle-purpose&device_token=" +
              http.url_encode(driver_token) + "&connection_generation=" + std::to_string(generation))
              .status == 401,
      "driver token was accepted as a device token");

  const auto ice = http.get_json(
      base + "/sessions/" + first_session_id + "/ice_servers?actor=driver-purpose&token=" +
      http.url_encode(driver_token));
  std::string turn_credential;
  for (const auto& server_config : ice.at("ice_servers")) {
    if (server_config.contains("credential")) {
      turn_credential = server_config.at("credential").get<std::string>();
      break;
    }
  }
  expect(!turn_credential.empty(), "TURN credential was not issued for purpose-isolation testing");
  expect(
      http.get(
              base + "/sessions/" + first_session_id + "?actor=driver-purpose&token=" +
              http.url_encode(turn_credential))
              .status == 401,
      "TURN credential was accepted as a driver token");
  expect(
      http.get(
              base + "/sessions/" + first_session_id + "?actor=vehicle-purpose&device_token=" +
              http.url_encode(turn_credential) + "&connection_generation=" + std::to_string(generation))
              .status == 401,
      "TURN credential was accepted as a device token");
  expect(
      http.get(
              base + "/sessions/" + first_session_id + "?actor=driver-purpose&token=" +
              http.url_encode(first_control_token))
              .status == 401,
      "control token was accepted as a driver token");

  static_cast<void>(http.post_json_response(
      base + "/sessions/" + first_session_id + "/end",
      {{"actor", "driver-purpose"}, {"token", driver_token}, {"reason", "purpose_test_replace"}}));
  const auto replacement = http.post_json_response(
      base + "/sessions",
      {{"driver_id", "driver-purpose"}, {"vehicle_id", "vehicle-purpose"}, {"token", driver_token}});
  const auto replacement_session_id = replacement.at("session_id").get<std::string>();
  const auto replacement_control_token = replacement.at("control_token").get<std::string>();
  expect(replacement_control_token != first_control_token, "replacement session reused the old control token");

  mine_teleop::ControlCommand replay;
  replay.vehicle_id = "vehicle-purpose";
  replay.driver_id = "driver-purpose";
  replay.session_id = replacement_session_id;
  replay.seq = 1;
  replay.sent_at_utc_ms = mine_teleop::now_ms();
  replay.control_token = first_control_token;
  mine_teleop::ControlReceiver receiver(
      replay.vehicle_id,
      replay.driver_id,
      replay.session_id,
      200,
      mine_teleop::kProtocolVersion,
      true,
      replacement_control_token);
  const auto stale = receiver.accept(replay, mine_teleop::now_ms());
  expect(
      !stale.accepted && stale.reason == "control_token_invalid",
      "replacement session accepted the stale control token");
  replay.control_token = replacement_control_token;
  expect(receiver.accept(replay, mine_teleop::now_ms()).accepted, "replacement control token was rejected");

  static_cast<void>(http.post_json_response(
      base + "/sessions/" + replacement_session_id + "/end",
      {{"actor", "driver-purpose"}, {"token", driver_token}, {"reason", "purpose_test_complete"}}));
  static_cast<void>(http.post_json_response(
      base + "/auth/driver_logout",
      {{"driver_id", "driver-purpose"}, {"token", driver_token}, {"reason", "purpose_test_complete"}}));
  server.stop();
}

void test_browser_event_logging_rotation_and_redaction() {
  const auto root = std::filesystem::temp_directory_path() /
      ("mine-teleop-control-log-test-" + std::to_string(mine_teleop::now_ms()));
  const auto log_path = root / "events.jsonl";
  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001";
  config.signaling_url = "https://signal.example.test";
  config.browser_event_log_path = log_path;
  config.browser_event_log_max_bytes = 1024;
  config.browser_event_log_files = 2;
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "dev-password");
  mine_teleop::DriverConsoleHttpApp app(runtime);

  mine_teleop::HttpRequest request;
  request.method = "POST";
  request.path = "/api/browser-event";
  for (int index = 0; index < 3; ++index) {
    request.body = mine_teleop::Json({
        {"event", "control_monitor_state"},
        {"sent_at_utc_ms", mine_teleop::now_ms()},
        {"details",
         {{"index", index},
          {"password", "never-write-this-secret"},
          {"nested", {{"access_token", "also-never-write-this"}}},
          {"message", std::string(640, static_cast<char>('a' + index))}}},
    }).dump();
    expect(app.handle(request).status == 200, "browser event could not be persisted");
  }

  expect(std::filesystem::is_regular_file(log_path), "current browser event log is missing");
  const auto backup_path = std::filesystem::path(log_path.string() + ".1");
  expect(std::filesystem::is_regular_file(backup_path), "browser event log did not rotate");
  const auto persisted = read_text_file(log_path) + read_text_file(backup_path);
  expect(persisted.find("control_monitor_state") != std::string::npos, "browser event name was not written");
  expect(persisted.find("[redacted]") != std::string::npos, "browser event credentials were not redacted");
  expect(persisted.find("never-write-this") == std::string::npos, "browser event log contains a credential");

  request.body = R"({"event":"bad event","details":{}})";
  expect(app.handle(request).status == 400, "invalid browser event name was accepted");
  request.body = R"({"event":"valid_event","sent_at_utc_ms":"not-a-number","details":{}})";
  expect(app.handle(request).status == 400, "invalid browser event timestamp was accepted");
  std::filesystem::remove_all(root);
}

void test_driver_vehicle_switch_releases_old_session() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {
      {"vehicle-001", "vehicle-secret-1"},
      {"vehicle-002", "vehicle-secret-2"},
  };
  signaling_config.driver_vehicle_permissions = {
      {"driver-console-001", {"vehicle-001", "vehicle-002"}},
  };
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto vehicle_one = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "switch-test-vehicle-1"}});
  const auto vehicle_two = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-002"},
       {"device_token", "vehicle-secret-2"},
       {"connection_id", "switch-test-vehicle-2"}});

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto first = driver.connect("vehicle-001");
  const auto second = driver.connect("vehicle-002");
  expect(
      first.value("session_id", "") != second.value("session_id", ""),
      "vehicle switch reused the old session");
  expect(second.value("vehicle_id", "") == "vehicle-002", "vehicle switch selected the wrong vehicle");

  const auto old_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=vehicle-secret-1&connection_generation=" +
      std::to_string(vehicle_one.at("connection_generation").get<std::uint64_t>()));
  expect(old_session.value("session_id", "").empty(), "vehicle switch left the old session active");
  const auto new_session = http.get_json(
      base + "/vehicles/vehicle-002/session?device_token=vehicle-secret-2&connection_generation=" +
      std::to_string(vehicle_two.at("connection_generation").get<std::uint64_t>()));
  expect(
      new_session.value("session_id", "") == second.value("session_id", ""),
      "vehicle switch did not activate the new session");
  const auto prepared = driver.send_control(
      {{"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}});
  expect(
      prepared.at("command").value("vehicle_id", "") == "vehicle-002",
      "post-switch control command targeted the old vehicle");

  const auto ended = driver.end_session("switch_test_complete");
  expect(!ended.value("connected", true), "explicit session end did not clear local authority");
  expect(driver.vehicles().value("authenticated", false), "session end unexpectedly logged out the driver");
  static_cast<void>(driver.disconnect("switch_test_logout"));
  server.stop();
}

void test_two_driver_two_vehicle_wss_isolation_and_safe_rejection() {
  const auto root = std::filesystem::temp_directory_path() /
      ("mine-teleop-two-driver-isolation-" + mine_teleop::random_token(6));
  std::filesystem::create_directories(root);
  const auto audit_path = root / "signaling-audit.jsonl";

  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {
      {"driver-console-001", "dev-password-1"},
      {"driver-console-002", "dev-password-2"},
  };
  signaling_config.device_tokens = {
      {"vehicle-001", "vehicle-secret-1"},
      {"vehicle-002", "vehicle-secret-2"},
  };
  signaling_config.driver_vehicle_permissions = {
      {"driver-console-001", {"vehicle-001", "vehicle-002"}},
      {"driver-console-002", {"vehicle-002"}},
  };
  signaling_config.audit_log_path = audit_path.string();
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto vehicle_one = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "two-driver-vehicle-1"}});
  const auto vehicle_two = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-002"},
       {"device_token", "vehicle-secret-2"},
       {"connection_id", "two-driver-vehicle-2"}});
  const auto generation_one = vehicle_one.at("connection_generation").get<std::uint64_t>();
  const auto generation_two = vehicle_two.at("connection_generation").get<std::uint64_t>();

  mine_teleop::DriverConfig driver_one_config;
  driver_one_config.driver_id = "driver-console-001";
  driver_one_config.signaling_url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/signaling";
  mine_teleop::DriverConfig driver_two_config;
  driver_two_config.driver_id = "driver-console-002";
  driver_two_config.signaling_url = driver_one_config.signaling_url;
  mine_teleop::DriverConsoleRuntime driver_one(driver_one_config, "vehicle-001", "dev-password-1");
  mine_teleop::DriverConsoleRuntime driver_two(driver_two_config, "vehicle-002", "dev-password-2");

  mine_teleop::Json connected_one;
  mine_teleop::Json connected_two;
  std::exception_ptr connect_failure_one;
  std::exception_ptr connect_failure_two;
  std::thread connect_one([&] {
    try {
      connected_one = driver_one.connect("vehicle-001");
    } catch (...) {
      connect_failure_one = std::current_exception();
    }
  });
  std::thread connect_two([&] {
    try {
      connected_two = driver_two.connect("vehicle-002");
    } catch (...) {
      connect_failure_two = std::current_exception();
    }
  });
  connect_one.join();
  connect_two.join();
  if (connect_failure_one) std::rethrow_exception(connect_failure_one);
  if (connect_failure_two) std::rethrow_exception(connect_failure_two);

  const auto session_one = connected_one.at("session_id").get<std::string>();
  const auto session_two = connected_two.at("session_id").get<std::string>();
  expect(session_one != session_two, "simultaneous drivers shared a session id");
  const auto active_health = http.get_json(base + "/health");
  expect(active_health.value("online_vehicles", 0U) == 2, "two-vehicle presence was not retained");
  expect(active_health.value("online_drivers", 0U) == 2, "two-driver WSS presence was not registered");
  expect(active_health.value("active_sessions", 0U) == 2, "two independent sessions were not active");

  expect_throws(
      [&] { static_cast<void>(driver_one.connect("vehicle-002")); },
      "busy vehicle switch unexpectedly succeeded");
  expect_throws(
      [&] { static_cast<void>(driver_two.connect("vehicle-001")); },
      "unauthorized vehicle switch unexpectedly succeeded");
  const auto status_one = driver_one.status();
  const auto status_two = driver_two.status();
  expect(status_one.value("session_id", "") == session_one, "busy switch released the first valid session");
  expect(status_one.value("vehicle_id", "") == "vehicle-001", "busy switch changed the first vehicle");
  expect(status_one.value("signaling_websocket_connected", false), "busy switch closed the first WSS session");
  expect(status_two.value("session_id", "") == session_two, "unauthorized switch released the second session");
  expect(status_two.value("vehicle_id", "") == "vehicle-002", "unauthorized switch changed the second vehicle");
  expect(
      status_two.value("signaling_websocket_connected", false),
      "unauthorized switch closed the second WSS session");

  const auto control_one = driver_one.send_control(
      {{"gear", "N"}, {"steering", -0.25}, {"throttle", 0.1}, {"brake", 0.0}});
  const auto control_two = driver_two.send_control(
      {{"gear", "N"}, {"steering", 0.25}, {"throttle", 0.2}, {"brake", 0.0}});
  const auto& command_one = control_one.at("command");
  const auto& command_two = control_two.at("command");
  expect(command_one.value("vehicle_id", "") == "vehicle-001", "first control command crossed vehicles");
  expect(command_one.value("session_id", "") == session_one, "first control command crossed sessions");
  expect(command_two.value("vehicle_id", "") == "vehicle-002", "second control command crossed vehicles");
  expect(command_two.value("session_id", "") == session_two, "second control command crossed sessions");
  const auto control_token_one = command_one.value("control_token", "");
  const auto control_token_two = command_two.value("control_token", "");
  expect(!control_token_one.empty(), "first session omitted its control token");
  expect(!control_token_two.empty(), "second session omitted its control token");
  expect(
      control_token_one != control_token_two,
      "independent sessions shared a control token");

  auto offer_one = signaling_request_for(
      session_one,
      1,
      "vehicle-001",
      "driver-console-001",
      "vehicle-001",
      "driver-console-001",
      "device_token",
      "vehicle-secret-1",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0\r\no=vehicle-001"}, {"media_tracks", mine_teleop::Json::array()}});
  offer_one["connection_generation"] = generation_one;
  auto offer_two = signaling_request_for(
      session_two,
      1,
      "vehicle-002",
      "driver-console-002",
      "vehicle-002",
      "driver-console-002",
      "device_token",
      "vehicle-secret-2",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0\r\no=vehicle-002"}, {"media_tracks", mine_teleop::Json::array()}});
  offer_two["connection_generation"] = generation_two;
  static_cast<void>(http.post_json_response(base + "/signaling/" + session_one + "/messages", offer_one));
  static_cast<void>(http.post_json_response(base + "/signaling/" + session_two + "/messages", offer_two));

  const auto pushed_one = driver_one.poll_signaling();
  const auto pushed_two = driver_two.poll_signaling();
  expect(pushed_one.at("messages").size() == 1, "first WSS session did not receive exactly one offer");
  expect(pushed_two.at("messages").size() == 1, "second WSS session did not receive exactly one offer");
  expect(
      pushed_one.at("messages").at(0).at("payload").value("sdp", "") == "v=0\r\no=vehicle-001",
      "first WSS session received the second vehicle offer");
  expect(
      pushed_two.at("messages").at(0).at("payload").value("sdp", "") == "v=0\r\no=vehicle-002",
      "second WSS session received the first vehicle offer");

  static_cast<void>(driver_one.send_webrtc_answer({{"type", "answer"}, {"sdp", "v=0\r\no=driver-001"}}));
  static_cast<void>(driver_two.send_webrtc_answer({{"type", "answer"}, {"sdp", "v=0\r\no=driver-002"}}));
  const auto answers_one = http.get_json(
      base + "/signaling/" + session_one +
      "/messages?recipient=vehicle-001&device_token=vehicle-secret-1&connection_generation=" +
      std::to_string(generation_one));
  const auto answers_two = http.get_json(
      base + "/signaling/" + session_two +
      "/messages?recipient=vehicle-002&device_token=vehicle-secret-2&connection_generation=" +
      std::to_string(generation_two));
  expect(answers_one.at("messages").size() == 1, "first vehicle did not receive exactly one answer");
  expect(answers_two.at("messages").size() == 1, "second vehicle did not receive exactly one answer");
  expect(
      answers_one.at("messages").at(0).at("payload").value("sdp", "") == "v=0\r\no=driver-001",
      "first vehicle received the second driver's answer");
  expect(
      answers_two.at("messages").at(0).at("payload").value("sdp", "") == "v=0\r\no=driver-002",
      "second vehicle received the first driver's answer");

  static_cast<void>(driver_one.disconnect("two_driver_isolation_complete"));
  static_cast<void>(driver_two.disconnect("two_driver_isolation_complete"));
  mine_teleop::Json released_health;
  for (int attempt = 0; attempt < 40; ++attempt) {
    released_health = http.get_json(base + "/health");
    if (released_health.value("active_sessions", 1U) == 0 &&
        released_health.value("online_drivers", 1U) == 0) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  expect(released_health.value("active_sessions", 1U) == 0, "driver logout left a session active");
  expect(released_health.value("online_drivers", 1U) == 0, "driver logout left WSS presence online");
  expect(released_health.value("online_vehicles", 0U) == 2, "driver logout removed vehicle presence");
  server.stop();

  const auto audit = read_text_file(audit_path);
  expect(audit.find(session_one) != std::string::npos, "audit omitted the first session");
  expect(audit.find(session_two) != std::string::npos, "audit omitted the second session");
  expect(audit.find("driver-console-001") != std::string::npos, "audit omitted the first driver");
  expect(audit.find("driver-console-002") != std::string::npos, "audit omitted the second driver");
  expect(audit.find("vehicle-001") != std::string::npos, "audit omitted the first vehicle");
  expect(audit.find("vehicle-002") != std::string::npos, "audit omitted the second vehicle");
  expect(audit.find("service_instance_id") != std::string::npos, "audit omitted service correlation");
  expect(audit.find("dev-password") == std::string::npos, "audit leaked a driver password");
  expect(audit.find("vehicle-secret") == std::string::npos, "audit leaked a vehicle secret");
  expect(audit.find(control_token_one) == std::string::npos, "audit leaked the first control token");
  expect(audit.find(control_token_two) == std::string::npos, "audit leaked the second control token");
  std::filesystem::remove_all(root);
}

void test_failed_logout_keeps_retryable_local_authority() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "logout-failure-vehicle"}}));
  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  static_cast<void>(driver.connect("vehicle-001"));
  server.stop();

  bool logout_failed = false;
  try {
    static_cast<void>(driver.disconnect("server_unreachable_test"));
  } catch (const std::exception&) {
    logout_failed = true;
  }
  expect(logout_failed, "logout unexpectedly succeeded after the signaling server stopped");
  const auto status = driver.status();
  expect(status.value("authenticated", false), "failed logout discarded the token needed for retry");
  expect(status.value("connected", false), "failed logout falsely reported the server session as released");
}

void test_local_proxy_preserves_upstream_auth_status() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  signaling_config.token_ttl_ms = 80;
  signaling_config.connection_reaper_interval_ms = 5;
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = "http://127.0.0.1:" + std::to_string(server.port());
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(
      driver_config, "vehicle-001", "dev-password");
  static_cast<void>(runtime->login("dev-password"));
  std::this_thread::sleep_for(std::chrono::milliseconds(130));

  mine_teleop::DriverConsoleHttpApp app(runtime);
  mine_teleop::HttpRequest request;
  request.method = "GET";
  request.path = "/api/vehicles";
  const auto response = app.handle(request);
  expect(response.status == 401, "local control proxy converted upstream token expiry into a conflict");
  expect(
      response.body.find("driver token") != std::string::npos,
      "local control proxy omitted the upstream authentication failure");
  const auto expired_health = signaling->health();
  expect(
      expired_health.value("online_drivers", std::size_t{1}) == 0,
      "expired driver token left a duplicate-login presence behind");
  const auto relogin = runtime->login("dev-password");
  expect(relogin.value("authenticated", false), "same driver could not log in after token expiry cleanup");
  static_cast<void>(runtime->disconnect("token_expiry_relogin_test_complete"));
  server.stop();
}

void test_control_authority_lease_renews_without_rotating_token() {
  const auto audit_path = std::filesystem::path("/tmp") /
      ("mine-teleop-control-renewal-" + mine_teleop::random_token(6) + ".jsonl");
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  // Keep the lease short enough for a fast test, but leave enough scheduling
  // margin for emulated amd64 builders where a 140 ms lease can expire during
  // a single host scheduling stall.
  signaling_config.control_token_ttl_ms = 400;
  signaling_config.token_ttl_ms = 3000;
  signaling_config.vehicle_heartbeat_timeout_ms = 3000;
  signaling_config.driver_heartbeat_timeout_ms = 3000;
  signaling_config.connection_reaper_interval_ms = 5;
  signaling_config.audit_log_path = audit_path.string();
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "control-renewal-vehicle"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  driver_config.time_sync_interval_ms = 60 * 1000;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connected = driver.connect("vehicle-001");
  const auto session_id = connected.at("session_id").get<std::string>();
  const auto initial_status = driver.status();
  const auto initial_expiry = initial_status.at("control_token_expires_at_utc_ms").get<std::int64_t>();
  const auto first_control = driver.send_control(
      {{"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}});
  const auto original_control_token = first_control.at("command").value("control_token", "");
  expect(!original_control_token.empty(), "initial control lease omitted its token");

  const auto vehicle_renewal = http.post_json(
      base + "/sessions/" + session_id + "/renew",
      {{"actor", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_generation", vehicle_generation}});
  expect(vehicle_renewal.status == 401, "vehicle participant renewed driver control authority");

  std::int64_t latest_expiry = initial_expiry;
  const auto renewal_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
  while (std::chrono::steady_clock::now() < renewal_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const auto status = driver.status();
    expect(status.value("session_id", "") == session_id, "control renewal changed the active session");
    expect(status.value("signaling_websocket_connected", false), "control renewal closed the WSS connection");
    latest_expiry = status.at("control_token_expires_at_utc_ms").get<std::int64_t>();
  }
  expect(latest_expiry > initial_expiry + 650, "control authority expiry was not extended repeatedly");
  expect(signaling->health().value("active_sessions", std::size_t{0}) == 1, "renewed session expired early");

  const auto renewed_vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=vehicle-secret-1&connection_generation=" +
      std::to_string(vehicle_generation));
  expect(
      renewed_vehicle_session.value("control_token", "") == original_control_token,
      "control authority renewal rotated the DataChannel token");
  expect(
      renewed_vehicle_session.value("control_token_expires_at_utc_ms", std::int64_t{0}) == latest_expiry,
      "vehicle did not observe the renewed control lease expiry");
  const auto final_control = driver.send_control(
      {{"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}});
  expect(
      final_control.at("command").value("control_token", "") == original_control_token,
      "renewed driver command changed the active control token");

  const auto remaining_ms = std::max<std::int64_t>(0, latest_expiry - mine_teleop::now_ms());
  std::this_thread::sleep_for(std::chrono::milliseconds(remaining_ms + 80));
  expect(signaling->health().value("active_sessions", std::size_t{1}) == 0, "unrenewed lease did not expire");
  bool authority_loss_reported = false;
  try {
    static_cast<void>(driver.poll_signaling());
  } catch (const std::runtime_error& error) {
    authority_loss_reported = std::string(error.what()).find("authority is no longer valid") != std::string::npos;
  }
  expect(authority_loss_reported, "expired renewed authority was not reported through WSS");
  expect(!driver.status().value("connected", true), "expired renewed authority remained connected locally");
  static_cast<void>(driver.disconnect("control_renewal_test_complete"));
  server.stop();

  const auto audit = read_text_file(audit_path);
  expect(audit.find("\"event\":\"control_authority_renewed\"") != std::string::npos, "renewal audit is missing");
  expect(audit.find(original_control_token) == std::string::npos, "renewal audit leaked the control token");
  std::filesystem::remove(audit_path);
}

void test_control_transport_and_loopback_policy() {
  expect(mine_teleop::is_loopback_bind_address("127.0.0.1"), "IPv4 loopback address was rejected");
  expect(mine_teleop::is_loopback_bind_address("::1"), "IPv6 loopback address was rejected");
  expect(!mine_teleop::is_loopback_bind_address("0.0.0.0"), "wildcard bind was accepted for the control page");
  expect(!mine_teleop::is_loopback_bind_address("192.168.1.10"), "LAN bind was accepted for the control page");
  expect(
      mine_teleop::normalize_signaling_http_url("wss://signal.example.test/signaling") ==
          "https://signal.example.test",
      "WSS signaling origin did not map to its HTTPS API origin");

  mine_teleop::DriverConfig insecure;
  insecure.driver_id = "driver-console-001";
  insecure.signaling_url = "http://signal.example.test";
  bool insecure_rejected = false;
  try {
    mine_teleop::DriverConsoleRuntime runtime(insecure, "vehicle-001", "test-password");
    static_cast<void>(runtime.status());
  } catch (const std::invalid_argument& error) {
    insecure_rejected = std::string(error.what()).find("HTTPS or WSS") != std::string::npos;
  }
  expect(insecure_rejected, "public plaintext signaling URL was accepted");

  for (const std::string invalid_url : {"https://", "https://user@signal.example.test", "http://localhost.evil"}) {
    auto invalid = insecure;
    invalid.signaling_url = invalid_url;
    bool rejected = false;
    try {
      mine_teleop::DriverConsoleRuntime runtime(invalid, "vehicle-001", "test-password");
      static_cast<void>(runtime.status());
    } catch (const std::invalid_argument&) {
      rejected = true;
    }
    expect(rejected, "malformed or non-loopback signaling URL was accepted: " + invalid_url);
  }
}

void test_mac_runtime_uses_websocket_signaling() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "wss-runtime-vehicle"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/signaling";
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connected = driver.connect("vehicle-001");
  const auto session_id = connected.at("session_id").get<std::string>();
  const auto status = driver.status();
  expect(status.value("signaling_transport", "") == "websocket", "Mac runtime did not select websocket signaling");
  expect(status.value("signaling_websocket_connected", false), "Mac runtime websocket did not connect");

  auto offer = signaling_request(
      session_id,
      1,
      "vehicle-001",
      "driver-console-001",
      "device_token",
      "vehicle-secret-1",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0\r\n"}, {"media_tracks", mine_teleop::Json::array()}});
  offer["connection_generation"] = vehicle_generation;
  static_cast<void>(http.post_json_response(base + "/signaling/" + session_id + "/messages", offer));
  const auto pushed = driver.poll_signaling();
  expect(pushed.at("messages").size() == 1, "vehicle offer was not pushed over websocket");
  expect(
      pushed.at("messages").at(0).value("type", "") == "webrtc_offer",
      "websocket push delivered the wrong signaling type");

  const auto answer = driver.send_webrtc_answer({{"type", "answer"}, {"sdp", "v=0\r\n"}});
  expect(answer.value("transport", "") == "websocket", "Mac answer fell back to HTTP signaling");
  const auto vehicle_messages = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=vehicle-001&device_token=vehicle-secret-1"
      "&connection_generation=" + std::to_string(vehicle_generation));
  expect(vehicle_messages.at("messages").size() == 1, "websocket answer was not queued for the vehicle");
  expect(
      vehicle_messages.at("messages").at(0).value("type", "") == "webrtc_answer",
      "vehicle received the wrong websocket signaling message");

  std::atomic<int> concurrent_failures{0};
  std::vector<std::thread> candidate_senders;
  for (int index = 0; index < 8; ++index) {
    candidate_senders.emplace_back([&driver, &concurrent_failures, index] {
      try {
        static_cast<void>(driver.send_webrtc_ice_candidate(
            {{"candidate", "candidate:" + std::to_string(index + 1)}}));
      } catch (const std::exception&) {
        ++concurrent_failures;
      }
    });
  }
  for (auto& thread : candidate_senders) thread.join();
  expect(concurrent_failures.load() == 0, "concurrent browser ICE candidates failed WSS serialization");
  const auto candidates = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=vehicle-001&device_token=vehicle-secret-1"
      "&connection_generation=" + std::to_string(vehicle_generation) + "&types=ice_candidate");
  expect(candidates.at("messages").size() == 8, "concurrent browser ICE candidates were lost or duplicated");
  std::uint64_t previous_sequence = 1;
  for (const auto& candidate : candidates.at("messages")) {
    expect(candidate.value("seq", std::uint64_t{0}) == previous_sequence + 1, "WSS send sequence was reordered");
    previous_sequence = candidate.at("seq").get<std::uint64_t>();
  }

  static_cast<void>(driver.disconnect("websocket_runtime_test_complete"));
  server.stop();
}

void test_websocket_handshake_and_participant_isolation() {
  expect(
      mine_teleop::websocket_accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
          "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=",
      "RFC 6455 websocket accept vector changed");
  bool invalid_key_rejected = false;
  try {
    static_cast<void>(mine_teleop::websocket_accept_key("not-base64"));
  } catch (const std::invalid_argument&) {
    invalid_key_rejected = true;
  }
  expect(invalid_key_rejected, "invalid websocket key was accepted");

  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "wss-isolation-vehicle"}}));
  const auto login = http.post_json_response(
      base + "/auth/driver_login",
      {{"driver_id", "driver-console-001"}, {"password", "dev-password"}});
  const auto token = login.at("token").get<std::string>();
  const auto session = http.post_json_response(
      base + "/sessions",
      {{"driver_id", "driver-console-001"}, {"vehicle_id", "vehicle-001"}, {"token", token}});
  const auto session_id = session.at("session_id").get<std::string>();

  bool intruder_rejected = false;
  try {
    mine_teleop::WebSocketClient intruder;
    intruder.connect(
        mine_teleop::signaling_websocket_url(base, session_id, "intruder"),
        {{"X-Mine-Teleop-Driver-Token", token}});
  } catch (const std::runtime_error&) {
    intruder_rejected = true;
  }
  expect(intruder_rejected, "non-participant websocket upgrade was accepted");

  mine_teleop::WebSocketClient websocket;
  websocket.connect(
      mine_teleop::signaling_websocket_url(base, session_id, "driver-console-001"),
      {{"X-Mine-Teleop-Driver-Token", token}});
  websocket.send_json(signaling_request(
      session_id,
      1,
      "vehicle-001",
      "driver-console-001",
      "",
      "",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0\r\n"}}));
  const auto rejected = websocket.receive_json(std::chrono::seconds(1));
  expect(rejected.status == mine_teleop::WebSocketReceiveStatus::Message, "websocket rejection response is missing");
  expect(
      rejected.message.value("error", "").find("authenticated websocket participant") != std::string::npos,
      "websocket sender isolation did not reject participant spoofing");
  websocket.close();

  const auto websocket_target = "/signaling/" + session_id +
      "/ws?participant=driver-console-001";
  const auto websocket_auth_header = "X-Mine-Teleop-Driver-Token: " + token + "\r\n";
  expect(
      websocket_target.find(token) == std::string::npos,
      "websocket target leaked the driver token");
  expect_throws(
      [&] {
        mine_teleop::WebSocketClient invalid_headers;
        invalid_headers.connect(
            "ws://127.0.0.1:" + std::to_string(server.port()) + websocket_target,
            {{"X-Mine-Teleop-Driver-Token", token + "\r\nInjected: value"}});
      },
      "websocket header injection was accepted");
  const auto missing_upgrade = http.get(
      base + websocket_target,
      {{"X-Mine-Teleop-Driver-Token", token}});
  expect(missing_upgrade.status == 400, "plain GET was incorrectly upgraded to websocket");
  expect(
      missing_upgrade.body.find("WebSocket Upgrade header is required") != std::string::npos,
      "missing websocket Upgrade header produced the wrong error");
  const auto raw_missing_upgrade = raw_http_exchange(
      server.port(),
      "GET " + websocket_target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
          std::to_string(server.port()) +
          "\r\nConnection: close\r\nX-Request-ID: attacker-controlled-request-id\r\n" +
          websocket_auth_header + "\r\n");
  expect(raw_missing_upgrade.starts_with("HTTP/1.1 400 "), "raw websocket rejection returned the wrong status");
  expect(
      raw_missing_upgrade.find("\r\nX-Request-ID: request-") != std::string::npos,
      "websocket rejection omitted the server request ID");
  expect(
      raw_missing_upgrade.find("attacker-controlled-request-id") == std::string::npos,
      "websocket rejection trusted a client-supplied request ID");

  std::string raw_upgrade_headers;
  int raw_socket = raw_websocket_connect(
      server.port(),
      websocket_target,
      &raw_upgrade_headers,
      "X-Request-ID: attacker-controlled-request-id\r\n" + websocket_auth_header);
  expect(
      raw_upgrade_headers.find("\r\nX-Request-ID: request-") != std::string::npos,
      "successful websocket upgrade omitted the server request ID");
  expect(
      raw_upgrade_headers.find("attacker-controlled-request-id") == std::string::npos,
      "websocket upgrade trusted a client-supplied request ID");
  raw_send_all(raw_socket, std::string("\x81\x02{}", 4));
  auto protocol_error = raw_receive_websocket_json(raw_socket);
  expect(
      protocol_error.value("error", "") == "client websocket frames must be masked",
      "unmasked websocket frame was not rejected");
  ::close(raw_socket);

  raw_socket = raw_websocket_connect(server.port(), websocket_target, nullptr, websocket_auth_header);
  std::string fragmented;
  fragmented.push_back(static_cast<char>(0x01));
  fragmented.push_back(static_cast<char>(0x82));
  fragmented.append("test", 4);
  fragmented.push_back(static_cast<char>(static_cast<unsigned char>('{') ^ static_cast<unsigned char>('t')));
  fragmented.push_back(static_cast<char>(static_cast<unsigned char>('}') ^ static_cast<unsigned char>('e')));
  raw_send_all(raw_socket, fragmented);
  protocol_error = raw_receive_websocket_json(raw_socket);
  expect(
      protocol_error.value("error", "") == "fragmented websocket messages are not supported",
      "fragmented websocket frame was not rejected");
  ::close(raw_socket);

  raw_socket = raw_websocket_connect(server.port(), websocket_target, nullptr, websocket_auth_header);
  raw_send_all(raw_socket, std::string("\x88\xfe\x00\x7e", 4));
  protocol_error = raw_receive_websocket_json(raw_socket);
  expect(
      protocol_error.value("error", "").find("no longer than 125 bytes") != std::string::npos,
      "oversized websocket control frame was not rejected");
  ::close(raw_socket);

  const auto expect_protocol_error = [&](std::string_view frame, std::string_view expected, std::string_view message) {
    const int protocol_socket =
        raw_websocket_connect(server.port(), websocket_target, nullptr, websocket_auth_header);
    try {
      raw_send_all(protocol_socket, frame);
      const auto response = raw_receive_websocket_json(protocol_socket);
      expect(
          response.value("event", "") == "websocket_protocol_error" &&
              response.value("error", "").find(expected) != std::string::npos,
          message);
      ::close(protocol_socket);
    } catch (...) {
      ::close(protocol_socket);
      throw;
    }
  };

  expect_protocol_error(
      raw_masked_websocket_frame(0xc1U, "{}"),
      "reserved bits",
      "websocket reserved bits were not rejected");
  expect_protocol_error(
      raw_masked_websocket_frame(0x83U, ""),
      "unsupported websocket opcode",
      "reserved websocket opcode was not rejected before payload processing");
  expect_protocol_error(
      raw_masked_websocket_header(0x81U, 2, 2),
      "not minimally encoded",
      "non-minimal 16-bit websocket length was accepted");
  expect_protocol_error(
      raw_masked_websocket_header(0x81U, 126, 8),
      "not minimally encoded",
      "non-minimal 64-bit websocket length was accepted");
  expect_protocol_error(
      raw_masked_websocket_header(0x81U, 512U * 1024U + 1U, 8),
      "message too large",
      "oversized websocket message header was accepted");
  expect_protocol_error(
      raw_masked_websocket_frame(0x81U, "not-json"),
      "invalid websocket JSON",
      "malformed websocket JSON was accepted");
  expect_protocol_error(
      raw_masked_websocket_frame(0x81U, "[]"),
      "JSON object",
      "non-object websocket JSON was accepted");
  expect_protocol_error(
      raw_masked_websocket_frame(0x88U, std::string("\0", 1)),
      "close payload must be empty or include a status code",
      "one-byte websocket close payload was accepted");
  expect_protocol_error(
      raw_masked_websocket_frame(0x88U, std::string("\x03\xed", 2)),
      "invalid websocket close status code",
      "reserved websocket close status was accepted");
  expect_protocol_error(
      raw_masked_websocket_frame(0x88U, std::string("\x03\xe8\xff", 3)),
      "close reason must be valid UTF-8",
      "invalid UTF-8 websocket close reason was accepted");

  std::atomic<int> concurrent_protocol_failures{0};
  std::vector<std::thread> protocol_clients;
  for (int index = 0; index < 32; ++index) {
    protocol_clients.emplace_back([&, index] {
      int socket = -1;
      try {
        socket = raw_websocket_connect(
            server.port(),
            websocket_target,
            nullptr,
            websocket_auth_header);
        const auto ping_payload = "load-" + std::to_string(index);
        raw_send_all(socket, raw_masked_websocket_frame(0x89U, ping_payload));
        const auto pong = raw_receive_websocket_frame(socket);
        if (!pong.final || pong.opcode != 0xaU || pong.payload != ping_payload) {
          throw std::runtime_error("websocket ping payload was not echoed by pong");
        }
        const std::string close_payload("\x03\xe8", 2);
        raw_send_all(socket, raw_masked_websocket_frame(0x88U, close_payload));
        const auto close = raw_receive_websocket_frame(socket);
        if (!close.final || close.opcode != 0x8U || close.payload != close_payload) {
          throw std::runtime_error("websocket close handshake did not echo the valid status");
        }
      } catch (const std::exception&) {
        ++concurrent_protocol_failures;
      }
      if (socket >= 0) ::close(socket);
    });
  }
  for (auto& client : protocol_clients) client.join();
  expect(
      concurrent_protocol_failures.load() == 0,
      "concurrent websocket ping/close protocol load failed");

  static_cast<void>(http.post_json_response(
      base + "/auth/driver_logout",
      {{"driver_id", "driver-console-001"}, {"token", token}, {"reason", "isolation_test_complete"}}));
  server.stop();
}

void test_websocket_delivery_replay_and_idempotent_acknowledgement() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "wss-replay-vehicle"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
  const auto login = http.post_json_response(
      base + "/auth/driver_login",
      {{"driver_id", "driver-console-001"}, {"password", "dev-password"}});
  const auto token = login.at("token").get<std::string>();
  const auto session = http.post_json_response(
      base + "/sessions",
      {{"driver_id", "driver-console-001"}, {"vehicle_id", "vehicle-001"}, {"token", token}});
  const auto session_id = session.at("session_id").get<std::string>();
  auto offer = signaling_request(
      session_id,
      1,
      "vehicle-001",
      "driver-console-001",
      "device_token",
      "vehicle-secret-1",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0\r\n"}});
  offer["connection_generation"] = vehicle_generation;
  const auto offer_ack =
      http.post_json_response(base + "/signaling/" + session_id + "/messages", offer);
  const auto expected_cursor = offer_ack.at("delivery_cursor").get<std::uint64_t>();
  const auto websocket_url =
      mine_teleop::signaling_websocket_url(base, session_id, "driver-console-001");
  const mine_teleop::HttpHeaders websocket_headers{
      {"X-Mine-Teleop-Driver-Token", token}};
  expect(
      websocket_url.find(token) == std::string::npos,
      "generated websocket URL leaked the driver token");
  const auto receive_event = [](
                                 mine_teleop::WebSocketClient& websocket,
                                 std::string_view expected_event,
                                 std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto remaining = std::max(
          std::chrono::milliseconds(1),
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now()));
      auto received = websocket.receive_json(remaining);
      if (received.status != mine_teleop::WebSocketReceiveStatus::Message ||
          received.message.value("event", "") == expected_event ||
          received.message.value("event", "") != "signaling_messages") {
        return received;
      }
    }
    return mine_teleop::WebSocketReceiveResult{};
  };

  mine_teleop::WebSocketClient first;
  first.connect(websocket_url, websocket_headers);
  const auto first_delivery = first.receive_json(std::chrono::seconds(1));
  expect(
      first_delivery.status == mine_teleop::WebSocketReceiveStatus::Message &&
          first_delivery.message.value("event", "") == "signaling_messages" &&
          first_delivery.message.value("delivery_cursor", std::uint64_t{0}) == expected_cursor,
      "first websocket delivery did not expose its cursor");
  first.close();

  mine_teleop::WebSocketClient replay;
  replay.connect(websocket_url, websocket_headers);
  const auto replayed = replay.receive_json(std::chrono::seconds(1));
  expect(
      replayed.status == mine_teleop::WebSocketReceiveStatus::Message &&
          replayed.message.value("delivery_cursor", std::uint64_t{0}) == expected_cursor &&
          replayed.message.at("messages").size() == 1,
      "unacknowledged websocket message was not replayed after reconnect");
  replay.send_json({{"event", "signaling_delivery_ack"}, {"delivery_cursor", expected_cursor + 1}});
  const auto invalid_ack = replay.receive_json(std::chrono::seconds(1));
  expect(
      invalid_ack.status == mine_teleop::WebSocketReceiveStatus::Message &&
          invalid_ack.message.value("event", "") == "signaling_message_rejected",
      "delivery acknowledgement beyond the sent cursor was accepted");
  replay.send_json({{"event", "signaling_delivery_ack"}, {"delivery_cursor", expected_cursor}});
  const auto confirmed = replay.receive_json(std::chrono::seconds(1));
  expect(
      confirmed.status == mine_teleop::WebSocketReceiveStatus::Message &&
          confirmed.message.value("event", "") == "signaling_delivery_acknowledged" &&
          confirmed.message.value("acknowledged", 0) == 1,
      "valid websocket delivery acknowledgement was not confirmed");
  replay.close();

  mine_teleop::WebSocketClient after_ack;
  after_ack.connect(websocket_url, websocket_headers);
  expect(
      after_ack.receive_json(std::chrono::milliseconds(150)).status ==
          mine_teleop::WebSocketReceiveStatus::Timeout,
      "acknowledged websocket message was delivered again");
  auto capabilities = signaling_request(
      session_id,
      1,
      "driver-console-001",
      "vehicle-001",
      "",
      "",
      "media_capabilities",
      {{"codecs", {"h264"}}});
  after_ack.send_json(capabilities);
  const auto first_outbound_ack = receive_event(after_ack, "signaling_ack", std::chrono::seconds(2));
  expect(
      first_outbound_ack.status == mine_teleop::WebSocketReceiveStatus::Message &&
          first_outbound_ack.message.value("event", "") == "signaling_ack" &&
          !first_outbound_ack.message.value("duplicate", true),
      "first websocket send was not acknowledged as a new message");
  after_ack.close();

  mine_teleop::WebSocketClient retry;
  retry.connect(websocket_url, websocket_headers);
  retry.send_json(capabilities);
  const auto retry_ack = receive_event(retry, "signaling_ack", std::chrono::seconds(2));
  expect(
      retry_ack.status == mine_teleop::WebSocketReceiveStatus::Message &&
          retry_ack.message.value("duplicate", false) &&
          retry_ack.message.value("message_id", "") == first_outbound_ack.message.value("message_id", ""),
      "identical websocket resend did not receive the stable idempotent acknowledgement");
  retry.close();
  const auto vehicle_messages = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=vehicle-001&device_token=vehicle-secret-1"
      "&connection_generation=" + std::to_string(vehicle_generation));
  expect(vehicle_messages.at("messages").size() == 1, "idempotent websocket resend duplicated the queued message");

  static_cast<void>(http.post_json_response(
      base + "/auth/driver_logout",
      {{"driver_id", "driver-console-001"}, {"token", token}, {"reason", "replay_test_complete"}}));
  server.stop();
}

void test_mac_runtime_retries_uncertain_websocket_send_without_duplication() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  auto websocket_attempts = std::make_shared<std::atomic<int>>(0);
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling, websocket_attempts](int socket, const auto& request) {
        if (!request.path.starts_with("/signaling/") || !request.path.ends_with("/ws")) return false;
        const auto key = request.headers.find("sec-websocket-key");
        if (key == request.headers.end()) return false;
        raw_send_all(
            socket,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " +
                mine_teleop::websocket_accept_key(key->second) + "\r\n\r\n");
        mine_teleop::ServerWebSocketConnection connection(socket, 512 * 1024);
        const auto received = connection.receive_json(std::chrono::seconds(2));
        if (received.status != mine_teleop::WebSocketReceiveStatus::Message) return true;
        mine_teleop::HttpRequest post;
        post.method = "POST";
        post.path = request.path.substr(0, request.path.size() - 3) + "/messages";
        post.target = post.path;
        post.body = received.message.dump();
        const auto response = signaling->handle(post);
        if (response.status != 200) {
          connection.send_json({{"event", "signaling_message_rejected"}, {"error", response.body}});
          return true;
        }
        const auto attempt = websocket_attempts->fetch_add(1) + 1;
        if (attempt == 1) return true;
        connection.send_json(mine_teleop::Json::parse(response.body));
        return true;
      });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "uncertain-send-vehicle"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connected = driver.connect("vehicle-001");
  const auto session_id = connected.at("session_id").get<std::string>();
  const auto acknowledgement = driver.send_media_capabilities({{"codecs", {"h264"}}});
  expect(acknowledgement.value("duplicate", false), "uncertain websocket send was not retried idempotently");
  expect(websocket_attempts->load() == 2, "uncertain websocket send did not use exactly one reconnect retry");
  const auto status = driver.status();
  expect(status.value("signaling_websocket_reconnects", 0U) >= 1, "uncertain send reconnect was not counted");
  const auto vehicle_messages = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=vehicle-001&device_token=vehicle-secret-1"
      "&connection_generation=" + std::to_string(vehicle_generation));
  expect(vehicle_messages.at("messages").size() == 1, "uncertain websocket retry duplicated the message");
  static_cast<void>(driver.disconnect("uncertain_send_test_complete"));
  server.stop();
}

void test_expired_websocket_authority_clears_local_control() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  signaling_config.control_token_ttl_ms = 80;
  signaling_config.connection_reaper_interval_ms = 5;
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "expired-wss-authority-vehicle"}}));

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  static_cast<void>(driver.connect("vehicle-001"));
  std::this_thread::sleep_for(std::chrono::milliseconds(130));
  bool authority_loss_reported = false;
  try {
    static_cast<void>(driver.poll_signaling());
  } catch (const std::runtime_error& error) {
    authority_loss_reported = std::string(error.what()).find("authority is no longer valid") != std::string::npos;
  }
  expect(authority_loss_reported, "expired WSS control authority was not reported");
  const auto status = driver.status();
  expect(!status.value("connected", true), "expired WSS authority remained connected locally");
  expect(!status.value("signaling_websocket_connected", true), "expired WSS connection remained open locally");
  bool control_rejected = false;
  try {
    static_cast<void>(driver.send_control(
        {{"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 1.0}}));
  } catch (const std::runtime_error&) {
    control_rejected = true;
  }
  expect(control_rejected, "expired WSS authority could still prepare control commands");
  static_cast<void>(driver.disconnect("expired_authority_test_complete"));
  server.stop();
}

void test_websocket_reconnect_preserves_active_authority() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer initial_server(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  initial_server.start();
  const auto port = initial_server.port();
  const auto base = "http://127.0.0.1:" + std::to_string(port);
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "wss-reconnect-vehicle"}}));
  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto initial = driver.connect("vehicle-001");
  const auto session_id = initial.at("session_id").get<std::string>();

  initial_server.stop();
  bool outage_retained = false;
  try {
    static_cast<void>(driver.poll_signaling());
  } catch (const std::runtime_error& error) {
    outage_retained = std::string(error.what()).find("retained pending verification") != std::string::npos;
  }
  expect(outage_retained, "full proxy outage was not reported as a retained unverified session");
  auto status = driver.status();
  expect(status.value("connected", false), "temporary proxy outage discarded the local session");
  expect(status.value("session_id", "") == session_id, "temporary proxy outage changed the local session id");
  bool connect_outage_retained = false;
  try {
    static_cast<void>(driver.connect("vehicle-001"));
  } catch (const std::runtime_error& error) {
    connect_outage_retained = std::string(error.what()).find("local authority was retained") != std::string::npos;
  }
  expect(connect_outage_retained, "connect during proxy outage discarded or misreported local authority");
  mine_teleop::SimpleHttpServer api_only_server(
      "127.0.0.1", port, [signaling](const auto& request) { return signaling->handle(request); });
  api_only_server.start();
  bool reconnect_failed = false;
  try {
    static_cast<void>(driver.connect("vehicle-001"));
  } catch (const std::runtime_error&) {
    reconnect_failed = true;
  }
  expect(reconnect_failed, "missing websocket route did not fail reconnect");
  status = driver.status();
  expect(status.value("connected", false), "transient WSS failure discarded active local authority");
  expect(status.value("session_id", "") == session_id, "transient WSS failure changed the session id");
  expect(!status.value("signaling_websocket_connected", true), "failed WSS reconnect was reported connected");
  api_only_server.stop();

  mine_teleop::SimpleHttpServer recovered_server(
      "127.0.0.1",
      port,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  recovered_server.start();
  const auto recovered = driver.connect("vehicle-001");
  expect(recovered.value("session_id", "") == session_id, "WSS reconnect created a replacement session");
  status = driver.status();
  expect(status.value("signaling_websocket_connected", false), "WSS did not reconnect to the live session");
  expect(status.value("signaling_websocket_reconnects", 0U) >= 1, "WSS reconnect was not counted");
  static_cast<void>(driver.disconnect("wss_reconnect_test_complete"));
  recovered_server.stop();
}

void test_signaling_process_restart_requires_fresh_authority() {
  const auto audit_path = std::filesystem::temp_directory_path() /
      ("mine-teleop-signaling-restart-" + mine_teleop::random_token(6) + ".jsonl");
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {
      {"driver-console-001", "restart-driver-password"},
      {"restart-api-driver", "restart-api-password"}};
  config.device_tokens = {{"vehicle-001", "restart-device-token"}};
  config.driver_vehicle_permissions = {
      {"driver-console-001", {"vehicle-001"}},
      {"restart-api-driver", {"vehicle-001"}}};
  config.audit_log_path = audit_path.string();

  auto signaling = std::make_shared<mine_teleop::SignalingService>(config);
  auto server = std::make_unique<mine_teleop::SimpleHttpServer>(
      "127.0.0.1",
      0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server->start();
  const auto port = server->port();
  const auto base = "http://127.0.0.1:" + std::to_string(port);
  const auto first_instance_id = signaling->health().value("service_instance_id", "");
  mine_teleop::HttpClient http;
  const auto first_online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "restart-device-token"},
       {"connection_id", "restart-vehicle-before"}});
  const auto first_generation = first_online.at("connection_generation").get<std::uint64_t>();
  const auto auxiliary_login = http.post_json_response(
      base + "/auth/driver_login",
      {{"driver_id", "restart-api-driver"}, {"password", "restart-api-password"}});
  const auto old_api_token = auxiliary_login.at("token").get<std::string>();

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(
      driver_config,
      "vehicle-001",
      "restart-driver-password");
  const auto first_connection = driver.connect("vehicle-001");
  const auto old_session_id = first_connection.at("session_id").get<std::string>();
  const auto first_vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=restart-device-token&connection_generation=" +
      std::to_string(first_generation));
  const auto old_control_token = first_vehicle_session.at("control_token").get<std::string>();

  server->stop();
  server.reset();

  bool outage_retained = false;
  try {
    static_cast<void>(driver.poll_signaling());
  } catch (const std::runtime_error& error) {
    outage_retained = std::string(error.what()).find("retained pending verification") != std::string::npos;
  }
  expect(outage_retained, "signaling process outage was not reported before restart recovery");
  const auto outage_vehicles = driver.vehicles();
  expect(
      outage_vehicles.value("stale", false) && !outage_vehicles.value("signaling_available", true),
      "signaling outage did not return an explicit safe stale vehicle snapshot");
  expect(
      outage_vehicles.at("vehicles").size() == 1 &&
          !outage_vehicles.at("vehicles").front().value("online", true) &&
          !outage_vehicles.at("vehicles").front().value("controllable", true),
      "signaling outage stale vehicle snapshot remained online or controllable");
  expect(driver.status().value("connected", false), "outage prematurely discarded unverified local authority");
  expect(!driver.status().value("signaling_available", true), "outage remained signaling-available in local status");
  signaling.reset();

  signaling = std::make_shared<mine_teleop::SignalingService>(config);
  server = std::make_unique<mine_teleop::SimpleHttpServer>(
      "127.0.0.1",
      port,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server->start();
  const auto second_instance_id = signaling->health().value("service_instance_id", "");
  expect(
      !first_instance_id.empty() && !second_instance_id.empty() && first_instance_id != second_instance_id,
      "signaling process restart reused its service instance ID");
  const auto second_online = http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "restart-device-token"},
       {"connection_id", "restart-vehicle-after"}});
  const auto second_generation = second_online.at("connection_generation").get<std::uint64_t>();

  expect(
      http.get(
              base + "/drivers/restart-api-driver/vehicles?token=" + http.url_encode(old_api_token))
              .status == 401,
      "pre-restart driver token was accepted by the new signaling process");
  expect(
      http.get(
              base + "/sessions/" + old_session_id + "?actor=vehicle-001&device_token=restart-device-token"
              "&connection_generation=" + std::to_string(second_generation))
              .status == 404,
      "pre-restart session survived in the new signaling process");

  const auto recovered_authentication = driver.vehicles();
  expect(
      recovered_authentication.value("signaling_restart_recovered", false),
      "control runtime did not automatically recover authentication after signaling restart");
  expect(
      !recovered_authentication.value("control_authority_recovered", true),
      "control runtime automatically restored expired control authority after signaling restart");
  expect(
      recovered_authentication.value("previous_service_instance_id", "") == first_instance_id &&
          recovered_authentication.value("service_instance_id", "") == second_instance_id,
      "control runtime restart recovery did not correlate the old and new service instances");
  auto status = driver.status();
  expect(status.value("authenticated", false), "control runtime did not remain authenticated after recovery");
  expect(!status.value("connected", true), "control runtime retained its pre-restart session");
  expect(
      !status.value("signaling_websocket_connected", true),
      "control runtime retained its pre-restart websocket");
  expect(
      status.value("signaling_service_instance_id", "") == second_instance_id &&
          status.value("signaling_restart_recoveries", std::uint64_t{0}) == 1,
      "control runtime did not expose exactly one signaling restart recovery");
  expect(
      signaling->health().value("online_drivers", std::size_t{0}) == 1 &&
          signaling->health().value("active_sessions", std::size_t{1}) == 0,
      "automatic restart recovery created control authority instead of authentication only");
  expect_throws(
      [&] {
        static_cast<void>(driver.send_control(
            {{"gear", "N"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 1.0}}));
      },
      "control runtime prepared a command with pre-restart authority");

  const auto replacement_connection = driver.connect("vehicle-001");
  const auto replacement_session_id = replacement_connection.at("session_id").get<std::string>();
  const auto replacement_vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=restart-device-token&connection_generation=" +
      std::to_string(second_generation));
  const auto replacement_control_token = replacement_vehicle_session.at("control_token").get<std::string>();
  expect(
      replacement_control_token != old_control_token,
      "signaling restart reissued the pre-restart control token");

  mine_teleop::ControlCommand replay;
  replay.vehicle_id = "vehicle-001";
  replay.driver_id = "driver-console-001";
  replay.session_id = replacement_session_id;
  replay.seq = 1;
  replay.sent_at_utc_ms = mine_teleop::now_ms();
  replay.control_token = old_control_token;
  mine_teleop::ControlReceiver receiver(
      replay.vehicle_id,
      replay.driver_id,
      replay.session_id,
      200,
      mine_teleop::kProtocolVersion,
      true,
      replacement_control_token);
  const auto old_replay = receiver.accept(replay, mine_teleop::now_ms());
  expect(
      !old_replay.accepted && old_replay.reason == "control_token_invalid",
      "post-restart session accepted the pre-restart control token");
  replay.control_token = replacement_control_token;
  expect(receiver.accept(replay, mine_teleop::now_ms()).accepted, "post-restart control token was rejected");

  static_cast<void>(driver.disconnect("signaling_restart_test_complete"));
  server->stop();
  server.reset();
  signaling.reset();

  std::ifstream audit_input(audit_path);
  std::string line;
  std::unordered_set<std::string> service_instances;
  int startup_events = 0;
  while (std::getline(audit_input, line)) {
    expect(line.find(old_api_token) == std::string::npos, "restart audit leaked the old driver token");
    expect(line.find(old_control_token) == std::string::npos, "restart audit leaked the old control token");
    expect(
        line.find(replacement_control_token) == std::string::npos,
        "restart audit leaked the replacement control token");
    const auto record = mine_teleop::Json::parse(line);
    service_instances.insert(record.value("service_instance_id", ""));
    if (record.value("event", "") == "signaling_service_started") ++startup_events;
  }
  expect(startup_events == 2, "signaling restart audit did not contain exactly two startup events");
  expect(service_instances.size() == 2, "signaling restart audit did not distinguish both service instances");
  std::filesystem::remove(audit_path);
}

void test_external_https_wss_mac_runtime() {
  const auto* configured = std::getenv("MINE_TELEOP_TEST_WSS_URL");
  if (configured == nullptr || std::string_view(configured).empty()) {
    throw std::runtime_error("MINE_TELEOP_TEST_WSS_URL is required for the external WSS test");
  }
  const std::string websocket_origin(configured);
  const auto api_origin = mine_teleop::normalize_signaling_http_url(websocket_origin);
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(
      api_origin + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"},
       {"device_token", "dev-device-secret"},
       {"connection_id", "external-wss-mac-runtime"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = websocket_origin;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connected = driver.connect("vehicle-001");
  const auto session_id = connected.at("session_id").get<std::string>();
  auto offer = signaling_request(
      session_id,
      1,
      "vehicle-001",
      "driver-console-001",
      "device_token",
      "dev-device-secret",
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0\r\n"}, {"media_tracks", mine_teleop::Json::array()}});
  offer["connection_generation"] = vehicle_generation;
  static_cast<void>(http.post_json_response(api_origin + "/signaling/" + session_id + "/messages", offer));
  const auto pushed = driver.poll_signaling();
  expect(pushed.at("messages").size() == 1, "TLS proxy did not deliver the WSS offer");
  const auto answer = driver.send_webrtc_answer({{"type", "answer"}, {"sdp", "v=0\r\n"}});
  expect(answer.value("transport", "") == "websocket", "TLS run did not send the answer over WSS");
  const auto status = driver.status();
  expect(status.value("signaling_websocket_connected", false), "TLS run lost its WSS connection");
  static_cast<void>(driver.disconnect("external_wss_test_complete"));
}

void test_signaling_audit_rotation_and_service_start() {
  const auto root = std::filesystem::temp_directory_path() /
      ("mine-teleop-signaling-audit-test-" + mine_teleop::random_token(6));
  std::filesystem::create_directories(root);
  const auto audit_path = root / "signaling-audit.jsonl";

  mine_teleop::SignalingServerConfig config;
  config.audit_log_path = audit_path.string();
  config.audit_log_max_bytes = 1024;
  config.audit_log_files = 3;
  config.connection_reaper_interval_ms = 1;

  auto invalid = config;
  invalid.audit_log_max_bytes = 1023;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "undersized signaling audit limit was accepted");
  invalid = config;
  invalid.audit_log_files = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero signaling audit retention was accepted");
  invalid.audit_log_files = 21;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "excessive signaling audit retention was accepted");
  invalid = config;
  invalid.audit_log_rotation_interval_ms = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero signaling audit rotation interval was accepted");
  invalid = config;
  invalid.audit_log_retention_days = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero-day signaling audit retention was accepted");
  invalid.audit_log_retention_days = 366;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "excessive signaling audit retention was accepted");
  invalid = config;
  invalid.audit_log_path = (root / "missing" / "audit.jsonl").string();
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "missing signaling audit directory did not fail at startup");

  for (int index = 0; index < 24; ++index) {
    mine_teleop::SignalingService service(config);
  }
  mine_teleop::SignalingService live_service(config);
  const auto live_instance_id = live_service.health().value("service_instance_id", "");
  expect(
      live_instance_id.starts_with("service-") && live_instance_id.size() == 32,
      "live service instance ID is invalid");

  std::size_t retained_records = 0;
  std::unordered_set<std::string> retained_instance_ids;
  for (int index = 0; index < config.audit_log_files; ++index) {
    const auto path = index == 0
        ? audit_path
        : std::filesystem::path(audit_path.string() + "." + std::to_string(index));
    expect(std::filesystem::is_regular_file(path), "signaling audit rotation file is missing");
    expect(
        std::filesystem::file_size(path) <= static_cast<std::uint64_t>(config.audit_log_max_bytes),
        "signaling audit rotation exceeded its size limit");
    std::ifstream input(path);
    std::string line;
    while (std::getline(input, line)) {
      const auto record = mine_teleop::Json::parse(line);
      expect(record.value("event", "") == "signaling_service_started", "unexpected startup audit event");
      expect(record.value("sent_at_utc_ms", 0LL) > 0, "startup audit omitted UTC time");
      expect(!record.contains("request_id"), "background startup audit received a request ID");
      expect(record.at("details").value("runtime", "") == "cpp", "startup audit omitted runtime");
      const auto instance_id = record.value("service_instance_id", "");
      expect(
          instance_id.starts_with("service-") && instance_id.size() == 32,
          "startup audit service instance ID is invalid");
      retained_instance_ids.insert(instance_id);
      ++retained_records;
    }
  }
  expect(retained_records > 0, "signaling audit rotation retained no complete records");
  expect(retained_instance_ids.size() > 1, "service restarts reused one instance ID");
  expect(retained_instance_ids.contains(live_instance_id), "health and startup audit instance IDs differ");
  expect(
      !std::filesystem::exists(std::filesystem::path(audit_path.string() + ".3")),
      "signaling audit rotation exceeded its file-count limit");

  const auto hourly_path = root / "signaling-audit-hourly.jsonl";
  auto hourly_config = config;
  hourly_config.audit_log_path = hourly_path.string();
  hourly_config.audit_log_max_bytes = 64 * 1024;
  hourly_config.audit_log_files = 3;
  hourly_config.audit_log_rotation_interval_ms = 60 * 60 * 1000;
  hourly_config.audit_log_retention_days = 7;
  for (int index = 0; index < 16; ++index) {
    const auto driver_id = "concurrent-driver-" + std::to_string(index);
    hourly_config.driver_passwords[driver_id] = "concurrent-password-" + std::to_string(index);
    hourly_config.driver_vehicle_permissions[driver_id] = {"vehicle-001"};
  }
  auto audit_time = std::make_shared<std::atomic<std::int64_t>>(1767225600000LL);
  mine_teleop::SignalingService hourly_service(
      hourly_config,
      [audit_time] { return audit_time->load(); });

  std::vector<int> login_statuses(16, 0);
  std::vector<std::thread> concurrent_logins;
  concurrent_logins.reserve(login_statuses.size());
  for (std::size_t index = 0; index < login_statuses.size(); ++index) {
    concurrent_logins.emplace_back([&, index] {
      try {
        mine_teleop::HttpRequest request;
        request.method = "POST";
        request.target = "/auth/driver_login";
        request.path = request.target;
        request.body = mine_teleop::Json({
            {"driver_id", "concurrent-driver-" + std::to_string(index)},
            {"password", "concurrent-password-" + std::to_string(index)},
        }).dump();
        login_statuses[index] = hourly_service.handle(request).status;
      } catch (const std::exception&) {
        login_statuses[index] = -1;
      }
    });
  }
  for (auto& login : concurrent_logins) login.join();
  expect(
      std::all_of(login_statuses.begin(), login_statuses.end(), [](const auto status) {
        return status == 200;
      }),
      "concurrent signaling audit writes failed");

  audit_time->store(1767229200000LL);
  mine_teleop::HttpRequest online;
  online.method = "POST";
  online.target = "/vehicles/online";
  online.path = online.target;
  online.body = mine_teleop::Json({
      {"vehicle_id", "vehicle-001"},
      {"device_token", "dev-device-secret"},
      {"connection_id", "hourly-rotation-vehicle"},
  }).dump();
  expect(hourly_service.handle(online).status == 200, "hourly audit rotation trigger failed");

  const auto first_hour_archive =
      root / "signaling-audit-hourly.20260101T000000Z.part00.jsonl";
  expect(std::filesystem::is_regular_file(first_hour_archive), "first UTC-hour audit slice was not archived");
  std::ifstream first_hour_input(first_hour_archive);
  std::string first_hour_line;
  std::size_t concurrent_login_records = 0;
  while (std::getline(first_hour_input, first_hour_line)) {
    const auto record = mine_teleop::Json::parse(first_hour_line);
    if (record.value("event", "") == "driver_login") ++concurrent_login_records;
  }
  expect(
      concurrent_login_records == login_statuses.size(),
      "concurrent audit writes produced missing or malformed JSONL records");

  const auto expired_archive =
      root / "signaling-audit-hourly.20251231T000000Z.part00.jsonl";
  const auto retained_archive =
      root / "signaling-audit-hourly.20260103T000000Z.part00.jsonl";
  {
    std::ofstream expired(expired_archive);
    expired << "{}\n";
    std::ofstream retained(retained_archive);
    retained << "{}\n";
  }
  audit_time->store(1767916800000LL);
  online.body = mine_teleop::Json({
      {"vehicle_id", "vehicle-001"},
      {"device_token", "dev-device-secret"},
      {"connection_id", "seven-day-retention-vehicle"},
  }).dump();
  expect(hourly_service.handle(online).status == 200, "seven-day audit retention trigger failed");
  expect(!std::filesystem::exists(expired_archive), "expired audit slice survived seven-day retention");
  expect(std::filesystem::exists(retained_archive), "recent audit slice was removed before seven days");
  std::filesystem::remove_all(root);
}

void test_signaling_audit_redacts_authenticated_reports() {
  const auto audit_path = std::filesystem::temp_directory_path() /
      ("mine-teleop-authenticated-report-audit-" + mine_teleop::random_token(6) + ".jsonl");
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-audit", "driver-password-audit"}};
  config.device_tokens = {{"vehicle-audit", "device-token-audit-secret"}};
  config.driver_vehicle_permissions = {{"driver-audit", {"vehicle-audit"}}};
  config.audit_log_path = audit_path.string();
  mine_teleop::SignalingService service(config);

  const auto post = [&](std::string_view path, const mine_teleop::Json& body) {
    mine_teleop::HttpRequest request;
    request.method = "POST";
    request.target = std::string(path);
    request.path = request.target;
    request.body = body.dump();
    return service.handle(request);
  };
  const auto online = mine_teleop::Json::parse(post(
      "/vehicles/online",
      {{"vehicle_id", "vehicle-audit"},
       {"device_token", "device-token-audit-secret"},
       {"connection_id", "vehicle-audit-runtime"}}).body);
  const auto generation = online.at("connection_generation").get<std::uint64_t>();
  const auto login = mine_teleop::Json::parse(post(
      "/auth/driver_login",
      {{"driver_id", "driver-audit"}, {"password", "driver-password-audit"}}).body);
  const auto token = login.at("token").get<std::string>();
  const auto session = mine_teleop::Json::parse(post(
      "/sessions",
      {{"driver_id", "driver-audit"}, {"vehicle_id", "vehicle-audit"}, {"token", token}}).body);
  const auto session_id = session.at("session_id").get<std::string>();

  expect(
      post(
          "/sessions/" + session_id + "/diagnostics",
          {{"actor", "driver-audit"},
           {"token", token},
           {"component", "driver_console"},
           {"rtt_ms", 18},
           {"packet_loss_percent", 0.25},
           {"jitter_ms", 3},
           {"video_latency_ms", 42},
           {"control_rate_hz", 20.0},
           {"token_expires_at_utc_ms", 1234},
           {"nested", {{"turn_credential", "nested-turn-credential-secret"}}}})
              .status == 200,
      "authenticated diagnostics report was rejected");
  expect(
      post(
          "/sessions/" + session_id + "/diagnostics",
          {{"actor", "driver-audit"},
           {"token", token},
           {"component", "driver_console"},
           {"rtt_ms", true},
           {"packet_loss_percent", 0.0},
           {"jitter_ms", 0},
           {"video_latency_ms", 0},
           {"control_rate_hz", 20.0}})
              .status == 400,
      "boolean diagnostics RTT was accepted");
  const auto first_usage = post(
      "/sessions/" + session_id + "/turn_usage",
      {{"actor", "driver-audit"},
       {"token", token},
       {"sample_seq", 1},
       {"bytes_sent", 6400},
       {"bytes_received", 3200},
       {"duration_ms", 1000}});
  expect(first_usage.status == 200, "authenticated TURN usage report was rejected");
  const auto first_usage_body = mine_teleop::Json::parse(first_usage.body).at("turn_usage");
  expect(
      first_usage_body.value("relay_bytes_total", 0) == 9600 &&
          first_usage_body.value("sample_count", 0) == 1 &&
          first_usage_body.value("last_bitrate_kbps", 0.0) == 76.8,
      "first TURN usage sample was not aggregated correctly");
  const auto duplicate_usage = post(
      "/sessions/" + session_id + "/turn_usage",
      {{"actor", "driver-audit"},
       {"token", token},
       {"sample_seq", 1},
       {"bytes_sent", 6400},
       {"bytes_received", 3200},
       {"duration_ms", 1000}});
  const auto duplicate_usage_body = mine_teleop::Json::parse(duplicate_usage.body);
  expect(
      duplicate_usage.status == 200 && duplicate_usage_body.value("duplicate", false) &&
          duplicate_usage_body.at("turn_usage").value("relay_bytes_total", 0) == 9600,
      "retried TURN usage sample was counted twice");
  expect(
      post(
          "/sessions/" + session_id + "/turn_usage",
          {{"actor", "driver-audit"},
           {"token", token},
           {"sample_seq", 1},
           {"bytes_sent", 1},
           {"bytes_received", 2},
           {"duration_ms", 3}})
              .status == 409,
      "TURN usage sample sequence accepted different retry content");
  const auto second_usage = post(
      "/sessions/" + session_id + "/turn_usage",
      {{"actor", "driver-audit"},
       {"token", token},
       {"sample_seq", 2},
       {"bytes_sent", 400},
       {"bytes_received", 0},
       {"duration_ms", 100}});
  expect(
      second_usage.status == 200 &&
          mine_teleop::Json::parse(second_usage.body).at("turn_usage").value("relay_bytes_total", 0) == 10000,
      "second TURN usage sample did not update the session total");
  const auto turn_health = service.health();
  expect(
      turn_health.value("turn_usage_sessions", 0) == 1 &&
          turn_health.value("turn_relay_bytes_total", 0) == 10000,
      "health did not expose the bounded TURN usage aggregate");
  expect(
      post(
          "/sessions/" + session_id + "/turn_usage",
          {{"actor", "driver-audit"},
           {"token", token},
           {"sample_seq", 3},
           {"bytes_sent", "400"},
           {"bytes_received", 0},
           {"duration_ms", 100}})
              .status == 400,
      "string TURN byte counter was accepted");
  expect(
      post(
          "/sessions/" + session_id + "/turn_usage",
          {{"actor", "driver-audit"},
           {"token", token},
           {"sample_seq", 3},
           {"bytes_sent", 0},
           {"bytes_received", 0},
           {"duration_ms", 0}})
              .status == 400,
      "zero-duration TURN usage sample was accepted");
  expect(
      post(
          "/sessions/" + session_id + "/control_timeout",
          {{"actor", "driver-audit"},
           {"token", token},
           {"last_valid_control_at_utc_ms", 1000},
           {"braking_at_utc_ms", 1800},
           {"control_timeout_ms", 800}})
              .status == 401,
      "driver was allowed to impersonate a vehicle control-timeout report");
  expect(
      post(
          "/sessions/" + session_id + "/control_timeout",
          {{"actor", "vehicle-audit"},
           {"device_token", "device-token-audit-secret"},
           {"connection_generation", generation},
           {"last_valid_control_at_utc_ms", 1800},
           {"braking_at_utc_ms", 1000},
           {"control_timeout_ms", 800}})
              .status == 400,
      "control-timeout report accepted braking before the last valid command");
  expect(
      post(
          "/sessions/" + session_id + "/control_timeout",
          {{"actor", "vehicle-audit"},
           {"device_token", "device-token-audit-secret"},
           {"connection_generation", generation},
           {"last_valid_control_at_utc_ms", 1000},
           {"braking_at_utc_ms", 1800},
           {"control_timeout_ms", 800}})
              .status == 200,
      "authenticated control timeout report was rejected");
  expect(
      post(
          "/sessions/" + session_id + "/abnormal_disconnect",
          {{"actor", "driver-audit"},
           {"token", token},
           {"reason", "ice_failed"},
           {"detected_by", "driver_console"}})
              .status == 200,
      "authenticated abnormal disconnect report was rejected");
  expect(
      post(
          "/sessions/" + session_id + "/abnormal_disconnect",
          {{"actor", "driver-audit"}, {"token", token}, {"reason", true}, {"detected_by", "driver_console"}})
              .status == 400,
      "non-string abnormal disconnect reason was accepted");
  expect(
      post(
          "/sessions/" + session_id + "/estop",
          {{"actor", "driver-audit"}, {"token", token}, {"reason", "operator"}, {"control_seq", 0}})
              .status == 200,
      "authenticated ESTOP report was rejected");
  expect(
      post(
          "/sessions/" + session_id + "/turn_relay",
          {{"actor", "driver-audit"},
           {"token", token},
           {"turn_url", "turn:turn.example.test:3478?transport=udp"},
           {"relay_candidate", "relay 203.0.113.10:49152"},
           {"selected_pair", "relay-to-srflx"}})
              .status == 200,
      "authenticated TURN relay path report was rejected");

  std::ifstream input(audit_path);
  std::string line;
  bool diagnostics_found = false;
  bool turn_usage_found = false;
  bool control_timeout_found = false;
  bool abnormal_disconnect_found = false;
  bool estop_found = false;
  bool turn_relay_found = false;
  while (std::getline(input, line)) {
    expect(line.find(token) == std::string::npos, "signaling audit leaked a driver token");
    expect(
        line.find("device-token-audit-secret") == std::string::npos,
        "signaling audit leaked a device token");
    expect(
        line.find("nested-turn-credential-secret") == std::string::npos,
        "signaling audit leaked a nested TURN credential");
    const auto record = mine_teleop::Json::parse(line);
    const auto event = record.value("event", "");
    if (event == "realtime_diagnostics") {
      diagnostics_found = true;
      expect(
          !record.at("details").contains("token") && !record.at("details").contains("nested"),
          "diagnostics audit retained non-whitelisted credential-bearing fields");
    } else if (event == "turn_relay_usage") {
      turn_usage_found = true;
      expect(!record.at("details").contains("token"), "TURN usage audit retained its token field");
    } else if (event == "control_timeout") {
      control_timeout_found = true;
      expect(
          !record.at("details").contains("device_token"),
          "control timeout audit retained its device token field");
    } else if (event == "abnormal_disconnect") {
      abnormal_disconnect_found = true;
    } else if (event == "estop") {
      estop_found = true;
    } else if (event == "turn_relay_enabled") {
      turn_relay_found = true;
    }
  }
  expect(
      diagnostics_found && turn_usage_found && control_timeout_found && abnormal_disconnect_found && estop_found &&
          turn_relay_found,
      "authenticated report audits are incomplete");
  std::filesystem::remove(audit_path);
}

void test_driver_webrtc_connection_audit_transitions() {
  const auto audit_path = std::filesystem::temp_directory_path() /
      ("mine-teleop-webrtc-connection-audit-" + mine_teleop::random_token(6) + ".jsonl");
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-webrtc", "driver-webrtc-password"}};
  config.device_tokens = {{"vehicle-webrtc", "vehicle-webrtc-token"}};
  config.driver_vehicle_permissions = {{"driver-webrtc", {"vehicle-webrtc"}}};
  config.audit_log_path = audit_path.string();
  auto service = std::make_shared<mine_teleop::SignalingService>(config);
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [service](const auto& request) { return service->handle(request); },
      8 * 1024 * 1024,
      [service](int socket, const auto& request) { return service->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(
      base + "/vehicles/online",
      {{"vehicle_id", "vehicle-webrtc"},
       {"device_token", "vehicle-webrtc-token"},
       {"connection_id", "vehicle-webrtc-runtime"}}));

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-webrtc";
  driver_config.signaling_url = base;
  driver_config.max_time_sync_uncertainty_ms = 25;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-webrtc", "driver-webrtc-password");
  static_cast<void>(driver.connect("vehicle-webrtc"));
  const mine_teleop::Json connected = {
      {"connection_state", "connected"},
      {"connection_method", "direct"},
      {"turn_in_use", false},
      {"time_sync", {{"synchronized", true}, {"uncertainty_ms", 2}}},
      {"streams", mine_teleop::Json::array()}};
  const auto first = driver.ingest_webrtc_metrics(connected);
  expect(
      first.value("reported", false) && first.value("audit_event", "") == "webrtc_connection_succeeded",
      "connected WebRTC transition was not audited");
  const auto duplicate = driver.ingest_webrtc_metrics(connected);
  expect(!duplicate.value("reported", true), "unchanged WebRTC state produced a duplicate audit");

  const auto failed = driver.ingest_webrtc_metrics(
      {{"connection_state", "failed"},
       {"connection_method", "unknown"},
       {"turn_in_use", false},
       {"time_sync", {{"synchronized", false}, {"uncertainty_ms", 100}}},
       {"streams", mine_teleop::Json::array()}});
  expect(
      failed.value("reported", false) && failed.value("audit_event", "") == "webrtc_connection_failed",
      "failed WebRTC transition was not audited");
  expect_throws(
      [&] {
        static_cast<void>(driver.ingest_webrtc_metrics(
            {{"connection_state", "connected"},
             {"connection_method", "TURN"},
             {"turn_in_use", false},
             {"time_sync", {{"synchronized", true}, {"uncertainty_ms", 1}}}}));
      },
      "inconsistent TURN metrics were accepted");
  static_cast<void>(driver.disconnect("webrtc_audit_test_complete"));
  server.stop();

  std::ifstream input(audit_path);
  std::string line;
  int succeeded_events = 0;
  int failed_events = 0;
  int time_anomalies = 0;
  while (std::getline(input, line)) {
    expect(line.find("driver-token-") == std::string::npos, "WebRTC audit leaked a driver token");
    const auto record = mine_teleop::Json::parse(line);
    const auto event = record.value("event", "");
    if (event == "webrtc_connection_succeeded") {
      ++succeeded_events;
      expect(
          record.at("details").value("connection_method", "") == "direct" &&
              !record.at("details").value("turn_in_use", true),
          "successful WebRTC audit omitted its direct path");
    } else if (event == "webrtc_connection_failed") {
      ++failed_events;
    } else if (event == "time_sync_anomaly") {
      ++time_anomalies;
      expect(
          !record.at("details").value("time_sync_acceptable", true),
          "time synchronization anomaly was marked acceptable");
    }
  }
  expect(
      succeeded_events == 1 && failed_events == 1 && time_anomalies == 1,
      "WebRTC connection audit transition counts are incorrect");
  std::filesystem::remove(audit_path);
}

void test_driver_login_failure_rate_limit_and_recovery() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-1", "correct-password"}};
  config.device_tokens = {{"vehicle-1", "device-token"}};
  config.driver_vehicle_permissions = {{"driver-1", {"vehicle-1"}}};
  config.login_max_failures = 3;
  config.login_failure_window_ms = 1000;
  config.login_lockout_ms = 25;
  const auto audit_path = std::filesystem::path("/tmp") /
      ("mine-teleop-login-rate-limit-" + mine_teleop::random_token(6) + ".jsonl");
  config.audit_log_path = audit_path.string();

  auto invalid = config;
  invalid.login_max_failures = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero login failure limit was accepted");
  invalid = config;
  invalid.login_failure_window_ms = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero login failure window was accepted");
  invalid = config;
  invalid.login_lockout_ms = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero login lockout was accepted");

  mine_teleop::SignalingService service(config);
  const auto login = [&](std::string_view driver_id, std::string_view password) {
    mine_teleop::HttpRequest request;
    request.method = "POST";
    request.target = "/auth/driver_login";
    request.path = request.target;
    request.body = mine_teleop::Json({
        {"driver_id", std::string(driver_id)},
        {"password", std::string(password)},
    }).dump();
    return service.handle(request);
  };

  expect(login("driver-1", "wrong-1").status == 401, "first failed login was not rejected");
  expect(login("driver-1", "wrong-2").status == 401, "second failed login was not rejected");
  const auto locked = login("driver-1", "wrong-3");
  expect(locked.status == 429, "failure threshold did not lock the driver login");
  const auto retry_after = std::find_if(locked.headers.begin(), locked.headers.end(), [](const auto& header) {
    return header.first == "Retry-After";
  });
  expect(retry_after != locked.headers.end() && retry_after->second == "1", "429 response omitted Retry-After");
  const auto cache_control = std::find_if(locked.headers.begin(), locked.headers.end(), [](const auto& header) {
    return header.first == "Cache-Control";
  });
  expect(
      cache_control != locked.headers.end() && cache_control->second == "no-store",
      "login response was cacheable");
  const auto locked_body = mine_teleop::Json::parse(locked.body);
  expect(locked_body.value("retry_after_ms", 0) > 0, "429 response omitted retry_after_ms");
  expect(login("driver-1", "correct-password").status == 429, "valid password bypassed active lockout");
  const auto locked_health = service.health();
  expect(locked_health.value("status", "") == "degraded", "active login lockout did not degrade health");
  expect(locked_health.value("login_locked_buckets", 0) == 1, "active login lockout count is inaccurate");
  expect(
      std::any_of(locked_health.at("alerts").begin(), locked_health.at("alerts").end(), [](const auto& alert) {
        return alert.value("code", "") == "login_lockout_active" &&
            alert.value("severity", "") == "warning" && alert.value("count", 0) == 1;
      }),
      "active login lockout alert is missing");

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  const auto recovered = login("driver-1", "correct-password");
  expect(recovered.status == 200, "login did not recover after lockout expiry");
  expect(
      std::find_if(recovered.headers.begin(), recovered.headers.end(), [](const auto& header) {
        return header.first == "Cache-Control" && header.second == "no-store";
      }) != recovered.headers.end(),
      "successful login token response was cacheable");
  const auto recovered_health = service.health();
  expect(
      recovered_health.value("status", "") == "ok" &&
          recovered_health.value("login_locked_buckets", 1) == 0 && recovered_health.at("alerts").empty(),
      "expired login lockout alert did not clear");

  expect(login("random-account-a", "wrong-a").status == 401, "first unknown account attempt was not rejected");
  expect(login("random-account-b", "wrong-b").status == 401, "second unknown account attempt was not rejected");
  expect(login("random-account-c", "wrong-c").status == 429, "unknown account attempts did not share one bounded bucket");

  auto concurrent_config = config;
  concurrent_config.audit_log_path.clear();
  concurrent_config.login_max_failures = 4;
  concurrent_config.login_lockout_ms = 1000;
  mine_teleop::SignalingService concurrent_service(concurrent_config);
  std::vector<int> concurrent_statuses(12, 0);
  std::vector<std::thread> attempts;
  attempts.reserve(concurrent_statuses.size());
  for (std::size_t index = 0; index < concurrent_statuses.size(); ++index) {
    attempts.emplace_back([&, index] {
      mine_teleop::HttpRequest request;
      request.method = "POST";
      request.target = "/auth/driver_login";
      request.path = request.target;
      request.body = mine_teleop::Json({
          {"driver_id", "parallel-unknown-" + std::to_string(index)},
          {"password", "wrong"},
      }).dump();
      concurrent_statuses[index] = concurrent_service.handle(request).status;
    });
  }
  for (auto& attempt : attempts) attempt.join();
  expect(
      std::count(concurrent_statuses.begin(), concurrent_statuses.end(), 401) == 3,
      "concurrent failure counter allowed the wrong number of pre-lockout attempts");
  expect(
      std::count(concurrent_statuses.begin(), concurrent_statuses.end(), 429) == 9,
      "concurrent failure counter did not consistently enforce lockout");

  auto unavailable_audit_config = config;
  unavailable_audit_config.login_max_failures = 1;
  unavailable_audit_config.login_lockout_ms = 1000;
  const auto unavailable_audit_root = std::filesystem::path("/tmp") /
      ("mine-teleop-unavailable-audit-" + mine_teleop::random_token(6));
  const auto unavailable_audit_saved = std::filesystem::path(unavailable_audit_root.string() + ".saved");
  std::filesystem::create_directories(unavailable_audit_root);
  unavailable_audit_config.audit_log_path = (unavailable_audit_root / "audit.jsonl").string();
  mine_teleop::SignalingService unavailable_audit_service(unavailable_audit_config);
  std::filesystem::rename(unavailable_audit_root, unavailable_audit_saved);
  const auto unavailable_audit_login = [&](std::string_view password) {
    mine_teleop::HttpRequest request;
    request.method = "POST";
    request.target = "/auth/driver_login";
    request.path = request.target;
    request.body = mine_teleop::Json({
        {"driver_id", "driver-1"},
        {"password", std::string(password)},
    }).dump();
    return unavailable_audit_service.handle(request);
  };
  expect_throws(
      [&] { static_cast<void>(unavailable_audit_login("wrong")); },
      "unavailable audit sink did not fail closed");
  expect(
      unavailable_audit_login("correct-password").status == 429,
      "audit sink failure bypassed the established login lockout");
  std::filesystem::remove_all(unavailable_audit_saved);

  std::ifstream audit_input(audit_path);
  const std::string audit_log((std::istreambuf_iterator<char>(audit_input)), std::istreambuf_iterator<char>());
  expect(audit_log.find("\"event\":\"driver_login_failed\"") != std::string::npos, "failed login was not audited");
  expect(
      audit_log.find("\"event\":\"driver_login_rate_limited\"") != std::string::npos,
      "login lockout was not audited");
  expect(audit_log.find("wrong-1") == std::string::npos, "audit log leaked a rejected password");
  expect(audit_log.find("random-account-a") == std::string::npos, "audit log retained attacker-controlled unknown IDs");
  std::filesystem::remove(audit_path);
}

void test_request_correlation_ids() {
  mine_teleop::SignalingServerConfig config;
  const auto audit_path = std::filesystem::path("/tmp") /
      ("mine-teleop-request-correlation-" + mine_teleop::random_token(6) + ".jsonl");
  config.audit_log_path = audit_path.string();
  mine_teleop::SignalingService service(config);

  mine_teleop::HttpRequest rejected_login;
  rejected_login.method = "POST";
  rejected_login.target = "/auth/driver_login";
  rejected_login.path = rejected_login.target;
  rejected_login.peer_address = "198.51.100.150";
  rejected_login.headers["x-request-id"] = "attacker-controlled-request-id";
  rejected_login.body = mine_teleop::Json({
      {"driver_id", "driver-console-001"},
      {"password", "wrong-password"},
  }).dump();
  const auto rejected = service.handle(rejected_login);
  expect(rejected.status == 401, "request-correlation login probe was not rejected");
  const auto request_id = response_header(rejected, "X-Request-ID");
  expect(request_id.starts_with("request-") && request_id.size() == 32, "HTTP response has an invalid request ID");
  expect(request_id != "attacker-controlled-request-id", "HTTP response trusted a client request ID");

  std::ifstream audit_input(audit_path);
  std::string audit_line;
  mine_teleop::Json audit_record;
  while (std::getline(audit_input, audit_line)) {
    const auto candidate = mine_teleop::Json::parse(audit_line);
    if (candidate.value("event", "") == "driver_login_failed") {
      audit_record = candidate;
      break;
    }
  }
  expect(audit_record.is_object(), "request-correlated audit record is missing");
  expect(audit_record.value("request_id", "") == request_id, "HTTP response and audit request IDs differ");
  expect(
      audit_record.value("service_instance_id", "") == service.health().value("service_instance_id", ""),
      "request audit and health service instance IDs differ");
  expect(audit_line.find("attacker-controlled-request-id") == std::string::npos, "audit trusted a client request ID");

  mine_teleop::HttpRequest missing;
  missing.method = "GET";
  missing.target = "/missing";
  missing.path = missing.target;
  missing.peer_address = "198.51.100.150";
  const auto not_found = service.handle(missing);
  expect(not_found.status == 404, "request-correlation missing route did not return 404");
  const auto missing_request_id = response_header(not_found, "X-Request-ID");
  expect(
      missing_request_id.starts_with("request-") && missing_request_id != request_id,
      "separate HTTP requests reused a request ID");

  std::vector<std::string> concurrent_ids(16);
  std::vector<int> concurrent_statuses(16, 0);
  std::vector<std::thread> requests;
  requests.reserve(concurrent_ids.size());
  for (std::size_t index = 0; index < concurrent_ids.size(); ++index) {
    requests.emplace_back([&, index] {
      mine_teleop::HttpRequest request;
      request.method = "GET";
      request.target = "/health";
      request.path = request.target;
      request.peer_address = "198.51.100.151";
      request.headers["x-request-id"] = "forged-" + std::to_string(index);
      const auto response = service.handle(request);
      concurrent_statuses[index] = response.status;
      concurrent_ids[index] = response_header(response, "X-Request-ID");
    });
  }
  for (auto& request : requests) request.join();
  expect(
      std::all_of(concurrent_statuses.begin(), concurrent_statuses.end(), [](const auto status) {
        return status == 200;
      }),
      "concurrent request-correlation health check failed");
  const std::unordered_set<std::string> unique_ids(concurrent_ids.begin(), concurrent_ids.end());
  expect(unique_ids.size() == concurrent_ids.size(), "concurrent HTTP requests shared a request ID");
  expect(
      std::all_of(concurrent_ids.begin(), concurrent_ids.end(), [](const auto& id) {
        return id.starts_with("request-") && id.size() == 32;
      }),
      "a concurrent HTTP response omitted its server request ID");
  std::filesystem::remove(audit_path);
}

void test_source_aware_api_and_websocket_rate_limit() {
  mine_teleop::SignalingServerConfig config;
  config.api_rate_limit_requests = 2;
  config.api_rate_limit_window_ms = 25;
  config.api_rate_limit_max_sources = 8;

  auto invalid = config;
  invalid.api_rate_limit_requests = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero API request limit was accepted");
  invalid = config;
  invalid.api_rate_limit_window_ms = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero API rate-limit window was accepted");
  invalid = config;
  invalid.api_rate_limit_max_sources = 0;
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "zero API source capacity was accepted");
  invalid = config;
  invalid.trusted_proxy_addresses = {"not-an-ip-address"};
  expect_throws(
      [&] { mine_teleop::SignalingService service(invalid); },
      "invalid trusted proxy address was accepted");

  auto maximum_window_config = config;
  maximum_window_config.api_rate_limit_requests = 1;
  maximum_window_config.api_rate_limit_window_ms = std::numeric_limits<std::int64_t>::max();
  mine_teleop::SignalingService maximum_window_service(maximum_window_config);
  mine_teleop::HttpRequest maximum_window_request;
  maximum_window_request.method = "GET";
  maximum_window_request.target = "/health";
  maximum_window_request.path = maximum_window_request.target;
  maximum_window_request.peer_address = "198.51.100.1";
  expect(maximum_window_service.handle(maximum_window_request).status == 200, "maximum API window rejected early");
  const auto maximum_window_limited = maximum_window_service.handle(maximum_window_request);
  expect(maximum_window_limited.status == 429, "maximum API window did not enforce its request limit");
  expect(
      std::any_of(maximum_window_limited.headers.begin(), maximum_window_limited.headers.end(), [](const auto& header) {
        return header.first == "Retry-After" && !header.second.empty() && header.second.front() != '-';
      }),
      "maximum API window overflowed Retry-After");

  mine_teleop::SignalingService service(config);
  const auto get = [&](std::string peer, std::string forwarded = {}) {
    mine_teleop::HttpRequest request;
    request.method = "GET";
    request.target = "/health";
    request.path = request.target;
    request.peer_address = std::move(peer);
    if (!forwarded.empty()) request.headers["x-forwarded-for"] = std::move(forwarded);
    return service.handle(request);
  };

  expect(get("127.0.0.1", "198.51.100.10").status == 200, "trusted proxy source was rejected early");
  expect(get("127.0.0.1", "198.51.100.10").status == 200, "trusted proxy source lost its quota");
  const auto limited = get("127.0.0.1", "198.51.100.10");
  expect(limited.status == 429, "trusted proxy source did not receive HTTP 429");
  expect(
      std::find(limited.headers.begin(), limited.headers.end(), std::pair<std::string, std::string>{"Cache-Control", "no-store"}) !=
          limited.headers.end(),
      "API 429 response was cacheable");
  expect(
      std::any_of(limited.headers.begin(), limited.headers.end(), [](const auto& header) {
        return header.first == "Retry-After" && header.second == "1";
      }),
      "API 429 response omitted Retry-After");
  expect(
      get("127.0.0.1", "198.51.100.11").status == 200,
      "independent forwarded source shared another source's quota");

  expect(get("203.0.113.20", "198.51.100.20").status == 200, "untrusted direct peer was rejected early");
  expect(get("203.0.113.20", "198.51.100.21").status == 200, "spoofed forwarding header changed direct quota");
  expect(
      get("203.0.113.20", "198.51.100.22").status == 429,
      "untrusted peer bypassed its quota by changing X-Forwarded-For");
  expect(
      get("127.0.0.1", "malformed-forwarded-source").status == 200,
      "malformed forwarding header did not fall back to the trusted peer");

  std::this_thread::sleep_for(std::chrono::milliseconds(40));
  expect(
      get("127.0.0.1", "198.51.100.10").status == 200,
      "API source quota did not recover after the fixed window expired");

  auto capacity_config = config;
  capacity_config.api_rate_limit_requests = 1;
  capacity_config.api_rate_limit_window_ms = 200;
  capacity_config.api_rate_limit_max_sources = 2;
  mine_teleop::SignalingService capacity_service(capacity_config);
  const auto capacity_get = [&](std::string source) {
    mine_teleop::HttpRequest request;
    request.method = "GET";
    request.target = "/health";
    request.path = request.target;
    request.peer_address = "127.0.0.1";
    request.headers["x-forwarded-for"] = std::move(source);
    return capacity_service.handle(request);
  };
  const auto initial_capacity_health = capacity_service.health();
  expect(
      initial_capacity_health.value("status", "") == "ok" && initial_capacity_health.at("alerts").empty(),
      "healthy source limiter reported an alert");
  expect(capacity_get("198.51.100.30").status == 200, "first bounded source failed");
  expect(capacity_get("198.51.100.31").status == 200, "second bounded source failed");
  expect(capacity_get("198.51.100.32").status == 200, "overflow bucket rejected its first request");
  expect(capacity_get("198.51.100.33").status == 429, "new sources did not share the bounded overflow bucket");
  const auto bounded_health = capacity_service.health();
  expect(
      bounded_health.value("api_rate_limit_tracked_sources", std::size_t{0}) == 2,
      "API source table exceeded its configured capacity");
  expect(
      bounded_health.value("api_rate_limit_overflow_active", false),
      "API overflow bucket was not reported active");
  expect(
      bounded_health.value("api_rate_limited_requests", std::uint64_t{0}) == 1,
      "API limited-request aggregate is inaccurate");
  expect(bounded_health.value("status", "") == "degraded", "source-capacity overflow did not degrade health");
  expect(
      std::any_of(bounded_health.at("alerts").begin(), bounded_health.at("alerts").end(), [](const auto& alert) {
        return alert.value("code", "") == "api_rate_limit_source_capacity" &&
            alert.value("severity", "") == "warning";
      }),
      "source-capacity health alert is missing");
  std::this_thread::sleep_for(std::chrono::milliseconds(225));
  const auto recovered_capacity_health = capacity_service.health();
  expect(
      recovered_capacity_health.value("status", "") == "ok" &&
          !recovered_capacity_health.value("api_rate_limit_overflow_active", true) &&
          recovered_capacity_health.at("alerts").empty(),
      "expired source-capacity alert did not clear");

  auto concurrent_config = config;
  concurrent_config.api_rate_limit_requests = 4;
  concurrent_config.api_rate_limit_window_ms = 1000;
  concurrent_config.api_rate_limit_max_sources = 4;
  mine_teleop::SignalingService concurrent_service(concurrent_config);
  std::vector<int> statuses(12, 0);
  std::vector<std::thread> requests;
  requests.reserve(statuses.size());
  for (std::size_t index = 0; index < statuses.size(); ++index) {
    requests.emplace_back([&, index] {
      mine_teleop::HttpRequest request;
      request.method = "GET";
      request.target = "/health";
      request.path = request.target;
      request.peer_address = "203.0.113.40";
      request.headers["x-forwarded-for"] = "198.51.100." + std::to_string(index + 40);
      statuses[index] = concurrent_service.handle(request).status;
    });
  }
  for (auto& request : requests) request.join();
  expect(std::count(statuses.begin(), statuses.end(), 200) == 4, "concurrent API quota allowed the wrong count");
  expect(std::count(statuses.begin(), statuses.end(), 429) == 8, "concurrent API quota was not enforced");

  auto websocket_config = config;
  websocket_config.api_rate_limit_requests = 1;
  websocket_config.api_rate_limit_window_ms = 1000;
  auto websocket_service = std::make_shared<mine_teleop::SignalingService>(websocket_config);
  mine_teleop::SimpleHttpServer websocket_server(
      "127.0.0.1",
      0,
      [websocket_service](const auto& request) { return websocket_service->handle(request); },
      8 * 1024 * 1024,
      [websocket_service](int socket, const auto& request) {
        return websocket_service->handle_websocket(socket, request);
      });
  websocket_server.start();
  mine_teleop::HttpClient http;
  expect(
      http.get_json("http://127.0.0.1:" + std::to_string(websocket_server.port()) + "/health")
              .value("status", "") == "ok",
      "HTTP request did not consume the shared source quota");
  const auto websocket_response = raw_http_exchange(
      websocket_server.port(),
      "GET /signaling/not-a-session/ws?participant=driver-console-001 HTTP/1.1\r\nHost: 127.0.0.1:" +
          std::to_string(websocket_server.port()) +
          "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
          "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
  websocket_server.stop();
  expect(websocket_response.starts_with("HTTP/1.1 429 "), "WSS Upgrade did not share the HTTP source quota");
  expect(websocket_response.find("\r\nRetry-After: 1\r\n") != std::string::npos, "WSS 429 omitted Retry-After");
  expect(
      websocket_response.find("\r\nCache-Control: no-store\r\n") != std::string::npos,
      "WSS 429 response was cacheable");
  expect(
      websocket_response.find("\r\nX-Request-ID: request-") != std::string::npos,
      "WSS 429 omitted the server request ID");
}

}  // namespace

int main() {
  std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"shared_control_protocol_vector", test_shared_control_protocol_vector},
      {"loopback_http_server_and_port_conflict", test_loopback_http_server_and_port_conflict},
      {"signaling_time_sync_applies_backward_utc_correction",
       test_signaling_time_sync_applies_backward_utc_correction},
      {"control_page_contract", test_control_page_contract},
      {"driver_gamepad_config", test_driver_gamepad_config},
      {"signaling_multi_identity_config", test_signaling_multi_identity_config},
      {"credential_purpose_separation_and_stale_control_replay",
       test_credential_purpose_separation_and_stale_control_replay},
      {"browser_event_logging_rotation_and_redaction", test_browser_event_logging_rotation_and_redaction},
      {"driver_vehicle_switch_releases_old_session", test_driver_vehicle_switch_releases_old_session},
      {"two_driver_two_vehicle_wss_isolation_and_safe_rejection",
       test_two_driver_two_vehicle_wss_isolation_and_safe_rejection},
      {"failed_logout_keeps_retryable_local_authority", test_failed_logout_keeps_retryable_local_authority},
      {"local_proxy_preserves_upstream_auth_status", test_local_proxy_preserves_upstream_auth_status},
      {"control_authority_lease_renews_without_rotating_token",
       test_control_authority_lease_renews_without_rotating_token},
      {"control_transport_and_loopback_policy", test_control_transport_and_loopback_policy},
      {"mac_runtime_uses_websocket_signaling", test_mac_runtime_uses_websocket_signaling},
      {"websocket_handshake_and_participant_isolation", test_websocket_handshake_and_participant_isolation},
      {"websocket_delivery_replay_and_idempotent_acknowledgement",
       test_websocket_delivery_replay_and_idempotent_acknowledgement},
      {"mac_runtime_retries_uncertain_websocket_send_without_duplication",
       test_mac_runtime_retries_uncertain_websocket_send_without_duplication},
      {"expired_websocket_authority_clears_local_control", test_expired_websocket_authority_clears_local_control},
      {"websocket_reconnect_preserves_active_authority", test_websocket_reconnect_preserves_active_authority},
      {"signaling_process_restart_requires_fresh_authority",
       test_signaling_process_restart_requires_fresh_authority},
      {"signaling_audit_rotation_and_service_start", test_signaling_audit_rotation_and_service_start},
      {"signaling_audit_redacts_authenticated_reports", test_signaling_audit_redacts_authenticated_reports},
      {"driver_webrtc_connection_audit_transitions", test_driver_webrtc_connection_audit_transitions},
      {"driver_login_failure_rate_limit_and_recovery", test_driver_login_failure_rate_limit_and_recovery},
      {"request_correlation_ids", test_request_correlation_ids},
      {"source_aware_api_and_websocket_rate_limit", test_source_aware_api_and_websocket_rate_limit},
  };
  if (std::getenv("MINE_TELEOP_TEST_WSS_URL") != nullptr) {
    tests.emplace_back("external_https_wss_mac_runtime", test_external_https_wss_mac_runtime);
  }
  int failures = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  return failures == 0 ? 0 : 1;
}
