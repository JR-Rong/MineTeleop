#include "mine_teleop/session_logs.hpp"
#include "mine_teleop/server.hpp"
#include <cstdlib>
#include <atomic>
#include <fstream>
#include <iostream>
#include <thread>

using namespace mine_teleop;
namespace fs = std::filesystem;
namespace {
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Directory {
  fs::path path = fs::temp_directory_path() / ("session-log-test-" + random_token(8));
  Directory() { fs::create_directories(path); }
  ~Directory() { std::error_code error; fs::remove_all(path, error); }
};
void write(const fs::path& path, const std::string& text) { std::ofstream out(path, std::ios::binary); out << text; }
std::string read(const fs::path& path) { std::ifstream in(path, std::ios::binary); return {std::istreambuf_iterator<char>(in), {}}; }
void env(const char* key, const std::string& value) {
#if defined(_WIN32)
  _putenv_s(key, value.c_str());
#else
  setenv(key, value.c_str(), 1);
#endif
}
Json metadata(std::int64_t start) {
  return {{"key", "service-a_session-000001"}, {"service_instance_id", "service-a"},
      {"session_id", "session-000001"}, {"driver_id", "driver"}, {"vehicle_id", "vehicle"},
      {"started_at_utc_ms", start}, {"ended_at_utc_ms", start + 1000}};
}
void collection_and_zip() {
  Directory dir; const auto start = now_ms(); const auto session = metadata(start);
  const auto record = [&](std::string id, std::string message, std::int64_t time) {
    return Json{{"session_id", id}, {"sent_at_utc_ms", time}, {"message", message},
                {"password", "do-not-export"}, {"service_instance_id", "service-a"}}.dump() + "\n";
  };
  write(dir.path / "audit.jsonl", record("session-000001",
      "selected {\"password\":\"embedded-secret\"} Authorization: Bearer bearer-secret", start));
  write(dir.path / "audit.20260928T010000Z.part00.jsonl", record("session-000001", "rotated", start + 2));
  write(dir.path / "audit.jsonl.1", record("session-000002", "other-session", start) +
      record("session-000001", "old-collision", start - 100000));
  const auto manifest = collect_session_logs({{dir.path / "audit.jsonl", "cloud.jsonl", true}}, session, dir.path / "snapshot");
  const auto text = read(dir.path / "snapshot/cloud.jsonl");
  check(text.find("selected") != std::string::npos && text.find("rotated") != std::string::npos, "rotation omitted");
  check(text.find("other-session") == std::string::npos && text.find("old-collision") == std::string::npos, "session scope leaked");
  check(text.find("do-not-export") == std::string::npos && text.find("[redacted]") != std::string::npos, "credential leaked");
  check(text.find("embedded-secret") == std::string::npos, "embedded credential leaked");
  check(text.find("bearer-secret") == std::string::npos, "raw HTTP authorization leaked");
  const auto chunk = session_log_chunk(dir.path / "snapshot", manifest, "cloud.jsonl", 0);
  append_session_log_chunk(dir.path / "copy", chunk);
  check(read(dir.path / "copy/cloud.jsonl") == text, "chunk roundtrip lost bytes");
  bool denied = false;
  try { session_log_chunk(dir.path / "snapshot", manifest, "../audit.jsonl", 0); } catch (...) { denied = true; }
  check(denied, "path traversal allowed");
  denied = false;
  try { append_session_log_chunk(dir.path / "copy", chunk); } catch (...) { denied = true; }
  check(denied, "out of order duplicate chunk accepted");
  append_session_log_chunk(dir.path / "copy", chunk, true);
  check(read(dir.path / "copy/cloud.jsonl") == text, "retried upload duplicated bytes");
  auto changed = chunk; changed["data_hex"] = "00";
  denied = false;
  try { append_session_log_chunk(dir.path / "copy", changed, true); } catch (...) { denied = true; }
  check(denied, "conflicting retry overwrote uploaded data");
  write(dir.path / "snapshot/empty.log", "");
  write_session_zip(dir.path / "snapshot", dir.path / "session.zip");
  const auto zip = read(dir.path / "session.zip");
  check(zip.starts_with("PK\3\4") && zip.find("PK\5\6") != std::string::npos && zip.find("empty.log") != std::string::npos,
        "ZIP does not contain valid records including empty file");
  const auto missing = collect_session_logs({{dir.path / "missing", "missing.log"}}, session, dir.path / "missing-snapshot");
  check(missing.at("status") == "partial", "missing log reported complete");
  write(dir.path / "dropped.jsonl", Json{{"logged_at_utc_ms", start}, {"event", "trace"},
      {"details", {{"dropped_since_last", 2}}}}.dump() + "\n");
  const auto dropped = collect_session_logs({{dir.path / "dropped.jsonl", "dropped.jsonl"}}, session, dir.path / "dropped-snapshot");
  check(dropped.at("status") == "partial", "source queue loss reported complete");
  // The actual bridge uses ISO UTC `ts` and a 16-frame array, not session IDs.
  const auto can_time = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::sys_days(std::chrono::year{2026}/9/28).time_since_epoch()).count() + 123;
  Json frames = Json::array();
  for (int i = 0; i < 16; ++i) frames.push_back({{"id", i}, {"data", "00 7D"}});
  write(dir.path / "can.jsonl", Json{{"ts", "2026-09-28T00:00:00.123Z"},
      {"kind", "can_tx_batch"}, {"frames", frames}}.dump() + "\n");
  collect_session_logs({{dir.path / "can.jsonl", "can.jsonl"}}, metadata(can_time), dir.path / "can-snapshot");
  check(Json::parse(read(dir.path / "can-snapshot/can.jsonl")).at("frames").size() == 16,
        "ISO bridge record lost timestamp or CAN frames");
  const auto snapshot_catalog = dir.path / "persist/audit.jsonl"; fs::create_directories(snapshot_catalog.parent_path());
  { SessionLogBroker broker(snapshot_catalog); broker.remember(session); }
  { SessionLogBroker broker(snapshot_catalog);
    check(broker.sessions("driver").size() == 1 && broker.sessions("other").empty(), "catalog restart/ownership failed");
    const auto job = broker.start(session, false); const auto state = broker.status(job.at("export_id").get<std::string>(), "driver");
    check(state.at("vehicle").at("reason") == "vehicle_offline", "offline vehicle was not reported");
  }
}

void trace_batch_session_filter() {
  Directory dir; const auto start = now_ms();
  const auto command = [&](std::string session, std::string marker, std::int64_t time) {
    return Json{{"trace_session_id", session}, {"command_sent_at_utc_ms", time},
                {"marker", marker}, {"control_token", "batch-secret"}};
  };
  const auto batch = [&](std::string service, std::int64_t time, Json commands) {
    return Json{{"service_instance_id", service}, {"sent_at_utc_ms", time},
        {"event", "cloud_native_control_trace_batch"}, {"details", {{"commands", commands}}}}.dump() + "\n";
  };
  // A batch can flush late and contain another driver's session. Filter its
  // records independently; retain matching commands without leaking siblings.
  write(dir.path / "audit.jsonl", batch("service-a", start + 4000, Json::array({
      command("session-000001", "wanted", start), command("session-000002", "other-driver", start),
      command("session-000001", "out-of-window", start - 10000)})) +
      batch("service-b", start, Json::array({command("session-000001", "old-service", start)})) +
      batch("service-a", start, Json::array({command("session-000002", "unrelated-batch", start)})));
  collect_session_logs({{dir.path / "audit.jsonl", "cloud.jsonl", true}}, metadata(start), dir.path / "out");
  const auto text = read(dir.path / "out/cloud.jsonl");
  check(text.find("wanted") != std::string::npos, "session command batch omitted");
  for (const auto* excluded : {"other-driver", "out-of-window", "old-service", "unrelated-batch", "batch-secret"})
    check(text.find(excluded) == std::string::npos, "trace batch scope or credential leak");
  check(Json::parse(text).at("details").at("commands").size() == 1, "unrelated commands survived filtering");
  write(dir.path / "runtime.log", Json{{"event", "vehicle_control_trace_batch"}, {"event_at_utc_ms", start},
      {"commands", {command("session-000001", "vehicle-wanted", start),
                    command("session-000002", "vehicle-other", start)}}}.dump() + "\n");
  collect_session_logs({{dir.path / "runtime.log", "runtime.log"}}, metadata(start), dir.path / "vehicle-out");
  const auto vehicle = Json::parse(read(dir.path / "vehicle-out/runtime.log"));
  check(vehicle.at("commands").size() == 1 && vehicle.at("commands").at(0).at("marker") == "vehicle-wanted",
        "vehicle trace batch leaked another session");
}

void vehicle_export_failure_reporting() {
  Directory dir; SessionLogBroker broker(dir.path / "audit.jsonl");
  const auto session = metadata(now_ms());
  {
    SessionLogBroker unclaimed(dir.path / "unclaimed/audit.jsonl");
    const auto id = unclaimed.start(session, true).at("export_id").get<std::string>();
    const auto status = unclaimed.status(id, "driver", true);
    check(status.at("vehicle").at("reason") == "vehicle_worker_unresponsive", "unclaimed task reported as upload failure");
  }
  const auto id = broker.start(session, true).at("export_id").get<std::string>();
  broker.vehicle("vehicle", {{"operation", "poll"}}, true);
  const Json chunk = {{"name", "runtime.log"}, {"offset", 0}, {"data_hex", "616263"}};
  broker.vehicle("vehicle", {{"operation", "append"}, {"export_id", id}, {"chunk", chunk}}, true);
  broker.vehicle("vehicle", {{"operation", "append"}, {"export_id", id}, {"chunk", chunk}}, true);
  auto status = broker.status(id, "driver");
  check(status.at("vehicle_progress").at("bytes_received") == 3, "retry counted duplicate progress");
  broker.vehicle("vehicle", {{"operation", "failed"}, {"export_id", id}, {"stage", "upload"}}, true);
  status = broker.status(id, "driver");
  check(status.at("vehicle").at("reason") == "vehicle_upload_failed", "upload failure reason hidden");
  check(status.at("vehicle").at("files").at(0).at("bytes") == 3, "partial upload discarded");
  const auto retained = broker.chunk(id, "driver", {{"source", "vehicle"}, {"name", "runtime.log"}, {"offset", 0}});
  check(retained.at("data_hex") == "616263", "partial upload cannot be downloaded");
  SessionLogBroker cancelled(dir.path / "cancelled/audit.jsonl");
  const auto cancelled_id = cancelled.start(session, true).at("export_id").get<std::string>();
  cancelled.vehicle("vehicle", {{"operation", "poll"}}, true);
  const auto stopped = cancelled.vehicle("vehicle", {{"operation", "append"}, {"export_id", cancelled_id}, {"chunk", chunk}}, false);
  check(stopped.at("cancelled").get<bool>() &&
      cancelled.status(cancelled_id, "driver").at("vehicle").at("reason") == "new_control_session_started",
      "export continued after control session resumed");
}

void session_window_scan_priority() {
  Directory dir; const auto start = now_ms();
  const auto record = [&](std::int64_t time, std::string message) {
    return Json{{"session_id", "session-000001"}, {"logged_at_utc_ms", time}, {"message", message}}.dump() + "\n";
  };
  const auto base = dir.path / "runtime.log";
  write(base.string() + ".2", record(start - 100000, "older"));
  // A historical session can straddle two rotations.
  write(base.string() + ".1", record(start - 99000, "older-end"));
  write(base, record(start, "latest"));
  write(base.string() + ".3", "{invalid-json}\n" + record(start, "unknown-edge"));
  // An out-of-order middle record must not be excluded by edge sampling.
  const auto padding = record(start - 200000, std::string(40000, 'x'));
  write(base.string() + ".4", padding + padding + record(start, "clock-jump") + padding + padding);
  const auto mtime = fs::file_time_type::clock::now();
  for (int i = 1; i <= 4; ++i) fs::last_write_time(base.string() + "." + std::to_string(i), mtime - std::chrono::hours(i));
  fs::last_write_time(base, mtime);
  const auto collect = [&](std::int64_t timestamp, const char* name) {
    return collect_session_logs({{base, "runtime.log"}}, metadata(timestamp), dir.path / name);
  };
  const auto latest = collect(start, "latest");
  const auto latest_files = latest.at("files").at(0).at("source_files");
  check(latest_files.at(0) == "runtime.log.3" && latest_files.at(1) == "runtime.log",
        "older unrelated rotations can exhaust budget before latest session");
  const auto text = read(dir.path / "latest/runtime.log");
  for (const auto* message : {"latest", "unknown-edge", "clock-jump"})
    check(text.find(message) != std::string::npos, "timestamp sampling excluded a matching record");
  const auto historical = collect(start - 100000, "historical");
  const auto historical_files = historical.at("files").at(0).at("source_files");
  check(historical_files.at(0) == "runtime.log.2" && historical_files.at(1) == "runtime.log.1",
        "current file displaced requested historical session rotations");
  const auto old_text = read(dir.path / "historical/runtime.log");
  check(old_text.find("older") < old_text.find("older-end") && old_text.find("latest") == std::string::npos,
        "historical window or rotation ordering changed");
  std::stop_source stop; stop.request_stop();
  const auto cancelled = collect_session_logs({{base, "runtime.log"}}, metadata(start), dir.path / "cancelled", {}, stop.get_token());
  check(cancelled.at("status") == "partial" && cancelled.at("files").at(0).at("probe_bytes") == 0,
        "cancelled export still sampled rotations");
}

void package_configuration() {
  const auto config = load_vehicle_config("configs/vehicle-agent.three-machine.field.yaml");
  check(config.vehicle_adapter.bridge_library_path == fs::absolute("lib/vendor/chassis/libmine_teleop_chassis_bridge.so").lexically_normal(),
        "bridge root is not package cwd");
  Directory directory; write(directory.path / "external.yaml", read("configs/vehicle-agent.three-machine.field.yaml"));
  check(load_vehicle_config(directory.path / "external.yaml").vehicle_adapter.bridge_library_path == config.vehicle_adapter.bridge_library_path,
        "external YAML changed bridge root");
  const auto cameras = config.enabled_cameras(); check(cameras.size() == 2, "capture channel count");
  for (std::size_t i=0;i<cameras.size();++i) check(cameras[i].backend == "ccg2" &&
      cameras[i].device == "/dev/ccg2-channel-" + std::to_string(i) && cameras[i].capture_width == 1920 &&
      cameras[i].capture_height == 1080 && cameras[i].capture_fps == 30 && cameras[i].critical_for_control,
      "invalid default CCG2 input");
}

void three_endpoint_export() {
  Directory dir;
  SignalingServerConfig config;
  config.driver_passwords = {{"driver", "driver-password"}, {"other", "other-password"}};
  config.device_tokens = {{"vehicle", "device-secret"}};
  config.driver_vehicle_permissions = {{"driver", {"vehicle"}}, {"other", {"vehicle"}}};
  config.audit_log_path = (dir.path / "audit.jsonl").string();
  config.api_rate_limit_requests = 10000;
  SignalingService service(config);
  std::atomic<int> append_attempts{0}, finish_attempts{0};
  std::atomic<bool> permanent_upload_failure{false};
  SimpleHttpServer server("127.0.0.1", 0, [&](const HttpRequest& request) {
    if (request.path == "/sessions/logs/vehicle") {
      const auto operation = request.json_body().value("operation", "");
      if (operation == "append") {
        if (permanent_upload_failure) return ServerResponse::json(503, {{"error", "persistent upload failure"}});
        const auto attempt = ++append_attempts;
        if (attempt == 1) return ServerResponse::json(503, {{"error", "temporary upload failure"}});
        auto response = service.handle(request);
        if (attempt == 2) return ServerResponse::json(503, {{"error", "append succeeded but response lost"}});
        return response;
      }
      if (operation == "finish") {
        auto response = service.handle(request);
        if (++finish_attempts == 1) return ServerResponse::json(503, {{"error", "finish succeeded but response lost"}});
        return response;
      }
    }
    return service.handle(request);
  }); server.start();
  const auto origin = "http://127.0.0.1:" + std::to_string(server.port()); HttpClient http;
  const auto login = [&](const std::string& id, const std::string& password) {
    return http.post_json_response(origin + "/auth/driver_login", {{"driver_id", id}, {"password", password}}).at("token").get<std::string>();
  };
  const auto token = login("driver", "driver-password");
  static_cast<void>(http.post_json_response(origin + "/vehicles/online", {{"vehicle_id", "vehicle"}, {"device_token", "device-secret"}, {"connection_id", "test"}}));
  const auto session = http.post_json_response(origin + "/sessions", {{"driver_id", "driver"}, {"vehicle_id", "vehicle"}, {"token", token}});
  const auto id = session.at("session_id").get<std::string>(); const auto timestamp = now_ms();
  const auto event = Json{{"logged_at_utc_ms", timestamp}, {"session_id", id}, {"event", "retained-test-event"},
      {"password", "must-redact"}, {"message", "device-secret"}}.dump() + "\n";
  write(dir.path / "vehicle-runtime.log", event + Json{{"logged_at_utc_ms", timestamp}, {"session_id", id},
      {"event", "large-runtime-record"}, {"payload", std::string(300000, 'x')}}.dump() + "\n");
  write(dir.path / "vcu.jsonl", Json{{"logged_at_utc_ms", timestamp}, {"kind", "can_tx_batch"}, {"data", "test-CAN-evidence"}}.dump() + "\n");
  write(dir.path / "browser.jsonl", Json{{"sent_at_utc_ms", timestamp}, {"session_id", id},
      {"event", "browser-event"}, {"password", "must-redact"}, {"message", token}}.dump() + "\n");
  static_cast<void>(http.post_json_response(origin + "/sessions/" + id + "/end", {{"actor", "driver"}, {"token", token}}));
  auto records = http.post_json_response(origin + "/sessions/logs/driver", {{"operation", "list"}, {"driver_id", "driver"}, {"token", token}}).at("sessions");
  check(records.size() == 1, "closed session absent"); const auto key = records[0].at("key");
  const auto other = login("other", "other-password");
  check(http.post_json(origin + "/sessions/logs/driver", {{"operation", "start"}, {"key", key},
      {"driver_id", "other"}, {"token", other}}).status >= 400, "other driver exported session");
  check(http.post_json(origin + "/sessions/logs/vehicle", {{"operation", "poll"}, {"vehicle_id", "vehicle"},
      {"device_token", "wrong"}}).status == 401, "wrong device credential accepted");
  env("MINE_TELEOP_VEHICLE_RUNTIME_LOG_PATH", (dir.path / "vehicle-runtime.log").string());
  env("MINE_TELEOP_VCU_LOG_PATH", (dir.path / "vcu.jsonl").string());
  VehicleConfig vehicle; vehicle.vehicle_id = "vehicle"; vehicle.cloud.signaling_url = origin;
  auto worker = start_vehicle_log_worker(vehicle, "device-secret");
  ControllerLogExport exported(origin, {}, {}, "driver", token, key.get<std::string>(), dir.path / "browser.jsonl");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
  while (exported.status().at("state") == "collecting" && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto status = exported.status();
  check(status.at("state") == "ready", status.dump().c_str());
  check(append_attempts >= 5 && finish_attempts == 2, "transient failures did not retry append and finish");
  const auto zip = exported.zip();
  for (const auto* required : {"controller/control-browser-events.jsonl", "cloud/signaling-audit.jsonl",
                               "vehicle/runtime.log", "vehicle/vcu-can.jsonl", "manifest.json", "test-CAN-evidence"})
    check(zip.find(required) != std::string::npos, "three-endpoint ZIP omitted source");
  check(zip.find("must-redact") == std::string::npos && zip.find("device-secret") == std::string::npos, "ZIP leaked credential");
  if (const char* output = std::getenv("MINE_TELEOP_TEST_EXPORT_ZIP")) write(output, zip);
  permanent_upload_failure = true;
  ControllerLogExport failed(origin, {}, {}, "driver", token, key.get<std::string>(), dir.path / "browser.jsonl");
  const auto failed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
  while (failed.status().at("state") == "collecting" && std::chrono::steady_clock::now() < failed_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto failure = failed.status();
  check(failure.at("state") == "ready" && !failure.at("complete").get<bool>(), "exhausted upload retry did not finish as partial");
  check(failure.at("manifest").at("vehicle").at("reason") == "vehicle_upload_failed", "worker swallowed upload failure");
  worker.request_stop(); worker.join();
  // A pending vehicle upload must not turn a desktop close into a 3-minute join.
  auto pending = std::make_unique<ControllerLogExport>(origin, std::vector<std::string>{}, fs::path{},
      "driver", token, key.get<std::string>(), dir.path / "browser.jsonl");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  check(pending->status().at("state") == "collecting", "export was not pending");
  const auto cancelled_at = std::chrono::steady_clock::now(); pending.reset();
  check(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(3), "export cancellation blocked shutdown");
  server.stop();
}
}
int main() {
  try { package_configuration(); collection_and_zip(); trace_batch_session_filter(); vehicle_export_failure_reporting(); session_window_scan_priority(); three_endpoint_export(); std::cout << "session_log_export_tests=passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
