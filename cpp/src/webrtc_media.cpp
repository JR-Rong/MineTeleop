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

CameraFailureDecision camera_failure_decision(
    const CameraConfig& camera,
    int failures_in_media_attempt,
    bool retryable) {
  if (failures_in_media_attempt <= 0) {
    throw std::invalid_argument("camera failure count must be positive");
  }
  return {
      camera.critical_for_control,
      retryable && failures_in_media_attempt <= camera.reopen_attempts
          ? CameraFailureAction::ReopenLane
          : CameraFailureAction::DisableLane,
  };
}

MediaSignalingErrorKind classify_media_signaling_error(const HttpStatusError& error) {
  if (error.status() == 404) return MediaSignalingErrorKind::SessionEnded;
  if (error.status() >= 500 && error.status() < 600) {
    return MediaSignalingErrorKind::ServiceUnavailable;
  }
  if (error.status() != 409) return MediaSignalingErrorKind::Fatal;

  const auto& issue_code = error.issue_code();
  const std::string_view message(error.what());
  const auto contains = [&](std::string_view value) { return message.find(value) != std::string_view::npos; };
  if (issue_code == "session_not_active" || contains("session is not active")) {
    return MediaSignalingErrorKind::SessionEnded;
  }
  if (issue_code == "vehicle_offline" || contains("vehicle is offline")) {
    return MediaSignalingErrorKind::ConnectionRefresh;
  }
  if (issue_code == "vehicle_connection_generation_stale" ||
      contains("vehicle connection generation is stale")) {
    return MediaSignalingErrorKind::ConnectionStale;
  }
  if (issue_code == "signaling_sequence_older" ||
      issue_code == "signaling_sequence_reused" ||
      contains("sequence is older than the previous message") ||
      contains("sequence was reused with different content")) {
    return MediaSignalingErrorKind::SequenceConflict;
  }
  return MediaSignalingErrorKind::Fatal;
}

std::uint64_t MediaSignalingSequence::next(
    std::uint64_t connection_generation,
    std::string_view session_id) {
  if (connection_generation == 0 || session_id.empty()) {
    throw std::invalid_argument("media signaling sequence scope is incomplete");
  }
  std::lock_guard lock(mutex_);
  if (connection_generation_ != connection_generation || session_id_ != session_id) {
    connection_generation_ = connection_generation;
    session_id_ = session_id;
    value_ = 0;
  }
  if (value_ == std::numeric_limits<std::uint64_t>::max()) {
    throw std::overflow_error("media signaling sequence is exhausted");
  }
  return ++value_;
}

std::uint64_t MediaSignalingSequence::current() const {
  std::lock_guard lock(mutex_);
  return value_;
}

bool CriticalCameraControlLatch::enter_session(std::string_view session_id) {
  if (session_id.empty()) {
    throw std::invalid_argument("critical camera control latch requires a non-empty session id");
  }
  std::lock_guard lock(mutex_);
  if (session_id_ != session_id) {
    session_id_ = session_id;
    inhibited_ = false;
  }
  return inhibited_;
}

bool CriticalCameraControlLatch::inhibit(std::string_view session_id) {
  if (session_id.empty()) {
    throw std::invalid_argument("critical camera control latch requires a non-empty session id");
  }
  std::lock_guard lock(mutex_);
  if (session_id_.empty()) {
    throw std::logic_error("critical camera control latch session was not entered");
  }
  if (session_id_ != session_id) {
    throw std::logic_error("critical camera control latch session does not match the active session");
  }
  const bool first_inhibition = !inhibited_;
  inhibited_ = true;
  return first_inhibition;
}

bool CriticalCameraControlLatch::inhibited_for(std::string_view session_id) const {
  if (session_id.empty()) {
    throw std::invalid_argument("critical camera control latch requires a non-empty session id");
  }
  std::lock_guard lock(mutex_);
  if (session_id_ != session_id) {
    throw std::logic_error("critical camera control latch session does not match the active session");
  }
  return inhibited_;
}

namespace {

std::int64_t steady_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

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

CameraIssue classify_camera_issue_impl(std::string_view error) {
  const auto contains = [&](std::string_view value) {
    return error.find(value) != std::string_view::npos;
  };
  if (contains("camera media source configuration is invalid") ||
      contains("native camera acquisition requires an mjpeg") ||
      contains("CCG2 camera acquisition requires a uyvy") ||
      contains("CCG2 capture requires") ||
      contains("unsupported camera backend") ||
      contains("camera input pipeline configuration is invalid") ||
      contains("camera input pipeline codec is unsupported")) {
    return {"camera_config_invalid", "camera_config", "Check the camera ID, backend, realtime/capture profiles, resolution, FPS, and input codec.", false};
  }
  if (contains("camera is not a vendor SDK source")) {
    return {"camera_source_type_invalid", "camera_config", "Use testsrc, a V4L2 path, or a supported mvs/aravis camera selector.", false};
  }
  if (contains("cannot create media pipe")) {
    return {"camera_bridge_pipe_failed", "vendor_bridge_start", "Check process file-descriptor limits and host resources.", true};
  }
  if (contains("cannot fork media process")) {
    return {"camera_bridge_fork_failed", "vendor_bridge_start", "Check process limits and available memory.", true};
  }
  if (contains("camera process exited")) {
    return {"camera_bridge_exited", "vendor_bridge_capture", "Run the configured vendor camera bridge directly and inspect its stderr.", true};
  }
  if (contains("MJPEG frame exceeded")) {
    return {"camera_frame_too_large", "camera_capture", "Check camera output format and bridge framing; the MJPEG frame limit is 16 MiB.", false};
  }
  if (contains("timed out waiting for camera frame") ||
      contains("timed out waiting for V4L2 frame")) {
    return {"camera_frame_timeout", "camera_capture", "Check camera power, USB link, selected node, FPS, and whether another process owns the device.", true};
  }
  if (contains("media poll failed") || contains("V4L2 poll failed")) {
    return {"camera_poll_failed", "camera_capture", "Check device health and kernel logs, then reconnect or restart the camera.", true};
  }
  if (contains("media read failed")) {
    return {"camera_bridge_read_failed", "vendor_bridge_capture", "Inspect the vendor bridge process and camera SDK logs.", true};
  }
  if (contains("cannot open V4L2 camera")) {
    return {"camera_open_failed", "v4l2_open", "Check the configured device path, camera connection, permissions, and whether another process owns the node; runtime reopen attempts are bounded.", true};
  }
  if (contains("VIDIOC_QUERYCAP failed")) {
    return {"camera_querycap_failed", "v4l2_capabilities", "Verify that the configured path is a V4L2 device node.", false};
  }
  if (contains("must support V4L2 capture and streaming")) {
    return {"camera_node_not_capture_capable", "v4l2_capabilities", "Select the capture-capable video-index node reported by v4l2-ctl, not its metadata node.", false};
  }
  if (contains("VIDIOC_S_FMT MJPEG failed")) {
    return {"camera_mjpeg_format_rejected", "v4l2_format", "Use a width, height, and MJPEG format advertised by v4l2-ctl.", false};
  }
  if (contains("VIDIOC_S_FMT CCG2 YUYV failed") ||
      contains("CCG2 driver did not negotiate reported YUYV")) {
    return {"camera_ccg2_yuyv_format_rejected", "v4l2_format", "Confirm the CCG2 node advertises YUYV at the configured width, height, and FPS.", false};
  }
  if (contains("CCG2 driver negotiated unexpected dimensions") ||
      contains("CCG2 driver returned invalid UYVY dimensions")) {
    return {"camera_ccg2_dimensions_mismatch", "v4l2_format", "Use the exact CCG2 V4L2 capture dimensions; board input status is diagnostic and is not the application frame height.", false};
  }
  if (contains("CCG2 driver returned bytesperline") ||
      contains("CCG2 driver returned sizeimage") ||
      contains("CCG2 driver returned an overflowing UYVY layout") ||
      contains("CCG2 UYVY bytesperline") ||
      contains("CCG2 UYVY frame layout overflows") ||
      contains("CCG2 UYVY packed frame size overflows")) {
    return {"camera_ccg2_layout_invalid", "v4l2_format", "Inspect the negotiated bytesperline/sizeimage and CCG2 driver version before capturing again.", false};
  }
  if (contains("CCG2 UYVY frame is shorter")) {
    return {"camera_ccg2_frame_short", "v4l2_capture", "Inspect bytesused, bytesperline, sizeimage, PCIe link health, and the CCG2 driver log.", true};
  }
  if (contains("CCG2 driver returned invalid timeperframe") ||
      contains("CCG2 driver returned unexpected timeperframe")) {
    return {"camera_ccg2_fps_mismatch", "v4l2_frame_rate", "Use a CCG2 capture FPS accepted exactly by the driver for the configured capture resolution.", false};
  }
  if (contains("CCG2 V4L2 buffer flagged error")) {
    return {"camera_ccg2_buffer_error", "v4l2_capture", "Inspect the reported V4L2 sequence/gap, PCIe link health, camera link status, and the CCG2 driver log.", true};
  }
  if (contains("does not provide native MJPEG")) {
    return {"camera_native_mjpeg_unavailable", "v4l2_format", "Choose a native MJPEG camera mode or add a supported raw-frame conversion path.", false};
  }
  if (contains("VIDIOC_S_PARM failed")) {
    return {"camera_fps_rejected", "v4l2_frame_rate", "Use an FPS advertised for the selected camera backend and capture resolution.", false};
  }
  if (contains("mmap buffers are unavailable")) {
    return {"camera_mmap_buffers_unavailable", "v4l2_buffers", "Check driver streaming support and available memory.", true};
  }
  if (contains("VIDIOC_QUERYBUF failed")) {
    return {"camera_query_buffer_failed", "v4l2_buffers", "Inspect the V4L2 driver and reconnect the camera.", true};
  }
  if (contains("mmap failed")) {
    return {"camera_mmap_failed", "v4l2_buffers", "Check memory pressure and the V4L2 driver.", true};
  }
  if (contains("VIDIOC_QBUF failed")) {
    return {"camera_queue_buffer_failed", "v4l2_stream", "Inspect the V4L2 driver and USB link, then restart capture.", true};
  }
  if (contains("VIDIOC_STREAMON failed")) {
    return {"camera_stream_on_failed", "v4l2_stream", "Check for device contention, USB bandwidth limits, and a supported capture mode.", true};
  }
  if (contains("VIDIOC_DQBUF failed")) {
    return {"camera_dequeue_buffer_failed", "v4l2_capture", "Inspect the V4L2 driver and USB link, then restart capture.", true};
  }
  if (contains("invalid capture buffer")) {
    return {"camera_invalid_capture_buffer", "v4l2_capture", "Treat this as a V4L2 driver fault and inspect kernel logs.", true};
  }
  if (contains("invalid MJPEG frame")) {
    return {"camera_invalid_mjpeg_frame", "camera_decode_boundary", "Verify camera/bridge MJPEG framing and USB data integrity.", true};
  }
  if (contains("camera frame does not match configured input caps")) {
    return {"camera_input_caps_mismatch", "camera_decode_boundary", "Check the selected backend and capture dimensions before restarting the media lane.", false};
  }
  if (contains("native JPEG encoder failed")) {
    return {"camera_test_jpeg_encode_failed", "test_source_encode", "Check the native JPEG runtime and requested test-source dimensions.", false};
  }
  if (contains("cannot allocate GStreamer camera buffer")) {
    return {"camera_gstreamer_buffer_allocation_failed", "gstreamer_push", "Check memory pressure and GStreamer allocation errors.", true};
  }
  if (contains("GStreamer appsrc rejected camera frame")) {
    return {"camera_appsrc_push_failed", "gstreamer_push", "Inspect the GStreamer bus error and downstream encoder state.", true};
  }
  return {"camera_capture_failed", "camera_capture", "Inspect the error text, camera device, and matching camera bridge or V4L2 diagnostics.", true};
}

class MediaSignalingClient {
 public:
  MediaSignalingClient(
      std::string origin,
      std::string vehicle_id,
      std::string device_token,
      std::string connection_id,
      std::shared_ptr<MediaSignalingSequence> sequence,
      std::vector<std::string> resolve_entries,
      std::filesystem::path ca_bundle)
      : origin_(trim_origin(std::move(origin))),
        vehicle_id_(std::move(vehicle_id)),
        device_token_(std::move(device_token)),
        connection_id_(std::move(connection_id)),
        http_(std::chrono::seconds(5), std::move(resolve_entries), std::move(ca_bundle)),
        sequence_(sequence ? std::move(sequence) : std::make_shared<MediaSignalingSequence>()) {
    if (vehicle_id_.empty() || device_token_.empty()) throw std::invalid_argument("vehicle id and device token are required");
    if (connection_id_.empty()) {
      connection_id_ = "vehicle-media-" + vehicle_id_ + "-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }
  }

  void register_online() {
    const auto response = http_.post_json_response(
        origin_ + "/vehicles/online",
        {{"vehicle_id", vehicle_id_},
         {"device_token", device_token_},
         {"connection_id", connection_id_}});
    connection_generation_ = response.at("connection_generation").get<std::uint64_t>();
  }

  TimeSyncStatus synchronize_time(int sample_count) {
    return clock_.synchronize(http_, origin_, sample_count);
  }

  [[nodiscard]] TimeSyncStatus time_sync_status() const { return clock_.status(); }
  [[nodiscard]] bool time_sync_refresh_due(int interval_ms) const { return clock_.refresh_due(interval_ms); }
  [[nodiscard]] std::int64_t now_ms() const { return clock_.now_ms(); }
  [[nodiscard]] std::int64_t from_local_system_ms(std::int64_t value) const {
    return clock_.from_local_system_ms(value);
  }

  bool discover_session() {
    require_connection();
    const auto response = http_.get_json(
        origin_ + "/vehicles/" + http_.url_encode(vehicle_id_) + "/session?connection_generation=" +
            std::to_string(connection_generation_),
        {{"X-Mine-Teleop-Device-Token", device_token_}});
    const auto next_session_id = response.value("session_id", "");
    session_id_ = next_session_id;
    driver_id_ = response.value("driver_id", "");
    control_token_ = response.value("control_token", "");
    return !session_id_.empty() && !driver_id_.empty() && !control_token_.empty();
  }

  Json poll(std::string_view types) {
    require_session();
    return http_.get_json(
        origin_ + "/signaling/" + http_.url_encode(session_id_) + "/messages?recipient=" +
            http_.url_encode(vehicle_id_) + "&connection_generation=" + std::to_string(connection_generation_) +
            "&types=" + http_.url_encode(types),
        {{"X-Mine-Teleop-Device-Token", device_token_}});
  }

  Json ice_servers() {
    require_session();
    return http_.get_json(
        origin_ + "/sessions/" + http_.url_encode(session_id_) + "/ice_servers?actor=" +
            http_.url_encode(vehicle_id_) + "&connection_generation=" + std::to_string(connection_generation_),
        {{"X-Mine-Teleop-Device-Token", device_token_}});
  }

  [[nodiscard]] std::string url_encode(std::string_view value) const { return http_.url_encode(value); }

  void send(std::string_view type, const Json& payload) {
    require_session();
    // Sequence allocation and HTTP delivery must share one critical section.
    // Otherwise concurrent ICE/camera callbacks can allocate N and N+1 but
    // reach the server in the opposite order, which the replay guard correctly
    // rejects as an older sequence.
    std::lock_guard lock(send_mutex_);
    const ProtocolMetadata metadata{
        kProtocolVersion,
        vehicle_id_,
        driver_id_,
        session_id_,
        sequence_->next(connection_generation_, session_id_),
        clock_.now_ms()};
    auto request = metadata.to_json();
    request["sender"] = vehicle_id_;
    request["recipient"] = driver_id_;
    request["device_token"] = device_token_;
    request["connection_generation"] = connection_generation_;
    request["type"] = type;
    request["payload"] = payload;
    static_cast<void>(http_.post_json_response(
        origin_ + "/signaling/" + http_.url_encode(session_id_) + "/messages",
        request));
  }

  [[nodiscard]] const std::string& session_id() const { return session_id_; }
  [[nodiscard]] const std::string& driver_id() const { return driver_id_; }
  [[nodiscard]] const std::string& control_token() const { return control_token_; }

 private:
  void require_connection() const {
    if (connection_generation_ == 0) throw std::runtime_error("media signaling connection is not registered");
  }

  void require_session() const {
    require_connection();
    if (session_id_.empty() || driver_id_.empty()) throw std::runtime_error("media signaling session is unavailable");
  }

  std::string origin_;
  std::string vehicle_id_;
  std::string device_token_;
  std::string connection_id_;
  std::uint64_t connection_generation_{0};
  HttpClient http_;
  SynchronizedClock clock_;
  std::string session_id_;
  std::string driver_id_;
  std::string control_token_;
  std::shared_ptr<MediaSignalingSequence> sequence_;
  std::mutex send_mutex_;
};

}  // namespace

CameraIssue classify_camera_issue(std::string_view error) {
  return classify_camera_issue_impl(error);
}

struct VehicleMediaRuntime::Impl {
  struct Lane {
    Impl* owner{nullptr};
    CameraConfig camera;
    MediaProfile profile;
    CameraInputSpec input;
    std::unique_ptr<CameraFrameSource> source;
    GstElement* appsrc{nullptr};
    GstElement* encoder{nullptr};
    std::thread thread;
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> encoded{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<bool> source_sequence_valid{false};
    std::atomic<std::uint64_t> source_sequence{0};
    std::atomic<std::uint64_t> source_sequence_gap{0};
    std::atomic<std::uint32_t> source_timeperframe_numerator{0};
    std::atomic<std::uint32_t> source_timeperframe_denominator{0};
    std::atomic<std::int64_t> last_capture_ms{0};
    std::atomic<std::int64_t> last_encoded_ms{0};
    std::atomic<std::int64_t> last_encoded_steady_ms{0};
    std::atomic<std::uint64_t> encode_latency_samples{0};
    std::atomic<std::uint64_t> encode_latency_total_ms{0};
    std::atomic<std::uint64_t> encode_latency_max_ms{0};
    std::atomic<bool> first_frame_reported{false};
    std::atomic<int> failure_count{0};
    std::atomic<int> reopen_count{0};
    std::atomic<bool> disabled{false};
    std::int64_t pipeline_started_ms{0};
    std::int64_t pipeline_started_steady_ms{0};
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
      int next_simulate_primary_failure_after_frames,
      std::string connection_id,
      std::shared_ptr<MediaSignalingSequence> signaling_sequence,
      std::shared_ptr<CriticalCameraControlLatch> next_critical_camera_control_latch)
      : config(std::move(next_config)),
        signaling(
            std::move(signaling_url),
            config.vehicle_id,
            std::move(device_token),
            std::move(connection_id),
            std::move(signaling_sequence),
            config.cloud.resolve_entries,
            config.cloud.ca_bundle),
        critical_camera_control_latch(
            next_critical_camera_control_latch
                ? std::move(next_critical_camera_control_latch)
                : std::make_shared<CriticalCameraControlLatch>()),
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
      const auto encoded_at_ms = lane->owner->signaling.now_ms();
      lane->last_encoded_ms = encoded_at_ms;
      lane->last_encoded_steady_ms = steady_now_ms();
      GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      if (buffer != nullptr && GST_BUFFER_PTS_IS_VALID(buffer)) {
        const auto captured_at_ms = lane->pipeline_started_ms + static_cast<std::int64_t>(GST_BUFFER_PTS(buffer) / GST_MSECOND);
        const auto latency_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(
            0, encoded_at_ms - captured_at_ms));
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
    // GStreamer 1.28 emits a final empty candidate after ICE gathering is
    // complete.  Trickle ICE endpoints accept only real candidates; the empty
    // marker must not be sent as an application signaling message.
    if (candidate == nullptr || *candidate == '\0') return;
    auto* self = static_cast<Impl*>(user_data);
    const auto count = ++self->local_ice_candidate_count;
    if (count == 1) {
      self->emit_diagnostic(
          "vehicle_webrtc_ice_candidate_discovered",
          "webrtc_local_ice_candidate_available",
          "ice_gathering",
          "",
          "No action is required; this is a connectivity milestone.",
          true,
          {{"candidate_count", count}, {"sdp_mline_index", mline_index}});
    }
    self->queue_signal(
        "ice_candidate",
        {{"candidate", candidate}, {"sdpMLineIndex", mline_index}});
  }

  static void on_control_channel_open(GstWebRTCDataChannel* channel, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    bool stale_channel = false;
    {
      std::lock_guard lock(self->control_mutex);
      // A late GStreamer callback may arrive while the media attempt is being
      // torn down.  Only the currently published channel may open control.
      if (self->stop_requested || self->control_channel != channel) {
        stale_channel = true;
      } else {
        self->control_link_open = true;
        self->control_link_ever_opened = true;
        self->control_link_opened_this_attempt = true;
      }
    }
    if (stale_channel) {
      gst_webrtc_data_channel_close(channel);
      return;
    }
    const bool cameras_ready = self->critical_cameras_ready();
    const bool control_inhibited = self->control_inhibited.load();
    bool adapter_ready = false;
    if (control_inhibited) {
      std::lock_guard lock(self->control_mutex);
      self->control_service_issue_code = "critical_camera_failed";
    } else if (cameras_ready) {
      adapter_ready = self->start_control_service();
    } else {
      std::lock_guard lock(self->control_mutex);
      self->control_service_issue_code = "critical_camera_not_ready";
    }
    self->send_vcu_handshake_status("driver_connected");
    std::cout << Json({
                     {"event", "vehicle_control_data_channel_open"},
                     {"event_at_utc_ms", self->signaling.now_ms()},
                     {"vehicle_id", self->config.vehicle_id},
                     {"driver_id", self->signaling.driver_id()},
                     {"session_id", self->signaling.session_id()},
                     {"ordered", false},
                     {"max_retransmits", 0},
                     {"adapter_ready", adapter_ready},
                     {"critical_cameras_ready", cameras_ready},
                     {"control_inhibited", control_inhibited},
                 }).dump()
              << '\n';
    if (control_inhibited || (cameras_ready && !adapter_ready)) {
      // Closing only the control DataChannel protects older controller builds
      // from treating an unacknowledged ESTOP as delivered.  RTP/video stays
      // on the PeerConnection and continues independently.  The callback's
      // channel argument remains alive for the duration of this signal, while
      // the member may already have been detached by stop_pipeline().
      gst_webrtc_data_channel_close(channel);
    }
  }

  static void on_control_channel_close(GstWebRTCDataChannel* channel, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    std::lock_guard lock(self->control_mutex);
    // stop_pipeline detaches the member before closing it, and a callback from
    // an older media attempt must not close the replacement attempt's adapter.
    if (self->control_channel != channel) return;
    const bool was_open = self->control_link_open.exchange(false);
    if (!was_open) return;
    ++self->control_link_loss_count;
    if (self->control_service_started && self->control_service) {
      try {
        self->control_service->close();
      } catch (const std::exception& error) {
        self->emit_diagnostic(
            "vehicle_vcu_safe_stop_failed",
            "vcu_safe_stop_or_close_failed",
            "vcu_data_channel_close",
            error.what(),
            "Keep the vehicle isolated and inspect the VCU JSONL log/CAN interface before restart.",
            true,
            {{"safety_action", "local_full_stop_requested"}});
      }
      self->control_service_started = false;
      self->control_service.reset();
    }
    std::cout << Json({
                     {"event", "vehicle_control_data_channel_closed"},
                     {"event_at_utc_ms", self->signaling.now_ms()},
                     {"vehicle_id", self->config.vehicle_id},
                     {"driver_id", self->signaling.driver_id()},
                     {"session_id", self->signaling.session_id()},
                     {"accepted_commands", self->accepted_control_commands.load()},
                     {"rejected_commands", self->rejected_control_commands.load()},
                     {"last_received_at_utc_ms", self->last_control_received_at_ms.load()},
                     {"safety_action", "local_full_stop"},
                 }).dump()
              << '\n';
  }

  static void on_control_channel_error(GstWebRTCDataChannel* channel, GError* error, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    std::cout << Json({
                     {"event", "vehicle_control_data_channel_error"},
                     {"event_at_utc_ms", self->signaling.now_ms()},
                     {"vehicle_id", self->config.vehicle_id},
                     {"session_id", self->signaling.session_id()},
                     {"error", error == nullptr ? "unknown data channel error" : error->message},
                 }).dump()
              << '\n';
    on_control_channel_close(channel, user_data);
  }

  static void on_control_message_string(GstWebRTCDataChannel* channel, gchar* data, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    self->handle_control_message(channel, data == nullptr ? "" : data);
  }

  static void on_offer_created(GstPromise* promise, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
      gst_promise_unref(promise);
      self->set_pipeline_error(
          "WebRTC offer promise failed",
          "webrtc_offer_promise_failed",
          "webrtc_offer",
          "Inspect webrtcbin/GStreamer errors and verify that all media lanes are linked.",
          true);
      return;
    }
    const auto* reply = gst_promise_get_reply(promise);
    GstWebRTCSessionDescription* offer = nullptr;
    gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    gst_promise_unref(promise);
    if (offer == nullptr) {
      self->set_pipeline_error(
          "WebRTC offer is missing",
          "webrtc_offer_missing",
          "webrtc_offer",
          "Inspect webrtcbin/GStreamer negotiation errors.",
          true);
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
    self->emit_diagnostic(
        "vehicle_webrtc_offer_created",
        "webrtc_offer_created",
        "webrtc_offer",
        "",
        "No action is required; wait for the controller answer.",
        true,
        {{"track_count", self->lanes.size()},
         {"codec", to_string(self->active_candidate.codec)},
         {"backend", to_string(self->active_candidate.backend)}});
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

  void emit_diagnostic(
      std::string_view event,
      std::string_view issue_code,
      std::string_view stage,
      std::string_view error,
      std::string_view operator_action,
      bool retryable,
      Json details = Json::object()) const {
    details["event"] = event;
    details["issue_code"] = issue_code;
    details["stage"] = stage;
    details["subsystem"] = "vehicle_media";
    details["severity"] = error.empty() ? "info" : "error";
    details["retryable"] = retryable;
    details["event_at_utc_ms"] = signaling.now_ms();
    details["vehicle_id"] = config.vehicle_id;
    details["driver_id"] = signaling.driver_id();
    details["session_id"] = signaling.session_id();
    details["operator_action"] = operator_action;
    if (!error.empty()) details["error"] = error;
    std::lock_guard lock(diagnostic_mutex);
    std::cout << details.dump() << std::endl;
  }

  void stop_control_for_pipeline_fault(std::string_view issue_code) {
    std::optional<std::string> close_error;
    GstWebRTCDataChannel* channel_to_close = nullptr;
    {
      std::lock_guard lock(control_mutex);
      control_service_issue_code = std::string(issue_code);
      if (control_service_started && control_service) {
        try {
          // close() applies the local safe-stop output before closing the
          // adapter.  This must happen in the faulting thread rather than wait
          // for a potentially blocked HTTP/media main loop.
          control_service->close();
        } catch (const std::exception& error) {
          close_error = error.what();
        }
      }
      control_service_started = false;
      control_service.reset();
      send_vcu_handshake_status_locked("media_pipeline_failed");
      if (control_channel != nullptr) {
        channel_to_close = GST_WEBRTC_DATA_CHANNEL(g_object_ref(control_channel));
      }
    }
    if (close_error) {
      emit_diagnostic(
          "vehicle_vcu_safe_stop_failed",
          "vcu_safe_stop_or_close_failed",
          "media_pipeline_safety",
          *close_error,
          "Use the physical emergency stop, keep the vehicle isolated, and inspect the VCU/CAN log.",
          true,
          {{"safety_action", "physical_estop_required"}});
    }
    if (channel_to_close != nullptr) {
      gst_webrtc_data_channel_close(channel_to_close);
      g_object_unref(channel_to_close);
    }
  }

  void set_pipeline_error(
      std::string value,
      std::string issue_code,
      std::string stage,
      std::string operator_action,
      bool retryable,
      Json details = Json::object()) {
    bool first = false;
    {
      std::lock_guard lock(error_mutex);
      if (pipeline_error.empty()) {
        pipeline_error = value;
        pipeline_issue_code = issue_code;
        pipeline_error_stage = stage;
        pipeline_operator_action = operator_action;
        pipeline_error_retryable = retryable;
        first = true;
      }
    }
    if (first) {
      // Publish teardown before emitting diagnostics.  DataChannel handlers,
      // VCU ticks, and adapter startup all gate on this atomic flag, so a
      // pipeline fault cannot leave one more control cycle runnable while the
      // media thread works its way back to the outer loop.
      stop_requested = true;
      stop_control_for_pipeline_fault(issue_code);
      details["codec"] = to_string(active_candidate.codec);
      details["backend"] = to_string(active_candidate.backend);
      details["safety_action"] = "local_full_stop";
      emit_diagnostic(
          "vehicle_media_pipeline_failed",
          issue_code,
          stage,
          value,
          operator_action,
          retryable,
          std::move(details));
    }
  }

  void emit_camera_failure(
      const Lane& lane,
      std::string_view error,
      int failure_count,
      const CameraFailureDecision& decision) const {
    const auto issue = classify_camera_issue(error);
    const auto failed_source_sequence = lane.source == nullptr
        ? std::optional<std::uint32_t>{}
        : lane.source->last_v4l2_sequence();
    const auto failed_source_sequence_gap = lane.source == nullptr
        ? std::uint64_t{0}
        : lane.source->last_v4l2_sequence_gap();
    const auto safety_action = decision.inhibit_control
        ? (decision.lane_action == CameraFailureAction::ReopenLane
               ? "local_full_stop_and_reopen_camera_lane"
               : "local_full_stop_and_disable_failed_camera_lane")
        : (decision.lane_action == CameraFailureAction::ReopenLane
               ? "reopen_noncritical_camera_lane_only"
               : "disable_noncritical_camera_lane_only");
    emit_diagnostic(
        "vehicle_camera_failed",
        issue.code,
        issue.stage,
        error,
        issue.action,
        issue.retryable,
        {
            {"camera_id", lane.camera.id},
            {"device", lane.camera.device},
            {"source_kind", camera_source_kind_name(classify_camera_source(lane.camera))},
            {"profile", lane.camera.realtime_profile},
            {"configured_width", lane.profile.width},
            {"configured_height", lane.profile.height},
            {"configured_fps", lane.profile.fps},
            {"capture_codec", lane.input.codec},
            {"capture_width", lane.input.width},
            {"capture_height", lane.input.height},
            {"capture_fps", lane.input.fps},
            {"captured_frames", lane.captured.load()},
            {"pushed_frames", lane.pushed.load()},
            {"encoded_frames", lane.encoded.load()},
            {"dropped_frames", lane.dropped.load()},
            {"source_sequence_valid", failed_source_sequence.has_value()},
            {"source_sequence", failed_source_sequence.value_or(0)},
            {"source_sequence_gap", failed_source_sequence_gap},
            {"critical_for_control", lane.camera.critical_for_control},
            {"failure_count", failure_count},
            {"reopen_attempts", lane.camera.reopen_attempts},
            {"safety_action", safety_action},
        });
  }

  void handle_control_message(GstWebRTCDataChannel* channel, std::string_view data) {
    if (data.empty() || data.size() > 64 * 1024) {
      ++rejected_control_commands;
      return;
    }
    try {
      const auto message = Json::parse(data);
      if (message.value("type", "") == "session_control_profile") {
        const auto request = SessionControlProfileRequest::from_json(message);
        std::lock_guard lock(control_mutex);
        if (control_channel != channel) return;
        SessionControlProfileResult result;
        if (stop_requested || control_inhibited || !control_service_started ||
            !control_service || !control_link_open) {
          result.protocol_version = request.protocol_version;
          result.vehicle_id = request.vehicle_id;
          result.driver_id = request.driver_id;
          result.session_id = request.session_id;
          result.seq = request.seq;
          result.sent_at_utc_ms = signaling.now_ms();
          result.accepted = false;
          result.reason = "driver_not_connected";
        } else {
          result = control_service->receive_session_profile(
              request,
              signaling.now_ms());
        }
        send_session_control_profile_status_locked(result);
        std::cout << Json({
                         {"event", "vehicle_session_control_profile_received"},
                         {"event_at_utc_ms", signaling.now_ms()},
                         {"vehicle_id", config.vehicle_id},
                         {"driver_id", signaling.driver_id()},
                         {"session_id", signaling.session_id()},
                         {"request_seq", request.seq},
                         {"accepted", result.accepted},
                         {"idempotent", result.idempotent},
                         {"reason", result.reason},
                     }).dump()
                  << '\n';
        return;
      }
      if (message.value("event", "") == "vcu_handshake_command") {
        const auto action = message.value("action", "");
        std::lock_guard lock(control_mutex);
        if (control_channel != channel) return;
        if (stop_requested || control_inhibited || !control_service_started ||
            !control_service || !control_link_open) {
          send_vcu_handshake_status_locked("driver_not_connected");
          return;
        }
        bool accepted = false;
        try {
          if (action == "connect") {
            accepted = control_service->request_vcu_handshake();
          } else if (action == "disconnect") {
            accepted = control_service->disconnect_vcu_handshake();
          }
        } catch (const std::exception& error) {
          emit_diagnostic(
              "vehicle_vcu_handshake_command_failed",
              "vcu_handshake_command_failed",
              "vcu_handshake_command",
              error.what(),
              "Inspect the VCU JSONL log and CAN interface before retrying the handshake.",
              true,
              {{"action", action}, {"safety_action", "local_full_stop"}});
          return;
        }
        std::cout << Json({
                         {"event", "vehicle_vcu_handshake_command"},
                         {"event_at_utc_ms", signaling.now_ms()},
                         {"vehicle_id", config.vehicle_id},
                         {"driver_id", signaling.driver_id()},
                         {"session_id", signaling.session_id()},
                         {"action", action},
                         {"accepted", accepted},
                         {"vcu_handshake", control_service->vcu_handshake_status().to_json()},
                     }).dump()
                  << '\n';
        send_vcu_handshake_status_locked(
            accepted ? "command_accepted" : "command_rejected");
        return;
      }
      const auto command = ControlCommand::from_json(message);
      std::lock_guard lock(control_mutex);
      if (stop_requested || control_inhibited || control_channel != channel ||
          !control_service_started || !control_service || !control_link_open) {
        ++rejected_control_commands;
        return;
      }
      const auto received_at_ms = signaling.now_ms();
      const auto result = control_service->receive_command(command, received_at_ms);
      if (result.accepted && result.command) {
        const auto accepted_count = ++accepted_control_commands;
        last_control_received_at_ms = received_at_ms;
        if (accepted_count == 1 || accepted_count % 100 == 0) {
          std::cout << Json({
                           {"event", "vehicle_data_channel_control_progress"},
                           {"event_at_utc_ms", received_at_ms},
                           {"vehicle_id", config.vehicle_id},
                           {"driver_id", signaling.driver_id()},
                           {"session_id", signaling.session_id()},
                           {"accepted_commands", accepted_count},
                           {"rejected_commands", rejected_control_commands.load()},
                       }).dump()
                    << '\n';
        }
      } else {
        ++rejected_control_commands;
        const auto now_ms = signaling.now_ms();
        if (!result.issue_code.empty()) {
          send_control_command_rejected_locked(command.seq, result.issue_code);
        }
        if (result.reason != last_control_rejection_reason ||
            !last_control_rejection_log_ms ||
            now_ms - *last_control_rejection_log_ms >= 5000) {
          last_control_rejection_reason = result.reason;
          last_control_rejection_log_ms = now_ms;
          const bool feedback_problem =
              result.reason == "can_feedback_missing" ||
              result.reason == "can_feedback_poll_failed";
          const auto diagnostic_issue_code = result.issue_code.empty()
              ? (feedback_problem
                     ? std::string("vcu_feedback_blocks_control")
                     : std::string("control_command_rejected"))
              : result.issue_code;
          emit_diagnostic(
              "vehicle_control_command_rejected",
              diagnostic_issue_code,
              feedback_problem ? "vcu_feedback_gate" : "control_validation",
              result.reason,
              result.issue_code == "vcu_drive_gear_change_moving_or_stale"
                  ? "Stop the vehicle, restore fresh speed and gear feedback, release the direction control, and select D/R again."
                  : feedback_problem
                  ? "Inspect VCU feedback freshness and the VCU JSONL log before requesting control."
                  : "Inspect command identity, sequence, timing, token, and configured safety limits.",
              true,
              {{"reason", result.reason}, {"safety_action", "local_full_stop"}});
        }
      }
      if (config.runtime.control_log_commands) {
        Json entry = {
            {"event", "vehicle_data_channel_control_received"},
            {"protocol_version", command.protocol_version},
            {"vehicle_id", command.vehicle_id},
            {"driver_id", command.driver_id},
            {"session_id", command.session_id},
            {"seq", command.seq},
            {"sent_at_utc_ms", command.sent_at_utc_ms},
            {"received_at_utc_ms", received_at_ms},
            {"accepted", result.accepted},
            {"reason", result.reason},
            {"requested_steering", command.steering},
            {"requested_throttle", command.throttle},
            {"requested_brake", command.brake},
            {"warnings", result.warnings},
        };
        if (result.command) {
          entry["effective_steering"] = result.command->steering;
          entry["effective_throttle"] = result.command->throttle;
          entry["effective_brake"] = result.command->brake;
        }
        std::cout << entry.dump() << '\n';
      }
    } catch (const std::exception& error) {
      ++rejected_control_commands;
      std::cout << Json({
                       {"event", "vehicle_data_channel_control_rejected"},
                       {"event_at_utc_ms", signaling.now_ms()},
                       {"vehicle_id", config.vehicle_id},
                       {"session_id", signaling.session_id()},
                       {"reason", "invalid_control_message"},
                       {"error", error.what()},
                   }).dump()
                << '\n';
    }
  }

  void send_control_command_rejected_locked(
      std::uint64_t command_seq,
      std::string_view issue_code) {
    if (control_channel == nullptr || !control_link_open) return;
    const std::string stable_issue_code =
        issue_code == "vcu_drive_gear_change_moving_or_stale"
        ? "vcu_drive_gear_change_moving_or_stale"
        : "vcu_control_apply_rejected";
    const auto timestamp_ms = signaling.now_ms();
    if (stable_issue_code == last_control_rejection_status_issue_code &&
        last_control_rejection_status_ms &&
        timestamp_ms - *last_control_rejection_status_ms < 500) {
      return;
    }
    last_control_rejection_status_issue_code = stable_issue_code;
    last_control_rejection_status_ms = timestamp_ms;
    const auto payload = Json({
        {"event", "control_command_rejected"},
        {"protocol_version", kProtocolVersion},
        {"vehicle_id", config.vehicle_id},
        {"driver_id", signaling.driver_id()},
        {"session_id", signaling.session_id()},
        {"control_status_seq", ++control_status_seq},
        {"command_seq", command_seq},
        {"accepted", false},
        {"issue_code", stable_issue_code},
    }).dump();
    gst_webrtc_data_channel_send_string(control_channel, payload.c_str());
  }

  void send_vcu_handshake_status(std::string_view result) {
    std::lock_guard lock(control_mutex);
    send_vcu_handshake_status_locked(result);
  }

  void send_latest_vehicle_telemetry_locked() {
    if (control_channel == nullptr || !control_link_open ||
        !control_service_started || !control_service) {
      return;
    }
    const auto& history = control_service->telemetry_history();
    if (history.empty()) return;
    const auto& telemetry = history.back();
    const auto sequence = telemetry.value("seq", std::uint64_t{0});
    if (sequence == last_vehicle_telemetry_seq) return;
    auto payload_value = telemetry;
    payload_value["control_status_seq"] = ++control_status_seq;
    const auto payload = payload_value.dump();
    gst_webrtc_data_channel_send_string(control_channel, payload.c_str());
    last_vehicle_telemetry_seq = sequence;
  }

  void send_session_control_profile_status_locked(
      const SessionControlProfileResult& result) {
    if (control_channel == nullptr || !control_link_open) return;
    auto message = result.to_json();
    message["event"] = "session_control_profile_status";
    message["control_status_seq"] = ++control_status_seq;
    if (control_service_started && control_service) {
      message["hard_limits"] = control_service->control_limits();
      message["session_control_profile"] =
          control_service->session_control_profile();
    }
    const auto payload = message.dump();
    gst_webrtc_data_channel_send_string(control_channel, payload.c_str());
  }

  [[nodiscard]] Json configured_control_limits() const {
    return {
        {"max_speed_kph", config.field_safety.max_speed_kph},
        {"max_throttle", config.field_safety.max_throttle},
        {"max_target_speed_kph",
         config.field_safety.max_speed_kph * config.field_safety.max_throttle},
        {"full_scale_motor_torque_nm", config.field_safety.full_scale_motor_torque_nm},
        {"max_brake_pressure_bar",
         config.field_safety.max_brake_pressure_bar},
        {"max_steering_angle_deg", config.field_safety.max_steering_angle_deg},
    };
  }

  void send_vcu_handshake_status_locked(std::string_view result) {
    if (control_channel == nullptr || !control_link_open) return;
    try {
      VcuHandshakeStatus status;
      Json hard_limits = configured_control_limits();
      if (control_service_started && control_service) {
        status = control_service->vcu_handshake_status();
        hard_limits = control_service->control_limits();
      } else {
        // A failed or unavailable adapter must never be reported as
        // "unsupported": the controller intentionally treats that state as
        // not requiring a VCU handshake.  "fault" keeps every driving command
        // fail-closed while the independent video tracks remain available.
        status.supported = true;
        status.state = "fault";
      }
      Json message = {
          {"event", "vcu_handshake_status"},
          {"protocol_version", kProtocolVersion},
          {"vehicle_id", config.vehicle_id},
          {"driver_id", signaling.driver_id()},
          {"session_id", signaling.session_id()},
          {"control_status_seq", ++control_status_seq},
          {"sent_at_utc_ms", signaling.now_ms()},
          {"driver_connected", true},
          {"result", result},
          {"adapter_ready", control_service_started && control_service != nullptr},
          {"status", status.to_json()},
          {"hard_limits", std::move(hard_limits)},
      };
      if (!control_service_issue_code.empty()) {
        message["issue_code"] = control_service_issue_code;
      }
      const auto payload = message.dump();
      gst_webrtc_data_channel_send_string(control_channel, payload.c_str());
      if (status.state != last_vcu_handshake_state) {
        emit_diagnostic(
            "vehicle_vcu_handshake_state_changed",
            "vcu_handshake_state_changed",
            "vcu_handshake",
            "",
            status.ready
                ? "No action is required; VCU control authority is ready."
                : "Use the status fields and VCU JSONL gate events to determine the next handshake prerequisite.",
            true,
            {{"from", last_vcu_handshake_state},
             {"to", status.state},
             {"status", status.to_json()}});
        last_vcu_handshake_state = status.state;
      }
    } catch (const std::exception& error) {
      std::cout << Json({
                       {"event", "vehicle_vcu_handshake_status_failed"},
                       {"event_at_utc_ms", signaling.now_ms()},
                       {"vehicle_id", config.vehicle_id},
                       {"session_id", signaling.session_id()},
                       {"error", error.what()},
                   }).dump()
                << '\n';
    }
  }

  void inhibit_control_for_critical_camera(const Lane& lane, std::string_view failure) {
    if (control_inhibited.exchange(true)) return;

    std::optional<std::string> latch_error;
    try {
      static_cast<void>(critical_camera_control_latch->inhibit(signaling.session_id()));
    } catch (const std::exception& error) {
      // The runtime-local latch already blocks commands.  Continue the safe
      // stop even if a programming or lifecycle error prevents the shared
      // session latch from being updated.
      latch_error = error.what();
    }

    std::optional<std::string> close_error;
    GstWebRTCDataChannel* channel_to_close = nullptr;
    {
      std::lock_guard lock(control_mutex);
      control_service_issue_code = "critical_camera_failed";
      if (control_service_started && control_service) {
        try {
          control_service->close();
        } catch (const std::exception& error) {
          close_error = error.what();
        }
      }
      control_service_started = false;
      control_service.reset();
      send_vcu_handshake_status_locked("critical_camera_failed");
      if (control_channel != nullptr) {
        channel_to_close = GST_WEBRTC_DATA_CHANNEL(g_object_ref(control_channel));
      }
    }

    emit_diagnostic(
        "vehicle_control_inhibited_by_camera",
        "critical_camera_control_inhibited",
        "camera_safety",
        std::string(failure),
        "Keep the vehicle stopped. Camera recovery restores video only; end this session and complete a fresh VCU handshake before driving again.",
        false,
        {{"camera_id", lane.camera.id},
         {"device", lane.camera.device},
         {"safety_action", "local_full_stop_control_channel_closed_video_continues"}});
    if (latch_error) {
      emit_diagnostic(
          "vehicle_control_inhibition_latch_failed",
          "critical_camera_control_latch_failed",
          "camera_safety",
          *latch_error,
          "Keep the vehicle stopped, end the current session, and do not resume control until a fresh session and VCU handshake succeed.",
          false,
          {{"camera_id", lane.camera.id},
           {"safety_action", "local_full_stop_physical_estop_if_state_uncertain"}});
    }
    if (close_error) {
      emit_diagnostic(
          "vehicle_vcu_safe_stop_failed",
          "vcu_safe_stop_or_close_failed",
          "camera_safety",
          *close_error,
          "Use the physical emergency stop, keep the vehicle isolated, and inspect the VCU JSONL log/CAN interface.",
          true,
          {{"camera_id", lane.camera.id},
           {"safety_action", "physical_estop_required_video_continues"}});
    }
    if (channel_to_close != nullptr) {
      gst_webrtc_data_channel_close(channel_to_close);
      g_object_unref(channel_to_close);
    }
  }

  [[nodiscard]] bool critical_cameras_ready() const {
    return std::all_of(lanes.begin(), lanes.end(), [](const auto& lane) {
      return !lane->camera.critical_for_control ||
          (!lane->disabled.load() && lane->last_encoded_steady_ms.load() > 0);
    });
  }

  void enforce_critical_camera_freshness() {
    if (!config.runtime.control_enabled || stop_requested || control_inhibited) return;
    const auto now_ms = steady_now_ms();
    for (const auto& lane : lanes) {
      if (!lane->camera.critical_for_control || lane->disabled.load()) continue;
      const auto last_encoded_ms = lane->last_encoded_steady_ms.load();
      const auto freshness_reference_ms = last_encoded_ms > 0
          ? last_encoded_ms
          : lane->pipeline_started_steady_ms;
      if (freshness_reference_ms <= 0 || now_ms - freshness_reference_ms <= frame_timeout_ms) continue;
      if (stop_requested) return;
      inhibit_control_for_critical_camera(
          *lane,
          "critical camera encoded output has been stale for more than " +
              std::to_string(frame_timeout_ms) + " ms");
      return;
    }
  }

  void start_control_when_cameras_ready() {
    if (!config.runtime.control_enabled || control_inhibited || !critical_cameras_ready()) return;
    {
      std::lock_guard lock(control_mutex);
      if (stop_requested || !control_link_open || control_channel == nullptr ||
          control_service_started || control_service_issue_code != "critical_camera_not_ready") {
        return;
      }
    }
    const bool adapter_ready = start_control_service();
    send_vcu_handshake_status(adapter_ready ? "critical_cameras_ready" : "adapter_start_failed");
    if (adapter_ready) return;

    GstWebRTCDataChannel* channel_to_close = nullptr;
    {
      std::lock_guard lock(control_mutex);
      if (control_channel != nullptr) {
        channel_to_close = GST_WEBRTC_DATA_CHANNEL(g_object_ref(control_channel));
      }
    }
    if (channel_to_close != nullptr) {
      gst_webrtc_data_channel_close(channel_to_close);
      g_object_unref(channel_to_close);
    }
  }

  [[nodiscard]] bool start_control_service() {
    if (!config.runtime.control_enabled) return false;
    try {
      {
        std::lock_guard lock(control_mutex);
        if (control_service_started && control_service) return true;
        // The on-open callback can race session teardown.  Recheck lifecycle
        // state while holding the same mutex used by stop_pipeline before
        // opening CAN, otherwise a late callback could resurrect the adapter
        // after the session has already closed.
        if (stop_requested || !control_link_open || control_channel == nullptr) {
          return false;
        }
        if (control_inhibited) {
          control_service_issue_code = "critical_camera_failed";
          return false;
        }
        control_service_issue_code.clear();
        control_service = std::make_unique<VehicleControlService>(
            config,
            signaling.driver_id(),
            signaling.session_id(),
            signaling.control_token(),
            create_vehicle_adapter(config));
        control_service->start(signaling.now_ms());
        // A critical camera can fail while a vendor adapter is synchronously
        // opening.  The latch is set before it waits for this mutex, so check
        // again before publishing the adapter as ready or accepting commands.
        if (stop_requested || control_inhibited) {
          try {
            control_service->close();
          } catch (const std::exception& error) {
            emit_diagnostic(
                "vehicle_vcu_safe_stop_failed",
                "vcu_safe_stop_or_close_failed",
                "camera_safety",
                error.what(),
                "Use the physical emergency stop and keep the vehicle isolated.",
                true,
                {{"safety_action", "physical_estop_required_video_continues"}});
          }
          control_service.reset();
          control_service_issue_code = control_inhibited
              ? "critical_camera_failed"
              : "media_pipeline_failed";
          return false;
        }
        control_service_started = true;
        last_vehicle_telemetry_seq = 0;
      }
      emit_diagnostic(
          "vehicle_vcu_adapter_ready",
          "vcu_adapter_ready",
          "vcu_adapter_start",
          "",
          "No action is required.",
          true,
          {{"adapter_type", config.vehicle_adapter.type},
           {"can_interface", config.vehicle_adapter.can_interface},
           {"can_bitrate", config.hardware.can_bitrate},
           {"can_tx_queue_length", config.hardware.can_tx_queue_length},
           {"bridge_library_path", config.vehicle_adapter.bridge_library_path.string()}});
      return true;
    } catch (const std::exception& error) {
      const std::string start_error = error.what();
      {
        std::lock_guard lock(control_mutex);
        control_service_started = false;
        control_service.reset();
        control_service_issue_code = "vcu_adapter_start_failed";
      }
      emit_diagnostic(
          "vehicle_vcu_adapter_start_failed",
          "vcu_adapter_start_failed",
          "vcu_adapter_start",
          start_error,
          "Check the adapter type, bridge library and dependencies, CAN interface state and bitrate, "
          "configured tx queue length, and VCU log path.",
          true,
          {{"adapter_type", config.vehicle_adapter.type},
           {"can_interface", config.vehicle_adapter.can_interface},
           {"can_bitrate", config.hardware.can_bitrate},
           {"can_tx_queue_length", config.hardware.can_tx_queue_length},
           {"bridge_library_path", config.vehicle_adapter.bridge_library_path.string()},
           {"safety_action", "control_not_started_video_continues"}});
      return false;
    }
  }

  void configure_control_data_channel() {
    if (!config.runtime.control_enabled) return;
    GstStructure* options = gst_structure_new(
        "mine-teleop-control-data-channel",
        "ordered",
        G_TYPE_BOOLEAN,
        FALSE,
        "max-retransmits",
        G_TYPE_INT,
        0,
        "protocol",
        G_TYPE_STRING,
        "mine-teleop-control-v1",
        "negotiated",
        G_TYPE_BOOLEAN,
        FALSE,
        nullptr);
    GstWebRTCDataChannel* channel = nullptr;
    g_signal_emit_by_name(webrtc, "create-data-channel", "control", options, &channel);
    gst_structure_free(options);
    if (channel == nullptr) {
      set_pipeline_error(
          "webrtcbin failed to create the control data channel",
          "control_data_channel_create_failed",
          "webrtc_data_channel",
          "Check GStreamer SCTP/DataChannel plugins and webrtcbin state.",
          false);
      throw std::runtime_error("webrtcbin failed to create the control data channel");
    }
    g_signal_connect(channel, "on-open", G_CALLBACK(on_control_channel_open), this);
    g_signal_connect(channel, "on-close", G_CALLBACK(on_control_channel_close), this);
    g_signal_connect(channel, "on-error", G_CALLBACK(on_control_channel_error), this);
    g_signal_connect(channel, "on-message-string", G_CALLBACK(on_control_message_string), this);
    {
      std::lock_guard lock(control_mutex);
      if (stop_requested) {
        g_signal_handlers_disconnect_by_data(channel, this);
        gst_webrtc_data_channel_close(channel);
        g_object_unref(channel);
        return;
      }
      // Status ordering is scoped to the DataChannel, not the adapter.  A
      // channel may publish driver_connected before critical cameras become
      // ready and then start the adapter later without replacing the channel.
      // Resetting in start_control_service would make that later status replay
      // an already-used sequence number and be rejected by the controller.
      control_status_seq = 0;
      last_control_rejection_status_issue_code.clear();
      last_control_rejection_status_ms.reset();
      control_channel = channel;
    }
  }

  void tick_control_service() {
    std::unique_lock lock(control_mutex);
    if (stop_requested || control_inhibited || !control_service_started || !control_service) return;
    const auto timestamp_ms = signaling.now_ms();
    try {
      control_service->tick(timestamp_ms);
    } catch (const std::exception& error) {
      emit_diagnostic(
          "vehicle_vcu_runtime_failed",
          "vcu_runtime_operation_failed",
          "vcu_control_tick",
          error.what(),
          "Inspect the VCU JSONL log and CAN interface; keep the vehicle stopped.",
          true,
          {{"safety_action", "local_full_stop_control_disabled_video_continues"}});
      try {
        control_service->close();
      } catch (const std::exception& close_error) {
        emit_diagnostic(
            "vehicle_vcu_safe_stop_failed",
            "vcu_safe_stop_or_close_failed",
            "vcu_control_tick",
            close_error.what(),
            "Use the physical emergency stop, keep the vehicle isolated, and inspect the VCU JSONL log/CAN interface.",
            true,
            {{"safety_action", "physical_estop_required_video_continues"}});
      }
      control_service_started = false;
      control_service.reset();
      control_service_issue_code = "vcu_runtime_operation_failed";
      send_vcu_handshake_status_locked("adapter_runtime_failed");
      GstWebRTCDataChannel* channel_to_close = nullptr;
      if (control_channel != nullptr) {
        channel_to_close = GST_WEBRTC_DATA_CHANNEL(g_object_ref(control_channel));
      }
      lock.unlock();
      if (channel_to_close != nullptr) {
        gst_webrtc_data_channel_close(channel_to_close);
        g_object_unref(channel_to_close);
      }
      return;
    }
    if (control_link_open &&
        (!last_vcu_status_ms || timestamp_ms - *last_vcu_status_ms >= 500)) {
      send_vcu_handshake_status_locked("status_update");
      last_vcu_status_ms = timestamp_ms;
    }
    if (control_link_open) send_latest_vehicle_telemetry_locked();
  }

  [[nodiscard]] std::string current_pipeline_error() const {
    std::lock_guard lock(error_mutex);
    return pipeline_error;
  }

  [[nodiscard]] Json current_pipeline_failure() const {
    std::lock_guard lock(error_mutex);
    return {
        {"issue_code", pipeline_issue_code},
        {"stage", pipeline_error_stage},
        {"operator_action", pipeline_operator_action},
        {"retryable", pipeline_error_retryable},
        {"error", pipeline_error},
    };
  }

  [[nodiscard]] std::vector<VideoCodec> negotiate_codecs(int timeout_ms) {
    if (forced_codec.has_value()) return {parse_video_codec(*forced_codec)};
    const auto preferred = parse_video_codec(config.hardware.preferred_codec);
    const auto fallback = parse_video_codec(config.hardware.fallback_codec);
    const auto deadline = signaling.now_ms() + std::max(0, timeout_ms);
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
        if (signaling.now_ms() >= deadline) throw;
        last_negotiation_warning = error.what();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    } while (signaling.now_ms() < deadline);
    return {fallback};
  }

  [[nodiscard]] std::string build_pipeline(const VideoEncoder& encoder) {
    std::ostringstream pipeline_text;
    pipeline_text << "webrtcbin name=webrtc bundle-policy=max-bundle latency=0 ice-transport-policy="
                  << config.cloud.ice_transport_policy << ' ';
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
          << build_camera_input_pipeline("source_" + id, lane->input, lane->profile)
          << "! " << encoder.pipeline_stage(settings, "encoder_" + id) << ' '
          << "! " << parser << " config-interval=-1 "
          << "! " << elementary_caps << ",stream-format=byte-stream,alignment=au "
          << "! tee name=encoded_" << id << ' '
          << "encoded_" << id << ". ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream "
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
        const auto pattern = directory / (std::to_string(signaling.now_ms()) + "_" + lane->camera.id + "_%05d.mp4");
        pipeline_text
            << "encoded_" << id << ". ! queue max-size-buffers="
            << std::max(2, lane->profile.fps * 2)
            << " max-size-bytes=0 max-size-time=0 leaky=downstream "
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

  [[nodiscard]] std::string gstreamer_ice_uri(std::string url) const {
    const auto separator = url.find(':');
    if (separator == std::string::npos) return url;
    auto remainder = url.substr(separator + 1);
    while (remainder.starts_with("//")) remainder.erase(0, 2);
    return url.substr(0, separator) + "://" + remainder;
  }

  [[nodiscard]] std::string gstreamer_turn_uri(
      std::string url,
      std::string_view username,
      std::string_view credential) const {
    const auto separator = url.find(':');
    if (separator == std::string::npos) throw std::invalid_argument("TURN URL has no scheme");
    auto remainder = url.substr(separator + 1);
    while (remainder.starts_with("//")) remainder.erase(0, 2);
    return url.substr(0, separator) + "://" + signaling.url_encode(username) + ":" +
        signaling.url_encode(credential) + "@" + remainder;
  }

  void configure_ice_servers() {
    bool stun_configured = false;
    bool turn_configured = false;
    for (const auto& server : ice_configuration.value("ice_servers", Json::array())) {
      if (!server.is_object()) continue;
      Json urls = server.value("urls", Json::array());
      if (urls.is_string()) urls = Json::array({urls});
      if (!urls.is_array()) continue;
      for (const auto& value : urls) {
        if (!value.is_string()) continue;
        const auto url = value.get<std::string>();
        if (!stun_configured && (url.starts_with("stun:") || url.starts_with("stuns:"))) {
          const auto configured = gstreamer_ice_uri(url);
          g_object_set(webrtc, "stun-server", configured.c_str(), nullptr);
          stun_configured = true;
        }
        if (url.starts_with("turn:") || url.starts_with("turns:")) {
          const auto username = server.value("username", "");
          const auto credential = server.value("credential", "");
          if (username.empty() || credential.empty()) throw std::runtime_error("TURN ICE server lacks credentials");
          const auto configured = gstreamer_turn_uri(url, username, credential);
          if (!turn_configured) {
            g_object_set(webrtc, "turn-server", configured.c_str(), nullptr);
            turn_configured = true;
          } else {
            gboolean added = FALSE;
            g_signal_emit_by_name(webrtc, "add-turn-server", configured.c_str(), &added);
            if (!added) throw std::runtime_error("webrtcbin rejected an additional TURN server");
          }
        }
      }
    }
  }

  void prepare_lanes() {
    lanes.clear();
    for (const auto& camera : config.enabled_cameras()) {
      auto lane = std::make_unique<Lane>();
      lane->owner = this;
      lane->camera = camera;
      lane->profile = config.realtime_profile(camera.realtime_profile);
      lane->input = camera_input_spec(camera, lane->profile);
      lanes.push_back(std::move(lane));
    }
  }

  [[nodiscard]] std::unique_ptr<CameraFrameSource> create_camera_source(const Lane& lane) const {
    MediaProfile capture = lane.profile;
    capture.codec = lane.input.codec;
    capture.width = lane.input.width;
    capture.height = lane.input.height;
    capture.fps = lane.input.fps;
    capture.encoder = "native";
    return std::make_unique<CameraFrameSource>(lane.camera, std::move(capture), frame_timeout_ms);
  }

  [[nodiscard]] bool wait_for_camera_reopen(int backoff_ms) const {
    auto remaining = std::chrono::milliseconds(backoff_ms);
    while (!stop_requested && remaining > std::chrono::milliseconds::zero()) {
      const auto slice = std::min(remaining, std::chrono::milliseconds(50));
      std::this_thread::sleep_for(slice);
      remaining -= slice;
    }
    return !stop_requested;
  }

  bool start_pipeline(const EncoderCandidate& candidate, int capture_interval_ms) {
    active_candidate = candidate;
    {
      std::lock_guard lock(error_mutex);
      pipeline_error.clear();
      pipeline_issue_code.clear();
      pipeline_error_stage.clear();
      pipeline_operator_action.clear();
      pipeline_error_retryable = false;
    }
    stop_requested = false;
    local_ice_candidate_count = 0;
    remote_ice_candidate_count = 0;
    answer_received_at_ms.reset();
    control_not_open_warning_fired = false;
    control_link_opened_this_attempt = false;
    prepare_lanes();
    auto encoder_choice = create_video_encoder(candidate);
    if (encoder_choice->factory_name().empty()) {
      set_pipeline_error(
          to_string(candidate.backend) + " " + to_string(candidate.codec) + " encoder factory is unavailable",
          "encoder_factory_unavailable",
          "encoder_selection",
          "Install/enable the requested hardware encoder or configure a working fallback backend.",
          false);
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
      set_pipeline_error(
          "cannot build GStreamer WebRTC pipeline: " + message,
          "gstreamer_pipeline_build_failed",
          "pipeline_build",
          "Check installed GStreamer plugins and the selected encoder/codec.",
          false);
      return false;
    }
    webrtc = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
    if (webrtc == nullptr) {
      set_pipeline_error(
          "WebRTC pipeline does not contain webrtcbin",
          "gstreamer_webrtcbin_missing",
          "pipeline_build",
          "Install the GStreamer WebRTC plugin and verify the packaged plugin path.",
          false);
      stop_pipeline();
      return false;
    }
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(on_negotiation_needed), this);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(on_ice_candidate), this);
    try {
      configure_ice_servers();
    } catch (const std::exception& error) {
      set_pipeline_error(
          "ICE server configuration failed: " + std::string(error.what()),
          "webrtc_ice_server_config_failed",
          "ice_configuration",
          "Check STUN/TURN URLs, TURN credentials, and the selected ICE transport policy.",
          false);
      stop_pipeline();
      throw;
    }

    // Supported GStreamer versions keep webrtcbin closed while it is in NULL.
    // Creating a DataChannel in that state returns nullptr, so transition the complete
    // pipeline to READY before asking webrtcbin to create the control channel.
    const auto ready_state = gst_element_set_state(pipeline, GST_STATE_READY);
    if (ready_state == GST_STATE_CHANGE_FAILURE) {
      set_pipeline_error(
          "GStreamer WebRTC pipeline failed to enter READY state",
          "gstreamer_ready_state_failed",
          "pipeline_ready",
          "Inspect GStreamer plugin, device, and encoder initialization errors.",
          true);
      stop_pipeline();
      return false;
    }
    configure_control_data_channel();

    for (const auto& lane : lanes) {
      const auto id = pipeline_identifier(lane->camera.id);
      lane->appsrc = gst_bin_get_by_name(GST_BIN(pipeline), ("source_" + id).c_str());
      lane->encoder = gst_bin_get_by_name(GST_BIN(pipeline), ("encoder_" + id).c_str());
      if (lane->appsrc == nullptr || lane->encoder == nullptr) {
        set_pipeline_error(
            "media pipeline lane is incomplete: " + lane->camera.id,
            "gstreamer_camera_lane_incomplete",
            "pipeline_link",
            "Check that the camera ID produces valid GStreamer element names and that appsrc/encoder elements were created.",
            false,
            {{"camera_id", lane->camera.id}, {"device", lane->camera.device}});
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
      set_pipeline_error(
          "GStreamer WebRTC pipeline failed to enter PLAYING state",
          "gstreamer_playing_state_failed",
          "pipeline_playing",
          "Inspect the GStreamer bus, camera availability, and encoder initialization.",
          true);
      stop_pipeline();
      return false;
    }
    started_ms = signaling.now_ms();
    for (const auto& lane : lanes) {
      lane->pipeline_started_ms = started_ms;
      lane->pipeline_started_steady_ms = steady_now_ms();
      lane->thread = std::thread([this, lane = lane.get(), capture_interval_ms] {
        std::uint64_t sequence = 0;
        bool recovery_pending = false;
        const bool pace_test_source =
            classify_camera_source(lane->camera) == CameraSourceKind::TestSource &&
            capture_interval_ms == 0;
        const auto test_source_interval =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)) /
            static_cast<std::int64_t>(std::max(1, lane->profile.fps));
        auto next_test_source_frame = std::chrono::steady_clock::now();
        while (!stop_requested) {
          try {
            if (!lane->source) lane->source = create_camera_source(*lane);
            if (pace_test_source) {
              const auto current = std::chrono::steady_clock::now();
              if (current < next_test_source_frame) {
                std::this_thread::sleep_until(next_test_source_frame);
              } else if (current - next_test_source_frame > test_source_interval) {
                next_test_source_frame = current;
              }
            }
            auto frame = lane->source->next(++sequence);
            if (stop_requested) break;
            if (classify_camera_source(lane->camera) == CameraSourceKind::Ccg2 &&
                (frame.codec != lane->input.codec || frame.width != lane->input.width ||
                 frame.height != lane->input.height)) {
              throw std::runtime_error(
                  "camera frame does not match configured input caps: " + lane->camera.id);
            }
            lane->source_sequence_valid = frame.source_sequence_valid;
            lane->source_sequence = frame.source_sequence;
            lane->source_sequence_gap = frame.source_sequence_gap;
            lane->source_timeperframe_numerator = frame.source_timeperframe_numerator;
            lane->source_timeperframe_denominator = frame.source_timeperframe_denominator;
            if (frame.source_sequence_valid && frame.source_sequence_gap > 0) {
              lane->dropped.fetch_add(frame.source_sequence_gap);
              emit_diagnostic(
                  "vehicle_camera_sequence_gap",
                  "camera_v4l2_sequence_gap",
                  "v4l2_capture",
                  "V4L2 capture sequence skipped one or more buffers.",
                  "Inspect the reported sequence/gap, PCIe link health, camera link status, and the CCG2 driver log.",
                  true,
                  {{"camera_id", lane->camera.id},
                   {"device", lane->camera.device},
                   {"source_sequence", frame.source_sequence},
                   {"source_sequence_gap", frame.source_sequence_gap}});
            }
            frame.captured_at_ms = signaling.from_local_system_ms(frame.captured_at_ms);
            ++lane->captured;
            lane->last_capture_ms = frame.captured_at_ms;
            const bool recovered_this_frame = recovery_pending;
            if (recovered_this_frame) {
              recovery_pending = false;
              {
                std::lock_guard lock(lane->error_mutex);
                lane->error.clear();
              }
              emit_diagnostic(
                  "vehicle_camera_recovered",
                  "camera_lane_recovered",
                  "camera_reopen",
                  "",
                  lane->camera.critical_for_control
                      ? "Video resumed, but vehicle control remains inhibited until a fresh session and VCU handshake."
                      : "No action is required; this optional camera lane resumed without changing vehicle control authority.",
                  true,
                  {{"camera_id", lane->camera.id},
                   {"device", lane->camera.device},
                   {"critical_for_control", lane->camera.critical_for_control},
                   {"reopen_count", lane->reopen_count.load()},
                   {"sequence", sequence}});
            }
            if (!lane->first_frame_reported.exchange(true)) {
              emit_diagnostic(
                  "vehicle_camera_first_frame",
                  "camera_first_frame_received",
                  "camera_capture",
                  "",
                  "No action is required; camera capture is producing frames.",
                  true,
                  {{"camera_id", lane->camera.id},
                   {"device", lane->camera.device},
                   {"source_kind", camera_source_kind_name(classify_camera_source(lane->camera))},
                   {"frame_codec", frame.codec},
                   {"frame_width", frame.width},
                   {"frame_height", frame.height},
                   {"frame_fps", frame.fps},
                   {"payload_bytes", frame.payload.size()},
                   {"source_bytes_per_line", frame.source_bytes_per_line},
                   {"source_size_image", frame.source_size_image},
                   {"source_bytes_used", frame.source_bytes_used},
                   {"source_sequence_valid", frame.source_sequence_valid},
                   {"source_sequence", frame.source_sequence},
                   {"source_sequence_gap", frame.source_sequence_gap},
                   {"source_timeperframe_numerator", frame.source_timeperframe_numerator},
                   {"source_timeperframe_denominator", frame.source_timeperframe_denominator},
                   {"sequence", sequence}});
            }
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.payload.size(), nullptr);
            if (buffer == nullptr) throw std::runtime_error("cannot allocate GStreamer camera buffer");
            gst_buffer_fill(buffer, 0, frame.payload.data(), frame.payload.size());
            const auto elapsed_ms = std::max<std::int64_t>(0, frame.captured_at_ms - started_ms);
            GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(elapsed_ms) * GST_MSECOND;
            GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DURATION(buffer) = GST_SECOND / static_cast<GstClockTime>(std::max(1, lane->input.fps));
            if (recovered_this_frame) {
              GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
            }
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
          } catch (const std::exception& error) {
            if (stop_requested) break;
            const std::string failure = error.what();
            const auto issue = classify_camera_issue(failure);
            const bool gstreamer_failure =
                failure.starts_with("cannot allocate GStreamer camera buffer") ||
                failure.starts_with("GStreamer appsrc rejected camera frame");
            if (gstreamer_failure) {
              set_pipeline_error(
                  "camera lane " + lane->camera.id + ": " + failure,
                  std::string(issue.code),
                  std::string(issue.stage),
                  std::string(issue.action),
                  issue.retryable,
                  {{"camera_id", lane->camera.id},
                   {"device", lane->camera.device},
                   {"failure_scope", "gstreamer_pipeline"}});
              break;
            }
            const auto failure_count = ++lane->failure_count;
            const auto decision = camera_failure_decision(lane->camera, failure_count, issue.retryable);
            // For a critical lane, latch and close vehicle control before any
            // potentially blocking diagnostic output or device/process cleanup.
            if (decision.inhibit_control) {
              inhibit_control_for_critical_camera(*lane, failure);
            }
            {
              std::lock_guard lock(lane->error_mutex);
              lane->error = failure;
            }
            emit_camera_failure(*lane, failure, failure_count, decision);
            lane->source.reset();

            if (decision.lane_action == CameraFailureAction::DisableLane) {
              lane->disabled = true;
              if (lane->appsrc != nullptr) {
                static_cast<void>(gst_app_src_end_of_stream(GST_APP_SRC(lane->appsrc)));
              }
              emit_diagnostic(
                  "vehicle_camera_lane_disabled",
                  "camera_reopen_exhausted",
                  "camera_reopen",
                  failure,
                  lane->camera.critical_for_control
                      ? "Keep the vehicle stopped, repair the critical camera, then end this session and complete a fresh VCU handshake."
                      : "Repair the optional camera before the next session; the other camera lanes and current control authority remain active.",
                  false,
                  {{"camera_id", lane->camera.id},
                   {"device", lane->camera.device},
                   {"critical_for_control", lane->camera.critical_for_control},
                   {"failure_count", failure_count},
                   {"reopen_count", lane->reopen_count.load()},
                   {"safety_action", lane->camera.critical_for_control
                                          ? "control_inhibited_disable_failed_camera_lane_video_continues"
                                          : "disable_noncritical_camera_lane_only"}});
              break;
            }

            const auto reopen_count = ++lane->reopen_count;
            recovery_pending = true;
            emit_diagnostic(
                "vehicle_camera_reopen_scheduled",
                "camera_lane_reopen_scheduled",
                "camera_reopen",
                failure,
                lane->camera.critical_for_control
                    ? "Control remains inhibited while the runtime reopens only this camera source; recovery does not restore driving authority."
                    : "The runtime will reopen only this noncritical camera source after the bounded backoff.",
                true,
                {{"camera_id", lane->camera.id},
                 {"device", lane->camera.device},
                 {"critical_for_control", lane->camera.critical_for_control},
                 {"reopen_count", reopen_count},
                 {"reopen_attempts", lane->camera.reopen_attempts},
                 {"retry_after_ms", lane->camera.reopen_backoff_ms},
                 {"safety_action", lane->camera.critical_for_control
                                        ? "local_full_stop_reopen_camera_lane_video_continues"
                                        : "reopen_noncritical_camera_lane_only"}});
            if (!wait_for_camera_reopen(lane->camera.reopen_backoff_ms)) break;
            next_test_source_frame = std::chrono::steady_clock::now();
          }
        }
      });
    }
    return true;
  }

  void stop_pipeline() {
    stop_requested = true;
    GstWebRTCDataChannel* channel_to_close = nullptr;
    {
      std::lock_guard lock(control_mutex);
      channel_to_close = std::exchange(control_channel, nullptr);
      control_link_open = false;
      if (control_service_started && control_service) {
        try {
          control_service->close();
        } catch (const std::exception& error) {
          emit_diagnostic(
              "vehicle_vcu_safe_stop_failed",
              "vcu_safe_stop_or_close_failed",
              "vcu_adapter_close",
              error.what(),
              "Keep the vehicle isolated and inspect the VCU JSONL log/CAN interface before restart.",
              true,
              {{"safety_action", "local_full_stop_requested"}});
        }
      }
      control_service_started = false;
      control_service.reset();
    }
    if (channel_to_close != nullptr) {
      g_signal_handlers_disconnect_by_data(channel_to_close, this);
      gst_webrtc_data_channel_close(channel_to_close);
      g_object_unref(channel_to_close);
    }
    // Let capture threads observe stop_requested and finish before EOS.  A
    // source may return its last frame while teardown is in progress; joining
    // first prevents a push-after-EOS race on appsrc.
    for (const auto& lane : lanes) {
      if (lane->thread.joinable()) lane->thread.join();
      lane->source.reset();
    }
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
    try {
      write_recording_sidecars();
    } catch (const std::exception& error) {
      emit_diagnostic(
          "vehicle_recording_sidecar_failed",
          "recording_sidecar_write_failed",
          "recording_finalize",
          error.what(),
          "Check recording directory permissions, free space, and filesystem health.",
          true);
      throw;
    }
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
      const auto timestamp = signaling.now_ms();
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
          set_pipeline_error(
              "driver returned an invalid WebRTC answer SDP",
              "webrtc_answer_sdp_invalid",
              "webrtc_answer",
              "Inspect the controller SDP and ensure browser/server codec negotiation matches the vehicle offer.",
              false);
          return;
        }
        auto* answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        GstPromise* promise = gst_promise_new();
        g_signal_emit_by_name(webrtc, "set-remote-description", answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);
        answer_received = true;
        answer_received_at_ms = signaling.now_ms();
        emit_diagnostic(
            "vehicle_webrtc_answer_applied",
            "webrtc_answer_applied",
            "webrtc_answer",
            "",
            "No action is required; wait for ICE/DTLS connection and video frames.",
            true,
            {{"codec", to_string(active_candidate.codec)},
             {"backend", to_string(active_candidate.backend)}});
      } else if (type == "ice_candidate") {
        const auto candidate = payload.value("candidate", "");
        const auto index = payload.value("sdpMLineIndex", 0U);
        if (!candidate.empty()) {
          g_signal_emit_by_name(webrtc, "add-ice-candidate", index, candidate.c_str());
          const auto count = ++remote_ice_candidate_count;
          if (count == 1) {
            emit_diagnostic(
                "vehicle_webrtc_remote_ice_candidate_received",
                "webrtc_remote_ice_candidate_available",
                "ice_connectivity",
                "",
                "No action is required; this is a connectivity milestone.",
                true,
                {{"candidate_count", count}, {"sdp_mline_index", index}});
          }
        }
      } else if (type == "media_fallback" && active_candidate.codec == VideoCodec::H265) {
        codec_fallback_requested = true;
        set_pipeline_error(
            "browser requested H.264 fallback: " + payload.value("reason", "decode failure"),
            "browser_codec_fallback_requested",
            "browser_decode",
            "Verify browser codec support; the runtime will retry with H.264.",
            true);
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
        set_pipeline_error(
            std::move(value),
            "gstreamer_bus_error",
            "pipeline_runtime",
            "Inspect the GStreamer error/debug text and the affected camera or encoder.",
            true);
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
      // A lane that exhausted its bounded reopen budget cannot contribute any
      // more frames.  Let finite diagnostic runs terminate; a disabled
      // critical lane still fails acceptance through control_inhibited.
      if (lane->disabled.load()) return true;
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
      const auto appsrc_queued_buffers = lane->appsrc == nullptr
          ? guint64{0}
          : gst_app_src_get_current_level_buffers(GST_APP_SRC(lane->appsrc));
      result.push_back({
          {"camera_id", lane->camera.id},
          {"critical_for_control", lane->camera.critical_for_control},
          {"lane_state", lane->disabled.load() ? "disabled" : (error.empty() ? "active" : "recovering")},
          {"captured_frames", lane->captured.load()},
          {"pushed_frames", lane->pushed.load()},
          {"encoded_frames", lane->encoded.load()},
          {"dropped_frames", lane->dropped.load()},
          {"pipeline_backlog_or_drop_frames", lane->pushed.load() > lane->encoded.load() ? lane->pushed.load() - lane->encoded.load() : 0},
          {"appsrc_queued_buffers", appsrc_queued_buffers},
          {"appsrc_queue_limit_buffers", kCameraAppSrcMaxBuffers},
          {"failure_count", lane->failure_count.load()},
          {"reopen_count", lane->reopen_count.load()},
          {"encoded_fps", lane->encoded.load() * 1000.0 / static_cast<double>(std::max<std::int64_t>(1, elapsed_ms))},
          {"capture_to_encoded_ms", lane->encode_latency_samples.load() == 0
                                           ? 0.0
                                           : static_cast<double>(lane->encode_latency_total_ms.load()) /
                                                 static_cast<double>(lane->encode_latency_samples.load())},
          {"capture_to_encoded_max_ms", lane->encode_latency_max_ms.load()},
          {"width", lane->profile.width},
          {"height", lane->profile.height},
          {"target_fps", lane->profile.fps},
          {"capture_codec", lane->input.codec},
          {"capture_width", lane->input.width},
          {"capture_height", lane->input.height},
          {"capture_fps", lane->input.fps},
          {"source_sequence_valid", lane->source_sequence_valid.load()},
          {"source_sequence", lane->source_sequence.load()},
          {"source_sequence_gap", lane->source_sequence_gap.load()},
          {"source_timeperframe_numerator", lane->source_timeperframe_numerator.load()},
          {"source_timeperframe_denominator", lane->source_timeperframe_denominator.load()},
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
    TimeSyncStatus initial_time_sync;
    try {
      initial_time_sync = signaling.synchronize_time(config.field_safety.time_sync_samples);
    } catch (const std::exception& error) {
      emit_diagnostic(
          "vehicle_media_time_sync_failed",
          "media_initial_time_sync_failed",
          "time_sync",
          error.what(),
          "Check signaling-server reachability and system clock/network latency.",
          true);
      throw;
    }
    if (config.field_safety.require_time_sync &&
        !initial_time_sync.acceptable(config.field_safety.max_time_sync_uncertainty_ms)) {
      emit_diagnostic(
          "vehicle_media_time_sync_failed",
          "media_time_sync_uncertainty_exceeded",
          "time_sync",
          "uncertainty exceeds configured field-safety limit",
          "Stabilize network time synchronization before enabling teleoperation.",
          true,
          {{"uncertainty_ms", initial_time_sync.uncertainty_ms},
           {"limit_ms", config.field_safety.max_time_sync_uncertainty_ms}});
      throw std::runtime_error(
          "media time synchronization uncertainty " + std::to_string(initial_time_sync.uncertainty_ms) +
          "ms exceeds limit " + std::to_string(config.field_safety.max_time_sync_uncertainty_ms) + "ms");
    }
    try {
      signaling.register_online();
    } catch (const std::exception& error) {
      emit_diagnostic(
          "vehicle_media_signaling_failed",
          "vehicle_signaling_registration_failed",
          "signaling_register",
          error.what(),
          "Check the signaling URL, TLS trust, device token, DNS/resolve overrides, and server health.",
          true);
      throw;
    }
    std::cout << Json({
                     {"event", "vehicle_media_waiting_for_session"},
                     {"vehicle_id", config.vehicle_id},
                     {"poll_interval_ms", config.runtime.teleop_poll_interval_ms},
                 }).dump()
              << std::endl;
    const auto session_deadline = signaling.now_ms() + 5000;
    while (true) {
      bool discovered = false;
      try {
        discovered = signaling.discover_session();
      } catch (const std::exception& error) {
        emit_diagnostic(
            "vehicle_media_signaling_failed",
            "vehicle_session_discovery_failed",
            "session_discovery",
            error.what(),
            "Check signaling-server health and whether this vehicle connection generation is current.",
            true);
        throw;
      }
      if (discovered) break;
      if (!continuous && signaling.now_ms() >= session_deadline) {
        emit_diagnostic(
            "vehicle_media_session_wait_failed",
            "active_driver_session_timeout",
            "session_discovery",
            "timed out waiting for an active driver session",
            "Log in on the controller and request control for this vehicle ID.",
            true);
        throw std::runtime_error("timed out waiting for an active driver session");
      }
      std::this_thread::sleep_for(
          std::chrono::milliseconds(config.runtime.teleop_poll_interval_ms));
    }
    const bool inherited_control_inhibition =
        critical_camera_control_latch->enter_session(signaling.session_id());
    control_inhibited.store(inherited_control_inhibition);
    if (inherited_control_inhibition) {
      emit_diagnostic(
          "vehicle_control_inhibition_retained",
          "critical_camera_control_inhibition_retained",
          "camera_safety",
          "a critical-camera fault already inhibited control for this active session",
          "Keep the vehicle stopped. End this session, start a new session, and complete a fresh VCU handshake before driving again.",
          false,
          {{"safety_action", "control_disabled_video_may_continue"}});
    }
    try {
      ice_configuration = signaling.ice_servers();
    } catch (const std::exception& error) {
      emit_diagnostic(
          "vehicle_media_signaling_failed",
          "ice_server_fetch_failed",
          "ice_server_fetch",
          error.what(),
          "Check signaling-server/TURN configuration and session authorization.",
          true);
      throw;
    }
    std::vector<VideoCodec> codecs;
    try {
      codecs = negotiate_codecs(3000);
    } catch (const std::exception& error) {
      emit_diagnostic(
          "vehicle_media_negotiation_failed",
          "media_codec_negotiation_failed",
          "codec_negotiation",
          error.what(),
          "Check browser-advertised codecs and vehicle preferred/fallback codec configuration.",
          false);
      throw;
    }
    std::vector<EncoderCandidate> candidates;
    for (const auto codec : codecs) {
      const auto codec_candidates = encoder_candidate_order(config.hardware, codec);
      candidates.insert(candidates.end(), codec_candidates.begin(), codec_candidates.end());
    }
    if (candidates.empty()) {
      emit_diagnostic(
          "vehicle_media_negotiation_failed",
          "video_encoder_candidates_empty",
          "encoder_selection",
          "no video encoder candidates are configured",
          "Configure at least one supported codec and hardware encoder backend.",
          false);
      throw std::runtime_error("no video encoder candidates are configured");
    }
    Json attempts = Json::array();
    Json errors = Json::array();
    Json final_lanes = Json::array();
    std::int64_t total_started_ms = signaling.now_ms();
    EncoderCandidate successful = candidates.front();

    for (std::size_t candidate_index = 0; candidate_index < candidates.size(); ++candidate_index) {
      answer_received = false;
      simulated_failure_fired = false;
      codec_fallback_requested = false;
      const auto candidate = candidates[candidate_index];
      const auto attempt_started = signaling.now_ms();
      if (!start_pipeline(candidate, capture_interval_ms)) {
        const auto error = current_pipeline_error();
        attempts.push_back({
            {"backend", to_string(candidate.backend)},
            {"codec", to_string(candidate.codec)},
            {"passed", false},
            {"failure", current_pipeline_failure()},
            {"error", error}});
        errors.push_back(error);
        stop_pipeline();
        if (candidate_index + 1 < candidates.size()) ++failover_count;
        continue;
      }
      const auto deadline = duration_ms > 0 ? total_started_ms + duration_ms : std::numeric_limits<std::int64_t>::max();
      auto next_media_status_ms = signaling.now_ms();
      while (!frame_target_reached(frame_count) && (continuous || signaling.now_ms() < deadline)) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        try {
          flush_outgoing_signals();
          process_signaling();
        } catch (const std::exception& error) {
          emit_diagnostic(
              "vehicle_media_signaling_failed",
              "session_signaling_exchange_failed",
              "session_signaling",
              error.what(),
              "Check signaling-server reachability, session state, and connection generation.",
              true,
              {{"safety_action", "local_full_stop"}});
          throw;
        }
        poll_bus();
        if (!current_pipeline_error().empty()) break;
        enforce_critical_camera_freshness();
        start_control_when_cameras_ready();
        tick_control_service();
        if (signaling.time_sync_refresh_due(config.field_safety.time_sync_interval_ms)) {
          try {
            const auto status = signaling.synchronize_time(config.field_safety.time_sync_samples);
            if (config.field_safety.require_time_sync &&
                !status.acceptable(config.field_safety.max_time_sync_uncertainty_ms)) {
              set_pipeline_error(
                  "media time synchronization uncertainty exceeds " +
                      std::to_string(config.field_safety.max_time_sync_uncertainty_ms) + "ms",
                  "media_runtime_time_sync_uncertainty_exceeded",
                  "time_sync_refresh",
                  "Stabilize network time synchronization before resuming teleoperation.",
                  true);
            }
          } catch (const std::exception& error) {
            if (config.field_safety.require_time_sync) {
              set_pipeline_error(
                  "media time synchronization failed: " + std::string(error.what()),
                  "media_runtime_time_sync_failed",
                  "time_sync_refresh",
                  "Check signaling-server reachability and network latency before resuming.",
                  true);
            }
          }
        }
        if (signaling.now_ms() >= next_media_status_ms) {
          queue_signal(
              "media_status",
              {{"codec", to_string(candidate.codec)},
               {"backend", to_string(candidate.backend)},
               {"time_sync", signaling.time_sync_status().to_json()},
               {"lanes", lane_metrics(std::max<std::int64_t>(1, signaling.now_ms() - attempt_started))}});
          next_media_status_ms = signaling.now_ms() + 1000;
        }
        if (simulate_primary_failure_after_frames > 0 && candidate_index == 0 && !simulated_failure_fired &&
            total_encoded() >= static_cast<std::uint64_t>(simulate_primary_failure_after_frames)) {
          simulated_failure_fired = true;
          set_pipeline_error(
              "simulated primary encoder failure",
              "simulated_encoder_failure",
              "encoder_runtime",
              "No operator action is required in a deliberate failover test.",
              true);
        }
        if (config.runtime.control_enabled && answer_received_at_ms &&
            !control_link_open && !control_link_opened_this_attempt &&
            !control_not_open_warning_fired &&
            signaling.now_ms() - *answer_received_at_ms >= 5000) {
          control_not_open_warning_fired = true;
          emit_diagnostic(
              "vehicle_control_data_channel_not_ready",
              "control_data_channel_open_timeout",
              "webrtc_data_channel",
              "control DataChannel did not open within 5000 ms after the WebRTC answer",
              "Check ICE/DTLS connectivity, TURN reachability, browser console errors, and SCTP plugins.",
              true,
              {{"timeout_ms", 5000},
               {"local_ice_candidates", local_ice_candidate_count.load()},
               {"remote_ice_candidates", remote_ice_candidate_count.load()},
               {"safety_action", "local_full_stop"}});
        }
        if (!current_pipeline_error().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      flush_outgoing_signals();
      const auto elapsed = std::max<std::int64_t>(1, signaling.now_ms() - attempt_started);
      final_lanes = lane_metrics(elapsed);
      if (current_pipeline_error().empty() && !answer_received) {
        set_pipeline_error(
            "controller WebRTC answer was not received",
            "webrtc_answer_missing",
            "webrtc_answer",
            "Check controller login/session state, signaling messages, and browser WebRTC offer handling.",
            true);
      }
      if (current_pipeline_error().empty() && total_encoded() == 0) {
        set_pipeline_error(
            "no encoded video frames were produced",
            "video_frames_not_encoded",
            "encoder_runtime",
            "Inspect camera capture counters, GStreamer bus errors, and encoder availability.",
            true);
      }
      const auto error = current_pipeline_error();
      const bool passed = error.empty() && answer_received && total_encoded() > 0 && !control_inhibited;
      attempts.push_back({
          {"backend", to_string(candidate.backend)},
          {"codec", to_string(candidate.codec)},
          {"passed", passed},
          {"answer_received", answer_received},
          {"duration_ms", elapsed},
          {"lanes", final_lanes},
          {"failure", current_pipeline_failure()},
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

    const auto total_elapsed = std::max<std::int64_t>(1, signaling.now_ms() - total_started_ms);
    bool fps_passed = !final_lanes.empty();
    for (const auto& lane : final_lanes) {
      const auto encoded_fps = lane.value("encoded_fps", 0.0);
      if (encoded_fps < config.hardware.min_realtime_fps) {
        if (lane.value("critical_for_control", true)) fps_passed = false;
        emit_diagnostic(
            "vehicle_camera_performance_failed",
            "camera_encoded_fps_below_minimum",
            "media_acceptance",
            "encoded FPS is below the configured minimum",
            "Check USB bandwidth, capture mode, encoder load, and pipeline backlog.",
            true,
            {{"camera_id", lane.value("camera_id", "")},
             {"encoded_fps", encoded_fps},
             {"minimum_fps", config.hardware.min_realtime_fps},
             {"captured_frames", lane.value("captured_frames", 0)},
             {"encoded_frames", lane.value("encoded_frames", 0)}});
      }
      const auto max_latency_ms = lane.value("capture_to_encoded_max_ms", 0);
      if (max_latency_ms > config.hardware.max_end_to_end_latency_ms) {
        emit_diagnostic(
            "vehicle_camera_performance_warning",
            "camera_capture_to_encode_latency_high",
            "media_acceptance",
            "capture-to-encode latency exceeds the configured end-to-end budget",
            "Check encoder saturation, CPU/GPU load, and pipeline backlog.",
            true,
            {{"camera_id", lane.value("camera_id", "")},
             {"capture_to_encoded_max_ms", max_latency_ms},
             {"max_end_to_end_latency_ms", config.hardware.max_end_to_end_latency_ms}});
      }
    }
    const bool passed = !attempts.empty() && attempts.back().value("passed", false) &&
        fps_passed && !control_inhibited;
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
        {"time_sync", signaling.time_sync_status().to_json()},
        {"fps_passed", fps_passed},
        {"control_inhibited", control_inhibited.load()},
        {"recording_enabled", !recording_root.empty()},
        {"recording_root", recording_root.string()},
        {"failover_count", failover_count},
        {"attempts", std::move(attempts)},
        {"errors", std::move(errors)},
        {"negotiation_warning", last_negotiation_warning},
        {"control_data_channel", {
             {"configured", config.runtime.control_enabled},
             {"ordered", false},
             {"max_retransmits", 0},
             {"ever_opened", control_link_ever_opened.load()},
             {"accepted_commands", accepted_control_commands.load()},
             {"rejected_commands", rejected_control_commands.load()},
             {"link_loss_count", control_link_loss_count.load()},
             {"last_received_at_utc_ms", last_control_received_at_ms.load()},
         }},
    };
  }

  VehicleConfig config;
  MediaSignalingClient signaling;
  std::shared_ptr<CriticalCameraControlLatch> critical_camera_control_latch;
  int frame_timeout_ms;
  std::filesystem::path recording_root;
  std::optional<std::string> forced_codec;
  int simulate_primary_failure_after_frames;
  GstElement* pipeline{nullptr};
  GstElement* webrtc{nullptr};
  GstWebRTCDataChannel* control_channel{nullptr};
  std::vector<std::unique_ptr<Lane>> lanes;
  std::atomic<bool> stop_requested{false};
  std::mutex signal_mutex;
  std::deque<std::pair<std::string, Json>> pending_signals;
  mutable std::mutex error_mutex;
  std::string pipeline_error;
  std::string pipeline_issue_code;
  std::string pipeline_error_stage;
  std::string pipeline_operator_action;
  bool pipeline_error_retryable{false};
  mutable std::mutex diagnostic_mutex;
  EncoderCandidate active_candidate{EncoderBackend::Nvenc, VideoCodec::H265};
  std::int64_t started_ms{0};
  bool answer_received{false};
  std::optional<std::int64_t> answer_received_at_ms;
  bool control_not_open_warning_fired{false};
  bool simulated_failure_fired{false};
  bool codec_fallback_requested{false};
  std::uint64_t failover_count{0};
  std::string last_negotiation_warning;
  Json ice_configuration{Json::object()};
  std::mutex control_mutex;
  std::unique_ptr<VehicleControlService> control_service;
  bool control_service_started{false};
  std::string control_service_issue_code;
  std::atomic<bool> control_link_open{false};
  std::atomic<bool> control_inhibited{false};
  std::atomic<bool> control_link_ever_opened{false};
  std::atomic<bool> control_link_opened_this_attempt{false};
  std::atomic<std::uint64_t> accepted_control_commands{0};
  std::atomic<std::uint64_t> rejected_control_commands{0};
  std::atomic<std::uint64_t> control_link_loss_count{0};
  std::atomic<std::int64_t> last_control_received_at_ms{0};
  std::optional<std::int64_t> last_vcu_status_ms;
  std::uint64_t last_vehicle_telemetry_seq{0};
  std::uint64_t control_status_seq{0};
  std::string last_vcu_handshake_state;
  std::string last_control_rejection_reason;
  std::optional<std::int64_t> last_control_rejection_log_ms;
  std::string last_control_rejection_status_issue_code;
  std::optional<std::int64_t> last_control_rejection_status_ms;
  std::atomic<std::uint64_t> local_ice_candidate_count{0};
  std::atomic<std::uint64_t> remote_ice_candidate_count{0};
};

VehicleMediaRuntime::VehicleMediaRuntime(
    VehicleConfig config,
    std::string signaling_url,
    std::string device_token,
    int frame_timeout_ms,
    std::filesystem::path recording_root,
    std::optional<std::string> forced_codec,
    int simulate_primary_failure_after_frames,
    std::string connection_id,
    std::shared_ptr<MediaSignalingSequence> signaling_sequence,
    std::shared_ptr<CriticalCameraControlLatch> critical_camera_control_latch)
    : impl_(std::make_unique<Impl>(
          std::move(config),
          std::move(signaling_url),
          std::move(device_token),
          frame_timeout_ms,
          std::move(recording_root),
          std::move(forced_codec),
          simulate_primary_failure_after_frames,
          std::move(connection_id),
          std::move(signaling_sequence),
          std::move(critical_camera_control_latch))) {}

VehicleMediaRuntime::~VehicleMediaRuntime() = default;

Json VehicleMediaRuntime::run(int frame_count, int duration_ms, int capture_interval_ms) {
  return impl_->run(frame_count, duration_ms, capture_interval_ms);
}

}  // namespace mine_teleop
