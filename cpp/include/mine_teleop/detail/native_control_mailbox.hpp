#pragma once

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace mine_teleop::detail {

struct NativeMediaScope {
  std::string session;
  std::uint64_t generation{};
  std::string attempt;
  bool operator==(const NativeMediaScope&) const = default;
};

// A bounded inbound courier for one negotiated media attempt. This class does
// not assert that a channel is open, that its ICE pair is direct, or that a
// message is authenticated. The runtime's transport bridge must validate the
// message and the selected pair before any command can be published.
class NativeControlMailbox {
 public:
  explicit NativeControlMailbox(NativeMediaScope scope) : scope_(std::move(scope)) {
    if (scope_.session.empty() || scope_.session.size() > 128 || scope_.generation == 0 ||
        scope_.attempt.empty() || scope_.attempt.size() > 128)
      throw std::invalid_argument("invalid native media scope");
  }

  bool attach(const NativeMediaScope& scope, std::string_view label, std::string_view protocol,
              bool ordered, int max_retransmits) {
    std::lock_guard lock(mutex_);
    if (scope != scope_ || retired_ || failed_ || label != "control" ||
        protocol != "mine-teleop-control-v1" || ordered || max_retransmits != 0) return false;
    if (attached_) { fail_locked(); return false; }
    attached_ = true;
    return true;
  }

  bool receive(const NativeMediaScope& scope, std::string_view payload) {
    std::lock_guard lock(mutex_);
    if (scope != scope_ || retired_ || failed_ || !attached_) return false;
    if (payload.empty() || payload.size() > 64 * 1024 || queue_.size() >= 64) {
      fail_locked();
      return false;
    }
    queue_.emplace_back(payload);
    return true;
  }

  std::optional<std::string> pop(const NativeMediaScope& scope) {
    std::lock_guard lock(mutex_);
    if (scope != scope_ || retired_ || failed_ || !attached_ || queue_.empty()) return std::nullopt;
    auto result = std::move(queue_.front());
    queue_.pop_front();
    return result;
  }

  void retire() {
    std::lock_guard lock(mutex_);
    retired_ = true;
    attached_ = false;
    queue_.clear();
  }

  [[nodiscard]] bool usable() const {
    std::lock_guard lock(mutex_);
    return attached_ && !retired_ && !failed_;
  }

 private:
  void fail_locked() { failed_ = true; attached_ = false; queue_.clear(); }
  const NativeMediaScope scope_;
  mutable std::mutex mutex_;
  std::deque<std::string> queue_;
  bool attached_{false}, retired_{false}, failed_{false};
};

}  // namespace mine_teleop::detail
