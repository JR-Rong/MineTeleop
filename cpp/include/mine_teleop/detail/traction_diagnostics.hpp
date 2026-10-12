#pragma once

#include "mine_teleop/vcu.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace mine_teleop::diagnostics {

// These observations never participate in control or safety decisions.
inline constexpr std::int64_t kTractionObservationDelayMs = 1000;
inline constexpr std::int64_t kDiagnosticMinimumIntervalMs = 1000;
inline constexpr std::int64_t kDiagnosticRepeatIntervalMs = 5000;
inline constexpr std::int64_t kDiagnosticMaximumSampleGapMs = 500;
inline constexpr double kMinimumObservedTorqueNm = 10.0;
inline constexpr double kNearZeroFeedbackTorqueNm = 2.0;

class StateLogLimiter {
 public:
  bool due(std::uint64_t key, std::int64_t now_ms) {
    if (seen_ && (now_ms - last_ms_ < kDiagnosticMinimumIntervalMs ||
                  (key == last_key_ &&
                   now_ms - last_ms_ < kDiagnosticRepeatIntervalMs))) return false;
    seen_ = true;
    last_ms_ = now_ms;
    last_key_ = key;
    return true;
  }

 private:
  bool seen_{false};
  std::int64_t last_ms_{0};
  std::uint64_t last_key_{0};
};

struct SentActuation {
  std::array<double, vcu::kMotorCount> torque_nm{};
  std::array<int, vcu::kMotorCount> motor_command{};
  std::array<int, vcu::kMotorCount> motor_mode{};
  std::array<double, vcu::kBrakeCount> brake_pressure_bar{};
  std::array<int, vcu::kParkingBrakeCount> parking_request{};
  std::uint32_t motor_mask{0};
  bool brake_frames_present{false};
  bool parking_frame_present{false};
};

inline SentActuation decode_sent_actuation(const std::vector<vcu::CanFrame>& frames) {
  SentActuation result;
  unsigned brake_mask = 0;
  for (const auto& frame : frames) {
    if (!frame.extended || frame.dlc != 8) continue;
    std::uint64_t payload = 0;
    for (std::size_t index = 0; index < 8; ++index) {
      payload |= static_cast<std::uint64_t>(frame.data[index]) << (index * 8U);
    }
    for (std::size_t index = 0; index < vcu::kMotorCount; ++index) {
      if (frame.id != 0x18F0D0F5U + index * 0x10000U) continue;
      result.motor_mask |= 1U << index;
      result.motor_command[index] = static_cast<int>(payload & 7U);
      result.motor_mode[index] = static_cast<int>((payload >> 3U) & 7U);
      result.torque_nm[index] = static_cast<double>((payload >> 8U) & 0xFFFFU) *
          vcu::kMotorTorqueResolutionNm + vcu::kMotorTorqueMinimumNm;
    }
    if (frame.id == 0x18FFD0F5U || frame.id == 0x18FAD0F5U) {
      const std::size_t base = frame.id == 0x18FFD0F5U ? 0 : 4;
      brake_mask |= base == 0 ? 1U : 2U;
      for (std::size_t index = 0; index < 4; ++index) {
        result.brake_pressure_bar[base + index] =
            static_cast<double>((payload >> (index * 16U + 4U)) & 0xFFFU) * 0.1;
      }
    } else if (frame.id == 0x18FBD0F5U) {
      result.parking_frame_present = true;
      for (std::size_t index = 0; index < vcu::kParkingBrakeCount; ++index) {
        result.parking_request[index] = static_cast<int>((payload >> (index * 8U)) & 3U);
      }
    }
  }
  result.brake_frames_present = brake_mask == 3U;
  return result;
}

struct TorqueObservation {
  std::uint32_t qualifying_motor_mask{0};
  std::int64_t longest_duration_ms{0};
};

class TorqueResponseMonitor {
 public:
  TorqueObservation observe(
      std::int64_t now_ms,
      const SentActuation& sent,
      bool send_ok,
      bool ready,
      const vcu::Feedback& feedback,
      std::uint32_t fresh_motor_mask) {
    TorqueObservation result;
    if (last_sample_ms_ >= 0 && (now_ms < last_sample_ms_ ||
        now_ms - last_sample_ms_ > kDiagnosticMaximumSampleGapMs)) {
      started_ms_.fill(-1);
    }
    last_sample_ms_ = now_ms;
    for (std::size_t index = 0; index < vcu::kMotorCount; ++index) {
      const auto bit = 1U << index;
      const double requested = sent.torque_nm[index];
      const bool no_response = send_ok && ready &&
          (sent.motor_mask & fresh_motor_mask & bit) != 0 &&
          sent.motor_command[index] == 1 && sent.motor_mode[index] == 1 &&
          std::isfinite(requested) && std::abs(requested) >= kMinimumObservedTorqueNm &&
          feedback.motor_torque_valid[index] &&
          std::isfinite(feedback.motor_torque_nm[index]) &&
          std::abs(feedback.motor_torque_nm[index]) <= kNearZeroFeedbackTorqueNm;
      const int direction = requested > 0.0 ? 1 : -1;
      if (!no_response) {
        started_ms_[index] = -1;
        continue;
      }
      if (started_ms_[index] < 0 || direction_[index] != direction ||
          now_ms < started_ms_[index]) {
        started_ms_[index] = now_ms;
        direction_[index] = direction;
      }
      const auto duration = now_ms - started_ms_[index];
      if (duration >= kTractionObservationDelayMs) {
        result.qualifying_motor_mask |= bit;
        result.longest_duration_ms = std::max(result.longest_duration_ms, duration);
      }
    }
    return result;
  }

 private:
  std::array<std::int64_t, vcu::kMotorCount> started_ms_{{-1, -1, -1, -1, -1, -1, -1, -1}};
  std::array<int, vcu::kMotorCount> direction_{};
  std::int64_t last_sample_ms_{-1};
};

}  // namespace mine_teleop::diagnostics
