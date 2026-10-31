#pragma once

#include "mine_teleop/detail/control_transport.hpp"
#include "mine_teleop/detail/native_control_mailbox.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

namespace mine_teleop::detail {

struct NativeCourierScope {
  NativeMediaScope media;
  std::string owner;
  std::uint64_t owner_epoch{};
  bool operator==(const NativeCourierScope&) const = default;
};

// The bridge owns signing, authentication, transport selection and the input
// lease. This filter only guards its short-lived output before a DC send.
// One instance belongs to one media attempt and one input owner; replace it
// on any scope change. A returned frame may be attempted once only. The owner
// must serialize take, the channel send and retirement against scope changes.
class NativeControlPublication {
 public:
  explicit NativeControlPublication(NativeCourierScope scope) : scope_(std::move(scope)) {
    if (scope_.media.session.empty() || scope_.media.generation == 0 || scope_.media.attempt.empty() ||
        scope_.owner.empty() || scope_.owner_epoch == 0) throw std::invalid_argument("invalid courier scope");
  }
  std::optional<std::string> take(const NativeCourierScope& current, const nlohmann::json& response,
                                  bool direct_ready, std::int64_t elapsed_ms) {
    if (retired_) return std::nullopt;
    if (current != scope_) { retire(); return std::nullopt; }
    if (!response.is_object()) return std::nullopt;
    try {
      if (response.value("protocol", "") != kControlTransportCapability ||
          !response.contains("frame") || !response.at("frame").is_object()) return std::nullopt;
      const auto& frame = response.at("frame");
      if (!frame.contains("seq") || !frame.at("seq").is_number_unsigned()) return std::nullopt;
      const auto seq = frame.at("seq").get<std::uint64_t>();
      // Browser courier accepts only JavaScript-safe integers. Preserve the
      // same bound until all transport participants support a wider sequence.
      if (seq == 0 || seq > 9007199254740991ULL || seq <= last_seen_) return std::nullopt;
      last_seen_ = seq;
      if (!direct_ready || elapsed_ms < 0 || response.value("enabled", false) != true ||
          !response.contains("valid_for_ms") || !response.at("valid_for_ms").is_number_integer() ||
          response.at("valid_for_ms").get<std::int64_t>() <= elapsed_ms ||
          frame.value("session_id", "") != scope_.media.session ||
          !frame.contains("control_transport") || !frame.at("control_transport").is_object()) return std::nullopt;
      const auto& transport = frame.at("control_transport");
      if (transport.value("protocol", "") != kControlTransportCapability || transport.value("path", "") != "dc" ||
          transport.value("media_attempt", "") != scope_.media.attempt ||
          !response.contains("frame_json") || !response.at("frame_json").is_string()) return std::nullopt;
      const auto bytes = response.at("frame_json").get<std::string>();
      if (bytes.empty() || bytes.size() > 64 * 1024 || bytes.find('\0') != std::string::npos ||
          nlohmann::json::parse(bytes) != frame) return std::nullopt;
      return bytes;
    } catch (const nlohmann::json::exception&) { return std::nullopt; }
  }
  void retire() { retired_ = true; }
  [[nodiscard]] std::uint64_t last_seen() const { return last_seen_; }
 private:
  const NativeCourierScope scope_;
  std::uint64_t last_seen_{};
  bool retired_{};
};
}  // namespace mine_teleop::detail
