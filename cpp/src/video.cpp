#include "mine_teleop/video.hpp"

#include <gst/gst.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace mine_teleop {
namespace {

std::string lower(std::string_view value) {
  std::string result(value);
  std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return result;
}

void ensure_gstreamer() {
  static std::once_flag initialized;
  static std::string error_message;
  std::call_once(initialized, [] {
    GError* error = nullptr;
    if (!gst_init_check(nullptr, nullptr, &error)) {
      error_message = error != nullptr ? error->message : "unknown GStreamer initialization failure";
      if (error != nullptr) g_error_free(error);
    }
  });
  if (!error_message.empty()) throw std::runtime_error("cannot initialize GStreamer: " + error_message);
}

std::string first_available(std::initializer_list<const char*> names) {
  for (const auto* name : names) {
    if (gstreamer_factory_available(name)) return name;
  }
  return {};
}

class NvencVideoEncoder final : public VideoEncoder {
 public:
  explicit NvencVideoEncoder(VideoCodec codec) : codec_(codec) {}

  [[nodiscard]] EncoderBackend backend() const override { return EncoderBackend::Nvenc; }
  [[nodiscard]] VideoCodec codec() const override { return codec_; }
  [[nodiscard]] std::string factory_name() const override {
    return codec_ == VideoCodec::H265 ? first_available({"nvh265enc", "nvautogpuh265enc"})
                                      : first_available({"nvh264enc", "nvautogpuh264enc"});
  }
  [[nodiscard]] std::string pipeline_stage(
      const VideoEncoderSettings& settings,
      std::string_view element_name) const override {
    const auto factory = factory_name();
    if (factory.empty()) throw std::runtime_error("NVIDIA " + to_string(codec_) + " encoder is unavailable");
    std::ostringstream value;
    value << factory << " name=" << element_name
          << " bitrate=" << settings.bitrate_kbps
          << " gop-size=" << settings.keyframe_interval_frames
          << " bframes=0 zerolatency=true rc-lookahead=0"
          << " preset=p1 tune=ultra-low-latency rc-mode=cbr";
    return value.str();
  }

 private:
  VideoCodec codec_;
};

class VaapiVideoEncoder final : public VideoEncoder {
 public:
  explicit VaapiVideoEncoder(VideoCodec codec) : codec_(codec) {}

  [[nodiscard]] EncoderBackend backend() const override { return EncoderBackend::Vaapi; }
  [[nodiscard]] VideoCodec codec() const override { return codec_; }
  [[nodiscard]] std::string factory_name() const override {
    return codec_ == VideoCodec::H265 ? first_available({"vah265enc", "vah265lpenc", "vaapih265enc"})
                                      : first_available({"vah264enc", "vah264lpenc", "vaapih264enc"});
  }
  [[nodiscard]] std::string pipeline_stage(
      const VideoEncoderSettings& settings,
      std::string_view element_name) const override {
    const auto factory = factory_name();
    if (factory.empty()) throw std::runtime_error("Intel VAAPI " + to_string(codec_) + " encoder is unavailable");
    std::ostringstream value;
    value << factory << " name=" << element_name << " bitrate=" << settings.bitrate_kbps;
    if (factory.starts_with("vaapi")) {
      value << " keyframe-period=" << settings.keyframe_interval_frames << " rate-control=cbr";
    } else {
      value << " key-int-max=" << settings.keyframe_interval_frames
            << " b-frames=0 target-usage=7 rate-control=cbr";
    }
    return value.str();
  }

 private:
  VideoCodec codec_;
};

}  // namespace

std::string to_string(VideoCodec codec) {
  return codec == VideoCodec::H265 ? "h265" : "h264";
}

std::string to_string(EncoderBackend backend) {
  return backend == EncoderBackend::Nvenc ? "nvenc" : "vaapi";
}

VideoCodec parse_video_codec(std::string_view value) {
  const auto normalized = lower(value);
  if (normalized == "h264" || normalized == "avc") return VideoCodec::H264;
  if (normalized == "h265" || normalized == "hevc") return VideoCodec::H265;
  throw std::invalid_argument("unsupported video codec: " + std::string(value));
}

EncoderBackend parse_encoder_backend(std::string_view value) {
  const auto normalized = lower(value);
  if (normalized == "nvenc" || normalized == "nvidia") return EncoderBackend::Nvenc;
  if (normalized == "vaapi" || normalized == "intel") return EncoderBackend::Vaapi;
  throw std::invalid_argument("unsupported hardware video encoder: " + std::string(value));
}

std::vector<EncoderCandidate> encoder_candidate_order(const HardwareConfig& hardware, VideoCodec codec) {
  const auto preferred = parse_encoder_backend(hardware.preferred_encoder);
  const auto fallback = parse_encoder_backend(hardware.fallback_encoder);
  std::vector<EncoderCandidate> candidates{{preferred, codec}};
  if (fallback != preferred) candidates.push_back({fallback, codec});
  return candidates;
}

std::unique_ptr<VideoEncoder> create_video_encoder(const EncoderCandidate& candidate) {
  if (candidate.backend == EncoderBackend::Nvenc) return std::make_unique<NvencVideoEncoder>(candidate.codec);
  return std::make_unique<VaapiVideoEncoder>(candidate.codec);
}

bool gstreamer_factory_available(std::string_view factory_name) {
  ensure_gstreamer();
  GstElementFactory* factory = gst_element_factory_find(std::string(factory_name).c_str());
  if (factory == nullptr) return false;
  gst_object_unref(factory);
  return true;
}

Json probe_video_encoders() {
  Json probes = Json::array();
  for (const auto backend : {EncoderBackend::Nvenc, EncoderBackend::Vaapi}) {
    for (const auto codec : {VideoCodec::H265, VideoCodec::H264}) {
      const EncoderCandidate candidate{backend, codec};
      const auto encoder = create_video_encoder(candidate);
      const auto factory = encoder->factory_name();
      probes.push_back({
          {"backend", to_string(backend)},
          {"codec", to_string(codec)},
          {"factory", factory},
          {"available", !factory.empty()},
      });
    }
  }
  return {{"event", "video_encoder_probe"}, {"runtime", "cpp"}, {"probes", std::move(probes)}};
}

}  // namespace mine_teleop
