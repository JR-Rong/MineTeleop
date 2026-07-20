#include "mine_teleop/core.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <dlfcn.h>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#include <yaml-cpp/yaml.h>

namespace mine_teleop {
namespace {

template <typename T>
T required(const YAML::Node& node, const char* key, std::string_view context) {
  const auto value = node[key];
  if (!value) {
    throw std::runtime_error(std::string(context) + "." + key + " is required");
  }
  try {
    return value.as<T>();
  } catch (const YAML::Exception& error) {
    throw std::runtime_error(std::string(context) + "." + key + ": " + error.what());
  }
}

template <typename T>
T optional(const YAML::Node& node, const char* key, T fallback) {
  const auto value = node[key];
  if (!value) {
    return fallback;
  }
  try {
    return value.as<T>();
  } catch (const YAML::Exception& error) {
    throw std::runtime_error(std::string(key) + ": " + error.what());
  }
}

void require_finite_range(double value, double minimum, double maximum, std::string_view label) {
  if (!std::isfinite(value) || value < minimum || value > maximum) {
    throw std::invalid_argument(std::string(label) + " must be a finite value in [" +
                                std::to_string(minimum) + ", " + std::to_string(maximum) + "]");
  }
}

int gear_to_bridge_value(std::string_view gear) {
  if (gear == "N") return 1;
  if (gear == "R") return 2;
  if (gear == "D") return 3;
  if (gear == "P") return 4;
  throw std::invalid_argument("unsupported gear");
}

std::string bridge_value_to_gear(int gear) {
  switch (gear) {
    case 2:
      return "R";
    case 3:
      return "D";
    case 4:
      return "P";
    default:
      return "N";
  }
}

struct BridgeTelemetry {
  double speed_mps;
  int gear;
  double steering_feedback;
  double throttle_feedback;
  double brake_feedback;
  int estop;
};

struct BridgeFeedback {
  int shake_hand_status;
  int epb_status[4];
  int gear_status;
  int mcu_mode[8];
  int eps_mode[4];
  double eps_angle[4];
  int ehb_mode[8];
  double vehicle_speed;
  int vehicle_speed_valid;
};

template <typename Function>
Function load_symbol(void* handle, const char* name) {
  dlerror();
  void* symbol = dlsym(handle, name);
  const char* error = dlerror();
  if (error != nullptr || symbol == nullptr) {
    throw std::runtime_error(std::string("dynamic library is missing required symbol ") + name +
                             (error == nullptr ? "" : std::string(": ") + error));
  }
  return reinterpret_cast<Function>(symbol);
}

}  // namespace

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void ControlCommand::validate() const {
  if (protocol_version != 1) {
    throw std::invalid_argument("unsupported protocol_version");
  }
  if (vehicle_id.empty()) {
    throw std::invalid_argument("vehicle_id is required");
  }
  if (session_id.empty()) {
    throw std::invalid_argument("session_id is required");
  }
  static const std::unordered_set<std::string> allowed_gears{"P", "R", "N", "D"};
  if (!allowed_gears.contains(gear)) {
    throw std::invalid_argument("gear must be one of P/R/N/D");
  }
  require_finite_range(steering, -1.0, 1.0, "steering");
  require_finite_range(throttle, 0.0, 1.0, "throttle");
  require_finite_range(brake, 0.0, 1.0, "brake");
}

Json ControlCommand::to_json() const {
  return {
      {"type", "control_command"},
      {"protocol_version", protocol_version},
      {"vehicle_id", vehicle_id},
      {"session_id", session_id},
      {"seq", seq},
      {"ts_ms", ts_ms},
      {"gear", gear},
      {"steering", steering},
      {"throttle", throttle},
      {"brake", brake},
      {"estop", estop},
      {"authority_token", authority_token},
  };
}

ControlCommand ControlCommand::from_json(const Json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("control command must be a JSON object");
  }
  if (value.value("type", "") != "control_command") {
    throw std::invalid_argument("type must be control_command");
  }
  ControlCommand command;
  try {
    command.protocol_version = value.at("protocol_version").get<int>();
    command.vehicle_id = value.at("vehicle_id").get<std::string>();
    command.session_id = value.at("session_id").get<std::string>();
    command.seq = value.at("seq").get<std::uint64_t>();
    command.ts_ms = value.at("ts_ms").get<std::int64_t>();
    command.gear = value.at("gear").get<std::string>();
    command.steering = value.at("steering").get<double>();
    command.throttle = value.at("throttle").get<double>();
    command.brake = value.at("brake").get<double>();
    command.estop = value.value("estop", false);
    command.authority_token = value.value("authority_token", "");
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid control command: ") + error.what());
  }
  command.validate();
  return command;
}

void LatestControlCommandMailbox::publish(ControlCommand command) {
  command.validate();
  std::lock_guard lock(mutex_);
  if (latest_) {
    ++dropped_count_;
  }
  latest_ = std::move(command);
}

std::optional<ControlCommand> LatestControlCommandMailbox::pop_latest() {
  std::lock_guard lock(mutex_);
  auto command = std::move(latest_);
  latest_.reset();
  return command;
}

std::size_t LatestControlCommandMailbox::pending_count() const {
  std::lock_guard lock(mutex_);
  return latest_ ? 1U : 0U;
}

std::uint64_t LatestControlCommandMailbox::dropped_count() const {
  std::lock_guard lock(mutex_);
  return dropped_count_;
}

ControlReceiver::ControlReceiver(
    std::string vehicle_id,
    std::string session_id,
    int max_command_gap_ms,
    int protocol_version,
    bool control_authority,
    std::string control_token,
    int timestamp_warning_skew_ms)
    : vehicle_id_(std::move(vehicle_id)),
      session_id_(std::move(session_id)),
      max_command_gap_ms_(max_command_gap_ms),
      protocol_version_(protocol_version),
      control_authority_(control_authority),
      control_token_(std::move(control_token)),
      timestamp_warning_skew_ms_(timestamp_warning_skew_ms) {
  if (vehicle_id_.empty() || session_id_.empty()) {
    throw std::invalid_argument("vehicle_id and session_id are required");
  }
  if (max_command_gap_ms_ <= 0 || timestamp_warning_skew_ms_ < 0) {
    throw std::invalid_argument("control receiver timing values are invalid");
  }
}

ReceiveResult ControlReceiver::accept(const ControlCommand& command, std::int64_t receive_time_ms) {
  if (receive_time_ms < 0) {
    throw std::invalid_argument("receive_time_ms must be non-negative");
  }
  try {
    command.validate();
  } catch (const std::exception& error) {
    return {false, std::string("invalid_command:") + error.what(), std::nullopt, {}};
  }
  if (command.protocol_version != protocol_version_) return {false, "wrong_protocol_version", std::nullopt, {}};
  if (command.vehicle_id != vehicle_id_) return {false, "wrong_vehicle", std::nullopt, {}};
  if (command.session_id != session_id_) return {false, "wrong_session", std::nullopt, {}};
  if (!control_authority_) return {false, "control_authority_missing", std::nullopt, {}};
  if (!control_token_.empty() && command.authority_token != control_token_) {
    return {false, "control_token_invalid", std::nullopt, {}};
  }
  if (last_seq_ && command.seq <= *last_seq_) return {false, "old_seq", std::nullopt, {}};
  if (last_valid_receive_ms_ && receive_time_ms < *last_valid_receive_ms_) {
    return {false, "receive_time_reversed", std::nullopt, {}};
  }
  if (last_valid_receive_ms_ && receive_time_ms - *last_valid_receive_ms_ > max_command_gap_ms_ && !command.estop) {
    return {false, "command_gap_exceeded", std::nullopt, {}};
  }
  last_seq_ = command.seq;
  last_valid_receive_ms_ = receive_time_ms;
  std::vector<std::string> warnings;
  if (std::llabs(receive_time_ms - command.ts_ms) > timestamp_warning_skew_ms_) {
    warnings.emplace_back("driver_timestamp_skew");
  }
  return {true, "accepted", command, std::move(warnings)};
}

std::string_view to_string(SafetyState state) {
  switch (state) {
    case SafetyState::Init:
      return "INIT";
    case SafetyState::Standby:
      return "STANDBY";
    case SafetyState::ControlActive:
      return "CONTROL_ACTIVE";
    case SafetyState::Degraded:
      return "DEGRADED";
    case SafetyState::TimeoutBrake:
      return "TIMEOUT_BRAKE";
    case SafetyState::Estop:
      return "ESTOP";
    case SafetyState::Fault:
      return "FAULT";
  }
  return "FAULT";
}

SafetyStateMachine::SafetyStateMachine(
    int degraded_timeout_ms,
    int control_timeout_ms,
    std::vector<DecelerationStage> profile)
    : degraded_timeout_ms_(degraded_timeout_ms),
      control_timeout_ms_(control_timeout_ms),
      profile_(std::move(profile)) {
  if (degraded_timeout_ms_ <= 0 || control_timeout_ms_ <= degraded_timeout_ms_) {
    throw std::invalid_argument("control timeout must be greater than degraded timeout");
  }
  if (profile_.empty()) {
    throw std::invalid_argument("deceleration profile must not be empty");
  }
  std::sort(profile_.begin(), profile_.end(), [](const auto& left, const auto& right) {
    return left.after_ms < right.after_ms;
  });
  for (const auto& stage : profile_) {
    if (stage.after_ms < 0) throw std::invalid_argument("deceleration after_ms must be non-negative");
    require_finite_range(stage.brake, 0.0, 1.0, "deceleration brake");
  }
}

void SafetyStateMachine::mark_ready(std::int64_t /*now_ms*/) {
  if (state_ == SafetyState::Init) state_ = SafetyState::Standby;
}

void SafetyStateMachine::on_valid_command(const ControlCommand& command, std::int64_t timestamp_ms) {
  if (command.estop) {
    last_valid_command_ = command;
    last_valid_receive_ms_ = timestamp_ms;
    state_ = SafetyState::Estop;
    return;
  }
  if (state_ == SafetyState::Estop || state_ == SafetyState::Fault) return;
  last_valid_command_ = command;
  last_valid_receive_ms_ = timestamp_ms;
  timeout_entered_ms_.reset();
  state_ = SafetyState::ControlActive;
}

void SafetyStateMachine::tick(std::int64_t timestamp_ms) {
  if (state_ == SafetyState::Init || state_ == SafetyState::Standby || state_ == SafetyState::Estop ||
      state_ == SafetyState::Fault || !last_valid_receive_ms_) {
    return;
  }
  const auto elapsed = timestamp_ms - *last_valid_receive_ms_;
  if (elapsed >= control_timeout_ms_) {
    if (state_ != SafetyState::TimeoutBrake) timeout_entered_ms_ = timestamp_ms;
    state_ = SafetyState::TimeoutBrake;
  } else if (elapsed >= degraded_timeout_ms_) {
    state_ = SafetyState::Degraded;
  }
}

ControlOutput SafetyStateMachine::current_output(std::int64_t timestamp_ms) const {
  const auto gear = last_valid_command_ ? last_valid_command_->gear : "N";
  const auto steering = last_valid_command_ ? last_valid_command_->steering : 0.0;
  switch (state_) {
    case SafetyState::ControlActive:
      if (last_valid_command_) {
        return {gear, steering, last_valid_command_->throttle, last_valid_command_->brake, false};
      }
      break;
    case SafetyState::Degraded:
      return {gear, steering, 0.0, last_valid_command_ ? last_valid_command_->brake : 0.0, false};
    case SafetyState::TimeoutBrake:
      return {gear, 0.0, 0.0, brake_for_timeout(timestamp_ms), false};
    case SafetyState::Estop:
      return {gear, 0.0, 0.0, 1.0, true};
    case SafetyState::Fault:
      return {gear, 0.0, 0.0, 1.0, false};
    case SafetyState::Init:
    case SafetyState::Standby:
      break;
  }
  return {};
}

bool SafetyStateMachine::reset_estop(
    bool local_confirmed,
    std::string_view authorized_by,
    std::int64_t /*now_ms*/) {
  if (state_ != SafetyState::Estop || !local_confirmed || authorized_by.empty()) return false;
  last_valid_command_.reset();
  last_valid_receive_ms_.reset();
  timeout_entered_ms_.reset();
  state_ = SafetyState::Standby;
  return true;
}

void SafetyStateMachine::mark_fault() { state_ = SafetyState::Fault; }

double SafetyStateMachine::brake_for_timeout(std::int64_t timestamp_ms) const {
  const auto entered = timeout_entered_ms_.value_or(timestamp_ms);
  const auto elapsed = timestamp_ms - entered;
  double chosen = 0.0;
  for (const auto& stage : profile_) {
    if (elapsed >= stage.after_ms) chosen = stage.brake;
  }
  return chosen;
}

const MediaProfile& VehicleConfig::realtime_profile(std::string_view name) const {
  const auto found = std::find_if(realtime_profiles.begin(), realtime_profiles.end(), [&](const auto& profile) {
    return profile.name == name;
  });
  if (found == realtime_profiles.end()) throw std::out_of_range("unknown realtime profile: " + std::string(name));
  return *found;
}

const MediaProfile& VehicleConfig::record_profile(std::string_view name) const {
  const auto found = std::find_if(record_profiles.begin(), record_profiles.end(), [&](const auto& value) {
    return value.name == name;
  });
  if (found == record_profiles.end()) throw std::runtime_error("unknown record media profile: " + std::string(name));
  return *found;
}

std::vector<CameraConfig> VehicleConfig::enabled_cameras() const {
  std::vector<CameraConfig> result;
  std::copy_if(cameras.begin(), cameras.end(), std::back_inserter(result), [](const auto& camera) {
    return camera.enabled;
  });
  return result;
}

Json VehicleConfig::redacted_summary() const {
  return {
      {"event", "effective_vehicle_config"},
      {"runtime", "cpp"},
      {"vehicle_id", vehicle_id},
      {"signaling_url", cloud.signaling_url},
      {"camera_count", enabled_cameras().size()},
      {"vehicle_adapter_type", vehicle_adapter.type},
      {"can_interface", hardware.can_interface},
      {"recording_root", recording.root_dir.string()},
      {"upload_enabled", upload.enabled},
  };
}

VehicleConfig load_vehicle_config(const std::filesystem::path& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::Exception& error) {
    throw std::runtime_error("failed to load vehicle config " + path.string() + ": " + error.what());
  }

  VehicleConfig config;
  const auto vehicle = root["vehicle"];
  config.vehicle_id = required<std::string>(vehicle, "id", "vehicle");
  config.vehicle_name = optional<std::string>(vehicle, "name", config.vehicle_id);

  const auto cloud = root["cloud"];
  config.cloud.signaling_url = required<std::string>(cloud, "signaling_url", "cloud");
  config.cloud.auth_url = optional<std::string>(cloud, "auth_url", "");

  const auto control = root["control"];
  config.control.rate_hz = optional<int>(control, "rate_hz", 20);
  config.control.max_command_gap_ms = optional<int>(control, "max_command_gap_ms", 200);
  config.control.degraded_timeout_ms = optional<int>(control, "degraded_timeout_ms", 300);
  config.control.control_timeout_ms = optional<int>(control, "control_timeout_ms", 800);
  const auto profile = control["timeout_action"]["deceleration_profile"];
  if (!profile || !profile.IsSequence()) throw std::runtime_error("control.timeout_action.deceleration_profile is required");
  for (const auto& item : profile) {
    DecelerationStage stage;
    stage.after_ms = required<int>(item, "after_ms", "deceleration_profile");
    const auto brake = item["brake"];
    if (!brake) throw std::runtime_error("deceleration_profile.brake is required");
    if (brake.IsScalar() && brake.Scalar() == "vehicle_defined_max_safe") {
      stage.brake = 1.0;
    } else {
      stage.brake = brake.as<double>();
    }
    config.control.deceleration_profile.push_back(stage);
  }

  const auto realtime = root["media"]["realtime_profiles"];
  if (!realtime || !realtime.IsMap()) throw std::runtime_error("media.realtime_profiles is required");
  for (const auto& entry : realtime) {
    MediaProfile value;
    value.name = entry.first.as<std::string>();
    const auto node = entry.second;
    value.codec = optional<std::string>(node, "codec", "h264");
    value.encoder = optional<std::string>(node, "encoder", "x264");
    value.width = required<int>(node, "width", value.name);
    value.height = required<int>(node, "height", value.name);
    value.fps = required<int>(node, "fps", value.name);
    value.bitrate_kbps = required<int>(node, "bitrate_kbps", value.name);
    config.realtime_profiles.push_back(std::move(value));
  }

  const auto records = root["media"]["record_profiles"];
  if (records && records.IsMap()) {
    for (const auto& entry : records) {
      MediaProfile value;
      value.name = entry.first.as<std::string>();
      const auto node = entry.second;
      value.codec = optional<std::string>(node, "codec", "h264");
      value.encoder = optional<std::string>(node, "encoder", "x264");
      const auto width = node["width"];
      const auto height = node["height"];
      const auto fps = node["fps"];
      value.width = width && width.IsScalar() && width.Scalar() != "source" ? width.as<int>() : 0;
      value.height = height && height.IsScalar() && height.Scalar() != "source" ? height.as<int>() : 0;
      value.fps = fps && fps.IsScalar() && fps.Scalar() != "source" ? fps.as<int>() : 0;
      value.bitrate_kbps = required<int>(node, "bitrate_kbps", value.name);
      value.segment_seconds = optional<int>(node, "segment_seconds", 60);
      if (value.segment_seconds <= 0) throw std::runtime_error("record segment_seconds must be positive");
      config.record_profiles.push_back(std::move(value));
    }
  }

  const auto cameras = root["cameras"];
  if (!cameras || !cameras.IsSequence()) throw std::runtime_error("cameras must be a list");
  for (const auto& node : cameras) {
    CameraConfig camera;
    camera.id = required<std::string>(node, "id", "camera");
    camera.enabled = optional<bool>(node, "enabled", true);
    camera.device = required<std::string>(node, "device", camera.id);
    camera.capture_width = optional<int>(node, "capture_width", 1280);
    camera.capture_height = optional<int>(node, "capture_height", 720);
    camera.capture_fps = optional<int>(node, "capture_fps", 30);
    camera.realtime_profile = required<std::string>(node, "realtime_profile", camera.id);
    camera.record_profile = optional<std::string>(node, "record_profile", "");
    static_cast<void>(config.realtime_profile(camera.realtime_profile));
    if (!camera.record_profile.empty()) static_cast<void>(config.record_profile(camera.record_profile));
    config.cameras.push_back(std::move(camera));
  }
  if (config.enabled_cameras().empty()) throw std::runtime_error("at least one camera must be enabled");

  const auto hardware = root["hardware"];
  const auto can = hardware["can"];
  config.hardware.can_interface = optional<std::string>(can, "interface", "can0");
  config.hardware.can_bitrate = optional<int>(can, "bitrate", 500000);
  const auto encoding = hardware["encoding"];
  config.hardware.vaapi_render_device = optional<std::string>(encoding, "vaapi_render_device", "/dev/dri/renderD128");
  config.hardware.dri_card_device = optional<std::string>(encoding, "dri_card_device", "/dev/dri/card1");
  config.hardware.ffmpeg_binary = optional<std::string>(encoding, "ffmpeg_binary", "ffmpeg");
  config.hardware.ffprobe_binary = optional<std::string>(encoding, "ffprobe_binary", "ffprobe");
  config.hardware.network_interface = optional<std::string>(hardware["network"], "interface", "wwan0");

  const auto safety = root["field_safety"];
  config.field_safety.commissioning_mode = optional<std::string>(safety, "commissioning_mode", "bench");
  config.field_safety.max_speed_kph = optional<double>(safety, "max_speed_kph", 40.0);
  config.field_safety.require_can_feedback_before_control =
      optional<bool>(safety, "require_can_feedback_before_control", true);
  config.field_safety.require_local_estop_reset = optional<bool>(safety, "require_local_estop_reset", true);
  config.field_safety.require_time_sync = optional<bool>(safety, "require_time_sync", true);

  const auto recording = root["recording"];
  config.recording.root_dir = optional<std::string>(recording, "root_dir", ".local/recordings");
  config.recording.min_free_gb = optional<double>(recording, "min_free_gb", 5.0);
  config.recording.delete_uploaded_when_below_free_gb =
      optional<double>(recording, "delete_uploaded_when_below_free_gb", 2.0);
  config.recording.delete_unuploaded_when_below_free_gb =
      optional<bool>(recording, "delete_unuploaded_when_below_free_gb", false);

  const auto upload = root["upload"];
  config.upload.enabled = optional<bool>(upload, "enabled", false);
  config.upload.backend = optional<std::string>(upload, "backend", "local_archive");
  config.upload.max_bandwidth_mbps = optional<double>(upload, "max_bandwidth_mbps", 5.0);
  config.upload.trigger_segments = optional<int>(upload, "trigger_segments", 20);
  config.upload.trigger_network_idle = optional<bool>(upload, "trigger_network_idle", true);
  config.upload.retry_initial_seconds = optional<int>(upload, "retry_initial_seconds", 10);
  config.upload.retry_max_seconds = optional<int>(upload, "retry_max_seconds", 600);

  const auto adapter = root["vehicle_adapter"];
  config.vehicle_adapter.type = optional<std::string>(adapter, "type", "mock");
  config.vehicle_adapter.can_interface = config.hardware.can_interface;
  YAML::Node chassis;
  const auto integration = adapter["integration"];
  if (integration && integration.IsMap()) chassis = integration["chassis_control"];
  if (chassis) {
    config.vehicle_adapter.can_interface = optional<std::string>(chassis, "can_interface", config.hardware.can_interface);
    config.vehicle_adapter.bridge_library_path = optional<std::string>(chassis, "bridge_library_path", "");
  }
  if (config.vehicle_adapter.can_interface != config.hardware.can_interface) {
    throw std::runtime_error("hardware.can.interface and vehicle adapter can_interface must match");
  }
  if (config.vehicle_adapter.type != "mock" && config.vehicle_adapter.bridge_library_path.empty()) {
    throw std::runtime_error("non-mock vehicle adapter requires bridge_library_path");
  }

  return config;
}

Json VehicleAdapterStatus::to_json() const {
  Json value = {
      {"adapter_type", adapter_type},
      {"opened", opened},
      {"healthy", healthy},
      {"applied_command_count", applied_command_count},
      {"safe_stop_count", safe_stop_count},
      {"feedback_ready", feedback_ready},
  };
  if (!can_interface.empty()) value["can_interface"] = can_interface;
  if (!library_path.empty()) value["library_path"] = library_path;
  if (!last_error.empty()) value["last_error"] = last_error;
  return value;
}

void MockVehicleAdapter::open() { opened_ = true; }
void MockVehicleAdapter::close() { opened_ = false; }

void MockVehicleAdapter::apply_control(const ControlCommand& command) {
  if (!opened_) throw std::runtime_error("mock vehicle adapter is not open");
  latest_output_ = ControlOutput{command.gear, command.steering, command.throttle, command.brake, command.estop};
  ++applied_command_count_;
}

void MockVehicleAdapter::apply_safe_stop(const ControlOutput& output) {
  if (!opened_) throw std::runtime_error("mock vehicle adapter is not open");
  latest_output_ = output;
  ++safe_stop_count_;
}

VehicleTelemetry MockVehicleAdapter::read_telemetry() {
  if (!latest_output_) return {};
  return {
      latest_output_->throttle * 2.0,
      latest_output_->gear,
      latest_output_->steering,
      latest_output_->throttle,
      latest_output_->brake,
      latest_output_->estop,
  };
}

bool MockVehicleAdapter::poll_feedback() { return opened_; }

bool MockVehicleAdapter::feedback_ready() const { return opened_; }

VehicleAdapterStatus MockVehicleAdapter::status() const {
  return {"mock", opened_, true, "", "", applied_command_count_, safe_stop_count_, "", opened_};
}

DynamicLibraryVehicleAdapter::DynamicLibraryVehicleAdapter(
    std::filesystem::path library_path,
    std::string can_interface,
    double max_speed_mps)
    : library_path_(std::move(library_path)),
      can_interface_(std::move(can_interface)),
      max_speed_mps_(max_speed_mps) {
  if (library_path_.empty() || can_interface_.empty() || max_speed_mps_ <= 0.0) {
    throw std::invalid_argument("dynamic adapter configuration is incomplete");
  }
}

DynamicLibraryVehicleAdapter::~DynamicLibraryVehicleAdapter() {
  try {
    close();
  } catch (...) {
  }
  if (handle_ != nullptr) dlclose(handle_);
}

void DynamicLibraryVehicleAdapter::ensure_loaded() {
  if (handle_ != nullptr) return;
  handle_ = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle_ == nullptr) {
    last_error_ = std::string("failed to load dynamic library: ") + dlerror();
    throw std::runtime_error(last_error_);
  }
  try {
    open_fn_ = load_symbol<OpenFn>(handle_, "mine_teleop_chassis_open");
    apply_fn_ = load_symbol<ApplyFn>(handle_, "mine_teleop_chassis_apply_state");
    stop_fn_ = load_symbol<StopFn>(handle_, "mine_teleop_chassis_emergency_stop");
    poll_feedback_fn_ = load_symbol<PollFeedbackFn>(handle_, "mine_teleop_chassis_poll_feedback");
    read_fn_ = load_symbol<ReadFn>(handle_, "mine_teleop_chassis_read_telemetry");
    close_fn_ = load_symbol<CloseFn>(handle_, "mine_teleop_chassis_close");
  } catch (...) {
    dlclose(handle_);
    handle_ = nullptr;
    throw;
  }
}

void DynamicLibraryVehicleAdapter::check_result(int result, std::string_view operation) {
  if (result == 0) {
    last_error_.clear();
    return;
  }
  last_error_ = std::string(operation) + " failed with code " + std::to_string(result);
  throw std::runtime_error(last_error_);
}

void DynamicLibraryVehicleAdapter::open() {
  ensure_loaded();
  check_result(open_fn_(can_interface_.c_str()), "mine_teleop_chassis_open");
  opened_ = true;
}

void DynamicLibraryVehicleAdapter::close() {
  if (!opened_ || close_fn_ == nullptr) return;
  check_result(close_fn_(), "mine_teleop_chassis_close");
  opened_ = false;
  feedback_ready_ = false;
}

void DynamicLibraryVehicleAdapter::apply_control(const ControlCommand& command) {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  const double velocity = command.throttle * (1.0 - command.brake) * max_speed_mps_;
  const double acceleration = command.throttle - command.brake;
  const double steering[4]{command.steering, command.steering, command.steering, command.steering};
  check_result(
      apply_fn_(gear_to_bridge_value(command.gear), velocity, acceleration, steering, 4),
      "mine_teleop_chassis_apply_state");
  ++applied_command_count_;
}

void DynamicLibraryVehicleAdapter::apply_safe_stop(const ControlOutput& output) {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  if (output.estop || output.brake >= 1.0) {
    check_result(stop_fn_(), "mine_teleop_chassis_emergency_stop");
  } else {
    const double steering[4]{output.steering, output.steering, output.steering, output.steering};
    check_result(
        apply_fn_(gear_to_bridge_value(output.gear), 0.0, -output.brake, steering, 4),
        "mine_teleop_chassis_apply_state");
  }
  ++safe_stop_count_;
}

bool DynamicLibraryVehicleAdapter::poll_feedback() {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  BridgeFeedback feedback{};
  const int result = poll_feedback_fn_(&feedback);
  if (result == 1) return false;
  check_result(result, "mine_teleop_chassis_poll_feedback");
  feedback_ready_ = true;
  return true;
}

bool DynamicLibraryVehicleAdapter::feedback_ready() const { return feedback_ready_; }

VehicleTelemetry DynamicLibraryVehicleAdapter::read_telemetry() {
  BridgeTelemetry telemetry{};
  check_result(read_fn_(&telemetry), "mine_teleop_chassis_read_telemetry");
  return {
      telemetry.speed_mps,
      bridge_value_to_gear(telemetry.gear),
      telemetry.steering_feedback,
      telemetry.throttle_feedback,
      telemetry.brake_feedback,
      telemetry.estop != 0,
  };
}

VehicleAdapterStatus DynamicLibraryVehicleAdapter::status() const {
  return {
      "dynamic_library",
      opened_,
      last_error_.empty(),
      can_interface_,
      library_path_.string(),
      applied_command_count_,
      safe_stop_count_,
      last_error_,
      feedback_ready_,
  };
}

std::unique_ptr<VehicleAdapter> create_vehicle_adapter(const VehicleConfig& config) {
  if (config.vehicle_adapter.type == "mock") return std::make_unique<MockVehicleAdapter>();
  if (config.vehicle_adapter.type == "can" || config.vehicle_adapter.type == "dynamic_library") {
    return std::make_unique<DynamicLibraryVehicleAdapter>(
        config.vehicle_adapter.bridge_library_path,
        config.vehicle_adapter.can_interface,
        config.field_safety.max_speed_kph / 3.6);
  }
  throw std::runtime_error("unsupported vehicle adapter type: " + config.vehicle_adapter.type);
}

VehicleControlService::VehicleControlService(
    const VehicleConfig& config,
    std::string session_id,
    std::string control_token,
    std::unique_ptr<VehicleAdapter> adapter,
    int telemetry_interval_ms)
    : vehicle_id_(config.vehicle_id),
      session_id_(std::move(session_id)),
      receiver_(config.vehicle_id, session_id_, config.control.max_command_gap_ms, 1, true, std::move(control_token)),
      safety_(
          config.control.degraded_timeout_ms,
          config.control.control_timeout_ms,
          config.control.deceleration_profile),
      adapter_(std::move(adapter)),
      require_feedback_before_control_(config.field_safety.require_can_feedback_before_control),
      telemetry_interval_ms_(telemetry_interval_ms) {
  if (!adapter_) throw std::invalid_argument("vehicle adapter is required");
  if (telemetry_interval_ms_ <= 0) throw std::invalid_argument("telemetry interval must be positive");
}

VehicleControlService::~VehicleControlService() {
  try {
    close();
  } catch (...) {
  }
}

void VehicleControlService::start(std::int64_t timestamp_ms) {
  if (started_) return;
  adapter_->open();
  safety_.mark_ready(timestamp_ms);
  started_ = true;
}

ReceiveResult VehicleControlService::receive_command(const ControlCommand& command, std::int64_t timestamp_ms) {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  auto result = receiver_.accept(command, timestamp_ms);
  if (!result.accepted || !result.command) return result;
  if (!result.command->estop) {
    try {
      adapter_->poll_feedback();
    } catch (const std::exception&) {
      if (require_feedback_before_control_) {
        safety_.mark_fault();
        adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
        return {false, "can_feedback_poll_failed", std::nullopt, result.warnings};
      }
    }
    if (require_feedback_before_control_ && !adapter_->feedback_ready()) {
      adapter_->apply_safe_stop(ControlOutput{"N", 0.0, 0.0, 1.0, false});
      return {false, "can_feedback_missing", std::nullopt, result.warnings};
    }
  }
  safety_.on_valid_command(*result.command, timestamp_ms);
  if (safety_.state() == SafetyState::ControlActive) {
    adapter_->apply_control(*result.command);
  } else {
    adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
  }
  return result;
}

void VehicleControlService::tick(std::int64_t timestamp_ms) {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  try {
    adapter_->poll_feedback();
  } catch (const std::exception&) {
    if (require_feedback_before_control_ && safety_.state() == SafetyState::ControlActive) {
      safety_.mark_fault();
    }
  }
  safety_.tick(timestamp_ms);
  if (safety_.state() == SafetyState::Degraded || safety_.state() == SafetyState::TimeoutBrake ||
      safety_.state() == SafetyState::Estop || safety_.state() == SafetyState::Fault) {
    adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
  }
  if (!last_telemetry_ms_ || timestamp_ms - *last_telemetry_ms_ >= telemetry_interval_ms_) {
    telemetry_history_.push_back(build_telemetry(timestamp_ms));
    last_telemetry_ms_ = timestamp_ms;
  }
}

bool VehicleControlService::reset_estop(
    bool local_confirmed,
    std::string_view authorized_by,
    std::int64_t timestamp_ms) {
  const bool reset = safety_.reset_estop(local_confirmed, authorized_by, timestamp_ms);
  if (reset) adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
  return reset;
}

void VehicleControlService::close() {
  if (!started_) return;
  adapter_->apply_safe_stop(ControlOutput{"N", 0.0, 0.0, 1.0, true});
  adapter_->close();
  started_ = false;
}

Json VehicleControlService::build_telemetry(std::int64_t timestamp_ms) {
  const auto telemetry = adapter_->read_telemetry();
  return {
      {"event", "vehicle_telemetry"},
      {"vehicle_id", vehicle_id_},
      {"session_id", session_id_},
      {"ts_ms", timestamp_ms},
      {"safety_state", to_string(safety_.state())},
      {"speed_mps", telemetry.speed_mps},
      {"gear", telemetry.gear},
      {"steering_feedback", telemetry.steering_feedback},
      {"throttle_feedback", telemetry.throttle_feedback},
      {"brake_feedback", telemetry.brake_feedback},
      {"estop", telemetry.estop},
      {"vehicle_adapter", adapter_->status().to_json()},
  };
}

Json VehicleControlService::summary() const {
  return {
      {"event", "vehicle_control_summary"},
      {"vehicle_id", vehicle_id_},
      {"session_id", session_id_},
      {"safety_state", to_string(safety_.state())},
      {"telemetry_count", telemetry_history_.size()},
      {"vehicle_adapter", adapter_->status().to_json()},
  };
}

}  // namespace mine_teleop
