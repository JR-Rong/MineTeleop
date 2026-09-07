#include "mine_teleop/vcu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace {

using mine_teleop::vcu::CanFrame;
using mine_teleop::vcu::Command;
using mine_teleop::vcu::ParallelController;
using mine_teleop::vcu::State;
using Json = nlohmann::json;

class TestFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, std::string_view message) {
  if (!condition) throw TestFailure(std::string(message));
}

Json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input) throw TestFailure("cannot open DBC codec contract input: " + path.string());
  try {
    return Json::parse(input);
  } catch (const Json::exception& error) {
    throw TestFailure("cannot parse DBC codec contract input " + path.string() + ": " + error.what());
  }
}

std::string replace_index_template(std::string value, int index) {
  const std::string placeholder{"{index02}"};
  const auto offset = value.find(placeholder);
  expect(offset != std::string::npos, "manifest family message name has no index placeholder");
  const auto index_text = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
  value.replace(offset, placeholder.size(), index_text);
  return value;
}

Json find_manifest_message(const Json& manifest, std::string_view name) {
  for (const auto& message : manifest.at("messages")) {
    if (message.at("name").get<std::string>() == name) return message;
  }
  for (const auto& family : manifest.at("families")) {
    for (const auto& instance : family.at("instances")) {
      const auto message_name = replace_index_template(
          family.at("dbc_message_name_template").get<std::string>(),
          instance.at("index").get<int>());
      if (message_name != name) continue;
      return Json{
          {"name", message_name},
          {"layout", family.at("layout")},
          {"arbitration_id", instance.at("arbitration_id")},
      };
    }
  }
  throw TestFailure("manifest has no CAN message named " + std::string(name));
}

std::uint32_t parse_hex_id(const std::string& value) {
  std::size_t consumed = 0;
  const auto parsed = std::stoul(value, &consumed, 16);
  expect(consumed == value.size(), "manifest CAN ID is not a complete hexadecimal value");
  expect(parsed <= std::numeric_limits<std::uint32_t>::max(), "manifest CAN ID exceeds uint32");
  return static_cast<std::uint32_t>(parsed);
}

std::array<std::uint8_t, 8> parse_hex_payload(std::string_view value) {
  expect(value.size() == 16, "golden CAN payload must have exactly 16 hex characters");
  std::array<std::uint8_t, 8> payload{};
  for (std::size_t index = 0; index < payload.size(); ++index) {
    std::size_t consumed = 0;
    const auto byte = std::stoul(std::string(value.substr(index * 2U, 2U)), &consumed, 16);
    expect(consumed == 2U && byte <= 0xFFU, "golden CAN payload has an invalid byte");
    payload[index] = static_cast<std::uint8_t>(byte);
  }
  return payload;
}

double parse_decimal(const Json& value, std::string_view field) {
  expect(value.is_string(), std::string(field) + " must be an exact decimal string");
  const auto text = value.get<std::string>();
  std::size_t consumed = 0;
  const auto parsed = std::stod(text, &consumed);
  expect(consumed == text.size() && std::isfinite(parsed), std::string(field) + " is not finite");
  return parsed;
}

int parse_integer(const Json& value, std::string_view field) {
  const auto parsed = parse_decimal(value, field);
  expect(std::trunc(parsed) == parsed, std::string(field) + " is not an integer");
  return static_cast<int>(parsed);
}

template <std::size_t Size>
std::array<double, Size> parse_decimal_array(const Json& value, std::string_view field) {
  expect(value.is_array() && value.size() == Size, std::string(field) + " has the wrong length");
  std::array<double, Size> output{};
  for (std::size_t index = 0; index < output.size(); ++index) {
    output[index] = parse_decimal(value.at(index), field);
  }
  return output;
}

Command command_from_vector(const Json& value) {
  Command command;
  command.gear = parse_integer(value.at("gear"), "gear");
  command.vehicle_speed_request_kph =
      parse_decimal(value.at("vehicle_speed_request_kph"), "vehicle_speed_request_kph");
  command.vehicle_speed_request_valid = value.at("vehicle_speed_request_valid").get<bool>();
  command.motor_torque_nm = parse_decimal_array<mine_teleop::vcu::kMotorCount>(
      value.at("motor_torque_nm"), "motor_torque_nm");
  command.motor_speed_rpm = parse_decimal_array<mine_teleop::vcu::kMotorCount>(
      value.at("motor_speed_rpm"), "motor_speed_rpm");
  command.steering_angle_deg = parse_decimal_array<mine_teleop::vcu::kSteeringAxisCount>(
      value.at("steering_angle_deg"), "steering_angle_deg");
  command.steering_speed_degps = parse_decimal_array<mine_teleop::vcu::kSteeringAxisCount>(
      value.at("steering_speed_degps"), "steering_speed_degps");
  command.brake_pressure_bar = parse_decimal_array<mine_teleop::vcu::kBrakeCount>(
      value.at("brake_pressure_bar"), "brake_pressure_bar");
  command.fault_reset = value.at("fault_reset").get<bool>();
  return command;
}

constexpr std::array<std::uint32_t, mine_teleop::vcu::kMotorCount> kMotorStatus01Ids{
    0x18A0F4D0U,
    0x18A3F4D0U,
    0x18A6F4D0U,
    0x18A9F4D0U,
    0x18ACF4D0U,
    0x18AFF4D0U,
    0x18B2F4D0U,
    0x18B5F4D0U,
};

constexpr std::array<std::uint32_t, mine_teleop::vcu::kMotorCount> kMotorStatus02Ids{
    0x18A1F4D0U,
    0x18A4F4D0U,
    0x18A7F4D0U,
    0x18AAF4D0U,
    0x18ADF4D0U,
    0x18B0F4D0U,
    0x18B3F4D0U,
    0x18B6F4D0U,
};

constexpr std::array<std::uint32_t, mine_teleop::vcu::kSteeringAxisCount> kSteeringStatusIds{
    0x18C0F4D0U,
    0x18C1F4D0U,
    0x18C2F4D0U,
    0x18C3F4D0U,
};

constexpr std::array<std::uint32_t, mine_teleop::vcu::kSteeringAxisCount> kBrakeStatusIds{
    0x18C8F4D0U,
    0x18C9F4D0U,
    0x18CAF4D0U,
    0x18CBF4D0U,
};

CanFrame handshake_feedback(int status) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuHandshake};
  frame.data[1] = static_cast<std::uint8_t>(status);
  return frame;
}

CanFrame parking_brake_feedback(int status) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuParkingBrake};
  frame.data[0] = static_cast<std::uint8_t>(status);
  frame.data[2] = static_cast<std::uint8_t>(status);
  frame.data[4] = static_cast<std::uint8_t>(status);
  frame.data[6] = static_cast<std::uint8_t>(status);
  return frame;
}

CanFrame gear_feedback(int gear) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuVehicleStatus};
  frame.data[0] = static_cast<std::uint8_t>((gear & 0x03) << 2U);
  return frame;
}

CanFrame speed_feedback_zero() {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuVehicleSpeed};
  constexpr std::uint16_t kRawZeroKph = 5000U;
  frame.data[0] = static_cast<std::uint8_t>(kRawZeroKph & 0xFFU);
  frame.data[1] = static_cast<std::uint8_t>((kRawZeroKph >> 8U) & 0xFFU);
  return frame;
}

CanFrame driver_gear_request_feedback(int gear) {
  CanFrame frame{mine_teleop::vcu::ids::kWvcuDriverIntention};
  frame.data[7] = static_cast<std::uint8_t>((gear & 0x07) << 1U);
  return frame;
}

CanFrame motor_mode_feedback(std::size_t motor) {
  CanFrame frame{kMotorStatus02Ids.at(motor)};
  frame.data[0] = 0x10U;
  return frame;
}

CanFrame motor_torque_feedback(std::size_t motor) {
  CanFrame frame{kMotorStatus01Ids.at(motor)};
  frame.data[0] = 0x40U;
  frame.data[1] = 0x9FU;
  frame.data[2] = 0x40U;
  frame.data[3] = 0x1FU;
  return frame;
}

CanFrame steering_feedback(std::size_t axis) {
  CanFrame frame{kSteeringStatusIds.at(axis)};
  frame.data[0] = 1U;
  return frame;
}

CanFrame brake_feedback(std::size_t pair) {
  CanFrame frame{kBrakeStatusIds.at(pair)};
  frame.data[0] = 1U;
  frame.data[4] = 1U;
  return frame;
}

void send_mode_feedback(ParallelController& controller) {
  for (std::size_t index = 0; index < mine_teleop::vcu::kMotorCount; ++index) {
    expect(controller.ingest(motor_mode_feedback(index)), "motor mode feedback was rejected");
    expect(controller.ingest(motor_torque_feedback(index)), "motor torque feedback was rejected");
  }
  for (std::size_t index = 0; index < mine_teleop::vcu::kSteeringAxisCount; ++index) {
    expect(controller.ingest(steering_feedback(index)), "steering feedback was rejected");
    expect(controller.ingest(brake_feedback(index)), "brake feedback was rejected");
  }
}

void advance_to_ready(ParallelController& controller, const Command& command) {
  expect(controller.ingest(handshake_feedback(3)), "manual handshake feedback was rejected");
  expect(controller.ingest(parking_brake_feedback(2)), "parked EPB feedback was rejected");
  expect(controller.ingest(speed_feedback_zero()), "zero-speed feedback was rejected");
  expect(controller.ingest(gear_feedback(1)), "neutral actual-gear feedback was rejected");
  expect(controller.ingest(driver_gear_request_feedback(1)), "driver neutral request feedback was rejected");
  expect(controller.request_parallel_handshake(), "N/park/manual handshake setup was rejected");
  expect(controller.set_command(command), "runtime vector command was rejected");
  for (int index = 0; index < 6; ++index) static_cast<void>(controller.tick());
  expect(controller.state() == State::WaitParallelHandshake, "initial handshake low period was not emitted");

  expect(controller.ingest(handshake_feedback(5)), "intelligent handshake feedback was rejected");
  static_cast<void>(controller.tick());
  expect(controller.state() == State::WaitParkingBrakeReleased, "handshake did not advance to EPB release");

  expect(controller.ingest(parking_brake_feedback(1)), "EPB release feedback was rejected");
  static_cast<void>(controller.tick());
  expect(controller.state() == State::WaitGear, "EPB release did not advance to gear gate");

  expect(controller.ingest(gear_feedback(command.gear)), "target gear feedback was rejected");
  static_cast<void>(controller.tick());
  expect(controller.state() == State::WaitActuatorModes, "gear feedback did not advance to actuator gate");

  send_mode_feedback(controller);
  static_cast<void>(controller.tick());
  expect(controller.state() == State::Ready, "actuator feedback did not reach ready state");
}

void test_ready_frames_match_independent_golden_vectors() {
  const auto vectors_path = std::filesystem::path("protocol/can/fixtures/jyr010-dbc-codec-vectors.json");
  const auto vectors = read_json(vectors_path);
  const auto manifest = read_json(vectors_path.parent_path() / vectors.at("manifest").get<std::string>());
  expect(vectors.at("format").get<std::string>() == "mine-teleop-jyr010-dbc-codec-vectors-v1",
      "unexpected DBC codec vector format");
  expect(manifest.at("format").get<std::string>() == "mine-teleop-jyr010-dbc-codec-manifest-v1",
      "unexpected DBC codec manifest format");

  std::map<std::string, Json> vectors_by_name;
  for (const auto& vector : vectors.at("vectors")) {
    const auto name = vector.at("name").get<std::string>();
    expect(vectors_by_name.emplace(name, vector).second, "DBC codec vector name is duplicated");
  }
  expect(vectors.at("runtime_cases").size() == 1U, "expected exactly one focused runtime DBC case");
  const auto& runtime_case = vectors.at("runtime_cases").at(0);
  const auto command = command_from_vector(runtime_case.at("command"));

  ParallelController controller;
  advance_to_ready(controller, command);
  expect(
      std::string(mine_teleop::vcu::state_name(controller.state())) ==
          runtime_case.at("expected_state").get<std::string>(),
      "runtime vector did not reach its declared state");

  const auto frames = controller.tick();
  const auto& expected_names = runtime_case.at("expected_vector_names");
  expect(frames.size() == expected_names.size(), "runtime emitted an unexpected CAN frame count");
  for (std::size_t index = 0; index < frames.size(); ++index) {
    const auto vector_name = expected_names.at(index).get<std::string>();
    const auto vector = vectors_by_name.find(vector_name);
    expect(vector != vectors_by_name.end(), "runtime case references an unknown golden vector");
    const auto message = find_manifest_message(manifest, vector->second.at("message").get<std::string>());
    const auto expected_id = parse_hex_id(message.at("arbitration_id").get<std::string>());
    const auto expected_data = parse_hex_payload(vector->second.at("expected_data_hex").get<std::string>());
    expect(frames[index].id == expected_id, "production codec emitted the wrong 29-bit arbitration ID");
    expect(frames[index].extended, "production codec emitted a non-extended CAN frame");
    expect(frames[index].dlc == 8U, "production codec emitted a non-eight-byte CAN frame");
    expect(frames[index].data == expected_data, "production codec payload differs from independent golden vector");
  }
}

void test_application_rejects_nonfinite_and_out_of_range_codec_inputs() {
  ParallelController controller;
  Command command;

  command.motor_torque_nm[0] = std::numeric_limits<double>::quiet_NaN();
  expect(!controller.set_command(command), "NaN torque reached the codec");

  command = Command{};
  command.brake_pressure_bar[0] = std::numeric_limits<double>::infinity();
  expect(!controller.set_command(command), "infinite brake pressure reached the codec");

  command = Command{};
  command.motor_torque_nm[0] = 838.4;
  expect(!controller.set_command(command), "out-of-range torque reached the codec");

  command = Command{};
  command.brake_pressure_bar[0] = 409.6;
  expect(!controller.set_command(command), "out-of-range brake pressure reached the codec");
}

}  // namespace

int main() {
  const std::vector<std::pair<std::string, void (*)()>> tests{
      {"ready_frames_match_independent_golden_vectors", test_ready_frames_match_independent_golden_vectors},
      {"application_rejects_nonfinite_and_out_of_range_codec_inputs",
       test_application_rejects_nonfinite_and_out_of_range_codec_inputs},
  };
  std::size_t passed = 0;
  for (const auto& [name, test] : tests) {
    try {
      test();
      ++passed;
      std::cout << "PASS " << name << '\n';
    } catch (const std::exception& error) {
      std::cerr << "FAIL " << name << ": " << error.what() << '\n';
      return 1;
    }
  }
  std::cout << "dbc_codec_contract_tests=passed cases=" << passed << '\n';
  return 0;
}
