#pragma once
// Shared, opt-in media bench core. No control input or scan-out claims.
#define GST_USE_UNSTABLE_API
#include "mine_teleop/server.hpp"
#include "mine_teleop/detail/gstreamer_ice_servers.hpp"
#include "mine_teleop/detail/ice_diagnostics.hpp"
#include "mine_teleop/detail/gstreamer_decoder.hpp"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/video/videooverlay.h>
#include <gst/webrtc/webrtc.h>
#include <agent.h>  // libnice's pkg-config include directory is include/nice
#ifdef __APPLE__
#include <gst/gstmacos.h>
#include <gst/gl/gl.h>
#include <net/if.h>
#include <netinet/in.h>
#endif
#include <cstdlib>
#include <functional>

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

namespace mine_teleop::bench {
using namespace mine_teleop;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
inline double bench_now_ms() { return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count(); }

// Bench addresses may use different but equivalent IPv6 spellings. Compare
// binary addresses rather than strings, without logging either address.
inline bool same_ip_address(const std::string& left, const std::string& right) {
  std::array<unsigned char, 16> a{}, b{};
  for (const int family : {AF_INET, AF_INET6}) {
    if (inet_pton(family, left.c_str(), a.data()) == 1 &&
        inet_pton(family, right.c_str(), b.data()) == 1) return a == b;
  }
  return false;
}

inline bool allowed_bench_remote_candidate(std::string_view value, const std::vector<std::string>& remotes) {
  if (remotes.empty()) return true;
  if (value.starts_with("a=")) value.remove_prefix(2);
  std::istringstream input{std::string(value)};
  std::string foundation, component, protocol, priority, address, port, marker, type;
  input >> foundation >> component >> protocol >> priority >> address >> port >> marker >> type;
  if (!foundation.starts_with("candidate:") || (protocol != "udp" && protocol != "UDP") ||
      marker != "typ" || (type != "host" && type != "srflx" && type != "prflx")) return false;
  return std::any_of(remotes.begin(), remotes.end(), [&](const auto& expected) {
    return same_ip_address(address, expected);
  });
}

inline std::string filter_bench_remote_sdp(const std::string& sdp, const std::vector<std::string>& remotes) {
  if (remotes.empty()) return sdp;
  std::istringstream input(sdp);
  std::string output, line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.starts_with("a=candidate:") && !allowed_bench_remote_candidate(line, remotes)) continue;
    output += line + "\r\n";
  }
  return output;
}

struct Options {
  std::string config, output, signaling_url, driver_id{"native-bench"}, decoder{"avdec_h264"};
  int seconds{30}, jitter_ms{30}, stall_ms{0};
  std::string ice_transport{"auto"};  // Hybrid bench can pin the comparison transport.
  std::vector<std::string> local_addresses, local_public_addresses, remote_addresses;
  std::string local_interface;
  bool allow_relay{false};
  std::string address_family{"dual"}, ice_route_mode{"all"};
  Json ice_servers{Json::array()};
  bool validate_only{false}, display{false};
  std::vector<std::uintptr_t> window_handles;
};

#ifdef __APPLE__
// Opt-in WAN probe only. Bind each libnice ICE socket as soon as its host
// candidate appears, then check the selected socket again before acceptance.
class InterfaceBinding {
 public:
  explicit InterfaceBinding(const std::string& name) : index_(if_nametoindex(name.c_str())) {
    if (!index_) throw std::invalid_argument("unknown local interface for native WAN probe");
  }
  static void on_candidate(NiceAgent* agent, NiceCandidate* candidate, gpointer data) {
    auto& self = *static_cast<InterfaceBinding*>(data);
    auto* sockets = nice_agent_get_sockets(agent, candidate->stream_id, candidate->component_id);
    if (!sockets || sockets->len == 0) { self.failed_ = true; if (sockets) g_ptr_array_unref(sockets); return; }
    for (guint i = 0; i < sockets->len; ++i) {
      auto* socket = G_SOCKET(g_ptr_array_index(sockets, i));
      if (!self.apply(socket) || !self.matches(socket)) self.failed_ = true;
      else ++self.bound_;
    }
    g_ptr_array_unref(sockets);
  }
  bool selected(NiceAgent* agent, guint stream, guint component) {
    auto* socket = nice_agent_get_selected_socket(agent, stream, component);
    const bool ok = socket && !failed_.load() && bound_.load() > 0 && matches(socket);
    if (socket) g_object_unref(socket);
    if (!ok) failed_ = true;
    return ok;
  }
  bool healthy() const { return !failed_.load() && bound_.load() > 0; }
 private:
  bool option(GSocket* socket, int& level, int& name) const {
    if (!socket || g_socket_get_socket_type(socket) != G_SOCKET_TYPE_DATAGRAM) return false;
    if (g_socket_get_family(socket) == G_SOCKET_FAMILY_IPV4) { level=IPPROTO_IP; name=IP_BOUND_IF; return true; }
    if (g_socket_get_family(socket) == G_SOCKET_FAMILY_IPV6) { level=IPPROTO_IPV6; name=IPV6_BOUND_IF; return true; }
    return false;
  }
  bool apply(GSocket* socket) const {
    int level=0,name=0; if (!option(socket,level,name)) return false;
    GError* error=nullptr; const bool ok=g_socket_set_option(socket,level,name,static_cast<int>(index_),&error);
    g_clear_error(&error); return ok;
  }
  bool matches(GSocket* socket) const {
    int level=0,name=0,value=0; if (!option(socket,level,name)) return false;
    GError* error=nullptr; const bool ok=g_socket_get_option(socket,level,name,&value,&error);
    g_clear_error(&error); return ok && value==static_cast<int>(index_);
  }
  unsigned index_;
  std::atomic<bool> failed_{false};
  std::atomic<unsigned> bound_{0};
};
#endif
inline int integer(const std::string& text, int minimum, int maximum) {
  int result{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || result < minimum || result > maximum)
    throw std::invalid_argument("numeric argument outside permitted range");
  return result;
}
inline Options options(int argc, char** argv) {
  Options result;
  for (int i = 1; i < argc; ++i) {
    const std::string flag = argv[i];
    if (flag == "--validate-only") { result.validate_only = true; continue; }
    if (flag == "--display") { result.display = true; continue; }
    if (i + 1 >= argc) throw std::invalid_argument("missing argument: " + flag);
    const std::string value = argv[++i];
    if (flag == "--config") result.config = value;
    else if (flag == "--output") result.output = value;
    else if (flag == "--signaling-url") result.signaling_url = value;
    else if (flag == "--driver-id") result.driver_id = value;
    else if (flag == "--seconds") result.seconds = integer(value, 5, 1800);
    else if (flag == "--jitter-ms") result.jitter_ms = integer(value, 0, 200);
    else if (flag == "--consumer-stall-ms") result.stall_ms = integer(value, 0, 500);
    else if (flag == "--decoder" && (value == "avdec_h264" || value == "nvv4l2decoder" || value == "vtdec_hw")) result.decoder = value;
    else if (flag == "--wan-local-address") result.local_addresses.push_back(value);
    else if (flag == "--wan-remote-address") result.remote_addresses.push_back(value);
    else if (flag == "--wan-family" && (value == "ipv4" || value == "ipv6")) result.address_family = value;
    else throw std::invalid_argument("unknown option: " + flag);
  }
  if (result.config.empty()) throw std::invalid_argument("--config is required");
  if (!result.validate_only && result.output.empty()) throw std::invalid_argument("--output is required");
  // Remote mode uses a loopback signaling tunnel; media still uses direct ICE.
  if (!result.signaling_url.empty()) {
    const std::string prefix = "http://127.0.0.1:";
    if (!result.signaling_url.starts_with(prefix))
      throw std::invalid_argument("--signaling-url must use a loopback HTTP tunnel");
    // Accept only an origin with a numeric port: no userinfo, alternate host,
    // path or fragment that could redirect bench credentials elsewhere.
    static_cast<void>(integer(result.signaling_url.substr(prefix.size()), 1, 65535));
  }
#ifndef MINE_TELEOP_PROBE_EMBEDDED_SENDER
  if (!result.validate_only && result.signaling_url.empty())
    throw std::invalid_argument("this build requires --signaling-url (no embedded sender)");
#endif
  if (result.display && result.stall_ms) throw std::invalid_argument("display mode does not inject consumer stalls");
  if (!result.local_addresses.empty() || !result.remote_addresses.empty()) {
    if (result.local_addresses.size() != 1 || result.remote_addresses.size() != 1 || result.address_family == "dual")
      throw std::invalid_argument("WAN receiver requires one local and one remote address and an explicit family");
    for (const auto& ip : {result.local_addresses.front(), result.remote_addresses.front()}) {
      const auto info = detail::ice_address_summary(ip);
      if (info.value("address_class", "") != "public" || info.value("address_family", "") != result.address_family)
        throw std::invalid_argument("WAN receiver addresses must be public and match the requested family");
    }
    if (same_ip_address(result.local_addresses.front(), result.remote_addresses.front()))
      throw std::invalid_argument("WAN receiver endpoints must differ");
    result.ice_transport = "udp";
    result.ice_route_mode = "direct";
  } else if (result.address_family != "dual") {
    throw std::invalid_argument("WAN family requires explicit local and remote addresses");
  }
  return result;
}
inline void validate(const VehicleConfig& config) {
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
inline Json structure_json(const GstStructure* structure) {
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

// Receiver-local time at the jitterbuffer input (after SRTP decryption).
// This is not a capture timestamp and not the kernel's packet arrival time.
struct ArrivalTimes {
  std::mutex mutex;
  std::map<std::uint64_t, double> latest;
  static GstPadProbeReturn record(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto& self = *static_cast<ArrivalTimes*>(data);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    GstRTPBuffer packet = GST_RTP_BUFFER_INIT;
    if (buffer && gst_rtp_buffer_map(buffer, GST_MAP_READ, &packet)) {
      const auto key = (static_cast<std::uint64_t>(gst_rtp_buffer_get_ssrc(&packet)) << 32) |
          gst_rtp_buffer_get_timestamp(&packet);
      gst_rtp_buffer_unmap(&packet);
      std::lock_guard lock(self.mutex);
      self.latest[key] = bench_now_ms();
      // A handful of frames in flight, bounded even with malformed traffic.
      if (self.latest.size() > 512) {
        auto oldest = std::min_element(self.latest.begin(), self.latest.end(),
            [](const auto& a, const auto& b) { return a.second < b.second; });
        self.latest.erase(oldest);
      }
    }
    return GST_PAD_PROBE_OK;
  }
  double get(std::uint64_t key) {
    std::lock_guard lock(mutex);
    const auto it = latest.find(key);
    return it == latest.end() ? 0 : it->second;
  }
};
struct Track {
  std::string pad;
  ArrivalTimes* arrivals{};
  GstElement* sink{};  // owned reference until the consumer stops
  std::jthread consumer;
  std::mutex mutex;
  std::map<GstClockTime, double> compressed_at, decoded_at;
  std::unordered_set<std::uint64_t> rtp_timestamps, decoded_pts;
  std::unordered_set<std::string> decoded_memory_types;
  std::uint64_t rtp_packets{}, decoded_buffers{}, duplicate_decoded_pts{}, invalid_decoded_pts{}, corrupt_decoded_buffers{}, identity_overflow{};
  Samples decode_ms, decoded_gap_ms, consumed_gap_ms, consume_wait_ms;
  Json decoded_events = Json::array(), drawn_events = Json::array(), received_events = Json::array();
  GstClockTime last_draw_pts{GST_CLOCK_TIME_NONE};
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
      const bool marker = gst_rtp_buffer_get_marker(&packet);
      gst_rtp_buffer_unmap(&packet);
      const double received = marker && self.arrivals ? self.arrivals->get(identity) : 0;
      std::lock_guard lock(self.mutex);
      if (received > 0 && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)) && self.received_events.size() < 131072)
        self.received_events.push_back({GST_BUFFER_PTS(buffer), received});
      ++self.rtp_packets;
      if (self.rtp_timestamps.size() < 131072) self.rtp_timestamps.insert(identity); else ++self.identity_overflow;
    }
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn compressed(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto& self = *static_cast<Track*>(data);
    const auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) { std::lock_guard lock(self.mutex); remember(self.compressed_at, GST_BUFFER_PTS(buffer), bench_now_ms()); }
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn raw(GstPad*, GstPadProbeInfo* info, gpointer data) {
    auto& self = *static_cast<Track*>(data);
    auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buffer) {
      const double now = bench_now_ms();
      std::lock_guard lock(self.mutex);
      ++self.decoded_buffers;
      if (GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED)) {
        ++self.corrupt_decoded_buffers; return GST_PAD_PROBE_DROP;
      }
      const auto pts = GST_BUFFER_PTS(buffer);
      for(guint index=0;index<gst_buffer_n_memory(buffer) && index<64;++index) {
        const auto* memory=gst_buffer_peek_memory(buffer,index);
        if(self.decoded_memory_types.size()<8)
          self.decoded_memory_types.insert(memory->allocator && memory->allocator->mem_type?memory->allocator->mem_type:"unknown");
      }
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
      if (self.decoded_events.size() < 131072) self.decoded_events.push_back({pts, now});
      self.last_decoded = now;
      ++self.decoded;
    }
    return GST_PAD_PROBE_OK;
  }
  // client-draw runs when GL is about to draw a sample, not at scan-out.
  // Returning FALSE preserves glimagesink's default renderer.
  static gboolean draw(GstElement*, gpointer, GstSample* sample, gpointer data) {
    auto& self = *static_cast<Track*>(data);
    auto* buffer = sample ? gst_sample_get_buffer(sample) : nullptr;
    if (buffer) {
      std::lock_guard lock(self.mutex);
      const auto pts = GST_BUFFER_PTS(buffer);
      if (GST_CLOCK_TIME_IS_VALID(pts) && pts != self.last_draw_pts) {
        self.last_draw_pts = pts;
        if (self.drawn_events.size() < 131072) self.drawn_events.push_back({pts, bench_now_ms()});
      }
    }
    return FALSE;
  }
  Json counters() {
    std::lock_guard lock(mutex);
    return {{"pad", pad}, {"decoded", decoded}, {"consumed", consumed}, {"drawn", drawn_events.size()},
            {"decode_count", decode_ms.count}, {"decode_sum_ms", decode_ms.sum}};
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
        const double now = bench_now_ms();
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
            {"corrupt_decoded_buffers", corrupt_decoded_buffers},
            {"decoded_memory_types", decoded_memory_types},
            {"not_consumed_frames", decoded - consumed}, {"injected_consumer_stalls", stalls},
            {"decoded_fps", decoded > 1 && last_decoded > first ? (static_cast<double>(decoded - 1) * 1000 / (last_decoded - first)) : 0},
            {"depay_to_decode", decode_ms.json()}, {"decoded_frame_gap", decoded_gap_ms.json()},
            {"consumed_frame_gap", consumed_gap_ms.json()}, {"decode_to_consume", consume_wait_ms.json()},
            {"received_events_pts_ns_clock_ms", received_events},
            {"decoded_events_pts_ns_clock_ms", decoded_events}, {"draw_events_pts_ns_clock_ms", drawn_events},
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
    detail::configure_webrtc_ice_servers(peer_, options.ice_servers, options.ice_route_mode != "direct");
    if (options.ice_route_mode == "relay") g_object_set(peer_, "ice-transport-policy", GST_WEBRTC_ICE_TRANSPORT_POLICY_RELAY, nullptr);
    g_signal_connect(pipeline_, "deep-element-added", G_CALLBACK(on_element), this);
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
#ifdef __APPLE__
    if (!options_.local_interface.empty()) {
      interface_binding_ = std::make_unique<InterfaceBinding>(options_.local_interface);
      g_signal_connect(nice_, "new-candidate-full", G_CALLBACK(InterfaceBinding::on_candidate), interface_binding_.get());
    }
#else
    if (!options_.local_interface.empty()) throw std::runtime_error("native interface pinning is macOS-only");
#endif
    for (const auto& local_address : options_.local_addresses) {
      NiceAddress address;
      nice_address_init(&address);
      if (!nice_address_set_from_string(&address, local_address.c_str()) ||
          !nice_agent_add_local_address(nice_, &address)) throw std::runtime_error("cannot bind bench ICE address");
    }
    if (options_.ice_transport == "tcp") g_object_set(nice_, "ice-udp", FALSE, nullptr);
    else if (options_.ice_transport == "udp") g_object_set(nice_, "ice-tcp", FALSE, nullptr);
    g_signal_connect(nice_, "new-selected-pair-full", G_CALLBACK(on_pair), this);
    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      throw std::runtime_error("receiver failed to start");
  }
  void stop() {
    if (stopped_) return;
    stopped_ = true;
    g_signal_handlers_disconnect_by_data(peer_, this);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (nice_) {
      g_signal_handlers_disconnect_by_data(nice_, this);
#ifdef __APPLE__
      if (interface_binding_) g_signal_handlers_disconnect_by_data(nice_, interface_binding_.get());
#endif
    }
    for (auto& track : tracks_) track->stop();
  }
  std::string answer(const Json& offer) {
    if (!negotiation_.empty()) throw std::runtime_error("probe stops on renegotiation; new measurement required");
    negotiation_ = offer.at("media_negotiation_id").get<std::string>();
    const auto text = filter_bench_remote_sdp(offer.at("sdp").get<std::string>(), options_.remote_addresses);
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
    if (!allowed_bench_remote_candidate(candidate.at("candidate").get<std::string>(), options_.remote_addresses)) return;
    g_signal_emit_by_name(peer_, "add-ice-candidate", candidate.value("sdpMLineIndex", 0U), candidate.at("candidate").get<std::string>().c_str());
  }
  Json selected_pair() { std::lock_guard lock(mutex_); return selected_pair_; }
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
  Json counters() {
    Json result = Json::array();
    std::lock_guard lock(mutex_);
    for (auto& track : tracks_) result.push_back(track->counters());
    return result;
  }
  Json report() {
    Json tracks = Json::array();
    std::lock_guard lock(mutex_);
    for (auto& track : tracks_) tracks.push_back(track->json());
    return {{"tracks", tracks}, {"selected_pair", selected_pair_}, {"selected_pair_changes", pair_changes_},
      {"surface_budget", {{"consumer_queue_slots",options_.display?1:0},{"appsink_slots",1},{"consumer_held_slots",1},
          {"display_queue_slots",options_.display?1:0},{"decoder_reference_surfaces",nullptr},
          {"gpu_inflight_surfaces",nullptr},{"pool_capacity",nullptr},{"verified",false},
          {"note","Known queue references may share a surface; driver pool and GL retention are unknown. No zero-copy claim."}}},
            {"local_candidates", local_candidates_}, {"jitter_target_ms", options_.jitter_ms},
            {"appsink_max_buffers", 1}, {"appsink_drop", true}, {"sink_sync", false}, {"decoder", options_.decoder},
            {"display_presented", false}, {"display_requested", options_.display},
            {"display_measurement", "GL draw callback is not scan-out"}, {"control_tested", false}};
  }
 private:
  static void on_element(GstBin*, GstBin*, GstElement* element, gpointer data) {
    auto& self = *static_cast<Receiver*>(data);
    auto* factory = gst_element_get_factory(element);
    if (!factory || std::string(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))) != "rtpjitterbuffer") return;
    auto* sink = gst_element_get_static_pad(element, "sink");
    gst_pad_add_probe(sink, GST_PAD_PROBE_TYPE_BUFFER, ArrivalTimes::record, &self.arrivals_, nullptr);
    gst_object_unref(sink);
  }
  static const char* candidate_type(NiceCandidateType type) {
    switch (type) {
      case NICE_CANDIDATE_TYPE_HOST: return "host";
      case NICE_CANDIDATE_TYPE_SERVER_REFLEXIVE: return "srflx";
      case NICE_CANDIDATE_TYPE_PEER_REFLEXIVE: return "prflx";
      case NICE_CANDIDATE_TYPE_RELAYED: return "relay";
    }
    return "unknown";
  }
  static void on_pair(NiceAgent* agent, guint stream, guint component, NiceCandidate* local, NiceCandidate* remote, gpointer data) {
    auto& self = *static_cast<Receiver*>(data);
    if (!local || !remote) return;
#ifdef __APPLE__
    const bool interface_ok = !self.interface_binding_ || self.interface_binding_->selected(agent,stream,component);
#else
    (void)agent;
    const bool interface_ok = true;
#endif
    std::lock_guard lock(self.mutex_);
    if (!interface_ok) self.error_ = "selected ICE socket is not bound to the requested physical interface";
    ++self.pair_changes_;
    char local_ip[NICE_ADDRESS_STRING_LEN]{}, remote_ip[NICE_ADDRESS_STRING_LEN]{};
    nice_address_to_string(&local->addr, local_ip); nice_address_to_string(&remote->addr, remote_ip);
    self.selected_pair_ = {{"local_address", local_ip}, {"remote_address", remote_ip}, {"source", "libnice-new-selected-pair-full"}, {"stream", stream}, {"component", component},
        {"local_port", nice_address_get_port(&local->addr)}, {"remote_port", nice_address_get_port(&remote->addr)},
        {"local_type", candidate_type(local->type)}, {"remote_type", candidate_type(remote->type)},
        {"local_transport", local->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "udp" : "tcp"},
        {"remote_transport", remote->transport == NICE_CANDIDATE_TRANSPORT_UDP ? "udp" : "tcp"}};
    // NiceCandidate.transport describes the allocation, not the client->TURN
    // leg. The supported libnice API cannot attest both peers' TURN legs/URLs.
    // Preserve that absence explicitly; no configured URL may fill this gap.
    self.selected_pair_["local_relay_protocol"] = "unknown";
    self.selected_pair_["remote_relay_protocol"] = "unknown";
    self.selected_pair_["interface_bound"] = interface_ok && !self.options_.local_interface.empty();
    // Latch even a brief forbidden path change between periodic samples.
    const auto& opts = self.options_;
    if (!opts.local_addresses.empty()) {
      const auto contains = [](const auto& values, const std::string& ip) {
        return std::any_of(values.begin(), values.end(), [&](const auto& expected) { return same_ip_address(expected, ip); });
      };
      const bool relay = local->type == NICE_CANDIDATE_TYPE_RELAYED || remote->type == NICE_CANDIDATE_TYPE_RELAYED;
      const bool local_known = contains(opts.local_addresses, local_ip) || contains(opts.local_public_addresses, local_ip);
      const bool family_ok = (std::string(local_ip).find(':') != std::string::npos) == (std::string(remote_ip).find(':') != std::string::npos) &&
          (opts.address_family == "dual" || ((std::string(local_ip).find(':') != std::string::npos) ? opts.address_family == "ipv6" : opts.address_family == "ipv4"));
      if (!family_ok || local->transport != NICE_CANDIDATE_TRANSPORT_UDP || remote->transport != NICE_CANDIDATE_TRANSPORT_UDP ||
          detail::ice_address_summary(remote_ip)["address_class"] != "public" ||
          relay || !local_known || !contains(opts.remote_addresses, remote_ip))
        self.error_ = "native pair differs from required public IP endpoints or relay policy";
      if (relay) self.error_ = "native WAN relay acceptance unavailable: TURN leg protocol/server evidence missing";
    }
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
    const auto decoder = detail::low_latency_decoder_stage(options_.decoder);
    auto description = std::string("rtph264depay name=depay wait-for-keyframe=true request-keyframe=true ! h264parse ! ") +
        decoder + " ! tee name=raw raw. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! appsink name=out sync=false max-buffers=1 drop=true wait-on-eos=false";
    if (!options_.display) description = std::string("rtph264depay name=depay wait-for-keyframe=true request-keyframe=true ! h264parse ! ") +
        decoder + " ! appsink name=out sync=false max-buffers=1 drop=true wait-on-eos=false";
    if (options_.display) description += " raw. ! queue max-size-buffers=1 max-size-bytes=0 max-size-time=0 leaky=downstream ! glimagesink name=display sync=false";
    auto* bin = gst_parse_bin_from_description(description.c_str(), TRUE, &error);
    if (error || !bin) {
      const std::string message = error ? error->message : "decode bin unavailable";
      g_clear_error(&error); if (bin) gst_object_unref(bin); throw std::runtime_error(message);
    }
    gst_bin_add(GST_BIN(pipeline_), bin);
#ifdef __APPLE__
    if (!options_.window_handles.empty()) {
      // The VideoToolbox GL allocator can discover another track's context via
      // the common pipeline. Separate display contexts keep each drawable in
      // its own NSView; otherwise both sinks draw into the last attached view.
      auto* display = gst_gl_display_new();
      auto* context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
      gst_context_set_gl_display(context, display);
      gst_element_set_context(bin, context);
      gst_context_unref(context);
      gst_object_unref(display);
    }
#endif
    auto track = std::make_unique<Track>();
    track->pad = GST_PAD_NAME(pad);
    track->arrivals = &arrivals_;
    track->sink = gst_bin_get_by_name(GST_BIN(bin), "out");
    if (options_.display) {
      auto* display = gst_bin_get_by_name(GST_BIN(bin), "display");
      if (!options_.window_handles.empty()) {
        const std::string name = GST_PAD_NAME(pad);
        const int index = name.starts_with("src_") ? integer(name.substr(4), 0, 7) : -1;
        if (index < 0 || static_cast<std::size_t>(index) >= options_.window_handles.size()) {
          gst_object_unref(display);
          throw std::runtime_error("native view has no matching media pad");
        }
        gst_video_overlay_set_window_handle(GST_VIDEO_OVERLAY(display), options_.window_handles[index]);
        gst_video_overlay_handle_events(GST_VIDEO_OVERLAY(display), FALSE);
      }
      g_signal_connect(display, "client-draw", G_CALLBACK(Track::draw), track.get());
      gst_object_unref(display);
    }
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
  ArrivalTimes arrivals_;
  Options options_;
  GstElement* pipeline_{}, *peer_{};
  NiceAgent* nice_{};
#ifdef __APPLE__
  std::unique_ptr<InterfaceBinding> interface_binding_;
#endif
  bool stopped_{};
  std::string negotiation_, error_;
  std::mutex mutex_;
  std::deque<Json> candidates_;
  std::vector<std::unique_ptr<Track>> tracks_;
  unsigned local_candidates_{};
  unsigned pair_changes_{};
  Json selected_pair_;
};

}  // namespace mine_teleop::bench
