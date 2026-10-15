#include "mine_teleop/detail/native_control_acknowledgement_window.hpp"
#include "mine_teleop/http.hpp"
#include "mine_teleop/media.hpp"
#include "mine_teleop/platform.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/websocket.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

namespace {
using mine_teleop::Json;
using mine_teleop::detail::NativeControlAcknowledgementWindow;
using mine_teleop::detail::NativeControlAckRecovery;

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template <typename Function>
void expect_throws(Function function, const std::string& message) {
  try { function(); } catch (const std::exception&) { return; }
  throw std::runtime_error(message);
}

void send_handshake(mine_teleop::SocketHandle socket, const std::string& bytes) {
  std::size_t sent = 0;
  while (sent < bytes.size()) {
#ifdef _WIN32
    const auto count = ::send(static_cast<SOCKET>(socket), bytes.data() + sent,
        static_cast<int>(bytes.size() - sent), 0);
#else
    const auto count = ::send(static_cast<int>(socket), bytes.data() + sent, bytes.size() - sent, 0);
#endif
    if (count <= 0) throw std::runtime_error("loopback ACK handshake failed");
    sent += static_cast<std::size_t>(count);
  }
}

void test_full_window_safety_records_remain_bounded_and_conservative() {
  NativeControlAcknowledgementWindow window;
  // Gaps represent local samples which were never sent. They must not become
  // healthy ACK evidence as a side effect of safety-packet coalescing.
  for (std::uint64_t index = 0; index < 128; ++index) window.note_sent(index * 2 + 1, index);
  expect_throws([&] { window.note_sent(257, 200); }, "ordinary overflow was no longer bounded");
  for (std::uint64_t sequence = 257; sequence < 1257; ++sequence) {
    window.note_sent(sequence, 200 + static_cast<std::int64_t>(sequence), true);
    expect(window.pending_count() == 128 && window.oldest_sequence() == 1 &&
        window.oldest_age_ms(1500) == 1500 && window.stall_age_ms(1500) == 1500,
        "ESTOP accounting exceeded capacity or renewed/lost the oldest timing");
  }
  const auto retired = window.retired_through_sequence();
  expect(retired > 1 && !window.acknowledge_through(retired, 1600) &&
      window.stall_age_ms(1600) == 1600 && window.last_ack_packet_age_ms() == 0,
      "retired ACK renewed the path or pretended to confirm a retained packet");
  expect_throws([&] { window.acknowledge_through(1257, 1600); }, "unsent future ACK was accepted");
  expect(window.acknowledge_through(1256, 1600) && window.pending_count() == 0 &&
      window.oldest_sequence() == 0 && window.retired_through_sequence() == 0 &&
      window.last_ack_packet_age_ms() == 144,
      "exact recent cumulative ACK did not cover retired and retained records");
  expect(!window.acknowledge_through(1256, 2000) && window.last_ack_packet_age_ms() == 144,
      "duplicate ACK changed the age of the last actual progress sample");
  window.reset();
  expect(window.retired_through_sequence() == 0 && window.pending_count() == 0,
      "session reset retained safety overflow state");
}

void test_ack_samples_count_only_new_progress_and_fresh_normal_clears() {
  NativeControlAckRecovery recovery;
  expect(!recovery.latest_sample_healthy(), "unknown ACK sample was marked healthy");
  expect(!recovery.note_ack_progress(500, 1, 201, 5000).entered &&
      recovery.consecutive_anomalies() == 1 && !recovery.latest_sample_healthy(),
      "first late ACK was not one tolerated sample or failed to reset raw sample health");
  expect(!recovery.note_ack_progress(550, 2, 400, 5000).entered &&
      recovery.consecutive_anomalies() == 2, "second late ACK was not a distinct sample");
  for (int index = 0; index < 20; ++index) {
    recovery.note_ack_progress(551 + index, 2, 1, 5000);
    recovery.note_ack_progress(551 + index, 1, 1, 5000);
    recovery.observe(551 + index, 1, 0, 2, 1, 5000);
  }
  expect(recovery.consecutive_anomalies() == 2 && !recovery.waiting(),
      "repeated polls/duplicate/regressing ACK cleared or accumulated a fault");
  expect(recovery.note_ack_progress(600, 3, 202, 5000).entered && recovery.waiting() &&
      recovery.consecutive_anomalies() == 3, "third late new ACK did not withdraw link readiness");
  expect(!recovery.note_ack_progress(601, 3, 10, 5000).recovered && recovery.waiting(),
      "a duplicate apparently fresh ACK restored readiness");
  expect(recovery.note_ack_progress(650, 4, 200, 5000).recovered && !recovery.waiting() &&
      recovery.consecutive_anomalies() == 0 && recovery.sample_sequence() == 4 && recovery.latest_sample_healthy(),
      "fresh new ACK did not clear its own category for the shared vehicle health window");
  recovery.note_ack_progress(700, 5, 201, 5000);
  recovery.note_ack_progress(750, 6, 10, 5000);
  expect(recovery.consecutive_anomalies() == 0,
      "fresh normal sample did not separate two late-sample runs");
}

void test_advancing_late_ack_keeps_session_but_no_progress_times_out() {
  NativeControlAckRecovery recovery;
  for (std::uint64_t sequence = 1; sequence <= 100; ++sequence) {
    const auto now = static_cast<std::int64_t>(sequence * 100);
    recovery.note_ack_progress(now, sequence, 600, 1000);
    const auto transition = recovery.observe(now, 6, 0, sequence, 600, 1000);
    expect(!transition.reconnect_due,
        "continuing abnormal ACK data forced a reconnect solely after the old time budget");
  }
  expect(recovery.waiting() && recovery.consecutive_anomalies() == 3,
      "continuing late ACK data did not keep independent braking readiness false");
  expect(!recovery.observe(10500, 6, 500, 100, 600, 1000).reconnect_due,
      "true stall skipped its bounded transport recovery attempt");
  expect(recovery.observe(11000, 6, 1000, 100, 600, 1000).reconnect_due,
      "no-new-ACK timeout was disabled along with the old shared readiness deadline");
  recovery.reset();
  expect(recovery.observe(500, 2, 500, 0, 0, 5000).entered && recovery.waiting(),
      "absence of ACK did not independently withdraw readiness without three polls");
}

void test_ack_event_revisions_survive_bad_normal_drain_and_socket_reopen() {
  NativeControlAckRecovery recovery;
  expect(recovery.bad_sample_revision() == 0 && recovery.brake_sample_revision() == 0,
      "unknown ACK data counted as an actual fault event");
  recovery.note_ack_progress(1000, 1, 201, 5000);
  recovery.note_ack_progress(1000, 2, 20, 5000);
  expect(recovery.latest_sample_healthy() && recovery.consecutive_anomalies() == 0 &&
      recovery.bad_sample_revision() == 1 && recovery.brake_sample_revision() == 0,
      "bad then normal in the same drain erased the event needed to reset the common window");
  recovery.note_ack_progress(1001, 2, 800, 5000);
  recovery.note_ack_progress(1001, 1, 800, 5000);
  expect(recovery.bad_sample_revision() == 1 && recovery.brake_sample_revision() == 0,
      "duplicate/regressing ACK was counted as a new revision");
  recovery.note_ack_progress(1100, 3, 201, 5000);
  recovery.note_ack_progress(1100, 4, 201, 5000);
  expect(recovery.bad_sample_revision() == 3 && recovery.brake_sample_revision() == 0,
      "one/two anomalies claimed the third-sample brake event");
  expect(recovery.note_ack_progress(1100, 5, 201, 5000).entered &&
      recovery.bad_sample_revision() == 4 && recovery.brake_sample_revision() == 1,
      "third new anomaly did not preserve a distinct brake event");
  recovery.note_ack_progress(1100, 6, 201, 5000);
  recovery.note_ack_progress(1100, 7, 20, 5000);
  expect(recovery.latest_sample_healthy() && !recovery.waiting() &&
      recovery.bad_sample_revision() == 5 && recovery.brake_sample_revision() == 1,
      "three bad then normal in one drain erased braking, or counted another crossing while already waiting");
  expect(recovery.observe(1600, 1, 500, 7, 20, 5000).entered &&
      recovery.brake_sample_revision() == 2 && recovery.bad_sample_revision() == 5,
      "independent no-progress stall failed to preserve its brake event");
  recovery.observe(1601, 1, 501, 7, 20, 5000);
  recovery.connection_reopened(1700);
  expect(recovery.brake_sample_revision() == 2 && recovery.bad_sample_revision() == 5 &&
      !recovery.latest_sample_healthy(), "same-session reopen erased event revisions or forged healthy data");
  expect(recovery.note_ack_progress(1750, 30, 20, 5000).recovered &&
      recovery.latest_sample_healthy() && recovery.brake_sample_revision() == 2 && recovery.bad_sample_revision() == 5,
      "fresh ACK after reopen erased pre-reconnect history");
  recovery.reset();
  expect(recovery.brake_sample_revision() == 0 && recovery.bad_sample_revision() == 0 &&
      !recovery.latest_sample_healthy(), "new-session reset retained the previous generation's history");
}

void test_runtime_full_window_estop_send_succeeds_without_socket_failure() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-console-001", "ack-test-password"}};
  config.device_tokens = {{"vehicle-001", "ack-test-device"}};
  config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(config);
  std::atomic<int> connections{0}, normal_packets{0}, estop_packets{0};
  std::atomic<int> warmup_acknowledgements{0};
  std::atomic<std::uint64_t> warmup_ack_sequence{0};
  std::atomic<bool> stop{false};
  std::atomic<std::uint64_t> sixth_estop_sequence{0};
  std::mutex witness_mutex;
  Json capacity_identity, sixth_estop_identity, sixth_estop_ack_identity;
  std::mutex input_mutex;
  Json emergency_input;
  mine_teleop::DriverConsoleRuntime* runtime = nullptr;
  std::string fixture_error;
  std::mutex error_mutex;
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
      [signaling](const auto& request) { return signaling->handle(request); }, 8 * 1024 * 1024,
      [&](mine_teleop::SocketHandle socket, const auto& request) {
        if (!request.query.contains("send_only")) return signaling->handle_websocket(socket, request);
        const auto key = request.headers.find("sec-websocket-key");
        if (key == request.headers.end()) return false;
        send_handshake(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + mine_teleop::websocket_accept_key(key->second) +
            "\r\nX-Mine-Teleop-Control-Capabilities: " + std::string(mine_teleop::kNativeControlCapabilities) + "\r\n\r\n");
        ++connections;
        mine_teleop::ServerWebSocketConnection connection(socket, 512 * 1024);
        try {
          std::deque<Json> unacknowledged_normal;
          bool emergency_requested = false;
          auto last_warmup_ack = std::chrono::steady_clock::now();
          const auto acknowledge_oldest_during_warmup = [&] {
            const auto now = std::chrono::steady_clock::now();
            if (emergency_requested || unacknowledged_normal.empty() ||
                now - last_warmup_ack < std::chrono::milliseconds(500)) return;
            // Real, sparse progress keeps the production five-second ACK
            // recovery deadline alive while this actual 128-entry window fills.
            const auto ack = unacknowledged_normal.front();
            connection.send_json(ack);
            unacknowledged_normal.pop_front();
            warmup_ack_sequence = ack.at("seq").get<std::uint64_t>();
            ++warmup_acknowledgements;
            last_warmup_ack = now;
          };
          while (!stop.load()) {
            const auto received = connection.receive_json(std::chrono::milliseconds(10));
            if (received.status == mine_teleop::WebSocketReceiveStatus::Closed) break;
            if (received.status != mine_teleop::WebSocketReceiveStatus::Message) {
              acknowledge_oldest_during_warmup();
              continue;
            }
            const bool estop = received.message.at("payload").value("estop", false);
            mine_teleop::HttpRequest post;
            post.method = "POST";
            post.path = request.path.substr(0, request.path.size() - 3) + "/messages";
            post.target = post.path;
            auto authenticated = received.message;
            authenticated["token"] = request.headers.at("x-mine-teleop-driver-token");
            post.body = authenticated.dump();
            const auto response = signaling->handle(post);
            expect(response.status == 200, "loopback cloud rejected native control: " + response.body);
            if (!estop) {
              expect(!emergency_requested, "ordinary packet raced ahead of the full-window emergency");
              ++normal_packets;
              unacknowledged_normal.push_back(Json::parse(response.body));
              if (unacknowledged_normal.size() == 128) {
                // Trigger inside the real receive handler: the next urgent
                // sender pass sees 128 real ordinary unacknowledged identities.
                // Freeze warmup ACKs before latching; do not query status first.
                emergency_requested = true;
                {
                  std::lock_guard lock(input_mutex);
                  expect(runtime && !emergency_input.empty(), "ESTOP fixture input was not ready");
                  expect(runtime->update_control_intent(emergency_input).value("accepted", false),
                      "valid emergency intent did not latch at the capacity boundary");
                }
                const auto status = runtime->status();
                const auto& pending = status.at("native_control");
                expect(pending.at("unacknowledged_count") == 128 &&
                    pending.at("last_ack_seq") == warmup_ack_sequence.load(),
                    "wire capacity boundary did not match real local accounting: " + pending.dump());
                std::lock_guard lock(witness_mutex);
                capacity_identity = {{"session_id", status.at("session_id")},
                    {"session_generation", pending.at("session_generation")},
                    {"pending", pending.at("unacknowledged_count")},
                    {"last_warmup_ack_seq", pending.at("last_ack_seq")}};
              }
              acknowledge_oldest_during_warmup();
            } else {
              const auto count = ++estop_packets;
              expect(received.message.at("payload").at("throttle") == 0.0 &&
                  received.message.at("payload").at("steering") == 0.0,
                  "full-window emergency packet retained motion");
              // Keep three full-window urgent writes unacknowledged, then
              // cumulatively ACK an exact sent emergency identity.
              if (count >= 4) {
                const auto acknowledgement = Json::parse(response.body);
                if (count == 6) {
                  const auto& payload = received.message.at("payload");
                  const auto command_seq = payload.at("seq").get<std::uint64_t>();
                  {
                    std::lock_guard lock(input_mutex);
                    expect(payload.at("session_id") == emergency_input.at("session_id") &&
                        payload.at("intent_seq") == emergency_input.at("intent_seq") &&
                        payload.at("driver_id") == "driver-console-001" &&
                        payload.at("vehicle_id") == "vehicle-001" && command_seq > 128,
                        "sixth emergency packet belongs to a different operation or session");
                  }
                  expect(acknowledgement.value("event", "") == "signaling_ack" &&
                      acknowledgement.value("type", "") == "control_command" &&
                      acknowledgement.at("seq") == command_seq,
                      "cloud ACK did not identify the exact sixth emergency packet");
                  {
                    std::lock_guard lock(witness_mutex);
                    // Whitelist identity and safety values; never retain or
                    // print an authentication token from the wire payload.
                    sixth_estop_identity = {{"seq", command_seq}, {"intent_seq", payload.at("intent_seq")},
                        {"session_id", payload.at("session_id")}, {"driver_id", payload.at("driver_id")},
                        {"vehicle_id", payload.at("vehicle_id")}, {"estop", payload.at("estop")},
                        {"throttle", payload.at("throttle")}, {"steering", payload.at("steering")}};
                    sixth_estop_ack_identity = {{"event", acknowledgement.at("event")},
                        {"type", acknowledgement.at("type")}, {"seq", acknowledgement.at("seq")}};
                  }
                  sixth_estop_sequence.store(command_seq, std::memory_order_release);
                }
                connection.send_json(acknowledgement);
              }
            }
          }
        } catch (const std::exception& error) {
          if (!stop.load()) { std::lock_guard lock(error_mutex); fixture_error = error.what(); }
        }
        return true;
      });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  const auto online = http.post_json_response(base + "/vehicles/online", {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
      {"device_token", "ack-test-device"}, {"connection_id", "full-window-fixture"}});
  std::atomic<bool> heartbeat_failed{false};
  std::jthread heartbeat([&](std::stop_token token) {
    mine_teleop::HttpClient keepalive(std::chrono::seconds(2));
    while (!token.stop_requested()) {
      try {
        static_cast<void>(keepalive.post_json_response(base + "/vehicles/heartbeat",
            {{"vehicle_id", "vehicle-001"}, {"device_token", "ack-test-device"},
             {"connection_generation", online.at("connection_generation")}}));
      } catch (const std::exception&) { heartbeat_failed = true; return; }
      for (int tick = 0; tick < 10 && !token.stop_requested(); ++tick)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  driver_config.rate_hz = 20;
  // All production clock/lease limits remain unchanged in this regression.
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "ack-test-password");
  runtime = &driver;
  const auto connected = driver.connect("vehicle-001");
  {
    std::lock_guard lock(input_mutex);
    emergency_input = {{"session_id", connected.at("session_id")},
        {"session_generation", connected.at("control_session_generation")},
        {"ui_instance_id", "full-window-ack-fixture"}, {"intent_seq", 1},
        {"gear", "D"}, {"steering", 0.0}, {"throttle", 0.0}, {"brake", 1.0}, {"estop", true}};
  }
  Json native;
  std::string observed_session;
  const auto expected_session = connected.at("session_id").get<std::string>();
  const auto expected_generation = connected.at("control_session_generation").get<std::uint64_t>();
  // This is a fixture completion budget, not a production safety timeout.
  // Sparse real ACK progress permits the window to fill at the default rate.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (std::chrono::steady_clock::now() < deadline) {
    // Read a durable peer witness first, then wait for local send accounting
    // and cumulative ACK progress to cover that exact emergency identity.
    // A server count observed after an older status snapshot is insufficient.
    const auto target = sixth_estop_sequence.load(std::memory_order_acquire);
    const auto status = driver.status();
    native = status.at("native_control");
    observed_session = status.value("session_id", "");
    if (target > 128 && observed_session == expected_session &&
        native.value("session_generation", 0U) == expected_generation &&
        native.value("last_seq", 0U) >= target && native.value("last_ack_seq", 0U) >= target &&
        native.value("commands_sent_total", 0U) >= static_cast<unsigned>(normal_packets.load()) + 6U) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  stop.store(true);
  static_cast<void>(driver.disconnect("full_window_ack_fixture_complete"));
  heartbeat.request_stop();
  heartbeat.join();
  server.stop();
  {
    std::lock_guard lock(error_mutex);
    expect(fixture_error.empty(), "full send-path mock failed: " + fixture_error);
  }
  Json capacity_witness, emergency_identity, acknowledgement_identity;
  {
    std::lock_guard lock(witness_mutex);
    capacity_witness = capacity_identity;
    emergency_identity = sixth_estop_identity;
    acknowledgement_identity = sixth_estop_ack_identity;
  }
  const auto witnessed_seq = sixth_estop_sequence.load(std::memory_order_acquire);
  expect(!heartbeat_failed && warmup_acknowledgements.load() > 0 &&
      normal_packets.load() - warmup_acknowledgements.load() == 128 &&
      capacity_witness.value("pending", 0) == 128 &&
      capacity_witness.value("session_id", "") == expected_session &&
      capacity_witness.value("session_generation", 0U) == expected_generation &&
      estop_packets.load() >= 6 && witnessed_seq > 128 &&
      !emergency_identity.empty() && acknowledgement_identity.value("seq", 0U) == witnessed_seq,
      "fixture did not execute repeated urgent writes at the full capacity boundary: " + native.dump());
  expect(observed_session == expected_session && native.value("session_generation", 0U) == expected_generation &&
      native.value("last_seq", 0U) >= witnessed_seq && native.value("last_ack_seq", 0U) >= witnessed_seq &&
      emergency_identity.at("session_id") == expected_session && emergency_identity.at("intent_seq") == 1 &&
      emergency_identity.at("estop") == true && emergency_identity.at("throttle") == 0.0 &&
      emergency_identity.at("steering") == 0.0 && native.value("effective_estop", false),
      "local accounting/ACK snapshot did not cover the witnessed emergency identity: " + native.dump());
  expect(connections.load() == 1 && native.value("websocket_connected", false) &&
      native.value("send_failures_total", 1U) == 0 &&
      native.value("commands_sent_total", 0U) >= static_cast<unsigned>(normal_packets.load()) + 6U &&
      native.value("last_ack_seq", 0U) > 128U && native.value("unacknowledged_count", 129U) <= 128U &&
      native.value("last_error", "nonempty").empty(),
      "successful ESTOP send/accounting was treated as socket failure or lost ACK identity: " + native.dump());
  std::cout << "[ACK_CAPACITY] " << Json{{"normal_packets", normal_packets.load()},
      {"warmup_acknowledgements", warmup_acknowledgements.load()}, {"capacity", capacity_witness},
      {"estop_packets", estop_packets.load()}, {"sixth_estop", emergency_identity},
      {"sixth_ack", acknowledgement_identity}}.dump() << '\n';
  // Sending/receiving ESTOP is not physical vehicle feedback. No fake vehicle
  // status is supplied and this test makes no claim of mechanical braking.
}

std::string coalesced_ack_frame(const Json& value);
void test_runtime_counts_each_drained_ack_and_duplicate_does_not_recover() {
  mine_teleop::SignalingServerConfig config;
  config.driver_passwords = {{"driver-console-001", "ack-samples-password"}};
  config.device_tokens = {{"vehicle-001", "ack-samples-device"}};
  config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
  auto signaling = std::make_shared<mine_teleop::SignalingService>(config);
  std::atomic<int> connections{0};
  std::atomic<bool> release_fresh{false}, late_batch_sent{false}, pending_prepared{false}, stop{false};
  std::atomic<std::uint64_t> third_sequence{0};
  std::mutex error_mutex;
  std::string fixture_error;
  mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
      [signaling](const auto& request) { return signaling->handle(request); }, 8 * 1024 * 1024,
      [&](mine_teleop::SocketHandle socket, const auto& request) {
        if (!request.query.contains("send_only")) return signaling->handle_websocket(socket, request);
        const auto key = request.headers.find("sec-websocket-key");
        if (key == request.headers.end()) return false;
        send_handshake(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + mine_teleop::websocket_accept_key(key->second) +
            "\r\nX-Mine-Teleop-Control-Capabilities: " + std::string(mine_teleop::kNativeControlCapabilities) + "\r\n\r\n");
        ++connections;
        mine_teleop::ServerWebSocketConnection connection(socket, 512 * 1024);
        std::vector<Json> first_three;
        auto release_at = std::chrono::steady_clock::time_point::max();
        try {
          while (!stop.load()) {
            const auto received = connection.receive_json(std::chrono::milliseconds(2));
            if (received.status == mine_teleop::WebSocketReceiveStatus::Closed) break;
            if (received.status == mine_teleop::WebSocketReceiveStatus::Message) {
              mine_teleop::HttpRequest post;
              post.method = "POST";
              post.path = request.path.substr(0, request.path.size() - 3) + "/messages";
              post.target = post.path;
              auto authenticated = received.message;
              authenticated["token"] = request.headers.at("x-mine-teleop-driver-token");
              post.body = authenticated.dump();
              const auto response = signaling->handle(post);
              expect(response.status == 200, "ACK sample fixture rejected a real command: " + response.body);
              const auto ack = Json::parse(response.body);
              if (first_three.size() < 3) {
                first_three.push_back(ack);
                if (first_three.size() == 3) {
                  third_sequence.store(ack.at("seq").get<std::uint64_t>());
                  release_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
                }
              } else if (release_fresh.load()) {
                connection.send_json(ack);
              }
            }
            if (pending_prepared.load() && !late_batch_sent.load() && std::chrono::steady_clock::now() >= release_at) {
              std::string batch;
              for (const auto& ack : first_three) batch += coalesced_ack_frame(ack);
              // These are real duplicate return messages, not new samples.
              for (int index = 0; index < 4; ++index) batch += coalesced_ack_frame(first_three.back());
              send_handshake(socket, batch); // One write establishes the same-drain injection premise.
              late_batch_sent.store(true);
            }
          }
        } catch (const std::exception& error) {
          if (!stop.load()) { std::lock_guard lock(error_mutex); fixture_error = error.what(); }
        }
        return true;
      });
  server.start();
  const auto base = "http://127.0.0.1:" + std::to_string(server.port());
  mine_teleop::HttpClient http;
  static_cast<void>(http.post_json_response(base + "/vehicles/online", {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities}, {"vehicle_id", "vehicle-001"},
      {"device_token", "ack-samples-device"}, {"connection_id", "ack-samples-fixture"}}));
  mine_teleop::DriverConfig driver_config;
  driver_config.driver_id = "driver-console-001";
  driver_config.signaling_url = base;
  driver_config.rate_hz = 100;
  mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "ack-samples-password");
  static_cast<void>(driver.connect("vehicle-001"));
  Json late_native;
  const auto preparation_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  auto late_deadline = std::chrono::steady_clock::time_point::min();
  std::int64_t pending_age_at_anchor = -1;
  // The independent500ms absence clock starts at the first pending send, not
  // connect(). Infer a conservative lower bound from a snapshot taken between
  // sample_started and return. Keep the SAME450ms observation window.
  while (std::chrono::steady_clock::now() < preparation_deadline && !pending_prepared.load()) {
    const auto sample_started = std::chrono::steady_clock::now();
    late_native = driver.status().at("native_control");
    if (late_native.value("unacknowledged_count", 0U) > 0 && late_native.value("last_ack_seq", 0U) == 0 &&
        !late_native.value("ack_braking_wait", true)) {
      pending_age_at_anchor = late_native.value("unacknowledged_age_ms", std::int64_t{-1});
      if (pending_age_at_anchor >= 0 && pending_age_at_anchor < 450) {
        late_deadline = sample_started + std::chrono::milliseconds(450 - pending_age_at_anchor);
        pending_prepared.store(true);
      } else break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  auto late_observed_at = std::chrono::steady_clock::now();
  while (pending_prepared.load() && std::chrono::steady_clock::now() < late_deadline) {
    late_native = driver.status().at("native_control");
    late_observed_at = std::chrono::steady_clock::now();
    if (late_batch_sent.load() && late_native.value("last_ack_seq", 0U) == third_sequence.load() &&
        late_native.value("ack_braking_wait", false)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  // Assert before the independent 500ms absence timeout could mask bad sample
  // counting. Duplicate ACKs were already present in the same receive drain.
  const bool three_sample_wait = pending_prepared.load() && late_observed_at < late_deadline && late_batch_sent.load() &&
      late_native.value("last_ack_seq", 0U) == third_sequence.load() &&
      late_native.value("ack_braking_wait", false) &&
      late_native.value("last_ack_packet_age_ms", std::int64_t{0}) > 200 &&
      late_native.value("ack_stall_age_ms", std::int64_t{500}) < 500;
  release_fresh.store(true);
  Json fresh_native;
  const auto fresh_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
  while (std::chrono::steady_clock::now() < fresh_deadline) {
    fresh_native = driver.status().at("native_control");
    if (!fresh_native.value("ack_braking_wait", true) &&
        fresh_native.value("last_ack_seq", 0U) > third_sequence.load()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  stop.store(true);
  static_cast<void>(driver.disconnect("ack_samples_fixture_complete"));
  server.stop();
  { std::lock_guard lock(error_mutex); expect(fixture_error.empty(), "ACK samples mock failed: " + fixture_error); }
  std::cout << "[ACK_BATCH] " << Json{{"pending_prepared", pending_prepared.load()}, {"pending_age_at_anchor_ms", pending_age_at_anchor},
      {"window_from_pending_ms", 450}, {"late_batch_sent", late_batch_sent.load()}, {"third_sequence", third_sequence.load()},
      {"observed_before_deadline", late_observed_at < late_deadline}, {"native", late_native}}.dump() << '\n';
  expect(three_sample_wait, "three late ACKs in one drain did not enter braking (or duplicates cleared it): " + late_native.dump());
  expect(connections.load() == 1 && fresh_native.value("send_failures_total", 1U) == 0 &&
      !fresh_native.value("ack_braking_wait", true) && fresh_native.value("requires_fresh_input", false) &&
      fresh_native.value("last_ack_packet_age_ms", std::int64_t{201}) <= 200,
      "fresh new ACK did not restore only its readiness while preserving rearm and the original socket: " + fresh_native.dump());
}

std::int64_t ack_probe_monotonic_ms(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}
std::string coalesced_ack_frame(const Json& value) {
  const auto payload = value.dump();
  if (payload.size() > 65535) throw std::runtime_error("coalesced ACK fixture unexpectedly large");
  std::string frame(1, static_cast<char>(0x81));
  if (payload.size() < 126) frame.push_back(static_cast<char>(payload.size()));
  else {
    frame.push_back(126);
    frame.push_back(static_cast<char>((payload.size() >> 8) & 255));
    frame.push_back(static_cast<char>(payload.size() & 255));
  }
  return frame + payload;
}

void test_runtime_coalesced_ack_history_restarts_one_common_healthy_window() {
    using Steady = std::chrono::steady_clock;
    mine_teleop::SignalingServerConfig config;
    config.driver_passwords = {{"driver-console-001", "coalescing-password"}};
    config.device_tokens = {{"vehicle-001", "coalescing-device"}};
    config.driver_vehicle_permissions = {{"driver-console-001", {"vehicle-001"}}};
    auto signaling = std::make_shared<mine_teleop::SignalingService>(config);
    std::atomic<bool> stop{false}, complete{false};
    std::atomic<int> connections{0};
    std::mutex evidence_mutex;
    Json packets = Json::array(), injection = Json::object();
    std::string fixture_error;
    mine_teleop::SimpleHttpServer server("127.0.0.1", 0,
        [signaling](const auto& request) { return signaling->handle(request); }, 8 * 1024 * 1024,
        [&](mine_teleop::SocketHandle socket, const auto& request) {
          if (!request.query.contains("send_only")) return signaling->handle_websocket(socket, request);
          const auto key = request.headers.find("sec-websocket-key");
          if (key == request.headers.end()) return false;
          send_handshake(socket, "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Accept: " + mine_teleop::websocket_accept_key(key->second) +
              "\r\nX-Mine-Teleop-Control-Capabilities: " + std::string(mine_teleop::kNativeControlCapabilities) + "\r\n\r\n");
          ++connections;
          mine_teleop::ServerWebSocketConnection connection(socket, 512 * 1024);
          mine_teleop::SessionControlRecoveryLatch latch;
          std::string session;
          bool real_link_wait_seen = false;
          int stage = 0; // actual no-ACK stall, recovery start, paired injection, normal return traffic
          std::int64_t initial_send_ms = 0, healthy_start_ms = 0, old_ack_received_ms = 0;
          Json old_ack, newest_ack;
          std::uint64_t initial_bad_revision = 0;
          try {
            while (!stop.load()) {
              const auto incoming = connection.receive_json(std::chrono::milliseconds(2));
              if (incoming.status == mine_teleop::WebSocketReceiveStatus::Closed) break;
              if (incoming.status == mine_teleop::WebSocketReceiveStatus::Message) {
                const auto received_ms = ack_probe_monotonic_ms(Steady::now());
                const auto command = mine_teleop::ControlCommand::from_json(incoming.message.at("payload"));
                if (session.empty()) {
                  session = command.session_id;
                  static_cast<void>(latch.enter_session(session));
                  static_cast<void>(latch.arm_for_control(session));
                  initial_send_ms = received_ms;
                }
                // All ACK JSON comes from the real authenticated cloud service;
                // only return delivery timing is controlled by this probe.
                mine_teleop::HttpRequest post;
                post.method = "POST";
                post.path = request.path.substr(0, request.path.size() - 3) + "/messages";
                post.target = post.path;
                auto authenticated = incoming.message;
                authenticated["token"] = request.headers.at("x-mine-teleop-driver-token");
                post.body = authenticated.dump();
                const auto response = signaling->handle(post);
                if (response.status != 200) throw std::runtime_error("cloud rejected probe traffic: " + response.body);
                newest_ack = Json::parse(response.body);
                if (!command.control_link_ready) real_link_wait_seen = true;
                const bool qualified_ready = command.control_link_ready && command.control_time_sync_ready &&
                    command.operator_input_ready && command.critical_video_ready;
                const bool ready = qualified_ready &&
                    (!latch.braking_wait_for(session) || command.recovery_sample_healthy);
                const auto phase = latch.observe_readiness(session, ready, received_ms, 5000, mine_teleop::ControlRecoveryCause::ControlLink,
                    {{"driver", command.recovery_bad_sample_revision, command.recovery_brake_revision}});
                if (stage == 0 && real_link_wait_seen && received_ms - initial_send_ms >= 650) {
                  connection.send_json(newest_ack);
                  stage = 1;
                } else if (stage == 1) {
                  if (qualified_ready && command.recovery_sample_healthy && latch.braking_wait_for(session)) {
                    healthy_start_ms = received_ms;
                    initial_bad_revision = command.recovery_bad_sample_revision;
                    old_ack_received_ms = received_ms;
                    old_ack = newest_ack;
                    stage = 2;
                  } else connection.send_json(newest_ack);
                } else if (stage == 3) connection.send_json(newest_ack);
                {
                  std::lock_guard lock(evidence_mutex);
                  packets.push_back({{"seq", command.seq}, {"received_monotonic_ms", received_ms},
                      {"healthy_window_elapsed_ms", healthy_start_ms > 0 ? Json(received_ms - healthy_start_ms) : Json(nullptr)},
                      {"control_link_ready", command.control_link_ready}, {"operator_input_ready", command.operator_input_ready},
                      {"control_time_sync_ready", command.control_time_sync_ready}, {"critical_video_ready", command.critical_video_ready},
                      {"recovery_sample_healthy", command.recovery_sample_healthy},
                      {"bad_revision", command.recovery_bad_sample_revision}, {"brake_revision", command.recovery_brake_revision},
                      {"latch_phase", phase == mine_teleop::ControlRecoveryPhase::Recovered ? "recovered" :
                          phase == mine_teleop::ControlRecoveryPhase::BrakingWait ? "braking_wait" : "ready"}});
                }
                if (stage == 3 && phase == mine_teleop::ControlRecoveryPhase::Recovered) complete.store(true);
              }
              const auto now = ack_probe_monotonic_ms(Steady::now());
              if (stage == 2 && now - healthy_start_ms >= 280 && !newest_ack.empty()) {
                // One write delivers two complete WS frames. The old exact
                // identity is >200ms old; the latest identity is still fresh.
                send_handshake(socket, coalesced_ack_frame(old_ack) + coalesced_ack_frame(newest_ack));
                std::lock_guard lock(evidence_mutex);
                injection = {{"old_seq", old_ack.at("seq")}, {"normal_seq", newest_ack.at("seq")},
                    {"injected_monotonic_ms", now}, {"old_ack_held_ms", now - old_ack_received_ms},
                    {"healthy_start_monotonic_ms", healthy_start_ms}, {"initial_bad_revision", initial_bad_revision}};
                stage = 3;
              }
            }
          } catch (const std::exception& error) {
            if (!stop.load()) { std::lock_guard lock(evidence_mutex); fixture_error = error.what(); }
          }
          return true;
        });
    server.start();
    const auto base = "http://127.0.0.1:" + std::to_string(server.port());
    mine_teleop::HttpClient http;
    static_cast<void>(http.post_json_response(base + "/vehicles/online", {{"native_control_capabilities", mine_teleop::kNativeControlCapabilities},
        {"vehicle_id", "vehicle-001"}, {"device_token", "coalescing-device"}, {"connection_id", "coalescing-probe"}}));
    Json final_status;
    {
      mine_teleop::DriverConfig driver_config;
      driver_config.driver_id = "driver-console-001";
      driver_config.signaling_url = base;
      mine_teleop::DriverConsoleRuntime driver(driver_config, "vehicle-001", "coalescing-password");
      const auto connected = driver.connect("vehicle-001");
      Json input = {{"session_id", connected.at("session_id")},
          {"session_generation", connected.at("control_session_generation")},
          {"ui_instance_id", "coalescing-probe-ui"}, {"intent_seq", 1}, {"gear", "N"},
          {"steering", 0.0}, {"throttle", 0.0}, {"brake", 0.0},
          {"critical_video_ready", true}, {"critical_video_sample_healthy", true},
          {"critical_video_bad_sample_revision", std::uint64_t{0}}, {"critical_video_brake_revision", std::uint64_t{0}},
          {"takeover", true}, {"ui_control_epoch", 0}};
      const auto taken = driver.update_control_intent(input);
      if (!taken.value("accepted", false)) throw std::runtime_error("probe initial neutral rejected: " + taken.dump());
      input["takeover"] = false;
      input["ui_control_epoch"] = taken.at("ui_control_epoch");
      const auto until = Steady::now() + std::chrono::seconds(4);
      while (!complete.load() && Steady::now() < until) {
        static_cast<void>(driver.update_control_intent(input));
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      final_status = driver.status();
      stop.store(true);
      static_cast<void>(driver.disconnect("coalescing_readonly_probe_complete"));
    }
    server.stop();
    expect(complete.load() && fixture_error.empty() && connections.load() == 1 &&
        final_status.at("native_control").value("send_failures_total", 1U) == 0,
        "coalescing mock did not retain the real socket: " + fixture_error + final_status.dump());
    expect(!injection.empty() && injection.at("old_ack_held_ms").get<std::int64_t>() >= 280,
        "coalescing mock never supplied a genuinely old exact ACK");
    const Json* changed = nullptr;
    const Json* recovered = nullptr;
    for (const auto& packet : packets) {
      if (!changed && packet.at("received_monotonic_ms") >= injection.at("injected_monotonic_ms") &&
          packet.at("bad_revision") != injection.at("initial_bad_revision")) changed = &packet;
      if (packet.at("latch_phase") == "recovered" && changed) recovered = &packet;
    }
    expect(changed && recovered && changed->at("recovery_sample_healthy") == true &&
        changed->at("control_link_ready") == true && changed->at("operator_input_ready") == true &&
        changed->at("control_time_sync_ready") == true && changed->at("critical_video_ready") == true &&
        changed->at("latch_phase") == "braking_wait",
        "late then normal ACK vanished from the next actually sent carrier: " + packets.dump());
    const auto stable_elapsed = recovered->at("received_monotonic_ms").get<std::int64_t>() -
        changed->at("received_monotonic_ms").get<std::int64_t>();
    expect(stable_elapsed >= mine_teleop::kControlRecoveryStableMs && stable_elapsed < 600,
        "bad revision did not restart exactly one shared 300ms window: " + packets.dump());
}
}  // namespace

int main(int argc, char** argv) {
  const std::vector<std::pair<std::string, std::function<void()>>> tests{
      {"full_window_safety_records_remain_bounded_and_conservative", test_full_window_safety_records_remain_bounded_and_conservative},
      {"ack_samples_count_only_new_progress_and_fresh_normal_clears", test_ack_samples_count_only_new_progress_and_fresh_normal_clears},
      {"advancing_late_ack_keeps_session_but_no_progress_times_out", test_advancing_late_ack_keeps_session_but_no_progress_times_out},
      {"ack_event_revisions_survive_bad_normal_drain_and_socket_reopen", test_ack_event_revisions_survive_bad_normal_drain_and_socket_reopen},
      {"runtime_full_window_estop_send_succeeds_without_socket_failure", test_runtime_full_window_estop_send_succeeds_without_socket_failure},
      {"runtime_counts_each_drained_ack_and_duplicate_does_not_recover", test_runtime_counts_each_drained_ack_and_duplicate_does_not_recover},
      {"runtime_coalesced_ack_history_restarts_one_common_healthy_window", test_runtime_coalesced_ack_history_restarts_one_common_healthy_window}};
  int failed = 0;
  int executed = 0;
  for (const auto& [name, test] : tests) {
    if (argc > 1 && name != argv[1]) continue;
    ++executed;
    try { test(); std::cout << "[PASS] " << name << '\n'; }
    catch (const std::exception& error) { ++failed; std::cerr << "[FAIL] " << name << ": " << error.what() << '\n'; }
  }
  if (executed == 0) { std::cerr << "unknown ACK test filter\n"; return 2; }
  return failed == 0 ? 0 : 1;
}
