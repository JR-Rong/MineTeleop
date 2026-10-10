#include <gst/sdp/gstsdpmessage.h>
#include "mine_teleop/video.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <sys/utsname.h>
#include <fstream>
#include <array>
#include <cmath>
#include <chrono>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace mine_teleop {
int minimum_h264_level_idc(int width,int height,int fps) {
  if(width<=0||height<=0||width>8192||height>8192||fps<=0||fps>120)
    throw std::invalid_argument("invalid H.264 dimensions/FPS");
  const auto blocks=std::int64_t((width+15)/16)*((height+15)/16);
  // The currently offered profiles start at 3.1. Never silently offer a level
  // whose MaxFS/MaxMBPS is smaller than the actual encoded canvas.
  struct Limit {int idc;std::int64_t fs,mbps;};
  for(const auto& l: {Limit{31,3600,108000},Limit{32,5120,216000},
                     Limit{40,8192,245760},Limit{42,8704,522240},
                     Limit{50,22080,589824},Limit{51,36864,983040}})
    if(blocks<=l.fs&&blocks*fps<=l.mbps)return l.idc;
  throw std::invalid_argument("canvas exceeds supported H.264 level constraints");
}
std::string h264_level_name(int idc) {
  return std::to_string(idc/10)+(idc%10?"."+std::to_string(idc%10):"");
}
std::string h264_profile_level_id(std::string_view sprop){
  const auto first=sprop.substr(0,sprop.find(','));gsize size=0;
  auto* data=g_base64_decode(std::string(first).c_str(),&size);std::string result;
  if(data&&size>=4&&(data[0]&31)==7){char hex[7];g_snprintf(hex,sizeof(hex),"%02x%02x%02x",data[1],data[2],data[3]);result=hex;}
  g_free(data);return result;
}
bool h264_answer_supports(std::string_view text,const std::vector<MediaProfile>& profiles,const std::vector<int>& encoded_levels){
  GstSDPMessage* sdp=nullptr;
  if(gst_sdp_message_new(&sdp)!=GST_SDP_OK)return false;
  if(gst_sdp_message_parse_buffer(reinterpret_cast<const guint8*>(text.data()),text.size(),sdp)!=GST_SDP_OK){gst_sdp_message_free(sdp);return false;}
          unsigned video=0;bool supported=true;
          for(unsigned m=0;m<gst_sdp_message_medias_len(sdp);++m){
            const auto* section=gst_sdp_message_get_media(sdp,m);
            if(std::string_view(gst_sdp_media_get_media(section))!="video")continue;
            if(video>=profiles.size()||gst_sdp_media_get_port(section)==0){supported=false;break;}
            const auto payload_id=std::to_string(96+video);
            bool offered=false;for(unsigned f=0;f<gst_sdp_media_formats_len(section);++f)if(payload_id==gst_sdp_media_get_format(section,f))offered=true;
            int receive_level=31;bool codec_selected=false;
            for(unsigned a=0;a<gst_sdp_media_attributes_len(section);++a){
              const auto* attr=gst_sdp_media_get_attribute(section,a);
              const std::string_view value(attr->value?attr->value:"");
              if(std::string_view(attr->key)=="rtpmap"&&value.starts_with(payload_id+" H264/"))codec_selected=offered;
              if(std::string_view(attr->key)!="fmtp"||!value.starts_with(payload_id+" "))continue;
              auto pos=value.find("profile-level-id=");
              if(pos!=std::string_view::npos){
                const auto hex=value.substr(pos+17,6);
                try{if(hex.size()!=6)throw std::runtime_error("bad profile-level-id");
                    std::size_t n=0;const auto parsed=std::stoul(std::string(hex),&n,16);
                    if(n!=6)throw std::runtime_error("bad profile-level-id");
                    receive_level=int(parsed&255);
                }catch(...){supported=false;}
              }
            }
            const auto& profile=profiles[video++];
            if(!codec_selected||receive_level<std::max(minimum_h264_level_idc(profile.width,profile.height,profile.fps),video<=encoded_levels.size()?encoded_levels[video-1]:0))supported=false;
          }
  gst_sdp_message_free(sdp);return supported&&video==profiles.size()&&!profiles.empty();
}

Json media_environment(const VehicleConfig& config,const EncoderCandidate& candidate){
  auto read=[](const std::filesystem::path& path){std::ifstream in(path);return std::string(std::istreambuf_iterator<char>(in),{});};
  auto cpu=read("/proc/cpuinfo");auto pos=cpu.find("model name");
  if(pos!=std::string::npos){pos=cpu.find(':',pos)+1;cpu=cpu.substr(pos,cpu.find('\n',pos)-pos);}
  else cpu=read("/proc/device-tree/model");
  struct utsname platform{};if(uname(&platform)!=0)throw std::runtime_error("uname failed");
  auto encoder=create_video_encoder(candidate);
  std::string drivers;
  for(const auto& c:config.enabled_cameras()){
    const auto path=std::filesystem::path("/sys/class/video4linux")/std::filesystem::path(c.device).filename()/"device/driver";
    std::error_code error;const auto driver=std::filesystem::canonical(path,error);
    drivers+=c.id+":"+c.device+":"+(error?"unverified":driver.string())+";";
  }
  const auto gpu=candidate.backend==EncoderBackend::Nvenc?read("/proc/driver/nvidia/version"):
      read(std::filesystem::path("/sys/class/drm")/config.hardware.vaapi_render_device.filename()/"device/uevent");
  return {{"cpu",cpu},{"encoder",encoder->factory_name()},{"driver",gpu},{"kernel",std::string(platform.release)+"/"+platform.machine},
          {"gstreamer",gst_version_string()},{"capture_driver",drivers}};
}

Json probe_media_profiles(const VehicleConfig& config,const EncoderCandidate& candidate,bool fixture){
  gst_init(nullptr,nullptr);Json report={{"schema",1},{"software_fixture",fixture},{"vehicle_environment",media_environment(config,candidate)},{"canvases",Json::array()},{"driving_qualified",false}};
  auto encoder=create_video_encoder(candidate);
  const std::array<std::array<int,3>,5> shapes={{{1280,1440,30},{512,896,20},{960,1080,30},{384,672,20},{2560,1440,30}}};
  for(const auto shape:shapes){
    const int level=minimum_h264_level_idc(shape[0],shape[1],shape[2]);Json result={{"width",shape[0]},{"height",shape[1]},{"fps",shape[2]},{"minimum_level_idc",level},{"passed",false}};
    GstElement* pipe=nullptr;GstElement* sink=nullptr;
    try{
      std::ostringstream text;text<<"videotestsrc is-live=true pattern=ball ! video/x-raw,format=NV12,width="<<shape[0]<<",height="<<shape[1]<<",framerate="<<shape[2]<<"/1 ! ";
      text<<(fixture?"x264enc tune=zerolatency speed-preset=ultrafast bframes=0 bitrate=4000 key-int-max=30":encoder->pipeline_stage({4000,30},"encoder"));
      text<<" ! video/x-h264,level=(string)"<<h264_level_name(level)<<" ! h264parse ! appsink name=probe sync=false max-buffers=2 drop=true";
      GError* error=nullptr;pipe=gst_parse_launch(text.str().c_str(),&error);
      if(error){const auto message=std::string(error->message);g_error_free(error);throw std::runtime_error(message);}
      if(!pipe)throw std::runtime_error("profile pipeline missing");
      sink=gst_bin_get_by_name(GST_BIN(pipe),"probe");
      if(gst_element_set_state(pipe,GST_STATE_PLAYING)==GST_STATE_CHANGE_FAILURE)throw std::runtime_error("profile cannot enter PLAYING");
      auto* sample=gst_app_sink_try_pull_sample(GST_APP_SINK(sink),5*GST_SECOND);
      if(!sample)throw std::runtime_error("encoder cannot produce the requested canvas");
      gchar* caps=gst_caps_to_string(gst_sample_get_caps(sample));result["actual_caps"]=caps?caps:"";g_free(caps);
      const auto* structure=gst_caps_get_structure(gst_sample_get_caps(sample),0);
      const auto* actual=gst_structure_get_string(structure,"level");
      result["actual_level"]=actual?actual:"";result["passed"]=actual&&std::lround(std::stod(actual)*10)>=level;
      gst_sample_unref(sample);
    }catch(const std::exception& error){result["error"]=error.what();}
    if(pipe)gst_element_set_state(pipe,GST_STATE_NULL);if(sink)gst_object_unref(sink);if(pipe)gst_object_unref(pipe);
    report["canvases"].push_back(result);
  }
  return report;
}
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

}  // namespace

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
