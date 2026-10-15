#include "mine_teleop/recording.hpp"
#include "mine_teleop/detail/recording_pipeline.hpp"
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/gst.h>
#include <chrono>

namespace mine_teleop {
void IsolatedRecordingSession::admit(std::size_t index, GstBuffer* buffer, const GstSegment& segment) noexcept {
  const auto count = buffer == nullptr ? 0 : gst_buffer_get_size(buffer);
  if (!can_admit(index, count)) { reject_admission(index, true); return; }
  if (segment.format != GST_FORMAT_TIME) { reject_admission(index, false); return; }
  const auto begin = std::chrono::steady_clock::now();
  GstMapInfo mapped{};
  if (!gst_buffer_map(buffer, &mapped, GST_MAP_READ)) { reject_admission(index, false); return; }
  const auto mapping_us = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count());
  detail::RecordingWireHeader header{};
  header.pts = GST_BUFFER_PTS(buffer); header.dts = GST_BUFFER_DTS(buffer); header.duration = GST_BUFFER_DURATION(buffer);
  header.buffer_flags = GST_BUFFER_FLAGS(buffer); header.segment_flags = segment.flags;
  header.rate = segment.rate; header.applied_rate = segment.applied_rate;
  header.start = segment.start; header.stop = segment.stop; header.time = segment.time; header.base = segment.base;
  header.offset = segment.offset; header.position = segment.position; header.segment_duration = segment.duration;
  admit_bytes(index, header, {mapped.data, mapped.size});
  gst_buffer_unmap(buffer, &mapped);
  note_mapping_duration(index, mapping_us);
}
} // namespace mine_teleop

namespace mine_teleop::detail {
RecordingTap::RecordingTap(GstElement* sink, IsolatedRecordingSession* recording, std::size_t lane)
    : sink_(sink), recording_(recording), lane_(lane) {
  if (!GST_IS_APP_SINK(sink_)) throw std::invalid_argument("recording tap must be an appsink");
  gst_object_ref(sink_);
  g_signal_connect(sink_, "new-sample", G_CALLBACK(+[](GstAppSink* sink, gpointer data) -> GstFlowReturn {
    auto& tap = *static_cast<RecordingTap*>(data);
    auto* sample = gst_app_sink_pull_sample(sink);
    if (sample != nullptr) {
      if (tap.recording_ != nullptr) {
        const auto* segment = gst_sample_get_segment(sample);
        if (segment) tap.recording_->admit(tap.lane_, gst_sample_get_buffer(sample), *segment);
        else tap.recording_->reject_admission(tap.lane_, false);
      }
      gst_sample_unref(sample);
    }
    // Recording failure must never back-pressure/error the live encoded tee.
    return GST_FLOW_OK;
  }), this);
  g_signal_connect(sink_, "eos", G_CALLBACK(+[](GstAppSink*, gpointer data) {
    auto& tap = *static_cast<RecordingTap*>(data);
    if (tap.recording_ != nullptr) tap.recording_->end_lane(tap.lane_);
    tap.eos_.store(true, std::memory_order_release);
  }), this);
}
RecordingTap::~RecordingTap() {
  g_signal_handlers_disconnect_by_data(sink_, this);
  gst_object_unref(sink_);
}

void stop_recording_pipeline(GstElement* pipeline, std::span<GstElement* const> sources,
                             std::span<RecordingTap* const> taps, IsolatedRecordingSession* recording) {
  static_cast<void>(taps);
  for (auto* source : sources) if (source != nullptr) gst_app_src_end_of_stream(GST_APP_SRC(source));
  if (recording) recording->request_stop();
  GstBus* bus = gst_element_get_bus(pipeline);
  if (bus != nullptr) {
    GstMessage* message = gst_bus_timed_pop_filtered(
        bus, 3 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    if (message != nullptr) gst_message_unref(message);
    gst_object_unref(bus);
  }
  gst_element_set_state(pipeline, GST_STATE_NULL);
}
}  // namespace mine_teleop::detail
