#include "mine_teleop/video.hpp"
#include "mine_teleop/gstreamer_media.hpp"

#include <gst/gst.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
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
    guint major = 0;
    guint minor = 0;
    guint micro = 0;
    guint nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    return build_nvenc_pipeline_stage(factory, settings, element_name, major, minor);
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

class CpuVideoEncoder final : public VideoEncoder {
 public:
  explicit CpuVideoEncoder(VideoCodec codec) : codec_(codec) {}

  [[nodiscard]] EncoderBackend backend() const override { return EncoderBackend::Cpu; }
  [[nodiscard]] VideoCodec codec() const override { return codec_; }
  [[nodiscard]] std::string factory_name() const override {
    return codec_ == VideoCodec::H264 ? first_available({"x264enc"}) : std::string{};
  }
  [[nodiscard]] std::string pipeline_stage(
      const VideoEncoderSettings& settings,
      std::string_view element_name) const override {
    if (factory_name().empty()) throw std::runtime_error("CPU H.264 x264 encoder is unavailable");
    std::ostringstream value;
    // Bound encoder parallelism and buffering so a bench CPU stream leaves
    // capacity for capture, signaling and the control watchdog.
    value << "x264enc name=" << element_name
          << " bitrate=" << settings.bitrate_kbps
          << " key-int-max=" << settings.keyframe_interval_frames
          << " speed-preset=ultrafast tune=zerolatency threads=4"
          << " bframes=0 rc-lookahead=0 sync-lookahead=0 byte-stream=true aud=true"
          << " ! video/x-h264,profile=constrained-baseline";
    return value.str();
  }

 private:
  VideoCodec codec_;
};

}  // namespace

int realtime_keyframe_interval_frames(int fps, int stale_brake_ms) {
  if (fps <= 0 || stale_brake_ms <= 0) {
    throw std::invalid_argument("realtime keyframe interval requires positive FPS and video budget");
  }
  // If loss recovery feedback is delayed or lost, a one-second GOP can outlast
  // the video safety budget. Reserve half that budget for the next IDR, leaving
  // room for transport and decoding without changing the safety deadline.
  const auto frames = static_cast<std::int64_t>(fps) * stale_brake_ms / 2000;
  return static_cast<int>(std::clamp<std::int64_t>(frames, 1, fps));
}

std::string build_nvenc_pipeline_stage(
    std::string_view factory_name,
    const VideoEncoderSettings& settings,
    std::string_view element_name,
    unsigned int gstreamer_major,
    unsigned int gstreamer_minor) {
  std::ostringstream value;
  value << factory_name << " name=" << element_name
        << " bitrate=" << settings.bitrate_kbps
        << " gop-size=" << settings.keyframe_interval_frames
        << " bframes=0 zerolatency=true rc-lookahead=0 rc-mode=cbr";
  const auto version_at_least = [=](unsigned int major, unsigned int minor) {
    return gstreamer_major > major ||
        (gstreamer_major == major && gstreamer_minor >= minor);
  };
  if (version_at_least(1, 22)) value << " preset=p1";
  if (version_at_least(1, 24)) value << " tune=ultra-low-latency";
  return value.str();
}

std::string to_string(VideoCodec codec) {
  return codec == VideoCodec::H265 ? "h265" : "h264";
}

std::string to_string(EncoderBackend backend) {
  switch (backend) {
    case EncoderBackend::Nvenc: return "nvenc";
    case EncoderBackend::Vaapi: return "vaapi";
    case EncoderBackend::Cpu: return "cpu";
  }
  throw std::invalid_argument("unknown video encoder backend");
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
  if (normalized == "cpu") return EncoderBackend::Cpu;
  throw std::invalid_argument("unsupported video encoder: " + std::string(value));
}

std::vector<EncoderCandidate> encoder_candidate_order(const HardwareConfig& hardware, VideoCodec codec) {
  const auto preferred = parse_encoder_backend(hardware.preferred_encoder);
  const auto fallback = parse_encoder_backend(hardware.fallback_encoder);
  if (hardware.require_hardware_encoder &&
      (preferred == EncoderBackend::Cpu || fallback == EncoderBackend::Cpu)) {
    throw std::invalid_argument("CPU encoding requires hardware.encoding.require_hardware_encoder=false");
  }
  std::vector<EncoderCandidate> candidates;
  const auto append = [&](EncoderBackend backend) {
    if (backend != EncoderBackend::Cpu || codec == VideoCodec::H264) {
      candidates.push_back({backend, codec});
    }
  };
  append(preferred);
  if (fallback != preferred) append(fallback);
  return candidates;
}

std::unique_ptr<VideoEncoder> create_video_encoder(const EncoderCandidate& candidate) {
  if (candidate.backend == EncoderBackend::Nvenc) return std::make_unique<NvencVideoEncoder>(candidate.codec);
  if (candidate.backend == EncoderBackend::Vaapi) return std::make_unique<VaapiVideoEncoder>(candidate.codec);
  if (candidate.backend == EncoderBackend::Cpu) return std::make_unique<CpuVideoEncoder>(candidate.codec);
  throw std::invalid_argument("unknown video encoder backend");
}

GstElement* parse_initialized_gstreamer_pipeline(const char* description, GError** error) {
  ensure_gstreamer();
  return gst_parse_launch(description, error);
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
  for (const auto backend : {EncoderBackend::Nvenc, EncoderBackend::Vaapi, EncoderBackend::Cpu}) {
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
  return {{"event", "video_encoder_probe"}, {"runtime", "cpp"}, {"availability_scope", "factory_only"},
      {"functional_validation", "not_run"}, {"probes", std::move(probes)}};
}

}  // namespace mine_teleop
