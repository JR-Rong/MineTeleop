#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string_view>

namespace mine_teleop::detail {
// One bounded direct attempt per runtime/session. The caller must tear down
// through its normal control-safe boundary before starting a fresh Peer.
class IceRoutePolicy {
 public:
  IceRoutePolicy(int direct_timeout_ms, bool has_turn, bool force_relay)
      : timeout_(direct_timeout_ms), has_turn_(has_turn),
        direct_(direct_timeout_ms > 0 && has_turn && !force_relay), force_relay_(force_relay) {
    if (direct_timeout_ms < 0 || direct_timeout_ms > 30000)
      throw std::invalid_argument("ICE direct timeout must be 0..30000 ms");
  }
  std::string_view mode() const { return force_relay_ ? "relay" : direct_ ? "direct" : "all"; }
  void begin_attempt() { answer_at_.reset(); connected_ = false; }
  bool allow_relay_retry(std::int64_t now, bool answer_applied, bool connected, bool failed) {
    if (!direct_ || !has_turn_ || now < 0) return false;
    if (connected) connected_ = true;
    if (!answer_applied) return false; // Never classify missing signaling as a NAT failure.
    if (!answer_at_) answer_at_ = now;
    if (!failed && (connected_ || now < *answer_at_ || now - *answer_at_ < timeout_)) return false;
    direct_ = false;
    ++fallbacks_;
    return true;
  }
  unsigned fallbacks() const { return fallbacks_; }
 private:
  int timeout_;
  bool has_turn_, direct_, force_relay_, connected_{false};
  unsigned fallbacks_{0};
  std::optional<std::int64_t> answer_at_;
};
}  // namespace mine_teleop::detail
