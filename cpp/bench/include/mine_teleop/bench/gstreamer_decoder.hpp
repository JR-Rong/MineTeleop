#pragma once
#include <stdexcept>
#include <string>
#include <string_view>

namespace mine_teleop::detail {
// Shared by the native receiver and the opt-in functional codec probe.
inline std::string low_latency_decoder_stage(std::string_view factory) {
  if (factory == "nvv4l2decoder") return "nvv4l2decoder name=decoder disable-dpb=true";
  if (factory == "avdec_h264" || factory == "avdec_h265")
    return std::string(factory) + " name=decoder max-threads=2";
  if (factory == "vtdec_hw" || factory == "vah264dec" || factory == "vah265dec" ||
      factory == "vaapih264dec" || factory == "vaapih265dec")
    return std::string(factory) + " name=decoder";
  throw std::invalid_argument("unsupported diagnostic decoder factory");
}
}  // namespace mine_teleop::detail
