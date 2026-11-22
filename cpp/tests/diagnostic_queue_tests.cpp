#include "mine_teleop/detail/diagnostic_queue.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using mine_teleop::detail::DiagnosticQueue;

void expect(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

void capacity_and_reuse() {
  DiagnosticQueue<int, 4> queue;
  for (int round = 0; round < 1000; ++round) {
    for (int i = 0; i < 4; ++i) expect(queue.try_push(round * 4 + i), "early full queue");
    expect(!queue.try_push(-1), "full queue overwritten");
    int value = -1;
    for (int i = 0; i < 4; ++i) {
      expect(queue.try_pop(value), "missing record");
      expect(value == round * 4 + i, "FIFO order changed");
    }
    expect(!queue.try_pop(value), "empty queue produced a record");
  }
  queue.close();
  expect(queue.drained() && queue.accepted_total() == 4000, "drain count differs");
  expect(!queue.try_push(4001), "closed queue accepted a record");
}

void concurrent_producers() {
  struct Record { int producer{}, sequence{}; };
  DiagnosticQueue<Record, 256> queue;
  constexpr int producers = 4, each = 50000;
  std::atomic<int> finished{0};
  std::atomic<bool> cancel{false};
  std::vector<std::thread> threads;
  for (int producer = 0; producer < producers; ++producer) {
    threads.emplace_back([&, producer] {
      for (int sequence = 0; sequence < each && !cancel.load(); ++sequence) {
        Record record{producer, sequence};
        // Tests retry on a deliberately saturated queue. Product trace callers
        // instead count and report a rejected record without waiting.
        while (!queue.try_push(std::move(record)) && !cancel.load()) std::this_thread::yield();
      }
      finished.fetch_add(1);
    });
  }
  int next[producers]{};
  bool ordered = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (finished.load() != producers || queue.pending() != 0) {
    Record record;
    if (queue.try_pop(record)) {
      if (record.producer < 0 || record.producer >= producers ||
          record.sequence != next[record.producer]++) ordered = false;
    } else std::this_thread::yield();
    if (std::chrono::steady_clock::now() > deadline) { cancel.store(true); break; }
  }
  for (auto& thread : threads) thread.join();
  queue.close();
  expect(!cancel.load() && ordered, "concurrent drain stalled or reordered records");
  for (int count : next) expect(count == each, "concurrent record missing or duplicated");
  expect(queue.drained() && queue.accepted_total() == producers * each, "concurrent total differs");
}

void close_during_publication() {
  struct Record {
    int value{};
    std::atomic<bool>* entered{};
    std::atomic<bool>* release{};
    Record& operator=(Record&& other) noexcept {
      if (other.entered) {
        other.entered->store(true);
        while (!other.release->load()) std::this_thread::yield();
      }
      value = other.value;
      return *this;
    }
  };
  DiagnosticQueue<Record, 2> queue;
  std::atomic<bool> entered{false}, release{false}, accepted{false};
  std::thread producer([&] {
    Record record{42, &entered, &release};
    accepted.store(queue.try_push(std::move(record)));
  });
  while (!entered.load()) std::this_thread::yield();
  queue.close();
  Record result;
  const bool drained_early = queue.drained();
  const bool popped_early = queue.try_pop(result);
  release.store(true);
  producer.join();
  expect(!drained_early && !popped_early, "unpublished slot treated as drained or readable");
  expect(accepted.load() && queue.try_pop(result) && result.value == 42, "close discarded in-flight record");
  expect(queue.drained(), "closed queue never drained");
}

int main() {
  try {
    capacity_and_reuse();
    concurrent_producers();
    close_during_publication();
    std::cout << "diagnostic_queue_tests=passed concurrent_records=200000\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
