#include "mine_teleop/recording.hpp"
#include "mine_teleop/detail/recording_transport.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace mine_teleop {
namespace {
using Clock = std::chrono::steady_clock;
using detail::RecordingPacketType;
using detail::RecordingWireHeader;
std::int64_t steady_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch()).count(); }
constexpr int kFinishBudgetMs = 8000;
std::mutex lease_mutex;
pid_t leased_child = 0;
bool lease_active = false;
// One live recorder process per media-service process, also across reconnects.
// A SIGKILL-pending kernel task cannot be reclaimed by a bounded user-space
// wait. Retain its PID and refuse another spawn; never detach a waiter thread.
bool claim_worker() {
  std::lock_guard lock(lease_mutex);
  if (leased_child > 0 && !lease_active) {
    int status = 0;
    const auto result = ::waitpid(leased_child, &status, WNOHANG);
    if (result == leased_child || (result < 0 && errno == ECHILD)) leased_child = 0;
  }
  if (lease_active || leased_child > 0) return false;
  lease_active = true;
  return true;
}
void publish_child(pid_t child) { std::lock_guard lock(lease_mutex); leased_child = child; }
void release_worker(bool reaped) {
  std::lock_guard lock(lease_mutex);
  lease_active = false;
  if (reaped) leased_child = 0;
}
std::filesystem::path sibling_worker() {
#ifdef __APPLE__
  std::uint32_t count = 0;
  _NSGetExecutablePath(nullptr, &count);
  std::vector<char> path(count + 1);
  if (_NSGetExecutablePath(path.data(), &count) != 0) throw std::runtime_error("cannot locate recorder sibling");
  return std::filesystem::weakly_canonical(path.data()).parent_path() / "mine-teleop-recorder";
#else
  std::array<char, 4096> path{};
  const auto count = ::readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (count <= 0 || static_cast<std::size_t>(count) >= path.size() - 1) throw std::runtime_error("cannot locate recorder sibling");
  return std::filesystem::path(std::string(path.data(), static_cast<std::size_t>(count))).parent_path() / "mine-teleop-recorder";
#endif
}
void nonblocking(int fd) {
  const auto flags = ::fcntl(fd, F_GETFL);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0)
    throw std::runtime_error("cannot configure recorder IPC");
#ifdef __APPLE__
  int enabled = 1;
  static_cast<void>(::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)));
#endif
}
pid_t spawn_worker(const std::filesystem::path& executable, int input, int status) {
  // Source FDs are moved high before dup2, so fd 3/4 crossings cannot clobber
  // either original. Neither credentials/environment nor network/CAN FDs leak.
  const int child_input = ::fcntl(input, F_DUPFD_CLOEXEC, 10);
  const int child_status = ::fcntl(status, F_DUPFD_CLOEXEC, 10);
  if (child_input < 0 || child_status < 0) {
    if (child_input >= 0) ::close(child_input);
    if (child_status >= 0) ::close(child_status);
    throw std::runtime_error("cannot duplicate recorder IPC");
  }
  const auto checked = [](int code) { if (code != 0) throw std::runtime_error("secure recorder spawn setup failed: " + std::string(std::strerror(code))); };
  struct Setup {
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
    bool actions_ready{false}, attributes_ready{false};
    int input, status;
    ~Setup() {
      if (attributes_ready) ::posix_spawnattr_destroy(&attributes);
      if (actions_ready) ::posix_spawn_file_actions_destroy(&actions);
      ::close(input); ::close(status);
    }
  } setup{{}, {}, false, false, child_input, child_status};
  checked(::posix_spawn_file_actions_init(&setup.actions)); setup.actions_ready = true;
  checked(::posix_spawnattr_init(&setup.attributes)); setup.attributes_ready = true;
  checked(::posix_spawn_file_actions_addopen(&setup.actions, 0, "/dev/null", O_RDONLY, 0));
  checked(::posix_spawn_file_actions_addopen(&setup.actions, 1, "/dev/null", O_WRONLY, 0));
  checked(::posix_spawn_file_actions_addopen(&setup.actions, 2, "/dev/null", O_WRONLY, 0));
  checked(::posix_spawn_file_actions_adddup2(&setup.actions, child_input, 3));
  checked(::posix_spawn_file_actions_adddup2(&setup.actions, child_status, 4));
  sigset_t mask, defaults;
  sigemptyset(&mask); sigemptyset(&defaults);
  sigaddset(&defaults, SIGTERM); sigaddset(&defaults, SIGINT); sigaddset(&defaults, SIGPIPE);
  checked(::posix_spawnattr_setsigmask(&setup.attributes, &mask));
  checked(::posix_spawnattr_setsigdefault(&setup.attributes, &defaults));
  short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
  flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#else
#if defined(__GLIBC__) && __GLIBC_PREREQ(2, 34)
  checked(::posix_spawn_file_actions_addclosefrom_np(&setup.actions, 5));
#else
  throw std::runtime_error("secure recorder requires atomic spawn closefrom support");
#endif
#endif
  checked(::posix_spawnattr_setflags(&setup.attributes, flags));
  std::vector<std::string> environment{"LANG=C", "GST_REGISTRY_FORK=no"};
  for (const auto* key : {"LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "GST_PLUGIN_PATH", "GST_PLUGIN_PATH_1_0", "GST_PLUGIN_SYSTEM_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0", "GST_REGISTRY", "GST_REGISTRY_1_0", "GST_PLUGIN_SCANNER", "GST_PLUGIN_SCANNER_1_0"}) {
    if (const auto* value = ::getenv(key); value != nullptr) environment.push_back(std::string(key) + "=" + value);
  }
  std::vector<char*> env;
  for (auto& value : environment) env.push_back(value.data());
  env.push_back(nullptr);
  auto name = executable.string();
  std::array<char*, 6> arguments{name.data(), const_cast<char*>("--ipc-in-fd"), const_cast<char*>("3"), const_cast<char*>("--ipc-status-fd"), const_cast<char*>("4"), nullptr};
  pid_t child = 0;
  const auto result = ::posix_spawn(&child, name.c_str(), &setup.actions, &setup.attributes, arguments.data(), env.data());
  if (result != 0) throw std::runtime_error("recorder spawn failed: " + std::string(std::strerror(result)));
  return child;
}
std::string failure_issue(unsigned code) {
  switch (code) {
    case 1: return "recording_admission_overflow";
    case 2: return "recording_invalid_access_unit";
    case 3: return "recording_worker_unavailable";
    case 4: return "recording_worker_failed";
    case 5: return "recording_finalize_timeout";
    case 6: return "recording_parent_eos_missing";
    case 7: return "recording_cleanup_pending";
    case 8: return "recording_status_unconfirmed";
    default: return "recording_failed";
  }
}
} // namespace

struct IsolatedRecordingSession::Impl {
  struct Lane {
    explicit Lane(std::size_t slots) : ring(slots, detail::kRecordingLaneByteLimit) {}
    detail::RecordingAuRing ring;
    std::atomic<bool> ended{false};
    std::atomic<std::uint64_t> accepted{0}, rejected{0}, overflows{0}, copy_total_us{0}, copy_max_us{0}, map_total_us{0}, map_max_us{0};
  };
  RecordingSessionSettings settings;
  std::vector<std::unique_ptr<Lane>> lanes;
  std::atomic<unsigned> failure{0};
  std::atomic<bool> stopping{false}, finished{false}, cleanup_pending{false};
  std::atomic<std::int64_t> stop_at{0};
  mutable std::mutex status_mutex;
  Json worker_status = Json::object();
  std::string local_error;
  std::jthread thread;

  explicit Impl(RecordingSessionSettings next) : settings(std::move(next)) {
    if (settings.lanes.empty() || settings.lanes.size() > detail::kRecordingLaneLimit)
      throw std::invalid_argument("recorder supports 1..16 bounded lanes");
    for (const auto& lane : settings.lanes) {
      if (lane.fps <= 0 || lane.fps > 120 || lane.segment_seconds <= 0) throw std::invalid_argument("invalid recorder rate/segment");
      lanes.push_back(std::make_unique<Lane>(static_cast<std::size_t>(std::max(2, lane.fps * 2))));
    }
    thread = std::jthread([this] { run(); });
  }
  ~Impl() { stop(); if (thread.joinable()) thread.join(); }
  void fail(unsigned code) noexcept { unsigned expected = 0; failure.compare_exchange_strong(expected, code); }
  void stop() noexcept {
    std::int64_t unset = 0;
    stop_at.compare_exchange_strong(unset, steady_ms());
    stopping.store(true, std::memory_order_release);
  }
  Json config() const {
    Json cameras = Json::array();
    for (const auto& lane : settings.lanes) cameras.push_back({{"camera_id", lane.camera_id}, {"codec", lane.codec}, {"encoder", lane.encoder}, {"fps", lane.fps}, {"segment_seconds", lane.segment_seconds}});
    return {{"schema_version", 1}, {"vehicle_id", settings.vehicle_id}, {"session_id", settings.session_id}, {"recording_attempt_id", settings.attempt_id}, {"root", settings.root.string()}, {"cameras", cameras}, {"incomplete_before", settings.incomplete_before}, {"session_history_known", settings.session_history_known}};
  }
  bool consume_status(int fd, std::string& pending) {
    std::array<char, 4096> bytes{};
    std::size_t budget = detail::kRecordingConfigByteLimit, messages = 0;
    bool progress = false;
    while (messages < 64) {
      // Parse buffered complete lines before reading. EOF/EAGAIN must never
      // hide a terminal snapshot left behind by the previous message budget.
      const auto newline = pending.find('\n');
      if (newline == std::string::npos) {
        if (budget == 0) break;
        const auto count = ::read(fd, bytes.data(), std::min(bytes.size(), budget));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        progress = true;
        budget -= static_cast<std::size_t>(count);
        pending.append(bytes.data(), static_cast<std::size_t>(count));
        if (pending.size() > detail::kRecordingConfigByteLimit) { fail(4); pending.clear(); break; }
        continue;
      }
      ++messages; progress = true;
      auto value = Json::parse(pending.substr(0, newline), nullptr, false);
      pending.erase(0, newline + 1);
      if (value.is_discarded() || !value.is_object() || !value.contains("recording_attempt_id") ||
          !value["recording_attempt_id"].is_string()) { fail(4); continue; }
      if (value["recording_attempt_id"] != settings.attempt_id) { fail(4); continue; }
      if (value.contains("camera_id")) {
        if (!value["camera_id"].is_string() || !value.contains("recording") || !value["recording"].is_object()) { fail(4); continue; }
        const auto id = value["camera_id"].get<std::string>();
        if (std::none_of(settings.lanes.begin(), settings.lanes.end(), [&](const auto& lane) { return lane.camera_id == id; })) { fail(4); continue; }
        const auto& record = value["recording"];
        bool valid = record.contains("schema_version") && record["schema_version"].is_number_integer() && record["schema_version"] == 1;
        for (const auto* field : {"configured_enabled", "active", "incomplete", "final_eos_received", "cleanup_pending", "summary_persisted"}) valid = valid && record.contains(field) && record[field].is_boolean();
        for (const auto* field : {"status_revision", "accepted_access_units", "worker_input_access_units", "finalized_segments", "unfinished_segments", "summary_revision"}) valid = valid && record.contains(field) && record[field].is_number_unsigned();
        for (const auto* field : {"state", "issue_code", "next_action", "recording_attempt_id"}) valid = valid && record.contains(field) && record[field].is_string() && record[field].get_ref<const std::string&>().size() <= 512;
        valid = valid && record.contains("reported_at_utc_ms") && record["reported_at_utc_ms"].is_number_integer();
        if (!valid || record["recording_attempt_id"] != settings.attempt_id) { fail(4); continue; }
        // Whitelist the contract instead of retaining arbitrary worker objects.
        Json safe_record = Json::object();
        for (const auto* field : {"schema_version", "configured_enabled", "active", "incomplete", "final_eos_received", "cleanup_pending", "summary_persisted", "status_revision", "accepted_access_units", "worker_input_access_units", "finalized_segments", "unfinished_segments", "summary_revision", "state", "issue_code", "next_action", "recording_attempt_id", "reported_at_utc_ms"}) safe_record[field] = record[field];
        value["recording"] = std::move(safe_record);
      }
      bool valid = true;
      for (const auto* field : {"state", "issue_code", "error"}) if (value.contains(field)) valid = valid && value[field].is_string() && value[field].get_ref<const std::string&>().size() <= 512;
      for (const auto* field : {"incomplete", "summary_persisted"}) if (value.contains(field)) valid = valid && value[field].is_boolean();
      if (value.contains("summary_revision")) valid = valid && value["summary_revision"].is_number_unsigned();
      if (value.contains("reported_at_utc_ms")) valid = valid && value["reported_at_utc_ms"].is_number_integer();
      if (!valid) { fail(4); continue; }
      Json safe = Json::object();
      for (const auto* field : {"recording_attempt_id", "camera_id", "recording", "state", "issue_code", "error", "incomplete", "summary_revision", "summary_persisted", "reported_at_utc_ms"}) if (value.contains(field)) safe[field] = value[field];
      std::lock_guard lock(status_mutex);
      if (safe.contains("camera_id")) {
        if (!worker_status.contains("cameras")) worker_status["cameras"] = Json::array();
        auto& cameras = worker_status["cameras"];
        const auto id = safe["camera_id"].get<std::string>();
        auto found = std::find_if(cameras.begin(), cameras.end(), [&](const auto& camera) { return camera["camera_id"] == id; });
        const Json camera{{"camera_id", id}, {"recording", safe["recording"]}};
        if (found == cameras.end()) cameras.push_back(camera); else *found = camera;
        safe.erase("camera_id"); safe.erase("recording");
      }
      worker_status.update(safe);
      if (safe.value("state", "") == "failed") fail(4);
    }
    return progress;
  }
  void run() noexcept {
    int input = -1, status = -1;
    pid_t child = 0;
    bool claimed = false, reaped = false;
    try {
      claimed = claim_worker();
      if (!claimed) { cleanup_pending = true; fail(7); finished = true; return; }
      const auto executable = settings.worker_executable.empty() ? sibling_worker() : settings.worker_executable;
      int data_pair[2]{-1, -1}, status_pipe[2]{-1, -1};
      #ifdef __linux__
      constexpr int socket_type = SOCK_STREAM | SOCK_CLOEXEC;
#else
      constexpr int socket_type = SOCK_STREAM;
#endif
      if (::socketpair(AF_UNIX, socket_type, 0, data_pair) < 0) throw std::runtime_error("recorder data IPC failed");
      const int socket_bytes = 8192;
      if (::setsockopt(data_pair[0], SOL_SOCKET, SO_SNDBUF, &socket_bytes, sizeof(socket_bytes)) < 0) { ::close(data_pair[0]); ::close(data_pair[1]); throw std::runtime_error("cannot bound recorder IPC socket"); }
      input = data_pair[0];
      #ifdef __linux__
      const auto pipe_result = ::pipe2(status_pipe, O_CLOEXEC);
#else
      const auto pipe_result = ::pipe(status_pipe);
#endif
      if (pipe_result < 0) { ::close(data_pair[1]); throw std::runtime_error("recorder status IPC failed"); }
      status = status_pipe[0];
      try {
        if (::fcntl(data_pair[1], F_SETFD, FD_CLOEXEC) < 0 || ::fcntl(status_pipe[1], F_SETFD, FD_CLOEXEC) < 0) throw std::runtime_error("cannot protect recorder child endpoints");
        nonblocking(input); nonblocking(status); child = spawn_worker(executable, data_pair[1], status_pipe[1]);
      }
      catch (...) { ::close(data_pair[1]); ::close(status_pipe[1]); throw; }
      ::close(data_pair[1]); ::close(status_pipe[1]);
      publish_child(child);
      auto start = config().dump();
      if (start.size() > detail::kRecordingConfigByteLimit) throw std::runtime_error("recorder config exceeds bound");
      RecordingWireHeader start_header{};
      start_header.type = RecordingPacketType::Start;
      start_header.bytes = static_cast<std::uint32_t>(start.size());
      std::vector<std::uint8_t> start_packet(sizeof(start_header) + start.size());
      std::memcpy(start_packet.data(), &start_header, sizeof(start_header));
      std::memcpy(start_packet.data() + sizeof(start_header), start.data(), start.size());
      std::size_t start_offset = 0, packet_offset = 0, cursor = 0;
      std::optional<std::size_t> selected;
      std::vector<bool> eos_sent(lanes.size(), false);
      RecordingWireHeader end_header{};
      end_header.type = RecordingPacketType::End;
      std::string pending_status;
      auto terminate_at = std::int64_t{0};
      while (!reaped) {
        consume_status(status, pending_status);
        int child_status = 0;
        const auto waited = ::waitpid(child, &child_status, WNOHANG);
        if (waited == child || (waited < 0 && errno == ECHILD)) {
          reaped = true;
          // The child can exit between polls with >64 queued terminal records.
          // Closed-process drain is bounded independently (at most 1 MiB/1024
          // messages); malformed/flooded evidence remains unconfirmed.
          for (unsigned round = 0; round < 16 && consume_status(status, pending_status); ++round) {}
          std::lock_guard lock(status_mutex);
          worker_status["child_exit_status"] = child_status;
          if (waited == child && (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0)) fail(4);
          else if (worker_status.value("state", "") != "finalized") fail(8);
          if (failure == 0) {
            for (std::size_t index = 0; index < lanes.size(); ++index) {
              if (!worker_status.contains("cameras") || !worker_status["cameras"].is_array()) { fail(8); break; }
              const auto found = std::find_if(worker_status["cameras"].begin(), worker_status["cameras"].end(), [&](const auto& camera) { return camera["camera_id"] == settings.lanes[index].camera_id; });
              if (!worker_status["cameras"].is_array() || found == worker_status["cameras"].end()) { fail(8); break; }
              const auto& record = (*found)["recording"];
              if (record["state"] != "finalized" || !record["final_eos_received"].get<bool>() || record["unfinished_segments"] != 0 || record["finalized_segments"] == 0 || record["worker_input_access_units"] != lanes[index]->accepted.load()) { fail(8); break; }
            }
          }
          break;
        }
        const auto now = steady_ms();
        if (stopping && now - stop_at.load() >= kFinishBudgetMs && failure == 0) fail(5);
        if (failure != 0) {
          if (terminate_at == 0) { ::shutdown(input, SHUT_WR); ::kill(child, SIGTERM); terminate_at = now; }
          if (now - terminate_at >= 100) ::kill(child, SIGKILL);
          if (now - terminate_at >= 300) { cleanup_pending = true; break; }
        } else {
          const std::uint8_t* pointer = nullptr;
          std::size_t count = 0;
          if (start_offset < start_packet.size()) { pointer = start_packet.data() + start_offset; count = start_packet.size() - start_offset; }
          else {
            if (!selected) {
              for (std::size_t offset = 0; offset < lanes.size(); ++offset) {
                const auto index = (cursor + offset) % lanes.size();
                if (lanes[index]->ring.front() || (lanes[index]->ended && !eos_sent[index])) { selected = index; cursor = (index + 1) % lanes.size(); break; }
              }
            }
            if (selected) {
              const auto index = *selected;
              const auto front = lanes[index]->ring.front();
              if (!front) { end_header.lane = static_cast<std::uint32_t>(index); end_header.pts = lanes[index]->accepted.load(); pointer = reinterpret_cast<const std::uint8_t*>(&end_header) + packet_offset; count = sizeof(end_header) - packet_offset; }
              else if (packet_offset < sizeof(RecordingWireHeader)) { pointer = reinterpret_cast<const std::uint8_t*>(front->header) + packet_offset; count = sizeof(RecordingWireHeader) - packet_offset; }
              else {
                const auto payload_offset = packet_offset - sizeof(RecordingWireHeader);
                if (payload_offset < front->first.size()) { pointer = front->first.data() + payload_offset; count = front->first.size() - payload_offset; }
                else { pointer = front->second.data() + payload_offset - front->first.size(); count = front->second.size() - (payload_offset - front->first.size()); }
              }
            }
          }
          if (pointer != nullptr && count != 0) {
#ifdef MSG_NOSIGNAL
            const auto sent = ::send(input, pointer, count, MSG_NOSIGNAL);
#else
            const auto sent = ::send(input, pointer, count, 0);
#endif
            if (sent > 0) {
              if (start_offset < start_packet.size()) start_offset += static_cast<std::size_t>(sent);
              else {
                packet_offset += static_cast<std::size_t>(sent);
                const auto front = lanes[*selected]->ring.front();
                const auto total = sizeof(RecordingWireHeader) + (front ? front->header->bytes : 0);
                if (packet_offset == total) {
                  if (front) lanes[*selected]->ring.pop(); else eos_sent[*selected] = true;
                  packet_offset = 0; selected.reset();
                }
              }
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) fail(4);
          }
        }
        pollfd waiters[2]{{status, POLLIN, 0}, {input, failure == 0 ? static_cast<short>(POLLOUT) : static_cast<short>(0), 0}};
        static_cast<void>(::poll(waiters, 2, 5));
        // POLLOUT is often immediately ready; avoid spinning while there is no
        // AU to send. This bounded wait contains no filesystem/GStreamer work.
        if (!selected && start_offset >= start_packet.size()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      consume_status(status, pending_status);
    } catch (const std::exception& error) {
      { std::lock_guard lock(status_mutex); local_error = error.what(); }
      fail(3);
      if (child > 0) {
        ::kill(child, SIGKILL);
        const auto deadline = steady_ms() + 300;
        while (steady_ms() < deadline) {
          int result = 0;
          const auto waited = ::waitpid(child, &result, WNOHANG);
          if (waited == child || (waited < 0 && errno == ECHILD)) { reaped = true; break; }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!reaped) cleanup_pending = true;
      }
    }
    if (input >= 0) ::close(input);
    if (status >= 0) ::close(status);
    if (claimed) release_worker(reaped || child == 0);
    finished = true;
  }
  Json status_for(std::size_t index) const {
    std::lock_guard lock(status_mutex);
    Json value = {{"schema_version", 1}, {"configured_enabled", true}, {"active", false}, {"state", "starting"}, {"incomplete", failure != 0 || settings.incomplete_before}, {"issue_code", ""}, {"next_action", "Wait for recording startup."}, {"recording_attempt_id", settings.attempt_id}, {"status_revision", 0}, {"worker_input_access_units", 0}, {"finalized_segments", 0}, {"unfinished_segments", 0}, {"final_eos_received", false}, {"summary_persisted", false}, {"summary_revision", 0}, {"reported_at_utc_ms", nullptr}};
    if (worker_status.contains("cameras") && worker_status["cameras"].is_array()) {
      for (const auto& camera : worker_status["cameras"]) if (camera.value("camera_id", "") == settings.lanes[index].camera_id && camera.contains("recording")) value.update(camera["recording"]);
    }
    value["incomplete"] = settings.incomplete_before || value.value("incomplete", false);
    value["accepted_access_units"] = lanes[index]->accepted.load();
    value["rejected_access_units"] = lanes[index]->rejected.load();
    value["admission_overflows"] = lanes[index]->overflows.load();
    value["queue_limit_access_units"] = std::max(2, settings.lanes[index].fps * 2);
    value["queue_limit_bytes"] = detail::kRecordingLaneByteLimit;
    value["queued_access_units"] = lanes[index]->ring.queued();
    value["copy_max_us"] = lanes[index]->copy_max_us.load();
    value["copy_total_us"] = lanes[index]->copy_total_us.load();
    value["map_max_us"] = lanes[index]->map_max_us.load();
    value["map_total_us"] = lanes[index]->map_total_us.load();
    value["copy_scope"] = "bounded_private_AU_copy_not_zero_cost_or_end_to_end_latency";
    value["cleanup_pending"] = cleanup_pending.load();
    if (failure != 0) {
      value["state"] = cleanup_pending ? "cleanup_pending" : "failed";
      value["active"] = false; value["incomplete"] = true;
      value["issue_code"] = worker_status.value("issue_code", failure_issue(failure.load()));
      if (value["issue_code"] == "") value["issue_code"] = failure_issue(failure.load());
      value["next_action"] = value["issue_code"] == "recording_status_unconfirmed" ?
          "Finalization evidence is unconfirmed. Preserve clips and inspect recorder status; recording completeness is unknown." :
          "Recording is incomplete. Preserve completed clips and inspect storage/recorder health; live video continues.";
      value["status_revision"] = value.value("status_revision", std::uint64_t{0}) + 1;
      // This failure/revision is a parent observation. The worker's previous
      // on-disk summary does not persist the overlay, even if that older
      // snapshot was successfully written. Keep its revision/time as provenance.
      value["summary_persisted"] = false;
    }
    if (!local_error.empty()) value["error"] = local_error;
    return value;
  }
};

IsolatedRecordingSession::IsolatedRecordingSession(RecordingSessionSettings settings) : impl_(std::make_unique<Impl>(std::move(settings))) {}
IsolatedRecordingSession::~IsolatedRecordingSession() = default;
bool IsolatedRecordingSession::can_admit(std::size_t index, std::size_t bytes) const noexcept {
  return index < impl_->lanes.size() && impl_->failure == 0 && !impl_->lanes[index]->ended && impl_->lanes[index]->ring.can_push(bytes);
}
void IsolatedRecordingSession::reject_admission(std::size_t index, bool overflow) noexcept {
  if (index >= impl_->lanes.size()) return;
  ++impl_->lanes[index]->rejected;
  if (impl_->failure == 0) { if (overflow) ++impl_->lanes[index]->overflows; impl_->fail(overflow ? 1U : 2U); }
}
void IsolatedRecordingSession::admit_bytes(std::size_t index, detail::RecordingWireHeader header, std::span<const std::uint8_t> bytes) noexcept {
  if (!can_admit(index, bytes.size())) { reject_admission(index, true); return; }
  auto& lane = *impl_->lanes[index];
  header.lane = static_cast<std::uint32_t>(index);
  const auto begin = Clock::now();
  const auto accepted = lane.ring.push(header, bytes);
  const auto duration = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count());
  lane.copy_total_us += duration;
  auto maximum = lane.copy_max_us.load();
  while (duration > maximum && !lane.copy_max_us.compare_exchange_weak(maximum, duration)) {}
  if (accepted) ++lane.accepted; else reject_admission(index, true);
}
void IsolatedRecordingSession::note_mapping_duration(std::size_t index, std::uint64_t duration) noexcept {
  if (index >= impl_->lanes.size()) return;
  auto& lane = *impl_->lanes[index]; lane.map_total_us += duration;
  auto maximum = lane.map_max_us.load();
  while (duration > maximum && !lane.map_max_us.compare_exchange_weak(maximum, duration)) {}
}
void IsolatedRecordingSession::end_lane(std::size_t index) noexcept { if (index < impl_->lanes.size()) impl_->lanes[index]->ended = true; }
void IsolatedRecordingSession::request_stop() noexcept { impl_->stop(); }
void IsolatedRecordingSession::finish() { impl_->stop(); if (impl_->thread.joinable()) impl_->thread.join(); }
void IsolatedRecordingSession::abort(std::string_view) noexcept { impl_->fail(6); impl_->stop(); }
Json IsolatedRecordingSession::lane_status(std::size_t index) const { return index < impl_->lanes.size() ? impl_->status_for(index) : Json::object(); }
Json IsolatedRecordingSession::session_status() const {
  Json cameras = Json::array();
  for (std::size_t index = 0; index < impl_->lanes.size(); ++index) cameras.push_back({{"camera_id", impl_->settings.lanes[index].camera_id}, {"recording", impl_->status_for(index)}});
  return {{"schema_version", 1}, {"kind", "recording_session_summary"}, {"upload_state", "not_applicable"}, {"vehicle_id", impl_->settings.vehicle_id}, {"session_id", impl_->settings.session_id}, {"recording_attempt_id", impl_->settings.attempt_id}, {"cameras", cameras}, {"completeness_scope", "current_runtime_known_attempts"}, {"session_history_known", impl_->settings.session_history_known}};
}
} // namespace mine_teleop
