// Windows bench viewer: native video with an explicitly isolated mock mode.
#include "native_receiver_core.hpp"
#include "mine_teleop/detail/native_video_health.hpp"
#include "mine_teleop/detail/native_mock_media_gate.hpp"
#include "mine_teleop/detail/native_mock_input_sequence.hpp"
#include "mine_teleop/detail/native_mock_profile_retry.hpp"
#include "mine_teleop/detail/native_mock_profile_state.hpp"
#include "mine_teleop/detail/native_mock_gear_transition.hpp"
#include "mine_teleop/detail/native_mock_keyboard.hpp"
#include "mine_teleop/detail/native_can_feedback.hpp"
#include "mine_teleop/detail/native_control_courier.hpp"
#include "mine_teleop/detail/native_viewer_observation.hpp"
#include "mine_teleop/detail/native_operator_readouts.hpp"
#include "mine_teleop/detail/native_identity_input.hpp"
#include "mine_teleop/detail/native_video_layout.hpp"
#include <gst/video/video.h>
#include <gio/gnetworking.h>
#include <windows.h>
#include <Xinput.h>
#include <shellapi.h>
#include <commdlg.h>
#include <algorithm>
#include <cstring>
#include <exception>
#include <cstdio>
#include <condition_variable>
#include <fstream>
#include <optional>
#include <string_view>

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

std::wstring wide(const std::string& value) {
  if (value.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0);
  if (count <= 0) throw std::runtime_error("invalid vehicle identity");
  std::wstring result(static_cast<std::size_t>(count), L'\0');
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), result.data(), count)) throw std::runtime_error("invalid vehicle identity");
  return result;
}

// Load the Windows system implementation rather than accepting a DLL from the
// working directory. Generic HID calibration is not supplied by XInput.
DWORD xinput_state(DWORD index, XINPUT_STATE* state) {
  using GetState = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);
  static const auto get_state = []() -> GetState {
    for (const auto* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
      if (const auto library = LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (const auto fn = GetProcAddress(library, "XInputGetState")) return reinterpret_cast<GetState>(fn);
        FreeLibrary(library);
      }
    }
    return nullptr;
  }();
  return get_state ? get_state(index, state) : ERROR_DEVICE_NOT_CONNECTED;
}

bool xinput_button(const XINPUT_GAMEPAD& pad, int index) {
  constexpr std::array<WORD, 16> buttons{XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
      XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER, 0, 0, XINPUT_GAMEPAD_BACK,
      XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_LEFT_THUMB, XINPUT_GAMEPAD_RIGHT_THUMB,
      XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT, XINPUT_GAMEPAD_DPAD_RIGHT};
  if (index == 6) return pad.bLeftTrigger >= 128;
  if (index == 7) return pad.bRightTrigger >= 128;
  return index >= 0 && static_cast<std::size_t>(index) < buttons.size() && (pad.wButtons & buttons[index]) != 0;
}

void draw_empty_video(HDC dc, RECT rect, const wchar_t* message) {
  FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, RGB(142, 160, 184));
  const auto previous = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
  DrawTextW(dc, message, -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(dc, previous);
}

// Each panel retains only the newest decoded frame. The video-health signal is
// emitted after GDI accepts the frame; it is a renderer submission, not proof
// of scan-out or of the vehicle capture timestamp.
struct RenderSession {
  struct Frame {
    std::string camera;
    std::uint64_t pts{};
    double decoded_at_ms{};
    int width{}, height{};
    std::vector<std::uint8_t> pixels;
  };
  std::mutex mutex;
  std::array<std::shared_ptr<Frame>, 2> latest{};
  std::array<std::uint64_t, 2> submissions{};
  std::array<std::optional<std::uint64_t>, 2> last_submitted{};
  std::array<double, 2> last_submission_ms{};
  std::array<double, 2> last_decoded_ms{};
  std::array<Samples, 2> submission_gaps{};
  std::array<Samples, 2> decoded_gaps{}, decoded_to_submission{};
  std::vector<std::string> cameras;
  std::shared_ptr<detail::NativeVideoHealth> health;
  std::array<HWND, 2> panels{};

  void accept(const std::string& camera, GstSample* sample) {
    const auto decoded_at_ms = bench_now_ms();
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
      frame->decoded_at_ms = decoded_at_ms;
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
      { std::lock_guard lock(mutex);
        if (last_decoded_ms[index] > 0) decoded_gaps[index].add(decoded_at_ms - last_decoded_ms[index]);
        last_decoded_ms[index] = decoded_at_ms;
        latest[index] = std::move(frame); }
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
      const auto fitted = detail::native_video_fit(frame->width, frame->height, rect.right, rect.bottom);
      FillRect(dc, &rect, reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      const int result = fitted ? StretchDIBits(dc, fitted->x, fitted->y, fitted->width, fitted->height,
          0, 0, frame->width, frame->height, frame->pixels.data(), &bitmap, DIB_RGB_COLORS, SRCCOPY) : 0;
      const bool visible = IsWindowVisible(parent) && !IsIconic(parent) && GetForegroundWindow() == parent;
      if (health) static_cast<void>(health->set_usable(frame->camera, 1, visible));
      if (result != 0 && result != static_cast<int>(GDI_ERROR) && GdiFlush() && visible) {
        if (health) static_cast<void>(health->render_submitted_now(frame->camera, 1, frame->pts));
        std::lock_guard lock(mutex);
        if (!last_submitted[index] || frame->pts > *last_submitted[index]) {
          const auto submitted_at = bench_now_ms();
          if (last_submission_ms[index] > 0) submission_gaps[index].add(submitted_at - last_submission_ms[index]);
          decoded_to_submission[index].add(submitted_at - frame->decoded_at_ms);
          last_submission_ms[index] = submitted_at;
          last_submitted[index] = frame->pts; ++submissions[index];
        }
      }
    } else if (dc) draw_empty_video(dc, rect, L"等待相机画面");
    EndPaint(panel, &ps);
  }
};

struct Window {
  HWND hwnd{}, driver_config{}, vehicle_config{}, driver_id{}, password{}, status{}, start{}, stop{}, vehicle_list{}, logs_button{}, can_detail{};
  std::array<HWND, 4> config_labels{};
  std::array<HWND, 6> operator_labels{}, operator_readouts{};
  std::array<HWND, 6> keycaps{};
  std::array<bool, 6> shown_keys{};
  HWND help{}, mock_info{}, mock_estop_button{};
  HFONT font{}, title_font{}, small_font{};
  HBRUSH background{}, surface{}, input_background{};
  std::array<HWND, 2> video{};
  std::mutex mutex;
  struct VehicleChoice { std::wstring label; bool eligible{}; std::string key; bool needs_approval{}; };
  std::mutex selection_mutex;
  std::condition_variable selection_ready;
  std::vector<VehicleChoice> vehicle_choices;
  bool awaiting_selection{}, selection_confirmed{};
  bool selection_logs_requested{}, selection_log_mode{}, selection_save_response{}, selection_busy{};
  std::size_t selection_index{};
  std::wstring selection_save_path;
  std::mutex intent_display_mutex;
  struct IntentDisplay { detail::NativeOperatorIntent input{}; std::int64_t observed_ms{}; bool valid{}; };
  IntentDisplay intent_display;
  std::mutex render_mutex;
  std::shared_ptr<RenderSession> render_session;
  std::wstring message{L"仅观看台架：控制禁用。车端隔离须独立核验。"};
  std::wstring mock_message{L"控制：等待连接\n画面：等待\n校时：等待"};
  std::wstring can_detail_message{L"CAN 反馈未知"};
  std::array<std::wstring, 6> operator_values{L"—", L"—", L"—", L"—", L"—", L"—"};
  std::array<std::wstring, 6> shown_operator_values{L"—", L"—", L"—", L"—", L"—", L"—"};
  std::wstring shown_mock_message{L"控制：等待连接\n画面：等待\n校时：等待"};
  bool operator_display_stale{false};
  std::jthread worker;
  std::atomic<bool> running{false};
  std::atomic<std::int64_t> last_observation_ms{0};
  std::atomic<std::int64_t> last_mock_observation_ms{0};
  std::atomic<bool> mock_estop_requested{false};
  std::atomic<bool> mock_profile_needs_confirmation{false}, mock_profile_confirmation_requested{false};
  std::atomic<bool> mock_profile_reconnect_required{false};
  std::atomic<bool> mock_gear_reconnect_required{false};
  std::atomic<bool> mock_ui_posted{false};
  detail::NativeMockKeyboard keyboard;
  std::mutex gamepad_mutex;
  GamepadConfig gamepad_config{.enabled = false};
  int gamepad_estop_hold_ms{500};
  std::int64_t gamepad_estop_since_ms{};
  DWORD gamepad_index{XUSER_MAX_COUNT};
  bool mock_control{false};
  std::string mock_local_ip, mock_peer_ip, mock_ip_family;

  // Called only on the UI thread. Unchanged text should not invalidate six
  // controls at every telemetry tick while both video panels are painting.
  void set_operator_value(std::size_t index, const std::wstring& value) {
    if (operator_readouts[index] && shown_operator_values[index] != value) {
      if (SetWindowTextW(operator_readouts[index], value.c_str())) shown_operator_values[index] = value;
    }
  }

  void set_mock_message(const std::wstring& value) {
    if (mock_info && shown_mock_message != value) {
      if (SetWindowTextW(mock_info, value.c_str())) shown_mock_message = value;
    }
  }

  void clear_input_display() {
    { std::lock_guard lock(intent_display_mutex); intent_display.valid = false; }
    for (std::size_t i = 0; i < shown_keys.size(); ++i) {
      shown_keys[i] = false;
      if (keycaps[i]) InvalidateRect(keycaps[i], nullptr, TRUE);
    }
    for (std::size_t i = 2; i < operator_readouts.size(); ++i)
      set_operator_value(i, L"—");
  }

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

  void report_mock(std::wstring value,
                   std::array<std::wstring, 6> values = {L"—", L"—", L"—", L"—", L"—", L"—"},
                   std::wstring can_details = L"CAN 反馈未知") {
    { std::lock_guard lock(mutex); mock_message = std::move(value); operator_values = std::move(values);
      can_detail_message = std::move(can_details); }
    last_mock_observation_ms = static_cast<std::int64_t>(bench_now_ms());
    if (!mock_ui_posted.exchange(true) && !PostMessageW(hwnd, WM_APP + 2, 0, 0))
      mock_ui_posted.store(false);
  }

  void request_mock_estop() {
    if (!mock_control || !running.load() || mock_estop_requested.exchange(true)) return;
    keyboard.withdraw();
    { std::lock_guard lock(intent_display_mutex); intent_display.valid = false; }
    report(L"已请求 mock 急停，等待车端确认。");
  }

  // Isolated bench mode is constrained to a mock adapter and zero speed. It
  // must not be used as a production driving interface.
  void run_mock_control(DriverConsoleRuntime& driver, Receiver& receiver,
                        const VehicleConfig& vehicle, const DriverConfig& config,
                        const Json& session, const std::string& attempt,
                        const std::shared_ptr<RenderSession>& render,
                        const std::shared_ptr<detail::NativeMockMediaGate>& gate,
                        bool wss_dc_mirror,
                        std::stop_token parent, std::stop_token stop) {
    const std::string session_id = session.at("session_id").get<std::string>();
    const auto generation = session.at("control_session_generation").get<std::uint64_t>();
    const std::string owner = "windows-native-mock";
    std::atomic<std::uint64_t> epoch{0};
    std::uint64_t profile_seq = 0;
    bool profile_accepted = false, handshake_sent = false;
    detail::NativeMockProfileRetry profile_retry;
    detail::NativeMockProfileState profile_state;
    std::optional<SessionControlProfile> requested_profile;
    std::optional<Json> profile_request;
    auto next_evidence = Clock::now();
    auto next_mock_ui = Clock::now();
    auto next_status = Clock::now();
    Json cached_status;
    Json telemetry;
    std::optional<bool> last_authorization_diagnostic;
    std::int64_t telemetry_at_ms = 0, telemetry_source_ms = 0;
    std::uint64_t telemetry_seq = 0, safety_stop_episodes = 0, last_result_seq = 0;
    std::uint64_t telemetry_status_observed = 0;
    std::uint64_t telemetry_wire_observed = 0, telemetry_wire_last_seq = 0;
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
    std::uint64_t mirrored_media_received = 0, mirrored_media_ready_after = 0;
    auto* channel = receiver.control_channel();
    if (!channel || !render->health) throw std::runtime_error("mock control requires scoped channel and critical video");
    // The input lease must not wait for signaling or the transport bridge.
    // The sender independently checks its current clock before publication.
    std::atomic<bool> input_failed{false};
    std::atomic<std::int64_t> clock_observed_ms{0}, input_authorized_at_ms{0};
    std::atomic<char> mock_confirmed_gear{'N'};
    std::atomic<char> mock_gear_rollback{0};
    std::mutex gear_mutex;
    detail::NativeMockGearTransition gear_transition;
    // Clock health must not depend on a courier/bridge cycle completing. The
    // observation age is measured from the start of the actual snapshot, so a
    // blocked clock read cannot make old evidence look fresh.
    std::jthread clock_worker([&](std::stop_token clock_stop) {
      while (!clock_stop.stop_requested() && !stop.stop_requested() && !parent.stop_requested()) {
        const auto observed_at = static_cast<std::int64_t>(bench_now_ms());
        try {
          const auto snapshot = driver.time_sync_snapshot();
          const bool healthy = snapshot.clock.synchronized && snapshot.clock.uncertainty_ms >= 0 &&
              snapshot.clock.uncertainty_ms <= 25 && snapshot.readiness.ready;
          clock_observed_ms.store(healthy ? observed_at : 0);
        } catch (...) { clock_observed_ms.store(0); }
        std::this_thread::sleep_for(25ms);
      }
      clock_observed_ms.store(0);
    });
    // Keep diagnostics off the lease worker's I/O path and bound memory if
    // the observation thread stalls. -1 means a short-circuited check.
    struct GuardState {
      bool focused, ui_alive, clock_ok, video_ready, video_sample_healthy, authorized, ready;
      int remote_ready, direct_ready;
      std::uint64_t bad_revision, brake_revision;
      detail::NativeVideoHealth::FaultCounts video_faults;
      bool operator==(const GuardState&) const = default;
    };
    struct GuardEvent { std::int64_t at, authorization_at, authorization_now; GuardState state; };
    std::mutex guard_mutex;
    std::vector<GuardEvent> guard_events;
    std::uint64_t guard_dropped = 0;
    struct InputEvent { std::int64_t at, wall_ms; std::uint64_t intent_seq; unsigned keys; char gear; };
    std::mutex input_trace_mutex;
    std::vector<InputEvent> input_events;
    std::uint64_t input_trace_dropped = 0;
    const auto flush_guard_trace = [&] {
      if (!config.control_trace_commands) return;
      std::vector<GuardEvent> events;
      std::uint64_t dropped = 0;
      { std::lock_guard lock(guard_mutex); events.swap(guard_events); std::swap(dropped, guard_dropped); }
      if (!events.empty() || dropped) {
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
              {"bad_revision", s.bad_revision}, {"brake_revision", s.brake_revision},
              {"video_faults", {{"unusable", s.video_faults.unusable}, {"stale", s.video_faults.stale},
                  {"invalid_frame", s.video_faults.invalid_frame}, {"replaced", s.video_faults.replaced},
                  {"explicit_fault", s.video_faults.explicit_fault}, {"clock", s.video_faults.clock}}}});
        }
        std::fprintf(stderr, "%s\n", Json{{"event", "native_mock_guard_batch"}, {"records", std::move(records)},
            {"dropped", dropped}}.dump().c_str());
      }
      std::vector<InputEvent> inputs;
      dropped = 0;
      { std::lock_guard lock(input_trace_mutex); inputs.swap(input_events); std::swap(dropped, input_trace_dropped); }
      if (!inputs.empty() || dropped) {
        Json input_records = Json::array();
        for (const auto& event : inputs) input_records.push_back({{"monotonic_ms", event.at}, {"wall_utc_ms", event.wall_ms},
            {"intent_seq", event.intent_seq}, {"keys", event.keys}, {"gear", std::string(1, event.gear)}});
        std::fprintf(stderr, "%s\n", Json{{"event", "native_mock_input_batch"}, {"session_id", session_id}, {"records", std::move(input_records)},
            {"dropped", dropped}}.dump().c_str());
      }
    };
    std::jthread input_worker([&](std::stop_token input_stop) {
      try {
        detail::NativeMockInputSequence input_sequence;
        bool withdrawn = false;
        std::optional<GuardState> previous_guard;
        std::int64_t last_input_trace_at = 0;
        unsigned last_input_keys = 256;
        while (!input_stop.stop_requested() && !stop.stop_requested() && !parent.stop_requested()) {
          const auto clock_at = clock_observed_ms.load();
          const auto now_ms = static_cast<std::int64_t>(bench_now_ms());
          const bool clock_ok = detail::native_mock_clock_fresh(clock_at, now_ms);
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
          const bool authorized = detail::native_mock_input_fresh(authorized_at, authorization_now_ms) &&
              !mock_gear_reconnect_required.load();
          if (config.control_trace_commands) {
            const GuardState state{focused, ui_alive, clock_ok, video_health.ready, video_health.sample_healthy,
                authorized, ready, remote_ready, direct_ready, video_health.bad_revision, video_health.brake_revision,
                video_health.faults};
            if (!previous_guard || *previous_guard != state) {
              std::lock_guard lock(guard_mutex);
              if (guard_events.size() < 128) guard_events.push_back({now_ms, authorized_at, authorization_now_ms, state});
              else ++guard_dropped;
              previous_guard = state;
            }
          }
          const auto confirmed_gear = mock_confirmed_gear.load();
          auto keyboard_input = keyboard.sample_now(ready && authorized && !mock_estop_requested.load(), confirmed_gear);
          std::optional<std::uint64_t> gear_generation;
          bool withdraw_gear_input = false;
          const auto rollback = mock_gear_rollback.load();
          {
            std::lock_guard lock(gear_mutex);
            if (!ready || !authorized || mock_estop_requested.load()) {
              gear_transition.cancel();
              mock_gear_rollback.store(0);
            } else if (rollback) {
              keyboard_input.gear = rollback;
              keyboard_input.throttle = keyboard_input.steering = 0.0;
              keyboard_input.key_mask = 0; keyboard_input.armed = false;
              if (const auto pending = gear_transition.pending()) gear_generation = pending->generation;
            } else if (keyboard_input.armed) {
              auto pending = gear_transition.pending();
              if (!pending && keyboard_input.gear != confirmed_gear) {
                gear_generation = gear_transition.begin(confirmed_gear, keyboard_input.gear);
                pending = gear_transition.pending();
                if (!pending) { withdraw_gear_input = true; keyboard_input.gear = confirmed_gear; }
              }
              if (pending) {
                gear_generation = pending->generation;
                if (keyboard_input.gear != pending->to && (keyboard_input.key_mask & 3U))
                  withdraw_gear_input = true;
                keyboard_input.gear = pending->to;
                keyboard_input.throttle = keyboard_input.steering = 0.0;
              }
            } else {
              // An active session retains its confirmed gear while input is
              // withdrawn; an unready session still uses the existing neutral.
              if (const auto pending = gear_transition.pending()) {
                keyboard_input.gear = pending->to;
                keyboard_input.throttle = keyboard_input.steering = 0.0;
                gear_generation = pending->generation;
              } else keyboard_input.gear = confirmed_gear;
            }
          }
          if (withdraw_gear_input) {
            keyboard.withdraw();
            keyboard_input.throttle = keyboard_input.steering = 0.0;
            keyboard_input.key_mask = 0; keyboard_input.armed = false;
          }
          const auto current_epoch = epoch.load();
          const bool estop = mock_estop_requested.load();
          if (estop || ((ready || current_epoch != 0) && (ui_alive || !withdrawn))) {
            Json input{{"session_id", session_id}, {"session_generation", generation},
                {"ui_instance_id", owner}, {"ui_control_epoch", current_epoch}, {"takeover", !estop && current_epoch == 0},
                {"gear", std::string(1, keyboard_input.gear)}, {"steering", keyboard_input.steering},
                {"throttle", keyboard_input.throttle}, {"brake", keyboard_input.brake},
                {"estop", estop}, {"critical_video_ready", ready},
                {"critical_video_sample_healthy", ready && video_health.sample_healthy},
                {"critical_video_bad_sample_revision", video_health.bad_revision},
                {"critical_video_brake_revision", video_health.brake_revision}};
            // Different key states can produce the same zero-limited intent.
            // Give them distinct local revisions without sending the key mask.
            auto input_identity = input;
            input_identity["mock_key_mask"] = keyboard_input.key_mask;
            input["intent_seq"] = input_sequence.next(input_identity);
            const auto result = driver.update_control_intent(input);
            if (!result.value("accepted", false)) throw std::runtime_error("mock input rejected");
            if (gear_generation && result.contains("intent_seq") && result.at("intent_seq").is_number_unsigned()) {
              std::lock_guard lock(gear_mutex);
              gear_transition.forwarded(*gear_generation, result.at("intent_seq").get<std::uint64_t>(), keyboard_input.gear);
            }
            if (config.control_trace_commands && keyboard_input.armed &&
                (keyboard_input.key_mask != last_input_keys || now_ms - last_input_trace_at >= 1000)) {
              std::lock_guard lock(input_trace_mutex);
              const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch()).count();
              if (input_events.size() < 128) input_events.push_back({now_ms, wall_ms,
                  input.at("intent_seq").get<std::uint64_t>(), keyboard_input.key_mask, keyboard_input.gear});
              else ++input_trace_dropped;
              last_input_keys = keyboard_input.key_mask; last_input_trace_at = now_ms;
            }
            {
              std::lock_guard lock(intent_display_mutex);
              intent_display = {{keyboard_input.gear, keyboard_input.steering, keyboard_input.throttle,
                                  keyboard_input.brake}, static_cast<std::int64_t>(bench_now_ms()),
                                ready && authorized && !estop};
            }
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
      const auto event = value.value("event", "");
      const auto status_seq = value.contains("control_status_seq") && value.at("control_status_seq").is_number_unsigned()
          ? value.at("control_status_seq").get<std::uint64_t>() : 0;
      bool gear_status_new = false;
      { std::lock_guard lock(gear_mutex); gear_status_new = gear_transition.observe_status(status_seq); }
      if (gear_status_new && event == "control_command_rejected" &&
          value.value("issue_code", "") == "control_input_rearm_required") {
        {
          std::lock_guard lock(gear_mutex);
          gear_transition.cancel();
          mock_gear_rollback.store(0);
        }
        keyboard.require_input_rearm();
        report(L"控制命令短暂中断，输入已清除并保持当前挡位；请释放方向键后重新按下。");
      }
      if (gear_status_new && event == "control_command_rejected" &&
          value.value("issue_code", "") == "vcu_drive_gear_change_moving_or_stale") {
        const auto positive_seq = [&](const char* field) -> std::uint64_t {
          return value.contains(field) && value.at(field).is_number_unsigned()
              ? value.at(field).get<std::uint64_t>() : 0;
        };
        auto intent = positive_seq("intent_seq");
        if (!intent) intent = positive_seq("command_seq");
        bool matched = false;
        {
          std::lock_guard lock(gear_mutex);
          const auto pending = gear_transition.pending();
          matched = pending && !mock_gear_rollback.load() && gear_transition.reject(status_seq, intent) ==
              detail::NativeMockGearTransition::Rejection::matched;
          if (matched) {
            mock_confirmed_gear.store(pending->from);
          }
          gear_transition.cancel();
          if (matched) {
            if (gear_transition.begin(pending->to, pending->from)) mock_gear_rollback.store(pending->from);
            else matched = false;
          }
        }
        keyboard.withdraw();
        if (matched) report(L"车端拒绝换挡；已清除输入并请求回退，请释放后重新操作。");
        else {
          input_authorized_at_ms.store(0);
          mock_gear_reconnect_required.store(true);
          report(L"换挡拒绝无法关联，普通输入已冻结；请安全断开并重新连接。");
        }
      }
      if (gear_status_new && event == "vehicle_telemetry" && value.contains("can_feedback") &&
          value.at("can_feedback").is_object() && cached_status.is_object() &&
          cached_status.contains("time_sync") && cached_status.at("time_sync").is_object()) {
        const auto& clock = cached_status.at("time_sync");
        const auto source = value.contains("sent_at_utc_ms") && value.at("sent_at_utc_ms").is_number_integer()
            ? value.at("sent_at_utc_ms").get<std::int64_t>() : 0;
        const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const bool fresh = clock.value("synchronized", false) &&
            detail::native_mock_source_fresh(source, wall, clock.value("offset_ms", std::int64_t{0}),
                clock.value("uncertainty_ms", std::int64_t{-1}));
        const auto actual = detail::native_feedback_confirmed_gear(value);
        if (fresh && actual) {
          std::lock_guard lock(gear_mutex);
          if (!gear_transition.pending() || gear_transition.confirm(status_seq, *actual, true)) {
            mock_confirmed_gear.store(*actual);
            if (mock_gear_rollback.load() == *actual) mock_gear_rollback.store(0);
          }
        }
      }
      Json profile_status;
      if (value.value("event", "") == "session_control_profile_status") profile_status = value;
      else if (value.value("event", "") == "vehicle_telemetry" && value.contains("session_control_profile"))
        profile_status = value.at("session_control_profile");
      if (profile_seq && profile_status.is_object() && value.contains("control_status_seq") &&
          value.at("control_status_seq").is_number_unsigned()) {
        const auto request = profile_status.contains("last_request_seq") &&
            profile_status.at("last_request_seq").is_number_unsigned()
            ? profile_status.at("last_request_seq").get<std::uint64_t>() : 0;
        bool valid = profile_status.value("accepted", false) && profile_status.value("active", false) &&
            profile_status.contains("applied_revision") && profile_status.at("applied_revision").is_number_unsigned() &&
            profile_status.at("applied_revision").get<std::uint64_t>() == profile_seq && requested_profile &&
            profile_status.contains("effective_profile") && profile_status.at("effective_profile").is_object();
        if (valid) {
          try { valid = !(SessionControlProfile::from_json(profile_status.at("effective_profile")) != *requested_profile); }
          catch (const std::exception&) { valid = false; }
        }
        const auto result = profile_state.observe(value.at("control_status_seq").get<std::uint64_t>(), request, valid);
        if (result == detail::NativeMockProfileState::Result::invalidated) {
          profile_accepted = false;
          input_authorized_at_ms.store(0);
          keyboard.withdraw();
          mock_profile_confirmation_requested.store(false);
          mock_profile_needs_confirmation.store(true);
          report(L"会话限幅已失效，输入已禁止；请人工重新确认。");
        } else if (result == detail::NativeMockProfileState::Result::accepted) {
          keyboard.set_brake_profile(requested_profile->service_brake_pressure_bar,
              requested_profile->hard_brake_pressure_bar, requested_profile->max_brake_pressure_bar);
          keyboard.set_control_limits(0.0, requested_profile->max_steering_angle_deg, 30.0);
          profile_accepted = true;
        }
      }
      if (value.value("event", "") == "vehicle_telemetry") {
        ++telemetry_status_observed;
        const auto seq = value.value("seq", std::uint64_t{0});
        if (seq <= telemetry_seq || !value.contains("control_timing") || !value.at("control_timing").is_object()) return;
        telemetry_seq = seq;
        telemetry_at_ms = static_cast<std::int64_t>(bench_now_ms());
        telemetry_source_ms = value.contains("sent_at_utc_ms") && value.at("sent_at_utc_ms").is_number_integer()
            ? value.at("sent_at_utc_ms").get<std::int64_t>() : 0;
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
            {"can_feedback", value.contains("can_feedback") && value.at("can_feedback").is_object()
                ? value.at("can_feedback") : Json(nullptr)},
            {"speed_mps", value.contains("speed_mps") && value.at("speed_mps").is_number()
                ? value.at("speed_mps") : Json(nullptr)}};
      }
    };
    std::exception_ptr control_failure;
    try { while (!stop.stop_requested() && !parent.stop_requested()) {
      const auto now = Clock::now();
      const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
      if (now >= next_status) { cached_status = driver.status(true); next_status = now + 25ms; }
      const auto& status = cached_status;
      const auto& sync = status.at("time_sync");
      const bool clock_ok = sync.value("synchronized", false) && sync.value("uncertainty_ms", 999) <= 25 &&
          sync.value("uncertainty_ms", -1) >= 0 && status.at("time_sync_readiness").value("ready", false);
      const bool direct_udp = channel->direct_udp(mock_local_ip, mock_peer_ip);
      const auto receive = [&]() -> std::optional<std::string> {
        while (auto bytes = channel->pop()) {
          if (bytes->size() <= 4096 && bytes->find("mock_media_status_v1") != std::string::npos) {
            const auto envelope = Json::parse(*bytes, nullptr, false);
            if (envelope.is_object() && envelope.value("event", "") == "mock_media_status_v1") {
              if (direct_udp) {
                ++mirrored_media_received;
                const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                if (gate->observe(envelope.value("status", Json::object()),
                    static_cast<std::int64_t>(bench_now_ms()), wall_ms + sync.value("offset_ms", std::int64_t{0}),
                    clock_ok ? sync.value("uncertainty_ms", std::int64_t{999}) : 999)) ++mirrored_media_ready_after;
              }
              continue;
            }
          }
          return bytes;
        }
        return std::nullopt;
      };
      const auto video_health = render->health->snapshot_now();
      const bool focused = IsWindowVisible(hwnd) && !IsIconic(hwnd) && GetForegroundWindow() == hwnd;
      if (!focused) for (const auto& id : render->cameras) static_cast<void>(render->health->set_usable(id, 1, false));
      bool ready = focused && clock_ok && gate->ready_now() && video_health.ready &&
          direct_udp;
      if (input_failed.load()) throw std::runtime_error("mock input worker failed");
      const auto current_epoch = epoch.load();
      if (current_epoch == 0) {
        // No command owner exists yet. Do not allow unsolicited telemetry to
        // fill the bounded channel before the safety gates admit neutral input.
        while (receive()) {}
      } else {
        const detail::NativeCourierScope current{{session_id, generation, attempt}, owner, current_epoch};
        if (!courier) courier = std::make_unique<detail::NativeControlCourier>(current, wss_dc_mirror);
        detail::NativeControlCourier::Io io{
            [&] {
              auto bytes = receive();
              if (config.control_trace_commands && bytes && bytes->find("\"vehicle_telemetry\"") != std::string::npos) {
                const auto envelope = Json::parse(*bytes, nullptr, false);
                if (envelope.is_object()) {
                  const auto value = envelope.contains("status") && envelope.at("status").is_object() ? envelope.at("status") : envelope;
                  if (value.contains("event") && value.at("event").is_string() && value.at("event") == "vehicle_telemetry" &&
                      value.contains("seq") && value.at("seq").is_number_unsigned()) {
                    ++telemetry_wire_observed;
                    telemetry_wire_last_seq = value.at("seq").get<std::uint64_t>();
                  }
                }
              }
              return bytes;
            }, [&] { return channel->direct_udp(mock_local_ip, mock_peer_ip); },
            [&](const Json& request) { return driver.control_transport_bridge(request); },
            [&](const std::string& bytes) {
              const auto started = static_cast<std::int64_t>(bench_now_ms());
              const auto frame = Json::parse(bytes, nullptr, false);
              const bool queued = channel->send(bytes);
              if (config.control_trace_commands) {
                if (frame.is_object() && frame.contains("seq") && frame.at("seq").is_number_unsigned()) {
                  if (dc_send_trace.size() < 128)
                    dc_send_trace.push_back({{"seq", frame.at("seq")}, {"started_monotonic_ms", started},
                        {"completed_monotonic_ms", static_cast<std::int64_t>(bench_now_ms())}, {"queued", queued},
                        {"wire_format", "json"}, {"wire_bytes", bytes.size()}});
                  else ++dc_send_trace_dropped;
                }
              }
              return queued;
            }, on_status};
        if (!courier->tick(current, io)) throw std::runtime_error("mock courier retired");
        // Receiving a mirrored status can revoke the gate during this tick.
        ready = focused && clock_ok && gate->ready_now() && video_health.ready && direct_udp;
        const auto transport = status.at("native_control").at("control_transport");
        const bool negotiated = transport.value("epoch", 0) > 0 && !transport.value("selecting", true);
        const auto recovery_state = telemetry.is_object() ? telemetry.value("safety_state", "") : std::string{};
        if (mock_profile_needs_confirmation.load() &&
            (recovery_state == "TIMEOUT_BRAKE" || recovery_state == "ESTOP" || recovery_state == "FAULT") &&
            !mock_profile_reconnect_required.exchange(true)) {
          mock_profile_confirmation_requested.store(false);
          report(L"车端处于超时制动或故障锁定；请断开会话并按安全流程重新连接，不能仅靠限幅确认恢复。");
        }
        if (!mock_profile_reconnect_required.load() && mock_profile_confirmation_requested.load() && mock_profile_needs_confirmation.load() &&
            ready && negotiated && requested_profile && !mock_estop_requested.load() &&
            detail::native_mock_input_fresh(telemetry_at_ms, static_cast<std::int64_t>(bench_now_ms())) &&
            detail::native_mock_source_fresh(telemetry_source_ms,
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(),
                sync.value("offset_ms", std::int64_t{0}), sync.value("uncertainty_ms", std::int64_t{-1})) &&
            telemetry.is_object() && telemetry.contains("speed_mps") && telemetry.at("speed_mps").is_number() &&
            telemetry.at("speed_mps").get<double>() == 0.0) {
          profile_request = driver.prepare_control_profile(requested_profile->to_json()).at("request");
          profile_seq = profile_request->at("seq").get<std::uint64_t>();
          if (!profile_state.begin(profile_seq, true)) throw std::runtime_error("invalid manual profile sequence");
          profile_retry = detail::NativeMockProfileRetry{};
          profile_accepted = false;
          keyboard.withdraw();
          mock_profile_confirmation_requested.store(false);
          mock_profile_needs_confirmation.store(false);
          report(L"已发送会话限幅确认，等待车端应答。");
        }
        if (!mock_profile_needs_confirmation.load() &&
            profile_retry.due(ready, negotiated, profile_accepted, current_epoch, now_ms)) {
          if (!profile_request) {
            SessionControlProfile profile;
            profile.target_speed_kph = 0; profile.max_motor_torque_nm = 0;
            profile.max_brake_pressure_bar = vehicle.field_safety.max_brake_pressure_bar;
            profile.service_brake_pressure_bar = std::min(config.control_limits.initial_service_brake_pressure_bar, profile.max_brake_pressure_bar);
            profile.hard_brake_pressure_bar = std::min(config.control_limits.initial_hard_brake_pressure_bar, profile.max_brake_pressure_bar);
            profile.max_steering_angle_deg = 0;
            profile.critical_camera_stale_brake_ms = vehicle.runtime.critical_camera_stale_brake_ms;
            profile.control_recovery_timeout_ms = vehicle.runtime.control_recovery_timeout_ms;
            requested_profile = profile;
            profile_request = driver.prepare_control_profile(profile.to_json()).at("request");
            profile_seq = profile_request->at("seq").get<std::uint64_t>();
            if (!profile_state.begin(profile_seq, false)) throw std::runtime_error("invalid initial profile sequence");
          }
          static_cast<void>(driver.send_control_management({{"source_session_id", session_id},
              {"source_control_session_generation", generation}, {"media_negotiation_id", attempt},
              {"ui_instance_id", owner}, {"ui_control_epoch", current_epoch}, {"message", *profile_request}}));
          profile_retry.sent(current_epoch, now_ms);
        }
        if (ready && negotiated && profile_accepted && !handshake_sent) {
          static_cast<void>(driver.send_control_management({{"source_session_id", session_id},
              {"source_control_session_generation", generation}, {"media_negotiation_id", attempt},
              {"ui_instance_id", owner}, {"ui_control_epoch", current_epoch},
              {"message", {{"event", "vcu_handshake_command"}, {"action", "connect"}}}}));
          handshake_sent = true;
        }
      }
      ready = focused && clock_ok && gate->ready_now() && video_health.ready && direct_udp;
      // The courier may have delivered newer telemetry after the loop's
      // initial timestamp. Compare against a clock read taken after delivery.
      const auto authorization_observed_ms = static_cast<std::int64_t>(bench_now_ms());
      const auto wall_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch()).count();
      const auto telemetry_gear = telemetry.is_object() ? telemetry.value("gear", "") : std::string{};
      const bool authorized = !mock_estop_requested.load() && !mock_gear_reconnect_required.load() && ready && profile_accepted && handshake_sent &&
          detail::native_mock_input_fresh(telemetry_at_ms, authorization_observed_ms) && telemetry.is_object() &&
          detail::native_mock_source_fresh(telemetry_source_ms, wall_ms,
              sync.value("offset_ms", std::int64_t{0}), sync.value("uncertainty_ms", std::int64_t{-1})) &&
          telemetry.value("safety_state", "") == "CONTROL_ACTIVE" &&
          !telemetry.value("braking_wait", true) && (telemetry_gear == "N" || telemetry_gear == "D" || telemetry_gear == "R") &&
          telemetry.contains("speed_mps") && telemetry.at("speed_mps").is_number() &&
          telemetry.at("speed_mps").get<double>() == 0.0;
      if (config.control_trace_commands &&
          (!last_authorization_diagnostic || *last_authorization_diagnostic != authorized)) {
        const bool object = telemetry.is_object();
        const auto offset = sync.contains("offset_ms") && sync.at("offset_ms").is_number_integer()
            ? sync.at("offset_ms").get<std::int64_t>() : std::int64_t{0};
        const auto uncertainty = sync.contains("uncertainty_ms") && sync.at("uncertainty_ms").is_number_integer()
            ? sync.at("uncertainty_ms").get<std::int64_t>() : std::int64_t{-1};
        const bool active = object && telemetry.contains("safety_state") &&
            telemetry.at("safety_state").is_string() && telemetry.at("safety_state") == "CONTROL_ACTIVE";
        const bool braking = !object || !telemetry.contains("braking_wait") ||
            !telemetry.at("braking_wait").is_boolean() || telemetry.at("braking_wait").get<bool>();
        const Json diagnostic{{"event", "native_mock_authorization_change"},
            {"monotonic_ms", authorization_observed_ms}, {"authorized", authorized},
            {"ready", ready}, {"profile_accepted", profile_accepted}, {"handshake_sent", handshake_sent},
            {"estop_requested", mock_estop_requested.load()}, {"telemetry_present", object},
            {"telemetry_receipt_fresh", detail::native_mock_input_fresh(telemetry_at_ms, authorization_observed_ms)},
            {"telemetry_source_fresh", detail::native_mock_source_fresh(telemetry_source_ms, wall_ms, offset, uncertainty)},
            {"telemetry_age_ms", telemetry_at_ms ? Json(authorization_observed_ms - telemetry_at_ms) : Json(nullptr)},
            {"telemetry_source_ms", telemetry_source_ms}, {"local_wall_ms", wall_ms},
            {"clock_offset_ms", offset}, {"clock_uncertainty_ms", uncertainty},
            {"control_active", active}, {"braking_wait", braking},
            {"gear_valid", telemetry_gear == "N" || telemetry_gear == "D" || telemetry_gear == "R"},
            {"zero_speed", object && telemetry.contains("speed_mps") && telemetry.at("speed_mps").is_number() &&
                telemetry.at("speed_mps").get<double>() == 0.0}};
        std::fprintf(stderr, "%s\n", diagnostic.dump().c_str());
        std::fflush(stderr);
        last_authorization_diagnostic = authorized;
      }
      input_authorized_at_ms.store(authorized ? authorization_observed_ms : 0);
      if (now >= next_mock_ui) {
        const auto native = status.at("native_control");
        const auto transport = native.at("control_transport");
        const auto path = transport.value("path", "unknown");
        const bool selecting = transport.value("selecting", true);
        const auto control_path = selecting ? L"切换中" : transport.value("mock_dual_control", false) ? L"双路已发送" :
            path == "dc" ? L"DataChannel" :
            path == "wss" ? L"WSS 回退" : L"未知";
        const auto time_uncertainty = sync.value("uncertainty_ms", -1);
        const bool telemetry_fresh = detail::native_mock_input_fresh(telemetry_at_ms, authorization_observed_ms) && telemetry.is_object();
        const auto safety_state = telemetry_fresh ? telemetry.value("safety_state", "") : std::string{};
        const auto vehicle_state = safety_state == "ESTOP" ? L"急停锁存" : safety_state == "FAULT" ? L"故障" :
            safety_state == "TIMEOUT_BRAKE" ? L"超时制动" : safety_state == "DEGRADED" ? L"降级" :
            safety_state == "CONTROL_ACTIVE" && !telemetry.value("braking_wait", true) ? L"控制正常" : L"等待或未知";
        const auto latest_result = native.value("vehicle_result_latency", Json(nullptr));
        std::wstring result_text = L"等待车端结果";
        if (latest_result.is_object() && latest_result.contains("result_age_ms") && latest_result.at("result_age_ms").is_number_integer() &&
            latest_result.at("result_age_ms").get<std::int64_t>() >= 0 && latest_result.at("result_age_ms").get<std::int64_t>() < 500 &&
            latest_result.contains("round_trip_ms") && latest_result.at("round_trip_ms").is_number_integer() &&
            latest_result.at("round_trip_ms").get<std::int64_t>() >= 0 &&
            latest_result.contains("accepted") && latest_result.at("accepted").is_boolean()) {
          result_text = std::to_wstring(latest_result.at("round_trip_ms").get<std::int64_t>()) + L" ms · " +
              (latest_result.at("accepted").get<bool>() ? L"软件接受" : L"已拒绝");
        }
        std::optional<double> speed;
        std::optional<char> actual_gear;
        const bool evidence_fresh = telemetry_fresh && detail::native_mock_source_fresh(telemetry_source_ms, wall_ms,
            sync.value("offset_ms", std::int64_t{0}), sync.value("uncertainty_ms", std::int64_t{-1}));
        const auto feedback = telemetry_fresh ? telemetry.value("can_feedback", Json(nullptr)) : Json(nullptr);
        const wchar_t* can_state = !evidence_fresh || !feedback.is_object() ? L"未知或已过期" :
            !feedback.value("supported", false) ? L"台架不提供完整 CAN 反馈" :
            !feedback.value("feedback_fresh", false) ? L"反馈过期" : L"反馈新鲜";
        if (evidence_fresh) {
          speed = detail::native_feedback_display_speed(telemetry);
          actual_gear = detail::native_feedback_display_gear(telemetry);
        }
        std::optional<detail::NativeOperatorIntent> shown_intent;
        {
          std::lock_guard lock(intent_display_mutex);
          if (authorized && intent_display.valid &&
              detail::native_mock_input_fresh(intent_display.observed_ms, static_cast<std::int64_t>(bench_now_ms())))
            shown_intent = intent_display.input;
        }
        report_mock(std::wstring(L"控制：") + control_path + (authorized ? L" · 输入就绪" : L" · 输入禁止") +
            L"\n画面：" + (video_health.ready && gate->ready_now() ? L"就绪" : L"未就绪") +
            L"  校时：" + (clock_ok ? std::wstring(L"±") + std::to_wstring(time_uncertainty) + L" ms" : L"不可用") +
            L"\n车端：" + (mock_estop_requested.load() && safety_state != "ESTOP" ? L"急停等待确认" : vehicle_state) +
            L"\n结果：" + result_text + L"\nCAN：" + can_state +
            L"\n会话限幅：" + (profile_accepted ? L"零速 / 零转向 · 已确认" : L"等待确认"),
            detail::native_operator_readouts(speed, actual_gear, shown_intent),
            detail::native_can_feedback_text(feedback, evidence_fresh));
        next_mock_ui = now + 100ms;
      }
      if (now >= next_evidence) {
        flush_dc_trace();
        flush_guard_trace();
        if (config.control_trace_commands) {
          const auto batch = receiver.drain_dtls_trace();
          Json records = Json::array();
          for (const auto& entry : batch.records)
            records.push_back({{"monotonic_us", entry.monotonic_us}, {"bytes", entry.bytes},
                {"type", entry.header.type}, {"epoch", entry.header.epoch}, {"record_seq", entry.header.sequence}});
          const auto wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
              std::chrono::system_clock::now().time_since_epoch()).count();
          const auto mono_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now().time_since_epoch()).count();
          const Json packet_trace{{"event", "native_mock_dtls_outbound_batch"}, {"records", std::move(records)},
              {"dropped", batch.dropped}, {"clock_wall_us", wall_us}, {"clock_monotonic_us", mono_us},
              {"clock_offset_ms", sync.value("offset_ms", std::int64_t{0})},
              {"clock_uncertainty_ms", sync.value("uncertainty_ms", std::int64_t{-1})}};
          std::fprintf(stderr, "%s\n", packet_trace.dump().c_str());
        }
        const auto native = status.at("native_control");
        const auto transport = native.at("control_transport");
        const auto feedback = transport.value("feedback_transport", Json::object());
        const auto sends = channel->send_stats();
        const Json evidence{{"event", "native_mock_observation"}, {"monotonic_ms", now_ms},
            {"video_ready", video_health.ready}, {"remote_media_ready", gate->ready(now_ms)},
            {"remote_media_reason", gate->reason(now_ms)},
            {"mock_mirrored_media_received", mirrored_media_received},
            {"mock_mirrored_media_ready_after", mirrored_media_ready_after},
            {"focused", focused}, {"clock_ready", clock_ok}, {"clock_uncertainty_ms", sync.value("uncertainty_ms", -1)},
            {"sctp_direct_udp", channel->direct_udp(mock_local_ip, mock_peer_ip)}, {"control_path", transport.value("path", "unknown")},
            {"mock_dual_control", transport.value("mock_dual_control", false)},
            {"mock_dual_last_vehicle_path", transport.value("mock_dual_last_vehicle_path", Json(nullptr))},
            {"control_selecting", transport.value("selecting", true)}, {"intent_fresh", native.value("intent_fresh", false)},
            {"control_failed_dc_trials", transport.value("failed_dc_trials", 0)},
            {"feedback_path", feedback.is_object() ? feedback.value("path", "unknown") : "unknown"},
            {"feedback_epoch", feedback.is_object() ? feedback.value("epoch", std::uint64_t{0}) : 0},
            {"feedback_selecting", feedback.is_object() ? feedback.value("selecting", true) : true},
            {"mock_idle_reprobe_used", transport.value("mock_idle_reprobe_used", false)},
            {"channel_send", {{"attempted", sends.attempted}, {"queued", sends.queued},
                {"buffered", sends.buffered}, {"unavailable", sends.unavailable}, {"errors", sends.errors}}},
            {"sent_total", native.value("commands_sent_total", std::uint64_t{0})},
            {"last_ack_seq", native.value("last_ack_seq", std::uint64_t{0})},
            {"last_gap_ms", native.value("last_gap_ms", std::int64_t{0})},
            {"profile_accepted", profile_accepted}, {"profile_attempts", profile_retry.attempts()},
            {"control_epoch", transport.value("epoch", std::uint64_t{0})}, {"handshake_sent", handshake_sent},
            {"telemetry_age_ms", telemetry_at_ms ? Json(authorization_observed_ms - telemetry_at_ms) : Json(nullptr)},
            {"telemetry_wire_observed", telemetry_wire_observed}, {"telemetry_wire_last_seq", telemetry_wire_last_seq},
            {"telemetry_status_observed", telemetry_status_observed},
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
              {"vehicle_received_path", latency.value("vehicle_received_path", Json(nullptr))},
              {"round_trip_ms", latency.value("round_trip_ms", Json(nullptr))},
              {"accepted", latency.value("accepted", Json(nullptr))}};
          std::fprintf(stderr, "%s\n", result.dump().c_str());
        }
      }
      std::this_thread::sleep_for(10ms);
    } } catch (...) { control_failure = std::current_exception(); }
    input_authorized_at_ms.store(0);
    keyboard.withdraw();
    flush_dc_trace();
    input_worker.request_stop();
    if (input_worker.joinable()) input_worker.join();
    clock_worker.request_stop();
    if (clock_worker.joinable()) clock_worker.join();
    flush_guard_trace();
    if (courier) courier->retire();
    std::array<Samples, 2> gaps, decoded_gaps, submission_waits;
    { std::lock_guard lock(render->mutex); gaps = render->submission_gaps;
      decoded_gaps = render->decoded_gaps; submission_waits = render->decoded_to_submission; }
    const Json summary{{"event", "native_mock_render_summary"}, {"scope", "foreground GDI submission intervals, not capture-to-screen"},
        {"streams", Json::array({gaps[0].json(), gaps[1].json()})},
        {"decoded_callback_gaps", Json::array({decoded_gaps[0].json(), decoded_gaps[1].json()})},
        {"decoded_callback_to_submission", Json::array({submission_waits[0].json(), submission_waits[1].json()})},
        {"safety_stop_episodes", safety_stop_episodes}};
    std::fprintf(stderr, "%s\n", summary.dump().c_str()); std::fflush(stderr);
    if (control_failure) std::rethrow_exception(control_failure);
  }

  void connect(std::string driver_path, std::string vehicle_path, std::string requested_driver, std::string secret,
               std::stop_token stop_token) {
    const char* phase = "configuration";
    { std::lock_guard lock(intent_display_mutex); intent_display.valid = false; }
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
      { std::lock_guard lock(gamepad_mutex); gamepad_config = config.gamepad; gamepad_estop_hold_ms = config.estop_hold_ms; }
      if (mock_control && !config.signaling_url.starts_with("ws://127.0.0.1:"))
        throw std::runtime_error("mock control requires an isolated loopback signaling tunnel");
      config.receive_only = !mock_control;
      config.defer_control_bootstrap = mock_control;
      config.mock_idle_direct_reprobe = mock_control;
      config.mock_dual_control = mock_control;
      config.max_time_sync_uncertainty_ms = vehicle.field_safety.max_time_sync_uncertainty_ms;
      config.time_sync_interval_ms = vehicle.field_safety.time_sync_interval_ms;
      config.time_sync_samples = vehicle.field_safety.time_sync_samples;
      // Only the explicitly enabled isolated mock trace may write control
      // diagnostics; ordinary receive-only viewing remains log-free.
      if (!mock_control || !config.control_trace_commands) config.browser_event_log_path.clear();
      DriverConsoleRuntime driver(config, vehicle.vehicle_id, secret);
      // Wake a pending mobile-approval request and retire control immediately
      // when the operator disconnects or closes the window.
      std::stop_callback on_stop(stop_token, [&driver] {
        try { driver.prepare_shutdown(); } catch (...) {}
      });
      bool connected = false;
      try {
        report(mock_control ? L"正在连接隔离 mock 台架；等待安全条件。" : L"正在登录并连接车端；仅观看，控制禁用。");
        phase = "login";
        static_cast<void>(driver.login(secret, requested_driver));
        if (!requested_driver.empty()) config.driver_id = requested_driver;
        std::fill(secret.begin(), secret.end(), '\0');
        if (stop_token.stop_requested()) throw std::runtime_error("cancelled");
        phase = "vehicles";
        const auto catalog = driver.vehicles();
        if (!catalog.is_object() || !catalog.contains("vehicles") || !catalog.at("vehicles").is_array())
          throw std::runtime_error("invalid authorized vehicle catalog");
        {
          std::lock_guard lock(selection_mutex);
          vehicle_choices.clear();
          for (const auto& entry : catalog.at("vehicles")) {
            if (!entry.is_object() || !entry.contains("vehicle_id") || !entry.at("vehicle_id").is_string()) continue;
            const auto id = entry.at("vehicle_id").get<std::string>();
            const bool matches = id == vehicle.vehicle_id;
            const bool available = entry.value("online", false) && entry.value("controllable", false);
            const bool approval = entry.value("mobile_approval_required", false);
            vehicle_choices.push_back({wide(id) + (matches ? L" · 台架配置匹配" : L" · 需对应台架配置") +
                (available ? L" · 可连接" : L" · 不可连接") + (approval ? L" · 需手机审批" : L""),
                matches && available, id, approval});
          }
          selection_confirmed = false;
          selection_logs_requested = false;
          selection_log_mode = false;
          selection_busy = false;
          awaiting_selection = true;
        }
        const auto authorized_choices = vehicle_choices;
        PostMessageW(hwnd, WM_APP + 3, 0, 0);
        report(L"请选择授权车辆；仅可连接与本地隔离台架配置匹配的车辆。");
        bool needs_mobile_approval = false;
        {
          std::unique_lock lock(selection_mutex);
          while (!stop_token.stop_requested()) {
            selection_ready.wait_for(lock, 100ms, [&] { return selection_confirmed || selection_logs_requested; });
            if (stop_token.stop_requested()) break;
            if (selection_logs_requested) {
              selection_logs_requested = false;
              if (selection_log_mode) {
                vehicle_choices = authorized_choices; selection_log_mode = false;
                PostMessageW(hwnd, WM_APP + 3, 0, 0);
                continue;
              }
              lock.unlock();
              try {
                const auto sessions = driver.log_sessions().at("sessions");
                if (!sessions.is_array()) throw std::runtime_error("invalid session catalog");
                std::vector<VehicleChoice> choices;
                for (const auto& item : sessions) {
                  if (!item.is_object() || !item.contains("key") || !item.at("key").is_string() ||
                      !item.contains("session_id") || !item.at("session_id").is_string() ||
                      !item.contains("vehicle_id") || !item.at("vehicle_id").is_string()) continue;
                  choices.push_back({wide(item.at("session_id").get<std::string>()) + L" · " +
                      wide(item.at("vehicle_id").get<std::string>()), true, item.at("key").get<std::string>()});
                }
                lock.lock(); vehicle_choices = std::move(choices); selection_log_mode = true;
                PostMessageW(hwnd, WM_APP + 3, 0, 0);
                lock.unlock(); report(L"选择已结束会话；日志仅对当前授权驾驶员可见。"); lock.lock();
              } catch (...) {
                if (lock.owns_lock()) lock.unlock();
                report(L"读取会话日志列表失败，请检查登录和信令。");
                lock.lock();
                PostMessageW(hwnd, WM_APP + 3, 0, 0);
              }
              continue;
            }
            if (!selection_confirmed) continue;
            selection_confirmed = false;
            if (selection_index >= vehicle_choices.size()) { PostMessageW(hwnd, WM_APP + 3, 0, 0); continue; }
            if (!selection_log_mode) { needs_mobile_approval = vehicle_choices[selection_index].needs_approval; break; }
            const auto log_key = vehicle_choices[selection_index].key;
            lock.unlock();
            try {
              PostMessageW(hwnd, WM_APP + 6, 0, 0);
              static_cast<void>(driver.start_log_export(log_key));
              report(L"正在收集三端日志，请保持车辆空闲并在线；最多约 10 分钟。");
              Json progress;
              while (!stop_token.stop_requested()) {
                progress = driver.log_export_status();
                if (progress.value("state", "") != "collecting") break;
                std::this_thread::sleep_for(500ms);
              }
              if (stop_token.stop_requested()) { lock.lock(); break; }
              if (progress.value("state", "") != "ready") throw std::runtime_error("log export failed");
              report(progress.value("logs_complete", false) ?
                  L"日志收集完成。ZIP 不含视频，录像完整性请查看摘要。" :
                  L"日志仅部分收集。ZIP 不含视频，请查看清单中的缺失原因。");
              lock.lock(); selection_save_response = false; selection_save_path.clear();
              PostMessageW(hwnd, WM_APP + 5, 0, 0);
              while (!selection_save_response && !stop_token.stop_requested()) selection_ready.wait_for(lock, 100ms);
              const auto path = selection_save_path;
              lock.unlock();
              if (!stop_token.stop_requested() && !path.empty()) {
                const auto zip = driver.log_export_zip();
                std::ofstream file(std::filesystem::path(path), std::ios::binary | std::ios::trunc);
                file.write(zip.data(), static_cast<std::streamsize>(zip.size()));
                file.close();
                if (!file) throw std::runtime_error("log save failed");
                report(L"日志 ZIP 已保存；录像文件不包含在内。");
              }
              lock.lock();
            } catch (...) {
              if (lock.owns_lock()) lock.unlock();
              report(L"日志收集或保存失败；可重新选择已结束会话重试。");
              lock.lock();
            }
            PostMessageW(hwnd, WM_APP + 3, 0, 0);
          }
          awaiting_selection = false;
          if (stop_token.stop_requested()) throw std::runtime_error("cancelled");
        }
        PostMessageW(hwnd, WM_APP + 4, 0, 0);
        phase = "connect";
        report(needs_mobile_approval ? L"等待手机审批；可随时断开取消申请。" : L"正在建立授权会话。");
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
              const bool wss_dc_mirror = mock_control &&
                  payload.value("mock_wss_dc_mirror", "") == detail::kMockWssDcMirrorCapability;
              if (mock_control) {
                options.local_addresses = {mock_local_ip};
                options.remote_addresses = {mock_peer_ip};
                options.address_family = mock_ip_family;
                options.ice_transport = "udp";
                options.ice_route_mode = "direct";
                options.trace_dtls_headers = config.control_trace_commands;
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
                control_worker = std::jthread([this, &driver, target, vehicle, config, session, negotiation, render, wss_dc_mirror,
                                                gate = mock_gate, stop_token](std::stop_token token) {
                  try { run_mock_control(driver, *target, vehicle, config, session, negotiation, render, gate, wss_dc_mirror, stop_token, token); }
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
              if (mock_control && mock_gear_reconnect_required.load())
                report(L"换挡拒绝无法关联，普通输入已冻结；请安全断开并重新连接。");
              else if (mock_control && mock_profile_reconnect_required.load())
                report(L"车端处于超时制动或故障锁定；请断开会话并按安全流程重新连接，不能仅靠限幅确认恢复。");
              else
                report((advancing ? L"渲染提交持续更新 · " : L"等待新画面或画面停滞 · ") + path +
                    (mock_control ? L" · 隔离 mock，零速" : L" · 仅观看，控制禁用"));
              next_observation = Clock::now() + 1s;
            }
          }
          std::this_thread::sleep_for(10ms);
        }
        if (control_worker.joinable()) { control_worker.request_stop(); control_worker.join(); }
        if (receiver) {
          receiver->stop();
          if (mock_control && config.control_trace_commands) {
            // Export the bounded packet and decode observations only after
            // stopping media; their trace probes run during the mock session.
            // Do not export the selected-pair addresses from the full report.
            try {
              const auto report = receiver->report();
              const Json timing{{"event", "native_mock_receiver_timing"},
                  {"scope", "receiver-local jitter input and decoded PTS; not capture or scan-out"},
                  {"jitter_input_markers", report.at("jitter_input_markers")},
                  {"ice_input_markers", report.at("ice_input_markers")},
                  {"tracks", report.at("tracks")}};
              std::fprintf(stderr, "%s\n", timing.dump().c_str()); std::fflush(stderr);
            } catch (...) {
              std::fprintf(stderr, "native_mock_receiver_timing_unavailable\n"); std::fflush(stderr);
            }
          }
        }
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
          detail.find("driver ID is required") != std::string_view::npos ? "invalid_driver_id" :
          detail.find("HTTP status 401") != std::string_view::npos ? "unauthorized" :
          detail.find("native view has no matching media pad") != std::string_view::npos ? "unmapped_pad" :
          detail.find("decode track pad link noformat") != std::string_view::npos ? "track_link_noformat" :
          detail.find("decode track pad link failed") != std::string_view::npos ? "track_link_other" :
          detail.find("decode track state sync failed") != std::string_view::npos ? "track_state_sync" :
          detail.find("GStreamer promise timed out") != std::string_view::npos ? "promise_timeout" : "other";
      std::fprintf(stderr, "native_viewer_failure phase=%s category=%s\n", phase, category);
      std::fill(secret.begin(), secret.end(), '\0');
      report(stop_token.stop_requested() ? L"已取消连接。" :
          std::string_view(category) == "invalid_driver_id" ? L"驾驶员 ID 无效：不能包含空白字符。" :
          std::string_view(category) == "unauthorized" ? L"登录失败：请检查驾驶员 ID 和密码。" :
          L"连接或视频失败。请检查配置、台架隔离、信令和 GStreamer 依赖。");
    }
    { std::lock_guard lock(render_mutex); render_session.reset(); }
    if (mock_control) report_mock(L"控制：已断开\n画面：未就绪\n校时：未就绪");
    for (const auto handle : video) InvalidateRect(handle, nullptr, TRUE);
    running = false;
    PostMessageW(hwnd, WM_APP + 1, 0, 0);
  }
};

HWND control(HWND parent, const wchar_t* type, const wchar_t* caption, DWORD style,
             int x, int y, int w, int h, int id = 0) {
  if (std::wstring_view(type) == L"EDIT" || std::wstring_view(type) == L"BUTTON") style |= WS_TABSTOP;
  return CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
}

void layout(Window& app, int width, int height) {
  if (!app.video[0]) return;
  constexpr int gap = 12, margin = 16, side_width = 300;
  const int side_x = width - margin - side_width;
  const int video_width = std::max(1, (side_x - 3 * margin) / 2);
  const int video_height = std::max(1, height - 250);
  MoveWindow(app.status, margin + 12, 78, width - 2 * margin - 24, 24, TRUE);
  const int strip_width = std::max(6, side_x - margin - gap);
  for (int i = 0; i < 6; ++i) {
    const int left = margin + i * strip_width / 6;
    const int cell_width = (i + 1) * strip_width / 6 - i * strip_width / 6;
    MoveWindow(app.operator_labels[i], left + 8, 126, std::max(1, cell_width - 12), 18, TRUE);
    MoveWindow(app.operator_readouts[i], left + 8, 146, std::max(1, cell_width - 12), 24, TRUE);
  }
  for (int i = 0; i < 2; ++i)
    MoveWindow(app.video[i], margin + i * (video_width + gap), 210, video_width, video_height, TRUE);
  const int left = side_x + 16, field_width = side_width - 32;
  for (int i = 0; i < 4; ++i) {
    MoveWindow(app.config_labels[i], left, 182 + i * 45, field_width, 18, TRUE);
    HWND field = i == 0 ? app.driver_config : i == 1 ? app.vehicle_config : i == 2 ? app.driver_id : app.password;
    MoveWindow(field, left, 200 + i * 45, field_width, 26, TRUE);
  }
  MoveWindow(app.start, left, 385, (field_width - gap) / 2, 34, TRUE);
  MoveWindow(app.stop, left + (field_width + gap) / 2, 385, (field_width - gap) / 2, 34, TRUE);
  MoveWindow(app.vehicle_list, left, 422, field_width - 80, 180, TRUE);
  MoveWindow(app.logs_button, left + field_width - 74, 422, 74, 26, TRUE);
  MoveWindow(app.can_detail, left, 182, field_width, 190, TRUE);
  MoveWindow(app.help, left, 450, field_width, app.mock_control ? 20 : 70, TRUE);
  if (app.mock_control) {
    constexpr int key_width = 40, key_height = 25, key_gap = 4;
    MoveWindow(app.keycaps[0], left + key_width + key_gap, 475, key_width, key_height, TRUE);
    for (int i = 1; i < 4; ++i)
      MoveWindow(app.keycaps[i], left + (i - 1) * (key_width + key_gap), 504, key_width, key_height, TRUE);
    MoveWindow(app.keycaps[4], left + 3 * (key_width + key_gap), 475, field_width - 3 * (key_width + key_gap), key_height, TRUE);
    MoveWindow(app.keycaps[5], left + 3 * (key_width + key_gap), 504, field_width - 3 * (key_width + key_gap), key_height, TRUE);
  }
  if (app.mock_estop_button) MoveWindow(app.mock_estop_button, left, 536, field_width, 30, TRUE);
  if (app.mock_info) MoveWindow(app.mock_info, left, 570, field_width, 92, TRUE);
}

void fill(HDC dc, int left, int top, int right, int bottom, HBRUSH brush) {
  RECT rect{left, top, right, bottom};
  FillRect(dc, &rect, brush);
}

void label(HDC dc, HFONT font, COLORREF color, int left, int top, int right, int bottom,
           const wchar_t* text) {
  const auto previous = SelectObject(dc, font);
  SetTextColor(dc, color);
  SetBkMode(dc, TRANSPARENT);
  RECT rect{left, top, right, bottom};
  DrawTextW(dc, text, -1, &rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  SelectObject(dc, previous);
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
      if (dc) draw_empty_video(dc, rect, id == 10 ? L"视频 1 · 等待连接" : L"视频 2 · 等待连接"); EndPaint(hwnd, &ps); }
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
      app->background = CreateSolidBrush(RGB(8, 16, 29));
      app->surface = CreateSolidBrush(RGB(15, 25, 41));
      app->input_background = CreateSolidBrush(RGB(17, 31, 50));
      app->font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
      app->title_font = CreateFontW(-21, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
      app->small_font = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
      app->config_labels[0] = control(hwnd, L"STATIC", L"控制端配置", 0, 0, 0, 1, 1);
      app->driver_config = control(hwnd, L"EDIT", L"configs/driver-console.dev.yaml", WS_BORDER | ES_AUTOHSCROLL, 0, 0, 1, 1);
      app->config_labels[1] = control(hwnd, L"STATIC", L"车端台架配置", 0, 0, 0, 1, 1);
      app->vehicle_config = control(hwnd, L"EDIT", L"configs/vehicle-agent.orin-cpu-bench.yaml", WS_BORDER | ES_AUTOHSCROLL, 0, 0, 1, 1);
      app->config_labels[2] = control(hwnd, L"STATIC", L"驾驶员 ID（留空使用配置）", 0, 0, 0, 1, 1);
      app->driver_id = control(hwnd, L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL, 0, 0, 1, 1);
      SendMessageW(app->driver_id, EM_SETLIMITTEXT, 128, 0);
      app->config_labels[3] = control(hwnd, L"STATIC", L"驾驶员密码", 0, 0, 0, 1, 1);
      app->password = control(hwnd, L"EDIT", L"", WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 0, 0, 1, 1);
      app->start = control(hwnd, L"BUTTON", app->mock_control ? L"连接台架" : L"连接视频", BS_OWNERDRAW, 0, 0, 1, 1, 1);
      app->stop = control(hwnd, L"BUTTON", L"断开", BS_OWNERDRAW, 0, 0, 1, 1, 2);
      app->vehicle_list = control(hwnd, L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 0, 0, 1, 1, 4);
      EnableWindow(app->vehicle_list, FALSE);
      app->logs_button = control(hwnd, L"BUTTON", L"会话日志", BS_OWNERDRAW, 0, 0, 1, 1, 5);
      EnableWindow(app->logs_button, FALSE);
      app->can_detail = control(hwnd, L"EDIT", L"CAN 反馈未知", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
          0, 0, 1, 1);
      ShowWindow(app->can_detail, SW_HIDE);
      app->help = control(hwnd, L"STATIC", app->mock_control ?
          L"按键状态 · E 急停 · 非执行确认" :
          L"仅观看台架画面\n控制功能保持禁用", 0, 0, 0, 1, 1);
      if (app->mock_control) {
        constexpr std::array<const wchar_t*, 6> captions{L"↑ W", L"← A", L"↓ S", L"→ D", L"SPACE 缓刹", L"B 急刹"};
        for (std::size_t i = 0; i < captions.size(); ++i)
          app->keycaps[i] = control(hwnd, L"STATIC", captions[i], SS_OWNERDRAW, 0, 0, 1, 1, 20 + static_cast<int>(i));
      }
      if (app->mock_control) app->mock_info = control(hwnd, L"STATIC", app->mock_message.c_str(), 0, 0, 0, 1, 1);
      if (app->mock_control) app->mock_estop_button = control(hwnd, L"BUTTON", L"请求急停", BS_OWNERDRAW, 0, 0, 1, 1, 3);
      EnableWindow(app->stop, FALSE);
      if (app->mock_estop_button) EnableWindow(app->mock_estop_button, FALSE);
      if (app->mock_control) app->message = L"隔离 mock 控制台：等待连接和安全条件。";
      app->status = control(hwnd, L"STATIC", app->message.c_str(), 0, 0, 0, 1, 1);
      {
        constexpr std::array<const wchar_t*, 6> labels{L"实测车速", L"实际挡位", L"指令挡位", L"转向", L"目标车速比例", L"刹车"};
        for (std::size_t i = 0; i < labels.size(); ++i) {
          app->operator_labels[i] = control(hwnd, L"STATIC", labels[i], 0, 0, 0, 1, 1);
          app->operator_readouts[i] = control(hwnd, L"STATIC", L"—", 0, 0, 0, 1, 1);
        }
      }
      app->video[0] = control(hwnd, L"MineTeleopNativePanel", L"", 0, 0, 0, 1, 1, 10);
      app->video[1] = control(hwnd, L"MineTeleopNativePanel", L"", 0, 0, 0, 1, 1, 11);
      for (auto child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(app->font), TRUE);
      for (const auto title : app->config_labels)
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app->small_font), TRUE);
      for (const auto title : app->operator_labels)
        SendMessageW(title, WM_SETFONT, reinterpret_cast<WPARAM>(app->small_font), TRUE);
      if (app->mock_control) SendMessageW(app->help, WM_SETFONT, reinterpret_cast<WPARAM>(app->small_font), TRUE);
      { RECT rect{}; GetClientRect(hwnd, &rect); layout(*app, rect.right, rect.bottom); }
      SetTimer(hwnd, 1, 1000, nullptr);
      if (app->mock_control) SetTimer(hwnd, 2, 50, nullptr);
      return 0;
    case WM_SIZE: {
      if (wparam == SIZE_MINIMIZED) { app->keyboard.withdraw(); app->clear_input_display(); app->suspend_render_health(); }
      layout(*app, LOWORD(lparam), HIWORD(lparam));
      InvalidateRect(hwnd, nullptr, TRUE);
      return 0;
    }
    case WM_GETMINMAXINFO: {
      auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
      info->ptMinTrackSize = POINT{900, 720};
      return 0;
    }
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{}; auto* dc = BeginPaint(hwnd, &ps);
      RECT rect{}; GetClientRect(hwnd, &rect);
      fill(dc, 0, 0, rect.right, rect.bottom, app->background);
      fill(dc, 16, 68, rect.right - 16, 111, app->input_background);
      fill(dc, rect.right - 316, 124, rect.right - 16, rect.bottom - 16, app->surface);
      fill(dc, 16, 120, rect.right - 328, 176, app->surface);
      label(dc, app->title_font, RGB(231, 237, 246), 16, 15, 300, 52, L"Mine Teleop 控制台");
      label(dc, app->small_font, RGB(142, 160, 184), 300, 16, rect.right - 16, 52,
          app->mock_control ? L"Windows 原生端  ·  隔离 mock 台架  ·  零车速" : L"Windows 原生端  ·  仅观看");
      label(dc, app->font, RGB(231, 237, 246), 16, 180, rect.right - 330, 205, L"实时视频");
      label(dc, app->font, RGB(231, 237, 246), rect.right - 300, 138, rect.right - 32, 168, L"连接与控制");
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CTLCOLORSTATIC: {
      auto* dc = reinterpret_cast<HDC>(wparam);
      const auto child = reinterpret_cast<HWND>(lparam);
      const bool is_status = child == app->status;
      const bool is_readout = std::find(app->operator_readouts.begin(), app->operator_readouts.end(), child) != app->operator_readouts.end();
      SetTextColor(dc, is_status || is_readout ? RGB(231, 237, 246) : RGB(142, 160, 184));
      SetBkColor(dc, is_status ? RGB(17, 31, 50) : RGB(15, 25, 41));
      return reinterpret_cast<LRESULT>(is_status ? app->input_background : app->surface);
    }
    case WM_CTLCOLOREDIT: {
      auto* dc = reinterpret_cast<HDC>(wparam);
      SetTextColor(dc, RGB(231, 237, 246)); SetBkColor(dc, RGB(17, 31, 50));
      return reinterpret_cast<LRESULT>(app->input_background);
    }
    case WM_DRAWITEM: {
      const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
      if (item->CtlID >= 20 && item->CtlID < 26 && app->mock_control) {
        const bool pressed = app->shown_keys[item->CtlID - 20];
        const auto brush = CreateSolidBrush(pressed ? RGB(14, 116, 144) : RGB(30, 47, 68));
        FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
        wchar_t caption[32]{}; GetWindowTextW(item->hwndItem, caption, 32);
        SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, RGB(231, 237, 246));
        const auto old = SelectObject(item->hDC, app->small_font);
        RECT bounds = item->rcItem; DrawTextW(item->hDC, caption, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(item->hDC, old);
        return TRUE;
      }
      if (item->CtlID != 1 && item->CtlID != 2 && item->CtlID != 3 && item->CtlID != 5) return DefWindowProcW(hwnd, msg, wparam, lparam);
      const bool enabled = !(item->itemState & ODS_DISABLED);
      const COLORREF color = !enabled ? RGB(38, 54, 77) : item->CtlID == 1 ? RGB(14, 116, 144) :
          item->CtlID == 3 ? RGB(150, 42, 55) : RGB(30, 47, 68);
      const auto brush = CreateSolidBrush(color); FillRect(item->hDC, &item->rcItem, brush); DeleteObject(brush);
      wchar_t caption[32]{}; GetWindowTextW(item->hwndItem, caption, 32);
      SetBkMode(item->hDC, TRANSPARENT); SetTextColor(item->hDC, enabled ? RGB(231, 237, 246) : RGB(142, 160, 184));
      const auto old = SelectObject(item->hDC, app->font);
      RECT bounds = item->rcItem; DrawTextW(item->hDC, caption, -1, &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      SelectObject(item->hDC, old);
      if (item->itemState & ODS_FOCUS) DrawFocusRect(item->hDC, &bounds);
      return TRUE;
    }
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE) { app->keyboard.withdraw(); app->clear_input_display(); app->suspend_render_health(); }
      else for (const auto panel : app->video) InvalidateRect(panel, nullptr, FALSE);
      return 0;
    case WM_COMMAND:
      if (LOWORD(wparam) == 1 && app->running.load() && app->mock_profile_needs_confirmation.load()) {
        if (app->mock_profile_reconnect_required.load()) return 0;
        app->keyboard.withdraw();
        app->mock_profile_confirmation_requested.store(true);
        EnableWindow(app->start, FALSE);
        SetFocus(hwnd);
        app->report(L"等待视频、时钟和 mock 状态恢复后发送人工确认。");
      } else if (LOWORD(wparam) == 1 && app->running.load()) {
        const auto selected = SendMessageW(app->vehicle_list, CB_GETCURSEL, 0, 0);
        bool valid = false;
        {
          std::lock_guard lock(app->selection_mutex);
          valid = app->awaiting_selection && selected != CB_ERR &&
              static_cast<std::size_t>(selected) < app->vehicle_choices.size() && app->vehicle_choices[static_cast<std::size_t>(selected)].eligible;
          if (valid && !app->selection_busy) {
            app->selection_index = static_cast<std::size_t>(selected); app->selection_confirmed = true; app->selection_busy = true;
          } else valid = false;
        }
        if (valid) { EnableWindow(app->start, FALSE); app->selection_ready.notify_one(); if (app->mock_control) SetFocus(hwnd); }
        else app->report(L"请选择有效的授权车辆或已结束会话。");
      } else if (LOWORD(wparam) == 1 && !app->running.exchange(true)) {
        try {
          if (app->worker.joinable()) app->worker.join();
          app->mock_estop_requested.store(false);
          app->mock_profile_needs_confirmation.store(false);
          app->mock_profile_confirmation_requested.store(false);
          app->mock_profile_reconnect_required.store(false);
          app->mock_gear_reconnect_required.store(false);
          auto secret = text_of(app->password);
          auto driver = utf8(text_of(app->driver_config)), vehicle = utf8(text_of(app->vehicle_config));
          auto requested_driver = utf8(detail::native_trim_identity(text_of(app->driver_id)));
          auto encoded = utf8(secret);
          SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
          SetWindowTextW(app->password, L"");
          if (encoded.empty()) throw std::runtime_error("password required");
          EnableWindow(app->start, FALSE); EnableWindow(app->stop, TRUE);
          for (const auto field : {app->driver_config, app->vehicle_config, app->driver_id, app->password}) EnableWindow(field, FALSE);
          app->worker = std::jthread([app, driver = std::move(driver), vehicle = std::move(vehicle), requested_driver = std::move(requested_driver),
                                      encoded = std::move(encoded)](std::stop_token token) mutable {
            app->connect(std::move(driver), std::move(vehicle), std::move(requested_driver), std::move(encoded), token);
          });
          if (app->mock_control) SetFocus(hwnd);
        } catch (...) { app->running = false; app->report(L"请输入有效的配置路径和密码。"); }
      } else if (LOWORD(wparam) == 2 && app->worker.joinable()) {
        app->keyboard.withdraw();
        app->worker.request_stop(); app->report(L"正在断开视频会话……");
      } else if (LOWORD(wparam) == 3) app->request_mock_estop();
      else if (LOWORD(wparam) == 5) {
        { std::lock_guard lock(app->selection_mutex);
          if (!app->awaiting_selection || app->selection_busy) return 0;
          app->selection_logs_requested = true; app->selection_busy = true;
        }
        EnableWindow(app->start, FALSE); EnableWindow(app->logs_button, FALSE);
        app->selection_ready.notify_one();
      }
      return 0;
    case WM_APP + 1: {
      std::lock_guard lock(app->mutex);
      SetWindowTextW(app->status, app->message.c_str());
      { std::lock_guard selection_lock(app->selection_mutex);
        const bool confirm_profile = app->running.load() && app->mock_profile_needs_confirmation.load();
        const bool reconnect_required = confirm_profile && app->mock_profile_reconnect_required.load();
        EnableWindow(app->start, !app->running.load() || (app->awaiting_selection && !app->selection_busy) ||
            (confirm_profile && !reconnect_required && !app->mock_profile_confirmation_requested.load()));
        if (confirm_profile) SetWindowTextW(app->start, reconnect_required ? L"请断开重连" : L"重新确认限幅");
        if (!app->running.load()) {
          ShowWindow(app->can_detail, SW_HIDE);
          for (const auto label : app->config_labels) ShowWindow(label, SW_SHOW);
          for (const auto field : {app->driver_config, app->vehicle_config, app->driver_id, app->password}) ShowWindow(field, SW_SHOW);
          EnableWindow(app->vehicle_list, FALSE);
          EnableWindow(app->logs_button, FALSE);
          SetWindowTextW(app->start, app->mock_control ? L"连接台架" : L"连接视频");
        }
      }
      EnableWindow(app->stop, app->running.load());
      for (const auto field : {app->driver_config, app->vehicle_config, app->driver_id, app->password})
        EnableWindow(field, !app->running.load());
      if (app->mock_estop_button) EnableWindow(app->mock_estop_button, app->running.load());
      return 0;
    }
    case WM_APP + 3: {
      std::lock_guard lock(app->selection_mutex);
      if (!app->awaiting_selection || !app->running.load()) return 0;
      app->selection_busy = false;
      SendMessageW(app->vehicle_list, CB_RESETCONTENT, 0, 0);
      int matching = -1;
      for (std::size_t i = 0; i < app->vehicle_choices.size(); ++i) {
        SendMessageW(app->vehicle_list, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(app->vehicle_choices[i].label.c_str()));
        if (app->vehicle_choices[i].eligible) matching = static_cast<int>(i);
      }
      if (matching >= 0) SendMessageW(app->vehicle_list, CB_SETCURSEL, matching, 0);
      EnableWindow(app->vehicle_list, TRUE); EnableWindow(app->start, TRUE);
      EnableWindow(app->logs_button, TRUE);
      SetWindowTextW(app->start, app->selection_log_mode ? L"收集日志" : L"连接所选");
      SetWindowTextW(app->logs_button, app->selection_log_mode ? L"返回车辆" : L"会话日志");
      return 0;
    }
    case WM_APP + 4:
      EnableWindow(app->vehicle_list, FALSE); EnableWindow(app->start, FALSE);
      EnableWindow(app->logs_button, FALSE);
      for (const auto label : app->config_labels) ShowWindow(label, SW_HIDE);
      for (const auto field : {app->driver_config, app->vehicle_config, app->driver_id, app->password}) ShowWindow(field, SW_HIDE);
      ShowWindow(app->can_detail, SW_SHOW);
      SetWindowTextW(app->start, app->mock_control ? L"连接台架" : L"连接视频");
      return 0;
    case WM_APP + 5: {
      wchar_t filename[MAX_PATH] = L"mine-teleop-session.zip";
      OPENFILENAMEW dialog{};
      dialog.lStructSize = sizeof(dialog); dialog.hwndOwner = hwnd;
      dialog.lpstrFilter = L"ZIP 文件\0*.zip\0\0"; dialog.lpstrFile = filename;
      dialog.nMaxFile = MAX_PATH; dialog.lpstrDefExt = L"zip";
      dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
      const bool chosen = GetSaveFileNameW(&dialog) != FALSE;
      { std::lock_guard lock(app->selection_mutex);
        app->selection_save_path = chosen ? filename : L""; app->selection_save_response = true; }
      app->selection_ready.notify_one();
      return 0;
    }
    case WM_APP + 6:
      EnableWindow(app->start, FALSE); EnableWindow(app->logs_button, FALSE); EnableWindow(app->vehicle_list, FALSE);
      return 0;
    case WM_APP + 2: {
      std::lock_guard lock(app->mutex);
      const bool stale = !app->running.load() || !detail::native_mock_input_fresh(
          app->last_mock_observation_ms.load(), static_cast<std::int64_t>(bench_now_ms()));
      const auto focus = GetFocus();
      const bool input_focused = GetForegroundWindow() == hwnd &&
          (focus == hwnd || focus == app->video[0] || focus == app->video[1]);
      for (std::size_t i = 0; i < app->operator_readouts.size(); ++i) {
        const bool hidden = stale || (i >= 2 && (!input_focused || app->mock_estop_requested.load()));
        app->set_operator_value(i, hidden ? L"—" : app->operator_values[i]);
      }
      app->operator_display_stale = stale;
      const auto details = stale ? std::wstring(L"CAN 反馈未知或遥测已过期") : app->can_detail_message;
      if (text_of(app->can_detail) != details) {
        const auto first_line = SendMessageW(app->can_detail, EM_GETFIRSTVISIBLELINE, 0, 0);
        SetWindowTextW(app->can_detail, details.c_str());
        SendMessageW(app->can_detail, EM_LINESCROLL, 0, first_line);
      }
      if (app->mock_info) app->set_mock_message(
          app->running.load() && app->last_mock_observation_ms.load() > 0 && !detail::native_mock_input_fresh(
              app->last_mock_observation_ms.load(), static_cast<std::int64_t>(bench_now_ms()))
              ? L"控制状态过期，请停止操作\n画面与校时状态未知" : app->mock_message);
      app->mock_ui_posted.store(false);
      return 0;
    }
    case WM_TIMER:
      if (wparam == 2 && app->mock_control) {
        const auto focus = GetFocus();
        const bool focused = app->running.load() && IsWindowVisible(hwnd) && !IsIconic(hwnd) &&
            GetForegroundWindow() == hwnd && (focus == hwnd || focus == app->video[0] || focus == app->video[1]);
        const auto down = [](int key) { return (GetAsyncKeyState(key) & 0x8000) != 0; };
        if (focused && down('E')) app->request_mock_estop();
        if (app->mock_info && app->running.load() && app->last_mock_observation_ms.load() > 0 &&
            !detail::native_mock_input_fresh(app->last_mock_observation_ms.load(), static_cast<std::int64_t>(bench_now_ms())))
          app->set_mock_message(L"控制状态过期，请停止操作\n画面与校时状态未知");
        if (!app->operator_display_stale && !detail::native_mock_input_fresh(
                app->last_mock_observation_ms.load(), static_cast<std::int64_t>(bench_now_ms()))) {
          for (std::size_t i = 0; i < app->operator_readouts.size(); ++i) app->set_operator_value(i, L"—");
          SetWindowTextW(app->can_detail, L"CAN 反馈未知或遥测已过期");
          app->operator_display_stale = true;
        }
        const bool up = down('W') || down(VK_UP), left = down('A') || down(VK_LEFT);
        const bool right = down('D') || down(VK_RIGHT), reverse = down('S') || down(VK_DOWN);
        const bool brake = down(VK_SPACE), hard_brake = down('B');
        detail::NativeMockKeyboard::Gamepad gamepad;
        GamepadConfig gamepad_config;
        int estop_hold_ms;
        { std::lock_guard lock(app->gamepad_mutex); gamepad_config = app->gamepad_config; estop_hold_ms = app->gamepad_estop_hold_ms; }
        XINPUT_STATE state{};
        if (!focused || !gamepad_config.enabled || app->gamepad_index >= XUSER_MAX_COUNT ||
            xinput_state(app->gamepad_index, &state) != ERROR_SUCCESS) {
          app->gamepad_index = XUSER_MAX_COUNT;
          if (focused && gamepad_config.enabled) for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            if (xinput_state(i, &state) == ERROR_SUCCESS) { app->gamepad_index = i; break; }
          }
        }
        const auto now_ms = static_cast<std::int64_t>(bench_now_ms());
        if (focused && app->gamepad_index < XUSER_MAX_COUNT) {
          const auto& p = state.Gamepad;
          const double axis = p.sThumbLX < 0 ? p.sThumbLX / 32768.0 : p.sThumbLX / 32767.0;
          const auto values = detail::NativeWebControl::standard_gamepad(axis, p.bRightTrigger / 255.0,
              p.bLeftTrigger / 255.0, gamepad_config.steering_center, gamepad_config.steering_range,
              gamepad_config.axis_deadzone, gamepad_config.steering_inverted);
          gamepad = {true, values.steering, values.throttle, values.brake, app->gamepad_index};
          if (xinput_button(p, gamepad_config.estop_button)) {
            if (!app->gamepad_estop_since_ms) app->gamepad_estop_since_ms = now_ms;
            if (now_ms - app->gamepad_estop_since_ms >= estop_hold_ms) app->request_mock_estop();
          } else app->gamepad_estop_since_ms = 0;
        } else app->gamepad_estop_since_ms = 0;
        const std::array<bool, 6> displayed{focused && up, focused && left, focused && reverse,
                                            focused && right, focused && brake, focused && hard_brake};
        for (std::size_t i = 0; i < displayed.size(); ++i) {
          if (displayed[i] != app->shown_keys[i]) {
            app->shown_keys[i] = displayed[i]; InvalidateRect(app->keycaps[i], nullptr, TRUE);
          }
        }
        app->keyboard.observe(focused, {up, left, right, brake, hard_brake, reverse}, gamepad, now_ms);
        return 0;
      }
      if (wparam == 1 && app->running.load() && detail::viewer_observation_stale(
          static_cast<std::int64_t>(bench_now_ms()), app->last_observation_ms.load()))
        SetWindowTextW(app->status, app->mock_control ?
            L"状态采样中断，画面可能停滞；请核验控制状态。" :
            L"状态采样中断，画面可能停滞；仅观看，控制禁用。");
      if (wparam == 1 && app->mock_info && app->running.load() && detail::viewer_observation_stale(
          static_cast<std::int64_t>(bench_now_ms()), app->last_mock_observation_ms.load()))
        app->set_mock_message(L"控制状态过期，请停止操作\n画面与校时状态未知");
      return 0;
    case WM_CLOSE:
      KillTimer(hwnd, 1);
      KillTimer(hwnd, 2);
      app->keyboard.withdraw();
      if (app->worker.joinable()) { app->worker.request_stop(); app->worker.join(); }
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      DeleteObject(app->font); DeleteObject(app->title_font); DeleteObject(app->small_font);
      DeleteObject(app->background); DeleteObject(app->surface); DeleteObject(app->input_background);
      PostQuitMessage(0); return 0;
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
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1100, 700, nullptr, nullptr, instance, &app);
  if (!hwnd) return 1;
  ShowWindow(hwnd, show); UpdateWindow(hwnd);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    const bool in_window = message.hwnd == hwnd || IsChild(hwnd, message.hwnd);
    if (in_window && message.message == WM_KEYDOWN && message.wParam == VK_TAB) {
      app.keyboard.withdraw(); app.clear_input_display();
      const bool backwards = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
      if (const auto next = GetNextDlgTabItem(hwnd, GetFocus(), backwards)) SetFocus(next);
      continue;
    }
    if (in_window && message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
        GetFocus() == app.password && IsWindowEnabled(app.start)) {
      SendMessageW(app.start, BM_CLICK, 0, 0);
      continue;
    }
    TranslateMessage(&message); DispatchMessageW(&message);
  }
  return static_cast<int>(message.wParam);
}
