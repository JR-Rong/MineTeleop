#include "mine_teleop/detail/gstreamer_freshness.hpp"
#include "mine_teleop/media.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using namespace mine_teleop::detail;
using Bytes = std::vector<std::uint8_t>;
void expect(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}
class Pipeline {
 public:
  explicit Pipeline(const std::string& description) {
    GError* error = nullptr;
    value_ = gst_parse_launch(description.c_str(), &error);
    const std::string detail = error == nullptr ? "" : error->message;
    if (error != nullptr) g_error_free(error);
    if (value_ == nullptr || !detail.empty()) {
      if (value_ != nullptr) gst_object_unref(value_);
      throw std::runtime_error("offline pipeline parse failed: " + detail);
    }
  }
  ~Pipeline() {
    gst_element_set_state(value_, GST_STATE_NULL);
    gst_object_unref(value_);
  }
  GstElement* get() const { return value_; }
  void play() { expect(gst_element_set_state(value_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "offline pipeline did not play"); }
 private:
  GstElement* value_{};
};
struct StopStreaming {
  GstElement* pipeline;
  ~StopStreaming() { gst_element_set_state(pipeline, GST_STATE_NULL); }
};
struct ObjectUnref { void operator()(GstElement* value) const { if (value) gst_object_unref(value); } };
using Element = std::unique_ptr<GstElement, ObjectUnref>;
Element element(Pipeline& pipeline, const char* name) {
  Element value(gst_bin_get_by_name(GST_BIN(pipeline.get()), name));
  expect(value != nullptr, "offline pipeline element missing");
  return value;
}
Bytes access_unit(AccessUnitCodec codec, std::initializer_list<int> types) {
  Bytes bytes;
  for (const auto type : types) {
    bytes.insert(bytes.end(), {0, 0, 0, 1});
    if (codec == AccessUnitCodec::H264) bytes.push_back(static_cast<std::uint8_t>(type | 0x60));
    else { bytes.push_back(static_cast<std::uint8_t>(type << 1)); bytes.push_back(1); }
    bytes.push_back(0x80);
  }
  return bytes;
}
void push(GstElement* source, const Bytes& bytes, GstClockTime pts) {
  auto* buffer = gst_buffer_new_allocate(nullptr, bytes.size(), nullptr);
  gst_buffer_fill(buffer, 0, bytes.data(), bytes.size());
  GST_BUFFER_PTS(buffer) = pts;
  GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
  GST_BUFFER_DURATION(buffer) = 50 * GST_MSECOND;
  expect(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK, "offline appsrc rejected test buffer");
}
template <typename Predicate> void eventually(Predicate predicate, std::string_view message) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!predicate() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  expect(predicate(), message);
}
GstClockTime pull_pts(GstElement* sink) {
  auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 3 * GST_SECOND);
  expect(sample != nullptr, "offline appsink did not receive expected buffer");
  const auto pts = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
  gst_sample_unref(sample);
  return pts;
}

// Uses the same production source-pad probe and bounded non-leaky send queue.
// Buffers are synthetic AUs here to control exact stale/duplicate sequences;
// a separate test below verifies actual codec output can be decoded after gaps.
void test_send_queue_backpressure_and_sample_count(AccessUnitCodec codec) {
  const std::string name = codec == AccessUnitCodec::H264 ? "h264" : "h265";
  Pipeline pipeline("appsrc name=input is-live=true format=time caps=video/x-" + name +
      ",stream-format=byte-stream,alignment=au ! " + mine_teleop::build_encoded_frame_queue("send", 2) +
      "! appsink name=output max-buffers=1 drop=false sync=false async=false");
  auto source = element(pipeline, "input"), sink = element(pipeline, "output"), queue = element(pipeline, "send");
  std::atomic<std::int64_t> now{1000};
  EncodedSendFreshness state(codec, 500);
  state.origin_steady_ms = 1000;
  state.now_ms = [&] { return now.load(); };
  auto* pad = gst_element_get_static_pad(queue.get(), "src");
  attach_encoded_send_freshness(pad, state);
  gst_object_unref(pad);
  std::atomic<unsigned> requests{0};
  auto* source_pad = gst_element_get_static_pad(source.get(), "src");
  gst_pad_add_probe(source_pad, GST_PAD_PROBE_TYPE_EVENT_UPSTREAM,
      +[](GstPad*, GstPadProbeInfo* info, gpointer data) -> GstPadProbeReturn {
        auto* event = GST_PAD_PROBE_INFO_EVENT(info);
        const auto* structure = gst_event_get_structure(event);
        if (structure != nullptr && gst_structure_has_name(structure, "GstForceKeyUnit")) ++*static_cast<std::atomic<unsigned>*>(data);
        return GST_PAD_PROBE_OK;
      }, &requests, nullptr);
  gst_object_unref(source_pad);
  std::atomic<bool> backpressure{false};
  g_signal_connect(queue.get(), "overrun", G_CALLBACK(+[](GstElement*, gpointer data) {
    *static_cast<std::atomic<bool>*>(data) = true;
  }), &backpressure);
  StopStreaming stop_before_callback_data{pipeline.get()};
  pipeline.play();
  const auto idr = codec == AccessUnitCodec::H264 ? access_unit(codec, {7, 8, 5}) : access_unit(codec, {32, 33, 34, 19});
  const auto delta = access_unit(codec, {1});
  push(source.get(), idr, 0);
  expect(pull_pts(sink.get()) == 0 && state.fresh_frames == 1, "initial fresh IDR missing");
  now = 1400;
  for (int i = 1; i <= 8; ++i) push(source.get(), delta, static_cast<GstClockTime>(i * 50) * GST_MSECOND);
  eventually([&] { return backpressure.load(); }, "fixture did not establish actual downstream backpressure");
  now = 2000;
  // At most two already-admitted samples can remain in/appsink. Drain them;
  // queued AUs inspected after the stall must be discarded at their true age.
  while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink.get()), 100 * GST_MSECOND)) gst_sample_unref(sample);
  eventually([&] { return state.consecutive_anomalies == 3; }, "three new stale AUs did not reach per-video threshold");
  expect(state.dropped_stale > 0 && requests == 1 && state.key_unit_requests == 1, "stale reference gap did not request exactly one actual upstream key unit");
  const auto old_sample_id = state.last_sample_id_ns.load();
  now = 2020;
  push(source.get(), idr, 400 * GST_MSECOND); // repeated old sample remains stale
  eventually([&] { return state.dropped_stale >= 2; }, "duplicate AU fixture was not evaluated");
  expect(state.consecutive_anomalies == 3 && state.last_sample_id_ns == old_sample_id, "duplicate/older AU changed anomaly counter or sample identity");
  push(source.get(), delta, 1000 * GST_MSECOND);
  eventually([&] { return state.dropped_waiting_idr > 0; }, "fresh dependent AU escaped IDR fence");
  expect(state.consecutive_anomalies == 3, "unusable fresh dependent AU cleared video fault");
  now = 2050;
  push(source.get(), idr, 1040 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 1040 * GST_MSECOND, "fresh complete IDR failed to resume output");
  expect(state.consecutive_anomalies == 0 && !state.blocked && state.last_fresh_frame_ms == 2040,
      "healthy new IDR did not clear its video counter with preserved capture time");
  const auto fresh_count = state.fresh_frames.load();
  push(source.get(), idr, 1040 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 1040 * GST_MSECOND && state.fresh_frames == fresh_count,
      "duplicate healthy AU counted as another recovery sample");
  now = 2100;
  push(source.get(), delta, GST_CLOCK_TIME_NONE);
  eventually([&] { return state.invalid_sample_blocked.load(); }, "unknown timestamp did not fail closed separately");
  push(source.get(), idr, 1090 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 1090 * GST_MSECOND && !state.invalid_sample_blocked,
      "fresh complete IDR did not recover invalid timestamp barrier");
  // Stop while callback data still outlives every streaming task.
  gst_element_set_state(pipeline.get(), GST_STATE_NULL);
}

void test_fault_history_between_control_reads(AccessUnitCodec codec) {
  const std::string name = codec == AccessUnitCodec::H264 ? "h264" : "h265";
  Pipeline pipeline("appsrc name=input is-live=true format=time caps=video/x-" + name +
      ",stream-format=byte-stream,alignment=au ! " + mine_teleop::build_encoded_frame_queue("send", 2) +
      "! appsink name=output max-buffers=1 drop=false sync=false async=false");
  auto source = element(pipeline, "input"), sink = element(pipeline, "output"), queue = element(pipeline, "send");
  std::atomic<std::int64_t> now{1000};
  EncodedSendFreshness state(codec, 500);
  state.origin_steady_ms = 1000;
  state.now_ms = [&] { return now.load(); };
  auto* pad = gst_element_get_static_pad(queue.get(), "src");
  attach_encoded_send_freshness(pad, state);
  gst_object_unref(pad);
  StopStreaming stop_before_callback_data{pipeline.get()};
  pipeline.play();
  const auto idr = codec == AccessUnitCodec::H264 ? access_unit(codec, {7, 8, 5}) : access_unit(codec, {32, 33, 34, 19});
  const auto delta = access_unit(codec, {1});
  push(source.get(), idr, 0);
  expect(pull_pts(sink.get()) == 0, "initial history-test IDR missing");
  const auto before_bad = state.bad_sample_revision.load(), before_brake = state.brake_sample_revision.load();
  // The control reader sees only the recovered IDR. The streaming probe must
  // preserve the preceding real stale buffer instead of publishing only good.
  now = 2000;
  push(source.get(), delta, 10 * GST_MSECOND);
  push(source.get(), idr, 990 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 990 * GST_MSECOND, "short stale/good history did not recover");
  expect(state.consecutive_anomalies == 0 && !state.blocked &&
      state.bad_sample_revision == before_bad + 1 && state.brake_sample_revision == before_brake,
      "latest healthy IDR erased a tolerable stale event between control reads");
  const auto after_short_bad = state.bad_sample_revision.load();
  push(source.get(), delta, 10 * GST_MSECOND); // old, not another sample
  push(source.get(), idr, 1000 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 1000 * GST_MSECOND && state.bad_sample_revision == after_short_bad &&
      state.brake_sample_revision == before_brake, "old stale buffer changed retained revision");
  now = 3000;
  for (const auto pts : {1050U, 1100U, 1150U, 1200U}) push(source.get(), delta, pts * GST_MSECOND);
  push(source.get(), idr, 1990 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 1990 * GST_MSECOND && state.consecutive_anomalies == 0 && !state.blocked &&
      state.bad_sample_revision == after_short_bad + 4 && state.brake_sample_revision == before_brake + 1,
      "third crossing followed by normal IDR was lost or fourth anomaly repeatedly braked");
  const auto before_invalid_bad = state.bad_sample_revision.load(), before_invalid_brake = state.brake_sample_revision.load();
  now = 3100;
  for (int repeat = 0; repeat < 3; ++repeat) push(source.get(), delta, GST_CLOCK_TIME_NONE);
  push(source.get(), idr, 2090 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 2090 * GST_MSECOND && !state.invalid_sample_blocked &&
      state.bad_sample_revision == before_invalid_bad + 1 && state.brake_sample_revision == before_invalid_brake + 1,
      "invalid-time barrier followed by normal IDR was lost or repeated unidentifiable buffers repeatedly braked");
  // Reading an already healthy status has no side effect on either category.
  const auto healthy_bad = state.bad_sample_revision.load(), healthy_brake = state.brake_sample_revision.load();
  for (int repeat = 0; repeat < 100; ++repeat) {
    expect(!state.invalid_sample_blocked && state.consecutive_anomalies == 0 &&
        state.bad_sample_revision == healthy_bad && state.brake_sample_revision == healthy_brake,
        "repeated control reads altered retained sample revisions");
  }
  push(source.get(), delta, GST_CLOCK_TIME_NONE);
  push(source.get(), idr, 2100 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 2100 * GST_MSECOND && state.bad_sample_revision == healthy_bad + 1 &&
      state.brake_sample_revision == healthy_brake + 1,
      "new invalid-time episode after valid recovery failed to preserve another brake event");
  const auto before_old_invalid_bad = state.bad_sample_revision.load();
  const auto before_old_invalid_brake = state.brake_sample_revision.load();
  now = 3200;
  push(source.get(), Bytes{0xde}, 2090 * GST_MSECOND); // Known older malformed AU must not become a new event.
  push(source.get(), delta, GST_CLOCK_TIME_NONE); // A real unknown-time fault still latches once in this barrier.
  push(source.get(), idr, 2190 * GST_MSECOND);
  expect(pull_pts(sink.get()) == 2190 * GST_MSECOND && !state.invalid_sample_blocked &&
      state.bad_sample_revision == before_old_invalid_bad + 1 && state.brake_sample_revision == before_old_invalid_brake + 1,
      "old malformed AU counted again or masked the subsequent first real invalid-time event");
}

void test_raw_delivery_ceiling_for_misreported_rate() {
  mine_teleop::MediaProfile profile;
  profile.width = 64; profile.height = 48; profile.fps = 20;
  const auto description = mine_teleop::build_camera_input_pipeline("input", {"uyvy", 64, 48, 20}, profile) +
      "! appsink name=output max-buffers=100 drop=false sync=false async=false";
  Pipeline pipeline(description);
  auto source = element(pipeline, "input"), sink = element(pipeline, "output");
  pipeline.play();
  const auto origin = std::chrono::steady_clock::now();
  for (int index = 0; index < 90; ++index) {
    std::this_thread::sleep_until(origin + std::chrono::nanoseconds(GST_SECOND * index / 30));
    const Bytes raw(64 * 48 * 2, static_cast<std::uint8_t>(index));
    push(source.get(), raw, GST_SECOND * static_cast<GstClockTime>(index) / 30);
  }
  gst_app_src_end_of_stream(GST_APP_SRC(source.get()));
  unsigned delivered = 0;
  GstClockTime previous = GST_CLOCK_TIME_NONE;
  bool independent = true;
  while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink.get()), 3 * GST_SECOND)) {
    const auto pts = GST_BUFFER_PTS(gst_sample_get_buffer(sample));
    independent = independent && (!GST_CLOCK_TIME_IS_VALID(previous) || pts > previous);
    previous = pts; ++delivered; gst_sample_unref(sample);
  }
  // Startup/EOS account for at most one boundary frame; no duplicate old
  // pictures may be inserted to advertise a rate the source did not deliver.
  expect(delivered >= 55 && delivered <= 61 && independent,
      "actual 30 FPS with declared 20 FPS did not produce unique bounded 20 FPS raw output: " + std::to_string(delivered));
}

void test_actual_codec_decode_after_reference_gap(AccessUnitCodec codec) {
  const std::string name = codec == AccessUnitCodec::H264 ? "h264" : "h265";
  const auto encoder = codec == AccessUnitCodec::H264
      ? "x264enc tune=zerolatency speed-preset=ultrafast key-int-max=10 bframes=0 byte-stream=true"
      : "x265enc tune=zerolatency speed-preset=ultrafast key-int-max=10 option-string=pools=1:frame-threads=1:open-gop=0";
  Pipeline encode("videotestsrc num-buffers=30 pattern=ball ! video/x-raw,width=128,height=96,framerate=20/1 ! " +
      std::string(encoder) + " ! " + name + "parse config-interval=-1 ! video/x-" + name +
      ",stream-format=byte-stream,alignment=au ! appsink name=output sync=false async=false");
  auto encoded = element(encode, "output");
  encode.play();
  struct Unit { Bytes bytes; GstClockTime pts; };
  std::vector<Unit> units;
  while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(encoded.get()), 3 * GST_SECOND)) {
    auto* buffer = gst_sample_get_buffer(sample);
    GstMapInfo mapped{};
    expect(gst_buffer_map(buffer, &mapped, GST_MAP_READ), "real encoder output could not be inspected");
    units.push_back({Bytes(mapped.data, mapped.data + mapped.size), GST_BUFFER_PTS(buffer)});
    gst_buffer_unmap(buffer, &mapped); gst_sample_unref(sample);
  }
  expect(units.size() == 30, "real encoder did not produce 30 pictures");
  Pipeline decode("appsrc name=input is-live=true format=time caps=video/x-" + name +
      ",stream-format=byte-stream,alignment=au ! " + name + "parse ! avdec_" + name +
      " ! appsink name=output sync=false async=false");
  auto source = element(decode, "input"), sink = element(decode, "output");
  decode.play();
  EncodedFreshnessGate gate(500);
  unsigned forwarded = 0, discarded = 0;
  bool resumed_idr = false;
  for (std::size_t index = 0; index < units.size(); ++index) {
    const auto info = inspect_access_unit(codec, units[index].bytes);
    const auto decision = gate.observe(info, index >= 1 && index <= 5 ? 501U : 1U);
    if (decision.forward) {
      if (discarded > 0 && !resumed_idr) {
        expect(info.idr && info.parameter_sets, "real codec resumed from a dependent/non-header AU");
        resumed_idr = true;
      }
      push(source.get(), units[index].bytes, units[index].pts);
      ++forwarded;
    } else ++discarded;
  }
  gst_app_src_end_of_stream(GST_APP_SRC(source.get()));
  unsigned decoded = 0;
  while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink.get()), 3 * GST_SECOND)) {
    ++decoded; gst_sample_unref(sample);
  }
  expect(discarded >= 5 && resumed_idr && decoded == forwarded && decoded >= 20,
      "actual " + name + " decoder did not recover every forwarded AU after IDR: decoded=" +
      std::to_string(decoded) + ", forwarded=" + std::to_string(forwarded));
}
} // namespace

int main() {
  gst_init(nullptr, nullptr);
  try {
    for (const auto codec : {AccessUnitCodec::H264, AccessUnitCodec::H265}) {
      test_send_queue_backpressure_and_sample_count(codec);
      std::cout << "[PASS] actual send queue/probe backpressure, IDR requests and new-sample counters "
          << (codec == AccessUnitCodec::H264 ? "H264" : "H265") << '\n';
      test_fault_history_between_control_reads(codec);
      std::cout << "[PASS] actual send probe preserves stale/third/invalid bad-to-good events between control reads "
          << (codec == AccessUnitCodec::H264 ? "H264" : "H265") << '\n';
      test_actual_codec_decode_after_reference_gap(codec);
      std::cout << "[PASS] actual software encoder/decoder reference-gap recovery "
          << (codec == AccessUnitCodec::H264 ? "H264" : "H265") << '\n';
    }
    test_raw_delivery_ceiling_for_misreported_rate();
    std::cout << "[PASS] declared 20 / actual 30 FPS independent raw drop-only ceiling\n";
  } catch (const std::exception& error) {
    std::cerr << "[FAIL] " << error.what() << '\n';
    return 1;
  }
}
