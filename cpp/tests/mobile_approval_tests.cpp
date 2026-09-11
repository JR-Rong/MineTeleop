#include "mine_teleop/server.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace mine_teleop;
namespace {
void check(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
Json body(const ServerResponse& response) { return Json::parse(response.body); }
SignalingServerConfig configuration() {
  SignalingServerConfig c;
  c.driver_passwords = {{"d1", "driver-secret"}, {"d2", "driver-secret"}};
  c.device_tokens = {{"v1", "device-secret"}, {"v2", "device-secret"}};
  c.driver_vehicle_permissions = {{"d1", {"v1", "v2"}}, {"d2", {"v1", "v2"}}};
  c.mobile_app_password = "approval-secret";
  c.mobile_approval_vehicles = {"v1"};
  c.admin_token = "admin-secret";
  return c;
}
struct Fixture {
  SignalingService service;
  std::string driver, approver;
  explicit Fixture(SignalingServerConfig config = configuration(), std::function<std::int64_t()> audit_clock = {})
      : service(std::move(config), std::move(audit_clock)) {
    driver = login_driver();
    approver = login_approver();
    online("v1", "connection-1"); online("v2", "connection-2");
  }
  ServerResponse call(std::string method, std::string path, Json data = Json::object(), std::string auth = "") {
    HttpRequest r;
    r.method = method; r.path = path; r.target = path; r.peer_address = "127.0.0.1";
    r.body = data.dump();
    if (!auth.empty()) r.headers["x-mine-teleop-approver-token"] = auth;
    return service.handle(r);
  }
  std::string login_driver(std::string id = "d1") {
    return body(call("POST", "/auth/driver_login", {{"driver_id", id}, {"password", "driver-secret"}})).at("token");
  }
  std::string login_approver() {
    return body(call("POST", "/mobile/api/login", {{"password", "approval-secret"}})).at("token");
  }
  Json online(std::string vehicle, std::string connection) {
    return body(call("POST", "/vehicles/online", {{"vehicle_id", vehicle}, {"connection_id", connection}, {"device_token", "device-secret"}}));
  }
  ServerResponse connect(std::string vehicle = "v1", std::string id = "d1", std::string credential = "") {
    return call("POST", "/sessions", {{"driver_id", id}, {"vehicle_id", vehicle}, {"token", credential.empty() ? driver : credential}});
  }
  std::string pending() {
    const auto response = connect();
    check(response.status == 409 && body(response).at("issue_code") == "mobile_approval_pending", "request was not gated");
    check(!body(response).contains("control_token") && !body(response).contains("session_id"), "pending request leaked authority");
    return body(response).at("request_id");
  }
  ServerResponse decide(std::string id, std::string decision = "approve", std::string credential = "") {
    return call("POST", "/mobile/api/requests/" + id + "/decision", {{"decision", decision}}, credential.empty() ? approver : credential);
  }
  Json inbox(std::string credential = "") {
    return body(call("GET", "/mobile/api/requests", Json::object(), credential.empty() ? approver : credential)).at("requests");
  }
};
ServerResponse app_login(Fixture& f, std::string peer, std::string password = "approval-secret", std::string forwarded = "") {
  HttpRequest request;
  request.method = "POST"; request.path = "/mobile/api/login"; request.peer_address = peer;
  request.body = Json{{"password", password}}.dump();
  if (!forwarded.empty()) request.headers["x-forwarded-for"] = forwarded;
  return f.service.handle(request);
}
void login_source_isolation() {
  Fixture f;
  for (int i = 0; i < 5; ++i) app_login(f, "127.0.0.1", "bad", "198.51.100.1");
  check(app_login(f, "127.0.0.1", "approval-secret", "198.51.100.1").status == 429, "blocked source bypassed lockout");
  check(app_login(f, "127.0.0.1", "approval-secret", "198.51.100.2").status == 200, "one source locked every phone");
  check(f.call("GET", "/mobile/api/requests", Json::object(), f.approver).status == 200, "lockout invalidated existing phone");
  for (int i = 0; i < 5; ++i) app_login(f, "198.51.100.3", "bad", "203.0.113." + std::to_string(i + 1));
  check(app_login(f, "198.51.100.3", "approval-secret", "203.0.113.99").status == 429,
        "untrusted forwarded header bypassed source lockout");
  check(f.connect("v2").status == 200, "App lockout affected driver");
}
void login_source_capacity_and_expiry() {
  auto c = configuration(); c.api_rate_limit_max_sources = 2;
  c.login_failure_window_ms = 150; c.login_lockout_ms = 250;
  Fixture f(c);
  app_login(f, "198.51.100.1", "bad"); app_login(f, "198.51.100.2", "bad");
  for (int i = 0; i < 5; ++i) app_login(f, "198.51.100.3", "bad");
  check(app_login(f, "198.51.100.4").status == 429, "overflow sources were not bounded by shared lockout");
  check(app_login(f, "198.51.100.1").status == 200, "overflow lockout affected tracked source");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  check(app_login(f, "198.51.100.4").status == 200, "expired source lockout did not recover");
}
void independent_app_token_expiry() {
  for (bool short_app : {true, false}) {
    auto c = configuration(); c.approver_token_ttl_ms = short_app ? 150 : 5000;
    c.token_ttl_ms = short_app ? 5000 : 150;
    Fixture f(c);
    const auto login = body(app_login(f, "198.51.100.1"));
    check(login.at("remaining_ms").get<std::int64_t>() > 0 &&
          login.at("remaining_ms").get<std::int64_t>() <= c.approver_token_ttl_ms, "invalid App remaining TTL");
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    check(f.call("GET", "/mobile/api/requests", Json::object(), f.approver).status == (short_app ? 401 : 200),
          "App token expiry is not independent");
    check(f.connect("v2").status == (short_app ? 200 : 401), "App TTL changed driver expiry");
  }
}
void gate_and_single_use() {
  Fixture f;
  check(f.connect("v2").status == 200, "default-off vehicle regressed");
  const auto id = f.pending();
  check(f.pending() == id && f.inbox().size() == 1, "retry duplicated request");
  check(f.service.health().at("active_sessions") == 1, "pending created session");
  check(f.decide(id).status == 200 && f.decide(id).status == 200, "approval retry is not idempotent");
  const auto session = f.connect(); check(session.status == 200, "approved session was not granted");
  check(body(session).contains("control_token"), "approved session omitted control token");
  check(f.decide(id).status == 409, "consumed approval was replayed");
  check(f.call("POST", "/sessions/" + body(session).at("session_id").get<std::string>() + "/end",
      {{"actor", "d1"}, {"token", f.driver}}).status == 200, "session did not end");
  check(f.pending() != id, "new session reused old approval");
}
void permissions_and_race() {
  Fixture f;
  const auto id = f.pending(), peer = f.login_approver();
  check(f.inbox(peer).size() == 1, "shared App login cannot see approval-required vehicle");
  check(f.decide(id, "approve", "invalid-app-token").status == 401, "invalid App token approved request");
  check(f.decide("unknown-request").status == 404, "unknown request accepted");
  check(f.decide(id, "approve", f.driver).status == 401, "driver token approved request");
  check(f.connect("v1", "d1", f.approver).status == 401, "approver token obtained control");
  const auto d2 = f.login_driver("d2");
  check(body(f.connect("v1", "d2", d2)).at("issue_code") == "mobile_approval_busy", "competing driver replaced pending request");
  ServerResponse approve, reject;
  std::thread one([&] { approve = f.decide(id); });
  std::thread two([&] { reject = f.decide(id, "reject", peer); });
  one.join(); two.join();
  check((approve.status == 200 && reject.status == 409) || (approve.status == 409 && reject.status == 200), "racing decisions both succeeded");
  if (approve.status == 200) {
    check(f.decide(id, "approve", peer).status == 409, "second actor overwrote decision");
    check(f.connect().status == 200, "winning approval not honored");
  } else {
    check(body(f.connect()).at("issue_code") == "mobile_approval_rejected", "winning rejection bypassed");
  }
}
void rejection_and_expiry() {
  auto c = configuration(); c.mobile_approval_timeout_ms = 150;
  Fixture f(c); const auto id = f.pending();
  check(f.decide(id, "reject").status == 200, "reject failed");
  check(body(f.connect()).at("issue_code") == "mobile_approval_rejected", "retry bypassed rejection");
  check(f.decide(id).status == 409, "rejection changed to approval");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  check(f.decide(id).status == 409 && f.inbox().at(0).at("state") == "expired", "expired request accepted");
  const auto next = f.pending(); check(next != id, "expired ID reused");
  check(f.decide(next).status == 200, "new approval failed");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  check(f.pending() != next, "expired approval granted control");
}
void connection_generation_and_revocation() {
  Fixture f;
  auto id = f.pending(); f.decide(id);
  f.online("v1", "replacement");
  check(f.decide(id).status == 409 && f.inbox().at(0).at("state") == "cancelled", "vehicle replacement retained approval");
  id = f.pending(); f.decide(id);
  f.call("POST", "/auth/driver_logout", {{"driver_id", "d1"}, {"token", f.driver}});
  f.driver = f.login_driver();
  check(f.pending() != id, "new login reused approval");
  id = f.inbox().at(0).at("request_id"); f.decide(id);
  f.call("POST", "/admin/revoke/vehicle", {{"id", "v1"}, {"admin_token", "admin-secret"}});
  f.call("POST", "/admin/restore/vehicle", {{"id", "v1"}, {"admin_token", "admin-secret"}});
  f.online("v1", "after-revoke");
  check(f.pending() != id && f.decide(id).status == 404, "revocation retained old approval");
}
void authentication_and_api_routes() {
  Fixture f;
  check(f.call("GET", "/mobile/api/requests").status == 401, "anonymous inbox accepted");
  HttpRequest query; query.method = "GET"; query.path = "/mobile/api/requests";
  query.query["token"] = f.approver;
  check(f.service.handle(query).status == 401, "mobile credentials accepted in URL");
  const auto old = f.approver; f.approver = f.login_approver();
  check(f.call("GET", "/mobile/api/requests", Json::object(), old).status == 200, "new phone unexpectedly logged out another phone");
  const auto response = f.call("GET", "/mobile/api/requests", Json::object(), f.approver);
  check(std::find(response.headers.begin(), response.headers.end(), std::pair<std::string,std::string>{"Cache-Control","no-store"}) != response.headers.end(), "private responses cacheable");
  check(f.call("POST", "/mobile/api/logout", Json::object(), f.approver).status == 200, "logout failed");
  check(f.call("GET", "/mobile/api/requests", Json::object(), f.approver).status == 401, "logout token still works");
  for (const auto* path : {"/mobile", "/mobile/", "/mobile/index.html", "/mobile/icon.svg", "/mobile/app.js", "/mobile/app.css", "/mobile/sw.js", "/mobile/manifest.webmanifest", "/mobile/icon-192.png", "/mobile/icon-512.png"}) {
    check(f.call("GET", path).status == 404, std::string("removed approval page still served: ") + path);
  }
  for (int n = 0; n < 5; ++n) f.call("POST", "/mobile/api/login", {{"password", "bad"}});
  check(f.call("POST", "/mobile/api/login", {{"password", "approval-secret"}}).status == 429, "mobile login is not throttled");
  check(f.connect("v2").status == 200, "mobile login lockout affected driver");
}
void audit_fail_closed() {
  const auto root = std::filesystem::temp_directory_path() / ("mobile-audit-" + random_token(6));
  std::filesystem::create_directories(root);
  auto c = configuration(); c.audit_log_path = (root / "audit.jsonl").string();
  {
    Fixture f(c);
    const auto id = f.pending();
    std::filesystem::remove(c.audit_log_path); std::filesystem::create_directory(c.audit_log_path);
    check(f.decide(id).status == 503 && f.inbox().at(0).at("state") == "pending", "unaudited approval accepted");
    std::filesystem::remove(c.audit_log_path);
    check(f.decide(id).status == 200, "audit recovery failed");
    std::filesystem::remove(c.audit_log_path); std::filesystem::create_directory(c.audit_log_path);
    check(f.connect().status == 503 && f.service.health().at("active_sessions") == 0, "audit failure issued authority");
  }
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(root);
  std::atomic<int> writes_before_failure{0};
  {
    Fixture f(c, [&] {
      if (writes_before_failure > 0 && --writes_before_failure == 0) {
        std::filesystem::remove(c.audit_log_path);
        std::filesystem::create_directory(c.audit_log_path);
      }
      return now_ms();
    });
    const auto id = f.pending(); f.decide(id);
    writes_before_failure = 3; // Both preflights succeed, then session-created audit fails.
    check(f.connect().status == 503 && f.service.health().at("active_sessions") == 0,
          "audit failure after grant preflight left active control authority");
  }
  std::filesystem::remove_all(root);
}
void invalid_config_and_restart() {
  for (int scenario = 0; scenario < 5; ++scenario) {
    auto c = configuration();
    if (scenario == 0) c.mobile_app_password.clear();
    if (scenario == 1) c.mobile_approval_timeout_ms = 0;
    if (scenario == 2) c.mobile_approval_vehicles.insert("unknown");
    if (scenario == 3) c.approver_token_ttl_ms = 0;
    if (scenario == 4) c.approver_token_ttl_ms = 7LL * 24 * 60 * 60 * 1000 + 1;
    bool threw = false; try { SignalingService service(c); } catch (const std::invalid_argument&) { threw = true; }
    check(threw, "invalid approval configuration accepted");
  }
  Fixture before; const auto id = before.pending(); before.decide(id);
  Fixture after;
  check(after.call("GET", "/mobile/api/requests", Json::object(), before.approver).status == 401, "restart retained login");
  check(after.decide(id).status == 404 && after.pending() != id, "restart reused authority");
}
void heartbeat_and_login_expiry() {
  for (bool vehicle_timeout : {true, false}) {
    auto c = configuration();
    if (vehicle_timeout) c.vehicle_heartbeat_timeout_ms = 150;
    else c.driver_heartbeat_timeout_ms = 150;
    Fixture f(c); const auto id = f.pending(); f.decide(id);
    std::this_thread::sleep_for(std::chrono::milliseconds(180));
    check(f.decide(id).status == 409, "heartbeat expiry retained approval");
    check(f.connect().status != 200 && f.service.health().at("active_sessions") == 0, "offline participant granted authority");
  }
  auto c = configuration(); c.approver_token_ttl_ms = 150;
  Fixture f(c); const auto id = f.pending();
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  check(f.decide(id).status == 401, "expired phone login can approve");
}
void expiry_during_audit() {
  const auto root = std::filesystem::temp_directory_path() / ("mobile-slow-audit-" + random_token(6));
  std::filesystem::create_directories(root);
  std::atomic<bool> stall{false};
  auto c = configuration(); c.mobile_approval_timeout_ms = 150; c.audit_log_path = (root / "audit.jsonl").string();
  {
    Fixture f(c, [&] { if (stall.exchange(false)) std::this_thread::sleep_for(std::chrono::milliseconds(180)); return now_ms(); });
    const auto id = f.pending(); f.decide(id); stall = true;
    check(f.connect().status == 409 && f.service.health().at("active_sessions") == 0, "audit delay allowed expired approval");
  }
  std::filesystem::remove_all(root);
}
void yaml_policy() {
  const auto root = std::filesystem::temp_directory_path() / ("mobile-config-" + random_token(6));
  std::filesystem::create_directories(root);
  std::ofstream(root / "secret") << "test-only-secret\n";
  const std::string prefix = "auth:\n  mobile_approval_timeout_ms: 75000\n  approver_token_ttl_ms: 600001\n  drivers:\n    - id: d\n      password_file: secret\n      vehicles: [v]\n  vehicles:\n    - id: v\n      device_token_file: secret\n      mobile_approval_required: true\n";
  const auto path = root / "config.yaml";
  std::ofstream(path) << prefix;
  const auto previous = std::filesystem::current_path();
  std::filesystem::create_directory(root / "config");
  std::filesystem::current_path(root);
  try {
    for (int mode = 0; mode < 2; ++mode) {
      if (mode == 1) std::ofstream(root / "config/app-token") << "\n";
      bool rejected = false;
      try { load_signaling_identity_config(path); } catch (const std::exception&) { rejected = true; }
      check(rejected, "missing or empty config/app-token accepted");
    }
    std::ofstream(root / "config/app-token") << "app-file-secret\n";
    const auto loaded = load_signaling_identity_config(path);
    check(loaded.mobile_approval_vehicles.contains("v") && loaded.mobile_approval_timeout_ms == 75000 &&
        loaded.approver_token_ttl_ms == 600001 && loaded.mobile_app_password == "app-file-secret", "config/app-token was not loaded");
    SignalingService validated(loaded);
  } catch (...) {
    std::filesystem::current_path(previous); std::filesystem::remove_all(root); throw;
  }
  std::filesystem::current_path(previous);
  std::filesystem::remove_all(root);
}
void automatic_driver_runtime() {
  for (const auto& outcome : {"approve", "reject", "timeout", "logout", "end", "replace"}) {
    auto c = configuration(); c.mobile_approval_timeout_ms = 1800;
    SignalingService service(c);
    SimpleHttpServer server("127.0.0.1", 0, [&](const auto& r) { return service.handle(r); }, 8 * 1024 * 1024,
        [&](SocketHandle socket, const auto& r) { return service.handle_websocket(socket, r); });
    server.start();
    const auto base = "http://127.0.0.1:" + std::to_string(server.port()); HttpClient http;
    const auto post = [&](const std::string& path, const Json& payload, const std::string& token = "") {
      HttpRequest r; r.method = "POST"; r.path = path; r.body = payload.dump();
      r.headers["x-mine-teleop-approver-token"] = token;
      return service.handle(r);
    };
    check(post("/vehicles/online", {{"vehicle_id","v1"},{"device_token","device-secret"},{"connection_id","runtime-test"}}).status == 200, "fixture registration failed");
    const auto token = body(post("/mobile/api/login", {{"password","approval-secret"}})).at("token").get<std::string>();
    DriverConfig dc; dc.driver_id = "d1"; dc.signaling_url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/signaling";
    dc.max_time_sync_uncertainty_ms = 2000; // Fixture exercises protocol compatibility, not clock safety.
    DriverConsoleRuntime driver(dc, "v1", "driver-secret");
    auto connecting = std::async(std::launch::async, [&] { return driver.connect("v1"); });
    Json pending;
    const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < limit) {
      pending = driver.status().at("pending_mobile_approval");
      if (pending.contains("request_id")) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(pending.contains("request_id"), "runtime did not expose approval wait state");
    const auto id = pending.at("request_id").get<std::string>();
    check(connecting.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready &&
        service.health().at("active_sessions") == 0, "connect finished before decision");
    const std::string scenario(outcome);
    if (scenario == "approve" || scenario == "reject") {
      check(post("/mobile/api/requests/" + id + "/decision", {{"decision",scenario}}, token).status == 200, "phone decision failed");
    } else if (scenario == "logout") {
      static_cast<void>(driver.disconnect("test-cancel"));
    } else if (scenario == "end") {
      static_cast<void>(driver.end_session("test-cancel"));
    } else if (scenario == "replace") {
      check(post("/vehicles/online", {{"vehicle_id","v1"},{"device_token","device-secret"},{"connection_id","replacement"}}).status == 200, "replacement failed");
    }
    check(connecting.wait_for(std::chrono::seconds(4)) == std::future_status::ready, "single connect did not finish promptly: " + scenario);
    bool connected = false;
    try { connected = connecting.get().at("connected").get<bool>(); }
    catch (const std::exception&) { check(scenario != "approve", "approved request did not automatically connect"); }
    check(connected == (scenario == "approve"), "unexpected control authority: " + scenario);
    check(driver.status().at("pending_mobile_approval").empty(), "finished operation left waiting status");
    if (connected) static_cast<void>(driver.end_session("test-complete"));
    check(service.health().at("active_sessions") == 0, "finished/cancelled request left authority");
    if (scenario != "approve") {
      check(post("/mobile/api/requests/" + id + "/decision", {{"decision","approve"}}, token).status == 409,
          "terminated request can be approved later: " + scenario);
    }
    // Cloud waiting is still on the original ID, even after a terminal state.
    const auto login = post("/auth/driver_heartbeat", {{"driver_id","d1"},{"token","wrong"}});
    check(login.status == 401, "fixture accepted invalid driver token");
  }
}
void scoped_retry_and_cancellation() {
  auto c = configuration(); c.mobile_approval_timeout_ms = 150;
  Fixture f(c); const auto id = f.pending();
  const auto request = Json{{"driver_id","d1"},{"vehicle_id","v1"},{"token",f.driver},{"approval_request_id",id}};
  const auto retry = [&] { return f.call("POST", "/sessions", request); };
  check(body(retry()).at("request_id") == id, "retry replaced request");
  std::this_thread::sleep_for(std::chrono::milliseconds(180));
  check(body(retry()).at("issue_code") == "mobile_approval_ended", "expired retry started new application");
  const auto next = f.pending();
  check(next != id && body(retry()).at("issue_code") == "mobile_approval_stale", "stale retry took over new application");
  f.decide(next);
  check(f.connect().status == 200, "approved session failed");
  auto cancellation = request; cancellation["approval_request_id"] = next;
  check(f.call("POST", "/sessions/approval/cancel", cancellation).status == 200 && f.service.health().at("active_sessions") == 0,
      "cancel after an uncertain grant left an orphan session");
  check(f.call("POST", "/sessions/approval/cancel", cancellation).status == 200, "cancellation retry not idempotent");
}
void cancel_during_grant_response() {
  auto c = configuration(); c.mobile_approval_timeout_ms = 3000;
  SignalingService service(c);
  std::promise<void> granted, release;
  auto release_future = release.get_future().share();
  SimpleHttpServer server("127.0.0.1", 0, [&](const auto& r) {
    auto response = service.handle(r);
    if (r.path == "/sessions" && response.status == 200) {
      granted.set_value(); release_future.wait();
    }
    return response;
  }, 8 * 1024 * 1024, [&](SocketHandle socket, const auto& r) { return service.handle_websocket(socket, r); });
  server.start();
  const auto call = [&](const std::string& path, const Json& data, const std::string& token = "") {
    HttpRequest r; r.method = "POST"; r.path = path; r.body = data.dump(); r.headers["x-mine-teleop-approver-token"] = token;
    return service.handle(r);
  };
  call("/vehicles/online", {{"vehicle_id","v1"},{"device_token","device-secret"},{"connection_id","race"}});
  const auto token = body(call("/mobile/api/login", {{"password","approval-secret"}})).at("token").get<std::string>();
  DriverConfig dc; dc.driver_id = "d1"; dc.signaling_url = "ws://127.0.0.1:" + std::to_string(server.port()) + "/signaling"; dc.max_time_sync_uncertainty_ms = 2000;
  DriverConsoleRuntime driver(dc, "v1", "driver-secret");
  auto connecting = std::async(std::launch::async, [&] { try { static_cast<void>(driver.connect("v1")); return true; } catch (...) { return false; } });
  Json pending;
  for (int n = 0; n < 200; ++n) {
    pending = driver.status().at("pending_mobile_approval"); if (!pending.empty()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  check(!pending.empty(), "race fixture did not begin waiting");
  call("/mobile/api/requests/" + pending.at("request_id").get<std::string>() + "/decision", {{"decision","approve"}}, token);
  const auto grant_ready = granted.get_future().wait_for(std::chrono::seconds(3)) == std::future_status::ready;
  if (!grant_ready) { release.set_value(); check(false, "grant response was not intercepted"); }
  auto exiting = std::async(std::launch::async, [&] { return driver.disconnect("race-cancel"); });
  // Keep the response suspended until exit has had a scheduling opportunity to signal cancellation.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  release.set_value();
  static_cast<void>(connecting.get()); static_cast<void>(exiting.get());
  check(service.health().at("active_sessions") == 0 && !driver.status().at("connected").get<bool>(), "late grant survived exit");
}

}
int main() {
  int failures = 0;
  for (const auto& [name, test] : std::vector<std::pair<std::string,std::function<void()>>>{
      {"login_source_isolation",login_source_isolation},{"login_source_capacity_and_expiry",login_source_capacity_and_expiry},
      {"independent_app_token_expiry",independent_app_token_expiry},{"gate_and_single_use",gate_and_single_use},{"permissions_and_race",permissions_and_race},
      {"rejection_and_expiry",rejection_and_expiry},{"connection_generation_and_revocation",connection_generation_and_revocation},
      {"authentication_and_api_routes",authentication_and_api_routes},{"audit_fail_closed",audit_fail_closed},
      {"invalid_config_and_restart",invalid_config_and_restart},{"heartbeat_and_login_expiry",heartbeat_and_login_expiry},
      {"expiry_during_audit",expiry_during_audit},{"yaml_policy",yaml_policy},{"automatic_driver_runtime",automatic_driver_runtime},
      {"scoped_retry_and_cancellation",scoped_retry_and_cancellation},{"cancel_during_grant_response",cancel_during_grant_response}}) {
    try { test(); std::cout << "[PASS] " << name << '\n'; }
    catch (const std::exception& e) { ++failures; std::cerr << "[FAIL] " << name << ": " << e.what() << '\n'; }
  }
  return failures ? 1 : 0;
}
