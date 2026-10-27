// Opt-in media-only bench. Reuses the product sender and session fencing, but
// never creates a vehicle control service, sends input or claims presentation.
#define GST_USE_UNSTABLE_API
#include "mine_teleop/server.hpp"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/webrtc/webrtc.h>
#include <nice/agent.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace {
using namespace mine_teleop;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
double now_ms() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }

struct Options {
  std::string config, output, decoder{"avdec_h264"};
  int seconds{30}, jitter_ms{30}, stall_ms{0};
  bool validate_only{false};
};
int integer(const std::string& text, int minimum, int maximum) {
  int result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result < minimum || result > maximum)
    throw std::invalid_argument("numeric argument outside permitted range");
  return result;
}
Options options(int argc, char** argv) {
  Options result;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--validate-only") { result.validate_only = true; continue; }
    if (i + 1 >= argc) throw std::invalid_argument("missing argument: " + flag);
    const std::string value = argv[++i];
    if (flag == "--config") result.config = value;
    else if (flag == "--output") result.output = value;
    else if (flag == "--seconds") result.seconds = integer(value, 5, 300);
    else if (flag == "--jitter-ms") result.jitter_ms = integer(value, 0, 200);
    else if (flag == "--consumer-stall-ms") result.stall_ms = integer(value, 0, 500);
    else if (flag == "--decoder" && (value == "avdec_h264" || value == "nvv4l2decoder")) result.decoder = value;
    else throw std::invalid_argument("unknown option: " + flag);
  }
  if (result.config.empty()) throw std::invalid_argument("--config is required");
  if (!result.validate_only && result.output.empty()) throw std::invalid_argument("--output is required");
  return result;
}
void validate(const VehicleConfig& config) {
  if (config.runtime.control_enabled || config.vehicle_adapter.type != "mock" ||
      config.field_safety.commissioning_mode != "bench" || config.field_safety.max_speed_kph != 0 ||
      config.recording.enabled || config.upload.enabled || !config.runtime.media_enabled)
    throw std::invalid_argument("requires media enabled, control disabled, mock adapter, bench, zero speed, recording/upload disabled");
  const auto cameras = config.enabled_cameras();
  if (cameras.empty() || cameras.size() > 8) throw std::invalid_argument("requires 1..8 enabled cameras");
  for (const auto& camera : cameras) {
    if (config.realtime_profile(camera.realtime_profile).codec != "h264")
      throw std::invalid_argument("native receiver probe supports H.264 only");
  }
}

// Bounded observations. Quantiles use all stored samples; truncation is explicit.
struct Samples {
  std::vector<double> values;
  std::uint64_t count{}, omitted{};
  double sum{}, maximum{};
  void add(double value) {
    if (!std::isfinite(value) || value < 0) return;
    ++count; sum += value; maximum = std::max(maximum, value);
    if (values.size() < 131072) values.push_back(value); else ++omitted;
  }
  Json json() const {
    auto sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const auto percentile = [&](double fraction) -> Json {
      if (sorted.empty()) return nullptr;
      return sorted[static_cast<std::size_t>(std::ceil(fraction * static_cast<double>(sorted.size()))) - 1];
    };
    return {{"count", count}, {"omitted", omitted}, {"mean_ms", count ? Json(sum / static_cast<double>(count)) : Json(nullptr)},
            {"p50_ms", percentile(.50)}, {"p95_ms", percentile(.95)}, {"p99_ms", percentile(.99)},
            {"max_ms", count ? Json(maximum) : Json(nullptr)}};
  }
};
struct StructureDelete { void operator()(GstStructure* value) const { if (value) gst_structure_free(value); } };
using Structure = std::unique_ptr<GstStructure, StructureDelete>;
struct PromiseState {
  std::mutex mutex;
  std::condition_variable condition;
  bool done{};
  Structure reply;
};
template<typename Emit> Structure request(Emit emit) {
  auto state = std::make_shared<PromiseState>();
  auto* promise = gst_promise_new_with_change_func([](GstPromise* value, gpointer data) {
    auto shared = *static_cast<std::shared_ptr<PromiseState>*>(data);
    std::lock_guard lock(shared->mutex);
    if (gst_promise_wait(value) == GST_PROMISE_RESULT_REPLIED) {
      if (const auto* reply = gst_promise_get_reply(value)) shared->reply.reset(gst_structure_copy(reply));
    }
    shared->done = true;
    shared->condition.notify_one();
  }, new std::shared_ptr<PromiseState>(state), [](gpointer data) {
    delete static_cast<std::shared_ptr<PromiseState>*>(data);
  });
  emit(promise);
  std::unique_lock lock(state->mutex);
  const bool completed = state->condition.wait_for(lock, 5s, [&] { return state->done; });
  lock.unlock();
  if (!completed) gst_promise_interrupt(promise);
  gst_promise_unref(promise);
  if (!completed) throw std::runtime_error("GStreamer promise timed out");
  if (state->reply && gst_structure_has_field(state->reply.get(), "error")) {
    gchar* text = gst_structure_to_string(state->reply.get());
    const std::string message = text ? text : "GStreamer promise failed";
    g_free(text);
    throw std::runtime_error(message);
  }
  return std::move(state->reply);
}
Json structure_json(const GstStructure* structure) {
  Json result = Json::object();
  if (!structure) return result;
  for (int i = 0; i < gst_structure_n_fields(structure); ++i) {
    const char* name = gst_structure_nth_field_name(structure, i);
    const GValue* value = gst_structure_get_value(structure, name);
    if (GST_VALUE_HOLDS_STRUCTURE(value)) result[name] = structure_json(gst_value_get_structure(value));
    else if (G_VALUE_HOLDS_STRING(value)) result[name] = g_value_get_string(value) ? g_value_get_string(value) : "";
    else if (G_VALUE_HOLDS_UINT(value)) result[name] = g_value_get_uint(value);
    else if (G_VALUE_HOLDS_UINT64(value)) result[name] = g_value_get_uint64(value);
    else if (G_VALUE_HOLDS_INT(value)) result[name] = g_value_get_int(value);
    else if (G_VALUE_HOLDS_INT64(value)) result[name] = g_value_get_int64(value);
    else if (G_VALUE_HOLDS_DOUBLE(value)) result[name] = g_value_get_double(value);
    else if (G_VALUE_HOLDS_BOOLEAN(value)) result[name] = static_cast<bool>(g_value_get_boolean(value));
    else { gchar* text = gst_value_serialize(value); result[name] = text ? text : ""; g_free(text); }
  }
  return result;
}

struct Track {
  std::string pad;
  GstElement* sink{};  // owned reference until the consumer stops
  std::jthread consumer;
  std::mutex mutex;
  std::map<GstClockTime, double> compressed_at, decoded_at;
  std::unordered_set<std::uint64_t> rtp_timestamps, decoded_pts;
  std::uint64_t rtp_packets{}, decoded_buffers{}, duplicate_decoded_pts{}, invalid_decoded_pts{}, identity_overflow{};
  Samples decode_ms, decoded_gap_ms, consumed_gap_ms, consume_wait_ms;
  std::uint64_t decoded{}, consumed{}, unmatched_decode{}, unmatched_consume{};
  double first{}, last_decoded{}, last_consumed{};
  int stalls{};
  ~Track() { stop(); if (sink) gst_object_unref(sink); }
  void stop() { consumer.request_stop(); if (consumer.joinable()) consumer.join(); }
  static void remember(std::map<GstClockTime, double>& times, GstClockTime pts, double time) {
    if (GST_CLOCK_TIME_IS_VALID(pts)) times[pts] = time;
    while (times.size() > 128) times.erase(times.begin());
  }
  static GstPadProbeReturn rtp(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto& self = *static_cast<Track*>(data);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstRTPBuffer packet = GST_RTP_BUFFER_INIT;
    if (buffer && gst_rtp_buffer_map(buffer, GST_MAP_READ, &packet)) {
      const auto identity = (static_cast<std::uint64_t>(gst_rtp_buffer_get_ssrc(&packet)) << 32) |
          gst_rtp_buffer_get_timestamp(&packet);
      gst_rtp_buffer_unmap(&packet);
      std::lock_guard lock(self.mutex);
      ++self.rtp_packets;
      if (self.rtp_timestamps.size() < 131072) self.rtp_timestamps.insert(identity); else ++self.identity_overflow;
    }
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn compressed(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto& self = *static_cast<Track*>(data);
    const auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) { std::lock_guard lock(self.mutex); remember(self.compressed_at, GST_BUFFER_PTS(buffer), now_ms()); }
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn raw(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto& self = *static_cast<Track*>(data);
    const auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) {
      const double now = now_ms();
      std::lock_guard lock(self.mutex);
      ++self.decoded_buffers;
      const auto pts = GST_BUFFER_PTS(buffer);
      if (!GST_CLOCK_TIME_IS_VALID(pts)) { ++self.invalid_decoded_pts; return GST_PAD_PROBE_DROP; }
      if (self.decoded_pts.size() >= 131072) { ++self.identity_overflow; return GST_PAD_PROBE_DROP; }
      // Decoder drain may repeat an output PTS. It must not refresh frame age,
      // inflate FPS or replace the newest sample with a repeat of old content.
      if (!self.decoded_pts.insert(pts).second) { ++self.duplicate_decoded_pts; return GST_PAD_PROBE_DROP; }
      const auto found = self.compressed_at.find(GST_BUFFER_PTS(buffer));
      if (found != self.compressed_at.end()) { self.decode_ms.add(now - found->second); self.compressed_at.erase(found); }
      else ++self.unmatched_decode;
      remember(self.decoded_at, GST_BUFFER_PTS(buffer), now);
      if (self.last_decoded > 0) self.decoded_gap_ms.add(now - self.last_decoded); else self.first = now;
      self.last_decoded = now;
      ++self.decoded;
    }
    return GST_PAD_PROBE_OK;
  }
  void start(int stall_ms) {
    consumer = std::jthread([this, stall_ms](std::stop_token stop) {
      auto next_stall = Clock::now() + 1s;
      while (!stop.stop_requested()) {
        if (stall_ms > 0 && Clock::now() >= next_stall) {
          std::this_thread::sleep_for(std::chrono::milliseconds(stall_ms));
          next_stall = Clock::now() + 1s;
          std::lock_guard lock(mutex); ++stalls;
        }
        GstSample* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 50 * GST_MSECOND);
        if (!sample) { if (gst_app_sink_is_eos(GST_APP_SINK(sink))) break; continue; }
        const double now = now_ms();
        const auto* buffer = gst_sample_get_buffer(sample);
        {
          std::lock_guard lock(mutex);
          const auto found = decoded_at.find(GST_BUFFER_PTS(buffer));
          if (found != decoded_at.end()) { consume_wait_ms.add(now - found->second); decoded_at.erase(found); }
          else ++unmatched_consume;
          if (last_consumed > 0) consumed_gap_ms.add(now - last_consumed);
          last_consumed = now; ++consumed;
        }
        gst_sample_unref(sample);
      }
    });
  }
  Json json() {
    std::lock_guard lock(mutex);
    return {{"pad", pad}, {"decoded_frames", decoded}, {"decoded_buffers", decoded_buffers}, {"consumed_frames", consumed},
            {"rtp_packets", rtp_packets}, {"rtp_unique_frame_timestamps", rtp_timestamps.size()},
            {"decoded_unique_pts", decoded_pts.size()}, {"duplicate_decoded_pts", duplicate_decoded_pts},
            {"invalid_decoded_pts", invalid_decoded_pts}, {"identity_observation_overflow", identity_overflow},
            {"not_consumed_frames", decoded - consumed}, {"injected_consumer_stalls", stalls},
            {"decoded_fps", decoded > 1 && last_decoded > first ? (static_cast<double>(decoded - 1) * 1000 / (last_decoded - first)) : 0},
            {"depay_to_decode", decode_ms.json()}, {"decoded_frame_gap", decoded_gap_ms.json()},
            {"consumed_frame_gap", consumed_gap_ms.json()}, {"decode_to_consume", consume_wait_ms.json()},
            {"unmatched_decode_pts", unmatched_decode}, {"unmatched_consume_pts", unmatched_consume}};
  }
};

class Receiver {
 public:
  explicit Receiver(const Options& options) : options_(options) {
    pipeline_ = gst_pipeline_new("native-receiver");
    peer_ = gst_element_factory_make("webrtcbin", "peer");
    if (!peer_) { gst_object_unref(pipeline_); pipeline_ = nullptr; throw std::runtime_error("webrtcbin unavailable"); }
    gst_bin_add(GST_BIN(pipeline_), peer_);
    g_object_set(peer_, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE,
                 "latency", static_cast<guint>(options.jitter_ms), nullptr);
    g_signal_connect(peer_, "pad-added", G_CALLBACK(on_pad), this);
    g_signal_connect(peer_, "on-ice-candidate", G_CALLBACK(on_candidate), this);
  }
  ~Receiver() { stop(); if (nice_) g_object_unref(nice_); gst_object_unref(pipeline_); }
  void start() {
    GObject* ice = nullptr;
    g_object_get(peer_, "ice-agent", &ice, nullptr);
    if (ice) {
      if (g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent")) g_object_get(ice, "agent", &nice_, nullptr);
      g_object_unref(ice);
    }
    if (!nice_) throw std::runtime_error("libnice selected-pair observation unavailable");
    g_signal_connect(nice_, "new-selected-pair-full", G_CALLBACK(on_pair), this);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      throw std::runtime_error("receiver failed to start");
  }
  void stop() {
    if (stopped_) return;
    stopped_ = true;
    g_signal_handlers_disconnect_by_data(peer_, this);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (nice_) g_signal_handlers_disconnect_by_data(nice_, this);
    for (auto& track : tracks_) track->stop();
  }
  std::string answer(const Json& offer) {
    if (!negotiation_.empty()) throw std::runtime_error("probe stops on renegotiation; new measurement required");
    negotiation_ = offer.at("media_negotiation_id").get<std::string>();
    const auto text = offer.at("sdp").get<std::string>();
    GstSDPMessage* sdp = nullptr;
    if (gst_sdp_message_new(&sdp) != GST_SDP_OK) throw std::runtime_error("SDP allocation failed");
    if (gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(text.data()), static_cast<guint>(text.size()), sdp) != GST_SDP_OK) {
      gst_sdp_message_free(sdp); throw std::runtime_error("invalid SDP offer");
    }
    auto* description = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
    try { request([&](GstPromise* promise) { g_signal_emit_by_name(peer_, "set-remote-description", description, promise); }); }
    catch (...) { gst_webrtc_session_description_free(description); throw; }
    gst_webrtc_session_description_free(description);
    auto reply = request([&](GstPromise* promise) { g_signal_emit_by_name(peer_, "create-answer", nullptr, promise); });
    GstWebRTCSessionDescription* result = nullptr;
    if (!reply || !gst_structure_get(reply.get(), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &result, nullptr) || !result)
      throw std::runtime_error("answer unavailable");
    try { request([&](GstPromise* promise) { g_signal_emit_by_name(peer_, "set-local-description", result, promise); }); }
    catch (...) { gst_webrtc_session_description_free(result); throw; }
    gchar* answer_text = gst_sdp_message_as_text(result->sdp);
    const std::string answer = answer_text ? answer_text : "";
    g_free(answer_text); gst_webrtc_session_description_free(result);
    return answer;
  }
  void candidate(const Json& payload) {
    if (payload.value("media_negotiation_id", "") != negotiation_) return;
    const auto candidate = payload.contains("candidate") && payload["candidate"].is_object() ? payload["candidate"] : payload;
    g_signal_emit_by_name(peer_, "add-ice-candidate", candidate.value("sdpMLineIndex", 0U), candidate.at("candidate").get<std::string>().c_str());
  }
  std::deque<Json> candidates() { std::lock_guard lock(mutex_); std::deque<Json> result; result.swap(candidates_); return result; }
  void check() {
    { std::lock_guard lock(mutex_); if (!error_.empty()) throw std::runtime_error(error_); }
    auto* bus = gst_element_get_bus(pipeline_);
    auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    gst_object_unref(bus);
    if (message) {
      GError* error = nullptr; gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      const std::string text = error ? error->message : "receiver pipeline error";
      g_clear_error(&error); g_free(debug); gst_message_unref(message);
      throw std::runtime_error(text);
    }
  }
  Json stats() {
    auto result = request([&](GstPromise* promise) { g_signal_emit_by_name(peer_, "get-stats", nullptr, promise); });
    return structure_json(result.get());
  }
  Json report() {
    Json tracks = Json::array();
    std::lock_guard lock(mutex_);
    for (auto& track : tracks_) tracks.push_back(track->json());
    return {{"tracks", tracks}, {"selected_pair", selected_pair_}, {"selected_pair_changes", pair_changes_},
            {"local_candidates", local_candidates_}, {"jitter_target_ms", options_.jitter_ms},
            {"appsink_max_buffers", 1}, {"appsink_drop", true}, {"sink_sync", false}, {"decoder", options_.decoder},
            {"display_presented", false}, {"control_tested", false}};
  }
 private:
  static const char* candidate_type(NiceCandidateType type) {
    switch (type) {
      case NICE_CANDIDATE_TYPE_HOST: return "host";
      case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE: return "srflx";
      case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE: return "prflx";
      case NICE_CANDIDATE_TYPE_RELAYED: return "relay";
    }
    return "unknown";
  }
  static void on_pair(NiceAgent*, guint stream, guint component, NiceCandidate* local, NiceCandidate* remote, gpointer data) {
    auto& self = *static_cast<Receiver*>(data);
    if (!local || !remote) return;
    std::lock_guard lock(self.mutex_);
    ++self.pair_changes_;
    self.selected_pair_ = {{"source", "libnice-new-selected-pair-full"}, {"stream", stream}, {"component", component},
        {"local_type", candidate_type(local->type)}, {"remote_type", candidate_type(remote->type)},
        {"local_transport", local->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "udp" : "tcp"},
        {"remote_transport", remote->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "udp" : "tcp"}};
  }
  static void on_candidate(GstElement*, guint index, gchar* text, gpointer data) {
    auto& self = *static_cast<Receiver*>(data);
    if (!text || !*text) return;
    std::lock_guard lock(self.mutex_);
    ++self.local_candidates_;
    if (self.candidates_.size() >= 256) { self.error_ = "candidate queue overflow"; return; }
    self.candidates_.push_back({{"candidate", text}, {"sdpMLineIndex", index}});
  }
  static void on_pad(GstElement*, GstPad* pad, gpointer data) {
    auto& self = *static_cast<Receiver*>(data);
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
    try { self.add_track(pad); }
    catch (const std::exception& error) { std::lock_guard lock(self.mutex_); self.error_ = error.what(); }
  }
  void add_track(GstPad* pad) {
    std::lock_guard lock(mutex_);
    if (tracks_.size() >= 8) throw std::runtime_error("too many receiver tracks");
    GError* error = nullptr;
    const auto decoder = options_.decoder == "nvv4l2decoder"
        ? "nvv4l2decoder name=decoder disable-dpb=true" : "avdec_h264 name=decoder max-threads=2";
    const auto description = std::string("rtph264depay name=depay wait-for-keyframe=true request-keyframe=true ! h264parse ! ") +
        decoder + " ! appsink name=out sync=false max-buffers=1 drop=true wait-on-eos=false";
    auto* bin = gst_parse_bin_from_description(description.c_str(), TRUE, &error);
    if (error || !bin) {
      const std::string message = error ? error->message : "decode bin unavailable";
      g_clear_error(&error); if (bin) gst_object_unref(bin); throw std::runtime_error(message);
    }
    gst_bin_add(GST_BIN(pipeline_), bin);
    auto track = std::make_unique<Track>();
    track->pad = GST_PAD_NAME(pad);
    track->sink = gst_bin_get_by_name(GST_BIN(bin), "out");
    for (const auto& [name, callback] : {std::pair{"depay", Track::compressed}, std::pair{"decoder", Track::raw}}) {
      auto* element = gst_bin_get_by_name(GST_BIN(bin), name);
      auto* source = gst_element_get_static_pad(element, "src");
      gst_pad_add_probe(source, GST_PAD_PROBE_TYPE_BUFFER, callback, track.get(), nullptr);
      gst_object_unref(source); gst_object_unref(element);
    }
    // Keep callback data alive even if linking or state synchronisation fails.
    auto* track_ptr = track.get();
    tracks_.push_back(std::move(track));
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, Track::rtp, track_ptr, nullptr);
    auto* input = gst_element_get_static_pad(bin, "sink");
    const auto linked = gst_pad_link(pad, input);
    gst_object_unref(input);
    if (linked != GST_PAD_LINK_OK || !gst_element_sync_state_with_parent(bin)) throw std::runtime_error("decode track link/start failed");
    track_ptr->start(options_.stall_ms);
  }
  Options options_;
  GstElement* pipeline_{}, *peer_{};
  NiceAgent* nice_{};
  bool stopped_{};
  std::string negotiation_, error_;
  std::mutex mutex_;
  std::deque<Json> candidates_;
  std::vector<std::unique_ptr<Track>> tracks_;
  unsigned local_candidates_{};
  unsigned pair_changes_{};
  Json selected_pair_;
};

Json run(const Options& opts, VehicleConfig config) {
  const std::string password = random_token(), device_token = random_token();
  SignalingServerConfig signaling;
  signaling.driver_passwords = {{"native-bench", password}};
  signaling.device_tokens = {{config.vehicle_id, device_token}};
  signaling.driver_vehicle_permissions = {{"native-bench", {config.vehicle_id}}};
  // Product identity validation requires an ICE URL. This loopback discard
  // endpoint cannot relay traffic; the probe deliberately uses host candidates.
  signaling.stun_urls = {"stun:127.0.0.1:9"}; signaling.turn_urls.clear();
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
              {"duration_requested_s", opts.seconds}, {"scope", "same-host WebRTC H264; decoder/consumer only; no display, input, CAN or WAN acceptance"},
              {"control_enabled", false}, {"adapter", "mock"}, {"external_signaling", false},
              {"stun_endpoint", "loopback-discard:9"}, {"turn_configured", false}};
  try {
    static_cast<void>(driver.login());
    bool online = false;
    const auto deadline = Clock::now() + 4s;
    while (!online && Clock::now() < deadline && !vehicle_done) {
      const auto vehicles = driver.vehicles();
      for (const auto& item : vehicles.at("vehicles"))
        if (item.value("vehicle_id", "") == config.vehicle_id && item.value("online", false)) online = true;
      if (!online) std::this_thread::sleep_for(20ms);
    }
    if (!online) throw std::runtime_error("bench vehicle registration timed out");
    const auto connected = driver.connect(config.vehicle_id, "native-media-probe");
    static_cast<void>(driver.send_media_capabilities({{"codecs", {"h264"}}}));
    Json scope{{"source_session_id", connected.at("session_id")},
               {"source_control_session_generation", connected.at("control_session_generation")}};
    const auto timeout = Clock::now() + std::chrono::seconds(opts.seconds + 15);
    std::string negotiation;
    std::deque<Json> early_candidates;
    auto next_stats = Clock::now();
    while (!vehicle_done && Clock::now() < timeout) {
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
        result["webrtc_stats"] = receiver.stats();
        next_stats = Clock::now() + 1s;
      }
    }
    if (!vehicle_done) throw std::runtime_error("bench duration exceeded");
  } catch (const std::exception& error) { result["error"] = error.what(); }
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
  const GstClockTime timestamps[] = {0, 33 * GST_MSECOND, 33 * GST_MSECOND, 66 * GST_MSECOND, GST_CLOCK_TIME_NONE};
  for (const auto pts : timestamps) {
    auto* buffer = gst_buffer_new_allocate(nullptr, 1, nullptr);
    GST_BUFFER_PTS(buffer) = pts;
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
  return ok && delivered == 3 && track.decoded == 3 && track.decoded_buffers == 5 &&
      track.duplicate_decoded_pts == 1 && track.invalid_decoded_pts == 1;
}
}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc == 2 && std::string(argv[1]) == "--self-test") {
      gst_init(nullptr, nullptr);
      const bool passed = pts_self_test();
      std::cout << "decoder PTS admission: " << (passed ? "PASS" : "FAIL") << '\n';
      return passed ? 0 : 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--help") {
      std::cout << "Media-only same-host WebRTC bench; no control or presentation acceptance.\n"
                   "--config FILE --output NEW_FILE [--seconds 5..300] [--jitter-ms 0..200]\n"
                   "[--decoder avdec_h264|nvv4l2decoder] [--consumer-stall-ms 0..500]\n"
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
    try { result = run(opts, config); }
    catch (const std::exception& error) {
      result = {{"schema", "native-receiver-bench-v1"}, {"passed", false}, {"startup_error", error.what()}};
    }
    output << result.dump(2) << '\n'; output.close();
    if (!output) throw std::runtime_error("could not write report");
    std::cout << Json{{"event", "native_receiver_probe_complete"}, {"passed", result.at("passed")}, {"output", opts.output}}.dump() << '\n';
    return result.at("passed").get<bool>() ? 0 : 1;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
