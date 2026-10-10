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

[[nodiscard]] std::string build_nvenc_pipeline_stage(
    std::string_view factory_name,
    const VideoEncoderSettings& settings,
    std::string_view element_name,
    unsigned int gstreamer_major,
    unsigned int gstreamer_minor);

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
// Macroblock/frame and macroblock/second constraints, independent of codec name.
int minimum_h264_level_idc(int width, int height, int fps);
bool h264_answer_supports(std::string_view sdp,const std::vector<MediaProfile>& profiles,const std::vector<int>& encoded_levels={});
std::string h264_profile_level_id(std::string_view sprop_parameter_sets);
std::string h264_level_name(int level_idc);
Json media_environment(const VehicleConfig&,const EncoderCandidate&);
Json probe_media_profiles(const VehicleConfig&,const EncoderCandidate&,bool software_fixture=false);

}  // namespace mine_teleop
