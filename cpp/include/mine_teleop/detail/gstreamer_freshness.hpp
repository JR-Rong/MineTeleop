#pragma once

#include "mine_teleop/detail/media_freshness.hpp"
#include "mine_teleop/detail/latency_histogram.hpp"

#include <gst/gst.h>
#include <gst/video/video-event.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>

namespace mine_teleop::detail {

inline std::int64_t media_steady_now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::optional<std::uint64_t> encoded_running_time_age_ms(
    const GstSegment& segment, GstClockTime pts, std::int64_t elapsed_ms) {
  if (segment.format != GST_FORMAT_TIME || !GST_CLOCK_TIME_IS_VALID(pts) || elapsed_ms < 0) {
    return std::nullopt;
  }
  const auto running_time = gst_segment_to_running_time(&segment, GST_FORMAT_TIME, pts);
  if (!GST_CLOCK_TIME_IS_VALID(running_time) ||
      running_time / GST_MSECOND > static_cast<std::uint64_t>(elapsed_ms)) return std::nullopt;
  return static_cast<std::uint64_t>(elapsed_ms) - running_time / GST_MSECOND;
}

// One instance per send branch, owned until its pipeline has reached NULL.
// Segment and gate state are accessed only by the serialized source-pad probe.
// Status counters are safe for diagnostics/control readers on other threads.
struct EncodedSendFreshness {
  EncodedSendFreshness(AccessUnitCodec next_codec, std::uint64_t max_age_ms)
      : codec(next_codec), gate(max_age_ms), max_age_ms(max_age_ms) {
    gst_segment_init(&segment, GST_FORMAT_UNDEFINED);
  }
  AccessUnitCodec codec;
  EncodedFreshnessGate gate;
  MediaSampleAnomalies samples;
  bool invalid_fault_event_latched{false}; // Serialized probe state, never read by control threads.
  GstSegment segment{};
  std::atomic<std::uint64_t> max_age_ms;
  std::int64_t origin_steady_ms{0};
  std::function<std::int64_t()> now_ms{media_steady_now_ms};
  std::atomic<bool> blocked{false};
  std::atomic<bool> invalid_sample_blocked{false};
  std::atomic<unsigned> consecutive_anomalies{0};
  std::atomic<std::uint64_t> bad_sample_revision{0};
  std::atomic<std::uint64_t> brake_sample_revision{0};
  std::atomic<GstClockTime> last_sample_id_ns{GST_CLOCK_TIME_NONE};
  std::atomic<std::int64_t> last_fresh_frame_ms{0};
  std::atomic<std::uint64_t> fresh_frames{0};
  std::atomic<std::uint64_t> dropped_stale{0};
  std::atomic<std::uint64_t> dropped_invalid{0};
  std::atomic<std::uint64_t> dropped_waiting_idr{0};
  std::atomic<std::uint64_t> key_unit_requests{0};
  std::atomic<std::uint64_t> key_unit_request_failures{0};
  // Streaming-thread state. One retry per reference-gap episode, never an
  // unbounded IDR storm. No buffer means the existing video watchdog applies.
  std::optional<std::int64_t> recovery_started_ms;
  unsigned recovery_requests{0};
  bool recovery_timeout_recorded{false};
  std::atomic<std::uint64_t> idr_recoveries{0}, idr_recovery_timeouts{0};
  std::atomic<std::uint64_t> idr_recovery_last_ms{0}, idr_recovery_max_ms{0};
  LatencyHistogram idr_recovery_latency;
  std::atomic<std::uint64_t> last_forwarded_age_ms{0};
};

inline GstPadProbeReturn encoded_send_freshness_probe(
    GstPad* pad, GstPadProbeInfo* info, gpointer user_data) {
  auto& state = *static_cast<EncodedSendFreshness*>(user_data);
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) != 0) {
    auto* event = GST_PAD_PROBE_INFO_EVENT(info);
    if (GST_EVENT_TYPE(event) == GST_EVENT_SEGMENT) {
      const GstSegment* segment = nullptr;
      gst_event_parse_segment(event, &segment);
      gst_segment_copy_into(segment, &state.segment);
    } else if (GST_EVENT_TYPE(event) == GST_EVENT_FLUSH_STOP) {
      gst_segment_init(&state.segment, GST_FORMAT_UNDEFINED);
      state.gate.reset();
      state.blocked = true;
      state.samples.reset_stream();
      state.last_sample_id_ns = GST_CLOCK_TIME_NONE;
      state.recovery_started_ms.reset();
      state.recovery_requests = 0;
      state.recovery_timeout_recorded = false;
    }
    return GST_PAD_PROBE_OK;
  }
  if ((GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) == 0) return GST_PAD_PROBE_OK;
  auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  const auto now_ms = state.now_ms();
  const auto age = buffer == nullptr ? std::nullopt : encoded_running_time_age_ms(
      state.segment, GST_BUFFER_PTS(buffer), now_ms - state.origin_steady_ms);
  AccessUnitInfo access_unit;
  GstMapInfo mapped{};
  if (buffer != nullptr && gst_buffer_map(buffer, &mapped, GST_MAP_READ)) {
    access_unit = inspect_access_unit(state.codec, {mapped.data, mapped.size});
    gst_buffer_unmap(buffer, &mapped);
  }
  if (buffer && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_CORRUPTED)) access_unit.valid = false;
  const auto sample_id = buffer != nullptr && state.segment.format == GST_FORMAT_TIME
      ? gst_segment_to_running_time(&state.segment, GST_FORMAT_TIME, GST_BUFFER_PTS(buffer))
      : GST_CLOCK_TIME_NONE;
  const auto previous_sample_id = state.samples.last_id();
  // A repeated/old IDR must not unlock the reference chain and allow the next
  // dependent picture to clear readiness. PTS is normalized across segments.
  if (state.gate.waiting_idr() && (!GST_CLOCK_TIME_IS_VALID(sample_id) ||
      (previous_sample_id && sample_id <= *previous_sample_id))) access_unit.idr = false;
  state.gate.set_max_age_ms(state.max_age_ms.load());
  const auto decision = state.gate.observe(access_unit, age);
  const auto previous_brake_revision = state.samples.brake_sample_revision();
  const bool new_sample = access_unit.picture && state.samples.observe(
      GST_CLOCK_TIME_IS_VALID(sample_id) ? std::optional<std::uint64_t>(sample_id) : std::nullopt,
      decision.forward);
  if (new_sample) {
    state.last_sample_id_ns = sample_id;
    state.consecutive_anomalies = state.samples.consecutive();
    if (!decision.forward) ++state.bad_sample_revision;
    if (state.samples.brake_sample_revision() != previous_brake_revision) ++state.brake_sample_revision;
  }
  if (!decision.forward) {
    state.blocked = true;
    switch (decision.reason) {
      case FreshnessDropReason::Stale: ++state.dropped_stale; break;
      case FreshnessDropReason::WaitingIdr: ++state.dropped_waiting_idr; break;
      default:
        ++state.dropped_invalid;
        // Unknown time/malformed data is a fail-closed barrier, rather than a
        // tolerable network sample. Latch one event for this invalid episode;
        // repeated unidentifiable buffers or reader polls cannot add events.
        state.invalid_sample_blocked = true;
        const bool known_old_sample = GST_CLOCK_TIME_IS_VALID(sample_id) && previous_sample_id &&
            sample_id <= *previous_sample_id;
        if (!known_old_sample && !state.invalid_fault_event_latched) {
          state.invalid_fault_event_latched = true;
          if (!new_sample) ++state.bad_sample_revision;
          if (state.samples.brake_sample_revision() == previous_brake_revision) ++state.brake_sample_revision;
        }
        break;
    }
    if (decision.request_key_unit && !state.recovery_started_ms) {
      state.recovery_started_ms = now_ms;
      state.recovery_requests = 0;
      state.recovery_timeout_recorded = false;
    }
    const auto elapsed = state.recovery_started_ms && now_ms >= *state.recovery_started_ms
        ? static_cast<std::uint64_t>(now_ms - *state.recovery_started_ms) : 0;
    const auto budget = std::max<std::uint64_t>(1, state.max_age_ms.load());
    if (state.recovery_requests == 2 && elapsed / budget >= 2 && !state.recovery_timeout_recorded) {
      ++state.idr_recovery_timeouts;
      state.recovery_timeout_recorded = true;
    }
    if ((decision.request_key_unit && state.recovery_requests == 0) ||
        (state.recovery_requests == 1 && elapsed >= budget)) {
      ++state.recovery_requests;
      const auto request_count = ++state.key_unit_requests;
      if (!gst_pad_send_event(pad, gst_video_event_new_upstream_force_key_unit(
              GST_CLOCK_TIME_NONE, TRUE, static_cast<guint>(request_count)))) ++state.key_unit_request_failures;
    }
    return GST_PAD_PROBE_DROP;
  }
  if (decision.picture && new_sample) {
    if (state.recovery_started_ms) {
      const auto elapsed = static_cast<std::uint64_t>(std::max<std::int64_t>(0, now_ms - *state.recovery_started_ms));
      ++state.idr_recoveries;
      state.idr_recovery_last_ms = elapsed;
      state.idr_recovery_latency.add(elapsed);
      state.idr_recovery_max_ms = std::max(state.idr_recovery_max_ms.load(), elapsed);
      state.recovery_started_ms.reset();
      state.recovery_requests = 0;
      state.recovery_timeout_recorded = false;
    }
    state.invalid_fault_event_latched = false;
    state.invalid_sample_blocked = false;
    state.last_forwarded_age_ms = *age;
    // Preserve capture age when an AU arrives late, rather than refreshing
    // readiness from the encoder/queue callback's arrival time.
    state.last_fresh_frame_ms = now_ms - static_cast<std::int64_t>(*age);
    ++state.fresh_frames;
    state.blocked = false;
  }
  return GST_PAD_PROBE_OK;
}

inline gulong attach_encoded_send_freshness(GstPad* pad, EncodedSendFreshness& state) {
  if (pad == nullptr) throw std::invalid_argument("encoded send source pad is missing");
  return gst_pad_add_probe(pad, static_cast<GstPadProbeType>(
      GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
      encoded_send_freshness_probe, &state, nullptr);
}

}  // namespace mine_teleop::detail
