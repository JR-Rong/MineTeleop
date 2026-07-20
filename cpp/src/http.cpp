#include "mine_teleop/http.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace mine_teleop {
namespace {

class CurlGlobal {
 public:
  CurlGlobal() {
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
      throw std::runtime_error("curl_global_init failed");
    }
  }
  ~CurlGlobal() { curl_global_cleanup(); }
};

void ensure_curl_global() {
  static CurlGlobal global;
  static_cast<void>(global);
}

std::size_t append_body(char* data, std::size_t size, std::size_t count, void* output) {
  const auto bytes = size * count;
  static_cast<std::string*>(output)->append(data, bytes);
  return bytes;
}

Json decode_json_response(const HttpResponse& response) {
  if (response.status < 200 || response.status >= 300) {
    throw std::runtime_error("HTTP request failed with status " + std::to_string(response.status) + ": " +
                             response.body.substr(0, 512));
  }
  try {
    auto value = Json::parse(response.body);
    if (!value.is_object()) throw std::runtime_error("expected JSON object response");
    return value;
  } catch (const Json::exception& error) {
    throw std::runtime_error(std::string("invalid JSON response: ") + error.what());
  }
}

}  // namespace

HttpClient::HttpClient(std::chrono::milliseconds timeout) : timeout_(timeout) {
  if (timeout_.count() <= 0) throw std::invalid_argument("HTTP timeout must be positive");
  ensure_curl_global();
}

HttpResponse HttpClient::get(std::string_view url) const { return request("GET", url, ""); }

HttpResponse HttpClient::post_json(std::string_view url, const Json& payload) const {
  return request("POST", url, payload.dump());
}

Json HttpClient::get_json(std::string_view url) const { return decode_json_response(get(url)); }

Json HttpClient::post_json_response(std::string_view url, const Json& payload) const {
  return decode_json_response(post_json(url, payload));
}

std::string HttpClient::url_encode(std::string_view value) const {
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");
  char* encoded = curl_easy_escape(curl, value.data(), static_cast<int>(value.size()));
  if (encoded == nullptr) {
    curl_easy_cleanup(curl);
    throw std::runtime_error("curl_easy_escape failed");
  }
  std::string result(encoded);
  curl_free(encoded);
  curl_easy_cleanup(curl);
  return result;
}

HttpResponse HttpClient::request(std::string_view method, std::string_view url, std::string_view body) const {
  if (!url.starts_with("http://") && !url.starts_with("https://")) {
    throw std::invalid_argument("HTTP URL must use http or https");
  }
  CURL* curl = curl_easy_init();
  if (curl == nullptr) throw std::runtime_error("curl_easy_init failed");
  HttpResponse response;
  curl_slist* headers = nullptr;
  try {
    curl_easy_setopt(curl, CURLOPT_URL, std::string(url).c_str());
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_.count()));
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(std::min<std::int64_t>(timeout_.count(), 3000)));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "mine-teleop-cpp/0.2");
    if (method == "POST") {
      headers = curl_slist_append(headers, "Content-Type: application/json");
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_POST, 1L);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.data());
      curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    }
    const auto result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
      throw std::runtime_error(std::string("HTTP request failed: ") + curl_easy_strerror(result));
    }
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.status);
  } catch (...) {
    if (headers != nullptr) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    throw;
  }
  if (headers != nullptr) curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  return response;
}

Json TimeSyncStatus::to_json() const {
  return {
      {"time_domain", "signaling_server"},
      {"synchronized", synchronized},
      {"offset_ms", offset_ms},
      {"round_trip_ms", round_trip_ms},
      {"uncertainty_ms", uncertainty_ms},
      {"synchronized_at_local_ms", synchronized_at_local_ms},
      {"sample_count", sample_count},
  };
}

bool TimeSyncStatus::acceptable(int max_uncertainty_ms) const {
  return max_uncertainty_ms >= 0 && synchronized && uncertainty_ms <= max_uncertainty_ms;
}

TimeSyncStatus SynchronizedClock::synchronize(
    const HttpClient& http, std::string_view signaling_origin, int sample_count) {
  if (sample_count < 3 || sample_count > 15) throw std::invalid_argument("time sync sample count must be between 3 and 15");
  const auto origin = normalize_signaling_http_url(signaling_origin);
  struct Sample {
    std::int64_t offset_ms;
    std::int64_t round_trip_ms;
  };
  std::vector<Sample> samples;
  samples.reserve(static_cast<std::size_t>(sample_count));
  for (int index = 0; index < sample_count; ++index) {
    const auto client_send_ms = mine_teleop::now_ms();
    const auto response = http.get_json(
        origin + "/time?client_send_ms=" + std::to_string(client_send_ms));
    const auto client_receive_ms = mine_teleop::now_ms();
    const auto echoed_client_send_ms = response.at("client_send_ms").get<std::int64_t>();
    const auto server_receive_ms = response.at("server_receive_ms").get<std::int64_t>();
    const auto server_send_ms = response.at("server_send_ms").get<std::int64_t>();
    if (echoed_client_send_ms != client_send_ms || server_send_ms < server_receive_ms) {
      throw std::runtime_error("signaling time endpoint returned an invalid four-timestamp sample");
    }
    const auto server_processing_ms = server_send_ms - server_receive_ms;
    const auto round_trip_ms = std::max<std::int64_t>(0, client_receive_ms - client_send_ms - server_processing_ms);
    const auto offset_ms = ((server_receive_ms - client_send_ms) + (server_send_ms - client_receive_ms)) / 2;
    samples.push_back({offset_ms, round_trip_ms});
  }
  std::sort(samples.begin(), samples.end(), [](const auto& left, const auto& right) {
    return left.round_trip_ms < right.round_trip_ms;
  });
  const auto selected_count = std::min<std::size_t>(3, samples.size());
  std::vector<std::int64_t> offsets;
  offsets.reserve(selected_count);
  for (std::size_t index = 0; index < selected_count; ++index) offsets.push_back(samples[index].offset_ms);
  std::sort(offsets.begin(), offsets.end());
  const auto selected_offset_ms = offsets[offsets.size() / 2];
  std::int64_t offset_spread_ms = 0;
  for (const auto offset_ms : offsets) {
    offset_spread_ms = std::max(offset_spread_ms, std::abs(offset_ms - selected_offset_ms));
  }
  TimeSyncStatus next{
      true,
      selected_offset_ms,
      samples.front().round_trip_ms,
      std::max<std::int64_t>((samples.front().round_trip_ms + 1) / 2, offset_spread_ms),
      mine_teleop::now_ms(),
      static_cast<int>(samples.size()),
  };

  const auto next_steady_anchor = std::chrono::steady_clock::now();
  const auto proposed_synchronized_ms = next.synchronized_at_local_ms + next.offset_ms;
  std::lock_guard lock(mutex_);
  if (status_.synchronized) {
    const auto current_synchronized_ms = synchronized_anchor_ms_ +
        std::chrono::duration_cast<std::chrono::milliseconds>(next_steady_anchor - steady_anchor_).count();
    synchronized_anchor_ms_ = std::max(current_synchronized_ms, proposed_synchronized_ms);
  } else {
    synchronized_anchor_ms_ = proposed_synchronized_ms;
  }
  steady_anchor_ = next_steady_anchor;
  status_ = next;
  return status_;
}

std::int64_t SynchronizedClock::now_ms() const {
  std::lock_guard lock(mutex_);
  if (!status_.synchronized) return mine_teleop::now_ms();
  return synchronized_anchor_ms_ +
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - steady_anchor_).count();
}

std::int64_t SynchronizedClock::from_local_system_ms(std::int64_t local_time_ms) const {
  std::lock_guard lock(mutex_);
  return status_.synchronized ? local_time_ms + status_.offset_ms : local_time_ms;
}

TimeSyncStatus SynchronizedClock::status() const {
  std::lock_guard lock(mutex_);
  return status_;
}

bool SynchronizedClock::refresh_due(int interval_ms) const {
  if (interval_ms <= 0) throw std::invalid_argument("time sync refresh interval must be positive");
  std::lock_guard lock(mutex_);
  return !status_.synchronized ||
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - steady_anchor_).count() >=
          interval_ms;
}

std::string normalize_signaling_http_url(std::string_view url) {
  std::string value(url);
  if (value.starts_with("ws://")) value.replace(0, 5, "http://");
  if (value.starts_with("wss://")) value.replace(0, 6, "https://");
  if (value.ends_with("/signaling")) value.resize(value.size() - std::string_view("/signaling").size());
  while (!value.empty() && value.back() == '/') value.pop_back();
  if (!value.starts_with("http://") && !value.starts_with("https://")) {
    throw std::invalid_argument("signaling URL must use ws, wss, http, or https");
  }
  return value;
}

VehicleTeleopRuntime::VehicleTeleopRuntime(
    VehicleConfig config,
    std::string signaling_url,
    std::string device_token,
    int telemetry_interval_ms)
    : config_(std::move(config)),
      signaling_http_url_(normalize_signaling_http_url(signaling_url)),
      device_token_(std::move(device_token)),
      telemetry_interval_ms_(telemetry_interval_ms) {
  if (device_token_.empty()) throw std::invalid_argument("device token is required");
  if (telemetry_interval_ms_ <= 0) throw std::invalid_argument("telemetry interval must be positive");
}

VehicleTeleopRuntime::~VehicleTeleopRuntime() {
  try {
    if (service_) service_->close();
  } catch (...) {
  }
}

TimeSyncStatus VehicleTeleopRuntime::refresh_time_sync() {
  const auto status = clock_.synchronize(
      http_, signaling_http_url_, config_.field_safety.time_sync_samples);
  if (config_.field_safety.require_time_sync &&
      !status.acceptable(config_.field_safety.max_time_sync_uncertainty_ms)) {
    throw std::runtime_error(
        "vehicle time synchronization uncertainty " + std::to_string(status.uncertainty_ms) +
        "ms exceeds limit " + std::to_string(config_.field_safety.max_time_sync_uncertainty_ms) + "ms");
  }
  return status;
}

Json VehicleTeleopRuntime::register_online() {
  return http_.post_json_response(
      signaling_http_url_ + "/vehicles/online",
      {{"vehicle_id", config_.vehicle_id}, {"device_token", device_token_}});
}

Json VehicleTeleopRuntime::register_offline() {
  return http_.post_json_response(
      signaling_http_url_ + "/vehicles/offline",
      {{"vehicle_id", config_.vehicle_id}, {"device_token", device_token_}});
}

bool VehicleTeleopRuntime::discover_session(std::int64_t timestamp_ms) {
  const auto response = http_.get_json(
      signaling_http_url_ + "/vehicles/" + http_.url_encode(config_.vehicle_id) + "/session?device_token=" +
      http_.url_encode(device_token_));
  const auto next_session = response.value("session_id", "");
  if (next_session.empty()) return false;
  if (next_session != session_id_) start_session(next_session, timestamp_ms);
  return true;
}

void VehicleTeleopRuntime::start_session(std::string session_id, std::int64_t timestamp_ms) {
  if (service_) service_->close();
  session_id_ = std::move(session_id);
  service_ = std::make_unique<VehicleControlService>(
      config_, session_id_, "", create_vehicle_adapter(config_), telemetry_interval_ms_);
  service_->start(timestamp_ms);
}

Json VehicleTeleopRuntime::poll_and_execute(std::int64_t timestamp_ms) {
  if (!service_ || session_id_.empty()) throw std::runtime_error("vehicle teleop runtime has no active session");
  const auto response = http_.get_json(
      signaling_http_url_ + "/signaling/" + http_.url_encode(session_id_) + "/messages?recipient=" +
      http_.url_encode(config_.vehicle_id) + "&device_token=" + http_.url_encode(device_token_) +
      "&types=control_command");
  const auto messages = response.value("messages", Json::array());
  if (!messages.is_array()) throw std::runtime_error("signaling messages response must contain a messages list");
  std::uint64_t received = 0;
  std::uint64_t applied = 0;
  Json current_logs = Json::array();
  for (const auto& message : messages) {
    if (!message.is_object() || message.value("type", "") != "control_command") continue;
    ++received;
    auto command = ControlCommand::from_json(message.at("payload"));
    auto result = service_->receive_command(command, timestamp_ms);
    if (!result.accepted || !result.command) continue;
    ++applied;
    ++processed_control_commands_;
    last_applied_command_ = *result.command;
    Json record = {
        {"event", "vehicle_control_command_received"},
        {"vehicle_id", command.vehicle_id},
        {"session_id", command.session_id},
        {"seq", command.seq},
        {"command_ts_ms", command.ts_ms},
        {"receive_time_ms", timestamp_ms},
        {"control_latency_ms", std::max<std::int64_t>(0, timestamp_ms - command.ts_ms)},
        {"gear", command.gear},
        {"steering", command.steering},
        {"throttle", command.throttle},
        {"brake", command.brake},
        {"estop", command.estop},
    };
    current_logs.push_back(record);
    control_receive_logs_.push_back(std::move(record));
  }
  service_->tick(timestamp_ms);
  return {
      {"received_control_commands", received},
      {"applied_control_commands", applied},
      {"safety_state", to_string(service_->safety_state())},
      {"control_receive_logs", std::move(current_logs)},
  };
}

Json VehicleTeleopRuntime::run(int duration_ms, int poll_interval_ms, int session_wait_ms, bool log_controls) {
  if (duration_ms < 0 || poll_interval_ms <= 0 || session_wait_ms < 0) {
    throw std::invalid_argument("teleop timing options are invalid");
  }
  try {
    static_cast<void>(refresh_time_sync());
  } catch (...) {
    if (config_.field_safety.require_time_sync) throw;
  }
  register_online();
  const auto session_deadline = clock_.now_ms() + session_wait_ms;
  while (!discover_session(clock_.now_ms())) {
    if (session_wait_ms > 0 && clock_.now_ms() >= session_deadline) {
      return {
          {"event", "vehicle_teleop_run"},
          {"vehicle_id", config_.vehicle_id},
          {"session_discovered", false},
          {"reason", "no_active_session"},
      };
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
  }
  const auto deadline = clock_.now_ms() + duration_ms;
  while (duration_ms == 0 || clock_.now_ms() < deadline) {
    if (clock_.refresh_due(config_.field_safety.time_sync_interval_ms)) {
      try {
        static_cast<void>(refresh_time_sync());
      } catch (...) {
        if (config_.field_safety.require_time_sync) throw;
      }
    }
    auto result = poll_and_execute(clock_.now_ms());
    if (log_controls) {
      for (const auto& record : result["control_receive_logs"]) std::cout << record.dump() << '\n';
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(poll_interval_ms));
  }
  auto value = summary();
  value["session_discovered"] = true;
  return value;
}

Json VehicleTeleopRuntime::summary() const {
  Json logs = Json::array();
  const auto first = control_receive_logs_.size() > 20 ? control_receive_logs_.size() - 20 : 0;
  for (std::size_t index = first; index < control_receive_logs_.size(); ++index) logs.push_back(control_receive_logs_[index]);
  Json result = {
      {"event", "vehicle_teleop_run"},
      {"vehicle_id", config_.vehicle_id},
      {"session_id", session_id_},
      {"processed_control_commands", processed_control_commands_},
      {"last_command", last_applied_command_ ? last_applied_command_->to_json() : Json(nullptr)},
      {"time_sync", clock_.status().to_json()},
      {"control_receive_logs", std::move(logs)},
  };
  if (service_) {
    result["safety_state"] = to_string(service_->safety_state());
    result["telemetry_count"] = service_->telemetry_history().size();
    result["vehicle_adapter"] = service_->adapter_status().to_json();
  } else {
    result["safety_state"] = "INIT";
    result["telemetry_count"] = 0;
  }
  return result;
}

}  // namespace mine_teleop
