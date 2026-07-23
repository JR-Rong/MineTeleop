#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "mine_teleop/core.hpp"

namespace mine_teleop {

enum class VideoCodec { H264, H265 };
enum class EncoderBackend { Nvenc, Vaapi };

std::string to_string(VideoCodec codec);
std::string to_string(EncoderBackend backend);
VideoCodec parse_video_codec(std::string_view value);
EncoderBackend parse_encoder_backend(std::string_view value);

struct EncoderCandidate {
  EncoderBackend backend;
  VideoCodec codec;
};

struct VideoEncoderSettings {
  int bitrate_kbps{3000};
  int keyframe_interval_frames{30};
};

class VideoEncoder {
 public:
  virtual ~VideoEncoder() = default;

  [[nodiscard]] virtual EncoderBackend backend() const = 0;
  [[nodiscard]] virtual VideoCodec codec() const = 0;
  [[nodiscard]] virtual std::string factory_name() const = 0;
  [[nodiscard]] virtual std::string pipeline_stage(
      const VideoEncoderSettings& settings,
      std::string_view element_name) const = 0;
};

[[nodiscard]] std::vector<EncoderCandidate> encoder_candidate_order(
    const HardwareConfig& hardware,
    VideoCodec codec);
[[nodiscard]] std::unique_ptr<VideoEncoder> create_video_encoder(const EncoderCandidate& candidate);
[[nodiscard]] bool gstreamer_factory_available(std::string_view factory_name);
[[nodiscard]] Json probe_video_encoders();

}  // namespace mine_teleop
