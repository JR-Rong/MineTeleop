#include "mine_teleop/core.hpp"

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

using namespace mine_teleop;

// Exit must always run the adapter close (the bridge's reverse VCU handshake
// and EPB park), even when the service-brake apply fails first. This adapter
// makes both steps observable so the ordering can be asserted.
class ExitFaultAdapter final : public VehicleAdapter {
 public:
  void open() override { opened = true; }
  void close() override {
    ++close_calls;
    if (close_throws) throw std::runtime_error("adapter close failed");
    opened = false;
  }
  [[nodiscard]] std::uint64_t configure_runtime_control_profile(
      const SessionControlProfile&, std::uint64_t revision) override {
    return revision;
  }
  void clear_runtime_control_profile() override {}
  [[nodiscard]] double session_motor_torque_limit_nm() const override {
    return 0.0;
  }
  [[nodiscard]] double session_brake_pressure_limit_bar() const override {
    return 0.0;
  }
  void apply_control(const ControlCommand&) override {}
  void apply_safe_stop(
      const ControlOutput&, VehicleStopContext) override {
    ++safe_stop_calls;
    throw std::runtime_error("adapter safe stop failed");
  }
  bool poll_feedback() override { return false; }
  bool request_vcu_handshake() override { return false; }
  bool disconnect_vcu_handshake() override { return false; }
  [[nodiscard]] bool feedback_ready() const override { return false; }
  [[nodiscard]] VcuHandshakeStatus vcu_handshake_status() const override {
    VcuHandshakeStatus status;
    status.supported = true;
    status.state = "standby";
    status.parking_ready = true;
    return status;
  }
  [[nodiscard]] VehicleTelemetry read_telemetry() override { return {}; }
  [[nodiscard]] VehicleAdapterStatus status() const override {
    return {"exit_fault", opened, true, "can0", "", 0, 0, "", false, 0, 0};
  }

  bool opened{false};
  bool close_throws{false};
  int close_calls{0};
  int safe_stop_calls{0};
};

void expect(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

void test_close_still_disarms_when_safe_stop_fails() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<ExitFaultAdapter>();
  auto* view = adapter.get();
  VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 10000);
  service.start(0);

  bool threw = false;
  try {
    service.close();
  } catch (const std::exception&) {
    threw = true;
  }
  expect(threw, "close hid the safe-stop failure from the caller");
  expect(view->safe_stop_calls == 1, "close did not attempt the service-brake apply");
  expect(
      view->close_calls == 1,
      "close skipped the adapter disarm after the service-brake apply failed");
  expect(!view->opened, "adapter stayed open after service close");

  service.close();
  expect(view->close_calls == 1, "second close re-ran the adapter disarm");
  expect(view->safe_stop_calls == 1, "second close re-ran the service-brake apply");
}

void test_close_propagates_adapter_disarm_failure_once() {
  auto config = load_vehicle_config("configs/vehicle-agent.dev.yaml");
  auto adapter = std::make_unique<ExitFaultAdapter>();
  auto* view = adapter.get();
  view->close_throws = true;
  VehicleControlService service(
      config, "driver-001", "session-001", "token", std::move(adapter), 10000);
  service.start(0);

  bool threw = false;
  try {
    service.close();
  } catch (const std::exception&) {
    threw = true;
  }
  expect(threw, "close hid an adapter disarm failure");
  expect(view->close_calls == 1, "close did not attempt the adapter disarm");
  service.close();
  expect(view->close_calls == 1, "a failed close left the service startable");
}

}  // namespace

int main() {
  const std::pair<const char*, void (*)()> tests[] = {
      {"close_still_disarms_when_safe_stop_fails",
       test_close_still_disarms_when_safe_stop_fails},
      {"close_propagates_adapter_disarm_failure_once",
       test_close_propagates_adapter_disarm_failure_once},
  };
  int failed = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      std::cout << "[PASS] " << name << '\n';
    } catch (const std::exception& error) {
      ++failed;
      std::cerr << "[FAIL] " << name << ": " << error.what() << '\n';
    }
  }
  return failed == 0 ? 0 : 1;
}
