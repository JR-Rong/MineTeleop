#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace mine_teleop {

using Json = nlohmann::json;

std::int64_t now_ms();

struct ControlCommand {
  int protocol_version{1};
  std::string vehicle_id;
  std::string session_id;
  std::uint64_t seq{0};
  std::int64_t ts_ms{0};
  std::string gear{"N"};
  double steering{0.0};
  double throttle{0.0};
  double brake{0.0};
  bool estop{false};
  std::string authority_token;

  void validate() const;
  [[nodiscard]] Json to_json() const;
  static ControlCommand from_json(const Json& value);
};

struct ReceiveResult {
  bool accepted{false};
  std::string reason;
  std::optional<ControlCommand> command;
  std::vector<std::string> warnings;
};

class LatestControlCommandMailbox {
 public:
  void publish(ControlCommand command);
  [[nodiscard]] std::optional<ControlCommand> pop_latest();
  [[nodiscard]] std::size_t pending_count() const;
  [[nodiscard]] std::uint64_t dropped_count() const;

 private:
  mutable std::mutex mutex_;
  std::optional<ControlCommand> latest_;
  std::uint64_t dropped_count_{0};
};

class ControlReceiver {
 public:
  ControlReceiver(
      std::string vehicle_id,
      std::string session_id,
      int max_command_gap_ms,
      int protocol_version = 1,
      bool control_authority = true,
      std::string control_token = {},
      int timestamp_warning_skew_ms = 5000);

  ReceiveResult accept(const ControlCommand& command, std::int64_t receive_time_ms);

 private:
  std::string vehicle_id_;
  std::string session_id_;
  int max_command_gap_ms_;
  int protocol_version_;
  bool control_authority_;
  std::string control_token_;
  int timestamp_warning_skew_ms_;
  std::optional<std::uint64_t> last_seq_;
  std::optional<std::int64_t> last_valid_receive_ms_;
};

enum class SafetyState {
  Init,
  Standby,
  ControlActive,
  Degraded,
  TimeoutBrake,
  Estop,
  Fault,
};

std::string_view to_string(SafetyState state);

struct DecelerationStage {
  int after_ms{0};
  double brake{0.0};
};

struct ControlOutput {
  std::string gear{"N"};
  double steering{0.0};
  double throttle{0.0};
  double brake{0.0};
  bool estop{false};
};

class SafetyStateMachine {
 public:
  SafetyStateMachine(int degraded_timeout_ms, int control_timeout_ms, std::vector<DecelerationStage> profile);

  void mark_ready(std::int64_t now_ms);
  void on_valid_command(const ControlCommand& command, std::int64_t now_ms);
  void tick(std::int64_t now_ms);
  [[nodiscard]] ControlOutput current_output(std::int64_t now_ms) const;
  bool reset_estop(bool local_confirmed, std::string_view authorized_by, std::int64_t now_ms);
  void mark_fault();

  [[nodiscard]] SafetyState state() const { return state_; }
  [[nodiscard]] std::optional<std::int64_t> last_valid_receive_ms() const { return last_valid_receive_ms_; }

 private:
  [[nodiscard]] double brake_for_timeout(std::int64_t now_ms) const;

  int degraded_timeout_ms_;
  int control_timeout_ms_;
  std::vector<DecelerationStage> profile_;
  SafetyState state_{SafetyState::Init};
  std::optional<ControlCommand> last_valid_command_;
  std::optional<std::int64_t> last_valid_receive_ms_;
  std::optional<std::int64_t> timeout_entered_ms_;
};

struct ControlConfig {
  int rate_hz{20};
  int max_command_gap_ms{200};
  int degraded_timeout_ms{300};
  int control_timeout_ms{800};
  std::vector<DecelerationStage> deceleration_profile;
};

struct CloudConfig {
  std::string signaling_url;
  std::string auth_url;
  std::filesystem::path device_token_file;
};

struct VehicleRuntimeConfig {
  bool control_enabled{true};
  bool media_enabled{true};
  bool control_log_commands{false};
  int teleop_poll_interval_ms{50};
  int media_frame_timeout_ms{3000};
  int media_capture_interval_ms{0};
};

struct MediaProfile {
  std::string name;
  std::string codec{"h264"};
  std::string encoder{"x264"};
  int width{1280};
  int height{720};
  int fps{30};
  int bitrate_kbps{3000};
  int segment_seconds{60};
};

struct CameraConfig {
  std::string id;
  bool enabled{true};
  std::string device;
  int capture_width{1280};
  int capture_height{720};
  int capture_fps{30};
  std::string realtime_profile;
  std::string record_profile;
};

struct RecordingConfig {
  bool enabled{false};
  std::filesystem::path root_dir{".local/recordings"};
  double min_free_gb{5.0};
  double delete_uploaded_when_below_free_gb{2.0};
  bool delete_unuploaded_when_below_free_gb{false};
};

struct UploadConfig {
  bool enabled{false};
  std::string backend{"local_archive"};
  double max_bandwidth_mbps{5.0};
  int trigger_segments{20};
  bool trigger_network_idle{true};
  int retry_initial_seconds{10};
  int retry_max_seconds{600};
};

struct VehicleAdapterConfig {
  std::string type{"mock"};
  std::string can_interface{"can0"};
  std::filesystem::path bridge_library_path;
};

struct HardwareConfig {
  std::string can_interface{"can0"};
  int can_bitrate{500000};
  std::filesystem::path vaapi_render_device{"/dev/dri/renderD128"};
  std::filesystem::path dri_card_device{"/dev/dri/card1"};
  std::string preferred_encoder{"nvenc"};
  std::string fallback_encoder{"vaapi"};
  std::string preferred_codec{"h265"};
  std::string fallback_codec{"h264"};
  bool require_hardware_encoder{true};
  int max_end_to_end_latency_ms{200};
  int min_realtime_fps{20};
  std::string network_interface{"wwan0"};
};

struct FieldSafetyConfig {
  std::string commissioning_mode{"bench"};
  double max_speed_kph{40.0};
  bool require_can_feedback_before_control{true};
  bool require_local_estop_reset{true};
  bool require_time_sync{true};
  int max_time_sync_uncertainty_ms{25};
  int time_sync_interval_ms{30000};
  int time_sync_samples{7};
};

struct VehicleConfig {
  std::string vehicle_id;
  std::string vehicle_name;
  CloudConfig cloud;
  VehicleRuntimeConfig runtime;
  ControlConfig control;
  std::vector<MediaProfile> realtime_profiles;
  std::vector<MediaProfile> record_profiles;
  std::vector<CameraConfig> cameras;
  RecordingConfig recording;
  UploadConfig upload;
  VehicleAdapterConfig vehicle_adapter;
  HardwareConfig hardware;
  FieldSafetyConfig field_safety;

  [[nodiscard]] const MediaProfile& realtime_profile(std::string_view name) const;
  [[nodiscard]] const MediaProfile& record_profile(std::string_view name) const;
  [[nodiscard]] std::vector<CameraConfig> enabled_cameras() const;
  [[nodiscard]] Json redacted_summary() const;
};

VehicleConfig load_vehicle_config(const std::filesystem::path& path);

struct VehicleTelemetry {
  double speed_mps{0.0};
  std::string gear{"N"};
  double steering_feedback{0.0};
  double throttle_feedback{0.0};
  double brake_feedback{0.0};
  bool estop{false};
};

struct VehicleAdapterStatus {
  std::string adapter_type;
  bool opened{false};
  bool healthy{true};
  std::string can_interface;
  std::string library_path;
  std::uint64_t applied_command_count{0};
  std::uint64_t safe_stop_count{0};
  std::string last_error;
  bool feedback_ready{false};

  [[nodiscard]] Json to_json() const;
};

class VehicleAdapter {
 public:
  virtual ~VehicleAdapter() = default;
  virtual void open() = 0;
  virtual void close() = 0;
  virtual void apply_control(const ControlCommand& command) = 0;
  virtual void apply_safe_stop(const ControlOutput& output) = 0;
  virtual bool poll_feedback() = 0;
  [[nodiscard]] virtual bool feedback_ready() const = 0;
  [[nodiscard]] virtual VehicleTelemetry read_telemetry() = 0;
  [[nodiscard]] virtual VehicleAdapterStatus status() const = 0;
};

class MockVehicleAdapter final : public VehicleAdapter {
 public:
  void open() override;
  void close() override;
  void apply_control(const ControlCommand& command) override;
  void apply_safe_stop(const ControlOutput& output) override;
  bool poll_feedback() override;
  [[nodiscard]] bool feedback_ready() const override;
  [[nodiscard]] VehicleTelemetry read_telemetry() override;
  [[nodiscard]] VehicleAdapterStatus status() const override;

 private:
  bool opened_{false};
  std::optional<ControlOutput> latest_output_;
  std::uint64_t applied_command_count_{0};
  std::uint64_t safe_stop_count_{0};
};

class DynamicLibraryVehicleAdapter final : public VehicleAdapter {
 public:
  DynamicLibraryVehicleAdapter(std::filesystem::path library_path, std::string can_interface, double max_speed_mps);
  ~DynamicLibraryVehicleAdapter() override;

  void open() override;
  void close() override;
  void apply_control(const ControlCommand& command) override;
  void apply_safe_stop(const ControlOutput& output) override;
  bool poll_feedback() override;
  [[nodiscard]] bool feedback_ready() const override;
  [[nodiscard]] VehicleTelemetry read_telemetry() override;
  [[nodiscard]] VehicleAdapterStatus status() const override;

 private:
  void ensure_loaded();
  void check_result(int result, std::string_view operation);

  std::filesystem::path library_path_;
  std::string can_interface_;
  double max_speed_mps_;
  void* handle_{nullptr};
  bool opened_{false};
  bool feedback_ready_{false};
  std::uint64_t applied_command_count_{0};
  std::uint64_t safe_stop_count_{0};
  std::string last_error_;

  using OpenFn = int (*)(const char*);
  using ApplyFn = int (*)(int, double, double, const double*, int);
  using StopFn = int (*)();
  using PollFeedbackFn = int (*)(void*);
  using ReadFn = int (*)(void*);
  using CloseFn = int (*)();
  OpenFn open_fn_{nullptr};
  ApplyFn apply_fn_{nullptr};
  StopFn stop_fn_{nullptr};
  PollFeedbackFn poll_feedback_fn_{nullptr};
  ReadFn read_fn_{nullptr};
  CloseFn close_fn_{nullptr};
};

std::unique_ptr<VehicleAdapter> create_vehicle_adapter(const VehicleConfig& config);

class VehicleControlService {
 public:
  VehicleControlService(
      const VehicleConfig& config,
      std::string session_id,
      std::string control_token,
      std::unique_ptr<VehicleAdapter> adapter,
      int telemetry_interval_ms = 100);
  ~VehicleControlService();

  void start(std::int64_t now_ms);
  ReceiveResult receive_command(const ControlCommand& command, std::int64_t now_ms);
  void tick(std::int64_t now_ms);
  bool reset_estop(bool local_confirmed, std::string_view authorized_by, std::int64_t now_ms);
  void close();

  [[nodiscard]] SafetyState safety_state() const { return safety_.state(); }
  [[nodiscard]] VehicleAdapterStatus adapter_status() const { return adapter_->status(); }
  [[nodiscard]] const std::vector<Json>& telemetry_history() const { return telemetry_history_; }
  [[nodiscard]] Json summary() const;

 private:
  [[nodiscard]] Json build_telemetry(std::int64_t now_ms);

  std::string vehicle_id_;
  std::string session_id_;
  ControlReceiver receiver_;
  SafetyStateMachine safety_;
  std::unique_ptr<VehicleAdapter> adapter_;
  bool require_feedback_before_control_{true};
  int telemetry_interval_ms_;
  std::optional<std::int64_t> last_telemetry_ms_;
  std::vector<Json> telemetry_history_;
  bool started_{false};
};

}  // namespace mine_teleop
