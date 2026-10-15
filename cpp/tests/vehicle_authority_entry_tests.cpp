#include "mine_teleop/detail/direct_authority.hpp"
#include "control_time_fixture.hpp"

#include <iostream>
#include <mutex>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::detail;
void expect(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
std::int64_t steady_now_ms() { return 100; }
struct WebSocketClient {};
struct Entry {
  struct { std::int64_t now_ms() const { return 100; } } signaling;
  DirectAuthorityLease direct_authority;
  Json native_control_websocket_authority_scope;
  std::mutex control_mutex;
  bool control_service_started{true}, direct_authority_stop_failed{false};
  std::unique_ptr<test::LogicalTimeControlService> control_service;
  unsigned invalidations{};
  void invalidate_native_control_trusted_gear_locked() { ++invalidations; }
  Entry() {
    auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
    control_service = std::make_unique<test::LogicalTimeControlService>(config, "d", "s", "t", std::make_unique<MockVehicleAdapter>());
    control_service->start(0);
    SessionControlProfileRequest profile; profile.vehicle_id = "vehicle-001"; profile.driver_id = "d"; profile.session_id = "s";
    profile.control_token = "t"; profile.seq = 1;
    expect(control_service->receive_session_profile(profile, 0).accepted, "common service profile failed");
    direct_authority.configure("issuer", "vehicle-001", "d", "s", 1, 600);
    direct_authority.begin_request("n", 0);
    const DirectAuthorityGrant grant{"issuer", "vehicle-001", "d", "s", "n", 1, 1, 1, 1000, 1600, 600};
    expect(direct_authority.accept_authenticated_cloud(grant.json(), 0, 1000, 0), "fixture grant failed");
    native_control_websocket_authority_scope = {{"protocol", kDirectAuthorityCapability}, {"issuer", "issuer"},
        {"vehicle_id", "vehicle-001"}, {"driver_id", "d"}, {"session_id", "s"}, {"connection_generation", 1}, {"authority_epoch", 1}};
    ControlCommand command; command.vehicle_id = "vehicle-001"; command.driver_id = "d"; command.session_id = "s";
    command.control_token = "t"; command.seq = 1; command.gear = "D"; command.throttle = .5;
    expect(control_service->receive_command(command, 0).accepted, "initial common service command failed");
  }
  ~Entry() { control_service->close(); }
#include "vehicle_authority_entry.inc"
};
void actual_entry_rejects_fresh_dc_after_trusted_explicit_denial() {
  Entry value; WebSocketClient socket;
  const auto reference = value.direct_authority.reference();
  expect(value.direct_authority.allows(reference, 100), "fixture grant was not live");
  try { value.handle_native_control_websocket_envelope(socket, {{"event", "signaling_authority_lost"},
      {"error", "fixture session revoked"}, {"authority_scope", value.native_control_websocket_authority_scope}}); }
  catch (const std::exception&) {} // Existing loop classifies the exception as a connection failure.
  expect(!value.direct_authority.valid(100), "COUNTEREXAMPLE: trusted explicit WSS denial left accepted short grant live");
  expect(!value.direct_authority.allows(reference, 101) && value.control_service->control_braking_wait(),
      "fresh DC was not fenced and braked before the original lease deadline");
}
void transport_failure_preserves_only_existing_deadline() {
  Entry value;
  const HttpTransportError unavailable("fixture connection absent", Json(nullptr));
  expect(transient_cloud_failure(unavailable), "transport outage was misclassified as explicit denial");
  // A lost socket delivers no cloud error envelope. Actual socket loss is
  // additionally covered by the native/cloud localhost outage fixture.
  expect(value.direct_authority.valid(100) && !value.direct_authority.valid(600), "transport outage changed or revoked the old bounded lease");
  Entry rejected; WebSocketClient socket;
  try { rejected.handle_native_control_websocket_envelope(socket, {{"event", "websocket_protocol_error"}, {"error", "fixture cloud rejected protocol"}}); }
  catch (const std::exception&) {}
  expect(!rejected.direct_authority.valid(100), "cloud-declared error was mistaken for an unreachable socket");
}
void duplicate_denial_is_terminal_and_idempotent() {
  Entry value; WebSocketClient socket; value.direct_authority.begin_request("pending-before-revoke", 50);
  Json notice = {{"event", "signaling_authority_lost"}, {"error", "revoked"}, {"authority_scope", value.native_control_websocket_authority_scope}};
  value.handle_native_control_websocket_envelope(socket, notice);
  const auto revision = value.direct_authority.rearm_revision();
  value.handle_native_control_websocket_envelope(socket, notice);
  expect(value.direct_authority.rearm_revision() == revision && value.invalidations == 1, "duplicate revocation retriggered a recovery boundary");
  const DirectAuthorityGrant delayed{"issuer", "vehicle-001", "d", "s", "pending-before-revoke", 1, 1, 2, 1050, 1650, 600};
  expect(!value.direct_authority.accept_authenticated_cloud(delayed.json(), 101, 1101, 0) && !value.direct_authority.renewal_due(102),
      "in-flight renewal reopened explicit revoked authority");
}
void stale_identity_epoch_and_legacy_connection_are_fenced() {
  for (const auto* field : {"issuer", "vehicle_id", "driver_id", "session_id", "connection_generation", "authority_epoch"}) {
    Entry value; WebSocketClient socket; auto wrong = value.native_control_websocket_authority_scope;
    if (std::string_view(field) == "connection_generation" || std::string_view(field) == "authority_epoch") wrong[field] = 2;
    else wrong[field] = "retired";
    value.handle_native_control_websocket_envelope(socket, {{"event", "signaling_authority_lost"}, {"error", "old scoped denial"}, {"authority_scope", wrong}});
    expect(value.direct_authority.valid(100) && !value.control_service->control_braking_wait(), "old scope/epoch denial revoked current authority");
  }
  Entry newer; WebSocketClient old_socket; const auto old_scope = newer.native_control_websocket_authority_scope;
  newer.direct_authority.begin_request("new-epoch", 50);
  const DirectAuthorityGrant grant{"issuer", "vehicle-001", "d", "s", "new-epoch", 1, 2, 2, 1050, 1650, 600};
  expect(newer.direct_authority.accept_authenticated_cloud(grant.json(), 60, 1060, 0), "new epoch fixture failed");
  newer.handle_native_control_websocket_envelope(old_socket, {{"event", "signaling_authority_lost"}, {"authority_scope", old_scope}});
  expect(newer.direct_authority.valid(100), "retired explicit epoch revoked a newer accepted epoch");
  Entry value; WebSocketClient socket;
  value.native_control_websocket_authority_scope["session_id"] = "retired";
  value.handle_native_control_websocket_envelope(socket, {{"event", "signaling_authority_lost"}, {"error", "legacy old-session notice"}});
  expect(value.direct_authority.valid(100), "unscoped old connection revoked new session");
  value.native_control_websocket_authority_scope["session_id"] = "s";
  value.handle_native_control_websocket_envelope(socket, {{"event", "signaling_authority_lost"}, {"error", "legacy current-session denial"}});
  expect(!value.direct_authority.valid(100), "current trusted legacy denial was treated as transport grace");
  Entry malformed; auto bad = malformed.native_control_websocket_authority_scope; bad.erase("authority_epoch");
  malformed.handle_native_control_websocket_envelope(socket, {{"event", "signaling_authority_lost"}, {"authority_scope", bad}});
  expect(!malformed.direct_authority.valid(100), "malformed trusted current denial was allowed network grace");
  Entry typed; auto invalid_type = typed.native_control_websocket_authority_scope; invalid_type["connection_generation"] = "1";
  typed.handle_native_control_websocket_envelope(socket, {{"event", "signaling_authority_lost"}, {"authority_scope", invalid_type}});
  expect(!typed.direct_authority.valid(100), "malformed current identity type was mistaken for a legitimate stale epoch");
}

}
int main() {
  try { actual_entry_rejects_fresh_dc_after_trusted_explicit_denial(); transport_failure_preserves_only_existing_deadline(); duplicate_denial_is_terminal_and_idempotent(); stale_identity_epoch_and_legacy_connection_are_fenced(); std::cout << "4 actual vehicle WSS entry denial groups passed; source prefix + real lease/common service, no socket/Gst proof\n"; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
