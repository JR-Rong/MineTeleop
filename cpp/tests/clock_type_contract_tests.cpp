#include "mine_teleop/core.hpp"

#include <concepts>
#include <cstdint>
#include <optional>
#include <string_view>

namespace {

using mine_teleop::ClockSample;
using mine_teleop::ControlCommand;
using mine_teleop::ControlOutput;
using mine_teleop::ControlReceiver;
using mine_teleop::MonotonicMillis;
using mine_teleop::ReceiveResult;
using mine_teleop::RecoveryCause;
using mine_teleop::SafetyStateMachine;
using mine_teleop::SessionControlProfileRequest;
using mine_teleop::SessionControlProfileResult;
using mine_teleop::UtcMillis;
using mine_teleop::VehicleControlService;

template <typename Time>
concept ControlReceiverAccepts =
    requires(ControlReceiver& receiver, const ControlCommand& command, Time time) {
      { receiver.accept(command, time) } -> std::same_as<ReceiveResult>;
    };

template <typename Time>
concept SafetyMarkReadyAccepts =
    requires(SafetyStateMachine& safety, Time time) { safety.mark_ready(time); };

template <typename Time>
concept SafetyOnValidCommandAccepts =
    requires(SafetyStateMachine& safety, const ControlCommand& command, Time time) {
      safety.on_valid_command(command, time);
    };

template <typename Time>
concept SafetyRecoverAccepts =
    requires(SafetyStateMachine& safety, const std::optional<ControlCommand>& command, Time time) {
      { safety.recover(RecoveryCause::AuthorizedHandshake, command, time) } -> std::same_as<bool>;
    };

template <typename Time>
concept SafetyTickAccepts = requires(SafetyStateMachine& safety, Time time) { safety.tick(time); };

template <typename Time>
concept SafetyCurrentOutputAccepts = requires(const SafetyStateMachine& safety, Time time) {
  { safety.current_output(time) } -> std::same_as<ControlOutput>;
};

template <typename Time>
concept SafetyResetEstopAccepts = requires(SafetyStateMachine& safety, Time time) {
  { safety.reset_estop(true, std::string_view{"operator"}, time) } -> std::same_as<bool>;
};

template <typename Time>
concept ServiceStartAccepts =
    requires(VehicleControlService& service, Time time) { service.start(time); };

template <typename Time>
concept ServiceReceiveCommandAccepts =
    requires(VehicleControlService& service, const ControlCommand& command, Time time) {
      { service.receive_command(command, time) } -> std::same_as<ReceiveResult>;
    };

template <typename Time>
concept ServiceReceiveSessionProfileAccepts = requires(
    VehicleControlService& service, const SessionControlProfileRequest& request, Time time) {
  { service.receive_session_profile(request, time) } -> std::same_as<SessionControlProfileResult>;
};

template <typename Time>
concept ServiceTickAccepts =
    requires(VehicleControlService& service, Time time) { service.tick(time); };

template <typename Time>
concept ServiceResetEstopAccepts = requires(VehicleControlService& service, Time time) {
  { service.reset_estop(true, std::string_view{"operator"}, time) } -> std::same_as<bool>;
};

static_assert(ControlReceiverAccepts<ClockSample>);
static_assert(!ControlReceiverAccepts<std::int64_t>);
static_assert(!ControlReceiverAccepts<UtcMillis>);

static_assert(SafetyMarkReadyAccepts<MonotonicMillis>);
static_assert(SafetyOnValidCommandAccepts<MonotonicMillis>);
static_assert(SafetyRecoverAccepts<MonotonicMillis>);
static_assert(SafetyTickAccepts<MonotonicMillis>);
static_assert(SafetyCurrentOutputAccepts<MonotonicMillis>);
static_assert(SafetyResetEstopAccepts<MonotonicMillis>);
static_assert(!SafetyMarkReadyAccepts<std::int64_t>);
static_assert(!SafetyOnValidCommandAccepts<std::int64_t>);
static_assert(!SafetyRecoverAccepts<std::int64_t>);
static_assert(!SafetyTickAccepts<std::int64_t>);
static_assert(!SafetyCurrentOutputAccepts<std::int64_t>);
static_assert(!SafetyResetEstopAccepts<std::int64_t>);
static_assert(!SafetyMarkReadyAccepts<UtcMillis>);
static_assert(!SafetyOnValidCommandAccepts<UtcMillis>);
static_assert(!SafetyRecoverAccepts<UtcMillis>);
static_assert(!SafetyTickAccepts<UtcMillis>);
static_assert(!SafetyCurrentOutputAccepts<UtcMillis>);
static_assert(!SafetyResetEstopAccepts<UtcMillis>);

static_assert(ServiceStartAccepts<ClockSample>);
static_assert(ServiceReceiveCommandAccepts<ClockSample>);
static_assert(ServiceReceiveSessionProfileAccepts<ClockSample>);
static_assert(ServiceTickAccepts<ClockSample>);
static_assert(ServiceResetEstopAccepts<ClockSample>);
static_assert(!ServiceStartAccepts<std::int64_t>);
static_assert(!ServiceReceiveCommandAccepts<std::int64_t>);
static_assert(!ServiceReceiveSessionProfileAccepts<std::int64_t>);
static_assert(!ServiceTickAccepts<std::int64_t>);
static_assert(!ServiceResetEstopAccepts<std::int64_t>);
static_assert(!ServiceStartAccepts<UtcMillis>);
static_assert(!ServiceReceiveCommandAccepts<UtcMillis>);
static_assert(!ServiceReceiveSessionProfileAccepts<UtcMillis>);
static_assert(!ServiceTickAccepts<UtcMillis>);
static_assert(!ServiceResetEstopAccepts<UtcMillis>);

}  // namespace

int main() {
  return 0;
}
