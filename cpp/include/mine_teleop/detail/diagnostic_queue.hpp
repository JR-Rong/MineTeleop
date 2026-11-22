#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace mine_teleop::detail {

// Bounded multiple-producer/single-consumer queue. Slot sequence numbers keep
// producers independent of the consumer's allocation and output work. A full
// queue rejects the record; the control path never waits for log output.
template <class T, std::size_t Capacity>
class DiagnosticQueue {
  static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0);
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(std::is_nothrow_move_assignable_v<T>);

 public:
  DiagnosticQueue() {
    for (std::size_t i = 0; i < Capacity; ++i) slots_[i].sequence.store(i);
  }
  DiagnosticQueue(const DiagnosticQueue&) = delete;
  DiagnosticQueue& operator=(const DiagnosticQueue&) = delete;

  bool try_push(T&& value) noexcept {
    // Sequential consistency also orders close/drained against a producer
    // which has entered but has not yet reserved or published its slot.
    active_producers_.fetch_add(1);
    struct Finish {
      std::atomic<std::uint64_t>& active;
      ~Finish() { active.fetch_sub(1); }
    } finish{active_producers_};
    if (closed_.load()) return false;
    auto position = write_.load(std::memory_order_relaxed);
    for (;;) {
      auto& slot = slots_[position & (Capacity - 1)];
      const auto sequence = slot.sequence.load(std::memory_order_acquire);
      const auto difference = static_cast<std::int64_t>(sequence - position);
      if (difference < 0) return false;
      if (difference == 0 && write_.compare_exchange_weak(
              position, position + 1, std::memory_order_relaxed)) {
        slot.value = std::move(value);
        slot.sequence.store(position + 1, std::memory_order_release);
        return true;
      }
      position = write_.load(std::memory_order_relaxed);
    }
  }

  // Exactly one consumer may call try_pop.
  bool try_pop(T& value) noexcept {
    const auto position = read_.load(std::memory_order_relaxed);
    auto& slot = slots_[position & (Capacity - 1)];
    if (slot.sequence.load(std::memory_order_acquire) != position + 1) return false;
    value = std::move(slot.value);
    read_.store(position + 1, std::memory_order_release);
    slot.sequence.store(position + Capacity, std::memory_order_release);
    return true;
  }

  std::uint64_t pending() const noexcept {
    const auto read = read_.load(std::memory_order_acquire);
    return write_.load(std::memory_order_acquire) - read;
  }
  std::uint64_t accepted_total() const noexcept { return write_.load(std::memory_order_acquire); }
  void close() noexcept { closed_.store(true); }
  bool drained() const noexcept {
    return closed_.load() && active_producers_.load() == 0 && pending() == 0;
  }

 private:
  struct Slot {
    std::atomic<std::uint64_t> sequence{0};
    T value{};
  };
  std::array<Slot, Capacity> slots_;
  alignas(64) std::atomic<std::uint64_t> write_{0};
  alignas(64) std::atomic<std::uint64_t> read_{0};
  std::atomic<std::uint64_t> active_producers_{0};
  std::atomic<bool> closed_{false};
};

}  // namespace mine_teleop::detail
