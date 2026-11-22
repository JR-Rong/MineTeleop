#include "mine_teleop/recording.hpp"
#include "mine_teleop/detail/recording_transport.hpp"

#include <gst/app/gstappsrc.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iomanip>
#include <memory>
#include <poll.h>
#include <set>
#include <stdexcept>
#include <thread>
#include <unistd.h>

namespace mine_teleop {
namespace {
using detail::RecordingWireHeader;
using detail::RecordingPacketType;
std::int64_t worker_utc_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
void read_exact(int fd, void* destination, std::size_t bytes) {
  auto* pointer = static_cast<std::uint8_t*>(destination);
  for (std::size_t count = 0; count < bytes;) {
    const auto read = ::read(fd, pointer + count, bytes - count);
    if (read < 0 && errno == EINTR) continue;
    if (read <= 0) throw std::runtime_error("recording IPC ended before complete packet/EOS");
    count += static_cast<std::size_t>(read);
  }
}
RecordingWireHeader read_header(int fd) {
  RecordingWireHeader value{};
  read_exact(fd, &value, sizeof(value));
  if (value.magic != detail::kRecordingWireMagic || value.version != detail::kRecordingWireVersion)
    throw std::runtime_error("recording IPC version/magic mismatch");
  return value;
}
std::string read_payload(int fd, std::size_t count, std::size_t limit) {
  if (count > limit) throw std::runtime_error("recording IPC payload exceeds bound");
  std::string value(count, '\0');
  read_exact(fd, value.data(), value.size());
  return value;
}
void validate_component(const std::string& value) {
  if (value.empty() || value.size() > 128 || value == "." || value == ".." ||
      !std::all_of(value.begin(), value.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '_' || character == '-' || character == '.';
      })) throw std::runtime_error("unsafe recording identity/path component");
}
void write_json(const std::filesystem::path& path, const Json& value) {
  const auto text = value.dump(2);
  if (text.size() > detail::kRecordingConfigByteLimit) throw std::runtime_error("recording metadata exceeds bound");
  auto temporary = path; temporary += ".tmp";
  {
    std::ofstream output(temporary, std::ios::trunc);
    if (!output) throw std::runtime_error("recording metadata open failed");
    output << text << '\n'; output.flush();
    if (!output) throw std::runtime_error("recording metadata write failed");
  }
  std::filesystem::rename(temporary, path);
}
using StatusDeadline = std::chrono::steady_clock::time_point;
void send_status(int fd, const Json& value, StatusDeadline deadline = {}) {
  const auto text = value.dump() + '\n';
  const auto pipe_bound = ::fpathconf(fd, _PC_PIPE_BUF);
  if (pipe_bound <= 0 || text.size() > std::min<std::size_t>(4096, static_cast<std::size_t>(pipe_bound)))
    throw std::runtime_error("recording status message exceeds atomic pipe bound");
  while (true) {
    const auto written = ::write(fd, text.data(), text.size());
    if (written == static_cast<ssize_t>(text.size())) return;
    if (written < 0 && errno == EINTR) continue;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // Periodic snapshots may be coalesced; terminal snapshots share one
      // bounded delivery budget and are never silently discarded on exit.
      if (deadline == StatusDeadline{}) return;
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("recording terminal status unconfirmed");
      pollfd waiter{fd, POLLOUT, 0};
      static_cast<void>(::poll(&waiter, 1, 5));
      continue;
    }
    throw std::runtime_error("recording status IPC failed");
  }
}
struct WorkerLane {
  std::string id, codec, encoder;
  GstElement* pipeline{nullptr};
  GstElement* source{nullptr};
  std::filesystem::path directory;
  std::set<std::string> pending;
  std::string last_closed;
  std::uint64_t input{0}, accepted{0}, finalized{0}, revision{0};
  bool ended{false}, bus_eos{false};
  ~WorkerLane() {
    if (pipeline) gst_element_set_state(pipeline, GST_STATE_NULL); // Only this disposable child may wait on writer I/O.
    if (source) gst_object_unref(source);
    if (pipeline) gst_object_unref(pipeline);
  }
};
class Worker {
 public:
  Worker(Json next, int status_fd, RecordingWorkerTestHooks test_hooks)
      : config(std::move(next)), status_fd(status_fd), hooks(test_hooks) {
    vehicle = config.at("vehicle_id").get<std::string>(); session = config.at("session_id").get<std::string>();
    attempt = config.at("recording_attempt_id").get<std::string>();
    validate_component(vehicle); validate_component(session); validate_component(attempt);
    const auto root = std::filesystem::path(config.at("root").get<std::string>());
    if (root.empty()) throw std::runtime_error("empty recording root");
    session_root = root / vehicle / session;
    incomplete = config.value("incomplete_before", false);
    std::filesystem::create_directories(session_root);
    const auto old_path = session_root / "recording-session.status";
    if (std::filesystem::is_regular_file(old_path) && std::filesystem::file_size(old_path) <= detail::kRecordingConfigByteLimit) {
      std::ifstream old(old_path); Json value; old >> value;
      if (value.value("schema_version", 0) == 1 && value.value("kind", "") == "recording_session_summary" &&
          value.value("vehicle_id", "") == vehicle && value.value("session_id", "") == session) {
        incomplete = incomplete || value.value("incomplete", true) || !value.value("final_eos_received", false);
        summary_revision = value.value("summary_revision", std::uint64_t{0});
      } else incomplete = true;
    }
    const auto& cameras = config.at("cameras");
    if (!cameras.is_array() || cameras.empty() || cameras.size() > detail::kRecordingLaneLimit) throw std::runtime_error("invalid recording lane count");
    std::set<std::string> ids;
    for (const auto& camera : cameras) {
      auto lane = std::make_unique<WorkerLane>();
      lane->id = camera.at("camera_id").get<std::string>(); validate_component(lane->id);
      if (!ids.insert(lane->id).second) throw std::runtime_error("duplicate recording camera");
      lane->codec = camera.at("codec").get<std::string>(); lane->encoder = camera.value("encoder", "");
      if (lane->codec != "h264" && lane->codec != "h265") throw std::runtime_error("unsupported recording codec");
      const auto fps = camera.at("fps").get<int>(), seconds = camera.at("segment_seconds").get<int>();
      if (fps <= 0 || fps > 120 || seconds <= 0 || seconds > 3600) throw std::runtime_error("invalid recording profile");
      lane->directory = session_root / lane->id; std::filesystem::create_directories(lane->directory);
      const auto parser = lane->codec + "parse";
      const auto format = lane->codec == "h265" ? "hvc1" : "avc";
      const auto description = "appsrc name=input is-live=true format=time handle-segment-change=true block=true max-buffers=2 max-bytes=" +
          std::to_string(detail::kRecordingLaneByteLimit) + " max-time=0 leaky-type=none caps=video/x-" + lane->codec +
          ",stream-format=byte-stream,alignment=au ! " + parser + " config-interval=-1 ! video/x-" + lane->codec +
          ",stream-format=" + format + ",alignment=au ! splitmuxsink name=recorder async-finalize=false max-size-time=" +
          std::to_string(static_cast<std::uint64_t>(seconds) * GST_SECOND);
      GError* error = nullptr;
      lane->pipeline = gst_parse_launch(description.c_str(), &error);
      const auto message = error == nullptr ? "" : std::string(error->message);
      if (error) g_error_free(error);
      if (!lane->pipeline || !message.empty()) throw std::runtime_error("recording pipeline parse failed: " + message);
      lane->source = gst_bin_get_by_name(GST_BIN(lane->pipeline), "input");
      auto* recorder = gst_bin_get_by_name(GST_BIN(lane->pipeline), "recorder");
      auto* sink = gst_element_factory_make("filesink", nullptr);
      if (!lane->source || !recorder || !sink) throw std::runtime_error("recording pipeline elements missing");
      const auto pattern = (lane->directory / (std::to_string(worker_utc_ms()) + "_" + attempt + "_%05d.mp4.partial")).string();
      bool sink_finalized = false;
      const auto finalized_observer = +[](gpointer data, GObject*) { *static_cast<bool*>(data) = true; };
      const bool sink_was_floating = hooks.emit_file_diagnostics && g_object_is_floating(sink);
      // Keep a caller-owned reference: splitmuxsink's sink setter consumes a
      // floating factory reference, so the later caller unref must be separate.
      gst_object_ref_sink(sink);
      if (hooks.emit_file_diagnostics) g_object_weak_ref(G_OBJECT(sink), finalized_observer, &sink_finalized);
      g_object_set(recorder, "sink", sink, "location", hooks.disk_full ? "/dev/full" : pattern.c_str(), nullptr);
      const auto sink_refs_after_property = hooks.emit_file_diagnostics ? GST_OBJECT_REFCOUNT_VALUE(sink) : 0;
      if (hooks.stall_write_ms > 0 || hooks.stall_finalize_ms > 0) {
        auto* pad = gst_element_get_static_pad(sink, "sink");
        gst_pad_add_probe(pad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          const auto& configured = *static_cast<RecordingWorkerTestHooks*>(data);
          const auto duration = (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0 ? configured.stall_write_ms :
              (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS ? configured.stall_finalize_ms : 0);
          if (duration > 0) std::this_thread::sleep_for(std::chrono::milliseconds(duration));
          return GST_PAD_PROBE_OK;
        }, &hooks, nullptr);
        gst_object_unref(pad);
      }
      gst_object_unref(sink); gst_object_unref(recorder);
      if (hooks.emit_file_diagnostics) {
        if (!sink_finalized) g_object_weak_unref(G_OBJECT(sink), finalized_observer, &sink_finalized);
        test_diagnostic([&] { return Json{{"event", "provided_sink_ownership"}, {"camera_id", lane->id},
          {"factory_floating", sink_was_floating}, {"refs_after_property", sink_refs_after_property},
          {"finalized_after_caller_unref", sink_finalized}}; });
      }
      if (gst_element_set_state(lane->pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
        if (hooks.emit_file_diagnostics) {
          auto* bus = gst_element_get_bus(lane->pipeline);
          while (auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR)) {
            GError* failure = nullptr; gchar* debug = nullptr;
            gst_message_parse_error(message, &failure, &debug);
            test_diagnostic([&] { return Json{{"event", "startup_gst_bus_error"}, {"camera_id", lane->id},
              {"error", failure ? std::string(failure->message).substr(0, 1024) : ""},
              {"debug", debug ? std::string(debug).substr(0, 2048) : ""},
              {"source", GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : ""}}; });
            if (failure) g_error_free(failure); g_free(debug); gst_message_unref(message);
          }
          gst_object_unref(bus);
        }
        throw std::runtime_error("recording pipeline failed to play");
      }
      lanes.push_back(std::move(lane));
    }
    publish("starting", false);
  }
  Json lane_status(const WorkerLane& lane, const std::string& state, bool eos) const {
    return {{"schema_version", 1}, {"configured_enabled", true}, {"active", state == "recording"}, {"state", state}, {"incomplete", incomplete},
      {"issue_code", issue}, {"next_action", issue.empty() ? (eos ? "This attempt finalized; earlier session completeness is not implied." : "Recording status requires finalized segment evidence.") : (issue == "recording_status_unconfirmed" ? "Finalization evidence is unconfirmed; preserve clips and inspect recorder status." : "Recording is incomplete; inspect storage/recorder health. Live video continues.")},
      {"recording_attempt_id", attempt}, {"status_revision", lane.revision}, {"accepted_access_units", lane.accepted}, {"worker_input_access_units", lane.input},
      {"finalized_segments", lane.finalized}, {"unfinished_segments", lane.pending.size()}, {"final_eos_received", eos},
      {"cleanup_pending", false}, {"summary_persisted", persisted}, {"summary_revision", summary_revision}, {"reported_at_utc_ms", worker_utc_ms()}};
  }
  template <typename Build>
  void test_diagnostic(Build&& build) const noexcept {
    if (!hooks.emit_file_diagnostics) return;
    try {
      auto value = build(); // Synchronous; all JSON allocation stays inside this guard.
      value["recording_attempt_id"] = attempt;
      value["reported_at_utc_ms"] = worker_utc_ms();
      Json cameras = Json::array();
      for (const auto& lane : lanes) cameras.push_back({{"camera_id", lane->id}, {"input", lane->input},
        {"accepted", lane->accepted}, {"finalized", lane->finalized}, {"pending", lane->pending},
        {"ended", lane->ended}, {"bus_eos", lane->bus_eos}});
      value["cameras"] = std::move(cameras);
      std::ofstream output(session_root / ("recording-worker-" + attempt + ".diagnostic.jsonl"), std::ios::app);
      output << value.dump() << '\n';
    } catch (...) {} // Test evidence must never change the recorder result.
  }
  void publish(const std::string& state, bool eos) {
    ++summary_revision; persisted = false;
    Json cameras = Json::array(), expected = Json::array();
    for (auto& lane : lanes) { ++lane->revision; expected.push_back(lane->id); cameras.push_back({{"camera_id", lane->id}, {"recording", lane_status(*lane, state, eos)}}); }
    Json summary{{"schema_version", 1}, {"kind", "recording_session_summary"}, {"upload_state", "not_applicable"}, {"vehicle_id", vehicle}, {"session_id", session},
      {"recording_attempt_id", attempt}, {"configured_enabled", true}, {"state", state}, {"incomplete", incomplete}, {"expected_cameras", expected},
      {"final_eos_received", eos}, {"summary_revision", summary_revision}, {"reported_at_utc_ms", worker_utc_ms()},
      {"completeness_scope", "current_runtime_known_attempts"}, {"session_history_known", config.value("session_history_known", false)}, {"attempt_complete", eos},
      {"completed_at_utc_ms", eos ? Json(worker_utc_ms()) : Json(nullptr)}, {"cameras", cameras}};
    for (auto& camera : summary["cameras"]) camera["recording"]["summary_persisted"] = true;
    try { write_json(session_root / "recording-session.status", summary); }
    catch (...) { issue = "recording_summary_write_failed"; throw; }
    persisted = true;
    const auto deadline = state == "finalized" || state == "finalizing" || state == "failed" ?
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500) : StatusDeadline{};
    try {
    for (const auto& lane : lanes) send_status(status_fd, {{"recording_attempt_id", attempt}, {"camera_id", lane->id}, {"recording", lane_status(*lane, state, eos)}}, deadline);
    send_status(status_fd, {{"recording_attempt_id", attempt}, {"state", state}, {"incomplete", incomplete}, {"issue_code", issue}, {"summary_revision", summary_revision}, {"summary_persisted", true}, {"reported_at_utc_ms", worker_utc_ms()}}, deadline);
    } catch (...) { issue = "recording_status_unconfirmed"; throw; }
  }
  void poll_lane(WorkerLane& lane, bool& eos) {
    auto* bus = gst_element_get_bus(lane.pipeline);
    while (auto* message = gst_bus_pop(bus)) {
      const auto type = GST_MESSAGE_TYPE(message);
      if (type == GST_MESSAGE_ERROR) {
        GError* error = nullptr; gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        const bool full = error && error->domain == GST_RESOURCE_ERROR && error->code == GST_RESOURCE_ERROR_NO_SPACE_LEFT;
        const auto text = error ? std::string(error->message) : "recording pipeline error";
        test_diagnostic([&] { return Json{{"event", "gst_bus_error"}, {"camera_id", lane.id},
          {"error", text.substr(0, 1024)}, {"debug", debug ? std::string(debug).substr(0, 2048) : ""},
          {"source", GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : ""}}; });
        if (error) g_error_free(error); g_free(debug); gst_message_unref(message); gst_object_unref(bus);
        issue = full ? "recording_disk_full" : "recording_write_failed";
        throw std::runtime_error(text);
      }
      if (type == GST_MESSAGE_EOS) { eos = true; lane.bus_eos = true; }
      if (type == GST_MESSAGE_ELEMENT) {
        const auto* structure = gst_message_get_structure(message);
        if (structure && (gst_structure_has_name(structure, "splitmuxsink-fragment-opened") || gst_structure_has_name(structure, "splitmuxsink-fragment-closed"))) {
          const auto* location = gst_structure_get_string(structure, "location");
          if (location && !hooks.disk_full) {
            const std::string name(location);
            test_diagnostic([&] { return Json{{"event", gst_structure_get_name(structure)}, {"camera_id", lane.id}, {"location", name}}; });
            if (gst_structure_has_name(structure, "splitmuxsink-fragment-opened")) {
              lane.pending.insert(name);
              if (lane.pending.size() > 2) throw std::runtime_error("recording pending fragments exceed bound");
            }
            else {
              if (name == lane.last_closed) { gst_message_unref(message); continue; }
              if (lane.pending.count(name) != 1) throw std::runtime_error("closed recording fragment lacks open evidence");
              auto temporary = std::filesystem::path(name);
              if (temporary.parent_path() != lane.directory || temporary.extension() != ".partial") throw std::runtime_error("unexpected recording fragment path");
              auto completed = temporary; completed.replace_extension();
              const Json metadata{{"schema_version", 1}, {"kind", "recording_segment"}, {"recording_status", "complete"}, {"finalized", true}, {"incomplete", false},
                {"vehicle_id", vehicle}, {"session_id", session}, {"camera_id", lane.id}, {"segment_id", completed.stem().string()}, {"recording_attempt_id", attempt},
                {"codec", lane.codec}, {"encoder", lane.encoder}, {"upload_state", "pending"}, {"video_file", completed.filename().string()},
                {"recording_time_scope", "encoded_stream_running_time_not_sensor_utc"},
                {"file_size_bytes", std::filesystem::file_size(temporary)}, {"hash_state", "pending"},
                {"integrity_scope", "fragment_closed_before_archive_digest"}};
              std::filesystem::rename(temporary, completed);
              auto sidecar = completed; sidecar.replace_extension(".json");
              write_json(sidecar, metadata);
              lane.pending.erase(name); lane.last_closed = name; ++lane.finalized;
            }
          }
        }
      }
      gst_message_unref(message);
    }
    gst_object_unref(bus);
  }
  void accept(const RecordingWireHeader& header, std::string payload) {
    if (header.lane >= lanes.size()) throw std::runtime_error("recording packet has unknown camera index");
    auto& lane = *lanes[header.lane];
    if (lane.ended) throw std::runtime_error("recording packet after lane EOS");
    if (lane.input == 0) test_diagnostic([&] { return Json{{"event", "first_access_unit"}, {"camera_id", lane.id},
      {"pts", header.pts}, {"dts", header.dts}, {"duration", header.duration}, {"flags", header.buffer_flags},
      {"segment_start", header.start}, {"segment_time", header.time}, {"segment_base", header.base}}; });
    if (!std::isfinite(header.rate) || header.rate <= 0 || !std::isfinite(header.applied_rate) || header.applied_rate <= 0)
      throw std::runtime_error("invalid recording timestamp segment");
    GstSegment segment; gst_segment_init(&segment, GST_FORMAT_TIME);
    segment.flags = static_cast<GstSegmentFlags>(header.segment_flags); segment.rate = header.rate; segment.applied_rate = header.applied_rate;
    segment.start = header.start; segment.stop = header.stop; segment.time = header.time; segment.base = header.base;
    segment.offset = header.offset; segment.position = header.position; segment.duration = header.segment_duration;
    auto* buffer = gst_buffer_new_allocate(nullptr, payload.size(), nullptr);
    gst_buffer_fill(buffer, 0, payload.data(), payload.size());
    GST_BUFFER_PTS(buffer) = header.pts; GST_BUFFER_DTS(buffer) = header.dts; GST_BUFFER_DURATION(buffer) = header.duration;
    GST_BUFFER_FLAGS(buffer) = static_cast<GstBufferFlags>(header.buffer_flags);
    auto* sample = gst_sample_new(buffer, nullptr, &segment, nullptr);
    gst_buffer_unref(buffer);
    const auto flow = gst_app_src_push_sample(GST_APP_SRC(lane.source), sample); gst_sample_unref(sample);
    bool ignored = false; poll_lane(lane, ignored);
    if (flow != GST_FLOW_OK) throw std::runtime_error("recording appsrc failed");
    ++lane.input; lane.accepted = lane.input;
    if (hooks.exit_after_access_units != 0 && lane.input >= hooks.exit_after_access_units) ::_exit(20);
    const auto now = worker_utc_ms();
    if (now - last_report >= 1000) { publish("recording", false); last_report = now; }
  }
  bool end(const RecordingWireHeader& header) {
    if (header.lane >= lanes.size()) throw std::runtime_error("recording EOS unknown camera index");
    auto& lane = *lanes[header.lane];
    test_diagnostic([&] { return Json{{"event", "lane_end_received"}, {"camera_id", lane.id}, {"expected_access_units", header.pts}}; });
    if (lane.ended || lane.input != header.pts || lane.input == 0) throw std::runtime_error("recording EOS does not cover every accepted AU");
    lane.accepted = header.pts; lane.ended = true;
    if (gst_app_src_end_of_stream(GST_APP_SRC(lane.source)) != GST_FLOW_OK) throw std::runtime_error("recording appsrc EOS failed");
    if (!std::all_of(lanes.begin(), lanes.end(), [](const auto& item) { return item->ended; })) return false;
    publish("finalizing", false);
    std::vector<bool> eos(lanes.size(), false);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!std::all_of(eos.begin(), eos.end(), [](bool value) { return value; })) {
      for (std::size_t index = 0; index < lanes.size(); ++index) { bool reached = lanes[index]->bus_eos; poll_lane(*lanes[index], reached); eos[index] = reached; }
      if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("recording final EOS timeout");
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (const auto& item : lanes) if (item->finalized == 0 || !item->pending.empty()) throw std::runtime_error("recording EOS lacks closed-fragment evidence");
    publish("finalized", true);
    test_diagnostic([&] { return Json{{"event", "worker_finalized"}}; });
    return true;
  }
  void failed(const std::string& error) noexcept {
    test_diagnostic([&] { return Json{{"event", "worker_failed"}, {"error", error.substr(0, 2048)}, {"issue_code", issue}}; });
    incomplete = true; if (issue.empty()) issue = "recording_worker_failed";
    try { publish("failed", false); }
    catch (const std::exception&) {
      try { send_status(status_fd, {{"recording_attempt_id", attempt}, {"state", "failed"}, {"incomplete", true}, {"summary_persisted", false}, {"issue_code", issue}, {"error", error.substr(0, 256)}, {"reported_at_utc_ms", worker_utc_ms()}}, std::chrono::steady_clock::now() + std::chrono::milliseconds(500)); }
      catch (const std::exception&) {}
    }
  }
 private:
  Json config;
  int status_fd;
  RecordingWorkerTestHooks hooks;
  std::string vehicle, session, attempt, issue;
  std::filesystem::path session_root;
  std::vector<std::unique_ptr<WorkerLane>> lanes;
  std::uint64_t summary_revision{0};
  std::int64_t last_report{0};
  bool incomplete{false}, persisted{false};
};
} // namespace

int run_recording_worker(int input_fd, int status_fd, RecordingWorkerTestHooks hooks) {
  // Registry/plugin scanner execs must not retain the parent's IPC endpoints.
  if (::fcntl(input_fd, F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(status_fd, F_SETFD, FD_CLOEXEC) < 0 ||
      ::fcntl(status_fd, F_SETFL, ::fcntl(status_fd, F_GETFL) | O_NONBLOCK) < 0) return 2;
  std::unique_ptr<Worker> worker;
  std::string attempt;
  std::filesystem::path test_trace;
  try {
    const auto start = read_header(input_fd);
    if (start.type != RecordingPacketType::Start) throw std::runtime_error("recording worker requires startup packet");
    const auto config = Json::parse(read_payload(input_fd, start.bytes, detail::kRecordingConfigByteLimit));
    attempt = config.at("recording_attempt_id").get<std::string>();
    if (hooks.emit_file_diagnostics) {
      const auto vehicle = config.at("vehicle_id").get<std::string>();
      const auto session = config.at("session_id").get<std::string>();
      validate_component(vehicle); validate_component(session); validate_component(attempt);
      const auto directory = std::filesystem::path(config.at("root").get<std::string>()) / vehicle / session;
      std::filesystem::create_directories(directory);
      test_trace = directory / ("recording-worker-" + attempt + ".diagnostic.jsonl");
    }
    gst_init(nullptr, nullptr);
    worker = std::make_unique<Worker>(config, status_fd, hooks);
    while (true) {
      const auto header = read_header(input_fd);
      if (header.type == RecordingPacketType::AccessUnit && header.bytes != 0) worker->accept(header, read_payload(input_fd, header.bytes, detail::kRecordingLaneByteLimit));
      else if (header.type == RecordingPacketType::End && header.bytes == 0) { if (worker->end(header)) return 0; }
      else throw std::runtime_error("unexpected recording packet");
    }
  } catch (const std::exception& error) {
    if (worker) worker->failed(error.what());
    else if (!attempt.empty()) {
      if (!test_trace.empty()) {
        try { std::ofstream output(test_trace, std::ios::app); output << Json({{"event", "worker_start_failed"},
          {"recording_attempt_id", attempt}, {"error", std::string(error.what()).substr(0, 2048)}}).dump() << '\n'; }
        catch (...) {}
      }
      try { send_status(status_fd, {{"recording_attempt_id", attempt}, {"state", "failed"}, {"incomplete", true}, {"summary_persisted", false}, {"issue_code", "recording_start_failed"}, {"error", std::string(error.what()).substr(0,256)}, {"reported_at_utc_ms", worker_utc_ms()}}, std::chrono::steady_clock::now() + std::chrono::milliseconds(500)); }
      catch (...) {}
    }
    return 1;
  }
}
} // namespace mine_teleop
