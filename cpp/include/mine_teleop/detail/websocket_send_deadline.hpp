#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <string_view>
#include <thread>
#include <utility>

#if defined(__APPLE__)
#include <dispatch/dispatch.h>
#endif

namespace mine_teleop::detail {

// Owns no CURL handle or socket. The connection owner supplies shutdown-only
// interruption and destroys this guard BEFORE freeing/reusing its descriptor.
// One deadline observer per connection also covers TLS backends which wait
// inside a single curl_easy_send, beyond the caller's CURLE_AGAIN loop.
class WebSocketSendDeadline {
 public:
  using Clock = std::chrono::steady_clock;
  explicit WebSocketSendDeadline(std::function<void()> interrupt)
      : interrupt_(std::move(interrupt)) {
#if defined(__APPLE__)
    const auto attributes = dispatch_queue_attr_make_with_qos_class(
        DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
    queue_ = dispatch_queue_create("mine-teleop.websocket-send-deadline", attributes);
    timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, DISPATCH_TIMER_STRICT, queue_);
    if (!timer_) {
      dispatch_release(queue_);
      throw std::runtime_error("could not create websocket send deadline timer");
    }
    dispatch_set_context(timer_, this);
    dispatch_source_set_event_handler_f(timer_, [](void* context) {
      static_cast<WebSocketSendDeadline*>(context)->on_timer();
    });
    dispatch_source_set_cancel_handler_f(timer_, [](void* context) {
      auto& owner = *static_cast<WebSocketSendDeadline*>(context);
      std::lock_guard lock(owner.mutex_);
      owner.cancelled_ = true;
      owner.changed_.notify_all();
    });
    dispatch_source_set_timer(timer_, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
    dispatch_resume(timer_);
#else
    worker_ = std::thread([this] { run(); });
#endif
  }
  WebSocketSendDeadline(const WebSocketSendDeadline&) = delete;
  WebSocketSendDeadline& operator=(const WebSocketSendDeadline&) = delete;
  ~WebSocketSendDeadline() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
#if defined(__APPLE__)
    // Cancellation is asynchronous. Its handler runs only after any event
    // callback has returned; wait before the socket owner can be destroyed.
    dispatch_source_cancel(timer_);
    { std::unique_lock lock(mutex_); changed_.wait(lock, [this] { return cancelled_; }); }
    dispatch_release(timer_);
    dispatch_release(queue_);
#else
    changed_.notify_all();
    worker_.join();
#endif
  }
  void arm(Clock::time_point deadline) {
    std::lock_guard lock(mutex_);
    if (interrupted_) throw std::runtime_error("websocket connection retired after interrupted send");
    deadline_ = deadline; armed_ = true; changed_locked();
  }
  void disarm() {
    std::lock_guard lock(mutex_);
    armed_ = false; changed_locked();
  }
  [[nodiscard]] bool finish() {
    std::lock_guard lock(mutex_);
    if (!interrupted_ && armed_ && Clock::now() < deadline_) {
      armed_ = false; changed_locked(); return true;
    }
    if (armed_) interrupt_locked();
    return false;
  }
  void interrupt() {
    std::lock_guard lock(mutex_);
    if (armed_) interrupt_locked();
  }
  [[nodiscard]] bool interrupted() const {
    std::lock_guard lock(mutex_); return interrupted_;
  }
 private:
  void interrupt_locked() {
    if (interrupted_) return;
    interrupted_ = true; armed_ = false; changed_locked();
    // Keep descriptor retirement synchronized with the owner's disarm/join.
    // This callback must be noexcept, shutdown-only and must not use libcurl.
    interrupt_();
  }
  void changed_locked() {
#if defined(__APPLE__)
    // Ordinary timed waits on macOS can coalesce a20ms deadline by ~10ms.
    // STRICT with zero leeway avoids that tolerance, but is not hard realtime.
    // Derive each one-shot from the original deadline; never renew its budget.
    const auto remaining = std::max(std::chrono::nanoseconds::zero(),
        std::chrono::ceil<std::chrono::nanoseconds>(deadline_ - Clock::now()));
    const auto start = armed_ && !stopping_
        ? dispatch_time(DISPATCH_TIME_NOW, remaining.count()) : DISPATCH_TIME_FOREVER;
    dispatch_source_set_timer(timer_, start, DISPATCH_TIME_FOREVER, 0);
#else
    ++revision_; changed_.notify_all();
#endif
  }
#if defined(__APPLE__)
  void on_timer() {
    std::lock_guard lock(mutex_);
    if (stopping_ || !armed_) return;
    // A queued event may belong to a previous send, or arrive slightly early.
    // It must not interrupt a newly armed operation before its own deadline.
    if (Clock::now() < deadline_) changed_locked();
    else interrupt_locked();
  }
#else
  void run() {
    std::unique_lock lock(mutex_);
    while (!stopping_) {
      changed_.wait(lock, [this] { return stopping_ || armed_; });
      if (stopping_) break;
      const auto revision = revision_;
      if (!changed_.wait_until(lock, deadline_, [this, revision] {
            return stopping_ || !armed_ || revision_ != revision;
          })) interrupt_locked();
    }
  }
#endif
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::function<void()> interrupt_;
  Clock::time_point deadline_{};
  bool stopping_{false}, armed_{false}, interrupted_{false};
#if defined(__APPLE__)
  dispatch_queue_t queue_{nullptr};
  dispatch_source_t timer_{nullptr};
  bool cancelled_{false};
#else
  std::size_t revision_{0};
  std::thread worker_;
#endif
};

struct WebSocketWriteProgress { std::size_t bytes{0}; bool again{false}; };
struct WebSocketWriteObservation {
  std::size_t bytes{0}, calls{0}, again{0}, polls{0};
  std::int64_t longest_call_ms{0}, poll_ms{0};
};

// All progress branches share ONE absolute deadline. A completed late write
// is an uncertain delivery, never success and never a retry on this stream.
template <class Send, class Wait, class Now, class Interrupted>
void send_websocket_bytes(std::string_view bytes,
    std::chrono::steady_clock::time_point deadline, std::stop_token stop,
    Send send, Wait wait, Now now, Interrupted interrupted,
    WebSocketWriteObservation& observation) {
  const auto check = [&] {
    if (stop.stop_requested()) throw std::runtime_error("websocket send cancelled");
    if (interrupted() || now() >= deadline) throw std::runtime_error("websocket send timed out");
  };
  while (observation.bytes < bytes.size()) {
    check();
    const auto started = now(); ++observation.calls;
    WebSocketWriteProgress progress;
    const auto finish_call = [&] {
      observation.longest_call_ms = std::max(observation.longest_call_ms,
          std::chrono::duration_cast<std::chrono::milliseconds>(now() - started).count());
    };
    try { progress = send(bytes.substr(observation.bytes)); }
    catch (...) { finish_call(); check(); throw; }
    finish_call();
    if (progress.bytes > bytes.size() - observation.bytes)
      throw std::runtime_error("websocket send returned invalid byte count");
    observation.bytes += progress.bytes;
    check();
    if (!progress.again) {
      if (progress.bytes == 0) throw std::runtime_error("websocket closed while sending");
      continue;
    }
    ++observation.again; ++observation.polls;
    const auto poll_started = now();
    const auto ready = wait(std::chrono::ceil<std::chrono::milliseconds>(deadline - poll_started));
    observation.poll_ms += std::chrono::duration_cast<std::chrono::milliseconds>(now() - poll_started).count();
    check();
    if (!ready) throw std::runtime_error("websocket send timed out");
  }
}
}  // namespace mine_teleop::detail
