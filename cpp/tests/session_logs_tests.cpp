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
Json recording_summary(const Json& session) {
  const auto finished = session.at("ended_at_utc_ms").get<std::int64_t>() + 6000;
  Json lane = {{"schema_version", 1}, {"configured_enabled", true}, {"active", false}, {"state", "finalized"},
      {"incomplete", false}, {"issue_code", ""}, {"next_action", ""}, {"recording_attempt_id", "attempt-current"},
      {"status_revision", 4}, {"accepted_access_units", 50}, {"worker_input_access_units", 50},
      {"finalized_segments", 1}, {"unfinished_segments", 0}, {"final_eos_received", true}, {"cleanup_pending", false}};
  return {{"schema_version", 1}, {"kind", "recording_session_summary"}, {"upload_state", "not_applicable"},
      {"vehicle_id", session.at("vehicle_id")}, {"session_id", session.at("session_id")},
      {"recording_attempt_id", "attempt-current"}, {"configured_enabled", true}, {"state", "finalized"}, {"incomplete", false},
      {"expected_cameras", Json::array({"front"})}, {"final_eos_received", true}, {"summary_revision", 4},
      {"reported_at_utc_ms", finished}, {"completed_at_utc_ms", finished}, {"attempt_complete", true},
      {"completeness_scope", "current_runtime_known_attempts"}, {"session_history_known", false},
      {"cameras", Json::array({{{"camera_id", "front"}, {"recording", lane}}})}};
}

void recording_terminal_failure_after_session_end() {
  Directory dir; const auto session = metadata(now_ms());
  const auto directory = dir.path / "recordings" / "vehicle" / "session-000001";
  fs::create_directories(directory);
  auto summary = recording_summary(session);
  summary["state"] = "recording"; summary["attempt_complete"] = false;
  summary["final_eos_received"] = false; summary["completed_at_utc_ms"] = nullptr;
  summary["reported_at_utc_ms"] = session.at("ended_at_utc_ms").get<std::int64_t>() - 500;
  auto& lane = summary["cameras"][0]["recording"];
  lane["state"] = "recording"; lane["active"] = true; lane["final_eos_received"] = false;
  lane["unfinished_segments"] = 1;
  write(directory / "recording-session.status", summary.dump());
  const auto started = Json{{"event", "vehicle_recording_attempt_started"}, {"vehicle_id", "vehicle"},
      {"session_id", "session-000001"}, {"recording_attempt_id", "attempt-current"},
      {"reported_at_utc_ms", session.at("started_at_utc_ms")}};
  const auto finished_at = session.at("ended_at_utc_ms").get<std::int64_t>() + 8600;
  const auto finished = Json{{"event", "vehicle_recording_attempt_finished"}, {"vehicle_id", "vehicle"},
      {"session_id", "session-000001"}, {"recording_attempt_id", "attempt-current"},
      {"event_at_utc_ms", finished_at}, {"reported_at_utc_ms", finished_at}, {"incomplete", true}};
  auto other = finished; other["session_id"] = "session-000002";
  auto other_vehicle = finished; other_vehicle["vehicle_id"] = "other-vehicle";
  auto reused_session = finished; reused_session["recording_attempt_id"] = "later-unrelated-attempt";
  reused_session["driver_id"] = "other-driver-must-not-export";
  reused_session["reported_at_utc_ms"] = finished_at + 1000;
  reused_session["event_at_utc_ms"] = finished_at + 1000;
  auto reused_start = started; reused_start["recording_attempt_id"] = "later-unrelated-attempt";
  reused_start["reported_at_utc_ms"] = session.at("ended_at_utc_ms").get<std::int64_t>() + 1000;
  const auto snapshot = dir.path / "snapshot/runtime.log";
  // Rotation scan order need not place the attempt-start before completion.
  write(dir.path / "runtime.log", finished.dump() + "\n" + reused_session.dump() + "\n" +
      other.dump() + "\n" + other_vehicle.dump() + "\n" + reused_start.dump() + "\n" + started.dump() + "\n");
  collect_session_logs({{dir.path / "runtime.log", "runtime.log"}}, session, dir.path / "snapshot");
  const auto exported = read(snapshot);
  check(exported.find("vehicle_recording_attempt_finished") != std::string::npos,
        "bounded recorder completion after the ordinary UTC tail was dropped");
  check(exported.find("other-vehicle") == std::string::npos && exported.find("session-000002") == std::string::npos,
        "late recorder completion crossed its vehicle/session boundary");
  check(exported.find("other-driver-must-not-export") == std::string::npos,
        "late completion from a reused session ID crossed the current attempt boundary");
  const auto evidence = collect_session_recording_evidence(dir.path / "recordings", session, {}, snapshot, true);
  check(evidence.at("evidence_status") == "incomplete" && evidence.at("complete") == false &&
        evidence.at("attempt_complete") == false,
        "terminal runtime failure was hidden by an older persisted recording snapshot");
  check(evidence.at("attempt_fence").at("kind") == "finished" &&
        evidence.at("attempt_fence").at("reported_at_utc_ms") == finished_at,
        "completion failure did not retain its actual terminal observation time");
  // The packaged launcher timestamps stdout when it relays it. Its delay
  // cannot move a real attempt out of this session or a later attempt into it.
  auto delayed_start = started;
  delayed_start["logged_at_utc_ms"] = session.at("ended_at_utc_ms").get<std::int64_t>() + 3000;
  delayed_start["relay_marker"] = "delayed-start-remains-outside-ordinary-log-window";
  auto misleading_relay = reused_start;
  misleading_relay["logged_at_utc_ms"] = session.at("started_at_utc_ms");
  auto missing_producer_start = started;
  missing_producer_start["recording_attempt_id"] = "missing-producer-time";
  missing_producer_start.erase("reported_at_utc_ms");
  missing_producer_start["logged_at_utc_ms"] = session.at("started_at_utc_ms");
  auto missing_producer_finished = finished;
  missing_producer_finished["recording_attempt_id"] = "missing-producer-time";
  missing_producer_finished["driver_id"] = "missing-producer-must-not-export";
  write(dir.path / "runtime.log", finished.dump() + "\n" + reused_session.dump() + "\n" +
      misleading_relay.dump() + "\n" + missing_producer_finished.dump() + "\n" +
      missing_producer_start.dump() + "\n" + delayed_start.dump() + "\n");
  const auto delayed_snapshot = dir.path / "delayed/runtime.log";
  collect_session_logs({{dir.path / "runtime.log", "runtime.log"}}, session, dir.path / "delayed");
  const auto delayed_export = read(delayed_snapshot);
  check(delayed_export.find("\"recording_attempt_id\":\"attempt-current\"") != std::string::npos,
        "launcher relay delay hid a valid producer-time attempt anchor");
  check(delayed_export.find("other-driver-must-not-export") == std::string::npos &&
        delayed_export.find("missing-producer-must-not-export") == std::string::npos,
        "launcher timestamps authorized a late completion without an in-session producer-time anchor");
  check(delayed_export.find("delayed-start-remains-outside-ordinary-log-window") == std::string::npos,
        "producer-time anchoring widened the ordinary log UTC window");
  check(collect_session_recording_evidence(dir.path / "recordings", session, {}, delayed_snapshot, true)
            .at("evidence_status") == "incomplete", "relay delay hid actual terminal failure evidence");
  // Even a positive disk snapshot cannot erase explicit terminal negative
  // evidence for that same attempt, including equal/coarse wall timestamps.
  auto conflicting = recording_summary(session);
  conflicting["reported_at_utc_ms"] = finished_at;
  write(directory / "recording-session.status", conflicting.dump());
  check(collect_session_recording_evidence(dir.path / "recordings", session, {}, snapshot, true)
            .at("attempt_complete") == false, "terminal failure did not veto otherwise complete disk evidence");
  write(directory / "recording-session.status", summary.dump());
  // Logs are only negative evidence. A success event cannot upgrade stale disk state.
  auto success = finished; success["incomplete"] = false;
  write(snapshot, started.dump() + "\n" + success.dump() + "\n");
  check(collect_session_recording_evidence(dir.path / "recordings", session, {}, snapshot, true)
            .at("evidence_status") == "unknown", "success log fabricated persisted EOS proof");
}

void recording_evidence_scope_and_completion() {
  Directory dir; const auto session = metadata(now_ms());
  const auto directory = dir.path / "recordings" / "vehicle" / "session-000001";
  fs::create_directories(directory);
  const auto status_path = directory / "recording-session.status";
  const auto started = Json{{"event", "vehicle_recording_attempt_started"}, {"vehicle_id", "vehicle"},
      {"session_id", "session-000001"}, {"recording_attempt_id", "attempt-current"},
      {"reported_at_utc_ms", session.at("started_at_utc_ms")}, {"configured_enabled", true}, {"summary_persisted", false}};
  const auto tail = Json{{"event", "session-ended"}, {"vehicle_id", "vehicle"}, {"session_id", "session-000001"},
      {"logged_at_utc_ms", session.at("ended_at_utc_ms")}};
  write(dir.path / "runtime.log", started.dump() + "\n" + tail.dump() + "\n");
  const auto logs = collect_session_logs({{dir.path / "runtime.log", "runtime.log"}}, session, dir.path / "snapshot");
  check(logs.at("status") == "available", "recording attempt timestamp was not accepted by the production collector");
  const auto snapshot = dir.path / "snapshot/runtime.log";
  const auto read_evidence = [&] {return collect_session_recording_evidence(dir.path / "recordings", session, {"recording-secret"}, snapshot, true);};
  check(read_evidence().at("evidence_status") == "unknown", "missing recording summary reported complete");
  auto summary = recording_summary(session); write(status_path, summary.dump());
  const auto evidence = read_evidence();
  check(evidence.at("evidence_status") == "attempt_finalized" && evidence.at("complete").is_null() && evidence.at("attempt_complete") == true,
        "late final EOS summary did not distinguish current attempt completion from unknown session history");
  check(evidence.at("included") == false, "recording metadata claims the video is included");
  check(session_recording_evidence(summary, session).at("evidence_status") == "unknown", "summary without a current-attempt fence reported completion");
  check(collect_session_recording_evidence(dir.path / "recordings", session, {}, snapshot, false).at("evidence_status") == "unknown",
        "incomplete runtime coverage was used as a positive current-attempt fence");
  for (const auto* field : {"final_eos_received", "attempt_complete"}) {
    auto changed = summary; changed[field] = false; write(status_path, changed.dump());
    check(read_evidence().at("evidence_status") == "unknown", "unfinished recording reported complete");
  }
  for (const auto* field : {"unfinished_segments", "finalized_segments", "worker_input_access_units"}) {
    auto changed = summary; changed["cameras"][0]["recording"][field] = std::string(field) == "unfinished_segments" ? 1 : 0;
    write(status_path, changed.dump());check(read_evidence().at("evidence_status") == "unknown", "incomplete AU/segment coverage reported complete");
  }
  auto mismatched = summary; mismatched["cameras"][0]["recording"]["recording_attempt_id"] = "old-attempt";write(status_path, mismatched.dump());
  check(read_evidence().at("evidence_status") == "unknown", "old lane finalization was spliced into a new summary attempt");
  mismatched = summary; mismatched["session_id"] = "other-session";write(status_path, mismatched.dump());
  check(read_evidence().at("evidence_status") == "unknown", "another session's recording summary was exported as evidence");
  auto failed = summary; failed["incomplete"] = true; failed["state"] = "failed";
  failed["cameras"][0]["recording"]["incomplete"] = true;failed["cameras"][0]["recording"]["next_action"] = "credential=recording-secret";
  failed["password"] = "must-never-export";write(status_path, failed.dump());
  const auto gap = read_evidence();check(gap.at("evidence_status") == "incomplete" && gap.at("complete") == false, "failed recording did not retain a gap");
  check(gap.dump().find("recording-secret") == std::string::npos && gap.dump().find("must-never-export") == std::string::npos,
        "recording metadata leaked unrelated fields or credentials");
  write(status_path, summary.dump());
  auto newer = started;newer["recording_attempt_id"] = "attempt-new";newer["reported_at_utc_ms"] = session.at("started_at_utc_ms").get<std::int64_t>() + 1;
  write(snapshot, started.dump() + "\n" + newer.dump() + "\n");
  check(read_evidence().at("evidence_status") == "unknown", "an old healthy file survived a new ENOSPC attempt and was reported complete");
  const auto late_status = Json{{"event", "vehicle_media_status"}, {"vehicle_id", "vehicle"}, {"session_id", "session-000001"},
      {"status", {{"lanes", Json::array({{{"recording", {{"configured_enabled", true}, {"recording_attempt_id", "attempt-current"},
          {"reported_at_utc_ms", summary.at("reported_at_utc_ms")}, {"summary_persisted", false}, {"incomplete", false}, {"state", "finalized"}}}}})}}}};
  write(snapshot, started.dump() + "\n" + late_status.dump() + "\n");
  check(read_evidence().at("evidence_status") == "unknown", "a newer unpersisted state accepted an old healthy summary");
  write(status_path, std::string(65537, 'x'));check(read_evidence().at("evidence_status") == "unknown", "oversized recording summary was trusted");
  write(status_path, "not-json");check(read_evidence().at("evidence_status") == "unknown", "malformed recording summary was trusted");
  fs::remove(status_path);fs::create_symlink(dir.path / "runtime.log", status_path);
  check(read_evidence().at("evidence_status") == "unknown", "recording summary symlink was followed");
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
  SessionLogBroker completed(dir.path / "completed/audit.jsonl");
  const auto complete_id = completed.start(session, true).at("export_id").get<std::string>();
  for (const auto* name : {"runtime.log", "vcu-can.jsonl"})
    completed.vehicle("vehicle", {{"operation", "append"}, {"export_id", complete_id},
        {"chunk", {{"name", name}, {"offset", 0}, {"data_hex", "616263"}}}}, true);
  const Json manifest = {{"status", "available"}, {"files", {{{"name", "runtime.log"}, {"bytes", 3}},
      {{"name", "vcu-can.jsonl"}, {"bytes", 3}}}}};
  const Json finish = {{"operation", "finish"}, {"export_id", complete_id}, {"manifest", manifest}};
  completed.vehicle("vehicle", finish, true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!completed.status(complete_id, "driver").at("ready").get<bool>() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  check(completed.status(complete_id, "driver").at("ready").get<bool>(), "receipt fixture collection timed out");
  completed.release(complete_id, "driver");
  check(completed.vehicle("vehicle", finish, true).at("accepted").get<bool>(), "released export lost finish receipt");
  bool denied = false;
  try { completed.vehicle("other-vehicle", finish, true); } catch (...) { denied = true; }
  check(denied, "finish receipt crossed vehicle ownership");
  denied = false; auto conflicting = finish; conflicting["manifest"]["status"] = "partial";
  try { completed.vehicle("vehicle", conflicting, true); } catch (...) { denied = true; }
  check(denied, "finish receipt accepted changed manifest");
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
  const auto cameras = config.enabled_cameras(); check(cameras.size() == 6, "capture channel count");
  for (std::size_t i=0;i<cameras.size();++i) check(cameras[i].backend == "ccg2" &&
      cameras[i].device == "/dev/ccg2-channel-" + std::to_string(i) && cameras[i].capture_width == 1920 &&
      cameras[i].capture_height == 1080 && cameras[i].capture_fps == 30 && cameras[i].critical_for_control,
      "invalid default CCG2 input");
}

void long_log_redaction() {
  Directory dir; const auto start = now_ms();
  const auto secret = std::string(300000, 'y');
  const auto plain = std::string(300000, 'x');
  std::string records;
  for (const auto& message : {plain, "Authorization: Bearer " + secret,
       "?device_token=" + secret + "&keep=ok", "{\"PASSWORD\":\"" + secret + "\"}",
       std::string(20000, 'a') + "_token=" + secret})
    records += Json{{"session_id", "session-000001"}, {"logged_at_utc_ms", start}, {"message", message}}.dump() + "\n";
  write(dir.path / "runtime.log", records);
  collect_session_logs({{dir.path / "runtime.log", "runtime.log"}}, metadata(start), dir.path / "snapshot");
  const auto text = read(dir.path / "snapshot/runtime.log");
  check(text.find(plain) != std::string::npos, "long noncredential text lost");
  check(text.find(secret) == std::string::npos && text.find("[redacted]") != std::string::npos,
        "long credential leaked");
  check(text.find("&keep=ok") != std::string::npos, "credential redaction consumed adjacent parameter");
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
  std::atomic<bool> finish_acknowledged{false};
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
        finish_acknowledged = response.status == 200;
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
  const auto online = http.post_json_response(origin + "/vehicles/online",
      {{"vehicle_id", "vehicle"}, {"device_token", "device-secret"}, {"connection_id", "test"}});
  const auto vehicle_generation = online.at("connection_generation").get<std::uint64_t>();
  std::atomic<bool> heartbeat_failed{false};
  // The export worker does not own vehicle presence. Model the separate
  // runtime heartbeat so slow retries cannot turn this into an offline test.
  std::jthread heartbeat([&](std::stop_token stop) {
    HttpClient keepalive_http(std::chrono::seconds(2));
    while (!stop.stop_requested()) {
      try {
        static_cast<void>(keepalive_http.post_json_response(origin + "/vehicles/heartbeat",
            {{"vehicle_id", "vehicle"}, {"device_token", "device-secret"},
             {"connection_generation", vehicle_generation}}));
      } catch (const std::exception&) {
        heartbeat_failed = true;
        return;
      }
      for (int tick = 0; tick < 10 && !stop.stop_requested(); ++tick)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
  });
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
  vehicle.recording.root_dir = dir.path / "recordings";
  const auto recording_directory = vehicle.recording.root_dir / "vehicle" / id;fs::create_directories(recording_directory);
  auto recording = recording_summary(records[0]);recording["incomplete"] = true;recording["state"] = "failed";
  write(recording_directory / "recording-session.status", recording.dump());
  auto worker = start_vehicle_log_worker(vehicle, "device-secret");
  ControllerLogExport exported(origin, {}, {}, "driver", token, key.get<std::string>(), dir.path / "browser.jsonl");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(18);
  // The cloud exposes a ready ZIP before the vehicle retries its lost finish
  // response. Wait for both independent completions before asserting retries.
  while ((exported.status().at("state") == "collecting" || !finish_acknowledged) &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto status = exported.status();
  check(status.at("state") == "ready", status.dump().c_str());
  check(status.at("logs_complete") == status.at("complete") && status.at("complete_scope") == "logs_only", "recording gap changed or obscured log completeness");
  check(status.at("recording").at("included") == false && status.at("recording").at("evidence_status") == "incomplete" &&
        status.at("manifest").at("recording").at("complete") == false, "production vehicle worker/export did not preserve failed recording evidence");
  check(append_attempts >= 5 && finish_attempts == 2 && finish_acknowledged,
        "transient failures did not retry append and finish");
  const auto zip = exported.zip();
  for (const auto* required : {"controller/control-browser-events.jsonl", "cloud/signaling-audit.jsonl",
                               "vehicle/runtime.log", "vehicle/vcu-can.jsonl", "manifest.json", "test-CAN-evidence"})
    check(zip.find(required) != std::string::npos, "three-endpoint ZIP omitted source");
  check(zip.find("must-redact") == std::string::npos && zip.find("device-secret") == std::string::npos, "ZIP leaked credential");
  check(zip.find(".mp4") == std::string::npos, "diagnostic export included video content");
  if (const char* output = std::getenv("MINE_TELEOP_TEST_EXPORT_ZIP")) write(output, zip);
  permanent_upload_failure = true;
  ControllerLogExport failed(origin, {}, {}, "driver", token, key.get<std::string>(), dir.path / "browser.jsonl");
  // Three upload attempts can each take 5s, in addition to polling, backoff,
  // and collecting the partial ZIP. This checks completion, not a latency SLA.
  const auto failed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  while (failed.status().at("state") == "collecting" && std::chrono::steady_clock::now() < failed_deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const auto failure = failed.status();
  check(failure.at("state") == "ready" && !failure.at("complete").get<bool>(),
        ("exhausted upload retry did not finish as partial: " + failure.dump()).c_str());
  check(failure.at("manifest").at("vehicle").at("reason") == "vehicle_upload_failed",
        ("worker swallowed upload failure: " + failure.dump()).c_str());
  worker.request_stop(); worker.join();
  // A pending vehicle upload must not turn a desktop close into a 3-minute join.
  auto pending = std::make_unique<ControllerLogExport>(origin, std::vector<std::string>{}, fs::path{},
      "driver", token, key.get<std::string>(), dir.path / "browser.jsonl");
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  check(pending->status().at("state") == "collecting", "export was not pending");
  const auto cancelled_at = std::chrono::steady_clock::now(); pending.reset();
  check(std::chrono::steady_clock::now() - cancelled_at < std::chrono::seconds(3), "export cancellation blocked shutdown");
  heartbeat.request_stop(); heartbeat.join();
  check(!heartbeat_failed, "fixture lost authenticated vehicle heartbeat");
  server.stop();
}
}
int main() {
  try { package_configuration(); collection_and_zip(); recording_terminal_failure_after_session_end(); recording_evidence_scope_and_completion(); trace_batch_session_filter(); vehicle_export_failure_reporting(); session_window_scan_priority(); long_log_redaction(); three_endpoint_export(); std::cout << "session_log_export_tests=passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
