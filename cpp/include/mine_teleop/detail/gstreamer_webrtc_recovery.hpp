#pragma once

#include <gst/gst.h>

#include <stdexcept>
#include <string>

namespace mine_teleop::detail {

// gst_parse_launch has already requested every fixed video sink pad. Configure
// those existing transceivers before create-offer. Codec preferences come from
// the linked payloader/capsfilter, not a first frame: otherwise one silent
// camera prevents on-negotiation-needed for every video and the DataChannel.
inline guint configure_webrtc_video_transceivers(GstElement* peer) {
  GArray* transceivers = nullptr;
  g_signal_emit_by_name(peer, "get-transceivers", &transceivers);
  if (transceivers == nullptr) throw std::runtime_error("WebRTC transceivers unavailable");
  const auto count = transceivers->len;
  for (guint index = 0; index < count; ++index) {
    auto* transceiver = g_array_index(transceivers, gpointer, index);
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(transceiver), "do-nack") == nullptr) {
      g_array_unref(transceivers);
      throw std::runtime_error("WebRTC video transceiver lacks NACK support");
    }
    auto* pad = gst_element_get_static_pad(peer, ("sink_" + std::to_string(index)).c_str());
    auto* caps = pad ? gst_pad_peer_query_caps(pad, nullptr) : nullptr;
    if (pad) gst_object_unref(pad);
    if (caps == nullptr || gst_caps_is_empty(caps) || !gst_caps_is_fixed(caps)) {
      if (caps) gst_caps_unref(caps);
      g_array_unref(transceivers);
      throw std::runtime_error("WebRTC video lane lacks fixed RTP codec capabilities");
    }
    guint ssrc = 0;
    if (!gst_structure_get_uint(gst_caps_get_structure(caps, 0), "ssrc", &ssrc) ||
        ssrc == 0 || ssrc == G_MAXUINT) {
      gst_caps_unref(caps);
      g_array_unref(transceivers);
      throw std::runtime_error("WebRTC video lane lacks an explicit RTP SSRC before negotiation");
    }
    g_object_set(transceiver, "codec-preferences", caps, nullptr);
    gst_caps_unref(caps);
    g_object_set(transceiver, "do-nack", TRUE, nullptr);
  }
  g_array_unref(transceivers);
  return count;
}

}  // namespace mine_teleop::detail
