#include "mine_teleop/core.hpp"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using mine_teleop::ControlCommand;
using mine_teleop::Json;

class TestFailure : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, std::string_view message) {
  if (!condition)
    throw TestFailure(std::string(message));
}

Json read_json(const std::filesystem::path& path) {
  std::ifstream input(path);
  if (!input)
    throw TestFailure("cannot open protocol contract input: " + path.string());
  try {
    return Json::parse(input);
  } catch (const Json::exception& error) {
    throw TestFailure("cannot parse protocol contract input " + path.string() + ": " +
                      error.what());
  }
}

[[nodiscard]] bool is_extension_keyword(std::string_view key) {
  return key.rfind("x-", 0) == 0;
}

void validate_schema_shape(const Json& schema) {
  const std::set<std::string> root_keywords{"$schema",     "$id",        "title",
                                            "description", "$comment",   "type",
                                            "required",    "properties", "additionalProperties"};
  const std::set<std::string> property_keywords{"title",     "description", "$comment", "type",
                                                "const",     "enum",        "minimum",  "maximum",
                                                "minLength", "default"};

  expect(schema.is_object(), "control-command schema is not an object");
  for (const auto& [key, ignored] : schema.items()) {
    static_cast<void>(ignored);
    expect(root_keywords.contains(key) || is_extension_keyword(key),
           "control-command schema uses an unsupported root keyword: " + key);
  }
  expect(schema.value("type", "") == "object", "control-command schema root must be an object");
  expect(schema.contains("required") && schema.at("required").is_array(),
         "schema required must be an array");
  expect(schema.contains("properties") && schema.at("properties").is_object(),
         "schema properties must be an object");
  expect(schema.contains("additionalProperties") &&
             schema.at("additionalProperties").is_boolean() &&
             schema.at("additionalProperties").get<bool>(),
         "v1 control-command schema must explicitly allow unknown fields");

  const auto& properties = schema.at("properties");
  for (const auto& required : schema.at("required")) {
    expect(required.is_string(), "schema required field must be a string");
    expect(properties.contains(required.get<std::string>()),
           "schema required field has no property definition");
  }
  for (const auto& [name, property] : properties.items()) {
    expect(property.is_object(), "schema property is not an object: " + name);
    expect(property.contains("type") && property.at("type").is_string(),
           "schema property has no type: " + name);
    for (const auto& [key, ignored] : property.items()) {
      static_cast<void>(ignored);
      expect(property_keywords.contains(key) || is_extension_keyword(key),
             "schema property uses an unsupported keyword: " + name + "." + key);
    }
  }
}

[[nodiscard]] bool matches_type(const Json& value, std::string_view expected) {
  if (expected == "object")
    return value.is_object();
  if (expected == "string")
    return value.is_string();
  if (expected == "boolean")
    return value.is_boolean();
  if (expected == "number") {
    return value.is_number() && std::isfinite(value.get<double>());
  }
  if (expected == "integer") {
    if (!value.is_number())
      return false;
    const auto numeric = value.get<double>();
    return std::isfinite(numeric) && std::trunc(numeric) == numeric;
  }
  throw TestFailure("unsupported JSON Schema type in contract test: " + std::string(expected));
}

[[nodiscard]] std::optional<std::string> validate_property(const Json& property, const Json& value,
                                                           std::string_view field) {
  const auto type = property.at("type").get<std::string>();
  if (!matches_type(value, type))
    return "wrong type for " + std::string(field);
  if (property.contains("const") && value != property.at("const")) {
    return "wrong const value for " + std::string(field);
  }
  if (property.contains("enum")) {
    bool matched = false;
    for (const auto& candidate : property.at("enum")) {
      if (value == candidate) {
        matched = true;
        break;
      }
    }
    if (!matched)
      return "value outside enum for " + std::string(field);
  }
  if (property.contains("minLength") &&
      value.get<std::string>().size() < property.at("minLength").get<std::size_t>()) {
    return "string shorter than minLength for " + std::string(field);
  }
  if (property.contains("minimum") && value.get<double>() < property.at("minimum").get<double>()) {
    return "number below minimum for " + std::string(field);
  }
  if (property.contains("maximum") && value.get<double>() > property.at("maximum").get<double>()) {
    return "number above maximum for " + std::string(field);
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<std::string> validate_message(const Json& schema, const Json& value) {
  if (!value.is_object())
    return "message is not an object";
  const auto& properties = schema.at("properties");
  for (const auto& required : schema.at("required")) {
    const auto field = required.get<std::string>();
    if (!value.contains(field))
      return "missing required field: " + field;
  }
  for (const auto& [field, field_value] : value.items()) {
    if (!properties.contains(field)) {
      if (!schema.at("additionalProperties").get<bool>())
        return "unknown field: " + field;
      continue;
    }
    if (const auto error = validate_property(properties.at(field), field_value, field))
      return error;
  }
  return std::nullopt;
}

struct ContractVector {
  std::string name;
  bool structural_valid{false};
  Json value;
};

std::vector<ContractVector> read_vectors(const Json& manifest,
                                         const std::filesystem::path& manifest_path) {
  expect(manifest.value("format", "") == "mine-teleop-control-command-contract-v1",
         "unexpected protocol contract manifest format");
  expect(manifest.contains("vectors") && manifest.at("vectors").is_array(),
         "manifest vectors must be an array");

  std::vector<ContractVector> vectors;
  for (const auto& entry : manifest.at("vectors")) {
    expect(entry.is_object(), "protocol vector entry is not an object");
    expect(entry.contains("name") && entry.at("name").is_string(), "protocol vector has no name");
    expect(entry.contains("structural_valid") && entry.at("structural_valid").is_boolean(),
           "protocol vector has no structural_valid boolean");
    const bool has_file = entry.contains("file");
    const bool has_value = entry.contains("value");
    expect(has_file != has_value, "protocol vector must have exactly one of file or value");

    Json value;
    if (has_file) {
      expect(entry.at("file").is_string(), "protocol vector file must be a string");
      value = read_json(manifest_path.parent_path() / entry.at("file").get<std::string>());
    } else {
      value = entry.at("value");
    }
    vectors.push_back({entry.at("name").get<std::string>(),
                       entry.at("structural_valid").get<bool>(), std::move(value)});
  }
  return vectors;
}

void run_contract_tests() {
  constexpr std::uint64_t kPortableJsonIntegerMaximum = 9007199254740991ULL;
  const auto manifest_path =
      std::filesystem::path("protocol/v1/fixtures/control-command-vectors.json");
  const auto manifest = read_json(manifest_path);
  expect(manifest.contains("schema") && manifest.at("schema").is_string(),
         "manifest schema path is missing");
  const auto schema =
      read_json(manifest_path.parent_path() / manifest.at("schema").get<std::string>());
  validate_schema_shape(schema);
  expect(schema.at("x-portable-json-integer-maximum").get<std::uint64_t>() ==
             kPortableJsonIntegerMaximum,
         "schema portable JSON integer maximum changed");
  const auto& schema_properties = schema.at("properties");
  expect(schema_properties.at("seq").at("maximum").get<std::uint64_t>() ==
                 kPortableJsonIntegerMaximum &&
             schema_properties.at("sent_at_utc_ms").at("maximum").get<std::uint64_t>() ==
                 kPortableJsonIntegerMaximum,
         "schema integer maxima must match the portable JSON integer maximum");
  expect(manifest.at("numeric_compatibility")
                 .at("portable_json_integer_maximum")
                 .get<std::uint64_t>() == kPortableJsonIntegerMaximum,
         "contract manifest portable JSON integer maximum changed");

  const auto vectors = read_vectors(manifest, manifest_path);
  std::size_t accepted = 0;
  for (const auto& vector : vectors) {
    const auto structural_error = validate_message(schema, vector.value);
    const bool structural_valid = !structural_error.has_value();
    expect(structural_valid == vector.structural_valid,
           "schema result disagrees with vector " + vector.name +
               (structural_error ? ": " + *structural_error : ""));
    if (!structural_valid)
      continue;

    ControlCommand command;
    try {
      command = ControlCommand::from_json(vector.value);
    } catch (const std::exception& error) {
      throw TestFailure("schema-valid vector was rejected by ControlCommand::from_json: " +
                        vector.name + ": " + error.what());
    }
    if (vector.value.contains("future_extension")) {
      expect(!command.to_json().contains("future_extension"),
             "unknown v1 extension leaked from command model: " + vector.name);
    }
    if (!vector.value.contains("estop")) {
      expect(!command.estop, "omitted v1 estop did not default to false: " + vector.name);
    }
    ++accepted;
  }
  expect(accepted > 0, "protocol contract has no valid vectors");
  std::cout << "protocol_v1_cpp_contract=passed vectors=" << vectors.size() << " valid=" << accepted
            << '\n';
}

}  // namespace

int main() {
  try {
    run_contract_tests();
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "protocol_v1_cpp_contract=failed error=" << error.what() << '\n';
    return 1;
  }
}
