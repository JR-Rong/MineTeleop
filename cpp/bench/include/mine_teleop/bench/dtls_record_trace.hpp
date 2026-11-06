#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace mine_teleop::detail {

struct DtlsRecordHeader {
  std::uint8_t type;
  std::uint16_t epoch;
  std::uint64_t sequence;
};

// The 13-byte DTLS 1.x record header contains no SCTP or application data.
inline std::optional<DtlsRecordHeader> parse_dtls_record_header(const std::uint8_t* data, std::size_t size) {
  if (!data || size < 13 || data[0] < 20 || data[0] > 25 || data[1] != 0xfe ||
      (data[2] != 0xff && data[2] != 0xfd && data[2] != 0xfc)) return std::nullopt;
  std::uint64_t sequence = 0;
  for (std::size_t i = 5; i < 11; ++i) sequence = (sequence << 8) | data[i];
  return DtlsRecordHeader{data[0], static_cast<std::uint16_t>((data[3] << 8) | data[4]), sequence};
}

// A diagnostic probe must never wait for the control worker or retain packet
// contents. Missing observations are explicit and invalidate full correlation.
class DtlsRecordTrace {
 public:
  struct Entry { std::int64_t monotonic_us; std::size_t bytes; DtlsRecordHeader header; };
  struct Batch { std::vector<Entry> records; std::uint64_t dropped; };
  static constexpr std::size_t kCapacity = 4096;

  void record(const std::uint8_t* header, std::size_t header_size, std::size_t bytes, std::int64_t now_us) {
    const auto parsed = parse_dtls_record_header(header, header_size);
    if (!parsed) return;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (!lock || count_ >= kCapacity) { ++dropped_; return; }
    records_[count_++] = {now_us, bytes, *parsed};
  }
  Batch drain() {
    std::lock_guard lock(mutex_);
    Batch batch{{}, dropped_.exchange(0)};
    batch.records.assign(records_.begin(), records_.begin() + count_);
    count_ = 0;
    return batch;
  }

 private:
  std::mutex mutex_;
  std::array<Entry, kCapacity> records_{};
  std::size_t count_{0};
  std::atomic<std::uint64_t> dropped_{0};
};

}  // namespace mine_teleop::detail
