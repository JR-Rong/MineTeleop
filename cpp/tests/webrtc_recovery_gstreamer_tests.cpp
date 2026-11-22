#include "mine_teleop/detail/gstreamer_webrtc_recovery.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/sdp/sdp.h>
#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
void expect(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}

void verify_offer(bool recovery) {
  GError* error = nullptr;
  std::string description = "webrtcbin name=peer bundle-policy=max-bundle ";
  for (unsigned lane = 0; lane < 8; ++lane) {
    const auto rtp_caps = "application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=" +
        std::to_string(96 + lane) + ",ssrc=(uint)" + std::to_string(1000 + lane) +
        ",packetization-mode=(string)1";
    description += "appsrc name=source" + std::to_string(lane) +
        " is-live=true format=time caps=" + rtp_caps + " ! " + rtp_caps + " ! queue ! peer. ";
  }
  auto* pipeline = gst_parse_launch(description.c_str(), &error);
  const std::string detail = error ? error->message : "";
  if (error) g_error_free(error);
  expect(pipeline && detail.empty(), "eight-lane RTP fixture could not be parsed");
  auto* peer = gst_bin_get_by_name(GST_BIN(pipeline), "peer");
  GstWebRTCSessionDescription* offer = nullptr;
  try {
    if (recovery) expect(mine_teleop::detail::configure_webrtc_video_transceivers(peer) == 8,
                         "recovery did not configure all pre-existing transceivers");
    expect(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE,
           "WebRTC fixture failed to enter READY");
    expect(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
           "WebRTC fixture failed to enter PLAYING");
    for (unsigned lane = 0; lane < 8; ++lane) {
      auto* source = gst_bin_get_by_name(GST_BIN(pipeline), ("source" + std::to_string(lane)).c_str());
      const guint8 packet[16] = {0x80, static_cast<guint8>(96 + lane), 0, 0,
          0, 0, 0, 0, 0, 0, 3, static_cast<guint8>(0xe8 + lane), 0x65, 0x88, 0x84, 0x21};
      auto* buffer = gst_buffer_new_allocate(nullptr, sizeof(packet), nullptr);
      gst_buffer_fill(buffer, 0, packet, sizeof(packet));
      GST_BUFFER_PTS(buffer) = 0;
      GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;
      expect(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK,
             "synthetic RTP admission failed");
      gst_object_unref(source);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    unsigned configured = 0;
    do {
      configured = 0;
      for (unsigned lane = 0; lane < 8; ++lane) {
        auto* pad = gst_element_get_static_pad(peer, ("sink_" + std::to_string(lane)).c_str());
        auto* caps = gst_pad_get_current_caps(pad);
        if (caps) { ++configured; gst_caps_unref(caps); }
        gst_object_unref(pad);
      }
      if (configured == 8) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    } while (std::chrono::steady_clock::now() < deadline);
    expect(configured == 8, "synthetic RTP caps did not reach every transceiver");
    auto* promise = gst_promise_new();
    g_signal_emit_by_name(peer, "create-offer", nullptr, promise);
    expect(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED, "offer promise did not reply");
    const auto* reply = gst_promise_get_reply(promise);
    const bool offered = reply && gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                                                    &offer, nullptr);
    gst_promise_unref(promise);
    expect(offered && offer, "eight-lane SDP offer unavailable");
    expect(gst_sdp_message_medias_len(offer->sdp) == 8, "offer lost a camera lane");
    for (guint lane = 0; lane < 8; ++lane) {
      const auto* media = gst_sdp_message_get_media(offer->sdp, lane);
      const auto payload = std::to_string(96 + lane);
      bool nack = false, rtx = false, apt = false;
      for (guint index = 0; index < gst_sdp_media_attributes_len(media); ++index) {
        const auto* attr = gst_sdp_media_get_attribute(media, index);
        const std::string key = attr->key ? attr->key : "";
        std::string value = attr->value ? attr->value : "";
        if (key == "rtcp-fb" && value == payload + " nack") nack = true;
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
          return static_cast<char>(std::tolower(c));
        });
        if (key == "rtpmap" && value.find("rtx/90000") != std::string::npos) rtx = true;
        if (key == "fmtp" && value.find("apt=" + payload) != std::string::npos) apt = true;
      }
      expect(nack == recovery && rtx == recovery && apt == recovery,
             "SDP NACK/RTX negotiation does not match the configured recovery policy");
    }
    gst_webrtc_session_description_free(offer);
    offer = nullptr;
  } catch (...) {
    if (offer) gst_webrtc_session_description_free(offer);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(peer);
    gst_object_unref(pipeline);
    throw;
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
  gst_object_unref(peer);
  gst_object_unref(pipeline);
}

void verify_start_without_camera_frames(bool configured, const std::string& codec,
                                       bool control_before_caps = true) {
  GError* error = nullptr;
  std::string description = "webrtcbin name=peer bundle-policy=max-bundle ";
  for (unsigned lane = 0; lane < 8; ++lane) {
    description += "appsrc name=source" + std::to_string(lane) +
        " is-live=true format=time caps=video/x-" + (codec == "H264" ? "h264" : "h265") +
        ",stream-format=byte-stream,alignment=au ! " + (codec == "H264" ? "rtph264pay" : "rtph265pay") +
        " pt=" + std::to_string(96 + lane) + " ssrc=" + std::to_string(1000 + lane) +
        " ! application/x-rtp,media=video,encoding-name=" + codec +
        ",payload=" + std::to_string(96 + lane) + ",ssrc=(uint)" + std::to_string(1000 + lane) +
        (codec == "H265" ? ",profile-id=(string)1,tier-flag=(string)0,tx-mode=(string)SRST" : ",packetization-mode=(string)1") +
        " ! peer. ";
  }
  auto* pipeline = gst_parse_launch(description.c_str(), &error);
  const std::string detail = error ? error->message : "";
  if (error) g_error_free(error);
  expect(pipeline && detail.empty(), "silent-camera fixture could not be parsed");
  auto* peer = gst_bin_get_by_name(GST_BIN(pipeline), "peer");
  struct FirstOffer {
    std::atomic<unsigned> negotiations{0};
    std::atomic<GstPromise*> promise{nullptr};
  } first;
  GstWebRTCDataChannel* channel = nullptr;
  GstWebRTCSessionDescription* offer = nullptr;
  try {
    const auto configure = [&] {
      expect(mine_teleop::detail::configure_webrtc_video_transceivers(peer) == 8,
             "silent-camera capabilities did not cover all lanes");
    };
    const auto wait_for_first_offer = [&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(800);
      while (std::chrono::steady_clock::now() < deadline && first.promise.load() == nullptr)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };
    const auto request_first_offer = +[](GstElement* source, gpointer data) {
      auto& capture = *static_cast<FirstOffer*>(data);
      if (++capture.negotiations != 1) return;
      auto* promise = gst_promise_new();
      g_signal_emit_by_name(source, "create-offer", nullptr, promise);
      capture.promise.store(promise);
    };
    g_signal_connect(peer, "on-negotiation-needed", G_CALLBACK(request_first_offer), &first);
    if (configured && !control_before_caps) configure();
    expect(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE,
           "silent-camera fixture failed to enter READY");
    // Exercise the old READY-state race deterministically. Verify the first
    // actual signal-driven offer, not a later manually requested offer.
    if (configured && !control_before_caps) {
      wait_for_first_offer();
      expect(first.promise.load() != nullptr, "old READY-state race did not trigger");
      expect(gst_promise_wait(first.promise.load()) == GST_PROMISE_RESULT_REPLIED,
             "old READY-state offer did not reply");
    }
    g_signal_emit_by_name(peer, "create-data-channel", "control", nullptr, &channel);
    expect(channel != nullptr, "control channel creation failed");
    if (configured && control_before_caps) configure();
    expect(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
           "silent-camera fixture failed to enter PLAYING");
    if (configured && control_before_caps) request_first_offer(peer, &first);
    wait_for_first_offer();
    expect((first.promise.load() != nullptr) == configured,
           "negotiation still depends on first camera frames");
    for (unsigned lane = 0; lane < 8; ++lane) {
      auto* pad = gst_element_get_static_pad(peer, ("sink_" + std::to_string(lane)).c_str());
      auto* caps = gst_pad_get_current_caps(pad);
      const bool received = caps != nullptr;
      if (caps) gst_caps_unref(caps);
      gst_object_unref(pad);
      expect(!received, "fixture unexpectedly injected media into a silent lane");
    }
    if (configured) {
      auto* promise = first.promise.exchange(nullptr);
      expect(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED, "silent-camera offer did not reply");
      const auto* reply = gst_promise_get_reply(promise);
      const bool offered = reply && gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION,
                                                      &offer, nullptr);
      gst_promise_unref(promise);
      expect(offered && offer, "silent-camera SDP offer unavailable");
      expect(gst_sdp_message_medias_len(offer->sdp) == (control_before_caps ? 9U : 8U),
             "first offer does not reflect the tested control-channel startup order");
      for (guint lane = 0; lane < 8; ++lane) {
        const auto* media = gst_sdp_message_get_media(offer->sdp, lane);
        expect(std::string(gst_sdp_media_get_media(media)) == "video", "camera media missing");
        const auto mapping = std::to_string(96 + lane) + " " + codec + "/90000";
        bool found = false, packetization = false, source_identity = false;
        for (guint index = 0; index < gst_sdp_media_attributes_len(media); ++index) {
          const auto* attr = gst_sdp_media_get_attribute(media, index);
          if (std::string(attr->key) == "rtpmap" && attr->value && mapping == attr->value) found = true;
          if (std::string(attr->key) == "ssrc" && attr->value &&
              std::string(attr->value).starts_with(std::to_string(1000 + lane) + " msid:"))
            source_identity = true;
          if (std::string(attr->key) == "fmtp" && attr->value &&
              std::string(attr->value).find(codec == "H264" ? "packetization-mode=1" : "tx-mode=SRST") != std::string::npos)
            packetization = true;
        }
        expect(found, "silent-camera offer changed codec or payload identity");
        expect(packetization, "silent-camera offer lost the payloader packetization mode");
        expect(source_identity, "first offer SSRC differs from the configured RTP source");
      }
      if (control_before_caps)
        expect(std::string(gst_sdp_media_get_media(gst_sdp_message_get_media(offer->sdp, 8))) == "application",
               "control DataChannel missing from first silent-camera offer");
    }
  } catch (...) {
    if (offer) gst_webrtc_session_description_free(offer);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    if (auto* pending = first.promise.exchange(nullptr)) { gst_promise_interrupt(pending); gst_promise_unref(pending); }
    if (channel) g_object_unref(channel);
    gst_object_unref(peer);
    gst_object_unref(pipeline);
    throw;
  }
  if (offer) gst_webrtc_session_description_free(offer);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  if (auto* pending = first.promise.exchange(nullptr)) { gst_promise_interrupt(pending); gst_promise_unref(pending); }
  if (channel) g_object_unref(channel);
  gst_object_unref(peer);
  gst_object_unref(pipeline);
}
void verify_control_only_offer() {
  GError* error = nullptr;
  auto* transport = gst_parse_launch("webrtcbin name=webrtc bundle-policy=max-bundle ice-transport-policy=all", &error);
  expect(!error && transport, "control-only webrtcbin unavailable");
  auto* pipeline = gst_pipeline_new(nullptr);
  gst_bin_add(GST_BIN(pipeline), transport);
  auto* peer = gst_bin_get_by_name(GST_BIN(pipeline), "webrtc");
  expect(peer != nullptr, "control-only pipeline lost transport");
  expect(gst_element_set_state(pipeline, GST_STATE_READY) != GST_STATE_CHANGE_FAILURE, "control-only READY failed");
  GstWebRTCDataChannel* channel = nullptr;
  g_signal_emit_by_name(peer, "create-data-channel", "control", nullptr, &channel);
  expect(channel != nullptr, "control-only SCTP unavailable");
  expect(mine_teleop::detail::configure_webrtc_video_transceivers(peer) == 0, "control-only mode created video");
  expect(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "control-only PLAYING failed");
  auto* promise = gst_promise_new();
  g_signal_emit_by_name(peer, "create-offer", nullptr, promise);
  expect(gst_promise_wait(promise) == GST_PROMISE_RESULT_REPLIED, "control-only offer did not reply");
  GstWebRTCSessionDescription* offer = nullptr;
  const auto* reply = gst_promise_get_reply(promise);
  expect(reply && gst_structure_get(reply, "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &offer, nullptr) && offer,
      "control-only SDP missing");
  expect(gst_sdp_message_medias_len(offer->sdp) == 1 &&
      std::string(gst_sdp_media_get_media(gst_sdp_message_get_media(offer->sdp, 0))) == "application",
      "control-only SDP must contain SCTP only");
  gst_webrtc_session_description_free(offer); gst_promise_unref(promise);
  gst_element_set_state(pipeline, GST_STATE_NULL);
  g_object_unref(channel); gst_object_unref(peer); gst_object_unref(pipeline);
}

void verify_missing_ssrc_rejected() {
  GError* error = nullptr;
  auto* pipeline = gst_parse_launch(
      "webrtcbin name=peer appsrc is-live=true format=time "
      "caps=application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96 "
      "! application/x-rtp,media=video,encoding-name=H264,clock-rate=90000,payload=96 ! peer.", &error);
  expect(!error && pipeline, "missing-SSRC fixture unavailable");
  auto* peer = gst_bin_get_by_name(GST_BIN(pipeline), "peer");
  bool rejected = false;
  try {
    mine_teleop::detail::configure_webrtc_video_transceivers(peer);
  } catch (const std::runtime_error& failure) {
    rejected = std::string(failure.what()).find("explicit RTP SSRC") != std::string::npos;
  }
  gst_object_unref(peer);
  gst_object_unref(pipeline);
  expect(rejected, "negotiation accepted a lane with no RTP source identity");
}

}  // namespace

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  try {
    verify_missing_ssrc_rejected();
    verify_control_only_offer();
    verify_offer(false);
    verify_offer(true);
    for (const std::string codec : {"H264", "H265"}) {
      verify_start_without_camera_frames(false, codec);
      verify_start_without_camera_frames(true, codec, false);
      verify_start_without_camera_frames(true, codec);
    }
    std::cout << "Eight-lane SDP NACK/RTX verified. H264/H265 negotiate eight silent lanes plus control without injecting media. No field-video acceptance.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
