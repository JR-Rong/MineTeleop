#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace mine_teleop {

using Json = nlohmann::json;

inline constexpr int kProtocolVersion = 1;
inline constexpr std::size_t kMaxVehicleTelemetryHistory = 1024;
inline constexpr double kDefaultFullScaleMotorTorqueNm = 300.0;
inline constexpr double kMaxFullScaleMotorTorqueNm = 640.0;
inline constexpr double kDefaultMotorTorqueRiseRateNmPerSecond = 0.0;
inline constexpr double kMaxMotorTorqueRiseRateNmPerSecond = 32000.0;
inline constexpr double kDefaultMaxBrakePressureBar = 100.0;
inline constexpr double kMaxOrdinaryBrakePressureBar = 327.6;
inline constexpr double kMaxEmergencyBrakePressureBar = 409.5;
inline constexpr int kSessionControlProfileMaxAgeMs = 2000;
inline constexpr int kSessionControlProfileVersion = 3;
static_assert(kMaxFullScaleMotorTorqueNm <= 800.0 * 0.8);
static_assert(kMaxFullScaleMotorTorqueNm <= 838.3 * 0.8);

std::int64_t now_ms();

enum class SessionState {
  Offline,
  Online,
  Reserved,
  Connecting,
  Active,
  Degraded,
  Stopping,
  Closed,
};

std::string_view to_string(SessionState state);
SessionState session_state_from_string(std::string_view value);

struct ProtocolMetadata {
  int protocol_version{kProtocolVersion};
  std::string vehicle_id;
  std::string driver_id;
  std::string session_id;
  std::uint64_t seq{0};
  std::int64_t sent_at_utc_ms{0};

  void validate() const;
  [[nodiscard]] Json to_json() const;
  static ProtocolMetadata from_json(const Json& value);
};

struct ControlCommand {
  int protocol_version{kProtocolVersion};
  std::string vehicle_id;
  std::string driver_id;
  std::string session_id;
  std::uint64_t seq{0};
  std::int64_t sent_at_utc_ms{0};
  std::string gear{"N"};
  double steering{0.0};
  double throttle{0.0};
  double brake{0.0};
  bool estop{false};
  std::string control_token;

  void validate() const;
  [[nodiscard]] Json to_json() const;
  static ControlCommand from_json(const Json& value);
};

struct SessionControlProfile {
  int profile_version{kSessionControlProfileVersion};
  double target_speed_kph{0.0};
  double max_motor_torque_nm{0.0};
  double max_brake_pressure_bar{0.0};
  double service_brake_pressure_bar{0.0};
  double hard_brake_pressure_bar{0.0};
  double max_steering_angle_deg{0.0};
  double speed_pid_kp{1.0};
  double speed_pid_ki{0.2};
  double speed_pid_kd{0.0};
  double speed_pid_derivative_filter_tau_ms{100.0};
  int speed_pid_max_dt_ms{100};
  double motor_torque_rise_rate_nm_per_s{kDefaultMotorTorqueRiseRateNmPerSecond};

  void validate() const;
  [[nodiscard]] Json to_json() const;
  static SessionControlProfile from_json(const Json& value);
  bool operator==(const SessionControlProfile&) const = default;
};

struct SessionControlProfileRequest {
  int protocol_version{kProtocolVersion};
  std::string vehicle_id;
  std::string driver_id;
  std::string session_id;
  std::uint64_t seq{0};
  std::int64_t sent_at_utc_ms{0};
  std::string control_token;
  SessionControlProfile profile;

  void validate() const;
  [[nodiscard]] Json to_json() const;
  static SessionControlProfileRequest from_json(const Json& value);
  bool operator==(const SessionControlProfileRequest&) const = default;
};

struct SessionControlProfileResult {
  int protocol_version{kProtocolVersion};
  std::string vehicle_id;
  std::string driver_id;
  std::string session_id;
  std::uint64_t seq{0};
  std::int64_t sent_at_utc_ms{0};
  bool accepted{false};
  bool idempotent{false};
  std::uint64_t applied_revision{0};
  std::string reason;
  std::optional<SessionControlProfile> effective_profile;

  void validate() const;
  [[nodiscard]] Json to_json() const;
  static SessionControlProfileResult from_json(const Json& value);
  bool operator==(const SessionControlProfileResult&) const = default;
};

[[nodiscard]] double dynamic_adapter_target_speed_mps(
    const ControlCommand& command,
    double max_speed_mps);

[[nodiscard]] double dynamic_adapter_target_acceleration(
    const ControlCommand& command,
    double traction_ceiling = 1.0,
    double session_max_brake_pressure_bar = 1.0,
    double vehicle_max_brake_pressure_bar = 1.0);

struct ReceiveResult {
  ReceiveResult() = default;
  ReceiveResult(
      bool accepted_value,
      std::string reason_value,
      std::optional<ControlCommand> command_value,
      std::vector<std::string> warnings_value,
      std::string issue_code_value = {})
      : accepted(accepted_value),
        reason(std::move(reason_value)),
        command(std::move(command_value)),
        warnings(std::move(warnings_value)),
        issue_code(std::move(issue_code_value)) {}

  bool accepted{false};
  std::string reason;
  std::optional<ControlCommand> command;
  std::vector<std::string> warnings;
  std::string issue_code;
};

class VehicleAdapterControlRejected final : public std::runtime_error {
 public:
  VehicleAdapterControlRejected(std::string issue_code, int result_code);

  [[nodiscard]] const std::string& issue_code() const noexcept {
    return issue_code_;
  }
  [[nodiscard]] int result_code() const noexcept { return result_code_; }

 private:
  std::string issue_code_;
  int result_code_;
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
      std::string driver_id,
      std::string session_id,
      int max_command_gap_ms,
      int protocol_version,
      bool control_authority,
      std::string control_token,
      int timestamp_warning_skew_ms = 5000);

  ReceiveResult accept(const ControlCommand& command, std::int64_t receive_time_ms);

 private:
  std::string vehicle_id_;
  std::string driver_id_;
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
  bool full_emergency_brake{false};
};

class SafetyStateMachine {
 public:
  SafetyStateMachine(int degraded_timeout_ms, int control_timeout_ms, std::vector<DecelerationStage> profile);

  void mark_ready(std::int64_t now_ms);
  void on_valid_command(const ControlCommand& command, std::int64_t now_ms);
  void tick(std::int64_t now_ms);
  [[nodiscard]] ControlOutput current_output(std::int64_t now_ms) const;
  bool reset_estop(bool local_confirmed, std::string_view authorized_by, std::int64_t now_ms);
  bool reset_to_standby();
  void mark_fault();

  [[nodiscard]] SafetyState state() const { return state_; }
  [[nodiscard]] std::optional<std::int64_t> last_valid_receive_ms() const { return last_valid_receive_ms_; }

 private:
  void enter_standby();
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
  std::vector<std::string> resolve_entries;
  std::filesystem::path ca_bundle;
  std::string ice_transport_policy{"all"};
};

[[nodiscard]] bool ice_transport_policy_is_valid(std::string_view value);

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
  bool critical_for_control{true};
  int reopen_attempts{3};
  int reopen_backoff_ms{500};
  std::string backend{"auto"};
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
  int can_tx_queue_length{100};
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
  double max_throttle{1.0};
  double full_scale_motor_torque_nm{kDefaultFullScaleMotorTorqueNm};
  double motor_torque_rise_rate_nm_per_s{
      kDefaultMotorTorqueRiseRateNmPerSecond};
  int speed_feedback_timeout_ms{200};
  double speed_pid_kp{1.0};
  double speed_pid_ki{0.2};
  double speed_pid_kd{0.0};
  double speed_pid_derivative_filter_tau_ms{100.0};
  int speed_pid_max_dt_ms{100};
  double hard_overspeed_margin_kph{3.6};
  double max_brake_pressure_bar{kDefaultMaxBrakePressureBar};
  double max_steering_angle_deg{30.0};
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
void validate_chassis_bridge_abi(const std::filesystem::path& library_path);

struct VehicleCanFeedback {
  bool supported{false};
  bool feedback_fresh{false};
  std::int64_t max_feedback_age_ms{-1};
  double speed_mps{0.0};
  bool speed_valid{false};
  int gear{0};
  bool gear_valid{false};
  int emergency_switch{0};
  int driver_gear_request{0};
  bool driver_gear_request_valid{false};
  int handshake_status{0};
  bool handshake_valid{false};
  std::array<int, 4> parking_brake_status{};
  std::array<bool, 4> parking_brake_valid{};
  std::array<int, 8> motor_mode{};
  std::array<bool, 8> motor_mode_valid{};
  std::array<double, 8> motor_torque_nm{};
  std::array<bool, 8> motor_torque_valid{};
  std::array<double, 8> motor_speed_rpm{};
  std::array<bool, 8> motor_speed_valid{};
  std::array<int, 4> steering_mode{};
  std::array<bool, 4> steering_valid{};
  std::array<double, 4> steering_angle_deg{};
  std::array<int, 8> brake_mode{};
  std::array<bool, 8> brake_valid{};
  std::array<double, 8> brake_pressure_bar{};

  [[nodiscard]] Json to_json() const;
};

struct VehicleTelemetry {
  double speed_mps{0.0};
  std::string gear{"N"};
  double steering_feedback{0.0};
  double throttle_feedback{0.0};
  double brake_feedback{0.0};
  bool estop{false};
  VehicleCanFeedback can_feedback;
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
  int can_bitrate{0};
  int can_tx_queue_length{0};

  [[nodiscard]] Json to_json() const;
};

struct VcuHandshakeStatus {
  bool supported{false};
  std::string state{"unsupported"};
  bool requested{false};
  bool ready{false};
  bool disarming{false};
  bool parking_ready{false};
  int driver_gear_request{0};
  bool driver_gear_request_valid{false};
  int handshake_status{0};
  bool handshake_valid{false};
  std::array<int, 4> epb_status{};
  std::array<bool, 4> epb_valid{};
  double speed_mps{0.0};
  bool speed_valid{false};

  [[nodiscard]] Json to_json() const;
};

class VehicleAdapter {
 public:
  virtual ~VehicleAdapter() = default;
  virtual void open() = 0;
  virtual void close() = 0;
  [[nodiscard]] virtual std::uint64_t configure_runtime_control_profile(
      const SessionControlProfile& profile,
      std::uint64_t profile_revision) = 0;
  virtual void clear_runtime_control_profile() = 0;
  [[nodiscard]] virtual double session_motor_torque_limit_nm() const = 0;
  [[nodiscard]] virtual double session_brake_pressure_limit_bar() const = 0;
  virtual void apply_control(const ControlCommand& command) = 0;
  virtual void apply_safe_stop(const ControlOutput& output) = 0;
  virtual bool poll_feedback() = 0;
  virtual bool request_vcu_handshake() = 0;
  virtual bool disconnect_vcu_handshake() = 0;
  [[nodiscard]] virtual bool feedback_ready() const = 0;
  [[nodiscard]] virtual VcuHandshakeStatus vcu_handshake_status() const = 0;
  [[nodiscard]] virtual VehicleTelemetry read_telemetry() = 0;
  [[nodiscard]] virtual VehicleAdapterStatus status() const = 0;
};

class MockVehicleAdapter final : public VehicleAdapter {
 public:
  void open() override;
  void close() override;
  [[nodiscard]] std::uint64_t configure_runtime_control_profile(
      const SessionControlProfile& profile,
      std::uint64_t profile_revision) override;
  void clear_runtime_control_profile() override;
  [[nodiscard]] double session_motor_torque_limit_nm() const override;
  [[nodiscard]] double session_brake_pressure_limit_bar() const override;
  void apply_control(const ControlCommand& command) override;
  void apply_safe_stop(const ControlOutput& output) override;
  bool poll_feedback() override;
  bool request_vcu_handshake() override;
  bool disconnect_vcu_handshake() override;
  [[nodiscard]] bool feedback_ready() const override;
  [[nodiscard]] VcuHandshakeStatus vcu_handshake_status() const override;
  [[nodiscard]] VehicleTelemetry read_telemetry() override;
  [[nodiscard]] VehicleAdapterStatus status() const override;

 private:
  bool opened_{false};
  std::optional<ControlOutput> latest_output_;
  std::uint64_t applied_command_count_{0};
  std::uint64_t safe_stop_count_{0};
  double session_motor_torque_limit_nm_{0.0};
  double session_brake_pressure_limit_bar_{0.0};
  std::uint64_t runtime_control_profile_revision_{0};
};

class DynamicLibraryVehicleAdapter final : public VehicleAdapter {
 public:
  DynamicLibraryVehicleAdapter(
      std::filesystem::path library_path,
      std::string can_interface,
      int can_bitrate,
      int can_tx_queue_length,
      double max_speed_mps,
      double full_scale_motor_torque_nm,
      double motor_torque_rise_rate_nm_per_s,
      double max_ordinary_brake_pressure_bar,
      int control_timeout_ms,
      int speed_feedback_timeout_ms,
      double speed_pid_kp,
      double speed_pid_ki,
      double speed_pid_kd,
      double speed_pid_derivative_filter_tau_ms,
      int speed_pid_max_dt_ms,
      double hard_overspeed_margin_mps);
  ~DynamicLibraryVehicleAdapter() override;

  void open() override;
  void close() override;
  [[nodiscard]] std::uint64_t configure_runtime_control_profile(
      const SessionControlProfile& profile,
      std::uint64_t profile_revision) override;
  void clear_runtime_control_profile() override;
  [[nodiscard]] double session_motor_torque_limit_nm() const override;
  [[nodiscard]] double session_brake_pressure_limit_bar() const override;
  void apply_control(const ControlCommand& command) override;
  void apply_safe_stop(const ControlOutput& output) override;
  bool poll_feedback() override;
  bool request_vcu_handshake() override;
  bool disconnect_vcu_handshake() override;
  [[nodiscard]] bool feedback_ready() const override;
  [[nodiscard]] VcuHandshakeStatus vcu_handshake_status() const override;
  [[nodiscard]] VehicleTelemetry read_telemetry() override;
  [[nodiscard]] VehicleAdapterStatus status() const override;

 private:
  void ensure_loaded();
  void check_result(int result, std::string_view operation);

  std::filesystem::path library_path_;
  std::string can_interface_;
  int can_bitrate_;
  int can_tx_queue_length_;
  double max_speed_mps_;
  double full_scale_motor_torque_nm_;
  double motor_torque_rise_rate_nm_per_s_;
  double max_ordinary_brake_pressure_bar_;
  double session_motor_torque_limit_nm_;
  double session_brake_pressure_limit_bar_;
  std::uint64_t runtime_control_profile_revision_{0};
  int control_timeout_ms_;
  int speed_feedback_timeout_ms_;
  double speed_pid_kp_;
  double speed_pid_ki_;
  double speed_pid_kd_;
  double speed_pid_derivative_filter_tau_ms_;
  int speed_pid_max_dt_ms_;
  double hard_overspeed_margin_mps_;
  void* handle_{nullptr};
  bool opened_{false};
  bool feedback_ready_{false};
  std::uint64_t applied_command_count_{0};
  std::uint64_t safe_stop_count_{0};
  std::string last_error_;

  using OpenV4Fn = int (*)(const void*);
  using ApplyFn = int (*)(int, double, double, const double*, int);
  using ApplyV2Fn = int (*)(int, double, double, const double*, int, void*);
  using ConfigureRuntimeControlV2Fn = int (*)(const void*, void*);
  using ClearRuntimeControlFn = int (*)(void*);
  using StopFn = int (*)();
  using HandshakeFn = int (*)();
  using PollFeedbackFn = int (*)(void*);
  using ReadHandshakeFn = int (*)(void*);
  using ReadFn = int (*)(void*);
  using ReadCanFeedbackV1Fn = int (*)(void*);
  using CloseFn = int (*)();
  OpenV4Fn open_v4_fn_{nullptr};
  ApplyFn apply_fn_{nullptr};
  ApplyV2Fn apply_v2_fn_{nullptr};
  ConfigureRuntimeControlV2Fn configure_runtime_control_v2_fn_{nullptr};
  ClearRuntimeControlFn clear_runtime_control_fn_{nullptr};
  StopFn stop_fn_{nullptr};
  HandshakeFn request_handshake_fn_{nullptr};
  HandshakeFn disconnect_handshake_fn_{nullptr};
  PollFeedbackFn poll_feedback_fn_{nullptr};
  ReadHandshakeFn read_handshake_fn_{nullptr};
  ReadFn read_fn_{nullptr};
  ReadCanFeedbackV1Fn read_can_feedback_v1_fn_{nullptr};
  CloseFn close_fn_{nullptr};
};

std::unique_ptr<VehicleAdapter> create_vehicle_adapter(const VehicleConfig& config);

class VehicleControlService {
 public:
  VehicleControlService(
      const VehicleConfig& config,
      std::string driver_id,
      std::string session_id,
      std::string control_token,
      std::unique_ptr<VehicleAdapter> adapter,
      int telemetry_interval_ms = 100);
  ~VehicleControlService();

  void start(std::int64_t now_ms);
  ReceiveResult receive_command(const ControlCommand& command, std::int64_t now_ms);
  SessionControlProfileResult receive_session_profile(
      const SessionControlProfileRequest& request,
      std::int64_t now_ms);
  void tick(std::int64_t now_ms);
  bool request_vcu_handshake();
  bool disconnect_vcu_handshake();
  bool reset_estop(bool local_confirmed, std::string_view authorized_by, std::int64_t now_ms);
  void close();

  [[nodiscard]] SafetyState safety_state() const { return safety_.state(); }
  [[nodiscard]] VehicleAdapterStatus adapter_status() const { return adapter_->status(); }
  [[nodiscard]] VcuHandshakeStatus vcu_handshake_status() const {
    return adapter_->vcu_handshake_status();
  }
  [[nodiscard]] Json control_limits() const;
  [[nodiscard]] Json session_control_profile() const;
  [[nodiscard]] const std::deque<Json>& telemetry_history() const { return telemetry_history_; }
  [[nodiscard]] Json summary() const;

 private:
  [[nodiscard]] bool refresh_adapter_safe_stop_state() noexcept;
  void clear_session_profile() noexcept;
  [[nodiscard]] SessionControlProfileResult profile_result(
      const SessionControlProfileRequest& request,
      std::int64_t now_ms,
      bool accepted,
      bool idempotent,
      std::string reason) const;
  [[nodiscard]] Json build_telemetry(std::int64_t now_ms);

  std::string vehicle_id_;
  std::string driver_id_;
  std::string session_id_;
  std::string control_token_;
  std::string commissioning_mode_;
  ControlReceiver receiver_;
  SafetyStateMachine safety_;
  std::unique_ptr<VehicleAdapter> adapter_;
  bool require_feedback_before_control_{true};
  double max_speed_kph_{40.0};
  double max_throttle_{1.0};
  double full_scale_motor_torque_nm_{kDefaultFullScaleMotorTorqueNm};
  double max_brake_pressure_bar_{kDefaultMaxBrakePressureBar};
  double max_steering_angle_deg_{30.0};
  double default_speed_pid_kp_{1.0};
  double default_speed_pid_ki_{0.2};
  double default_speed_pid_kd_{0.0};
  double default_speed_pid_derivative_filter_tau_ms_{100.0};
  int default_speed_pid_max_dt_ms_{100};
  double default_motor_torque_rise_rate_nm_per_s_{
      kDefaultMotorTorqueRiseRateNmPerSecond};
  int speed_feedback_timeout_ms_{200};
  double hard_overspeed_margin_kph_{3.6};
  Json read_only_control_safety_;
  int telemetry_interval_ms_;
  std::optional<std::int64_t> last_telemetry_ms_;
  std::uint64_t telemetry_sequence_{0};
  std::deque<Json> telemetry_history_;
  std::optional<SessionControlProfile> active_session_profile_;
  std::optional<SessionControlProfileRequest> last_session_profile_request_;
  std::optional<SessionControlProfileResult> last_session_profile_result_;
  std::optional<ControlCommand> last_effective_command_;
  bool adapter_safe_stop_active_{false};
  bool started_{false};
};

}  // namespace mine_teleop
