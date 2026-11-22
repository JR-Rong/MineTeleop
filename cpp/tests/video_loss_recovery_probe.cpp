#include "mine_teleop/video.hpp"

#include <gst/gst.h>
#include <gst/rtp/gstrtpbuffer.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
constexpr int kFps = 30;
constexpr int kVideoBudgetMs = 500;

struct Observation {
  bool block_feedback{false};
  std::atomic<unsigned> dropped{0}, requests{0}, decoded{0}, idrs{0};
  std::atomic<std::uint64_t> first_rtp_ts{std::numeric_limits<std::uint64_t>::max()};
  std::atomic<long long> last_ns{0}, max_gap_ns{0}, last_idr_pts{-1}, max_idr_gap_ns{0};
};

void maximum(std::atomic<long long>& target, long long value) {
  auto current = target.load();
  while (value > current && !target.compare_exchange_weak(current, value)) {}
}

GstPadProbeReturn inject_loss(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto& observed = *static_cast<Observation*>(data);
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_UPSTREAM) != 0 &&
      gst_event_has_name(GST_PAD_PROBE_INFO_EVENT(info), "GstForceKeyUnit")) {
    ++observed.requests;
    if (observed.block_feedback) return GST_PAD_PROBE_DROP;
  }
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) != 0) {
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (gst_rtp_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), GST_MAP_READ, &rtp)) {
      const auto timestamp = gst_rtp_buffer_get_timestamp(&rtp);
      gst_rtp_buffer_unmap(&rtp);
      auto first = observed.first_rtp_ts.load();
      if (first == std::numeric_limits<std::uint64_t>::max()) {
        observed.first_rtp_ts = timestamp;
        first = timestamp;
      }
      const auto frame = static_cast<std::uint32_t>(timestamp - static_cast<std::uint32_t>(first)) / 3000;
      // Use the RTP clock: encoders may offset buffer PTS by many hours.
      if (frame >= 35 && frame <= 38) {
        ++observed.dropped;
        return GST_PAD_PROBE_DROP;
      }
    }
  }
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn observe_encoder(GstPad*, GstPadProbeInfo* info, gpointer data) {
  auto& observed = *static_cast<Observation*>(data);
  auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (buffer && !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT) &&
      GST_CLOCK_TIME_IS_VALID(GST_BUFFER_PTS(buffer))) {
    const auto pts = static_cast<long long>(GST_BUFFER_PTS(buffer));
    const auto previous = observed.last_idr_pts.exchange(pts);
    if (previous >= 0) maximum(observed.max_idr_gap_ns, pts - previous);
    ++observed.idrs;
  }
  return GST_PAD_PROBE_OK;
}

void observe_decode(GstElement*, GstBuffer*, GstPad*, gpointer data) {
  auto& observed = *static_cast<Observation*>(data);
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
  const auto previous = observed.last_ns.exchange(now);
  if (previous > 0 && observed.decoded.load() > 5) maximum(observed.max_gap_ns, now - previous);
  ++observed.decoded;
}

struct PipelineDelete {
  void operator()(GstElement* pipeline) const {
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
  }
};
}  // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    bool legacy = false;
    Observation observed;
    for (int index = 1; index < argc; ++index) {
      const std::string argument = argv[index];
      if (argument == "--legacy-gop") legacy = true;
      else if (argument == "--drop-keyframe-feedback") observed.block_feedback = true;
      else if (argument == "--help") {
        std::cout << "Synthetic NVENC H264 loss probe; no cameras or CAN. Five seconds at 720p30, "
                     "125 kbps, four RTP frames dropped. Options: --legacy-gop --drop-keyframe-feedback\n";
        return 0;
      } else throw std::invalid_argument("unknown probe argument");
    }
    std::string decoder;
    for (const char* name : {"avdec_h264", "openh264dec", "nvh264dec", "nvautogpuh264dec"}) {
      if (mine_teleop::gstreamer_factory_available(name)) { decoder = name; break; }
    }
    if (decoder.empty()) throw std::runtime_error("probe requires an H264 decoder");
    guint major = 0, minor = 0, micro = 0, nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    const int gop = legacy ? kFps : mine_teleop::realtime_keyframe_interval_frames(kFps, kVideoBudgetMs);
    const auto encoder = mine_teleop::build_nvenc_pipeline_stage("nvh264enc", {125, gop}, "enc", major, minor);
    const auto description =
        "videotestsrc is-live=true num-buffers=150 pattern=ball ! "
        "video/x-raw,format=NV12,width=1280,height=720,framerate=30/1 ! " + encoder +
        " ! h264parse config-interval=-1 ! video/x-h264,stream-format=byte-stream,alignment=au ! "
        "rtph264pay name=pay config-interval=-1 pt=96 mtu=1200 ! "
        "application/x-rtp,media=video,encoding-name=H264,payload=96,clock-rate=90000 ! "
        "rtpjitterbuffer latency=30 do-lost=true ! rtph264depay wait-for-keyframe=true request-keyframe=true ! "
        "h264parse ! video/x-h264,stream-format=byte-stream,alignment=au ! " + decoder +
        " ! fakesink name=out sync=false signal-handoffs=true";
    GError* error = nullptr;
    std::unique_ptr<GstElement, PipelineDelete> pipeline(gst_parse_launch(description.c_str(), &error));
    if (error) {
      const std::string message = error->message;
      g_error_free(error);
      throw std::runtime_error(message);
    }
    if (!pipeline) throw std::runtime_error("probe pipeline unavailable");
    auto* pay = gst_bin_get_by_name(GST_BIN(pipeline.get()), "pay");
    auto* pad = gst_element_get_static_pad(pay, "src");
    gst_pad_add_probe(pad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_UPSTREAM),
                      inject_loss, &observed, nullptr);
    gst_object_unref(pad);
    gst_object_unref(pay);
    auto* enc = gst_bin_get_by_name(GST_BIN(pipeline.get()), "enc");
    pad = gst_element_get_static_pad(enc, "src");
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, observe_encoder, &observed, nullptr);
    gst_object_unref(pad);
    gst_object_unref(enc);
    auto* output = gst_bin_get_by_name(GST_BIN(pipeline.get()), "out");
    g_signal_connect(output, "handoff", G_CALLBACK(observe_decode), &observed);
    gst_object_unref(output);
    if (gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE)
      throw std::runtime_error("probe pipeline could not start");
    auto* bus = gst_element_get_bus(pipeline.get());
    auto* message = gst_bus_timed_pop_filtered(bus, 12 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));
    gst_object_unref(bus);
    const bool eos = message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS;
    if (message && GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
      gchar* debug = nullptr;
      gst_message_parse_error(message, &error, &debug);
      std::cerr << GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) << ": " << error->message << '\n';
      g_error_free(error);
      g_free(debug);
    }
    if (message) gst_message_unref(message);
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    const auto gap_ms = static_cast<double>(observed.max_gap_ns.load()) / GST_MSECOND;
    const bool exercised = eos && observed.dropped > 0 && observed.decoded > 100 && observed.idrs > 2;
    const bool within_budget = exercised && gap_ms < kVideoBudgetMs;
    std::cout << mine_teleop::Json{
        {"runtime", gst_version_string()}, {"decoder", decoder}, {"gop_frames", gop},
        {"feedback_blocked", observed.block_feedback}, {"dropped_rtp_packets", observed.dropped.load()},
        {"force_key_unit_requests", observed.requests.load()}, {"decoded_frames", observed.decoded.load()},
        {"max_decoded_gap_ms", gap_ms}, {"idr_count", observed.idrs.load()},
        {"max_idr_gap_ms", static_cast<double>(observed.max_idr_gap_ns.load()) / GST_MSECOND},
        {"eos", eos}, {"fault_exercised", exercised}, {"within_500ms", within_budget},
        {"scope", "synthetic decoder output; no field presentation or control acceptance"}}.dump() << '\n';
    return exercised && (legacy || within_budget) ? 0 : 1;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
