#pragma once

#include "mine_teleop/detail/rtp_marker.hpp"
#include <gst/gst.h>
#include <array>
#include <chrono>
#include <memory>
#include <string_view>

namespace mine_teleop::detail {

// Only install for explicit isolated mock diagnostics. Lifetime is shared by
// the bin signal and each pad probe, including asynchronous pipeline teardown.
inline void install_ice_output_marker_probe(GstElement* pipeline, const std::shared_ptr<RtpMarkerCounters>& counters) {
  auto* signal_owner = new std::shared_ptr<RtpMarkerCounters>(counters);
  g_signal_connect_data(pipeline, "deep-element-added", G_CALLBACK(+[](GstBin*, GstBin*, GstElement* element, gpointer data) {
    auto* factory = gst_element_get_factory(element);
    if (!factory || std::string_view(gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory))) != "nicesink") return;
    auto* pad = gst_element_get_static_pad(element, "sink");
    if (!pad) return;
    auto* owner = new std::shared_ptr<RtpMarkerCounters>(*static_cast<std::shared_ptr<RtpMarkerCounters>*>(data));
    gst_pad_add_probe(pad, static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
        +[](GstPad*, GstPadProbeInfo* info, gpointer value) -> GstPadProbeReturn {
          auto& counts = **static_cast<std::shared_ptr<RtpMarkerCounters>*>(value);
          const auto record = [&](GstBuffer* buffer) {
            std::array<std::uint8_t, 12> header{};
            if (!buffer || gst_buffer_extract(buffer, 0, header.data(), header.size()) != header.size()) return;
            const auto ssrc = rtp_marker_ssrc(header);
            if (!ssrc) return;
            const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            counts.observe(*ssrc, now);
          };
          if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) record(GST_PAD_PROBE_INFO_BUFFER(info));
          else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
            auto* list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
            if (list) for (guint i = 0; i < gst_buffer_list_length(list); ++i) record(gst_buffer_list_get(list, i));
          }
          return GST_PAD_PROBE_OK;
        }, owner, +[](gpointer value) { delete static_cast<std::shared_ptr<RtpMarkerCounters>*>(value); });
    gst_object_unref(pad);
  }), signal_owner, +[](gpointer value, GClosure*) { delete static_cast<std::shared_ptr<RtpMarkerCounters>*>(value); },
      static_cast<GConnectFlags>(0));
}

}  // namespace mine_teleop::detail
