#include "mine_teleop/detail/diagnostic_emitter.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <stdexcept>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#endif

namespace mine_teleop::detail {
namespace {

constexpr auto kStdoutPollSlice = std::chrono::milliseconds(10);
constexpr auto kStdoutWriteTimeout = std::chrono::milliseconds(100);

class UnavailableDiagnosticSink final : public DiagnosticSink {
 public:
  [[nodiscard]] DiagnosticWriteResult write(std::string_view, std::stop_token) noexcept override {
    return DiagnosticWriteResult::Failed;
  }
};

#if defined(__unix__) || defined(__APPLE__)
class StdoutDiagnosticSink final : public DiagnosticSink {
 public:
  [[nodiscard]] DiagnosticWriteResult write(std::string_view jsonl_line,
                                            std::stop_token stop_token) noexcept override {
    if (jsonl_line.empty() || jsonl_line.back() != '\n') {
      return DiagnosticWriteResult::Failed;
    }

    const long configured_pipe_buf = ::fpathconf(STDOUT_FILENO, _PC_PIPE_BUF);
    const auto atomic_write_limit = configured_pipe_buf > 0
                                        ? static_cast<std::size_t>(configured_pipe_buf)
                                        : static_cast<std::size_t>(PIPE_BUF);
    if (jsonl_line.size() > atomic_write_limit) {
      return DiagnosticWriteResult::Failed;
    }

    // All webrtc_media stdout is routed through DiagnosticEmitter. A try-lock
    // makes a second runtime drop instead of waiting on the same descriptor;
    // it also preserves one atomic JSONL writer without changing stdout's
    // process-wide O_NONBLOCK state.
    std::unique_lock output_lock(output_mutex_, std::try_to_lock);
    if (!output_lock.owns_lock())
      return DiagnosticWriteResult::TimedOut;

    // A closed launcher pipe must be a diagnostic failure, not a SIGPIPE that
    // takes the media runtime down. The mask belongs only to this worker and
    // is discarded when the worker exits.
    sigset_t sigpipe_mask{};
    sigemptyset(&sigpipe_mask);
    sigaddset(&sigpipe_mask, SIGPIPE);
    static_cast<void>(::pthread_sigmask(SIG_BLOCK, &sigpipe_mask, nullptr));

    const auto deadline = std::chrono::steady_clock::now() + kStdoutWriteTimeout;
    for (;;) {
      if (stop_token.stop_requested())
        return DiagnosticWriteResult::Cancelled;
      const auto now = std::chrono::steady_clock::now();
      if (now >= deadline)
        return DiagnosticWriteResult::TimedOut;
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
      const auto wait =
          std::max(std::chrono::milliseconds(1), std::min(remaining, kStdoutPollSlice));
      pollfd poll_fd{STDOUT_FILENO, POLLOUT, 0};
      const int ready = ::poll(&poll_fd, 1, static_cast<int>(wait.count()));
      if (ready == 0)
        continue;
      if (ready < 0) {
        if (errno == EINTR)
          continue;
        return DiagnosticWriteResult::Failed;
      }
      if ((poll_fd.revents & POLLOUT) == 0)
        return DiagnosticWriteResult::Failed;
      const auto written = ::write(STDOUT_FILENO, jsonl_line.data(), jsonl_line.size());
      if (written == static_cast<ssize_t>(jsonl_line.size())) {
        return DiagnosticWriteResult::Written;
      }
      if (written < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        continue;

      // A <= PIPE_BUF pipe write is all-or-nothing. Do not retry a suffix:
      // that could turn a JSONL stream into an unparsable partial record.
      return DiagnosticWriteResult::Failed;
    }
  }

 private:
  static std::mutex output_mutex_;
};

std::mutex StdoutDiagnosticSink::output_mutex_;
#endif

}  // namespace

struct DiagnosticEmitter::State {
  State(std::shared_ptr<DiagnosticSink> next_sink, Limits next_limits)
      : sink(std::move(next_sink)), limits(next_limits) {}

  std::shared_ptr<DiagnosticSink> sink;
  Limits limits;
  std::atomic<bool> accepting{true};
  std::mutex mutex;
  std::condition_variable ready;
  std::condition_variable finished;
  std::deque<std::string> queue;
  std::stop_source cancellation;
  bool stopping{false};
  bool worker_finished{false};
  std::atomic<std::uint64_t> enqueued_total{0};
  std::atomic<std::uint64_t> emitted_total{0};
  std::atomic<std::uint64_t> dropped_total{0};
  std::atomic<std::uint64_t> oversized_total{0};
  std::atomic<std::uint64_t> malformed_total{0};
  std::atomic<std::uint64_t> sink_failures_total{0};
  std::atomic<std::uint64_t> sink_timeouts_total{0};
  std::atomic<std::uint64_t> shutdown_timeouts_total{0};
};

std::shared_ptr<DiagnosticSink> make_stdout_diagnostic_sink() {
#if defined(__unix__) || defined(__APPLE__)
  return std::make_shared<StdoutDiagnosticSink>();
#else
  return std::make_shared<UnavailableDiagnosticSink>();
#endif
}

DiagnosticEmitter::DiagnosticEmitter(std::shared_ptr<DiagnosticSink> sink, Limits limits) {
  if (!sink)
    throw std::invalid_argument("diagnostic emitter requires a sink");
  if (limits.queue_capacity == 0 || limits.max_line_bytes < 2 ||
      limits.shutdown_timeout <= std::chrono::milliseconds::zero()) {
    throw std::invalid_argument("diagnostic emitter limits are invalid");
  }
  state_ = std::make_shared<State>(std::move(sink), limits);
  worker_ = std::thread([state = state_] { worker_loop(std::move(state)); });
}

DiagnosticEmitter::~DiagnosticEmitter() {
  stop();
}

bool DiagnosticEmitter::submit(std::string json_record) noexcept {
  const auto state = state_;
  if (!state)
    return false;
  const auto drop = [&] { state->dropped_total.fetch_add(1, std::memory_order_relaxed); };
  if (!state->accepting.load(std::memory_order_acquire)) {
    drop();
    return false;
  }
  if (json_record.find_first_of("\r\n") != std::string::npos) {
    state->malformed_total.fetch_add(1, std::memory_order_relaxed);
    drop();
    return false;
  }
  if (json_record.size() + 1 > state->limits.max_line_bytes) {
    state->oversized_total.fetch_add(1, std::memory_order_relaxed);
    drop();
    return false;
  }
  try {
    std::unique_lock lock(state->mutex, std::try_to_lock);
    if (!lock.owns_lock() || state->stopping ||
        state->queue.size() >= state->limits.queue_capacity) {
      drop();
      return false;
    }
    json_record.push_back('\n');
    state->queue.push_back(std::move(json_record));
    state->enqueued_total.fetch_add(1, std::memory_order_relaxed);
    lock.unlock();
    state->ready.notify_one();
    return true;
  } catch (...) {
    drop();
    return false;
  }
}

void DiagnosticEmitter::note_producer_drop() noexcept {
  if (state_)
    state_->dropped_total.fetch_add(1, std::memory_order_relaxed);
}

void DiagnosticEmitter::worker_loop(std::shared_ptr<State> state) noexcept {
  try {
    for (;;) {
      std::string line;
      {
        std::unique_lock lock(state->mutex);
        state->ready.wait(lock, [&] {
          return state->stopping || state->cancellation.stop_requested() || !state->queue.empty();
        });
        if (state->cancellation.stop_requested()) {
          state->dropped_total.fetch_add(state->queue.size(), std::memory_order_relaxed);
          state->queue.clear();
          break;
        }
        if (state->queue.empty()) {
          if (state->stopping)
            break;
          continue;
        }
        line = std::move(state->queue.front());
        state->queue.pop_front();
      }

      DiagnosticWriteResult result = DiagnosticWriteResult::Failed;
      try {
        result = state->sink->write(line, state->cancellation.get_token());
      } catch (...) {
        result = DiagnosticWriteResult::Failed;
      }
      switch (result) {
        case DiagnosticWriteResult::Written:
          state->emitted_total.fetch_add(1, std::memory_order_relaxed);
          break;
        case DiagnosticWriteResult::TimedOut:
          state->sink_timeouts_total.fetch_add(1, std::memory_order_relaxed);
          state->dropped_total.fetch_add(1, std::memory_order_relaxed);
          break;
        case DiagnosticWriteResult::Cancelled:
        case DiagnosticWriteResult::Failed:
          state->sink_failures_total.fetch_add(1, std::memory_order_relaxed);
          state->dropped_total.fetch_add(1, std::memory_order_relaxed);
          break;
      }
    }
  } catch (...) {
    state->sink_failures_total.fetch_add(1, std::memory_order_relaxed);
  }
  {
    std::lock_guard lock(state->mutex);
    state->worker_finished = true;
  }
  state->finished.notify_all();
}

void DiagnosticEmitter::stop() noexcept {
  const auto state = state_;
  if (!state)
    return;
  {
    std::lock_guard lifecycle_lock(lifecycle_mutex_);
    if (!worker_.joinable())
      return;
    if (!stop_started_) {
      stop_started_ = true;
      state->accepting.store(false, std::memory_order_release);
      {
        std::lock_guard state_lock(state->mutex);
        state->stopping = true;
      }
      state->ready.notify_all();
    }
  }

  std::unique_lock state_lock(state->mutex);
  bool finished = state->finished.wait_for(state_lock, state->limits.shutdown_timeout,
                                           [&] { return state->worker_finished; });
  state_lock.unlock();
  if (!finished) {
    state->shutdown_timeouts_total.fetch_add(1, std::memory_order_relaxed);
    state->cancellation.request_stop();
    {
      std::lock_guard lock(state->mutex);
      state->dropped_total.fetch_add(state->queue.size(), std::memory_order_relaxed);
      state->queue.clear();
    }
    state->ready.notify_all();
  }

  std::lock_guard lifecycle_lock(lifecycle_mutex_);
  if (!worker_.joinable())
    return;
  // The production stdout sink has a fixed poll deadline and checks the stop
  // token between polls. Test sinks use the same cooperative contract, so this
  // joins rather than orphaning a worker that could outlive shutdown.
  worker_.join();
}

DiagnosticEmitterStats DiagnosticEmitter::stats() const noexcept {
  DiagnosticEmitterStats result;
  const auto state = state_;
  if (!state)
    return result;
  result.enqueued_total = state->enqueued_total.load(std::memory_order_relaxed);
  result.emitted_total = state->emitted_total.load(std::memory_order_relaxed);
  result.dropped_total = state->dropped_total.load(std::memory_order_relaxed);
  result.oversized_total = state->oversized_total.load(std::memory_order_relaxed);
  result.malformed_total = state->malformed_total.load(std::memory_order_relaxed);
  result.sink_failures_total = state->sink_failures_total.load(std::memory_order_relaxed);
  result.sink_timeouts_total = state->sink_timeouts_total.load(std::memory_order_relaxed);
  result.shutdown_timeouts_total = state->shutdown_timeouts_total.load(std::memory_order_relaxed);
  result.accepting = state->accepting.load(std::memory_order_acquire);
  try {
    std::lock_guard lock(state->mutex);
    result.queued = state->queue.size();
  } catch (...) {
  }
  return result;
}

}  // namespace mine_teleop::detail
