#include "mine_teleop/core.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
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

struct BridgeOpenConfigV1 {
  std::uint32_t struct_size;
  const char* can_interface;
  double full_scale_motor_torque_nm;
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
      {"max_brake", field_safety.max_brake},
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
    camera.critical_for_control = optional<bool>(node, "critical_for_control", true);
    camera.reopen_attempts = optional<int>(node, "reopen_attempts", 3);
    camera.reopen_backoff_ms = optional<int>(node, "reopen_backoff_ms", 500);
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
      optional<double>(safety, "full_scale_motor_torque_nm", 41.25);
  config.field_safety.max_brake = optional<double>(safety, "max_brake", 1.0);
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
  if (!std::isfinite(config.field_safety.full_scale_motor_torque_nm) ||
      config.field_safety.full_scale_motor_torque_nm < 0.0 ||
      config.field_safety.full_scale_motor_torque_nm > 165.0) {
    throw std::runtime_error(
        "field_safety.full_scale_motor_torque_nm must be finite and in [0, 165] Nm; "
        "higher values require a validated chassis torque mapping");
  }
  if (!std::isfinite(config.field_safety.max_speed_kph) ||
      config.field_safety.max_speed_kph < 0.0 ||
      !std::isfinite(config.field_safety.max_throttle) ||
      config.field_safety.max_throttle < 0.0 ||
      config.field_safety.max_throttle > 1.0 ||
      !std::isfinite(config.field_safety.max_brake) ||
      config.field_safety.max_brake < 0.0 ||
      config.field_safety.max_brake > 1.0 ||
      !std::isfinite(config.field_safety.max_steering_angle_deg) ||
      config.field_safety.max_steering_angle_deg < 0.0 ||
      config.field_safety.max_steering_angle_deg > 30.0 ||
      config.field_safety.max_time_sync_uncertainty_ms < 0 ||
      config.field_safety.time_sync_interval_ms <= 0 ||
      config.field_safety.time_sync_samples < 3 || config.field_safety.time_sync_samples > 15) {
    throw std::runtime_error("field_safety limits or time sync settings are invalid");
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
       !safety["full_scale_motor_torque_nm"] || !safety["max_brake"] ||
       !safety["max_steering_angle_deg"])) {
    throw std::runtime_error(
        "non-mock vehicle adapter requires explicit field_safety max_speed_kph, "
        "max_throttle, full_scale_motor_torque_nm, max_brake, and "
        "max_steering_angle_deg");
  }
  if (config.vehicle_adapter.type != "mock" && config.field_safety.max_speed_kph <= 0.0) {
    throw std::runtime_error("non-mock vehicle adapter requires field_safety.max_speed_kph > 0");
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
    double full_scale_motor_torque_nm)
    : library_path_(std::move(library_path)),
      can_interface_(std::move(can_interface)),
      can_bitrate_(can_bitrate),
      can_tx_queue_length_(can_tx_queue_length),
      max_speed_mps_(max_speed_mps),
      full_scale_motor_torque_nm_(full_scale_motor_torque_nm) {
  if (library_path_.empty() || can_interface_.empty() || can_bitrate_ <= 0 ||
      can_tx_queue_length_ < 16 || max_speed_mps_ <= 0.0 ||
      !std::isfinite(full_scale_motor_torque_nm_) ||
      full_scale_motor_torque_nm_ < 0.0 || full_scale_motor_torque_nm_ > 165.0) {
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
#if defined(_WIN32)
  handle_ = static_cast<void*>(LoadLibraryW(library_path_.wstring().c_str()));
  if (handle_ == nullptr) {
    last_error_ = "failed to load dynamic library (Windows error " + std::to_string(GetLastError()) + ")";
    throw std::runtime_error(last_error_);
  }
#else
  handle_ = dlopen(library_path_.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (handle_ == nullptr) {
    last_error_ = std::string("failed to load dynamic library: ") + dlerror();
    throw std::runtime_error(last_error_);
  }
#endif
  try {
    open_v1_fn_ = load_symbol<OpenV1Fn>(handle_, "mine_teleop_chassis_open_v1");
    apply_fn_ = load_symbol<ApplyFn>(handle_, "mine_teleop_chassis_apply_state");
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
    const BridgeOpenConfigV1 config{
        sizeof(BridgeOpenConfigV1), can_interface_.c_str(), full_scale_motor_torque_nm_};
    check_result(open_v1_fn_(&config), "mine_teleop_chassis_open_v1");
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
        config.field_safety.full_scale_motor_torque_nm);
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
      receiver_(
          config.vehicle_id,
          driver_id_,
          session_id_,
          config.control.max_command_gap_ms,
          kProtocolVersion,
          true,
          std::move(control_token)),
      safety_(
          config.control.degraded_timeout_ms,
          config.control.control_timeout_ms,
          config.control.deceleration_profile),
      adapter_(std::move(adapter)),
      require_feedback_before_control_(config.field_safety.require_can_feedback_before_control),
      max_speed_kph_(config.field_safety.max_speed_kph),
      max_throttle_(config.field_safety.max_throttle),
      full_scale_motor_torque_nm_(config.field_safety.full_scale_motor_torque_nm),
      max_brake_(config.field_safety.max_brake),
      max_steering_angle_deg_(config.field_safety.max_steering_angle_deg),
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
  auto& effective = *result.command;
  const auto limited_throttle = std::min(effective.throttle, max_throttle_);
  const auto limited_brake = std::min(effective.brake, max_brake_);
  const auto steering_limit = max_steering_angle_deg_ / 30.0;
  const auto limited_steering =
      std::clamp(effective.steering, -steering_limit, steering_limit);
  if (limited_throttle != effective.throttle) {
    result.warnings.emplace_back("vehicle_max_throttle_applied");
    effective.throttle = limited_throttle;
  }
  if (!effective.estop && limited_brake != effective.brake) {
    result.warnings.emplace_back("vehicle_max_brake_applied");
    effective.brake = limited_brake;
  }
  if (limited_steering != effective.steering) {
    result.warnings.emplace_back("vehicle_max_steering_applied");
    effective.steering = limited_steering;
  }
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

bool VehicleControlService::request_vcu_handshake() {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  return adapter_->request_vcu_handshake();
}

bool VehicleControlService::disconnect_vcu_handshake() {
  if (!started_) throw std::runtime_error("vehicle control service is not started");
  return adapter_->disconnect_vcu_handshake();
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
    if (telemetry_history_.size() == kMaxVehicleTelemetryHistory) telemetry_history_.pop_front();
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
  };
}

Json VehicleControlService::control_limits() const {
  return {
      {"max_speed_kph", max_speed_kph_},
      {"max_throttle", max_throttle_},
      {"full_scale_motor_torque_nm", full_scale_motor_torque_nm_},
      {"max_brake", max_brake_},
      {"max_steering_angle_deg", max_steering_angle_deg_},
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
  };
}

}  // namespace mine_teleop
