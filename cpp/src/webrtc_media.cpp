#include "mine_teleop/media.hpp"

#include "mine_teleop/server.hpp"
#include "mine_teleop/surround.hpp"
#include "mine_teleop/media_health.hpp"
#include "mine_teleop/video.hpp"

#include <gst/app/gstappsrc.h>
#include <linux/videodev2.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#if GST_CHECK_VERSION(1,28,0)
#include <gst/webrtc/ice.h>
#include <gst/webrtc/icetransport.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdlib>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <filesystem>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <syncstream>
#include <set>
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

bool camera_encoded_frame_fresh(std::int64_t last_encoded_ms, std::int64_t now_ms, int timeout_ms) {
  return last_encoded_ms > 0 && now_ms >= last_encoded_ms &&
      now_ms - last_encoded_ms <= timeout_ms;
}

CriticalCameraControlLatch::CriticalCameraControlLatch()
    : control_epoch_(std::stoull(random_token(6), nullptr, 16)) {}
std::uint64_t CriticalCameraControlLatch::control_epoch() const {std::lock_guard lock(mutex_);return control_epoch_;}
std::uint64_t CriticalCameraControlLatch::revoke_input(){std::lock_guard lock(mutex_);return ++control_epoch_;}

bool CriticalCameraControlLatch::rebuild_allowed() const {std::lock_guard lock(mutex_);return rebuild_allowed_;}
void CriticalCameraControlLatch::confirm_parked_rebuild(){std::lock_guard lock(mutex_);rebuild_allowed_=true;}

bool CriticalCameraControlLatch::enter_session(std::string_view session_id) {
  if (session_id.empty()) {
    throw std::invalid_argument("critical camera control latch requires a non-empty session id");
  }
  std::lock_guard lock(mutex_);
  if (session_id_ != session_id) {
    if(!session_id_.empty())++control_epoch_;
    session_id_ = session_id;
    inhibited_ = false;
    armed_ = false;
    rebuild_allowed_=true;
    startup_started_ms_.reset();
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

bool CriticalCameraControlLatch::startup_grace_active(std::string_view session_id, std::int64_t now_ms) {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id || now_ms < 0) {
    throw std::logic_error("invalid critical camera startup scope or time");
  }
  if (!startup_started_ms_) startup_started_ms_ = now_ms;
  return !inhibited_ && !armed_ && now_ms >= *startup_started_ms_ &&
      now_ms - *startup_started_ms_ < kCriticalCameraStartupTimeoutMs;
}

bool CriticalCameraControlLatch::arm_for_control(std::string_view session_id) {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id) {
    throw std::logic_error("critical camera control scope does not match the active session");
  }
  if (inhibited_) return false;
  armed_ = true;
  rebuild_allowed_=false;
  return true;
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

  [[nodiscard]] std::string native_control_websocket_url() const {
    require_session();
    return signaling_websocket_url(
               origin_,
               session_id_,
               vehicle_id_,
               std::to_string(connection_generation_)) +
        "&types=control_command,media_quiesce";
  }

  [[nodiscard]] HttpHeaders native_control_websocket_headers() const {
    require_session();
    return {{"X-Mine-Teleop-Device-Token", device_token_}};
  }

  Json ice_servers(std::string_view attempt="") {
    require_session();
    return http_.get_json(
        origin_ + "/sessions/" + http_.url_encode(session_id_) + "/ice_servers?actor=" +
            http_.url_encode(vehicle_id_) + "&connection_generation=" + std::to_string(connection_generation_)+"&media_attempt_id="+http_.url_encode(attempt),
        {{"X-Mine-Teleop-Device-Token", device_token_}});
  }

  Json relay(std::string_view operation,Json payload){
    payload["actor"]=vehicle_id_;payload["device_token"]=device_token_;payload["connection_generation"]=connection_generation_;
    return http_.post_json_response(origin_+"/sessions/"+http_.url_encode(session_id_)+"/relay/"+std::string(operation),payload);
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
    std::mutex frame_mutex;
    std::shared_ptr<const EncodedFrame> latest;
    std::deque<std::shared_ptr<const EncodedFrame>> history;
    std::uint64_t source_generation{};
    struct Provenance { std::vector<SourceFrameIdentity> inputs; bool healthy; Json stages=Json::object(); };
    std::map<GstClockTime,Provenance> provenance;
    std::map<GstClockTime,Json> encoded_provenance;
    std::uint32_t last_trace_timestamp{};
    bool trace_seen{};int actual_h264_level{};
    GstSegment encoded_segment;
    Lane(){gst_segment_init(&encoded_segment,GST_FORMAT_TIME);}
  };

  Impl(
      VehicleConfig next_config,
      std::string signaling_url,
      std::string device_token,
      int next_frame_timeout_ms,
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
        forced_codec(std::move(next_forced_codec)),
        simulate_primary_failure_after_frames(next_simulate_primary_failure_after_frames) {
    if (frame_timeout_ms <= 0) throw std::invalid_argument("frame timeout must be positive");
    if (simulate_primary_failure_after_frames < 0) {
      throw std::invalid_argument("simulated primary failure frame count must be non-negative");
    }
    const auto lock_root=std::filesystem::path(std::getenv("MINE_TELEOP_CAMERA_LOCK_DIR")?std::getenv("MINE_TELEOP_CAMERA_LOCK_DIR"):"/run/lock/mine-teleop-cameras");
    std::filesystem::create_directories(lock_root);
    maintenance_lock_fd=::open((lock_root/("vehicle-"+sha256_text(config.vehicle_id)+".lock")).c_str(),O_CREAT|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
    if(maintenance_lock_fd<0||::flock(maintenance_lock_fd,LOCK_SH|LOCK_NB)!=0){if(maintenance_lock_fd>=0)::close(maintenance_lock_fd);maintenance_lock_fd=-1;throw std::runtime_error("vehicle cameras locked for calibration maintenance");}
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
    if(maintenance_lock_fd>=0)::close(maintenance_lock_fd);
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
    if(GST_PAD_PROBE_INFO_TYPE(info)&GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM){
      auto* event=GST_PAD_PROBE_INFO_EVENT(info);
      if(GST_EVENT_TYPE(event)==GST_EVENT_SEGMENT){const GstSegment* segment=nullptr;gst_event_parse_segment(event,&segment);std::lock_guard lock(lane->frame_mutex);lane->encoded_segment=*segment;}
      return GST_PAD_PROBE_OK;
    }
    if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
      ++lane->encoded;
      const auto encoded_at_ms = lane->owner->signaling.now_ms();
      lane->last_encoded_ms = encoded_at_ms;
      lane->last_encoded_steady_ms = steady_now_ms();
      auto* encoded=GST_PAD_PROBE_INFO_BUFFER(info);
      if(encoded&&GST_BUFFER_PTS_IS_VALID(encoded)){
        std::lock_guard lock(lane->frame_mutex);const auto pts=gst_segment_to_running_time(&lane->encoded_segment,GST_FORMAT_TIME,GST_BUFFER_PTS(encoded));auto it=lane->provenance.find(pts);
        if(it!=lane->provenance.end()){
          const bool credited=lane->owner->health.encoded(std::hash<std::string>{}(lane->camera.id),it->second.inputs,steady_now_ms(),it->second.healthy);
          if(lane->owner->config.runtime.media_frame_trace){
            Json sources=Json::array();
            for(const auto& f:it->second.inputs)sources.push_back({
                {"camera_id",f.camera_id},{"source_generation",f.source_generation},
                {"source_sequence",f.sequence},{"source_steady_ms",f.captured_steady_ms},{"time_quality",f.time_quality},{"read_started_steady_ms",f.read_started_steady_ms},{"read_finished_steady_ms",f.read_finished_steady_ms},
                {"frame_id",lane->owner->media_attempt_id+":"+f.camera_id+":"+std::to_string(f.source_generation)+":"+std::to_string(f.sequence)}});
            lane->encoded_provenance[pts]={{"stream_id",lane->camera.id},
                {"composite_id",lane->owner->media_attempt_id+":"+lane->camera.id+":"+std::to_string(pts)},
                {"pts_ns",pts},{"stages",it->second.stages},{"source_frames",sources},{"healthy",credited},
                {"encoded_steady_ms",steady_now_ms()}};
            while(lane->encoded_provenance.size()>64)lane->encoded_provenance.erase(lane->encoded_provenance.begin());
          }
          lane->provenance.erase(lane->provenance.begin(),std::next(it));
        }
      }
      GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
      if (buffer != nullptr && GST_BUFFER_PTS_IS_VALID(buffer)) {
        GstClockTime running;{std::lock_guard lock(lane->frame_mutex);running=gst_segment_to_running_time(&lane->encoded_segment,GST_FORMAT_TIME,GST_BUFFER_PTS(buffer));}
        if(running==GST_CLOCK_TIME_NONE)return GST_PAD_PROBE_OK;
        const auto captured_at_ms = lane->pipeline_started_steady_ms + static_cast<std::int64_t>(running / GST_MSECOND);
        const auto latency_ms = static_cast<std::uint64_t>(std::max<std::int64_t>(
            0, steady_now_ms() - captured_at_ms));
        ++lane->encode_latency_samples;
        lane->encode_latency_total_ms += latency_ms;
        auto observed = lane->encode_latency_max_ms.load();
        while (latency_ms > observed && !lane->encode_latency_max_ms.compare_exchange_weak(observed, latency_ms)) {
        }
      }
    }
    return GST_PAD_PROBE_OK;
  }

  static GstPadProbeReturn trace_rtp(GstPad*,GstPadProbeInfo* info,gpointer user_data){
    auto* lane=static_cast<Lane*>(user_data);
    if(!lane->owner->config.runtime.media_frame_trace)return GST_PAD_PROBE_OK;
    auto inspect=[&](GstBuffer* buffer){
      if(!buffer||!GST_BUFFER_PTS_IS_VALID(buffer))return;
      GstMapInfo bytes;if(!gst_buffer_map(buffer,&bytes,GST_MAP_READ))return;
      const auto* b=bytes.data;
      if(bytes.size>=12&&(b[0]>>6)==2&&(b[1]&128)){
        const auto stamp=(std::uint32_t(b[4])<<24)|(std::uint32_t(b[5])<<16)|(std::uint32_t(b[6])<<8)|b[7];
        std::lock_guard lock(lane->frame_mutex);
        const auto pts=gst_segment_to_running_time(&lane->encoded_segment,GST_FORMAT_TIME,GST_BUFFER_PTS(buffer));
        const auto it=lane->encoded_provenance.find(pts);
        if(it!=lane->encoded_provenance.end()&&(!lane->trace_seen||lane->last_trace_timestamp!=stamp)){
          auto trace=it->second;trace["rtp_timestamp"]=stamp;
          trace["ssrc"]=(std::uint32_t(b[8])<<24)|(std::uint32_t(b[9])<<16)|(std::uint32_t(b[10])<<8)|b[11];
          trace["rtp_marker_sequence"]=(unsigned(b[2])<<8)|b[3];
          trace["rtp_steady_ms"]=steady_now_ms();lane->trace_seen=true;lane->last_trace_timestamp=stamp;
          lane->owner->queue_signal("media_frame",std::move(trace));
        }
      }
      gst_buffer_unmap(buffer,&bytes);
    };
    if(GST_PAD_PROBE_INFO_TYPE(info)&GST_PAD_PROBE_TYPE_BUFFER)inspect(GST_PAD_PROBE_INFO_BUFFER(info));
    if(GST_PAD_PROBE_INFO_TYPE(info)&GST_PAD_PROBE_TYPE_BUFFER_LIST){
      auto* list=GST_PAD_PROBE_INFO_BUFFER_LIST(info);for(unsigned i=0;i<gst_buffer_list_length(list);++i)inspect(gst_buffer_list_get(list,i));
    }
    return GST_PAD_PROBE_OK;
  }

  static void on_ice_candidate(GstElement* origin, guint mline_index, gchar* candidate, gpointer user_data) {
    auto* self = static_cast<Impl*>(user_data);
    if(self->stop_requested||origin!=self->webrtc)return;
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
        {{"candidate", candidate?candidate:""}, {"end_of_candidates",candidate==nullptr||*candidate=='\0'}, {"sdpMLineIndex", mline_index}});
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

  struct OfferContext {Impl* owner;GstElement* origin;std::string attempt;};
  static void on_offer_created(GstPromise* promise, gpointer user_data) {
    auto* context=static_cast<OfferContext*>(user_data);auto* self=context->owner;
    if(self->stop_requested||context->origin!=self->webrtc||context->attempt!=self->media_attempt_id){
      gst_promise_unref(promise);return;
    }
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

    auto completion=std::make_shared<DescriptionCompletion>();
    GstPromise* local=gst_promise_new_with_change_func(description_completed,
        new std::shared_ptr<DescriptionCompletion>(completion),destroy_description_completion);
    g_signal_emit_by_name(self->webrtc,"set-local-description",offer,local);

    gchar* text = gst_sdp_message_as_text(offer->sdp);
    Json tracks = Json::array();
    for (const auto& lane : self->lanes) {
      tracks.push_back({
          {"camera_id", lane->camera.id},
          {"stream_id",lane->camera.id},
          {"kind",lane->camera.id=="drive_mosaic"?"drive_mosaic":lane->camera.id=="surround_bev"?"surround_bev":lane->camera.id=="fish_diagnostic"?"fish_diagnostic":"camera"},
          {"source_camera_ids",lane->camera.id=="drive_mosaic"?Json::array({"drive_front","drive_rear"}):(lane->camera.id=="surround_bev"||lane->camera.id=="fish_diagnostic")?Json::array({"fish_front","fish_rear","fish_left","fish_right"}):Json::array({lane->camera.id})},
          {"regions",lane->camera.id=="drive_mosaic"?Json::array({{{"camera_id","drive_front"},{"x",0},{"y",0},{"width",lane->profile.width},{"height",lane->profile.height/2}},{{"camera_id","drive_rear"},{"x",0},{"y",lane->profile.height/2},{"width",lane->profile.width},{"height",lane->profile.height/2}}}):Json::array()},
          {"calibration_hash",self->calibration?self->calibration->hash:""},
          {"codec", to_string(self->active_candidate.codec)},
          {"backend", to_string(self->active_candidate.backend)},
          {"width", lane->profile.width},
          {"height", lane->profile.height},
          {"fps", lane->profile.fps},
          {"minimum_h264_level_idc",minimum_h264_level_idc(lane->profile.width,lane->profile.height,lane->profile.fps)},
          {"bitrate_kbps", lane->profile.bitrate_kbps},
          {"driving_available",!self->config.surround.diagnostic_partition},
      });
      if(lane->camera.id=="fish_diagnostic"){
        Json regions=Json::array();const std::array<std::string,4> ids{"fish_front","fish_rear","fish_left","fish_right"};
        for(unsigned i=0;i<4;++i)regions.push_back({{"camera_id",ids[i]},{"x",(i%2)*1280},{"y",(i/2)*720},{"width",1280},{"height",720}});
        tracks.back()["regions"]=std::move(regions);
      }
    }
    Json published_calibration=self->calibration?self->calibration->document:Json(nullptr);
    if(self->calibration&&!self->two_stream)for(const auto& lane:self->lanes){
      if(!published_calibration.at("cameras").contains(lane->camera.id))continue;
      auto& c=published_calibration["cameras"][lane->camera.id];
      const auto size=c.at("runtime_size").get<std::array<int,2>>();
      const double sx=double(lane->profile.width)/size[0],sy=double(lane->profile.height)/size[1];
      auto a=c.at("A_runtime_from_calibration").get<std::array<double,9>>();
      for(unsigned k=0;k<3;++k){a[k]=sx*a[k]+(.5*sx-.5)*a[6+k];a[3+k]=sy*a[3+k]+(.5*sy-.5)*a[6+k];}
      c["A_runtime_from_calibration"]=a;c["runtime_size"]={lane->profile.width,lane->profile.height};
      c["image_transform"]["resize"]=c["runtime_size"];
    }
    std::lock_guard description_lock(self->description_mutex);
    self->local_description={local,completion,Json{
        {"type","webrtc_offer"},{"payload",
        {{"type", "offer"},{"vehicle_id",self->config.vehicle_id},{"media_protocol_version",2},
         {"sdp", text == nullptr ? "" : text},
         {"codec", to_string(self->active_candidate.codec)},
         {"backend", to_string(self->active_candidate.backend)},
         {"media_tracks", std::move(tracks)},
         {"media_mode",self->config.surround.diagnostic_partition?"two-partition-diagnostic":self->two_stream?"two":"full"},
         {"calibration",published_calibration},
         {"media_attempt_id",self->media_attempt_id},{"relay_lease_id",self->relay_activated?self->relay_lease.value("lease_id",""):""},{"control_epoch",self->critical_camera_control_latch->control_epoch()}}}}};
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
    if(self->stop_requested||webrtc!=self->webrtc)return;
    self->offer_requested=true;
  }

  void queue_signal(std::string type, Json payload) {
    std::lock_guard lock(signal_mutex);
    payload["media_attempt_id"]=media_attempt_id;
    if(type=="media_frame"&&pending_signals.size()>=256)return;
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
    details["media_attempt_id"]=media_attempt_id;
    details["control_epoch"]=critical_camera_control_latch->control_epoch();
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
      critical_camera_control_latch->revoke_input();
      const bool retain_latched_fault=control_service &&
          (control_service->safety_state()==SafetyState::Estop || control_service->safety_state()==SafetyState::Fault);
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
      if(!retain_latched_fault)control_service.reset();
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

  void handle_control_message(
      GstWebRTCDataChannel* channel,
      std::string_view data,
      ControlMessageTransport transport,
      const NativeControlDeliveryTraceContext* delivery_trace) {
    check_local_relay_deadline();
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
    if(relay_path_invalid.load()){++rejected_control_commands;return;}
    try {
      const auto message = Json::parse(data);
      if(control_quiescing||message.value("control_epoch",std::uint64_t{0})!=critical_camera_control_latch->control_epoch()){
        ++rejected_control_commands;return;
      }
      if (message.value("type", "") == "session_control_profile") {
        if (transport != ControlMessageTransport::DataChannel) {
          ++rejected_control_commands;
          return;
        }
        const auto request = SessionControlProfileRequest::from_json(message);
        std::lock_guard lock(control_mutex);
        if (control_channel != channel) return;
        if (control_quiescing || message.value("control_epoch",std::uint64_t{0})!=critical_camera_control_latch->control_epoch()) return;
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
        if (transport != ControlMessageTransport::DataChannel) {
          ++rejected_control_commands;
          return;
        }
        const auto action = message.value("action", "");
        std::lock_guard lock(control_mutex);
        if (control_channel != channel) return;
        if (control_quiescing || message.value("control_epoch",std::uint64_t{0})!=critical_camera_control_latch->control_epoch()) return;
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
      if (transport == ControlMessageTransport::DataChannel) {
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
      const bool intent_fresh = message.value("intent_fresh", false);
      const bool stale_safe_heartbeat =
          !command.estop && !intent_fresh &&
          command.steering == 0.0 && command.throttle == 0.0 &&
          command.brake == 0.0;
      const auto control_mutex_wait_started_monotonic_ms = steady_now_ms();
      std::unique_lock lock(control_mutex);
      // A callback can pass the initial check before quiesce takes the mutex.
      // Validate again at the actuation boundary, after any epoch transition.
      if(control_quiescing||command.control_epoch!=critical_camera_control_latch->control_epoch()){
        ++rejected_control_commands;return;
      }
      const auto control_mutex_acquired_monotonic_ms = steady_now_ms();
      const auto control_mutex_acquired_at_utc_ms = signaling.now_ms();
      const auto active_session_id = signaling.session_id();
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
          !control_service_started || !control_service || !control_link_open) {
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
        if (control_service->safety_state() != SafetyState::ControlActive) {
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
        const auto previous_accepted_monotonic_ms =
            native_control_last_accepted_monotonic_ms.load();
        if (previous_accepted_monotonic_ms <= 0 ||
            control_mutex_acquired_monotonic_ms < previous_accepted_monotonic_ms ||
            control_mutex_acquired_monotonic_ms - previous_accepted_monotonic_ms >=
                config.control.degraded_timeout_ms) {
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
      const auto receive_apply_started_at_utc_ms = signaling.now_ms();
      const auto receive_apply_started_monotonic_ms = steady_now_ms();
      ReceiveResult result;
      std::int64_t receive_apply_completed_at_utc_ms = 0;
      std::int64_t receive_apply_completed_monotonic_ms = 0;
      try {
        result = control_service->receive_command(command, received_at_ms);
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
          send_control_command_rejected_locked(command.seq, intent_seq, result.issue_code);
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
                  : feedback_problem
                  ? "Inspect VCU feedback freshness and the VCU JSONL log before requesting control."
                  : "Inspect command identity, sequence, timing, token, and configured safety limits.",
              true,
              {{"reason", result.reason},
               {"transport", transport_name},
               {"safety_action",
                result.issue_code == "vcu_drive_gear_change_moving_or_stale"
                    ? "traction_withdrawn_retained_gear"
                    : "local_full_stop"}});
        }
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
    const auto envelope_received_at_utc_ms = signaling.now_ms();
    const auto envelope_received_monotonic_ms = steady_now_ms();
    if (!envelope.is_object()) {
      throw std::invalid_argument("native control WebSocket envelope must be an object");
    }
    if (envelope.contains("error")) {
      throw std::runtime_error(
          envelope.value("error", "native control WebSocket was rejected"));
    }
    const auto event = envelope.value("event", "");
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
    bool handled_quiesce=false;
    for (const auto& message : messages) {
      try {
        if(message.value("type","")=="media_quiesce"){
          request_quiesce(message.at("payload"));handled_quiesce=true;continue;
        }
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
    if(!selected&&handled_quiesce){websocket.send_json({{"event","signaling_delivery_ack"},{"delivery_cursor",delivery_cursor}},kNativeControlWebSocketSendTimeout);return;}
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
          &delivery_trace);
    }

    // Acknowledge the delivered view even when its ordinary command was
    // intentionally discarded. This prevents reconnect from replaying an old
    // mailbox entry; a newer controller sample is required to resume control.
    const auto ack_send_started_at_utc_ms = signaling.now_ms();
    const auto ack_send_started_monotonic_ms = steady_now_ms();
    try {
      websocket.send_json(
          {{"event", "signaling_delivery_ack"},
           {"delivery_cursor", delivery_cursor},
           {"trace_session_id", payload.value("session_id", "")},
           {"seq", payload.value("seq", std::uint64_t{0})},
           {"intent_seq", payload.value("intent_seq", std::uint64_t{0})}},
          kNativeControlWebSocketSendTimeout);
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
        enqueue_control_trace({
            {"stage", "delivery_ack_sent"},
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
        });
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
           !lifecycle_stopping.load()) {
      const auto slice = std::min(remaining, std::chrono::milliseconds(25));
      std::this_thread::sleep_for(slice);
      remaining -= slice;
    }
    return !stop_token.stop_requested() && !lifecycle_stopping.load();
  }

  void native_control_websocket_loop(std::stop_token stop_token) noexcept {
    auto reconnect_delay = kNativeControlReconnectInitialDelay;
    while (!stop_token.stop_requested() && !lifecycle_stopping.load()) {
      native_control_websocket_connection_attempts_total.fetch_add(
          1,
          std::memory_order_relaxed);
      try {
        WebSocketClient websocket(
            kNativeControlWebSocketConnectTimeout,
            config.cloud.resolve_entries,
            config.cloud.ca_bundle);
        websocket.connect(
            signaling.native_control_websocket_url(),
            signaling.native_control_websocket_headers());
        if (stop_token.stop_requested() || lifecycle_stopping.load()) break;
        native_control_websocket_connected.store(true, std::memory_order_relaxed);
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
        while (!stop_token.stop_requested() && !lifecycle_stopping.load()) {
          const auto received = websocket.receive_json(
              kNativeControlWebSocketReceiveTimeout);
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
      } catch (const std::invalid_argument& error) {
        if (!stop_token.stop_requested() && !lifecycle_stopping.load()) {
          note_native_control_transport_error(error.what(), true);
        }
      } catch (const std::exception& error) {
        if (!stop_token.stop_requested() && !lifecycle_stopping.load()) {
          note_native_control_transport_error(error.what(), false);
        }
      } catch (...) {
        if (!stop_token.stop_requested() && !lifecycle_stopping.load()) {
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
    while (!stop_token.stop_requested() && !lifecycle_stopping.load()) {
      const auto before_wait = std::chrono::steady_clock::now();
      if (before_wait < next_tick) std::this_thread::sleep_until(next_tick);
      if (stop_token.stop_requested() || lifecycle_stopping.load()) break;
      next_tick += kNativeControlWatchdogInterval;
      native_control_watchdog_ticks_total.fetch_add(1, std::memory_order_relaxed);
      try {
        check_local_relay_deadline();
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

  void start_native_control_transport() {
    if (!config.runtime.control_enabled || stop_requested.load()) return;
    if (native_control_websocket_thread.joinable() ||
        native_control_watchdog_thread.joinable()) {
      throw std::logic_error("native control transport threads are already running");
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
      native_control_watchdog_thread.request_stop();
      native_control_watchdog_thread.join();
      throw;
    }
  }

  void stop_native_control_transport_threads() {
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
  }

  void send_control_command_rejected_locked(
      std::uint64_t command_seq,
      std::uint64_t intent_seq,
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
        {"control_epoch",critical_camera_control_latch->control_epoch()},
        {"media_attempt_id",media_attempt_id},
        {"command_seq", command_seq},
        {"intent_seq", intent_seq},
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
    payload_value["control_epoch"]=critical_camera_control_latch->control_epoch();
    payload_value["media_attempt_id"]=media_attempt_id;
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
    message["control_epoch"]=critical_camera_control_latch->control_epoch();
    message["media_attempt_id"]=media_attempt_id;
    if (control_service_started && control_service) {
      message["hard_limits"] = control_service->control_limits();
      message["session_control_profile"] =
          control_service->session_control_profile();
    }
    const auto payload = message.dump();
    gst_webrtc_data_channel_send_string(control_channel, payload.c_str());
  }

  [[nodiscard]] Json configured_control_limits() const {
    return vehicle_control_limits(config);
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
          {"control_epoch",critical_camera_control_latch->control_epoch()},
          {"protocol_version", kProtocolVersion},
          {"vehicle_id", config.vehicle_id},
          {"driver_id", signaling.driver_id()},
          {"session_id", signaling.session_id()},
          {"control_status_seq", ++control_status_seq},
        {"control_epoch",critical_camera_control_latch->control_epoch()},
        {"media_attempt_id",media_attempt_id},
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
      critical_camera_control_latch->revoke_input();
      control_service_issue_code = "critical_camera_failed";
      invalidate_native_control_trusted_gear_locked();
      if (control_service_started && control_service) {
        try {
          control_service->close({
              VehicleStopSource::SoftwareFault,
              VehicleStopReason::CriticalCameraFailed});
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

    const auto last_encoded_ms = lane.last_encoded_steady_ms.load();
    emit_diagnostic(
        "vehicle_control_inhibited_by_camera",
        "critical_camera_control_inhibited",
        "camera_safety",
        std::string(failure),
        "Keep the vehicle stopped. Camera recovery restores video only; end this session and complete a fresh VCU handshake before driving again.",
        false,
        {{"camera_id", lane.camera.id},
         {"device", lane.camera.device},
         {"safety_action", "local_full_stop_control_channel_closed_video_continues"},
         {"captured_frames", lane.captured.load()},
         {"encoded_frames", lane.encoded.load()},
         {"last_encoded_age_ms", last_encoded_ms > 0
             ? Json(steady_now_ms() - last_encoded_ms) : Json(nullptr)},
         {"startup_timeout_ms", kCriticalCameraStartupTimeoutMs}});
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
    return health.stale(steady_now_ms(),frame_timeout_ms,true).empty();
  }

  void enforce_critical_camera_freshness() {
    if(!config.runtime.control_enabled||stop_requested||control_inhibited)return;
    const auto now=steady_now_ms();
    if(critical_camera_control_latch->startup_grace_active(signaling.session_id(),now))return;
    const auto failure=health.stale(now,frame_timeout_ms,false);
    if(failure.empty())return;
    for(auto* lane:capture_lanes())if(lane->camera.critical_for_control&&failure.starts_with(lane->camera.id+":")){
      inhibit_control_for_critical_camera(*lane,failure);return;
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

  [[nodiscard]] bool start_control_service(bool parking_only=false) {
    if (!config.runtime.control_enabled) return false;
    try {
      {
        std::lock_guard lock(control_mutex);
        if (control_service && (control_service->safety_state()==SafetyState::Estop || control_service->safety_state()==SafetyState::Fault)) {
          control_service_issue_code="latched_fault_requires_existing_reset";
          return false;
        }
        if (control_service_started && control_service) return true;
        // The on-open callback can race session teardown.  Recheck lifecycle
        // state while holding the same mutex used by stop_pipeline before
        // opening CAN, otherwise a late callback could resurrect the adapter
        // after the session has already closed.
        if (lifecycle_stopping || (stop_requested && !parking_only) || (!parking_only && (!control_link_open || control_channel == nullptr))) {
          return false;
        }
        if (control_inhibited) {
          control_service_issue_code = "critical_camera_failed";
          return false;
        }
        if (!parking_only && !critical_cameras_ready()) {
          control_service_issue_code = "critical_camera_not_ready";
          return false;
        }
        if (!parking_only && !critical_camera_control_latch->arm_for_control(signaling.session_id())) {
          control_service_issue_code = "critical_camera_failed";
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
        if (lifecycle_stopping || (stop_requested && !parking_only) || control_inhibited) {
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

  void request_quiesce(const Json& request){
    if(lifecycle_stopping)return;
    if(request.value("media_attempt_id","")!=media_attempt_id||request.value("control_epoch",std::uint64_t{0})!=critical_camera_control_latch->control_epoch())return;
    const auto mode=request.value("mode",config.surround.mode),profile=request.value("profile",config.surround.profile);
    if((mode!="full"&&mode!="two")||(profile!="720p"&&profile!="540p")||request.value("request_id","").empty())return;
    if(!control_inhibited)static_cast<void>(start_control_service(true));
    std::lock_guard lock(control_mutex);
    if(control_service && (control_service->safety_state()==SafetyState::Estop||control_service->safety_state()==SafetyState::Fault)){queue_signal("media_quiesce_ack",{{"request_id",request.at("request_id")},{"parked",false},{"control_epoch",critical_camera_control_latch->control_epoch()},{"reason","latched_fault_requires_existing_reset"}});return;}
    if(!control_service_started||!control_service){queue_signal("media_quiesce_ack",{{"request_id",request.at("request_id")},{"parked",false},{"reason","vehicle_feedback_unavailable"}});return;}
    control_quiescing=true;critical_camera_control_latch->revoke_input();invalidate_native_control_trusted_gear_locked();
    native_control_command_freshness_cutoff_at_ms=signaling.now_ms();
    control_service->disconnect_vcu_handshake();
    quiesce_request=request;quiesce_deadline=steady_now_ms()+10000;
  }
  void check_quiesce(){
    std::lock_guard lock(control_mutex);
    if(quiesce_request.is_null()||!control_service)return;
    const auto handshake=control_service->vcu_handshake_status();
    const auto& history=control_service->telemetry_history();
    bool fresh=false;
    if(!history.empty()){const auto& last=history.back();const auto age=signaling.now_ms()-last.value("sent_at_utc_ms",std::int64_t{0});fresh=age>=0&&age<=std::min(200,config.field_safety.speed_feedback_timeout_ms)&&!last.value("estop",false)&&last.value("can_feedback",Json::object()).value("feedback_fresh",false);}
    const bool parked=fresh&&handshake.speed_valid&&std::abs(handshake.speed_mps)<=.1&&handshake.parking_ready&&!handshake.ready&&!handshake.requested&&!handshake.disarming&&handshake.handshake_valid;
    if(!parked&&steady_now_ms()<quiesce_deadline)return;
    queue_signal("media_quiesce_ack",{{"request_id",quiesce_request.at("request_id")},{"parked",parked},{"control_epoch",critical_camera_control_latch->control_epoch()},{"reason",parked?"vehicle_confirmed_parked":"parking_feedback_not_confirmed"}});
    if(parked){config.surround.mode=quiesce_request.value("mode",config.surround.mode);config.surround.profile=quiesce_request.value("profile",config.surround.profile);critical_camera_control_latch->confirm_parked_rebuild();media_rebuild_requested=true;}
    quiesce_request=nullptr;
  }
  void tick_control_service() {
    std::unique_lock lock(control_mutex);
    if (lifecycle_stopping || (stop_requested && !control_quiescing) || control_inhibited || !control_service_started || !control_service) return;
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
          if(config.surround.mode!="full"&&supports(VideoCodec::H264))return {VideoCodec::H264};
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
          << "! " << elementary_caps << ",stream-format=byte-stream,alignment=au"
          << (encoder.codec()==VideoCodec::H264 ? ",level=(string)"+h264_level_name(minimum_h264_level_idc(lane->profile.width,lane->profile.height,lane->profile.fps))+" " : "") << ' '
          << "! valve name=gate_" << id << " drop=" << (media_admission_ready?"false":"true") << " drop-mode=transform-to-gap "
          << "! tee name=encoded_" << id << ' '

          << "encoded_" << id << ". ! queue max-size-buffers=2 max-size-bytes=0 max-size-time=0 leaky=downstream "
          << "! " << payloader << " name=pay_" << id << " config-interval=-1 pt=" << payload_type << ' '
          << "! capsfilter name=rtp_caps_" << id << " caps=\"application/x-rtp,media=video,encoding-name=" << encoding_name << ",payload=" << payload_type
          << (encoder.codec() == VideoCodec::H265
                  ? ",profile-id=(string)1,tier-flag=(string)0,tx-mode=(string)SRST"
                  : "")
          << "\" ! webrtc. ";
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
    lanes.clear();inputs.clear();surround_renderer.reset();
    two_stream=false;calibration.reset();
    if(!config.surround.diagnostic_partition&&!config.surround.calibration_file.empty()) {
      try{calibration=load_surround_calibration(config.surround.calibration_file,config);
        const auto& approved=calibration->document.at("acceptance");
        if(approved.at("vehicle_environment")!=media_environment(config,active_candidate))throw std::runtime_error("vehicle CPU/driver/GStreamer/encoder baseline changed; repeat phase-one qualification");
        if(config.surround.max_skew_ms==0)config.surround.max_skew_ms=approved.at("timing").at("hard_max_skew_ms").get<int>();
        if(config.surround.max_frame_age_ms>approved.at("timing").at("max_frame_age_ms").get<int>())throw std::runtime_error("configured age exceeds measured admission limit");
        if(config.surround.max_skew_ms>approved.at("timing").at("hard_max_skew_ms").get<int>())throw std::runtime_error("configured alignment limit exceeds the measured admission limit");
        two_stream=config.surround.mode!="full";}
      catch(const std::exception& error){if(config.surround.mode=="two")throw;emit_diagnostic("vehicle_surround_unavailable","calibration_not_qualified","calibration",error.what(),"Complete calibration and field acceptance before enabling two-stream driving.",false);}
    }
    if(config.surround.diagnostic_partition){
      if(config.runtime.control_enabled||config.field_safety.commissioning_mode!="bench")throw std::runtime_error("diagnostic partition is prohibited for driving");
      two_stream=true;config.surround.max_skew_ms=40;
    }
    if(config.surround.mode=="two"&&!two_stream)throw std::runtime_error("two-stream mode requires accepted calibration");
    std::vector<std::string> critical;
    for (const auto& camera : config.enabled_cameras()) {
      auto lane = std::make_unique<Lane>();
      lane->owner = this;
      lane->camera = camera;
      lane->profile = config.realtime_profile(camera.realtime_profile);
      lane->input = camera_input_spec(camera, lane->profile);
      if(camera.critical_for_control)critical.push_back(camera.id);
      lanes.push_back(std::move(lane));
    }
    health.reset(critical,steady_now_ms());
    if(two_stream) {
      const std::array<std::string,6> ids{"drive_front","drive_rear","fish_front","fish_rear","fish_left","fish_right"};
      for(const auto& id:ids)if(std::none_of(lanes.begin(),lanes.end(),[&](const auto& x){return x->camera.id==id&&x->camera.critical_for_control;}))throw std::runtime_error("two-stream mode requires six critical cameras: "+id);
      if(lanes.size()!=6)throw std::runtime_error("two-stream mode requires exactly six configured inputs");
      inputs=std::move(lanes);lanes.clear();
      const bool low=config.surround.profile=="540p";
      for(int i=0;i<2;++i){auto lane=std::make_unique<Lane>();lane->owner=this;
        lane->camera.id=i?(config.surround.diagnostic_partition?"fish_diagnostic":"surround_bev"):"drive_mosaic";lane->camera.critical_for_control=true;
        lane->profile=config.realtime_profile(inputs.front()->camera.realtime_profile);
        lane->profile.width=i?(low?384:512):(low?960:1280);
        lane->profile.height=i?(low?672:896):(low?1080:1440);
        lane->profile.fps=i?20:30;lane->profile.bitrate_kbps=i?(low?800:1200):(low?1400:2800);
        if(config.surround.diagnostic_partition){lane->profile.width=i?2560:1280;lane->profile.height=1440;lane->profile.fps=30;lane->profile.bitrate_kbps=i?6000:3000;}
        lane->input={"rgba",lane->profile.width,lane->profile.height,lane->profile.fps};
        lanes.push_back(std::move(lane));
      }
      if(!config.surround.diagnostic_partition)surround_renderer=std::make_unique<surround::Renderer>(calibration->cameras,calibration->region,
          calibration->vehicle_length,calibration->vehicle_width,lanes[1]->profile.width,lanes[1]->profile.height);
    }
  }

  std::vector<Lane*> capture_lanes() const {
    std::vector<Lane*> out;for(const auto& l:two_stream?inputs:lanes)out.push_back(l.get());return out;
  }
  SourceFrameIdentity identity(const EncodedFrame& f) const {
    return {f.camera_id,f.seq,f.captured_steady_ms,true,f.source_generation,
      f.exposure_time_trusted?"v4l2_soe":((f.v4l2_timestamp_flags&V4L2_BUF_FLAG_TIMESTAMP_MASK)==V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC?"v4l2_eof":"read_completion"),f.read_started_steady_ms,f.read_finished_steady_ms};
  }
  void push_frame(Lane& lane,const EncodedFrame& frame,std::vector<SourceFrameIdentity> used,bool healthy_frame,Json stages=Json::object()) {
    GstBuffer* buffer=gst_buffer_new_allocate(nullptr,frame.payload.size(),nullptr);
    if(!buffer)throw std::runtime_error("cannot allocate composed frame");
    gst_buffer_fill(buffer,0,frame.payload.data(),frame.payload.size());
    const auto pts=GstClockTime(std::max<std::int64_t>(0,frame.captured_steady_ms-lane.pipeline_started_steady_ms))*GST_MSECOND;
    GST_BUFFER_PTS(buffer)=pts;GST_BUFFER_DTS(buffer)=GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer)=GST_SECOND/GstClockTime(lane.input.fps);
    {std::lock_guard lock(lane.frame_mutex);lane.provenance[pts]={std::move(used),healthy_frame,std::move(stages)};while(lane.provenance.size()>32)lane.provenance.erase(lane.provenance.begin());}
    const auto flow=gst_app_src_push_buffer(GST_APP_SRC(lane.appsrc),buffer);
    if(flow!=GST_FLOW_OK&&!stop_requested)throw std::runtime_error("composite appsrc rejected frame");
    ++lane.pushed;++lane.captured;lane.last_capture_ms=frame.captured_at_ms;
  }
  void start_compositors() {
    if(!two_stream)return;
    for(unsigned index=0;index<2;++index){auto* lane=lanes[index].get();lane->thread=std::thread([this,lane,index]{
      const std::vector<std::string> ids=index?std::vector<std::string>{"fish_front","fish_rear","fish_left","fish_right"}:std::vector<std::string>{"drive_front","drive_rear"};
      const auto interval=std::chrono::nanoseconds(1000000000/lane->profile.fps);auto deadline=std::chrono::steady_clock::now();
      while(!stop_requested){try{
        const auto alignment_started=steady_now_ms();
        std::array<std::shared_ptr<const EncodedFrame>,4> frames{};std::array<surround::Image,4> images{};
        std::vector<SourceFrameIdentity> used;unsigned mask=0;
        if(index){
          const auto wait_until=steady_now_ms()+(config.surround.diagnostic_partition?20:calibration->document.at("acceptance").at("timing").at("alignment_wait_ms").get<int>());
          do {
            std::array<std::vector<std::shared_ptr<const EncodedFrame>>,4> histories;
            for(unsigned i=0;i<4;++i){auto it=std::find_if(inputs.begin(),inputs.end(),[&](const auto& x){return x->camera.id==ids[i];});
              std::lock_guard lock((*it)->frame_mutex);histories[i].assign((*it)->history.begin(),(*it)->history.end());}
            std::int64_t best=0;
            for(unsigned bits=0;bits<16;++bits){std::int64_t lo=std::numeric_limits<std::int64_t>::max(),hi=0;bool valid=true;
              std::array<std::shared_ptr<const EncodedFrame>,4> group;
              for(unsigned i=0;i<4;++i){auto choice=(bits>>i)&1u;if(choice>=histories[i].size()){valid=false;break;}
                group[i]=histories[i][choice];const auto stamp=group[i]->captured_steady_ms;
                if(stamp<=0||steady_now_ms()-stamp>config.surround.max_frame_age_ms){valid=false;break;}lo=std::min(lo,stamp);hi=std::max(hi,stamp);}
              if(valid&&hi-lo<=config.surround.max_skew_ms&&lo>best){frames=group;best=lo;}}
            if(best||stop_requested||steady_now_ms()>=wait_until)break;
            std::unique_lock lock(alignment_mutex);alignment_cv.wait_for(lock,std::chrono::milliseconds(2));
          }while(true);
        }
        const auto now=steady_now_ms();
        for(unsigned i=0;i<ids.size();++i){if(!index){auto it=std::find_if(inputs.begin(),inputs.end(),[&](const auto& x){return x->camera.id==ids[i];});
            std::lock_guard lock((*it)->frame_mutex);frames[i]=(*it)->latest;}
          if(!frames[i])continue;
          const auto& f=*frames[i];
          images[i]={reinterpret_cast<const std::uint8_t*>(f.payload.data()),f.width,f.height,std::size_t(f.width)*(f.codec=="uyvy"?2:4),f.codec=="uyvy"?surround::Format::Uyvy:surround::Format::Rgba};
          if(f.captured_steady_ms>0&&now>=f.captured_steady_ms&&now-f.captured_steady_ms<=config.surround.max_frame_age_ms){mask|=1u<<i;used.push_back(identity(f));}
        }
        bool healthy_frame=used.size()==ids.size()&&health.composed(index,used,now,config.surround.max_frame_age_ms,index?config.surround.max_skew_ms:100);
        EncodedFrame result;result.camera_id=lane->camera.id;result.codec="rgba";result.width=lane->profile.width;result.height=lane->profile.height;result.captured_at_ms=signaling.now_ms();result.captured_steady_ms=now;
        result.payload.assign(std::size_t(result.width)*result.height*4,char(0));auto* rgba=reinterpret_cast<std::uint8_t*>(result.payload.data());
        if(index&&config.surround.diagnostic_partition){
          for(unsigned i=0;i<4;++i)if(mask&(1u<<i))surround::resize_into(images[i],rgba+((i/2)*std::size_t(result.height/2)*result.width+(i%2)*(result.width/2))*4,result.width/2,result.height/2,std::size_t(result.width)*4);
        }else if(index){if(!healthy_frame)mask=0;surround_renderer->render(images,mask,rgba);}
        else for(unsigned i=0;i<2;++i)if(mask&(1u<<i))surround::resize_into(images[i],rgba+std::size_t(i)*result.width*(result.height/2)*4,result.width,result.height/2,std::size_t(result.width)*4);
        push_frame(*lane,result,std::move(used),healthy_frame,{{"alignment_started_steady_ms",alignment_started},{"composition_started_steady_ms",now},{"composition_finished_steady_ms",steady_now_ms()}});
      }catch(const std::exception& error){if(!stop_requested)inhibit_control_for_critical_camera(*lane,error.what());}
      deadline+=interval;auto now=std::chrono::steady_clock::now();if(deadline<now)deadline=now;std::this_thread::sleep_until(deadline);
      }
    });}
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
    lifecycle_stopping=false;stop_requested = false;control_quiescing=false;
    offer_started=false;offer_requested=false;
    relay_path_invalid=false;relay_fault_stopped=false;
    media_attempt_id=random_token(16);
    local_ice_candidate_count = 0;
    remote_ice_candidate_count = 0;
    answer_received_at_ms.reset();
    control_not_open_warning_fired = false;
    control_link_opened_this_attempt = false;
    if(!critical_camera_control_latch->rebuild_allowed()){
      set_pipeline_error("vehicle parking acknowledgement required for media rebuild","media_quiesce_required","media_rebuild","Request vehicle-confirmed parking/disarm before retrying media.",false);return false;
    }
    media_rebuild_requested=false;
    prepare_lanes();
    relay_lease=Json::object();relay_confirmed=false;relay_activated=false;relay_released=false;
    relay_deadline=0;relay_terminal=false;selected_transport=0;direct_since=0;driver_direct_health_until=0;last_relay_renew=0;media_admission_ready=true;
    try{relay_lease=signaling.relay("request",{{"media_attempt_id",media_attempt_id},{"profile",two_stream&&!config.surround.diagnostic_partition?(config.surround.profile=="540p"?"two-540p":"two-720p"):"full"}});
      if(relay_lease.value("approved",false)){media_admission_ready=false;relay_deadline=steady_now_ms()+15000;}
      else emit_diagnostic("vehicle_relay_denied","relay_admission_denied","relay_admission",relay_lease.value("reason",""),"Direct ICE remains available; relay needs capacity and a qualified profile.",false);
    }catch(const std::exception& error){emit_diagnostic("vehicle_relay_unavailable","relay_admission_unavailable","relay_admission",error.what(),"Direct ICE remains available.",true);}
    ice_configuration=signaling.ice_servers();
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
        gst_pad_add_probe(encoder_src,static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER|GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM), count_encoded, lane.get(), nullptr);
        gst_object_unref(encoder_src);
      }
      if(config.runtime.media_frame_trace){
        auto* pay=gst_bin_get_by_name(GST_BIN(pipeline),("pay_"+pipeline_identifier(lane->camera.id)).c_str());
        auto* src=pay?gst_element_get_static_pad(pay,"src"):nullptr;
        if(src){gst_pad_add_probe(src,static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER|GST_PAD_PROBE_TYPE_BUFFER_LIST),trace_rtp,lane.get(),nullptr);gst_object_unref(src);}
        if(pay)gst_object_unref(pay);
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
    for (auto* lane : capture_lanes()) {
      lane->pipeline_started_ms = started_ms;
      lane->pipeline_started_steady_ms = steady_now_ms();
      lane->thread = std::thread([this, lane, capture_interval_ms] {
        std::uint64_t sequence = 0,wrap=0;std::uint32_t previous_source_sequence=0;bool sequence_seen=false;
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
            if (!lane->source) {lane->source = create_camera_source(*lane);++lane->source_generation;wrap=0;sequence_seen=false;}
            if (pace_test_source) {
              const auto current = std::chrono::steady_clock::now();
              if (current < next_test_source_frame) {
                std::this_thread::sleep_until(next_test_source_frame);
              } else if (current - next_test_source_frame > test_source_interval) {
                next_test_source_frame = current;
              }
            }
            auto frame = lane->source->next(++sequence);
            frame.source_generation=lane->source_generation;
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
                   {"v4l2_timestamp_us",frame.v4l2_timestamp_us},{"v4l2_timestamp_flags",frame.v4l2_timestamp_flags},
                   {"exposure_time_trusted",frame.exposure_time_trusted},{"read_started_steady_ms",frame.read_started_steady_ms},{"read_finished_steady_ms",frame.read_finished_steady_ms},
                   {"source_timeperframe_numerator", frame.source_timeperframe_numerator},
                   {"source_timeperframe_denominator", frame.source_timeperframe_denominator},
                   {"sequence", sequence}});
            }
            auto source_identity=identity(frame);
            if(frame.source_sequence_valid){if(sequence_seen&&frame.source_sequence<previous_source_sequence&&std::uint32_t(frame.source_sequence-previous_source_sequence)<=0x7fffffffU)wrap+=std::uint64_t{1}<<32;sequence_seen=true;previous_source_sequence=frame.source_sequence;source_identity.sequence=wrap+frame.source_sequence;frame.seq=source_identity.sequence;}
            bool timing_valid=true;
            if(two_stream&&!config.surround.diagnostic_partition&&lane->camera.id.starts_with("fish_")){
              const auto basis=calibration->document.at("acceptance").at("timing").at("age_basis").get<std::string>();
              timing_valid=source_identity.time_quality==basis;
            }
            const bool raw_valid=timing_valid&&health.captured(source_identity,steady_now_ms());
            if(!two_stream&&!raw_valid){++lane->dropped;continue;}
            if(two_stream){
              if(!raw_valid)frame.captured_steady_ms=0;
              if(frame.codec!="uyvy"){frame.payload=decode_frame_rgba(frame);frame.codec="rgba";}
              {std::lock_guard lock(lane->frame_mutex);lane->latest=std::make_shared<EncodedFrame>(std::move(frame));
                if(!raw_valid)lane->history.clear();
                else {lane->history.push_back(lane->latest);while(lane->history.size()>2)lane->history.pop_front();}}
              alignment_cv.notify_all();
              if(pace_test_source)next_test_source_frame+=test_source_interval;
              if(capture_interval_ms>0)std::this_thread::sleep_for(std::chrono::milliseconds(capture_interval_ms));
              continue;
            }
            GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.payload.size(), nullptr);
            if (buffer == nullptr) throw std::runtime_error("cannot allocate GStreamer camera buffer");
            gst_buffer_fill(buffer, 0, frame.payload.data(), frame.payload.size());
            const auto elapsed_ms = std::max<std::int64_t>(0, frame.captured_steady_ms - lane->pipeline_started_steady_ms);
            GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(elapsed_ms) * GST_MSECOND;
            {std::lock_guard lock(lane->frame_mutex);lane->provenance[GST_BUFFER_PTS(buffer)]={{source_identity},raw_valid};while(lane->provenance.size()>32)lane->provenance.erase(lane->provenance.begin());}
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
    start_compositors();
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
    lifecycle_stopping=true;stop_requested = true;
    if(webrtc)g_signal_handlers_disconnect_by_data(webrtc,this);
    relay_deadline=0;
    if(transport_stats.promise){gst_promise_interrupt(transport_stats.promise);gst_promise_unref(transport_stats.promise);transport_stats={};}
    if(pipeline)critical_camera_control_latch->revoke_input();
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
    if(relay_lease.value("approved",false)&&!relay_released){try{signaling.relay("release",{{"media_attempt_id",media_attempt_id}});}catch(...){}relay_released=true;}
    if (channel_to_close != nullptr) {
      g_signal_handlers_disconnect_by_data(channel_to_close, this);
      gst_webrtc_data_channel_close(channel_to_close);
      g_object_unref(channel_to_close);
    }
    // Let capture threads observe stop_requested and finish before EOS.  A
    // source may return its last frame while teardown is in progress; joining
    // first prevents a push-after-EOS race on appsrc.
    for(auto* lane:capture_lanes()){if(lane->thread.joinable())lane->thread.join();lane->source.reset();}
    if(two_stream)for(const auto& lane:lanes)if(lane->thread.joinable())lane->thread.join();
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
#if GST_CHECK_VERSION(1,28,0)
    if(auto* ice=tracked_ice.exchange(nullptr)){g_signal_handlers_disconnect_by_data(ice,this);gst_object_unref(ice);}
#endif
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
    {std::lock_guard lock(description_mutex);for(auto* p:{&offer_description,&local_description,&remote_description})if(p->promise){gst_promise_interrupt(p->promise);gst_promise_unref(p->promise);*p={};}pending_remote_ice.clear();}
    if (pipeline != nullptr) {
      gst_object_unref(pipeline);
      pipeline = nullptr;
    }
    {std::lock_guard lock(signal_mutex);pending_signals.clear();}
  }

  void flush_outgoing_signals() {
    std::deque<std::pair<std::string, Json>> values;
    {
      std::lock_guard lock(signal_mutex);
      values.swap(pending_signals);
    }
    for (const auto& [type, payload] : values) signaling.send(type, payload);
  }

  struct DescriptionCompletion {std::atomic<bool> complete{false};bool success{false};};
  struct PendingDescription {GstPromise* promise{nullptr};std::shared_ptr<DescriptionCompletion> completion;Json message;};
  static void destroy_description_completion(gpointer p){delete static_cast<std::shared_ptr<DescriptionCompletion>*>(p);}
  static void description_completed(GstPromise* promise,gpointer p){
    const auto state=*static_cast<std::shared_ptr<DescriptionCompletion>*>(p);
    if(gst_promise_wait(promise)==GST_PROMISE_RESULT_REPLIED){const auto* reply=gst_promise_get_reply(promise);state->success=!reply||!gst_structure_has_field(reply,"error");}
    state->complete.store(true,std::memory_order_release);
  }
  void finish_descriptions(){
    // GStreamer callbacks only complete promises. The media thread owns all
    // SDP application and publication, so retired callbacks cannot mutate it.
    if(!offer_started&&offer_requested&&media_admission_ready&&webrtc){
      bool caps_ready=true;
      for(const auto& lane:lanes){
        if(!lane->encoded.load()){caps_ready=false;break;}
        if(active_candidate.codec==VideoCodec::H264){
          auto* pay=gst_bin_get_by_name(GST_BIN(pipeline),("pay_"+pipeline_identifier(lane->camera.id)).c_str());
          auto* pad=pay?gst_element_get_static_pad(pay,"src"):nullptr;auto* caps=pad?gst_pad_get_current_caps(pad):nullptr;
          const auto* sprop=caps?gst_structure_get_string(gst_caps_get_structure(caps,0),"sprop-parameter-sets"):nullptr;
          const auto profile=h264_profile_level_id(sprop?sprop:"");
          if(profile.empty())caps_ready=false;
          else {
            lane->actual_h264_level=int(std::strtoul(profile.c_str(),nullptr,16)&255);
            if(lane->actual_h264_level<minimum_h264_level_idc(lane->profile.width,lane->profile.height,lane->profile.fps)){
              caps_ready=false;set_pipeline_error("actual RTP H.264 SPS level is insufficient","h264_encoder_level_insufficient","encoder_qualification","Use a qualified encoder or parked full preview.",false);
            }else{
              auto* filter=gst_bin_get_by_name(GST_BIN(pipeline),("rtp_caps_"+pipeline_identifier(lane->camera.id)).c_str());
              auto* out=filter?gst_element_get_static_pad(filter,"src"):nullptr;auto* current=out?gst_pad_get_current_caps(out):nullptr;
              const auto* advertised=current?gst_structure_get_string(gst_caps_get_structure(current,0),"profile-level-id"):nullptr;
              if(!filter||!advertised||profile!=advertised){
                caps_ready=false;auto* wanted=gst_caps_copy(caps);gst_caps_set_simple(wanted,"profile-level-id",G_TYPE_STRING,profile.c_str(),nullptr);if(filter)g_object_set(filter,"caps",wanted,nullptr);gst_caps_unref(wanted);
              }
              if(current)gst_caps_unref(current);
              if(out)gst_object_unref(out);
              if(filter)gst_object_unref(filter);
            }
          }
          if(caps)gst_caps_unref(caps);
          if(pad)gst_object_unref(pad);
          if(pay)gst_object_unref(pay);
        }
      }
      if(caps_ready&&active_candidate.codec==VideoCodec::H264){
        GArray* transceivers=nullptr;g_signal_emit_by_name(webrtc,"get-transceivers",&transceivers);
        caps_ready=transceivers&&transceivers->len==lanes.size();
        if(caps_ready)for(unsigned i=0;i<transceivers->len;++i){
          auto* filter=gst_bin_get_by_name(GST_BIN(pipeline),("rtp_caps_"+pipeline_identifier(lanes[i]->camera.id)).c_str());
          auto* out=filter?gst_element_get_static_pad(filter,"src"):nullptr;
          auto* preferences=out?gst_pad_get_current_caps(out):nullptr;
          if(preferences)g_object_set(g_array_index(transceivers,GstWebRTCRTPTransceiver*,i),"codec-preferences",preferences,nullptr);
          else caps_ready=false;
          if(preferences)gst_caps_unref(preferences);
          if(out)gst_object_unref(out);
          if(filter)gst_object_unref(filter);
        }
        if(transceivers)g_array_unref(transceivers);
      }
      if(caps_ready){offer_started=true;offer_requested=false;
        auto completion=std::make_shared<DescriptionCompletion>();auto* promise=gst_promise_new_with_change_func(description_completed,new std::shared_ptr<DescriptionCompletion>(completion),destroy_description_completion);
        offer_description={promise,completion,Json{{"attempt",media_attempt_id}}};g_signal_emit_by_name(webrtc,"create-offer",nullptr,promise);
      }
    }
    if(offer_description.promise&&offer_description.completion->complete.load(std::memory_order_acquire)){
      auto pending=std::move(offer_description);offer_description={};
      OfferContext context{this,webrtc,pending.message.at("attempt")};on_offer_created(pending.promise,&context);
    }

    std::lock_guard lock(description_mutex);
    for(auto* pending:{&local_description,&remote_description}){
      if(!pending->promise||!pending->completion->complete.load(std::memory_order_acquire))continue;
      const bool remote=pending==&remote_description;
      if(!pending->completion->success)set_pipeline_error("SDP description rejected","webrtc_sdp_set_failed","sdp","Inspect SDP and codec negotiation logs.",true);
      else if(remote){answer_received=true;answer_received_at_ms=signaling.now_ms();
        for(const auto& ice:pending_remote_ice)g_signal_emit_by_name(webrtc,"add-ice-candidate",ice.value("sdpMLineIndex",0U),ice.value("end_of_candidates",false)?nullptr:ice.value("candidate","").c_str());
        remote_ice_candidate_count+=pending_remote_ice.size();pending_remote_ice.clear();
        emit_diagnostic("vehicle_webrtc_answer_applied","webrtc_answer_applied","sdp","","Remote SDP completed; queued candidates applied.",true);
      }else queue_signal(pending->message.at("type"),pending->message.at("payload"));
      gst_promise_unref(pending->promise);*pending={};
    }
  }
  void update_relay(){
    if(!relay_lease.value("approved",false)||relay_released)return;
    if(relay_terminal){signaling.relay("release",{{"media_attempt_id",media_attempt_id}});relay_released=true;return;}
    const auto now=steady_now_ms();
    if(now-last_relay_renew>=5000){
      try{const auto state=signaling.relay("renew",{{"lease_id",relay_lease.at("lease_id")},{"media_attempt_id",media_attempt_id}});
        if(relay_terminal.load()||steady_now_ms()>=relay_deadline.load())return;
        relay_deadline=now+15000;last_relay_renew=now;
        if(state.value("usage",Json::object()).value("over_budget",false)){
          // Both initial profiles already use their qualified bitrate floor.
          // Resolution/profile changes require parking rather than an unsafe
          // unqualified downshift while driving.
          stop_control_for_pipeline_fault("relay_budget_exceeded");
          set_pipeline_error("relay sustained egress exceeds the accepted profile budget","relay_budget_exceeded","relay_budget","Park before selecting a different accepted profile.",false);return;
        }
      }catch(const std::exception& error){emit_diagnostic("vehicle_relay_renew_failed","relay_renew_failed","relay_lease",error.what(),"Local lease deadline remains authoritative.",true);last_relay_renew=now;}
    }
    if(!relay_confirmed&&critical_cameras_ready()){
      double applied=0;
      for(const auto& lane:lanes){
        auto* spec=g_object_class_find_property(G_OBJECT_GET_CLASS(lane->encoder),"bitrate");
        if(!spec)throw std::runtime_error("encoder has no readable bitrate policy");
        GValue value=G_VALUE_INIT,number=G_VALUE_INIT;g_value_init(&value,spec->value_type);g_value_init(&number,G_TYPE_DOUBLE);
        g_object_get_property(G_OBJECT(lane->encoder),"bitrate",&value);
        const bool ok=g_value_transform(&value,&number);if(ok)applied+=g_value_get_double(&number)*1000;
        g_value_unset(&number);g_value_unset(&value);if(!ok)throw std::runtime_error("encoder bitrate policy cannot be verified");
      }
      if(applied!=relay_lease.at("video_bps").get<double>())throw std::runtime_error("applied encoder policy differs from relay lease");
      signaling.relay("confirm",{{"lease_id",relay_lease.at("lease_id")},{"media_attempt_id",media_attempt_id},{"policy_version",relay_lease.at("policy_version")},{"applied_video_bps",applied},{"healthy_encoded_frames",true}});
      relay_confirmed=true;
    }
    if(relay_confirmed&&!relay_activated){
      const auto ice=signaling.ice_servers(media_attempt_id);
      if(ice.value("relay_lease_id","")!=relay_lease.value("lease_id",""))return;
      ice_configuration=ice;configure_ice_servers();relay_activated=true;media_admission_ready=true;
      for(const auto& lane:lanes){auto* valve=gst_bin_get_by_name(GST_BIN(pipeline),("gate_"+pipeline_identifier(lane->camera.id)).c_str());if(valve){g_object_set(valve,"drop",FALSE,nullptr);gst_object_unref(valve);}}
      on_negotiation_needed(webrtc,this);
    }
    if(selected_transport.load()==1&&driver_direct_health_until>=now&&direct_since>0&&now-direct_since>=10000){signaling.relay("release",{{"media_attempt_id",media_attempt_id}});relay_released=true;relay_deadline=0;
      emit_diagnostic("vehicle_relay_reclaim_started","relay_reclaim_started","relay_lease","","Direct media and control healthy for ten seconds; quota awaits allocation reclamation.",true);}
  }
  void check_local_relay_deadline(){
    if(relay_path_invalid.load()&&!relay_fault_stopped.exchange(true)){
      stop_control_for_pipeline_fault("relay_without_lease");
      set_pipeline_error("relay selection has no current local qualification","relay_without_lease","relay_lease","Park and obtain a new media attempt before restoring relay.",false);
    }
    const auto deadline=relay_deadline.load();
    if(deadline>0&&steady_now_ms()>=deadline&&!relay_terminal.exchange(true)){relay_deadline=0;
      if(selected_transport.load()!=1){stop_control_for_pipeline_fault("relay_lease_expired");set_pipeline_error("local relay lease expired","relay_lease_expired","relay_lease","Restore admission and confirm vehicle parking before retrying.",false);}
      // A proven direct transport keeps its authority; returning to relay
      // after this terminal revocation requires parked admission.
    }
  }
  static const GstStructure* stats_object(const GstStructure* root,const char* id){
    if(!id)return nullptr;
    const auto* v=gst_structure_get_value(root,id);
    return v&&GST_VALUE_HOLDS_STRUCTURE(v)?gst_value_get_structure(v):nullptr;
  }
#if GST_CHECK_VERSION(1,28,0)
  static void selected_pair_changed(GstWebRTCICETransport* ice,gpointer data){
    auto* self=static_cast<Impl*>(data);if(self->stop_requested||ice!=self->tracked_ice)return;
    GstWebRTCICEConnectionState state;g_object_get(ice,"state",&state,nullptr);
    auto* pair=gst_webrtc_ice_transport_get_selected_candidate_pair(ice);
    int path=0;
    if(pair&&pair->local&&pair->remote&&pair->local->stats&&pair->remote->stats&&
        (state==GST_WEBRTC_ICE_CONNECTION_STATE_CONNECTED||state==GST_WEBRTC_ICE_CONNECTION_STATE_COMPLETED)){
      const auto* a=pair->local->stats->type;const auto* b=pair->remote->stats->type;
      if(a&&b)path=(std::string_view(a)=="relay"||std::string_view(b)=="relay")?2:1;
    }
    if(pair)gst_webrtc_ice_candidate_pair_free(pair);
    self->selected_transport=path;
    if(path==2&&(!self->relay_activated.load()||self->relay_released.load()||self->relay_terminal.load()||
         steady_now_ms()>=self->relay_deadline.load()))self->relay_path_invalid=true;
  }
  static void transport_state_changed(GstWebRTCICETransport* ice,GParamSpec*,gpointer data){selected_pair_changed(ice,data);}
  void bind_transport_notifications(){
    if(tracked_ice)return;
    GArray* transceivers=nullptr;g_signal_emit_by_name(webrtc,"get-transceivers",&transceivers);
    if(!transceivers)return;
    for(unsigned i=0;i<transceivers->len&&!tracked_ice;++i){
      auto* trans=g_array_index(transceivers,GstWebRTCRTPTransceiver*,i);
      GstWebRTCRTPSender* sender=nullptr;GstWebRTCDTLSTransport* dtls=nullptr;GstWebRTCICETransport* ice=nullptr;
      g_object_get(trans,"sender",&sender,nullptr);if(sender)g_object_get(sender,"transport",&dtls,nullptr);
      if(dtls)g_object_get(dtls,"transport",&ice,nullptr);
      if(sender)gst_object_unref(sender);
      if(dtls)gst_object_unref(dtls);
      if(ice){tracked_ice=ice;g_signal_connect(ice,"on-selected-candidate-pair-change",G_CALLBACK(selected_pair_changed),this);
        g_signal_connect(ice,"notify::state",G_CALLBACK(transport_state_changed),this);selected_pair_changed(ice,this);}
    }
    g_array_unref(transceivers);
  }
#endif
  void sample_transport(){
    if(!webrtc||!answer_received)return;
#if GST_CHECK_VERSION(1,28,0)
    bind_transport_notifications();
#endif
    const auto now=steady_now_ms();
    if(selected_transport.load()!=1||driver_direct_health_until<now)direct_since=0;
    if(transport_stats.promise&&transport_stats.completion->complete.load(std::memory_order_acquire)){
      bool selected=false,relay=false;const auto* reply=gst_promise_get_reply(transport_stats.promise);
      if(reply)for(int i=0;i<gst_structure_n_fields(reply);++i){const auto* transport=stats_object(reply,gst_structure_nth_field_name(reply,i));if(!transport)continue;
        const auto* pair=stats_object(reply,gst_structure_get_string(transport,"selected-candidate-pair-id"));if(!pair)continue;
        const auto* local=stats_object(reply,gst_structure_get_string(pair,"local-candidate-id"));const auto* remote=stats_object(reply,gst_structure_get_string(pair,"remote-candidate-id"));
        if(!local||!remote)continue;
        const auto* lt=gst_structure_get_string(local,"candidate-type");const auto* rt=gst_structure_get_string(remote,"candidate-type");if(!lt||!rt)continue;
        selected=true;relay=std::string_view(lt)=="relay"||std::string_view(rt)=="relay";
        const auto path=std::string(lt)+"/"+rt;if(path!=selected_path){selected_path=path;emit_diagnostic("vehicle_ice_selected_pair","ice_selected_pair","ice_connectivity","","Actual transport selected candidate pair changed.",true,{{"local_type",lt},{"remote_type",rt},{"relay",relay}});}break;
      }
#if !GST_CHECK_VERSION(1,28,0)
      selected_transport=selected?(relay?2:1):0;
#endif
      if(selected&&!relay&&control_link_open&&critical_cameras_ready()&&driver_direct_health_until>=now){if(!direct_since)direct_since=now;}else direct_since=0;
      if(selected&&relay&&(!relay_activated||relay_released||relay_terminal.load())){stop_control_for_pipeline_fault("relay_without_lease");set_pipeline_error("selected relay has no current lease","relay_without_lease","relay_lease","Request a parked media restart.",false);}
      gst_promise_unref(transport_stats.promise);transport_stats={};
    }
    if(!transport_stats.promise&&now-last_transport_stats>=1000){auto completion=std::make_shared<DescriptionCompletion>();auto* promise=gst_promise_new_with_change_func(description_completed,new std::shared_ptr<DescriptionCompletion>(completion),destroy_description_completion);transport_stats={promise,completion,Json{}};last_transport_stats=now;g_signal_emit_by_name(webrtc,"get-stats",nullptr,promise);}
  }
  void process_signaling() {
    finish_descriptions();
    sample_transport();
    update_relay();
    const auto response = signaling.poll("webrtc_answer,ice_candidate,media_fallback,media_path_health");
    for (const auto& message : response.value("messages", Json::array())) {
      const auto type = message.value("type", "");
      const auto payload = message.value("payload", Json::object());
      if(payload.value("media_attempt_id","")!=media_attempt_id)continue;
      if(type=="media_path_health"){
        bool all=payload.value("direct_selected",false)&&payload.value("control_data_channel_open",false)&&payload.value("control_epoch",std::uint64_t{0})==critical_camera_control_latch->control_epoch();
        const auto reported=payload.value("healthy_stream_ids",Json::array());std::set<std::string> healthy;
        if(!reported.is_array())all=false;
        else for(const auto& id:reported){if(!id.is_string()){all=false;break;}healthy.insert(id.get<std::string>());}
        for(const auto& lane:lanes)if(!healthy.contains(lane->camera.id))all=false;
        driver_direct_health_until=all?steady_now_ms()+2500:0;if(!all)direct_since=0;
        continue;
      }
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
        if(active_candidate.codec==VideoCodec::H264){
          std::vector<MediaProfile> profiles;std::vector<int> levels;for(const auto& lane:lanes){profiles.push_back(lane->profile);levels.push_back(lane->actual_h264_level);}
          if(!h264_answer_supports(sdp_text,profiles,levels)){
            gst_sdp_message_free(sdp);
            set_pipeline_error("controller SDP cannot receive the encoded H.264 canvas","h264_receive_level_insufficient","sdp","Use a qualified decoder/encoder pair or switch to full preview after parking.",false);return;
          }
        }

        auto* answer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_ANSWER, sdp);
        auto completion=std::make_shared<DescriptionCompletion>();
        GstPromise* promise=gst_promise_new_with_change_func(description_completed,
          new std::shared_ptr<DescriptionCompletion>(completion),destroy_description_completion);
        {std::lock_guard lock(description_mutex);
          if(remote_description.promise){gst_promise_unref(promise);gst_webrtc_session_description_free(answer);continue;}
          remote_description={promise,completion,Json{}};
        }
        g_signal_emit_by_name(webrtc,"set-remote-description",answer,promise);
        gst_webrtc_session_description_free(answer);
        emit_diagnostic(
            "vehicle_webrtc_answer_setting",
            "webrtc_answer_setting",
            "webrtc_answer",
            "",
            "No action is required; wait for ICE/DTLS connection and video frames.",
            true,
            {{"codec", to_string(active_candidate.codec)},
             {"backend", to_string(active_candidate.backend)}});
      } else if (type == "ice_candidate") {
        const auto candidate = payload.value("candidate", "");
        const auto index = payload.value("sdpMLineIndex", 0U);
        if (!candidate.empty()||payload.value("end_of_candidates",false)) {
          if(!answer_received){if(pending_remote_ice.size()<512)pending_remote_ice.push_back(payload);continue;}
          g_signal_emit_by_name(webrtc, "add-ice-candidate", index, candidate.empty()?nullptr:candidate.c_str());
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
      bool pipeline_started=false;
      try{pipeline_started=start_pipeline(candidate,capture_interval_ms);}catch(const std::exception& e){set_pipeline_error(e.what(),"media_profile_unsupported","media_profile","Keep parked; use full-mode preview or repeat encoder/calibration qualification.",false);}
      if (!pipeline_started) {
        const auto error = current_pipeline_error();
        attempts.push_back({
            {"backend", to_string(candidate.backend)},
            {"codec", to_string(candidate.codec)},
            {"passed", false},
            {"failure", current_pipeline_failure()},
            {"error", error}});
        errors.push_back(error);
        const bool preview_fallback=two_stream&&!config.surround.diagnostic_partition&&config.surround.mode=="auto"&&critical_camera_control_latch->rebuild_allowed();
        stop_pipeline();
        if(preview_fallback){config.surround.mode="full";emit_diagnostic("vehicle_media_profile_fallback","two_stream_unsupported","media_profile",error,"Full-mode preview selected; repeat two-stream qualification.",false);--candidate_index;continue;}
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
        } catch (const HttpTransportError& error) {
          if(!continuous)throw;
          set_pipeline_error(error.what(),"session_signaling_transport_failed","session_signaling",
              "Restore WSS and request vehicle-confirmed parking before rebuilding media.",true);
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
        check_quiesce();
        if(media_rebuild_requested)break;
        if(!current_pipeline_error().empty()){
          if(!continuous||critical_camera_control_latch->rebuild_allowed())break;
          queue_signal("media_status",{{"control_epoch",critical_camera_control_latch->control_epoch()},{"control_issue_code","media_quiesce_required"},{"failure",current_pipeline_failure()}});
          try{flush_outgoing_signals();}catch(const HttpTransportError&){}
          std::this_thread::sleep_for(std::chrono::milliseconds(200));continue;
        }
        enforce_critical_camera_freshness();
        start_control_when_cameras_ready();
        // The independent native-control watchdog thread owns the VCU tick;
        // media HTTP latency must not schedule control safety progression.
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
               {"control_issue_code", control_inhibited.load() ? "critical_camera_failed" : ""},
               {"time_sync", signaling.time_sync_status().to_json()},
               {"control_epoch",critical_camera_control_latch->control_epoch()},{"source_health",health.snapshot(steady_now_ms())},
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
        check_quiesce();
        if(media_rebuild_requested)break;
        if (!current_pipeline_error().empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      flush_outgoing_signals();
      if(media_rebuild_requested){stop_pipeline();--candidate_index;continue;}
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
      if (encoded_fps < (lane.value("camera_id","")=="surround_bev"?20:config.hardware.min_realtime_fps)) {
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

  int maintenance_lock_fd{-1};
  VehicleConfig config;
  MediaSignalingClient signaling;
  std::shared_ptr<CriticalCameraControlLatch> critical_camera_control_latch;
  int frame_timeout_ms;
  std::optional<std::string> forced_codec;
  int simulate_primary_failure_after_frames;
  GstElement* pipeline{nullptr};
  GstElement* webrtc{nullptr};
  GstWebRTCDataChannel* control_channel{nullptr};
  std::vector<std::unique_ptr<Lane>> lanes;
  std::vector<std::unique_ptr<Lane>> inputs;
  MediaHealth health;
  bool two_stream{false};
  std::optional<SurroundCalibration> calibration;
  std::unique_ptr<surround::Renderer> surround_renderer;
  std::atomic<bool> stop_requested{false};
  // Media faults fence driving; parking WSS/feedback survive until teardown.
  std::atomic<bool> lifecycle_stopping{false};
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
  std::string media_attempt_id;
  std::mutex description_mutex;
  PendingDescription offer_description,local_description,remote_description,transport_stats;
  Json relay_lease=Json::object();
  std::atomic<bool> media_admission_ready{true};
  bool relay_confirmed{};
  std::atomic<bool> relay_activated{false},relay_released{false};
  std::atomic<bool> relay_path_invalid{false},relay_fault_stopped{false};
#if GST_CHECK_VERSION(1,28,0)
  std::atomic<GstWebRTCICETransport*> tracked_ice{};
#endif
  std::atomic<std::int64_t> relay_deadline{0};
  std::atomic<bool> relay_terminal{false};
  std::atomic<int> selected_transport{0};
  std::int64_t last_relay_renew{},direct_since{},last_transport_stats{},driver_direct_health_until{};
  std::string selected_path;
  std::vector<Json> pending_remote_ice;
  bool answer_received{false};
  std::optional<std::int64_t> answer_received_at_ms;
  bool control_not_open_warning_fired{false};
  bool simulated_failure_fired{false};
  bool codec_fallback_requested{false};
  std::uint64_t failover_count{0};
  std::string last_negotiation_warning;
  Json ice_configuration = Json::object();
  std::mutex control_trace_mutex;
  std::condition_variable control_trace_cv;
  std::deque<Json> control_trace_queue;
  std::thread control_trace_worker;
  bool control_trace_stop_requested{false};
  std::atomic<bool> control_trace_accepting{false};
  std::atomic<std::uint64_t> control_trace_enqueued_total{0};
  std::atomic<std::uint64_t> control_trace_dropped_total{0};
  std::mutex control_mutex;
  std::unique_ptr<VehicleControlService> control_service;
  // Guarded by control_mutex. Stale-safe heartbeats may only preserve this
  // accepted fresh gear; ESTOP freezes its gear for the pipeline lifetime.
  std::optional<std::string> native_control_last_accepted_fresh_gear;
  std::optional<std::string> native_control_estop_frozen_gear;
  std::jthread native_control_watchdog_thread;
  std::jthread native_control_websocket_thread;
  bool control_service_started{false};
  std::string control_service_issue_code;
  std::atomic<bool> control_link_open{false};
  std::atomic<bool> control_inhibited{false};
  Json quiesce_request;
  std::atomic<bool> offer_started{false};
  std::atomic<bool> offer_requested{false};
  std::int64_t quiesce_deadline{};
  std::atomic<bool> media_rebuild_requested{false};
  std::atomic<bool> control_quiescing{false};
  std::mutex alignment_mutex;
  std::condition_variable alignment_cv;
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
