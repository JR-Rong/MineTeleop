#pragma once

#include "mine_teleop/core.hpp"

#include <atomic>
#include <memory>
#include <utility>

namespace mine_teleop::test {

// Existing deterministic vectors advance a logical clock explicitly. Runtime
// services use steady_clock; this fixture never changes the production default.
class LogicalTimeControlService : public VehicleControlService {
 public:
  LogicalTimeControlService(const VehicleConfig& config, std::string driver_id,
      std::string session_id, std::string token, std::unique_ptr<VehicleAdapter> adapter,
      int telemetry_interval_ms = 100)
      : LogicalTimeControlService(std::make_shared<std::atomic<std::int64_t>>(0),
            config, std::move(driver_id), std::move(session_id), std::move(token),
            std::move(adapter), telemetry_interval_ms) {}

  void start(std::int64_t time) { clock_->store(time); VehicleControlService::start(time); }
  void tick(std::int64_t time) { clock_->store(time); VehicleControlService::tick(time); }
  ReceiveResult receive_command(const ControlCommand& command, std::int64_t time) {
    clock_->store(time);
    return VehicleControlService::receive_command(command, time);
  }
  SessionControlProfileResult receive_session_profile(const SessionControlProfileRequest& request,
      std::int64_t time) {
    clock_->store(time);
    return VehicleControlService::receive_session_profile(request, time);
  }
  void set_control_braking_wait(bool waiting, std::int64_t time,
      ControlRecoveryCause cause = ControlRecoveryCause::CriticalVideo) {
    clock_->store(time);
    VehicleControlService::set_control_braking_wait(waiting, time, cause);
  }
  bool reset_estop(bool confirmed, std::string_view by, std::int64_t time) {
    clock_->store(time);
    return VehicleControlService::reset_estop(confirmed, by, time);
  }
  Json control_timing(std::int64_t time) const {
    const auto previous = clock_->exchange(time);
    struct RestoreClock {
      std::shared_ptr<std::atomic<std::int64_t>> clock;
      std::int64_t previous;
      ~RestoreClock() { clock->store(previous); }
    } restore{clock_, previous};
    return VehicleControlService::control_timing(time);
  }

 private:
  LogicalTimeControlService(std::shared_ptr<std::atomic<std::int64_t>> clock,
      const VehicleConfig& config, std::string driver_id, std::string session_id,
      std::string token, std::unique_ptr<VehicleAdapter> adapter, int telemetry_interval_ms)
      : VehicleControlService(config, std::move(driver_id), std::move(session_id),
            std::move(token), std::move(adapter), telemetry_interval_ms,
            [clock] { return clock->load(); }), clock_(std::move(clock)) {}
  std::shared_ptr<std::atomic<std::int64_t>> clock_;
};

}  // namespace mine_teleop::test
