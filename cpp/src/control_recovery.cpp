#include "mine_teleop/media.hpp"

#include <stdexcept>
#include <algorithm>

namespace mine_teleop {

ReadinessRevisionChanges ReadinessRevisionFence::observe(
    const std::vector<ReadinessSampleRevision>& sources) {
  if (sources.size() > 64) throw std::invalid_argument("too many readiness producers");
  ReadinessRevisionChanges changes;
  if (initialized_) {
    changes.bad = sources.size() != sources_.size();
    for (const auto& current : sources) {
      const auto previous = std::find_if(sources_.begin(), sources_.end(),
          [&](const auto& item) { return item.source == current.source; });
      if (previous == sources_.end()) {
        changes.bad = true;
        changes.brake = changes.brake || current.brake != 0;
      } else {
        changes.bad = changes.bad || previous->bad != current.bad;
        changes.brake = changes.brake || previous->brake != current.brake;
      }
    }
  }
  initialized_ = true;
  sources_ = sources;
  return changes;
}

bool SessionControlRecoveryLatch::enter_session(std::string_view session_id) {
  if (session_id.empty()) {
    throw std::invalid_argument("critical camera control latch requires a non-empty session id");
  }
  std::lock_guard lock(mutex_);
  if (session_id_ != session_id) {
    session_id_ = session_id;
    inhibited_ = false;
    armed_ = false;
    startup_started_ms_.reset();
    braking_started_ms_.reset();
    braking_timeout_ms_ = 0;
    recovered_started_ms_.reset();
    cause_ = ControlRecoveryCause::CriticalVideo;
    revisions_.reset();
  }
  return inhibited_;
}

bool SessionControlRecoveryLatch::inhibit(std::string_view session_id) {
  if (session_id.empty()) {
    throw std::invalid_argument("critical camera control latch requires a non-empty session id");
  }
  std::lock_guard lock(mutex_);
  if (session_id_.empty()) {
    throw std::logic_error("critical camera control latch session was not entered");
  }
  if (session_id_ != session_id) {
    throw std::logic_error("critical camera control latch session does not match the active session");
  }
  const bool first_inhibition = !inhibited_;
  inhibited_ = true;
  return first_inhibition;
}

bool SessionControlRecoveryLatch::inhibited_for(std::string_view session_id) const {
  if (session_id.empty()) {
    throw std::invalid_argument("critical camera control latch requires a non-empty session id");
  }
  std::lock_guard lock(mutex_);
  if (session_id_ != session_id) {
    throw std::logic_error("critical camera control latch session does not match the active session");
  }
  return inhibited_;
}

bool SessionControlRecoveryLatch::startup_grace_active(std::string_view session_id, std::int64_t now_ms) {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id || now_ms < 0) {
    throw std::logic_error("invalid critical camera startup scope or time");
  }
  if (!startup_started_ms_) startup_started_ms_ = now_ms;
  return !inhibited_ && !armed_ && now_ms >= *startup_started_ms_ &&
      now_ms - *startup_started_ms_ < kCriticalCameraStartupTimeoutMs;
}

bool SessionControlRecoveryLatch::arm_for_control(std::string_view session_id) {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id) {
    throw std::logic_error("critical camera control scope does not match the active session");
  }
  if (inhibited_) return false;
  armed_ = true;
  return true;
}

ControlRecoveryPhase SessionControlRecoveryLatch::observe_recovery(
    std::string_view session_id, bool ready, std::int64_t now_ms, int recovery_timeout_ms,
    ControlRecoveryCause cause) {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id || now_ms < 0 || recovery_timeout_ms <= 0)
    throw std::logic_error("invalid critical-video protection scope or time");
  return observe_recovery_locked(ready, now_ms, recovery_timeout_ms, cause);
}

ControlRecoveryPhase SessionControlRecoveryLatch::observe_readiness(
    std::string_view session_id, bool ready, std::int64_t now_ms, int recovery_timeout_ms,
    ControlRecoveryCause cause, const std::vector<ReadinessSampleRevision>& sources) {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id || now_ms < 0 || recovery_timeout_ms <= 0)
    throw std::logic_error("invalid control readiness scope or time");
  const auto changes = revisions_.observe(sources);
  if (changes.bad) recovered_started_ms_.reset();
  // A third bad sample or independent absence cannot disappear when a newer
  // normal snapshot overwrites it before the next control observer runs.
  return observe_recovery_locked(ready && !changes.brake, now_ms, recovery_timeout_ms, cause);
}

ControlRecoveryPhase SessionControlRecoveryLatch::observe_recovery_locked(
    bool ready, std::int64_t now_ms, int recovery_timeout_ms, ControlRecoveryCause cause) {
  if (inhibited_) return ControlRecoveryPhase::Locked;
  if (!ready && !braking_started_ms_) {
    braking_started_ms_ = now_ms;
    braking_timeout_ms_ = recovery_timeout_ms;
    cause_ = cause;
  }
  if (!braking_started_ms_) return ControlRecoveryPhase::Ready;
  // Readiness may alternate or remain unavailable beyond the former recovery
  // deadline. The independent command/feedback watchdog still owns genuine
  // stream loss; elapsed waiting alone never revokes an authenticated session.
  if (!ready) recovered_started_ms_.reset();
  else if (!recovered_started_ms_) recovered_started_ms_ = now_ms;
  else if (now_ms - *recovered_started_ms_ >= kControlRecoveryStableMs) {
    braking_started_ms_.reset();
    braking_timeout_ms_ = 0;
    recovered_started_ms_.reset();
    return ControlRecoveryPhase::Recovered;
  }
  return ControlRecoveryPhase::BrakingWait;
}

bool SessionControlRecoveryLatch::braking_wait_for(std::string_view session_id) const {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id)
    throw std::logic_error("critical-video protection session mismatch");
  return braking_started_ms_.has_value() && !inhibited_;
}

ControlRecoveryCause SessionControlRecoveryLatch::cause_for(std::string_view session_id) const {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id)
    throw std::logic_error("control recovery session mismatch");
  return cause_;
}

std::optional<std::int64_t> SessionControlRecoveryLatch::deadline_for(std::string_view session_id) const {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id) throw std::logic_error("control recovery session mismatch");
  return std::nullopt; // Compatibility field; no readiness-driven session deadline.
}

Json SessionControlRecoveryLatch::recovery_status_for(std::string_view session_id) const {
  std::lock_guard lock(mutex_);
  if (session_id.empty() || session_id_ != session_id) throw std::logic_error("control recovery session mismatch");
  return {{"cause", to_string(cause_)}, {"phase", inhibited_ ? "locked" : braking_started_ms_ ? "braking_wait" : "ready"},
          {"timeout_ms", nullptr}, {"legacy_timeout_ms", braking_timeout_ms_},
          {"deadline_monotonic_ms", nullptr}, {"session_retained", !inhibited_},
          {"healthy_window_started_monotonic_ms", recovered_started_ms_ ? Json(*recovered_started_ms_) : Json(nullptr)},
          {"healthy_window_required_ms", kControlRecoveryStableMs},
          {"next_action", inhibited_ ? "inspect_explicit_safety_failure" : braking_started_ms_ ? "wait_for_all_required_conditions" : "release_then_press_again"}};
}


} // namespace mine_teleop
