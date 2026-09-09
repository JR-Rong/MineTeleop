#include "mine_teleop/detail/diagnostic_emitter.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <array>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

using mine_teleop::detail::DiagnosticEmitter;
using mine_teleop::detail::DiagnosticSink;
using mine_teleop::detail::DiagnosticWriteResult;
using Json = nlohmann::json;

class TestFailure final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

void expect(bool condition, std::string_view message) {
  if (!condition)
    throw TestFailure(std::string(message));
}

template <typename Predicate>
bool wait_until(Predicate&& predicate, std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return predicate();
}

class CollectingSink final : public DiagnosticSink {
 public:
  [[nodiscard]] DiagnosticWriteResult write(std::string_view jsonl_line,
                                            std::stop_token stop_token) noexcept override {
    if (stop_token.stop_requested())
      return DiagnosticWriteResult::Cancelled;
    try {
      std::lock_guard lock(mutex_);
      lines_.emplace_back(jsonl_line);
      return DiagnosticWriteResult::Written;
    } catch (...) {
      return DiagnosticWriteResult::Failed;
    }
  }

  [[nodiscard]] std::vector<std::string> lines() const {
    std::lock_guard lock(mutex_);
    return lines_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> lines_;
};

class FailingSink final : public DiagnosticSink {
 public:
  [[nodiscard]] DiagnosticWriteResult write(std::string_view, std::stop_token) noexcept override {
    return DiagnosticWriteResult::Failed;
  }
};

// This deliberately models a blocked but cooperative sink. A sink that never
// observes its stop token violates DiagnosticSink's internal contract, so it
// is not a valid bounded-shutdown test double.
class CooperativeBlockingSink final : public DiagnosticSink {
 public:
  [[nodiscard]] DiagnosticWriteResult write(std::string_view,
                                            std::stop_token stop_token) noexcept override {
    {
      std::lock_guard lock(mutex_);
      entered_ = true;
    }
    entered_cv_.notify_all();
    std::unique_lock lock(mutex_);
    while (!stop_token.stop_requested()) {
      entered_cv_.wait_for(lock, std::chrono::milliseconds(2));
    }
    return DiagnosticWriteResult::Cancelled;
  }

  [[nodiscard]] bool wait_until_entered(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return entered_cv_.wait_for(lock, timeout, [&] { return entered_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable entered_cv_;
  bool entered_{false};
};

DiagnosticEmitter::Limits test_limits(std::size_t queue_capacity = 128) {
  auto limits = DiagnosticEmitter::Limits{};
  limits.queue_capacity = queue_capacity;
  limits.max_line_bytes = 256;
  limits.shutdown_timeout = std::chrono::milliseconds(80);
  return limits;
}

void test_rejects_multiline_and_oversized_records() {
  auto sink = std::make_shared<CollectingSink>();
  DiagnosticEmitter emitter(sink, test_limits());

  expect(!emitter.submit("{\"event\":\"one\"}\n{\"event\":\"two\"}"),
         "emitter accepted a multi-line JSONL record");
  expect(!emitter.submit(std::string(300, 'x')),
         "emitter accepted a record above the configured line limit");
  emitter.stop();

  const auto stats = emitter.stats();
  expect(stats.malformed_total == 1, "multi-line record was not counted as malformed");
  expect(stats.oversized_total == 1, "oversized record was not counted");
  expect(stats.dropped_total == 2, "rejected records were not counted as dropped");
  expect(sink->lines().empty(), "rejected records reached the sink");
}

void test_failing_sink_is_observable_and_does_not_kill_worker() {
  auto sink = std::make_shared<FailingSink>();
  DiagnosticEmitter emitter(sink, test_limits());
  constexpr int kRecords = 6;
  for (int index = 0; index < kRecords; ++index) {
    expect(emitter.submit("{\"event\":\"sink_failure\",\"index\":" + std::to_string(index) + "}"),
           "failing sink unexpectedly rejected a bounded record");
  }
  expect(wait_until([&] { return emitter.stats().sink_failures_total >= kRecords; },
                    std::chrono::milliseconds(500)),
         "failing sink did not report every failed write");
  emitter.stop();

  const auto stats = emitter.stats();
  expect(stats.sink_failures_total == kRecords,
         "failed writes were lost after the first sink failure");
  expect(stats.emitted_total == 0, "failing sink reported an emitted line");
  expect(stats.dropped_total == kRecords, "failed writes were not counted as dropped");
}

void test_full_cooperative_blocking_sink_drops_and_shutdown_is_bounded() {
  auto sink = std::make_shared<CooperativeBlockingSink>();
  DiagnosticEmitter emitter(sink, test_limits(2));
  expect(emitter.submit("{\"event\":\"blocked\",\"index\":0}"),
         "cooperative blocking sink did not accept the first record");
  expect(sink->wait_until_entered(std::chrono::milliseconds(200)),
         "cooperative blocking sink was never reached by the worker");

  constexpr int kQueuedAttempts = 10;
  for (int index = 0; index < kQueuedAttempts; ++index) {
    static_cast<void>(
        emitter.submit("{\"event\":\"blocked\",\"index\":" + std::to_string(index + 1) + "}"));
  }
  const auto started = std::chrono::steady_clock::now();
  emitter.stop();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started);
  const auto stats = emitter.stats();
  expect(elapsed < std::chrono::milliseconds(250),
         "diagnostic shutdown waited indefinitely for a cooperative blocked sink");
  expect(stats.shutdown_timeouts_total == 1,
         "cooperative blocked sink shutdown did not expose the cancellation deadline");
  expect(stats.dropped_total == static_cast<std::uint64_t>(kQueuedAttempts + 1),
         "cooperative blocked sink did not account for the blocked and queued records");
}

#if defined(__unix__) || defined(__APPLE__)
class ScopedStdoutPipe final {
 public:
  ScopedStdoutPipe() {
    std::cout.flush();
    expect(::pipe(pipe_fds_.data()) == 0, "cannot create stdout test pipe");
    saved_stdout_ = ::dup(STDOUT_FILENO);
    expect(saved_stdout_ >= 0, "cannot duplicate stdout for pipe test");
    expect(::dup2(pipe_fds_[1], STDOUT_FILENO) >= 0, "cannot redirect stdout to test pipe");
    ::close(pipe_fds_[1]);
    pipe_fds_[1] = -1;
  }

  ~ScopedStdoutPipe() {
    restore();
    if (pipe_fds_[0] >= 0)
      ::close(pipe_fds_[0]);
  }

  ScopedStdoutPipe(const ScopedStdoutPipe&) = delete;
  ScopedStdoutPipe& operator=(const ScopedStdoutPipe&) = delete;

  [[nodiscard]] std::string restore_and_read_all() {
    restore();
    std::string result;
    std::array<char, 4096> buffer{};
    while (true) {
      const auto received = ::read(pipe_fds_[0], buffer.data(), buffer.size());
      if (received == 0)
        break;
      expect(received > 0, "cannot read stdout test pipe");
      result.append(buffer.data(), static_cast<std::size_t>(received));
    }
    ::close(pipe_fds_[0]);
    pipe_fds_[0] = -1;
    return result;
  }

 private:
  void restore() noexcept {
    if (saved_stdout_ < 0)
      return;
    static_cast<void>(::dup2(saved_stdout_, STDOUT_FILENO));
    ::close(saved_stdout_);
    saved_stdout_ = -1;
  }

  std::array<int, 2> pipe_fds_{{-1, -1}};
  int saved_stdout_{-1};
};

void test_real_stdout_pipe_concurrent_producers_have_bounded_loss_and_complete_jsonl_lines() {
  ScopedStdoutPipe stdout_pipe;
  const int stdout_flags_before = ::fcntl(STDOUT_FILENO, F_GETFL);
  expect(stdout_flags_before >= 0, "cannot read stdout flags before diagnostic sink test");
  auto limits = test_limits(512);
  limits.shutdown_timeout = std::chrono::milliseconds(120);
  DiagnosticEmitter emitter(mine_teleop::detail::make_stdout_diagnostic_sink(), limits);
  const std::string payload(160, 'x');
  constexpr int kProducerCount = 4;
  constexpr int kRecordsPerProducer = 200;
  std::vector<std::thread> producers;
  producers.reserve(kProducerCount);
  for (int producer = 0; producer < kProducerCount; ++producer) {
    producers.emplace_back([&emitter, &payload, producer] {
      for (int index = 0; index < kRecordsPerProducer; ++index) {
        static_cast<void>(emitter.submit(
            "{\"event\":\"stdout_pipe\",\"producer\":" + std::to_string(producer) +
            ",\"index\":" + std::to_string(index) + ",\"payload\":\"" + payload + "\"}"));
      }
    });
  }
  for (auto& producer : producers)
    producer.join();
  expect(wait_until([&] { return emitter.stats().sink_timeouts_total > 0; },
                    std::chrono::milliseconds(600)),
         "real stdout pipe did not expose its bounded write deadline when full");
  emitter.stop();
  const auto stats = emitter.stats();
  const int stdout_flags_after = ::fcntl(STDOUT_FILENO, F_GETFL);
  expect(stdout_flags_after >= 0, "cannot read stdout flags after diagnostic sink test");
  expect((stdout_flags_after & O_NONBLOCK) == (stdout_flags_before & O_NONBLOCK),
         "diagnostic sink changed the process stdout O_NONBLOCK flag");
  const auto bytes = stdout_pipe.restore_and_read_all();
  expect(stats.sink_timeouts_total > 0, "full stdout pipe loss was not observable");
  expect(stats.emitted_total + stats.dropped_total ==
             static_cast<std::uint64_t>(kProducerCount * kRecordsPerProducer),
         "stdout pipe producer records were neither emitted nor counted as dropped");
  std::size_t line_start = 0;
  std::uint64_t parsed_lines = 0;
  while (line_start < bytes.size()) {
    const auto newline = bytes.find('\n', line_start);
    expect(newline != std::string::npos, "stdout pipe ended with a partial JSONL record");
    const auto parsed = Json::parse(bytes.substr(line_start, newline - line_start));
    expect(parsed.value("event", "") == "stdout_pipe" && parsed.contains("producer") &&
               parsed.contains("index"),
           "stdout pipe emitted a corrupted JSON record");
    ++parsed_lines;
    line_start = newline + 1;
  }
  expect(parsed_lines == stats.emitted_total,
         "real stdout pipe bytes do not match the emitter's successful line count");
}
#endif

void test_concurrent_producers_preserve_jsonl_record_boundaries() {
  auto sink = std::make_shared<CollectingSink>();
  DiagnosticEmitter emitter(sink, test_limits(512));
  constexpr int kProducerCount = 6;
  constexpr int kRecordsPerProducer = 48;
  std::vector<std::thread> producers;
  producers.reserve(kProducerCount);
  for (int producer = 0; producer < kProducerCount; ++producer) {
    producers.emplace_back([&emitter, producer] {
      for (int index = 0; index < kRecordsPerProducer; ++index) {
        static_cast<void>(
            emitter.submit("{\"event\":\"concurrent\",\"producer\":" + std::to_string(producer) +
                           ",\"index\":" + std::to_string(index) + "}"));
      }
    });
  }
  for (auto& producer : producers)
    producer.join();
  emitter.stop();

  const auto stats = emitter.stats();
  const auto lines = sink->lines();
  const auto submitted = static_cast<std::uint64_t>(kProducerCount * kRecordsPerProducer);
  expect(stats.emitted_total + stats.dropped_total == submitted,
         "concurrent producer records were neither emitted nor counted as dropped");
  expect(lines.size() == stats.emitted_total,
         "sink line count differs from emitter emission count");
  for (const auto& line : lines) {
    expect(!line.empty() && line.back() == '\n', "emitted diagnostic is not a complete JSONL line");
    const auto parsed = Json::parse(line);
    expect(parsed.value("event", "") == "concurrent" && parsed.contains("producer") &&
               parsed.contains("index"),
           "concurrent output lost a diagnostic record boundary");
  }
}

struct TestCase {
  const char* name;
  void (*function)();
};

}  // namespace

int main() {
  const TestCase tests[] = {
      {"rejects_multiline_and_oversized_records", test_rejects_multiline_and_oversized_records},
      {"failing_sink_is_observable_and_does_not_kill_worker",
       test_failing_sink_is_observable_and_does_not_kill_worker},
      {"full_cooperative_blocking_sink_drops_and_shutdown_is_bounded",
       test_full_cooperative_blocking_sink_drops_and_shutdown_is_bounded},
#if defined(__unix__) || defined(__APPLE__)
      {"real_stdout_pipe_concurrent_producers_have_bounded_loss_and_complete_jsonl_lines",
       test_real_stdout_pipe_concurrent_producers_have_bounded_loss_and_complete_jsonl_lines},
#endif
      {"concurrent_producers_preserve_jsonl_record_boundaries",
       test_concurrent_producers_preserve_jsonl_record_boundaries},
  };

  int failures = 0;
  for (const auto& test : tests) {
    try {
      test.function();
      std::cout << "PASS " << test.name << '\n';
    } catch (const std::exception& error) {
      ++failures;
      std::cerr << "FAIL " << test.name << ": " << error.what() << '\n';
    }
  }
  if (failures != 0) {
    std::cerr << failures << " diagnostic emitter test(s) failed\n";
    return 1;
  }
  std::cout << "diagnostic_emitter_tests=passed count=" << std::size(tests) << '\n';
  return 0;
}
