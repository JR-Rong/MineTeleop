#include "mine_teleop/media.hpp"

#include "mine_teleop/server.hpp"
#include "mine_teleop/upload.hpp"
#include "mine_teleop/video.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace mine_teleop {
namespace {

std::string trim_origin(std::string value) {
  if (value.starts_with("ws://")) value.replace(0, 5, "http://");
  if (value.starts_with("wss://")) value.replace(0, 6, "https://");
  if (value.ends_with("/signaling")) value.resize(value.size() - std::string_view("/signaling").size());
  while (!value.empty() && value.back() == '/') value.pop_back();
  if (!value.starts_with("http://") && !value.starts_with("https://")) {
    throw std::invalid_argument("media signaling URL must use ws, wss, http, or https");
  }
  return value;
}

std::string pipeline_identifier(std::string_view value) {
  std::string result;
  result.reserve(value.size());
  for (const unsigned char character : value) {
    result.push_back(std::isalnum(character) ? static_cast<char>(character) : '_');
  }
  if (result.empty() || std::isdigit(static_cast<unsigned char>(result.front()))) result.insert(result.begin(), '_');
  return result;
}

std::string quote_pipeline(std::string_view value) {
  std::string result{"\""};
  for (const auto character : value) {
    if (character == '\\' || character == '"') result.push_back('\\');
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

std::string iso_time(std::int64_t timestamp_ms) {
  const std::time_t seconds = static_cast<std::time_t>(timestamp_ms / 1000);
  std::tm value{};
  gmtime_r(&seconds, &value);
  std::ostringstream output;
  output << std::put_time(&value, "%Y-%m-%dT%H:%M:%S") << '.' << std::setw(3) << std::setfill('0')
         << (timestamp_ms % 1000) << 'Z';
  return output.str();
}

class MediaSignalingClient {
 public:
  MediaSignalingClient(std::string origin, std::string vehicle_id, std::string device_token)
      : origin_(trim_origin(std::move(origin))),
        vehicle_id_(std::move(vehicle_id)),
        device_token_(std::move(device_token)) {
    if (vehicle_id_.empty() || device_token_.empty()) throw std::invalid_argument("vehicle id and device token are required");
  }

  void register_online() {
    static_cast<void>(http_.post_json_response(
        origin_ + "/vehicles/online", {{"vehicle_id", vehicle_id_}, {"device_token", device_token_}}));
  }

  bool discover_session() {
    const auto response = http_.get_json(
        origin_ + "/vehicles/" + http_.url_encode(vehicle_id_) + "/session?device_token=" +
        http_.url_encode(device_token_));
    session_id_ = response.value("session_id", "");
    driver_id_ = response.value("driver_id", "");
    return !session_id_.empty() && !driver_id_.empty();
  }

  Json poll(std::string_view types) {
    require_session();
    return http_.get_json(
        origin_ + "/signaling/" + http_.url_encode(session_id_) + "/messages?recipient=" +
        http_.url_encode(vehicle_id_) + "&device_token=" + http_.url_encode(device_token_) +
        "&types=" + http_.url_encode(types));
  }

  void send(std::string_view type, const Json& payload) {
    require_session();
    static_cast<void>(http_.post_json_response(
        origin_ + "/signaling/" + http_.url_encode(session_id_) + "/messages",
        {{"sender", vehicle_id_},
         {"recipient", driver_id_},
         {"device_token", device_token_},
         {"type", type},
         {"payload", payload}}));
  }

  [[nodiscard]] const std::string& session_id() const { return session_id_; }

 private:
  void require_session() const {
    if (session_id_.empty() || driver_id_.empty()) throw std::runtime_error("media signaling session is unavailable");
  }

  std::string origin_;
  std::string vehicle_id_;
  std::string device_token_;
  HttpClient http_{std::chrono::seconds(5)};
  std::string session_id_;
  std::string driver_id_;
};

}  // namespace

struct VehicleMediaRuntime::Impl {
  struct Lane {
    CameraConfig camera;
    MediaProfile profile;
    std::unique_ptr<CameraFrameSource> source;
    GstElement* appsrc{nullptr};
    GstElement* encoder{nullptr};
    std::thread thread;
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> encoded{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::int64_t> last_capture_ms{0};
    std::atomic<std::uint64_t> encode_latency_samples{0};
    std::atomic<std::uint64_t> encode_latency_total_ms{0};
    std::atomic<std::uint64_t> encode_latency_max_ms{0};
    std::int64_t pipeline_started_ms{0};
    std::mutex error_mutex;
    std::string error;
  };

  Impl(
      VehicleConfig next_config,
      std::string signaling_url,
      std::string device_token,
      int next_frame_timeout_ms,
      std::filesystem::path next_recording_root,
      std::optional<std::string> next_forced_codec,
      int next_simulate_primary_failure_after_frames)
      : config(std::move(next_config)),
        signaling(std::move(signaling_url), config.vehicle_id, std::move(device_token)),
        frame_timeout_ms(next_frame_timeout_ms),
        recording_root(std::move(next_recording_root)),
        forced_codec(std::move(next_forced_codec)),
        simulate_primary_failure_after_frames(next_simulate_primary_failure_after_frames) {
    if (frame_timeout_ms <= 0) throw std::invalid_argument("frame timeout must be positive");
    if (simulate_primary_failure_after_frames < 0) {
      throw std::invalid_argument("simulated primary failure frame count must be non-negative");
    }
  }

  ~Impl() {
    try {
      stop_pipeline();
    } catch (...) {
    }
  }

  static GstPadProbeReturn count_encoded(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
    auto* lane = static_cast<Lane*>(user_data);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
      ++lane->encoded;
      GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      if (buffer != nullptr && GST_BUFFER_PTS_IS_VALID(buffer)) {
        const auto captured_at_ms = lane->pipeline_started_ms + static_cast<std::int64_t>(GST_BUFFER_PTS(buffer) / GST_MSECOND);
        const auto latency_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ms() - captured_at_ms));
        ++lane->encode_latency_samples;
        lane->encode_latency_total_ms += latency_ms;
        auto observed = lane->encode_latency_max_ms.load();
        while (latency_ms > observed && !lane->encode_latency_max_ms.compare_exchange_weak(observed, latency_ms)) {
        }
      }
    }
    return GST_PAD_PROBE_OK;
  }

  static void on_ice_candidate(GstElement*, guint mline_index, gchar* candidate, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    self->queue_signal(
        "ice_candidate",
        {{"candidate", candidate == nullptr ? "" : candidate}, {"sdpMLineIndex", mline_index}});
  }

  static void on_offer_created(GstPromise* promise, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
      gst_promise_unref(promise);
      self->set_pipeline_error("WebRTC offer promise failed");
      return;
    }
    const auto* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);
    if (offer == nullptr) {
      self->set_pipeline_error("WebRTC offer is missing");
      return;
    }

    GstPromise* local = gst_promise_new();
    g_signal_emit_by_name(self->webrtc, "set-local-description", offer, local);
    gst_promise_interrupt(local);
    gst_promise_unref(local);

    gchar* text = gst_sdp_message_as_text(offer->sdp);
    Json tracks = Json::array();
    for (const auto& lane : self->lanes) {
      tracks.push_back({
          {"camera_id", lane->camera.id},
          {"codec", to_string(self->active_candidate.codec)},
          {"backend", to_string(self->active_candidate.backend)},
          {"width", lane->profile.width},
          {"height", lane->profile.height},
          {"fps", lane->profile.fps},
          {"bitrate_kbps", lane->profile.bitrate_kbps},
      });
    }
    self->queue_signal(
        "webrtc_offer",
        {{"type", "offer"},
         {"sdp", text == nullptr ? "" : text},
         {"codec", to_string(self->active_candidate.codec)},
         {"backend", to_string(self->active_candidate.backend)},
         {"media_tracks", std::move(tracks)}});
    g_free(text);
    gst_webrtc_session_description_free(offer);
  }

  static void on_negotiation_needed(GstElement* webrtc, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    GstPromise* promise = gst_promise_new_with_change_func(on_offer_created, self, nullptr);
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
  }

  void queue_signal(std::string type, Json payload) {
    std::lock_guard lock(signal_mutex);
    pending_signals.emplace_back(std::move(type), std::move(payload));
  }

  void set_pipeline_error(std::string value) {
    std::lock_guard lock(error_mutex);
    if (pipeline_error.empty()) pipeline_error = std::move(value);
  }

  [[nodiscard]] std::string current_pipeline_error() const {
    std::lock_guard lock(error_mutex);
    return pipeline_error;
  }

  [[nodiscard]] std::vector<VideoCodec> negotiate_codecs(int timeout_ms) {
    if (forced_codec.has_value()) return {parse_video_codec(*forced_codec)};
    const auto preferred = parse_video_codec(config.hardware.preferred_codec);
    const auto fallback = parse_video_codec(config.hardware.fallback_codec);
    const auto deadline = now_ms() + std::max(0, timeout_ms);
    do {
      try {
        const auto response = signaling.poll("media_capabilities");
        for (const auto& message : response.value("messages", Json::array())) {
          if (message.value("type", "") != "media_capabilities") continue;
          const auto payload = message.value("payload", Json::object());
          std::vector<std::string> codecs = payload.value("codecs", std::vector<std::string>{});
          const auto supports = [&](VideoCodec codec) {
            const auto expected = to_string(codec);
            return std::any_of(codecs.begin(), codecs.end(), [&](const auto& value) {
              std::string normalized(value);
              std::transform(normalized.begin(), normalized.end(), normalized.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
              });
              return normalized == expected || (codec == VideoCodec::H265 && normalized == "hevc") ||
                     (codec == VideoCodec::H264 && normalized == "avc");
            });
          };
          std::vector<VideoCodec> result;
          if (supports(preferred)) result.push_back(preferred);
          if (fallback != preferred && supports(fallback)) result.push_back(fallback);
          if (!result.empty()) return result;
          throw std::runtime_error("driver does not advertise H.264 or H.265 WebRTC decoding");
        }
      } catch (const std::exception& error) {
        if (now_ms() >= deadline) throw;
        last_negotiation_warning = error.what();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (now_ms() < deadline);
    return {fallback};
  }

  [[nodiscard]] std::string build_pipeline(const VideoEncoder& encoder) {
    std::ostringstream pipeline_text;
    pipeline_text << "webrtcbin name=webrtc bundle-policy=max-bundle latency=0 ";
    int payload_type = 96;
    for (const auto& lane : lanes) {
      const auto id = pipeline_identifier(lane->camera.id);
      const auto parser = encoder.codec() == VideoCodec::H265 ? "h265parse" : "h264parse";
      const auto payloader = encoder.codec() == VideoCodec::H265 ? "rtph265pay" : "rtph264pay";
      const auto encoding_name = encoder.codec() == VideoCodec::H265 ? "H265" : "H264";
      const auto elementary_caps = encoder.codec() == VideoCodec::H265 ? "video/x-h265" : "video/x-h264";
      const auto recording_stream_format = encoder.codec() == VideoCodec::H265 ? "hvc1" : "avc";
      VideoEncoderSettings settings{lane->profile.bitrate_kbps, std::max(1, lane->profile.fps)};
      pipeline_text
          << "appsrc name=source_" << id
          << " is-live=true format=time do-timestamp=false block=false max-bytes=524288 "
          << "caps=image/jpeg,width=" << lane->profile.width << ",height=" << lane->profile.height
          << ",framerate=" << lane->profile.fps << "/1 "
          << "! queue max-size-buffers=2 leaky=downstream "
          << "! jpegdec ! videoconvert ! videoscale "
          << "! video/x-raw,format=NV12,width=" << lane->profile.width << ",height=" << lane->profile.height
          << ",framerate=" << lane->profile.fps << "/1 "
          << "! queue max-size-buffers=2 leaky=downstream "
          << "! " << encoder.pipeline_stage(settings, "encoder_" + id) << ' '
          << "! " << parser << " config-interval=-1 "
          << "! " << elementary_caps << ",stream-format=byte-stream,alignment=au "
          << "! tee name=encoded_" << id << ' '
          << "encoded_" << id << ". ! queue max-size-buffers=2 leaky=downstream "
          << "! " << payloader << " name=pay_" << id << " config-interval=-1 pt=" << payload_type << ' '
          << "! application/x-rtp,media=video,encoding-name=" << encoding_name << ",payload=" << payload_type
          << (encoder.codec() == VideoCodec::H265
                  ? ",profile-id=(string)1,tier-flag=(string)0,tx-mode=(string)SRST"
                  : "")
          << " ! webrtc. ";
      if (!recording_root.empty() && !lane->camera.record_profile.empty()) {
        const auto& record_profile = config.record_profile(lane->camera.record_profile);
        const auto directory = recording_root / config.vehicle_id / signaling.session_id() / lane->camera.id;
        std::filesystem::create_directories(directory);
        const auto pattern = directory / (std::to_string(now_ms()) + "_" + lane->camera.id + "_%05d.mp4");
        pipeline_text
            << "encoded_" << id << ". ! queue "
            << "! " << parser << " config-interval=-1 "
            << "! " << elementary_caps << ",stream-format=" << recording_stream_format << ",alignment=au "
            << "! splitmuxsink name=recorder_" << id
            << " muxer-factory=mp4mux async-finalize=true max-size-time="
            << static_cast<std::int64_t>(record_profile.segment_seconds) * GST_SECOND
            << " location=" << quote_pipeline(pattern.string()) << ' ';
      }
      ++payload_type;
    }
    return pipeline_text.str();
  }

  void prepare_lanes() {
    lanes.clear();
    for (const auto& camera : config.enabled_cameras()) {
      auto lane = std::make_unique<Lane>();
      lane->camera = camera;
      lane->profile = config.realtime_profile(camera.realtime_profile);
      MediaProfile capture = lane->profile;
      capture.codec = "mjpeg";
      capture.encoder = "native";
      lane->source = std::make_unique<CameraFrameSource>(camera, capture, frame_timeout_ms);
      lanes.push_back(std::move(lane));
    }
  }

  bool start_pipeline(const EncoderCandidate& candidate, int capture_interval_ms) {
    active_candidate = candidate;
    pipeline_error.clear();
    stop_requested = false;
    prepare_lanes();
    auto encoder_choice = create_video_encoder(candidate);
    if (encoder_choice->factory_name().empty()) {
      set_pipeline_error(to_string(candidate.backend) + " " + to_string(candidate.codec) + " encoder factory is unavailable");
      return false;
    }
    GError* parse_error = nullptr;
    const auto description = build_pipeline(*encoder_choice);
    pipeline = gst_parse_launch(description.c_str(), &parse_error);
    if (parse_error != nullptr || pipeline == nullptr) {
      const std::string message = parse_error != nullptr ? parse_error->message : "unknown pipeline parse error";
      if (parse_error != nullptr) g_error_free(parse_error);
      if (pipeline != nullptr) {
        gst_object_unref(pipeline);
        pipeline = nullptr;
      }
      set_pipeline_error("cannot build GStreamer WebRTC pipeline: " + message);
      return false;
    }
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    if (webrtc == nullptr) {
      set_pipeline_error("WebRTC pipeline does not contain webrtcbin");
      stop_pipeline();
      return false;
    }
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), this);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), this);

    for (const auto& lane : lanes) {
      const auto id = pipeline_identifier(lane->camera.id);
      lane->appsrc = gst_bin_get_by_name(GST_BIN(pipeline), ("source_" + id).c_str());
      lane->encoder = gst_bin_get_by_name(GST_BIN(pipeline), ("encoder_" + id).c_str());
      if (lane->appsrc == nullptr || lane->encoder == nullptr) {
        set_pipeline_error("media pipeline lane is incomplete: " + lane->camera.id);
        stop_pipeline();
        return false;
      }
      GstPad* encoder_src = gst_element_get_static_pad(lane->encoder, "src");
      if (encoder_src != nullptr) {
        gst_pad_add_probe(encoder_src, GST_PAD_PROBE_TYPE_BUFFER, count_encoded, lane.get(), nullptr);
        gst_object_unref(encoder_src);
      }
    }

    const auto state = gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (state == GST_STATE_CHANGE_FAILURE) {
      set_pipeline_error("GStreamer WebRTC pipeline failed to enter PLAYING state");
      stop_pipeline();
      return false;
    }
    started_ms = now_ms();
    for (const auto& lane : lanes) {
      lane->pipeline_started_ms = started_ms;
      lane->thread = std::thread([this, lane = lane.get(), capture_interval_ms] {
        std::uint64_t sequence = 0;
        const bool pace_test_source = lane->camera.device == "testsrc" && capture_interval_ms == 0;
        const auto test_source_interval =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)) /
            static_cast<std::int64_t>(std::max(1, lane->profile.fps));
        auto next_test_source_frame = std::chrono::steady_clock::now();
        try {
          while (!stop_requested) {
            if (pace_test_source) {
              const auto current = std::chrono::steady_clock::now();
              if (current < next_test_source_frame) {
                std::this_thread::sleep_until(next_test_source_frame);
              } else if (current - next_test_source_frame > test_source_interval) {
                next_test_source_frame = current;
              }
            }
            auto frame = lane->source->next(++sequence);
            ++lane->captured;
            lane->last_capture_ms = frame.captured_at_ms;
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.payload.size(), nullptr);
            if (buffer == nullptr) throw std::runtime_error("cannot allocate GStreamer camera buffer");
            gst_buffer_fill(buffer, 0, frame.payload.data(), frame.payload.size());
            const auto elapsed_ms = std::max<std::int64_t>(0, frame.captured_at_ms - started_ms);
            GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(elapsed_ms) * GST_MSECOND;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(buffer) = GST_SECOND / static_cast<GstClockTime>(std::max(1, lane->profile.fps));
            const auto flow = gst_app_src_push_buffer(GST_APP_SRC(lane->appsrc), buffer);
            if (flow != GST_FLOW_OK) {
              ++lane->dropped;
              if (flow != GST_FLOW_FLUSHING && flow != GST_FLOW_EOS) {
                throw std::runtime_error("GStreamer appsrc rejected camera frame: " + std::to_string(flow));
              }
              break;
            }
            ++lane->pushed;
            if (pace_test_source) {
              next_test_source_frame += test_source_interval;
            } else if (capture_interval_ms > 0) {
              std::this_thread::sleep_for(std::chrono::milliseconds(capture_interval_ms));
            }
          }
        } catch (const std::exception& error) {
          std::lock_guard lock(lane->error_mutex);
          lane->error = error.what();
          set_pipeline_error("camera " + lane->camera.id + ": " + error.what());
        }
      });
    }
    return true;
  }

  void stop_pipeline() {
    stop_requested = true;
    if (pipeline != nullptr) {
      for (const auto& lane : lanes) {
        if (lane->appsrc != nullptr) gst_app_src_end_of_stream(GST_APP_SRC(lane->appsrc));
      }
      GstBus* bus = gst_element_get_bus(pipeline);
      if (bus != nullptr) {
        GstMessage* message = gst_bus_timed_pop_filtered(
            bus, 3 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
        if (message != nullptr) gst_message_unref(message);
        gst_object_unref(bus);
      }
      gst_element_set_state(pipeline, GST_STATE_NULL);
    }
    for (const auto& lane : lanes) {
      if (lane->thread.joinable()) lane->thread.join();
    }
    for (const auto& lane : lanes) {
      if (lane->appsrc != nullptr) {
        gst_object_unref(lane->appsrc);
        lane->appsrc = nullptr;
      }
      if (lane->encoder != nullptr) {
        gst_object_unref(lane->encoder);
        lane->encoder = nullptr;
      }
    }
    if (webrtc != nullptr) {
      gst_object_unref(webrtc);
      webrtc = nullptr;
    }
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
    write_recording_sidecars();
  }

  void write_recording_sidecars() const {
    const auto session_root = recording_root / config.vehicle_id / signaling.session_id();
    if (recording_root.empty() || !std::filesystem::exists(session_root)) return;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(session_root)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".mp4") continue;
      auto metadata_path = entry.path();
      metadata_path.replace_extension(".json");
      if (std::filesystem::exists(metadata_path)) continue;
      const auto camera_id = entry.path().parent_path().filename().string();
      const auto segment_id = entry.path().stem().string();
      const auto timestamp = now_ms();
      const Json metadata = {
          {"vehicle_id", config.vehicle_id},
          {"session_id", signaling.session_id()},
          {"camera_id", camera_id},
          {"segment_id", segment_id},
          {"started_at", iso_time(timestamp)},
          {"ended_at", iso_time(timestamp)},
          {"codec", to_string(active_candidate.codec)},
          {"encoder", to_string(active_candidate.backend)},
          {"upload_state", "pending"},
          {"video_file", entry.path().filename().string()},
          {"file_size_bytes", entry.file_size()},
          {"video_sha256", sha256_file(entry.path())},
      };
      const auto temporary = metadata_path.string() + ".tmp";
      {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) throw std::runtime_error("cannot write recording sidecar: " + metadata_path.string());
        output << std::setw(2) << metadata << '\n';
      }
      std::filesystem::rename(temporary, metadata_path);
    }
  }

  void flush_outgoing_signals() {
    std::deque<std::pair<std::string, Json>> values;
    {
      std::lock_guard lock(signal_mutex);
      values.swap(pending_signals);
    }
    for (const auto& [type, payload] : values) signaling.send(type, payload);
  }

  void process_signaling() {
    const auto response = signaling.poll("webrtc_answer,ice_candidate,media_fallback");
    for (const auto& message : response.value("messages", Json::array())) {
      const auto type = message.value("type", "");
      const auto payload = message.value("payload", Json::object());
      if (type == "webrtc_answer") {
        const auto sdp_text = payload.value("sdp", "");
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new(&sdp) != GST_SDP_OK ||
            gst_sdp_message_parse_buffer(
                reinterpret_cast<const guint8*>(sdp_text.data()), sdp_text.size(), sdp) != GST_SDP_OK) {
          if (sdp != nullptr) gst_sdp_message_free(sdp);
          throw std::runtime_error("driver returned an invalid WebRTC answer SDP");
        }
        auto* answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        GstPromise* promise = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);
        answer_received = true;
      } else if (type == "ice_candidate") {
        const auto candidate = payload.value("candidate", "");
        const auto index = payload.value("sdpMLineIndex", 0U);
        if (!candidate.empty()) g_signal_emit_by_name(webrtc, "add-ice-candidate", index, candidate.c_str());
      } else if (type == "media_fallback" && active_candidate.codec == VideoCodec::H265) {
        codec_fallback_requested = true;
        set_pipeline_error("browser requested H.264 fallback: " + payload.value("reason", "decode failure"));
      }
    }
  }

  void poll_bus() {
    if (pipeline == nullptr) return;
    GstBus* bus = gst_element_get_bus(pipeline);
    if (bus == nullptr) return;
    while (GstMessage* message = gst_bus_pop(bus)) {
      if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);
        std::string value = error != nullptr ? error->message : "unknown GStreamer error";
        if (debug != nullptr && *debug != '\0') value += ": " + std::string(debug);
        if (error != nullptr) g_error_free(error);
        g_free(debug);
        set_pipeline_error(std::move(value));
      }
      gst_message_unref(message);
    }
    gst_object_unref(bus);
  }

  [[nodiscard]] std::uint64_t total_encoded() const {
    std::uint64_t count = 0;
    for (const auto& lane : lanes) count += lane->encoded.load();
    return count;
  }

  [[nodiscard]] bool frame_target_reached(int frame_count) const {
    if (frame_count <= 0) return false;
    return std::all_of(lanes.begin(), lanes.end(), [&](const auto& lane) {
      return lane->encoded.load() >= static_cast<std::uint64_t>(frame_count);
    });
  }

  [[nodiscard]] Json lane_metrics(std::int64_t elapsed_ms) const {
    Json result = Json::array();
    for (const auto& lane : lanes) {
      std::string error;
      {
        std::lock_guard lock(lane->error_mutex);
        error = lane->error;
      }
      result.push_back({
          {"camera_id", lane->camera.id},
          {"captured_frames", lane->captured.load()},
          {"pushed_frames", lane->pushed.load()},
          {"encoded_frames", lane->encoded.load()},
          {"dropped_frames", lane->dropped.load()},
          {"pipeline_backlog_or_drop_frames", lane->pushed.load() > lane->encoded.load() ? lane->pushed.load() - lane->encoded.load() : 0},
          {"encoded_fps", lane->encoded.load() * 1000.0 / static_cast<double>(std::max<std::int64_t>(1, elapsed_ms))},
          {"capture_to_encoded_ms", lane->encode_latency_samples.load() == 0
                                           ? 0.0
                                           : static_cast<double>(lane->encode_latency_total_ms.load()) /
                                                 static_cast<double>(lane->encode_latency_samples.load())},
          {"capture_to_encoded_max_ms", lane->encode_latency_max_ms.load()},
          {"width", lane->profile.width},
          {"height", lane->profile.height},
          {"target_fps", lane->profile.fps},
          {"error", error},
      });
    }
    return result;
  }

  Json run(int frame_count, int duration_ms, int capture_interval_ms) {
    const bool continuous = frame_count == 0 && duration_ms == 0;
    if (!continuous && frame_count <= 0 && duration_ms < 0) {
      throw std::invalid_argument("frame_count or duration_ms is required");
    }
    if (capture_interval_ms < 0) throw std::invalid_argument("capture interval must be non-negative");
    failover_count = 0;
    last_negotiation_warning.clear();
    signaling.register_online();
    const auto session_deadline = now_ms() + 5000;
    while (!signaling.discover_session()) {
      if (now_ms() >= session_deadline) throw std::runtime_error("timed out waiting for an active driver session");
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto codecs = negotiate_codecs(3000);
    std::vector<EncoderCandidate> candidates;
    for (const auto codec : codecs) {
      const auto codec_candidates = encoder_candidate_order(config.hardware, codec);
      candidates.insert(candidates.end(), codec_candidates.begin(), codec_candidates.end());
    }
    if (candidates.empty()) throw std::runtime_error("no video encoder candidates are configured");
    Json attempts = Json::array();
    Json errors = Json::array();
    Json final_lanes = Json::array();
    std::int64_t total_started_ms = now_ms();
    EncoderCandidate successful = candidates.front();

    for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
      answer_received = false;
      simulated_failure_fired = false;
      codec_fallback_requested = false;
      const auto candidate = candidates[candidate_index];
      const auto attempt_started = now_ms();
      if (!start_pipeline(candidate, capture_interval_ms)) {
        const auto error = current_pipeline_error();
        attempts.push_back({{"backend", to_string(candidate.backend)}, {"codec", to_string(candidate.codec)}, {"passed", false}, {"error", error}});
        errors.push_back(error);
        stop_pipeline();
        if (candidate_index + 1 < candidates.size()) ++failover_count;
        continue;
      }
      const auto deadline = duration_ms > 0 ? total_started_ms + duration_ms : std::numeric_limits<std::int64_t>::max();
      auto next_media_status_ms = now_ms();
      while (!frame_target_reached(frame_count) && (continuous || now_ms() < deadline)) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        flush_outgoing_signals();
        process_signaling();
        poll_bus();
        if (now_ms() >= next_media_status_ms) {
          queue_signal(
              "media_status",
              {{"codec", to_string(candidate.codec)},
               {"backend", to_string(candidate.backend)},
               {"lanes", lane_metrics(std::max<std::int64_t>(1, now_ms() - attempt_started))}});
          next_media_status_ms = now_ms() + 1000;
        }
        if (simulate_primary_failure_after_frames > 0 && candidate_index == 0 && !simulated_failure_fired &&
            total_encoded() >= static_cast<std::uint64_t>(simulate_primary_failure_after_frames)) {
          simulated_failure_fired = true;
          set_pipeline_error("simulated primary encoder failure");
        }
        if (!current_pipeline_error().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      flush_outgoing_signals();
      const auto elapsed = std::max<std::int64_t>(1, now_ms() - attempt_started);
      final_lanes = lane_metrics(elapsed);
      const auto error = current_pipeline_error();
      const bool passed = error.empty() && total_encoded() > 0;
      attempts.push_back({
          {"backend", to_string(candidate.backend)},
          {"codec", to_string(candidate.codec)},
          {"passed", passed},
          {"answer_received", answer_received},
          {"duration_ms", elapsed},
          {"lanes", final_lanes},
          {"error", error},
      });
      stop_pipeline();
      if (passed) {
        successful = candidate;
        break;
      }
      errors.push_back(error);
      if (codec_fallback_requested) {
        while (candidate_index + 1 < candidates.size() && candidates[candidate_index + 1].codec == candidate.codec) {
          ++candidate_index;
        }
      }
      if (candidate_index + 1 < candidates.size()) ++failover_count;
    }

    const auto total_elapsed = std::max<std::int64_t>(1, now_ms() - total_started_ms);
    bool fps_passed = !final_lanes.empty();
    for (const auto& lane : final_lanes) {
      if (lane.value("encoded_fps", 0.0) < config.hardware.min_realtime_fps) fps_passed = false;
    }
    const bool passed = !attempts.empty() && attempts.back().value("passed", false) && fps_passed;
    return {
        {"event", "vehicle_media_webrtc_summary"},
        {"runtime", "cpp"},
        {"passed", passed},
        {"vehicle_id", config.vehicle_id},
        {"session_id", signaling.session_id()},
        {"transport", "webrtc-srtp"},
        {"codec", to_string(successful.codec)},
        {"encoder_backend", to_string(successful.backend)},
        {"camera_count", config.enabled_cameras().size()},
        {"duration_ms", total_elapsed},
        {"minimum_fps", config.hardware.min_realtime_fps},
        {"max_end_to_end_latency_ms", config.hardware.max_end_to_end_latency_ms},
        {"fps_passed", fps_passed},
        {"recording_enabled", !recording_root.empty()},
        {"recording_root", recording_root.string()},
        {"failover_count", failover_count},
        {"attempts", std::move(attempts)},
        {"errors", std::move(errors)},
        {"negotiation_warning", last_negotiation_warning},
    };
  }

  VehicleConfig config;
  MediaSignalingClient signaling;
  int frame_timeout_ms;
  std::filesystem::path recording_root;
  std::optional<std::string> forced_codec;
  int simulate_primary_failure_after_frames;
  GstElement* pipeline{nullptr};
  GstElement* webrtc{nullptr};
  std::vector<std::unique_ptr<Lane>> lanes;
  std::atomic<bool> stop_requested{false};
  std::mutex signal_mutex;
  std::deque<std::pair<std::string, Json>> pending_signals;
  mutable std::mutex error_mutex;
  std::string pipeline_error;
  EncoderCandidate active_candidate{EncoderBackend::Nvenc, VideoCodec::H265};
  std::int64_t started_ms{0};
  bool answer_received{false};
  bool simulated_failure_fired{false};
  bool codec_fallback_requested{false};
  std::uint64_t failover_count{0};
  std::string last_negotiation_warning;
};

VehicleMediaRuntime::VehicleMediaRuntime(
    VehicleConfig config,
    std::string signaling_url,
    std::string device_token,
    int frame_timeout_ms,
    std::filesystem::path recording_root,
    std::optional<std::string> forced_codec,
    int simulate_primary_failure_after_frames)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          std::move(signaling_url),
          std::move(device_token),
          frame_timeout_ms,
          std::move(recording_root),
          std::move(forced_codec),
          simulate_primary_failure_after_frames)) {}

VehicleMediaRuntime::~VehicleMediaRuntime() = default;

Json VehicleMediaRuntime::run(int frame_count, int duration_ms, int capture_interval_ms) {
  return impl_->run(frame_count, duration_ms, capture_interval_ms);
}

}  // namespace mine_teleop
