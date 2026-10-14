// Actual local encoder tee + production send gate + isolated splitmux/filesink.
// No HTTP, WebRTC peer, physical camera, CAN, server, or installation is used.
#include "mine_teleop/recording.hpp"
#include "mine_teleop/detail/gstreamer_freshness.hpp"
#include "mine_teleop/media.hpp"
#include <gst/app/gstappsink.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <unistd.h>
#include <fcntl.h>
#include <sys/wait.h>
using namespace mine_teleop;
namespace {
std::int64_t steady_ms(){return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
void check(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct Pipeline {
  GstElement* value{};
  explicit Pipeline(const std::string& description){GError* error=nullptr;value=gst_parse_launch(description.c_str(),&error);std::string message=error?error->message:"";if(error)g_error_free(error);check(value&&message.empty(),("parse "+message).c_str());}
  ~Pipeline(){gst_element_set_state(value,GST_STATE_NULL);gst_object_unref(value);}
  GstElement* element(const std::string& name){auto* result=gst_bin_get_by_name(GST_BIN(value),name.c_str());check(result,"element absent");return result;}
};
struct Tap {IsolatedRecordingSession* recorder;std::size_t lane;};
struct Counters {std::atomic<unsigned> decoded{0};};
void tap(GstElement* element,Tap& context){
  g_signal_connect(element,"new-sample",G_CALLBACK(+[](GstAppSink* sink,gpointer data)->GstFlowReturn{
    auto& c=*static_cast<Tap*>(data);auto* sample=gst_app_sink_pull_sample(sink);
    if(sample){const auto* segment=gst_sample_get_segment(sample);if(segment)c.recorder->admit(c.lane,gst_sample_get_buffer(sample),*segment);else c.recorder->reject_admission(c.lane,false);gst_sample_unref(sample);}return GST_FLOW_OK;
  }),&context);
  g_signal_connect(element,"eos",G_CALLBACK(+[](GstAppSink*,gpointer data){auto& c=*static_cast<Tap*>(data);c.recorder->end_lane(c.lane);}),&context);
}
unsigned decode_file(const std::filesystem::path& path,const std::string& codec){
  // paths are generated inside a fixed safe test directory; no user path/URI.
  Pipeline decode("filesrc location="+path.string()+" ! qtdemux ! "+codec+"parse ! avdec_"+codec+" ! fakesink name=decoded sync=false async=false signal-handoffs=true enable-last-sample=false");
  auto* sink=decode.element("decoded");Counters result;
  g_signal_connect(sink,"handoff",G_CALLBACK(+[](GstElement*,GstBuffer*,GstPad*,gpointer data){++static_cast<Counters*>(data)->decoded;}),&result);
  check(gst_element_set_state(decode.value,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"decode play");auto* bus=gst_element_get_bus(decode.value);auto* message=gst_bus_timed_pop_filtered(bus,5*GST_SECOND,static_cast<GstMessageType>(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
  const bool done=message&&GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS;if(message)gst_message_unref(message);gst_object_unref(bus);gst_object_unref(sink);check(done,"completed MP4 must actually decode to EOS");return result.decoded;
}
int fd_count(){int count=0;for(int fd=0;fd<1024;++fd)if(::fcntl(fd,F_GETFD)>=0)++count;return count;}
Json read(const std::filesystem::path& path){std::ifstream input(path);Json value;input>>value;return value;}
void scenario(const std::filesystem::path& self,const std::filesystem::path& root,const std::string& mode,const std::string& codec,int lane_count=1,unsigned frames=120){
  const auto session=mode+"_"+codec;const auto dir=root/"vehicle"/session;std::filesystem::remove_all(dir);
  RecordingSessionSettings settings;settings.vehicle_id="vehicle";settings.session_id=session;settings.attempt_id=mode;settings.root=root;settings.worker_executable=self;
  std::string description;
  for(int i=0;i<lane_count;++i){const auto id=std::to_string(i);settings.lanes.push_back({"cam"+id,codec,"software",20,1});
    const auto encoder=codec=="h264"?"x264enc tune=zerolatency speed-preset=ultrafast key-int-max=20 bframes=0 byte-stream=true":"x265enc tune=zerolatency speed-preset=ultrafast key-int-max=20 option-string=pools=1:frame-threads=1:open-gop=0";
    description+="videotestsrc is-live=true num-buffers="+std::to_string(frames)+" pattern=ball ! video/x-raw,format=I420,width=160,height=120,framerate=20/1 ! "+encoder+" ! "+codec+"parse config-interval=-1 ! video/x-"+codec+",stream-format=byte-stream,alignment=au ! tee name=t"+id+" t"+id+". ! "+build_encoded_frame_queue("send"+id,2)+" ! "+codec+"parse ! avdec_"+codec+" ! fakesink name=send_sink"+id+" sync=false async=false signal-handoffs=true enable-last-sample=false t"+id+". ! appsink name=record_tap"+id+" emit-signals=true max-buffers=1 drop=false sync=false async=false enable-last-sample=false ";
  }
  IsolatedRecordingSession recorder(settings);Pipeline pipeline(description);
  std::vector<std::unique_ptr<detail::EncodedSendFreshness>> gates;std::vector<Tap> taps;std::vector<std::unique_ptr<Counters>> counts;
  taps.reserve(static_cast<std::size_t>(lane_count));
  std::vector<GstElement*> taps_elements;
  for(int i=0;i<lane_count;++i){const auto id=std::to_string(i);gates.push_back(std::make_unique<detail::EncodedSendFreshness>(codec=="h264"?detail::AccessUnitCodec::H264:detail::AccessUnitCodec::H265,500));gates.back()->origin_steady_ms=steady_ms();gates.back()->now_ms=[] {return steady_ms();};auto* queue=pipeline.element("send"+id);auto* pad=gst_element_get_static_pad(queue,"src");detail::attach_encoded_send_freshness(pad,*gates.back());gst_object_unref(pad);gst_object_unref(queue);
    counts.push_back(std::make_unique<Counters>());auto* sink=pipeline.element("send_sink"+id);g_signal_connect(sink,"handoff",G_CALLBACK(+[](GstElement*,GstBuffer*,GstPad*,gpointer data){++static_cast<Counters*>(data)->decoded;}),counts.back().get());gst_object_unref(sink);
    taps.push_back({&recorder,static_cast<std::size_t>(i)});auto* recording=pipeline.element("record_tap"+id);tap(recording,taps.back());taps_elements.push_back(recording);
  }
  check(gst_element_set_state(pipeline.value,GST_STATE_PLAYING)!=GST_STATE_CHANGE_FAILURE,"play actual tee");auto* bus=gst_element_get_bus(pipeline.value);auto* message=gst_bus_timed_pop_filtered(bus,15*GST_SECOND,static_cast<GstMessageType>(GST_MESSAGE_EOS|GST_MESSAGE_ERROR));
  const bool eos=message&&GST_MESSAGE_TYPE(message)==GST_MESSAGE_EOS;if(message)gst_message_unref(message);gst_object_unref(bus);check(eos,"record fault must not post error/EOS-stop live encoded tee");
  recorder.request_stop();gst_element_set_state(pipeline.value,GST_STATE_NULL);const auto finish_start=std::chrono::steady_clock::now();recorder.finish();check(std::chrono::steady_clock::now()-finish_start<std::chrono::seconds(9),"recorder-only stop deadline");
  for(std::size_t i=0;i<taps_elements.size();++i){g_signal_handlers_disconnect_by_data(taps_elements[i],&taps[i]);gst_object_unref(taps_elements[i]);} // NULL already stops callbacks.
  const bool healthy=mode=="healthy"||mode=="reconnect";
  for(int i=0;i<lane_count;++i){check(counts[static_cast<std::size_t>(i)]->decoded==frames,"fresh send remains fully decodable under actual record fault");const auto state=recorder.lane_status(static_cast<std::size_t>(i));
    if(healthy){if(!(state["state"]=="finalized"&&state["final_eos_received"]==true&&state["unfinished_segments"]==0)){try{
      std::cerr<<"REC_DIAG case="<<session<<" camera=cam"<<i<<" lane_status="<<state.dump()<<" session_status="<<recorder.session_status().dump()<<" artifact_dir="<<dir<<'\n';
      const auto summary=dir/"recording-session.status";if(std::filesystem::is_regular_file(summary))std::cerr<<"REC_DIAG persistent_summary="<<read(summary).dump()<<'\n';
      const auto trace=dir/("recording-worker-"+mode+".diagnostic.jsonl");if(std::filesystem::is_regular_file(trace)&&std::filesystem::file_size(trace)<=512U*1024U){std::ifstream input(trace);std::cerr<<"REC_DIAG worker_trace_begin\n"<<input.rdbuf()<<"REC_DIAG worker_trace_end\n";}
    }catch(const std::exception& diagnostic){std::cerr<<"REC_DIAG unavailable="<<diagnostic.what()<<'\n';}}check(state["state"]=="finalized"&&state["final_eos_received"]==true&&state["unfinished_segments"]==0,"actual splitmux closes every fragment");check(state["accepted_access_units"]==frames&&state["worker_input_access_units"]==frames,"parent-to-appsrc exact AU coverage");unsigned decoded=0;for(const auto& entry:std::filesystem::directory_iterator(dir/("cam"+std::to_string(i))))if(entry.path().extension()==".mp4"){decoded+=decode_file(entry.path(),codec);auto side=entry.path();side.replace_extension(".json");const auto metadata=read(side);check(metadata["finalized"]==true&&metadata["incomplete"]==false&&metadata["hash_state"]=="pending","only actual closed fragment gains pending complete sidecar");}check(decoded==frames,"completed recording decodes all original pictures");
    }else{check(state["incomplete"]==true&&state["active"]==false&&state["state"]!="finalized","record failure explicit inactive/incomplete");if(mode=="diskfull")check(state["issue_code"]=="recording_disk_full","actual filesink ENOSPC diagnosis");for(const auto& entry:std::filesystem::recursive_directory_iterator(dir))if(entry.path().extension()==".json"){const auto metadata=read(entry.path());check(metadata["finalized"]==true&&metadata["incomplete"]==false,"unfinished clip has no complete uploadable sidecar");}}
  }
  if(healthy){const auto summary=read(dir/"recording-session.status");check(summary["attempt_complete"]==true&&summary["session_history_known"]==false,"attempt proof does not claim full SID completeness");}
  std::cout<<"PASS actual tee/record worker "<<mode<<" "<<codec<<" lanes="<<lane_count<<" frames="<<frames<<'\n';
}
}
int main(int argc,char** argv){
  if(argc==5){ // Production IPC interface, controlled test hooks only in this test binary.
    // Start config cannot be consumed twice. Hook selected by safe test-only
    // executable symlink filename, never production config/environment.
    const auto name=std::filesystem::path(argv[0]).filename().string();RecordingWorkerTestHooks hooks;hooks.emit_file_diagnostics=true;
    if(name=="slowwrite")hooks.stall_write_ms=10000;if(name=="slowfinalize")hooks.stall_finalize_ms=10000;if(name=="diskfull")hooks.disk_full=true;if(name=="workerdead")hooks.exit_after_access_units=10;
    return run_recording_worker(3,4,hooks);
  }
  try{gst_init(nullptr,nullptr);const auto self=std::filesystem::absolute(argv[0]);const auto root=std::filesystem::temp_directory_path()/("mine_teleop_recording_gst_"+std::to_string(::getpid()));std::filesystem::create_directories(root);
    scenario(self,root,"healthy","h264",2);scenario(self,root,"healthy","h265");
    for(const auto* mode:{"slowwrite","slowfinalize","diskfull","workerdead"}){const auto helper=root/mode;std::filesystem::create_symlink(self,helper);scenario(helper,root,mode,"h264",1,std::string(mode)=="slowfinalize"?20:120);}
    const auto before=fd_count();for(int i=0;i<3;++i)scenario(self,root,"reconnect","h264",1,40);check(fd_count()==before,"actual recorder reconnect leaked parent FDs");int child_status=0;check(::waitpid(-1,&child_status,WNOHANG)<0&&errno==ECHILD,"actual recorder left unreaped child");
    std::filesystem::remove_all(root);return 0;
  }catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
}
