#include "mine_teleop/detail/direct_authority.hpp"
#include "mine_teleop/server.hpp"
#include "control_time_fixture.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
void expect(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class Action> void rejects(Action action) { bool failed = false; try { action(); } catch (const std::exception&) { failed = true; } expect(failed, "invalid authority accepted"); }
DirectAuthorityGrant grant(std::uint64_t epoch = 1, std::uint64_t sequence = 1, std::string nonce = "n1", std::int64_t issued = 1000) {
  return {"issuer", "vehicle-001", "driver-001", "session-001", std::move(nonce), 1, epoch, sequence, issued, issued + 600, 600};
}
DirectAuthorityLease lease() { DirectAuthorityLease result; result.configure("issuer", "vehicle-001", "driver-001", "session-001", 1, 600); return result; }
void nonce_clock_and_duplicate_deadlines() {
  auto value = lease(); value.begin_request("n1", 100);
  auto invalid = grant().json(); invalid["nonce"] = "wrong";
  expect(!value.accept_authenticated_cloud(invalid, 150, 1050, 10), "unbound cloud grant accepted");
  expect(value.accept_authenticated_cloud(grant().json(), 150, 1050, 10), "valid authenticated grant rejected");
  expect(value.valid(689) && !value.valid(690), "uncertainty/request-start conservative deadline changed");
  expect(!value.observe_vehicle_accepted(grant().json(), 600, 1500, 10) && !value.valid(690), "duplicate grant received a new whole TTL");
  expect(value.observe_expiry(690) && !value.observe_expiry(691), "expiry did not produce one recovery boundary");
  value.begin_request("n2", 700);
  expect(!value.accept_authenticated_cloud(grant(1, 2, "n2", 1600).json(), 750, 1650, 10), "vehicle revived expired authority without new epoch");
  expect(value.accept_authenticated_cloud(grant(2, 3, "n2", 1600).json(), 750, 1650, 10), "new authority epoch never recovered");
  const auto revision = value.rearm_revision(); value.revoke();
  expect(!value.valid(751) && value.rearm_revision() > revision, "explicit revocation tolerated as a transient outage");
  rejects([&] { value.begin_request("n3", 800); });
}
void ordinary_traffic_cannot_extend_authority_and_parser_is_strict() {
  auto value = lease(); value.begin_request("n1", 0);
  expect(value.accept_authenticated_cloud(grant().json(), 0, 1000, 0), "initial grant rejected");
  const auto reference = value.reference();
  for (int now = 0; now < 600; now += 10) expect(value.allows(reference, now), "valid current authority disappeared");
  expect(!value.allows(reference, 600), "fresh DC commands renewed authority");
  auto invalid = grant().json(); invalid["duration_ms"] = 600.1; rejects([&] { DirectAuthorityGrant::parse(invalid); });
  invalid = grant().json(); invalid["expires_at_utc_ms"] = std::numeric_limits<std::uint64_t>::max(); rejects([&] { DirectAuthorityGrant::parse(invalid); });
  auto other = reference; other["authority_epoch"] = 2; expect(!value.allows(other, 100), "foreign authority epoch accepted");
  auto future = reference; future["lease_seq"] = 999; expect(!value.allows(future, 100), "DC fabricated a future lease");
  auto delayed = lease(); delayed.begin_request("n1", 0);
  expect(!delayed.accept_authenticated_cloud(grant().json(), 601, 1601, 0), "delayed response resurrected a past request");
}
void native_observation_rearms_after_a_gap_but_not_every_renewal() {
  auto value = lease();
  expect(value.observe_vehicle_accepted(grant().json(), 0, 1000, 0), "native did not observe vehicle-accepted grant");
  const auto revision = value.rearm_revision();
  expect(value.observe_vehicle_accepted(grant(1, 2, "n2", 1200).json(), 210, 1210, 0), "same-epoch renewal rejected");
  expect(value.rearm_revision() == revision, "every cloud renewal invalidated a healthy operator input");
  expect(value.observe_vehicle_accepted(grant(1, 3, "n3", 1800).json(), 850, 1850, 0), "native could not recover a missed result cohort");
  expect(value.rearm_revision() > revision, "gap recovery resurrected held input without rearm boundary");
}
void real_common_service_stops_and_dc_rejection_does_not_refresh_watchdog() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  test::LogicalTimeControlService service(config, "driver-001", "session-001", "fixture", std::make_unique<MockVehicleAdapter>());
  service.start(0);
  SessionControlProfileRequest profile; profile.vehicle_id = "vehicle-001"; profile.driver_id = "driver-001";
  profile.session_id = "session-001"; profile.control_token = "fixture"; profile.seq = 1;
  expect(service.receive_session_profile(profile, 0).accepted, "fixture profile failed");
  auto authority = lease(); authority.begin_request("n1", 0); authority.accept_authenticated_cloud(grant().json(), 0, 1000, 0);
  ControlCommand command; command.vehicle_id = "vehicle-001"; command.driver_id = "driver-001"; command.session_id = "session-001";
  command.control_token = "fixture"; command.seq = 1; command.gear = "D"; command.throttle = .5;
  expect(authority.allows(authority.reference(), 0) && service.receive_command(command, 0).accepted, "initial leased motion failed");
  expect(authority.observe_expiry(600), "vehicle watchdog did not observe independent authority expiry");
  service.set_control_braking_wait(true, 600, ControlRecoveryCause::ControlLink);
  expect(service.control_braking_wait(), "authority expiry did not request braking");
  for (int now = 600; now <= config.control.control_timeout_ms; now += 10) {
    expect(!authority.allows(authority.reference(), now), "fresh ordinary traffic bypassed expired authority");
    service.tick(now); // Rejected DC is never forwarded to receive_command.
  }
  expect(service.safety_state() == SafetyState::TimeoutBrake && !service.session_control_profile().value("active", false),
      "expired direct authorization masked existing hard watchdog/profile reset");
  service.close();
}
struct CloudFixture {
  std::filesystem::path directory;
  SignalingServerConfig config;
  std::unique_ptr<SignalingService> service;
  Json login1, login2, online, session;
  CloudFixture() {
    directory = std::filesystem::temp_directory_path() / ("mine-direct-authority-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(directory);
    config.experimental_direct_authority_lease_ms = 80; // TEST only, not the 600ms review proposal or product default.
    config.experimental_direct_authority_journal = directory / "authority.json";
    config.driver_passwords = {{"d1", "public-fixture-password"}, {"d2", "public-fixture-password"}};
    config.device_tokens = {{"v", "public-fixture-device"}}; config.driver_vehicle_permissions = {{"d1", {"v"}}, {"d2", {"v"}}};
    start();
  }
  ~CloudFixture() { service.reset(); std::filesystem::remove_all(directory); }
  Json post(std::string path, Json body, int expected = 200) {
    HttpRequest request; request.method = "POST"; request.path = path; request.target = path; request.peer_address = "127.0.0.1"; request.body = body.dump();
    const auto response = service->handle(request);
    if (response.status != expected) throw std::runtime_error(path + ": unexpected status " + std::to_string(response.status));
    return Json::parse(response.body);
  }
  void start() { service = std::make_unique<SignalingService>(config); }
  void participants(bool register_vehicle = true) {
    login1 = post("/auth/driver_login", {{"driver_id", "d1"}, {"password", "public-fixture-password"}});
    login2 = post("/auth/driver_login", {{"driver_id", "d2"}, {"password", "public-fixture-password"}});
    if (register_vehicle) online = post("/vehicles/online", {{"vehicle_id", "v"}, {"device_token", "public-fixture-device"}, {"connection_id", "runtime"}, {"native_control_capabilities", kNativeControlCapabilities}});
  }
  Json session_body(bool second = false) {
    return {{"driver_id", second ? "d2" : "d1"}, {"vehicle_id", "v"}, {"token", (second ? login2 : login1).at("token")},
        {"required_control_transport_capability", kControlTransportCapability}, {"required_direct_authority_capability", kDirectAuthorityCapability}};
  }
  Json request(std::string nonce) {
    return {{"device_token", "public-fixture-device"}, {"connection_generation", online.at("connection_generation")},
        {"session_id", session.at("session_id")}, {"nonce", std::move(nonce)}, {"maximum_duration_ms", 80}};
  }
};
void actual_cloud_revoke_does_not_free_the_old_grant_early() {
  CloudFixture cloud; cloud.participants(); cloud.session = cloud.post("/sessions", cloud.session_body());
  auto wrong = cloud.request("x"); wrong["device_token"] = "wrong"; cloud.post("/vehicles/v/direct-authority", wrong, 401);
  auto low = cloud.request("x"); low["maximum_duration_ms"] = 79; cloud.post("/vehicles/v/direct-authority", low, 409);
  const auto first = cloud.post("/vehicles/v/direct-authority", cloud.request("n1"));
  expect(cloud.post("/vehicles/v/direct-authority", cloud.request("n1")) == first, "same nonce extended actual cloud grant");
  cloud.post("/sessions/" + cloud.session.at("session_id").get<std::string>() + "/end", {{"actor", "d1"}, {"token", cloud.login1.at("token")}});
  cloud.post("/sessions", cloud.session_body(true), 409);
  cloud.post("/vehicles/online", {{"vehicle_id", "v"}, {"device_token", "public-fixture-device"}, {"connection_id", "other"}}, 409);
  cloud.post("/vehicles/v/direct-authority", cloud.request("n2"), 409);
  std::this_thread::sleep_for(std::chrono::milliseconds(90));
  expect(cloud.post("/sessions", cloud.session_body(true)).contains("session_id"), "expired old grant blocked new authority indefinitely");
  std::ifstream journal(cloud.config.experimental_direct_authority_journal); const auto text = std::string(std::istreambuf_iterator<char>(journal), {});
  expect(text.find("public-fixture-password") == std::string::npos && text.find("public-fixture-device") == std::string::npos, "durable authority journal saved credentials");
}
void restart_is_exclusive_and_waits_a_full_prior_duration() {
  CloudFixture cloud; cloud.participants(); cloud.session = cloud.post("/sessions", cloud.session_body());
  const auto old = cloud.post("/vehicles/v/direct-authority", cloud.request("n1"));
  rejects([&] { SignalingService duplicate(cloud.config); });
  cloud.service.reset(); cloud.start(); cloud.participants(false);
  cloud.post("/vehicles/online", {{"vehicle_id", "v"}, {"device_token", "public-fixture-device"}, {"connection_id", "runtime"}}, 409);
  std::this_thread::sleep_for(std::chrono::milliseconds(90));
  cloud.online = cloud.post("/vehicles/online", {{"vehicle_id", "v"}, {"device_token", "public-fixture-device"}, {"connection_id", "runtime"}});
  cloud.session = cloud.post("/sessions", cloud.session_body());
  const auto next = cloud.post("/vehicles/v/direct-authority", cloud.request("n2"));
  expect(next.at("issuer") != old.at("issuer") && next.at("authority_epoch") > old.at("authority_epoch"), "cloud restart reused old issuer/authority identity");
}
void failed_journal_retry_cannot_issue_an_unpersisted_grant() {
  CloudFixture cloud; cloud.participants(); cloud.session = cloud.post("/sessions", cloud.session_body());
  const auto blocked = cloud.config.experimental_direct_authority_journal.string() + ".tmp"; std::filesystem::create_directory(blocked);
  cloud.post("/vehicles/v/direct-authority", cloud.request("n1"), 503);
  cloud.post("/vehicles/v/direct-authority", cloud.request("n1"), 503);
  std::filesystem::remove(blocked);
  expect(cloud.post("/vehicles/v/direct-authority", cloud.request("n1")).contains("authority_epoch") &&
      std::filesystem::is_regular_file(cloud.config.experimental_direct_authority_journal), "durability retry did not recover conservatively");
}
}
int main() {
  try {
    expect(!DriverConfig{}.enable_experimental_direct_authority && SignalingServerConfig{}.experimental_direct_authority_lease_ms == 0 &&
        VehicleRuntimeConfig{}.experimental_direct_authority_max_ms == 0, "unreviewed direct authority enabled by default");
    nonce_clock_and_duplicate_deadlines(); ordinary_traffic_cannot_extend_authority_and_parser_is_strict(); native_observation_rearms_after_a_gap_but_not_every_renewal();
    real_common_service_stops_and_dc_rejection_does_not_refresh_watchdog(); actual_cloud_revoke_does_not_free_the_old_grant_early();
    restart_is_exclusive_and_waits_a_full_prior_duration(); failed_journal_retry_cannot_issue_an_unpersisted_grant();
    std::cout << "7 direct authority cases passed; real cloud handlers/journal/common service, no sockets or hardware\n";
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
