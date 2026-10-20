// Explicit hardware bench utility. Never registered as an automatic CTest.
// No VehicleMediaRuntime, control service, CAN adapter or HTTP client is created.
#include "mine_teleop/media.hpp"
#include "mine_teleop/detail/gstreamer_freshness.hpp"
#include "mine_teleop/upload.hpp"
#include "bench_capture_native.hpp"
#include <csignal>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <poll.h>
#include <unistd.h>

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using mine_teleop::Json;
using Clock = std::chrono::steady_clock;
std::int64_t milliseconds(Clock::time_point time) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
}
struct Options {
  std::filesystem::path config;
  std::filesystem::path evidence_dir;
  int frames{150};
  bool media{false};
  bool validate_only{false};
};
Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument(argv[index]);
    if (argument == "--config" && index + 1 < argc) options.config = argv[++index];
    else if (argument == "--evidence-dir" && index + 1 < argc) options.evidence_dir = argv[++index];
    else if (argument == "--frames" && index + 1 < argc) {
      const std::string value(argv[++index]);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), options.frames);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || options.frames < 30 || options.frames > 300)
        throw std::invalid_argument("--frames must be an integer from 30 to 300");
    } else if (argument == "--media") options.media = true;
    else if (argument == "--validate-only") options.validate_only = true;
    else throw std::invalid_argument("unknown/incomplete option: " + argument);
  }
  if (options.config.empty() || !options.config.is_absolute() || !std::filesystem::is_regular_file(options.config))
    throw std::invalid_argument("provide an explicit absolute regular-file --config path to a dedicated bench copy");
  if (!options.validate_only && (options.evidence_dir.empty() || !options.evidence_dir.is_absolute())) throw std::invalid_argument("capture requires explicit --evidence-dir /absolute/new-task-directory");
  return options;
}
std::vector<mine_teleop::CameraConfig> validate_config(const mine_teleop::VehicleConfig& config) {
  if (config.runtime.control_enabled || config.vehicle_adapter.type != "mock")
    throw std::invalid_argument("bench probe requires control_enabled=false and vehicle_adapter.type=mock");
  if (config.runtime.critical_camera_stale_brake_ms <= 0)
    throw std::invalid_argument("bench video freshness budget must be positive");
  auto cameras = config.enabled_cameras();
  std::set<std::string> expected{"/dev/ccg2-channel-6", "/dev/ccg2-channel-7"};
  if (cameras.size() != expected.size()) throw std::invalid_argument("exactly channels 6 and 7 must be enabled in the bench copy");
  for (const auto& camera : cameras) {
    if (camera.backend != "ccg2" || expected.erase(camera.device) != 1 || camera.capture_fps != 20)
      throw std::invalid_argument("only explicit ccg2 channels 6/7 with capture_fps=20 are allowed");
    const auto& output = config.realtime_profile(camera.realtime_profile);
    if (output.fps != 20) throw std::invalid_argument("bench realtime output must request 20 FPS");
    static_cast<void>(mine_teleop::camera_input_spec(camera, output));
  }
  return cameras;
}

struct StageSamples {
  std::mutex mutex;
  std::uint64_t count{0}, invalid_age{0};
  std::int64_t first_callback_ms{0}, last_callback_ms{0};
  std::vector<std::uint64_t> ages_ms;
  void note(std::int64_t now, std::optional<std::uint64_t> age) {
    std::lock_guard lock(mutex);
    if (count++ == 0) first_callback_ms = now;
    last_callback_ms = now;
    if (age) ages_ms.push_back(*age); else ++invalid_age;
  }
  Json snapshot(std::int64_t window_ms) {
    std::lock_guard lock(mutex);
    auto ages = ages_ms;
    std::sort(ages.begin(), ages.end());
    const auto span = last_callback_ms - first_callback_ms;
    std::uint64_t total = 0;
    for (const auto age : ages) total += age;
    return {{"count", count}, {"first_callback_monotonic_ms", first_callback_ms},
      {"last_callback_monotonic_ms", last_callback_ms},
      {"fps_over_capture_window", window_ms > 0 ? Json(count * 1000.0 / window_ms) : Json(nullptr)},
      {"callback_delivery_fps", count > 1 && span > 0 ? Json((count - 1) * 1000.0 / span) : Json(nullptr)},
      {"invalid_age_samples", invalid_age}, {"valid_age_samples", ages.size()},
      {"age_mean_ms", ages.empty() ? Json(nullptr) : Json(static_cast<double>(total) / ages.size())},
      {"age_p95_ms", ages.empty() ? Json(nullptr) : Json(ages[(ages.size() - 1) * 95 / 100])},
      {"age_max_ms", ages.empty() ? Json(nullptr) : Json(ages.back())}};
  }
};

struct ElementUnref { void operator()(GstElement* value) const { if (value != nullptr) gst_object_unref(value); } };
using Element = std::unique_ptr<GstElement, ElementUnref>;

class LocalMedia {
 public:
  LocalMedia(const mine_teleop::CameraInputSpec& input, const mine_teleop::MediaProfile& output, int freshness_ms)
      : freshness_(mine_teleop::detail::AccessUnitCodec::H264, static_cast<std::uint64_t>(freshness_ms)) {
    gst_segment_init(&encoded_segment_, GST_FORMAT_UNDEFINED);
    const auto description = mine_teleop::build_camera_input_pipeline("input", input, output) +
      "! x264enc name=encoder tune=zerolatency speed-preset=ultrafast key-int-max=" + std::to_string(output.fps) +
      " bframes=0 byte-stream=true bitrate=" + std::to_string(std::max(1, output.bitrate_kbps)) +
      " ! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au ! " +
      mine_teleop::build_encoded_frame_queue("send", 2) +
      "! h264parse ! avdec_h264 ! appsink name=decoded emit-signals=true max-buffers=2 drop=false sync=false async=false";
    GError* error = nullptr;
    pipeline_ = gst_parse_launch(description.c_str(), &error);
    const std::string message = error == nullptr ? "" : error->message;
    if (error != nullptr) g_error_free(error);
    if (pipeline_ == nullptr || !message.empty()) {
      if (pipeline_ != nullptr) gst_object_unref(pipeline_);
      pipeline_ = nullptr;
      throw std::runtime_error("local software pipeline parse failed: " + message);
    }
    try {
      input_ = require_element("input");
      Element encoder(require_element("encoder"));
      Element queue(require_element("send"));
      Element decoded(require_element("decoded"));
      auto* sink_pad = gst_element_get_static_pad(encoder.get(), "sink");
      if (sink_pad == nullptr) throw std::runtime_error("local encoder sink pad is missing");
      gst_pad_add_probe(sink_pad, GST_PAD_PROBE_TYPE_BUFFER,
        +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          auto& self = *static_cast<LocalMedia*>(data);
          const auto now = mine_teleop::detail::media_steady_now_ms();
          const auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
          const auto elapsed = now - self.freshness_.origin_steady_ms;
          const auto pts = buffer == nullptr ? GST_CLOCK_TIME_NONE : GST_BUFFER_PTS(buffer);
          const auto age = GST_CLOCK_TIME_IS_VALID(pts) && elapsed >= 0 && pts / GST_MSECOND <= static_cast<std::uint64_t>(elapsed)
              ? std::optional<std::uint64_t>(static_cast<std::uint64_t>(elapsed) - pts / GST_MSECOND) : std::nullopt;
          self.raw_.note(now, age);
          return GST_PAD_PROBE_OK;
        }, this, nullptr);
      gst_object_unref(sink_pad);
      auto* src_pad = gst_element_get_static_pad(encoder.get(), "src");
      if (src_pad == nullptr) throw std::runtime_error("local encoder source pad is missing");
      gst_pad_add_probe(src_pad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
        +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
          auto& self = *static_cast<LocalMedia*>(data);
          if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
            auto* event = GST_PAD_PROBE_INFO_EVENT(info);
            if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
              const GstSegment* segment = nullptr;
              gst_event_parse_segment(event, &segment);
              gst_segment_copy_into(segment, &self.encoded_segment_);
            }
          } else {
            const auto now = mine_teleop::detail::media_steady_now_ms();
            self.encoded_.note(now, mine_teleop::detail::encoded_running_time_age_ms(
                self.encoded_segment_, GST_BUFFER_PTS(GST_PAD_PROBE_INFO_BUFFER(info)), now - self.freshness_.origin_steady_ms));
          }
          return GST_PAD_PROBE_OK;
        }, this, nullptr);
      gst_object_unref(src_pad);
      auto* send_pad = gst_element_get_static_pad(queue.get(), "src");
      mine_teleop::detail::attach_encoded_send_freshness(send_pad, freshness_);
      gst_object_unref(send_pad);
      g_signal_connect(decoded.get(), "new-sample", G_CALLBACK(+[](GstAppSink* sink, gpointer data) -> GstFlowReturn {
        auto& self = *static_cast<LocalMedia*>(data);
        auto* sample = gst_app_sink_pull_sample(sink);
        if (sample == nullptr) return GST_FLOW_EOS;
        const auto now = mine_teleop::detail::media_steady_now_ms();
        const auto* buffer = gst_sample_get_buffer(sample);
        const auto* segment = gst_sample_get_segment(sample);
        const auto age = segment == nullptr || buffer == nullptr ? std::nullopt : mine_teleop::detail::encoded_running_time_age_ms(
            *segment, GST_BUFFER_PTS(buffer), now - self.freshness_.origin_steady_ms);
        self.decoded_.note(now, age);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
      }), this);
      freshness_.origin_steady_ms = mine_teleop::detail::media_steady_now_ms();
      if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
        throw std::runtime_error("local software pipeline could not enter PLAYING");
    } catch (...) { close(); throw; }
  }
  ~LocalMedia() { close(); }
  bool push(const mine_teleop::EncodedFrame& frame) {
    const auto elapsed = milliseconds(frame.captured_steady_at) - freshness_.origin_steady_ms;
    if (elapsed < 0) { ++preorigin_dropped_; return false; }
    auto* buffer = gst_buffer_new_allocate(nullptr, frame.payload.size(), nullptr);
    if (buffer == nullptr) throw std::runtime_error("local GStreamer buffer allocation failed");
    gst_buffer_fill(buffer, 0, frame.payload.data(), frame.payload.size());
    GST_BUFFER_PTS(buffer) = static_cast<GstClockTime>(elapsed) * GST_MSECOND;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DURATION(buffer) = GST_SECOND / 20;
    const auto flow = gst_app_src_push_buffer(GST_APP_SRC(input_), buffer);
    if (flow != GST_FLOW_OK) throw std::runtime_error("local appsrc rejected camera frame: " + std::to_string(flow));
    ++pushed_; return true;
  }
  void finish() {
    if (finished_) return;
    gst_app_src_end_of_stream(GST_APP_SRC(input_));
    auto* bus = gst_element_get_bus(pipeline_);
    auto* message = gst_bus_timed_pop_filtered(bus, 5 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    drain_completed_ms_ = mine_teleop::detail::media_steady_now_ms();
    eos_seen_ = message != nullptr && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message != nullptr && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      GError* error = nullptr; gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      error_ = error == nullptr ? "unknown local pipeline error" : error->message;
      if (error != nullptr) g_error_free(error);
      g_free(debug);
    }
    if (message != nullptr) gst_message_unref(message);
    gst_object_unref(bus);
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    finished_ = true;
    if (!eos_seen_ || !error_.empty()) throw std::runtime_error(error_.empty() ? "local pipeline EOS drain timed out" : error_);
  }
  Json snapshot(std::int64_t window_ms) {
    return {{"mode", "local_x264_h264parse_production_send_gate_avdec_h264"},
      {"scope", "no_WebRTC_network_recording_or_hardware_encoder"},
      {"origin_monotonic_ms", freshness_.origin_steady_ms}, {"drain_completed_monotonic_ms", drain_completed_ms_},
      {"pushed_frames", pushed_}, {"preorigin_raw_frames_dropped", preorigin_dropped_},
      {"raw_to_encoder", raw_.snapshot(window_ms)}, {"encoded", encoded_.snapshot(window_ms)},
      {"decoded", decoded_.snapshot(window_ms)}, {"fresh_send_frames", freshness_.fresh_frames.load()},
      {"fresh_send_fps_over_capture_window", window_ms > 0 ? Json(freshness_.fresh_frames.load() * 1000.0 / window_ms) : Json(nullptr)},
      {"send_consecutive_anomalies", freshness_.consecutive_anomalies.load()},
      {"send_stale_dropped", freshness_.dropped_stale.load()}, {"send_invalid_dropped", freshness_.dropped_invalid.load()},
      {"send_dependent_dropped", freshness_.dropped_waiting_idr.load()},
      {"send_key_unit_requests", freshness_.key_unit_requests.load()},
      {"send_key_unit_request_failures", freshness_.key_unit_request_failures.load()},
      {"last_forwarded_capture_age_ms", freshness_.last_forwarded_age_ms.load()},
      {"eos_seen", eos_seen_}, {"error", error_}};
  }
 private:
  GstElement* require_element(const char* name) {
    auto* element = gst_bin_get_by_name(GST_BIN(pipeline_), name);
    if (element == nullptr) throw std::runtime_error(std::string("local pipeline element missing: ") + name);
    return element;
  }
  void close() {
    if (pipeline_ != nullptr) gst_element_set_state(pipeline_, GST_STATE_NULL);
    if (input_ != nullptr) { gst_object_unref(input_); input_ = nullptr; }
    if (pipeline_ != nullptr) { gst_object_unref(pipeline_); pipeline_ = nullptr; }
  }
  GstElement* pipeline_{nullptr};
  GstElement* input_{nullptr};
  mine_teleop::detail::EncodedSendFreshness freshness_;
  GstSegment encoded_segment_{};
  StageSamples raw_, encoded_, decoded_;
  std::uint64_t pushed_{0}, preorigin_dropped_{0};
  bool finished_{false}, eos_seen_{false};
  std::int64_t drain_completed_ms_{0};
  std::string error_;
};

Json capture_lane(const mine_teleop::VehicleConfig& config, const mine_teleop::CameraConfig& camera, const Options& options, int guarded_fd) {
  const auto output = config.realtime_profile(camera.realtime_profile);
  const auto input = mine_teleop::camera_input_spec(camera, output);
  auto capture = output;
  capture.codec = input.codec; capture.width = input.width; capture.height = input.height;
  capture.fps = input.fps; capture.encoder = "native";
  Json mutations = Json::array();
  mine_teleop::BenchPreopenedV4l2Fd bench{guarded_fd, [&](std::string_view operation, int rc, int code, const Json& fields) {
    mutations.push_back({{"operation",operation},{"rc",rc},{"errno",code},{"returned_fields",fields},
      {"recorded_at_utc_ms",mine_teleop::bench::utc_ms()},{"recorded_at_monotonic_ms",milliseconds(Clock::now())}});
  }};
  mine_teleop::CameraFrameSource source(camera, capture, 3000, std::move(bench));
  std::unique_ptr<LocalMedia> media;
  Json samples = Json::array();
  std::string error;
  const auto admission = Clock::now();
  std::optional<Clock::time_point> first_delivery, last_delivery, first_capture, last_capture;
  std::uint64_t gap_total = 0;
  Json first_diagnostics;
  try {
    if (options.media) media = std::make_unique<LocalMedia>(input, output, config.runtime.critical_camera_stale_brake_ms);
    for (int index = 1; index <= options.frames; ++index) {
      if (Clock::now() - admission > std::chrono::seconds(25)) throw std::runtime_error("bounded bench capture window expired");
      const auto frame = source.next(static_cast<std::uint64_t>(index));
      const auto delivered = Clock::now();
      if (!first_delivery) { first_delivery = delivered; first_capture = frame.captured_steady_at; first_diagnostics = source.capture_diagnostics(); }
      last_delivery = delivered; last_capture = frame.captured_steady_at;
      gap_total += frame.source_sequence_gap;
      const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(delivered - frame.captured_steady_at).count();
      samples.push_back({{"frame", index}, {"delivered_monotonic_ms", milliseconds(delivered)},
        {"captured_monotonic_ms", milliseconds(frame.captured_steady_at)}, {"capture_age_at_delivery_ms", age},
        {"payload_bytes", frame.payload.size()}, {"source_sequence_valid", frame.source_sequence_valid},
        {"source_sequence", frame.source_sequence}, {"source_sequence_gap", frame.source_sequence_gap},
        {"device_timestamp_us", source.capture_diagnostics().value("device_timestamp_us", std::int64_t{0})},
        {"buffer_flags", source.capture_diagnostics().value("buffer_flags", std::uint32_t{0})},
        {"capture_timestamp_scope", source.capture_diagnostics().value("capture_timestamp_scope", "unknown")}});
      if (media) static_cast<void>(media->push(frame));
    }
    if (media) media->finish();
  } catch (const std::exception& failure) { error = failure.what(); }
  const auto window_ms = first_delivery && last_delivery
      ? std::chrono::duration_cast<std::chrono::milliseconds>(*last_delivery - *first_delivery).count() : 0;
  const auto capture_span_ms = first_capture && last_capture
      ? std::chrono::duration_cast<std::chrono::milliseconds>(*last_capture - *first_capture).count() : 0;
  const auto transitions = samples.empty() ? 0 : samples.size() - 1;
  Json result = {{"camera_id", camera.id}, {"device", camera.device}, {"requested_capture_fps", 20},
    {"target_encode_fps", 20}, {"captured_frames", samples.size()}, {"completed_requested_frames", samples.size() == static_cast<std::size_t>(options.frames)},
    {"first_delivery_monotonic_ms", first_delivery ? Json(milliseconds(*first_delivery)) : Json(nullptr)},
    {"last_delivery_monotonic_ms", last_delivery ? Json(milliseconds(*last_delivery)) : Json(nullptr)},
    {"delivery_window_ms", window_ms}, {"observed_delivery_fps", window_ms > 0 ? Json(transitions * 1000.0 / window_ms) : Json(nullptr)},
    {"capture_timestamp_window_ms", capture_span_ms}, {"capture_timestamp_fps", capture_span_ms > 0 ? Json(transitions * 1000.0 / capture_span_ms) : Json(nullptr)},
    {"capture_rate_scope", "actual_host_driver_delivery_not_sensor_trigger_verification"},
    {"completion_scope", "frame_sample_collection_only_not_20fps_or_vehicle_acceptance"},
    {"driver_diagnostics_first", first_diagnostics}, {"driver_diagnostics_last", source.capture_diagnostics()},
    {"source_sequence_gaps_total", gap_total}, {"frames", std::move(samples)}, {"mutation_ioctls",mutations},
    {"mutation_trace_scope","normal_child_result_only_crash_trace_may_be_missing"}, {"error", error}};
  if (media) result["local_media"] = media->snapshot(window_ms);
  return result;
}
volatile std::sig_atomic_t supervisor_signal=0;
void request_stop(int value){supervisor_signal=value;}
struct OwnedBenchFd {
  int value{-1};
  explicit OwnedBenchFd(int fd):value(fd){}
  ~OwnedBenchFd(){reset();}
  void reset()noexcept{if(value>=0){::close(value);value=-1;}}
};
struct SupervisedChild {
  pid_t pid{-1};bool reaped{false},termination_attempted{false};int status{0},wait_error{0};
  ~SupervisedChild(){if(pid>0&&!reaped)terminate();}
  bool observe(){if(reaped)return true;const auto rc=::waitpid(pid,&status,WNOHANG);if(rc==pid)reaped=true;else if(rc<0&&errno!=EINTR)wait_error=errno;return reaped;}
  void terminate()noexcept{if(pid<=0||reaped||wait_error!=0)return;if(termination_attempted){observe();return;}termination_attempted=true;::kill(pid,SIGTERM);const auto start=Clock::now();while(Clock::now()-start<std::chrono::seconds(2)){if(observe()||wait_error!=0)return;if(Clock::now()-start>=std::chrono::milliseconds(200))::kill(pid,SIGKILL);std::this_thread::sleep_for(std::chrono::milliseconds(5));}}
};
Json supervise_capture(const mine_teleop::VehicleConfig& config,const std::vector<mine_teleop::CameraConfig>& cameras,const Options& options,mine_teleop::bench::CaptureContainment& guard,mine_teleop::bench::DurableJournal& journal,SupervisedChild& child){
  std::string payload;payload.reserve(2U*1024U*1024U); // All large admission allocation precedes fork/mutation.
  int channel[2]{-1,-1};if(::pipe2(channel,O_CLOEXEC)<0)throw std::runtime_error("bench result IPC create failed");
  OwnedBenchFd read_fd(channel[0]),write_fd(channel[1]);
  const auto parent_pid=::getpid();
  child.pid=::fork();
  if(child.pid<0){read_fd.reset();write_fd.reset();throw std::runtime_error("bench capture fork failed before source mutation");}
  if(child.pid==0){
    read_fd.reset();journal.close_in_child();
    // Plugin initialization occurs only after fork, before child threads start.
    // Parent death stops the child, but cannot restore a dead parent's snapshots.
    ::signal(SIGTERM,SIG_DFL);::signal(SIGINT,SIG_DFL);::signal(SIGPIPE,SIG_DFL);
    if(::prctl(PR_SET_PDEATHSIG,SIGTERM)<0||::getppid()!=parent_pid)::_exit(3);
    try{
      ::setenv("GST_REGISTRY_FORK","no",1);if(options.media)gst_init(nullptr,nullptr);
      std::vector<Json> results(cameras.size());std::vector<std::jthread> workers;
      for(std::size_t i=0;i<cameras.size();++i)workers.emplace_back([&,i]{try{results[i]=capture_lane(config,cameras[i],options,guard.fd(i));}catch(const std::exception& error){results[i]={{"device",cameras[i].device},{"error",error.what()}};}});
      for(auto& worker:workers)worker.join();
      const auto payload=Json({{"results",results},{"capture_threads_joined",true},{"source_objects_destroyed",true}}).dump();
      for(std::size_t at=0;at<payload.size();){const auto n=::write(channel[1],payload.data()+at,payload.size()-at);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::runtime_error("bench result IPC failed");at+=static_cast<std::size_t>(n);}
      write_fd.reset();::_exit(0);
    }catch(...){::_exit(3);}
  }
  write_fd.reset();const auto flags=::fcntl(channel[0],F_GETFL);if(flags<0||::fcntl(channel[0],F_SETFL,flags|O_NONBLOCK)<0){read_fd.reset();child.terminate();return {{"child_pid",child.pid},{"child_reaped",child.reaped},{"error","bench IPC nonblocking failed"}};}
  std::string error;const auto started=Clock::now();auto next_occupancy=started;
  try{
    journal.record({{"event","bench_capture_child_started"},{"child_pid",child.pid},{"potential_mutation_dispatched",true}});
    while(!child.observe()){
      if(child.wait_error!=0)throw std::runtime_error("waitpid child lifecycle unconfirmed; no PID-based restore permitted");
      if(supervisor_signal!=0)throw std::runtime_error("supervisor received stop signal "+std::to_string(supervisor_signal));
      if(Clock::now()-started>std::chrono::seconds(40))throw std::runtime_error("supervised capture/teardown deadline exceeded");
      if(Clock::now()>=next_occupancy){const auto occupied=guard.occupancy(child.pid);if(!occupied.value("known",false)||!occupied.at("foreign_owners").empty())throw std::runtime_error("foreign/unknown selected-device owner observed during capture");next_occupancy=Clock::now()+std::chrono::milliseconds(500);}
      std::array<char,8192> data{};for(unsigned i=0;i<8;++i){const auto n=::read(channel[0],data.data(),data.size());if(n<0&&errno==EINTR)continue;if(n<=0)break;if(payload.size()+static_cast<std::size_t>(n)>2U*1024U*1024U)throw std::runtime_error("bench result exceeds 2MiB bound");payload.append(data.data(),static_cast<std::size_t>(n));}
      pollfd waiter{channel[0],POLLIN,0};static_cast<void>(::poll(&waiter,1,10));
    }
  }catch(const std::exception& failure){error=failure.what();child.terminate();}
  // Once reaped, no writer remains: drain the bounded final result before parse.
  if(child.reaped){std::array<char,8192> data{};for(unsigned i=0;i<256;++i){const auto n=::read(channel[0],data.data(),data.size());if(n<=0)break;if(payload.size()+static_cast<std::size_t>(n)>2U*1024U*1024U){error="bench result exceeds 2MiB bound";break;}payload.append(data.data(),static_cast<std::size_t>(n));}}
  read_fd.reset();auto result=Json::parse(payload,nullptr,false);if(result.is_discarded()||!result.is_object())result=Json::object();
  result["child_pid"]=child.pid;result["child_reaped"]=child.reaped;result["child_exit_status"]=child.status;result["waitpid_errno"]=child.wait_error;
  result["child_exited_normally"]=child.reaped&&WIFEXITED(child.status)&&WEXITSTATUS(child.status)==0;
  result["all_child_threads_and_fds_gone"]=child.reaped;result["supervisor_error"]=error;
  result["cleanup_pending"]=!child.reaped;return result;
}

} // namespace

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Explicit CCG2 channels 6/7 bench capture; no CAN/HTTP/WebRTC.\n"
        "--config /absolute/path/to/dedicated-copy.yaml --evidence-dir /absolute/new-task-directory [--frames 150] [--media] [--validate-only]\n"
        "Requires exactly 6/7 enabled, backend ccg2, capture/output 20 FPS, control disabled, mock adapter.\n";
    return 0;
  }
  try {
    const auto options = parse_options(argc, argv);
    const auto config = mine_teleop::load_vehicle_config(options.config);
    const auto cameras = validate_config(config); // All guards precede any device open/GStreamer play.
    if (options.validate_only) {
      std::cout << Json({{"event", "bench_capture_config_validated"}, {"device_io", false},
        {"control_enabled", false}, {"adapter_type", "mock"}, {"enabled_devices", {"/dev/ccg2-channel-6", "/dev/ccg2-channel-7"}}}).dump() << '\n';
      return 0;
    }
    ::signal(SIGCHLD,SIG_DFL);
    struct sigaction action{};action.sa_handler=request_stop;sigemptyset(&action.sa_mask);
    if(::sigaction(SIGINT,&action,nullptr)<0||::sigaction(SIGTERM,&action,nullptr)<0)throw std::runtime_error("cannot install cooperative supervisor signals");
    mine_teleop::bench::NativeDeviceOps ops;mine_teleop::bench::CaptureContainment guard(ops);
    mine_teleop::bench::DurableJournal journal(options.evidence_dir);
    guard.snapshot({cameras[0].device,cameras[1].device});
    journal.record({{"event","bench_capture_dual_baseline_durable"},{"config_sha256",mine_teleop::sha256_file(options.config)},
      {"binary_sha256",mine_teleop::sha256_file("/proc/self/exe")},{"snapshot",guard.baselines()},
      {"scope","G_FMT_G_PARM_not_unknown_driver_private_sensor_controls"}});
    if(supervisor_signal!=0)throw std::runtime_error("stop signal before capture; no mutation dispatched");
    guard.permit_mutation_after_durable_snapshot();
    SupervisedChild child;
    mine_teleop::bench::ScopedRestore restore_on_exit(guard,child,[&](const Json& state){
      journal.record({{"event","bench_restore_after_supervisor_exception"},{"restore_result",state},{"child_pid",child.pid},{"child_reaped",child.reaped}});
    });
    Json capture;
    try{capture=supervise_capture(config,cameras,options,guard,journal,child);}
    catch(const std::exception& failure){child.terminate();capture={{"child_pid",child.pid},{"child_reaped",child.reaped},{"cleanup_pending",child.pid>0&&!child.reaped},{"supervisor_error",failure.what()}};}
    // A reaped child proves all its threads, mappings and fd duplicates exited,
    // including abnormal exits. No restore runs against a still-live child.
    Json restored;
    try{restored=restore_on_exit.finish();}catch(const std::exception& failure){restored={{"success",false},{"error",failure.what()}};}
    capture["child_reaped"]=child.reaped;capture["cleanup_pending"]=child.pid>0&&!child.reaped;capture["all_child_threads_and_fds_gone"]=child.reaped;
    bool measurement=capture.value("child_exited_normally",false)&&capture.value("supervisor_error","").empty()&&capture.contains("results")&&capture["results"].is_array()&&capture["results"].size()==2;
    if(measurement)for(const auto& lane:capture["results"])measurement=measurement&&lane.value("completed_requested_frames",false)&&lane.value("error","").empty();
    Json result={{"event","bench_capture_supervised_result"},{"measurement_completed",measurement},
      {"capture20_acceptance","separate_delivery_timestamp_sequence_review_not_automatic_from_collection"},
      {"cleanup_result",{{"child_reaped",capture.value("child_reaped",false)},{"cleanup_pending",capture.value("cleanup_pending",true)}}},
      {"restore_result",restored},{"capture_result",capture},{"evidence_path",journal.path().string()},
      {"overall_success",measurement&&restored.value("success",false)},{"control_enabled",false},{"adapter_type","mock"},
      {"failure_boundary","supervisor_SIGKILL_power_loss_kernel_hang_or_external_owner_race_cannot_guarantee_restore"}};
    try{journal.record(result);}catch(const std::exception& failure){result["evidence_persisted"]=false;result["evidence_error"]=failure.what();result["overall_success"]=false;std::cout<<result.dump()<<'\n';return 2;}
    std::cout<<result.dump()<<'\n';
    return measurement&&restored.value("success",false)?0:1;
  } catch (const std::exception& error) {
    std::cerr << Json({{"event", "bench_capture_probe_rejected_or_failed"}, {"error", error.what()}}).dump() << '\n';
    return 2;
  }
}
