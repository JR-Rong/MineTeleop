#include "mine_teleop/recording.hpp"
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
