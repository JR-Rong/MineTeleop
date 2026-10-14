#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace mine_teleop::detail {

inline constexpr std::uint32_t kRecordingWireMagic = 0x4d545231;
inline constexpr std::uint32_t kRecordingWireVersion = 1;
inline constexpr std::size_t kRecordingLaneByteLimit = 4U * 1024U * 1024U;
inline constexpr std::size_t kRecordingLaneLimit = 16;
inline constexpr std::size_t kRecordingConfigByteLimit = 64U * 1024U;
enum class RecordingPacketType : std::uint32_t { Start = 1, AccessUnit = 2, End = 3 };

// Local same-build/same-host IPC only. No raw pointers or GstBuffer ownership
// crosses exec. Explicit scalar segment fields preserve encoder timestamp
// shifts without relabelling old pictures from IPC delivery time.
struct RecordingWireHeader {
  std::uint32_t magic{kRecordingWireMagic}, version{kRecordingWireVersion};
  RecordingPacketType type{RecordingPacketType::AccessUnit};
  std::uint32_t lane{0}, bytes{0}, buffer_flags{0}, segment_flags{0}, reserved{0};
  std::uint64_t pts{0}, dts{0}, duration{0};
  double rate{1.0}, applied_rate{1.0};
  std::uint64_t start{0}, stop{0}, time{0}, base{0}, offset{0}, position{0}, segment_duration{0};
};
static_assert(std::is_trivially_copyable_v<RecordingWireHeader>);
static_assert(offsetof(RecordingWireHeader, pts) == 32);

// Exactly one serialized GStreamer producer and one IPC consumer per lane.
// Storage is preallocated: no allocator, file I/O, blocking mutex or retained
// encoder-pool buffer on admission. A rejected whole AU ends recording; it is
// never silently skipped while continuing an allegedly complete reference chain.
class RecordingAuRing {
 public:
  RecordingAuRing(std::size_t slots, std::size_t bytes)
      : slots_(slots), bytes_(bytes), headers_(slots), storage_(std::make_unique<std::uint8_t[]>(bytes)) {
    if (slots == 0 || bytes == 0) throw std::invalid_argument("recording ring needs finite slots and bytes");
  }
  [[nodiscard]] bool can_push(std::size_t count) const {
    return count != 0 && count <= bytes_ &&
        produced_.load(std::memory_order_relaxed) - consumed_.load(std::memory_order_acquire) < slots_ &&
        produced_bytes_.load(std::memory_order_relaxed) - consumed_bytes_.load(std::memory_order_acquire) <= bytes_ - count;
  }
  [[nodiscard]] bool push(RecordingWireHeader header, std::span<const std::uint8_t> payload) {
    if (!can_push(payload.size())) return false;
    const auto byte_sequence = produced_bytes_.load(std::memory_order_relaxed);
    const auto index = static_cast<std::size_t>(byte_sequence % bytes_);
    const auto first = std::min(payload.size(), bytes_ - index);
    std::memcpy(storage_.get() + index, payload.data(), first);
    if (first < payload.size()) std::memcpy(storage_.get(), payload.data() + first, payload.size() - first);
    header.bytes = static_cast<std::uint32_t>(payload.size());
    headers_[produced_.load(std::memory_order_relaxed) % slots_] = header;
    produced_bytes_.store(byte_sequence + payload.size(), std::memory_order_relaxed);
    produced_.fetch_add(1, std::memory_order_release);
    return true;
  }
  struct ReadView { const RecordingWireHeader* header; std::span<const std::uint8_t> first, second; };
  [[nodiscard]] std::optional<ReadView> front() const {
    const auto consumed = consumed_.load(std::memory_order_relaxed);
    if (consumed == produced_.load(std::memory_order_acquire)) return std::nullopt;
    const auto& header = headers_[consumed % slots_];
    const auto index = static_cast<std::size_t>(consumed_bytes_.load(std::memory_order_relaxed) % bytes_);
    const auto first = std::min<std::size_t>(header.bytes, bytes_ - index);
    return ReadView{&header, {storage_.get() + index, first}, {storage_.get(), header.bytes - first}};
  }
  void pop() {
    const auto consumed = consumed_.load(std::memory_order_relaxed);
    const auto count = headers_[consumed % slots_].bytes;
    consumed_bytes_.fetch_add(count, std::memory_order_release);
    consumed_.fetch_add(1, std::memory_order_release);
  }
  [[nodiscard]] std::size_t queued() const {
    const auto consumed = consumed_.load(std::memory_order_acquire);
    const auto produced = produced_.load(std::memory_order_acquire);
    return static_cast<std::size_t>(std::min<std::uint64_t>(slots_, produced - consumed)); // Approximate diagnostic only.
  }
 private:
  std::size_t slots_, bytes_;
  std::vector<RecordingWireHeader> headers_;
  std::unique_ptr<std::uint8_t[]> storage_;
  alignas(64) std::atomic<std::uint64_t> produced_{0}, produced_bytes_{0};
  alignas(64) std::atomic<std::uint64_t> consumed_{0}, consumed_bytes_{0};
};

} // namespace mine_teleop::detail
