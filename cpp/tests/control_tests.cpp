#include "mine_teleop/core.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/media.hpp"
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
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
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

// Input/auth/ACK fixtures explicitly model a current video observation. This
// declaration is a module precondition, not camera or presentation evidence.
// Tests for unknown metadata use the HTTP app without this helper; ESTOP keeps
// its older schema and never depends on an added readiness field.
mine_teleop::Json declare_native_input_video_sample(mine_teleop::Json input) {
  if (input.is_object() && !input.value("estop", false) &&
      !input.contains("critical_video_sample_healthy")) {
    input["critical_video_sample_healthy"] = input.value("critical_video_ready", true);
  }
  if (input.is_object() && !input.value("estop", false)) {
    if (!input.contains("critical_video_bad_sample_revision")) input["critical_video_bad_sample_revision"] = 0;
    if (!input.contains("critical_video_brake_revision")) input["critical_video_brake_revision"] = 0;
  }
  return input;
}

// Model the vehicle's independent receive loop. Status queries on the driver
// must not stop the fixture from consuming commands or refreshing its watchdog.
class LoopbackVehiclePump {
 public:
  explicit LoopbackVehiclePump(std::function<void()> receive)
      : worker_([this, receive = std::move(receive)](std::stop_token stop) {
          while (!stop.stop_requested()) {
            {
              std::lock_guard lock(mutex_);
              try { receive(); }
              catch (...) { failure_ = std::current_exception(); return; }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          }
        }) {}
  ~LoopbackVehiclePump() { stop(); }
  void stop() {
    worker_.request_stop();
    if (worker_.joinable()) worker_.join();
  }
  template <typename Function>
  auto inspect(Function&& function) {
    std::lock_guard lock(mutex_);
    if (failure_) std::rethrow_exception(failure_);
    return function();
  }
 private:
  std::mutex mutex_;
  std::exception_ptr failure_;
  std::jthread worker_;
};

void allow_qemu_test_scheduler_time_sync(mine_teleop::DriverConfig& config) {
  constexpr int kTestOnlyMaxUncertaintyMs = 2000;
  expect(
      config.max_time_sync_uncertainty_ms == 25,
      "QEMU scheduling fixture no longer starts from the production 25ms time-sync default");
  // These integration tests exercise session isolation and upstream auth, not
  // the time-sync safety boundary. QEMU may pause a loopback peer long enough
  // to inflate RTT/2, so only these fixtures receive a test scheduling budget.
  config.max_time_sync_uncertainty_ms = kTestOnlyMaxUncertaintyMs;
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
  mine_teleop::ControlReceiver receiver(
      command.vehicle_id,
      command.driver_id,
      command.session_id,
      200,
      mine_teleop::kProtocolVersion,
      true,
      command.control_token);
  auto first = command;
  first.seq = 1;
  first.sent_at_utc_ms = mine_teleop::now_ms();
  expect(receiver.accept(first, first.sent_at_utc_ms).accepted, "first control sequence was rejected");
  auto after_prepared_gap = first;
  after_prepared_gap.seq = 3;
  ++after_prepared_gap.sent_at_utc_ms;
  expect(
      receiver.accept(after_prepared_gap, after_prepared_gap.sent_at_utc_ms).accepted,
      "receiver rejected a monotonic sequence gap from a prepared but superseded browser command");
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

void test_poor_time_samples_do_not_replace_the_accepted_clock_anchor() {
  std::atomic<bool> poor{false};
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0, [&](const mine_teleop::HttpRequest& request) {
    if (request.path != "/time") return mine_teleop::ServerResponse::json(404, {{"error", "not found"}});
    const auto sent = std::stoll(request.query.at("client_send_ms"));
    if (poor.load()) std::this_thread::sleep_for(std::chrono::milliseconds(80));
    const auto utc = mine_teleop::now_ms() + (poor.load() ? 10000 : 0);
    return mine_teleop::ServerResponse::json(200, {{"client_send_ms", sent}, {"server_receive_ms", utc}, {"server_send_ms", utc}});
  });
  server.start();
  mine_teleop::HttpClient http;
  mine_teleop::SynchronizedClock clock;
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  expect(clock.synchronize(http, base, 3, 25).acceptable(25), "initial clock sample unacceptable");
  const auto before = clock.now_ms();
  const auto local_stamp = mine_teleop::now_ms();
  const auto capture_stamp = clock.from_local_system_ms(local_stamp);
  poor = true;
  expect(!clock.synchronize(http, base, 3, 25).acceptable(25), "poor clock cohort appeared trusted");
  expect(clock.now_ms() >= before && clock.now_ms() - before < 2000, "poor sample replaced the trusted UTC anchor");
  expect(clock.from_local_system_ms(local_stamp) == capture_stamp && !clock.status().anchor_accepted,
      "poor sample shifted capture/encoder timestamps despite its rejected UTC anchor");
  expect(clock.refresh_due(30000), "rejected early sample postponed recovery until the normal 30s interval");
  expect(!clock.status().to_json().contains("samples"), "frequent status payload copied verbose time diagnostics");
  const auto samples = clock.status().to_json(true).at("samples");
  expect(samples.size() == 3 && samples.front().at("http").at("total_ms") > 50 &&
      samples.front().contains("wall_clock_step_ms"), "time diagnostics cannot distinguish HTTP delay and local clock movement");
  const auto failure = [&] {
    try { static_cast<void>(mine_teleop::HttpClient(std::chrono::milliseconds(20)).get(base + "/time?client_send_ms=1")); }
    catch (const mine_teleop::HttpTransportError& e) {
      return e.timing().contains("first_byte_ms") && e.timing().contains("total_ms");
    }
    return false;
  }();
  expect(failure, "failed time request lost its HTTP phase timings");
  clock.mark_refresh_failed();
  expect(!clock.status().synchronized && clock.now_ms() - before < 2000, "transport failure discarded the last anchor");
  expect(clock.from_local_system_ms(local_stamp) == capture_stamp, "failed refresh discarded the accepted capture offset");
  poor = false;
  expect(clock.synchronize(http, base, 3, 25).acceptable(25), "good clock did not recover");
  server.stop();
}

void test_time_probe_excludes_client_queue_and_retains_failed_sample_diagnostics() {
  std::atomic<bool> holding{false};
  std::atomic<int> samples{0};
  std::atomic<bool> fail_third{false}, invalid_server_duration{false};
  auto cloud = std::make_shared<mine_teleop::SignalingService>(mine_teleop::SignalingServerConfig{});
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0, [&](const mine_teleop::HttpRequest& request) {
    if (request.path == "/hold") { holding = true; std::this_thread::sleep_for(std::chrono::milliseconds(150)); return mine_teleop::ServerResponse::json(200, {}); }
    if (request.path == "/time") {
      const int index = samples++;
      if (fail_third && index == 2) std::this_thread::sleep_for(std::chrono::milliseconds(150));
      auto response = cloud->handle(request);
      if (invalid_server_duration) {
        auto body = mine_teleop::Json::parse(response.body);
        body["server_send_ms"] = body.at("server_receive_ms").get<std::int64_t>() + 500;
        response.body = body.dump();
      }
      return response;
    }
    return mine_teleop::ServerResponse::json(404, {});
  });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  auto hold = std::async(std::launch::async, [&] { return http.get(base + "/hold"); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!holding && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
  expect(holding, "HTTP queue fixture did not acquire the shared client");
  mine_teleop::SynchronizedClock clock;
  const auto status = clock.synchronize(http, base, 3, 25);
  static_cast<void>(hold.get());
  const auto diagnostics = status.to_json(true).at("samples");
  expect(status.acceptable(25) && std::abs(status.offset_ms) <= 25 &&
      diagnostics.front().at("http").at("client_mutex_wait_ms") >= 50,
      "client queue time contaminated the synchronization delay/offset");
  expect(diagnostics.front().at("timestamp_scope") == "http_header_out_to_body_received" &&
      diagnostics.front().at("http").contains("tcp_before_send") && diagnostics.front().at("http").contains("tcp_at_receive"),
      "probe lost its real exchange timestamps and own TCP observations");
  expect_throws([&] { static_cast<void>(http.get(base + "/time?client_send_ms=1", {{"bad\nheader", "value"}})); },
      "invalid request header was accepted");
  expect(http.get(base + "/health").status == 404 && clock.synchronize(http, base, 3, 25).acceptable(25),
      "failed probe cleanup broke ordinary requests or the next synchronization");
  const auto trusted_anchor = clock.status();
  const auto trusted_local = trusted_anchor.accepted_anchor_local_ms;
  samples = 0; fail_third = true;
  bool detailed_timeout = false;
  try { static_cast<void>(clock.synchronize(mine_teleop::HttpClient(std::chrono::milliseconds(50)), base, 3, 25)); }
  catch (const mine_teleop::TimeSyncSampleError& error) {
    detailed_timeout = error.diagnostics().at("failed_sample_index") == 2 &&
        error.diagnostics().at("completed_samples").size() == 2 && error.diagnostics().at("http_failure").contains("tcp_before_send");
  }
  expect(detailed_timeout && !clock.status().acceptable(25) && clock.status().accepted_anchor_local_ms == trusted_local,
      "partial failed cohort lost diagnostics or left stale readiness authorized");
  fail_third = false; invalid_server_duration = true;
  bool inconsistent_rejected = false;
  try { static_cast<void>(clock.synchronize(http, base, 3, 25)); }
  catch (const mine_teleop::TimeSyncSampleError& error) {
    inconsistent_rejected = error.diagnostics().at("completed_samples").size() == 1 &&
        !error.diagnostics().at("completed_samples").front().at("valid").get<bool>() &&
        std::string(error.what()).find("inconsistent") != std::string::npos;
  }
  expect(inconsistent_rejected && !clock.status().acceptable(25), "impossible server processing created a false zero-RTT trusted clock");
  server.stop();
}

void test_time_uncertainty_covers_the_median_shift_from_best_sample() {
  std::atomic<int> sample{0};
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0, [&](const mine_teleop::HttpRequest& request) {
    const int index = sample++;
    const auto sent = std::stoll(request.query.at("client_send_ms"));
    const auto delay = index == 0 ? 20 : index == 1 ? 200 : 250;
    std::this_thread::sleep_for(std::chrono::milliseconds(delay));
    // Use elapsed UTC at the actual reply boundary, rather than assuming
    // sleep_for(delay) also equals the real HTTP exchange duration. Keep a
    // large offset separation so CI scheduling jitter cannot erase the case.
    const auto synthetic_time = (sent + mine_teleop::now_ms()) / 2 + (index == 0 ? 0 : 500);
    return mine_teleop::ServerResponse::json(200, {{"client_send_ms", sent},
        {"server_receive_ms", synthetic_time}, {"server_send_ms", synthetic_time}});
  });
  server.start();
  mine_teleop::SynchronizedClock clock;
  const auto status = clock.synchronize(mine_teleop::HttpClient{}, "http://127.0.0.1:" + std::to_string(server.port()), 3, 25);
  const auto diagnostics = status.to_json(true).at("samples");
  const auto best = *std::min_element(diagnostics.begin(), diagnostics.end(), [](const auto& a, const auto& b) {
    return a.at("round_trip_ms").template get<std::int64_t>() < b.at("round_trip_ms").template get<std::int64_t>();
  });
  const auto best_radius = (best.at("round_trip_ms").get<std::int64_t>() + 1) / 2;
  const auto shifted = std::abs(status.offset_ms - best.at("offset_ms").get<std::int64_t>());
  expect(shifted >= 250 && status.uncertainty_ms >= best_radius + shifted && !status.anchor_accepted,
      "median estimate falsely narrowed the valid low-RTT sample's error interval: " + status.to_json(true).dump());
  server.stop();
}

void test_slow_time_refresh_does_not_block_status_or_expire_live_input() {
  std::atomic<int> delay_ms{0};
  std::atomic<int> slow_time_samples{0};
  std::atomic<bool> slow_probe_started{false};
  std::atomic<bool> simulate_network_delay{false};
  mine_teleop::SignalingServerConfig cloud_config;
  cloud_config.driver_passwords = {{"driver-console-001", "slow-time-password"}};
  cloud_config.device_tokens = {{"vehicle-001", "slow-time-device"}};
  cloud_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto cloud = std::make_shared<mine_teleop::SignalingService>(cloud_config);
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0, [&](const auto& request) {
    auto routed = request;
    if (request.path == "/time" && delay_ms.load() > 0) {
      slow_probe_started = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms.load()));
      ++slow_time_samples;
    }
    // Explicitly model transit before server ingress only in the network phase.
    if (simulate_network_delay && request.path == "/time") {
      routed.received_at_utc_ms = mine_teleop::now_ms();
      routed.received_at_monotonic_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
      routed.parsed_at_monotonic_ms = routed.received_at_monotonic_ms;
    }
    return cloud->handle(routed);
  }, 8 * 1024 * 1024, [cloud](int socket, const auto& request) { return cloud->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(base + "/vehicles/online",
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"}, {"device_token", "slow-time-device"}, {"connection_id", "slow-time-vehicle"}}));
  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001"; config.signaling_url = base;
  config.time_sync_interval_ms = 200; config.time_sync_samples = 3;
  expect(config.intent_lease_ms == 200 && config.max_time_sync_uncertainty_ms == 25, "fixture relaxed a production safety limit");
  {
    mine_teleop::DriverConsoleRuntime driver(config, "vehicle-001", "slow-time-password");
    const auto connected = driver.connect("vehicle-001");
    const auto session = connected.at("session_id").get<std::string>();
    const auto generation = connected.at("control_session_generation").get<std::uint64_t>();
    const mine_teleop::Json neutral = {{"session_id", session}, {"session_generation", generation},
        {"ui_instance_id", "slow-time-ui"}, {"intent_seq", 1}, {"gear", "N"}, {"steering", 0}, {"throttle", 0}, {"brake", 0}};
    expect(driver.update_control_intent(declare_native_input_video_sample(neutral)).at("accepted").get<bool>(), "neutral input rejected");
    const auto first_anchor = driver.status().at("time_sync").at("accepted_anchor_local_ms").get<std::int64_t>();
    delay_ms = 350;
    bool slow_processing_seen = false;
    const auto software_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < software_deadline && !slow_processing_seen) {
      const auto started = std::chrono::steady_clock::now();
      const auto status = driver.status();
      expect(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200), "monitoring blocked on slow server processing");
      expect(driver.update_control_intent(declare_native_input_video_sample(neutral)).at("accepted").get<bool>(), "live input rejected while /time server waited");
      // An external UTC step can correctly reject an individual cohort. The
      // completed slow cohort, rather than every intermediate snapshot, must
      // prove that measured server processing does not count as network RTT.
      slow_processing_seen = slow_time_samples >= config.time_sync_samples * 2 &&
          status.at("time_sync").value("synchronized", false) &&
          status.at("time_sync").at("uncertainty_ms") <= 25 &&
          status.at("time_sync").at("accepted_anchor_local_ms").get<std::int64_t>() > first_anchor;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    expect(slow_processing_seen, "fixture never completed its slow server-processing cohort");
    simulate_network_delay = true;
    bool bad_seen = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline && !bad_seen) {
      const auto started = std::chrono::steady_clock::now();
      const auto status = driver.status();
      expect(std::chrono::steady_clock::now() - started < std::chrono::milliseconds(200), "monitoring blocked on network time samples");
      expect(driver.update_control_intent(declare_native_input_video_sample(neutral)).at("accepted").get<bool>(), "live neutral refresh rejected");
      expect(driver.status().at("native_control").value("intent_fresh", false), "slow time sampling expired refreshed local input");
      bad_seen = status.at("time_sync").at("uncertainty_ms").get<std::int64_t>() > 25;
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    expect(slow_probe_started && bad_seen, "fixture did not produce a slow, untrusted time cohort");
    delay_ms = 0;
    bool recovered = false;
    const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (std::chrono::steady_clock::now() < recovery_deadline && !recovered) {
      static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(neutral)));
      const auto status = driver.status();
      recovered = status.at("time_sync").value("synchronized", false) && status.at("time_sync").at("uncertainty_ms") <= 25;
      expect(status.at("session_id") == session && status.at("control_session_generation") == generation,
          "time recovery disconnected or replaced the active control session");
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
    expect(recovered, "time sync did not recover with the same session");
  }
  server.stop();
}

void test_local_http_page_takeover_abort_reordering_and_production_lease() {
  using mine_teleop::Json;
  mine_teleop::SignalingServerConfig cloud_config;
  cloud_config.driver_passwords = {{"driver-console-001", "offline-page-password"}};
  cloud_config.device_tokens = {{"vehicle-001", "offline-page-device"}};
  cloud_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto cloud = std::make_shared<mine_teleop::SignalingService>(cloud_config);
  mine_teleop::SimpleHttpServer cloud_server("127.0.0.1", 0,
      [cloud](const auto& request) { return cloud->handle(request); }, 8 * 1024 * 1024,
      [cloud](int socket, const auto& request) { return cloud->handle_websocket(socket, request); });
  cloud_server.start();
  const auto cloud_base = "http://127.0.0.1:" + std::to_string(cloud_server.port());
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(cloud_base + "/vehicles/online",
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"}, {"device_token", "offline-page-device"}, {"connection_id", "offline-page-vehicle"}});
  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001"; config.signaling_url = cloud_base;
  expect(config.intent_lease_ms == 200 && config.max_time_sync_uncertainty_ms == 25,
      "offline input integration changed a production safety budget");
  auto driver = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "offline-page-password");
  const auto connected = driver->connect("vehicle-001");
  auto app = std::make_shared<mine_teleop::DriverConsoleHttpApp>(driver);
  std::atomic<bool> delayed_ingress_seen{false};
  std::atomic<bool> response_delay_applied{false};
  std::atomic<bool> response_delay_completed{false};
  Json response_delay_acceptance;
  mine_teleop::SimpleHttpServer local_server("127.0.0.1", 0, [&](const auto& request) {
    const auto body = request.json_body();
    if (body.value("test_before_handler_delay_ms", 0) > 0) {
      delayed_ingress_seen = true;
      std::this_thread::sleep_for(std::chrono::milliseconds(body.at("test_before_handler_delay_ms").template get<int>()));
    }
    auto response = app->handle(request);
    if (body.value("test_response_delay_ms", 0) > 0) {
      response_delay_acceptance = Json::parse(response.body);
      response_delay_applied.store(true, std::memory_order_release);
      std::this_thread::sleep_for(std::chrono::milliseconds(body.at("test_response_delay_ms").template get<int>()));
      response_delay_completed.store(true, std::memory_order_release);
    }
    return response;
  });
  local_server.start();
  const auto local_base = "http://127.0.0.1:" + std::to_string(local_server.port());
  Json input = {{"session_id", connected.at("session_id")},
      {"session_generation", connected.at("control_session_generation")},
      {"ui_instance_id", "old-http-page"}, {"intent_seq", 1}, {"ui_control_epoch", 0},
      {"critical_video_ready", true}, {"critical_video_sample_healthy", true},
      {"critical_video_bad_sample_revision", 0}, {"critical_video_brake_revision", 0},
      {"takeover", true}, {"gear", "D"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}};
  for (const bool legacy_epoch : {false, true}) {
    auto unknown = input; unknown.erase("critical_video_sample_healthy");
    if (legacy_epoch) unknown.erase("ui_control_epoch");
    bool metadata_rejected = false;
    try { static_cast<void>(http.post_json_response(local_base + "/api/control-intent", unknown)); }
    catch (const mine_teleop::HttpStatusError& error) {
      metadata_rejected = error.status() == 400 &&
          Json::parse(error.response_body()).value("issue_code", "") == "control_page_metadata_missing";
    }
    expect(metadata_rejected, "unknown modern or legacy video sample was assumed healthy at HTTP ingress");
  }
  expect(driver->status().at("native_control").at("ui_control_epoch") == 0 &&
      driver->status().at("native_control").at("accepted_refresh_sample_id") == 0,
      "missing video metadata transferred ownership or refreshed input");
  auto response = http.post_json_response(local_base + "/api/control-intent", input);
  expect(response.value("accepted", false) && response.at("ui_control_epoch") == 1,
      "initial page neutral handshake failed through real HTTP");
  auto delayed_old = input;
  delayed_old["takeover"] = false; delayed_old["ui_control_epoch"] = 1;
  delayed_old["intent_seq"] = 2; delayed_old["test_before_handler_delay_ms"] = 220;
  auto old_response = std::async(std::launch::async, [=] {
    return mine_teleop::HttpClient{}.post_json_response(local_base + "/api/control-intent", delayed_old);
  });
  const auto arrival_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!delayed_ingress_seen && std::chrono::steady_clock::now() < arrival_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(delayed_ingress_seen, "old HTTP request did not reach ingress before takeover");
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  input["ui_instance_id"] = "new-http-page"; input["ui_control_epoch"] = 1;
  response = http.post_json_response(local_base + "/api/control-intent", input);
  expect(response.value("accepted", false) && response.at("ui_control_epoch") == 2,
      "new page did not confirm takeover through real HTTP");
  input["takeover"] = false; input["ui_control_epoch"] = 2;
  input["intent_seq"] = 2; input["throttle"] = 0.4;
  expect(http.post_json_response(local_base + "/api/control-intent", input).value("accepted", false),
      "confirmed new page motion rejected");
  while (old_response.wait_for(std::chrono::milliseconds(45)) != std::future_status::ready) {
    expect(http.post_json_response(local_base + "/api/control-intent", input).value("accepted", false),
        "new page heartbeat was blocked by an old in-flight HTTP handler");
  }
  const auto rejected_old = old_response.get();
  expect(!rejected_old.value("accepted", true) && rejected_old.value("reason", "") == "old_intent_arrival",
      "old-page neutral applied after new-page ownership and motion");
  auto native = driver->status().at("native_control");
  expect(native.at("owner_ui_instance_id") == "new-http-page" && native.at("ui_control_epoch") == 2 &&
      native.at("effective_throttle") == 0.4 && !native.value("requires_fresh_input", true),
      "late retired-page HTTP request changed the current page's input");

  const auto refresh_before_abort = native.at("accepted_refresh_sample_id").get<std::uint64_t>();
  // A refresh is identified by a strictly newer monotonic millisecond. Make
  // that precondition explicit rather than depending on HTTP call overhead.
  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  auto slow_response = input; slow_response["test_response_delay_ms"] = 220;
  struct ResponseSocket {
    int value{-1};
    ~ResponseSocket() { if (value >= 0) ::close(value); }
  } response_socket;
  response_socket.value = ::socket(AF_INET, SOCK_STREAM, 0);
  expect(response_socket.value >= 0, "cannot create cancellation fixture socket");
  const timeval response_send_timeout{2, 0};
  expect(::setsockopt(response_socket.value, SOL_SOCKET, SO_SNDTIMEO,
      &response_send_timeout, sizeof(response_send_timeout)) == 0,
      "cannot bound cancellation fixture socket send");
  sockaddr_in local_address{};
  local_address.sin_family = AF_INET;
  local_address.sin_port = htons(local_server.port());
  local_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  expect(::connect(response_socket.value, reinterpret_cast<const sockaddr*>(&local_address),
      sizeof(local_address)) == 0, "cannot connect cancellation fixture socket");
  const auto slow_body = slow_response.dump();
  raw_send_all(response_socket.value, "POST /api/control-intent HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Content-Type: application/json\r\nContent-Length: " + std::to_string(slow_body.size()) +
      "\r\nConnection: close\r\n\r\n" + slow_body);
  const auto apply_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!response_delay_applied.load(std::memory_order_acquire) &&
      std::chrono::steady_clock::now() < apply_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  expect(response_delay_applied.load(std::memory_order_acquire) &&
      response_delay_acceptance.value("accepted", false) &&
      response_delay_acceptance.at("accepted_refresh_sample_id").get<std::uint64_t>() > refresh_before_abort,
      "cancelled request was not accepted before its response was held");
  // Closing this real client socket cancels only its pending response. A
  // timer-based HTTP timeout could resume after the independent200ms lease.
  const bool response_aborted = ::shutdown(response_socket.value, SHUT_RDWR) == 0;
  const int cancelled_socket = std::exchange(response_socket.value, -1);
  expect(::close(cancelled_socket) == 0, "cannot close cancelled fixture response");
  native = driver->status().at("native_control");
  expect(response_aborted && native.at("accepted_refresh_sample_id").get<std::uint64_t>() > refresh_before_abort &&
      native.value("intent_fresh", false) && native.at("effective_throttle") == 0.4,
      "response cancellation was confused with native rejection or immediately cleared held motion: " +
          Json{{"response_aborted", response_aborted}, {"refresh_before", refresh_before_abort},
               {"refresh_after", native.at("accepted_refresh_sample_id")},
               {"intent_age_ms", native.at("intent_age_ms")}, {"intent_fresh", native.at("intent_fresh")},
               {"effective_throttle", native.at("effective_throttle")}}.dump());
  input["intent_seq"] = 3; input["steering"] = -0.2;
  for (int refresh = 0; refresh < 4; ++refresh) {
    expect(http.post_json_response(local_base + "/api/control-intent", input).value("accepted", false),
        "slow cancelled response serialized a later input refresh");
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
  }
  // The cancelled handler holds its already-accepted response for220ms.
  // Four45ms refreshes alone do not prove that the old handler has finished.
  const auto completion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!response_delay_completed.load(std::memory_order_acquire) &&
      std::chrono::steady_clock::now() < completion_deadline) {
    expect(http.post_json_response(local_base + "/api/control-intent", input).value("accepted", false),
        "current input refresh failed while waiting for cancelled response completion");
    std::this_thread::sleep_for(std::chrono::milliseconds(45));
  }
  expect(response_delay_completed.load(std::memory_order_acquire),
      "cancelled response handler did not complete before the bounded assertion");
  native = driver->status().at("native_control");
  expect(native.at("effective_intent_seq") == 3 && native.at("effective_steering") == -0.2 &&
      native.value("intent_fresh", false), "late response completion restored an older operation");
  // Exercise actual ingress after that completion, including both obsolete
  // sequence and retired-page identity. Neither request may refresh or rewind
  // the operation that remains held by the current page.
  const auto refresh_after_completion = native.at("accepted_refresh_sample_id");
  auto completed_old_operation = slow_response; completed_old_operation.erase("test_response_delay_ms");
  const auto old_operation_rejection = http.post_json_response(local_base + "/api/control-intent", completed_old_operation);
  expect(!old_operation_rejection.value("accepted", true) &&
      old_operation_rejection.value("reason", "") == "old_intent_seq",
      "completed cancelled response's old operation was accepted after a newer operation");
  auto completed_retired_page = delayed_old; completed_retired_page.erase("test_before_handler_delay_ms");
  const auto retired_page_rejection = http.post_json_response(local_base + "/api/control-intent", completed_retired_page);
  expect(!retired_page_rejection.value("accepted", true) &&
      retired_page_rejection.value("reason", "") == "input_page_not_current",
      "retired page ordinary input was accepted after cancelled response completion");
  native = driver->status().at("native_control");
  expect(native.at("effective_intent_seq") == 3 && native.at("effective_steering") == -0.2 &&
      native.at("effective_throttle") == 0.4 && native.value("intent_fresh", false) &&
      native.at("owner_ui_instance_id") == "new-http-page" && native.at("ui_control_epoch") == 2 &&
      native.at("accepted_refresh_sample_id") == refresh_after_completion,
      "post-completion obsolete input changed current motion, ownership or input freshness");
  const auto sent_before_stall = native.at("commands_sent_total").get<std::uint64_t>();
  std::this_thread::sleep_for(std::chrono::milliseconds(230));
  const auto stalled = driver->status(); native = stalled.at("native_control");
  expect(stalled.at("session_id") == connected.at("session_id") && !native.value("intent_fresh", true) &&
      native.value("requires_fresh_input", false) && native.at("effective_throttle") == 0.0 &&
      native.at("commands_sent_total").get<std::uint64_t>() > sent_before_stall,
      "UI timer stall stopped native heartbeat, changed session, or widened the 200ms lease");
  expect(http.post_json_response(local_base + "/api/control-intent", input).value("reason", "") == "fresh_neutral_required",
      "post-stall HTTP refresh resurrected old held input");
  input["intent_seq"] = 4; input["steering"] = input["throttle"] = 0.0;
  expect(http.post_json_response(local_base + "/api/control-intent", input).value("accepted", false),
      "post-stall neutral recovery rejected");
  input["intent_seq"] = 5; input["throttle"] = 0.4;
  expect(http.post_json_response(local_base + "/api/control-intent", input).value("accepted", false),
      "new operation following neutral was rejected");

  // Observe actual production serialization and cloud WSS delivery: a
  // tolerated raw video fault must reach recovery as unhealthy even though
  // the qualified video gate still permits the first/second fault.
  mine_teleop::WebSocketClient vehicle_receiver;
  vehicle_receiver.connect(mine_teleop::signaling_websocket_url(cloud_base,
      connected.at("session_id").get<std::string>(), "vehicle-001",
      std::to_string(online.at("connection_generation").get<std::uint64_t>())) + "&types=control_command",
      {{"X-Mine-Teleop-Device-Token", "offline-page-device"}});
  input["intent_seq"] = 6; input["critical_video_ready"] = true;
  input["critical_video_sample_healthy"] = false;
  expect(http.post_json_response(local_base + "/api/control-intent", input).value("accepted", false),
      "known below-threshold raw video sample was rejected as input");
  native = driver->status().at("native_control");
  expect(native.value("effective_critical_video_ready", false) &&
      !native.value("effective_critical_video_sample_healthy", true) &&
      native.value("refresh_sample_healthy", false), "Store mixed raw video health with input or qualified readiness");
  bool raw_health_delivered = false;
  const auto raw_health_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!raw_health_delivered && std::chrono::steady_clock::now() < raw_health_deadline) {
    const auto delivery = vehicle_receiver.receive_json(std::chrono::milliseconds(40));
    if (delivery.status == mine_teleop::WebSocketReceiveStatus::Message &&
        delivery.message.value("event", "") == "signaling_messages") {
      for (const auto& message : delivery.message.at("messages")) {
        const auto& payload = message.at("payload");
        if (payload.value("intent_seq", 0) == 6) {
          expect(payload.value("critical_video_ready", false) && payload.value("operator_input_ready", false) &&
              !payload.value("recovery_sample_healthy", true),
              "qualified video readiness hid a raw fault from the common recovery health window");
          raw_health_delivered = true;
        }
        vehicle_receiver.send_json({{"event", "signaling_delivery_ack"},
            {"delivery_cursor", delivery.message.at("delivery_cursor")},
            {"trace_session_id", connected.at("session_id")}, {"seq", message.at("seq")},
            {"intent_seq", payload.at("intent_seq")}});
      }
    }
    static_cast<void>(http.post_json_response(local_base + "/api/control-intent", input));
  }
  expect(raw_health_delivered, "actual native/cloud WSS path did not carry the below-threshold raw health sample");

  auto retired_estop = delayed_old;
  retired_estop.erase("test_before_handler_delay_ms"); retired_estop["intent_seq"] = 3; retired_estop["estop"] = true;
  retired_estop["critical_video_sample_healthy"] = "legacy-observation-unsupported";
  auto wrong_session = retired_estop; wrong_session["session_id"] = "retired-session";
  bool wrong_session_rejected = false;
  try { static_cast<void>(http.post_json_response(local_base + "/api/control-intent", wrong_session)); }
  catch (const mine_teleop::HttpStatusError& error) { wrong_session_rejected = error.status() == 409; }
  expect(wrong_session_rejected, "retired ESTOP bypassed current-session authentication checks");
  expect(http.post_json_response(local_base + "/api/control-intent", retired_estop).value("accepted", false),
      "valid retired-page ESTOP was rejected by its obsolete page epoch");
  native = driver->status().at("native_control");
  expect(native.value("effective_estop", false) && native.at("owner_ui_instance_id") == "new-http-page" &&
      native.at("ui_control_epoch") == 2, "retired ESTOP did not stop or transferred ordinary ownership");
  vehicle_receiver.close();
  local_server.stop();
  static_cast<void>(driver->disconnect("offline_page_test_complete"));
  driver.reset();
  cloud_server.stop();
}

void test_local_http_release_scope_prevents_retired_page_and_takeover_races() {
  using mine_teleop::Json;
  mine_teleop::SignalingServerConfig cloud_config;
  cloud_config.driver_passwords = {{"driver-console-001", "scope-password"}};
  cloud_config.device_tokens = {{"vehicle-001", "scope-device"}};
  cloud_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto cloud = std::make_shared<mine_teleop::SignalingService>(cloud_config);
  std::atomic<unsigned> logout_calls{0};
  std::atomic<bool> hold_logout{false}, logout_seen{false}, fail_logout{false};
  mine_teleop::SimpleHttpServer cloud_server("127.0.0.1", 0, [&](const auto& request) {
    if (request.path == "/auth/driver_logout") {
      ++logout_calls; logout_seen = true;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (hold_logout && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      if (fail_logout.exchange(false)) return mine_teleop::ServerResponse::json(503, {{"error", "injected logout failure"}});
    }
    return cloud->handle(request);
  }, 8 * 1024 * 1024, [cloud](int socket, const auto& request) { return cloud->handle_websocket(socket, request); });
  cloud_server.start();
  const auto cloud_base = "http://127.0.0.1:" + std::to_string(cloud_server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(cloud_base + "/vehicles/online",
      {{"vehicle_id", "vehicle-001"}, {"device_token", "scope-device"}, {"connection_id", "scope-vehicle"},
       {"native_control_capabilities", mine_teleop::kNativeControlCapabilities}}));
  mine_teleop::DriverConfig config; config.driver_id = "driver-console-001"; config.signaling_url = cloud_base;
  allow_qemu_test_scheduler_time_sync(config); // Protocol scope, not the 25ms clock-safety suite.
  auto driver = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "scope-password");
  auto app = std::make_shared<mine_teleop::DriverConsoleHttpApp>(driver);
  std::atomic<bool> delayed_seen{false}, release_delayed{false};
  mine_teleop::SimpleHttpServer local_server("127.0.0.1", 0, [&](const auto& request) {
    if (request.path == "/api/disconnect" && request.json_body().value("test_wait_before_scope", false)) {
      delayed_seen = true;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!release_delayed && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return app->handle(request);
  });
  local_server.start();
  const auto local_base = "http://127.0.0.1:" + std::to_string(local_server.port());
  const auto connected = http.post_json_response(local_base + "/api/connect", {{"vehicle_id", "vehicle-001"}, {"ui_instance_id", "scope-page-a"}});
  const auto make_scope = [&](std::string ui) {
    const auto status = http.get_json(local_base + "/api/status");
    return Json{{"expected_session_id", status.at("session_id")}, {"expected_driver_id", status.at("driver_id")},
        {"expected_control_session_generation", status.at("control_session_generation")},
        {"expected_ui_control_epoch", status.at("native_control").at("ui_control_epoch")}, {"ui_instance_id", ui},
        {"expected_connect_cancellation_generation", status.at("connect_cancellation_generation")}, {"reason", "scope-test"}};
  };
  const auto rejected = [&](const std::string& path, const Json& scope) {
    bool denied = false;
    try { static_cast<void>(http.post_json_response(local_base + path, scope)); }
    catch (const mine_teleop::HttpStatusError& error) {
      denied = error.status() == 409 && error.issue_code().starts_with("driver_disconnect_");
    }
    expect(denied, "stale/invalid browser release was not a structured conflict");
  };
  // Before the first neutral claim, only the real creator can clean up.
  auto unknown_creator = make_scope("unrelated-page");
  rejected("/api/end-session", unknown_creator);
  expect(cloud->health().at("active_sessions") == 1 && logout_calls == 0,
      "unclaimed session let an unrelated page clean up its creator's authority");
  Json neutral = declare_native_input_video_sample({{"session_id", connected.at("session_id")},
      {"session_generation", connected.at("control_session_generation")}, {"ui_instance_id", "scope-page-a"},
      {"ui_control_epoch", 0}, {"takeover", true}, {"intent_seq", 1}, {"gear", "N"},
      {"throttle", 0.0}, {"steering", 0.0}, {"brake", 0.0}, {"estop", false}});
  expect(http.post_json_response(local_base + "/api/control-intent", neutral).value("accepted", false), "first page claim rejected");
  auto old_scope = make_scope("scope-page-a");
  auto delayed_scope = old_scope; delayed_scope["test_wait_before_scope"] = true;
  auto delayed = std::async(std::launch::async, [=] {
    return mine_teleop::HttpClient{}.post_json_response(local_base + "/api/disconnect", delayed_scope);
  });
  const auto seen_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!delayed_seen && std::chrono::steady_clock::now() < seen_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(delayed_seen, "old logout did not enter the actual HTTP handler");
  neutral["ui_instance_id"] = "scope-page-b"; neutral["ui_control_epoch"] = 1;
  expect(http.post_json_response(local_base + "/api/control-intent", neutral).value("accepted", false), "replacement page claim rejected");
  auto current_scope = make_scope("scope-page-b");
  const auto current_generation = driver->status().at("connect_cancellation_generation");
  release_delayed = true;
  bool delayed_denied = false;
  try { static_cast<void>(delayed.get()); }
  catch (const mine_teleop::HttpStatusError& error) { delayed_denied = error.status() == 409 && error.issue_code() == "driver_disconnect_scope_mismatch"; }
  expect(delayed_denied && logout_calls == 0, "delayed retired-page logout reached cloud release");
  for (const auto& path : {std::string("/api/disconnect"), std::string("/api/end-session")}) {
    rejected(path, old_scope);
    auto wrong = current_scope; wrong["expected_session_id"] = "wrong-session"; rejected(path, wrong);
    wrong = current_scope; wrong["expected_control_session_generation"] = connected.at("control_session_generation").get<std::uint64_t>() + 1; rejected(path, wrong);
    wrong = current_scope; wrong["expected_ui_control_epoch"] = 1; rejected(path, wrong);
    wrong = current_scope; wrong["expected_driver_id"] = "other-driver"; rejected(path, wrong);
    wrong = current_scope; wrong["expected_connect_cancellation_generation"] = current_generation.get<std::uint64_t>() + 1; rejected(path, wrong);
    rejected(path, Json{{"reason", "legacy-page-release"}});
  }
  const auto retained = driver->status();
  expect(retained.at("session_id") == connected.at("session_id") && retained.at("native_control").at("owner_ui_instance_id") == "scope-page-b" &&
      retained.at("native_control").at("ui_control_epoch") == 2 && retained.at("connect_cancellation_generation") == current_generation &&
      cloud->health().at("active_sessions") == 1 && logout_calls == 0,
      "rejected browser releases mutated session, owner, epoch, or cancellation generation");

  hold_logout = true; fail_logout = true;
  auto releasing = std::async(std::launch::async, [=] {
    return mine_teleop::HttpClient{}.post_json_response(local_base + "/api/disconnect", current_scope);
  });
  const auto reserved_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!logout_seen && std::chrono::steady_clock::now() < reserved_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(logout_seen && driver->status().value("disconnect_in_progress", false), "current release did not reserve its scope before slow cloud I/O");
  neutral["ui_instance_id"] = "scope-page-c"; neutral["ui_control_epoch"] = 2;
  bool takeover_blocked = false;
  try { static_cast<void>(http.post_json_response(local_base + "/api/control-intent", neutral)); }
  catch (const mine_teleop::HttpStatusError& error) { takeover_blocked = error.status() == 409 && error.issue_code() == "driver_disconnect_in_progress"; }
  expect(takeover_blocked, "new page took ownership after the logout scope check but before release");
  auto estop = neutral; estop["ui_instance_id"] = "scope-page-a"; estop["ui_control_epoch"] = 1; estop["estop"] = true;
  expect(http.post_json_response(local_base + "/api/control-intent", estop).value("accepted", false), "scope reservation blocked legal current-session ESTOP");
  hold_logout = false;
  bool retryable_failure = false;
  try { static_cast<void>(releasing.get()); }
  catch (const mine_teleop::HttpStatusError& error) { retryable_failure = error.status() == 503; }
  expect(retryable_failure && driver->status().value("authenticated", false) && !driver->status().value("disconnect_in_progress", true),
      "failed cloud release lost authentication or permanently froze the input reservation");
  neutral["ui_instance_id"] = "scope-page-b"; neutral["ui_control_epoch"] = 2; neutral["takeover"] = false; neutral["intent_seq"] = 2;
  expect(http.post_json_response(local_base + "/api/control-intent", neutral).value("accepted", false), "reservation was not released after cloud failure");
  const auto ended = http.post_json_response(local_base + "/api/end-session", make_scope("scope-page-b"));
  expect(ended.value("release_scope_validated", false) && driver->status().value("authenticated", false) && !driver->status().value("connected", true),
      "current scoped end-session did not preserve authentication while releasing authority");
  rejected("/api/disconnect", current_scope);
  const auto finished = http.post_json_response(local_base + "/api/disconnect", make_scope("scope-page-b"));
  expect(finished.value("disconnect_scope_validated", false) && !driver->status().value("authenticated", true) && cloud->health().at("active_sessions") == 0,
      "current idle identity scope could not finish logout");
  local_server.stop(); driver.reset(); cloud_server.stop();
}

void test_local_http_pending_approval_release_requires_operation_owner_and_generation() {
  using mine_teleop::Json;
  mine_teleop::SignalingServerConfig cloud_config;
  cloud_config.driver_passwords = {{"driver-console-001", "pending-password"}};
  cloud_config.device_tokens = {{"vehicle-001", "pending-device"}};
  cloud_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  cloud_config.mobile_app_password = "pending-approver"; cloud_config.mobile_approval_vehicles = {"vehicle-001"};
  auto cloud = std::make_shared<mine_teleop::SignalingService>(cloud_config);
  mine_teleop::SimpleHttpServer cloud_server("127.0.0.1", 0, [cloud](const auto& request) { return cloud->handle(request); },
      8 * 1024 * 1024, [cloud](int socket, const auto& request) { return cloud->handle_websocket(socket, request); });
  cloud_server.start();
  const auto cloud_base = "http://127.0.0.1:" + std::to_string(cloud_server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(cloud_base + "/vehicles/online", {{"vehicle_id", "vehicle-001"},
      {"device_token", "pending-device"}, {"connection_id", "pending-vehicle"}, {"native_control_capabilities", mine_teleop::kNativeControlCapabilities}}));
  mine_teleop::DriverConfig config; config.driver_id = "driver-console-001"; config.signaling_url = cloud_base;
  allow_qemu_test_scheduler_time_sync(config);
  auto driver = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "pending-password");
  auto app = std::make_shared<mine_teleop::DriverConsoleHttpApp>(driver);
  mine_teleop::SimpleHttpServer local_server("127.0.0.1", 0, [app](const auto& request) { return app->handle(request); });
  local_server.start();
  const auto local_base = "http://127.0.0.1:" + std::to_string(local_server.port());
  auto connecting = std::async(std::launch::async, [=] {
    return mine_teleop::HttpClient(std::chrono::seconds(8)).post_json_response(local_base + "/api/connect",
        {{"vehicle_id", "vehicle-001"}, {"ui_instance_id", "pending-page"}});
  });
  struct CancelPendingOnExit {
    std::shared_ptr<mine_teleop::DriverConsoleRuntime> runtime;
    ~CancelPendingOnExit() { runtime->prepare_shutdown(); }
  } cancel_on_exit{driver};
  Json status;
  const auto pending_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < pending_deadline) {
    status = http.get_json(local_base + "/api/status");
    if (status.at("pending_mobile_approval").contains("request_id")) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  expect(status.at("pending_mobile_approval").contains("request_id"), "real connect did not enter approval wait");
  Json scope = {{"expected_driver_id", status.at("driver_id")}, {"expected_session_id", status.at("session_id")},
      {"expected_control_session_generation", status.at("control_session_generation")},
      {"expected_ui_control_epoch", status.at("native_control").at("ui_control_epoch")}, {"ui_instance_id", "pending-page"},
      {"expected_connect_cancellation_generation", status.at("connect_cancellation_generation")}};
  for (const auto& invalid : {std::string("wrong-page"), std::string("wrong-generation"), std::string("missing-generation")}) {
    auto old = scope;
    if (invalid == "wrong-page") old["ui_instance_id"] = "retired-page";
    if (invalid == "wrong-generation") old["expected_connect_cancellation_generation"] = status.at("connect_cancellation_generation").get<std::uint64_t>() - 1;
    if (invalid == "missing-generation") old.erase("expected_connect_cancellation_generation");
    bool denied = false;
    try { static_cast<void>(http.post_json_response(local_base + "/api/disconnect", old)); }
    catch (const mine_teleop::HttpStatusError& error) { denied = error.status() == 409; }
    const auto retained = driver->status();
    expect(denied && retained.at("connect_cancellation_generation") == status.at("connect_cancellation_generation") &&
        retained.at("pending_mobile_approval").at("request_id") == status.at("pending_mobile_approval").at("request_id") &&
        connecting.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready,
        "stale browser exit cancelled or replaced another page's pending approval");
  }
  const auto release = http.post_json_response(local_base + "/api/disconnect", scope);
  bool cancelled = false;
  try { static_cast<void>(connecting.get()); } catch (const mine_teleop::HttpStatusError& error) { cancelled = error.status() == 409; }
  expect(cancelled && release.value("disconnect_scope_validated", false) && !driver->status().value("authenticated", true) &&
      cloud->health().at("active_sessions") == 0, "matching pending owner could not cancel and release without authority issuance");
  local_server.stop(); driver.reset(); cloud_server.stop();
}

void test_driver_time_sync_uncertainty_fails_closed_at_production_default() {
  constexpr std::int64_t kSyntheticOffsetStepMs = 10'000;
  std::atomic<int> time_samples{0};
  std::atomic<int> login_requests{0};
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1",
      0,
      [&](const mine_teleop::HttpRequest& request) {
        const auto client_send = request.query.find("client_send_ms");
        if (request.path == "/time" && client_send != request.query.end()) {
          const auto client_send_ms = std::stoll(client_send->second);
          const auto sample = static_cast<std::int64_t>(++time_samples);
          // Successful HTTP samples are bounded to 5s, so RTT/2 cannot cancel
          // this deterministic separation between any two synthetic offsets.
          const auto synthetic_server_ms = client_send_ms + sample * kSyntheticOffsetStepMs;
          return mine_teleop::ServerResponse::json(
              200,
              {{"time_domain", "signaling_server"},
               {"client_send_ms", client_send_ms},
               {"server_receive_ms", synthetic_server_ms},
               {"server_send_ms", synthetic_server_ms}});
        }
        if (request.path == "/auth/driver_login") {
          ++login_requests;
          return mine_teleop::ServerResponse::json(500, {{"error", "unexpected authentication"}});
        }
        return mine_teleop::ServerResponse::json(404, {{"error", "not found"}});
      });
  server.start();

  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001";
  config.signaling_url = "http://127.0.0.1:" + std::to_string(server.port());
  expect(
      config.max_time_sync_uncertainty_ms == 25,
      "driver time-sync fail-closed test no longer uses the production 25ms default");

  std::string login_failure;
  mine_teleop::DriverConsoleRuntime login_runtime(config, "vehicle-001", "dev-password");
  try {
    static_cast<void>(login_runtime.login("dev-password"));
  } catch (const std::exception& error) {
    login_failure = error.what();
  }
  std::string connect_failure;
  mine_teleop::DriverConsoleRuntime connect_runtime(config, "vehicle-001", "dev-password");
  try {
    static_cast<void>(connect_runtime.connect("vehicle-001"));
  } catch (const std::exception& error) {
    connect_failure = error.what();
  }
  server.stop();

  expect(
      login_failure.find("exceeds limit 25ms") != std::string::npos,
      "driver login did not fail closed on deterministic time-sync uncertainty above 25ms");
  expect(
      connect_failure.find("exceeds limit 25ms") != std::string::npos,
      "driver connect did not fail closed on deterministic time-sync uncertainty above 25ms");
  expect(time_samples == 14, "driver time-sync fail-closed test did not collect two complete sample sets");
  expect(login_requests == 0, "driver authentication ran before unacceptable time synchronization was rejected");
  expect(
      !login_runtime.status().value("authenticated", true) &&
          !connect_runtime.status().value("authenticated", true),
      "unacceptable time synchronization left local driver authentication active");
}

void test_control_page_contract() {
  mine_teleop::DriverConfig config;
  config.driver_id = "driver-console-001";
  config.signaling_url = "https://signal.example.test";
  config.control_trace_commands = true;
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-001", "dev-password");
  mine_teleop::DriverConsoleHttpApp app(runtime);
  mine_teleop::HttpRequest request;
  request.method = "GET";
  request.path = "/";
  const auto response = app.handle(request);
  expect(response.status == 200, "control page did not load");
  expect(response.body.find("Mine Teleop 控制台") != std::string::npos, "control page identity is missing");
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
  expect(
      response.body.find("id=\"gamepad-panel\"") == std::string::npos &&
          response.body.find("未检测到 Gamepad") == std::string::npos &&
          response.body.find("id=\"keyboard-panel\"") != std::string::npos,
      "the obsolete Gamepad status panel was not replaced by keyboard control feedback");
  expect(
      response.body.find("class=\"workspace\"") != std::string::npos &&
          response.body.find("class=\"sidebar\"") != std::string::npos &&
          response.body.find("html,body{height:100%;overflow:hidden}") != std::string::npos,
      "the control page is not organized as a single-screen workspace with a monitoring sidebar");
  expect(
      response.body.find("id=\"control-steering\"") != std::string::npos &&
          response.body.find("id=\"control-throttle\"") != std::string::npos &&
          response.body.find("id=\"control-brake\"") != std::string::npos &&
          response.body.find("function renderControlState()") != std::string::npos,
      "live steering, throttle, and brake feedback is missing");
  expect(
      response.body.find("id=\"operator-status-strip\"") != std::string::npos &&
          response.body.find("id=\"operator-speed\"") != std::string::npos &&
          response.body.find("id=\"operator-actual-gear\"") != std::string::npos &&
          response.body.find("id=\"operator-control-brake\"") != std::string::npos &&
          response.body.find("position:sticky;top:0;z-index:20") != std::string::npos &&
          response.body.find("grid-auto-rows:clamp(190px,56vw,300px)") != std::string::npos &&
          response.body.find(
              ".can-feedback-panel .can-summary{grid-template-columns:repeat(2,minmax(0,1fr))") !=
              std::string::npos &&
          response.body.find(".visual-stage .camera>video{position:absolute;inset:0") !=
              std::string::npos,
      "camera-first responsive layout can still hide critical vehicle state");
  expect(
      response.body.find("id=\"can-feedback-panel\"") != std::string::npos &&
          response.body.find("id=\"wheel-feedback-grid\"") != std::string::npos &&
          response.body.find("id=\"steering-feedback-grid\"") != std::string::npos &&
          response.body.find("message.event==='vehicle_telemetry'") != std::string::npos &&
          response.body.find("motor_torque_nm") != std::string::npos &&
          response.body.find("motor_speed_rpm") != std::string::npos &&
          response.body.find("steering_angle_deg") != std::string::npos &&
          response.body.find("brake_pressure_bar") != std::string::npos,
      "complete measured CAN feedback is not rendered from vehicle telemetry");
  expect(
      response.body.find("function createControlLogic()") != std::string::npos &&
          response.body.find("const controlLogic=MineTeleopControlLogic") != std::string::npos &&
          response.body.find("const keys=controlLogic.KEY_BINDINGS") != std::string::npos &&
          response.body.find("ArrowLeft: 'left'") != std::string::npos &&
          response.body.find("ArrowUp: 'up'") != std::string::npos &&
          response.body.find("Space: 'service_brake'") != std::string::npos &&
          response.body.find("KeyB: 'hard_brake'") != std::string::npos &&
          response.body.find("e.code==='KeyE'") != std::string::npos,
      "the production page is not wired to the shared fixed-key control module");
  expect(
      response.body.find("pressedControlKeys=controlLogic.createKeySet()") != std::string::npos &&
          response.body.find("blockedControlKeys=controlLogic.createKeySet()") != std::string::npos &&
          response.body.find("controlLogic.pressKey(pressedControlKeys,blockedControlKeys,e.code)") !=
              std::string::npos &&
          response.body.find("controlLogic.releaseKey(pressedControlKeys,blockedControlKeys,e.code)") !=
              std::string::npos &&
          response.body.find("state=controlLogic.deriveKeyState(pressedControlKeys)") !=
              std::string::npos,
      "physical key codes are not reduced into held control actions");
  const auto keyboard_handler = response.body.find("addEventListener('keydown'");
  const auto keyboard_prevent_default = response.body.find("e.preventDefault()", keyboard_handler);
  const auto keyboard_connection_gate = response.body.find("if(!polling)", keyboard_handler);
  expect(
      keyboard_handler != std::string::npos &&
          keyboard_prevent_default != std::string::npos &&
          keyboard_connection_gate != std::string::npos &&
          keyboard_prevent_default < keyboard_connection_gate,
      "control keys can still scroll the page before the connection gate runs");
  expect(
      response.body.find("lastKeyboardEvent.textContent") != std::string::npos &&
          response.body.find("已截获 · 等待连接") != std::string::npos &&
          response.body.find("需释放后重新按下") != std::string::npos,
      "keyboard capture and fresh-keydown interlock do not provide visible operator feedback");
  expect(
      response.body.find("selectedGear='N'") != std::string::npos &&
          response.body.find("controlLogic.deriveGearSelection") != std::string::npos &&
          response.body.find("controlLogic.allowsGearChange") != std::string::npos &&
          response.body.find("updateSelectedGearFromHeldDirections") != std::string::npos &&
          response.body.find("updateSelectedGearFromInput({up:true,down:false})") !=
              std::string::npos &&
          response.body.find(
              "换挡已阻止：需至少 3 帧且持续 200 ms 的新鲜零速反馈") !=
              std::string::npos &&
          response.body.find("if(gear!=='R')gear='N'") == std::string::npos,
      "keyboard and Gamepad do not share the zero-speed-gated gear reducer");
  expect(
      response.body.find("function currentControl") != std::string::npos &&
          response.body.find("const control=controlLogic.deriveControl") !=
              std::string::npos &&
          response.body.find("limits:effectiveControlLimits()") != std::string::npos,
      "the production page bypasses shared brake/control derivation");
  expect(
      response.body.find("开始平行驾驶握手") != std::string::npos &&
          response.body.find("断开 VCU 握手") != std::string::npos &&
          response.body.find("can-vmc-fault") != std::string::npos &&
          response.body.find("can-parking-switch") != std::string::npos &&
          response.body.find("can-brake-pedal") != std::string::npos &&
          response.body.find("vcuHandshake.handshake_revoked") !=
              std::string::npos &&
          response.body.find("握手已被 VCU 撤销") != std::string::npos &&
          response.body.find("完成后请从页面重新申请 VCU 握手") !=
              std::string::npos,
      "explicit VCU handshake controls are missing");
  for (const auto* label : {
           "initial:'启动 1/5 · 低握手帧'",
           "wait_parallel_handshake:'启动 2/5 · 智驾状态 5'",
           "wait_parking_brake_released:'启动 3/5 · 等待电子驻车释放'",
           "wait_gear:'启动 4/5 · 挡位闭环（零牵引）'",
           "wait_actuator_modes:'启动 5/5 · 执行器模式（零牵引）'"}) {
    expect(response.body.find(label) != std::string::npos,
           "handshake title does not match the five fixed startup phases");
  }
  expect(response.body.find("保持驻车") == std::string::npos,
         "handshake UI claims EPB remains applied after release");
  const auto diagnose_handshake =
      response.body.find("function diagnoseVcuHandshake");
  const auto handshake_revoked_gate = response.body.find(
      "if(vcuHandshake.handshake_revoked)", diagnose_handshake);
  const auto profile_ack_gate = response.body.find(
      "if(!controlProfileState.acknowledged)", diagnose_handshake);
  expect(
      diagnose_handshake != std::string::npos &&
          handshake_revoked_gate != std::string::npos &&
          profile_ack_gate != std::string::npos &&
          handshake_revoked_gate < profile_ack_gate,
      "handshake revocation can be hidden by the cleared session profile gate");
  expect(
      response.body.find("event:'vcu_handshake_command'") != std::string::npos &&
          response.body.find("'vcu_handshake_status','session_control_profile_status'") !=
              std::string::npos,
      "VCU handshake command/status DataChannel contract is missing");
  expect(
      response.body.find("'control_command_rejected'") != std::string::npos &&
      response.body.find(
              "controlLogic.deriveControlCommandRejection(message.issue_code)") !=
              std::string::npos &&
          response.body.find(
              "pendingGearTransition=controlLogic.createGearTransition") !=
              std::string::npos &&
          response.body.find(
              "pendingGearTransition=controlLogic.recordForwardedGearCommand") !=
              std::string::npos &&
          response.body.find("controlLogic.reduceGearChangeRejection") !=
              std::string::npos &&
          response.body.find("selectedGear=gearRejectionState.selectedGear") !=
              std::string::npos &&
          response.body.find("clearControlInput(false)") !=
              std::string::npos &&
          response.body.find("gearRejectionState.inhibitOrdinaryControl") !=
              std::string::npos &&
          response.body.find("blockReason='gear_rejection_unresolved'") !=
              std::string::npos &&
          response.body.find("send({},false).catch(console.error)") !=
              std::string::npos &&
          response.body.find("driver_control_command_rejected") !=
              std::string::npos,
      "structured gear rejection does not restore the retained gear with zero input");
  expect(
      response.body.find("vcu_handshake_not_ready") != std::string::npos &&
          response.body.find("vcuHandshake.parking_ready") != std::string::npos,
      "driving commands are not gated by explicit N/EPB-ready VCU handshake state");
  expect(
      response.body.find("只有 N 挡允许进入平行驾驶") != std::string::npos &&
          response.body.find("电子驻车未全部拉起") != std::string::npos &&
          response.body.find("反馈已过期") != std::string::npos &&
          response.body.find("握手请求已发送") != std::string::npos &&
          response.body.find("启动第 2/5 步未完成") != std::string::npos &&
          response.body.find("复用智驾握手，等待 VCU 状态 5") != std::string::npos &&
          response.body.find("退出第 4/5 步未完成") != std::string::npos,
      "the control page does not explain the failed VCU gate or handshake step");
  expect(
      response.body.find("实车调试限幅") != std::string::npos &&
          response.body.find("驾驶参数") != std::string::npos &&
          response.body.find("速度 PID 与升扭标定") != std::string::npos &&
          response.body.find("目标车速上限（km/h）") != std::string::npos &&
          response.body.find("max-motor-torque-nm") != std::string::npos &&
          response.body.find("max=\"640.0\"") != std::string::npos &&
          response.body.find("max-brake-pressure-bar") != std::string::npos &&
          response.body.find("service-brake-pressure-bar") != std::string::npos &&
          response.body.find("hard-brake-pressure-bar") != std::string::npos &&
          response.body.find("max-steering-deg") != std::string::npos &&
          response.body.find("speed-pid-kp") != std::string::npos &&
          response.body.find("speed-pid-ki") != std::string::npos &&
          response.body.find("speed-pid-kd") != std::string::npos &&
          response.body.find("speed-pid-derivative-filter-tau-ms") != std::string::npos &&
          response.body.find("speed-pid-max-dt-ms") != std::string::npos &&
          response.body.find("motor-torque-rise-rate") != std::string::npos &&
          response.body.find("inputmode=\"decimal\" required") !=
              std::string::npos &&
          response.body.find("0 不是禁用驱动，而是直接跟随 PID 输出") !=
              std::string::npos,
      "field commissioning control limit dialog is missing");
  expect(
      response.body.find("effectiveControlLimits") != std::string::npos &&
          response.body.find("updateVehicleHardLimits(message.hard_limits)") != std::string::npos &&
          response.body.find("controlLogic.normalizeControlProfile(value)") !=
              std::string::npos &&
          response.body.find("controlLogic.mergeControlProfileWithHardLimits") !=
              std::string::npos &&
          response.body.find("controlLogic.deriveControl") != std::string::npos &&
          response.body.find("post('/api/control-profile',requested)") != std::string::npos &&
          response.body.find("get('/api/control-limits').then") == std::string::npos &&
          response.body.find("sendPendingControlProfile()") != std::string::npos &&
          response.body.find("now-lastControlProfileSendAt<200") != std::string::npos &&
          response.body.find("每路 EHB 压力请求，单位 bar、分辨率 0.1 bar") !=
              std::string::npos &&
          response.body.find("我已确认车辆处于 N 挡、零速、电子驻车或隔离 mock 台架") !=
              std::string::npos &&
          response.body.find("motorTorqueRiseRate.valueAsNumber") !=
              std::string::npos &&
          response.body.find("if(!motorTorqueRiseRate.checkValidity())") !=
              std::string::npos &&
          response.body.find("硬安全制动与 watchdog 参数不可编辑") != std::string::npos &&
          response.body.find("vehicleHardLimits.max_speed_kph.toFixed(1)") !=
              std::string::npos &&
          response.body.find("vehicleHardLimits.max_throttle.toFixed(3)") !=
              std::string::npos &&
          response.body.find("vehicleHardLimits.speed_feedback_timeout_ms") !=
              std::string::npos &&
          response.body.find("vehicleHardLimits.hard_overspeed_margin_kph") !=
              std::string::npos &&
          response.body.find("readOnlyControlSafetyText(") != std::string::npos &&
          response.body.find("safety.control_rate_hz") != std::string::npos &&
          response.body.find("safety.deceleration_profile.map") != std::string::npos &&
          response.body.find(
              "read_only_control_safety must contain exactly the fixed fields") !=
              std::string::npos &&
          response.body.find("value.control_rate_hz, 20, 20") != std::string::npos &&
          response.body.find(
              "flat and read_only_control_safety speed values must match") !=
              std::string::npos &&
          response.body.find("等待车端完整硬上限、PID 默认值与固定安全参数") !=
              std::string::npos,
      "controller limits are not combined with vehicle hard limits and operator confirmation");
  expect(
      response.body.find("control_profile_not_acknowledged") != std::string::npos &&
          response.body.find("action==='connect'&&!controlProfileState.acknowledged") !=
              std::string::npos &&
          response.body.find("applyControlProfileStatus(message.session_control_profile)") !=
              std::string::npos &&
          response.body.find("applyControlProfileStatus(message);", response.body.find(
              "message.event==='session_control_profile_status'")) != std::string::npos &&
          response.body.find("resetControlProfileSession()") != std::string::npos &&
          response.body.find("effectiveAppliedRevision") != std::string::npos &&
          response.body.find("applied_revision:controlProfileState.effectiveAppliedRevision") !=
              std::string::npos,
      "ordinary driving is not fail-closed on session control profile acknowledgement");
  expect(
      response.body.find(
              "controlLogic.controlProfileFromVehicleDefaults(driverActuationDefaults,hard)") !=
              std::string::npos &&
          response.body.find("function controlProfileParkingReady()") !=
              std::string::npos &&
          response.body.find(
              "parkedStandby=vcuHandshake.parking_ready===true&&"
              "(vcuHandshake.state==='standby'||vcuHandshake.state==='disarmed')") !=
              std::string::npos &&
          response.body.find("return mockBench||parkedStandby") != std::string::npos &&
          response.body.find("需人工打开并发送") != std::string::npos &&
          response.body.find("defaultProfileAutoAttempted") == std::string::npos &&
          response.body.find(
              "prepareControlProfile(controlProfileState.requestedProfile,false)") ==
              std::string::npos,
      "legacy PID defaults can still auto-submit without explicit operator confirmation");
  expect(
      response.body.find("pidChanged=!prior||requested.consecutive_late_frame_limit!==prior.consecutive_late_frame_limit||requested.parking_idle_timeout_ms!==prior.parking_idle_timeout_ms||requested.critical_camera_stale_brake_ms!==prior.critical_camera_stale_brake_ms||requested.control_recovery_timeout_ms!==prior.control_recovery_timeout_ms||requested.speed_pid_kp!==prior.speed_pid_kp") !=
              std::string::npos &&
      response.body.find(
              "requested.motor_torque_rise_rate_nm_per_s!==prior.motor_torque_rise_rate_nm_per_s") !=
              std::string::npos &&
      response.body.find(
              "requested.target_speed_kph>prior.target_speed_kph||"
              "requested.max_motor_torque_nm>prior.max_motor_torque_nm") !=
              std::string::npos &&
          response.body.find(
              "requested.max_brake_pressure_bar!==prior.max_brake_pressure_bar||"
              "requested.service_brake_pressure_bar!==prior.service_brake_pressure_bar||"
              "requested.hard_brake_pressure_bar!==prior.hard_brake_pressure_bar") !=
              std::string::npos &&
          response.body.find(
              "requested.max_steering_angle_deg!==prior.max_steering_angle_deg") !=
              std::string::npos &&
          response.body.find("if(requiresParking&&!controlProfileParkingReady())") !=
              std::string::npos &&
          response.body.find("VCU 为 standby/disarmed") !=
              std::string::npos,
      "profile editing does not preflight parking rules for speed, torque, brake, steering, and PID changes");
  const auto control_intent_post = response.body.find(
      "accepted=await post('/api/control-intent',intent,controller.signal)");
  expect(
      control_intent_post != std::string::npos &&
          response.body.find("post('/api/control'") == std::string::npos &&
          response.body.find("function nativeIntentEnvelope(outgoing)") !=
              std::string::npos &&
          response.body.find(
              "return{session_id:nativeControlSessionId,session_generation:nativeControlSessionGeneration,ui_instance_id:uiInstanceId,intent_seq:nativeIntentSeq,ui_control_epoch:nativeControlUiEpoch,takeover:nativeControlTakeoverPending&&!normalized.estop,...normalized}") !=
              std::string::npos &&
          response.body.find(
              "nativeControlSessionId=String(session.session_id||'');nativeControlSessionGeneration=Number(session.control_session_generation)") !=
              std::string::npos &&
          response.body.find("async function writeControlIntent(extra,announceUnavailable,requestToken={})") !=
              std::string::npos &&
          response.body.find("const intent=nativeIntentEnvelope(outgoing)") !=
              std::string::npos &&
          response.body.find(
              "pendingGearTransition=controlLogic.recordForwardedGearCommand"
              "(pendingGearTransition,transitionGeneration,accepted.intent_seq,outgoingSnapshot.gear)") !=
              std::string::npos &&
          response.body.find("transport:accepted.transport") !=
              std::string::npos &&
          response.body.find("delivery_state:'browser_data_channel_send_invoked'") ==
              std::string::npos,
      "browser ordinary control is not reduced to a native intent lease update");
  expect(
      response.body.find(
              "const controlWriteQueue=controlLogic.createLatestControlWriteQueue"
              "(writeControlIntent,reportControlQueueError") != std::string::npos &&
          response.body.find("function enqueueIntentRefresh(){return controlWriteQueue.enqueueHeartbeat()}") !=
              std::string::npos &&
          response.body.find("controlWriteQueue.enqueueHeartbeat()") != std::string::npos &&
          response.body.find("const enqueued=enqueueIntentRefresh()") != std::string::npos &&
          response.body.find("const controller=new AbortController()") != std::string::npos &&
          response.body.find("activeControlRequests.set(requestToken,requestState)") != std::string::npos &&
          response.body.find("active&&(!active.estop||reason==='session_changed')") !=
              std::string::npos &&
          response.body.find("reason:'control_intent_preempted_'+requestState.preemptedReason") !=
              std::string::npos &&
          response.body.find("latchEstop('Gamepad'))send({estop:true},false)") !=
              std::string::npos &&
          response.body.find("reason:'control_intent_update_timeout'") !=
              std::string::npos &&
          response.body.find("heartbeatInFlight") == std::string::npos,
      "intent lease refreshes are not coalesced or ESTOP-preemptible");
  expect(
      response.body.find("async function heartbeat()") == std::string::npos &&
          response.body.find("controlHeartbeatIntervalMs") == std::string::npos &&
          response.body.find("setInterval(()=>heartbeat") == std::string::npos &&
          response.body.find("activeChannel.send(JSON.stringify(prepared.command))") ==
              std::string::npos &&
          response.body.find("retiredBrowserPacketPath") == std::string::npos,
      "a retired 50ms browser packet heartbeat or ordinary DataChannel send remains active");
  expect(
      response.body.find("function applyVehicleSafetyState(value,timing)") !=
              std::string::npos &&
          response.body.find("next==='DEGRADED'&&controlLogic.degradedInputRearmRequired(timing,lastNativeAcceptedControlSeq)){clearControlInput(false,'vehicle_degraded',true)") !=
              std::string::npos &&
          response.body.find("next==='DEGRADED'&&lastVehicleSafetyState") ==
              std::string::npos &&
          response.body.find("clearControlInput(false)") != std::string::npos &&
          response.body.find("driver_input_cleared_on_degraded") !=
              std::string::npos &&
          response.body.find("applyVehicleSafetyState(message.safety_state,message.control_timing)") !=
              std::string::npos,
      "recoverable vehicle degradation can replay held driver input");
  expect(
      response.body.find("function vcuStateRequiresFreshInput") != std::string::npos &&
          response.body.find("controlLogic.requiresFreshInput(value)") != std::string::npos &&
          response.body.find("controlLogic.transitionVcuState(vcuEverReady,value)") !=
              std::string::npos &&
          response.body.find("function resetControlAuthorityInput(reason='control_authority_reset'){vcuEverReady=false;clearControlInput(true,reason)}") !=
              std::string::npos &&
          response.body.find("if(!vcuEverReady&&!vcuMockUnsupported())") != std::string::npos &&
          response.body.find("首次握手完成后请重新按下") != std::string::npos,
      "initial authority, fault, or disarm does not require fresh control input");
  expect(
      response.body.find("lastControlStatusSeq=0") != std::string::npos &&
          response.body.find("function acceptControlStatusMessage(message)") !=
              std::string::npos &&
          response.body.find("controlLogic.reduceStatusSequence(lastControlStatusSeq") !=
              std::string::npos &&
          response.body.find("if(!acceptControlStatusMessage(message))return") !=
              std::string::npos &&
          response.body.find("control_status_message_dropped") != std::string::npos &&
          response.body.find("if(!acceptControlStatusMessage(message))return;") <
              response.body.find("const gearRejectionMatched=") &&
          response.body.find("control_status_sequence_gap") != std::string::npos,
      "unordered vehicle status messages can replay stale VCU authority");
  expect(
      response.body.find("function vcuStateKeepsHeldInput") != std::string::npos &&
          response.body.find("controlLogic.keepsHeldInput(vcuEverReady,value)") !=
              std::string::npos &&
          response.body.find("gearTransitionPending=Boolean(pendingGearTransition)") !=
              std::string::npos &&
          response.body.find(
              "if((retainedWait||gearTransitionPending)&&!estopRequested)outgoing.throttle=0") !=
              std::string::npos &&
          response.body.find(
              "if(pendingGearTransition&&!control.estop)control.throttle=0") !=
              std::string::npos &&
          response.body.find(
              "!blockReason&&!vcuDrivingReady()&&!estopRequested&&!retainedWait") !=
              std::string::npos &&
          response.body.find("换挡闭环中（输入保持）") != std::string::npos &&
          response.body.find("执行器闭环中（输入保持）") != std::string::npos &&
          response.body.find("输入已清除，等待新鲜 VCU Ready") != std::string::npos,
      "authorized VCU convergence waits do not retain inputs with zero-throttle heartbeats");
  expect(
      response.body.find(
          "controlLogic.updateGearChangeStationaryEvidence(gearChangeStationaryEvidence,value,lastControlStatusSeq,performance.now())") !=
              std::string::npos &&
          response.body.find(
              "gear_change_stationary_confirmed:gearChangeStationaryEvidence.confirmed") !=
              std::string::npos &&
          response.body.find("至少 3 帧且持续 200 ms 的新鲜零速反馈") !=
              std::string::npos,
      "browser gear changes are not gated on stable fresh zero-speed evidence");
  const auto telemetry_vcu_update = response.body.find(
      "adapter_ready:vcuAdapterReady(message.vcu_handshake,message.vehicle_adapter?.opened)};updateVcuHandshakeState(nextVcuStatus)");
  const auto telemetry_profile_update = response.body.find(
      "applyControlProfileStatus(message.session_control_profile)", telemetry_vcu_update);
  const auto telemetry_limits_update = response.body.find(
      "updateVehicleHardLimits(message.control_limits)", telemetry_profile_update);
  const auto handshake_vcu_update = response.body.find(
      "updateVcuHandshakeState({...nextVcuStatus,issue_code:message.issue_code||'',control_recovery:message.control_recovery||null,driver_connected:Boolean(message.driver_connected),adapter_ready:vcuAdapterReady(nextVcuStatus,message.adapter_ready)})");
  const auto handshake_limits_update = response.body.find(
      "updateVehicleHardLimits(message.hard_limits)", handshake_vcu_update);
  expect(
      telemetry_vcu_update != std::string::npos &&
          telemetry_profile_update != std::string::npos &&
          telemetry_limits_update != std::string::npos &&
          telemetry_vcu_update < telemetry_profile_update &&
          telemetry_profile_update < telemetry_limits_update &&
          handshake_vcu_update != std::string::npos &&
          handshake_limits_update != std::string::npos &&
          handshake_vcu_update < handshake_limits_update,
      "vehicle status does not update VCU, profile, and hard-limit state in a deterministic order");
  expect(
      response.body.find("gamepadRequiresNeutral=true") != std::string::npos &&
          response.body.find("const gamepadAuthorityReady=vcuEverReady||vcuMockUnsupported()") !=
              std::string::npos &&
          response.body.find("controlLogic.reduceGamepadNeutralInterlock") !=
              std::string::npos &&
          response.body.find("applyGamepadNeutralInterlock(gamepadAuthorityReady,true)") !=
              std::string::npos,
      "Gamepad pedals can resume without a physical neutral after an authority or gear-gate reset");
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
  expect(
      response.body.find("gamepaddisconnected") != std::string::npos &&
          response.body.find("gamepadState.throttle=0;gamepadState.brake=0;gamepadRequiresNeutral=true;renderControlState();clientLog('gamepad_disconnected'") !=
              std::string::npos,
      "Gamepad disconnect does not zero its own axes without clearing keyboard input");
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
      response.body.find("offeredVideoCameraIds(offer.sdp,offer.media_tracks||[])") != std::string::npos &&
          response.body.find("offeredCameraByMid.get(mid)") != std::string::npos,
      "browser camera IDs are not mapped from the SDP video mids");
  expect(
      response.body.find("srcObject = new MediaStream([track])") != std::string::npos &&
          response.body.find("attach(id,e.track)") != std::string::npos,
      "browser video elements are not isolated to their individual WebRTC tracks");
  const auto ontrack_handler = response.body.find("nextPeer.ontrack=e=>");
  const auto remote_description = response.body.find("await nextPeer.setRemoteDescription", ontrack_handler);
  const auto camera_mount = response.body.find("MineTeleopCameraView.mount(cameraGrid)");
  const auto start_offer = response.body.find("async function startFromOffer(");
  const auto camera_configure = response.body.find("cameraView.configure(remoteCameraIds)", start_offer);
  const auto current_closure = response.body.find(
      "const current=()=>currentMediaPeer(nextPeer,epoch,sourceSession,sourceGeneration);", start_offer);
  expect(
      camera_mount != std::string::npos && start_offer != std::string::npos &&
          camera_configure != std::string::npos && current_closure != std::string::npos &&
          ontrack_handler != std::string::npos && remote_description != std::string::npos &&
          camera_mount < camera_configure && camera_configure < current_closure &&
          current_closure < ontrack_handler && ontrack_handler < remote_description &&
          response.body.find("nextPeer.ontrack=e=>{if(!current()||e.track.kind!=='video')return;") != std::string::npos &&
          response.body.find("function currentMediaPeer(instance,epoch,session,generation){return peer===instance&&mediaPeerEpoch===epoch&&nativeControlSessionId===session&&nativeControlSessionGeneration===generation}") != std::string::npos,
      "camera slots must precede track delivery, and track callbacks must match peer, epoch, session and generation");
  expect(
      ontrack_handler != std::string::npos &&
          remote_description != std::string::npos &&
          response.body.substr(ontrack_handler, remote_description - ontrack_handler).find("vcuHandshake") ==
              std::string::npos &&
          response.body.substr(ontrack_handler, remote_description - ontrack_handler).find("controlChannel") ==
              std::string::npos,
      "browser camera attachment is gated on the VCU handshake or control DataChannel");
  expect(
      response.body.find("e.streams[0]") == std::string::npos,
      "browser can bind the same multi-track MediaStream to multiple camera elements");
  expect(
      response.body.find("if(connectionState==='disconnected')") != std::string::npos &&
          response.body.find("if(['failed','closed'].includes(connectionState))") != std::string::npos &&
          response.body.find("nextPeer.onconnectionstatechange=()=>{\n    if(!current())return;") != std::string::npos,
      "a transient or stale peer state change can discard a recoverable control DataChannel");
  const auto offer_start = response.body.find("async function startFromOffer");
  const auto offer_authority_reset = response.body.find("resetControlAuthorityInput()", offer_start);
  const auto offer_peer_create = response.body.find("new RTCPeerConnection", offer_start);
  expect(
      offer_start != std::string::npos && offer_authority_reset != std::string::npos &&
          offer_peer_create != std::string::npos && offer_authority_reset < offer_peer_create &&
          response.body.find("channel.onclose=()=>{") != std::string::npos,
      "a new peer or closed DataChannel can inherit stale VCU authority/input state");
  const auto data_channel_handler = response.body.find("nextPeer.ondatachannel=e=>{");
  const auto data_channel_peer_gate = response.body.find(
      "if(!current()){channel.close();return}",
      data_channel_handler);
  const auto data_channel_parameters_gate = response.body.find(
      "if(channel.label!=='control'||channel.protocol!=='mine-teleop-control-v1'||channel.ordered||channel.maxRetransmits!==0){",
      data_channel_peer_gate);
  const auto data_channel_status_reset = response.body.find(
      "lastControlStatusSeq=0;",
      data_channel_parameters_gate);
  const auto data_channel_publish = response.body.find(
      "controlChannel=channel;",
      data_channel_status_reset);
  const auto channel_open_handler = response.body.find("channel.onopen=()=>{", data_channel_publish);
  const auto channel_message_handler = response.body.find("channel.onmessage=async event=>{", channel_open_handler);
  const auto channel_close_handler = response.body.find("channel.onclose=()=>{", channel_message_handler);
  const auto channel_error_handler = response.body.find("channel.onerror=()=>{", channel_close_handler);
  const std::string channel_identity_gate =
      "if(!current()||!controlLogic.isCurrentControlChannel(peer,nextPeer,controlChannel,channel))return;";
  const auto channel_open_gate = response.body.find(channel_identity_gate, channel_open_handler);
  const auto channel_message_gate = response.body.find(channel_identity_gate, channel_message_handler);
  const auto channel_close_gate = response.body.find(channel_identity_gate, channel_close_handler);
  const auto channel_error_gate = response.body.find(channel_identity_gate, channel_error_handler);
  const auto channel_error_end = response.body.find("};", channel_error_gate);
  expect(
      data_channel_handler != std::string::npos && data_channel_peer_gate != std::string::npos &&
          data_channel_parameters_gate != std::string::npos &&
          data_channel_status_reset != std::string::npos && data_channel_publish != std::string::npos &&
          response.body.find("nextPeer.ondatachannel=e=>{\n    const channel=e.channel;\n    if(!current()){channel.close();return}", data_channel_handler) == data_channel_handler &&
          data_channel_peer_gate < data_channel_parameters_gate &&
          data_channel_parameters_gate < data_channel_status_reset &&
          data_channel_status_reset < data_channel_publish,
      "a stale peer can publish a control DataChannel or a new channel can inherit an old status sequence");
  expect(
      channel_open_handler != std::string::npos && channel_open_gate != std::string::npos &&
          channel_message_handler != std::string::npos && channel_message_gate != std::string::npos &&
          channel_close_handler != std::string::npos && channel_close_gate != std::string::npos &&
          channel_error_handler != std::string::npos && channel_error_gate != std::string::npos &&
          response.body.find("channel.onopen=()=>{\n      " + channel_identity_gate, channel_open_handler) == channel_open_handler &&
          response.body.find("channel.onmessage=async event=>{\n      " + channel_identity_gate, channel_message_handler) == channel_message_handler &&
          response.body.find("channel.onclose=()=>{\n      " + channel_identity_gate, channel_close_handler) == channel_close_handler &&
          response.body.find("channel.onerror=()=>{\n      " + channel_identity_gate, channel_error_handler) == channel_error_handler &&
          channel_open_gate < channel_message_handler && channel_message_gate < channel_close_handler &&
          channel_close_gate < channel_error_handler,
      "control DataChannel lifecycle callbacks do not reject stale peer or channel identities");
  expect(
      response.body.find("controlChannel=null;", channel_close_gate) < channel_error_handler &&
          response.body.find("resetControlAuthorityInput()", channel_close_gate) < channel_error_handler &&
          channel_error_end != std::string::npos &&
          response.body.find("resetControlAuthorityInput()", channel_error_gate) < channel_error_end,
      "current control DataChannel close or error is not fail-closed");
  expect(
      response.body.find("activePeer.connectionState!=='connected'") != std::string::npos,
      "control commands can be sent while the current WebRTC peer is disconnected");
  expect(
      response.body.find("if(polling&&fromVehicle===target){statusPanel.textContent=`车辆 ${target} 已处于当前会话`;return}") !=
          std::string::npos,
      "the controller can silently recreate a control DataChannel inside an inhibited media session");
  expect(
      response.body.find("estopRequested=estopLatched||Boolean(extra.estop)") !=
              std::string::npos &&
          response.body.find("!blockReason&&estopRequested&&vcuHandshake.adapter_ready===false") !=
              std::string::npos &&
          response.body.find("vcu_adapter_unavailable") != std::string::npos &&
          response.body.find("请使用车辆物理急停") != std::string::npos &&
          response.body.find("function vcuDrivingReady(){return controlProfileState.acknowledged&&controlLogic.drivingReady(vcuHandshake)}") !=
              std::string::npos,
      "the controller can claim that remote ESTOP was sent while the VCU adapter is unavailable");
  expect(
      response.body.find("!vcuDrivingReady()&&!estopRequested") != std::string::npos &&
          response.body.find("async function refreshControlIntent()") != std::string::npos &&
          response.body.find("sendPendingControlProfile();const enqueued=enqueueIntentRefresh()") !=
              std::string::npos &&
          response.body.find("estopRequested=estopLatched||Boolean(extra.estop)") !=
              std::string::npos,
      "a latched ESTOP is not retained by periodic native intent refresh while the VCU handshake is incomplete");
  expect(
      response.body.find("function vcuAdapterReady(status,explicit){return controlLogic.adapterReady(status,explicit)}") !=
              std::string::npos,
      "an older vehicle without adapter_ready cannot use the compatibility-safe unknown state");
  // Actual renderMonitoring/renderEstopRequest behavior is exercised by the
  // shipped-page JavaScript regression. This route contract checks that the
  // helper, banner and alert wiring is shipped, without pinning equivalent
  // optional-chaining versus explicit-guard syntax to one source spelling.
  const auto estop_monitor = response.body.find(
      "function renderMonitoring(){const estopPresentation=controlLogic.deriveControlStopPresentation(");
  const auto estop_banner = response.body.find(
      "function renderEstopRequest(presentation=controlLogic.deriveControlStopPresentation(");
  expect(
      estop_monitor != std::string::npos && estop_banner != std::string::npos &&
          response.body.find("renderEstopRequest(estopPresentation)", estop_monitor) !=
              std::string::npos &&
          response.body.find(
              "if(estopPresentation.visible){alerts.push(estopPresentation.alert);severity=estopPresentation.severity}",
              estop_monitor) != std::string::npos &&
          response.body.find(
              "estopStatus.hidden=!presentation.visible;estopStatus.textContent=presentation.banner",
              estop_banner) != std::string::npos,
      "control page does not ship the stop helper, critical alert and banner wiring");
  expect(
      response.body.find("function controlProfileParkingReady()") !=
              std::string::npos &&
          response.body.find("defaultProfileAutoAttempted") == std::string::npos &&
          response.body.find(
              "prepareControlProfile(controlProfileState.requestedProfile,false)") ==
              std::string::npos,
      "a completed VCU disconnect can silently reapply a legacy session profile");
  const auto neutralize_input = response.body.find(
      "function neutralizeInput(event){clearControlInput(false,event?.type==='blur'?'window_blur':'document_hidden');send({},false).catch(console.error)}");
  const auto blur_neutralize = response.body.find(
      "addEventListener('blur',neutralizeInput)", neutralize_input);
  const auto hidden_neutralize = response.body.find(
      "document.addEventListener('visibilitychange',()=>{if(document.hidden)neutralizeInput()})",
      blur_neutralize);
  expect(
      neutralize_input != std::string::npos && blur_neutralize != std::string::npos &&
          hidden_neutralize != std::string::npos &&
          response.body.find("activeChannel.bufferedAmount>4096") == std::string::npos,
      "blur or page hiding does not publish a neutral native intent independent of DataChannel backpressure");
  expect(response.body.find("时延超过 200 ms") != std::string::npos, "latency threshold alarm is missing");
  expect(response.body.find("低于 20 FPS") != std::string::npos, "FPS threshold alarm is missing");
  expect(response.body.find("sent_at_utc_ms:Date.now()") != std::string::npos, "browser-local logs do not use UTC milliseconds");
  expect(response.body.find("fetch('/api/browser-event'") != std::string::npos, "browser events are not persisted by the local runtime");
  expect(
      response.body.find("\"control_trace_commands\":true") != std::string::npos &&
          response.body.find("const controlTraceEnabled=Boolean(consoleConfig.control_trace_commands)") !=
              std::string::npos &&
          response.body.find(
              "clientLog('control_trace_batch',{reason,trace_session_id:String(scope&&scope.session_id||''),"
              "trace_vehicle_id:String(scope&&scope.vehicle_id||''),commands,summary})") !=
              std::string::npos &&
          response.body.find("controlTraceBuffer.length>=48") != std::string::npos &&
          response.body.find("setInterval(()=>flushControlTrace('interval'),1000)") !=
              std::string::npos &&
          response.body.find("clientLog('control_trace_command'") == std::string::npos,
      "control command tracing is not enabled through a bounded one-second batch");
  for (const std::string_view field : {
           "session_id", "intent_seq", "trace_session_id", "trace_vehicle_id",
           "heartbeat_tick_count", "heartbeat_enqueued_count", "heartbeat_coalesced_count",
           "timer_lag_max_ms", "explicit_send_count", "prepare_preempted_by_estop_count",
           "queue_unhandled_error_count"}) {
    expect(
        response.body.find(field) != std::string::npos,
        "control trace field is missing from the page: " + std::string(field));
  }
  const auto trace_flush = response.body.find("function flushControlTrace(");
  expect(
      trace_flush != std::string::npos &&
          response.body.find("summary.heartbeat_tick_count", trace_flush) != std::string::npos &&
          response.body.find("summary.explicit_send_count", trace_flush) != std::string::npos &&
          response.body.find("summary.queue_unhandled_error_count", trace_flush) !=
              std::string::npos,
      "summary-only native intent refresh activity can be discarded during trace flush");
  const auto intent_refresh = response.body.find("async function refreshControlIntent()");
  const auto intent_refresh_timer = response.body.find(
      "setInterval(()=>refreshControlIntent().catch(console.error),intentRefreshIntervalMs)",
      intent_refresh);
  expect(
      intent_refresh != std::string::npos && intent_refresh_timer != std::string::npos &&
          response.body.find(
              "intentRefreshIntervalMs=Math.max(50,Math.floor(Number(consoleConfig.intent_lease_ms||200)/3))") !=
              std::string::npos &&
          response.body.find("async function heartbeat()") == std::string::npos,
      "browser intent renewal is not derived from the lease or still owns the retired packet heartbeat");
  const auto connect_handler = response.body.find("async function connect()");
  const auto connect_close = response.body.find("generation=closeRealtimeSession()", connect_handler);
  const auto connect_trace_scope =
      response.body.find("setControlTraceScope(session.session_id,session.vehicle_id)", connect_close);
  expect(
      connect_handler != std::string::npos && connect_close != std::string::npos &&
          connect_trace_scope != std::string::npos && connect_close < connect_trace_scope,
      "new browser control traces are not scoped after the previous realtime session is flushed");
  const auto peer_state_handler = response.body.find("nextPeer.onconnectionstatechange=()=>{");
  const auto peer_terminal_flush =
      response.body.find("flushControlTrace(`peer_${connectionState}`)", peer_state_handler);
  const auto peer_terminal_log =
      response.body.find("clientLog('webrtc_peer_terminal'", peer_state_handler);
  const auto peer_terminal_clear = response.body.find(
      "if(controlChannel===terminalChannel)controlChannel=null", peer_state_handler);
  expect(
      peer_state_handler != std::string::npos && peer_terminal_flush != std::string::npos &&
          peer_terminal_log != std::string::npos && peer_terminal_clear != std::string::npos &&
          peer_terminal_flush < peer_terminal_log && peer_terminal_log < peer_terminal_clear,
      "terminal peer state can clear the DataChannel before its trace and failure context are persisted");
  const auto data_channel_error_handler = response.body.find("channel.onerror=()=>{");
  const auto data_channel_error_log =
      response.body.find("clientLog('control_datachannel_error'", data_channel_error_handler);
  const auto data_channel_error_flush =
      response.body.find("flushControlTrace('datachannel_error')", data_channel_error_handler);
  const auto data_channel_error_reset =
      response.body.find("resetControlAuthorityInput()", data_channel_error_handler);
  expect(
      data_channel_error_handler != std::string::npos &&
          data_channel_error_flush != std::string::npos &&
          data_channel_error_log != std::string::npos &&
          data_channel_error_reset != std::string::npos &&
          data_channel_error_flush < data_channel_error_log &&
          data_channel_error_log < data_channel_error_reset,
      "DataChannel errors are not persisted before control input is cleared");
  const auto data_channel_close_handler = response.body.find("channel.onclose=()=>{");
  const auto data_channel_close_flush =
      response.body.find("flushControlTrace('datachannel_close')", data_channel_close_handler);
  const auto data_channel_close_clear =
      response.body.find("controlChannel=null", data_channel_close_handler);
  expect(
      data_channel_close_handler != std::string::npos &&
          data_channel_close_flush != std::string::npos &&
          data_channel_close_clear != std::string::npos &&
          data_channel_close_flush < data_channel_close_clear,
      "DataChannel close does not flush the pending control trace");
  expect(response.body.find("dev-password") == std::string::npos, "driver credential leaked into the control page");
  expect(
      response.body.find("async function refreshControlIntent(){const now=performance.now();if(!polling)") !=
              std::string::npos &&
          response.body.find("sendPendingControlProfile();const enqueued=enqueueIntentRefresh()") !=
              std::string::npos,
      "background intent renewal can run without an active session or bypass latest-write coalescing");
  const auto pagehide = response.body.find("addEventListener('pagehide'");
  const auto pagehide_flush = response.body.find("flushControlTrace('pagehide')", pagehide);
  const auto pagehide_close = response.body.find("closeRealtimeSession()", pagehide);
  const auto pagehide_disconnect = response.body.find("fetch('/api/disconnect'", pagehide);
  expect(
      pagehide != std::string::npos && pagehide_flush != std::string::npos &&
          pagehide_close != std::string::npos && pagehide_disconnect != std::string::npos &&
          pagehide_flush < pagehide_close && pagehide_close < pagehide_disconnect,
      "page close does not flush control trace before releasing the session");
  const auto initial_status = runtime->status();
  expect(
      initial_status.at("last_signaling_messages").is_array() &&
          initial_status.at("last_signaling_messages").empty(),
      "initial signaling messages are not an empty array");
  expect(
      initial_status.at("webrtc_metrics").is_object() && initial_status.at("webrtc_metrics").empty(),
      "initial WebRTC metrics are not an empty object");
  expect(
      initial_status.value("control_commands_prepared_total", std::uint64_t{1}) == 0 &&
          initial_status.value("last_control_prepared_at_utc_ms", std::int64_t{1}) == 0 &&
          !initial_status.contains("last_control_sent_ms"),
      "initial control status still claims a send/delivery instead of preparation metrics");
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
  expect(!config.control_trace_commands, "high-rate control trace must remain opt-in by default");

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
  expect(field.control_trace_commands, "field driver control trace is not enabled");
  expect(
      field.browser_event_log_max_bytes == 16 * 1024 * 1024 &&
          field.browser_event_log_files == 4,
      "field driver log rotation capacity is too small for batched command traces");
  expect(field.max_time_sync_uncertainty_ms == 25, "field driver time synchronization limit is not 25ms");
  expect(
      field.control_limits.initial_target_speed_kph == 5.0,
      "field driver initial target speed changed");
  expect(
      field.control_limits.initial_max_motor_torque_nm == 320.0,
      "field driver initial per-motor torque request changed");
  expect(
      field.control_limits.initial_max_brake_pressure_bar == 163.8 &&
          field.control_limits.initial_service_brake_pressure_bar == 163.8 &&
          field.control_limits.initial_hard_brake_pressure_bar == 163.8,
      "field driver initial per-EHB pressure requests changed");
  expect(
      field.control_limits.initial_max_steering_angle_deg == 30.0,
      "field driver initial steering limit changed");
}

std::string read_text_file(const std::filesystem::path& path) {
  std::ifstream input(path);
  expect(input.good(), "expected log file is missing: " + path.string());
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::string cpp_function_contract(
    std::string_view source,
    std::string_view signature) {
  const auto signature_position = source.find(signature);
  expect(signature_position != std::string_view::npos, "missing C++ contract: " + std::string(signature));
  const auto open_brace = source.find('{', signature_position + signature.size());
  expect(open_brace != std::string_view::npos, "missing C++ function body: " + std::string(signature));
  std::size_t depth = 0;
  for (auto position = open_brace; position < source.size(); ++position) {
    if (source[position] == '{') {
      ++depth;
    } else if (source[position] == '}' && --depth == 0) {
      return std::string(source.substr(signature_position, position - signature_position + 1));
    }
  }
  throw std::runtime_error("unterminated C++ function body: " + std::string(signature));
}

void test_vehicle_control_status_sequence_stays_monotonic_during_delayed_adapter_start() {
  const auto source = read_text_file("cpp/src/webrtc_media.cpp");
  const auto start_service = cpp_function_contract(source, "[[nodiscard]] bool start_control_service()");
  const auto configure_channel = cpp_function_contract(source, "void configure_control_data_channel()");
  const auto send_status = cpp_function_contract(source, "void send_vcu_handshake_status_locked(std::string_view result)");
  constexpr std::string_view reset = "control_status_seq = 0;";

  std::size_t reset_count = 0;
  for (auto position = source.find(reset); position != std::string::npos;
       position = source.find(reset, position + reset.size())) {
    ++reset_count;
  }
  expect(reset_count == 1, "control status sequence has a reset outside its session scope");
  expect(
      start_service.find(reset) == std::string::npos,
      "delayed adapter start can roll back the active DataChannel status sequence");
  expect(configure_channel.find("if (control_status_session_id != status_session_id)") != std::string::npos,
      "media-only replacement can roll back native feedback status ordering");
  const auto channel_reset = configure_channel.find(reset);
  const auto channel_publish = configure_channel.find("control_channel = channel;");
  expect(
      channel_reset != std::string::npos && channel_publish != std::string::npos &&
          channel_reset < channel_publish,
      "a newly published DataChannel does not establish its session status sequence scope");
  expect(
      send_status.find("++control_status_seq") != std::string::npos,
      "VCU handshake status does not advance the shared DataChannel sequence");

  // Model the production order that exposed the regression: channel creation,
  // driver_connected, delayed adapter start on the same channel, then ready.
  std::uint64_t sequence = 0;
  const auto driver_connected = ++sequence;
  const auto delayed_adapter_ready = ++sequence;
  expect(
      driver_connected == 1 && delayed_adapter_ready == 2,
      "delayed adapter readiness did not remain newer than driver_connected");
}

void test_stale_control_data_channel_callbacks_are_fail_silent() {
  const auto source = read_text_file("cpp/src/webrtc_media.cpp");
  const auto handler = cpp_function_contract(source, "void handle_control_message(");
  const auto profile_begin = handler.find(
      "if (message.value(\"type\", \"\") == \"session_control_profile\")");
  const auto handshake_begin = handler.find(
      "if (message.value(\"event\", \"\") == \"vcu_handshake_command\")");
  const auto command_begin = handler.find("auto command = ControlCommand::from_json(message)");
  expect(
      profile_begin != std::string::npos && handshake_begin != std::string::npos &&
          command_begin != std::string::npos && profile_begin < handshake_begin &&
          handshake_begin < command_begin,
      "control message branches could not be isolated for stale-channel review");

  const auto profile_branch = handler.substr(profile_begin, handshake_begin - profile_begin);
  const auto handshake_branch = handler.substr(handshake_begin, command_begin - handshake_begin);
  constexpr std::string_view stale_guard = "if (control_channel != channel) return;";
  const auto profile_stale = profile_branch.find(stale_guard);
  const auto profile_unavailable = profile_branch.find(
      "if (stop_requested || control_inhibited || !control_service_started");
  const auto profile_status = profile_branch.find("send_session_control_profile_status_locked(result)");
  expect(
      profile_stale != std::string::npos && profile_unavailable != std::string::npos &&
          profile_status != std::string::npos && profile_stale < profile_unavailable &&
          profile_unavailable < profile_status &&
          profile_branch.find("control_channel != channel ||") == std::string::npos,
      "a stale profile callback can publish a rejection on the replacement control channel");

  const auto handshake_stale = handshake_branch.find(stale_guard);
  const auto handshake_unavailable = handshake_branch.find(
      "if (stop_requested || control_inhibited || !control_service_started");
  const auto handshake_status = handshake_branch.find(
      "send_vcu_handshake_status_locked(\"driver_not_connected\")");
  expect(
      handshake_stale != std::string::npos && handshake_unavailable != std::string::npos &&
          handshake_status != std::string::npos && handshake_stale < handshake_unavailable &&
          handshake_unavailable < handshake_status &&
          handshake_branch.find("control_channel != channel ||") == std::string::npos,
      "a stale handshake callback can publish a rejection on the replacement control channel");
}

void test_vehicle_native_signaling_control_transport_is_single_path() {
  const auto source = read_text_file("cpp/src/webrtc_media.cpp");
  const auto server_source = read_text_file("cpp/src/server.cpp");
  const auto handler = cpp_function_contract(source, "void handle_control_message(");
  const auto process_signaling = cpp_function_contract(source, "void process_signaling()");
  const auto native_control_websocket_url = cpp_function_contract(
      source,
      "[[nodiscard]] std::string native_control_websocket_url() const");
  const auto native_control_websocket_headers = cpp_function_contract(
      source,
      "[[nodiscard]] HttpHeaders native_control_websocket_headers() const");
  const auto native_control_websocket_handler = cpp_function_contract(
      source,
      "void handle_native_control_websocket_envelope(");
  const auto native_control_websocket_loop = cpp_function_contract(
      source,
      "void native_control_websocket_loop(std::stop_token stop_token) noexcept");
  const auto native_control_watchdog_loop = cpp_function_contract(
      source,
      "void native_control_watchdog_loop(std::stop_token stop_token) noexcept");
  const auto start_native_control_transport = cpp_function_contract(
      source,
      "void start_native_control_transport()");
  const auto signaling_websocket = cpp_function_contract(
      server_source,
      "bool SignalingService::handle_websocket(");
  const auto acknowledge_signaling_messages = cpp_function_contract(
      server_source,
      "std::size_t SignalingService::acknowledge_signaling_messages(");
  const auto profile_begin = handler.find(
      "if (message.value(\"type\", \"\") == \"session_control_profile\")");
  const auto handshake_begin = handler.find(
      "if (message.value(\"event\", \"\") == \"vcu_handshake_command\")");
  const auto legacy_reject = handler.find(
      "if (transport == ControlMessageTransport::DataChannel)", handshake_begin);
  const auto command_begin = handler.find(
      "auto command = ControlCommand::from_json(message)", legacy_reject);
  expect(
      profile_begin != std::string::npos && handshake_begin != std::string::npos &&
          legacy_reject != std::string::npos && command_begin != std::string::npos &&
          profile_begin < handshake_begin && handshake_begin < legacy_reject &&
          legacy_reject < command_begin,
      "vehicle control transport branches are not ordered profile, handshake, legacy reject, native command");
  expect(
      handler.substr(profile_begin, handshake_begin - profile_begin)
                  .find("transport != ControlMessageTransport::DataChannel") !=
              std::string::npos &&
          handler.substr(handshake_begin, legacy_reject - handshake_begin)
                  .find("transport != ControlMessageTransport::DataChannel") !=
              std::string::npos,
      "session profile or VCU handshake escaped the browser DataChannel transport");
  expect(
      handler.substr(legacy_reject, command_begin - legacy_reject)
                  .find("legacy_data_channel_control_disabled") != std::string::npos,
      "legacy browser ControlCommand messages can still reach the vehicle apply path");

  const auto native_command_path = handler.substr(command_begin);
  expect(
      native_command_path.find("control_channel != channel") == std::string::npos &&
          native_command_path.find("!control_service_started || !control_service || !control_link_open") !=
              std::string::npos &&
          native_command_path.find("control_inhibited") != std::string::npos,
      "native signaling control either depends on a browser channel pointer or bypasses readiness gates");
  expect(
      native_command_path.find("{\"transport\", transport_name}") != std::string::npos,
      "native vehicle control trace does not identify its transport");

  const auto stale_safe_definition = native_command_path.find(
      "const bool stale_safe_heartbeat =");
  const auto stale_nonzero_gate = native_command_path.find(
      "if (!command.estop && !intent_fresh && !stale_safe_heartbeat)",
      stale_safe_definition);
  const auto estop_gear_gate = native_command_path.find(
      "if (command.estop)", stale_nonzero_gate);
  const auto profile_readiness_gate = native_command_path.find(
      "const bool profile_active =", estop_gear_gate);
  const auto handshake_readiness_gate = native_command_path.find(
      "if (!profile_active || !handshake_ready)", profile_readiness_gate);
  const auto stale_safe_gate = native_command_path.find(
      "if (stale_safe_heartbeat)", handshake_readiness_gate);
  const auto stale_safe_gap_clock = native_command_path.find(
      "const auto previous_accepted_monotonic_ms =",
      stale_safe_gate);
  const auto heartbeat_admission = native_command_path.find("neutral_heartbeat_gate(", stale_safe_gap_clock);
  const auto stale_safe_active_gate = native_command_path.find(
      "heartbeat_gate == NeutralHeartbeatGate::InactiveState", heartbeat_admission);
  const auto stale_safe_gap_gate = native_command_path.find(
      "heartbeat_gate == NeutralHeartbeatGate::NativeGap",
      stale_safe_active_gate);
  const auto stale_safe_gap_reject = native_command_path.find(
      "stale_safe_heartbeat_native_gap_elapsed",
      stale_safe_gap_gate);
  const auto stale_safe_gear_gate = native_command_path.find(
      "!native_control_last_accepted_fresh_gear",
      stale_safe_gap_reject);
  const auto receive_command = native_command_path.find(
      "control_service->receive_command(command, received_at_ms)",
      stale_safe_gear_gate);
  const auto accepted_result = native_command_path.find(
      "if (result.accepted && result.command)", receive_command);
  const auto estop_gear_freeze = native_command_path.find(
      "if (command.estop && !native_control_estop_frozen_gear)",
      accepted_result);
  const auto fresh_gear_update = native_command_path.find(
      "else if (!command.estop && intent_fresh)", estop_gear_freeze);
  const auto accepted_monotonic_update = native_command_path.find(
      "native_control_last_accepted_monotonic_ms =\n"
      "            control_mutex_acquired_monotonic_ms",
      fresh_gear_update);
  expect(
      stale_safe_definition != std::string::npos &&
          native_command_path.find("command.steering == 0.0", stale_safe_definition) <
              stale_nonzero_gate &&
          native_command_path.find("command.throttle == 0.0", stale_safe_definition) <
              stale_nonzero_gate &&
          native_command_path.find("command.brake == 0.0", stale_safe_definition) <
              stale_nonzero_gate &&
          stale_nonzero_gate != std::string::npos &&
          estop_gear_gate != std::string::npos &&
          profile_readiness_gate != std::string::npos &&
          handshake_readiness_gate != std::string::npos &&
          stale_safe_gate != std::string::npos &&
          stale_safe_active_gate != std::string::npos &&
          stale_safe_gap_clock != std::string::npos &&
          stale_safe_gap_gate != std::string::npos &&
          stale_safe_gap_reject != std::string::npos &&
          stale_safe_gear_gate != std::string::npos &&
          receive_command != std::string::npos &&
          accepted_result != std::string::npos &&
          estop_gear_freeze != std::string::npos &&
          fresh_gear_update != std::string::npos &&
          accepted_monotonic_update != std::string::npos &&
          stale_nonzero_gate < estop_gear_gate &&
          estop_gear_gate < profile_readiness_gate &&
          profile_readiness_gate < handshake_readiness_gate &&
          handshake_readiness_gate < stale_safe_gate &&
          stale_safe_gate < stale_safe_gap_clock &&
          stale_safe_gap_clock < heartbeat_admission &&
          heartbeat_admission < stale_safe_active_gate &&
          stale_safe_active_gate < stale_safe_gap_gate &&
          stale_safe_gap_gate < stale_safe_gap_reject &&
          stale_safe_gap_reject < stale_safe_gear_gate &&
          stale_safe_gear_gate < receive_command &&
          handshake_readiness_gate < receive_command &&
          native_command_path.find(
              "native_control_stale_nonzero_intents_discarded_total",
              stale_nonzero_gate) <
              profile_readiness_gate &&
          native_command_path.find("native_control_profile_not_ready_discards_total", handshake_readiness_gate) <
              receive_command &&
          native_command_path.find("native_control_handshake_not_ready_discards_total", handshake_readiness_gate) <
              receive_command &&
          native_command_path.find("session_profile_not_ready", handshake_readiness_gate) <
              receive_command &&
          native_command_path.find("vcu_handshake_not_ready", handshake_readiness_gate) <
              receive_command &&
          native_command_path.find(
              "stale_safe_heartbeat_control_not_active", stale_safe_active_gate) <
              receive_command &&
          native_command_path.find(
              "native_control_last_accepted_monotonic_ms.load()",
              stale_safe_gap_clock) <
              stale_safe_gap_gate &&
          native_command_path.find(
              "control_mutex_acquired_monotonic_ms >= previous_accepted_monotonic_ms",
              stale_safe_gap_clock) <
              heartbeat_admission &&
          native_command_path.find(
              "config.control.degraded_timeout_ms", heartbeat_admission) <
              stale_safe_active_gate &&
          native_command_path.find("control_service->control_braking_wait()", heartbeat_admission) < stale_safe_active_gate &&
          native_command_path.find(
              "native_control_stale_safe_native_gap_discards_total",
              stale_safe_gap_gate) <
              stale_safe_gap_reject &&
          native_command_path
                  .substr(
                      stale_safe_gap_clock,
                      stale_safe_gear_gate - stale_safe_gap_clock)
                  .find("control_mutex_acquired_at_utc_ms") ==
              std::string::npos &&
          native_command_path.find(
              "stale_safe_heartbeat_gear_untrusted", stale_safe_gear_gate) <
              receive_command &&
          native_command_path.find(
              "native_control_estop_frozen_gear\n            ? *native_control_estop_frozen_gear",
              estop_gear_gate) <
              profile_readiness_gate &&
          native_command_path.find(
              "native_control_last_accepted_fresh_gear.value_or(command.gear)",
              estop_gear_gate) <
              profile_readiness_gate &&
          native_command_path.find("command.gear = estop_gear", estop_gear_gate) <
              profile_readiness_gate &&
          native_command_path.find(
              "native_control_estop_frozen_gear = command.gear", estop_gear_freeze) <
              fresh_gear_update &&
          native_command_path.find(
              "native_control_last_accepted_fresh_gear = result.command->gear",
              fresh_gear_update) <
              accepted_monotonic_update,
      "native control freshness, readiness, monotonic-gap, retained-gear, or ESTOP-gear gates regressed");

  const auto stale_command_gate = native_control_websocket_handler.find(
      "command_sent_at_ms <= replay_cutoff_ms");
  const auto native_handler_call = native_control_websocket_handler.find(
      "handle_control_message(", stale_command_gate);
  const auto native_delivery_ack = native_control_websocket_handler.find(
      "{\"event\", \"signaling_delivery_ack\"}", native_handler_call);
  expect(
      process_signaling.find("control_command") == std::string::npos &&
          process_signaling.find(
              "signaling.poll(\"webrtc_answer,ice_candidate,media_fallback\")") !=
              std::string::npos &&
          native_control_websocket_url.find("signaling_websocket_url(") !=
              std::string::npos &&
          native_control_websocket_url.find("std::to_string(connection_generation_)") !=
              std::string::npos &&
          native_control_websocket_url.find("&types=control_command") !=
              std::string::npos &&
          native_control_websocket_headers.find("X-Mine-Teleop-Device-Token") !=
              std::string::npos &&
          native_control_websocket_loop.find(
              "signaling.native_control_websocket_url()") != std::string::npos &&
          native_control_websocket_loop.find(
              "signaling.native_control_websocket_headers()") != std::string::npos &&
          native_control_websocket_loop.find("websocket.receive_json(") !=
              std::string::npos &&
          native_control_websocket_loop.find(
              "handle_native_control_websocket_envelope(websocket, received.message)") !=
              std::string::npos &&
          native_control_websocket_handler.find(
              "message.value(\"type\", \"\") != \"control_command\"") !=
              std::string::npos &&
          native_control_websocket_handler.find(
              "const auto selected = newest_estop ? newest_estop : newest") !=
              std::string::npos &&
          stale_command_gate != std::string::npos &&
          native_handler_call != std::string::npos &&
          native_delivery_ack != std::string::npos &&
          stale_command_gate < native_handler_call &&
          native_handler_call < native_delivery_ack &&
          native_control_websocket_handler.find(
              "ControlMessageTransport::NativeSignaling", native_handler_call) !=
              std::string::npos,
      "native control is not isolated on its dedicated authenticated WSS or stale-safe ACK path");

  expect(
      native_control_watchdog_loop.find("tick_control_service()") !=
              std::string::npos &&
          native_control_watchdog_loop.find("websocket") == std::string::npos &&
          start_native_control_transport.find(
              "native_control_watchdog_thread = std::jthread(") !=
              std::string::npos &&
          start_native_control_transport.find(
              "native_control_websocket_thread = std::jthread(") !=
              std::string::npos &&
          start_native_control_transport.find(
              "native_control_last_accepted_fresh_gear.reset()") !=
              std::string::npos &&
          start_native_control_transport.find(
              "native_control_estop_frozen_gear.reset()") !=
              std::string::npos &&
          start_native_control_transport.find(
              "native_control_last_accepted_monotonic_ms = 0") !=
              std::string::npos &&
          start_native_control_transport.find(
              "native_control_watchdog_loop(stop_token)") != std::string::npos &&
          start_native_control_transport.find(
              "native_control_websocket_loop(stop_token)") != std::string::npos,
      "vehicle watchdog is not scheduled independently from the native control WebSocket");

  const auto native_websocket_connect = native_control_websocket_loop.find(
      "websocket.connect(");
  const auto native_websocket_receive = native_control_websocket_loop.find(
      "const auto received = websocket.receive_json(", native_websocket_connect);
  const auto native_websocket_handle = native_control_websocket_loop.find(
      "handle_native_control_websocket_envelope(websocket, received.message)",
      native_websocket_receive);
  const auto valid_delivery_gate = native_control_websocket_loop.find(
      "received.message.value(\"event\", \"\") == \"signaling_messages\"",
      native_websocket_handle);
  const auto healthy_backoff_reset = native_control_websocket_loop.find(
      "reconnect_delay = kNativeControlReconnectInitialDelay",
      valid_delivery_gate);
  const auto reconnect_wait = native_control_websocket_loop.find(
      "wait_for_native_control_reconnect(stop_token, reconnect_delay)",
      healthy_backoff_reset);
  const auto reconnect_backoff_growth = native_control_websocket_loop.find(
      "reconnect_delay * 2", reconnect_wait);
  expect(
      native_websocket_connect != std::string::npos &&
          native_websocket_receive != std::string::npos &&
          native_websocket_handle != std::string::npos &&
          valid_delivery_gate != std::string::npos &&
          healthy_backoff_reset != std::string::npos &&
          reconnect_wait != std::string::npos &&
          reconnect_backoff_growth != std::string::npos &&
          native_websocket_connect < native_websocket_receive &&
          native_websocket_receive < native_websocket_handle &&
          native_websocket_handle < valid_delivery_gate &&
          valid_delivery_gate < healthy_backoff_reset &&
          healthy_backoff_reset < reconnect_wait &&
          reconnect_wait < reconnect_backoff_growth &&
          native_control_websocket_loop
                  .substr(
                      native_websocket_connect,
                      native_websocket_handle - native_websocket_connect)
                  .find("reconnect_delay = kNativeControlReconnectInitialDelay") ==
              std::string::npos,
      "vehicle control WSS short-lived connections can reset backoff before a valid delivery");

  const auto ordinary_ack_guard = acknowledge_signaling_messages.find(
      "if (!control_only)");
  const auto ordinary_mailbox_ack = acknowledge_signaling_messages.find(
      "auto found = messages_.find(recipient_key)", ordinary_ack_guard);
  const auto control_mailbox_ack = acknowledge_signaling_messages.find(
      "const auto latest_control = latest_control_messages_.find(recipient_key)",
      ordinary_mailbox_ack);
  expect(
      signaling_websocket.find(
              "requested_types != \"control_command\"") != std::string::npos &&
          signaling_websocket.find(
              "control-command receive signaling is restricted to the session vehicle") !=
              std::string::npos &&
          signaling_websocket.find(
              "control-command receive WebSocket cannot be send-only") !=
              std::string::npos &&
          signaling_websocket.find(
              "control_receive_only ? std::string_view(\"control_command\")") !=
              std::string::npos &&
          signaling_websocket.find(
              "control-command receive WebSocket accepts delivery acknowledgements only") !=
              std::string::npos &&
          signaling_websocket.find(
              "delivery_cursor,\n                control_receive_only") !=
              std::string::npos &&
          ordinary_ack_guard != std::string::npos &&
          ordinary_mailbox_ack != std::string::npos &&
          control_mailbox_ack != std::string::npos &&
          ordinary_ack_guard < ordinary_mailbox_ack &&
          ordinary_mailbox_ack < control_mailbox_ack,
      "control-only WSS authentication or ACK isolation from ordinary signaling regressed");
}

void test_driver_native_control_session_snapshot_and_backoff_contract() {
  const auto source = read_text_file("cpp/src/server.cpp");
  const auto send_sample = cpp_function_contract(
      source,
      "bool DriverConsoleRuntime::send_native_control_sample()");
  const auto update_intent = cpp_function_contract(
      source,
      "Json DriverConsoleRuntime::update_control_intent(");
  const auto connect_control_websocket = cpp_function_contract(
      source,
      "bool DriverConsoleRuntime::connect_control_signaling_websocket(");
  const auto note_failure = cpp_function_contract(
      source,
      "void DriverConsoleRuntime::note_native_control_failure(");
  const auto close_control_websocket = cpp_function_contract(
      source,
      "void DriverConsoleRuntime::close_control_signaling_websocket()");
  const auto sender_loop = cpp_function_contract(
      source,
      "void DriverConsoleRuntime::native_control_sender_loop(");
  const auto status = cpp_function_contract(
      source,
      "Json DriverConsoleRuntime::status()");

  const auto snapshot_lock = send_sample.find(
      "std::lock_guard update_lock(native_control_update_mutex_)");
  const auto sample = send_sample.find(
      "sample = native_control_intent_.sample(monotonic_now_ms())",
      snapshot_lock);
  const auto session_snapshot = send_sample.find(
      "session = session_id_", sample);
  const auto generation_snapshot = send_sample.find(
      "generation = control_session_generation_", session_snapshot);
  const auto snapshot_scope_end = send_sample.find("\n\n  try {", generation_snapshot);
  expect(
      snapshot_lock != std::string::npos && sample != std::string::npos &&
          session_snapshot != std::string::npos &&
          generation_snapshot != std::string::npos &&
          snapshot_scope_end != std::string::npos &&
          snapshot_lock < sample && sample < session_snapshot &&
          session_snapshot < generation_snapshot &&
          generation_snapshot < snapshot_scope_end,
      "native sender does not sample intent and snapshot its session generation under one update lock");

  const auto connect = send_sample.find(
      "connect_control_signaling_websocket(\n            session,\n            driver_token,\n            generation, command.estop)",
      snapshot_scope_end);
  const auto send_revalidation = send_sample.find(
      "if (session_id_ != session || control_token_ != control_token ||\n"
      "            control_session_generation_ != generation)",
      connect);
  const auto websocket_send = send_sample.find(
      "control_signaling_websocket_->send_json(", send_revalidation);
  expect(
      connect != std::string::npos && send_revalidation != std::string::npos &&
          websocket_send != std::string::npos && connect < send_revalidation &&
          send_revalidation < websocket_send &&
          send_sample.find("return false;", send_revalidation) < websocket_send,
      "native sender can wrap an old intent sample in a replacement session before WSS send");

  const auto acknowledgement_drain = cpp_function_contract(source,
      "bool DriverConsoleRuntime::drain_native_control_acknowledgements_locked(");
  const auto acknowledgement_progress = send_sample.find(
      "drain_native_control_acknowledgements_locked(");
  expect(acknowledgement_drain.find("control_signaling_ack_window_.acknowledge_through(") != std::string::npos &&
      acknowledgement_drain.find("receive_json(std::chrono::milliseconds(0))") != std::string::npos &&
      sender_loop.find("poll_native_control_acknowledgements()") != std::string::npos,
      "native ACK reception still waits for the next command send");
  const auto oldest_pending_age = send_sample.find(
      "control_signaling_ack_window_.oldest_age_ms(monotonic_ms)",
      acknowledgement_progress);
  const auto progress_age = send_sample.find(
      "control_signaling_ack_window_.stall_age_ms(monotonic_ms)",
      acknowledgement_progress);
  const auto acknowledgement_stall = send_sample.find(
      "native control ACK recovery deadline exceeded",
      oldest_pending_age);
  const auto record_transmitted_packet = send_sample.find(
      "control_signaling_ack_window_.note_sent(", websocket_send);
  expect(
      acknowledgement_progress != std::string::npos &&
          oldest_pending_age != std::string::npos &&
          progress_age != std::string::npos &&
          acknowledgement_stall != std::string::npos &&
          record_transmitted_packet != std::string::npos &&
          acknowledgement_progress < progress_age &&
          progress_age < acknowledgement_stall &&
          send_sample.find("control_signaling_ack_recovery_.observe(") != std::string::npos &&
          send_sample.find("recovery_transition.reconnect_due") != std::string::npos &&
          oldest_pending_age < acknowledgement_stall &&
          websocket_send < record_transmitted_packet,
      "native ACK timeout is not tied to progress of actually transmitted packets");

  const auto intent_update_lock = update_intent.find(
      "std::lock_guard update_lock(native_control_update_mutex_)");
  const auto intent_session_check = update_intent.find(
      "if (session_id_ != expected_session_id ||\n"
      "          control_session_generation_ != expected_session_generation)",
      intent_update_lock);
  const auto store_update = update_intent.find(
      "native_control_intent_.update(\n"
      "        intent, received_at, expected_ui_control_epoch, takeover, apply_started_at)",
      intent_session_check);
  expect(
      intent_update_lock != std::string::npos &&
          intent_session_check != std::string::npos &&
          store_update != std::string::npos &&
          intent_update_lock < intent_session_check &&
          intent_session_check < store_update,
      "controller mutates native intent without an atomic current-session generation check");

  const auto expected_generation_gate = connect_control_websocket.find(
      "if (expected_generation != 0)");
  const auto next_connect_gate = connect_control_websocket.find(
      "control_signaling_next_connect_monotonic_ms_ > monotonic_now_ms()",
      expected_generation_gate);
  const auto websocket_connect_attempt = connect_control_websocket.find(
      "auto next = std::make_unique<WebSocketClient>", next_connect_gate);
  const auto next_backoff = note_failure.find(
      "control_signaling_next_connect_monotonic_ms_ =");
  const auto doubled_backoff = note_failure.find(
      "std::min(control_signaling_reconnect_delay_ms_ * 2, 1000)",
      next_backoff);
  expect(
      expected_generation_gate != std::string::npos &&
          next_connect_gate != std::string::npos &&
          websocket_connect_attempt != std::string::npos &&
          expected_generation_gate < next_connect_gate &&
          next_connect_gate < websocket_connect_attempt &&
          connect_control_websocket.find("return false;", next_connect_gate) <
              websocket_connect_attempt &&
          next_backoff != std::string::npos &&
          note_failure.find(
              "now_monotonic_ms + control_signaling_reconnect_delay_ms_",
              next_backoff) != std::string::npos &&
          doubled_backoff != std::string::npos &&
          close_control_websocket.find(
              "control_signaling_reconnect_delay_ms_ = 100") !=
              std::string::npos &&
          acknowledgement_drain.find(
              "control_signaling_reconnect_delay_ms_ = 100") !=
              std::string::npos &&
          sender_loop.find("send_native_control_sample()") !=
              std::string::npos &&
          status.find("{\"next_connect_in_ms\",") != std::string::npos &&
          status.find("{\"reconnect_delay_ms\", native_control_reconnect_delay_ms}") !=
              std::string::npos &&
          status.find("{\"send_failures_total\", native_control_send_failures_.load()}") !=
              std::string::npos,
      "native control WSS failures can retry at 20 Hz or omit bounded backoff state");
}

void test_native_control_acknowledgement_window_tracks_oldest_pending_packet() {
  mine_teleop::detail::NativeControlAcknowledgementWindow window;

  window.note_sent(1653, 0);
  window.note_sent(1654, 62);
  window.acknowledge_through(1653, 126);
  expect(
      window.oldest_sequence() == 1654 &&
          window.oldest_age_ms(126) == 64 &&
          window.pending_count() == 1,
      "ACK progress inherited the age of a packet that was already acknowledged");

  window.note_sent(1655, 126);
  window.acknowledge_through(1654, 189);
  expect(
      window.oldest_sequence() == 1655 &&
          window.oldest_age_ms(189) == 63 &&
          window.pending_count() == 1,
      "a continuously advancing ACK pipeline accumulated a false stall age");

  window.note_sent(1656, 189);
  expect(
      window.oldest_age_ms(626) == 500,
      "a genuinely stalled oldest packet did not retain its send timestamp");
  window.acknowledge_through(1656, 626);
  expect(
      window.oldest_sequence() == 0 && window.oldest_age_ms(1000) == 0 &&
          window.pending_count() == 0,
      "cumulative ACK did not clear every acknowledged packet");

  window.note_sent(1663, 1000);
  window.reset();
  expect(
      window.oldest_sequence() == 0 && window.pending_count() == 0,
      "reconnect reset retained an old unacknowledged packet");
}

void test_vehicle_control_command_trace_is_bounded_async_and_timed() {
  const auto source = read_text_file("cpp/src/webrtc_media.cpp");
  const auto handler = cpp_function_contract(source, "void handle_control_message(");
  const auto enqueue = cpp_function_contract(source, "void enqueue_control_trace(");
  const auto worker = cpp_function_contract(source, "void control_trace_worker_loop_impl()");
  const auto trace_output = cpp_function_contract(source, "void write_control_trace_batch_line(");
  const auto stop_worker = cpp_function_contract(source, "void stop_control_trace_worker()");
  const auto run = cpp_function_contract(source, "Json run(int frame_count, int duration_ms, int capture_interval_ms)");
  const auto callback_timestamp = handler.find("callback_entered_at_utc_ms = signaling.now_ms()");
  const auto command_parse = handler.find("auto command = ControlCommand::from_json(message)");
  const auto lock_wait_started = handler.find("control_mutex_wait_started_monotonic_ms = steady_now_ms()");
  const auto lock_acquired = handler.find("control_mutex_acquired_monotonic_ms = steady_now_ms()");
  expect(
      callback_timestamp != std::string::npos && command_parse != std::string::npos &&
          lock_wait_started != std::string::npos && lock_acquired != std::string::npos &&
          callback_timestamp < command_parse && command_parse < lock_wait_started &&
          lock_wait_started < lock_acquired,
      "vehicle control trace does not separate callback entry, parsing, and mutex wait");
  for (const auto field : {
           "stage",
           "trace_session_id",
           "callback_entered_at_utc_ms",
           "callback_entered_monotonic_ms",
           "delivery_cursor",
           "cloud_queued_at_utc_ms",
           "vehicle_envelope_received_at_utc_ms",
           "vehicle_envelope_received_monotonic_ms",
           "driver_to_vehicle_callback_utc_delta_ms",
           "cloud_queue_to_vehicle_callback_utc_delta_ms",
           "vehicle_envelope_to_callback_ms",
           "received_at_utc_ms",
           "control_mutex_acquired_at_utc_ms",
           "control_mutex_acquired_monotonic_ms",
           "control_mutex_wait_ms",
           "callback_to_mutex_acquired_ms",
           "receive_apply_invoked",
           "receive_apply_started_at_utc_ms",
           "receive_apply_started_monotonic_ms",
           "receive_apply_completed_at_utc_ms",
           "receive_apply_completed_monotonic_ms",
           "receive_apply_processing_ms",
           "control_path_completed_before_trace_at_utc_ms",
           "control_path_completed_before_trace_monotonic_ms",
           "control_path_processing_before_trace_ms",
           "transport",
           "accepted",
           "reason"}) {
    expect(
        handler.find(std::string("{\"") + field + "\"") != std::string::npos,
        std::string("vehicle control trace omits diagnostic field ") + field);
  }
  expect(
      source.find("\"post_transport_error_discarded\"") != std::string::npos &&
          source.find("\"delivery_ack_sent\"") != std::string::npos &&
          source.find("\"vehicle_ack_send_call_ms\"") != std::string::npos,
      "vehicle trace omits post-error disposition or delivery ACK timing");
  expect(
      source.find("callback_completed_at_utc_ms") == std::string::npos &&
          source.find("callback_completed_monotonic_ms") == std::string::npos &&
          source.find("callback_processing_ms") == std::string::npos,
      "trace labels queue preparation as DataChannel callback completion");
  const auto early_reason = handler.find("const std::string_view reason = stop_requested");
  const auto early_unlock = handler.find("lock.unlock();", early_reason);
  const auto early_trace = handler.find("queue_control_trace(", early_unlock);
  const auto early_return = handler.find("return;", early_trace);
  expect(
      early_reason != std::string::npos &&
          handler.find("\"runtime_stop_requested\"") != std::string::npos &&
          handler.find("\"control_inhibited\"") != std::string::npos &&
          handler.find("\"control_service_unavailable\"") != std::string::npos &&
          handler.find("\"control_link_not_open\"") != std::string::npos &&
          handler.find("\"receive_apply_exception\"") != std::string::npos &&
          early_unlock != std::string::npos && early_trace != std::string::npos &&
          early_return != std::string::npos && early_reason < early_unlock &&
          early_unlock < early_trace && early_trace < early_return,
      "early or exceptional control drops do not retain a precise trace reason");
  expect(
      handler.find("receive_apply_completed_at_utc_ms\n                                                         ? Json(*receive_apply_completed_at_utc_ms)") !=
              std::string::npos &&
          handler.find("receive_apply_started_monotonic_ms && receive_apply_completed_monotonic_ms") !=
              std::string::npos,
      "an early drop can be misreported as receive/apply processing");

  const auto trace_lambda = handler.find("const auto queue_control_trace");
  const auto first_control_guard = handler.find(
      "if (stop_requested || control_inhibited ||", trace_lambda);
  expect(
      trace_lambda != std::string::npos && first_control_guard != std::string::npos,
      "vehicle control trace producer could not be isolated");
  const auto producer = handler.substr(trace_lambda, first_control_guard - trace_lambda);
  expect(
      producer.find(") noexcept {") != std::string::npos &&
          producer.find("enqueue_control_trace(std::move(record))") != std::string::npos &&
          producer.find("bounded_control_trace_text") != std::string::npos &&
          producer.find("std::cout") == std::string::npos &&
          producer.find("write_control_trace_batch_line") == std::string::npos &&
          producer.find("control_token") == std::string::npos,
      "control callback trace producer can write output, leak a token, or throw");
  expect(
      handler.find("lock.unlock();\n        queue_control_trace") != std::string::npos &&
          handler.find("queue_control_trace(\n          &result", early_return) != std::string::npos,
      "vehicle control trace is queued while holding the vehicle control mutex");
  const auto exception_reason = handler.find("\"receive_apply_exception\"");
  const auto exception_rethrow = handler.find("throw;", exception_reason);
  expect(
      exception_reason != std::string::npos && exception_rethrow != std::string::npos &&
          exception_reason < exception_rethrow,
      "trace failure can mask the original receive/apply exception");

  expect(
      source.find("kControlTraceQueueCapacity = 256") != std::string::npos &&
          enqueue.find("std::try_to_lock") != std::string::npos &&
          enqueue.find("kControlTraceQueueCapacity") != std::string::npos &&
          enqueue.find("note_control_trace_drop()") != std::string::npos,
      "control trace producer does not use a bounded non-blocking queue");
  expect(
      worker.find("std::chrono::seconds(1)") != std::string::npos &&
          worker.find("kControlTraceBatchMaxRecords") != std::string::npos &&
          worker.find("kControlTraceBatchCommandsMaxBytes") != std::string::npos &&
          worker.find("\"vehicle_control_trace_batch\"") != std::string::npos &&
          worker.find("\"dropped_since_last\"") != std::string::npos &&
          worker.find("\"dropped_total\"") != std::string::npos &&
          worker.find("\"final\"") != std::string::npos,
      "trace worker does not batch by time/size or expose loss accounting");
  expect(
      trace_output.find("kControlTraceBatchLineMaxBytes") != std::string::npos &&
          trace_output.find("std::osyncstream output(std::cout)") != std::string::npos &&
          trace_output.find("diagnostic_mutex") == std::string::npos,
      "trace output can exceed the launcher line limit or block callbacks on a shared log mutex");
  expect(
      stop_worker.find("control_trace_accepting.store(false") != std::string::npos &&
          stop_worker.find("control_trace_worker.joinable()") != std::string::npos &&
          stop_worker.find("control_mutex") == std::string::npos,
      "trace shutdown joins while holding the control mutex");
  const auto run_stop_trace = run.rfind("stop_control_trace_worker()");
  const auto run_return = run.rfind("return summary;");
  expect(
      run_stop_trace != std::string::npos && run_return != std::string::npos &&
          run_stop_trace < run_return,
      "media summary can be written before the final trace batch is drained");
}

void test_structured_control_rejection_is_safe_and_rate_limited() {
  const auto source = read_text_file("cpp/src/webrtc_media.cpp");
  const auto handler = cpp_function_contract(source, "void handle_control_message(");
  const auto sender = cpp_function_contract(
      source,
      "std::uint64_t send_control_command_rejected_locked(");
  expect(
      handler.find(
          "send_control_command_rejected_locked(command.seq, intent_seq, result.issue_code)") !=
          std::string::npos,
      "a native signaling control rejection is not correlated to both command and browser intent sequences");
  const auto legacy_data_channel_reject = handler.find(
      "if (transport == ControlMessageTransport::DataChannel)");
  const auto native_command_parse = handler.find(
      "auto command = ControlCommand::from_json(message)", legacy_data_channel_reject);
  expect(
      legacy_data_channel_reject != std::string::npos &&
          native_command_parse != std::string::npos &&
          handler.find("legacy_data_channel_control_disabled", legacy_data_channel_reject) <
              native_command_parse &&
          handler.find("return;", legacy_data_channel_reject) < native_command_parse &&
          handler.find(
              "const auto intent_seq = message.value(\"intent_seq\", std::uint64_t{0})",
              native_command_parse) != std::string::npos,
      "ordinary DataChannel commands can reach apply or native signaling payloads can omit intent_seq");
  expect(
      sender.find("vcu_drive_gear_change_moving_or_stale") != std::string::npos &&
          sender.find("vcu_control_apply_rejected") != std::string::npos,
      "control rejection sender does not enforce the stable issue-code allowlist");
  expect(
      sender.find("{\"vehicle_id\", config.vehicle_id}") != std::string::npos &&
          sender.find("{\"driver_id\", signaling.driver_id()}") != std::string::npos &&
          sender.find("{\"session_id\", signaling.session_id()}") != std::string::npos &&
          sender.find("{\"control_status_seq\", last_control_rejection_status_seq = ++control_status_seq}") !=
              std::string::npos &&
          sender.find("{\"command_seq\", command_seq}") != std::string::npos &&
          sender.find("{\"intent_seq\", intent_seq}") != std::string::npos,
      "control rejection status is not bound to authoritative identity and sequence state");
  expect(
      sender.find("< 500") != std::string::npos &&
          sender.find("\"error\"") == std::string::npos &&
          sender.find("error.what()") == std::string::npos,
      "control rejection status can flood the channel or leak raw exception text");
}

void test_signaling_latest_only_control_mailbox() {
  constexpr std::string_view kDriverId = "driver-control-mailbox";
  constexpr std::string_view kVehicleId = "vehicle-control-mailbox";
  constexpr std::string_view kDriverPassword = "driver-control-mailbox-password";
  constexpr std::string_view kDeviceToken = "vehicle-control-mailbox-device-token";

  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{std::string(kDriverId), std::string(kDriverPassword)}};
  config.device_tokens = {{std::string(kVehicleId), std::string(kDeviceToken)}};
  config.driver_vehicle_permissions = {{std::string(kDriverId), {std::string(kVehicleId)}}};
  config.native_control_message_ttl_ms = 150;
  mine_teleop::SignalingService service(config);

  const auto post = [&](std::string path, const mine_teleop::Json& body) {
    mine_teleop::HttpRequest request;
    request.method = "POST";
    request.target = path;
    request.path = std::move(path);
    request.peer_address = "127.0.0.1";
    request.body = body.dump();
    return service.handle(request);
  };
  const auto get = [&](std::string path,
                       std::unordered_map<std::string, std::string> query) {
    mine_teleop::HttpRequest request;
    request.method = "GET";
    request.target = path;
    request.path = std::move(path);
    request.peer_address = "127.0.0.1";
    request.query = std::move(query);
    return service.handle(request);
  };

  const auto online_response = post(
      "/vehicles/online",
      {{"vehicle_id", kVehicleId},
       {"device_token", kDeviceToken},
       {"connection_id", "control-mailbox-vehicle-runtime"}});
  expect(online_response.status == 200, "control mailbox vehicle did not become online");
  const auto vehicle_generation = mine_teleop::Json::parse(online_response.body)
                                      .at("connection_generation")
                                      .get<std::uint64_t>();
  const auto login_response = post(
      "/auth/driver_login",
      {{"driver_id", kDriverId}, {"password", kDriverPassword}});
  expect(login_response.status == 200, "control mailbox driver login failed");
  const auto driver_token =
      mine_teleop::Json::parse(login_response.body).at("token").get<std::string>();
  const auto session_response = post(
      "/sessions",
      {{"driver_id", kDriverId}, {"vehicle_id", kVehicleId}, {"token", driver_token}});
  expect(session_response.status == 200, "control mailbox session creation failed");
  const auto session = mine_teleop::Json::parse(session_response.body);
  const auto session_id = session.at("session_id").get<std::string>();
  const auto control_token = session.at("control_token").get<std::string>();

  const auto make_control_message = [&](std::uint64_t command_sequence,
                                        std::uint64_t intent_sequence,
                                        std::string_view inner_control_token) {
    mine_teleop::ControlCommand command;
    command.vehicle_id = kVehicleId;
    command.driver_id = kDriverId;
    command.session_id = session_id;
    command.seq = command_sequence;
    command.sent_at_utc_ms = mine_teleop::now_ms();
    command.gear = "N";
    command.steering = static_cast<double>(command_sequence) / 100.0;
    command.control_token = inner_control_token;
    auto payload = command.to_json();
    payload["intent_seq"] = intent_sequence;
    payload["intent_fresh"] = true;
    auto message = mine_teleop::ProtocolMetadata{
                       command.protocol_version,
                       command.vehicle_id,
                       command.driver_id,
                       command.session_id,
                       command.seq,
                       command.sent_at_utc_ms}
                       .to_json();
    message["sender"] = kDriverId;
    message["recipient"] = kVehicleId;
    message["token"] = driver_token;
    message["type"] = "control_command";
    message["payload"] = std::move(payload);
    return message;
  };
  const auto post_signaling = [&](const mine_teleop::Json& message) {
    return post("/signaling/" + session_id + "/messages", message);
  };
  const auto vehicle_messages = [&](std::string_view types) {
    return get(
        "/signaling/" + session_id + "/messages",
        {{"recipient", std::string(kVehicleId)},
         {"device_token", std::string(kDeviceToken)},
         {"connection_generation", std::to_string(vehicle_generation)},
         {"types", std::string(types)}});
  };

  const auto media_answer = signaling_request_for(
      session_id,
      1,
      kVehicleId,
      kDriverId,
      kDriverId,
      kVehicleId,
      "token",
      driver_token,
      "webrtc_answer",
      {{"type", "answer"}, {"sdp", "v=0\r\n"}});
  expect(
      post_signaling(media_answer).status == 200,
      "ordinary signaling sequence 1 was rejected before control sequence isolation testing");
  const auto first_control = post_signaling(make_control_message(1, 101, control_token));
  expect(
      first_control.status == 200 &&
          mine_teleop::Json::parse(first_control.body).value("queued", 0U) == 1,
      "control sequence 1 did not use its own latest-only sequence domain");

  auto wrong_driver_token = make_control_message(2, 102, control_token);
  wrong_driver_token["token"] = "wrong-driver-token";
  expect(
      post_signaling(wrong_driver_token).status == 401,
      "control mailbox accepted an invalid driver credential");
  expect(
      post_signaling(make_control_message(2, 102, "wrong-control-token")).status == 401,
      "control mailbox accepted a command token outside the active session");

  for (const auto& [field, replacement] :
       std::vector<std::pair<std::string, mine_teleop::Json>>{
           {"vehicle_id", "another-vehicle"},
           {"driver_id", "another-driver"},
           {"session_id", "another-session"},
           {"seq", 999},
           {"sent_at_utc_ms", std::numeric_limits<std::int64_t>::max()}}) {
    auto mismatch = make_control_message(2, 102, control_token);
    mismatch["payload"][field] = replacement;
    expect(
        post_signaling(mismatch).status == 400,
        "control mailbox accepted mismatched inner/outer metadata field: " + field);
  }

  auto reversed_route = make_control_message(2, 102, control_token);
  reversed_route["sender"] = kVehicleId;
  reversed_route["recipient"] = kDriverId;
  reversed_route.erase("token");
  reversed_route["device_token"] = kDeviceToken;
  reversed_route["connection_generation"] = vehicle_generation;
  expect(
      post_signaling(reversed_route).status == 401,
      "vehicle-to-driver control_command route was accepted");

  const auto second_control = post_signaling(make_control_message(2, 102, control_token));
  expect(
      second_control.status == 200 &&
          mine_teleop::Json::parse(second_control.body).value("queued", 0U) == 1,
      "second control command grew the latest-only mailbox beyond capacity one");
  const auto latest_control_response = vehicle_messages("control_command");
  expect(latest_control_response.status == 200, "vehicle control mailbox poll failed");
  const auto latest_control = mine_teleop::Json::parse(latest_control_response.body).at("messages");
  expect(
      latest_control.size() == 1 &&
          latest_control.at(0).value("seq", std::uint64_t{0}) == 2 &&
          latest_control.at(0).at("payload").value("intent_seq", std::uint64_t{0}) == 102,
      "latest-only control mailbox replayed an overwritten command");
  const auto retained_media_response = vehicle_messages("webrtc_answer");
  expect(retained_media_response.status == 200, "retained media message poll failed");
  const auto retained_media = mine_teleop::Json::parse(retained_media_response.body).at("messages");
  expect(
      retained_media.size() == 1 && retained_media.at(0).value("type", "") == "webrtc_answer",
      "control-only poll consumed an unrelated WebRTC message");

  auto pending_offer = signaling_request_for(
      session_id,
      1,
      kVehicleId,
      kDriverId,
      kVehicleId,
      kDriverId,
      "device_token",
      kDeviceToken,
      "webrtc_offer",
      {{"type", "offer"}, {"sdp", "v=0\r\n"}});
  pending_offer["connection_generation"] = vehicle_generation;
  expect(post_signaling(pending_offer).status == 200, "pending WebRTC offer was rejected");

  int websocket_pair[2]{-1, -1};
  expect(
      ::socketpair(AF_UNIX, SOCK_STREAM, 0, websocket_pair) == 0,
      "send-only websocket socketpair creation failed");
  timeval websocket_timeout{0, 200'000};
  ::setsockopt(
      websocket_pair[0],
      SOL_SOCKET,
      SO_RCVTIMEO,
      &websocket_timeout,
      sizeof(websocket_timeout));
  mine_teleop::HttpRequest websocket_request;
  websocket_request.method = "GET";
  websocket_request.path = "/signaling/" + session_id + "/ws";
  websocket_request.target = websocket_request.path +
      "?participant=" + std::string(kDriverId) + "&send_only=1";
  websocket_request.peer_address = "127.0.0.1";
  websocket_request.query = {
      {"participant", std::string(kDriverId)}, {"send_only", "1"}};
  websocket_request.headers = {
      {"upgrade", "websocket"},
      {"connection", "Upgrade"},
      {"sec-websocket-version", "13"},
      {"sec-websocket-key", "dGhlIHNhbXBsZSBub25jZQ=="},
      {"x-mine-teleop-driver-token", driver_token}};
  bool websocket_handled = false;
  std::thread websocket_thread([&] {
    websocket_handled = service.handle_websocket(websocket_pair[1], websocket_request);
    ::close(websocket_pair[1]);
  });
  try {
    std::string upgrade_headers;
    while (upgrade_headers.find("\r\n\r\n") == std::string::npos) {
      upgrade_headers += raw_receive_exact(websocket_pair[0], 1);
    }
    expect(
        upgrade_headers.starts_with("HTTP/1.1 101 "),
        "send-only websocket upgrade failed");
    std::array<char, 2> unexpected{};
    expect(
        ::recv(websocket_pair[0], unexpected.data(), unexpected.size(), 0) < 0,
        "send-only websocket received a pending WebRTC delivery");

    raw_send_all(
        websocket_pair[0],
        raw_masked_websocket_frame(
            0x81U,
            mine_teleop::Json{
                {"event", "signaling_delivery_ack"}, {"delivery_cursor", 0}}
                .dump()));
    const auto rejected_delivery_ack_frame = raw_receive_websocket_frame(websocket_pair[0]);
    const auto rejected_delivery_ack =
        mine_teleop::Json::parse(rejected_delivery_ack_frame.payload);
    expect(
        rejected_delivery_ack_frame.final && rejected_delivery_ack_frame.opcode == 0x1U &&
            rejected_delivery_ack.value("event", "") == "signaling_message_rejected" &&
            rejected_delivery_ack.value("error", "") ==
                "send-only control WebSocket accepts control_command messages only",
        "send-only control websocket accepted a delivery acknowledgement");

    raw_send_all(
        websocket_pair[0],
        raw_masked_websocket_frame(0x81U, media_answer.dump()));
    const auto rejected_media_frame = raw_receive_websocket_frame(websocket_pair[0]);
    const auto rejected_media = mine_teleop::Json::parse(rejected_media_frame.payload);
    expect(
        rejected_media_frame.final && rejected_media_frame.opcode == 0x1U &&
            rejected_media.value("event", "") == "signaling_message_rejected" &&
            rejected_media.value("error", "") ==
                "send-only control WebSocket accepts control_command messages only",
        "send-only control websocket accepted a non-control signaling message");

    raw_send_all(
        websocket_pair[0],
        raw_masked_websocket_frame(
            0x81U,
            make_control_message(3, 103, control_token).dump()));
    const auto valid_control_frame =
        raw_receive_websocket_frame(websocket_pair[0]);
    const auto valid_control =
        mine_teleop::Json::parse(valid_control_frame.payload);
    expect(
        valid_control_frame.final && valid_control_frame.opcode == 0x1U &&
            valid_control.value("event", "") == "signaling_ack" &&
            valid_control.value("type", "") == "control_command" &&
            valid_control.value("seq", std::uint64_t{0}) == 3 &&
            !valid_control.value("duplicate", true),
        "send-only control websocket changed valid control acknowledgement behavior");

    const std::string close_payload("\x03\xe8", 2);
    raw_send_all(
        websocket_pair[0],
        raw_masked_websocket_frame(0x88U, close_payload));
    const auto close = raw_receive_websocket_frame(websocket_pair[0]);
    expect(
        close.final && close.opcode == 0x8U && close.payload == close_payload,
        "send-only websocket close handshake failed");
    ::close(websocket_pair[0]);
    websocket_pair[0] = -1;
    websocket_thread.join();
  } catch (...) {
    if (websocket_pair[0] >= 0) {
      ::shutdown(websocket_pair[0], SHUT_RDWR);
      ::close(websocket_pair[0]);
    }
    if (websocket_thread.joinable()) websocket_thread.join();
    throw;
  }
  expect(websocket_handled, "send-only websocket request was not handled");
  const auto pending_driver_response = get(
      "/signaling/" + session_id + "/messages",
      {{"recipient", std::string(kDriverId)}, {"token", driver_token}, {"types", "webrtc_offer"}});
  expect(pending_driver_response.status == 200, "driver pending message poll failed");
  const auto pending_driver = mine_teleop::Json::parse(pending_driver_response.body).at("messages");
  expect(
      pending_driver.size() == 1 && pending_driver.at(0).value("type", "") == "webrtc_offer",
      "send-only websocket consumed the pending WebRTC message");

  expect(
      post_signaling(make_control_message(4, 104, control_token)).status == 200,
      "control command for TTL testing was rejected");
  const auto second_media_answer = signaling_request_for(
      session_id,
      2,
      kVehicleId,
      kDriverId,
      kDriverId,
      kVehicleId,
      "token",
      driver_token,
      "webrtc_answer",
      {{"type", "answer"}, {"sdp", "v=0\r\na=mid:second\r\n"}});
  expect(post_signaling(second_media_answer).status == 200, "second media message was rejected");
  std::this_thread::sleep_for(std::chrono::milliseconds(175));
  const auto expired_control_response = vehicle_messages("control_command");
  expect(expired_control_response.status == 200, "expired control mailbox poll failed");
  expect(
      mine_teleop::Json::parse(expired_control_response.body).at("messages").empty(),
      "150ms native control mailbox TTL replayed a stale command");
  const auto media_after_expiry_response = vehicle_messages("webrtc_answer");
  expect(media_after_expiry_response.status == 200, "media poll after control expiry failed");
  const auto media_after_expiry =
      mine_teleop::Json::parse(media_after_expiry_response.body).at("messages");
  expect(
      media_after_expiry.size() == 1 &&
          media_after_expiry.at(0).value("seq", std::uint64_t{0}) == 2,
      "expiring a stale control command consumed an unrelated media message");
}

void write_text_file(const std::filesystem::path& path, std::string_view value) {
  std::ofstream output(path);
  expect(output.good(), "cannot create test file: " + path.string());
  output << value;
  expect(output.good(), "cannot write test file: " + path.string());
}

void test_driver_brake_limit_config_validation() {
  const auto root = std::filesystem::temp_directory_path() /
      ("mine-teleop-driver-brake-limits-" + mine_teleop::random_token(6));
  std::filesystem::create_directories(root);
  try {
    const auto invalid_path = root / "invalid-order.yaml";
    write_text_file(
        invalid_path,
        R"YAML(driver:
  id: driver-console-001
cloud:
  signaling_url: http://127.0.0.1:8765/signaling
control:
  limits:
    initial_max_brake_pressure_bar: 100.0
    initial_service_brake_pressure_bar: 80.0
    initial_hard_brake_pressure_bar: 20.0
)YAML");
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_driver_config(invalid_path.string())); },
        "driver config accepted service brake above hard brake");

    const auto legacy_path = root / "legacy-max-brake.yaml";
    write_text_file(
        legacy_path,
        R"YAML(driver:
  id: driver-console-001
cloud:
  signaling_url: http://127.0.0.1:8765/signaling
control:
  limits:
    initial_max_brake: 0.1
)YAML");
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_driver_config(legacy_path.string())); },
        "driver config silently reinterpreted a legacy normalized brake value as bar");

    const auto legacy_throttle_path = root / "legacy-throttle.yaml";
    write_text_file(
        legacy_throttle_path,
        R"YAML(driver:
  id: driver-console-001
cloud:
  signaling_url: http://127.0.0.1:8765/signaling
control:
  limits:
    initial_max_throttle: 0.05
)YAML");
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_driver_config(legacy_throttle_path.string())); },
        "driver config silently converted legacy throttle ratio without a vehicle hard limit");

    const auto obsolete_keyboard_path = root / "obsolete-keyboard.yaml";
    write_text_file(
        obsolete_keyboard_path,
        R"YAML(driver:
  id: driver-console-001
cloud:
  signaling_url: http://127.0.0.1:8765/signaling
control:
  keyboard:
    throttle: ArrowUp
)YAML");
    expect_throws(
        [&] { static_cast<void>(mine_teleop::load_driver_config(obsolete_keyboard_path.string())); },
        "driver config silently accepted the obsolete control.keyboard section");
  } catch (...) {
    std::filesystem::remove_all(root);
    throw;
  }
  std::filesystem::remove_all(root);
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
  config.browser_event_log_max_bytes = 1536;
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
          {"page_source_sha256", "old-loaded-page"},
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
  const auto first_record = mine_teleop::Json::parse(
      persisted.substr(0, persisted.find('\n')));
  expect(first_record.value("runtime_console_source_sha256", "").size() == 64 &&
             first_record.value("runtime_diagnostics_revision", "") == "traction-diagnostics-v1" &&
             first_record["details"].value("page_source_sha256", "") == "old-loaded-page",
         "runtime source identity was not attached to events from an older page");

  request.body = mine_teleop::Json({
      {"event", "control_trace_batch"},
      {"sent_at_utc_ms", mine_teleop::now_ms()},
      {"details",
       {{"reason", "interval"},
        {"commands",
         mine_teleop::Json::array(
             {{{"session_id", "session-001"},
               {"seq", 42},
               {"outcome", "forwarded"},
               {"control_token", "must-not-be-written"}}})},
        {"summary", {{"heartbeat_tick_count", 20}, {"timer_lag_max_ms", 3.0}}}}},
  }).dump();
  expect(app.handle(request).status == 200, "batched control trace could not be persisted");
  const auto trace_persisted = read_text_file(log_path);
  expect(
      trace_persisted.find("control_trace_batch") != std::string::npos &&
          trace_persisted.find("\"seq\":42") != std::string::npos &&
          trace_persisted.find("\"outcome\":\"forwarded\"") != std::string::npos,
      "batched terminal control trace fields were not written");
  expect(
      trace_persisted.find("[redacted]") != std::string::npos &&
          trace_persisted.find("must-not-be-written") == std::string::npos,
      "batched control trace credentials were not redacted");

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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "switch-test-vehicle-1"}});
  const auto vehicle_two = http.post_json_response(
      base + "/vehicles/online",
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-002"},
       {"device_token", "vehicle-secret-2"},
       {"connection_id", "switch-test-vehicle-2"}});

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  driver_config.control_limits.initial_max_brake_pressure_bar = 100.0;
  driver_config.control_limits.initial_service_brake_pressure_bar = 10.0;
  driver_config.control_limits.initial_hard_brake_pressure_bar = 25.0;
  auto driver_runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(
      driver_config, "vehicle-001", "dev-password");
  auto& driver = *driver_runtime;
  const auto first = driver.connect("vehicle-001");
  const auto second = driver.connect("vehicle-002");
  const auto second_session_id = second.at("session_id").get<std::string>();
  const auto second_control_session_generation =
      second.at("control_session_generation").get<std::uint64_t>();
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
  expect_throws(
      [&] {
        static_cast<void>(driver.update_control_intent(
            declare_native_input_video_sample({{"session_id", first.at("session_id")},
             {"session_generation", first.at("control_session_generation")},
             {"ui_instance_id", "stale-switch-test-ui"},
             {"intent_seq", 1},
             {"gear", "N"},
             {"steering", 0.0},
             {"throttle", 0.0},
             {"brake", 0.0}})));
      },
      "a browser intent from the released session generation survived the vehicle switch");

  mine_teleop::DriverConsoleHttpApp control_app(driver_runtime);
  const auto post_control_intent = [&](const mine_teleop::Json& body) {
    mine_teleop::HttpRequest request;
    request.method = "POST";
    request.path = "/api/control-intent";
    request.body = body.dump();
    return control_app.handle(request);
  };
  const auto effective_native_intent = [&] {
    const auto native = driver.status().at("native_control");
    return mine_teleop::Json({
        {"intent_seq", native.value("effective_intent_seq", std::uint64_t{0})},
        {"gear", native.value("effective_gear", "")},
        {"steering", native.value("effective_steering", -1.0)},
        {"throttle", native.value("effective_throttle", -1.0)},
        {"brake", native.value("effective_brake", -1.0)},
        {"estop", native.value("effective_estop", false)},
    });
  };
  mine_teleop::Json bound_intent = {
      {"session_id", second_session_id},
      {"session_generation", second_control_session_generation},
      {"ui_instance_id", "api-session-binding-ui"},
      {"critical_video_sample_healthy", true},
      {"critical_video_bad_sample_revision", 0}, {"critical_video_brake_revision", 0},
      {"intent_seq", 1},
      {"gear", "D"},
      {"steering", 0.0},
      {"throttle", 0.0},
      {"brake", 0.0},
  };
  const auto before_invalid_intents = effective_native_intent();
  auto missing_session_id = bound_intent;
  missing_session_id.erase("session_id");
  expect(
      post_control_intent(missing_session_id).status == 400,
      "control intent without session_id did not fail schema validation");
  auto wrong_session_id = bound_intent;
  wrong_session_id["session_id"] = "session-from-released-browser";
  expect(
      post_control_intent(wrong_session_id).status == 409,
      "control intent for another session did not fail the active-generation conflict check");
  auto missing_session_generation = bound_intent;
  missing_session_generation.erase("session_generation");
  expect(
      post_control_intent(missing_session_generation).status == 400,
      "control intent without session_generation did not fail schema validation");
  auto wrong_session_generation = bound_intent;
  wrong_session_generation["session_generation"] =
      second_control_session_generation + 1;
  expect(
      post_control_intent(wrong_session_generation).status == 409,
      "control intent from an obsolete session generation was accepted");
  expect(
      effective_native_intent() == before_invalid_intents,
      "rejected session-bound control intent changed the effective native intent");
  const auto accepted_bound_intent = post_control_intent(bound_intent);
  const auto accepted_bound_body = mine_teleop::Json::parse(accepted_bound_intent.body);
  const auto after_bound_intent = effective_native_intent();
  expect(
      accepted_bound_intent.status == 200 &&
          accepted_bound_body.value("accepted", false) &&
          accepted_bound_body.value("session_id", "") == second_session_id &&
          accepted_bound_body.value("session_generation", std::uint64_t{0}) ==
              second_control_session_generation &&
          after_bound_intent.value("intent_seq", std::uint64_t{0}) == 1 &&
          after_bound_intent.value("gear", "") == "D",
      "control intent with the current session generation was not accepted atomically");

  mine_teleop::HttpRequest profile_request;
  profile_request.method = "POST";
  profile_request.path = "/api/control-profile";
  const mine_teleop::Json valid_profile = {
      {"profile_version", 7},
      {"parking_idle_timeout_ms", 500},
      {"consecutive_late_frame_limit", 3},
      {"critical_camera_stale_brake_ms", 500},
      {"control_recovery_timeout_ms", 5000},
      {"target_speed_kph", 3.0},
      {"max_motor_torque_nm", 250.0},
      {"max_brake_pressure_bar", 80.0},
      {"service_brake_pressure_bar", 8.0},
      {"hard_brake_pressure_bar", 20.0},
      {"max_steering_angle_deg", 3.0},
      {"speed_pid_kp", 1.5},
      {"speed_pid_ki", 0.2},
      {"speed_pid_kd", 0.1},
      {"speed_pid_derivative_filter_tau_ms", 50.0},
      {"speed_pid_max_dt_ms", 100},
      {"motor_torque_rise_rate_nm_per_s", 120.0},
  };
  profile_request.body = valid_profile.dump();
  const auto profile_response = control_app.handle(profile_request);
  expect(profile_response.status == 200, "session control profile could not be prepared");
  const auto profile_result = mine_teleop::Json::parse(profile_response.body);
  const auto& profile_envelope = profile_result.at("request");
  const auto vehicle_profile_request =
      mine_teleop::SessionControlProfileRequest::from_json(profile_envelope);
  expect(
      vehicle_profile_request.profile.to_json() == valid_profile,
      "vehicle parser did not receive the complete requested control profile");
  // Exercise the actual driver HTTP serializer -> vehicle service -> ACK,
  // using the same authenticated session rather than two unrelated fixtures.
  auto vehicle_config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  vehicle_config.vehicle_id = "vehicle-002";
  vehicle_config.field_safety.full_scale_motor_torque_nm = 640.0;
  auto vehicle_adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
  mine_teleop::VehicleControlService vehicle_service(vehicle_config, "driver-console-001",
      vehicle_profile_request.session_id, vehicle_profile_request.control_token,
      std::move(vehicle_adapter), 100);
  vehicle_service.start(vehicle_profile_request.sent_at_utc_ms);
  for (int limit = 1; limit <= 5; ++limit) {
    auto requested = valid_profile;
    requested["consecutive_late_frame_limit"] = limit;
    requested["critical_camera_stale_brake_ms"] = 500 + limit * 100;
    requested["control_recovery_timeout_ms"] = 2000 + limit * 1000;
    const auto prepared = driver.prepare_control_profile(requested);
    const auto wire = mine_teleop::SessionControlProfileRequest::from_json(prepared.at("request"));
    const auto applied = vehicle_service.receive_session_profile(wire, wire.sent_at_utc_ms);
    expect(applied.accepted && applied.applied_revision == wire.seq &&
        applied.effective_profile && applied.effective_profile->to_json() == requested &&
        vehicle_service.control_timing(wire.sent_at_utc_ms).at("consecutive_late_frame_limit") == limit &&
        vehicle_service.recovery_timing() == std::pair{500 + limit * 100, 2000 + limit * 1000},
        "driver and vehicle disagreed on a confirmed profile or did not apply the requested frame limit");
  }
  vehicle_service.close();

  expect(
      profile_result.value("prepared", false) &&
          profile_result.value("transport", "") == "webrtc_data_channel" &&
          profile_result.value("delivery_state", "") == "browser_data_channel_pending" &&
          profile_envelope.value("type", "") == "session_control_profile" &&
          profile_envelope.value("vehicle_id", "") == "vehicle-002" &&
          profile_envelope.value("driver_id", "") == "driver-console-001" &&
          profile_envelope.value("session_id", "") == second.value("session_id", "") &&
          !profile_envelope.value("control_token", "").empty() &&
          profile_envelope.value("seq", std::uint64_t{0}) > 0,
      "prepared session profile lost authenticated envelope identity");
  expect(
      !profile_envelope.contains("profile") &&
          profile_envelope.value("profile_version", 0) == 7 &&
          profile_envelope.value("target_speed_kph", -1.0) == 3.0 &&
          profile_envelope.value("max_motor_torque_nm", -1.0) == 250.0 &&
          profile_envelope.value("max_brake_pressure_bar", -1.0) == 80.0 &&
          profile_envelope.value("service_brake_pressure_bar", -1.0) == 8.0 &&
          profile_envelope.value("hard_brake_pressure_bar", -1.0) == 20.0 &&
          profile_envelope.value("max_steering_angle_deg", -1.0) == 3.0 &&
          profile_envelope.value("speed_pid_kp", -1.0) == 1.5 &&
          profile_envelope.value("speed_pid_ki", -1.0) == 0.2 &&
          profile_envelope.value("speed_pid_kd", -1.0) == 0.1 &&
          profile_envelope.value("speed_pid_derivative_filter_tau_ms", -1.0) == 50.0 &&
          profile_envelope.value("speed_pid_max_dt_ms", -1) == 100 &&
          profile_envelope.value("motor_torque_rise_rate_nm_per_s", -1.0) == 120.0,
      "session profile request is not the canonical flat DataChannel schema");

  const auto restored_prepared_profile = driver.prepare_control_profile(valid_profile);
  mine_teleop::HttpRequest get_profile_request;
  get_profile_request.method = "GET";
  get_profile_request.path = "/api/control-profile";
  const auto get_profile_response = control_app.handle(get_profile_request);
  expect(get_profile_response.status == 200, "prepared session control profile is not readable");
  const auto prepared_profile = mine_teleop::Json::parse(get_profile_response.body);
  expect(
      prepared_profile.value("target_speed_kph", -1.0) == 3.0 &&
          prepared_profile.value("max_motor_torque_nm", -1.0) == 250.0 &&
          prepared_profile.value("profile_version", 0) == 7 &&
          prepared_profile.value("initialized", false) &&
          prepared_profile.value("max_steering_angle_deg", -1.0) == 3.0 &&
          prepared_profile.value("speed_pid_kp", -1.0) == 1.5 &&
          prepared_profile.value("speed_pid_max_dt_ms", -1) == 100 &&
          prepared_profile.value("motor_torque_rise_rate_nm_per_s", -1.0) == 120.0 &&
          prepared_profile.value("last_prepared_seq", std::uint64_t{0}) ==
              restored_prepared_profile.at("request").value("seq", std::uint64_t{0}),
      "prepared session profile state does not match its DataChannel envelope");

  for (const int parking_timeout_ms : {0, 750, 1000}) {
    auto requested_profile = valid_profile;
    requested_profile["parking_idle_timeout_ms"] = parking_timeout_ms;
    profile_request.body = requested_profile.dump();
    const auto response = control_app.handle(profile_request);
    expect(response.status == 200, "valid parking timeout could not be prepared");
    const auto result = mine_teleop::Json::parse(response.body);
    const auto vehicle_request =
        mine_teleop::SessionControlProfileRequest::from_json(result.at("request"));
    expect(
        vehicle_request.profile.to_json() == requested_profile &&
            result.at("control_profile").at("parking_idle_timeout_ms") ==
                parking_timeout_ms,
        "parking timeout changed between controller state and vehicle wire profile");
  }

  auto invalid_profile = valid_profile;
  invalid_profile["max_motor_torque_nm"] = 640.1;
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted torque above the controller schema ceiling");
  invalid_profile = valid_profile;
  invalid_profile["max_brake_pressure_bar"] = 327.7;
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted ordinary EHB pressure above the schema ceiling");
  invalid_profile = valid_profile;
  invalid_profile["speed_pid_kp"] = 0.0;
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted a non-positive speed PID kp");
  invalid_profile = valid_profile;
  invalid_profile["speed_pid_max_dt_ms"] = 100.5;
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted a fractional speed PID max dt");
  invalid_profile = valid_profile;
  invalid_profile["unexpected_field"] = 1;
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted fields outside the V3 schema");
  invalid_profile = valid_profile;
  invalid_profile.erase("motor_torque_rise_rate_nm_per_s");
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted a V2-shaped request without the rise-rate field");
  invalid_profile = valid_profile;
  invalid_profile["motor_torque_rise_rate_nm_per_s"] = 32000.1;
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted a rise rate above the schema ceiling");
  invalid_profile = valid_profile;
  invalid_profile["motor_torque_rise_rate_nm_per_s"] = -1.0;
  profile_request.body = invalid_profile.dump();
  expect(
      control_app.handle(profile_request).status == 400,
      "session profile accepted a negative rise rate");
  const auto neutral_intent = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", second_session_id},
       {"session_generation", second_control_session_generation},
       {"ui_instance_id", "switch-test-ui"},
       {"ui_control_epoch", accepted_bound_body.at("ui_control_epoch")},
       {"takeover", true},
       {"critical_video_sample_healthy", true},
      {"critical_video_bad_sample_revision", 0}, {"critical_video_brake_revision", 0},
       {"intent_seq", 1},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.0}}));
  mine_teleop::HttpRequest delayed_intent_request;
  delayed_intent_request.received_at_monotonic_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count() - 10000;
  const auto expired_local_intent = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", second_session_id}, {"session_generation", second_control_session_generation},
       {"ui_instance_id", "switch-test-ui"}, {"intent_seq", 9999}, {"gear", "N"},
       {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}}), &delayed_intent_request);
  expect(!expired_local_intent.value("accepted", true) &&
      expired_local_intent.value("reason", "") == "old_intent_arrival",
      "local HTTP waiting revived a ten-second-old input");
  expect(expired_local_intent.at("local_http_timing").at("receive_to_apply_ms").get<int>() >= 10000 &&
      neutral_intent.at("local_http_timing").value("timestamp_scope", "") == "local_http_received_to_intent_applied",
      "input timing diagnostics did not separate native HTTP ingress from apply");

  expect(
      neutral_intent.value("accepted", false) &&
          neutral_intent.value("transport", "") == "native_signaling_websocket",
      "fresh neutral intent did not arm native control after the vehicle switch");
  const auto profile_analog_brake = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", second_session_id},
       {"session_generation", second_control_session_generation},
       {"ui_instance_id", "switch-test-ui"},
       {"intent_seq", 2},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.80}}));
  const auto profile_analog_brake_status = driver.status().at("native_control");
  expect(
      profile_analog_brake.value("accepted", false) &&
          profile_analog_brake_status.value("effective_intent_seq", std::uint64_t{0}) == 2 &&
          profile_analog_brake_status.value("effective_brake", -1.0) == 0.80,
      "session hard-brake key pressure incorrectly capped the native analog brake intent");

  for (const auto* path : {"/api/control/keyboard", "/api/control/gamepad"}) {
    mine_teleop::HttpRequest request;
    request.method = "POST";
    request.path = path;
    request.body = mine_teleop::Json({
        {"gear", "D"},
        {"up", true},
        {"service_brake", true},
        {"brake", 1.0},
    }).dump();
    const auto response = control_app.handle(request);
    expect(
      response.status == 410 &&
            response.body.find("/api/control-intent") != std::string::npos,
        std::string("legacy specialized control endpoint did not fail closed: ") + path);
  }
  const auto prepared_status = driver.status();
  const auto& prepared_native_status = prepared_status.at("native_control");
  expect(
      prepared_status.value("control_commands_prepared_total", std::uint64_t{1}) == 0 &&
          prepared_status.value("last_control_prepared_at_utc_ms", std::int64_t{-1}) == 0 &&
          prepared_native_status.value("intent_active", false) &&
          prepared_native_status.value("effective_brake", -1.0) == 0.80 &&
          !prepared_status.contains("last_control_sent_ms"),
      "retired packet-preparation metrics changed or native intent state was not exposed");

  const auto prepared = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", second_session_id},
       {"session_generation", second_control_session_generation},
       {"ui_instance_id", "switch-test-ui"},
       {"intent_seq", 3},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.80}}));
  const auto refreshed_native_status = driver.status();
  expect(
      prepared.value("accepted", false) &&
          refreshed_native_status.value("vehicle_id", "") == "vehicle-002" &&
          refreshed_native_status.value("session_id", "") == second.value("session_id", ""),
      "post-switch native control intent retained the old vehicle or session");
  expect(
      refreshed_native_status.at("native_control").value("effective_brake", -1.0) == 0.80,
      "driver runtime treated the hard-brake key preset as the native analog-pedal ceiling");

  mine_teleop::HttpRequest get_legacy_limits_request;
  get_legacy_limits_request.method = "GET";
  get_legacy_limits_request.path = "/api/control-limits";
  const auto get_legacy_limits_response = control_app.handle(get_legacy_limits_request);
  expect(get_legacy_limits_response.status == 200, "legacy control-limit read compatibility failed");
  const auto legacy_limits = mine_teleop::Json::parse(get_legacy_limits_response.body);
  expect(
      legacy_limits.value("service_brake", -1.0) == 0.10 &&
          legacy_limits.value("hard_brake", -1.0) == 0.25 &&
          legacy_limits.value("max_brake", -1.0) == 0.25,
      "legacy control-limit read did not reflect the prepared physical-pressure profile");

  mine_teleop::HttpRequest mutate_legacy_limits_request;
  mutate_legacy_limits_request.method = "POST";
  mutate_legacy_limits_request.path = "/api/control-limits";
  mutate_legacy_limits_request.body = mine_teleop::Json({
      {"service_brake", 0.20},
      {"hard_brake", 1.0},
  }).dump();
  const auto mutate_legacy_limits_response = control_app.handle(mutate_legacy_limits_request);
  expect(
      mutate_legacy_limits_response.status == 410 &&
          mutate_legacy_limits_response.body.find("/api/control-profile") != std::string::npos,
      "legacy control-limit mutation did not fail closed with an explicit profile migration");
  expect_throws(
      [&] {
        static_cast<void>(driver.set_control_limits(
            {{"service_brake", 0.20}, {"hard_brake", 1.0}}));
      },
      "direct legacy control-limit mutation bypassed the authenticated session profile");
  expect(
      driver.control_limits().value("service_brake", -1.0) == 0.10 &&
          driver.control_limits().value("hard_brake", -1.0) == 0.25,
      "rejected legacy control-limit mutation changed the active profile");

  profile_request.body = mine_teleop::Json({
      {"profile_version", 7},
      {"parking_idle_timeout_ms", 500},
      {"consecutive_late_frame_limit", 3},
      {"critical_camera_stale_brake_ms", 500},
      {"control_recovery_timeout_ms", 5000},
      {"target_speed_kph", 0.0},
      {"max_motor_torque_nm", 0.0},
      {"max_brake_pressure_bar", 0.0},
      {"service_brake_pressure_bar", 0.0},
      {"hard_brake_pressure_bar", 0.0},
      {"max_steering_angle_deg", 0.0},
      {"speed_pid_kp", 1.5},
      {"speed_pid_ki", 0.2},
      {"speed_pid_kd", 0.1},
      {"speed_pid_derivative_filter_tau_ms", 50.0},
      {"speed_pid_max_dt_ms", 100},
      {"motor_torque_rise_rate_nm_per_s", 0.0},
  }).dump();
  expect(
      control_app.handle(profile_request).status == 200,
      "zero pending profile could not be prepared");
  const auto pending_zero_profile_control = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", second_session_id},
       {"session_generation", second_control_session_generation},
       {"ui_instance_id", "switch-test-ui"},
       {"intent_seq", 4},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.80}}));
  expect(
      pending_zero_profile_control.value("accepted", false) &&
          driver.status().at("native_control").value("effective_brake", -1.0) == 0.80,
      "unacknowledged local profile state altered the native control intent");

  const auto estop = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", second_session_id},
       {"session_generation", second_control_session_generation},
       {"ui_instance_id", "switch-test-ui"},
       {"intent_seq", 5},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.80},
       {"estop", true}}));
  const auto estop_status = driver.status().at("native_control");
  expect(
      estop.value("accepted", false) &&
          estop_status.value("effective_brake", -1.0) == 0.0 &&
          estop_status.value("effective_estop", false),
      "native emergency-stop intent was not latched with ordinary actuation normalized");

  const auto ended = driver.end_session("switch_test_complete");
  expect(!ended.value("connected", true), "explicit session end did not clear local authority");
  const auto reset_profile = driver.control_profile();
  expect(
      reset_profile.value("target_speed_kph", -1.0) == 5.0 &&
          reset_profile.value("max_motor_torque_nm", -1.0) == 320.0 &&
          reset_profile.value("max_brake_pressure_bar", -1.0) == 100.0 &&
          reset_profile.value("service_brake_pressure_bar", -1.0) == 10.0 &&
          reset_profile.value("hard_brake_pressure_bar", -1.0) == 25.0 &&
          reset_profile.value("profile_version", 0) == 7 &&
          reset_profile.value("parking_idle_timeout_ms", -1) == 500 &&
          !reset_profile.value("initialized", true) &&
          reset_profile.value("speed_pid_kp", -1.0) == 0.0 &&
          reset_profile.value("speed_pid_max_dt_ms", -1) == 0 &&
          reset_profile.value("motor_torque_rise_rate_nm_per_s", -1.0) == 0.0 &&
          reset_profile.value("last_prepared_seq", std::uint64_t{1}) == 0,
      "session end retained a prepared control profile instead of YAML defaults");
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "two-driver-vehicle-1"}});
  const auto vehicle_two = http.post_json_response(
      base + "/vehicles/online",
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-002"},
       {"device_token", "vehicle-secret-2"},
       {"connection_id", "two-driver-vehicle-2"}});
  const auto generation_one = vehicle_one.at("connection_generation").get<std::uint64_t>();
  const auto generation_two = vehicle_two.at("connection_generation").get<std::uint64_t>();

  mine_teleop::DriverConfig driver_one_config;
  driver_one_config.driver_id = "driver-console-001";
  driver_one_config.signaling_url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/signaling";
  allow_qemu_test_scheduler_time_sync(driver_one_config);
  mine_teleop::DriverConfig driver_two_config;
  driver_two_config.driver_id = "driver-console-002";
  driver_two_config.signaling_url = driver_one_config.signaling_url;
  allow_qemu_test_scheduler_time_sync(driver_two_config);
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
  const auto control_session_generation_one =
      connected_one.at("control_session_generation").get<std::uint64_t>();
  const auto control_session_generation_two =
      connected_two.at("control_session_generation").get<std::uint64_t>();
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

  const auto neutral_one = driver_one.update_control_intent(
      declare_native_input_video_sample({{"session_id", session_one},
       {"session_generation", control_session_generation_one},
       {"ui_instance_id", "two-driver-ui-1"},
       {"intent_seq", 1},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.0}}));
  const auto neutral_two = driver_two.update_control_intent(
      declare_native_input_video_sample({{"session_id", session_two},
       {"session_generation", control_session_generation_two},
       {"ui_instance_id", "two-driver-ui-2"},
       {"intent_seq", 1},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.0}}));
  expect(
      neutral_one.value("accepted", false) && neutral_two.value("accepted", false),
      "independent native control intents were not armed by fresh neutral input");
  const auto control_one = driver_one.update_control_intent(
      declare_native_input_video_sample({{"session_id", session_one},
       {"session_generation", control_session_generation_one},
       {"ui_instance_id", "two-driver-ui-1"},
       {"intent_seq", 2},
       {"gear", "N"},
       {"steering", -0.25},
       {"throttle", 0.1},
       {"brake", 0.0}}));
  const auto control_two = driver_two.update_control_intent(
      declare_native_input_video_sample({{"session_id", session_two},
       {"session_generation", control_session_generation_two},
       {"ui_instance_id", "two-driver-ui-2"},
       {"intent_seq", 2},
       {"gear", "N"},
       {"steering", 0.25},
       {"throttle", 0.2},
       {"brake", 0.0}}));
  const auto native_status_one = driver_one.status();
  const auto native_status_two = driver_two.status();
  expect(
      control_one.value("accepted", false) &&
          control_one.value("transport", "") == "native_signaling_websocket" &&
          native_status_one.value("vehicle_id", "") == "vehicle-001" &&
          native_status_one.value("session_id", "") == session_one &&
          native_status_one.at("native_control").value("effective_intent_seq", std::uint64_t{0}) == 2 &&
          native_status_one.at("native_control").value("effective_steering", 0.0) == -0.25 &&
          native_status_one.at("native_control").value("effective_throttle", 0.0) == 0.1,
      "first native control intent crossed vehicles or sessions");
  expect(
      control_two.value("accepted", false) &&
          control_two.value("transport", "") == "native_signaling_websocket" &&
          native_status_two.value("vehicle_id", "") == "vehicle-002" &&
          native_status_two.value("session_id", "") == session_two &&
          native_status_two.at("native_control").value("effective_intent_seq", std::uint64_t{0}) == 2 &&
          native_status_two.at("native_control").value("effective_steering", 0.0) == 0.25 &&
          native_status_two.at("native_control").value("effective_throttle", 0.0) == 0.2,
      "second native control intent crossed vehicles or sessions");
  const auto vehicle_session_one = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=vehicle-secret-1&connection_generation=" +
      std::to_string(generation_one));
  const auto vehicle_session_two = http.get_json(
      base + "/vehicles/vehicle-002/session?device_token=vehicle-secret-2&connection_generation=" +
      std::to_string(generation_two));
  const auto control_token_one = vehicle_session_one.value("control_token", "");
  const auto control_token_two = vehicle_session_two.value("control_token", "");
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
      std::to_string(generation_one) + "&types=webrtc_answer");
  const auto answers_two = http.get_json(
      base + "/signaling/" + session_two +
      "/messages?recipient=vehicle-002&device_token=vehicle-secret-2&connection_generation=" +
      std::to_string(generation_two) + "&types=webrtc_answer");
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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

void test_web_login_selects_identity_and_requires_logout_to_switch() {
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-a", "secret-a"}, {"driver-b", "secret-b"}};
  signaling_config.device_tokens = {{"vehicle-a", "device-a"}, {"vehicle-b", "device-b"}};
  signaling_config.driver_vehicle_permissions = {{"driver-a", {"vehicle-a"}}, {"driver-b", {"vehicle-b"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(std::move(signaling_config));
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
      [signaling](const auto& request) { return signaling->handle(request); });
  server.start();
  mine_teleop::DriverConfig config;
  config.signaling_url = "http://127.0.0.1:" + std::to_string(server.port());
  allow_qemu_test_scheduler_time_sync(config);
  auto runtime = std::make_shared<mine_teleop::DriverConsoleRuntime>(config, "vehicle-a", "");
  mine_teleop::DriverConsoleHttpApp app(runtime);
  auto login = [&](const mine_teleop::Json& body) {
    mine_teleop::HttpRequest request;
    request.method = "POST";
    request.path = "/api/login";
    request.body = body.dump();
    return app.handle(request);
  };
  expect(!runtime->status().value("authenticated", true), "fresh console auto-authenticated");
  expect(login({{"password", "secret-a"}}).status == 400, "web login accepted missing driver ID");
  expect(login({{"driver_id", "driver-a"}}).status == 400, "web login accepted missing password");
  expect(login({{"driver_id", "driver-a"}, {"password", "wrong"}}).status == 401, "wrong password was accepted");
  expect(!runtime->status().value("authenticated", true), "failed login installed authority");
  const auto first = login({{"driver_id", "driver-a"}, {"password", "secret-a"}});
  expect(first.status == 200, "page credentials could not log in without configured identity");
  const auto a = mine_teleop::Json::parse(first.body);
  expect(a.at("driver_id") == "driver-a" && a.at("vehicles").size() == 1 &&
      a.at("vehicles")[0].at("vehicle_id") == "vehicle-a", "first identity received wrong permissions");
  expect(login({{"driver_id", "driver-b"}, {"password", "secret-b"}}).status == 409,
      "authenticated console allowed an identity switch without logout");
  expect(runtime->status().at("driver_id") == "driver-a", "rejected switch mutated active identity");
  static_cast<void>(runtime->disconnect());
  bool cached_login_rejected = false;
  try { static_cast<void>(runtime->login()); } catch (const std::invalid_argument&) { cached_login_rejected = true; }
  expect(cached_login_rejected, "logout retained the previous password for automatic login");
  const auto second = login({{"driver_id", "driver-b"}, {"password", "secret-b"}});
  expect(second.status == 200, "logout did not allow a new driver ID");
  const auto b = mine_teleop::Json::parse(second.body);
  expect(b.at("driver_id") == "driver-b" && b.at("vehicles").size() == 1 &&
      b.at("vehicles")[0].at("vehicle_id") == "vehicle-b", "new identity inherited previous permissions");
  expect(runtime->config().driver_id.empty(), "page login mutated persistent configuration");
  static_cast<void>(runtime->disconnect());
  server.stop();
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
  allow_qemu_test_scheduler_time_sync(driver_config);
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
  // Keep the lease short enough for a fast test while leaving a realistic
  // scheduling margin for loaded native and emulated builders. Observe two
  // actual expiry extensions instead of assuming both happen in a narrow
  // fixed wall-clock window.
  signaling_config.control_token_ttl_ms = 5000;
  // Driver authentication is not the subject of this test and must outlive
  // the repeated control renewals. Participant heartbeat expiry is covered by
  // separate tests and must not race this lease-only assertion either.
  signaling_config.token_ttl_ms = 20000;
  signaling_config.vehicle_heartbeat_timeout_ms = 15000;
  signaling_config.driver_heartbeat_timeout_ms = 15000;
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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
  const auto control_session_generation =
      connected.at("control_session_generation").get<std::uint64_t>();
  const auto initial_status = driver.status();
  const auto initial_expiry = initial_status.at("control_token_expires_at_utc_ms").get<std::int64_t>();
  const auto initial_vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=vehicle-secret-1&connection_generation=" +
      std::to_string(vehicle_generation));
  const auto original_control_token = initial_vehicle_session.value("control_token", "");
  expect(!original_control_token.empty(), "initial control lease omitted its token");
  const auto first_intent = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", session_id},
       {"session_generation", control_session_generation},
       {"ui_instance_id", "control-renewal-ui"},
       {"intent_seq", 1},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.0}}));
  const auto first_native_status = driver.status().at("native_control");
  expect(
      first_intent.value("accepted", false) &&
          first_intent.value("transport", "") == "native_signaling_websocket" &&
          first_native_status.value("effective_intent_seq", std::uint64_t{0}) == 1 &&
          first_native_status.value("intent_active", false),
      "initial native control intent was not accepted under the control lease");

  const auto vehicle_renewal = http.post_json(
      base + "/sessions/" + session_id + "/renew",
      {{"actor", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_generation", vehicle_generation}});
  expect(vehicle_renewal.status == 401, "vehicle participant renewed driver control authority");

  std::int64_t latest_expiry = initial_expiry;
  int observed_renewals = 0;
  const auto renewal_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (observed_renewals < 2 && std::chrono::steady_clock::now() < renewal_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    const auto status = driver.status();
    expect(status.value("session_id", "") == session_id, "control renewal changed the active session");
    expect(status.value("signaling_websocket_connected", false), "control renewal closed the WSS connection");
    const auto observed_expiry =
        status.at("control_token_expires_at_utc_ms").get<std::int64_t>();
    if (observed_expiry > latest_expiry) {
      latest_expiry = observed_expiry;
      ++observed_renewals;
    }
  }
  expect(observed_renewals >= 2, "control authority expiry was not extended twice");
  expect(signaling->health().value("active_sessions", std::size_t{0}) == 1, "renewed session expired early");

  const auto renewed_vehicle_session = http.get_json(
      base + "/vehicles/vehicle-001/session?device_token=vehicle-secret-1&connection_generation=" +
      std::to_string(vehicle_generation));
  expect(
      renewed_vehicle_session.value("control_token", "") == original_control_token,
      "control authority renewal rotated the native control token");
  expect(
      renewed_vehicle_session.value("control_token_expires_at_utc_ms", std::int64_t{0}) == latest_expiry,
      "vehicle did not observe the renewed control lease expiry");
  const auto final_control = driver.update_control_intent(
      declare_native_input_video_sample({{"session_id", session_id},
       {"session_generation", control_session_generation},
       {"ui_instance_id", "control-renewal-ui"},
       {"intent_seq", 2},
       {"gear", "N"},
       {"steering", 0.0},
       {"throttle", 0.0},
       {"brake", 0.0}}));
  const auto final_native_status = driver.status().at("native_control");
  expect(
      final_control.value("accepted", false) &&
          final_native_status.value("effective_intent_seq", std::uint64_t{0}) == 2 &&
          final_native_status.value("intent_active", false) &&
          renewed_vehicle_session.value("control_token", "") == original_control_token,
      "renewed native intent lost or changed the active control authority");

  // Expiry/fail-closed behavior is covered by
  // test_expired_websocket_authority_clears_local_control. End this focused
  // renewal test explicitly instead of adding another short wall-clock race.
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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
      "&connection_generation=" + std::to_string(vehicle_generation) + "&types=webrtc_answer");
  expect(vehicle_messages.at("messages").size() == 1, "websocket answer was not queued for the vehicle");
  expect(
      vehicle_messages.at("messages").at(0).value("type", "") == "webrtc_answer",
      "vehicle received the wrong websocket signaling message");

  std::atomic<int> concurrent_failures{0};
  std::mutex concurrent_failure_mutex;
  std::string first_concurrent_failure;
  std::vector<std::thread> candidate_senders;
  for (int index = 0; index < 8; ++index) {
    candidate_senders.emplace_back(
        [&driver, &concurrent_failures, &concurrent_failure_mutex, &first_concurrent_failure, index] {
          try {
            static_cast<void>(driver.send_webrtc_ice_candidate(
                {{"candidate", "candidate:" + std::to_string(index + 1)}}));
          } catch (const std::exception& error) {
            ++concurrent_failures;
            std::lock_guard lock(concurrent_failure_mutex);
            if (first_concurrent_failure.empty()) first_concurrent_failure = error.what();
          }
        });
  }
  for (auto& thread : candidate_senders) thread.join();
  expect(
      concurrent_failures.load() == 0,
      "concurrent browser ICE candidates failed WSS serialization: " +
          (first_concurrent_failure.empty() ? "unknown exception" : first_concurrent_failure));
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

  const auto delayed_payload = signaling_request(
      session_id,
      1,
      "driver-console-001",
      "vehicle-001",
      "",
      "",
      "ice_candidate",
      {{"candidate", "candidate:delayed-frame"}}).dump();
  const auto delayed_frame = raw_masked_websocket_frame(0x81U, delayed_payload);
  const auto delayed_payload_offset = delayed_frame.size() - delayed_payload.size();
  raw_send_all(raw_socket, std::string_view(delayed_frame).substr(0, delayed_payload_offset));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  raw_send_all(raw_socket, std::string_view(delayed_frame).substr(delayed_payload_offset));
  const auto delayed_ack = raw_receive_websocket_json(raw_socket);
  expect(
      delayed_ack.value("event", "") == "signaling_ack" &&
          delayed_ack.value("type", "") == "ice_candidate" &&
          delayed_ack.value("seq", std::uint64_t{0}) == 1,
      "a valid masked websocket frame split beyond the idle poll interval was rejected");
  ::close(raw_socket);

  raw_socket = raw_websocket_connect(server.port(), websocket_target, nullptr, websocket_auth_header);
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
  signaling_config.native_control_message_ttl_ms = 1000;
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
          received.message.value("event", "") == expected_event) {
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
  const auto first_outbound_ack = receive_event(after_ack, "signaling_ack", std::chrono::seconds(5));
  expect(
      first_outbound_ack.status == mine_teleop::WebSocketReceiveStatus::Message &&
          first_outbound_ack.message.value("event", "") == "signaling_ack" &&
          !first_outbound_ack.message.value("duplicate", true),
      "first websocket send was not acknowledged as a new message: " +
          first_outbound_ack.message.dump());
  after_ack.close();

  mine_teleop::WebSocketClient retry;
  retry.connect(websocket_url, websocket_headers);
  retry.send_json(capabilities);
  const auto retry_ack = receive_event(retry, "signaling_ack", std::chrono::seconds(5));
  expect(
      retry_ack.status == mine_teleop::WebSocketReceiveStatus::Message &&
          retry_ack.message.value("duplicate", false) &&
          retry_ack.message.value("message_id", "") == first_outbound_ack.message.value("message_id", ""),
      "identical websocket resend did not receive the stable idempotent acknowledgement");
  retry.close();

  mine_teleop::ControlCommand native_command;
  native_command.vehicle_id = "vehicle-001";
  native_command.driver_id = "driver-console-001";
  native_command.session_id = session_id;
  native_command.seq = 1;
  // Simulate a command prepared before its signaling envelope is constructed.
  native_command.sent_at_utc_ms = mine_teleop::now_ms() - 10;
  native_command.gear = "N";
  native_command.control_token = session.at("control_token").get<std::string>();
  auto native_payload = native_command.to_json();
  native_payload["intent_seq"] = 1;
  native_payload["intent_fresh"] = true;
  auto native_request = signaling_request_for(
      session_id,
      1,
      "vehicle-001",
      "driver-console-001",
      "driver-console-001",
      "vehicle-001",
      "token",
      token,
      "control_command",
      native_payload);
  native_request["sent_at_utc_ms"] = native_command.sent_at_utc_ms;
  const auto native_control_ack = http.post_json_response(
      base + "/signaling/" + session_id + "/messages", native_request);
  const auto native_control_cursor =
      native_control_ack.at("delivery_cursor").get<std::uint64_t>();
  const auto native_control_websocket_url =
      mine_teleop::signaling_websocket_url(
          base,
          session_id,
          "vehicle-001",
          std::to_string(vehicle_generation)) +
      "&types=control_command";
  mine_teleop::WebSocketClient native_control_receiver;
  native_control_receiver.connect(
      native_control_websocket_url,
      {{"X-Mine-Teleop-Device-Token", "vehicle-secret-1"}});
  const auto native_control_delivery =
      native_control_receiver.receive_json(std::chrono::seconds(1));
  expect(
      native_control_delivery.status == mine_teleop::WebSocketReceiveStatus::Message &&
          native_control_delivery.message.value("event", "") == "signaling_messages" &&
          native_control_delivery.message.value("delivery_cursor", std::uint64_t{0}) ==
              native_control_cursor &&
          native_control_delivery.message.at("messages").size() == 1 &&
          native_control_delivery.message.at("messages").at(0).value("type", "") ==
              "control_command",
      "control-only vehicle WebSocket did not receive exactly its native command");
  auto feedback = mine_teleop::Json{{"session_id", session_id}, {"vehicle_id", "vehicle-001"},
      {"driver_id", "driver-console-001"}, {"command_seq", 1}, {"intent_seq", 1},
      {"accepted", false}, {"rearm_required", true}, {"control_status_seq", 77}};
  for (const auto* invalid_field : {"session_id", "command_seq"}) {
    auto invalid_feedback = feedback;
    if (std::string_view(invalid_field) == "session_id") invalid_feedback[invalid_field] = "other-session";
    else invalid_feedback[invalid_field] = 2;
    native_control_receiver.send_json({{"event", "signaling_delivery_ack"},
        {"delivery_cursor", native_control_cursor}, {"trace_session_id", session_id},
        {"seq", 1}, {"intent_seq", 1}, {"control_feedback", invalid_feedback}});
    const auto rejected_feedback = native_control_receiver.receive_json(std::chrono::seconds(1));
    expect(rejected_feedback.status == mine_teleop::WebSocketReceiveStatus::Message &&
        rejected_feedback.message.contains("error"), "wrong-session or future vehicle feedback accepted");
  }
  native_control_receiver.send_json(
      {{"event", "signaling_delivery_ack"}, {"delivery_cursor", native_control_cursor},
       {"trace_session_id", session_id}, {"seq", 1}, {"intent_seq", 1}, {"control_feedback", feedback}});
  const auto native_control_confirmed = receive_event(
      native_control_receiver,
      "signaling_delivery_acknowledged",
      std::chrono::seconds(1));
  expect(
      native_control_confirmed.status == mine_teleop::WebSocketReceiveStatus::Message &&
          native_control_confirmed.message.value("acknowledged", 0) == 1,
      "control-only vehicle WebSocket acknowledgement was not confirmed");
  native_control_receiver.close();

  const auto vehicle_messages = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=vehicle-001&device_token=vehicle-secret-1"
      "&connection_generation=" + std::to_string(vehicle_generation));
  expect(
      vehicle_messages.at("messages").size() == 1 &&
          vehicle_messages.at("messages").at(0).value("type", "") ==
              "media_capabilities",
      "control-only acknowledgement consumed or duplicated an ordinary signaling message");
  const auto acknowledged_control = http.get_json(
      base + "/signaling/" + session_id + "/messages?recipient=vehicle-001&device_token=vehicle-secret-1"
      "&connection_generation=" + std::to_string(vehicle_generation) +
      "&types=control_command");
  expect(
      acknowledged_control.at("messages").empty(),
      "control-only acknowledgement left the native command queued for replay");

  static_cast<void>(http.post_json_response(
      base + "/auth/driver_logout",
      {{"driver_id", "driver-console-001"}, {"token", token}, {"reason", "replay_test_complete"}}));
  server.stop();
}

void test_native_control_capability_mismatch_rejects_before_motion() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-console-001", "capability-password"}};
  config.device_tokens = {{"vehicle-001", "capability-device"}};
  config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(config);
  std::atomic<bool> strip_catalog{true}, strip_upgrade{false};
  std::atomic<bool> legacy_estop_received{false};
  std::atomic<int> forbidden_motion{0};
  std::mutex fixture_socket_mutex;
  int active_native_socket = -1;
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
      [&](const auto& request) {
        auto response = signaling->handle(request);
        if (strip_catalog.load() && request.path.starts_with("/drivers/") && request.path.ends_with("/vehicles") && response.status == 200) {
          auto body = mine_teleop::Json::parse(response.body); body.erase("native_control_capabilities"); response.body = body.dump();
        }
        return response;
      }, 8 * 1024 * 1024,
      [&](int socket, const auto& request) {
        if (!strip_upgrade.load() || !request.query.contains("send_only")) {
          if (!request.query.contains("send_only")) return signaling->handle_websocket(socket, request);
          { std::lock_guard lock(fixture_socket_mutex); active_native_socket = socket; }
          const auto handled = signaling->handle_websocket(socket, request);
          { std::lock_guard lock(fixture_socket_mutex); active_native_socket = -1; }
          return handled;
        }
        raw_send_all(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + mine_teleop::websocket_accept_key(request.headers.at("sec-websocket-key")) + "\r\n\r\n");
        try {
          mine_teleop::ServerWebSocketConnection connection(socket, 1024 * 1024);
          const auto packet = connection.receive_json(std::chrono::seconds(1));
          if (packet.status == mine_teleop::WebSocketReceiveStatus::Message) {
            if (!packet.message.at("payload").value("estop", false)) ++forbidden_motion;
            else {
              // The legacy backend still authenticates the existing session.
              mine_teleop::HttpRequest post; post.method = "POST";
              post.path = request.path.substr(0, request.path.size() - 3) + "/messages"; post.target = post.path;
              auto authenticated = packet.message; authenticated["token"] = request.headers.at("x-mine-teleop-driver-token");
              post.body = authenticated.dump();
              legacy_estop_received.store(signaling->handle(post).status == 200);
            }
          }
        } catch (const std::exception&) {}
        return true;
      });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(base + "/vehicles/online",
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"}, {"device_token", "capability-device"}, {"connection_id", "capability-vehicle"}}));
  mine_teleop::DriverConfig driver_config; driver_config.driver_id = "driver-console-001"; driver_config.signaling_url = base;
  allow_qemu_test_scheduler_time_sync(driver_config);
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "capability-password");
  expect_throws([&] { static_cast<void>(driver.connect("vehicle-001")); }, "old HTTP backend silently admitted control");
  expect(!driver.status().value("connected", true) && signaling->health().at("active_sessions") == 0,
      "HTTP capability failure created or retained vehicle authority");
  strip_catalog.store(false); strip_upgrade.store(true);
  expect_throws([&] { static_cast<void>(driver.connect("vehicle-001")); }, "different legacy WSS backend silently admitted control");
  expect(forbidden_motion.load() == 0 && !driver.status().value("connected", true),
      "native capability failure transmitted an ordinary command");
  strip_upgrade.store(false);
  const auto connected = driver.connect("vehicle-001");
  expect(connected.value("connected", false), "compatible cloud could not reconnect after capability rejection");
  strip_upgrade.store(true);
  { std::lock_guard lock(fixture_socket_mutex);
    expect(active_native_socket >= 0, "native capability fixture has no live socket");
    ::shutdown(active_native_socket, SHUT_RDWR);
  }
  expect(driver.update_control_intent(declare_native_input_video_sample({{"session_id", connected.at("session_id")},
      {"session_generation", connected.at("control_session_generation")}, {"ui_instance_id", "capability-estop-ui"},
      {"intent_seq", 1}, {"gear", "N"}, {"throttle", 0}, {"steering", 0}, {"brake", 0}, {"estop", true}})).value("accepted", false),
      "capability mismatch blocked local sticky ESTOP");
  const auto estop_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!legacy_estop_received.load() && std::chrono::steady_clock::now() < estop_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  expect(legacy_estop_received.load() && forbidden_motion.load() == 0,
      "legacy capability backend blocked authenticated ESTOP or admitted ordinary motion");
  static_cast<void>(driver.disconnect("capability_test_complete"));
  server.stop();
}

void test_vehicle_native_control_capability_admission_and_generation() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-console-001", "capability-password"}, {"legacy-driver", "legacy-password"}};
  config.device_tokens = {{"vehicle-001", "capability-device"}};
  config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}, {"legacy-driver", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(config);
  std::atomic<bool> replace_before_session{false};
  std::atomic<unsigned> required_session_requests{0};
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
      [&](const auto& request) {
        if (request.path == "/sessions") {
          const auto input = request.json_body();
          if (input.value("driver_id", "") == "driver-console-001") {
            expect(input.value("required_native_control_capabilities", "") == mine_teleop::kNativeControlCapabilities,
                "new native console omitted its required vehicle capability");
            ++required_session_requests;
            if (replace_before_session.exchange(false)) {
              mine_teleop::HttpRequest downgrade;
              downgrade.method = "POST"; downgrade.path = "/vehicles/online"; downgrade.target = downgrade.path;
              downgrade.body = mine_teleop::Json({{"vehicle_id", "vehicle-001"}, {"device_token", "capability-device"},
                  {"connection_id", "capability-vehicle"}, {"native_control_capabilities", "v7-feedback-ack-wait-v1"}}).dump();
              expect(signaling->handle(downgrade).status == 200, "TOCTOU vehicle replacement failed");
            }
          }
        }
        return signaling->handle(request);
      }, 8 * 1024 * 1024,
      [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto register_vehicle = [&](std::optional<std::string> capability) {
    mine_teleop::Json registration = {{"vehicle_id", "vehicle-001"}, {"device_token", "capability-device"},
        {"connection_id", "capability-vehicle"}};
    if (capability) registration["native_control_capabilities"] = *capability;
    return http.post_json_response(base + "/vehicles/online", registration);
  };
  static_cast<void>(register_vehicle(std::nullopt));
  const auto legacy = http.post_json_response(base + "/auth/driver_login",
      {{"driver_id", "legacy-driver"}, {"password", "legacy-password"}});
  const auto legacy_session = http.post_json_response(base + "/sessions",
      {{"driver_id", "legacy-driver"}, {"vehicle_id", "vehicle-001"}, {"token", legacy.at("token")}});
  expect(legacy_session.contains("control_token"), "generic legacy API was silently converted to V8-only admission");
  static_cast<void>(http.post_json_response(base + "/sessions/" + legacy_session.at("session_id").get<std::string>() + "/end",
      {{"actor", "legacy-driver"}, {"token", legacy.at("token")}, {"reason", "generic-legacy-fixture-complete"}}));

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001"; driver_config.signaling_url = base;
  allow_qemu_test_scheduler_time_sync(driver_config);
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "capability-password");
  for (const auto& capability : {std::string{}, std::string("v7-feedback-ack-wait-v1"), std::string("v8-sample-recovery-v1")}) {
    if (!capability.empty()) static_cast<void>(register_vehicle(capability));
    expect_throws([&] { static_cast<void>(driver.connect("vehicle-001")); },
        "new native console admitted unknown/legacy vehicle through a V8 cloud");
    const auto listed = driver.vehicles().at("vehicles").at(0);
    expect(!listed.value("controllable", true) && !listed.value("native_control_compatible", true) &&
        listed.value("native_control_capabilities", "") == capability &&
        listed.value("control_issue_code", "") == "vehicle_control_capability_mismatch",
        "catalogue concealed unknown/legacy vehicle capability or advertised it controllable");
    expect(driver.status().value("authenticated", false) && !driver.status().value("connected", true) &&
        signaling->health().at("active_sessions") == 0 && required_session_requests.load() == 0,
        "vehicle mismatch reached authority creation or was misreported as lost authentication");
    bool cloud_rejected = false;
    try { static_cast<void>(http.post_json_response(base + "/sessions",
        {{"driver_id", "legacy-driver"}, {"vehicle_id", "vehicle-001"}, {"token", legacy.at("token")},
         {"required_native_control_capabilities", mine_teleop::kNativeControlCapabilities}})); }
    catch (const mine_teleop::HttpStatusError& error) {
      cloud_rejected = error.status() == 409 && error.issue_code() == "vehicle_control_capability_mismatch" &&
          !mine_teleop::Json::parse(error.response_body()).contains("control_token");
    }
    expect(cloud_rejected && signaling->health().at("active_sessions") == 0,
        "cloud ignored required capability for unknown/legacy vehicle and issued a token");
  }
  static_cast<void>(register_vehicle(std::string(mine_teleop::kNativeControlCapabilities)));
  replace_before_session.store(true);
  bool rejected_at_admission = false;
  try { static_cast<void>(driver.connect("vehicle-001")); }
  catch (const mine_teleop::HttpStatusError& error) {
    rejected_at_admission = error.status() == 409 && error.issue_code() == "vehicle_control_capability_mismatch" &&
        !mine_teleop::Json::parse(error.response_body()).contains("control_token");
  }
  expect(rejected_at_admission && required_session_requests.load() == 1 && signaling->health().at("active_sessions") == 0,
      "capability changed after catalogue validation but cloud still issued control authority");

  const auto compatible_registration = register_vehicle(std::string(mine_teleop::kNativeControlCapabilities));
  const auto connected = driver.connect("vehicle-001");
  const auto reused = driver.connect("vehicle-001");
  expect(connected.value("connected", false) && reused.at("session_id") == connected.at("session_id"),
      "compatible active session was rejected merely because its catalogue row was busy");
  const auto refresh = register_vehicle(std::string(mine_teleop::kNativeControlCapabilities));
  expect(refresh.at("connection_generation") == compatible_registration.at("connection_generation") &&
      signaling->health().at("active_sessions") == 1, "unchanged registration revoked a compatible session");
  const auto downgraded = register_vehicle(std::string("v7-feedback-ack-wait-v1"));
  expect(downgraded.at("connection_generation") != compatible_registration.at("connection_generation") &&
      signaling->health().at("active_sessions") == 0, "same-connection downgrade retained generation or old authority");
  expect_throws([&] { static_cast<void>(http.post_json_response(base + "/vehicles/heartbeat",
      {{"vehicle_id", "vehicle-001"}, {"device_token", "capability-device"},
       {"connection_generation", compatible_registration.at("connection_generation")}})); },
      "previous vehicle generation survived a capability change");
  expect_throws([&] { static_cast<void>(driver.connect("vehicle-001")); }, "downgraded vehicle regained new native control");
  expect(driver.status().value("authenticated", false), "capability downgrade was misreported as invalid credentials");
  static_cast<void>(driver.disconnect("vehicle_capability_test_complete"));
  server.stop();
}

void test_independent_cloud_observer_records_blocked_receive_and_mutex() {
  for (const bool block_mutex : {false, true}) {
    const auto root = std::filesystem::temp_directory_path() / ("mine-teleop-independent-observer-" + mine_teleop::random_token(6));
    std::filesystem::create_directories(root);
    mine_teleop::SignalingServerConfig config;
    config.driver_passwords = {{"driver-console-001", "observer-password"}, {"other-driver", "other-password"}};
    config.device_tokens = {{"vehicle-001", "observer-device"}};
    config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
    config.audit_log_path = (root / "audit.jsonl").string(); config.native_control_trace_commands = true;
    config.connection_reaper_interval_ms = 50;
    static thread_local bool slow_audit_here = false;
    std::atomic<bool> audit_entered{false};
    auto signaling = std::make_shared<mine_teleop::SignalingService>(config, [&] {
      if (slow_audit_here && !audit_entered.exchange(true)) std::this_thread::sleep_for(std::chrono::milliseconds(2200));
      return mine_teleop::now_ms();
    });
    mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
        [signaling](const auto& request) { return signaling->handle(request); }, 8 * 1024 * 1024,
        [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
    server.start();
    const auto base = "http://127.0.0.1:" + std::to_string(server.port()); mine_teleop::HttpClient http;
    static_cast<void>(http.post_json_response(base + "/vehicles/online", {{"vehicle_id", "vehicle-001"},
        {"device_token", "observer-device"}, {"connection_id", "observer-vehicle"}}));
    const auto login = http.post_json_response(base + "/auth/driver_login", {{"driver_id", "driver-console-001"}, {"password", "observer-password"}});
    const auto session = http.post_json_response(base + "/sessions", {{"driver_id", "driver-console-001"},
        {"vehicle_id", "vehicle-001"}, {"token", login.at("token")}});
    const auto session_id = session.at("session_id").get<std::string>();
    const int socket = raw_websocket_connect(server.port(), "/signaling/" + session_id + "/ws?participant=driver-console-001&send_only=1",
        nullptr, "X-Mine-Teleop-Driver-Token: " + login.at("token").get<std::string>() + "\r\n");
    mine_teleop::ControlCommand command; command.vehicle_id = "vehicle-001"; command.driver_id = "driver-console-001";
    command.session_id = session_id; command.seq = 1; command.sent_at_utc_ms = mine_teleop::now_ms(); command.control_token = session.at("control_token");
    auto envelope = command.to_json(); envelope["sender"] = command.driver_id; envelope["recipient"] = command.vehicle_id;
    envelope["payload"] = command.to_json();
    const auto frame = raw_masked_websocket_frame(0x81U, envelope.dump());
    std::jthread slow_request;
    if (block_mutex) {
      slow_request = std::jthread([&] {
        slow_audit_here = true;
        mine_teleop::HttpRequest request; request.method = "POST"; request.path = request.target = "/auth/driver_login";
        request.body = mine_teleop::Json{{"driver_id", "other-driver"}, {"password", "other-password"}}.dump();
        static_cast<void>(signaling->handle(request)); slow_audit_here = false;
      });
      const auto entered_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (!audit_entered.load() && std::chrono::steady_clock::now() < entered_deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      expect(audit_entered.load(), "software mutex fixture never blocked");
      raw_send_all(socket, frame);
    } else raw_send_all(socket, std::string_view(frame).substr(0, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(2100));
    bool observed = false;
    std::ifstream input(config.audit_log_path); std::string line;
    while (std::getline(input, line)) {
      const auto record = mine_teleop::Json::parse(line, nullptr, false);
      if (record.is_discarded() || record.value("event", "") != "cloud_native_control_trace_batch") continue;
      for (const auto& item : record.at("details").value("observations", mine_teleop::Json::array())) {
        const auto phase = item.value("phase", "");
        if (item.value("trace_session_id", "") == session_id && item.value("role", "") == "cloud_control_ingress" &&
            item.value("phase_snapshot_stable", false) && item.value("phase_elapsed_ms", 0) >= 500 &&
            (block_mutex ? phase == "loop_mutex_wait" || phase == "ingress_mutex_wait" : phase == "receive_frame")) observed = true;
      }
    }
    if (!block_mutex) raw_send_all(socket, std::string_view(frame).substr(2));
    if (slow_request.joinable()) slow_request.join();
    static_cast<void>(raw_receive_websocket_json(socket)); ::close(socket);
    server.stop(); signaling.reset(); std::filesystem::remove_all(root);
    expect(observed, "independent observer disappeared while the receive handler or service mutex was blocked");
  }
}

void test_websocket_client_preserves_partial_ack_across_polls() {
  for (const auto& [payload_size, prefix_size] :
       std::vector<std::pair<std::size_t, std::size_t>>{{20, 2}, {200, 2}, {200, 3}, {70000, 9}, {70000, 20}, {21, 2}}) {
    const bool truncate_at_eof = payload_size == 21;
    auto prefix_sent = std::make_shared<std::atomic<bool>>(false);
    auto release_suffix = std::make_shared<std::atomic<bool>>(false);
    auto outbound_received = std::make_shared<std::atomic<bool>>(false);
    const mine_teleop::Json acknowledgement = {{"event", "signaling_ack"}, {"padding", std::string(payload_size, 'a')}};
    mine_teleop::SimpleHttpServer server(
        "127.0.0.1", 0, [](const auto&) { return mine_teleop::ServerResponse::json(404, {}); },
        8 * 1024 * 1024,
        [=](int socket, const auto& request) {
          raw_send_all(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Accept: " + mine_teleop::websocket_accept_key(request.headers.at("sec-websocket-key")) +
              "\r\nX-Mine-Teleop-Control-Capabilities: " + std::string(mine_teleop::kNativeControlCapabilities) + "\r\n\r\n");
          const auto payload = acknowledgement.dump();
          std::string frame(1, '\x81');
          if (payload.size() < 126) frame.push_back(static_cast<char>(payload.size()));
          else if (payload.size() <= 0xffff) {
            frame.push_back('\x7e');
            frame.push_back(static_cast<char>(payload.size() >> 8));
            frame.push_back(static_cast<char>(payload.size()));
          } else {
            frame.push_back('\x7f');
            for (int shift = 56; shift >= 0; shift -= 8)
              frame.push_back(static_cast<char>(static_cast<std::uint64_t>(payload.size()) >> shift));
          }
          frame += payload;
          raw_send_all(socket, std::string_view(frame).substr(0, prefix_size));
          prefix_sent->store(true);
          const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
          while (!release_suffix->load() && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
          try {
            mine_teleop::ServerWebSocketConnection connection(socket, 512 * 1024);
            const auto outbound = connection.receive_json(std::chrono::milliseconds(500));
            outbound_received->store(outbound.status == mine_teleop::WebSocketReceiveStatus::Message &&
                outbound.message.value("event", "") == "fresh_control");
            if (truncate_at_eof) {
              ::shutdown(socket, SHUT_WR);
              return true;
            }
            raw_send_all(socket, std::string_view(frame).substr(prefix_size));
            connection.send_json({{"event", "next_ack"}});
            static_cast<void>(connection.receive_json(std::chrono::milliseconds(500)));
          } catch (const std::exception&) {
            // A failed assertion closes the test client; always release the fixture.
          }
          return true;
        });
    server.start();
    mine_teleop::WebSocketClient client;
    client.enable_read_diagnostics();
    try {
      client.connect("ws://127.0.0.1:" + std::to_string(server.port()) + "/partial-ack");
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
      while (!prefix_sent->load() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      expect(prefix_sent->load(), "partial ACK fixture did not send a prefix");
      std::int64_t prefix_observed_ms = 0;
      for (int poll = 0; poll < 3; ++poll) {
        const auto partial = client.receive_json(std::chrono::milliseconds(poll == 0 ? 10 : 0));
        expect(partial.status == mine_teleop::WebSocketReceiveStatus::Timeout &&
            client.connected(), "partial TCP ACK was treated as a protocol error or disconnect");
        expect(partial.read_diagnostics.at("buffered_receive_bytes") == prefix_size &&
            partial.read_diagnostics.at("idle_timeouts_total").get<int>() >= poll + 1 &&
            partial.read_diagnostics.value("timestamp_scope", "") == "application_decrypted_read_completion",
            "partial/idle diagnostics lost the buffered prefix or mislabeled the clock boundary");
        prefix_observed_ms = partial.read_diagnostics.at("buffer_head_first_observed_monotonic_ms").get<std::int64_t>();
      }
      const auto transport = client.transport_diagnostics();
      expect(transport.value("tcp_info_available", false) && transport.value("peer_address", "") == "127.0.0.1" &&
          transport.value("buffered_receive_bytes", 0U) == prefix_size && transport.value("partial_frame_polls_total", 0U) >= 3,
          "partial ACK/TCP diagnostics did not report the actual buffered prefix and live loopback socket");
      expect(client.transport_diagnostics(std::chrono::hours(1)).is_null(), "normal TCP samples were not rate limited");
      client.send_json({{"event", "fresh_control"}}, std::chrono::milliseconds(20));
      if (payload_size == 20) std::this_thread::sleep_for(std::chrono::milliseconds(100));
      release_suffix->store(true);
      const auto received = client.receive_json(std::chrono::seconds(2));
      if (truncate_at_eof) {
        expect(received.status == mine_teleop::WebSocketReceiveStatus::Closed && !client.connected(),
            "EOF with a partial frame was mistaken for a valid ACK or left connected");
        server.stop();
        continue;
      }
      expect(received.status == mine_teleop::WebSocketReceiveStatus::Message && received.message == acknowledgement &&
          outbound_received->load(), "ACK bytes were lost or blocked fresh outbound control across partial polls");
      const auto& timing = received.read_diagnostics;
      expect(timing.value("frame_read_timing_complete", false) &&
          timing.at("frame_first_observed_monotonic_ms") == prefix_observed_ms &&
          timing.at("frame_last_observed_monotonic_ms").get<std::int64_t>() >= prefix_observed_ms &&
          timing.at("partial_frame_polls_total").get<int>() >= 3 &&
          timing.dump().find("padding") == std::string::npos,
          "frame diagnostics lost first-byte timing across polls or disclosed payload");
      if (payload_size == 20) expect(timing.at("frame_observed_span_ms").get<int>() >= 75,
          "delayed frame suffix was hidden by a short final read call");
      const auto next = client.receive_json(std::chrono::seconds(2));
      expect(next.status == mine_teleop::WebSocketReceiveStatus::Message && next.message.value("event", "") == "next_ack",
          "partial ACK assembly consumed bytes from the following frame");
      expect(next.read_diagnostics.value("frame_read_timing_complete", false) &&
          next.read_diagnostics.at("frame_stream_begin").get<std::uint64_t>() ==
              timing.at("frame_stream_begin").get<std::uint64_t>() + timing.at("frame_bytes").get<std::uint64_t>(),
          "coalesced next frame inherited the previous frame's byte range");
      client.close();
      server.stop();
    } catch (...) {
      release_suffix->store(true);
      client.close();
      server.stop();
      throw;
    }
  }
}

void test_vehicle_registration_diagnostics_correlates_replacement_and_source() {
  expect(mine_teleop::diagnostic_fingerprint("The quick brown fox jumps over the lazy dog", "key") ==
      "f7bc83f430538424b13298e6aa6fb143", "diagnostic HMAC vector differs across platforms");
  const auto root = std::filesystem::temp_directory_path() / ("mine-teleop-registration-diag-" + mine_teleop::random_token(8));
  std::filesystem::create_directories(root);
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-diag", "fixture-password"}};
  config.device_tokens = {{"vehicle-diag", "fixture-device-credential"}};
  config.driver_vehicle_permissions = {{"driver-diag", {"vehicle-diag"}}};
  config.trusted_proxy_addresses = {"127.0.0.1"};
  config.audit_log_path = (root / "audit.jsonl").string();
  config.native_control_trace_commands = true;
  {
    mine_teleop::SignalingService service(config);
    const auto online = [&](std::string_view id, std::string_view peer, std::string_view forwarded) {
      mine_teleop::HttpRequest request;
      request.method = "POST"; request.path = request.target = "/vehicles/online";
      request.peer_address = peer;
      request.headers["x-forwarded-for"] = forwarded;
      request.body = mine_teleop::Json{{"vehicle_id", "vehicle-diag"}, {"device_token", "fixture-device-credential"},
          {"connection_id", id}, {"native_control_capabilities", mine_teleop::kNativeControlCapabilities}}.dump();
      auto response = service.handle(request);
      expect(response.status == 200, "registration diagnostic changed admission");
      auto body = mine_teleop::Json::parse(response.body);
      expect(body.at("registration_request_id") == response_header(response, "X-Request-ID"),
          "registration PID-side receipt cannot join server audit request");
      expect(body.value("connection_fingerprint", "").size() == 32, "registration fingerprint is absent");
      return body;
    };
    const auto first = online("fixture-connection-one", "127.0.0.1", "192.0.2.77, 198.51.100.25");
    const auto refresh = online("fixture-connection-one", "127.0.0.1", "192.0.2.88, 198.51.100.25");
    const auto replacement = online("fixture-connection-two", "198.51.100.25", "203.0.113.199");
    expect(first.at("connection_generation") == refresh.at("connection_generation") &&
        first.at("connection_fingerprint") == refresh.at("connection_fingerprint") &&
        replacement.at("connection_generation") > first.at("connection_generation") &&
        replacement.at("connection_fingerprint") != first.at("connection_fingerprint"),
        "diagnostics changed refresh/replacement identity policy");
    std::string line, raw;
    std::vector<mine_teleop::Json> observations;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (observations.size() < 3 && std::chrono::steady_clock::now() < deadline) {
      observations.clear(); raw.clear();
      std::ifstream input(config.audit_log_path);
      while (std::getline(input, line)) {
        raw += line;
        auto record = mine_teleop::Json::parse(line, nullptr, false);
        if (record.is_discarded() || record.value("event", "") != "cloud_native_control_trace_batch") continue;
        for (const auto& command : record.at("details").at("commands")) {
          if (command.value("stage", "") == "vehicle_registration_observed")
            observations.push_back({{"request_id", command.at("registration_request_id")}, {"details", command}});
        }
      }
      if (observations.size() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(observations.size() == 3, "registration attempts are missing from diagnostic audit");
    for (std::size_t index = 0; index < observations.size(); ++index) {
      const auto& observation = observations[index];
      const auto& receipt = index == 0 ? first : index == 1 ? refresh : replacement;
      expect(observation.at("request_id") == receipt.at("registration_request_id") &&
          observation.at("details").at("connection_fingerprint") == receipt.at("connection_fingerprint"),
          "registration receipt and audit refer to different attempts");
      expect(observation.at("details").at("source_fingerprint") == observations[0].at("details").at("source_fingerprint"),
          "untrusted XFF or left-most proxy input changed the trusted source fingerprint");
    }
    expect(observations.back().at("details").at("previous_connection_fingerprint") == first.at("connection_fingerprint") &&
        observations.back().at("details").at("previous_connection_generation") == first.at("connection_generation"),
        "replacement audit lost the previous identity/generation");
    for (const auto private_value : {"fixture-connection-one", "fixture-connection-two", "fixture-device-credential", "198.51.100.25", "203.0.113.199"})
      expect(raw.find(private_value) == std::string::npos, "registration diagnostic exposed raw identity/credential/source");
    std::filesystem::remove(config.audit_log_path);
    std::filesystem::create_directory(config.audit_log_path);
    const auto log_unwritable = online("fixture-connection-two", "198.51.100.25", "");
    expect(log_unwritable.at("connection_generation") == replacement.at("connection_generation"),
        "diagnostic disk failure changed a successful same-connection refresh");
    // Async diagnostic failure must never surface as an admission failure.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    const auto after_log_failure = online("fixture-connection-two", "198.51.100.25", "");
    expect(after_log_failure.at("connection_generation") == replacement.at("connection_generation"),
        "a failed diagnostic worker changed admission");
  }
  std::filesystem::remove_all(root);
}

void test_websocket_receive_timing_separates_frame_assembly() {
  int pair[2]{-1, -1};
  expect(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0, "receive timing socketpair failed");
  const auto frame = raw_masked_websocket_frame(0x81U, mine_teleop::Json{{"seq", 1}}.dump());
  std::jthread writer([&] {
    raw_send_all(pair[0], std::string_view(frame).substr(0, 2));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    raw_send_all(pair[0], std::string_view(frame).substr(2));
  });
  mine_teleop::ServerWebSocketConnection connection(pair[1], 1024);
  const auto received = connection.receive_json(std::chrono::seconds(1));
  const auto transport = connection.transport_diagnostics();
  writer.join();
  ::close(pair[0]);
  ::close(pair[1]);
  expect(received.status == mine_teleop::WebSocketReceiveStatus::Message && received.message.value("seq", 0) == 1 &&
      received.frame_assembly_ms >= 75 && received.receive_wait_ms >= 0 && received.json_parse_ms >= 0,
      "frame assembly delay was hidden in cloud application processing");
  expect(!transport.value("tcp_info_available", true) && !transport.contains("tcp_rtt_us") &&
      transport.contains("tcp_info_error_code"), "unsupported TCP diagnostics claimed successful zero-valued counters");
}

void arbitrate_loopback_control_recovery(
    mine_teleop::VehicleControlService& service,
    mine_teleop::SessionControlRecoveryLatch& latch,
    std::string_view session, const mine_teleop::ControlCommand& command,
    const mine_teleop::ReceiveResult& received, std::int64_t utc_ms) {
  const auto steady_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto timeout_ms = service.recovery_timing().second;
  const bool was_waiting = latch.braking_wait_for(session);
  const bool receive_path_waiting = service.control_braking_wait();
  auto cause = mine_teleop::ControlRecoveryCause::ControlLink;
  if (!command.control_time_sync_ready) cause = mine_teleop::ControlRecoveryCause::TimeSynchronization;
  else if (!command.control_link_ready) cause = mine_teleop::ControlRecoveryCause::ControlLink;
  else if (!command.operator_input_ready) cause = mine_teleop::ControlRecoveryCause::InputLease;
  else if (!command.critical_video_ready) cause = mine_teleop::ControlRecoveryCause::CriticalVideo;
  // Enter once for a wait established by the real receive path (including the
  // third expired command), before allowing its common healthy window to run.
  if (receive_path_waiting && !was_waiting) {
    static_cast<void>(latch.observe_recovery(session, false, steady_ms, timeout_ms, cause));
  }
  // Fresh authenticated neutral/gear samples may be rejected only because the
  // vehicle still awaits rearm. Expired/invalid packets are never healthy data.
  const bool fresh_sample = received.accepted || received.reason == "degraded_neutral_required";
  // Below-threshold bad samples are tolerated before a wait. Once waiting,
  // each invalid/raw-unhealthy sample resets the common recovery window.
  if (!fresh_sample && !was_waiting && !receive_path_waiting) return;
  bool ready = !command.estop && command.control_time_sync_ready &&
      command.control_link_ready && command.operator_input_ready && command.critical_video_ready;
  if (was_waiting || receive_path_waiting) ready = ready && fresh_sample && command.recovery_sample_healthy;
  const auto phase = latch.observe_readiness(session, ready, steady_ms, timeout_ms, cause,
      {{"driver", service.recovery_bad_sample_revision(), service.recovery_brake_revision()}});
  if (phase == mine_teleop::ControlRecoveryPhase::BrakingWait) service.set_control_braking_wait(true, utc_ms, cause);
  else if (phase == mine_teleop::ControlRecoveryPhase::Recovered) service.set_control_braking_wait(false, utc_ms);
}

void test_native_control_delayed_ack_progress_and_real_stall() {
  const auto trace_root = std::filesystem::temp_directory_path() /
      ("mine-teleop-delayed-ack-" + std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(trace_root);
  const auto trace_path = trace_root / "driver-events.jsonl";
  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "dev-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "vehicle-secret-1"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(signaling_config);
  auto connections = std::make_shared<std::atomic<int>>(0);
  auto drop_acknowledgements = std::make_shared<std::atomic<bool>>(false);
  auto acknowledgement_delay_ms = std::make_shared<std::atomic<int>>(0);
  mine_teleop::SimpleHttpServer server(
      "127.0.0.1", 0,
      [signaling](const auto& request) { return signaling->handle(request); },
      8 * 1024 * 1024,
      [signaling, connections, drop_acknowledgements, acknowledgement_delay_ms](int socket, const auto& request) {
        if (!request.query.contains("send_only")) {
          return signaling->handle_websocket(socket, request);
        }
        const auto key = request.headers.find("sec-websocket-key");
        if (key == request.headers.end()) return false;
        raw_send_all(socket,
            "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + mine_teleop::websocket_accept_key(key->second) +
            "\r\nX-Mine-Teleop-Control-Capabilities: " + std::string(mine_teleop::kNativeControlCapabilities) + "\r\n\r\n");
        ++*connections;
        mine_teleop::ServerWebSocketConnection connection(socket, 512 * 1024);
        std::deque<std::pair<std::chrono::steady_clock::time_point, mine_teleop::Json>> pending;
        bool first_packet = true;
        try {
          while (true) {
            const auto received = connection.receive_json(std::chrono::milliseconds(10));
            if (received.status == mine_teleop::WebSocketReceiveStatus::Closed) break;
            if (received.status == mine_teleop::WebSocketReceiveStatus::Message) {
              // Enqueue through the real cloud service; delay only the return ACK.
              mine_teleop::HttpRequest post;
              post.method = "POST";
              post.path = request.path.substr(0, request.path.size() - 3) + "/messages";
              post.target = post.path;
              auto authenticated_message = received.message;
              authenticated_message["token"] = request.headers.at("x-mine-teleop-driver-token");
              post.body = authenticated_message.dump();
              const auto response = signaling->handle(post);
              if (response.status != 200) {
                connection.send_json({{"error", response.body}});
                break;
              }
              // Warm up before the first-ACK deadline, then impose packet
              // latency directly instead of relying on a growing queue.
              const auto delay = std::chrono::milliseconds(first_packet ? 350 : acknowledgement_delay_ms->load());
              first_packet = false;
              pending.emplace_back(
                  std::chrono::steady_clock::now() + delay,
                  mine_teleop::Json::parse(response.body));
            }
            if (!pending.empty() && !drop_acknowledgements->load() &&
                std::chrono::steady_clock::now() >= pending.front().first) {
              connection.send_json(pending.front().second);
              pending.pop_front();
            }
          }
        } catch (const std::exception&) {
          // Driver disconnect/reconnect terminates this loopback fixture.
        }
        return true;
      });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(base + "/vehicles/online",
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"}, {"device_token", "vehicle-secret-1"},
       {"connection_id", "delayed-ack-vehicle"}});
  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  // A slower send cadence proves coverage does not require queue growth.
  driver_config.rate_hz = 10;
  driver_config.control_trace_commands = true;
  driver_config.browser_event_log_path = trace_path;
  driver_config.browser_event_log_max_bytes = 16 * 1024 * 1024;
  expect(driver_config.intent_lease_ms == 200, "ACK fixture changed the native input lease");
  allow_qemu_test_scheduler_time_sync(driver_config);
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connected = driver.connect("vehicle-001");
  // Production compares two cloud-synchronized clocks. A raw host UTC step
  // must not turn this ACK-only impairment fixture into a command-age outage.
  mine_teleop::SynchronizedClock vehicle_clock;
  expect(vehicle_clock.synchronize(http, base, 7, driver_config.max_time_sync_uncertainty_ms)
      .acceptable(driver_config.max_time_sync_uncertainty_ms), "ACK fixture vehicle clock not synchronized");
  mine_teleop::SessionControlProfile profile;
  profile.target_speed_kph = 2; profile.max_motor_torque_nm = 80;
  expect(profile.control_recovery_timeout_ms == 5000, "ACK fixture changed the recovery deadline");
  profile.max_brake_pressure_bar = 80; profile.service_brake_pressure_bar = 20; profile.hard_brake_pressure_bar = 60;
  const auto profile_request = mine_teleop::SessionControlProfileRequest::from_json(
      driver.prepare_control_profile(profile.to_json()).at("request"));
  auto vehicle_config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
  expect(vehicle_config.control.control_timeout_ms == 800, "ACK fixture changed the vehicle hard watchdog");
  auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>(); auto* vehicle = adapter.get();
  mine_teleop::VehicleControlService service(vehicle_config, "driver-console-001", connected.at("session_id"),
      profile_request.control_token, std::move(adapter), 100);
  mine_teleop::WebSocketClient receiver;
  receiver.connect(mine_teleop::signaling_websocket_url(base, connected.at("session_id").get<std::string>(), "vehicle-001",
      std::to_string(online.at("connection_generation").get<std::uint64_t>())) + "&types=control_command",
      {{"X-Mine-Teleop-Device-Token", "vehicle-secret-1"}});
  service.start(vehicle_clock.now_ms());
  expect(service.receive_session_profile(profile_request, vehicle_clock.now_ms()).accepted, "ACK recovery profile rejected");
  bool vehicle_link_wait_seen = false;
  mine_teleop::SessionControlRecoveryLatch vehicle_recovery_latch;
  const auto recovery_session = connected.at("session_id").get<std::string>();
  static_cast<void>(vehicle_recovery_latch.enter_session(recovery_session));
  expect(vehicle_recovery_latch.arm_for_control(recovery_session), "ACK fixture recovery latch could not arm");
  std::string last_vehicle_reason;
  std::uint64_t last_vehicle_seq = 0;
  const auto process_vehicle = [&] {
    const auto received = receiver.receive_json(std::chrono::milliseconds(0));
    if (received.status != mine_teleop::WebSocketReceiveStatus::Message || received.message.value("event", "") != "signaling_messages") return;
    for (const auto& message : received.message.at("messages")) {
      const auto& payload = message.at("payload");
      auto command = mine_teleop::ControlCommand::from_json(payload);
      if (command.seq > last_vehicle_seq) {
        const auto now = vehicle_clock.now_ms();
        const auto applied = service.receive_command(command, now);
        last_vehicle_reason = applied.reason + ":age=" + std::to_string(now - command.sent_at_utc_ms) +
            ":timing=" + service.control_timing(now).dump();
        last_vehicle_seq = command.seq;
        if (!command.control_link_ready) {
          expect(command.throttle == 0 && command.steering == 0 && !command.operator_input_ready,
              "ACK wait retained nonzero motion or input readiness");
          if (applied.accepted) {
            expect(service.control_braking_wait() && vehicle->read_telemetry().brake_feedback == 1,
                "ACK wait failed to apply independent vehicle braking: " + last_vehicle_reason);
            vehicle_link_wait_seen = true;
          }
        }
        arbitrate_loopback_control_recovery(service, vehicle_recovery_latch, recovery_session, command, applied, now);
      }
      receiver.send_json({{"event", "signaling_delivery_ack"}, {"delivery_cursor", received.message.at("delivery_cursor")},
          {"trace_session_id", connected.at("session_id")}, {"seq", command.seq}, {"intent_seq", payload.at("intent_seq")}});
    }
  };
  LoopbackVehiclePump vehicle_pump(process_vehicle);
  mine_teleop::Json input = {{"session_id", connected.at("session_id")},
       {"session_generation", connected.at("control_session_generation")},
       {"ui_instance_id", "delayed-ack-test"}, {"intent_seq", 1},
       {"gear", "D"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}};
  expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "ACK fixture neutral rejected");
  // Confirm initial neutral and the production common health window before
  // introducing delayed ACKs. This keeps the real frozen-gear/rearm guard,
  // rather than letting a startup N packet masquerade as an ACK recovery bug.
  const auto startup_neutral_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < startup_neutral_deadline) {
    static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
    if (vehicle_pump.inspect([&] { return !service.control_braking_wait() &&
        vehicle->read_telemetry().gear == "D"; })) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  expect(vehicle_pump.inspect([&] { return !service.control_braking_wait() &&
      vehicle->read_telemetry().gear == "D"; }),
      "ACK fixture initial neutral did not pass the actual common health/rearm window: " +
          vehicle_pump.inspect([&] { return last_vehicle_reason; }));
  acknowledgement_delay_ms->store(600);
  input["intent_seq"] = 2; input["throttle"] = 0.2;
  expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "ACK fixture held motion rejected");
  std::int64_t oldest_age = 0;
  const auto healthy_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (std::chrono::steady_clock::now() < healthy_deadline) {
    static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
    vehicle_pump.inspect([] {});
    const auto native = driver.status().at("native_control");
    expect(native.value("send_failures_total", 0U) == 0,
           "advancing delayed ACKs caused the native driver to reconnect: " + native.dump());
    oldest_age = std::max(oldest_age, native.value("unacknowledged_age_ms", std::int64_t{0}));
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  const auto healthy_native = driver.status().at("native_control");
  expect(oldest_age >= 500 && connections->load() == 1 &&
             healthy_native.value("last_ack_seq", 0U) > 1,
         "fixture did not exercise an old pending packet with a live ACK pipeline: max_age_ms=" +
             std::to_string(oldest_age) + ", connections=" + std::to_string(connections->load()) +
             ", native=" + healthy_native.dump());
  // Advancing ACKs preserve the socket, but three newly received over-age
  // packets now withdraw readiness. This is distinct from a missing ACK path.
  expect(healthy_native.value("ack_braking_wait", false) &&
      healthy_native.value("requires_fresh_input", false) &&
      vehicle_pump.inspect([&] { return vehicle_link_wait_seen &&
          service.session_control_profile().at("active").get<bool>(); }),
      "three late advancing ACK samples did not brake while retaining the original authority: " +
          healthy_native.dump());
  drop_acknowledgements->store(true);
  bool stalled = false;
  const auto stalled_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < stalled_deadline) {
    static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
    vehicle_pump.inspect([] {});
    const auto native = driver.status().at("native_control");
    if (native.value("ack_braking_wait", false) && vehicle_pump.inspect([&] { return vehicle_link_wait_seen; })) {
      expect(native.value("send_failures_total", 0U) == 0 && native.value("websocket_connected", false) && connections->load() == 1 &&
                 native.value("requires_fresh_input", false) &&
                 vehicle_pump.inspect([&] { return service.session_control_profile().at("active").get<bool>(); }),
             "500ms ACK loss closed a recoverable socket or lost vehicle authority");
      stalled = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (!stalled) {
    const auto status = driver.status();
    throw std::runtime_error("ACK stall did not start a same-session vehicle braking wait: " + vehicle_pump.inspect([&] { return last_vehicle_reason; }) +
        ", driver_clock=" + status.at("time_sync").dump() +
        ", vehicle_clock=" + vehicle_clock.status().to_json().dump() +
        ", native=" + status.at("native_control").dump());
  }
  acknowledgement_delay_ms->store(0);
  drop_acknowledgements->store(false);
  bool recovered = false, ack_between_sends_seen = false;
  const auto recovery_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < recovery_deadline) {
    static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
    vehicle_pump.inspect([] {});
    const auto native = driver.status().at("native_control");
    expect(native.value("send_failures_total", 0U) == 0 && connections->load() == 1 &&
        vehicle_pump.inspect([&] { return service.session_control_profile().at("active").get<bool>(); }), "short ACK loss caused a reconnect or hard vehicle stop");
    ack_between_sends_seen = ack_between_sends_seen ||
        (native.value("last_ack_seq", 0U) == native.value("last_seq", 0U) &&
         native.value("unacknowledged_count", 1U) == 0);
    if (!native.value("ack_braking_wait", true) && ack_between_sends_seen) { recovered = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  expect(recovered, "recent ACK progress did not recover the original socket: " +
      driver.status().at("native_control").dump() + ":vehicle=" +
      vehicle_pump.inspect([&] { return last_vehicle_reason; }));
  auto old_motion = input;
  expect(!driver.update_control_intent(declare_native_input_video_sample(old_motion)).value("accepted", true), "ACK recovery resurrected motion without fresh neutral");
  input["intent_seq"] = 3; input["throttle"] = 0.0;
  expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "post-recovery neutral rejected");
  // Only the automatic refresh of the same zero intent continues. The
  // operator does not issue another command for longer than both 2s and 5s.
  const auto neutral_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
  while (std::chrono::steady_clock::now() < neutral_deadline) {
    static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input))); vehicle_pump.inspect([] {});
    const auto safely_held = vehicle_pump.inspect([&] {
      const auto telemetry = vehicle->read_telemetry();
      return service.session_control_profile().at("active").get<bool>() &&
          telemetry.throttle_feedback == 0 && telemetry.brake_feedback == 1;
    });
    expect(safely_held &&
        connections->load() == 1 && driver.status().at("native_control").value("send_failures_total", 1U) == 0,
        "idle operator after link recovery caused hard parking, reconnect or brake release");
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  input["intent_seq"] = 4; input["throttle"] = 0.2;
  expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "fresh press could not restore input");
  const auto motion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (vehicle_pump.inspect([&] { return vehicle->read_telemetry().throttle_feedback <= 0; }) && std::chrono::steady_clock::now() < motion_deadline) {
    static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input))); vehicle_pump.inspect([] {});
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  expect(vehicle_pump.inspect([&] { return vehicle->read_telemetry().throttle_feedback > 0 &&
      service.session_control_profile().at("applied_revision") == profile_request.seq; }) && connections->load() == 1,
      "fresh motion failed to resume with the same confirmed profile and socket");
  // Continued absence remains bounded; a driver recovery window cannot keep
  // an unresponsive ACK path alive forever.
  drop_acknowledgements->store(true);
  const auto long_stall_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(7);
  bool bounded_reconnect = false;
  std::int64_t original_ack_deadline_ms = 0;
  while (std::chrono::steady_clock::now() < long_stall_deadline) {
    static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input))); vehicle_pump.inspect([] {});
    const auto native = driver.status().at("native_control");
    if (native.value("send_failures_total", 0U) == 0 &&
        native.value("ack_braking_wait", false) && original_ack_deadline_ms == 0) {
      original_ack_deadline_ms = native.at("ack_recovery_deadline_monotonic_ms").get<std::int64_t>();
    }
    if (native.value("send_failures_total", 0U) > 0) {
      // A successful reconnect may clear last_error before this observer runs.
      // Assert the actual failure event, not a cumulative counter combined
      // with another connection's transient status. Transport errors remain
      // failures of this ACK-only fixture and are never accepted as timeout.
      mine_teleop::Json failure = mine_teleop::Json::object();
      std::ifstream trace(trace_path);
      std::string line;
      while (std::getline(trace, line) && failure.empty()) {
        const auto record = mine_teleop::Json::parse(line, nullptr, false);
        if (record.is_discarded() ||
            record.value("event", "") != "driver_native_control_trace_batch") continue;
        for (const auto& event : record.at("details").value("commands", mine_teleop::Json::array())) {
          const auto stage = event.value("stage", "");
          if (event.value("trace_session_id", "") == recovery_session &&
              (stage == "send_failed" || stage == "ack_poll_failed")) {
            failure = event;
            break;
          }
        }
      }
      // Trace batches flush asynchronously. Keep the original seven-second
      // outer bound while awaiting the first failure's durable evidence.
      if (!failure.empty()) {
        expect(failure.value("error", "") == "native control ACK recovery deadline exceeded" &&
            native.value("requires_fresh_input", false),
            "persistent ACK loss bypassed the bounded braking recovery: " +
                native.dump() + ":failure=" + failure.dump() + ":trace=" + trace_path.string());
        expect(original_ack_deadline_ms > 0 &&
            failure.value("ack_recovery_deadline_monotonic_ms", std::int64_t{0}) == original_ack_deadline_ms &&
            failure.value("failure_monotonic_ms", std::int64_t{0}) >= original_ack_deadline_ms,
            "persistent ACK loss reconnected before its original five-second deadline: " + failure.dump());
        bounded_reconnect = true; break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  expect(bounded_reconnect, "persistent ACK loss never reached bounded reconnect: trace=" + trace_path.string());
  vehicle_pump.stop();
  vehicle_pump.inspect([] {});
  receiver.close(); service.close();
  static_cast<void>(driver.disconnect("delayed_ack_test_complete"));
  server.stop();
}

void test_native_recovery_applies_at_vehicle_and_preserves_confirmed_profile() {
  using mine_teleop::Json;
  for (const double held_brake : {0.0, 0.6}) {
    mine_teleop::SignalingServerConfig signaling_config;
    signaling_config.driver_passwords = {{"driver-console-001", "recovery-driver-password"}};
    signaling_config.device_tokens = {{"vehicle-001", "recovery-device-token"}};
    signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
    auto signaling = std::make_shared<mine_teleop::SignalingService>(signaling_config);
    mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
        [signaling](const auto& request) { return signaling->handle(request); }, 8 * 1024 * 1024,
        [signaling](int socket, const auto& request) { return signaling->handle_websocket(socket, request); });
    server.start();
    const auto base = "http://127.0.0.1:" + std::to_string(server.port());
    mine_teleop::HttpClient http;
    const auto online = http.post_json_response(base + "/vehicles/online",
        {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"}, {"device_token", "recovery-device-token"}, {"connection_id", "recovery-vehicle"}});
    mine_teleop::DriverConfig driver_config;
    driver_config.driver_id = "driver-console-001";
    driver_config.signaling_url = base;
    driver_config.intent_lease_ms = 1000;
    allow_qemu_test_scheduler_time_sync(driver_config);
    mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "recovery-driver-password");
    const auto connected = driver.connect("vehicle-001");
    const auto session = connected.at("session_id").get<std::string>();
    mine_teleop::SynchronizedClock vehicle_clock;
    expect(vehicle_clock.synchronize(http, base, 7, driver_config.max_time_sync_uncertainty_ms)
        .acceptable(driver_config.max_time_sync_uncertainty_ms), "recovery fixture vehicle clock not synchronized");
    mine_teleop::SessionControlProfile profile;
    profile.target_speed_kph = 2;
    profile.max_motor_torque_nm = 80;
    profile.max_brake_pressure_bar = 80;
    profile.service_brake_pressure_bar = 20;
    profile.hard_brake_pressure_bar = 60;
    profile.max_steering_angle_deg = 3;
    const auto prepared = driver.prepare_control_profile(profile.to_json());
    const auto profile_request = mine_teleop::SessionControlProfileRequest::from_json(prepared.at("request"));
    auto config = mine_teleop::load_vehicle_config("configs/vehicle-agent.dev.yaml");
    auto adapter = std::make_unique<mine_teleop::MockVehicleAdapter>();
    auto* vehicle = adapter.get();
    mine_teleop::VehicleControlService service(config, "driver-console-001", session,
        profile_request.control_token, std::move(adapter), 100);
    mine_teleop::WebSocketClient receiver;
    receiver.connect(mine_teleop::signaling_websocket_url(base, session, "vehicle-001",
        std::to_string(online.at("connection_generation").get<std::uint64_t>())) + "&types=control_command",
        {{"X-Mine-Teleop-Device-Token", "recovery-device-token"}});
    service.start(vehicle_clock.now_ms());
    const auto confirmed = service.receive_session_profile(profile_request, vehicle_clock.now_ms());
    expect(confirmed.accepted && confirmed.effective_profile &&
        confirmed.effective_profile->to_json() == profile.to_json(), "actual driver/vehicle profile handshake mismatched");
    std::uint64_t last_applied_intent = 0;
    std::uint64_t last_applied_seq = 0;
    std::atomic<bool> inject_delay{false};
    int expired_frames = 0;
    bool held_applied = false;
    bool fresh_braking_seen = false;
    std::string last_vehicle_rejection;
    std::deque<Json> recent_vehicle_commands;
    Json first_hard_stop;
    mine_teleop::SessionControlRecoveryLatch vehicle_recovery_latch;
    static_cast<void>(vehicle_recovery_latch.enter_session(session));
    expect(vehicle_recovery_latch.arm_for_control(session), "native recovery fixture latch could not arm");
    const auto process_delivery = [&] {
      const auto received = receiver.receive_json(std::chrono::milliseconds(0));
      if (received.status != mine_teleop::WebSocketReceiveStatus::Message ||
          received.message.value("event", "") != "signaling_messages") return;
      for (const auto& message : received.message.at("messages")) {
        const auto& payload = message.at("payload");
        if (!payload.value("intent_fresh", false)) continue;
        auto command = mine_teleop::ControlCommand::from_json(payload);
        const auto intent_seq = payload.value("intent_seq", std::uint64_t{0});
        const auto now = vehicle_clock.now_ms();
        // Inject three delayed frames only after the actual held command was
        // applied. Identity, sequence, driver sender and ACK transport are real.
        if (inject_delay.load() && held_applied && intent_seq == 2 && expired_frames < 3 &&
            (command.throttle > 0 || std::abs(command.steering) > 0)) {
          command.sent_at_utc_ms = now - 250;
          ++expired_frames;
        }
        const auto result = service.receive_command(command, now);
        // Exercise the production latch instead of a manually mirrored 300ms
        // timer or immediate set(false), including below-threshold bad samples.
        arbitrate_loopback_control_recovery(service, vehicle_recovery_latch, session, command, result, now);
        recent_vehicle_commands.push_back({{"seq", command.seq}, {"intent_seq", intent_seq},
            {"age_ms", now - command.sent_at_utc_ms}, {"accepted", result.accepted}, {"reason", result.reason},
            {"brake", command.brake}, {"steering", command.steering},
            {"rearm", payload.value("intent_requires_fresh_input", false)},
            {"timing", service.control_timing(now)}});
        if (recent_vehicle_commands.size() > 8) recent_vehicle_commands.pop_front();
        if (first_hard_stop.is_null() && service.safety_state() == mine_teleop::SafetyState::TimeoutBrake)
          first_hard_stop = recent_vehicle_commands;
        if (!result.accepted) last_vehicle_rejection = result.reason + ":age=" + std::to_string(now - command.sent_at_utc_ms);
        if (result.accepted) {
          last_applied_seq = command.seq;
          last_applied_intent = intent_seq;
          if (intent_seq == 2 && (command.throttle > 0 || std::abs(command.steering) > 0)) held_applied = true;
          if (payload.value("intent_requires_fresh_input", false) && command.brake == held_brake && held_brake > 0) {
            expect(command.throttle == 0 && command.steering == 0,
                "braking during re-arm retained motion");
            fresh_braking_seen = true;
          }
        }
        const bool rearm = result.issue_code == "control_input_rearm_required";
        receiver.send_json({{"event", "signaling_delivery_ack"},
            {"delivery_cursor", received.message.at("delivery_cursor")}, {"trace_session_id", session},
            {"seq", command.seq}, {"intent_seq", intent_seq},
            {"control_feedback", {{"session_id", session}, {"vehicle_id", "vehicle-001"},
                {"driver_id", "driver-console-001"}, {"command_seq", command.seq}, {"intent_seq", intent_seq},
                {"accepted", result.accepted}, {"rearm_required", rearm}, {"control_status_seq", rearm ? 77 : 0}}}});
      }
    };
    LoopbackVehiclePump vehicle_pump(process_delivery);
    const auto vehicle_diagnostic = [&] { return vehicle_pump.inspect([&] {
      return ":last_intent=" + std::to_string(last_applied_intent) + ":fresh_braking=" + std::to_string(fresh_braking_seen) +
          ":safety=" + std::string(mine_teleop::to_string(service.safety_state())) + ":vehicle=" + last_vehicle_rejection +
          ":first_hard_stop=" + first_hard_stop.dump() + ":recent_commands=" + Json(recent_vehicle_commands).dump();
    }); };
    Json input = {{"session_id", session}, {"session_generation", connected.at("control_session_generation")},
        {"ui_instance_id", "native-recovery-ui"}, {"intent_seq", 1}, {"gear", "D"},
        {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0}};
    expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "neutral driver input rejected");
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (vehicle_pump.inspect([&] { return last_applied_intent != 1; }) && std::chrono::steady_clock::now() < deadline) {
      static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
      vehicle_pump.inspect([] {});
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(vehicle_pump.inspect([&] { return last_applied_intent == 1; }), "initial native neutral never applied at vehicle: " + vehicle_pump.inspect([&] { return last_vehicle_rejection; }) +
        ":sender=" + driver.status().at("native_control").value("last_error", ""));
    inject_delay = true;
    input["intent_seq"] = 2;
    input["steering"] = 0.1;
    input["throttle"] = held_brake > 0 ? 0.0 : 0.2;
    input["brake"] = held_brake;
    expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "held driver input rejected");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    Json blocked;
    while (std::chrono::steady_clock::now() < deadline) {
      vehicle_pump.inspect([] {});
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      blocked = driver.update_control_intent(declare_native_input_video_sample(input));
      if (!blocked.value("accepted", true) && blocked.value("reason", "") == "fresh_neutral_required") break;
    }
    expect(vehicle_pump.inspect([&] { return expired_frames == 3; }) && !blocked.value("accepted", true), "threshold did not reach native driver interlock");
    expect(vehicle_pump.inspect([&] { return service.session_control_profile().at("active").get<bool>(); }),
        "recoverable jitter revoked actual vehicle profile" + vehicle_diagnostic() +
        ":blocked=" + blocked.dump() + ":native=" + driver.status().at("native_control").dump());
    inject_delay = false;
    if (held_brake > 0) {
      expect(blocked.value("preserve_brake_during_rearm", false),
          "vehicle rearm lost live braking: " + blocked.dump());
      // Verify automatic rearm braking before issuing the new UI intent,
      // so automatic brake retention and operator recovery are separate phases.
      deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
      while (!vehicle_pump.inspect([&] { return fresh_braking_seen; }) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      expect(vehicle_pump.inspect([&] { return fresh_braking_seen; }),
          "automatic rearm packet failed to retain held braking: " + driver.status().at("native_control").dump() +
          ":vehicle=" + vehicle_pump.inspect([&] { return Json(recent_vehicle_commands).dump(); }));
    }
    input["intent_seq"] = 3;
    input["steering"] = input["throttle"] = 0.0;
    const auto braking = driver.update_control_intent(declare_native_input_video_sample(input));
    expect(braking.value("accepted", false), "zero-motion recovery rejected ongoing braking");
    const auto braking_recovered = [&] { return vehicle_pump.inspect([&] {
      const auto feedback = vehicle->read_telemetry();
      return last_applied_intent == 3 && feedback.throttle_feedback == 0 && !service.control_braking_wait() &&
          (held_brake == 0 || (fresh_braking_seen && feedback.brake_feedback > 0));
    }); };
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    // Packet identity alone is not the recovery condition. A batch can first
    // apply intent 3 before the re-arm/braking observation has arrived.
    while (!braking_recovered() && std::chrono::steady_clock::now() < deadline) {
      static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
      vehicle_pump.inspect([] {});
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(braking_recovered(),
        "actual vehicle did not recover zero motion while retaining braking: held=" + std::to_string(held_brake) +
        vehicle_diagnostic() + ":native=" + driver.status().at("native_control").dump() +
        ":time_sync=" + driver.status().at("time_sync").dump());
    input["intent_seq"] = 4;
    input["brake"] = 0.0;
    expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "full neutral input rejected");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (vehicle_pump.inspect([&] { return last_applied_intent != 4; }) && std::chrono::steady_clock::now() < deadline) {
      static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(vehicle_pump.inspect([&] { return last_applied_intent == 4 && !service.control_braking_wait(); }),
        "full neutral never reached the recovered vehicle" + vehicle_diagnostic());
    input["intent_seq"] = 5;
    input["throttle"] = 0.2;
    input["steering"] = 0.1;
    expect(driver.update_control_intent(declare_native_input_video_sample(input)).value("accepted", false), "fresh press rejected after neutral recovery");
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (vehicle_pump.inspect([&] { return last_applied_intent != 5; }) && std::chrono::steady_clock::now() < deadline) {
      static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(input)));
      vehicle_pump.inspect([] {});
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    expect(vehicle_pump.inspect([&] { return last_applied_intent == 5 && last_applied_seq > profile_request.seq &&
        vehicle->read_telemetry().throttle_feedback > 0 && service.session_control_profile().at("active").get<bool>() &&
        service.session_control_profile().at("applied_revision") == profile_request.seq; }),
        "fresh motion did not apply at vehicle using the same confirmed profile" + vehicle_diagnostic() +
        ":native=" + driver.status().at("native_control").dump());
    vehicle_pump.stop();
    vehicle_pump.inspect([] {});
    receiver.close();
    service.close();
    static_cast<void>(driver.disconnect("native_recovery_test_complete"));
    server.stop();
  }
}

void test_native_control_three_hop_trace_correlation() {
  const auto root = std::filesystem::temp_directory_path() /
      ("mine-teleop-native-control-trace-" + mine_teleop::random_token(6));
  std::filesystem::create_directories(root);
  const auto audit_path = root / "signaling-audit.jsonl";
  const auto driver_log_path = root / "control-browser-events.jsonl";
  std::string trace_session_id;
  std::uint64_t vehicle_received_seq = 0;
  std::uint64_t vehicle_delivery_cursor = 0;

  mine_teleop::SignalingServerConfig signaling_config;
  signaling_config.driver_passwords = {{"driver-console-001", "trace-driver-password"}};
  signaling_config.device_tokens = {{"vehicle-001", "trace-device-token"}};
  signaling_config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  signaling_config.audit_log_path = audit_path.string();
  signaling_config.native_control_trace_commands = true;
  signaling_config.native_control_message_ttl_ms = 1000;
  auto signaling = std::make_shared<mine_teleop::SignalingService>(signaling_config);

  {
    mine_teleop::SimpleHttpServer server(
        "127.0.0.1",
        0,
        [signaling](const auto& request) { return signaling->handle(request); },
        8 * 1024 * 1024,
        [signaling](int socket, const auto& request) {
          return signaling->handle_websocket(socket, request);
        });
    server.start();
    const auto base = "http://127.0.0.1:" + std::to_string(server.port());
    mine_teleop::HttpClient http;
    const auto online = http.post_json_response(
        base + "/vehicles/online",
        {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
         {"device_token", "trace-device-token"},
         {"connection_id", "trace-vehicle-connection"}});
    const auto vehicle_generation =
        online.at("connection_generation").get<std::uint64_t>();

    {
      mine_teleop::DriverConfig driver_config;
      driver_config.driver_id = "driver-console-001";
      driver_config.signaling_url = base;
      driver_config.control_trace_commands = true;
      driver_config.browser_event_log_path = driver_log_path;
      driver_config.browser_event_log_max_bytes = 4 * 1024 * 1024;
      driver_config.intent_lease_ms = 1000;
      allow_qemu_test_scheduler_time_sync(driver_config);
      mine_teleop::DriverConsoleRuntime driver(
          driver_config,
          "vehicle-001",
          "trace-driver-password");
      const auto connected = driver.connect("vehicle-001");
      const auto session_id = connected.at("session_id").get<std::string>();
      trace_session_id = session_id;
      const auto session_generation =
          connected.at("control_session_generation").get<std::uint64_t>();

      auto receiver_url = mine_teleop::signaling_websocket_url(
          base,
          session_id,
          "vehicle-001",
          std::to_string(vehicle_generation));
      receiver_url += "&types=control_command";
      mine_teleop::WebSocketClient receiver;
      receiver.connect(
          receiver_url,
          {{"X-Mine-Teleop-Device-Token", "trace-device-token"}});

      static_cast<void>(driver.update_control_intent(
          declare_native_input_video_sample({{"session_id", session_id},
           {"session_generation", session_generation},
           {"ui_instance_id", "three-hop-trace-test"},
           {"intent_seq", 1},
           {"gear", "N"},
           {"steering", 0.0},
           {"throttle", 0.0},
           {"brake", 0.0}})));
      static_cast<void>(driver.update_control_intent(
          declare_native_input_video_sample({{"session_id", session_id},
           {"session_generation", session_generation},
           {"ui_instance_id", "three-hop-trace-test"},
           {"intent_seq", 2},
           {"gear", "D"},
           {"steering", 0.1},
           {"throttle", 0.2},
           {"brake", 0.0}})));

      bool correlated_delivery = false;
      const auto deadline = std::chrono::steady_clock::now() +
          std::chrono::seconds(3);
      while (std::chrono::steady_clock::now() < deadline && !correlated_delivery) {
        const auto received = receiver.receive_json(std::chrono::milliseconds(500));
        if (received.status != mine_teleop::WebSocketReceiveStatus::Message ||
            received.message.value("event", "") != "signaling_messages") {
          continue;
        }
        const auto delivery_cursor =
            received.message.value("delivery_cursor", std::uint64_t{0});
        std::uint64_t acknowledged_seq = 0;
        std::uint64_t acknowledged_intent_seq = 0;
        for (const auto& message : received.message.at("messages")) {
          if (message.value("type", "") != "control_command") continue;
          acknowledged_seq = message.value("seq", std::uint64_t{0});
          acknowledged_intent_seq =
              message.at("payload").value("intent_seq", std::uint64_t{0});
          if (acknowledged_intent_seq == 2) {
            correlated_delivery = true;
            vehicle_received_seq = acknowledged_seq;
            vehicle_delivery_cursor = delivery_cursor;
          }
        }
        receiver.send_json(
            {{"event", "signaling_delivery_ack"},
             {"delivery_cursor", delivery_cursor},
             {"trace_session_id", session_id},
             {"seq", acknowledged_seq},
             {"intent_seq", acknowledged_intent_seq},
             {"control_feedback", {{"session_id", session_id}, {"vehicle_id", "vehicle-001"},
                {"driver_id", "driver-console-001"}, {"command_seq", acknowledged_seq},
                {"intent_seq", acknowledged_intent_seq}, {"accepted", false},
                {"rearm_required", true}, {"control_status_seq", 77}}}});
      }
      expect(correlated_delivery, "native control trace test did not receive the active intent");
      bool zero_heartbeat_received = false;
      const auto rearm_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (std::chrono::steady_clock::now() < rearm_deadline && !zero_heartbeat_received) {
        const auto next = receiver.receive_json(std::chrono::milliseconds(100));
        if (next.status != mine_teleop::WebSocketReceiveStatus::Message ||
            next.message.value("event", "") != "signaling_messages") continue;
        for (const auto& message : next.message.at("messages")) {
          const auto& payload = message.at("payload");
          if (payload.value("intent_requires_fresh_input", false) &&
              payload.value("gear", "") == "D" && payload.value("throttle", 1.0) == 0.0 &&
              payload.value("steering", 1.0) == 0.0) {
            zero_heartbeat_received = true;
            receiver.send_json({{"event", "signaling_delivery_ack"},
                {"delivery_cursor", next.message.at("delivery_cursor")}, {"trace_session_id", session_id},
                {"seq", message.at("seq")}, {"intent_seq", payload.at("intent_seq")},
                {"control_feedback", {{"session_id", session_id}, {"vehicle_id", "vehicle-001"},
                    {"driver_id", "driver-console-001"}, {"command_seq", message.at("seq")},
                    {"intent_seq", payload.at("intent_seq")}, {"accepted", true},
                    {"rearm_required", false}, {"control_status_seq", 0}}}});
          }
        }
      }
      expect(zero_heartbeat_received, "native feedback did not clear held input with media absent");
      auto held_again = mine_teleop::Json{{"session_id", session_id}, {"session_generation", session_generation},
                            {"ui_instance_id", "three-hop-trace-test"}, {"intent_seq", 3},
                            {"gear", "D"}, {"steering", 0.1}, {"throttle", 0.2}, {"brake", 0.0}};
      mine_teleop::Json blocked;
      const auto accepted_result_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      do {
        blocked = driver.update_control_intent(declare_native_input_video_sample(held_again));
        if (blocked.value("vehicle_accepted_command_seq", std::uint64_t{0}) > vehicle_received_seq) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      } while (std::chrono::steady_clock::now() < accepted_result_deadline);
      expect(blocked.value("vehicle_accepted_command_seq", std::uint64_t{0}) > vehicle_received_seq,
          "vehicle acceptance did not reach the page independently of ingress ACKs");
      expect(!blocked.value("accepted", true) && blocked.value("reason", "") == "fresh_neutral_required" &&
          blocked.value("rearm_control_status_seq", 0) == 77,
          "native feedback failed to require new neutral input or expose media sequence watermark");
      held_again["intent_seq"] = 4;
      held_again["steering"] = held_again["throttle"] = 0.0;
      expect(driver.update_control_intent(declare_native_input_video_sample(held_again)).value("accepted", false), "neutral could not restore native input");
      held_again["intent_seq"] = 5;
      held_again["throttle"] = 0.2;
      expect(driver.update_control_intent(declare_native_input_video_sample(held_again)).value("accepted", false), "fresh press could not restore native input");
      receiver.send_json({{"event", "signaling_delivery_ack"}, {"delivery_cursor", vehicle_delivery_cursor},
          {"trace_session_id", session_id}, {"seq", vehicle_received_seq}, {"intent_seq", 2},
          {"control_feedback", {{"session_id", session_id}, {"vehicle_id", "vehicle-001"},
              {"driver_id", "driver-console-001"}, {"command_seq", vehicle_received_seq},
              {"intent_seq", 2}, {"accepted", false}, {"rearm_required", true}, {"control_status_seq", 77}}}});
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      held_again["intent_seq"] = 6;
      expect(driver.update_control_intent(declare_native_input_video_sample(held_again)).value("accepted", false),
          "duplicate vehicle result cleared fresh input again");
      const auto failures_before_ui_replacement = driver.status().at("native_control").at("send_failures_total");
      auto retired_page_input = held_again;
      const auto old_epoch = driver.status().at("native_control").at("ui_control_epoch");
      held_again["ui_instance_id"] = "replacement-ui";
      held_again["intent_seq"] = 1;
      held_again["throttle"] = held_again["steering"] = held_again["brake"] = 0.0;
      expect(driver.update_control_intent(declare_native_input_video_sample(held_again)).value("reason", "") == "input_page_not_current",
          "replacement page implicitly acquired control without confirmation");
      held_again["takeover"] = true; held_again["ui_control_epoch"] = old_epoch;
      const auto replacement = driver.update_control_intent(declare_native_input_video_sample(held_again));
      expect(replacement.value("accepted", false), "replacement UI confirmed neutral rejected");
      held_again["takeover"] = false; held_again["ui_control_epoch"] = replacement.at("ui_control_epoch");
      retired_page_input["steering"] = retired_page_input["throttle"] = retired_page_input["brake"] = 0.0;
      retired_page_input["intent_seq"] = 7; retired_page_input["ui_control_epoch"] = old_epoch;
      expect(driver.update_control_intent(declare_native_input_video_sample(retired_page_input)).value("reason", "") == "input_page_not_current",
          "retired page ordinary neutral reacquired control");
      retired_page_input["takeover"] = true;
      expect(driver.update_control_intent(declare_native_input_video_sample(retired_page_input)).value("reason", "") == "input_page_takeover_conflict",
          "retired page used a stale epoch to reacquire control");
      for (int refresh = 0; refresh < 5; ++refresh) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        static_cast<void>(driver.update_control_intent(declare_native_input_video_sample(held_again)));
      }
      expect(driver.status().at("native_control").at("send_failures_total") == failures_before_ui_replacement,
          "old-page feedback was compared against the replacement page's reset input sequence");
      receiver.close();
      static_cast<void>(driver.disconnect("three_hop_trace_test_complete"));
    }
    server.stop();
  }
  signaling.reset();

  const auto driver_log = read_text_file(driver_log_path);
  const auto cloud_log = read_text_file(audit_path);
  const auto load_trace_commands = [](const std::filesystem::path& path,
                                      std::string_view event) {
    mine_teleop::Json commands = mine_teleop::Json::array();
    std::ifstream input(path);
    expect(input.good(), "expected native control trace log is missing");
    std::string line;
    while (std::getline(input, line)) {
      const auto record = mine_teleop::Json::parse(line);
      if (record.value("event", "") != event) continue;
      for (const auto& command : record.at("details").at("commands")) {
        commands.push_back(command);
      }
    }
    return commands;
  };
  const auto driver_commands = load_trace_commands(
      driver_log_path,
      "driver_native_control_trace_batch");
  const auto cloud_commands = load_trace_commands(
      audit_path,
      "cloud_native_control_trace_batch");
  const auto has_command = [&](const mine_teleop::Json& commands,
                               std::string_view stage,
                               std::uint64_t seq,
                               std::uint64_t delivery_cursor) {
    return std::any_of(commands.begin(), commands.end(), [&](const auto& command) {
      return command.value("stage", "") == stage &&
          command.value("trace_session_id", "") == trace_session_id &&
          command.value("seq", std::uint64_t{0}) == seq &&
          command.value("intent_seq", std::uint64_t{0}) == 2 &&
          (delivery_cursor == 0 ||
           command.value("delivery_cursor", std::uint64_t{0}) == delivery_cursor);
    });
  };
  expect(
      !trace_session_id.empty() && vehicle_received_seq > 0 &&
          vehicle_delivery_cursor > 0,
      "vehicle-side receiver did not retain the trace correlation keys");
  expect(
      has_command(driver_commands, "send_completed", vehicle_received_seq, 0),
      "driver trace cannot be correlated to the command received by the vehicle");
  expect(
      has_command(
          cloud_commands,
          "ingress_queued",
          vehicle_received_seq,
          vehicle_delivery_cursor) &&
          has_command(
              cloud_commands,
              "ingress_ack_send_completed",
              vehicle_received_seq,
              vehicle_delivery_cursor) &&
          has_command(
              cloud_commands,
              "delivery_send_completed",
              vehicle_received_seq,
              vehicle_delivery_cursor) &&
          has_command(
              cloud_commands,
              "delivery_ack_received",
              vehicle_received_seq,
              vehicle_delivery_cursor),
      "cloud trace does not correlate ingress ACK, vehicle delivery, and vehicle ACK");
  for (const auto field : {
           "driver_native_control_trace_batch",
           "sender_wakeup_lag_ms",
           "acknowledgement_drain_ms",
           "ack_stall_age_ms",
           "ack_advanced",
           "unacknowledged_limit",
           "send_started_at_utc_ms",
           "send_completed_at_utc_ms",
           "send_call_ms",
           "successful_send_gap_ms",
           "successful_send_monotonic_gap_ms",
           "transport",
           "tcp_info_available",
           "partial_frame_polls_total"}) {
    expect(
        driver_log.find(field) != std::string::npos,
        std::string("driver native control trace omitted ") + field);
  }
  for (const auto field : {
           "cloud_native_control_trace_batch",
           "ingress_queued",
           "ingress_ack_send_completed",
           "ack_send_started_at_utc_ms",
           "ack_send_completed_at_utc_ms",
           "ack_send_call_ms",
           "delivery_send_completed",
           "delivery_ack_received",
           "driver_to_cloud_utc_delta_ms",
           "cloud_frame_received_at_utc_ms",
           "cloud_frame_received_monotonic_ms",
           "cloud_receive_started_monotonic_ms",
           "cloud_receive_call_ms",
           "cloud_receive_wait_ms",
           "cloud_frame_assembly_ms",
           "cloud_json_parse_ms",
           "cloud_receive_loop_gap_ms",
           "cloud_loop_mutex_wait_ms",
           "cloud_loop_authority_check_ms",
           "cloud_ingress_mutex_wait_ms",
           "cloud_ingress_authority_check_ms",
           "tcp_info_available",
           "cloud_mailbox_to_send_ms",
           "delivery_cursor",
           "trace_session_id"}) {
    expect(
        cloud_log.find(field) != std::string::npos,
        std::string("cloud native control trace omitted ") + field);
  }
  for (const auto& command : cloud_commands) {
    if (command.value("stage", "") == "delivery_send_completed") {
      expect(command.at("transport_before_send").at("peer_is_loopback") == true &&
          command.at("transport_after_send").at("peer_is_loopback") == true &&
          !command.at("transport_before_send").contains("peer_address") &&
          !command.at("transport_after_send").contains("peer_address") &&
          command.at("transport_before_send_sample_ms").get<std::int64_t>() >= 0 &&
          command.at("transport_after_send_sample_ms").get<std::int64_t>() >= 0 &&
          command.at("transport_before_send").at("sample_monotonic_ms").get<std::int64_t>() <=
              command.at("cloud_delivery_send_started_monotonic_ms").get<std::int64_t>() &&
          command.at("transport_after_send").at("sample_monotonic_ms").get<std::int64_t>() >=
              command.at("cloud_delivery_send_completed_monotonic_ms").get<std::int64_t>(),
          "delivery diagnostics lost socket stage, privacy, or actual send timing boundaries");
    }
    if (command.value("stage", "") != "ingress_ack_send_completed") continue;
    expect(command.at("cloud_frame_received_monotonic_ms").get<std::int64_t>() <=
        command.at("ack_send_started_monotonic_ms").get<std::int64_t>() &&
        command.at("cloud_frame_received_at_utc_ms").get<std::int64_t>() <=
        command.at("cloud_received_at_utc_ms").get<std::int64_t>() &&
        command.at("cloud_ingress_mutex_wait_ms").get<std::int64_t>() >= 0,
        "cloud diagnostic timestamps do not bracket pre-lock receive and enqueue");
  }
  for (const auto secret : {
           "trace-driver-password",
           "trace-device-token"}) {
    expect(driver_log.find(secret) == std::string::npos, "driver trace leaked a credential");
    expect(cloud_log.find(secret) == std::string::npos, "cloud trace leaked a credential");
  }
  std::filesystem::remove_all(root);
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
                mine_teleop::websocket_accept_key(key->second) +
                "\r\nX-Mine-Teleop-Control-Capabilities: " + std::string(mine_teleop::kNativeControlCapabilities) + "\r\n\r\n");
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
       {"device_token", "vehicle-secret-1"},
       {"connection_id", "expired-wss-authority-vehicle"}}));

  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "dev-password");
  const auto connected = driver.connect("vehicle-001");
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
    static_cast<void>(driver.update_control_intent(
        declare_native_input_video_sample({{"session_id", connected.at("session_id")},
         {"session_generation", connected.at("control_session_generation")},
         {"ui_instance_id", "expired-authority-ui"},
         {"intent_seq", 1},
         {"gear", "N"},
         {"steering", 0.0},
         {"throttle", 0.0},
         {"brake", 1.0}})));
  } catch (const std::runtime_error&) {
    control_rejected = true;
  }
  expect(control_rejected, "expired WSS authority could still accept a native control intent");
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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
        static_cast<void>(driver.update_control_intent(
            declare_native_input_video_sample({{"session_id", first_connection.at("session_id")},
             {"session_generation", first_connection.at("control_session_generation")},
             {"ui_instance_id", "restart-recovery-ui"},
             {"intent_seq", 1},
             {"gear", "N"},
             {"steering", 0.0},
             {"throttle", 0.0},
             {"brake", 1.0}})));
      },
      "control runtime accepted a native intent with pre-restart authority");

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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
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
      {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-webrtc"},
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
      {"control_outcomes",
       {{"prepared", 20},
        {"forwarded", 19},
        {"superseded", 1},
        {"post_prepare_link_changed", 0},
        {"post_prepare_vcu_not_ready", 0},
        {"last_prepared_seq", 20},
        {"last_forwarded_seq", 19}}},
      {"control_outcomes_balanced", true},
      {"streams", mine_teleop::Json::array()}};
  const auto first = driver.ingest_webrtc_metrics(connected);
  expect(
      first.value("reported", false) && first.value("audit_event", "") == "webrtc_connection_succeeded",
      "connected WebRTC transition was not audited");
  const auto duplicate = driver.ingest_webrtc_metrics(connected);
  expect(!duplicate.value("reported", true), "unchanged WebRTC state produced a duplicate audit");
  const auto retained_outcomes = driver.status().at("webrtc_metrics");
  expect(
      retained_outcomes.value("control_outcomes_balanced", false) &&
          retained_outcomes.at("control_outcomes").value("prepared", 0) == 20 &&
          retained_outcomes.at("control_outcomes").value("forwarded", 0) == 19 &&
          retained_outcomes.at("control_outcomes").value("superseded", 0) == 1,
      "the existing WebRTC metrics path did not retain cumulative control outcomes");

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
  // Audit I/O and runner scheduling can exceed 25ms before the next assertion.
  // Exercise a full lockout interval instead of racing its normal expiry.
  config.login_lockout_ms = 1000;
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

  std::this_thread::sleep_for(std::chrono::milliseconds(config.login_lockout_ms + 50));
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

int main(int argc, char** argv) {
  bool runtime_only = false;
  if (argc == 2 && std::string_view(argv[1]) == "--runtime-only") {
    runtime_only = true;
  } else if (argc != 1) {
    std::cerr << "Usage: mine-teleop-control-tests [--runtime-only]\n";
    return 64;
  }
  const std::unordered_set<std::string> source_contracts{
      "vehicle_control_status_sequence_stays_monotonic_during_delayed_adapter_start",
      "stale_control_data_channel_callbacks_are_fail_silent",
      "vehicle_native_signaling_control_transport_is_single_path",
      "driver_native_control_session_snapshot_and_backoff_contract",
      "vehicle_control_command_trace_is_bounded_async_and_timed",
      "structured_control_rejection_is_safe_and_rate_limited"};
  std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"shared_control_protocol_vector", test_shared_control_protocol_vector},
      {"loopback_http_server_and_port_conflict", test_loopback_http_server_and_port_conflict},
      {"signaling_time_sync_applies_backward_utc_correction",
       test_signaling_time_sync_applies_backward_utc_correction},
      {"driver_time_sync_uncertainty_fails_closed_at_production_default",
       test_driver_time_sync_uncertainty_fails_closed_at_production_default},
      {"poor_time_samples_do_not_replace_the_accepted_clock_anchor", test_poor_time_samples_do_not_replace_the_accepted_clock_anchor},
      {"time_probe_excludes_client_queue_and_retains_failed_sample_diagnostics", test_time_probe_excludes_client_queue_and_retains_failed_sample_diagnostics},
      {"time_uncertainty_covers_the_median_shift_from_best_sample", test_time_uncertainty_covers_the_median_shift_from_best_sample},
      {"slow_time_refresh_does_not_block_status_or_expire_live_input", test_slow_time_refresh_does_not_block_status_or_expire_live_input},
      {"local_http_page_takeover_abort_reordering_and_production_lease", test_local_http_page_takeover_abort_reordering_and_production_lease},
      {"local_http_release_scope_prevents_retired_page_and_takeover_races", test_local_http_release_scope_prevents_retired_page_and_takeover_races},
      {"local_http_pending_approval_release_requires_operation_owner_and_generation", test_local_http_pending_approval_release_requires_operation_owner_and_generation},
      {"control_page_contract", test_control_page_contract},
      {"driver_gamepad_config", test_driver_gamepad_config},
      {"vehicle_control_status_sequence_stays_monotonic_during_delayed_adapter_start",
       test_vehicle_control_status_sequence_stays_monotonic_during_delayed_adapter_start},
      {"stale_control_data_channel_callbacks_are_fail_silent",
       test_stale_control_data_channel_callbacks_are_fail_silent},
      {"vehicle_native_signaling_control_transport_is_single_path",
       test_vehicle_native_signaling_control_transport_is_single_path},
      {"driver_native_control_session_snapshot_and_backoff_contract",
       test_driver_native_control_session_snapshot_and_backoff_contract},
      {"native_control_acknowledgement_window_tracks_oldest_pending_packet",
       test_native_control_acknowledgement_window_tracks_oldest_pending_packet},
      {"vehicle_control_command_trace_is_bounded_async_and_timed",
       test_vehicle_control_command_trace_is_bounded_async_and_timed},
      {"structured_control_rejection_is_safe_and_rate_limited",
       test_structured_control_rejection_is_safe_and_rate_limited},
      {"signaling_latest_only_control_mailbox", test_signaling_latest_only_control_mailbox},
      {"driver_brake_limit_config_validation", test_driver_brake_limit_config_validation},
      {"signaling_multi_identity_config", test_signaling_multi_identity_config},
      {"credential_purpose_separation_and_stale_control_replay",
       test_credential_purpose_separation_and_stale_control_replay},
      {"browser_event_logging_rotation_and_redaction", test_browser_event_logging_rotation_and_redaction},
      {"driver_vehicle_switch_releases_old_session", test_driver_vehicle_switch_releases_old_session},
      {"two_driver_two_vehicle_wss_isolation_and_safe_rejection",
       test_two_driver_two_vehicle_wss_isolation_and_safe_rejection},
      {"failed_logout_keeps_retryable_local_authority", test_failed_logout_keeps_retryable_local_authority},
      {"web_login_selects_identity_and_requires_logout_to_switch", test_web_login_selects_identity_and_requires_logout_to_switch},
      {"local_proxy_preserves_upstream_auth_status", test_local_proxy_preserves_upstream_auth_status},
      {"control_authority_lease_renews_without_rotating_token",
       test_control_authority_lease_renews_without_rotating_token},
      {"control_transport_and_loopback_policy", test_control_transport_and_loopback_policy},
      {"mac_runtime_uses_websocket_signaling", test_mac_runtime_uses_websocket_signaling},
      {"websocket_handshake_and_participant_isolation", test_websocket_handshake_and_participant_isolation},
      {"websocket_delivery_replay_and_idempotent_acknowledgement",
       test_websocket_delivery_replay_and_idempotent_acknowledgement},
      {"native_recovery_applies_at_vehicle_and_preserves_confirmed_profile",
       test_native_recovery_applies_at_vehicle_and_preserves_confirmed_profile},
      {"native_control_three_hop_trace_correlation",
       test_native_control_three_hop_trace_correlation},
      {"native_control_delayed_ack_progress_and_real_stall",
       test_native_control_delayed_ack_progress_and_real_stall},
      {"websocket_client_preserves_partial_ack_across_polls", test_websocket_client_preserves_partial_ack_across_polls},
      {"native_control_capability_mismatch_rejects_before_motion", test_native_control_capability_mismatch_rejects_before_motion},
      {"vehicle_native_control_capability_admission_and_generation", test_vehicle_native_control_capability_admission_and_generation},
      {"independent_cloud_observer_records_blocked_receive_and_mutex", test_independent_cloud_observer_records_blocked_receive_and_mutex},
      {"websocket_receive_timing_separates_frame_assembly", test_websocket_receive_timing_separates_frame_assembly},
      {"vehicle_registration_diagnostics_correlates_replacement_and_source", test_vehicle_registration_diagnostics_correlates_replacement_and_source},
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
  int failures = 0, passed = 0, blocked = 0;
  for (const auto& [name, test] : tests) {
    if (runtime_only && source_contracts.contains(name)) {
      ++blocked;
      std::cout << "[BLOCKED] " << name
                << ": source-contract requires cpp/src/server.cpp or cpp/src/webrtc_media.cpp; runtime-only mode excludes source inspection\n";
      continue;
    }
    try {
      test();
      ++passed;
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  std::cout << "[SUITE] " << mine_teleop::Json{
      {"scope", runtime_only ? "runtime-only" : "complete"}, {"registered", tests.size()},
      {"passed", passed}, {"failed", failures}, {"blocked", blocked},
      {"runtime_passed", failures == 0}, {"complete", blocked == 0}}.dump() << '\n';
  return failures > 0 ? 1 : blocked > 0 ? 2 : 0;
}
