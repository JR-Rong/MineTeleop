#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

namespace mine_teleop::detail {

// A sink owns the actual I/O boundary. This is an internal cooperative
// contract: write() must observe the stop token promptly. stop() first waits
// only for its configured drain window, then requests that token and joins;
// it deliberately has no detached-worker fallback. A non-cooperative custom
// sink is unsupported because it would invalidate the shutdown bound.
enum class DiagnosticWriteResult {
  Written,
  Cancelled,
  TimedOut,
  Failed,
};

class DiagnosticSink {
 public:
  virtual ~DiagnosticSink() = default;

  [[nodiscard]] virtual DiagnosticWriteResult write(
      std::string_view jsonl_line,
      std::stop_token stop_token) noexcept = 0;
};

[[nodiscard]] std::shared_ptr<DiagnosticSink> make_stdout_diagnostic_sink();

struct DiagnosticEmitterStats {
  std::uint64_t enqueued_total{0};
  std::uint64_t emitted_total{0};
  std::uint64_t dropped_total{0};
  std::uint64_t oversized_total{0};
  std::uint64_t malformed_total{0};
  std::uint64_t sink_failures_total{0};
  std::uint64_t sink_timeouts_total{0};
  std::uint64_t shutdown_timeouts_total{0};
  std::size_t queued{0};
  bool accepting{false};
};

// A bounded JSONL queue for diagnostics. submit() never waits on the sink or
// on a contended queue mutex; shutdown first stops producers, then drains only
// within its deadline and requests cancellation before joining the worker.
class DiagnosticEmitter final {
 public:
  static constexpr std::size_t kDefaultQueueCapacity = 256;
  static constexpr std::size_t kDefaultMaxLineBytes = 3U * 1024U;

  struct Limits {
    std::size_t queue_capacity{kDefaultQueueCapacity};
    std::size_t max_line_bytes{kDefaultMaxLineBytes};
    std::chrono::milliseconds shutdown_timeout{250};
  };

  explicit DiagnosticEmitter(std::shared_ptr<DiagnosticSink> sink, Limits limits);
  ~DiagnosticEmitter();

  DiagnosticEmitter(const DiagnosticEmitter&) = delete;
  DiagnosticEmitter& operator=(const DiagnosticEmitter&) = delete;

  // The input is one JSON record without its trailing newline. Newlines and
  // oversized records are rejected before they can corrupt a JSONL stream.
  [[nodiscard]] bool submit(std::string json_record) noexcept;

  // Account for a producer-side serialization failure that prevented submit().
  void note_producer_drop() noexcept;

  // Stop new producers, drain only for the configured bounded interval, then
  // request cancellation and join. It is idempotent. The bound after
  // cancellation depends on the internal cooperative DiagnosticSink contract.
  void stop() noexcept;

  [[nodiscard]] DiagnosticEmitterStats stats() const noexcept;

 private:
  struct State;

  static void worker_loop(std::shared_ptr<State> state) noexcept;

  std::shared_ptr<State> state_;
  std::thread worker_;
  mutable std::mutex lifecycle_mutex_;
  bool stop_started_{false};
};

}  // namespace mine_teleop::detail
