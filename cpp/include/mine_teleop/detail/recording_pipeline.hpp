#pragma once

#include "mine_teleop/recording.hpp"
#include <gst/gst.h>
#include <atomic>
#include <span>

namespace mine_teleop::detail {

// One serialized encoded appsink producer per recording lane. The recorder
// outlives this tap, and the caller stops the pipeline before destroying it.
class RecordingTap {
 public:
  RecordingTap(GstElement* sink, IsolatedRecordingSession* recording, std::size_t lane);
  ~RecordingTap();
  RecordingTap(const RecordingTap&) = delete;
  RecordingTap& operator=(const RecordingTap&) = delete;
  [[nodiscard]] bool eos_received() const { return eos_.load(std::memory_order_acquire); }
 private:
  GstElement* sink_;
  IsolatedRecordingSession* recording_;
  std::size_t lane_;
  std::atomic<bool> eos_{false};
};

// Capture producers have already joined and control has already stopped.
// Requests source EOS, bounds the media drain, then stops streaming callbacks.
void stop_recording_pipeline(GstElement* pipeline, std::span<GstElement* const> sources,
                             std::span<RecordingTap* const> taps, IsolatedRecordingSession* recording);

}  // namespace mine_teleop::detail
