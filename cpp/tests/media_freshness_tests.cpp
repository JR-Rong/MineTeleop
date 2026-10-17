#include "mine_teleop/detail/media_freshness.hpp"

#include <iostream>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace mine_teleop::detail;
using Bytes = std::vector<std::uint8_t>;
void expect(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}
Bytes access_unit(AccessUnitCodec codec, std::initializer_list<int> types) {
  Bytes bytes;
  for (const auto type : types) {
    bytes.insert(bytes.end(), {0, 0, 0, 1});
    if (codec == AccessUnitCodec::H264) bytes.push_back(static_cast<std::uint8_t>(type | 0x60));
    else {
      bytes.push_back(static_cast<std::uint8_t>(type << 1));
      bytes.push_back(1);
    }
    bytes.push_back(0x80);
  }
  return bytes;
}
void test_reference_chain(AccessUnitCodec codec) {
  const auto initial = codec == AccessUnitCodec::H264 ? access_unit(codec, {7, 8, 5}) : access_unit(codec, {32, 33, 34, 19});
  const auto delta = access_unit(codec, {1});
  const auto idr_without_headers = access_unit(codec, {codec == AccessUnitCodec::H264 ? 5 : 19});
  EncodedFreshnessGate gate(500);
  const auto idr = inspect_access_unit(codec, initial);
  EncodedFreshnessGate new_stream(500);
  expect(!new_stream.observe(inspect_access_unit(codec, delta), 0).forward, "new stream began with a dependent AU");
  const auto dependent = inspect_access_unit(codec, delta);
  expect(idr.valid && idr.picture && idr.idr && idr.parameter_sets, "complete IDR not detected");
  expect(gate.observe(idr, 10).forward, "fresh initial IDR rejected");
  expect(gate.observe(dependent, 499).forward, "healthy dependent AU discarded");
  const auto stale = gate.observe(dependent, 500);
  expect(!stale.forward && stale.request_key_unit && stale.reason == FreshnessDropReason::Stale, "stale boundary did not discard and request IDR");
  expect(!gate.observe(dependent, 501).request_key_unit, "same stale episode repeatedly requested keyframes");
  expect(!gate.observe(dependent, 1).forward, "fresh dependent AU resumed after a dropped reference");
  expect(!gate.observe(inspect_access_unit(codec, idr_without_headers), 1).forward, "IDR without codec headers resumed");
  expect(!gate.observe(idr, 700).forward, "obsolete IDR resumed");
  expect(gate.observe(idr, 1).forward && !gate.waiting_idr(), "complete fresh IDR did not restore reference chain");
  expect(gate.observe(dependent, 2).forward, "fresh dependent AU failed after IDR");
  expect(!gate.observe(dependent, std::nullopt).forward, "unknown time was treated fresh");
  expect(!gate.observe(dependent, 0).forward, "unknown-time reference drop failed to fence later AU");
  expect(gate.observe(idr, 0).forward, "fresh IDR did not recover unknown-time drop");
  gate.set_max_age_ms(1000);
  expect(gate.observe(dependent, 600).forward, "explicit changed production budget ignored");
  gate.reset();
  const auto after_flush = gate.observe(dependent, 0);
  expect(!after_flush.forward && after_flush.request_key_unit, "flush reset retained old references or failed to request IDR");
  expect(gate.observe(idr, 0).forward, "flush reset could not restart with IDR");
}
void test_capture_timestamp_age() {
  using Clock = std::chrono::steady_clock;
  const auto begin = Clock::time_point(std::chrono::milliseconds(1000));
  const auto finish = Clock::time_point(std::chrono::milliseconds(1100));
  const auto old = select_capture_steady_time(begin, finish, 500000);
  expect(old.used_device_timestamp && old.value == Clock::time_point(std::chrono::milliseconds(500)), "queued device buffer was relabelled from callback arrival");
  const auto waiting = select_capture_steady_time(begin, finish, std::nullopt);
  expect(!waiting.used_device_timestamp && waiting.value == begin, "fallback hid 100ms acquisition wait");
  for (const auto invalid : {0LL, -1LL, 2000000LL, std::numeric_limits<long long>::max()}) {
    const auto fallback = select_capture_steady_time(begin, finish, invalid);
    expect(!fallback.used_device_timestamp && fallback.value == begin, "invalid/future device time became fresh");
  }
  expect(checked_device_timestamp_us(1, 500000) == 1500000, "valid device timestamp conversion changed");
  expect(!checked_device_timestamp_us(-1, 0) && !checked_device_timestamp_us(1, 1000000) &&
      !checked_device_timestamp_us(std::numeric_limits<std::int64_t>::max(), 0), "malformed device timestamp overflowed or became valid");
}
void test_new_sample_counter() {
  MediaSampleAnomalies first, second;
  expect(first.observe(100, false) && first.consecutive() == 1, "first new video anomaly not counted");
  for (int i = 0; i < 100; ++i) expect(!first.observe(100, false), "repeated sample counted again");
  expect(!first.observe(100, true) && first.consecutive() == 1, "duplicate normal sample cleared counter");
  expect(!first.observe(99, true) && first.consecutive() == 1, "older normal sample cleared counter");
  expect(!first.observe(std::nullopt, true) && first.consecutive() == 1, "unknown sample cleared counter");
  expect(second.observe(101, false) && second.consecutive() == 1 && first.consecutive() == 1, "separate streams accumulated together");
  expect(first.observe(101, false) && first.consecutive() == 2, "second new anomaly not counted");
  expect(first.observe(102, false) && first.consecutive() == 3, "third new anomaly did not reach threshold");
  expect(first.observe(103, false) && first.consecutive() == 3, "saturating anomaly count overflowed");
  expect(first.observe(104, true) && first.consecutive() == 0 && second.consecutive() == 1, "new valid normal sample failed to clear only this stream");
  expect(first.observe(105, false) && first.consecutive() == 1, "normal sample did not break consecutiveness");
  first.reset_stream();
  expect(first.consecutive() == 1 && first.observe(1, false) && first.consecutive() == 2, "stream reset hid fault without a healthy sample");
}
void test_fault_history_between_recovery_reads() {
  MediaSampleAnomalies video;
  expect(video.observe(10, true), "initial normal picture missing");
  const auto before_bad = video.bad_sample_revision();
  const auto before_brake = video.brake_sample_revision();
  // A recovery reader does not inspect state during this complete episode.
  expect(video.observe(11, false) && video.observe(12, true), "short bad/good episode rejected");
  expect(video.consecutive() == 0 && video.bad_sample_revision() == before_bad + 1 &&
      video.brake_sample_revision() == before_brake,
      "latest normal picture erased a tolerable bad event between recovery reads");
  const auto after_short_bad = video.bad_sample_revision();
  expect(!video.observe(11, false) && !video.observe(12, false) && !video.observe(std::nullopt, false),
      "duplicate/old/unknown picture counted as a new fault");
  expect(video.bad_sample_revision() == after_short_bad && video.brake_sample_revision() == before_brake,
      "repeated observations changed retained fault history");
  for (const auto id : {13U, 14U, 15U, 16U}) expect(video.observe(id, false), "new bad picture rejected");
  expect(video.observe(17, true) && video.consecutive() == 0 &&
      video.bad_sample_revision() == after_short_bad + 4 && video.brake_sample_revision() == before_brake + 1,
      "third-crossing episode was erased by normal data or saturated samples produced repeated brakes");
  const auto retained_bad = video.bad_sample_revision(), retained_brake = video.brake_sample_revision();
  video.reset_stream();
  expect(video.observe(1, true) && video.bad_sample_revision() == retained_bad &&
      video.brake_sample_revision() == retained_brake, "stream reset or healthy restart erased event history");
  for (const auto id : {2U, 3U, 4U}) expect(video.observe(id, false), "restarted stream picture rejected");
  expect(video.brake_sample_revision() == retained_brake + 1,
      "normal data did not allow a later independent three-anomaly episode to brake");
}
void test_invalid_and_cra() {
  const auto codec = AccessUnitCodec::H265;
  EncodedFreshnessGate gate(500);
  const auto idr = inspect_access_unit(codec, access_unit(codec, {32, 33, 34, 19}));
  expect(gate.observe(idr, 0).forward, "initial H265 IDR rejected");
  expect(!gate.observe({}, 1).forward, "malformed AU forwarded");
  const auto cra = inspect_access_unit(codec, access_unit(codec, {32, 33, 34, 21}));
  expect(cra.valid && !cra.idr && !gate.observe(cra, 1).forward, "CRA mistaken for independently restartable IDR");
  const auto mixed = inspect_access_unit(codec, access_unit(codec, {32, 33, 34, 19, 1}));
  expect(!mixed.idr && !gate.observe(mixed, 1).forward, "mixed dependent pictures mistaken for clean IDR");
  expect(!inspect_access_unit(codec, Bytes{0, 0, 1, 0x26}).valid, "truncated H265 header accepted");
  expect(!inspect_access_unit(codec, Bytes{0, 0, 1, 0x26, 0}).valid, "zero temporal id accepted");
  expect(!inspect_access_unit(AccessUnitCodec::H264, Bytes{0x65, 0x80}).valid, "non byte-stream AU accepted");
  expect(!inspect_access_unit(AccessUnitCodec::H264, Bytes{0, 0, 1, 0xe5}).valid, "forbidden bit accepted");
  expect(!inspect_access_unit(AccessUnitCodec::H264, Bytes{0, 0, 1}).valid, "empty NAL accepted");
}
} // namespace

int main() {
  try {
    test_reference_chain(AccessUnitCodec::H264);
    std::cout << "[PASS] H264 stale AU fences dependent frames until fresh complete IDR\n";
    test_reference_chain(AccessUnitCodec::H265);
    std::cout << "[PASS] H265 stale AU fences dependent frames until fresh complete IDR\n";
    test_capture_timestamp_age();
    std::cout << "[PASS] monotonic device capture keeps queued age, fallback keeps acquisition wait\n";
    test_new_sample_counter();
    std::cout << "[PASS] only new per-stream samples count/clear, normal data breaks consecutiveness\n";
    test_fault_history_between_recovery_reads();
    std::cout << "[PASS] recovery reads retain short and third-crossing bad/good events after normal data/reset\n";
    test_invalid_and_cra();
    std::cout << "[PASS] malformed, mixed and CRA units cannot restart dropped references\n";
  } catch (const std::exception& error) {
    std::cerr << "[FAIL] " << error.what() << '\n';
    return 1;
  }
}
