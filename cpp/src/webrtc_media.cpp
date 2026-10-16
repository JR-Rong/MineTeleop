#include "mine_teleop/media.hpp"
#include "mine_teleop/detail/control_transport.hpp"
#include "mine_teleop/detail/direct_authority.hpp"
#include "mine_teleop/detail/emergency_stop.hpp"
#include "mine_teleop/detail/feedback_transport.hpp"

#include "mine_teleop/gstreamer_media.hpp"
#include "mine_teleop/detail/gstreamer_freshness.hpp"
#include "mine_teleop/detail/recording_pipeline.hpp"
#include "mine_teleop/recording.hpp"
#include "mine_teleop/server.hpp"
#include "mine_teleop/upload.hpp"
#include "mine_teleop/video.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <syncstream>
#include <thread>
#include <utility>
#include <vector>
#include <unistd.h>

namespace mine_teleop {

std::optional<std::uint64_t> encoded_frame_latency_ms(
    const GstSegment& segment, GstClockTime pts, std::int64_t elapsed_ms) {
  return detail::encoded_running_time_age_ms(segment, pts, elapsed_ms);
}

void isolate_bundled_rtp_lane_eos(GstPad* rtp_src_pad, std::atomic<bool>& pipeline_stopping) {
  if (rtp_src_pad == nullptr) throw std::invalid_argument("camera RTP source pad is missing");
  gst_pad_add_probe(
      rtp_src_pad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
      +[](GstPad*, GstPadProbeInfo* info, gpointer data) {
        const auto* stopping = static_cast<const std::atomic<bool>*>(data);
        // A failed camera may finalize its encoder/recording, but its EOS must
        // not end the shared RTP session used by the other bundled cameras.
        if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_EOS && !stopping->load()) {
          return GST_PAD_PROBE_DROP;
        }
        return GST_PAD_PROBE_OK;
      },
      &pipeline_stopping, nullptr);
}

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

bool camera_encoded_frame_fresh(std::int64_t last_encoded_ms, std::int64_t now_ms, int timeout_ms) {
  return last_encoded_ms > 0 && now_ms >= last_encoded_ms &&
      now_ms - last_encoded_ms <= timeout_ms;
}

NeutralHeartbeatGate neutral_heartbeat_gate(
    SafetyState state, bool braking_wait, std::int64_t native_gap_ms, int degraded_timeout_ms) {
  if (state != SafetyState::ControlActive && !(braking_wait && state == SafetyState::Degraded))
    return NeutralHeartbeatGate::InactiveState;
  if (native_gap_ms < 0 || (!braking_wait && native_gap_ms >= degraded_timeout_ms))
    return NeutralHeartbeatGate::NativeGap;
  // Only authenticated, age-checked zero packets in the retained fresh gear
  // reach this gate. During braking wait, real heartbeats can maintain braking
  // even after degradation; receive_command still enforces the hard watchdog.
  return NeutralHeartbeatGate::Accepted;
}

namespace {

constexpr auto kNativeControlWatchdogInterval = std::chrono::milliseconds(50);
constexpr auto kNativeControlWebSocketConnectTimeout = std::chrono::milliseconds(1000);
constexpr auto kNativeControlWebSocketReceiveTimeout = std::chrono::milliseconds(100);
constexpr auto kNativeControlWebSocketSendTimeout = std::chrono::milliseconds(20);
constexpr auto kNativeControlReconnectInitialDelay = std::chrono::milliseconds(100);
constexpr auto kNativeControlReconnectMaximumDelay = std::chrono::milliseconds(1000);
constexpr auto kNativeControlDiagnosticInterval = std::chrono::milliseconds(5000);

struct NativeControlDeliveryTraceContext {
  std::uint64_t delivery_cursor{0};
  std::int64_t cloud_queued_at_utc_ms{0};
  std::int64_t envelope_received_at_utc_ms{0};
  std::int64_t envelope_received_monotonic_ms{0};
  std::size_t envelope_message_count{0};
  std::size_t valid_message_count{0};
  std::size_t superseded_message_count{0};
};

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
        http_(std::chrono::seconds(5), resolve_entries, ca_bundle),
        time_sync_http_(std::chrono::milliseconds(500), resolve_entries, ca_bundle),
        direct_resolve_entries_(resolve_entries), direct_ca_bundle_(ca_bundle),
        sequence_(sequence ? std::move(sequence) : std::make_shared<MediaSignalingSequence>()) {
    if (vehicle_id_.empty() || device_token_.empty()) throw std::invalid_argument("vehicle id and device token are required");
    if (connection_id_.empty()) {
      connection_id_ = "vehicle-media-" + vehicle_id_ + "-" +
          std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    }
  }

  void register_online() {
    const auto started_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto response = http_.post_json_response(
        origin_ + "/vehicles/online",
        {{"vehicle_id", vehicle_id_},
         {"device_token", device_token_},
         {"connection_id", connection_id_},
         {"native_control_capabilities", kNativeControlCapabilities}});
    connection_generation_ = response.at("connection_generation").get<std::uint64_t>();
    // No I/O here; the owner emits this through its bounded trace worker.
    const auto receipt_field = [&](std::string_view field) -> std::string {
      const auto found = response.find(std::string(field));
      if (found == response.end() || !found->is_string()) return {};
      const auto value = found->get<std::string>();
      const auto hex = [](std::string_view text) {
        return std::all_of(text.begin(), text.end(), [](char character) {
          return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        });
      };
      if (field == "connection_fingerprint") return value.size() == 32 && hex(value) ? value : std::string{};
      if (field == "registration_request_id") return value.starts_with("request-") && value.size() == 32 &&
          hex(std::string_view(value).substr(8)) ? value : std::string{};
      return value == "same_connection_refresh" || value == "capabilities_changed_new_generation" ||
          value == "replace_previous_connection" || value == "new_connection" ? value : std::string{};
    };
    try { registration_diagnostics_ = Json({{"stage", "vehicle_registration_completed"},
        {"vehicle_id", vehicle_id_}, {"process_id", ::getpid()},
        {"media_client_instance_id", runtime_instance_id_}, {"media_client_created_monotonic_ms", runtime_started_ms_},
        {"registration_started_monotonic_ms", started_ms},
        {"registration_completed_monotonic_ms", std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count()},
        {"registration_request_id", receipt_field("registration_request_id")},
        {"connection_fingerprint", receipt_field("connection_fingerprint")},
        {"connection_generation", connection_generation_},
        {"duplicate_policy", receipt_field("duplicate_policy")}});
    } catch (...) { registration_diagnostics_ = Json::object(); }
  }

  [[nodiscard]] const Json& registration_diagnostics() const { return registration_diagnostics_; }

  TimeSyncStatus synchronize_time(int sample_count, int max_uncertainty_ms) {
    return clock_.synchronize(time_sync_http_, origin_, sample_count, max_uncertainty_ms);
  }
  void mark_time_sync_failed() { clock_.mark_refresh_failed(); }

  [[nodiscard]] TimeSyncStatus time_sync_status() const { return clock_.status(); }
  [[nodiscard]] bool time_sync_refresh_due(int interval_ms) const { return clock_.refresh_due(interval_ms); }
  [[nodiscard]] std::int64_t now_ms() const { return clock_.now_ms(); }
  [[nodiscard]] std::int64_t from_local_system_ms(std::int64_t value) const {
    return clock_.from_local_system_ms(value);
  }
  [[nodiscard]] std::int64_t from_steady_time(std::chrono::steady_clock::time_point value) const {
    return clock_.from_steady_time(value);
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
    transport_mac_key_ = response.value("transport_mac_key", "");
    direct_authority_configuration_ = response.value("direct_authority_configuration", Json(nullptr));
    control_lease_expires_at_ms_.store(response.value("control_token_expires_at_utc_ms", std::int64_t{0}));
    return !session_id_.empty() && !driver_id_.empty() && !control_token_.empty();
  }

  Json poll(std::string_view types) {
    require_session();
    return (direct_session_http_ ? *direct_session_http_ : http_).get_json(
        origin_ + "/signaling/" + http_.url_encode(session_id_) + "/messages?recipient=" +
            http_.url_encode(vehicle_id_) + "&connection_generation=" + std::to_string(connection_generation_) +
            "&types=" + http_.url_encode(types),
        {{"X-Mine-Teleop-Device-Token", device_token_}});
  }

  [[nodiscard]] std::string native_control_websocket_url() const {
    require_session();
    return signaling_websocket_url(
               origin_,
               session_id_,
               vehicle_id_,
               std::to_string(connection_generation_)) +
        "&types=control_command" + (transport_mac_key_.size() >= 32
                   ? "&transport_protocol=" + std::string(detail::kControlTransportCapability) : "");
  }

  [[nodiscard]] HttpHeaders native_control_websocket_headers() const {
    require_session();
    return {{"X-Mine-Teleop-Device-Token", device_token_}};
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
    static_cast<void>((direct_session_http_ ? *direct_session_http_ : http_).post_json_response(
        origin_ + "/signaling/" + http_.url_encode(session_id_) + "/messages",
        request));
  }

  [[nodiscard]] const std::string& session_id() const { return session_id_; }
  [[nodiscard]] const std::string& driver_id() const { return driver_id_; }
  [[nodiscard]] const std::string& control_token() const { return control_token_; }
  [[nodiscard]] const std::string& transport_mac_key() const { return transport_mac_key_; }
  [[nodiscard]] const Json& direct_authority_configuration() const { return direct_authority_configuration_; }
  void bound_direct_session_requests(int budget_ms) {
    direct_session_http_ = std::make_unique<HttpClient>(std::chrono::milliseconds(std::max(1, budget_ms / 3)), direct_resolve_entries_, direct_ca_bundle_);
  }
  Json renew_direct_authority(std::string nonce, int maximum_duration_ms, std::uint64_t retired_epoch) {
    require_session();
    const auto duration = direct_authority_configuration_.at("maximum_duration_ms").get<int>();
    HttpClient bounded(std::chrono::milliseconds(std::max(1, duration / 3)), direct_resolve_entries_, direct_ca_bundle_);
    return bounded.post_json_response(origin_ + "/vehicles/" + bounded.url_encode(vehicle_id_) + "/direct-authority",
        {{"device_token", device_token_}, {"connection_generation", connection_generation_}, {"session_id", session_id_},
         {"nonce", std::move(nonce)}, {"maximum_duration_ms", maximum_duration_ms}, {"retired_authority_epoch", retired_epoch}});
  }
  [[nodiscard]] std::int64_t control_lease_expiry() const { return control_lease_expires_at_ms_.load(); }
  void observe_lease(std::int64_t expiry) {
    if (expiry <= 0) return;
    // Current-session renewal only extends this deadline. A delayed WSS lease
    // cannot regress a newer authenticated RPC value; discover_session resets
    // the cache for a new session, and explicit denial revokes independently.
    auto current = control_lease_expires_at_ms_.load();
    while (expiry > current && !control_lease_expires_at_ms_.compare_exchange_weak(current, expiry)) {}
  }

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
  Json registration_diagnostics_ = Json::object();
  const std::string runtime_instance_id_{"runtime-" + random_token(12)};
  const std::int64_t runtime_started_ms_{std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count()};
  HttpClient http_;
  HttpClient time_sync_http_;
  SynchronizedClock clock_;
  std::string session_id_;
  std::string driver_id_;
  std::string control_token_;
  std::string transport_mac_key_;
  Json direct_authority_configuration_;
  std::unique_ptr<HttpClient> direct_session_http_;
  std::vector<std::string> direct_resolve_entries_;
  std::filesystem::path direct_ca_bundle_;
  std::atomic<std::int64_t> control_lease_expires_at_ms_{0};
  std::shared_ptr<MediaSignalingSequence> sequence_;
  std::mutex send_mutex_;
};

}  // namespace

CameraIssue classify_camera_issue(std::string_view error) {
  return classify_camera_issue_impl(error);
}

struct VehicleMediaRuntime::Impl {
  struct MediaSignalState {
    std::mutex mutex;
    Impl* owner{nullptr};
    GstElement* peer{nullptr};
    std::string negotiation_id;
    std::string compatibility_issue;
    bool local_description_ready{false};
    bool offer_queued{false};
    std::vector<std::string> local_ufrags;
    std::vector<std::string> remote_ufrags;
    std::deque<Json> pending_remote_candidates;
    std::deque<Json> pending_local_candidates;
    std::deque<std::pair<std::string, Json>> pending;
    std::size_t attempt_count{0};
    std::size_t offer_count{0};
    std::string session_id;
    void begin_attempt(Impl* next_owner, GstElement* next_peer, const std::string& session) {
      std::lock_guard lock(mutex);
      if (session_id != session) { session_id = session; attempt_count = 0; offer_count = 0; }
      owner = next_owner; peer = next_peer; negotiation_id.clear(); compatibility_issue.clear();
      local_description_ready = false; offer_queued = false;
      local_ufrags.clear(); remote_ufrags.clear();
      pending_local_candidates.clear(); pending_remote_candidates.clear(); pending.clear();
      ++attempt_count;
    }
  };
  struct MediaOfferContext {
    std::shared_ptr<MediaSignalState> state;
    GstElement* peer;
    std::string negotiation_id;
    ~MediaOfferContext() { gst_object_unref(peer); }
  };
  struct MediaPeerReference {
    GstElement* value;
    ~MediaPeerReference() { gst_object_unref(value); }
  };
  static std::vector<std::string> ice_ufrags(std::string_view sdp) {
    std::vector<std::string> result;
    for (std::size_t begin = 0; begin < sdp.size();) {
      const auto end = sdp.find('\n', begin);
      auto line = sdp.substr(begin, end == std::string_view::npos ? sdp.size() - begin : end - begin);
      if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
      constexpr std::string_view prefix = "a=ice-ufrag:";
      if (line.starts_with(prefix) && line.size() > prefix.size() && line.size() - prefix.size() <= 256 && result.size() < 128) {
        const std::string value(line.substr(prefix.size()));
        if (std::find(result.begin(), result.end(), value) == result.end()) result.push_back(value);
      }
      if (end == std::string_view::npos) break;
      begin = end + 1;
    }
    return result;
  }
  static std::string candidate_ufrag(const Json& candidate) {
    if (candidate.contains("usernameFragment") && candidate["usernameFragment"].is_string())
      return candidate["usernameFragment"].get<std::string>();
    const auto text = candidate.value("candidate", "");
    const auto position = text.find(" ufrag ");
    if (position == std::string::npos) return {};
    const auto begin = position + 7;
    return text.substr(begin, text.find(' ', begin) - begin);
  }
  static constexpr std::size_t kControlTraceQueueCapacity = 256;
  static constexpr std::size_t kControlTraceBatchMaxRecords = 32;
  static constexpr std::size_t kControlTraceBatchCommandsMaxBytes = 40U * 1024U;
  static constexpr std::size_t kControlTraceBatchLineMaxBytes = 48U * 1024U;
  static constexpr std::size_t kControlTraceTextMaxBytes = 128;
  static constexpr std::size_t kControlTraceMaxWarnings = 8;

  enum class ControlMessageTransport {
    DataChannel,
    NativeSignaling,
  };

  [[nodiscard]] static constexpr std::string_view control_transport_name(
      ControlMessageTransport transport) {
    return transport == ControlMessageTransport::NativeSignaling
        ? "native_signaling_websocket"
        : "webrtc_data_channel";
  }

  struct Lane {
    Impl* owner{nullptr};
    CameraConfig camera;
    MediaProfile profile;
    CameraInputSpec input;
    std::unique_ptr<CameraFrameSource> source;
    GstElement* appsrc{nullptr};
    GstElement* encoder{nullptr};
    GstElement* send_queue{nullptr};
    GstElement* record_tap{nullptr};
    std::unique_ptr<detail::RecordingTap> recording_tap;
    std::size_t recording_index{std::numeric_limits<std::size_t>::max()};
    std::unique_ptr<detail::EncodedSendFreshness> send_freshness;
    std::thread thread;
    std::atomic<std::uint64_t> captured{0};
    std::atomic<std::uint64_t> pushed{0};
    std::atomic<std::uint64_t> raw_to_encoder{0};
    std::atomic<std::uint64_t> encoded{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> encoded_queue_backpressure{0};
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
    std::atomic<std::uint64_t> encode_latency_invalid_samples{0};
    std::atomic<bool> encode_latency_current_valid{false};
    std::atomic<std::uint64_t> encoder_output_pts_ms{0};
    std::atomic<std::uint64_t> encoder_segment_start_ms{0};
    GstSegment encoder_output_segment{}; // Only the encoder src probe accesses this.
    std::atomic<std::int64_t> last_capture_steady_ms{0};
    std::atomic<std::int64_t> capture_interval_max_ms{0};
    std::atomic<bool> first_frame_reported{false};
    std::atomic<int> failure_count{0};
    std::atomic<int> reopen_count{0};
    std::atomic<bool> disabled{false};
    std::atomic<bool> camera_faulted{false};
    std::atomic<std::uint64_t> video_wait_encoded_baseline{0};
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
      std::shared_ptr<SessionControlRecoveryLatch> next_control_recovery_latch)
      : config(std::move(next_config)),
        signaling(
            std::move(signaling_url),
            config.vehicle_id,
            std::move(device_token),
            std::move(connection_id),
            std::move(signaling_sequence),
            config.cloud.resolve_entries,
            config.cloud.ca_bundle),
        control_recovery_latch(
            next_control_recovery_latch
                ? std::move(next_control_recovery_latch)
                : std::make_shared<SessionControlRecoveryLatch>()),
        frame_timeout_ms(next_frame_timeout_ms),
        recording_root(std::move(next_recording_root)),
        forced_codec(std::move(next_forced_codec)),
        simulate_primary_failure_after_frames(next_simulate_primary_failure_after_frames) {
    if (frame_timeout_ms <= 0) throw std::invalid_argument("frame timeout must be positive");
    if (simulate_primary_failure_after_frames < 0) {
      throw std::invalid_argument("simulated primary failure frame count must be non-negative");
    }
    start_control_trace_worker();
  }

  ~Impl() {
    try {
      // stop_pipeline closes control_service before joining either native
      // control thread, so no late delivery can postpone the final safe stop.
      stop_pipeline();
    } catch (...) {
    }
    stop_control_trace_worker();
  }

  // control_mutex must be held. A fresh gear is scoped to one active
  // profile/VCU admission epoch and must never survive authority revocation.
  void invalidate_native_control_trusted_gear_locked() noexcept {
    if (!native_control_last_accepted_fresh_gear) return;
    native_control_last_accepted_fresh_gear.reset();
    native_control_trusted_gear_invalidations_total.fetch_add(
        1,
        std::memory_order_relaxed);
  }

  static std::string bounded_control_trace_text(std::string_view value) {
    if (value.size() <= kControlTraceTextMaxBytes) return std::string(value);
    return std::string(value.substr(0, kControlTraceTextMaxBytes)) + "...[truncated]";
  }

  void write_control_trace_batch_line(const Json& entry) const {
    const auto line = entry.dump();
    if (line.size() > kControlTraceBatchLineMaxBytes) {
      throw std::length_error("vehicle control trace batch exceeds the JSONL line limit");
    }
    std::osyncstream output(std::cout);
    output << line << '\n' << std::flush;
    output.emit();
    if (!output) throw std::runtime_error("cannot write vehicle control trace batch");
  }

  void note_control_trace_drop() noexcept {
    control_trace_dropped_total.fetch_add(1, std::memory_order_relaxed);
    control_trace_cv.notify_one();
  }

  void enqueue_control_trace(Json record) noexcept {
    if (!control_trace_accepting.load(std::memory_order_acquire)) {
      note_control_trace_drop();
      return;
    }
    try {
      std::unique_lock lock(control_trace_mutex, std::try_to_lock);
      if (!lock.owns_lock()) {
        note_control_trace_drop();
        return;
      }
      if (control_trace_stop_requested) {
        lock.unlock();
        note_control_trace_drop();
        return;
      }
      if (control_trace_queue.size() >= kControlTraceQueueCapacity) {
        lock.unlock();
        note_control_trace_drop();
        return;
      }
      control_trace_queue.push_back(std::move(record));
      control_trace_enqueued_total.fetch_add(1, std::memory_order_relaxed);
      lock.unlock();
      control_trace_cv.notify_one();
    } catch (...) {
      note_control_trace_drop();
    }
  }

  void control_trace_worker_loop_impl() {
    std::uint64_t reported_dropped_total = 0;
    std::uint64_t emitted_total = 0;
    std::uint64_t output_error_total = 0;
    std::uint64_t batch_seq = 0;
    for (;;) {
      std::vector<Json> records;
      bool final = false;
      {
        std::unique_lock lock(control_trace_mutex);
        control_trace_cv.wait_for(
            lock,
            std::chrono::seconds(1),
            [this] {
              return control_trace_stop_requested ||
                  control_trace_queue.size() >= kControlTraceBatchMaxRecords;
            });
        const auto count = std::min(
            control_trace_queue.size(),
            kControlTraceBatchMaxRecords);
        records.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
          records.push_back(std::move(control_trace_queue.front()));
          control_trace_queue.pop_front();
        }
        final = control_trace_stop_requested && control_trace_queue.empty();
      }

      const auto emit_batch = [this,
                               &reported_dropped_total,
                               &emitted_total,
                               &output_error_total,
                               &batch_seq](Json commands,
                                           std::size_t command_count,
                                           bool final) noexcept {
        const auto dropped_total = control_trace_dropped_total.load(std::memory_order_relaxed);
        const auto dropped_since_last = dropped_total - reported_dropped_total;
        try {
          const auto next_emitted_total = emitted_total + command_count;
          write_control_trace_batch_line({
              {"event", "vehicle_control_trace_batch"},
              {"event_at_utc_ms", signaling.now_ms()},
              {"vehicle_id", bounded_control_trace_text(config.vehicle_id)},
              {"batch_seq", ++batch_seq},
              {"final", final},
              {"queue_capacity", kControlTraceQueueCapacity},
              {"commands", std::move(commands)},
              {"enqueued_total", control_trace_enqueued_total.load(std::memory_order_relaxed)},
              {"emitted_total", next_emitted_total},
              {"dropped_since_last", dropped_since_last},
              {"dropped_total", dropped_total},
              {"output_error_total", output_error_total},
          });
          emitted_total = next_emitted_total;
          reported_dropped_total = dropped_total;
          return true;
        } catch (...) {
          ++output_error_total;
          control_trace_dropped_total.fetch_add(command_count, std::memory_order_relaxed);
          return false;
        }
      };

      std::size_t record_index = 0;
      bool final_batch_written = false;
      while (record_index < records.size()) {
        Json commands = Json::array();
        std::size_t commands_bytes = 2;
        std::size_t command_count = 0;
        while (record_index < records.size()) {
          std::size_t command_bytes = 0;
          try {
            command_bytes = records[record_index].dump().size();
          } catch (...) {
            ++output_error_total;
            note_control_trace_drop();
            ++record_index;
            continue;
          }
          if (command_bytes > kControlTraceBatchCommandsMaxBytes) {
            note_control_trace_drop();
            ++record_index;
            continue;
          }
          const auto separator_bytes = command_count == 0 ? 0U : 1U;
          if (command_count > 0 &&
              commands_bytes + separator_bytes + command_bytes >
                  kControlTraceBatchCommandsMaxBytes) {
            break;
          }
          commands.push_back(std::move(records[record_index++]));
          commands_bytes += separator_bytes + command_bytes;
          ++command_count;
        }
        if (command_count > 0) {
          const bool last = final && record_index == records.size();
          const bool written = emit_batch(std::move(commands), command_count, last);
          final_batch_written = last && written;
        }
      }

      const auto dropped_total = control_trace_dropped_total.load(std::memory_order_relaxed);
      if ((final && !final_batch_written) ||
          (records.empty() && dropped_total != reported_dropped_total)) {
        static_cast<void>(emit_batch(Json::array(), 0, final));
      }
      if (final) return;
    }
  }

  void control_trace_worker_loop() noexcept {
    try {
      control_trace_worker_loop_impl();
    } catch (...) {
      // Diagnostic tracing must never terminate or alter the control runtime.
    }
  }

  void start_control_trace_worker() noexcept {
    if (!config.runtime.control_log_commands) return;
    try {
      control_trace_worker = std::thread([this] { control_trace_worker_loop(); });
      control_trace_accepting.store(true, std::memory_order_release);
    } catch (...) {
      control_trace_accepting.store(false, std::memory_order_release);
    }
  }

  void stop_control_trace_worker() {
    control_trace_accepting.store(false, std::memory_order_release);
    {
      std::lock_guard lock(control_trace_mutex);
      control_trace_stop_requested = true;
    }
    control_trace_cv.notify_all();
    if (control_trace_worker.joinable()) control_trace_worker.join();
  }

  static GstPadProbeReturn count_encoded(GstPad*, GstPadProbeInfo* info, gpointer user_data) {
    auto* lane = static_cast<Lane*>(user_data);
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
      auto* event = GST_PAD_PROBE_INFO_EVENT(info);
      if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
        const GstSegment* segment = nullptr;
        gst_event_parse_segment(event, &segment);
        gst_segment_copy_into(segment, &lane->encoder_output_segment);
        lane->encoder_segment_start_ms = segment->start / GST_MSECOND;
      } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
        gst_segment_init(&lane->encoder_output_segment, GST_FORMAT_UNDEFINED);
      }
      return GST_PAD_PROBE_OK;
    }
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
      ++lane->encoded;
      const auto encoded_at_ms = lane->owner->signaling.now_ms();
      lane->last_encoded_ms = encoded_at_ms;
      lane->last_encoded_steady_ms = steady_now_ms();
      GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      const auto latency = buffer == nullptr ? std::nullopt : encoded_frame_latency_ms(
          lane->encoder_output_segment, GST_BUFFER_PTS(buffer),
          lane->last_encoded_steady_ms.load() - lane->pipeline_started_steady_ms);
      lane->encode_latency_current_valid = latency.has_value();
      if (buffer != nullptr && GST_BUFFER_PTS_IS_VALID(buffer)) {
        lane->encoder_output_pts_ms = GST_BUFFER_PTS(buffer) / GST_MSECOND;
      }
      if (latency) {
        const auto latency_ms = *latency;
        ++lane->encode_latency_samples;
        lane->encode_latency_total_ms += latency_ms;
        auto observed = lane->encode_latency_max_ms.load();
        while (latency_ms > observed && !lane->encode_latency_max_ms.compare_exchange_weak(observed, latency_ms)) {
        }
      } else {
        ++lane->encode_latency_invalid_samples;
      }
    }
    return GST_PAD_PROBE_OK;
  }

  static void on_ice_candidate(GstElement* peer, guint mline_index, gchar* candidate, gpointer user_data) {
    // GStreamer 1.28 emits a final empty candidate after ICE gathering is
    // complete.  Trickle ICE endpoints accept only real candidates; the empty
    // marker must not be sent as an application signaling message.
    if (candidate == nullptr || *candidate == '\0') return;
    const auto state = *static_cast<std::shared_ptr<MediaSignalState>*>(user_data);
    std::lock_guard lock(state->mutex);
    if (state->peer != peer || state->owner == nullptr || state->negotiation_id.empty() || !state->local_description_ready) return;
    auto* self = state->owner;
    const auto ufrag = candidate_ufrag(Json{{"candidate", candidate}});
    if (!ufrag.empty() && std::find(state->local_ufrags.begin(), state->local_ufrags.end(), ufrag) == state->local_ufrags.end()) return;
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
    Json payload{{"candidate", candidate}, {"sdpMLineIndex", mline_index},
        {"media_negotiation_id", state->negotiation_id}};
    if (state->offer_queued) {
      state->pending.emplace_back("ice_candidate", std::move(payload));
    } else if (state->pending_local_candidates.size() < 128) {
      state->pending_local_candidates.push_back(std::move(payload));
    } else {
      self->set_pipeline_error("WebRTC local ICE candidate buffer exceeds limit",
          "webrtc_ice_buffer_limit", "ice_gathering",
          "Inspect interface/candidate count before retrying the media connection.", false);
    }
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
        self->invalidate_native_control_trusted_gear_locked();
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
      self->control_service_issue_code = std::string(self->recovery_inhibition_issue());
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
    bool waiting_for_camera = false;
    {
      std::lock_guard lock(self->control_mutex);
      waiting_for_camera = self->control_service_issue_code == "critical_camera_not_ready";
    }
    if (control_inhibited || (cameras_ready && !adapter_ready && !waiting_for_camera)) {
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
    self->invalidate_native_control_trusted_gear_locked();
    ++self->control_link_loss_count;
    if (self->control_transport_fence.negotiated() && self->control_service_started && self->control_service) {
      self->control_service->set_control_braking_wait(true, self->signaling.now_ms(), ControlRecoveryCause::ControlLink);
      return;
    }
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
    self->handle_control_message(
        channel,
        data == nullptr ? "" : data,
        ControlMessageTransport::DataChannel,
        nullptr);
  }

  static void on_offer_created(GstPromise* promise, gpointer user_data) {
    const auto* context = static_cast<MediaOfferContext*>(user_data);
    const auto state = context->state;
    const auto negotiation_id = context->negotiation_id;
    MediaPeerReference source{GST_ELEMENT(gst_object_ref(context->peer))};
    if (gst_promise_wait(promise) != GST_PROMISE_RESULT_REPLIED) {
      gst_promise_unref(promise);
      std::lock_guard lock(state->mutex);
      if (state->peer != source.value || state->owner == nullptr || state->negotiation_id != negotiation_id) return;
      state->owner->set_pipeline_error(
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
      std::lock_guard lock(state->mutex);
      if (state->peer != source.value || state->owner == nullptr || state->negotiation_id != negotiation_id) return;
      state->owner->set_pipeline_error(
          "WebRTC offer is missing",
          "webrtc_offer_missing",
          "webrtc_offer",
          "Inspect webrtcbin/GStreamer negotiation errors.",
          true);
      return;
    }

    gchar* text = gst_sdp_message_as_text(offer->sdp);
    Json tracks = Json::array();
    {
    std::lock_guard lock(state->mutex);
    if (state->peer != source.value || state->owner == nullptr || state->negotiation_id != negotiation_id) {
      g_free(text); gst_webrtc_session_description_free(offer); return;
    }
    auto* self = state->owner;
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
    }
    {
      std::lock_guard lock(state->mutex);
      if (state->peer != source.value || state->owner == nullptr || state->negotiation_id != negotiation_id) {
        g_free(text); gst_webrtc_session_description_free(offer); return;
      }
      state->local_ufrags = ice_ufrags(text == nullptr ? "" : text);
      state->local_description_ready = true;
    }
    GstPromise* local = gst_promise_new();
    // The captured source is held alive; an old promise never touches the
    // replacement self->webrtc. Candidate callbacks may run during this call.
    g_signal_emit_by_name(source.value, "set-local-description", offer, local);
    gst_promise_interrupt(local);
    gst_promise_unref(local);
    {
    std::lock_guard lock(state->mutex);
    if (state->peer == source.value && state->owner != nullptr && state->negotiation_id == negotiation_id) {
    auto* self = state->owner;
    state->pending.emplace_back("webrtc_offer", Json{
         {"type", "offer"},
         {"control_transport_capability", self->signaling.transport_mac_key().size() >= 32 ? detail::kControlTransportCapability : std::string_view{}},
         {"emergency_stop_capability", self->config.runtime.experimental_direct_authority_max_ms > 0 && self->signaling.transport_mac_key().size() >= 32 ? detail::kEmergencyStopCapability : std::string_view{}},
         {"feedback_transport_capability", self->config.runtime.experimental_direct_authority_max_ms > 0 && self->signaling.transport_mac_key().size() >= 32 ? detail::kFeedbackTransportCapability : std::string_view{}},
         {"sdp", text == nullptr ? "" : text},
         {"media_negotiation_id", negotiation_id},
         {"legacy_bootstrap_allowed", state->attempt_count == 1 && state->offer_count == 1},
         {"codec", to_string(self->active_candidate.codec)},
         {"backend", to_string(self->active_candidate.backend)},
         {"media_tracks", std::move(tracks)}});
    state->offer_queued = true;
    for (auto& candidate : state->pending_local_candidates)
      state->pending.emplace_back("ice_candidate", std::move(candidate));
    state->pending_local_candidates.clear();
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
    }
    }
    g_free(text);
    gst_webrtc_session_description_free(offer);
  }

  static void on_negotiation_needed(GstElement* webrtc, gpointer user_data) {
    const auto state = *static_cast<std::shared_ptr<MediaSignalState>*>(user_data);
    std::unique_ptr<MediaOfferContext> context;
    {
      std::lock_guard lock(state->mutex);
      if (state->peer != webrtc || state->owner == nullptr) return;
      // Fixed lanes use a new webrtcbin for every retry. Keep this peer's
      // epoch immutable, including the check-to-description-emission gap.
      // Same-peer renegotiation and ICE restart are not supported here.
      if (!state->negotiation_id.empty()) return;
      state->negotiation_id = random_token(12);
      state->local_description_ready = false; state->offer_queued = false;
      state->local_ufrags.clear(); state->pending_local_candidates.clear();
      ++state->offer_count;
      state->remote_ufrags.clear(); state->pending_remote_candidates.clear();
      std::erase_if(state->pending, [](const auto& value) {
        return value.first == "webrtc_offer" || value.first == "ice_candidate";
      });
      context.reset(new MediaOfferContext{
          state, GST_ELEMENT(gst_object_ref(webrtc)), state->negotiation_id});
    }
    GstPromise* promise = gst_promise_new_with_change_func(on_offer_created, context.release(),
        +[](gpointer value) { delete static_cast<MediaOfferContext*>(value); });
    g_signal_emit_by_name(webrtc, "create-offer", nullptr, promise);
  }

  void queue_signal(std::string type, Json payload) {
    std::lock_guard lock(media_signal_state->mutex);
    if (media_signal_state->peer != nullptr)
      media_signal_state->pending.emplace_back(std::move(type), std::move(payload));
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
    if (!details.contains("severity")) details["severity"] = error.empty() ? "info" : "error";
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
      invalidate_native_control_trusted_gear_locked();
      if (control_service_started && control_service) {
        try {
          // close() applies the local safe-stop output before closing the
          // adapter.  This must happen in the faulting thread rather than wait
          // for a potentially blocked HTTP/media main loop.
          control_service->close({
              VehicleStopSource::SoftwareFault,
              VehicleStopReason::MediaPipelineFailed});
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
               ? "braking_wait_and_reopen_camera_lane"
               : "braking_wait_and_disable_failed_camera_lane")
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
            {"capture_diagnostics", lane.source == nullptr ? Json::object() : lane.source->capture_diagnostics()},
            {"last_capture_age_ms", lane.last_capture_steady_ms.load() == 0 ? -1 :
                steady_now_ms() - lane.last_capture_steady_ms.load()},
            {"critical_for_control", lane.camera.critical_for_control},
            {"failure_count", failure_count},
            {"reopen_attempts", lane.camera.reopen_attempts},
            {"safety_action", safety_action},
        });
  }

  void handle_control_message(
      GstWebRTCDataChannel* channel,
      std::string_view data,
      ControlMessageTransport transport,
      const NativeControlDeliveryTraceContext* delivery_trace,
      Json* native_feedback = nullptr) {
    const auto callback_entered_monotonic_ms = steady_now_ms();
    const auto callback_entered_at_utc_ms = signaling.now_ms();
    const std::string transport_name(control_transport_name(transport));
    const auto delivery_cursor =
        delivery_trace == nullptr ? std::uint64_t{0} : delivery_trace->delivery_cursor;
    const auto cloud_queued_at_utc_ms =
        delivery_trace == nullptr ? std::int64_t{0} : delivery_trace->cloud_queued_at_utc_ms;
    const auto envelope_received_at_utc_ms = delivery_trace == nullptr
        ? std::int64_t{0}
        : delivery_trace->envelope_received_at_utc_ms;
    const auto envelope_received_monotonic_ms = delivery_trace == nullptr
        ? std::int64_t{0}
        : delivery_trace->envelope_received_monotonic_ms;
    const auto envelope_message_count =
        delivery_trace == nullptr ? std::size_t{0} : delivery_trace->envelope_message_count;
    const auto valid_message_count =
        delivery_trace == nullptr ? std::size_t{0} : delivery_trace->valid_message_count;
    const auto superseded_message_count = delivery_trace == nullptr
        ? std::size_t{0}
        : delivery_trace->superseded_message_count;
    if (data.empty() || data.size() > 64 * 1024) {
      ++rejected_control_commands;
      return;
    }
    try {
      const auto message = Json::parse(data);
      if (message.value("type", "") == "session_control_profile") {
        const auto request = SessionControlProfileRequest::from_json(message);
        std::lock_guard lock(control_mutex);
        if (transport != ControlMessageTransport::DataChannel && !control_transport_fence.negotiated() && !direct_authority.valid(steady_now_ms())) { ++rejected_control_commands; return; }
        if (transport == ControlMessageTransport::DataChannel && control_channel != channel) return;
        if (config.runtime.experimental_direct_authority_max_ms > 0 &&
            (transport == ControlMessageTransport::DataChannel || !direct_authority.valid(steady_now_ms()))) {
          ++rejected_control_commands; return; // Parameter changes require a live authenticated cloud delivery.
        }
        SessionControlProfileResult result;
        if (stop_requested || control_inhibited || !control_service_started ||
            !control_service || (!control_link_open && !control_transport_fence.negotiated() && !cloud_control_authority_live_locked())) {
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
          if (result.accepted) invalidate_native_control_trusted_gear_locked();
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
        if (transport != ControlMessageTransport::DataChannel && !control_transport_fence.negotiated() && !direct_authority.valid(steady_now_ms())) { ++rejected_control_commands; return; }
        if (transport == ControlMessageTransport::DataChannel && control_channel != channel) return;
        if (stop_requested || control_inhibited || !control_service_started ||
            !control_service || (!control_link_open && !control_transport_fence.negotiated() && !cloud_control_authority_live_locked())) {
          send_vcu_handshake_status_locked("driver_not_connected");
          return;
        }
        if (action == "connect" && config.runtime.experimental_direct_authority_max_ms > 0 &&
            (transport == ControlMessageTransport::DataChannel || !direct_authority.valid(steady_now_ms()))) {
          ++rejected_control_commands; send_vcu_handshake_status_locked("authority_cloud_delivery_required"); return;
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
        if (accepted) invalidate_native_control_trusted_gear_locked();
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
      if (transport == ControlMessageTransport::DataChannel && !message.contains("control_transport") && !message.contains("emergency_stop")) {
        // Session profile and VCU handshake status still use the browser's
        // DataChannel, but periodic actuation commands have a single native
        // signaling path.  Reject mixed-version browser commands so one seq
        // cannot be applied once through each transport.
        const auto rejected_count = ++rejected_control_commands;
        if (rejected_count == 1 || rejected_count % 100 == 0) {
          std::cout << Json({
                           {"event", "vehicle_control_transport_rejected"},
                           {"event_at_utc_ms", signaling.now_ms()},
                           {"vehicle_id", config.vehicle_id},
                           {"driver_id", signaling.driver_id()},
                           {"session_id", signaling.session_id()},
                           {"transport", transport_name},
                           {"reason", "legacy_data_channel_control_disabled"},
                           {"rejected_commands", rejected_count},
                       }).dump()
                    << '\n';
        }
        return;
      }
      auto command = ControlCommand::from_json(message);
      const auto wire_requested_gear = command.gear;
      const auto intent_seq = message.value("intent_seq", std::uint64_t{0});
      if (intent_seq == 0) throw std::invalid_argument("intent_seq must be positive");
      if (!message.contains("intent_fresh") || !message.at("intent_fresh").is_boolean())
        throw std::invalid_argument("intent_fresh must be a boolean");
      const bool intent_fresh = message.at("intent_fresh").get<bool>();
      if (command.session_id != signaling.session_id() || command.vehicle_id != config.vehicle_id ||
          command.driver_id != signaling.driver_id() || command.control_token != signaling.control_token())
        throw std::invalid_argument("control result authority mismatch");
      if ((message.contains("control_transport") || message.contains("emergency_stop") || message.contains("feedback_request")) && !detail::verify_transport_message(message, signaling.transport_mac_key(), false))
        throw std::invalid_argument("native control authentication failed");
      std::string current_attempt;
      { std::lock_guard scope_lock(media_signal_state->mutex); current_attempt = media_signal_state->negotiation_id; }
      Json result_message = {{"event", "vehicle_control_result"}, {"protocol", detail::kControlTransportCapability},
          {"session_id", command.session_id}, {"vehicle_id", command.vehicle_id}, {"driver_id", command.driver_id},
          {"command_seq", command.seq}, {"intent_seq", intent_seq}, {"accepted", false},
          {"rearm_required", false}, {"control_status_seq", 0}, {"reason", "control_not_applied"},
          {"transport_admitted", false}, {"transport_selected", false}};
      if (config.runtime.experimental_direct_authority_max_ms > 0)
        result_message["command_received_path"] = transport == ControlMessageTransport::DataChannel ? "dc" : "wss";
      if (message.contains("control_transport")) {
        const auto stamp = detail::ControlTransportStamp::from_json(message.at("control_transport"));
        result_message["control_transport"] = stamp.to_json();
      }
      if (message.contains("feedback_request") && message.at("feedback_request").is_object() && message.at("feedback_request").dump().size() <= 1024)
        result_message["feedback_request"] = message.at("feedback_request");
      bool emergency_reserved = false, verified_stop_only = false;
      if (message.contains("emergency_stop") && message.at("emergency_stop").is_object() && message.at("emergency_stop").dump().size() <= 1024)
        result_message["emergency_stop"] = message.at("emergency_stop");
      struct PublishResult {
        std::function<void()> publish;
        ~PublishResult() { try { publish(); } catch (...) {} }
      } publish_result{[&, this] {
        if (signaling.transport_mac_key().size() < 32) return;
        std::lock_guard result_lock(control_mutex);
        result_message["result_seq"] = ++control_result_seq;
        result_message["authority_epoch"] = control_transport_fence.epoch();
        if (direct_authority.configured()) result_message["direct_authority"] = direct_authority.status(steady_now_ms());
        if (message.contains("feedback_request")) result_message["feedback_transport"] = feedback_transport_fence.response(
            message.at("feedback_request"), direct_authority.reference(), current_attempt, control_status_seq);
        else if (feedback_transport_fence.negotiated()) result_message["feedback_transport"] = feedback_transport_fence.state();
        if (config.runtime.experimental_direct_authority_max_ms > 0) {
          if (control_service) emergency_stop_receiver.observe_latched(control_service->safety_state() == SafetyState::Estop);
          result_message["emergency_reset_epoch"] = emergency_stop_receiver.reset_epoch();
          if (message.contains("emergency_stop")) result_message["software_estop_latched"] = control_service && control_service->safety_state() == SafetyState::Estop;
        }
        if (signaling.transport_mac_key().size() >= 32)
          result_message["vehicle_mac"] = detail::transport_message_mac(result_message, signaling.transport_mac_key(), true);
        if (emergency_reserved) static_cast<void>(emergency_stop_receiver.complete(message, result_message));
        const bool selected_feedback = feedback_transport_fence.negotiated() && message.contains("feedback_request") && !command.estop;
        const bool probe = selected_feedback && message.at("feedback_request").value("probe", false);
        const bool via_wss = !selected_feedback || probe || feedback_transport_fence.path() == detail::ControlPath::Wss;
        const bool via_dc = selected_feedback ? probe || feedback_transport_fence.path() == detail::ControlPath::DataChannel :
            transport == ControlMessageTransport::DataChannel || message.contains("emergency_stop");
        if (via_wss) latest_control_result = result_message;
        if (native_feedback && via_wss) *native_feedback = result_message;
        if (control_link_open && control_channel && via_dc &&
            (transport != ControlMessageTransport::DataChannel || control_channel == channel))
          gst_webrtc_data_channel_send_string(control_channel, result_message.dump().c_str());
      }};
      const bool stale_safe_heartbeat =
          !command.estop && !intent_fresh &&
          command.steering == 0.0 && command.throttle == 0.0 &&
          command.brake == 0.0;
      const auto control_mutex_wait_started_monotonic_ms = steady_now_ms();
      std::unique_lock lock(control_mutex);
      const auto control_mutex_acquired_monotonic_ms = steady_now_ms();
      const auto control_mutex_acquired_at_utc_ms = signaling.now_ms();
      const auto active_session_id = signaling.session_id();
      if (command.session_id == active_session_id && command.vehicle_id == config.vehicle_id) {
        last_parsed_control_arrival_ms = callback_entered_at_utc_ms;
        last_parsed_control_arrival_seq = command.seq;
      }
      const bool control_log_commands = config.runtime.control_log_commands;
      const auto queue_control_trace = [this,
                                        &command,
                                        callback_entered_at_utc_ms,
                                        callback_entered_monotonic_ms,
                                        control_mutex_wait_started_monotonic_ms,
                                        control_mutex_acquired_at_utc_ms,
                                        control_mutex_acquired_monotonic_ms,
                                        active_session_id,
                                        control_log_commands,
                                        intent_seq,
                                        intent_fresh,
                                        stale_safe_heartbeat,
                                        wire_requested_gear,
                                        transport_name,
                                        delivery_cursor,
                                        cloud_queued_at_utc_ms,
                                        envelope_received_at_utc_ms,
                                        envelope_received_monotonic_ms,
                                        envelope_message_count,
                                        valid_message_count,
                                        superseded_message_count](
                                           const ReceiveResult* result,
                                           std::string_view reason,
                                           bool receive_apply_invoked,
                                           std::optional<std::int64_t> receive_apply_started_at_utc_ms,
                                           std::optional<std::int64_t> receive_apply_started_monotonic_ms,
                                           std::optional<std::int64_t> receive_apply_completed_at_utc_ms,
                                           std::optional<std::int64_t> receive_apply_completed_monotonic_ms) noexcept {
        if (!control_log_commands) return;
        try {
          const auto completed_monotonic_ms = steady_now_ms();
          const auto completed_at_utc_ms = signaling.now_ms();
          Json warnings = Json::array();
          if (result != nullptr) {
            for (std::size_t index = 0;
                 index < std::min(result->warnings.size(), kControlTraceMaxWarnings);
                 ++index) {
              warnings.push_back(bounded_control_trace_text(result->warnings[index]));
            }
          }
          Json record = {
              {"stage", "receive_apply"},
              {"protocol_version", command.protocol_version},
              {"vehicle_id", bounded_control_trace_text(command.vehicle_id)},
              {"driver_id", bounded_control_trace_text(command.driver_id)},
              {"session_id", bounded_control_trace_text(command.session_id)},
              {"trace_session_id", bounded_control_trace_text(command.session_id)},
              {"active_session_id", bounded_control_trace_text(active_session_id)},
              {"transport", transport_name},
              {"seq", command.seq},
              {"intent_seq", intent_seq},
              {"intent_fresh", intent_fresh},
              {"critical_video_ready", command.critical_video_ready},
              {"control_time_sync_ready", command.control_time_sync_ready},
              {"operator_input_ready", command.operator_input_ready},
              {"control_link_ready", command.control_link_ready},
              {"stale_safe_heartbeat", stale_safe_heartbeat},
              {"delivery_cursor", delivery_cursor},
              {"cloud_queued_at_utc_ms", cloud_queued_at_utc_ms},
              {"vehicle_envelope_received_at_utc_ms", envelope_received_at_utc_ms},
              {"vehicle_envelope_received_monotonic_ms", envelope_received_monotonic_ms},
              {"envelope_message_count", envelope_message_count},
              {"valid_message_count", valid_message_count},
              {"superseded_message_count", superseded_message_count},
              {"sent_at_utc_ms", command.sent_at_utc_ms},
              {"callback_entered_at_utc_ms", callback_entered_at_utc_ms},
              {"callback_entered_monotonic_ms", callback_entered_monotonic_ms},
              {"driver_to_vehicle_callback_utc_delta_ms",
               callback_entered_at_utc_ms - command.sent_at_utc_ms},
              {"cloud_queue_to_vehicle_callback_utc_delta_ms",
               cloud_queued_at_utc_ms > 0
                   ? callback_entered_at_utc_ms - cloud_queued_at_utc_ms
                   : 0},
              {"vehicle_envelope_to_callback_ms",
               envelope_received_monotonic_ms > 0
                   ? std::max<std::int64_t>(
                         0,
                         callback_entered_monotonic_ms -
                             envelope_received_monotonic_ms)
                   : 0},
              {"control_mutex_wait_started_monotonic_ms", control_mutex_wait_started_monotonic_ms},
              {"received_at_utc_ms", control_mutex_acquired_at_utc_ms},
              {"control_mutex_acquired_at_utc_ms", control_mutex_acquired_at_utc_ms},
              {"control_mutex_acquired_monotonic_ms", control_mutex_acquired_monotonic_ms},
              {"control_mutex_wait_ms", std::max<std::int64_t>(
                                            0,
                                            control_mutex_acquired_monotonic_ms -
                                                control_mutex_wait_started_monotonic_ms)},
              {"callback_to_mutex_acquired_ms", std::max<std::int64_t>(
                                                      0,
                                                      control_mutex_acquired_monotonic_ms -
                                                          callback_entered_monotonic_ms)},
              {"receive_apply_invoked", receive_apply_invoked},
              {"receive_apply_started_at_utc_ms", receive_apply_started_at_utc_ms
                                                       ? Json(*receive_apply_started_at_utc_ms)
                                                       : Json(nullptr)},
              {"receive_apply_started_monotonic_ms", receive_apply_started_monotonic_ms
                                                       ? Json(*receive_apply_started_monotonic_ms)
                                                       : Json(nullptr)},
              {"receive_apply_completed_at_utc_ms", receive_apply_completed_at_utc_ms
                                                         ? Json(*receive_apply_completed_at_utc_ms)
                                                         : Json(nullptr)},
              {"receive_apply_completed_monotonic_ms", receive_apply_completed_monotonic_ms
                                                         ? Json(*receive_apply_completed_monotonic_ms)
                                                         : Json(nullptr)},
              {"receive_apply_processing_ms",
               receive_apply_started_monotonic_ms && receive_apply_completed_monotonic_ms
                   ? Json(std::max<std::int64_t>(
                         0,
                         *receive_apply_completed_monotonic_ms -
                             *receive_apply_started_monotonic_ms))
                   : Json(nullptr)},
              {"control_path_completed_before_trace_at_utc_ms", completed_at_utc_ms},
              {"control_path_completed_before_trace_monotonic_ms", completed_monotonic_ms},
              {"control_path_processing_before_trace_ms", std::max<std::int64_t>(
                                                              0,
                                                              completed_monotonic_ms -
                                                                  callback_entered_monotonic_ms)},
              {"accepted", result != nullptr && result->accepted},
              {"reason", bounded_control_trace_text(reason)},
              {"requested_gear", bounded_control_trace_text(wire_requested_gear)},
              {"transport_effective_gear", bounded_control_trace_text(command.gear)},
              {"estop_gear_overridden",
               command.estop && wire_requested_gear != command.gear},
              {"requested_steering", command.steering},
              {"requested_throttle", command.throttle},
              {"requested_brake", command.brake},
              {"requested_estop", command.estop},
              {"warnings", std::move(warnings)},
          };
          if (result != nullptr && result->command) {
            record["effective_gear"] = bounded_control_trace_text(result->command->gear);
            record["effective_steering"] = result->command->steering;
            record["effective_throttle"] = result->command->throttle;
            record["effective_brake"] = result->command->brake;
            record["effective_estop"] = result->command->estop;
          }
          enqueue_control_trace(std::move(record));
        } catch (...) {
          note_control_trace_drop();
        }
      };
      if (stop_requested || control_inhibited ||
          !control_service_started || !control_service || (!control_link_open && !control_transport_fence.negotiated() && !cloud_control_authority_live_locked())) {
        ++rejected_control_commands;
        const std::string_view reason = stop_requested
            ? "runtime_stop_requested"
            : control_inhibited
            ? "control_inhibited"
            : (!control_service_started || !control_service)
            ? "control_service_unavailable"
            : "control_link_not_open";
        lock.unlock();
        queue_control_trace(
            nullptr,
            reason,
            false,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt);
        return;
      }
      if (transport == ControlMessageTransport::DataChannel && control_channel != channel) return;
      if (message.contains("emergency_stop")) {
        if (config.runtime.experimental_direct_authority_max_ms <= 0) { result_message["reason"] = "experimental_stop_disabled"; return; }
        emergency_stop_receiver.observe_latched(control_service->safety_state() == SafetyState::Estop);
        const auto stop = emergency_stop_receiver.inspect(message, signaling.transport_mac_key(), direct_authority.reference(),
            current_attempt, transport == ControlMessageTransport::DataChannel, signaling.now_ms(), config.control.max_command_gap_ms);
        if (stop.action == detail::EmergencyStopReceiver::Action::Cached || stop.action == detail::EmergencyStopReceiver::Action::Pending) {
          if (stop.action == detail::EmergencyStopReceiver::Action::Cached) {
            latest_control_result = stop.cached;
            if (native_feedback) *native_feedback = stop.cached;
            if (control_link_open && control_channel) gst_webrtc_data_channel_send_string(control_channel, stop.cached.dump().c_str());
          }
          publish_result.publish = [] {}; return; // No service call, no watchdog/lease refresh.
        }
        if (stop.action != detail::EmergencyStopReceiver::Action::ApplyOnce) { ++rejected_control_commands; result_message["reason"] = stop.reason; return; }
        emergency_reserved = verified_stop_only = true;
      }
      if (!command.estop && config.runtime.experimental_direct_authority_max_ms > 0 &&
          (!direct_authority.configured() || !direct_authority.allows(message.value("direct_authority", Json(nullptr)), steady_now_ms()))) {
        ++rejected_control_commands; result_message["reason"] = "direct_authority_unavailable"; return;
      }
      if (message.contains("control_transport") || message.contains("feedback_request") || control_transport_fence.negotiated()) {
        const auto decision = control_transport_fence.admit(command,
            message.contains("control_transport") ? &message.at("control_transport") : nullptr,
            transport == ControlMessageTransport::DataChannel ? detail::ControlPath::DataChannel : detail::ControlPath::Wss,
            current_attempt, signaling.now_ms(), signaling.control_lease_expiry(), config.control.max_command_gap_ms,
            direct_authority.configured() ? direct_authority.valid(steady_now_ms()) :
                (native_control_websocket_connected.load() && native_control_transport_cloud_capable.load()), verified_stop_only);
        result_message["reason"] = decision.reason;
        result_message["transport_admitted"] = decision.admitted;
        result_message["transport_selected"] = decision.admitted && message.contains("control_transport") &&
            message.at("control_transport").value("action", "") == "select";
        if (!decision.admitted) { ++rejected_control_commands; return; }
        if (decision.changed) {
          invalidate_native_control_trusted_gear_locked();
          control_service->set_control_braking_wait(true, signaling.now_ms(), ControlRecoveryCause::ControlLink);
        }
      } else {
        result_message["transport_admitted"] = true;
      }
      if (!command.estop && config.runtime.experimental_direct_authority_max_ms > 0 && message.contains("feedback_request")) {
        const auto before = feedback_transport_fence.state();
        static_cast<void>(feedback_transport_fence.select(message.at("feedback_request"), direct_authority.reference(), current_attempt, control_status_seq));
        if (before != feedback_transport_fence.state()) {
          pending_control_status.clear();
          // Re-publish current snapshots with new status publication sequences;
          // original CAN/evidence timestamps remain unchanged.
          for (const auto& [kind, snapshot] : latest_feedback_status) {
            if (kind == "control_command_rejected") continue; // Event notification is not a fresh state snapshot.
            auto status = snapshot; status["control_status_seq"] = ++control_status_seq;
            send_control_status_locked(std::move(status));
          }
        }
      }
      if (!command.estop && !intent_fresh && !stale_safe_heartbeat) {
        native_control_stale_nonzero_intents_discarded_total.fetch_add(
            1,
            std::memory_order_relaxed);
        lock.unlock();
        queue_control_trace(
            nullptr,
            "stale_nonzero_intent_discarded",
            false,
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt);
        return;
      }
      if (command.estop) {
        const auto estop_gear = native_control_estop_frozen_gear
            ? *native_control_estop_frozen_gear
            : native_control_last_accepted_fresh_gear.value_or(command.gear);
        if (command.gear != estop_gear) {
          native_control_estop_gear_overrides_total.fetch_add(
              1,
              std::memory_order_relaxed);
        }
        command.gear = estop_gear;
      }
      if (!command.estop) {
        const bool profile_active =
            control_service->session_control_profile().value("active", false);
        bool handshake_ready = false;
        const auto adapter_status = control_service->adapter_status();
        const bool mock_bench_bypass =
            config.field_safety.commissioning_mode == "bench" &&
            adapter_status.adapter_type == "mock";
        if (mock_bench_bypass) {
          handshake_ready = true;
        } else {
          try {
            handshake_ready = control_service->vcu_handshake_status().ready;
          } catch (...) {
            handshake_ready = false;
          }
        }
        if (!profile_active || !handshake_ready) {
          invalidate_native_control_trusted_gear_locked();
          if (!profile_active) {
            native_control_profile_not_ready_discards_total.fetch_add(
                1,
                std::memory_order_relaxed);
          } else {
            native_control_handshake_not_ready_discards_total.fetch_add(
                1,
                std::memory_order_relaxed);
          }
          const std::string_view reason = !profile_active
              ? "session_profile_not_ready"
              : "vcu_handshake_not_ready";
          // Bootstrap and pre-handshake neutral packets are transport liveness
          // only. Feeding them to receive_command would trigger
          // SessionProfileRequired safe-stop and clear the profile needed to
          // complete the handshake.
          lock.unlock();
          queue_control_trace(
              nullptr,
              reason,
              false,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt);
          return;
        }
      }
      if (stale_safe_heartbeat) {
        const auto previous_accepted_monotonic_ms = native_control_last_accepted_monotonic_ms.load();
        const auto gap = previous_accepted_monotonic_ms > 0 && control_mutex_acquired_monotonic_ms >= previous_accepted_monotonic_ms
            ? control_mutex_acquired_monotonic_ms - previous_accepted_monotonic_ms : -1;
        const auto heartbeat_gate = neutral_heartbeat_gate(control_service->safety_state(),
            control_service->control_braking_wait(), gap, config.control.degraded_timeout_ms);
        if (heartbeat_gate == NeutralHeartbeatGate::InactiveState) {
          native_control_stale_safe_inactive_state_discards_total.fetch_add(
              1,
              std::memory_order_relaxed);
          lock.unlock();
          queue_control_trace(
              nullptr,
              "stale_safe_heartbeat_control_not_active",
              false,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt);
          return;
        }
        if (heartbeat_gate == NeutralHeartbeatGate::NativeGap) {
          // Close the scheduling race in which the independent watchdog has
          // not yet observed an already-expired native packet gap. Otherwise
          // receive_command could advance to DEGRADED and immediately recover
          // it with this stale neutral packet in the same call.
          native_control_stale_safe_native_gap_discards_total.fetch_add(
              1,
              std::memory_order_relaxed);
          lock.unlock();
          queue_control_trace(
              nullptr,
              "stale_safe_heartbeat_native_gap_elapsed",
              false,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt);
          return;
        }
        if (!native_control_last_accepted_fresh_gear ||
            command.gear != *native_control_last_accepted_fresh_gear) {
          native_control_stale_safe_untrusted_gear_discards_total.fetch_add(
              1,
              std::memory_order_relaxed);
          lock.unlock();
          queue_control_trace(
              nullptr,
              "stale_safe_heartbeat_gear_untrusted",
              false,
              std::nullopt,
              std::nullopt,
              std::nullopt,
              std::nullopt);
          return;
        }
        // Browser input freshness and native transport liveness are separate
        // safety signals. A lease-expired command may maintain (but never
        // establish or recover) CONTROL_ACTIVE only after the native sender
        // has forced every actuator request to exact zero and retained the
        // last gear accepted from fresh operator input. A real WSS/process gap
        // still enters DEGRADED/TIMEOUT_BRAKE and requires fresh neutral input.
        native_control_stale_safe_heartbeats_forwarded_total.fetch_add(
            1,
            std::memory_order_relaxed);
      }
      const auto received_at_ms = control_mutex_acquired_at_utc_ms;
      if (!command.estop && config.field_safety.require_time_sync &&
          !vehicle_time_readiness().ready) {
        control_service->set_control_braking_wait(true, received_at_ms, ControlRecoveryCause::TimeSynchronization);
      }
      const auto receive_apply_started_at_utc_ms = signaling.now_ms();
      const auto receive_apply_started_monotonic_ms = steady_now_ms();
      ReceiveResult result;
      std::int64_t receive_apply_completed_at_utc_ms = 0;
      std::int64_t receive_apply_completed_monotonic_ms = 0;
      try {
        result = control_service->receive_command(command, received_at_ms);
        last_evaluated_control_seq = command.seq;
        last_evaluated_control_reason = result.reason;
        receive_apply_completed_at_utc_ms = signaling.now_ms();
        receive_apply_completed_monotonic_ms = steady_now_ms();
      } catch (...) {
        const auto completed_at_utc_ms = signaling.now_ms();
        const auto completed_monotonic_ms = steady_now_ms();
        // receive_command validates token/sequence before latching ESTOP, but
        // the subsequent physical apply_safe_stop may throw. Freeze the chosen
        // gear when that outer ESTOP latch is observable so a later ESTOP
        // cannot rewrite it; an unauthenticated/replayed ESTOP never reaches
        // SafetyState::Estop and therefore cannot poison this state.
        if (command.estop && !native_control_estop_frozen_gear &&
            control_service &&
            control_service->safety_state() == SafetyState::Estop) {
          native_control_estop_frozen_gear = command.gear;
          native_control_estop_gear_freezes_total.fetch_add(
              1,
              std::memory_order_relaxed);
          native_control_estop_gear_freezes_after_apply_error_total.fetch_add(
              1,
              std::memory_order_relaxed);
        }
        lock.unlock();
        queue_control_trace(
            nullptr,
            "receive_apply_exception",
            true,
            receive_apply_started_at_utc_ms,
            receive_apply_started_monotonic_ms,
            completed_at_utc_ms,
            completed_monotonic_ms);
        throw;
      }
      std::uint64_t rearm_status_seq = 0;
      if (config.runtime.control_log_commands &&
          (result.reason == "command_age_exceeded" || result.reason == "command_gap_exceeded" ||
           result.issue_code == "control_input_rearm_required")) {
        const auto timing = control_service->control_timing(received_at_ms);
        enqueue_control_trace({{"stage", "control_timing_decision"}, {"session_id", command.session_id},
                               {"vehicle_id", command.vehicle_id}, {"seq", command.seq},
                               {"reason", result.reason}, {"control_timing", timing}});
      }
      if (result.accepted && result.command) {
        const auto accepted_count = ++accepted_control_commands;
        if (command.estop && !native_control_estop_frozen_gear) {
          native_control_estop_frozen_gear = command.gear;
          native_control_estop_gear_freezes_total.fetch_add(
              1,
              std::memory_order_relaxed);
        } else if (!command.estop && intent_fresh) {
          native_control_last_accepted_fresh_gear = result.command->gear;
          native_control_fresh_gear_updates_total.fetch_add(
              1,
              std::memory_order_relaxed);
        }
        if (stale_safe_heartbeat) {
          native_control_stale_safe_heartbeats_accepted_total.fetch_add(
              1,
              std::memory_order_relaxed);
        }
        last_control_received_at_ms = received_at_ms;
        last_accepted_control_seq = command.seq;
        native_control_last_accepted_monotonic_ms =
            control_mutex_acquired_monotonic_ms;
        if (accepted_count == 1 || accepted_count % 100 == 0) {
          std::cout << Json({
                           {"event", "vehicle_native_control_progress"},
                           {"event_at_utc_ms", received_at_ms},
                           {"vehicle_id", config.vehicle_id},
                           {"driver_id", signaling.driver_id()},
                           {"session_id", signaling.session_id()},
                           {"transport", transport_name},
                           {"accepted_commands", accepted_count},
                           {"rejected_commands", rejected_control_commands.load()},
                       }).dump()
                    << '\n';
        }
      } else {
        ++rejected_control_commands;
        const auto now_ms = signaling.now_ms();
        if (!result.issue_code.empty()) {
          rearm_status_seq = send_control_command_rejected_locked(command.seq, intent_seq, result.issue_code);
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
                  ? "Stop the vehicle, restore fresh speed and gear feedback, release the direction control, and select the intended gear again."
                  : result.issue_code == "control_input_rearm_required"
                  ? "Send fresh zero-traction/zero-steering input in the current gear, then release and press the controls again."
                  : feedback_problem
                  ? "Inspect VCU feedback freshness and the VCU JSONL log before requesting control."
                  : "Inspect command identity, sequence, timing, token, and configured safety limits.",
              true,
              {{"reason", result.reason},
               {"transport", transport_name},
               {"safety_action",
                result.issue_code == "vcu_drive_gear_change_moving_or_stale"
                    ? "traction_withdrawn_retained_gear"
                    : result.issue_code == "control_input_rearm_required"
                    ? "fresh_neutral_input_requested"
                    : "local_full_stop"}});
        }
      }
      observe_control_hard_stop_locked(received_at_ms);
      control_transport_fence.note_legacy_sequence(command.seq);
      result_message["accepted"] = result.accepted;
      result_message["reason"] = result.reason;
      result_message["rearm_required"] = result.issue_code == "control_input_rearm_required";
      result_message["control_status_seq"] = rearm_status_seq;
      if (native_feedback && signaling.transport_mac_key().size() < 32) {
        *native_feedback = {{"session_id", command.session_id}, {"vehicle_id", command.vehicle_id},
            {"driver_id", command.driver_id}, {"command_seq", command.seq}, {"intent_seq", intent_seq},
            {"accepted", result.accepted}, {"rearm_required", result.issue_code == "control_input_rearm_required"},
            {"control_status_seq", rearm_status_seq}};
      }
      lock.unlock();
      queue_control_trace(
          &result,
          result.reason,
          true,
          receive_apply_started_at_utc_ms,
          receive_apply_started_monotonic_ms,
          receive_apply_completed_at_utc_ms,
          receive_apply_completed_monotonic_ms);
    } catch (const std::exception& error) {
      ++rejected_control_commands;
      std::cout << Json({
                       {"event", "vehicle_control_message_rejected"},
                       {"event_at_utc_ms", signaling.now_ms()},
                       {"vehicle_id", config.vehicle_id},
                       {"session_id", signaling.session_id()},
                       {"transport", transport_name},
                       {"reason", "invalid_control_message"},
                       {"error", error.what()},
                   }).dump()
                << '\n';
    }
  }

  void note_native_control_transport_error(
      std::string_view error,
      bool protocol_error) noexcept {
    const auto total =
        native_control_transport_errors_total.fetch_add(1, std::memory_order_relaxed) + 1;
    const auto consecutive =
        native_control_transport_consecutive_errors.fetch_add(1, std::memory_order_relaxed) + 1;
    if (protocol_error) {
      native_control_protocol_errors_total.fetch_add(1, std::memory_order_relaxed);
    }
    const auto error_at_ms = signaling.now_ms();
    native_control_transport_last_error_at_ms.store(error_at_ms, std::memory_order_relaxed);
    // An uncertain connection or protocol boundary invalidates every ordinary
    // command timestamped at or before it. The reconnect path therefore never
    // applies a retained command as catch-up traffic. ESTOP remains exempt.
    native_control_command_freshness_cutoff_at_ms.store(
        error_at_ms,
        std::memory_order_relaxed);
    native_control_websocket_connected.store(false, std::memory_order_relaxed);
    const auto now_monotonic_ms = steady_now_ms();
    const auto last_log_ms =
        native_control_transport_last_error_log_monotonic_ms.load(
            std::memory_order_relaxed);
    if (last_log_ms != 0 &&
        now_monotonic_ms - last_log_ms < kNativeControlDiagnosticInterval.count()) {
      return;
    }
    native_control_transport_last_error_log_monotonic_ms.store(
        now_monotonic_ms,
        std::memory_order_relaxed);
    try {
      emit_diagnostic(
          "vehicle_native_control_websocket_failed",
          protocol_error
              ? "native_control_websocket_protocol_failed"
              : "native_control_websocket_connection_failed",
          "native_control_signaling",
          error,
          "Check signaling-server reachability and protocol compatibility; the independent vehicle watchdog will withdraw traction if fresh commands do not resume.",
          true,
          {{"transport", "native_signaling_websocket"},
           {"protocol_error", protocol_error},
           {"consecutive_errors", consecutive},
           {"errors_total", total},
           {"safety_action", "local_watchdog_safe_stop"}});
    } catch (...) {
      // Observability must not terminate the receiver or watchdog threads.
    }
  }

  void handle_native_control_websocket_envelope(
      WebSocketClient& websocket,
      const Json& envelope) {
    Json native_feedback = Json::object();
    const auto envelope_received_at_utc_ms = signaling.now_ms();
    const auto envelope_received_monotonic_ms = steady_now_ms();
    if (!envelope.is_object()) {
      throw std::invalid_argument("native control WebSocket envelope must be an object");
    }
    const auto event = envelope.value("event", "");
    if (event == "signaling_authority_lost") {
      std::lock_guard lock(control_mutex);
      if (direct_authority.configured()) {
        if (direct_authority.observe_authenticated_cloud_denial(envelope, native_control_websocket_authority_scope)) {
          brake_direct_authority_locked();
        }
        return; // A stale/duplicate denial also must not become a new connection fault.
      }
    }
    if (envelope.contains("error")) {
      // A cloud-declared rejection is not an absent/unreachable transport.
      { std::lock_guard lock(control_mutex);
        auto denial = envelope; denial["event"] = "signaling_authority_lost";
        if (direct_authority.observe_authenticated_cloud_denial(denial, native_control_websocket_authority_scope)) {
          brake_direct_authority_locked();
        }
      }
      throw std::runtime_error(
          envelope.value("error", "native control WebSocket was rejected"));
    }
    if (event == "control_authority_lease") {
      signaling.observe_lease(envelope.at("expires_at_utc_ms").get<std::int64_t>());
      return;
    }
    if (event == "signaling_delivery_acknowledged") {
      native_control_delivery_acknowledgements_total.fetch_add(
          1,
          std::memory_order_relaxed);
      return;
    }
    if (event != "signaling_messages") {
      throw std::invalid_argument(
          "native control WebSocket returned an unexpected event");
    }
    const auto delivery_cursor =
        envelope.at("delivery_cursor").get<std::uint64_t>();
    if (delivery_cursor == 0) {
      throw std::invalid_argument(
          "native control WebSocket delivery cursor must be positive");
    }
    const auto& messages = envelope.at("messages");
    if (!messages.is_array() || messages.empty()) {
      throw std::invalid_argument(
          "native control WebSocket messages must be a non-empty array");
    }

    native_control_websocket_envelopes_total.fetch_add(1, std::memory_order_relaxed);
    native_control_websocket_messages_total.fetch_add(
        messages.size(),
        std::memory_order_relaxed);
    std::optional<Json> newest;
    std::optional<Json> newest_estop;
    std::uint64_t newest_cursor = 0;
    std::uint64_t newest_estop_cursor = 0;
    std::size_t valid_messages = 0;
    std::string first_protocol_issue;
    for (const auto& message : messages) {
      try {
        if (!message.is_object() ||
            message.value("type", "") != "control_command") {
          throw std::invalid_argument("unexpected message type");
        }
        const auto message_cursor =
            message.at("delivery_cursor").get<std::uint64_t>();
        if (message_cursor == 0 || message_cursor > delivery_cursor) {
          throw std::invalid_argument("invalid message delivery cursor");
        }
        const auto& payload = message.at("payload");
        if (!payload.is_object()) {
          throw std::invalid_argument("control payload must be an object");
        }
        const auto command = ControlCommand::from_json(payload);
        const auto intent_seq = payload.at("intent_seq").get<std::uint64_t>();
        if (intent_seq == 0) {
          throw std::invalid_argument("intent_seq must be positive");
        }
        if (!payload.contains("intent_fresh") ||
            !payload.at("intent_fresh").is_boolean()) {
          throw std::invalid_argument("intent_fresh must be a boolean");
        }
        ++valid_messages;
        if (message_cursor >= newest_cursor) {
          newest = message;
          newest_cursor = message_cursor;
        }
        if (command.estop && message_cursor >= newest_estop_cursor) {
          newest_estop = message;
          newest_estop_cursor = message_cursor;
        }
      } catch (const std::exception& error) {
        if (first_protocol_issue.empty()) first_protocol_issue = error.what();
      }
    }
    const auto selected = newest_estop ? newest_estop : newest;
    if (!selected) {
      throw std::invalid_argument(
          first_protocol_issue.empty()
              ? "native control WebSocket contained no valid control command"
              : "invalid native control WebSocket message: " + first_protocol_issue);
    }
    if (valid_messages > 1) {
      native_control_websocket_superseded_messages_total.fetch_add(
          valid_messages - 1,
          std::memory_order_relaxed);
    }
    if (!first_protocol_issue.empty() && !newest_estop) {
      // A protocol-anomalous batch cannot authorize ordinary actuation. Only
      // a valid ESTOP is allowed to cross that error boundary below.
      throw std::invalid_argument(
          "native control WebSocket contained an invalid sibling message: " +
          first_protocol_issue);
    }

    const auto& payload = selected->at("payload");
    const auto command_sent_at_ms =
        payload.at("sent_at_utc_ms").get<std::int64_t>();
    const auto replay_cutoff_ms =
        native_control_command_freshness_cutoff_at_ms.load(
            std::memory_order_relaxed);
    if (!payload.value("estop", false) && replay_cutoff_ms > 0 &&
        command_sent_at_ms <= replay_cutoff_ms) {
      native_control_websocket_post_error_discards_total.fetch_add(
          1,
          std::memory_order_relaxed);
      if (config.runtime.control_log_commands) {
        try {
          enqueue_control_trace({
              {"stage", "post_transport_error_discarded"},
              {"protocol_version", payload.value("protocol_version", "")},
              {"vehicle_id", bounded_control_trace_text(payload.value("vehicle_id", ""))},
              {"driver_id", bounded_control_trace_text(payload.value("driver_id", ""))},
              {"session_id", bounded_control_trace_text(payload.value("session_id", ""))},
              {"trace_session_id", bounded_control_trace_text(payload.value("session_id", ""))},
              {"transport", "native_signaling_websocket"},
              {"seq", payload.value("seq", std::uint64_t{0})},
              {"intent_seq", payload.value("intent_seq", std::uint64_t{0})},
              {"delivery_cursor", selected->value("delivery_cursor", std::uint64_t{0})},
              {"sent_at_utc_ms", command_sent_at_ms},
              {"cloud_queued_at_utc_ms", selected->value("queued_at_utc_ms", std::int64_t{0})},
              {"vehicle_envelope_received_at_utc_ms", envelope_received_at_utc_ms},
              {"vehicle_envelope_received_monotonic_ms", envelope_received_monotonic_ms},
              {"accepted", false},
              {"reason", "post_transport_error_freshness_cutoff"},
          });
        } catch (...) {
          note_control_trace_drop();
        }
      }
    } else {
      const NativeControlDeliveryTraceContext delivery_trace{
          selected->value("delivery_cursor", std::uint64_t{0}),
          selected->value("queued_at_utc_ms", std::int64_t{0}),
          envelope_received_at_utc_ms,
          envelope_received_monotonic_ms,
          messages.size(),
          valid_messages,
          valid_messages > 0 ? valid_messages - 1 : 0,
      };
      handle_control_message(
          nullptr,
          payload.dump(),
          ControlMessageTransport::NativeSignaling,
          &delivery_trace, &native_feedback);
    }

    // Acknowledge the delivered view even when its ordinary command was
    // intentionally discarded. This prevents reconnect from replaying an old
    // mailbox entry; a newer controller sample is required to resume control.
    const auto ack_send_started_at_utc_ms = signaling.now_ms();
    const auto ack_send_started_monotonic_ms = steady_now_ms();
    try {
      Json delivery_ack = {{"event", "signaling_delivery_ack"},
                           {"delivery_cursor", delivery_cursor},
                           {"trace_session_id", payload.value("session_id", "")},
                           {"seq", payload.value("seq", std::uint64_t{0})},
                           {"intent_seq", payload.value("intent_seq", std::uint64_t{0})}};
      if (!native_feedback.empty()) delivery_ack["control_feedback"] = native_feedback;
      websocket.send_json(delivery_ack, kNativeControlWebSocketSendTimeout);
    } catch (const std::exception& error) {
      if (config.runtime.control_log_commands) {
        try {
          enqueue_control_trace({
              {"stage", "delivery_ack_send_failed"},
              {"vehicle_id", bounded_control_trace_text(config.vehicle_id)},
              {"driver_id", bounded_control_trace_text(payload.value("driver_id", ""))},
              {"session_id", bounded_control_trace_text(payload.value("session_id", ""))},
              {"trace_session_id", bounded_control_trace_text(payload.value("session_id", ""))},
              {"seq", payload.value("seq", std::uint64_t{0})},
              {"intent_seq", payload.value("intent_seq", std::uint64_t{0})},
              {"delivery_cursor", delivery_cursor},
              {"vehicle_ack_send_started_at_utc_ms", ack_send_started_at_utc_ms},
              {"vehicle_ack_send_started_monotonic_ms", ack_send_started_monotonic_ms},
              {"vehicle_ack_send_failed_at_utc_ms", signaling.now_ms()},
              {"vehicle_ack_send_failed_monotonic_ms", steady_now_ms()},
              {"error", bounded_control_trace_text(error.what())},
          });
        } catch (...) {
          note_control_trace_drop();
        }
      }
      throw;
    }
    const auto ack_send_completed_at_utc_ms = signaling.now_ms();
    const auto ack_send_completed_monotonic_ms = steady_now_ms();
    if (config.runtime.control_log_commands) {
      try {
        Json acknowledgement_trace = {
            {"stage", "delivery_ack_sent"},
            {"vehicle_feedback_present", !native_feedback.empty()},
            {"vehicle_feedback_rearm_required", native_feedback.value("rearm_required", false)},
            {"vehicle_feedback_status_seq", native_feedback.value("control_status_seq", std::uint64_t{0})},
            {"vehicle_id", bounded_control_trace_text(config.vehicle_id)},
            {"driver_id", bounded_control_trace_text(payload.value("driver_id", ""))},
            {"session_id", bounded_control_trace_text(payload.value("session_id", ""))},
            {"trace_session_id", bounded_control_trace_text(payload.value("session_id", ""))},
            {"seq", payload.value("seq", std::uint64_t{0})},
            {"intent_seq", payload.value("intent_seq", std::uint64_t{0})},
            {"delivery_cursor", delivery_cursor},
            {"vehicle_ack_send_started_at_utc_ms", ack_send_started_at_utc_ms},
            {"vehicle_ack_send_started_monotonic_ms", ack_send_started_monotonic_ms},
            {"vehicle_ack_send_completed_at_utc_ms", ack_send_completed_at_utc_ms},
            {"vehicle_ack_send_completed_monotonic_ms", ack_send_completed_monotonic_ms},
            {"vehicle_ack_send_call_ms", std::max<std::int64_t>(
                                                    0,
                                                    ack_send_completed_monotonic_ms -
                                                        ack_send_started_monotonic_ms)},
        };
        try {
          auto transport = websocket.transport_diagnostics();
          transport["peer_is_loopback"] = transport.contains("peer_address")
              ? Json(is_loopback_bind_address(transport.at("peer_address").get<std::string>())) : Json(nullptr);
          transport.erase("peer_address");
          acknowledgement_trace["transport_after_ack_send"] = std::move(transport);
        } catch (...) { acknowledgement_trace["transport_sample_unavailable"] = true; }
        enqueue_control_trace(std::move(acknowledgement_trace));
      } catch (...) {
        note_control_trace_drop();
      }
    }
    native_control_delivery_acks_sent_total.fetch_add(1, std::memory_order_relaxed);
    native_control_websocket_last_message_at_ms.store(
        signaling.now_ms(),
        std::memory_order_relaxed);
    if (!first_protocol_issue.empty()) {
      // A valid ESTOP above is deliberately applied before an anomalous
      // sibling causes a freshness barrier and reconnect.
      throw std::invalid_argument(
          "native control WebSocket contained an invalid sibling message: " +
          first_protocol_issue);
    }
  }

  bool wait_for_native_control_reconnect(
      std::stop_token stop_token,
      std::chrono::milliseconds delay) const noexcept {
    auto remaining = delay;
    while (remaining.count() > 0 && !stop_token.stop_requested() &&
           !stop_requested.load()) {
      const auto slice = std::min(remaining, std::chrono::milliseconds(25));
      std::this_thread::sleep_for(slice);
      remaining -= slice;
    }
    return !stop_token.stop_requested() && !stop_requested.load();
  }

  void native_control_websocket_loop(std::stop_token stop_token) noexcept {
    auto reconnect_delay = kNativeControlReconnectInitialDelay;
    while (!stop_token.stop_requested() && !stop_requested.load()) {
      native_control_websocket_connection_attempts_total.fetch_add(
          1,
          std::memory_order_relaxed);
      try {
        WebSocketClient websocket(
            kNativeControlWebSocketConnectTimeout,
            config.cloud.resolve_entries,
            config.cloud.ca_bundle);
        websocket.enable_read_diagnostics(config.runtime.control_log_commands);
        { std::lock_guard lock(control_mutex); native_control_websocket_authority_scope = direct_authority.scope(); }
        websocket.connect(
            signaling.native_control_websocket_url(),
            signaling.native_control_websocket_headers());
        if (stop_token.stop_requested() || stop_requested.load()) break;
        native_control_transport_cloud_capable.store(signaling.transport_mac_key().size() >= 32 && websocket.response_header("x-mine-teleop-transport-capabilities") == detail::kControlTransportCapability);
        native_control_websocket_connected.store(true, std::memory_order_relaxed);
        std::uint64_t last_result_sent = 0;
        const auto connections =
            native_control_websocket_connections_total.fetch_add(
                1,
                std::memory_order_relaxed) +
            1;
        if (connections > 1) {
          native_control_websocket_reconnects_total.fetch_add(
              1,
              std::memory_order_relaxed);
        }
        native_control_transport_consecutive_errors.store(0, std::memory_order_relaxed);
        native_control_websocket_last_connected_at_ms.store(
            signaling.now_ms(),
            std::memory_order_relaxed);
        std::int64_t previous_receive_completed_ms = steady_now_ms();
        std::int64_t next_idle_diagnostic_ms = 0;
        while (!stop_token.stop_requested() && !stop_requested.load()) {
          if (native_control_transport_cloud_capable.load()) {
            Json result;
            std::map<std::string, Json> statuses;
            { std::lock_guard result_lock(control_mutex); result = latest_control_result; statuses.swap(pending_control_status); }
            std::vector<Json> ordered_status;
            for (const auto& [event, status] : statuses) { static_cast<void>(event); ordered_status.push_back(status); }
            std::sort(ordered_status.begin(), ordered_status.end(), [](const Json& left, const Json& right) {
              return (left.contains("status") ? left.at("status") : left).at("control_status_seq").get<std::uint64_t>() <
                  (right.contains("status") ? right.at("status") : right).at("control_status_seq").get<std::uint64_t>(); });
            for (const auto& status : ordered_status) {
              if (status.value("event", "") == "vehicle_control_status") websocket.send_json(status, kNativeControlWebSocketSendTimeout);
              else websocket.send_json({{"event", "vehicle_control_status"}, {"protocol", detail::kControlTransportCapability},
                  {"session_id", signaling.session_id()}, {"vehicle_id", config.vehicle_id}, {"driver_id", signaling.driver_id()},
                  {"status", status}}, kNativeControlWebSocketSendTimeout);
            }
            if (result.is_object() && result.value("result_seq", std::uint64_t{0}) > last_result_sent) {
              websocket.send_json(result, kNativeControlWebSocketSendTimeout);
              last_result_sent = result.at("result_seq").get<std::uint64_t>();
            }
          }
          const auto receive_started_ms = steady_now_ms();
          const auto received = websocket.receive_json(
              kNativeControlWebSocketReceiveTimeout);
          const auto receive_completed_ms = steady_now_ms();
          if (config.runtime.control_log_commands &&
              (received.status == WebSocketReceiveStatus::Message || receive_completed_ms >= next_idle_diagnostic_ms ||
               receive_started_ms - previous_receive_completed_ms >= 100)) {
            try {
            Json trace = {{"stage", "websocket_receive_loop"}, {"session_id", signaling.session_id()},
                                   {"vehicle_id", config.vehicle_id},
                                   {"connection_number", connections},
                                   {"websocket_request_id", websocket.response_header("x-request-id").substr(0, 128)},
                                   {"receive_started_monotonic_ms", receive_started_ms},
                                   {"receive_completed_monotonic_ms", receive_completed_ms},
                                   {"between_receive_calls_ms", std::max<std::int64_t>(0, receive_started_ms - previous_receive_completed_ms)},
                                   {"receive_wait_ms", receive_completed_ms - receive_started_ms},
                                   {"json_parse_ms", received.json_parse_ms},
                                   {"read_diagnostics", received.read_diagnostics},
                                   {"message_received", received.status == WebSocketReceiveStatus::Message}};
            try {
              const auto& envelope = received.message;
              if (envelope.is_object() && envelope.contains("delivery_cursor") && envelope.at("delivery_cursor").is_number_unsigned())
                trace["delivery_cursor"] = envelope.at("delivery_cursor");
              if (envelope.is_object() && envelope.contains("messages") && envelope.at("messages").is_array() &&
                  !envelope.at("messages").empty()) {
                const auto& message = envelope.at("messages").front();
                trace["envelope_message_count"] = envelope.at("messages").size();
                if (message.is_object() && message.contains("payload") && message.at("payload").is_object()) {
                  const auto& payload = message.at("payload");
                  if (payload.contains("seq") && payload.at("seq").is_number_unsigned()) trace["first_message_seq"] = payload.at("seq");
                }
              }
              auto transport = websocket.transport_diagnostics(std::chrono::seconds(1));
              if (!transport.is_null()) {
                transport["peer_is_loopback"] = transport.contains("peer_address")
                    ? Json(is_loopback_bind_address(transport.at("peer_address").get<std::string>())) : Json(nullptr);
                transport.erase("peer_address");
                trace["transport"] = std::move(transport);
              }
            } catch (...) { trace["transport_sample_unavailable"] = true; }
            enqueue_control_trace(std::move(trace));
            next_idle_diagnostic_ms = receive_completed_ms + 250;
            } catch (...) { note_control_trace_drop(); }
          }
          previous_receive_completed_ms = receive_completed_ms;
          if (received.status == WebSocketReceiveStatus::Timeout) continue;
          if (received.status == WebSocketReceiveStatus::Closed) {
            throw std::runtime_error("native control WebSocket closed");
          }
          handle_native_control_websocket_envelope(websocket, received.message);
          // A successful upgrade alone is not evidence of a healthy control
          // path: a peer or proxy can accept and immediately close repeatedly.
          // Reset backoff only after one valid control delivery was fully
          // processed (including its delivery ACK).
          if (received.message.value("event", "") == "signaling_messages") {
            reconnect_delay = kNativeControlReconnectInitialDelay;
          }
          native_control_transport_consecutive_errors.store(
              0,
              std::memory_order_relaxed);
        }
        native_control_websocket_connected.store(false, std::memory_order_relaxed);
      } catch (const HttpStatusError& error) {
        if (!stop_token.stop_requested() && !stop_requested.load()) {
          if (!detail::transient_cloud_failure(error)) {
            std::lock_guard lock(control_mutex);
            if (direct_authority.observe_authenticated_cloud_denial({{"event", "signaling_authority_lost"}}, native_control_websocket_authority_scope)) {
              brake_direct_authority_locked();
            }
          }
          note_native_control_transport_error(error.what(), false);
        }
      } catch (const std::invalid_argument& error) {
        if (!stop_token.stop_requested() && !stop_requested.load()) {
          note_native_control_transport_error(error.what(), true);
        }
      } catch (const std::exception& error) {
        if (!stop_token.stop_requested() && !stop_requested.load()) {
          note_native_control_transport_error(error.what(), false);
        }
      } catch (...) {
        if (!stop_token.stop_requested() && !stop_requested.load()) {
          note_native_control_transport_error(
              "unknown native control WebSocket failure",
              false);
        }
      }
      native_control_websocket_connected.store(false, std::memory_order_relaxed);
      if (!wait_for_native_control_reconnect(stop_token, reconnect_delay)) break;
      reconnect_delay = std::min(
          reconnect_delay * 2,
          kNativeControlReconnectMaximumDelay);
    }
    native_control_websocket_connected.store(false, std::memory_order_relaxed);
  }

  void native_control_watchdog_loop(std::stop_token stop_token) noexcept {
    auto next_tick = std::chrono::steady_clock::now();
    while (!stop_token.stop_requested() && !stop_requested.load()) {
      const auto before_wait = std::chrono::steady_clock::now();
      if (before_wait < next_tick) std::this_thread::sleep_until(next_tick);
      if (stop_token.stop_requested() || stop_requested.load()) break;
      next_tick += kNativeControlWatchdogInterval;
      native_control_watchdog_ticks_total.fetch_add(1, std::memory_order_relaxed);
      try {
        enforce_control_recovery();
        tick_control_service();
      } catch (...) {
        // tick_control_service owns its fail-safe shutdown and diagnostics.
      }
      const auto completed = std::chrono::steady_clock::now();
      if (completed > next_tick) {
        const auto overdue_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(completed - next_tick)
                .count();
        const auto skipped =
            overdue_ms / kNativeControlWatchdogInterval.count() + 1;
        native_control_watchdog_skipped_intervals_total.fetch_add(
            static_cast<std::uint64_t>(skipped),
            std::memory_order_relaxed);
        next_tick += kNativeControlWatchdogInterval * skipped;
      }
    }
  }

  void time_sync_loop(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested() && !stop_requested.load()) {
      const auto poll_ms = signaling.time_sync_status().acceptable(config.field_safety.max_time_sync_uncertainty_ms) ? 500 : 100;
      if (!wait_for_native_control_reconnect(stop_token, std::chrono::milliseconds(poll_ms))) break;
      if (!signaling.time_sync_refresh_due(config.field_safety.time_sync_interval_ms)) continue;
      const auto started = steady_now_ms();
      std::string error;
      Json http_failure;
      Json sample_failure;
      try {
        static_cast<void>(signaling.synchronize_time(config.field_safety.time_sync_samples,
                                                    config.field_safety.max_time_sync_uncertainty_ms));
      } catch (const std::exception& failure) {
        signaling.mark_time_sync_failed();
        error = std::string(failure.what()).substr(0, 512);
        if (const auto* transport = dynamic_cast<const HttpTransportError*>(&failure)) http_failure = transport->timing();
        if (const auto* sample = dynamic_cast<const TimeSyncSampleError*>(&failure)) { sample_failure = sample->diagnostics(); http_failure = sample_failure.value("http_failure", Json(nullptr)); }
      }
      try {
        enforce_control_recovery();
        enqueue_control_trace({{"stage", "time_sync_refresh"}, {"session_id", signaling.session_id()},
            {"duration_ms", steady_now_ms() - started}, {"time_sync", signaling.time_sync_status().to_json(true)},
            {"error", error}, {"http_failure", std::move(http_failure)}, {"sample_failure", std::move(sample_failure)}, {"safety_action", "recovery_guard_owns_control"}});
      } catch (const std::exception& failure) {
        set_pipeline_error(failure.what(), "control_recovery_brake_failed", "control_recovery",
                           "Keep the vehicle stopped and inspect the adapter/CAN failure.", false);
      }
    }
  }

  void start_native_control_transport() {
    if (stop_requested.load()) return;
    if (native_control_websocket_thread.joinable() ||
        native_control_watchdog_thread.joinable() || time_sync_worker.joinable()) {
      throw std::logic_error("native control transport threads are already running");
    }
    // Media-only deployments still refresh their capture timestamp clock.
    time_sync_worker = std::jthread([this](std::stop_token token) { time_sync_loop(token); });
    if (!config.runtime.control_enabled) return;
    if (config.runtime.experimental_direct_authority_max_ms > 0) {
      const auto& authority = signaling.direct_authority_configuration();
      if (!authority.is_object() || authority.value("protocol", "") != detail::kDirectAuthorityCapability ||
          authority.at("maximum_duration_ms").get<int>() > config.runtime.experimental_direct_authority_max_ms ||
          config.runtime.experimental_direct_authority_max_ms > config.control.control_timeout_ms)
        throw std::invalid_argument("experimental direct authority capability/budget mismatch");
      signaling.bound_direct_session_requests(authority.at("maximum_duration_ms").get<int>());
      direct_authority.configure(authority.at("issuer").get<std::string>(), config.vehicle_id, signaling.driver_id(), signaling.session_id(),
          authority.at("connection_generation").get<std::uint64_t>(), authority.at("maximum_duration_ms").get<int>());
      direct_authority_stop_failed = false;
      direct_authority_worker = std::jthread([this](std::stop_token token) { direct_authority_loop(token); });
    }
    {
      std::lock_guard lock(control_mutex);
      native_control_last_accepted_fresh_gear.reset();
      native_control_estop_frozen_gear.reset();
      native_control_last_accepted_monotonic_ms = 0;
    }
    native_control_transport_consecutive_errors.store(0, std::memory_order_relaxed);
    native_control_command_freshness_cutoff_at_ms.store(
        signaling.now_ms(),
        std::memory_order_relaxed);
    native_control_watchdog_thread = std::jthread(
        [this](std::stop_token stop_token) {
          native_control_watchdog_loop(stop_token);
        });
    try {
      native_control_websocket_thread = std::jthread(
          [this](std::stop_token stop_token) {
            native_control_websocket_loop(stop_token);
          });
    } catch (...) {
      stop_native_control_transport_threads();
      throw;
    }
  }

  void stop_native_control_transport_threads() {
    if (direct_authority_worker.joinable()) direct_authority_worker.request_stop();
    if (time_sync_worker.joinable()) time_sync_worker.request_stop();
    if (native_control_websocket_thread.joinable()) {
      native_control_websocket_thread.request_stop();
    }
    if (native_control_watchdog_thread.joinable()) {
      native_control_watchdog_thread.request_stop();
    }
    if (native_control_websocket_thread.joinable()) {
      native_control_websocket_thread.join();
    }
    if (native_control_watchdog_thread.joinable()) {
      native_control_watchdog_thread.join();
    }
    if (time_sync_worker.joinable()) time_sync_worker.join();
    if (direct_authority_worker.joinable()) direct_authority_worker.join();
  }

  // Caller owns control_mutex. A failed physical stop never escapes a worker
  // catch or becomes a claimed success; the existing tick fault path closes it.
  void brake_direct_authority_locked() noexcept {
    try {
      invalidate_native_control_trusted_gear_locked();
      if (control_service_started && control_service)
        control_service->set_control_braking_wait(true, signaling.now_ms(), ControlRecoveryCause::ControlLink);
    } catch (...) {
      direct_authority.revoke(); direct_authority_stop_failed = true;
    }
  }

  void mark_direct_authority_worker_failed() noexcept {
    try {
      std::lock_guard lock(control_mutex);
      direct_authority.revoke(); direct_authority_stop_failed = true;
      invalidate_native_control_trusted_gear_locked();
    } catch (...) { control_inhibited.store(true); }
  }

  void direct_authority_loop(std::stop_token token) noexcept {
    try {
    const auto duration = signaling.direct_authority_configuration().at("maximum_duration_ms").get<int>();
    std::int64_t next_attempt_ms = 0;
    while (!token.stop_requested() && !stop_requested.load()) {
      bool due;
      { std::lock_guard lock(control_mutex); due = direct_authority.renewal_due(steady_now_ms()) && steady_now_ms() >= next_attempt_ms; }
      if (due) {
        const auto nonce = random_token(12);
        try {
          std::uint64_t retired_epoch = 0;
          { std::lock_guard lock(control_mutex);
            if (!direct_authority.valid(steady_now_ms()) && direct_authority.reference().is_object()) retired_epoch = direct_authority.reference().value("authority_epoch", std::uint64_t{0});
            direct_authority.begin_request(nonce, steady_now_ms()); }
          next_attempt_ms = steady_now_ms() + duration / 3;
          const auto response = signaling.renew_direct_authority(nonce, config.runtime.experimental_direct_authority_max_ms, retired_epoch);
          const auto control_lease_expiry = detail::direct_authority_control_lease_expiry(response);
          const auto clock = signaling.time_sync_status();
          std::lock_guard lock(control_mutex);
          const auto revision = direct_authority.rearm_revision();
          const bool accepted = clock.acceptable(config.field_safety.max_time_sync_uncertainty_ms) &&
              direct_authority.accept_authenticated_cloud(response, steady_now_ms(), signaling.now_ms(), clock.uncertainty_ms);
          if (accepted) { signaling.observe_lease(control_lease_expiry); next_attempt_ms = 0; }
          // A delayed or conservatively expired response grants nothing. The
          // next authenticated request retires the observed expired epoch;
          // only a new epoch can recover the vehicle after that boundary.
          if (accepted && revision != direct_authority.rearm_revision() && control_service_started && control_service) {
            brake_direct_authority_locked();
          }
        } catch (const HttpStatusError& error) {
          // An authenticated denial is a revocation, never a network grace.
          if (error.status() == 401 || error.status() == 403 || error.status() == 404 || error.status() == 409) {
            std::lock_guard lock(control_mutex); direct_authority.revoke();
            brake_direct_authority_locked();
          }
        } catch (const HttpTransportError&) {
          // No refresh. The independent watchdog enforces the old deadline.
        } catch (const std::exception&) {
          std::lock_guard lock(control_mutex); direct_authority.revoke();
          brake_direct_authority_locked();
        }
      }
      // Existing watchdog cadence, not a cloud liveness heartbeat/deadline.
      std::this_thread::sleep_for(kNativeControlWatchdogInterval);
    }
    } catch (...) { mark_direct_authority_worker_failed(); }
  }

  std::uint64_t send_control_command_rejected_locked(
      std::uint64_t command_seq,
      std::uint64_t intent_seq,
      std::string_view issue_code) {
    const std::string stable_issue_code =
        issue_code == "vcu_drive_gear_change_moving_or_stale"
        ? "vcu_drive_gear_change_moving_or_stale"
        : issue_code == "control_input_rearm_required"
        ? "control_input_rearm_required"
        : "vcu_control_apply_rejected";
    const auto timestamp_ms = signaling.now_ms();
    if (stable_issue_code == last_control_rejection_status_issue_code &&
        last_control_rejection_status_ms &&
        timestamp_ms - *last_control_rejection_status_ms < 500) {
      return last_control_rejection_status_seq;
    }
    last_control_rejection_status_issue_code = stable_issue_code;
    last_control_rejection_status_ms = timestamp_ms;
    const auto payload = Json({
        {"event", "control_command_rejected"},
        {"protocol_version", kProtocolVersion},
        {"vehicle_id", config.vehicle_id},
        {"driver_id", signaling.driver_id()},
        {"session_id", signaling.session_id()},
        {"control_status_seq", last_control_rejection_status_seq = ++control_status_seq},
        {"command_seq", command_seq},
        {"intent_seq", intent_seq},
        {"accepted", false},
        {"issue_code", stable_issue_code},
    });
    send_control_status_locked(payload);
    return last_control_rejection_status_seq;
  }

  void send_vcu_handshake_status(std::string_view result) {
    std::lock_guard lock(control_mutex);
    send_vcu_handshake_status_locked(result);
  }

  void send_control_status_locked(Json message) {
    if (config.runtime.experimental_direct_authority_max_ms > 0) {
      const auto kind = message.value("event", "");
      if (kind != "vehicle_telemetry" && kind != "vcu_handshake_status" && kind != "session_control_profile_status" && kind != "control_command_rejected") return;
      if ((message.contains("session_id") && message.at("session_id") != signaling.session_id()) ||
          (message.contains("vehicle_id") && message.at("vehicle_id") != config.vehicle_id) ||
          (message.contains("driver_id") && message.at("driver_id") != signaling.driver_id())) return;
      message["session_id"] = signaling.session_id(); message["vehicle_id"] = config.vehicle_id; message["driver_id"] = signaling.driver_id();
      latest_feedback_status[kind] = message;
      if (feedback_transport_fence.negotiated()) {
        Json wrapper = {{"event", "vehicle_control_status"}, {"protocol", detail::kControlTransportCapability},
            {"session_id", signaling.session_id()}, {"vehicle_id", config.vehicle_id}, {"driver_id", signaling.driver_id()},
            {"feedback_transport", feedback_transport_fence.state()}, {"status", std::move(message)}};
        wrapper["vehicle_mac"] = detail::transport_message_mac(wrapper, signaling.transport_mac_key(), true);
        if (feedback_transport_fence.path() == detail::ControlPath::DataChannel && control_channel && control_link_open)
          gst_webrtc_data_channel_send_string(control_channel, wrapper.dump().c_str());
        else if (feedback_transport_fence.path() == detail::ControlPath::Wss) pending_control_status[kind] = std::move(wrapper);
        return;
      }
    }
    if (control_channel != nullptr && control_link_open &&
        (!control_transport_fence.negotiated() || control_transport_fence.path() == detail::ControlPath::DataChannel)) {
      gst_webrtc_data_channel_send_string(control_channel, message.dump().c_str());
    } else if (control_transport_fence.negotiated() || cloud_control_authority_live_locked()) {
      if ((message.contains("session_id") && message.at("session_id") != signaling.session_id()) ||
          (message.contains("vehicle_id") && message.at("vehicle_id") != config.vehicle_id) ||
          (message.contains("driver_id") && message.at("driver_id") != signaling.driver_id())) return;
      const auto event = message.value("event", "unknown");
      pending_control_status[event] = std::move(message);
    }
  }

  void send_latest_vehicle_telemetry_locked() {
    if ((!control_link_open && !control_transport_fence.negotiated() && !cloud_control_authority_live_locked()) ||
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
    send_control_status_locked(std::move(payload_value));
    last_vehicle_telemetry_seq = sequence;
  }

  void send_session_control_profile_status_locked(
      const SessionControlProfileResult& result) {
    if (!control_link_open && !control_transport_fence.negotiated() && !cloud_control_authority_live_locked()) return;
    auto message = result.to_json();
    message["event"] = "session_control_profile_status";
    message["control_status_seq"] = ++control_status_seq;
    if (control_service_started && control_service) {
      message["hard_limits"] = control_service->control_limits();
      message["session_control_profile"] =
          control_service->session_control_profile();
    }
    send_control_status_locked(std::move(message));
  }

  [[nodiscard]] Json configured_control_limits() const {
    return vehicle_control_limits(config);
  }

  void send_vcu_handshake_status_locked(std::string_view result) {
    if (!control_link_open && !control_transport_fence.negotiated() && !cloud_control_authority_live_locked()) return;
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
      bool handshake_admission_ready = status.ready;
      if (control_service_started && control_service &&
          config.field_safety.commissioning_mode == "bench" &&
          control_service->adapter_status().adapter_type == "mock") {
        handshake_admission_ready = true;
      }
      if (!handshake_admission_ready) {
        invalidate_native_control_trusted_gear_locked();
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
      if (control_service_issue_code == "control_recovery_braking_wait" ||
          control_service_issue_code == "control_recovery_timeout" || control_service_issue_code == "critical_camera_failed") {
        message["control_recovery"] = control_recovery_latch->recovery_status_for(signaling.session_id());
      }
      send_control_status_locked(std::move(message));
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
      invalidate_native_control_trusted_gear_locked();
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

  [[nodiscard]] std::string_view recovery_inhibition_issue() const {
    return control_recovery_latch->cause_for(signaling.session_id()) == ControlRecoveryCause::CriticalVideo
        ? "critical_camera_failed" : "control_recovery_timeout";
  }

  void inhibit_control_for_recovery(const Lane* lane, std::string_view failure) {
    if (control_inhibited.exchange(true)) return;
    const auto cause = control_recovery_latch->cause_for(signaling.session_id());
    const std::string issue(recovery_inhibition_issue());

    std::optional<std::string> latch_error;
    try {
      static_cast<void>(control_recovery_latch->inhibit(signaling.session_id()));
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
      control_service_issue_code = issue;
      invalidate_native_control_trusted_gear_locked();
      if (control_service_started && control_service) {
        try {
          control_service->close({
              (cause == ControlRecoveryCause::InputLease || cause == ControlRecoveryCause::ControlLink)
                  ? VehicleStopSource::Watchdog : VehicleStopSource::SoftwareFault,
              cause == ControlRecoveryCause::CriticalVideo ? VehicleStopReason::CriticalCameraFailed
                  : (cause == ControlRecoveryCause::InputLease || cause == ControlRecoveryCause::ControlLink) ? VehicleStopReason::OuterControlTimeout
                  : VehicleStopReason::MediaPipelineFailed});
        } catch (const std::exception& error) {
          close_error = error.what();
        }
      }
      control_service_started = false;
      control_service.reset();
      send_vcu_handshake_status_locked(issue);
      if (control_channel != nullptr) {
        channel_to_close = GST_WEBRTC_DATA_CHANNEL(g_object_ref(control_channel));
      }
    }

    const auto last_encoded_ms = lane ? lane->last_encoded_steady_ms.load() : 0;
    emit_diagnostic(
        "vehicle_control_recovery_timeout",
        issue,
        "control_recovery",
        std::string(failure),
        "Keep the vehicle stopped; the recovery deadline expired. End this session and complete a fresh VCU handshake before driving again.",
        false,
        {{"camera_id", lane ? lane->camera.id : ""},
         {"device", lane ? lane->camera.device : ""},
         {"recovery_cause", to_string(cause)},
         {"safety_action", "local_full_stop_control_channel_closed_video_continues"},
         {"captured_frames", lane ? lane->captured.load() : 0},
         {"encoded_frames", lane ? lane->encoded.load() : 0},
         {"last_encoded_age_ms", last_encoded_ms > 0
             ? Json(steady_now_ms() - last_encoded_ms) : Json(nullptr)},
         {"startup_timeout_ms", kCriticalCameraStartupTimeoutMs}});
    if (latch_error) {
      emit_diagnostic(
          "vehicle_control_inhibition_latch_failed",
          "control_recovery_latch_failed",
          "camera_safety",
          *latch_error,
          "Keep the vehicle stopped, end the current session, and do not resume control until a fresh session and VCU handshake succeed.",
          false,
          {{"camera_id", lane ? lane->camera.id : ""},
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
          {{"camera_id", lane ? lane->camera.id : ""},
           {"safety_action", "physical_estop_required_video_continues"}});
    }
    if (channel_to_close != nullptr) {
      gst_webrtc_data_channel_close(channel_to_close);
      g_object_unref(channel_to_close);
    }
  }

  [[nodiscard]] TimeSyncReadinessState vehicle_time_readiness() const {
    return vehicle_time_sync_readiness_gate.observe(signaling.time_sync_status(),
        config.field_safety.max_time_sync_uncertainty_ms, steady_now_ms(),
        TimeSyncReadinessGate::candidate_timeout_ms(config.field_safety.time_sync_interval_ms,
            config.field_safety.time_sync_samples));
  }

  [[nodiscard]] bool critical_cameras_ready() const {
    if (config.field_safety.require_time_sync && !vehicle_time_readiness().ready) return false;
    if (control_recovery_latch->braking_wait_for(signaling.session_id())) return false;
    return std::all_of(lanes.begin(), lanes.end(), [&](const auto& lane) {
      return !lane->camera.critical_for_control ||
          (!lane->disabled.load() && !lane->camera_faulted.load() && lane->send_freshness &&
           !lane->send_freshness->invalid_sample_blocked.load() &&
           lane->send_freshness->consecutive_anomalies.load() < 3 &&
           camera_encoded_frame_fresh(lane->send_freshness->last_fresh_frame_ms.load(),
               steady_now_ms(), lane->send_freshness->max_age_ms.load()));
    });
  }

  [[nodiscard]] std::pair<int, int> recovery_timing_configured() {
    std::lock_guard lock(control_mutex);
    return control_service_started && control_service ? control_service->recovery_timing()
        : std::pair{config.runtime.critical_camera_stale_brake_ms, config.runtime.control_recovery_timeout_ms};
  }

  void apply_control_recovery_phase(ControlRecoveryPhase phase, const Lane* failed_lane = nullptr, bool new_wait = false) {
    if (phase == ControlRecoveryPhase::Locked) {
      const auto lane = failed_lane ? failed_lane : [&]() -> const Lane* {
        for (const auto& candidate : lanes) if (candidate->camera.critical_for_control) return candidate.get();
        return nullptr;
      }();
      inhibit_control_for_recovery(lane, "control readiness did not recover before the braking-wait deadline");
      return;
    }
    const auto [stale_brake_ms, recovery_timeout_ms] = recovery_timing_configured();
    const bool waiting = phase == ControlRecoveryPhase::BrakingWait;
    if (new_wait) for (const auto& lane : lanes)
      lane->video_wait_encoded_baseline = lane->send_freshness ? lane->send_freshness->fresh_frames.load() : 0;
    bool changed = false;
    bool soft_brake_request_active = false;
    bool soft_brake_applied_this_transition = false;
    Json control_snapshot;
    Json critical_lanes;
    std::int64_t sampled_at_monotonic_ms = 0;
    const auto sample_readiness = [&] {
      sampled_at_monotonic_ms = steady_now_ms();
      critical_lanes = Json::array();
      for (const auto& lane : lanes) {
        if (!lane->camera.critical_for_control) continue;
        const auto last_capture = lane->last_capture_steady_ms.load();
        const auto last_encoded = lane->last_encoded_steady_ms.load();
        critical_lanes.push_back({{"camera_id", lane->camera.id}, {"device", lane->camera.device},
            {"recording", recording_status_for(*lane)},
          {"captured_frames", lane->captured.load()}, {"encoded_frames", lane->encoded.load()},
            {"last_capture_age_ms", last_capture > 0 ? Json(std::max<std::int64_t>(0, sampled_at_monotonic_ms - last_capture)) : Json(nullptr)},
            {"last_encoded_age_ms", last_encoded > 0 ? Json(std::max<std::int64_t>(0, sampled_at_monotonic_ms - last_encoded)) : Json(nullptr)},
            {"recovery_encoded_baseline", lane->video_wait_encoded_baseline.load()},
            {"encoded_queue_backpressure_count", lane->encoded_queue_backpressure.load()},
          {"send_freshness_scope", "capture_pts_to_send_queue_source_monotonic_not_network_delivery"},
          {"fresh_send_frames", lane->send_freshness == nullptr ? 0 : lane->send_freshness->fresh_frames.load()},
          {"send_waiting_idr", lane->send_freshness != nullptr && lane->send_freshness->blocked.load()},
          {"send_consecutive_anomalies", lane->send_freshness == nullptr ? 0 : lane->send_freshness->consecutive_anomalies.load()},
          {"send_invalid_sample_blocked", lane->send_freshness != nullptr && lane->send_freshness->invalid_sample_blocked.load()},
          {"send_last_sample_id_ns", lane->send_freshness == nullptr || !GST_CLOCK_TIME_IS_VALID(lane->send_freshness->last_sample_id_ns.load())
              ? Json(nullptr) : Json(lane->send_freshness->last_sample_id_ns.load())},
          {"send_stale_access_units_dropped", lane->send_freshness == nullptr ? 0 : lane->send_freshness->dropped_stale.load()},
          {"send_invalid_access_units_dropped", lane->send_freshness == nullptr ? 0 : lane->send_freshness->dropped_invalid.load()},
          {"send_dependent_access_units_dropped", lane->send_freshness == nullptr ? 0 : lane->send_freshness->dropped_waiting_idr.load()},
          {"send_key_unit_requests", lane->send_freshness == nullptr ? 0 : lane->send_freshness->key_unit_requests.load()},
          {"send_key_unit_request_failures", lane->send_freshness == nullptr ? 0 : lane->send_freshness->key_unit_request_failures.load()},
          {"last_forwarded_frame_age_ms", lane->send_freshness == nullptr ? Json(nullptr) : Json(lane->send_freshness->last_forwarded_age_ms.load())},
            {"failure_count", lane->failure_count.load()}, {"camera_faulted", lane->camera_faulted.load()},
            {"disabled", lane->disabled.load()}});
      }
    };
    try {
      std::lock_guard lock(control_mutex);
      if (control_service_started && control_service) {
        changed = control_service->control_braking_wait() != waiting;
        const bool record_transition = changed || new_wait || phase == ControlRecoveryPhase::Recovered ||
            (waiting && control_service_issue_code != "control_recovery_braking_wait");
        const auto before = record_transition
            ? std::optional(control_service->adapter_status()) : std::nullopt;
        if (record_transition) sample_readiness();
        control_service->set_control_braking_wait(waiting, signaling.now_ms(), control_recovery_latch->cause_for(signaling.session_id()));
        if (before) {
          const auto after = control_service->adapter_status();
          const auto state = control_service->safety_state();
          const bool profile_active = control_service->session_control_profile().value("active", false);
          const auto timing = control_service->control_timing(signaling.now_ms());
          soft_brake_request_active = profile_active && after.opened &&
              (state == SafetyState::ControlActive || state == SafetyState::Degraded) &&
              (waiting || timing.value("recovery_brake_hold", false));
          soft_brake_applied_this_transition = soft_brake_request_active &&
              after.safe_stop_count > before->safe_stop_count;
          control_snapshot = {{"control_service_active", true}, {"session_profile_active", profile_active},
              {"safety_state", to_string(state)}, {"control_timing", timing},
              {"adapter_safe_stop_count_before", before->safe_stop_count},
              {"adapter_safe_stop_count_after", after.safe_stop_count}};
        }
      }
      if (waiting && control_service_started) {
        changed = changed || control_service_issue_code != "control_recovery_braking_wait";
        control_service_issue_code = "control_recovery_braking_wait";
      }
      else if (control_service_issue_code == "control_recovery_braking_wait") control_service_issue_code.clear();
      if (changed || phase == ControlRecoveryPhase::Recovered)
        send_vcu_handshake_status_locked(waiting ? "control_braking_wait" : "control_recovered");
    } catch (const std::exception& error) {
      // A failed physical brake must revoke control immediately, including
      // when this transition runs inside a capture thread's error handler.
      set_pipeline_error(
          "control recovery safety output failed: " + std::string(error.what()),
          "control_recovery_brake_failed", "control_recovery",
          "Keep the vehicle stopped and inspect the adapter/CAN failure; use the physical emergency stop if braking is unavailable.",
          false);
      return;
    }
    if (changed || new_wait || phase == ControlRecoveryPhase::Recovered) {
      // Sample at the transition: five-second media reports cannot explain a
      // short encoded-frame gap. These counters describe software outputs;
      // physical brake completion still requires correlated CAN feedback.
      if (sampled_at_monotonic_ms == 0) sample_readiness();
      if (control_snapshot.is_null())
        control_snapshot = {{"control_service_active", false}, {"session_profile_active", false}};
      const auto service_cause = control_snapshot.value("control_timing", Json::object()).value("control_recovery_cause", "");
      emit_diagnostic(
        waiting ? "vehicle_control_braking_wait" : "vehicle_control_recovered",
        waiting ? "control_recovery_braking_wait" : "control_recovered",
        "control_recovery", "",
        !soft_brake_request_active ? "Control authority is inactive; readiness changes do not restore driving authority."
            : waiting ? "Keep motion input neutral while control readiness recovers; the session and profile remain installed."
                : "Control readiness recovered; release old motion controls and provide fresh neutral input before driving again.",
        true, {{"stale_brake_ms", stale_brake_ms},
               {"recovery_timeout_ms", recovery_timeout_ms},
               {"recovery_stable_ms", kControlRecoveryStableMs},
               {"recovery_deadline_monotonic_ms", control_recovery_latch->deadline_for(signaling.session_id()).has_value() ? Json(*control_recovery_latch->deadline_for(signaling.session_id())) : Json(nullptr)},
               {"camera_id", failed_lane ? failed_lane->camera.id : ""},
               {"failed_camera_present", failed_lane != nullptr},
               {"time_sync", signaling.time_sync_status().to_json()},
               {"readiness_sampled_at_monotonic_ms", sampled_at_monotonic_ms},
               {"critical_lanes", std::move(critical_lanes)},
               {"control_snapshot", std::move(control_snapshot)},
               {"soft_brake_request_active", soft_brake_request_active},
               {"soft_brake_applied_this_transition", soft_brake_applied_this_transition},
               {"recovery_cause", to_string(control_recovery_latch->cause_for(signaling.session_id()))},
               {"recovery_latched_cause", to_string(control_recovery_latch->cause_for(signaling.session_id()))},
               {"control_service_recovery_cause", service_cause},
               {"safety_action", !soft_brake_request_active ? "readiness_observed_control_not_active"
                   : waiting ? "brake_with_session_retained" : "fresh_neutral_input_requested"}});
    }
  }

  void pause_control_for_critical_camera(Lane& lane) {
    std::lock_guard phase_lock(control_recovery_phase_mutex);
    lane.camera_faulted = true;
    const auto timing = recovery_timing_configured();
    const bool was_waiting = control_recovery_latch->braking_wait_for(signaling.session_id());
    apply_control_recovery_phase(control_recovery_latch->observe_recovery(
        signaling.session_id(), false, steady_now_ms(), timing.second), &lane, !was_waiting);
  }

  void enforce_control_recovery() {
    // Observe and apply one transition together: a capture failure must not
    // race a previously observed Ready phase that would release its brake.
    std::lock_guard phase_lock(control_recovery_phase_mutex);
    if (!config.runtime.control_enabled || stop_requested || control_inhibited) return;
    const auto now_ms = steady_now_ms();
    const auto [stale_brake_ms, recovery_timeout_ms] = recovery_timing_configured();
    const bool was_waiting = control_recovery_latch->braking_wait_for(signaling.session_id());
    const bool startup_grace = control_recovery_latch->startup_grace_active(signaling.session_id(), now_ms);
    bool ready = true;
    auto cause = ControlRecoveryCause::CriticalVideo;
    const Lane* failed_lane = nullptr;
    std::vector<ReadinessSampleRevision> revisions;
    for (const auto& lane : lanes) {
      if (lane->camera.critical_for_control && lane->send_freshness)
        revisions.push_back({"video:" + lane->camera.id,
            lane->send_freshness->bad_sample_revision.load(), lane->send_freshness->brake_sample_revision.load()});
    }
    for (const auto& lane : lanes) {
      if (!lane->camera.critical_for_control) continue;
      if (lane->send_freshness) lane->send_freshness->max_age_ms = stale_brake_ms;
      if (lane->disabled.load() || lane->camera_faulted.load() || !lane->send_freshness ||
          lane->send_freshness->invalid_sample_blocked.load() ||
          lane->send_freshness->consecutive_anomalies.load() >= 3 ||
          !camera_encoded_frame_fresh(lane->send_freshness->last_fresh_frame_ms.load(), now_ms, stale_brake_ms) ||
          (was_waiting && (lane->send_freshness->consecutive_anomalies.load() != 0 ||
           lane->send_freshness->fresh_frames.load() < lane->video_wait_encoded_baseline.load() + 3))) {
        ready = false;
        failed_lane = lane.get();
        break;
      }
    }
    bool receive_path_waiting = false;
    {
      std::lock_guard lock(control_mutex);
      if (control_service_started && control_service) {
        receive_path_waiting = control_service->control_braking_wait();
        // An already active receive-path wait may have healthy current flags
        // (late packets or a coalesced historical event). Preserve its actual
        // source instead of latching the default camera cause in the observer.
        if (receive_path_waiting && !failed_lane) cause = control_service->control_recovery_cause();
        revisions.push_back({"driver", control_service->recovery_bad_sample_revision(),
            control_service->recovery_brake_revision()});
        ready = ready && control_service->driver_critical_video_ready();
        if (!control_service->driver_critical_video_ready()) cause = ControlRecoveryCause::CriticalVideo;
        if (was_waiting || receive_path_waiting)
          ready = ready && control_service->recovery_samples_healthy() &&
              control_service->control_timing(signaling.now_ms()).value("consecutive_late_frames", 0) == 0;
        if (!control_service->operator_input_ready()) { ready = false; cause = ControlRecoveryCause::InputLease; }
        if (!control_service->driver_control_link_ready()) { ready = false; cause = ControlRecoveryCause::ControlLink; }
        if (!control_service->driver_time_sync_ready()) { ready = false; cause = ControlRecoveryCause::TimeSynchronization; }
      }
    }
    const auto time_readiness = vehicle_time_readiness();
    if (config.field_safety.require_time_sync)
      revisions.push_back({"clock", time_readiness.bad_cohort_revision, time_readiness.brake_cohort_revision});
    if (config.field_safety.require_time_sync &&
        (!time_readiness.ready || ((was_waiting || receive_path_waiting) && !time_readiness.latest_candidate_healthy))) {
      ready = false; cause = ControlRecoveryCause::TimeSynchronization;
    }
    if (receive_path_waiting && !was_waiting)
      static_cast<void>(control_recovery_latch->observe_recovery(signaling.session_id(), false,
          now_ms, recovery_timeout_ms, cause));
    // Startup still grants no authority while waiting for its first frames.
    // A readiness wait retains the same session. Command and feedback absence
    // remain governed by their independent watchdogs, not this stable window.
    if (startup_grace && !was_waiting && cause == ControlRecoveryCause::CriticalVideo) return;
    const auto phase = control_recovery_latch->observe_readiness(
        signaling.session_id(), ready, now_ms, recovery_timeout_ms, cause, revisions);
    apply_control_recovery_phase(phase, failed_lane, !was_waiting && phase == ControlRecoveryPhase::BrakingWait);
  }

  // Called with control_mutex held. This grants no offline authority: both
  // the authenticated native socket and the regular cloud lease must be live.
  bool cloud_control_authority_live_locked() const {
    return native_control_websocket_connected.load() && native_control_transport_cloud_capable.load() &&
        signaling.control_lease_expiry() > signaling.now_ms();
  }

  void start_control_when_cameras_ready() {
    if (!config.runtime.control_enabled || control_inhibited || !critical_cameras_ready()) return;
    {
      std::lock_guard lock(control_mutex);
      if (stop_requested || (!(control_link_open && control_channel != nullptr) && !cloud_control_authority_live_locked()) ||
          control_service_started || (control_service_issue_code != "critical_camera_not_ready" && !control_service_issue_code.empty())) {
        return;
      }
    }
    const bool adapter_ready = start_control_service();
    send_vcu_handshake_status(adapter_ready ? "critical_cameras_ready" : "adapter_start_failed");
    if (adapter_ready) return;

    GstWebRTCDataChannel* channel_to_close = nullptr;
    {
      std::lock_guard lock(control_mutex);
      if (control_service_issue_code == "critical_camera_not_ready") return;
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
        if (stop_requested || (!(control_link_open && control_channel != nullptr) && !cloud_control_authority_live_locked())) {
          return false;
        }
        if (control_inhibited) {
          control_service_issue_code = std::string(recovery_inhibition_issue());
          return false;
        }
        if (!critical_cameras_ready()) {
          control_service_issue_code = "critical_camera_not_ready";
          return false;
        }
        if (!control_recovery_latch->arm_for_control(signaling.session_id())) {
          control_service_issue_code = std::string(recovery_inhibition_issue());
          return false;
        }
        invalidate_native_control_trusted_gear_locked();
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
            control_service->close(
                control_inhibited
                    ? VehicleStopContext{
                          VehicleStopSource::SoftwareFault,
                          VehicleStopReason::CriticalCameraFailed}
                    : VehicleStopContext{
                          VehicleStopSource::Session,
                          VehicleStopReason::SessionLost});
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
              ? std::string(recovery_inhibition_issue())
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
      // Both native feedback and DataChannel messages use this session's
      // ordering. Replacing only the media channel must not roll it back.
      const auto status_session_id = signaling.session_id();
      if (control_status_session_id != status_session_id) {
        control_status_session_id = status_session_id;
        control_status_seq = 0;
        last_control_rejection_status_seq = 0;
        last_control_rejection_status_issue_code.clear();
        last_control_rejection_status_ms.reset();
      }
      control_channel = channel;
    }
  }

  // Runs under control_mutex, including receive-triggered watchdog decisions.
  // Arrival is diagnostic evidence only; rejected packets never renew control.
  void observe_control_hard_stop_locked(std::int64_t timestamp_ms) noexcept {
    if (!config.runtime.control_log_commands || !control_service) return;
    const auto state = control_service->safety_state();
    const bool hard = state == SafetyState::TimeoutBrake || state == SafetyState::Fault || state == SafetyState::Estop;
    if (!hard) { last_observed_hard_stop_state.reset(); return; }
    if (last_observed_hard_stop_state == state) return;
    last_observed_hard_stop_state = state;
    try {
      const auto timing = control_service->control_timing(timestamp_ms);
      enqueue_control_trace({
          {"stage", "control_hard_stop_transition"}, {"event_at_utc_ms", timestamp_ms},
          {"session_id", signaling.session_id()}, {"vehicle_id", config.vehicle_id},
          {"safety_state", to_string(state)}, {"session_profile_active", control_service->session_control_profile().value("active", false)},
          {"control_timing", timing},
          {"last_parsed_control_arrival_seq", last_parsed_control_arrival_seq},
          {"last_parsed_control_arrival_age_ms", last_parsed_control_arrival_ms
              ? Json(std::max<std::int64_t>(0, timestamp_ms - *last_parsed_control_arrival_ms)) : Json(nullptr)},
          {"last_evaluated_control_seq", last_evaluated_control_seq},
          {"last_evaluated_control_reason", bounded_control_trace_text(last_evaluated_control_reason)},
          {"last_accepted_control_seq", last_accepted_control_seq},
          {"last_accepted_control_age_ms", timing.at("last_valid_control_age_ms")},
          {"last_accepted_control_utc_age_ms", last_control_received_at_ms.load() > 0
              ? Json(std::max<std::int64_t>(0, timestamp_ms - last_control_received_at_ms.load())) : Json(nullptr)},
          {"control_status_seq", control_status_seq},
          {"accepted_commands_total", accepted_control_commands.load()},
          {"rejected_commands_total", rejected_control_commands.load()},
          {"native_wss_last_message_at_utc_ms", native_control_websocket_last_message_at_ms.load()},
          {"watchdog_ticks_total", native_control_watchdog_ticks_total.load()},
          {"watchdog_skipped_intervals_total", native_control_watchdog_skipped_intervals_total.load()}});
    } catch (...) { note_control_trace_drop(); }
  }

  void tick_control_service() {
    std::unique_lock lock(control_mutex);
    if (stop_requested || control_inhibited || !control_service_started || !control_service) return;
    const auto timestamp_ms = signaling.now_ms();
    emergency_stop_receiver.observe_latched(control_service->safety_state() == SafetyState::Estop);
    try {
      if (direct_authority_stop_failed) throw std::runtime_error("direct authority worker/physical brake failed; stop is unconfirmed");
      if (direct_authority.observe_expiry(steady_now_ms())) {
        invalidate_native_control_trusted_gear_locked();
        control_service->set_control_braking_wait(true, timestamp_ms, ControlRecoveryCause::ControlLink);
      }
      control_service->tick(timestamp_ms);
      observe_control_hard_stop_locked(timestamp_ms);
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
        control_service->close({
            VehicleStopSource::SoftwareFault,
            VehicleStopReason::VcuStateFault});
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
      invalidate_native_control_trusted_gear_locked();
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
    if ((control_link_open || control_transport_fence.negotiated() || cloud_control_authority_live_locked()) &&
        (!last_vcu_status_ms || timestamp_ms - *last_vcu_status_ms >= 500)) {
      send_vcu_handshake_status_locked("status_update");
      last_vcu_status_ms = timestamp_ms;
    }
    if (control_link_open || control_transport_fence.negotiated() || cloud_control_authority_live_locked()) send_latest_vehicle_telemetry_locked();
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
      VideoEncoderSettings settings{lane->profile.bitrate_kbps, std::max(1, lane->profile.fps)};
      pipeline_text
          << build_camera_input_pipeline("source_" + id, lane->input, lane->profile)
          << "! " << encoder.pipeline_stage(settings, "encoder_" + id) << ' '
          << "! " << parser << " config-interval=-1 "
          << "! " << elementary_caps << ",stream-format=byte-stream,alignment=au "
          << "! tee name=encoded_" << id << ' '
          << "encoded_" << id << ". ! " << build_encoded_frame_queue("send_queue_" + id, 2)
          << "! " << payloader << " name=pay_" << id << " config-interval=-1 pt=" << payload_type << ' '
          << "! application/x-rtp,media=video,encoding-name=" << encoding_name << ",payload=" << payload_type
          << (encoder.codec() == VideoCodec::H265
                  ? ",profile-id=(string)1,tier-flag=(string)0,tx-mode=(string)SRST"
                  : "")
          << " ! webrtc. ";
      if (!recording_root.empty() && !lane->camera.record_profile.empty()) {
        pipeline_text
            << "encoded_" << id << ". ! "
            << "appsink name=record_tap_" << id << " emit-signals=true max-buffers=1 drop=false sync=false async=false enable-last-sample=false ";
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

  Json recording_status_for(const Lane& lane) const {
    const bool configured = !recording_root.empty() && !lane.camera.record_profile.empty();
    if (configured && isolated_recording && lane.recording_index != std::numeric_limits<std::size_t>::max())
      return isolated_recording->lane_status(lane.recording_index);
    if (configured && lane.recording_index < recording_final_status.size()) return recording_final_status[lane.recording_index];
    return {{"schema_version", 1}, {"configured_enabled", configured}, {"active", false},
      {"state", configured ? "failed" : "disabled"}, {"incomplete", configured}, {"recording_attempt_id", recording_attempt_id},
      {"issue_code", configured ? "recording_start_failed" : ""},
      {"next_action", configured ? "Recording is incomplete; inspect recorder/storage. Live video continues." : "Recording is disabled."},
      {"summary_persisted", false}, {"final_eos_received", false}, {"cleanup_pending", false}, {"status_revision", configured ? 1 : 0},
      {"error", recording_setup_error}};
  }

  void prepare_recording(const EncoderCandidate& candidate) {
    recording_final_status.clear(); recording_setup_error.clear();
    if (recording_current_session != signaling.session_id()) {
      recording_current_session = signaling.session_id(); recording_session_incomplete = false;
    }
    try {
    RecordingSessionSettings settings;
    settings.vehicle_id = config.vehicle_id; settings.session_id = signaling.session_id(); settings.root = recording_root;
    settings.attempt_id = random_token(12); recording_attempt_id = settings.attempt_id;
    settings.incomplete_before = recording_session_incomplete;
    Json expected = Json::array();
    for (const auto& lane : lanes) {
      if (recording_root.empty() || lane->camera.record_profile.empty()) continue;
      const auto& profile = config.record_profile(lane->camera.record_profile);
      lane->recording_index = settings.lanes.size();
      settings.lanes.push_back({lane->camera.id, to_string(candidate.codec), to_string(candidate.backend), lane->profile.fps, profile.segment_seconds});
      expected.push_back(lane->camera.id);
    }
    if (settings.lanes.empty()) return;
    emit_diagnostic("vehicle_recording_attempt_started", "recording_attempt_requested", "recording_start", "",
      "Recording startup is independent of live video; completion requires recorder evidence.", true,
      {{"recording_attempt_id", recording_attempt_id}, {"reported_at_utc_ms", signaling.now_ms()},
       {"configured_enabled", true}, {"summary_persisted", false}, {"recording_expected_cameras", expected}});
    isolated_recording = std::make_unique<IsolatedRecordingSession>(std::move(settings));
    }
    catch (const std::exception& error) {
      recording_setup_error = error.what(); recording_session_incomplete = true;
      emit_diagnostic("vehicle_recording_failed", "recording_start_failed", "recording_start", error.what(),
        "Recording is incomplete. Inspect recorder resources; live video continues.", false,
        {{"recording_attempt_id", recording_attempt_id}, {"reported_at_utc_ms", signaling.now_ms()}, {"active", false}, {"incomplete", true}, {"summary_persisted", false}});
    }
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
    prepare_recording(candidate);
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
    media_signal_state->begin_attempt(this, webrtc, signaling.session_id());
    const auto connect_media_signal = [&](const char* name, GCallback callback) {
      auto* state = new std::shared_ptr<MediaSignalState>(media_signal_state);
      g_signal_connect_data(webrtc, name, callback, state,
          +[](gpointer data, GClosure*) { delete static_cast<std::shared_ptr<MediaSignalState>*>(data); },
          static_cast<GConnectFlags>(0));
    };
    connect_media_signal("on-negotiation-needed", G_CALLBACK(on_negotiation_needed));
    connect_media_signal("on-ice-candidate", G_CALLBACK(on_ice_candidate));
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
      lane->send_queue = gst_bin_get_by_name(GST_BIN(pipeline), ("send_queue_" + id).c_str());
      lane->record_tap = gst_bin_get_by_name(GST_BIN(pipeline), ("record_tap_" + id).c_str());
      if (lane->record_tap != nullptr) {
        lane->recording_tap = std::make_unique<detail::RecordingTap>(
            lane->record_tap, isolated_recording.get(), lane->recording_index);
      }
      auto* payloader = gst_bin_get_by_name(GST_BIN(pipeline), ("pay_" + id).c_str());
      auto* rtp_src_pad = payloader == nullptr ? nullptr : gst_element_get_static_pad(payloader, "src");
      if (payloader != nullptr) gst_object_unref(payloader);
      if (lane->appsrc == nullptr || lane->encoder == nullptr || lane->send_queue == nullptr ||
          rtp_src_pad == nullptr) {
        if (rtp_src_pad != nullptr) gst_object_unref(rtp_src_pad);
        set_pipeline_error(
            "media pipeline lane is incomplete: " + lane->camera.id,
            "gstreamer_camera_lane_incomplete",
            "pipeline_link",
            "Check the camera element names and appsrc/encoder/queue/RTP payloader elements.",
            false,
            {{"camera_id", lane->camera.id}, {"device", lane->camera.device}});
        stop_pipeline();
        return false;
      }
      isolate_bundled_rtp_lane_eos(rtp_src_pad, stop_requested);
      gst_object_unref(rtp_src_pad);
      g_signal_connect(lane->send_queue, "overrun", G_CALLBACK(+[](GstElement*, gpointer data) {
        ++static_cast<Lane*>(data)->encoded_queue_backpressure;
      }), lane.get());
      lane->send_freshness = std::make_unique<detail::EncodedSendFreshness>(
          candidate.codec == VideoCodec::H265 ? detail::AccessUnitCodec::H265 : detail::AccessUnitCodec::H264,
          static_cast<std::uint64_t>(config.runtime.critical_camera_stale_brake_ms));
      auto* send_src = gst_element_get_static_pad(lane->send_queue, "src");
      detail::attach_encoded_send_freshness(send_src, *lane->send_freshness);
      gst_object_unref(send_src);
      auto* encoder_sink = gst_element_get_static_pad(lane->encoder, "sink");
      if (encoder_sink != nullptr) {
        gst_pad_add_probe(encoder_sink, GST_PAD_PROBE_TYPE_BUFFER,
            +[](GstPad*, GstPadProbeInfo*, gpointer data) -> GstPadProbeReturn {
              ++static_cast<Lane*>(data)->raw_to_encoder;
              return GST_PAD_PROBE_OK;
            }, lane.get(), nullptr);
        gst_object_unref(encoder_sink);
      }
      GstPad* encoder_src = gst_element_get_static_pad(lane->encoder, "src");
      if (encoder_src != nullptr) {
        gst_pad_add_probe(encoder_src, static_cast<GstPadProbeType>(
            GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), count_encoded, lane.get(), nullptr);
        gst_object_unref(encoder_src);
      }
      auto* factory = gst_element_get_factory(lane->encoder);
      auto* gstreamer_version = gst_version_string();
      emit_diagnostic("vehicle_media_lane_configured", "media_lane_diagnostics_ready", "pipeline_config",
          "", "Use the capture, encoder and receiver counters to locate the first stalled stage.", true,
          {{"camera_id", lane->camera.id}, {"gstreamer_version", gstreamer_version},
           {"encoder_factory", factory == nullptr ? "" : GST_OBJECT_NAME(factory)},
           {"diagnostics_schema_version", 2}});
      g_free(gstreamer_version);
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
      lane->send_freshness->origin_steady_ms = lane->pipeline_started_steady_ms;
      lane->thread = std::thread([this, lane = lane.get(), capture_interval_ms] {
        std::uint64_t sequence = 0;
        bool recovery_pending = false;
        auto next_capture_sample_ms = std::int64_t{0};
        std::int64_t previous_capture_sample_ms = 0;
        std::uint64_t previous_capture_sample_frames = 0;
        const bool pace_test_source =
            classify_camera_source(lane->camera) == CameraSourceKind::TestSource &&
            capture_interval_ms == 0;
        const auto test_source_interval =
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::seconds(1)) /
            static_cast<std::int64_t>(std::max(1, lane->profile.fps));
        auto next_test_source_frame = std::chrono::steady_clock::now();
        while (!stop_requested && !lane->disabled.load()) {
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
            frame.captured_at_ms = signaling.from_steady_time(frame.captured_steady_at);
            ++lane->captured;
            const auto captured_steady_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                frame.captured_steady_at.time_since_epoch()).count();
            const auto previous_capture_ms = lane->last_capture_steady_ms.exchange(captured_steady_ms);
            const auto capture_gap_ms = previous_capture_ms == 0 ? 0 : captured_steady_ms - previous_capture_ms;
            auto observed_gap = lane->capture_interval_max_ms.load();
            while (capture_gap_ms > observed_gap && !lane->capture_interval_max_ms.compare_exchange_weak(
                observed_gap, capture_gap_ms)) {}
            lane->last_capture_ms = frame.captured_at_ms;
            if (captured_steady_ms >= next_capture_sample_ms) {
              next_capture_sample_ms = captured_steady_ms + 5000;
              const auto sampled_frames = lane->captured.load();
              const auto sample_duration_ms = previous_capture_sample_ms > 0 ? captured_steady_ms - previous_capture_sample_ms : 0;
              const Json observed_fps = sample_duration_ms > 0
                  ? Json((sampled_frames - previous_capture_sample_frames) * 1000.0 / sample_duration_ms) : Json(nullptr);
              previous_capture_sample_ms = captured_steady_ms;
              previous_capture_sample_frames = sampled_frames;
              emit_diagnostic("vehicle_camera_capture_sample", "camera_capture_sample", "camera_capture",
                  "", "Compare observed delivery FPS with the capture request. Nominal settings do not verify sensor cadence; report device limitations and validate the raw drop-only fallback when delivery is faster.", true,
                  {{"camera_id", lane->camera.id}, {"device", lane->camera.device},
                   {"captured_frames", lane->captured.load()},
                   {"raw_to_encoder_frames", lane->raw_to_encoder.load()}, {"encoded_frames", lane->encoded.load()},
                   {"fresh_send_frames", lane->send_freshness->fresh_frames.load()},
                   {"requested_capture_fps", lane->input.fps}, {"target_encode_fps", lane->profile.fps},
                   {"capture_fps_observed", observed_fps}, {"capture_fps_sample_duration_ms", sample_duration_ms},
                   {"capture_interval_ms", capture_gap_ms}, {"capture_interval_max_ms", lane->capture_interval_max_ms.load()},
                   {"payload_bytes", frame.payload.size()}, {"source_sequence", frame.source_sequence},
                   {"capture_diagnostics", lane->source->capture_diagnostics()}});
            }
            const bool recovered_this_frame = recovery_pending;
            if (recovered_this_frame) {
              recovery_pending = false;
              {
                std::lock_guard lock(lane->error_mutex);
                lane->error.clear();
                lane->camera_faulted = false;
              }
              emit_diagnostic(
                  "vehicle_camera_recovered",
                  "camera_lane_recovered",
                  "camera_reopen",
                  "",
                  lane->camera.critical_for_control
                      ? "Video capture resumed; keep braking until encoded output and the driver view are fresh."
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
            const auto elapsed_ms = captured_steady_ms - lane->pipeline_started_steady_ms;
            if (elapsed_ms < 0) {
              // A driver buffer can predate pipeline admission. Discard this
              // independent raw/JPEG frame instead of relabelling it as new.
              ++lane->dropped;
              continue;
            }
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.payload.size(), nullptr);
            if (buffer == nullptr) throw std::runtime_error("cannot allocate GStreamer camera buffer");
            gst_buffer_fill(buffer, 0, frame.payload.data(), frame.payload.size());
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
                failure.starts_with("cannot allocate GStreamer camera buffer");
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
            // For a critical lane, withdraw motion and brake before any
            // potentially blocking diagnostic output or device/process cleanup.
            if (decision.inhibit_control) {
              pause_control_for_critical_camera(*lane);
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
                      ? "Keep braking while repairing the critical camera; this disabled lane cannot recover in the current runtime and will reach the safety-stop deadline."
                      : "Repair the optional camera before the next session; the other camera lanes and current control authority remain active.",
                  false,
                  {{"camera_id", lane->camera.id},
                   {"device", lane->camera.device},
                   {"critical_for_control", lane->camera.critical_for_control},
                   {"failure_count", failure_count},
                   {"reopen_count", lane->reopen_count.load()},
                   {"safety_action", lane->camera.critical_for_control
                                          ? "braking_wait_disable_failed_camera_lane_video_continues"
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
                    ? "The vehicle is braking while this camera reopens; timely video recovery retains the session and requires fresh motion input."
                    : "The runtime will reopen only this noncritical camera source after the bounded backoff.",
                true,
                {{"camera_id", lane->camera.id},
                 {"device", lane->camera.device},
                 {"critical_for_control", lane->camera.critical_for_control},
                 {"reopen_count", reopen_count},
                 {"reopen_attempts", lane->camera.reopen_attempts},
                 {"retry_after_ms", lane->camera.reopen_backoff_ms},
                 {"safety_action", lane->camera.critical_for_control
                                        ? "braking_wait_reopen_camera_lane_video_continues"
                                        : "reopen_noncritical_camera_lane_only"}});
            if (!wait_for_camera_reopen(lane->camera.reopen_backoff_ms)) break;
            next_test_source_frame = std::chrono::steady_clock::now();
          }
        }
      });
    }
    try {
      start_native_control_transport();
    } catch (const std::exception& error) {
      set_pipeline_error(
          "cannot start native control transport: " + std::string(error.what()),
          "native_control_transport_start_failed",
          "native_control_signaling",
          "Keep the vehicle stopped and inspect native thread/runtime resource availability.",
          true,
          {{"transport", "native_signaling_websocket"},
           {"watchdog_interval_ms", kNativeControlWatchdogInterval.count()},
           {"websocket_connect_timeout_ms",
            kNativeControlWebSocketConnectTimeout.count()}});
      return false;
    }
    return true;
  }

  void stop_pipeline() {
    stop_requested = true;
    {
      std::lock_guard lock(media_signal_state->mutex);
      media_signal_state->owner = nullptr; media_signal_state->peer = nullptr;
      media_signal_state->compatibility_issue.clear();
      media_signal_state->negotiation_id.clear(); media_signal_state->remote_ufrags.clear();
      media_signal_state->local_description_ready = false;
      media_signal_state->offer_queued = false;
      media_signal_state->local_ufrags.clear();
      media_signal_state->pending_local_candidates.clear();
      media_signal_state->pending_remote_candidates.clear(); media_signal_state->pending.clear();
    }
    GstWebRTCDataChannel* channel_to_close = nullptr;
    {
      std::lock_guard lock(control_mutex);
      channel_to_close = std::exchange(control_channel, nullptr);
      control_link_open = false;
      invalidate_native_control_trusted_gear_locked();
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
    // The independent watchdog and WSS receiver can be waiting for
    // control_mutex, but neither can produce adapter output after the guarded
    // close/reset above. Join only after that fail-safe boundary has completed.
    stop_native_control_transport_threads();
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
    if (isolated_recording && (pipeline == nullptr || std::any_of(lanes.begin(), lanes.end(), [](const auto& lane) {
          return lane->recording_index != std::numeric_limits<std::size_t>::max() &&
              (lane->record_tap == nullptr || !lane->recording_tap);
        }))) isolated_recording->abort("recording_parent_eos_missing");
    if (pipeline != nullptr) {
      std::vector<GstElement*> sources;
      std::vector<detail::RecordingTap*> taps;
      for (const auto& lane : lanes) {
        if (lane->appsrc != nullptr) sources.push_back(lane->appsrc);
        if (lane->recording_tap) taps.push_back(lane->recording_tap.get());
      }
      detail::stop_recording_pipeline(pipeline, sources, taps, isolated_recording.get());
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
      if (lane->send_queue != nullptr) {
        gst_object_unref(lane->send_queue);
        lane->send_queue = nullptr;
      }
      if (lane->record_tap != nullptr) {
        lane->recording_tap.reset();
        gst_object_unref(lane->record_tap); lane->record_tap = nullptr;
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
    if (isolated_recording) {
      isolated_recording->finish();
      for (const auto& lane : lanes) {
        if (lane->recording_index == std::numeric_limits<std::size_t>::max()) continue;
        const auto status = isolated_recording->lane_status(lane->recording_index);
        if (status.value("incomplete", true) || status.value("state", "") != "finalized") recording_session_incomplete = true;
        recording_final_status.push_back(status);
      }
      emit_diagnostic("vehicle_recording_attempt_finished", recording_session_incomplete ? "recording_incomplete" : "recording_attempt_finalized", "recording_finalize", "",
        recording_session_incomplete ? "Recording is incomplete; preserve completed clips and inspect storage." : "This recording attempt finalized; earlier session coverage is not implied.", true,
        {{"recording_attempt_id", recording_attempt_id}, {"reported_at_utc_ms", signaling.now_ms()}, {"incomplete", recording_session_incomplete}, {"recording", isolated_recording->session_status()}});
      isolated_recording.reset();
    }
  }

  void process_control_management(const Json& message, const Json& payload) {
        const auto sent = payload.value("sent_at_utc_ms", std::int64_t{0});
        const auto now = signaling.now_ms();
        const auto seq = message.value("seq", std::uint64_t{0});
        bool allowed = false;
        { std::lock_guard control_lock(control_mutex);
          allowed = (control_transport_fence.negotiated() || direct_authority.valid(steady_now_ms())) && native_control_websocket_connected.load() &&
              native_control_transport_cloud_capable.load() && signaling.control_lease_expiry() > now &&
              payload.value("protocol", "") == detail::kControlTransportCapability &&
              payload.value("session_id", "") == signaling.session_id() && payload.value("vehicle_id", "") == config.vehicle_id &&
              payload.value("driver_id", "") == signaling.driver_id() && payload.value("control_token", "") == signaling.control_token() &&
              sent >= now - config.control.max_command_gap_ms && sent <= now + config.control.max_command_gap_ms &&
              seq > last_control_management_seq;
          if (allowed) last_control_management_seq = seq;
        }
        if (allowed) handle_control_message(nullptr, payload.at("message").dump(), ControlMessageTransport::NativeSignaling, nullptr);
  }

  void flush_outgoing_signals() {
    std::deque<std::pair<std::string, Json>> values;
    {
      std::lock_guard lock(media_signal_state->mutex);
      values.swap(media_signal_state->pending);
    }
    for (const auto& [type, payload] : values) {
      if (type == "webrtc_offer" || type == "ice_candidate") {
        std::lock_guard lock(media_signal_state->mutex);
        if (media_signal_state->peer == nullptr ||
            payload.value("media_negotiation_id", "") != media_signal_state->negotiation_id) continue;
      }
      signaling.send(type, payload);
    }
  }

  void apply_remote_ice(const Json& payload) {
    const auto candidate = payload.value("candidate", "");
    if (candidate.empty()) return;
    if (candidate.size() > 8192) throw std::runtime_error("WebRTC ICE candidate exceeds limit");
    GstElement* source = nullptr;
    {
      std::lock_guard lock(media_signal_state->mutex);
      auto& state = *media_signal_state;
      if (state.peer == nullptr || state.negotiation_id.empty()) return;
      const auto id = payload.value("media_negotiation_id", "");
      if (!id.empty() && id != state.negotiation_id) return;
      const auto ufrag = candidate_ufrag(payload);
      if (state.remote_ufrags.empty()) {
        // Even tagged trickle candidates wait for the corresponding answer.
        // This keeps pre-answer candidates within the application scope fence.
        if (id.empty() && ufrag.empty() && (state.attempt_count != 1 || state.offer_count != 1)) return;
        if (state.pending_remote_candidates.size() >= 128) throw std::runtime_error("WebRTC ICE candidate buffer exceeds limit");
        state.pending_remote_candidates.push_back(payload); return;
      }
      if (!ufrag.empty()) {
        if (std::find(state.remote_ufrags.begin(), state.remote_ufrags.end(), ufrag) == state.remote_ufrags.end()) return;
      } else if (id.empty() && (state.attempt_count != 1 || state.offer_count != 1)) return;
      source = GST_ELEMENT(gst_object_ref(state.peer));
    }
    MediaPeerReference reference{source};
    const auto index = payload.value("sdpMLineIndex", 0U);
    g_signal_emit_by_name(source, "add-ice-candidate", index, candidate.c_str());
    const auto count = ++remote_ice_candidate_count;
    if (count == 1) emit_diagnostic("vehicle_webrtc_remote_ice_candidate_received",
        "webrtc_remote_ice_candidate_available", "ice_connectivity", "",
        "No action is required; this is a connectivity milestone.", true,
        {{"candidate_count", count}, {"sdp_mline_index", index}});
  }

  Json media_scope_status() const {
    std::lock_guard lock(media_signal_state->mutex);
    return {{"id", media_signal_state->negotiation_id},
        {"issue_code", media_signal_state->compatibility_issue},
        {"next_action", media_signal_state->compatibility_issue.empty() ? "" :
            "Keep control stopped. Update controller and vehicle packages together, reopen the control page, and start a fresh session."}};
  }

  void process_signaling() {
    const auto response = signaling.poll("webrtc_answer,ice_candidate,media_fallback,control_management");
    for (const auto& message : response.value("messages", Json::array())) {
      const auto type = message.value("type", "");
      const auto payload = message.value("payload", Json::object());
      if (type == "control_management") { process_control_management(message, payload); continue; }
      if (type == "webrtc_answer") {
        GstElement* source = nullptr;
        std::string negotiation_id;
        {
          std::lock_guard lock(media_signal_state->mutex);
          if (media_signal_state->peer == nullptr || media_signal_state->negotiation_id.empty()) continue;
          const auto id = payload.value("media_negotiation_id", "");
          if (!id.empty() && id != media_signal_state->negotiation_id) continue;
          // An untagged legacy answer cannot be associated with a retry.
          // Keep first-connection compatibility, then require the offer id.
          if (id.empty() && !media_signal_state->remote_ufrags.empty()) continue;
          if (id.empty() && (media_signal_state->attempt_count != 1 || media_signal_state->offer_count != 1)) {
            if (media_signal_state->compatibility_issue.empty()) {
              media_signal_state->compatibility_issue = "media_negotiation_scope_missing";
              emit_diagnostic("vehicle_media_scope_rejected", "media_negotiation_scope_missing", "webrtc_answer",
                  "The retry answer does not identify its offer.",
                  "Keep control stopped. Update controller and vehicle packages together, reopen the control page, and start a fresh session.",
                  false, {{"media_negotiation_id", media_signal_state->negotiation_id}});
            }
            continue;
          }
          source = GST_ELEMENT(gst_object_ref(media_signal_state->peer));
          negotiation_id = media_signal_state->negotiation_id;
        }
        MediaPeerReference reference{source};
        const auto sdp_text = payload.value("sdp", "");
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new(&sdp) != GST_SDP_OK ||
            gst_sdp_message_parse_buffer(
                reinterpret_cast<const guint8*>(sdp_text.data()), sdp_text.size(), sdp) != GST_SDP_OK) {
          if (sdp != nullptr) gst_sdp_message_free(sdp);
          std::lock_guard lock(media_signal_state->mutex);
          if (media_signal_state->peer != source || media_signal_state->negotiation_id != negotiation_id) continue;
          set_pipeline_error(
              "driver returned an invalid WebRTC answer SDP",
              "webrtc_answer_sdp_invalid",
              "webrtc_answer",
              "Inspect the controller SDP and ensure browser/server codec negotiation matches the vehicle offer.",
              false);
          return;
        }
        auto* answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        std::deque<Json> pending;
        {
          std::lock_guard lock(media_signal_state->mutex);
          if (media_signal_state->peer != source || media_signal_state->negotiation_id != negotiation_id) {
            gst_webrtc_session_description_free(answer); continue;
          }
          media_signal_state->compatibility_issue.clear();
          media_signal_state->remote_ufrags = ice_ufrags(sdp_text);
          pending.swap(media_signal_state->pending_remote_candidates);
        }
        GstPromise* promise = gst_promise_new();
        g_signal_emit_by_name(source, "set-remote-description", answer, promise);
        gst_promise_interrupt(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);
        for (const auto& candidate : pending) apply_remote_ice(candidate);
        std::lock_guard lock(media_signal_state->mutex);
        if (media_signal_state->peer != source || media_signal_state->negotiation_id != negotiation_id) continue;
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
        apply_remote_ice(payload);
      } else if (type == "media_fallback" && active_candidate.codec == VideoCodec::H265) {
        {
          std::lock_guard lock(media_signal_state->mutex);
          const auto id = payload.value("media_negotiation_id", "");
          if (!id.empty() && id != media_signal_state->negotiation_id) continue;
        }
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
        const auto source = GST_MESSAGE_SRC(message);
        const auto failed = std::find_if(lanes.begin(), lanes.end(), [&](const auto& lane) {
          return source != nullptr && (source == GST_OBJECT(lane->encoder) || source == GST_OBJECT(lane->appsrc) ||
                                       source == GST_OBJECT(lane->send_queue));
        });
        if (failed != lanes.end()) {
          // A known camera branch can fail while control and other RTP tracks
          // remain usable. Stop that branch, then let the video deadline own
          // the hard stop instead of tearing down the whole control session.
          auto& lane = **failed;
          lane.disabled = true;
          if (lane.camera.critical_for_control) pause_control_for_critical_camera(lane);
          { std::lock_guard lock(lane.error_mutex); lane.error = value; }
          emit_diagnostic("vehicle_camera_encoder_failed", "camera_encoder_bus_error", "camera_encoder",
              value, "Inspect this camera encoder; critical video remains in braking wait until recovery or its deadline.",
              false, {{"camera_id", lane.camera.id}, {"source_element", GST_OBJECT_NAME(source)},
                      {"critical_for_control", lane.camera.critical_for_control},
                      {"safety_action", lane.camera.critical_for_control ? "brake_with_session_retained" : "disable_noncritical_camera_lane_only"}});
        } else {
          // Shared transport/DataChannel failures can damage the independent
          // control authority too; retain their original fail-safe shutdown.
          set_pipeline_error(
              std::move(value), "gstreamer_bus_error", "pipeline_runtime",
              "Inspect the GStreamer error/debug text and the shared transport element.",
              true, {{"source_element", source == nullptr ? "" : GST_OBJECT_NAME(source)}});
        }
      } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
        ++gstreamer_warning_count;
        const auto current = steady_now_ms();
        if (gstreamer_warning_count <= 10 || current >= next_gstreamer_warning_log_ms) {
          next_gstreamer_warning_log_ms = current + 5000;
          GError* warning = nullptr;
          gchar* debug = nullptr;
          gst_message_parse_warning(message, &warning, &debug);
          emit_diagnostic("vehicle_media_pipeline_warning", "gstreamer_bus_warning", "pipeline_runtime",
              warning == nullptr ? "unknown GStreamer warning" : std::string(warning->message).substr(0, 2048),
              "Correlate the source element, capture samples and receiver loss/decode counters.", true,
              {{"source_element", GST_MESSAGE_SRC(message) == nullptr ? "" : GST_OBJECT_NAME(GST_MESSAGE_SRC(message))},
               {"severity", "warning"},
               {"debug", debug == nullptr ? "" : std::string(debug).substr(0, 2048)},
               {"warning_count", gstreamer_warning_count}});
          if (warning != nullptr) g_error_free(warning);
          g_free(debug);
        }
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
      guint encoded_queued_buffers = 0;
      if (lane->send_queue != nullptr) {
        g_object_get(lane->send_queue, "current-level-buffers", &encoded_queued_buffers, nullptr);
      }
      result.push_back({
          {"camera_id", lane->camera.id},
          {"critical_for_control", lane->camera.critical_for_control},
          {"lane_state", lane->disabled.load() ? "disabled" : (error.empty() ? "active" : "recovering")},
          {"captured_frames", lane->captured.load()},
          {"pushed_frames", lane->pushed.load()},
          {"raw_to_encoder_frames", lane->raw_to_encoder.load()},
          {"encoded_frames", lane->encoded.load()},
          {"dropped_frames", lane->dropped.load()},
          {"pipeline_backlog_or_drop_frames", lane->pushed.load() > lane->encoded.load() ? lane->pushed.load() - lane->encoded.load() : 0},
          {"appsrc_queued_buffers", appsrc_queued_buffers},
          {"appsrc_queue_limit_buffers", kCameraAppSrcMaxBuffers},
          {"encoded_queued_buffers", encoded_queued_buffers},
          {"encoded_queue_backpressure_count", lane->encoded_queue_backpressure.load()},
          {"send_freshness_scope", "capture_pts_to_send_queue_source_monotonic_not_network_delivery"},
          {"fresh_send_frames", lane->send_freshness == nullptr ? 0 : lane->send_freshness->fresh_frames.load()},
          {"send_waiting_idr", lane->send_freshness != nullptr && lane->send_freshness->blocked.load()},
          {"send_consecutive_anomalies", lane->send_freshness == nullptr ? 0 : lane->send_freshness->consecutive_anomalies.load()},
          {"send_invalid_sample_blocked", lane->send_freshness != nullptr && lane->send_freshness->invalid_sample_blocked.load()},
          {"send_last_sample_id_ns", lane->send_freshness == nullptr || !GST_CLOCK_TIME_IS_VALID(lane->send_freshness->last_sample_id_ns.load())
              ? Json(nullptr) : Json(lane->send_freshness->last_sample_id_ns.load())},
          {"send_stale_access_units_dropped", lane->send_freshness == nullptr ? 0 : lane->send_freshness->dropped_stale.load()},
          {"send_invalid_access_units_dropped", lane->send_freshness == nullptr ? 0 : lane->send_freshness->dropped_invalid.load()},
          {"send_dependent_access_units_dropped", lane->send_freshness == nullptr ? 0 : lane->send_freshness->dropped_waiting_idr.load()},
          {"send_key_unit_requests", lane->send_freshness == nullptr ? 0 : lane->send_freshness->key_unit_requests.load()},
          {"send_key_unit_request_failures", lane->send_freshness == nullptr ? 0 : lane->send_freshness->key_unit_request_failures.load()},
          {"last_forwarded_frame_age_ms", lane->send_freshness == nullptr ? Json(nullptr) : Json(lane->send_freshness->last_forwarded_age_ms.load())},
          {"failure_count", lane->failure_count.load()},
          {"reopen_count", lane->reopen_count.load()},
          {"encoded_fps", lane->encoded.load() * 1000.0 / static_cast<double>(std::max<std::int64_t>(1, elapsed_ms))},
          {"capture_to_encoded_ms", lane->encode_latency_samples.load() == 0
                                           ? 0.0
                                           : static_cast<double>(lane->encode_latency_total_ms.load()) /
                                                 static_cast<double>(lane->encode_latency_samples.load())},
          {"capture_to_encoded_max_ms", lane->encode_latency_max_ms.load()},
          {"capture_to_encoded_valid", lane->encode_latency_current_valid.load()},
          {"capture_to_encoded_samples", lane->encode_latency_samples.load()},
          {"capture_to_encoded_invalid_samples", lane->encode_latency_invalid_samples.load()},
          {"encoder_output_pts_ms", lane->encoder_output_pts_ms.load()},
          {"encoder_segment_start_ms", lane->encoder_segment_start_ms.load()},
          {"last_capture_age_ms", lane->last_capture_steady_ms.load() == 0 ? -1 :
              steady_now_ms() - lane->last_capture_steady_ms.load()},
          {"last_encoded_age_ms", lane->last_encoded_steady_ms.load() == 0 ? -1 :
              steady_now_ms() - lane->last_encoded_steady_ms.load()},
          {"capture_interval_max_ms", lane->capture_interval_max_ms.load()},
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
      initial_time_sync = signaling.synchronize_time(config.field_safety.time_sync_samples, config.field_safety.max_time_sync_uncertainty_ms);
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
      try {
        if (config.runtime.control_log_commands && !signaling.registration_diagnostics().empty())
          enqueue_control_trace(signaling.registration_diagnostics());
      } catch (...) { note_control_trace_drop(); }
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
    control_transport_fence.reset(config.vehicle_id, signaling.driver_id(), signaling.session_id(), signaling.control_token());
    emergency_stop_receiver.reset(config.vehicle_id, signaling.driver_id(), signaling.session_id(), signaling.control_token());
    feedback_transport_fence.reset(); latest_feedback_status.clear(); pending_control_status.clear();
    const bool inherited_control_inhibition =
        control_recovery_latch->enter_session(signaling.session_id());
    control_inhibited.store(inherited_control_inhibition);
    if (inherited_control_inhibition) {
      emit_diagnostic(
          "vehicle_control_inhibition_retained",
          "control_recovery_inhibition_retained",
          "camera_safety",
          "a sustained control fault already inhibited control for this active session",
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
      auto next_media_log_monotonic_ms = steady_now_ms();
      while (!frame_target_reached(frame_count) && (continuous || signaling.now_ms() < deadline)) {
        while (g_main_context_iteration(nullptr, false)) {
        }
        try {
          flush_outgoing_signals();
          process_signaling();
        } catch (const std::exception& error) {
          bool preserve_direct = false;
          if (detail::transient_cloud_failure(error)) {
            std::lock_guard lock(control_mutex);
            const auto now = steady_now_ms();
            const auto last = native_control_last_accepted_monotonic_ms.load();
            preserve_direct = direct_authority.valid(now) && control_transport_fence.path() == detail::ControlPath::DataChannel &&
                control_link_open && !control_inhibited && control_service_started && control_service &&
                control_service->safety_state() == SafetyState::ControlActive && last > 0 && now >= last &&
                now - last < config.control.degraded_timeout_ms && vehicle_time_readiness().ready;
          }
          if (!preserve_direct) {
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
          // Preserve only the already-authorized healthy PC; no grant, ICE
          // restart, VCU connect or parameter increase can occur offline.
        }
        poll_bus();
        if (!current_pipeline_error().empty()) break;
        enforce_control_recovery();
        start_control_when_cameras_ready();
        // The independent native-control watchdog thread owns the VCU tick;
        // media HTTP latency must not schedule control safety progression.
        // Dedicated time sampling keeps media and control transport progressing.
        if (signaling.now_ms() >= next_media_status_ms) {
          const Json status = {
              {"vehicle_id", config.vehicle_id}, {"session_id", signaling.session_id()}, {"reported_at_utc_ms", signaling.now_ms()},
              {"recording_attempt_id", recording_attempt_id},
              {"codec", to_string(candidate.codec)},
              {"backend", to_string(candidate.backend)},
              {"media_negotiation", media_scope_status()},
              {"control_issue_code", control_inhibited.load() ? std::string(recovery_inhibition_issue()) : control_recovery_latch->braking_wait_for(signaling.session_id()) ? "control_recovery_braking_wait" : ""},
              {"control_recovery", control_recovery_latch->recovery_status_for(signaling.session_id())},
              {"time_sync", signaling.time_sync_status().to_json()},
              {"lanes", lane_metrics(std::max<std::int64_t>(1, signaling.now_ms() - attempt_started))}};
          if (steady_now_ms() >= next_media_log_monotonic_ms) {
            std::cout << Json({
                {"event", "vehicle_media_status"},
                {"event_at_utc_ms", signaling.now_ms()},
                {"event_at_monotonic_ms", steady_now_ms()},
                {"vehicle_id", config.vehicle_id},
                {"session_id", signaling.session_id()},
                {"status", status}}).dump() << '\n';
            next_media_log_monotonic_ms = steady_now_ms() + 5000;
          }
          queue_signal(
              "media_status", status);
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
      for (auto& row : final_lanes) {
        for (const auto& lane : lanes) if (row.value("camera_id", "") == lane->camera.id) row["recording"] = recording_status_for(*lane);
      }
      attempts.back()["lanes"] = final_lanes;
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
    Json summary = {
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
        {"native_control_signaling", {
             {"configured", config.runtime.control_enabled},
             {"transport", "native_signaling_websocket"},
             {"websocket_connected", native_control_websocket_connected.load()},
             {"websocket_connect_timeout_ms",
              kNativeControlWebSocketConnectTimeout.count()},
             {"websocket_receive_timeout_ms",
              kNativeControlWebSocketReceiveTimeout.count()},
             {"watchdog_interval_ms", kNativeControlWatchdogInterval.count()},
             {"connection_attempts_total",
              native_control_websocket_connection_attempts_total.load()},
             {"connections_total",
              native_control_websocket_connections_total.load()},
             {"reconnects_total",
              native_control_websocket_reconnects_total.load()},
             {"transport_errors_total",
              native_control_transport_errors_total.load()},
             {"protocol_errors_total",
              native_control_protocol_errors_total.load()},
             {"consecutive_errors",
              native_control_transport_consecutive_errors.load()},
             {"envelopes_received_total",
              native_control_websocket_envelopes_total.load()},
             {"messages_received_total",
              native_control_websocket_messages_total.load()},
             {"messages_superseded_total",
              native_control_websocket_superseded_messages_total.load()},
             {"post_error_discards_total",
              native_control_websocket_post_error_discards_total.load()},
             {"stale_nonzero_intent_discards_total",
              native_control_stale_nonzero_intents_discarded_total.load()},
             {"stale_safe_heartbeats_forwarded_total",
              native_control_stale_safe_heartbeats_forwarded_total.load()},
             {"stale_safe_heartbeats_accepted_total",
              native_control_stale_safe_heartbeats_accepted_total.load()},
             {"stale_safe_inactive_state_discards_total",
              native_control_stale_safe_inactive_state_discards_total.load()},
             {"stale_safe_native_gap_discards_total",
              native_control_stale_safe_native_gap_discards_total.load()},
             {"stale_safe_untrusted_gear_discards_total",
              native_control_stale_safe_untrusted_gear_discards_total.load()},
             {"fresh_gear_updates_total",
              native_control_fresh_gear_updates_total.load()},
             {"trusted_gear_invalidations_total",
              native_control_trusted_gear_invalidations_total.load()},
             {"estop_gear_freezes_total",
              native_control_estop_gear_freezes_total.load()},
             {"estop_gear_freezes_after_apply_error_total",
              native_control_estop_gear_freezes_after_apply_error_total.load()},
             {"estop_gear_overrides_total",
              native_control_estop_gear_overrides_total.load()},
             {"profile_not_ready_discards_total",
              native_control_profile_not_ready_discards_total.load()},
             {"handshake_not_ready_discards_total",
              native_control_handshake_not_ready_discards_total.load()},
             {"delivery_acks_sent_total",
              native_control_delivery_acks_sent_total.load()},
             {"delivery_acknowledgements_total",
              native_control_delivery_acknowledgements_total.load()},
             {"accepted_commands", accepted_control_commands.load()},
             {"rejected_commands", rejected_control_commands.load()},
             {"last_received_at_utc_ms", last_control_received_at_ms.load()},
             {"watchdog_ticks_total",
              native_control_watchdog_ticks_total.load()},
             {"watchdog_skipped_intervals_total",
              native_control_watchdog_skipped_intervals_total.load()},
             {"last_connected_at_utc_ms",
              native_control_websocket_last_connected_at_ms.load()},
             {"last_message_at_utc_ms",
              native_control_websocket_last_message_at_ms.load()},
             {"last_error_at_utc_ms",
              native_control_transport_last_error_at_ms.load()},
             {"freshness_cutoff_at_utc_ms",
              native_control_command_freshness_cutoff_at_ms.load()},
         }},
    };
    // The caller writes the summary after run() returns.  Drain and join the
    // trace worker first so a late final trace line cannot interleave with it.
    stop_control_trace_worker();
    return summary;
  }

  VehicleConfig config;
  MediaSignalingClient signaling;
  std::shared_ptr<SessionControlRecoveryLatch> control_recovery_latch;
  mutable TimeSyncReadinessGate vehicle_time_sync_readiness_gate;
  int frame_timeout_ms;
  std::filesystem::path recording_root;
  std::unique_ptr<IsolatedRecordingSession> isolated_recording;
  std::vector<Json> recording_final_status;
  std::string recording_attempt_id, recording_current_session, recording_setup_error;
  bool recording_session_incomplete{false};
  std::optional<std::string> forced_codec;
  int simulate_primary_failure_after_frames;
  GstElement* pipeline{nullptr};
  GstElement* webrtc{nullptr};
  detail::ControlTransportFence control_transport_fence;
  detail::DirectAuthorityLease direct_authority;
  bool direct_authority_stop_failed{false}; // control_mutex; terminal until new runtime/session setup
  Json native_control_websocket_authority_scope;
  detail::EmergencyStopReceiver emergency_stop_receiver;
  detail::FeedbackTransportFence feedback_transport_fence;
  std::map<std::string, Json> latest_feedback_status;
  Json latest_control_result;
  std::map<std::string, Json> pending_control_status;
  std::uint64_t last_control_management_seq{0};
  std::uint64_t control_result_seq{0};
  std::atomic<bool> native_control_transport_cloud_capable{false};
  GstWebRTCDataChannel* control_channel{nullptr};
  std::vector<std::unique_ptr<Lane>> lanes;
  std::atomic<bool> stop_requested{false};
  std::shared_ptr<MediaSignalState> media_signal_state{std::make_shared<MediaSignalState>()};
  mutable std::mutex error_mutex;
  std::string pipeline_error;
  std::string pipeline_issue_code;
  std::string pipeline_error_stage;
  std::string pipeline_operator_action;
  bool pipeline_error_retryable{false};
  mutable std::mutex diagnostic_mutex;
  EncoderCandidate active_candidate{EncoderBackend::Nvenc, VideoCodec::H265};
  std::int64_t started_ms{0};
  std::uint64_t gstreamer_warning_count{0};
  std::int64_t next_gstreamer_warning_log_ms{0};
  bool answer_received{false};
  std::optional<std::int64_t> answer_received_at_ms;
  bool control_not_open_warning_fired{false};
  bool simulated_failure_fired{false};
  bool codec_fallback_requested{false};
  std::uint64_t failover_count{0};
  std::string last_negotiation_warning;
  Json ice_configuration{Json::object()};
  std::mutex control_trace_mutex;
  std::condition_variable control_trace_cv;
  std::deque<Json> control_trace_queue;
  std::thread control_trace_worker;
  bool control_trace_stop_requested{false};
  std::atomic<bool> control_trace_accepting{false};
  std::atomic<std::uint64_t> control_trace_enqueued_total{0};
  std::atomic<std::uint64_t> control_trace_dropped_total{0};
  std::mutex control_mutex;
  // Acquired before control_mutex; no control handler takes this mutex.
  std::mutex control_recovery_phase_mutex;
  std::unique_ptr<VehicleControlService> control_service;
  // Guarded by control_mutex. Stale-safe heartbeats may only preserve this
  // accepted fresh gear; ESTOP freezes its gear for the pipeline lifetime.
  std::optional<std::string> native_control_last_accepted_fresh_gear;
  std::optional<std::string> native_control_estop_frozen_gear;
  std::jthread native_control_watchdog_thread;
  std::jthread native_control_websocket_thread;
  std::jthread time_sync_worker;
  std::jthread direct_authority_worker;
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
  std::atomic<std::int64_t> native_control_last_accepted_monotonic_ms{0};
  std::atomic<bool> native_control_websocket_connected{false};
  std::atomic<std::uint64_t> native_control_websocket_connection_attempts_total{0};
  std::atomic<std::uint64_t> native_control_websocket_connections_total{0};
  std::atomic<std::uint64_t> native_control_websocket_reconnects_total{0};
  std::atomic<std::uint64_t> native_control_transport_errors_total{0};
  std::atomic<std::uint64_t> native_control_protocol_errors_total{0};
  std::atomic<std::uint64_t> native_control_transport_consecutive_errors{0};
  std::atomic<std::uint64_t> native_control_websocket_envelopes_total{0};
  std::atomic<std::uint64_t> native_control_websocket_messages_total{0};
  std::atomic<std::uint64_t> native_control_websocket_superseded_messages_total{0};
  std::atomic<std::uint64_t> native_control_websocket_post_error_discards_total{0};
  std::atomic<std::uint64_t>
      native_control_stale_nonzero_intents_discarded_total{0};
  std::atomic<std::uint64_t>
      native_control_stale_safe_heartbeats_forwarded_total{0};
  std::atomic<std::uint64_t>
      native_control_stale_safe_heartbeats_accepted_total{0};
  std::atomic<std::uint64_t>
      native_control_stale_safe_inactive_state_discards_total{0};
  std::atomic<std::uint64_t>
      native_control_stale_safe_native_gap_discards_total{0};
  std::atomic<std::uint64_t>
      native_control_stale_safe_untrusted_gear_discards_total{0};
  std::atomic<std::uint64_t> native_control_fresh_gear_updates_total{0};
  std::atomic<std::uint64_t>
      native_control_trusted_gear_invalidations_total{0};
  std::atomic<std::uint64_t> native_control_estop_gear_freezes_total{0};
  std::atomic<std::uint64_t>
      native_control_estop_gear_freezes_after_apply_error_total{0};
  std::atomic<std::uint64_t> native_control_estop_gear_overrides_total{0};
  std::atomic<std::uint64_t> native_control_profile_not_ready_discards_total{0};
  std::atomic<std::uint64_t> native_control_handshake_not_ready_discards_total{0};
  std::atomic<std::uint64_t> native_control_delivery_acks_sent_total{0};
  std::atomic<std::uint64_t> native_control_delivery_acknowledgements_total{0};
  std::atomic<std::uint64_t> native_control_watchdog_ticks_total{0};
  std::atomic<std::uint64_t> native_control_watchdog_skipped_intervals_total{0};
  std::atomic<std::int64_t> native_control_websocket_last_connected_at_ms{0};
  std::atomic<std::int64_t> native_control_websocket_last_message_at_ms{0};
  std::atomic<std::int64_t> native_control_transport_last_error_at_ms{0};
  std::atomic<std::int64_t> native_control_command_freshness_cutoff_at_ms{0};
  std::atomic<std::int64_t> native_control_transport_last_error_log_monotonic_ms{0};
  std::optional<std::int64_t> last_vcu_status_ms;
  std::uint64_t last_vehicle_telemetry_seq{0};
  std::string control_status_session_id;
  std::uint64_t control_status_seq{0};
  std::uint64_t last_control_rejection_status_seq{0};
  std::string last_vcu_handshake_state;
  std::string last_control_rejection_reason;
  std::optional<SafetyState> last_observed_hard_stop_state;
  std::optional<std::int64_t> last_parsed_control_arrival_ms;
  std::uint64_t last_parsed_control_arrival_seq{0};
  std::uint64_t last_evaluated_control_seq{0};
  std::string last_evaluated_control_reason;
  std::uint64_t last_accepted_control_seq{0};
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
    std::shared_ptr<SessionControlRecoveryLatch> control_recovery_latch)
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
          std::move(control_recovery_latch))) {}

VehicleMediaRuntime::~VehicleMediaRuntime() = default;

Json VehicleMediaRuntime::run(int frame_count, int duration_ms, int capture_interval_ms) {
  return impl_->run(frame_count, duration_ms, capture_interval_ms);
}

}  // namespace mine_teleop
