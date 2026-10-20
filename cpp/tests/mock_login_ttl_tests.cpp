#include "mine_teleop/server.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace mine_teleop;
namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
SignalingServerConfig configuration() {
  SignalingServerConfig c;
  c.driver_passwords = {{"mock-fixture", "public-fixture"}, {"ordinary", "public-fixture"}};
  c.device_tokens = {{"test_bench_001", "public-device-fixture"}, {"vehicle-001", "public-device-fixture"}};
  c.driver_vehicle_permissions = {{"mock-fixture", {"test_bench_001"}}, {"ordinary", {"vehicle-001", "test_bench_001"}}};
  c.token_ttl_ms = 60 * 60 * 1000;
  return c;
}
struct Fixture {
  SignalingService service;
  explicit Fixture(SignalingServerConfig c) : service(std::move(c)) {}
  ServerResponse post(const std::string& path, Json data) {
    HttpRequest request; request.method = "POST"; request.path = request.target = path;
    request.peer_address = "127.0.0.1"; request.body = data.dump();
    return service.handle(request);
  }
  Json login(const char* driver, std::int64_t expected_ttl) {
    const auto before = now_ms();
    const auto response = post("/auth/driver_login", {{"driver_id", driver}, {"password", "public-fixture"},
        {"mock_test_login_ttl_ms", 999999999}, {"token_ttl_ms", 999999999}, {"expires_at_ms", 9999999999999LL}});
    const auto after = now_ms();
    check(response.status == 200, "fixture login failed");
    const auto result = Json::parse(response.body);
    const auto expires = result.at("expires_at_ms").get<std::int64_t>();
    check(expires >= before + expected_ttl && expires <= after + expected_ttl, "login TTL ignored configuration or accepted client override");
    return result;
  }
  void online(const char* vehicle) {
    check(post("/vehicles/online", {{"vehicle_id", vehicle}, {"device_token", "public-device-fixture"},
        {"connection_id", std::string("connection-") + vehicle}}).status == 200, "fixture vehicle registration failed");
  }
};
void defaults_and_target() {
  const SignalingServerConfig defaults;
  check(defaults.token_ttl_ms == 30 * 60 * 1000 && defaults.mock_test_login_ttl_ms.empty(), "source defaults changed");
  { Fixture f(configuration()); f.login("mock-fixture", 3600000); f.login("ordinary", 3600000); }
  auto c = configuration(); c.mock_test_login_ttl_ms = {{"mock-fixture", 240LL * 60 * 1000}};
  Fixture f(c); const auto login = f.login("mock-fixture", 14400000); f.login("ordinary", 3600000);
  f.online("vehicle-001"); f.online("test_bench_001");
  const auto token = login.at("token");
  check(f.post("/sessions", {{"driver_id", "mock-fixture"}, {"token", token}, {"vehicle_id", "vehicle-001"},
      {"vehicles", Json::array({"vehicle-001"})}}).status == 401, "mock driver escaped its bench permission");
  check(f.post("/sessions", {{"driver_id", "mock-fixture"}, {"token", token}, {"vehicle_id", "test_bench_001"}}).status == 200,
      "authorized mock driver could not open its bench session");
}
void invalid_scope() {
  for (int scenario = 0; scenario < 7; ++scenario) {
    auto c = configuration(); c.mock_test_login_ttl_ms = {{"mock-fixture", 14400000}};
    if (scenario == 0) c.mock_test_login_ttl_ms = {{"missing", 14400000}};
    if (scenario == 1) c.mock_test_login_ttl_ms = {{"ordinary", 14400000}};
    if (scenario == 2) c.driver_vehicle_permissions["mock-fixture"] = {"vehicle-001"};
    if (scenario == 3) c.driver_vehicle_permissions["mock-fixture"].clear();
    if (scenario == 4) c.mock_test_login_ttl_ms["mock-fixture"] = 0;
    if (scenario == 5) c.mock_test_login_ttl_ms["mock-fixture"] = -1;
    if (scenario == 6) c.mock_test_login_ttl_ms["mock-fixture"] = 14400001;
    bool rejected = false;
    try { SignalingService service(c); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "invalid target or TTL did not fail closed at startup");
  }
}
void expiry_is_not_renewal() {
  auto c = configuration(); c.mock_test_login_ttl_ms = {{"mock-fixture", 1800}};
  Fixture f(c); const auto login = f.login("mock-fixture", 1800);
  const auto token = login.at("token"); const auto expiry = login.at("expires_at_ms").get<std::int64_t>();
  f.online("test_bench_001");
  const auto opened = f.post("/sessions", {{"driver_id", "mock-fixture"}, {"token", token}, {"vehicle_id", "test_bench_001"}});
  check(opened.status == 200, "expiry fixture session failed");
  const auto session = Json::parse(opened.body);
  const auto renew_path = "/sessions/" + session.at("session_id").get<std::string>() + "/renew";
  const Json renewal{{"actor", "mock-fixture"}, {"token", token}, {"control_token", session.at("control_token")}};
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  unsigned heartbeats = 0;
  while (now_ms() < expiry - 200) {
    check(std::chrono::steady_clock::now() < deadline, "expiry clock did not advance");
    check(f.post("/auth/driver_heartbeat", {{"driver_id", "mock-fixture"}, {"token", token}}).status == 200, "live heartbeat failed");
    check(f.post(renew_path, renewal).status == 200, "live control lease renewal failed");
    ++heartbeats;
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  check(heartbeats > 0, "expiry fixture never exercised heartbeat and control renewal");
  while (now_ms() < expiry) {
    check(std::chrono::steady_clock::now() < deadline, "expiry clock did not reach original boundary");
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  check(f.post("/auth/driver_heartbeat", {{"driver_id", "mock-fixture"}, {"token", token}}).status == 401,
      "heartbeats or control renewals extended login TTL");
  check(f.post(renew_path, renewal).status != 200, "expired login renewed the old control session");
  check(f.service.health().value("active_sessions", 999) == 0, "expired login retained its active control session");
  const auto replacement = f.login("mock-fixture", 1800);
  check(replacement.at("token") != token && replacement.at("connection_generation") != login.at("connection_generation"), "new login reused old authority");
  check(f.post(renew_path, {{"actor", "mock-fixture"}, {"token", replacement.at("token")}, {"control_token", session.at("control_token")}}).status != 200,
      "fresh login automatically recovered the old session");
}
void identity_yaml() {
  const auto root = std::filesystem::temp_directory_path() / ("mock-login-ttl-fixture-" + random_token(6));
  std::filesystem::create_directories(root);
  struct Cleanup { std::filesystem::path path; ~Cleanup() { std::filesystem::remove_all(path); } } cleanup{root};
  std::ofstream(root / "public-fixture") << "public-test-fixture-not-a-real-credential\n";
  const auto path = root / "identity.yaml";
  const auto write = [&](std::string value) {
    std::ofstream out(path);
    out << "auth:\n  drivers:\n    - id: mock-fixture\n      password_file: public-fixture\n      vehicles: [test_bench_001]\n";
    if (!value.empty()) out << "      mock_test_login_ttl_ms: " << value << '\n';
    out << "  vehicles:\n    - id: test_bench_001\n      device_token_file: public-fixture\n";
  };
  write(""); check(load_signaling_identity_config(path).mock_test_login_ttl_ms.empty(), "omitted YAML override enabled itself");
  write("14400000");
  const auto c = load_signaling_identity_config(path);
  check(c.mock_test_login_ttl_ms.at("mock-fixture") == 14400000 && c.token_ttl_ms == 1800000, "identity override changed global default");
  { SignalingService service(c); }
  write("invalid"); bool rejected = false;
  try { static_cast<void>(load_signaling_identity_config(path)); } catch (const std::invalid_argument&) { rejected = true; }
  check(rejected, "non-integer YAML override was accepted");
}
}
int main() {
  try { defaults_and_target(); invalid_scope(); expiry_is_not_renewal(); identity_yaml(); }
  catch (const std::exception& error) { std::cerr << "FAIL mock login TTL: " << error.what() << '\n'; return 1; }
  std::cout << "PASS default/target TTL, client override rejection, scope validation, expiry/no session renewal, identity YAML\n";
  return 0;
}
