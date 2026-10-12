#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <gst/gst.h>

namespace mine_teleop {

// The flag must outlive the pad and becomes true before whole-pipeline EOS.
void isolate_bundled_rtp_lane_eos(GstPad* rtp_src_pad, std::atomic<bool>& pipeline_stopping);

// Encoders may shift PTS and the output segment together (NVENC uses 1000h).
[[nodiscard]] std::optional<std::uint64_t> encoded_frame_latency_ms(
    const GstSegment& segment, GstClockTime pts, std::int64_t elapsed_ms);

}  // namespace mine_teleop
