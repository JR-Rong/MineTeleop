#include "mine_teleop/core.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if defined(__linux__)
#include <linux/can/netlink.h>
#include <linux/if_link.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sockios.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <yaml-cpp/yaml.h>

namespace mine_teleop {
namespace {

constexpr double kChassisControlMaxTargetSpeedMps = 20.0;
constexpr double kChassisControlMaxTargetSpeedKph =
    kChassisControlMaxTargetSpeedMps * 3.6;
constexpr double kChassisControlMaxSteeringAngleDeg = 30.0;
constexpr int kMinSpeedFeedbackTimeoutMs = 20;
constexpr int kMaxSpeedFeedbackTimeoutMs = 500;
constexpr int kMinSpeedPidMaxDtMs = 20;
constexpr int kMaxSpeedPidMaxDtMs = 200;
constexpr double kMaxSpeedPidGain = 100.0;
constexpr double kMaxSpeedPidDerivativeFilterTauMs = 2000.0;
constexpr double kMaxHardOverspeedMarginKph = 36.0;

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

double vehicle_normalized_brake_request(
    double session_normalized_brake,
    double session_max_brake_pressure_bar,
    double vehicle_max_brake_pressure_bar) {
  require_finite_range(
      session_normalized_brake, 0.0, 1.0, "session_normalized_brake");
  require_finite_range(
      vehicle_max_brake_pressure_bar,
      0.0,
      kMaxOrdinaryBrakePressureBar,
      "vehicle_max_brake_pressure_bar");
  require_finite_range(
      session_max_brake_pressure_bar,
      0.0,
      vehicle_max_brake_pressure_bar,
      "session_max_brake_pressure_bar");
  return vehicle_max_brake_pressure_bar > 0.0
      ? session_normalized_brake * session_max_brake_pressure_bar /
          vehicle_max_brake_pressure_bar
      : 0.0;
}

void normalize_and_validate_deceleration_profile(
    std::vector<DecelerationStage>& profile) {
  constexpr double kBrakeComparisonTolerance = 1e-9;
  if (profile.empty()) {
    throw std::invalid_argument("deceleration profile must not be empty");
  }
  if (profile.front().after_ms != 0) {
    throw std::invalid_argument(
        "deceleration profile must start at after_ms 0");
  }
  double previous_brake = 0.0;
  int previous_after_ms = -1;
  for (auto& stage : profile) {
    if (stage.after_ms < 0) {
      throw std::invalid_argument(
          "deceleration after_ms must be non-negative");
    }
    if (previous_after_ms >= 0 && stage.after_ms <= previous_after_ms) {
      throw std::invalid_argument(
          "deceleration after_ms values must be strictly increasing");
    }
    require_finite_range(stage.brake, 0.0, 1.0, "deceleration brake");
    if (stage.brake + kBrakeComparisonTolerance < previous_brake) {
      throw std::invalid_argument(
          "deceleration brake stages must be non-decreasing");
    }
    if (stage.brake < previous_brake) stage.brake = previous_brake;
    previous_brake = stage.brake;
    previous_after_ms = stage.after_ms;
  }
  if (std::abs(profile.back().brake - 1.0) >
      kBrakeComparisonTolerance) {
    throw std::invalid_argument(
        "deceleration profile must end with full safety brake 1.0");
  }
  profile.back().brake = 1.0;
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

struct BridgeOpenConfigV1 {
  std::uint32_t struct_size;
  const char* can_interface;
  double full_scale_motor_torque_nm;
};

struct BridgeOpenConfigV2 {
  std::uint32_t struct_size;
  const char* can_interface;
  double full_scale_motor_torque_nm;
  double hard_speed_limit_mps;
  std::int32_t control_timeout_ms;
  std::int32_t speed_feedback_timeout_ms;
  double speed_pid_kp;
  double speed_pid_ki;
  double speed_pid_kd;
  double speed_pid_derivative_filter_tau_ms;
  std::int32_t speed_pid_max_dt_ms;
  double hard_overspeed_margin_mps;
};

struct BridgeOpenConfigV3 {
  std::uint32_t struct_size;
  const char* can_interface;
  double full_scale_motor_torque_nm;
  double hard_speed_limit_mps;
  std::int32_t control_timeout_ms;
  std::int32_t speed_feedback_timeout_ms;
  double speed_pid_kp;
  double speed_pid_ki;
  double speed_pid_kd;
  double speed_pid_derivative_filter_tau_ms;
  std::int32_t speed_pid_max_dt_ms;
  double hard_overspeed_margin_mps;
  double max_ordinary_brake_pressure_bar;
};

struct BridgeOpenConfigV4 {
  std::uint32_t struct_size;
  const char* can_interface;
  double full_scale_motor_torque_nm;
  double hard_speed_limit_mps;
  std::int32_t control_timeout_ms;
  std::int32_t speed_feedback_timeout_ms;
  double speed_pid_kp;
  double speed_pid_ki;
  double speed_pid_kd;
  double speed_pid_derivative_filter_tau_ms;
  std::int32_t speed_pid_max_dt_ms;
  double hard_overspeed_margin_mps;
  double max_ordinary_brake_pressure_bar;
  double motor_torque_rise_rate_nm_per_s;
};

struct BridgeApplyResultV1 {
  std::uint32_t struct_size;
  std::int32_t result_code;
  std::uint32_t issue_id;
  std::uint32_t reserved;
};

struct BridgeRuntimeControlConfigV1 {
  std::uint32_t struct_size;
  std::uint32_t profile_version;
  std::uint64_t profile_revision;
  double target_speed_limit_mps;
  double max_motor_torque_nm;
  double max_brake_pressure_bar;
  double max_steering_request;
  double speed_pid_kp;
  double speed_pid_ki;
  double speed_pid_kd;
  double speed_pid_derivative_filter_tau_ms;
  std::int32_t speed_pid_max_dt_ms;
  std::uint32_t reserved;
  double motor_torque_rise_rate_nm_per_s;
};

struct BridgeRuntimeControlResultV1 {
  std::uint32_t struct_size;
  std::int32_t result_code;
  std::uint32_t issue_id;
  std::uint32_t reserved;
  std::uint64_t applied_revision;
};

static_assert(sizeof(BridgeApplyResultV1) == 16U);
static_assert(offsetof(BridgeRuntimeControlConfigV1, reserved) == 84U);
static_assert(sizeof(BridgeRuntimeControlConfigV1) == 96U);
static_assert(sizeof(BridgeRuntimeControlResultV1) == 24U);

constexpr std::uint32_t kBridgeApplyIssueNone = 0U;
constexpr std::uint32_t kBridgeApplyIssueDriveGearChangeMovingOrStale = 5U;

std::string bridge_apply_issue_code(std::uint32_t issue_id) {
  return issue_id == kBridgeApplyIssueDriveGearChangeMovingOrStale
      ? "vcu_drive_gear_change_moving_or_stale"
      : "vcu_control_apply_rejected";
}

#define MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(field) \
  static_assert(offsetof(BridgeOpenConfigV3, field) == \
      offsetof(BridgeOpenConfigV2, field))
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(struct_size);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(can_interface);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(full_scale_motor_torque_nm);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(hard_speed_limit_mps);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(control_timeout_ms);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(speed_feedback_timeout_ms);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(speed_pid_kp);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(speed_pid_ki);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(speed_pid_kd);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(speed_pid_derivative_filter_tau_ms);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(speed_pid_max_dt_ms);
MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD(hard_overspeed_margin_mps);
#undef MINE_TELEOP_ASSERT_BRIDGE_V3_PREFIX_FIELD
static_assert(
    offsetof(BridgeOpenConfigV3, max_ordinary_brake_pressure_bar) ==
    sizeof(BridgeOpenConfigV2));
static_assert(
    sizeof(BridgeOpenConfigV3) == sizeof(BridgeOpenConfigV2) + sizeof(double));
#define MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(field) \
  static_assert(offsetof(BridgeOpenConfigV4, field) == \
      offsetof(BridgeOpenConfigV3, field))
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(struct_size);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(can_interface);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(full_scale_motor_torque_nm);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(hard_speed_limit_mps);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(control_timeout_ms);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(speed_feedback_timeout_ms);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(speed_pid_kp);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(speed_pid_ki);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(speed_pid_kd);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(speed_pid_derivative_filter_tau_ms);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(speed_pid_max_dt_ms);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(hard_overspeed_margin_mps);
MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD(max_ordinary_brake_pressure_bar);
#undef MINE_TELEOP_ASSERT_BRIDGE_V4_PREFIX_FIELD
static_assert(
    offsetof(BridgeOpenConfigV4, motor_torque_rise_rate_nm_per_s) ==
    sizeof(BridgeOpenConfigV3));
static_assert(
    sizeof(BridgeOpenConfigV4) == sizeof(BridgeOpenConfigV3) + sizeof(double));

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
  int driver_gear_request;
  int driver_gear_request_valid;
};

struct BridgeHandshakeStatus {
  int state;
  int requested;
  int ready;
  int disarming;
  int parking_ready;
  int driver_gear_request;
  int driver_gear_request_valid;
  int handshake_status;
  int handshake_valid;
  int epb_status[4];
  int epb_valid[4];
  double speed_mps;
  int speed_valid;
};

struct BridgeCanFeedbackV1 {
  int feedback_fresh;
  long long max_feedback_age_ms;
  double speed_mps;
  int speed_valid;
  int gear;
  int gear_valid;
  int emergency_switch;
  int driver_gear_request;
  int driver_gear_request_valid;
  int handshake_status;
  int handshake_valid;
  int epb_status[4];
  int epb_valid[4];
  int motor_mode[8];
  int motor_mode_valid[8];
  double motor_torque_nm[8];
  int motor_torque_valid[8];
  double motor_speed_rpm[8];
  int motor_speed_valid[8];
  int steering_mode[4];
  int steering_valid[4];
  double steering_angle_deg[4];
  int brake_mode[8];
  int brake_valid[8];
  double brake_pressure_bar[8];
};

std::string bridge_handshake_state(int state) {
  static constexpr std::array<std::string_view, 14> kStates{
      "standby",
      "initial",
      "wait_parallel_handshake",
      "wait_parking_brake_released",
      "wait_gear",
      "wait_actuator_modes",
      "ready",
      "disarm_torque",
      "disarm_stop",
      "disarm_neutral",
      "disarm_parking_brake",
      "disarm_manual",
      "disarmed",
      "fault",
  };
  if (state < 0 || static_cast<std::size_t>(state) >= kStates.size()) {
    return "unknown";
  }
  return std::string(kStates[static_cast<std::size_t>(state)]);
}

template <typename Function>
Function load_symbol(void* handle, const char* name) {
#if defined(_WIN32)
  const auto symbol = GetProcAddress(static_cast<HMODULE>(handle), name);
  if (symbol == nullptr) {
    throw std::runtime_error(
        std::string("dynamic library is missing required symbol ") + name +
        " (Windows error " + std::to_string(GetLastError()) + ")");
  }
  return reinterpret_cast<Function>(symbol);
#else
  dlerror();
  void* symbol = dlsym(handle, name);
  const char* error = dlerror();
  if (error != nullptr || symbol == nullptr) {
    throw std::runtime_error(std::string("dynamic library is missing required symbol ") + name +
                             (error == nullptr ? "" : std::string(": ") + error));
  }
  return reinterpret_cast<Function>(symbol);
#endif
}

template <typename Function>
Function load_optional_symbol(void* handle, const char* name) noexcept {
#if defined(_WIN32)
  return reinterpret_cast<Function>(
      GetProcAddress(static_cast<HMODULE>(handle), name));
#else
  dlerror();
  void* symbol = dlsym(handle, name);
  if (dlerror() != nullptr) return nullptr;
  return reinterpret_cast<Function>(symbol);
#endif
}

void unload_dynamic_library(void* handle) {
  if (handle == nullptr) return;
#if defined(_WIN32)
  FreeLibrary(static_cast<HMODULE>(handle));
#else
  dlclose(handle);
#endif
}

void* open_dynamic_library(const std::filesystem::path& path) {
#if defined(_WIN32)
  void* handle = static_cast<void*>(LoadLibraryW(path.wstring().c_str()));
  if (handle == nullptr) {
    throw std::runtime_error(
        "failed to load dynamic library (Windows error " +
        std::to_string(GetLastError()) + ")");
  }
#else
  void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle == nullptr) {
    throw std::runtime_error(
        std::string("failed to load dynamic library: ") + dlerror());
  }
#endif
  return handle;
}

void validate_chassis_bridge_abi_handle(void* handle) {
  using QueryFn = std::uint32_t (*)();
  using OpenV1Fn = int (*)(const BridgeOpenConfigV1*);
  using OpenV2Fn = int (*)(const BridgeOpenConfigV2*);
  using OpenV3Fn = int (*)(const BridgeOpenConfigV3*);
  using OpenV4Fn = int (*)(const BridgeOpenConfigV4*);
  using ApplyV2Fn = int (*)(
      int,
      double,
      double,
      const double*,
      int,
      BridgeApplyResultV1*);
  using ConfigureRuntimeControlFn = int (*)(
      const BridgeRuntimeControlConfigV1*,
      BridgeRuntimeControlResultV1*);
  using ClearRuntimeControlFn = int (*)(BridgeRuntimeControlResultV1*);
  const auto version = load_symbol<QueryFn>(
      handle, "mine_teleop_chassis_abi_version")();
  const auto config_size = load_symbol<QueryFn>(
      handle, "mine_teleop_chassis_open_config_v4_size")();
  const auto legacy_v3_config_size = load_symbol<QueryFn>(
      handle, "mine_teleop_chassis_open_config_v3_size")();
  const auto legacy_v2_config_size = load_symbol<QueryFn>(
      handle, "mine_teleop_chassis_open_config_v2_size")();
  const auto runtime_control_config_size = load_symbol<QueryFn>(
      handle, "mine_teleop_chassis_runtime_control_config_v1_size")();
  static_cast<void>(load_symbol<OpenV1Fn>(
      handle, "mine_teleop_chassis_open_v1"));
  static_cast<void>(load_symbol<OpenV2Fn>(
      handle, "mine_teleop_chassis_open_v2"));
  static_cast<void>(load_symbol<OpenV3Fn>(
      handle, "mine_teleop_chassis_open_v3"));
  static_cast<void>(load_symbol<OpenV4Fn>(
      handle, "mine_teleop_chassis_open_v4"));
  static_cast<void>(load_symbol<ApplyV2Fn>(
      handle, "mine_teleop_chassis_apply_state_v2"));
  static_cast<void>(load_symbol<ConfigureRuntimeControlFn>(
      handle, "mine_teleop_chassis_configure_runtime_control_v1"));
  static_cast<void>(load_symbol<ClearRuntimeControlFn>(
      handle, "mine_teleop_chassis_clear_runtime_control_v1"));
  if (version != 4U || config_size != sizeof(BridgeOpenConfigV4) ||
      legacy_v3_config_size != sizeof(BridgeOpenConfigV3) ||
      legacy_v2_config_size != sizeof(BridgeOpenConfigV2) ||
      runtime_control_config_size != sizeof(BridgeRuntimeControlConfigV1)) {
    throw std::runtime_error(
        "chassis bridge ABI mismatch: expected version 4 and V4 config size " +
        std::to_string(sizeof(BridgeOpenConfigV4)) + ", got version " +
        std::to_string(version) + " and size " + std::to_string(config_size) +
        "; legacy V3 size expected " +
        std::to_string(sizeof(BridgeOpenConfigV3)) + ", got " +
        std::to_string(legacy_v3_config_size) +
        "; legacy V2 size expected " +
        std::to_string(sizeof(BridgeOpenConfigV2)) + ", got " +
        std::to_string(legacy_v2_config_size) +
        "; runtime control V1 size expected " +
        std::to_string(sizeof(BridgeRuntimeControlConfigV1)) + ", got " +
        std::to_string(runtime_control_config_size));
  }
}

#if defined(__linux__)

class ScopedFileDescriptor {
 public:
  explicit ScopedFileDescriptor(int value) : value_(value) {}
  ~ScopedFileDescriptor() {
    if (value_ >= 0) ::close(value_);
  }

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  [[nodiscard]] int get() const { return value_; }

 private:
  int value_;
};

std::runtime_error socketcan_system_error(
    std::string_view operation,
    std::string_view interface,
    int error_number = errno) {
  return std::runtime_error(
      "SocketCAN " + std::string(operation) + " failed for interface " +
      std::string(interface) + ": " + std::strerror(error_number) +
      " (errno " + std::to_string(error_number) + ")");
}

int socketcan_bitrate(int interface_index, std::string_view interface) {
  ScopedFileDescriptor socket_fd(
      ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE));
  if (socket_fd.get() < 0) {
    throw socketcan_system_error("netlink socket", interface);
  }

  struct LinkRequest {
    nlmsghdr header;
    ifinfomsg link;
  };
  LinkRequest request{};
  request.header.nlmsg_len = NLMSG_LENGTH(sizeof(ifinfomsg));
  request.header.nlmsg_type = RTM_GETLINK;
  request.header.nlmsg_flags = NLM_F_REQUEST;
  request.header.nlmsg_seq = 1;
  request.link.ifi_family = AF_UNSPEC;
  request.link.ifi_index = interface_index;

  sockaddr_nl kernel{};
  kernel.nl_family = AF_NETLINK;
  if (::sendto(
          socket_fd.get(),
          &request,
          request.header.nlmsg_len,
          0,
          reinterpret_cast<const sockaddr*>(&kernel),
          sizeof(kernel)) < 0) {
    throw socketcan_system_error("bitrate query", interface);
  }

  std::array<char, 8192> response{};
  while (true) {
    const ssize_t received = ::recv(socket_fd.get(), response.data(), response.size(), 0);
    if (received < 0) {
      if (errno == EINTR) continue;
      throw socketcan_system_error("bitrate response", interface);
    }
    if (received == 0) {
      throw std::runtime_error(
          "SocketCAN bitrate query returned an empty response for interface " +
          std::string(interface));
    }

    int remaining = static_cast<int>(received);
    for (auto* message = reinterpret_cast<nlmsghdr*>(response.data());
         NLMSG_OK(message, remaining);
         message = NLMSG_NEXT(message, remaining)) {
      if (message->nlmsg_seq != request.header.nlmsg_seq) continue;
      if (message->nlmsg_type == NLMSG_ERROR) {
        const auto* error = reinterpret_cast<const nlmsgerr*>(NLMSG_DATA(message));
        if (error->error == 0) continue;
        throw socketcan_system_error("bitrate query", interface, -error->error);
      }
      if (message->nlmsg_type == NLMSG_DONE) {
        throw std::runtime_error(
            "SocketCAN bitrate is unavailable for interface " +
            std::string(interface));
      }
      if (message->nlmsg_type != RTM_NEWLINK) continue;

      const auto* link = reinterpret_cast<const ifinfomsg*>(NLMSG_DATA(message));
      if (link->ifi_index != interface_index) continue;

      std::string link_kind;
      std::optional<int> bitrate;
      int attributes_length = IFLA_PAYLOAD(message);
      for (auto* attribute = IFLA_RTA(link);
           RTA_OK(attribute, attributes_length);
           attribute = RTA_NEXT(attribute, attributes_length)) {
        if (attribute->rta_type != IFLA_LINKINFO) continue;
        int link_info_length = RTA_PAYLOAD(attribute);
        for (auto* link_info = reinterpret_cast<rtattr*>(RTA_DATA(attribute));
             RTA_OK(link_info, link_info_length);
             link_info = RTA_NEXT(link_info, link_info_length)) {
          if (link_info->rta_type == IFLA_INFO_KIND) {
            const auto* begin = static_cast<const char*>(RTA_DATA(link_info));
            const auto* end = begin + RTA_PAYLOAD(link_info);
            link_kind.assign(begin, std::find(begin, end, '\0'));
          } else if (link_info->rta_type == IFLA_INFO_DATA) {
            int can_info_length = RTA_PAYLOAD(link_info);
            for (auto* can_info = reinterpret_cast<rtattr*>(RTA_DATA(link_info));
                 RTA_OK(can_info, can_info_length);
                 can_info = RTA_NEXT(can_info, can_info_length)) {
              if (can_info->rta_type != IFLA_CAN_BITTIMING ||
                  RTA_PAYLOAD(can_info) < static_cast<int>(sizeof(can_bittiming))) {
                continue;
              }
              can_bittiming bit_timing{};
              std::memcpy(&bit_timing, RTA_DATA(can_info), sizeof(bit_timing));
              bitrate = static_cast<int>(bit_timing.bitrate);
            }
          }
        }
      }
      if (link_kind != "can") {
        throw std::runtime_error(
            "configured SocketCAN interface " + std::string(interface) +
            " is not a CAN network device");
      }
      if (!bitrate || *bitrate <= 0) {
        throw std::runtime_error(
            "SocketCAN bitrate is unavailable for interface " +
            std::string(interface));
      }
      return *bitrate;
    }
  }
}

ifreq socketcan_interface_request(
    int socket_fd,
    std::string_view interface,
    unsigned long operation,
    std::string_view operation_name) {
  ifreq request{};
  std::memcpy(request.ifr_name, interface.data(), interface.size());
  request.ifr_name[interface.size()] = '\0';
  if (::ioctl(socket_fd, operation, &request) < 0) {
    throw socketcan_system_error(operation_name, interface);
  }
  return request;
}

void prepare_socketcan(
    std::string_view interface,
    int configured_bitrate,
    int configured_tx_queue_length) {
  if (interface.empty() || interface.size() >= IFNAMSIZ) {
    throw std::runtime_error("configured SocketCAN interface name is invalid");
  }
  ScopedFileDescriptor socket_fd(
      ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0));
  if (socket_fd.get() < 0) {
    throw socketcan_system_error("control socket", interface);
  }

  const auto index_request =
      socketcan_interface_request(socket_fd.get(), interface, SIOCGIFINDEX, "interface lookup");
  const auto flags_request =
      socketcan_interface_request(socket_fd.get(), interface, SIOCGIFFLAGS, "state query");
  if ((flags_request.ifr_flags & IFF_UP) == 0) {
    throw std::runtime_error(
        "configured SocketCAN interface " + std::string(interface) +
        " is down; bring it up with bitrate " +
        std::to_string(configured_bitrate) + " before starting the vehicle runtime");
  }

  const int actual_bitrate = socketcan_bitrate(index_request.ifr_ifindex, interface);
  if (actual_bitrate != configured_bitrate) {
    throw std::runtime_error(
        "SocketCAN bitrate mismatch for interface " + std::string(interface) +
        ": configured " + std::to_string(configured_bitrate) +
        ", actual " + std::to_string(actual_bitrate));
  }

  auto queue_request =
      socketcan_interface_request(socket_fd.get(), interface, SIOCGIFTXQLEN, "tx queue query");
  if (queue_request.ifr_qlen != configured_tx_queue_length) {
    queue_request.ifr_qlen = configured_tx_queue_length;
    if (::ioctl(socket_fd.get(), SIOCSIFTXQLEN, &queue_request) < 0) {
      throw socketcan_system_error("tx queue configuration", interface);
    }
    queue_request =
        socketcan_interface_request(socket_fd.get(), interface, SIOCGIFTXQLEN, "tx queue verification");
    if (queue_request.ifr_qlen != configured_tx_queue_length) {
      throw std::runtime_error(
          "SocketCAN tx queue verification failed for interface " +
          std::string(interface) + ": configured " +
          std::to_string(configured_tx_queue_length) + ", actual " +
          std::to_string(queue_request.ifr_qlen));
    }
  }
}

#endif

}  // namespace

VehicleAdapterControlRejected::VehicleAdapterControlRejected(
    std::string issue_code,
    int result_code)
    : std::runtime_error(
          "vehicle adapter rejected control with code " +
          std::to_string(result_code)),
      issue_code_(
          issue_code == "vcu_drive_gear_change_moving_or_stale"
              ? std::move(issue_code)
              : std::string("vcu_control_apply_rejected")),
      result_code_(result_code) {}

void validate_chassis_bridge_abi(const std::filesystem::path& library_path) {
  if (library_path.empty()) {
    throw std::invalid_argument("chassis bridge library path is empty");
  }
  void* handle = open_dynamic_library(library_path);
  try {
    validate_chassis_bridge_abi_handle(handle);
  } catch (...) {
    unload_dynamic_library(handle);
    throw;
  }
  unload_dynamic_library(handle);
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string_view to_string(SessionState state) {
  switch (state) {
    case SessionState::Offline:
      return "offline";
    case SessionState::Online:
      return "online";
    case SessionState::Reserved:
      return "reserved";
    case SessionState::Connecting:
      return "connecting";
    case SessionState::Active:
      return "active";
    case SessionState::Degraded:
      return "degraded";
    case SessionState::Stopping:
      return "stopping";
    case SessionState::Closed:
      return "closed";
  }
  throw std::invalid_argument("unknown session state");
}

SessionState session_state_from_string(std::string_view value) {
  if (value == "offline") return SessionState::Offline;
  if (value == "online") return SessionState::Online;
  if (value == "reserved") return SessionState::Reserved;
  if (value == "connecting") return SessionState::Connecting;
  if (value == "active") return SessionState::Active;
  if (value == "degraded") return SessionState::Degraded;
  if (value == "stopping") return SessionState::Stopping;
  if (value == "closed") return SessionState::Closed;
  throw std::invalid_argument("unsupported session state");
}

void ProtocolMetadata::validate() const {
  if (protocol_version != kProtocolVersion) throw std::invalid_argument("unsupported protocol_version");
  if (vehicle_id.empty()) throw std::invalid_argument("vehicle_id is required");
  if (driver_id.empty()) throw std::invalid_argument("driver_id is required");
  if (session_id.empty()) throw std::invalid_argument("session_id is required");
  if (seq == 0) throw std::invalid_argument("seq must be positive");
  if (sent_at_utc_ms < 0) throw std::invalid_argument("sent_at_utc_ms must be non-negative");
}

Json ProtocolMetadata::to_json() const {
  return {
      {"protocol_version", protocol_version},
      {"vehicle_id", vehicle_id},
      {"driver_id", driver_id},
      {"session_id", session_id},
      {"seq", seq},
      {"sent_at_utc_ms", sent_at_utc_ms},
  };
}

ProtocolMetadata ProtocolMetadata::from_json(const Json& value) {
  if (!value.is_object()) throw std::invalid_argument("protocol message must be a JSON object");
  ProtocolMetadata metadata;
  try {
    metadata.protocol_version = value.at("protocol_version").get<int>();
    metadata.vehicle_id = value.at("vehicle_id").get<std::string>();
    metadata.driver_id = value.at("driver_id").get<std::string>();
    metadata.session_id = value.at("session_id").get<std::string>();
    metadata.seq = value.at("seq").get<std::uint64_t>();
    metadata.sent_at_utc_ms = value.at("sent_at_utc_ms").get<std::int64_t>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid protocol metadata: ") + error.what());
  }
  metadata.validate();
  return metadata;
}

void ControlCommand::validate() const {
  ProtocolMetadata{protocol_version, vehicle_id, driver_id, session_id, seq, sent_at_utc_ms}.validate();
  if (control_token.empty()) {
    throw std::invalid_argument("control_token is required");
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
  auto value = ProtocolMetadata{protocol_version, vehicle_id, driver_id, session_id, seq, sent_at_utc_ms}.to_json();
  value["type"] = "control_command";
  value["gear"] = gear;
  value["steering"] = steering;
  value["throttle"] = throttle;
  value["brake"] = brake;
  value["estop"] = estop;
  value["control_token"] = control_token;
  return value;
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
    const auto metadata = ProtocolMetadata::from_json(value);
    command.protocol_version = metadata.protocol_version;
    command.vehicle_id = metadata.vehicle_id;
    command.driver_id = metadata.driver_id;
    command.session_id = metadata.session_id;
    command.seq = metadata.seq;
    command.sent_at_utc_ms = metadata.sent_at_utc_ms;
    command.gear = value.at("gear").get<std::string>();
    command.steering = value.at("steering").get<double>();
    command.throttle = value.at("throttle").get<double>();
    command.brake = value.at("brake").get<double>();
    command.estop = value.value("estop", false);
    command.control_token = value.at("control_token").get<std::string>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(std::string("invalid control command: ") + error.what());
  }
  command.validate();
  return command;
}

void SessionControlProfile::validate() const {
  if (profile_version != kSessionControlProfileVersion) {
    throw std::invalid_argument("session control profile version must be 3");
  }
  require_finite_range(target_speed_kph, 0.0, kChassisControlMaxTargetSpeedKph, "target_speed_kph");
  require_finite_range(
      max_motor_torque_nm,
      0.0,
      kMaxFullScaleMotorTorqueNm,
      "max_motor_torque_nm");
  require_finite_range(
      max_brake_pressure_bar,
      0.0,
      kMaxOrdinaryBrakePressureBar,
      "max_brake_pressure_bar");
  require_finite_range(
      service_brake_pressure_bar,
      0.0,
      kMaxOrdinaryBrakePressureBar,
      "service_brake_pressure_bar");
  require_finite_range(
      hard_brake_pressure_bar,
      0.0,
      kMaxOrdinaryBrakePressureBar,
      "hard_brake_pressure_bar");
  if (service_brake_pressure_bar > hard_brake_pressure_bar ||
      hard_brake_pressure_bar > max_brake_pressure_bar) {
    throw std::invalid_argument(
        "brake pressure must satisfy service <= hard <= max");
  }
  require_finite_range(
      max_steering_angle_deg,
      0.0,
      kChassisControlMaxSteeringAngleDeg,
      "max_steering_angle_deg");
  require_finite_range(speed_pid_kp, 0.0, kMaxSpeedPidGain, "speed_pid_kp");
  if (speed_pid_kp <= 0.0) {
    throw std::invalid_argument("speed_pid_kp must be positive");
  }
  require_finite_range(speed_pid_ki, 0.0, kMaxSpeedPidGain, "speed_pid_ki");
  require_finite_range(speed_pid_kd, 0.0, kMaxSpeedPidGain, "speed_pid_kd");
  require_finite_range(
      speed_pid_derivative_filter_tau_ms,
      0.0,
      kMaxSpeedPidDerivativeFilterTauMs,
      "speed_pid_derivative_filter_tau_ms");
  if (speed_pid_max_dt_ms < kMinSpeedPidMaxDtMs ||
      speed_pid_max_dt_ms > kMaxSpeedPidMaxDtMs) {
    throw std::invalid_argument("speed_pid_max_dt_ms must be in [20, 200]");
  }
  require_finite_range(
      motor_torque_rise_rate_nm_per_s,
      0.0,
      kMaxMotorTorqueRiseRateNmPerSecond,
      "motor_torque_rise_rate_nm_per_s");
}

Json SessionControlProfile::to_json() const {
  validate();
  return {
      {"profile_version", profile_version},
      {"target_speed_kph", target_speed_kph},
      {"max_motor_torque_nm", max_motor_torque_nm},
      {"max_brake_pressure_bar", max_brake_pressure_bar},
      {"service_brake_pressure_bar", service_brake_pressure_bar},
      {"hard_brake_pressure_bar", hard_brake_pressure_bar},
      {"max_steering_angle_deg", max_steering_angle_deg},
      {"speed_pid_kp", speed_pid_kp},
      {"speed_pid_ki", speed_pid_ki},
      {"speed_pid_kd", speed_pid_kd},
      {"speed_pid_derivative_filter_tau_ms", speed_pid_derivative_filter_tau_ms},
      {"speed_pid_max_dt_ms", speed_pid_max_dt_ms},
      {"motor_torque_rise_rate_nm_per_s", motor_torque_rise_rate_nm_per_s},
  };
}

SessionControlProfile SessionControlProfile::from_json(const Json& value) {
  if (!value.is_object()) {
    throw std::invalid_argument("session control profile must be a JSON object");
  }
  SessionControlProfile profile;
  try {
    profile.profile_version = value.at("profile_version").get<int>();
    profile.target_speed_kph = value.at("target_speed_kph").get<double>();
    profile.max_motor_torque_nm = value.at("max_motor_torque_nm").get<double>();
    profile.max_brake_pressure_bar =
        value.at("max_brake_pressure_bar").get<double>();
    profile.service_brake_pressure_bar =
        value.at("service_brake_pressure_bar").get<double>();
    profile.hard_brake_pressure_bar =
        value.at("hard_brake_pressure_bar").get<double>();
    profile.max_steering_angle_deg =
        value.at("max_steering_angle_deg").get<double>();
    profile.speed_pid_kp = value.at("speed_pid_kp").get<double>();
    profile.speed_pid_ki = value.at("speed_pid_ki").get<double>();
    profile.speed_pid_kd = value.at("speed_pid_kd").get<double>();
    profile.speed_pid_derivative_filter_tau_ms =
        value.at("speed_pid_derivative_filter_tau_ms").get<double>();
    profile.speed_pid_max_dt_ms =
        value.at("speed_pid_max_dt_ms").get<int>();
    profile.motor_torque_rise_rate_nm_per_s =
        value.at("motor_torque_rise_rate_nm_per_s").get<double>();
  } catch (const Json::exception& error) {
    throw std::invalid_argument(
        std::string("invalid session control profile: ") + error.what());
  }
  profile.validate();
  return profile;
}

void SessionControlProfileRequest::validate() const {
  ProtocolMetadata{
      protocol_version, vehicle_id, driver_id, session_id, seq, sent_at_utc_ms}
      .validate();
  if (control_token.empty()) {
    throw std::invalid_argument("control_token is required");
  }
  profile.validate();
}

Json SessionControlProfileRequest::to_json() const {
  validate();
  auto value = ProtocolMetadata{
                   protocol_version,
                   vehicle_id,
                   driver_id,
                   session_id,
                   seq,
                   sent_at_utc_ms}
                   .to_json();
  value["type"] = "session_control_profile";
  value["control_token"] = control_token;
  const auto profile_json = profile.to_json();
  for (const auto& [key, field] : profile_json.items()) value[key] = field;
  return value;
}

SessionControlProfileRequest SessionControlProfileRequest::from_json(
    const Json& value) {
  if (!value.is_object() ||
      value.value("type", "") != "session_control_profile") {
    throw std::invalid_argument(
        "type must be session_control_profile");
  }
  SessionControlProfileRequest request;
  try {
    const auto metadata = ProtocolMetadata::from_json(value);
    request.protocol_version = metadata.protocol_version;
    request.vehicle_id = metadata.vehicle_id;
    request.driver_id = metadata.driver_id;
    request.session_id = metadata.session_id;
    request.seq = metadata.seq;
    request.sent_at_utc_ms = metadata.sent_at_utc_ms;
    request.control_token = value.at("control_token").get<std::string>();
    request.profile = SessionControlProfile::from_json(value);
  } catch (const Json::exception& error) {
    throw std::invalid_argument(
        std::string("invalid session control profile request: ") + error.what());
  }
  request.validate();
  return request;
}

void SessionControlProfileResult::validate() const {
  ProtocolMetadata{
      protocol_version, vehicle_id, driver_id, session_id, seq, sent_at_utc_ms}
      .validate();
  if (reason.empty()) throw std::invalid_argument("profile result reason is required");
  if (effective_profile) effective_profile->validate();
  if (accepted && !effective_profile) {
    throw std::invalid_argument("accepted profile result requires an effective profile");
  }
  if (accepted && applied_revision != seq) {
    throw std::invalid_argument(
        "accepted profile result requires applied_revision equal to seq");
  }
  if (!accepted && applied_revision != 0) {
    throw std::invalid_argument(
        "rejected profile result requires applied_revision zero");
  }
}

Json SessionControlProfileResult::to_json() const {
  validate();
  auto value = ProtocolMetadata{
                   protocol_version,
                   vehicle_id,
                   driver_id,
                   session_id,
                   seq,
                   sent_at_utc_ms}
                   .to_json();
  value["event"] = "session_control_profile_result";
  value["last_request_seq"] = seq;
  value["active"] = effective_profile.has_value();
  value["accepted"] = accepted;
  value["idempotent"] = idempotent;
  value["applied_revision"] = applied_revision;
  value["reason"] = reason;
  value["effective_profile"] = effective_profile
      ? effective_profile->to_json()
      : Json(nullptr);
  return value;
}

SessionControlProfileResult SessionControlProfileResult::from_json(
    const Json& value) {
  if (!value.is_object() ||
      value.value("event", "") != "session_control_profile_result") {
    throw std::invalid_argument("event must be session_control_profile_result");
  }
  SessionControlProfileResult result;
  std::uint64_t last_request_seq = 0;
  bool active = false;
  try {
    const auto metadata = ProtocolMetadata::from_json(value);
    result.protocol_version = metadata.protocol_version;
    result.vehicle_id = metadata.vehicle_id;
    result.driver_id = metadata.driver_id;
    result.session_id = metadata.session_id;
    result.seq = metadata.seq;
    result.sent_at_utc_ms = metadata.sent_at_utc_ms;
    last_request_seq = value.at("last_request_seq").get<std::uint64_t>();
    active = value.at("active").get<bool>();
    result.accepted = value.at("accepted").get<bool>();
    result.idempotent = value.at("idempotent").get<bool>();
    result.applied_revision =
        value.at("applied_revision").get<std::uint64_t>();
    result.reason = value.at("reason").get<std::string>();
    if (!value.at("effective_profile").is_null()) {
      result.effective_profile =
          SessionControlProfile::from_json(value.at("effective_profile"));
    }
  } catch (const Json::exception& error) {
    throw std::invalid_argument(
        std::string("invalid session control profile result: ") + error.what());
  }
  if (last_request_seq != result.seq) {
    throw std::invalid_argument(
        "session control profile result last_request_seq does not match seq");
  }
  if (active != result.effective_profile.has_value()) {
    throw std::invalid_argument(
        "session control profile result active flag does not match effective_profile");
  }
  result.validate();
  return result;
}

double dynamic_adapter_target_speed_mps(
    const ControlCommand& command,
    double max_speed_mps) {
  command.validate();
  if (!std::isfinite(max_speed_mps) || max_speed_mps < 0.0) {
    throw std::invalid_argument("max_speed_mps must be finite and non-negative");
  }
  const bool driving_gear = command.gear == "D" || command.gear == "R";
  // This is only the local PID setpoint; it is never forwarded as a VCU
  // vehicle-speed request. Analog throttle selects a proportional target
  // speed, while the independent hard limit is passed in the current
  // versioned open config.
  return driving_gear && command.brake == 0.0
      ? std::clamp(command.throttle, 0.0, 1.0) * max_speed_mps
      : 0.0;
}

double dynamic_adapter_target_acceleration(
    const ControlCommand& command,
    double traction_ceiling,
    double session_max_brake_pressure_bar,
    double vehicle_max_brake_pressure_bar) {
  command.validate();
  require_finite_range(traction_ceiling, 0.0, 1.0, "traction_ceiling");
  // Treat braking as an independent, dominant request. A malformed or stale
  // producer must never turn simultaneous throttle and brake into traction.
  const double brake_ratio = vehicle_normalized_brake_request(
      command.brake,
      session_max_brake_pressure_bar,
      vehicle_max_brake_pressure_bar);
  return command.brake > 0.0
      ? -brake_ratio
      : (command.throttle > 0.0 ? traction_ceiling : 0.0);
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
    std::string driver_id,
    std::string session_id,
    int max_command_gap_ms,
    int protocol_version,
    bool control_authority,
    std::string control_token,
    int timestamp_warning_skew_ms)
    : vehicle_id_(std::move(vehicle_id)),
      driver_id_(std::move(driver_id)),
      session_id_(std::move(session_id)),
      max_command_gap_ms_(max_command_gap_ms),
      protocol_version_(protocol_version),
      control_authority_(control_authority),
      control_token_(std::move(control_token)),
      timestamp_warning_skew_ms_(timestamp_warning_skew_ms) {
  if (vehicle_id_.empty() || driver_id_.empty() || session_id_.empty()) {
    throw std::invalid_argument("vehicle_id, driver_id, and session_id are required");
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
  if (command.driver_id != driver_id_) return {false, "wrong_driver", std::nullopt, {}};
  if (command.session_id != session_id_) return {false, "wrong_session", std::nullopt, {}};
  if (!control_authority_) return {false, "control_authority_missing", std::nullopt, {}};
  if (command.control_token != control_token_) {
    return {false, "control_token_invalid", std::nullopt, {}};
  }
  if (last_seq_ && command.seq <= *last_seq_) return {false, "old_seq", std::nullopt, {}};
  const auto timestamp_delta_ms = receive_time_ms - command.sent_at_utc_ms;
  if (!command.estop && timestamp_delta_ms > max_command_gap_ms_) {
    return {false, "command_age_exceeded", std::nullopt, {}};
  }
  if (!command.estop && timestamp_delta_ms < -max_command_gap_ms_) {
    return {false, "command_timestamp_in_future", std::nullopt, {}};
  }
  if (last_valid_receive_ms_ && receive_time_ms < *last_valid_receive_ms_) {
    return {false, "receive_time_reversed", std::nullopt, {}};
  }
  if (last_valid_receive_ms_ && receive_time_ms - *last_valid_receive_ms_ > max_command_gap_ms_ && !command.estop) {
    // Drop the first command after a gap, but re-arm timing so the next fresh
    // heartbeat can recover instead of permanently locking out control.
    last_valid_receive_ms_ = receive_time_ms;
    return {false, "command_gap_exceeded", std::nullopt, {}};
  }
  last_seq_ = command.seq;
  last_valid_receive_ms_ = receive_time_ms;
  std::vector<std::string> warnings;
  if (std::llabs(receive_time_ms - command.sent_at_utc_ms) > timestamp_warning_skew_ms_) {
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
  normalize_and_validate_deceleration_profile(profile_);
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
      {
        const double brake = brake_for_timeout(timestamp_ms);
        return {gear, 0.0, 0.0, brake, false, brake >= 1.0};
      }
    case SafetyState::Estop:
      return {gear, 0.0, 0.0, 1.0, true, true};
    case SafetyState::Fault:
      return {gear, 0.0, 0.0, 1.0, false, true};
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
  enter_standby();
  return true;
}

bool SafetyStateMachine::reset_to_standby() {
  if (state_ == SafetyState::Estop || state_ == SafetyState::Fault) return false;
  enter_standby();
  return true;
}

void SafetyStateMachine::enter_standby() {
  last_valid_command_.reset();
  last_valid_receive_ms_.reset();
  timeout_entered_ms_.reset();
  state_ = SafetyState::Standby;
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

bool ice_transport_policy_is_valid(std::string_view value) {
  return value == "all" || value == "relay";
}

Json VehicleConfig::redacted_summary() const {
  const auto enabled = enabled_cameras();
  const auto critical_camera_count = std::count_if(enabled.begin(), enabled.end(), [](const auto& camera) {
    return camera.critical_for_control;
  });
  return {
      {"event", "effective_vehicle_config"},
      {"runtime", "cpp"},
      {"vehicle_id", vehicle_id},
      {"signaling_url", cloud.signaling_url},
      {"ice_transport_policy", cloud.ice_transport_policy},
      {"device_token_file_configured", !cloud.device_token_file.empty()},
      {"control_enabled", runtime.control_enabled},
      {"media_enabled", runtime.media_enabled},
      {"teleop_poll_interval_ms", runtime.teleop_poll_interval_ms},
      {"camera_count", enabled.size()},
      {"critical_camera_count", critical_camera_count},
      {"vehicle_adapter_type", vehicle_adapter.type},
      {"can_interface", hardware.can_interface},
      {"can_bitrate", hardware.can_bitrate},
      {"can_tx_queue_length", hardware.can_tx_queue_length},
      {"max_speed_kph", field_safety.max_speed_kph},
      {"max_throttle", field_safety.max_throttle},
      {"full_scale_motor_torque_nm", field_safety.full_scale_motor_torque_nm},
      {"motor_torque_rise_rate_nm_per_s",
       field_safety.motor_torque_rise_rate_nm_per_s},
      {"speed_feedback_timeout_ms", field_safety.speed_feedback_timeout_ms},
      {"speed_pid_kp", field_safety.speed_pid_kp},
      {"speed_pid_ki", field_safety.speed_pid_ki},
      {"speed_pid_kd", field_safety.speed_pid_kd},
      {"speed_pid_derivative_filter_tau_ms", field_safety.speed_pid_derivative_filter_tau_ms},
      {"speed_pid_max_dt_ms", field_safety.speed_pid_max_dt_ms},
      {"hard_overspeed_margin_kph", field_safety.hard_overspeed_margin_kph},
      {"max_brake_pressure_bar", field_safety.max_brake_pressure_bar},
      {"max_steering_angle_deg", field_safety.max_steering_angle_deg},
      {"require_time_sync", field_safety.require_time_sync},
      {"max_time_sync_uncertainty_ms", field_safety.max_time_sync_uncertainty_ms},
      {"recording_root", recording.root_dir.string()},
      {"recording_enabled", recording.enabled},
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
  config.cloud.device_token_file = optional<std::string>(cloud, "device_token_file", "");
  config.cloud.resolve_entries = optional<std::vector<std::string>>(cloud, "resolve", {});
  config.cloud.ca_bundle = optional<std::string>(cloud, "ca_bundle", "");
  config.cloud.ice_transport_policy = optional<std::string>(cloud, "ice_transport_policy", "all");
  if (!config.cloud.device_token_file.empty() && config.cloud.device_token_file.is_relative()) {
    config.cloud.device_token_file = (path.parent_path() / config.cloud.device_token_file).lexically_normal();
  }
  if (!config.cloud.ca_bundle.empty() && config.cloud.ca_bundle.is_relative()) {
    config.cloud.ca_bundle = (path.parent_path() / config.cloud.ca_bundle).lexically_normal();
  }
  for (const auto& entry : config.cloud.resolve_entries) {
    if (entry.empty() || entry.find_first_of("\r\n") != std::string::npos) {
      throw std::runtime_error("cloud.resolve contains an invalid entry");
    }
  }
  if (!ice_transport_policy_is_valid(config.cloud.ice_transport_policy)) {
    throw std::runtime_error("cloud.ice_transport_policy must be all or relay");
  }

  const auto runtime = root["runtime"];
  config.runtime.control_enabled = optional<bool>(runtime, "control_enabled", true);
  config.runtime.media_enabled = optional<bool>(runtime, "media_enabled", true);
  config.runtime.control_log_commands = optional<bool>(runtime, "control_log_commands", false);
  config.runtime.teleop_poll_interval_ms = optional<int>(runtime, "teleop_poll_interval_ms", 50);
  config.runtime.media_frame_timeout_ms = optional<int>(runtime, "media_frame_timeout_ms", 3000);
  config.runtime.media_capture_interval_ms = optional<int>(runtime, "media_capture_interval_ms", 0);
  if (!config.runtime.control_enabled && !config.runtime.media_enabled) {
    throw std::runtime_error("runtime must enable control or media");
  }
  if (config.runtime.teleop_poll_interval_ms <= 0 || config.runtime.media_frame_timeout_ms <= 0 ||
      config.runtime.media_capture_interval_ms < 0) {
    throw std::runtime_error("runtime timing settings are invalid");
  }

  const auto control = root["control"];
  config.control.rate_hz = optional<int>(control, "rate_hz", 20);
  config.control.max_command_gap_ms = optional<int>(control, "max_command_gap_ms", 200);
  config.control.degraded_timeout_ms = optional<int>(control, "degraded_timeout_ms", 300);
  config.control.control_timeout_ms = optional<int>(control, "control_timeout_ms", 800);
  if (config.control.rate_hz != 20) {
    throw std::runtime_error(
        "control.rate_hz must be 20; the upstream command rate is fixed at 20 Hz");
  }
  if (config.control.max_command_gap_ms <= 0 ||
      config.control.max_command_gap_ms > 60000 ||
      config.control.degraded_timeout_ms <= 0 ||
      config.control.degraded_timeout_ms > 60000 ||
      config.control.control_timeout_ms <= config.control.degraded_timeout_ms ||
      config.control.control_timeout_ms > 60000) {
    throw std::runtime_error(
        "control timing requires positive values no greater than 60000 ms and degraded_timeout_ms < control_timeout_ms");
  }
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
  normalize_and_validate_deceleration_profile(
      config.control.deceleration_profile);

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
    camera.critical_for_control = optional<bool>(node, "critical_for_control", true);
    camera.reopen_attempts = optional<int>(node, "reopen_attempts", 3);
    camera.reopen_backoff_ms = optional<int>(node, "reopen_backoff_ms", 500);
    camera.backend = optional<std::string>(node, "backend", "auto");
    camera.device = required<std::string>(node, "device", camera.id);
    camera.capture_width = optional<int>(node, "capture_width", 1280);
    camera.capture_height = optional<int>(node, "capture_height", 720);
    camera.capture_fps = optional<int>(node, "capture_fps", 30);
    camera.realtime_profile = required<std::string>(node, "realtime_profile", camera.id);
    camera.record_profile = optional<std::string>(node, "record_profile", "");
    if (camera.reopen_attempts < 0 || camera.reopen_attempts > 10) {
      throw std::runtime_error(camera.id + ".reopen_attempts must be in [0, 10]");
    }
    if (camera.reopen_backoff_ms < 0 || camera.reopen_backoff_ms > 60000) {
      throw std::runtime_error(camera.id + ".reopen_backoff_ms must be in [0, 60000]");
    }
    if (camera.backend != "auto" && camera.backend != "ccg2") {
      throw std::runtime_error(camera.id + ".backend must be auto or ccg2");
    }
    if (camera.backend == "ccg2" &&
        (camera.capture_width <= 0 || camera.capture_height <= 0 || camera.capture_fps <= 0 ||
         camera.capture_width % 2 != 0)) {
      throw std::runtime_error(
          camera.id + ".capture_width must be positive and even; capture_height and capture_fps must be positive");
    }
    static_cast<void>(config.realtime_profile(camera.realtime_profile));
    if (!camera.record_profile.empty()) static_cast<void>(config.record_profile(camera.record_profile));
    config.cameras.push_back(std::move(camera));
  }
  const auto enabled_cameras = config.enabled_cameras();
  if (enabled_cameras.empty()) throw std::runtime_error("at least one camera must be enabled");
  if (config.runtime.control_enabled &&
      std::none_of(enabled_cameras.begin(), enabled_cameras.end(), [](const auto& camera) {
        return camera.critical_for_control;
      })) {
    throw std::runtime_error(
        "control-enabled vehicle requires at least one enabled camera with critical_for_control: true");
  }

  const auto hardware = root["hardware"];
  const auto can = hardware["can"];
  config.hardware.can_interface = optional<std::string>(can, "interface", "can0");
  config.hardware.can_bitrate = optional<int>(can, "bitrate", 500000);
  config.hardware.can_tx_queue_length = optional<int>(can, "tx_queue_length", 100);
  if (config.hardware.can_interface.empty() || config.hardware.can_interface.size() >= 16 ||
      config.hardware.can_interface.find_first_of("\r\n/") != std::string::npos) {
    throw std::runtime_error("hardware.can.interface is invalid");
  }
  if (config.hardware.can_bitrate <= 0 || config.hardware.can_bitrate > 1000000) {
    throw std::runtime_error("hardware.can.bitrate must be in [1, 1000000]");
  }
  if (config.hardware.can_tx_queue_length <= 0 ||
      config.hardware.can_tx_queue_length > 65535) {
    throw std::runtime_error("hardware.can.tx_queue_length must be in [1, 65535]");
  }
  const auto encoding = hardware["encoding"];
  config.hardware.vaapi_render_device = optional<std::string>(encoding, "vaapi_render_device", "/dev/dri/renderD128");
  config.hardware.dri_card_device = optional<std::string>(encoding, "dri_card_device", "/dev/dri/card1");
  config.hardware.preferred_encoder = optional<std::string>(encoding, "preferred_encoder", "nvenc");
  config.hardware.fallback_encoder = optional<std::string>(encoding, "fallback_encoder", "vaapi");
  config.hardware.preferred_codec = optional<std::string>(encoding, "preferred_codec", "h265");
  config.hardware.fallback_codec = optional<std::string>(encoding, "fallback_codec", "h264");
  config.hardware.require_hardware_encoder = optional<bool>(encoding, "require_hardware_encoder", true);
  config.hardware.max_end_to_end_latency_ms = optional<int>(encoding, "max_end_to_end_latency_ms", 200);
  config.hardware.min_realtime_fps = optional<int>(encoding, "min_realtime_fps", 20);
  if (config.hardware.preferred_encoder != "nvenc" && config.hardware.preferred_encoder != "vaapi") {
    throw std::runtime_error("hardware.encoding.preferred_encoder must be nvenc or vaapi");
  }
  if (config.hardware.fallback_encoder != "nvenc" && config.hardware.fallback_encoder != "vaapi") {
    throw std::runtime_error("hardware.encoding.fallback_encoder must be nvenc or vaapi");
  }
  if (config.hardware.preferred_codec != "h264" && config.hardware.preferred_codec != "h265") {
    throw std::runtime_error("hardware.encoding.preferred_codec must be h264 or h265");
  }
  if (config.hardware.fallback_codec != "h264" && config.hardware.fallback_codec != "h265") {
    throw std::runtime_error("hardware.encoding.fallback_codec must be h264 or h265");
  }
  if (config.hardware.max_end_to_end_latency_ms <= 0 || config.hardware.min_realtime_fps <= 0) {
    throw std::runtime_error("hardware.encoding acceptance thresholds must be positive");
  }
  config.hardware.network_interface = optional<std::string>(hardware["network"], "interface", "wwan0");

  const auto safety = root["field_safety"];
  config.field_safety.commissioning_mode = optional<std::string>(safety, "commissioning_mode", "bench");
  config.field_safety.max_speed_kph = optional<double>(safety, "max_speed_kph", 40.0);
  config.field_safety.max_throttle = optional<double>(safety, "max_throttle", 1.0);
  config.field_safety.full_scale_motor_torque_nm =
      optional<double>(
          safety,
          "full_scale_motor_torque_nm",
          kDefaultFullScaleMotorTorqueNm);
  config.field_safety.motor_torque_rise_rate_nm_per_s = optional<double>(
      safety,
      "motor_torque_rise_rate_nm_per_s",
      kDefaultMotorTorqueRiseRateNmPerSecond);
  config.field_safety.speed_feedback_timeout_ms =
      optional<int>(safety, "speed_feedback_timeout_ms", 200);
  config.field_safety.speed_pid_kp = optional<double>(safety, "speed_pid_kp", 1.0);
  config.field_safety.speed_pid_ki = optional<double>(safety, "speed_pid_ki", 0.2);
  config.field_safety.speed_pid_kd = optional<double>(safety, "speed_pid_kd", 0.0);
  config.field_safety.speed_pid_derivative_filter_tau_ms =
      optional<double>(safety, "speed_pid_derivative_filter_tau_ms", 100.0);
  config.field_safety.speed_pid_max_dt_ms =
      optional<int>(safety, "speed_pid_max_dt_ms", 100);
  config.field_safety.hard_overspeed_margin_kph =
      optional<double>(safety, "hard_overspeed_margin_kph", 3.6);
  if (safety && safety["max_brake"]) {
    throw std::runtime_error(
        "field_safety.max_brake used legacy normalized units; migrate explicitly to max_brake_pressure_bar");
  }
  config.field_safety.max_brake_pressure_bar = optional<double>(
      safety,
      "max_brake_pressure_bar",
      kDefaultMaxBrakePressureBar);
  config.field_safety.max_steering_angle_deg =
      optional<double>(safety, "max_steering_angle_deg", 30.0);
  config.field_safety.require_can_feedback_before_control =
      optional<bool>(safety, "require_can_feedback_before_control", true);
  config.field_safety.require_local_estop_reset = optional<bool>(safety, "require_local_estop_reset", true);
  config.field_safety.require_time_sync = optional<bool>(safety, "require_time_sync", true);
  config.field_safety.max_time_sync_uncertainty_ms =
      optional<int>(safety, "max_time_sync_uncertainty_ms", 25);
  config.field_safety.time_sync_interval_ms = optional<int>(safety, "time_sync_interval_ms", 30000);
  config.field_safety.time_sync_samples = optional<int>(safety, "time_sync_samples", 7);
  if (!config.field_safety.require_local_estop_reset) {
    throw std::runtime_error(
        "field_safety.require_local_estop_reset must be true; remote-only ESTOP reset is unsupported");
  }
  if (!std::isfinite(config.field_safety.full_scale_motor_torque_nm) ||
      config.field_safety.full_scale_motor_torque_nm < 0.0 ||
      config.field_safety.full_scale_motor_torque_nm >
          kMaxFullScaleMotorTorqueNm) {
    throw std::runtime_error(
        "field_safety.full_scale_motor_torque_nm must be finite and in [0, 640.0] Nm");
  }
  if (!std::isfinite(config.field_safety.motor_torque_rise_rate_nm_per_s) ||
      config.field_safety.motor_torque_rise_rate_nm_per_s < 0.0 ||
      config.field_safety.motor_torque_rise_rate_nm_per_s >
          kMaxMotorTorqueRiseRateNmPerSecond) {
    throw std::runtime_error(
        "field_safety.motor_torque_rise_rate_nm_per_s must be finite and in [0, 32000.0] Nm/s; zero disables additional rise limiting");
  }
  if (!std::isfinite(config.field_safety.max_speed_kph) ||
      config.field_safety.max_speed_kph < 0.0 ||
      config.field_safety.max_speed_kph > kChassisControlMaxTargetSpeedKph) {
    throw std::runtime_error(
        "field_safety.max_speed_kph must be finite and in [0, 72] km/h; "
        "the current ChassisControl target-speed input is limited to 20 m/s");
  }
  if (!std::isfinite(config.field_safety.max_throttle) ||
      config.field_safety.max_throttle < 0.0 ||
      config.field_safety.max_throttle > 1.0 ||
      !std::isfinite(config.field_safety.max_brake_pressure_bar) ||
      config.field_safety.max_brake_pressure_bar < 0.0 ||
      config.field_safety.max_brake_pressure_bar >
          kMaxOrdinaryBrakePressureBar ||
      !std::isfinite(config.field_safety.max_steering_angle_deg) ||
      config.field_safety.max_steering_angle_deg < 0.0 ||
      config.field_safety.max_steering_angle_deg > 30.0 ||
      config.field_safety.max_time_sync_uncertainty_ms < 0 ||
      config.field_safety.time_sync_interval_ms <= 0 ||
      config.field_safety.time_sync_samples < 3 || config.field_safety.time_sync_samples > 15) {
    throw std::runtime_error("field_safety limits or time sync settings are invalid");
  }
  if (config.field_safety.speed_feedback_timeout_ms < kMinSpeedFeedbackTimeoutMs ||
      config.field_safety.speed_feedback_timeout_ms > kMaxSpeedFeedbackTimeoutMs ||
      config.field_safety.speed_feedback_timeout_ms > config.control.control_timeout_ms ||
      !std::isfinite(config.field_safety.speed_pid_kp) ||
      config.field_safety.speed_pid_kp <= 0.0 ||
      config.field_safety.speed_pid_kp > kMaxSpeedPidGain ||
      !std::isfinite(config.field_safety.speed_pid_ki) ||
      config.field_safety.speed_pid_ki < 0.0 ||
      config.field_safety.speed_pid_ki > kMaxSpeedPidGain ||
      !std::isfinite(config.field_safety.speed_pid_kd) ||
      config.field_safety.speed_pid_kd < 0.0 ||
      config.field_safety.speed_pid_kd > kMaxSpeedPidGain ||
      !std::isfinite(config.field_safety.speed_pid_derivative_filter_tau_ms) ||
      config.field_safety.speed_pid_derivative_filter_tau_ms < 0.0 ||
      config.field_safety.speed_pid_derivative_filter_tau_ms >
          kMaxSpeedPidDerivativeFilterTauMs ||
      config.field_safety.speed_pid_max_dt_ms < kMinSpeedPidMaxDtMs ||
      config.field_safety.speed_pid_max_dt_ms > kMaxSpeedPidMaxDtMs ||
      !std::isfinite(config.field_safety.hard_overspeed_margin_kph) ||
      config.field_safety.hard_overspeed_margin_kph <= 0.0 ||
      config.field_safety.hard_overspeed_margin_kph >
          kMaxHardOverspeedMarginKph) {
    throw std::runtime_error(
        "field_safety local speed PID, feedback timeout, max dt, or hard overspeed margin is invalid");
  }

  const auto recording = root["recording"];
  config.recording.enabled = optional<bool>(recording, "enabled", false);
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
  if (config.vehicle_adapter.type != "mock" &&
      (!safety || !safety["max_speed_kph"] || !safety["max_throttle"] ||
       !safety["full_scale_motor_torque_nm"] ||
       !safety["motor_torque_rise_rate_nm_per_s"] ||
       !safety["max_brake_pressure_bar"] ||
       !safety["max_steering_angle_deg"] ||
       !safety["speed_feedback_timeout_ms"] || !safety["speed_pid_kp"] ||
       !safety["speed_pid_ki"] || !safety["speed_pid_kd"] ||
       !safety["speed_pid_derivative_filter_tau_ms"] ||
       !safety["speed_pid_max_dt_ms"] ||
       !safety["hard_overspeed_margin_kph"])) {
    throw std::runtime_error(
        "non-mock vehicle adapter requires explicit field_safety max_speed_kph, "
        "max_throttle, full_scale_motor_torque_nm, motor_torque_rise_rate_nm_per_s, "
        "max_brake_pressure_bar, steering limits, speed PID gains/timing, feedback "
        "timeout, and hard overspeed margin");
  }
  if (config.vehicle_adapter.type != "mock" &&
      config.hardware.can_tx_queue_length < 16) {
    throw std::runtime_error(
        "non-mock vehicle adapter requires hardware.can.tx_queue_length >= 16");
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
  if (can_bitrate > 0) value["can_bitrate"] = can_bitrate;
  if (can_tx_queue_length > 0) value["can_tx_queue_length"] = can_tx_queue_length;
  return value;
}

Json VehicleCanFeedback::to_json() const {
  return {
      {"supported", supported},
      {"feedback_fresh", feedback_fresh},
      {"max_feedback_age_ms", max_feedback_age_ms},
      {"speed_mps", speed_mps},
      {"speed_valid", speed_valid},
      {"gear", gear},
      {"gear_valid", gear_valid},
      {"emergency_switch", emergency_switch},
      {"driver_gear_request", driver_gear_request},
      {"driver_gear_request_valid", driver_gear_request_valid},
      {"handshake_status", handshake_status},
      {"handshake_valid", handshake_valid},
      {"parking_brake_status", parking_brake_status},
      {"parking_brake_valid", parking_brake_valid},
      {"motor_mode", motor_mode},
      {"motor_mode_valid", motor_mode_valid},
      {"motor_torque_nm", motor_torque_nm},
      {"motor_torque_valid", motor_torque_valid},
      {"motor_speed_rpm", motor_speed_rpm},
      {"motor_speed_valid", motor_speed_valid},
      {"steering_mode", steering_mode},
      {"steering_valid", steering_valid},
      {"steering_angle_deg", steering_angle_deg},
      {"brake_mode", brake_mode},
      {"brake_valid", brake_valid},
      {"brake_pressure_bar", brake_pressure_bar},
  };
}

Json VcuHandshakeStatus::to_json() const {
  return {
      {"supported", supported},
      {"state", state},
      {"requested", requested},
      {"ready", ready},
      {"disarming", disarming},
      {"parking_ready", parking_ready},
      {"driver_gear_request", driver_gear_request},
      {"driver_gear_request_valid", driver_gear_request_valid},
      {"handshake_status", handshake_status},
      {"handshake_valid", handshake_valid},
      {"epb_status", epb_status},
      {"epb_valid", epb_valid},
      {"speed_mps", speed_mps},
      {"speed_valid", speed_valid},
  };
}

void MockVehicleAdapter::open() { opened_ = true; }
void MockVehicleAdapter::close() { opened_ = false; }

std::uint64_t MockVehicleAdapter::configure_runtime_control_profile(
    const SessionControlProfile& profile,
    std::uint64_t profile_revision) {
  profile.validate();
  if (profile_revision == 0) {
    throw std::invalid_argument("profile_revision must be positive");
  }
  session_motor_torque_limit_nm_ = profile.max_motor_torque_nm;
  session_brake_pressure_limit_bar_ = profile.max_brake_pressure_bar;
  runtime_control_profile_revision_ = profile_revision;
  return runtime_control_profile_revision_;
}

void MockVehicleAdapter::clear_runtime_control_profile() {
  session_motor_torque_limit_nm_ = 0.0;
  session_brake_pressure_limit_bar_ = 0.0;
  runtime_control_profile_revision_ = 0;
}

double MockVehicleAdapter::session_motor_torque_limit_nm() const {
  return session_motor_torque_limit_nm_;
}

double MockVehicleAdapter::session_brake_pressure_limit_bar() const {
  return session_brake_pressure_limit_bar_;
}

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
  VehicleTelemetry telemetry;
  telemetry.speed_mps = latest_output_->throttle * 2.0;
  telemetry.gear = latest_output_->gear;
  telemetry.steering_feedback = latest_output_->steering;
  telemetry.throttle_feedback = latest_output_->throttle;
  telemetry.brake_feedback = latest_output_->brake;
  telemetry.estop = latest_output_->estop;
  return telemetry;
}

bool MockVehicleAdapter::poll_feedback() { return opened_; }

bool MockVehicleAdapter::request_vcu_handshake() { return false; }

bool MockVehicleAdapter::disconnect_vcu_handshake() { return false; }

bool MockVehicleAdapter::feedback_ready() const { return opened_; }

VcuHandshakeStatus MockVehicleAdapter::vcu_handshake_status() const {
  return {};
}

VehicleAdapterStatus MockVehicleAdapter::status() const {
  return {
      "mock",
      opened_,
      true,
      "",
      "",
      applied_command_count_,
      safe_stop_count_,
      "",
      opened_,
      0,
      0,
  };
}

DynamicLibraryVehicleAdapter::DynamicLibraryVehicleAdapter(
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
    double hard_overspeed_margin_mps)
    : library_path_(std::move(library_path)),
      can_interface_(std::move(can_interface)),
      can_bitrate_(can_bitrate),
      can_tx_queue_length_(can_tx_queue_length),
      max_speed_mps_(max_speed_mps),
      full_scale_motor_torque_nm_(full_scale_motor_torque_nm),
      motor_torque_rise_rate_nm_per_s_(motor_torque_rise_rate_nm_per_s),
      max_ordinary_brake_pressure_bar_(max_ordinary_brake_pressure_bar),
      session_motor_torque_limit_nm_(0.0),
      session_brake_pressure_limit_bar_(0.0),
      control_timeout_ms_(control_timeout_ms),
      speed_feedback_timeout_ms_(speed_feedback_timeout_ms),
      speed_pid_kp_(speed_pid_kp),
      speed_pid_ki_(speed_pid_ki),
      speed_pid_kd_(speed_pid_kd),
      speed_pid_derivative_filter_tau_ms_(speed_pid_derivative_filter_tau_ms),
      speed_pid_max_dt_ms_(speed_pid_max_dt_ms),
      hard_overspeed_margin_mps_(hard_overspeed_margin_mps) {
  if (library_path_.empty() || can_interface_.empty() || can_bitrate_ <= 0 ||
      can_tx_queue_length_ < 16 || !std::isfinite(max_speed_mps_) ||
      max_speed_mps_ < 0.0 ||
      max_speed_mps_ > kChassisControlMaxTargetSpeedMps ||
      !std::isfinite(full_scale_motor_torque_nm_) ||
      full_scale_motor_torque_nm_ < 0.0 ||
      full_scale_motor_torque_nm_ > kMaxFullScaleMotorTorqueNm ||
      !std::isfinite(motor_torque_rise_rate_nm_per_s_) ||
      motor_torque_rise_rate_nm_per_s_ < 0.0 ||
      motor_torque_rise_rate_nm_per_s_ >
          kMaxMotorTorqueRiseRateNmPerSecond ||
      !std::isfinite(max_ordinary_brake_pressure_bar_) ||
      max_ordinary_brake_pressure_bar_ < 0.0 ||
      max_ordinary_brake_pressure_bar_ > kMaxOrdinaryBrakePressureBar ||
      control_timeout_ms_ < 20 || control_timeout_ms_ > 60000 ||
      speed_feedback_timeout_ms_ < kMinSpeedFeedbackTimeoutMs ||
      speed_feedback_timeout_ms_ > kMaxSpeedFeedbackTimeoutMs ||
      speed_feedback_timeout_ms_ > control_timeout_ms_ ||
      !std::isfinite(speed_pid_kp_) || speed_pid_kp_ <= 0.0 ||
      speed_pid_kp_ > kMaxSpeedPidGain ||
      !std::isfinite(speed_pid_ki_) || speed_pid_ki_ < 0.0 ||
      speed_pid_ki_ > kMaxSpeedPidGain ||
      !std::isfinite(speed_pid_kd_) || speed_pid_kd_ < 0.0 ||
      speed_pid_kd_ > kMaxSpeedPidGain ||
      !std::isfinite(speed_pid_derivative_filter_tau_ms_) ||
      speed_pid_derivative_filter_tau_ms_ < 0.0 ||
      speed_pid_derivative_filter_tau_ms_ > kMaxSpeedPidDerivativeFilterTauMs ||
      speed_pid_max_dt_ms_ < kMinSpeedPidMaxDtMs ||
      speed_pid_max_dt_ms_ > kMaxSpeedPidMaxDtMs ||
      !std::isfinite(hard_overspeed_margin_mps_) ||
      hard_overspeed_margin_mps_ <= 0.0 ||
      hard_overspeed_margin_mps_ > kMaxHardOverspeedMarginKph / 3.6) {
    throw std::invalid_argument("dynamic adapter configuration is incomplete");
  }
}

DynamicLibraryVehicleAdapter::~DynamicLibraryVehicleAdapter() {
  try {
    close();
  } catch (...) {
  }
  unload_dynamic_library(handle_);
}

void DynamicLibraryVehicleAdapter::ensure_loaded() {
  if (handle_ != nullptr) return;
  handle_ = open_dynamic_library(library_path_);
  try {
    validate_chassis_bridge_abi_handle(handle_);
    open_v4_fn_ = load_symbol<OpenV4Fn>(handle_, "mine_teleop_chassis_open_v4");
    apply_fn_ = load_symbol<ApplyFn>(handle_, "mine_teleop_chassis_apply_state");
    apply_v2_fn_ = load_symbol<ApplyV2Fn>(
        handle_, "mine_teleop_chassis_apply_state_v2");
    configure_runtime_control_fn_ = load_symbol<ConfigureRuntimeControlFn>(
        handle_, "mine_teleop_chassis_configure_runtime_control_v1");
    clear_runtime_control_fn_ = load_symbol<ClearRuntimeControlFn>(
        handle_, "mine_teleop_chassis_clear_runtime_control_v1");
    stop_fn_ = load_symbol<StopFn>(handle_, "mine_teleop_chassis_emergency_stop");
    request_handshake_fn_ = load_symbol<HandshakeFn>(
        handle_,
        "mine_teleop_chassis_request_parallel_handshake");
    disconnect_handshake_fn_ = load_symbol<HandshakeFn>(
        handle_,
        "mine_teleop_chassis_disconnect_parallel_handshake");
    poll_feedback_fn_ = load_symbol<PollFeedbackFn>(handle_, "mine_teleop_chassis_poll_feedback");
    read_handshake_fn_ = load_symbol<ReadHandshakeFn>(
        handle_,
        "mine_teleop_chassis_read_handshake_status");
    read_fn_ = load_symbol<ReadFn>(handle_, "mine_teleop_chassis_read_telemetry");
    read_can_feedback_v1_fn_ = load_optional_symbol<ReadCanFeedbackV1Fn>(
        handle_,
        "mine_teleop_chassis_read_can_feedback_v1");
    close_fn_ = load_symbol<CloseFn>(handle_, "mine_teleop_chassis_close");
  } catch (...) {
    unload_dynamic_library(handle_);
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
  try {
    ensure_loaded();
#if defined(__linux__)
    prepare_socketcan(can_interface_, can_bitrate_, can_tx_queue_length_);
#endif
    const BridgeOpenConfigV4 config{
        sizeof(BridgeOpenConfigV4),
        can_interface_.c_str(),
        full_scale_motor_torque_nm_,
        max_speed_mps_,
        control_timeout_ms_,
        speed_feedback_timeout_ms_,
        speed_pid_kp_,
        speed_pid_ki_,
        speed_pid_kd_,
        speed_pid_derivative_filter_tau_ms_,
        speed_pid_max_dt_ms_,
        hard_overspeed_margin_mps_,
        max_ordinary_brake_pressure_bar_,
        motor_torque_rise_rate_nm_per_s_};
    check_result(open_v4_fn_(&config), "mine_teleop_chassis_open_v4");
    opened_ = true;
  } catch (const std::exception& error) {
    last_error_ = error.what();
    throw;
  }
}

void DynamicLibraryVehicleAdapter::close() {
  if (!opened_ || close_fn_ == nullptr) return;
  check_result(close_fn_(), "mine_teleop_chassis_close");
  opened_ = false;
  feedback_ready_ = false;
}

std::uint64_t DynamicLibraryVehicleAdapter::configure_runtime_control_profile(
    const SessionControlProfile& profile,
    std::uint64_t profile_revision) {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  profile.validate();
  if (profile_revision == 0 ||
      profile.target_speed_kph > max_speed_mps_ * 3.6 + 1e-9 ||
      profile.max_motor_torque_nm > full_scale_motor_torque_nm_ + 1e-9 ||
      profile.max_brake_pressure_bar > max_ordinary_brake_pressure_bar_ + 1e-9) {
    throw std::invalid_argument("runtime control profile exceeds vehicle limits");
  }
  const BridgeRuntimeControlConfigV1 config{
      sizeof(BridgeRuntimeControlConfigV1),
      static_cast<std::uint32_t>(profile.profile_version),
      profile_revision,
      profile.target_speed_kph / 3.6,
      profile.max_motor_torque_nm,
      profile.max_brake_pressure_bar,
      profile.max_steering_angle_deg / kChassisControlMaxSteeringAngleDeg,
      profile.speed_pid_kp,
      profile.speed_pid_ki,
      profile.speed_pid_kd,
      profile.speed_pid_derivative_filter_tau_ms,
      profile.speed_pid_max_dt_ms,
      0U,
      profile.motor_torque_rise_rate_nm_per_s};
  BridgeRuntimeControlResultV1 result{};
  const int result_code = configure_runtime_control_fn_(&config, &result);
  if (result.struct_size != sizeof(BridgeRuntimeControlResultV1) ||
      result.result_code != result_code || result.reserved != 0U ||
      (result_code == 0 &&
       (result.issue_id != 0U || result.applied_revision != profile_revision))) {
    throw std::runtime_error(
        "mine_teleop_chassis_configure_runtime_control_v1 returned an invalid result structure");
  }
  if (result_code != 0) {
    throw std::runtime_error(
        "mine_teleop_chassis_configure_runtime_control_v1 rejected profile with code " +
        std::to_string(result_code) + " and issue " +
        std::to_string(result.issue_id));
  }
  session_motor_torque_limit_nm_ = profile.max_motor_torque_nm;
  session_brake_pressure_limit_bar_ = profile.max_brake_pressure_bar;
  runtime_control_profile_revision_ = result.applied_revision;
  return runtime_control_profile_revision_;
}

void DynamicLibraryVehicleAdapter::clear_runtime_control_profile() {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  BridgeRuntimeControlResultV1 result{};
  const int result_code = clear_runtime_control_fn_(&result);
  if (result.struct_size != sizeof(BridgeRuntimeControlResultV1) ||
      result.result_code != result_code || result.reserved != 0U ||
      (result_code == 0 &&
       (result.issue_id != 0U || result.applied_revision != 0U))) {
    throw std::runtime_error(
        "mine_teleop_chassis_clear_runtime_control_v1 returned an invalid result structure");
  }
  check_result(result_code, "mine_teleop_chassis_clear_runtime_control_v1");
  session_motor_torque_limit_nm_ = 0.0;
  session_brake_pressure_limit_bar_ = 0.0;
  runtime_control_profile_revision_ = 0;
}

double DynamicLibraryVehicleAdapter::session_motor_torque_limit_nm() const {
  return session_motor_torque_limit_nm_;
}

double DynamicLibraryVehicleAdapter::session_brake_pressure_limit_bar() const {
  return session_brake_pressure_limit_bar_;
}

void DynamicLibraryVehicleAdapter::apply_control(const ControlCommand& command) {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  const double velocity = dynamic_adapter_target_speed_mps(command, max_speed_mps_);
  const double traction_ceiling = full_scale_motor_torque_nm_ > 0.0
      ? session_motor_torque_limit_nm_ / full_scale_motor_torque_nm_
      : 0.0;
  const double acceleration = dynamic_adapter_target_acceleration(
      command,
      traction_ceiling,
      session_brake_pressure_limit_bar_,
      max_ordinary_brake_pressure_bar_);
  const double steering[4]{command.steering, command.steering, command.steering, command.steering};
  BridgeApplyResultV1 apply_result{};
  const int result = apply_v2_fn_(
      gear_to_bridge_value(command.gear),
      velocity,
      acceleration,
      steering,
      4,
      &apply_result);
  if (apply_result.struct_size != sizeof(BridgeApplyResultV1) ||
      apply_result.result_code != result || apply_result.reserved != 0U ||
      (result == 0 && apply_result.issue_id != kBridgeApplyIssueNone)) {
    last_error_ = "mine_teleop_chassis_apply_state_v2 returned an invalid result structure";
    throw std::runtime_error(last_error_);
  }
  if (result != 0) {
    last_error_ = "mine_teleop_chassis_apply_state_v2 rejected control with code " +
        std::to_string(result);
    throw VehicleAdapterControlRejected(
        bridge_apply_issue_code(apply_result.issue_id),
        result);
  }
  last_error_.clear();
  ++applied_command_count_;
}

void DynamicLibraryVehicleAdapter::apply_safe_stop(const ControlOutput& output) {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  if (output.estop || output.full_emergency_brake) {
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

bool DynamicLibraryVehicleAdapter::request_vcu_handshake() {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  const int result = request_handshake_fn_();
  if (result != 0) return false;
  feedback_ready_ = false;
  return true;
}

bool DynamicLibraryVehicleAdapter::disconnect_vcu_handshake() {
  if (!opened_) throw std::runtime_error("dynamic vehicle adapter is not open");
  check_result(
      disconnect_handshake_fn_(),
      "mine_teleop_chassis_disconnect_parallel_handshake");
  feedback_ready_ = false;
  return true;
}

bool DynamicLibraryVehicleAdapter::feedback_ready() const { return feedback_ready_; }

VcuHandshakeStatus DynamicLibraryVehicleAdapter::vcu_handshake_status() const {
  if (!opened_) {
    VcuHandshakeStatus status;
    status.supported = true;
    status.state = "closed";
    return status;
  }
  BridgeHandshakeStatus raw{};
  const int result = read_handshake_fn_(&raw);
  if (result != 0) {
    throw std::runtime_error(
        "mine_teleop_chassis_read_handshake_status failed with code " +
        std::to_string(result));
  }
  VcuHandshakeStatus status;
  status.supported = true;
  status.state = bridge_handshake_state(raw.state);
  status.requested = raw.requested != 0;
  status.ready = raw.ready != 0;
  status.disarming = raw.disarming != 0;
  status.parking_ready = raw.parking_ready != 0;
  status.driver_gear_request = raw.driver_gear_request;
  status.driver_gear_request_valid = raw.driver_gear_request_valid != 0;
  status.handshake_status = raw.handshake_status;
  status.handshake_valid = raw.handshake_valid != 0;
  for (std::size_t index = 0; index < status.epb_status.size(); ++index) {
    status.epb_status[index] = raw.epb_status[index];
    status.epb_valid[index] = raw.epb_valid[index] != 0;
  }
  status.speed_mps = raw.speed_mps;
  status.speed_valid = raw.speed_valid != 0;
  return status;
}

VehicleTelemetry DynamicLibraryVehicleAdapter::read_telemetry() {
  BridgeTelemetry telemetry{};
  check_result(read_fn_(&telemetry), "mine_teleop_chassis_read_telemetry");
  VehicleTelemetry result;
  result.speed_mps = telemetry.speed_mps;
  result.gear = bridge_value_to_gear(telemetry.gear);
  result.steering_feedback = telemetry.steering_feedback;
  result.throttle_feedback = telemetry.throttle_feedback;
  result.brake_feedback = telemetry.brake_feedback;
  result.estop = telemetry.estop != 0;
  if (read_can_feedback_v1_fn_ == nullptr) return result;

  BridgeCanFeedbackV1 raw{};
  check_result(
      read_can_feedback_v1_fn_(&raw),
      "mine_teleop_chassis_read_can_feedback_v1");
  auto& feedback = result.can_feedback;
  feedback.supported = true;
  feedback.feedback_fresh = raw.feedback_fresh != 0;
  feedback.max_feedback_age_ms = raw.max_feedback_age_ms;
  feedback.speed_mps = raw.speed_mps;
  feedback.speed_valid = raw.speed_valid != 0;
  feedback.gear = raw.gear;
  feedback.gear_valid = raw.gear_valid != 0;
  feedback.emergency_switch = raw.emergency_switch;
  feedback.driver_gear_request = raw.driver_gear_request;
  feedback.driver_gear_request_valid = raw.driver_gear_request_valid != 0;
  feedback.handshake_status = raw.handshake_status;
  feedback.handshake_valid = raw.handshake_valid != 0;
  for (std::size_t index = 0; index < feedback.parking_brake_status.size(); ++index) {
    feedback.parking_brake_status[index] = raw.epb_status[index];
    feedback.parking_brake_valid[index] = raw.epb_valid[index] != 0;
  }
  for (std::size_t index = 0; index < feedback.motor_mode.size(); ++index) {
    feedback.motor_mode[index] = raw.motor_mode[index];
    feedback.motor_mode_valid[index] = raw.motor_mode_valid[index] != 0;
    feedback.motor_torque_nm[index] = raw.motor_torque_nm[index];
    feedback.motor_torque_valid[index] = raw.motor_torque_valid[index] != 0;
    feedback.motor_speed_rpm[index] = raw.motor_speed_rpm[index];
    feedback.motor_speed_valid[index] = raw.motor_speed_valid[index] != 0;
    feedback.brake_mode[index] = raw.brake_mode[index];
    feedback.brake_valid[index] = raw.brake_valid[index] != 0;
    feedback.brake_pressure_bar[index] = raw.brake_pressure_bar[index];
  }
  for (std::size_t index = 0; index < feedback.steering_mode.size(); ++index) {
    feedback.steering_mode[index] = raw.steering_mode[index];
    feedback.steering_valid[index] = raw.steering_valid[index] != 0;
    feedback.steering_angle_deg[index] = raw.steering_angle_deg[index];
  }
  return result;
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
      can_bitrate_,
      can_tx_queue_length_,
  };
}

std::unique_ptr<VehicleAdapter> create_vehicle_adapter(const VehicleConfig& config) {
  if (config.vehicle_adapter.type == "mock") return std::make_unique<MockVehicleAdapter>();
  if (config.vehicle_adapter.type == "can" || config.vehicle_adapter.type == "dynamic_library") {
    return std::make_unique<DynamicLibraryVehicleAdapter>(
        config.vehicle_adapter.bridge_library_path,
        config.vehicle_adapter.can_interface,
        config.hardware.can_bitrate,
        config.hardware.can_tx_queue_length,
        config.field_safety.max_speed_kph / 3.6,
        config.field_safety.full_scale_motor_torque_nm,
        config.field_safety.motor_torque_rise_rate_nm_per_s,
        config.field_safety.max_brake_pressure_bar,
        config.control.control_timeout_ms,
        config.field_safety.speed_feedback_timeout_ms,
        config.field_safety.speed_pid_kp,
        config.field_safety.speed_pid_ki,
        config.field_safety.speed_pid_kd,
        config.field_safety.speed_pid_derivative_filter_tau_ms,
        config.field_safety.speed_pid_max_dt_ms,
        config.field_safety.hard_overspeed_margin_kph / 3.6);
  }
  throw std::runtime_error("unsupported vehicle adapter type: " + config.vehicle_adapter.type);
}

VehicleControlService::VehicleControlService(
    const VehicleConfig& config,
    std::string driver_id,
    std::string session_id,
    std::string control_token,
    std::unique_ptr<VehicleAdapter> adapter,
    int telemetry_interval_ms)
    : vehicle_id_(config.vehicle_id),
      driver_id_(std::move(driver_id)),
      session_id_(std::move(session_id)),
      control_token_(std::move(control_token)),
      commissioning_mode_(config.field_safety.commissioning_mode),
      receiver_(
          config.vehicle_id,
          driver_id_,
          session_id_,
          config.control.max_command_gap_ms,
          kProtocolVersion,
          true,
          control_token_),
      safety_(
          config.control.degraded_timeout_ms,
          config.control.control_timeout_ms,
          config.control.deceleration_profile),
      adapter_(std::move(adapter)),
      require_feedback_before_control_(config.field_safety.require_can_feedback_before_control),
      max_speed_kph_(config.field_safety.max_speed_kph),
      max_throttle_(config.field_safety.max_throttle),
      full_scale_motor_torque_nm_(config.field_safety.full_scale_motor_torque_nm),
      max_brake_pressure_bar_(config.field_safety.max_brake_pressure_bar),
      max_steering_angle_deg_(config.field_safety.max_steering_angle_deg),
      default_speed_pid_kp_(config.field_safety.speed_pid_kp),
      default_speed_pid_ki_(config.field_safety.speed_pid_ki),
      default_speed_pid_kd_(config.field_safety.speed_pid_kd),
      default_speed_pid_derivative_filter_tau_ms_(
          config.field_safety.speed_pid_derivative_filter_tau_ms),
      default_speed_pid_max_dt_ms_(config.field_safety.speed_pid_max_dt_ms),
      default_motor_torque_rise_rate_nm_per_s_(
          config.field_safety.motor_torque_rise_rate_nm_per_s),
      speed_feedback_timeout_ms_(config.field_safety.speed_feedback_timeout_ms),
      hard_overspeed_margin_kph_(config.field_safety.hard_overspeed_margin_kph),
      telemetry_interval_ms_(telemetry_interval_ms) {
  if (!adapter_) throw std::invalid_argument("vehicle adapter is required");
  if (telemetry_interval_ms_ <= 0) throw std::invalid_argument("telemetry interval must be positive");
  Json deceleration_profile = Json::array();
  for (const auto& stage : config.control.deceleration_profile) {
    deceleration_profile.push_back(
        {{"after_ms", stage.after_ms}, {"brake", stage.brake}});
  }
  read_only_control_safety_ = {
      {"control_rate_hz", config.control.rate_hz},
      {"max_command_gap_ms", config.control.max_command_gap_ms},
      {"degraded_timeout_ms", config.control.degraded_timeout_ms},
      {"control_timeout_ms", config.control.control_timeout_ms},
      {"deceleration_profile", std::move(deceleration_profile)},
      {"speed_feedback_timeout_ms",
       config.field_safety.speed_feedback_timeout_ms},
      {"hard_overspeed_margin_kph",
       config.field_safety.hard_overspeed_margin_kph},
      {"require_can_feedback_before_control",
       config.field_safety.require_can_feedback_before_control},
      {"require_local_estop_reset",
       config.field_safety.require_local_estop_reset},
      {"require_time_sync", config.field_safety.require_time_sync},
      {"max_time_sync_uncertainty_ms",
       config.field_safety.max_time_sync_uncertainty_ms},
      {"time_sync_interval_ms", config.field_safety.time_sync_interval_ms},
      {"time_sync_samples", config.field_safety.time_sync_samples},
      {"commissioning_mode", config.field_safety.commissioning_mode},
  };
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
  try {
    adapter_->clear_runtime_control_profile();
  } catch (...) {
    adapter_->close();
    throw;
  }
  safety_.mark_ready(timestamp_ms);
  started_ = true;
}

SessionControlProfileResult VehicleControlService::profile_result(
    const SessionControlProfileRequest& request,
    std::int64_t timestamp_ms,
    bool accepted,
    bool idempotent,
    std::string reason) const {
  SessionControlProfileResult result;
  result.vehicle_id = vehicle_id_;
  result.driver_id = driver_id_;
  result.session_id = session_id_;
  result.seq = request.seq == 0 ? 1 : request.seq;
  result.sent_at_utc_ms = std::max<std::int64_t>(timestamp_ms, 0);
  result.accepted = accepted;
  result.idempotent = idempotent;
  result.applied_revision = accepted ? result.seq : 0;
  result.reason = std::move(reason);
  result.effective_profile = active_session_profile_;
  return result;
}

SessionControlProfileResult VehicleControlService::receive_session_profile(
    const SessionControlProfileRequest& request,
    std::int64_t timestamp_ms) {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  if (timestamp_ms < 0) {
    throw std::invalid_argument("receive_time_ms must be non-negative");
  }
  try {
    ProtocolMetadata{
        request.protocol_version,
        request.vehicle_id,
        request.driver_id,
        request.session_id,
        request.seq,
        request.sent_at_utc_ms}
        .validate();
    if (request.control_token.empty()) {
      throw std::invalid_argument("control_token is required");
    }
  } catch (const std::exception& error) {
    return profile_result(
        request,
        timestamp_ms,
        false,
        false,
        std::string("invalid_profile:") + error.what());
  }
  if (request.vehicle_id != vehicle_id_) {
    return profile_result(request, timestamp_ms, false, false, "wrong_vehicle");
  }
  if (request.driver_id != driver_id_) {
    return profile_result(request, timestamp_ms, false, false, "wrong_driver");
  }
  if (request.session_id != session_id_) {
    return profile_result(request, timestamp_ms, false, false, "wrong_session");
  }
  if (request.control_token != control_token_) {
    return profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "control_token_invalid");
  }
  if (last_session_profile_request_) {
    if (request.seq < last_session_profile_request_->seq) {
      return profile_result(request, timestamp_ms, false, false, "old_seq");
    }
    if (request.seq == last_session_profile_request_->seq) {
      const auto& previous = *last_session_profile_request_;
      const bool is_identical_request =
          request.protocol_version == previous.protocol_version &&
          request.vehicle_id == previous.vehicle_id &&
          request.driver_id == previous.driver_id &&
          request.session_id == previous.session_id &&
          request.seq == previous.seq &&
          request.control_token == previous.control_token &&
          request.profile == previous.profile;
      if (!is_identical_request) {
        return profile_result(
            request,
            timestamp_ms,
            false,
            false,
            "profile_seq_conflict");
      }
      if (!active_session_profile_ && last_session_profile_result_ &&
          last_session_profile_result_->accepted) {
        return profile_result(
            request,
            timestamp_ms,
            false,
            true,
            "session_profile_cleared");
      }
      auto replay = *last_session_profile_result_;
      replay.sent_at_utc_ms = timestamp_ms;
      replay.idempotent = true;
      replay.effective_profile = active_session_profile_;
      last_session_profile_result_ = replay;
      return replay;
    }
  }
  try {
    request.profile.validate();
  } catch (const std::exception& error) {
    return profile_result(
        request,
        timestamp_ms,
        false,
        false,
        std::string("invalid_profile:") + error.what());
  }
  const auto timestamp_delta_ms = timestamp_ms - request.sent_at_utc_ms;
  if (timestamp_delta_ms > kSessionControlProfileMaxAgeMs) {
    return profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "profile_age_exceeded");
  }
  if (timestamp_delta_ms < -kSessionControlProfileMaxAgeMs) {
    return profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "profile_timestamp_in_future");
  }

  const auto cache_result = [&](SessionControlProfileResult result) {
    last_session_profile_request_ = request;
    last_session_profile_result_ = result;
    return result;
  };
  const double target_speed_ceiling_kph = max_speed_kph_ * max_throttle_;
  if (request.profile.target_speed_kph > target_speed_ceiling_kph + 1e-9) {
    return cache_result(profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "target_speed_exceeds_vehicle_limit"));
  }
  if (request.profile.max_motor_torque_nm >
      full_scale_motor_torque_nm_ + 1e-9) {
    return cache_result(profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "motor_torque_exceeds_vehicle_limit"));
  }
  if (request.profile.max_brake_pressure_bar >
      max_brake_pressure_bar_ + 1e-9) {
    return cache_result(profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "brake_pressure_exceeds_vehicle_limit"));
  }
  if (request.profile.max_steering_angle_deg >
      max_steering_angle_deg_ + 1e-9) {
    return cache_result(profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "steering_exceeds_vehicle_limit"));
  }

  const double current_torque_limit = active_session_profile_
      ? active_session_profile_->max_motor_torque_nm
      : 0.0;
  const double current_target_speed = active_session_profile_
      ? active_session_profile_->target_speed_kph
      : 0.0;
  const bool torque_increase =
      request.profile.max_motor_torque_nm > current_torque_limit + 1e-9;
  const bool target_speed_increase =
      request.profile.target_speed_kph > current_target_speed + 1e-9;
  const bool torque_decrease =
      request.profile.max_motor_torque_nm + 1e-9 < current_torque_limit;
  const bool target_speed_decrease =
      request.profile.target_speed_kph + 1e-9 < current_target_speed;
  SessionControlProfile empty_profile{};
  empty_profile.speed_pid_kp = default_speed_pid_kp_;
  empty_profile.speed_pid_ki = default_speed_pid_ki_;
  empty_profile.speed_pid_kd = default_speed_pid_kd_;
  empty_profile.speed_pid_derivative_filter_tau_ms =
      default_speed_pid_derivative_filter_tau_ms_;
  empty_profile.speed_pid_max_dt_ms = default_speed_pid_max_dt_ms_;
  empty_profile.motor_torque_rise_rate_nm_per_s =
      default_motor_torque_rise_rate_nm_per_s_;
  const auto& current_profile = active_session_profile_
      ? *active_session_profile_
      : empty_profile;
  const bool brake_parameters_changed =
      request.profile.max_brake_pressure_bar !=
          current_profile.max_brake_pressure_bar ||
      request.profile.service_brake_pressure_bar !=
          current_profile.service_brake_pressure_bar ||
      request.profile.hard_brake_pressure_bar !=
          current_profile.hard_brake_pressure_bar;
  const bool steering_changed =
      request.profile.max_steering_angle_deg !=
      current_profile.max_steering_angle_deg;
  const bool pid_parameters_changed =
      request.profile.speed_pid_kp != current_profile.speed_pid_kp ||
      request.profile.speed_pid_ki != current_profile.speed_pid_ki ||
      request.profile.speed_pid_kd != current_profile.speed_pid_kd ||
      request.profile.speed_pid_derivative_filter_tau_ms !=
          current_profile.speed_pid_derivative_filter_tau_ms ||
      request.profile.speed_pid_max_dt_ms !=
          current_profile.speed_pid_max_dt_ms ||
      request.profile.motor_torque_rise_rate_nm_per_s !=
          current_profile.motor_torque_rise_rate_nm_per_s;
  const auto adapter_status = adapter_->status();
  const bool mock_bench_bypass =
      commissioning_mode_ == "bench" && adapter_status.adapter_type == "mock";
  const bool first_profile = !active_session_profile_;
  if ((first_profile || target_speed_increase || torque_increase ||
       brake_parameters_changed || steering_changed || pid_parameters_changed) &&
      !mock_bench_bypass) {
    VcuHandshakeStatus handshake_status;
    bool handshake_status_available = false;
    try {
      handshake_status = adapter_->vcu_handshake_status();
      handshake_status_available = true;
    } catch (...) {
    }
    if (!handshake_status_available || !handshake_status.parking_ready) {
      return cache_result(profile_result(
          request,
          timestamp_ms,
          false,
          false,
          "parking_ready_required_for_profile_increase"));
    }
    if (handshake_status.state != "standby" &&
        handshake_status.state != "disarmed") {
      return cache_result(profile_result(
          request,
          timestamp_ms,
          false,
          false,
          "standby_or_disarmed_required_for_profile_change"));
    }
  }

  try {
    const auto applied_revision =
        adapter_->configure_runtime_control_profile(
            request.profile,
            request.seq);
    if (applied_revision != request.seq) {
      throw std::runtime_error(
          "adapter applied a runtime control revision inconsistent with request seq");
    }
    if ((target_speed_decrease || torque_decrease) &&
        !brake_parameters_changed && !steering_changed &&
        !pid_parameters_changed &&
        last_effective_command_ &&
        safety_.state() == SafetyState::ControlActive) {
      auto reapplied = *last_effective_command_;
      const double profile_throttle_limit = max_speed_kph_ > 0.0
          ? request.profile.target_speed_kph / max_speed_kph_
          : 0.0;
      reapplied.throttle = std::min(
          reapplied.throttle,
          std::min(max_throttle_, profile_throttle_limit));
      adapter_->apply_control(reapplied);
      last_effective_command_ = std::move(reapplied);
    }
  } catch (...) {
    try {
      adapter_->apply_safe_stop(
          ControlOutput{"N", 0.0, 0.0, 1.0, true});
    } catch (...) {
    }
    clear_session_profile();
    return cache_result(profile_result(
        request,
        timestamp_ms,
        false,
        false,
        "adapter_session_profile_apply_failed"));
  }

  active_session_profile_ = request.profile;
  return cache_result(profile_result(
      request,
      timestamp_ms,
      true,
      false,
      "accepted"));
}

ReceiveResult VehicleControlService::receive_command(const ControlCommand& command, std::int64_t timestamp_ms) {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  auto result = receiver_.accept(command, timestamp_ms);
  if (!result.accepted || !result.command) return result;
  auto& effective = *result.command;
  if (!effective.estop && !active_session_profile_) {
    if (!adapter_safe_stop_active_) {
      adapter_->apply_safe_stop(
          ControlOutput{"N", 0.0, 0.0, 1.0, false, true});
    }
    return {false, "session_control_profile_required", std::nullopt, {}};
  }
  const auto vehicle_limited_throttle =
      std::min(effective.throttle, max_throttle_);
  const double session_throttle_limit = active_session_profile_
      ? (max_speed_kph_ > 0.0
             ? active_session_profile_->target_speed_kph / max_speed_kph_
             : 0.0)
      : max_throttle_;
  const auto limited_throttle = std::min(
      vehicle_limited_throttle,
      session_throttle_limit);
  const double session_steering_angle_limit = active_session_profile_
      ? active_session_profile_->max_steering_angle_deg
      : max_steering_angle_deg_;
  const auto steering_limit =
      std::min(max_steering_angle_deg_, session_steering_angle_limit) / 30.0;
  const auto limited_steering =
      std::clamp(effective.steering, -steering_limit, steering_limit);
  if (vehicle_limited_throttle != effective.throttle) {
    result.warnings.emplace_back("vehicle_max_throttle_applied");
  }
  if (limited_throttle != vehicle_limited_throttle) {
    result.warnings.emplace_back("session_target_speed_applied");
  }
  if (limited_throttle != effective.throttle) {
    effective.throttle = limited_throttle;
  }
  if (limited_steering != effective.steering) {
    result.warnings.emplace_back(
        active_session_profile_ &&
                session_steering_angle_limit + 1e-9 < max_steering_angle_deg_
            ? "session_max_steering_applied"
            : "vehicle_max_steering_applied");
    effective.steering = limited_steering;
  }
  bool feedback_poll_failed = false;
  if (!result.command->estop) {
    try {
      adapter_->poll_feedback();
    } catch (const std::exception&) {
      feedback_poll_failed = true;
    }
  }
  const bool adapter_safety_observed = refresh_adapter_safe_stop_state();
  if (!result.command->estop && adapter_safe_stop_active_) {
    return {false, "adapter_safe_stop_active", std::nullopt, result.warnings};
  }
  if (!result.command->estop) {
    if (!adapter_safety_observed) {
      safety_.mark_fault();
      adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
      clear_session_profile();
      return {false, "adapter_safety_status_unavailable", std::nullopt, result.warnings};
    }
    if (feedback_poll_failed && require_feedback_before_control_) {
      safety_.mark_fault();
      adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
      clear_session_profile();
      return {false, "can_feedback_poll_failed", std::nullopt, result.warnings};
    }
    if (require_feedback_before_control_ && !adapter_->feedback_ready()) {
      adapter_->apply_safe_stop(
          ControlOutput{"N", 0.0, 0.0, 1.0, false, true});
      clear_session_profile();
      return {false, "can_feedback_missing", std::nullopt, result.warnings};
    }
  }
  auto safety_command = *result.command;
  if (!safety_command.estop && active_session_profile_) {
    safety_command.brake = vehicle_normalized_brake_request(
        safety_command.brake,
        active_session_profile_->max_brake_pressure_bar,
        max_brake_pressure_bar_);
  }
  if (!safety_command.estop &&
      safety_.state() != SafetyState::Estop &&
      safety_.state() != SafetyState::Fault) {
    try {
      adapter_->apply_control(*result.command);
    } catch (const VehicleAdapterControlRejected& error) {
      result.accepted = false;
      result.reason = "adapter_control_rejected";
      result.command.reset();
      result.issue_code = error.issue_code();
      return result;
    }
    safety_.on_valid_command(safety_command, timestamp_ms);
    last_effective_command_ = *result.command;
    return result;
  }

  // ESTOP is a safety latch, not an ordinary actuator transaction: preserve
  // it even when the adapter cannot apply the physical stop. Ordinary commands
  // received while ESTOP/Fault is already latched must not reach apply_control.
  safety_.on_valid_command(safety_command, timestamp_ms);
  if (!adapter_safe_stop_active_ ||
      safety_.state() == SafetyState::Estop ||
      safety_.state() == SafetyState::Fault) {
    if (safety_.state() == SafetyState::Estop ||
        safety_.state() == SafetyState::Fault) {
      clear_session_profile();
    }
    adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
  }
  return result;
}

bool VehicleControlService::request_vcu_handshake() {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  if (!active_session_profile_) return false;
  if (safety_.state() == SafetyState::Estop ||
      safety_.state() == SafetyState::Fault) {
    return false;
  }
  if (!adapter_->request_vcu_handshake()) return false;
  if (!safety_.reset_to_standby()) return false;
  adapter_safe_stop_active_ = false;
  return true;
}

bool VehicleControlService::disconnect_vcu_handshake() {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  const bool disconnected = adapter_->disconnect_vcu_handshake();
  if (disconnected) {
    adapter_safe_stop_active_ = true;
    clear_session_profile();
  }
  return disconnected;
}

void VehicleControlService::tick(std::int64_t timestamp_ms) {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  bool feedback_poll_failed = false;
  try {
    adapter_->poll_feedback();
  } catch (const std::exception&) {
    feedback_poll_failed = true;
  }
  const bool adapter_safety_observed = refresh_adapter_safe_stop_state();
  if ((!adapter_safety_observed || feedback_poll_failed) &&
      !adapter_safe_stop_active_ &&
      require_feedback_before_control_ &&
      safety_.state() != SafetyState::Estop &&
      safety_.state() != SafetyState::Fault) {
    safety_.mark_fault();
  }
  safety_.tick(timestamp_ms);
  if (safety_.state() == SafetyState::Degraded || safety_.state() == SafetyState::TimeoutBrake ||
      safety_.state() == SafetyState::Estop || safety_.state() == SafetyState::Fault) {
    const bool adapter_owns_recoverable_stop =
        adapter_safe_stop_active_ &&
        (safety_.state() == SafetyState::Degraded ||
         safety_.state() == SafetyState::TimeoutBrake);
    if (!adapter_owns_recoverable_stop) {
      adapter_->apply_safe_stop(safety_.current_output(timestamp_ms));
    }
    clear_session_profile();
  }
  if (!last_telemetry_ms_ || timestamp_ms - *last_telemetry_ms_ >= telemetry_interval_ms_) {
    try {
      if (telemetry_history_.size() == kMaxVehicleTelemetryHistory) telemetry_history_.pop_front();
      telemetry_history_.push_back(build_telemetry(timestamp_ms));
      last_telemetry_ms_ = timestamp_ms;
    } catch (...) {
      // Control safety was already evaluated above from the same adapter. A
      // failed observability snapshot must not tear down an adapter-owned stop
      // or prevent the outer fault path from keeping the vehicle stopped.
    }
  }
}

bool VehicleControlService::reset_estop(
    bool local_confirmed,
    std::string_view authorized_by,
    std::int64_t timestamp_ms) {
  if (safety_.state() != SafetyState::Estop || !local_confirmed || authorized_by.empty()) {
    return false;
  }
  std::string adapter_gear;
  try {
    const auto telemetry = adapter_->read_telemetry();
    const auto handshake = adapter_->vcu_handshake_status();
    adapter_gear = telemetry.gear;
    adapter_safe_stop_active_ = telemetry.estop || handshake.disarming;
    if (handshake.disarming) return false;
  } catch (...) {
    return false;
  }

  // Clear only the adapter's recoverable software stop before releasing the
  // outer ESTOP latch. Preserve the actual gear so a stopped D/R vehicle does
  // not enter WaitGear before the bridge can clear its software stop. A
  // physical or hard safety latch rejects this ordinary zero-output apply;
  // keep the outer latch and service alive in that case.
  try {
    adapter_->apply_safe_stop(ControlOutput{adapter_gear, 0.0, 0.0, 0.0, false});
  } catch (...) {
    adapter_safe_stop_active_ = true;
    return false;
  }

  const bool reset = safety_.reset_estop(local_confirmed, authorized_by, timestamp_ms);
  if (!reset) return false;
  adapter_safe_stop_active_ = false;
  static_cast<void>(refresh_adapter_safe_stop_state());
  return true;
}

void VehicleControlService::close() {
  if (!started_) return;
  adapter_->apply_safe_stop(ControlOutput{"N", 0.0, 0.0, 1.0, true});
  clear_session_profile();
  adapter_->close();
  started_ = false;
}

Json VehicleControlService::build_telemetry(std::int64_t timestamp_ms) {
  const auto telemetry = adapter_->read_telemetry();
  return {
      {"event", "vehicle_telemetry"},
      {"protocol_version", kProtocolVersion},
      {"vehicle_id", vehicle_id_},
      {"driver_id", driver_id_},
      {"session_id", session_id_},
      {"seq", ++telemetry_sequence_},
      {"sent_at_utc_ms", timestamp_ms},
      {"safety_state", to_string(safety_.state())},
      {"speed_mps", telemetry.speed_mps},
      {"gear", telemetry.gear},
      {"steering_feedback", telemetry.steering_feedback},
      {"throttle_feedback", telemetry.throttle_feedback},
      {"brake_feedback", telemetry.brake_feedback},
      {"estop", telemetry.estop},
      {"can_feedback", telemetry.can_feedback.to_json()},
      {"vehicle_adapter", adapter_->status().to_json()},
      {"vcu_handshake", adapter_->vcu_handshake_status().to_json()},
      {"control_limits", control_limits()},
      {"session_control_profile", session_control_profile()},
  };
}

bool VehicleControlService::refresh_adapter_safe_stop_state() noexcept {
  try {
    const auto telemetry = adapter_->read_telemetry();
    const auto handshake = adapter_->vcu_handshake_status();
    adapter_safe_stop_active_ = telemetry.estop || handshake.disarming;
    if (adapter_safe_stop_active_) clear_session_profile();
    return true;
  } catch (...) {
    // Preserve an already observed adapter-owned stop. Otherwise callers move
    // active control into their existing outer fault/safe-stop path.
    return false;
  }
}

void VehicleControlService::clear_session_profile() noexcept {
  active_session_profile_.reset();
  last_effective_command_.reset();
  if (last_session_profile_result_) {
    last_session_profile_result_->accepted = false;
    last_session_profile_result_->idempotent = false;
    last_session_profile_result_->applied_revision = 0;
    last_session_profile_result_->reason = "session_profile_cleared";
    last_session_profile_result_->effective_profile.reset();
  }
  try {
    adapter_->clear_runtime_control_profile();
  } catch (...) {
  }
}

Json VehicleControlService::control_limits() const {
  return {
      {"max_speed_kph", max_speed_kph_},
      {"max_throttle", max_throttle_},
      {"full_scale_motor_torque_nm", full_scale_motor_torque_nm_},
      {"max_brake_pressure_bar", max_brake_pressure_bar_},
      {"max_steering_angle_deg", max_steering_angle_deg_},
      {"default_speed_pid_kp", default_speed_pid_kp_},
      {"default_speed_pid_ki", default_speed_pid_ki_},
      {"default_speed_pid_kd", default_speed_pid_kd_},
      {"default_speed_pid_derivative_filter_tau_ms",
       default_speed_pid_derivative_filter_tau_ms_},
      {"default_speed_pid_max_dt_ms", default_speed_pid_max_dt_ms_},
      {"default_motor_torque_rise_rate_nm_per_s",
       default_motor_torque_rise_rate_nm_per_s_},
      {"motor_torque_rise_rate_limits_nm_per_s",
       {{"min", 0.0}, {"max", kMaxMotorTorqueRiseRateNmPerSecond}}},
      {"speed_pid_limits",
       {
           {"kp", {{"min", 0.0}, {"max", kMaxSpeedPidGain}}},
           {"ki", {{"min", 0.0}, {"max", kMaxSpeedPidGain}}},
           {"kd", {{"min", 0.0}, {"max", kMaxSpeedPidGain}}},
           {"derivative_filter_tau_ms",
            {{"min", 0.0}, {"max", kMaxSpeedPidDerivativeFilterTauMs}}},
           {"max_dt_ms",
            {{"min", kMinSpeedPidMaxDtMs}, {"max", kMaxSpeedPidMaxDtMs}}},
       }},
      {"speed_feedback_timeout_ms", speed_feedback_timeout_ms_},
      {"hard_overspeed_margin_kph", hard_overspeed_margin_kph_},
      {"speed_feedback_timeout_ms_read_only", true},
      {"hard_overspeed_margin_kph_read_only", true},
      {"read_only_control_safety", read_only_control_safety_},
  };
}

Json VehicleControlService::session_control_profile() const {
  return {
      {"last_request_seq", last_session_profile_request_
           ? Json(last_session_profile_request_->seq)
           : Json(nullptr)},
      {"active", active_session_profile_.has_value()},
      {"accepted", last_session_profile_result_
           ? last_session_profile_result_->accepted
           : false},
      {"idempotent", last_session_profile_result_
           ? last_session_profile_result_->idempotent
           : false},
      {"applied_revision", last_session_profile_result_
           ? last_session_profile_result_->applied_revision
           : 0},
      {"reason", last_session_profile_result_
           ? last_session_profile_result_->reason
           : "not_received"},
      {"effective_profile", active_session_profile_
           ? active_session_profile_->to_json()
           : Json(nullptr)},
  };
}

Json VehicleControlService::summary() const {
  return {
      {"event", "vehicle_control_summary"},
      {"vehicle_id", vehicle_id_},
      {"session_id", session_id_},
      {"safety_state", to_string(safety_.state())},
      {"telemetry_count", telemetry_sequence_},
      {"telemetry_retained_count", telemetry_history_.size()},
      {"vehicle_adapter", adapter_->status().to_json()},
      {"vcu_handshake", adapter_->vcu_handshake_status().to_json()},
      {"control_limits", control_limits()},
      {"session_control_profile", session_control_profile()},
  };
}

}  // namespace mine_teleop
