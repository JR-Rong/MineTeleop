// Opt-in synthetic diagnostic. Run via codec_roundtrip_probe.py so a driver
// hanging during init/state change/destruction cannot hang the caller.
#include "mine_teleop/video.hpp"
#include "mine_teleop/gstreamer_media.hpp"
#include "mine_teleop/detail/media_freshness.hpp"
#include "mine_teleop/detail/gstreamer_decoder.hpp"
#include "mine_teleop/detail/latency_histogram.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <charconv>
#include <chrono>
#include <iostream>
#include <memory>
#include <map>
#include <mutex>

namespace {
using namespace mine_teleop;
using Clock=std::chrono::steady_clock;
void require(bool value,const char* why) {if(!value)throw std::runtime_error(why);}
int integer(const char* text,int maximum) {
  int result=0;const std::string value(text);
  const auto parsed=std::from_chars(value.data(),value.data()+value.size(),result);
  require(parsed.ec==std::errc{} && parsed.ptr==value.data()+value.size() && result>0 && result<=maximum,"invalid bounded integer argument");
  return result;
}
struct Unref {void operator()(GstElement* value) const {if(value)gst_object_unref(value);}};
struct SampleUnref {void operator()(GstSample* value) const {if(value)gst_sample_unref(value);}};
using Element=std::unique_ptr<GstElement,Unref>;
using Sample=std::unique_ptr<GstSample,SampleUnref>;
class Pipeline {
 public:
  explicit Pipeline(const std::string& description) {
    GError* error=nullptr;value_=parse_initialized_gstreamer_pipeline(description.c_str(),&error);
    if(error || !value_) {
      const std::string message=error?error->message:"pipeline unavailable";
      g_clear_error(&error);if(value_)gst_object_unref(value_);
      throw std::runtime_error(message);
    }
  }
  ~Pipeline(){gst_element_set_state(value_,GST_STATE_NULL);gst_object_unref(value_);}
  Element element(const char* name){return Element(gst_bin_get_by_name(GST_BIN(value_),name));}
  void play(){require(gst_element_set_state(value_,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"pipeline failed to enter PLAYING");}
  Sample pull(GstElement* sink) {
    Sample result(gst_app_sink_try_pull_sample(GST_APP_SINK(sink),2*GST_SECOND));
    if(!result) {
      auto* bus=gst_element_get_bus(value_);auto* message=gst_bus_pop_filtered(bus,GST_MESSAGE_ERROR);gst_object_unref(bus);
      if(message) {
        GError* error=nullptr;gchar* debug=nullptr;gst_message_parse_error(message,&error,&debug);
        const std::string why=error?error->message:"GStreamer error";
        g_clear_error(&error);g_free(debug);gst_message_unref(message);throw std::runtime_error(why);
      }
      throw std::runtime_error("no output sample before phase deadline");
    }
    return result;
  }
 private:
  GstElement* value_{};
};
double elapsed_ms(Clock::time_point start){return std::chrono::duration<double,std::milli>(Clock::now()-start).count();}
struct EncoderTimings {
  std::mutex mutex;
  std::map<GstClockTime,Clock::time_point> input;
  GstSegment segment{};
  detail::LatencyHistogram latency;
  unsigned matched{},unmatched{},peak_pending{};
  EncoderTimings(){gst_segment_init(&segment,GST_FORMAT_UNDEFINED);}
  static GstPadProbeReturn enter(GstPad*,GstPadProbeInfo* info,gpointer data) {
    auto& self=*static_cast<EncoderTimings*>(data);auto* buffer=GST_PAD_PROBE_INFO_BUFFER(info);
    if(buffer) {std::lock_guard lock(self.mutex);self.input[GST_BUFFER_PTS(buffer)]=Clock::now();
      self.peak_pending=std::max(self.peak_pending,static_cast<unsigned>(self.input.size()));}
    return GST_PAD_PROBE_OK;
  }
  static GstPadProbeReturn leave(GstPad*,GstPadProbeInfo* info,gpointer data) {
    auto& self=*static_cast<EncoderTimings*>(data);
    if((GST_PAD_PROBE_INFO_TYPE(info)&GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM)!=0) {
      auto* event=GST_PAD_PROBE_INFO_EVENT(info);
      if(GST_EVENT_TYPE(event)==GST_EVENT_SEGMENT){const GstSegment* segment=nullptr;gst_event_parse_segment(event,&segment);self.segment=*segment;}
      return GST_PAD_PROBE_OK;
    }
    auto* buffer=GST_PAD_PROBE_INFO_BUFFER(info);if(!buffer)return GST_PAD_PROBE_OK;
    const auto pts=self.segment.format==GST_FORMAT_TIME?
        gst_segment_to_running_time(&self.segment,GST_FORMAT_TIME,GST_BUFFER_PTS(buffer)):GST_CLOCK_TIME_NONE;
    std::lock_guard lock(self.mutex);const auto found=self.input.find(pts);
    if(found==self.input.end())++self.unmatched;
    else {self.latency.add(static_cast<std::uint64_t>(elapsed_ms(found->second)));++self.matched;self.input.erase(found);}
    return GST_PAD_PROBE_OK;
  }
};
Json properties(GstElement* element) {
  Json result=Json::object();
  // Read back only non-sensitive codec properties, including defaults. Null
  // means unsupported, never a guessed equivalent from another backend.
  for(const char* name:{"bframes","b-frames","rc-lookahead","sync-lookahead","zerolatency","tune","preset",
      "speed-preset","rc-mode","rate-control","bitrate","key-int-max","keyframe-period",
      "vbv-buf-capacity","vbv-buffer-size","async-depth","num-extra-surfaces","max-threads","disable-dpb"}) {
    const auto* spec=g_object_class_find_property(G_OBJECT_GET_CLASS(element),name);
    result[name]=nullptr;
    if(!spec || !(spec->flags&G_PARAM_READABLE))continue;
    GValue value=G_VALUE_INIT;g_value_init(&value,spec->value_type);g_object_get_property(G_OBJECT(element),name,&value);
    if(G_VALUE_HOLDS_UINT(&value))result[name]=g_value_get_uint(&value);
    else if(G_VALUE_HOLDS_INT(&value))result[name]=g_value_get_int(&value);
    else if(G_VALUE_HOLDS_BOOLEAN(&value))result[name]=static_cast<bool>(g_value_get_boolean(&value));
    else {auto* text=gst_value_serialize(&value);if(text)result[name]=text;g_free(text);}
    g_value_unset(&value);
  }
  return result;
}
Json probe(int argc,char** argv) {
  require(argc==9,"use wrapper: worker ENCODER CODEC DECODER WIDTH HEIGHT FPS BITRATE_KBPS GOP");
  const EncoderCandidate candidate{parse_encoder_backend(argv[1]),parse_video_codec(argv[2])};
  const std::string decoder(argv[3]),codec=to_string(candidate.codec);
  const int width=integer(argv[4],4096),height=integer(argv[5],4096),fps=integer(argv[6],120);
  const VideoEncoderSettings settings{integer(argv[7],100000),integer(argv[8],240)};
  require(width%2==0 && height%2==0,"NV12 dimensions must be even");
  Json result{{"schema","codec-roundtrip-v1"},{"passed",false},{"backend",to_string(candidate.backend)},
      {"codec",codec},{"decoder",decoder},{"width",width},{"height",height},{"fps",fps},
      {"bitrate_kbps",settings.bitrate_kbps},{"gop",settings.keyframe_interval_frames},
      {"source","synthetic NV12; no capture device"},{"render_tested",false},{"network_tested",false}};
  std::string stage="factory_inventory";
  try {
    auto encoder=create_video_encoder(candidate);
    result["encoder_factory"]=encoder->factory_name();
    result["encoder_factory_available"]=!encoder->factory_name().empty();
    result["decoder_factory_available"]=gstreamer_factory_available(decoder);
    require(result["encoder_factory_available"].get<bool>(),"encoder factory unavailable");
    require(result["decoder_factory_available"].get<bool>(),"decoder factory unavailable");
    const auto decoder_stage=detail::low_latency_decoder_stage(decoder);
    const auto encoder_stage=encoder->pipeline_stage(settings,"encoder");
    result["encoder_stage"]=encoder_stage;result["decoder_stage"]=decoder_stage;
    {
      stage="encode_initial_idr";
      const auto encode_started=Clock::now();
      EncoderTimings timings; // Outlives pipeline streaming callbacks.
      Pipeline encode("videotestsrc num-buffers=16 pattern=smpte ! video/x-raw,format=NV12,width="+
          std::to_string(width)+",height="+std::to_string(height)+",framerate="+std::to_string(fps)+"/1 ! "+
          encoder_stage+" ! "+codec+"parse config-interval=-1 ! video/x-"+codec+
          ",stream-format=byte-stream,alignment=au ! identity name=timing ! appsink name=out sync=false async=false max-buffers=1 enable-last-sample=false");
      auto sink=encode.element("out"),encoder_element=encode.element("encoder"),timing_element=encode.element("timing");
      auto* in_pad=gst_element_get_static_pad(encoder_element.get(),"sink");
      auto* out_pad=gst_element_get_static_pad(timing_element.get(),"src");
      gst_pad_add_probe(in_pad,GST_PAD_PROBE_TYPE_BUFFER,EncoderTimings::enter,&timings,nullptr);
      gst_pad_add_probe(out_pad,static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER|GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),EncoderTimings::leave,&timings,nullptr);
      gst_object_unref(in_pad);gst_object_unref(out_pad);
      encode.play();auto encoded=encode.pull(sink.get());
      auto actual_encoder=encode.element("encoder");
      result["encoder_properties"]=properties(actual_encoder.get());
      const auto vbv=result["encoder_properties"]["vbv-buf-capacity"];
      result["vbv_budget_frames"]=vbv.is_number()?Json(vbv.get<double>()*fps/1000.0):Json(nullptr);
      result["vbv_budget_note"]="x264 capacity in milliseconds expressed as frame budgets; not measured queue latency";
      result["encode_probe_ms"]=elapsed_ms(encode_started);
      auto* buffer=gst_sample_get_buffer(encoded.get());auto* segment=gst_sample_get_segment(encoded.get());
      require(buffer && segment && segment->format==GST_FORMAT_TIME && GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer)),"invalid encoded PTS/segment");
      require(!GST_BUFFER_FLAG_IS_SET(buffer,GST_BUFFER_FLAG_CORRUPTED),"corrupt encoded output");
      const auto running=gst_segment_to_running_time(segment,GST_FORMAT_TIME,GST_BUFFER_PTS(buffer));
      require(GST_CLOCK_TIME_IS_VALID(running),"encoded PTS outside segment");
      GstMapInfo mapped{};require(gst_buffer_map(buffer,&mapped,GST_MAP_READ),"cannot read encoded AU");
      const auto info=detail::inspect_access_unit(candidate.codec==VideoCodec::H264?detail::AccessUnitCodec::H264:detail::AccessUnitCodec::H265,{mapped.data,mapped.size});
      result["encoded_bytes"]=mapped.size;gst_buffer_unmap(buffer,&mapped);
      require(info.valid && info.picture && info.idr && info.parameter_sets,"initial AU is not a complete IDR with parameter sets");
      result["initial_idr_valid"]=true;result["encoded_running_pts_ns"]=running;
      stage="decode_initial_idr";
      const auto decode_started=Clock::now();
      Pipeline decode("appsrc name=input format=time is-live=false ! "+codec+"parse ! "+decoder_stage+
          " ! appsink name=out sync=false async=false max-buffers=1 enable-last-sample=false");
      auto input=decode.element("input"),output=decode.element("out");
      gst_app_src_set_caps(GST_APP_SRC(input.get()),gst_sample_get_caps(encoded.get()));
      decode.play();
      // Encoders may offset PTS and segment together (e.g. x264's 1000h).
      // appsrc starts a fresh zero-based segment, so forward running time.
      auto* copy=gst_buffer_copy_deep(buffer);require(copy!=nullptr,"encoded AU copy failed");
      GST_BUFFER_PTS(copy)=running;GST_BUFFER_DTS(copy)=GST_CLOCK_TIME_NONE;
      require(gst_app_src_push_buffer(GST_APP_SRC(input.get()),copy)==GST_FLOW_OK,"decoder rejected input");
      require(gst_app_src_end_of_stream(GST_APP_SRC(input.get()))==GST_FLOW_OK,"decoder EOS failed");
      auto decoded=decode.pull(output.get());result["decode_probe_ms"]=elapsed_ms(decode_started);
      auto actual_decoder=decode.element("decoder");result["decoder_properties"]=properties(actual_decoder.get());
      auto* raw=gst_sample_get_buffer(decoded.get());GstVideoInfo video{};
      require(raw && gst_buffer_get_size(raw)>0 && !GST_BUFFER_FLAG_IS_SET(raw,GST_BUFFER_FLAG_CORRUPTED),"no valid decoded frame");
      require(gst_video_info_from_caps(&video,gst_sample_get_caps(decoded.get())) &&
          static_cast<int>(video.width)==width && static_cast<int>(video.height)==height,"decoded dimensions differ");
      require(GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(raw)) && GST_BUFFER_PTS(raw)==running,"decoded frame PTS differs from input identity");
      result["decoded_pts_ns"]=GST_BUFFER_PTS(raw);result["decoded_frames"]=1;
      result["decoded_format"]=gst_video_format_to_string(video.finfo->format);
      result["decoded_memory_types"]=Json::array();
      for(guint index=0;index<gst_buffer_n_memory(raw);++index) {
        auto* memory=gst_buffer_peek_memory(raw,index);
        result["decoded_memory_types"].push_back(memory->allocator && memory->allocator->mem_type?memory->allocator->mem_type:"unknown");
      }
      result["buffer_budget"]={{"encoded_appsink",1},{"decoded_appsink",1},{"render_queue",0},
          {"driver_reference_surfaces",nullptr},{"hardware_pool_capacity",nullptr},{"surface_budget_verified",false}};
      for(unsigned index=1;index<16;++index) {auto next=encode.pull(sink.get());}
      {
        std::lock_guard lock(timings.mutex);
        require(timings.matched==16 && timings.unmatched==0 && timings.input.empty(),"encoder input/output PTS correlation incomplete");
        result["encoder_input_to_parsed_output"]=timings.latency.json();
        result["peak_encoder_inputs_pending"]=timings.peak_pending;
        result["timing_scope"]="16 synthetic frames, encoder sink to parsed AU; floor milliseconds (0 means <1 ms). Excludes capture, network, decoder and display. Startup included; not sustained performance.";
      }
      stage="teardown";
    }
    // Success is reported only after both pipelines have been destroyed.
    result["passed"]=true;result["stage"]="complete";
  } catch(const std::exception& error){result["stage"]=stage;result["error"]=error.what();}
  return result;
}
}
int main(int argc,char** argv) {
  try {const auto result=probe(argc,argv);std::cout<<result.dump()<<'\n';return result.at("passed").get<bool>()?0:1;}
  catch(const std::exception& error){std::cout<<nlohmann::json({{"schema","codec-roundtrip-v1"},{"passed",false},{"stage","arguments"},{"error",error.what()}}).dump()<<'\n';return 1;}
}
