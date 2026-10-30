// Windows bench viewer: native windows and native WebRTC, with no control input.
#include "native_receiver_core.hpp"
#include "mine_teleop/detail/native_viewer_observation.hpp"
#include <windows.h>
#include <shellapi.h>
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

struct Window {
  HWND hwnd{}, driver_config{}, vehicle_config{}, password{}, status{}, start{}, stop{};
  std::array<HWND, 2> video{};
  std::mutex mutex;
  std::wstring message{L"仅观看台架：控制禁用。车端隔离须独立核验。"};
  std::jthread worker;
  std::atomic<bool> running{false};
  std::atomic<std::int64_t> last_observation_ms{0};

  void report(std::wstring value) {
    { std::lock_guard lock(mutex); message = std::move(value); }
    last_observation_ms = static_cast<std::int64_t>(bench_now_ms());
    PostMessageW(hwnd, WM_APP + 1, 0, 0);
  }

  void connect(std::string driver_path, std::string vehicle_path, std::string secret,
               std::stop_token stop_token) {
    const char* phase = "configuration";
    try {
      const auto vehicle = load_vehicle_config(vehicle_path);
      validate(vehicle);  // Local configuration is not remote containment evidence.
      if (vehicle.enabled_cameras().size() > video.size())
        throw std::runtime_error("viewer supports at most two camera tracks");
      auto config = load_driver_config(driver_path);
      config.receive_only = true;
      config.max_time_sync_uncertainty_ms = vehicle.field_safety.max_time_sync_uncertainty_ms;
      config.time_sync_interval_ms = vehicle.field_safety.time_sync_interval_ms;
      config.time_sync_samples = vehicle.field_safety.time_sync_samples;
      config.browser_event_log_path.clear();
      DriverConsoleRuntime driver(config, vehicle.vehicle_id, secret);
      std::fill(secret.begin(), secret.end(), '\0');
      bool connected = false;
      try {
        report(L"正在登录并连接车端；仅观看，控制禁用。");
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
              options.display = true;
              options.ice_servers = payload.value("ice_servers", ice);
              options.ice_route_mode = payload.value("ice_route_mode", "all");
              for (const auto handle : video) options.window_handles.push_back(reinterpret_cast<std::uintptr_t>(handle));
              receiver = std::make_unique<Receiver>(options);
              receiver->start();
              negotiation = payload.at("media_negotiation_id").get<std::string>();
              auto answer = scope;
              answer.update({{"type", "answer"}, {"sdp", receiver->answer(payload)}, {"media_negotiation_id", negotiation}});
              static_cast<void>(driver.send_webrtc_answer(answer));
              for (const auto& candidate : early) receiver->candidate(candidate);
              early.clear();
              report(L"已提交媒体应答，等待画面；仅观看，控制禁用。");
            } else if (type == "ice_candidate") {
              phase = "remote_candidate";
              if (receiver) receiver->candidate(payload);
              else if (early.size() < 256) early.push_back(payload);
              else throw std::runtime_error("early ICE candidate limit reached");
            }
          }
          if (receiver) {
            phase = "local_candidates";
            for (const auto& candidate : receiver->candidates()) {
              auto outgoing = scope;
              outgoing.update({{"candidate", candidate}, {"media_negotiation_id", negotiation}});
              static_cast<void>(driver.send_webrtc_ice_candidate(outgoing));
            }
            if (Clock::now() >= next_observation) {
              phase = "observation";
              const auto tracks = receiver->counters();
              std::vector<std::uint64_t> current;
              for (const auto& track : tracks) current.push_back(track.value("drawn", std::uint64_t{}));
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
              report((advancing ? L"绘制回调持续更新 · " : L"等待新画面或画面停滞 · ") + path +
                  L" · 仅观看，控制禁用");
              next_observation = Clock::now() + 1s;
            }
          }
          std::this_thread::sleep_for(10ms);
        }
        if (receiver) receiver->stop();
      } catch (...) {
        if (connected) { try { static_cast<void>(driver.end_session("native_viewer_error")); } catch (...) {} }
        throw;
      }
      if (connected) { try { static_cast<void>(driver.end_session("native_viewer_disconnect")); } catch (...) {} }
      report(L"已断开；控制始终禁用。");
    } catch (const std::exception& error) {
      // Report only a fixed category and phase: network errors can contain
      // addresses or credentials and must not be copied into diagnostic logs.
      const std::string_view detail = error.what();
      const char* category = detail.find("native view has no matching media pad") != std::string_view::npos ? "unmapped_pad" :
          detail.find("decode track pad link noformat") != std::string_view::npos ? "track_link_noformat" :
          detail.find("decode track pad link failed") != std::string_view::npos ? "track_link_other" :
          detail.find("decode track state sync failed") != std::string_view::npos ? "track_state_sync" :
          detail.find("GStreamer promise timed out") != std::string_view::npos ? "promise_timeout" : "other";
      std::fprintf(stderr, "native_viewer_failure phase=%s category=%s\n", phase, category);
      std::fill(secret.begin(), secret.end(), '\0');
      report(stop_token.stop_requested() ? L"已取消连接。" :
          L"连接或视频失败。请检查配置、台架隔离、信令和 GStreamer 依赖。");
    }
    running = false;
    PostMessageW(hwnd, WM_APP + 1, 0, 0);
  }
};

HWND control(HWND parent, const wchar_t* type, const wchar_t* caption, DWORD style,
             int x, int y, int w, int h, int id = 0) {
  return CreateWindowExW(0, type, caption, WS_CHILD | WS_VISIBLE | style, x, y, w, h,
      parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
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
      EnableWindow(app->stop, FALSE);
      app->status = control(hwnd, L"STATIC", app->message.c_str(), 0, 12, 108, 900, 25);
      app->video[0] = control(hwnd, L"STATIC", L"", SS_BLACKRECT, 12, 140, 460, 300);
      app->video[1] = control(hwnd, L"STATIC", L"", SS_BLACKRECT, 480, 140, 460, 300);
      SetTimer(hwnd, 1, 1000, nullptr);
      return 0;
    case WM_SIZE: {
      const int width = LOWORD(lparam), height = HIWORD(lparam);
      const int panel_width = std::max(1, (width - 36) / 2), panel_height = std::max(1, height - 152);
      MoveWindow(app->video[0], 12, 140, panel_width, panel_height, TRUE);
      MoveWindow(app->video[1], 24 + panel_width, 140, panel_width, panel_height, TRUE);
      return 0;
    }
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
        } catch (...) { app->running = false; app->report(L"请输入有效的配置路径和密码。"); }
      } else if (LOWORD(wparam) == 2 && app->worker.joinable()) {
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
      if (wparam == 1 && app->running.load() && detail::viewer_observation_stale(
          static_cast<std::int64_t>(bench_now_ms()), app->last_observation_ms.load()))
        SetWindowTextW(app->status, L"状态采样中断，画面可能停滞；仅观看，控制禁用。");
      return 0;
    case WM_CLOSE:
      KillTimer(hwnd, 1);
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
  if (argv && argc == 3 && std::wstring(argv[1]) == L"--validate-config") {
    int result = 0;
    try {
      const auto config = mine_teleop::load_vehicle_config(utf8(argv[2]));
      mine_teleop::bench::validate(config);
      if (config.enabled_cameras().size() > 2) result = 1;
    } catch (...) { result = 1; }
    LocalFree(argv);
    return result;
  }
  if (argv) LocalFree(argv);
  gst_init(nullptr, nullptr);
  Window app;
  WNDCLASSW klass{};
  klass.lpfnWndProc = procedure; klass.hInstance = instance; klass.lpszClassName = L"MineTeleopNativeViewer";
  klass.hCursor = LoadCursor(nullptr, IDC_ARROW);
  klass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
  if (!RegisterClassW(&klass)) return 1;
  const auto hwnd = CreateWindowExW(0, klass.lpszClassName, L"MineTeleop Windows 台架视频（仅观看）",
      WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 980, 530, nullptr, nullptr, instance, &app);
  if (!hwnd) return 1;
  ShowWindow(hwnd, show); UpdateWindow(hwnd);
  MSG message{};
  while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
  return static_cast<int>(message.wParam);
}
