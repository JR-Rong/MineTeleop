// Windows bench viewer: native video with an explicitly isolated mock mode.
#include "native_receiver_core.hpp"
#include "mine_teleop/detail/native_video_health.hpp"
#include "mine_teleop/detail/native_mock_media_gate.hpp"
#include "mine_teleop/detail/native_mock_input_sequence.hpp"
#include "mine_teleop/detail/native_mock_keyboard.hpp"
#include "mine_teleop/detail/native_control_courier.hpp"
#include "mine_teleop/detail/native_viewer_observation.hpp"
#include <gst/video/video.h>
#include <gio/gnetworking.h>
#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include <cstdio>

namespace {
using namespace mine_teleop;
using namespace mine_teleop::bench;

std::string utf8(const std::wstring& value) {
  if (value.empty()) return {};
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  if (!size) throw std::runtime_error("invalid text input");
  std::string result(static_cast<std::size_t>(size), '\0');
  if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                           result.data(), size, nullptr, nullptr)) throw std::runtime_error("invalid text input");
  return result;
}

std::wstring text_of(HWND control) {
  const int count = GetWindowTextLengthW(control);
  std::wstring result(static_cast<std::size_t>(count) + 1, L'\0');
  GetWindowTextW(control, result.data(), count + 1);
  result.resize(static_cast<std::size_t>(count));
  return result;
}

// Each panel retains only the newest decoded frame. The video-health signal is
// emitted after GDI accepts the frame; it is a renderer submission, not proof
// of scan-out or of the vehicle capture timestamp.
struct RenderSession {
  struct Frame {
    std::string camera;
    std::uint64_t pts{};
    int width{}, height{};
    std::vector<std::uint8_t> pixels;
  };
  std::mutex mutex;
  std::array<std::shared_ptr<Frame>, 2> latest{};
  std::array<std::uint64_t, 2> submissions{};
  std::array<std::optional<std::uint64_t>, 2> last_submitted{};
  std::array<double, 2> last_submission_ms{};
  std::array<Samples, 2> submission_gaps{};
  std::vector<std::string> cameras;
  std::shared_ptr<detail::NativeVideoHealth> health;
  std::array<HWND, 2> panels{};

  void accept(const std::string& camera, GstSample* sample) {
    const auto it = std::find(cameras.begin(), cameras.end(), camera);
    if (it == cameras.end()) throw std::runtime_error("unmapped software frame");
    const auto index = static_cast<std::size_t>(it - cameras.begin());
    GstVideoInfo info{};
    auto* buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    if (!buffer || !GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) ||
        !gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
        GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_BGRA ||
        GST_VIDEO_INFO_WIDTH(&info) <= 0 || GST_VIDEO_INFO_HEIGHT(&info) <= 0 ||
        GST_VIDEO_INFO_WIDTH(&info) > 4096 || GST_VIDEO_INFO_HEIGHT(&info) > 4096)
      throw std::runtime_error("invalid software video frame");
    GstVideoFrame mapped{};
    if (!gst_video_frame_map(&mapped, &info, buffer, GST_MAP_READ)) throw std::runtime_error("software frame mapping failed");
    try {
      auto frame = std::make_shared<Frame>();
      frame->camera = camera; frame->pts = GST_BUFFER_PTS(buffer);
      frame->width = static_cast<int>(GST_VIDEO_INFO_WIDTH(&info));
      frame->height = static_cast<int>(GST_VIDEO_INFO_HEIGHT(&info));
      const auto row = static_cast<std::size_t>(frame->width) * 4;
      const auto stride = GST_VIDEO_FRAME_PLANE_STRIDE(&mapped, 0);
      if (stride < 0 || static_cast<std::size_t>(stride) < row) throw std::runtime_error("invalid software frame stride");
      frame->pixels.resize(row * static_cast<std::size_t>(frame->height));
      const auto* pixels = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&mapped, 0));
      for (int y = 0; y < frame->height; ++y)
        std::memcpy(frame->pixels.data() + static_cast<std::size_t>(y) * row,
                    pixels + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride), row);
      { std::lock_guard lock(mutex); latest[index] = std::move(frame); }
      gst_video_frame_unmap(&mapped);
      InvalidateRect(panels[index], nullptr, FALSE);
    } catch (...) { gst_video_frame_unmap(&mapped); throw; }
  }

  void paint(std::size_t index, HWND panel, HWND parent) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(panel, &ps);
    std::shared_ptr<Frame> frame;
    { std::lock_guard lock(mutex); frame = latest[index]; }
    RECT rect{}; GetClientRect(panel, &rect);
    if (dc && frame && rect.right > 0 && rect.bottom > 0) {
      BITMAPINFO bitmap{};
      bitmap.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      bitmap.bmiHeader.biWidth = frame->width;
      bitmap.bmiHeader.biHeight = -frame->height;
      bitmap.bmiHeader.biPlanes = 1;
      bitmap.bmiHeader.biBitCount = 32;
      bitmap.bmiHeader.biCompression = BI_RGB;
      const int result = StretchDIBits(dc, 0, 0, rect.right, rect.bottom, 0, 0, frame->width, frame->height,
                                       frame->pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY);
      const bool visible = IsWindowVisible(parent) && !IsIconic(parent) && GetForegroundWindow() == parent;
      if (health) static_cast<void>(health->set_usable(frame->camera, 1, visible));
      if (result != 0 && result != static_cast<int>(GDI_ERROR) && GdiFlush() && visible) {
        if (health) static_cast<void>(health->render_submitted_now(frame->camera, 1, frame->pts));
        std::lock_guard lock(mutex);
        if (!last_submitted[index] || frame->pts > *last_submitted[index]) {
          const auto submitted_at = bench_now_ms();
          if (last_submission_ms[index] > 0) submission_gaps[index].add(submitted_at - last_submission_ms[index]);
          last_submission_ms[index] = submitted_at;
          last_submitted[index] = frame->pts; ++submissions[index];
        }
      }
    } else if (dc) FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    EndPaint(panel, &ps);
  }
};

struct Window {
  HWND hwnd{}, driver_config{}, vehicle_config{}, password{}, status{}, start{}, stop{};
  std::array<HWND, 2> video{};
  std::mutex mutex;
  std::mutex render_mutex;
  std::shared_ptr<RenderSession> render_session;
  std::wstring message{L"仅观看台架：控制禁用。车端隔离须独立核验。"};
  std::jthread worker;
  std::atomic<bool> running{false};
  std::atomic<std::int64_t> last_observation_ms{0};
  detail::NativeMockKeyboard keyboard;
  bool mock_control{false};
  std::string mock_local_ip, mock_peer_ip, mock_ip_family;

  void suspend_render_health() {
    std::shared_ptr<RenderSession> current;
    { std::lock_guard lock(render_mutex); current = render_session; }
    if (current && current->health)
      for (const auto& camera : current->cameras) static_cast<void>(current->health->set_usable(camera, 1, false));
  }

  void report(std::wstring value) {
    { std::lock_guard lock(mutex); message = std::move(value); }
    last_observation_ms = static_cast<std::int64_t>(bench_now_ms());
    PostMessageW(hwnd, WM_APP + 1, 0, 0);
  }

  // Isolated bench mode is constrained to a mock adapter and zero speed. It
  // must not be used as a production driving interface.
  void run_mock_control(DriverConsoleRuntime& driver, Receiver& receiver,
                        const VehicleConfig& vehicle, const DriverConfig& config,
                        const Json& session, const std::string& attempt,
                        const std::shared_ptr<RenderSession>& render,
                        const std::shared_ptr<detail::NativeMockMediaGate>& gate,
                        std::stop_token parent, std::stop_token stop) {
    const std::string session_id = session.at("session_id").get<std::string>();
    const auto generation = session.at("control_session_generation").get<std::uint64_t>();
    const std::string owner = "windows-native-mock";
    std::atomic<std::uint64_t> epoch{0};
    std::uint64_t profile_seq = 0;
    bool profile_sent = false, profile_accepted = false, handshake_sent = false;
    std::optional<SessionControlProfile> requested_profile;
    auto next_evidence = Clock::now();
    auto next_status = Clock::now();
    Json cached_status;
    Json telemetry;
    std::int64_t telemetry_at_ms = 0;
    std::uint64_t telemetry_seq = 0, safety_stop_episodes = 0, last_result_seq = 0;
    bool was_active = false, stop_episode = false;
    Json dc_send_trace = Json::array();
    std::uint64_t dc_send_trace_dropped = 0;
    const auto flush_dc_trace = [&] {
      if (!config.control_trace_commands || (dc_send_trace.empty() && !dc_send_trace_dropped)) return;
      const Json trace{{"event", "native_mock_dc_batch"}, {"session_id", session_id},
          {"records", std::move(dc_send_trace)}, {"dropped", dc_send_trace_dropped}};
      std::fprintf(stderr, "%s\n", trace.dump().c_str());
      dc_send_trace = Json::array(); dc_send_trace_dropped = 0;
    };
    std::unique_ptr<detail::NativeControlCourier> courier;
    auto* channel = receiver.control_channel();
    if (!channel || !render->health) throw std::runtime_error("mock control requires scoped channel and critical video");
    // The input lease must not wait for signaling or the transport bridge.
    // The sender independently checks its current clock before publication.
    std::atomic<bool> input_failed{false};
    std::atomic<std::int64_t> clock_observed_ms{0}, input_authorized_at_ms{0};
    // Keep diagnostics off the lease worker's I/O path and bound memory if
    // the observation thread stalls. -1 means a short-circuited check.
    struct GuardState {
      bool focused, ui_alive, clock_ok, video_ready, video_sample_healthy, authorized, ready;
      int remote_ready, direct_ready;
      std::uint64_t bad_revision, brake_revision;
      bool operator==(const GuardState&) const = default;
    };
    struct GuardEvent { std::int64_t at, authorization_at, authorization_now; GuardState state; };
    std::mutex guard_mutex;
    std::vector<GuardEvent> guard_events;
    std::uint64_t guard_dropped = 0;
    const auto flush_guard_trace = [&] {
      if (!config.control_trace_commands) return;
      std::vector<GuardEvent> events;
      std::uint64_t dropped = 0;
      { std::lock_guard lock(guard_mutex); events.swap(guard_events); std::swap(dropped, guard_dropped); }
      if (events.empty() && !dropped) return;
      Json records = Json::array();
      for (const auto& event : events) {
        const auto& s = event.state;
        records.push_back({{"monotonic_ms", event.at}, {"focused", s.focused}, {"ui_alive", s.ui_alive},
            {"clock_ok", s.clock_ok}, {"video_ready", s.video_ready}, {"video_sample_healthy", s.video_sample_healthy},
            {"authorized", s.authorized}, {"ready", s.ready},
            {"remote_ready", s.remote_ready < 0 ? Json(nullptr) : Json(s.remote_ready != 0)},
            {"direct_ready", s.direct_ready < 0 ? Json(nullptr) : Json(s.direct_ready != 0)},
            {"authorization_age_ms", event.authorization_at > 0 ?
                Json(event.authorization_now - event.authorization_at) : Json(nullptr)},
            {"bad_revision", s.bad_revision}, {"brake_revision", s.brake_revision}});
      }
      std::fprintf(stderr, "%s\n", Json{{"event", "native_mock_guard_batch"}, {"records", std::move(records)},
          {"dropped", dropped}}.dump().c_str());
    };
    std::jthread input_worker([&](std::stop_token input_stop) {
      try {
        detail::NativeMockInputSequence input_sequence;
        bool withdrawn = false;
        std::optional<GuardState> previous_guard;
        while (!input_stop.stop_requested() && !stop.stop_requested() && !parent.stop_requested()) {
          const auto clock_at = clock_observed_ms.load();
          const auto now_ms = static_cast<std::int64_t>(bench_now_ms());
          const bool clock_ok = clock_at > 0 && now_ms >= clock_at && now_ms - clock_at < 500;
          const auto video_health = render->health->snapshot_now();
          const bool focused = IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd;
          if (!focused) for (const auto& id : render->cameras) static_cast<void>(render->health->set_usable(id, 1, false));
          const bool ui_alive = focused && keyboard.heartbeat_alive_now();
          const int remote_ready = ui_alive && clock_ok ? static_cast<int>(gate->ready_now()) : -1;
          const int direct_ready = remote_ready == 1 && video_health.ready ?
              static_cast<int>(channel->direct_udp(mock_local_ip, mock_peer_ip)) : -1;
          const bool ready = ui_alive && clock_ok && remote_ready == 1 && video_health.ready && direct_ready == 1;
          const auto authorized_at = input_authorized_at_ms.load();
          const auto authorization_now_ms = static_cast<std::int64_t>(bench_now_ms());
          const bool authorized = authorized_at > 0 && authorization_now_ms >= authorized_at &&
              authorization_now_ms - authorized_at < 200;
          if (config.control_trace_commands) {
            const GuardState state{focused, ui_alive, clock_ok, video_health.ready, video_health.sample_healthy,
                authorized, ready, remote_ready, direct_ready, video_health.bad_revision, video_health.brake_revision};
            if (!previous_guard || *previous_guard != state) {
              std::lock_guard lock(guard_mutex);
              if (guard_events.size() < 128) guard_events.push_back({now_ms, authorized_at, authorization_now_ms, state});
              else ++guard_dropped;
              previous_guard = state;
            }
          }
          const auto keyboard_input = keyboard.sample_now(ready && authorized);
          const auto current_epoch = epoch.load();
          if ((ready || current_epoch != 0) && (ui_alive || !withdrawn)) {
            Json input{{"session_id", session_id}, {"session_generation", generation},
                {"ui_instance_id", owner}, {"ui_control_epoch", current_epoch}, {"takeover", current_epoch == 0},
                {"gear", "N"}, {"steering", keyboard_input.steering},
                {"throttle", keyboard_input.throttle}, {"brake", keyboard_input.brake},
                {"estop", false}, {"critical_video_ready", ready},
                {"critical_video_sample_healthy", ready && video_health.sample_healthy},
                {"critical_video_bad_sample_revision", video_health.bad_revision},
                {"critical_video_brake_revision", video_health.brake_revision}};
            input["intent_seq"] = input_sequence.next(input);
            const auto result = driver.update_control_intent(input);
            if (!result.value("accepted", false)) throw std::runtime_error("mock input rejected");
            epoch.store(result.at("ui_control_epoch").get<std::uint64_t>());
            withdrawn = !ui_alive;
          }
          if (ui_alive) withdrawn = false;
          std::this_thread::sleep_for(60ms);
        }
      } catch (const std::exception& error) {
        const std::string_view detail = error.what();
        const char* category = detail.find("mock input rejected") != std::string_view::npos ? "rejected" :
            detail.find("stale or different native control session") != std::string_view::npos ? "session_changed" :
            detail.find("not connected") != std::string_view::npos ? "disconnected" : "other";
        std::fprintf(stderr, "native_mock_input_failure category=%s\n", category); std::fflush(stderr);
        input_failed.store(true);
      } catch (...) { input_failed.store(true); }
    });
    auto on_status = [&](const Json& envelope) {
      const auto value = envelope.contains("status") && envelope.at("status").is_object() ? envelope.at("status") : envelope;
      if (!value.is_object() || value.value("session_id", "") != session_id ||
          value.value("vehicle_id", "") != vehicle.vehicle_id || value.value("driver_id", "") != config.driver_id) return;
      Json profile_status;
      if (value.value("event", "") == "session_control_profile_status") profile_status = value;
      else if (value.value("event", "") == "vehicle_telemetry" && value.contains("session_control_profile"))
        profile_status = value.at("session_control_profile");
      if (profile_seq && profile_status.is_object() && profile_status.contains("last_request_seq") &&
          profile_status.at("last_request_seq").is_number_unsigned() &&
          profile_status.at("last_request_seq").get<std::uint64_t>() == profile_seq) {
        if (!profile_status.value("accepted", false) || !profile_status.value("active", false))
          throw std::runtime_error("mock control profile inactive or rejected");
        if (!profile_status.contains("effective_profile") || !profile_status.at("effective_profile").is_object() ||
            !requested_profile || SessionControlProfile::from_json(profile_status.at("effective_profile")) != *requested_profile)
          throw std::runtime_error("mock control profile changed");
        profile_accepted = true;
      }
      if (value.value("event", "") == "vehicle_telemetry") {
        const auto seq = value.value("seq", std::uint64_t{0});
        if (seq <= telemetry_seq || !value.contains("control_timing") || !value.at("control_timing").is_object()) return;
        telemetry_seq = seq;
        telemetry_at_ms = static_cast<std::int64_t>(bench_now_ms());
        const auto state = value.value("safety_state", "unknown");
        const bool braking = value.at("control_timing").value("control_braking_wait", true);
        if (state == "CONTROL_ACTIVE" && !braking) was_active = true;
        const bool stopped = was_active && (braking || state == "DEGRADED" || state == "TIMEOUT_BRAKE" ||
                                            state == "ESTOP" || state == "FAULT");
        if (stopped && !stop_episode) ++safety_stop_episodes;
        stop_episode = stopped;
        telemetry = {{"seq", seq}, {"safety_state", state}, {"braking_wait", braking},
            {"control_recovery_cause", value.at("control_timing").value("control_recovery_cause", "")},
            {"last_valid_command_seq", value.at("control_timing").value("last_valid_command_seq", std::uint64_t{0})},
            {"stop_sequence", value.value("stop_sequence", std::uint64_t{0})},
            {"gear", value.value("gear", "")},
            {"speed_mps", value.contains("speed_mps") && value.at("speed_mps").is_number()
                ? value.at("speed_mps") : Json(nullptr)}};
      }
    };
    while (!stop.stop_requested() && !parent.stop_requested()) {
      const auto now = Clock::now();
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      if (now >= next_status) { cached_status = driver.status(true); next_status = now + 25ms; }
      const auto& status = cached_status;
      const auto& sync = status.at("time_sync");
      const bool clock_ok = sync.value("synchronized", false) && sync.value("uncertainty_ms", 999) <= 25 &&
          sync.value("uncertainty_ms", -1) >= 0 && status.at("time_sync_readiness").value("ready", false);
      const auto video_health = render->health->snapshot_now();
      const bool focused = IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd;
      if (!focused) for (const auto& id : render->cameras) static_cast<void>(render->health->set_usable(id, 1, false));
      const bool ready = focused && clock_ok && gate->ready_now() && video_health.ready &&
          channel->direct_udp(mock_local_ip, mock_peer_ip);
      clock_observed_ms.store(clock_ok ? now_ms : 0);
      if (input_failed.load()) throw std::runtime_error("mock input worker failed");
      const auto current_epoch = epoch.load();
      if (current_epoch == 0) {
        // No command owner exists yet. Do not allow unsolicited telemetry to
        // fill the bounded channel before the safety gates admit neutral input.
        while (channel->pop()) {}
      } else {
        const detail::NativeCourierScope current{{session_id, generation, attempt}, owner, current_epoch};
        if (!courier) courier = std::make_unique<detail::NativeControlCourier>(current);
        detail::NativeControlCourier::Io io{
            [&] { return channel->pop(); }, [&] { return channel->direct_udp(mock_local_ip, mock_peer_ip); },
            [&](const Json& request) { return driver.control_transport_bridge(request); },
            [&](const std::string& bytes) {
              const auto started = static_cast<std::int64_t>(bench_now_ms());
              const bool queued = channel->send(bytes);
              if (config.control_trace_commands) {
                const auto frame = Json::parse(bytes, nullptr, false);
                if (frame.is_object() && frame.contains("seq") && frame.at("seq").is_number_unsigned()) {
                  if (dc_send_trace.size() < 128)
                    dc_send_trace.push_back({{"seq", frame.at("seq")}, {"started_monotonic_ms", started},
                        {"completed_monotonic_ms", static_cast<std::int64_t>(bench_now_ms())}, {"queued", queued}});
                  else ++dc_send_trace_dropped;
                }
              }
              return queued;
            }, on_status};
        if (!courier->tick(current, io)) throw std::runtime_error("mock courier retired");
        const auto transport = status.at("native_control").at("control_transport");
        const bool negotiated = transport.value("epoch", 0) > 0 && !transport.value("selecting", true);
        if (ready && negotiated && !profile_sent) {
          SessionControlProfile profile;
          profile.target_speed_kph = 0; profile.max_motor_torque_nm = 0;
          profile.max_brake_pressure_bar = vehicle.field_safety.max_brake_pressure_bar;
          profile.service_brake_pressure_bar = std::min(config.control_limits.initial_service_brake_pressure_bar, profile.max_brake_pressure_bar);
          profile.hard_brake_pressure_bar = std::min(config.control_limits.initial_hard_brake_pressure_bar, profile.max_brake_pressure_bar);
          profile.max_steering_angle_deg = 0;
          profile.critical_camera_stale_brake_ms = vehicle.runtime.critical_camera_stale_brake_ms;
          profile.control_recovery_timeout_ms = vehicle.runtime.control_recovery_timeout_ms;
          requested_profile = profile;
          auto prepared = driver.prepare_control_profile(profile.to_json());
          const auto message = prepared.at("request");
          profile_seq = message.at("seq").get<std::uint64_t>();
          static_cast<void>(driver.send_control_management({{"source_session_id", session_id},
              {"source_control_session_generation", generation}, {"media_negotiation_id", attempt},
              {"ui_instance_id", owner}, {"ui_control_epoch", current_epoch}, {"message", message}}));
          profile_sent = true;
        }
        if (ready && negotiated && profile_accepted && !handshake_sent) {
          static_cast<void>(driver.send_control_management({{"source_session_id", session_id},
              {"source_control_session_generation", generation}, {"media_negotiation_id", attempt},
              {"ui_instance_id", owner}, {"ui_control_epoch", current_epoch},
              {"message", {{"event", "vcu_handshake_command"}, {"action", "connect"}}}}));
          handshake_sent = true;
        }
      }
      const bool authorized = ready && profile_accepted && handshake_sent && telemetry_at_ms > 0 &&
          now_ms >= telemetry_at_ms && now_ms - telemetry_at_ms < 200 && telemetry.is_object() &&
          telemetry.value("safety_state", "") == "CONTROL_ACTIVE" &&
          !telemetry.value("braking_wait", true) && telemetry.value("gear", "") == "N" &&
          telemetry.contains("speed_mps") && telemetry.at("speed_mps").is_number() &&
          telemetry.at("speed_mps").get<double>() == 0.0;
      input_authorized_at_ms.store(authorized ? now_ms : 0);
      if (now >= next_evidence) {
        flush_dc_trace();
        flush_guard_trace();
        const auto native = status.at("native_control");
        const auto transport = native.at("control_transport");
        const auto sends = channel->send_stats();
        const Json evidence{{"event", "native_mock_observation"}, {"monotonic_ms", now_ms},
            {"video_ready", video_health.ready}, {"remote_media_ready", gate->ready(now_ms)},
            {"remote_media_reason", gate->reason(now_ms)},
            {"focused", focused}, {"clock_ready", clock_ok}, {"clock_uncertainty_ms", sync.value("uncertainty_ms", -1)},
            {"sctp_direct_udp", channel->direct_udp(mock_local_ip, mock_peer_ip)}, {"control_path", transport.value("path", "unknown")},
            {"control_selecting", transport.value("selecting", true)}, {"intent_fresh", native.value("intent_fresh", false)},
            {"control_failed_dc_trials", transport.value("failed_dc_trials", 0)},
            {"channel_send", {{"attempted", sends.attempted}, {"queued", sends.queued},
                {"buffered", sends.buffered}, {"unavailable", sends.unavailable}, {"errors", sends.errors}}},
            {"sent_total", native.value("commands_sent_total", std::uint64_t{0})},
            {"last_ack_seq", native.value("last_ack_seq", std::uint64_t{0})},
            {"last_gap_ms", native.value("last_gap_ms", std::int64_t{0})},
            {"profile_accepted", profile_accepted}, {"handshake_sent", handshake_sent},
            {"telemetry_age_ms", telemetry_at_ms ? Json(now_ms - telemetry_at_ms) : Json(nullptr)},
            {"telemetry", telemetry}, {"safety_stop_episodes", safety_stop_episodes}};
        std::fprintf(stderr, "%s\n", evidence.dump().c_str()); std::fflush(stderr);
        next_evidence = now + 1s;
      }
      const auto native = status.at("native_control");
      const auto latency = native.value("vehicle_result_latency", Json(nullptr));
      if (latency.is_object() && latency.contains("command_seq") && latency.at("command_seq").is_number_unsigned()) {
        const auto seq = latency.at("command_seq").get<std::uint64_t>();
        if (seq > last_result_seq) {
          last_result_seq = seq;
          const Json result{{"event", "native_mock_vehicle_result"}, {"monotonic_ms", now_ms},
              {"command_seq", seq}, {"command_path", latency.value("command_path", Json(nullptr))},
              {"round_trip_ms", latency.value("round_trip_ms", Json(nullptr))},
              {"accepted", latency.value("accepted", Json(nullptr))}};
          std::fprintf(stderr, "%s\n", result.dump().c_str());
        }
      }
      std::this_thread::sleep_for(10ms);
    }
    input_authorized_at_ms.store(0);
    keyboard.withdraw();
    flush_dc_trace();
    input_worker.request_stop();
    if (input_worker.joinable()) input_worker.join();
    flush_guard_trace();
    if (courier) courier->retire();
    std::array<Samples, 2> gaps;
    { std::lock_guard lock(render->mutex); gaps = render->submission_gaps; }
    const Json summary{{"event", "native_mock_render_summary"}, {"scope", "foreground GDI submission intervals, not capture-to-screen"},
        {"streams", Json::array({gaps[0].json(), gaps[1].json()})}, {"safety_stop_episodes", safety_stop_episodes}};
    std::fprintf(stderr, "%s\n", summary.dump().c_str()); std::fflush(stderr);
  }

  void connect(std::string driver_path, std::string vehicle_path, std::string secret,
               std::stop_token stop_token) {
    const char* phase = "configuration";
    try {
      const auto vehicle = load_vehicle_config(vehicle_path);
      if (mock_control) validate_mock_control(vehicle);
      else validate(vehicle);  // Local configuration is not remote containment evidence.
      if (vehicle.enabled_cameras().size() > video.size())
        throw std::runtime_error("viewer supports at most two camera tracks");
      auto render = std::make_shared<RenderSession>();
      std::vector<std::string> critical;
      for (const auto& camera : vehicle.enabled_cameras()) {
        render->cameras.push_back(camera.id);
        if (camera.critical_for_control) critical.push_back(camera.id);
      }
      render->panels = video;
      if (!critical.empty()) {
        render->health = std::make_shared<detail::NativeVideoHealth>(critical, vehicle.runtime.critical_camera_stale_brake_ms);
        for (const auto& id : critical) static_cast<void>(render->health->attach(id, 1));
      }
      { std::lock_guard lock(render_mutex); render_session = render; }
      auto config = load_driver_config(driver_path);
      if (mock_control && !config.signaling_url.starts_with("ws://127.0.0.1:"))
        throw std::runtime_error("mock control requires an isolated loopback signaling tunnel");
      config.receive_only = !mock_control;
      config.defer_control_bootstrap = mock_control;
      config.max_time_sync_uncertainty_ms = vehicle.field_safety.max_time_sync_uncertainty_ms;
      config.time_sync_interval_ms = vehicle.field_safety.time_sync_interval_ms;
      config.time_sync_samples = vehicle.field_safety.time_sync_samples;
      // Only the explicitly enabled isolated mock trace may write control
      // diagnostics; ordinary receive-only viewing remains log-free.
      if (!mock_control || !config.control_trace_commands) config.browser_event_log_path.clear();
      DriverConsoleRuntime driver(config, vehicle.vehicle_id, secret);
      std::fill(secret.begin(), secret.end(), '\0');
      bool connected = false;
      try {
        report(mock_control ? L"正在连接隔离 mock 台架；等待安全条件。" : L"正在登录并连接车端；仅观看，控制禁用。");
        phase = "login";
        static_cast<void>(driver.login());
        if (stop_token.stop_requested()) throw std::runtime_error("cancelled");
        phase = "connect";
        const auto session = driver.connect(vehicle.vehicle_id, "windows-native-viewer");
        connected = true;
        const auto ice = driver.ice_servers().at("ice_servers");
        static_cast<void>(driver.send_media_capabilities({{"codecs", {"h264"}}}));
        Json scope{{"source_session_id", session.at("session_id")},
                   {"source_control_session_generation", session.at("control_session_generation")}};
        std::unique_ptr<Receiver> receiver;
        std::shared_ptr<detail::NativeMockMediaGate> mock_gate;
        std::jthread control_worker;
        std::deque<Json> early;
        std::string negotiation;
        auto next_observation = Clock::now();
        std::vector<std::uint64_t> previous_draws;
        while (!stop_token.stop_requested()) {
          phase = "receiver_check";
          if (receiver) receiver->check();
          phase = "signaling_poll";
          const auto polled = driver.poll_signaling();
          for (const auto& message : polled.at("messages")) {
            const auto type = message.value("type", "");
            const auto payload = message.value("payload", Json::object());
            if (type == "webrtc_offer") {
              phase = "offer";
              if (receiver) throw std::runtime_error("renegotiation requires a new viewer session");
              Options options;
              options.display = false;
              options.ice_servers = payload.value("ice_servers", ice);
              options.ice_route_mode = payload.value("ice_route_mode", "all");
              if (mock_control) {
                options.local_addresses = {mock_local_ip};
                options.remote_addresses = {mock_peer_ip};
                options.address_family = mock_ip_family;
                options.ice_transport = "udp";
                options.ice_route_mode = "direct";
              }
              negotiation = payload.at("media_negotiation_id").get<std::string>();
              if (payload.value("control_transport_capability", "") == detail::kControlTransportCapability)
                options.control_scope = detail::NativeMediaScope{
                    session.at("session_id").get<std::string>(),
                    session.at("control_session_generation").get<std::uint64_t>(), negotiation};
              for (const auto& camera : vehicle.enabled_cameras()) options.camera_ids.push_back(camera.id);
              options.software_frame = [render](const std::string& camera, GstSample* sample) { render->accept(camera, sample); };
              receiver = std::make_unique<Receiver>(options);
              receiver->start();
              auto answer = scope;
              answer.update({{"type", "answer"}, {"sdp", receiver->answer(payload)}, {"media_negotiation_id", negotiation}});
              static_cast<void>(driver.send_webrtc_answer(answer));
              for (const auto& candidate : early) receiver->candidate(candidate);
              early.clear();
              if (mock_control) {
                if (!receiver->control_channel()) throw std::runtime_error("mock control capability not negotiated");
                mock_gate = std::make_shared<detail::NativeMockMediaGate>(
                    session.at("session_id").get<std::string>(), vehicle.vehicle_id, negotiation, render->cameras);
                auto* target = receiver.get();
                control_worker = std::jthread([this, &driver, target, vehicle, config, session, negotiation, render,
                                                gate = mock_gate, stop_token](std::stop_token token) {
                  try { run_mock_control(driver, *target, vehicle, config, session, negotiation, render, gate, stop_token, token); }
                  catch (const std::exception& error) {
                    const std::string_view detail = error.what();
                    const char* category = detail.find("mock input worker failed") != std::string_view::npos ? "input_worker" :
                        detail.find("mock courier retired") != std::string_view::npos ? "courier_retired" :
                        detail.find("mock control profile inactive or rejected") != std::string_view::npos ? "profile_inactive" :
                        detail.find("mock control profile changed") != std::string_view::npos ? "profile_changed" :
                        detail.find("retired page") != std::string_view::npos ? "retired_owner" :
                        detail.find("old session") != std::string_view::npos ? "session_changed" :
                        dynamic_cast<const Json::exception*>(&error) ? "json" : "other";
                    std::fprintf(stderr, "native_mock_control_failure category=%s\n", category); std::fflush(stderr);
                    report(L"Mock 控制终止：安全条件或控制通道失效。");
                    try { static_cast<void>(driver.end_session("native_mock_control_error")); } catch (...) {}
                  } catch (...) {
                    std::fprintf(stderr, "native_mock_control_failure category=unknown\n"); std::fflush(stderr);
                    report(L"Mock 控制终止：安全条件或控制通道失效。");
                    try { static_cast<void>(driver.end_session("native_mock_control_error")); } catch (...) {}
                  }
                });
              }
              report(mock_control ? L"已提交媒体应答，等待双路和 mock 安全条件。" : L"已提交媒体应答，等待画面；仅观看，控制禁用。");
            } else if (type == "ice_candidate") {
              phase = "remote_candidate";
              if (receiver) receiver->candidate(payload);
              else if (early.size() < 256) early.push_back(payload);
              else throw std::runtime_error("early ICE candidate limit reached");
            } else if (type == "media_status" && mock_gate) {
              const auto status = driver.status(true);
              const auto& sync = status.at("time_sync");
              const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
              const auto uncertainty = sync.value("uncertainty_ms", std::int64_t{999});
              const auto now_ms = static_cast<std::int64_t>(bench_now_ms());
              static_cast<void>(mock_gate->observe(payload, now_ms, wall_ms + sync.value("offset_ms", std::int64_t{0}),
                  sync.value("synchronized", false) ? uncertainty : 999));
            }
          }
          if (receiver) {
            // Receive-only observation: no result or command is sent to the
            // runtime until the scoped control courier is connected.
            if (!mock_control) if (auto* channel = receiver->control_channel())
              while (channel->pop()) {}
            phase = "local_candidates";
            for (const auto& candidate : receiver->candidates()) {
              auto outgoing = scope;
              outgoing.update({{"candidate", candidate}, {"media_negotiation_id", negotiation}});
              static_cast<void>(driver.send_webrtc_ice_candidate(outgoing));
            }
            if (Clock::now() >= next_observation) {
              phase = "observation";
              std::vector<std::uint64_t> current;
              { std::lock_guard lock(render->mutex);
                for (std::size_t index = 0; index < render->cameras.size(); ++index) current.push_back(render->submissions[index]); }
              const bool advancing = detail::viewer_draws_advance(current, previous_draws, vehicle.enabled_cameras().size());
              previous_draws = std::move(current);
              const auto pair = receiver->selected_pair();
              std::wstring path = L"路径未确认";
              if (pair.is_object()) {
                switch (detail::viewer_path(pair.value("local_type", ""), pair.value("remote_type", ""),
                                             pair.value("local_transport", ""), pair.value("remote_transport", ""))) {
                  case detail::ViewerPath::relay: path = L"中继"; break;
                  case detail::ViewerPath::direct_udp: path = L"UDP 直连"; break;
                  case detail::ViewerPath::other: path = L"非 UDP 路径"; break;
                  case detail::ViewerPath::unknown: break;
                }
              }
              report((advancing ? L"渲染提交持续更新 · " : L"等待新画面或画面停滞 · ") + path +
                  (mock_control ? L" · 隔离 mock，零速" : L" · 仅观看，控制禁用"));
              next_observation = Clock::now() + 1s;
            }
          }
          std::this_thread::sleep_for(10ms);
        }
        if (control_worker.joinable()) { control_worker.request_stop(); control_worker.join(); }
        if (receiver) receiver->stop();
      } catch (...) {
        if (connected) { try { static_cast<void>(driver.end_session("native_viewer_error")); } catch (...) {} }
        throw;
      }
      if (connected) { try { static_cast<void>(driver.end_session("native_viewer_disconnect")); } catch (...) {} }
      report(mock_control ? L"已断开 mock 台架。" : L"已断开；控制始终禁用。");
    } catch (const std::exception& error) {
      // Report only a fixed category and phase: network errors can contain
      // addresses or credentials and must not be copied into diagnostic logs.
      const std::string_view detail = error.what();
      const char* category = detail.find("time synchronization uncertainty") != std::string_view::npos ? "clock_uncertainty" :
          detail.find("time sync") != std::string_view::npos ? "time_sync" :
          detail.find("HTTP status 401") != std::string_view::npos ? "unauthorized" :
          detail.find("native view has no matching media pad") != std::string_view::npos ? "unmapped_pad" :
          detail.find("decode track pad link noformat") != std::string_view::npos ? "track_link_noformat" :
          detail.find("decode track pad link failed") != std::string_view::npos ? "track_link_other" :
          detail.find("decode track state sync failed") != std::string_view::npos ? "track_state_sync" :
          detail.find("GStreamer promise timed out") != std::string_view::npos ? "promise_timeout" : "other";
      std::fprintf(stderr, "native_viewer_failure phase=%s category=%s\n", phase, category);
      std::fill(secret.begin(), secret.end(), '\0');
      report(stop_token.stop_requested() ? L"已取消连接。" :
          L"连接或视频失败。请检查配置、台架隔离、信令和 GStreamer 依赖。");
    }
    { std::lock_guard lock(render_mutex); render_session.reset(); }
    for (const auto handle : video) InvalidateRect(handle, nullptr, TRUE);
    running = false;
    PostMessageW(hwnd, WM_APP + 1, 0, 0);
  }
};

HWND control(HWND parent, const wchar_t* type, const wchar_t* caption, DWORD style,
             int x, int y, int w, int h, int id = 0) {
  return CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

LRESULT CALLBACK panel_procedure(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* app = reinterpret_cast<Window*>(GetWindowLongPtrW(GetParent(hwnd), GWLP_USERDATA));
  if (msg == WM_LBUTTONDOWN && app && app->mock_control) { SetFocus(app->hwnd); return 0; }
  if (msg == WM_ERASEBKGND) return 1;
  if (msg == WM_PAINT && app) {
    std::shared_ptr<RenderSession> render;
    { std::lock_guard lock(app->render_mutex); render = app->render_session; }
    const auto id = GetDlgCtrlID(hwnd);
    if (render && id >= 10 && id <= 11) render->paint(static_cast<std::size_t>(id - 10), hwnd, app->hwnd);
    else { PAINTSTRUCT ps{}; auto* dc = BeginPaint(hwnd, &ps); RECT rect{}; GetClientRect(hwnd, &rect);
      if (dc) FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); EndPaint(hwnd, &ps); }
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK procedure(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  auto* app = reinterpret_cast<Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (msg == WM_NCCREATE) {
    app = static_cast<Window*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
    app->hwnd = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
  }
  if (!app) return DefWindowProcW(hwnd, msg, wparam, lparam);
  switch (msg) {
    case WM_CREATE:
      control(hwnd, L"STATIC", L"控制端配置", 0, 12, 14, 90, 22);
      app->driver_config = control(hwnd, L"EDIT", L"configs/driver-console.dev.yaml", WS_BORDER | ES_AUTOHSCROLL, 102, 10, 430, 24);
      control(hwnd, L"STATIC", L"车端台架配置", 0, 12, 46, 90, 22);
      app->vehicle_config = control(hwnd, L"EDIT", L"configs/vehicle-agent.orin-cpu-bench.yaml", WS_BORDER | ES_AUTOHSCROLL, 102, 42, 430, 24);
      control(hwnd, L"STATIC", L"驾驶员密码", 0, 12, 78, 90, 22);
      app->password = control(hwnd, L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 102, 74, 230, 24);
      app->start = control(hwnd, L"BUTTON", L"连接视频", BS_PUSHBUTTON, 345, 73, 88, 26, 1);
      app->stop = control(hwnd, L"BUTTON", L"断开", BS_PUSHBUTTON, 444, 73, 88, 26, 2);
      if (app->mock_control)
        control(hwnd, L"STATIC", L"仅 mock：W/↑ 油门 · A/D 转向 · S/空格 制动", 0, 545, 78, 390, 22);
      EnableWindow(app->stop, FALSE);
      app->status = control(hwnd, L"STATIC", app->message.c_str(), 0, 12, 108, 900, 25);
      app->video[0] = control(hwnd, L"MineTeleopNativePanel", L"", 0, 12, 140, 460, 300, 10);
      app->video[1] = control(hwnd, L"MineTeleopNativePanel", L"", 0, 480, 140, 460, 300, 11);
      SetTimer(hwnd, 1, 1000, nullptr);
      if (app->mock_control) SetTimer(hwnd, 2, 50, nullptr);
      return 0;
    case WM_SIZE: {
      if (wparam == SIZE_MINIMIZED) { app->keyboard.withdraw(); app->suspend_render_health(); }
      const int width = LOWORD(lparam), height = HIWORD(lparam);
      const int panel_width = std::max(1, (width - 36) / 2), panel_height = std::max(1, height - 152);
      MoveWindow(app->video[0], 12, 140, panel_width, panel_height, TRUE);
      MoveWindow(app->video[1], 24 + panel_width, 140, panel_width, panel_height, TRUE);
      return 0;
    }
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) { app->keyboard.withdraw(); app->suspend_render_health(); }
      else for (const auto panel : app->video) InvalidateRect(panel, nullptr, FALSE);
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == 1 && !app->running.exchange(true)) {
        try {
          if (app->worker.joinable()) app->worker.join();
          auto secret = text_of(app->password);
          auto driver = utf8(text_of(app->driver_config)), vehicle = utf8(text_of(app->vehicle_config));
          auto encoded = utf8(secret);
          SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
          SetWindowTextW(app->password, L"");
          if (encoded.empty()) throw std::runtime_error("password required");
          EnableWindow(app->start, FALSE); EnableWindow(app->stop, TRUE);
          app->worker = std::jthread([app, driver = std::move(driver), vehicle = std::move(vehicle),
                                      encoded = std::move(encoded)](std::stop_token token) mutable {
            app->connect(std::move(driver), std::move(vehicle), std::move(encoded), token);
          });
          if (app->mock_control) SetFocus(hwnd);
        } catch (...) { app->running = false; app->report(L"请输入有效的配置路径和密码。"); }
      } else if (LOWORD(wparam) == 2 && app->worker.joinable()) {
        app->keyboard.withdraw();
        app->worker.request_stop(); app->report(L"正在断开视频会话……");
      }
      return 0;
    case WM_APP + 1: {
      std::lock_guard lock(app->mutex);
      SetWindowTextW(app->status, app->message.c_str());
      EnableWindow(app->start, !app->running.load());
      EnableWindow(app->stop, app->running.load());
      return 0;
    }
    case WM_TIMER:
      if (wparam == 2 && app->mock_control) {
        const auto focus = GetFocus();
        const bool focused = app->running.load() && IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
            GetForegroundWindow() == hwnd && (focus == hwnd || focus == app->video[0] || focus == app->video[1]);
        const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        app->keyboard.observe_now(focused,
            {down('W') || down(VK_UP), down('A') || down(VK_LEFT), down('D') || down(VK_RIGHT),
             down('S') || down(VK_DOWN) || down(VK_SPACE)});
        return 0;
      }
      if (wparam == 1 && app->running.load() && detail::viewer_observation_stale(
          static_cast<std::int64_t>(bench_now_ms()), app->last_observation_ms.load()))
        SetWindowTextW(app->status, L"状态采样中断，画面可能停滞；仅观看，控制禁用。");
      return 0;
    case WM_CLOSE:
      KillTimer(hwnd, 1);
      KillTimer(hwnd, 2);
      app->keyboard.withdraw();
      if (app->worker.joinable()) { app->worker.request_stop(); app->worker.join(); }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    default: return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}
}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
  int argc = 0;
  auto** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv && argc == 3 && std::wstring(argv[1]) == L"--time-sync-probe") {
    int result = 1;
    try {
      const auto url = utf8(argv[2]);
      if (url != "http://127.0.0.1:38767") throw std::invalid_argument("isolated loopback time origin required");
      mine_teleop::HttpClient http;
      mine_teleop::SynchronizedClock clock;
      const auto status = clock.synchronize(http, url, 7, 25);
      const auto summary = mine_teleop::Json{{"synchronized", status.synchronized},
          {"uncertainty_ms", status.uncertainty_ms}, {"round_trip_ms", status.round_trip_ms},
          {"passed", status.acceptable(25)}};
      std::printf("%s\n", summary.dump().c_str()); std::fflush(stdout);
      result = status.acceptable(25) ? 0 : 1;
    } catch (...) { std::fprintf(stderr, "time_sync_probe_failed\n"); std::fflush(stderr); }
    LocalFree(argv);
    return result;
  }
  if (argv && argc == 3 && (std::wstring(argv[1]) == L"--validate-config" ||
                            std::wstring(argv[1]) == L"--validate-mock-config")) {
    int result = 0;
    try {
      const auto config = mine_teleop::load_vehicle_config(utf8(argv[2]));
      if (std::wstring(argv[1]) == L"--validate-mock-config") mine_teleop::bench::validate_mock_control(config);
      else mine_teleop::bench::validate(config);
      if (config.enabled_cameras().size() > 2) result = 1;
    } catch (...) { result = 1; }
    LocalFree(argv);
    return result;
  }
  const bool mock_control = argv && argc == 6 && std::wstring(argv[1]) == L"--mock-control" &&
      std::wstring(argv[2]) == L"--local-ip" && std::wstring(argv[4]) == L"--peer-ip";
  if (argc != 1 && !mock_control) { if (argv) LocalFree(argv); return 1; }
  std::string local_ip, peer_ip, family;
  if (mock_control) {
    try {
      g_networking_init();
      local_ip = utf8(argv[3]); peer_ip = utf8(argv[5]);
      const auto local = mine_teleop::detail::ice_address_summary(local_ip), remote = mine_teleop::detail::ice_address_summary(peer_ip);
      family = local.value("address_family", "");
      if (local.value("address_class", "") != "public" || remote.value("address_class", "") != "public" ||
          (family != "ipv4" && family != "ipv6") || remote.value("address_family", "") != family ||
          mine_teleop::bench::same_ip_address(local_ip, peer_ip)) throw std::runtime_error("invalid WAN endpoints");
    } catch (...) { LocalFree(argv); return 1; }
  }
  if (argv) LocalFree(argv);
  gst_init(nullptr, nullptr);
  Window app;
  app.mock_control = mock_control;
  app.mock_local_ip = std::move(local_ip); app.mock_peer_ip = std::move(peer_ip); app.mock_ip_family = std::move(family);
  WNDCLASSW panel{};
  panel.lpfnWndProc = panel_procedure; panel.hInstance = instance; panel.lpszClassName = L"MineTeleopNativePanel";
  if (!RegisterClassW(&panel)) return 1;
  WNDCLASSW klass{};
  klass.lpfnWndProc = procedure; klass.hInstance = instance; klass.lpszClassName = L"MineTeleopNativeViewer";
  klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassW(&klass)) return 1;
  const auto hwnd = CreateWindowExW(0, klass.lpszClassName,
      mock_control ? L"MineTeleop Windows 隔离 Mock 控制（零速）" : L"MineTeleop Windows 台架视频（仅观看）",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 530, nullptr, nullptr, instance, &app);
  if (!hwnd) return 1;
  ShowWindow(hwnd, show); UpdateWindow(hwnd);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  return static_cast<int>(message.wParam);
}
