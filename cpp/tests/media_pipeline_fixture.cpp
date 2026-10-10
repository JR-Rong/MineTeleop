#include "mine_teleop/video.hpp"
#include <atomic>
#include <chrono>
#include <gst/app/gstappsink.h>
#include <gst/webrtc/webrtc.h>
#include <iostream>
#include <stdexcept>
#include <thread>
using namespace mine_teleop;
static void check(bool ok, const char *what) {
  if (!ok)
    throw std::runtime_error(what);
}
static GstPadProbeReturn count(GstPad *, GstPadProbeInfo *info, gpointer data) {
  if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)
    ++*static_cast<std::atomic<int> *>(data);
  return GST_PAD_PROBE_OK;
}
int main(int argc, char **argv) {
  const bool fish =
      argc > 1 && std::string_view(argv[1]) == "--fish-diagnostic";
  const int width = fish ? 2560 : 1280, height = 1440, level = fish ? 50 : 40;
  gst_init(nullptr, nullptr);
  GstElement *pipe = nullptr;
  GstElement *sink = nullptr;
  GstElement *decoded = nullptr;
  GstElement *gate = nullptr;
  GstElement *level_caps = nullptr;
  GstElement *peer = nullptr;
  try {
    GError *error = nullptr;
    const auto pipeline_text =
        std::string("videotestsrc is-live=true pattern=ball ! "
                    "video/x-raw,format=NV12,width=") +
        std::to_string(width) +
        ",height=1440,framerate=30/1 ! "
        "x264enc name=encoder tune=zerolatency speed-preset=ultrafast "
        "bframes=0 key-int-max=10 bitrate=2800 ! h264parse config-interval=-1 "
        "! video/x-h264,stream-format=byte-stream,alignment=au,level=(string)" +
        h264_level_name(level) +
        " "
        "! valve name=gate drop=true drop-mode=transform-to-gap ! tee "
        "name=admitted admitted. ! queue max-size-buffers=2 leaky=downstream ! "
        "rtph264pay config-interval=-1 pt=96 ! capsfilter name=level_caps "
        "caps=\"application/x-rtp,media=video,encoding-name=H264,payload=96\" "
        "! tee name=rtptee rtptee. ! queue max-size-buffers=2 leaky=downstream "
        "! appsink name=rtp sync=false async=false max-buffers=2 drop=true "
        "rtptee. ! queue max-size-buffers=2 leaky=downstream ! webrtcbin "
        "name=peer bundle-policy=max-bundle admitted. ! queue "
        "max-size-buffers=2 leaky=downstream ! h264parse ! openh264dec ! "
        "video/x-raw ! appsink name=decoded sync=false async=false "
        "max-buffers=2 drop=true";
    pipe = gst_parse_launch(pipeline_text.c_str(), &error);
    if (error) {
      auto text = std::string(error->message);
      g_error_free(error);
      throw std::runtime_error(text);
    }
    check(pipe, "pipeline missing");
    sink = gst_bin_get_by_name(GST_BIN(pipe), "rtp");
    decoded = gst_bin_get_by_name(GST_BIN(pipe), "decoded");
    gate = gst_bin_get_by_name(GST_BIN(pipe), "gate");
    level_caps = gst_bin_get_by_name(GST_BIN(pipe), "level_caps");
    peer = gst_bin_get_by_name(GST_BIN(pipe), "peer");
    auto *encoder = gst_bin_get_by_name(GST_BIN(pipe), "encoder");
    auto *pad = gst_element_get_static_pad(encoder, "src");
    std::atomic<int> encoded{};
    gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, count, &encoded, nullptr);
    gst_object_unref(pad);
    gst_object_unref(encoder);
    check(gst_element_set_state(pipe, GST_STATE_PLAYING) !=
              GST_STATE_CHANGE_FAILURE,
          "PLAYING failed");
    const auto until =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (encoded < 3 && std::chrono::steady_clock::now() < until)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    check(encoded >= 3, "closed valve blocked encoder prewarm");
    auto *sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 100 * GST_MSECOND);
    check(!sample, "RTP emitted before admission");
    g_object_set(gate, "drop", FALSE, nullptr);
    sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
    check(sample, "admitted stream produced no RTP");
    const GstStructure *cap = nullptr;
    std::string profile;
    const auto caps_until =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    do {
      cap = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
      const auto *sprop = gst_structure_get_string(cap, "sprop-parameter-sets");
      profile = h264_profile_level_id(sprop ? sprop : "");
      if (!profile.empty())
        break;
      gst_sample_unref(sample);
      sample =
          gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 500 * GST_MSECOND);
    } while (sample && std::chrono::steady_clock::now() < caps_until);
    check(sample && !profile.empty() &&
              std::stoul(profile, nullptr, 16) % 256 >= unsigned(level),
          "actual RTP caps do not advertise Level 4");
    // Apply the actual SPS profile/level before publishing the offer, as the
    // vehicle does. Gst 1.28's payloader only supplied sprop-parameter-sets.
    auto *wanted = gst_caps_copy(gst_sample_get_caps(sample));
    gst_caps_set_simple(wanted, "profile-level-id", G_TYPE_STRING,
                        profile.c_str(), nullptr);
    g_object_set(level_caps, "caps", wanted, nullptr);
    gst_caps_unref(wanted);
    auto *out = gst_element_get_static_pad(level_caps, "src");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    bool advertised = false;
    while (std::chrono::steady_clock::now() < deadline) {
      auto *actual = gst_pad_get_current_caps(out);
      if (actual) {
        const auto *value = gst_structure_get_string(
            gst_caps_get_structure(actual, 0), "profile-level-id");
        advertised = value && profile == value;
        gst_caps_unref(actual);
      }
      if (advertised)
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    gst_object_unref(out);
    check(advertised, "actual SPS profile did not reach RTP capsfilter");
    GArray *transceivers = nullptr;
    g_signal_emit_by_name(peer, "get-transceivers", &transceivers);
    check(transceivers && transceivers->len == 1, "video transceiver missing");
    auto *preferences = gst_caps_copy(gst_sample_get_caps(sample));
    gst_caps_set_simple(preferences, "profile-level-id", G_TYPE_STRING,
                        profile.c_str(), nullptr);
    g_object_set(g_array_index(transceivers, GstWebRTCRTPTransceiver *, 0),
                 "codec-preferences", preferences, nullptr);
    gst_caps_unref(preferences);
    g_array_unref(transceivers);
    auto *promise = gst_promise_new();
    g_signal_emit_by_name(peer, "create-offer", nullptr, promise);
    check(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED,
          "SDP offer did not complete");
    GstWebRTCSessionDescription *offer = nullptr;
    gst_structure_get(gst_promise_get_reply(promise), "offer",
                      GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr);
    check(offer, "actual webrtcbin offer missing");
    gchar *text = gst_sdp_message_as_text(offer->sdp);
    MediaProfile canvas;
    canvas.width = width;
    canvas.height = height;
    canvas.fps = 30;
    check(h264_answer_supports(text, {canvas}, {level}),
          "actual SDP does not advertise SPS Level 4");
    g_free(text);
    gst_webrtc_session_description_free(offer);
    gst_promise_unref(promise);
    gst_sample_unref(sample);
    sample =
        gst_app_sink_try_pull_sample(GST_APP_SINK(decoded), 3 * GST_SECOND);
    check(sample, "software decoder cannot decode 1280x1440");
    int w = 0, h = 0;
    cap = gst_caps_get_structure(gst_sample_get_caps(sample), 0);
    gst_structure_get_int(cap, "width", &w);
    gst_structure_get_int(cap, "height", &h);
    check(w == width && h == height, "decoded canvas differs");
    gst_sample_unref(sample);
    gst_element_set_state(pipe, GST_STATE_NULL);
    gst_object_unref(sink);
    gst_object_unref(decoded);
    gst_object_unref(gate);
    gst_object_unref(level_caps);
    gst_object_unref(peer);
    gst_object_unref(pipe);
    std::cout << "PASS software fixture: prewarm without RTP, actual "
                 "SPS/RTP/SDP Level "
              << h264_level_name(level) << ", software decoded canvas " << width
              << "x" << height << " (not hardware qualification)\n";
    return 0;
  } catch (const std::exception &e) {
    if (pipe)
      gst_element_set_state(pipe, GST_STATE_NULL);
    if (sink)
      gst_object_unref(sink);
    if (decoded)
      gst_object_unref(decoded);
    if (gate)
      gst_object_unref(gate);
    if (level_caps)
      gst_object_unref(level_caps);
    if (peer)
      gst_object_unref(peer);
    if (pipe)
      gst_object_unref(pipe);
    std::cerr << e.what() << '\n';
    return 1;
  }
}
