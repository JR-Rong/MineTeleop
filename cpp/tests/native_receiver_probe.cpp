#include "native_receiver_core.hpp"

namespace {
using namespace mine_teleop::bench;
void receive(const Options& opts, const std::string& vehicle_id, DriverConsoleRuntime& driver,
             Receiver& receiver, Json& result, const std::function<bool()>& done, bool embedded,
             std::size_t expected_tracks) {
  try {
    static_cast<void>(driver.login());
    bool online = false;
    const auto deadline = Clock::now() + 4s;
    while (!online && Clock::now() < deadline && !done()) {
      const auto vehicles = driver.vehicles();
      for (const auto& item : vehicles.at("vehicles"))
        if (item.value("vehicle_id", "") == vehicle_id && item.value("online", false)) online = true;
      if (!online) std::this_thread::sleep_for(20ms);
    }
    if (!online) throw std::runtime_error("bench vehicle registration timed out");
    const auto connected = driver.connect(vehicle_id, "native-media-probe");
    static_cast<void>(driver.send_media_capabilities({{"codecs", {"h264"}}}));
    Json scope{{"source_session_id", connected.at("session_id")},
               {"source_control_session_generation", connected.at("control_session_generation")}};
    const auto timeout = Clock::now() + std::chrono::seconds(opts.seconds + (embedded ? 15 : 0));
    const auto setup_deadline = Clock::now() + 60s;
    std::optional<Clock::time_point> window_started;
    std::string negotiation;
    std::deque<Json> early_candidates;
    auto next_stats = Clock::now();
    if (!opts.full_window) result["measurement_start_clock_ms"] = bench_now_ms();
    while (!done() && (opts.full_window || Clock::now() < timeout)) {
      if (opts.full_window && !window_started && Clock::now() >= setup_deadline)
        throw std::runtime_error("full-window receiver did not become ready before the setup deadline");
      if (window_started && Clock::now() - *window_started >= std::chrono::seconds(opts.seconds)) break;
      receiver.check();
      const auto polled = driver.poll_signaling();
      for (const auto& message : polled.at("messages")) {
        const auto type = message.value("type", "");
        const auto payload = message.value("payload", Json::object());
        if (type == "webrtc_offer") {
          negotiation = payload.at("media_negotiation_id").get<std::string>();
          auto answer = scope;
          answer.update({{"type", "answer"}, {"sdp", receiver.answer(payload)}, {"media_negotiation_id", negotiation}});
          static_cast<void>(driver.send_webrtc_answer(answer));
          for (const auto& candidate : early_candidates) receiver.candidate(candidate);
          early_candidates.clear();
        } else if (type == "ice_candidate") {
          if (!negotiation.empty()) receiver.candidate(payload);
          else if (early_candidates.size() < 256) early_candidates.push_back(payload);
          else throw std::runtime_error("early candidate queue overflow");
        }
      }
      for (const auto& candidate : receiver.candidates()) {
        auto outgoing = scope;
        outgoing.update({{"candidate", candidate}, {"media_negotiation_id", negotiation}});
        static_cast<void>(driver.send_webrtc_ice_candidate(outgoing));
      }
      // Preserve the selected pair while the sender is still connected.
      if (!negotiation.empty() && Clock::now() >= next_stats) {
        const auto counters = receiver.counters();
        if (opts.full_window && !window_started && counters.size() == expected_tracks &&
            receiver.selected_pair().is_object() &&
            std::all_of(counters.begin(), counters.end(), [](const auto& track) {
              return track.value("decoded", 0) > 30 && track.value("drawn", 0) > 0;
            })) {
          window_started = Clock::now();
          result["measurement_start_clock_ms"] = bench_now_ms();
        }
        result["webrtc_stats"] = receiver.stats();
        if (!opts.full_window || window_started)
          result["samples"].push_back({{"clock_ms", bench_now_ms()}, {"tracks", counters},
                                        {"webrtc", result["webrtc_stats"]}});
        next_stats = Clock::now() + 1s;
      }
    }
    receiver.check();
    if (opts.full_window) {
      const auto elapsed = window_started ? std::chrono::duration<double>(Clock::now() - *window_started).count() : 0.0;
      result["full_window_elapsed_seconds"] = elapsed;
      result["full_window_completed"] = elapsed >= opts.seconds;
    }
    if (embedded && !done()) throw std::runtime_error("bench duration exceeded");
  } catch (const std::exception& error) { result["error"] = error.what(); }
}

#ifdef MINE_TELEOP_PROBE_EMBEDDED_SENDER
Json run(const Options& opts, VehicleConfig config) {
  const std::string password = random_token(), device_token = random_token();
  SignalingServerConfig signaling;
  signaling.driver_passwords = {{"native-bench", password}};
  signaling.device_tokens = {{config.vehicle_id, device_token}};
  signaling.driver_vehicle_permissions = {{"native-bench", {config.vehicle_id}}};
  // Same-host diagnostic only: identity validation requires an ICE URL.
  // Use an unreachable loopback high port, never Chromium-restricted port 9.
  // This is not a STUN service or a WAN configuration.
  signaling.stun_urls = {"stun:127.0.0.1:34789"}; signaling.turn_urls.clear();
  signaling.api_rate_limit_requests = 12000;
  auto cloud = std::make_shared<SignalingService>(signaling);
  SimpleHttpServer server("127.0.0.1", 0,
      [cloud](const HttpRequest& request) { return cloud->handle(request); }, 8 * 1024 * 1024,
      [cloud](SocketHandle socket, const HttpRequest& request) { return cloud->handle_websocket(socket, request); });
  server.start();
  const auto url = "http://127.0.0.1:" + std::to_string(server.port());
  VehicleMediaRuntime vehicle(config, url, device_token);
  DriverConfig driver_config;
  driver_config.driver_id = "native-bench"; driver_config.signaling_url = url;
  driver_config.browser_event_log_path.clear();
  DriverConsoleRuntime driver(driver_config, config.vehicle_id, password);
  Receiver receiver(opts);
  receiver.start();
  Json vehicle_result;
  std::string vehicle_error;
  std::atomic<bool> vehicle_done{false};
  // Declared last: stop and join the sender before its dependencies are destroyed.
  std::jthread sender([&](std::stop_token token) {
    try { vehicle_result = vehicle.run(0, opts.seconds * 1000, 0, token); }
    catch (const std::exception& error) { vehicle_error = error.what(); }
    vehicle_done = true;
  });
  Json result{{"schema", "native-receiver-bench-v1"}, {"runtime", gst_version_string()},
              {"duration_requested_s", opts.seconds}, {"scope", "same-host WebRTC H264; decoder/consumer and optional GL draw; no scan-out, input, CAN or WAN acceptance"},
              {"control_enabled", false}, {"adapter", "mock"}, {"external_signaling", false},
              {"stun_endpoint", "loopback-unreachable:34789"}, {"turn_configured", false}};
  receive(opts, config.vehicle_id, driver, receiver, result, [&] { return vehicle_done.load(); }, true,
          config.enabled_cameras().size());
  sender.request_stop(); sender.join();
  receiver.stop();
  result["receiver"] = receiver.report();
  result["vehicle"] = vehicle_result;
  if (!vehicle_error.empty()) result["vehicle_error"] = vehicle_error;
  const auto& pair = result["receiver"]["selected_pair"];
  const bool direct_udp = pair.is_object() && pair.value("local_type", "") == "host" &&
      pair.value("remote_type", "") == "host" && pair.value("local_transport", "") == "udp" &&
      pair.value("remote_transport", "") == "udp";
  result["direct_host_udp_observed"] = direct_udp;
  bool frames = result["receiver"]["tracks"].size() == config.enabled_cameras().size();
  for (const auto& track : result["receiver"]["tracks"]) frames = frames && track.at("decoded_frames").get<int>() > 30 && track.at("consumed_frames").get<int>() > 30;
  result["passed"] = !result.contains("error") && vehicle_error.empty() &&
      vehicle_result.value("passed", false) && frames && direct_udp;
  result["pass_criteria"] = "sender acceptance, >30 decoded/consumed frames per configured camera, observed host/UDP pair; not a display or latency SLA";
  try { static_cast<void>(driver.end_session("native_receiver_probe_finished")); } catch (...) {}
  return result;
}

#endif

Json run_remote(const Options& opts, const VehicleConfig& config) {
  const auto* password = std::getenv("MINE_TELEOP_DRIVER_PASSWORD");
  if (!password || !*password) throw std::invalid_argument("MINE_TELEOP_DRIVER_PASSWORD is required");
  DriverConfig driver_config;
  driver_config.driver_id = opts.driver_id; driver_config.signaling_url = opts.signaling_url;
  driver_config.browser_event_log_path.clear();
  DriverConsoleRuntime driver(driver_config, config.vehicle_id, password);
  Receiver receiver(opts);
  receiver.start();
  Json result{{"schema", "native-receiver-remote-v1"}, {"runtime", gst_version_string()},
              {"duration_requested_s", opts.seconds}, {"scope", "receive-only bench; sender containment must be verified separately"},
              {"external_signaling", true}, {"control_tested", false}};
  receive(opts, config.vehicle_id, driver, receiver, result, [] { return false; }, false,
          config.enabled_cameras().size());
  receiver.stop();
  result["receiver"] = receiver.report();
  const auto& pair = result["receiver"]["selected_pair"];
  bool frames = result["receiver"]["tracks"].size() == config.enabled_cameras().size();
  for (const auto& track : result["receiver"]["tracks"])
    frames = frames && track.at("decoded_frames").get<int>() > 30 &&
        (!opts.display || track.at("draw_events_pts_ns_clock_ms").size() > 30);
  result["direct_host_udp_observed"] = pair.is_object() && pair.value("local_type", "") == "host" &&
      pair.value("remote_type", "") == "host" && pair.value("local_transport", "") == "udp" && pair.value("remote_transport", "") == "udp";
  const bool expected_pair = opts.local_addresses.empty() || (pair.is_object() &&
      same_ip_address(opts.local_addresses.front(), pair.value("local_address", "")) &&
      same_ip_address(opts.remote_addresses.front(), pair.value("remote_address", "")));
  result["expected_wan_endpoints_observed"] = opts.local_addresses.empty() ? Json(nullptr) : Json(expected_pair);
  result["passed"] = !result.contains("error") && frames && expected_pair && result["direct_host_udp_observed"].get<bool>() &&
      (!opts.full_window || result.value("full_window_completed", false));
  result["pass_criteria"] = "configured tracks decoded (and GL drawn if requested), host/UDP; no latency or scan-out SLA";
  try { static_cast<void>(driver.end_session("native_receiver_probe_finished")); } catch (...) {}
  return result;
}

// Regression for repeated decoder output observed on the Orin bench. Exercise
// the actual pad filter: duplicate/invalid PTS must not reach its downstream.
bool pts_self_test() {
  Track track;
  std::atomic<unsigned> delivered{};
  GError* error = nullptr;
  auto* pipeline = gst_parse_launch("appsrc name=input format=time ! identity name=decoded ! fakesink name=out sync=false signal-handoffs=true", &error);
  if (error || !pipeline) {
    g_clear_error(&error); if (pipeline) gst_object_unref(pipeline); return false;
  }
  auto* source = gst_bin_get_by_name(GST_BIN(pipeline), "input");
  auto* decoded = gst_bin_get_by_name(GST_BIN(pipeline), "decoded");
  auto* output = gst_bin_get_by_name(GST_BIN(pipeline), "out");
  auto* pad = gst_element_get_static_pad(decoded, "src");
  gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, Track::raw, &track, nullptr);
  g_signal_connect(output, "handoff", G_CALLBACK(+[](GstElement*, GstBuffer*, GstPad*, gpointer data) {
    ++*static_cast<std::atomic<unsigned>*>(data);
  }), &delivered);
  bool ok = gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE;
  const GstClockTime timestamps[] = {0, 33 * GST_MSECOND, 33 * GST_MSECOND, 66 * GST_MSECOND, GST_CLOCK_TIME_NONE, 99 * GST_MSECOND};
  for (const auto pts : timestamps) {
    auto* buffer = gst_buffer_new_allocate(nullptr, 1, nullptr);
    GST_BUFFER_PTS(buffer) = pts;
    if(pts==99*GST_MSECOND)GST_BUFFER_FLAG_SET(buffer,GST_BUFFER_FLAG_CORRUPTED);
    ok = (gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK) && ok;
  }
  static_cast<void>(gst_app_src_end_of_stream(GST_APP_SRC(source)));
  auto* bus = gst_element_get_bus(pipeline);
  auto* message = gst_bus_timed_pop_filtered(bus, 2 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
  ok = ok && message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
  if (message) gst_message_unref(message);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(bus); gst_object_unref(pad); gst_object_unref(output);
  gst_object_unref(decoded); gst_object_unref(source); gst_object_unref(pipeline);
  return ok && delivered == 3 && track.decoded == 3 && track.decoded_buffers == 6 &&
      track.duplicate_decoded_pts == 1 && track.invalid_decoded_pts == 1 && track.corrupt_decoded_buffers == 1;
}

bool wan_candidate_self_test() {
  const std::vector<std::string> allowed{"2001:db8::2"};
  const std::string good = "candidate:1 1 UDP 123 2001:0db8:0:0:0:0:0:2 8000 typ host";
  const std::string bad_ip = "candidate:2 1 UDP 123 2001:db8::3 8000 typ host";
  const std::string relay = "candidate:3 1 UDP 123 2001:db8::2 8000 typ relay";
  const std::string tcp = "candidate:4 1 TCP 123 2001:db8::2 8000 typ host";
  const auto sdp = filter_bench_remote_sdp("v=0\r\na=" + good + "\r\na=" + bad_ip + "\r\na=" + relay + "\r\n", allowed);
  return allowed_bench_remote_candidate(good, allowed) && !allowed_bench_remote_candidate(bad_ip, allowed) &&
      !allowed_bench_remote_candidate(relay, allowed) && !allowed_bench_remote_candidate(tcp, allowed) &&
      !allowed_bench_remote_candidate("invalid", allowed) && sdp.find(good) != std::string::npos &&
      sdp.find(bad_ip) == std::string::npos && sdp.find(relay) == std::string::npos;
}
#ifdef __APPLE__
bool interface_binding_self_test(const char* address) {
  auto* context = g_main_context_new();
  g_main_context_push_thread_default(context);
  auto* agent = nice_agent_new(context, NICE_COMPATIBILITY_RFC5245);
  g_object_set(agent, "ice-tcp", FALSE, nullptr);
  NiceAddress local;
  nice_address_init(&local);
  bool ok = nice_address_set_from_string(&local, address) && nice_agent_add_local_address(agent, &local);
  const auto stream = nice_agent_add_stream(agent, 1);
  InterfaceBinding binding("lo0");
  g_signal_connect(agent, "new-candidate-full", G_CALLBACK(InterfaceBinding::on_candidate), &binding);
  ok = nice_agent_gather_candidates(agent, stream) && ok;
  const auto deadline = Clock::now() + 2s;
  while (!binding.healthy() && Clock::now() < deadline) {
    while (g_main_context_iteration(context, FALSE)) {}
    g_usleep(1000);
  }
  ok = ok && binding.healthy();
  g_signal_handlers_disconnect_by_data(agent, &binding);
  g_object_unref(agent);
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
  return ok;
}
#endif
}  // namespace

int probe_main(int argc, char** argv, void*) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
      gst_init(nullptr, nullptr);
      const bool pts_passed = pts_self_test();
      const bool wan_passed = wan_candidate_self_test();
      bool passed = pts_passed && wan_passed;
      std::cout << "decoder PTS admission: " << (pts_passed ? "PASS" : "FAIL") << '\n';
      std::cout << "WAN candidate filtering: " << (wan_passed ? "PASS" : "FAIL") << '\n';
#ifdef __APPLE__
      const bool ipv4_passed = interface_binding_self_test("127.0.0.1");
      const bool ipv6_passed = interface_binding_self_test("::1");
      std::cout << "physical interface binding IPv4/IPv6: " << (ipv4_passed && ipv6_passed ? "PASS" : "FAIL") << '\n';
      passed = passed && ipv4_passed && ipv6_passed;
#endif
      return passed ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "Media-only WebRTC bench; no control or scan-out acceptance.\n"
                   "--config FILE --output NEW_FILE [--seconds 5..1800] [--jitter-ms 0..200]\n"
                   "[--decoder avdec_h264|nvv4l2decoder|vtdec_hw] [--consumer-stall-ms 0..500]\n"
                   "[--signaling-url http://127.0.0.1:PORT --driver-id ID] [--display]\n"
                   "[--wan-family ipv4|ipv6 --wan-local-address IP --wan-remote-address IP]\n"
                   "[--full-window] starts the duration only after both decoding and GL draw are observed.\n"
                   "Remote mode requires MINE_TELEOP_DRIVER_PASSWORD and a separately contained sender.\n"
                   "--config FILE --validate-only checks containment without starting a runtime.\n";
      return 0;
    }
    const auto opts = options(argc, argv);
    const auto config = load_vehicle_config(opts.config);
    validate(config);
    if (opts.validate_only) { std::cout << "bench config accepted; no runtime started\n"; return 0; }
    // Refuse overwriting evidence before starting any network or camera work.
    if (std::filesystem::exists(opts.output)) throw std::invalid_argument("--output must be a new file");
    std::ofstream output(opts.output);
    if (!output) throw std::runtime_error("could not create output file");
    gst_init(nullptr, nullptr);
    Json result;
    try {
      if (!opts.signaling_url.empty()) result = run_remote(opts, config);
#ifdef MINE_TELEOP_PROBE_EMBEDDED_SENDER
      else result = run(opts, config);
#endif
    }
    catch (const std::exception& error) {
      result = {{"schema", "native-receiver-bench-v1"}, {"passed", false}, {"startup_error", error.what()}};
    }
    output << result.dump(2) << '\n'; output.close();
    if (!output) throw std::runtime_error("could not write report");
    std::cout << Json{{"event", "native_receiver_probe_complete"}, {"passed", result.at("passed")}, {"output", opts.output}}.dump() << '\n';
    return result.at("passed").get<bool>() ? 0 : 1;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

int main(int argc, char** argv) {
#ifdef __APPLE__
  // Cocoa/GL must own the main thread; GStreamer runs the probe on a worker.
  return gst_macos_main(probe_main, argc, argv, nullptr);
#else
  return probe_main(argc, argv, nullptr);
#endif
}
