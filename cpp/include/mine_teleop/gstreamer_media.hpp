#pragma once

#include <atomic>
#include <gst/gst.h>

namespace mine_teleop {

// The flag must outlive the pad and becomes true before whole-pipeline EOS.
void isolate_bundled_rtp_lane_eos(GstPad* rtp_src_pad, std::atomic<bool>& pipeline_stopping);

}  // namespace mine_teleop
