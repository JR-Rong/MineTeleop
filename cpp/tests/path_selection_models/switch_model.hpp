#pragma once
#include "common.hpp"
#include "mine_teleop/detail/lowest_latency_path_policy.hpp"

#include <limits>
#include <stdexcept>

namespace mine_teleop::path_selection_models {
// Test-only transaction lifecycle around advisory recommendations. It neither
// changes the runtime path nor decides whether cloud loss permits control.
class SwitchModel {
 public:
  using Path = detail::NetworkPath;
  enum class Axis { Control, Feedback };
  enum class Phase { Idle, WaitingNeutral, AwaitBarrier, AwaitVehicleResults, SafetyHold };
  enum class Evidence { VehicleControlResult, Probe, CloudIngress, IceRtt };
  struct Proposal {
    Authority authority;
    Axis axis;
    Path path;
    std::uint64_t attempt{}, feedback_cohort{};
    bool selectable{}, candidate_healthy{}, active_failed{};
    Preconditions prerequisites;
  };
  struct Barrier {
    Authority authority;
    std::uint64_t ticket{}, epoch{}, attempt{};
    Axis axis;
    Path path;
    Preconditions prerequisites;
  };
  struct Result {
    Authority authority;
    std::uint64_t ticket{}, command_seq{}, intent_seq{}, attempt{}, feedback_cohort{};
    Path path;
    Evidence evidence;
    bool matched_native_send{}, accepted{}, fresh{}, neutral{};
    Preconditions prerequisites;
  };
  SwitchModel(Authority authority, std::size_t required_results)
      : authority_(std::move(authority)), required_results_(required_results) {
    if (!authority_.valid() || !required_results_) throw std::invalid_argument("explicit scope and result count required");
  }
  std::optional<std::uint64_t> propose(const Proposal& proposal, bool operator_neutral) {
    if (phase_ != Phase::Idle || proposal.authority != authority_ || !proposal.prerequisites.satisfied() ||
        !proposal.attempt || !proposal.selectable || !proposal.candidate_healthy ||
        proposal.feedback_cohort != feedback_cohort_ || ticket_ == std::numeric_limits<std::uint64_t>::max()) return {};
    const auto index = proposal.axis == Axis::Control ? 0U : 1U;
    if ((paths_[index] == proposal.path && attempts_[index] == proposal.attempt) ||
        epochs_[index] == std::numeric_limits<std::uint64_t>::max() ||
        (proposal.axis == Axis::Feedback && feedback_cohort_ == std::numeric_limits<std::uint64_t>::max())) return {};
    proposal_ = proposal; ++ticket_; qualified_ = 0; last_result_seq_ = 0;
    const bool control = proposal.axis == Axis::Control;
    phase_ = control && !operator_neutral && !proposal.active_failed ? Phase::WaitingNeutral : Phase::AwaitBarrier;
    withdraw_motion_ = proposal.active_failed || (control && phase_ != Phase::WaitingNeutral);
    return ticket_;
  }
  bool neutral_boundary(std::uint64_t ticket) {
    if (ticket != ticket_ || phase_ != Phase::WaitingNeutral) return false;
    phase_ = Phase::AwaitBarrier; withdraw_motion_ = true; return true;
  }
  bool barrier(const Barrier& value) {
    if (phase_ != Phase::AwaitBarrier || value.authority != authority_ || value.ticket != ticket_ ||
        !value.prerequisites.satisfied() || value.axis != proposal_->axis || value.path != proposal_->path ||
        value.attempt != proposal_->attempt) return false;
    const auto index = value.axis == Axis::Control ? 0U : 1U;
    if (value.epoch != epochs_[index] + 1) return false;
    phase_ = Phase::AwaitVehicleResults; return true;
  }
  bool result(const Result& value) {
    if (phase_ != Phase::AwaitVehicleResults || value.authority != authority_ || value.ticket != ticket_ ||
        !value.prerequisites.satisfied() || value.evidence != Evidence::VehicleControlResult || !value.matched_native_send ||
        !value.command_seq || !value.intent_seq || value.command_seq <= last_result_seq_ ||
        value.attempt != proposal_->attempt || value.path != proposal_->path || value.feedback_cohort != feedback_cohort_) return false;
    last_result_seq_ = value.command_seq;
    if (!value.accepted || !value.fresh || (proposal_->axis == Axis::Control && !value.neutral)) { qualified_ = 0; return false; }
    if (++qualified_ < required_results_) return false;
    const auto index = proposal_->axis == Axis::Control ? 0U : 1U;
    paths_[index] = proposal_->path; attempts_[index] = proposal_->attempt; ++epochs_[index];
    if (proposal_->axis == Axis::Feedback) ++feedback_cohort_;
    phase_ = Phase::Idle; proposal_.reset(); withdraw_motion_ = false;
    return true; // Caller still owes the existing runtime recovery/repress gates.
  }
  void interrupt_for_safety() {
    if (ticket_ != std::numeric_limits<std::uint64_t>::max()) ++ticket_;
    phase_ = Phase::SafetyHold; proposal_.reset(); qualified_ = 0; withdraw_motion_ = true;
    // No automatic resume or new route claim before external reconciliation.
  }
  Phase phase() const { return phase_; }
  bool withdraw_motion() const { return withdraw_motion_; }
  std::uint64_t feedback_cohort() const { return feedback_cohort_; }
  Path confirmed_path(Axis axis) const { return paths_[axis == Axis::Control ? 0U : 1U]; }
 private:
  Authority authority_;
  std::size_t required_results_, qualified_{};
  std::uint64_t ticket_{}, feedback_cohort_{1}, last_result_seq_{};
  std::array<Path, 2> paths_{Path::CloudWss, Path::CloudWss};
  std::array<std::uint64_t, 2> attempts_{1, 1}, epochs_{};
  Phase phase_{Phase::Idle};
  bool withdraw_motion_{};
  std::optional<Proposal> proposal_;
};
}  // namespace mine_teleop::path_selection_models
