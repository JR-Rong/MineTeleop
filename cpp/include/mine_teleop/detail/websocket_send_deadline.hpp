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

namespace mine_teleop::detail {

// Owns no CURL handle or socket. The connection owner supplies shutdown-only
// interruption and destroys this guard BEFORE freeing/reusing its descriptor.
// One sleeping observer per connection also covers TLS backends which wait
// inside a single curl_easy_send, beyond the caller's CURLE_AGAIN loop.
class WebSocketSendDeadline {
 public:
  using Clock = std::chrono::steady_clock;
  explicit WebSocketSendDeadline(std::function<void()> interrupt)
      : interrupt_(std::move(interrupt)), worker_([this] { run(); }) {}
  WebSocketSendDeadline(const WebSocketSendDeadline&) = delete;
  WebSocketSendDeadline& operator=(const WebSocketSendDeadline&) = delete;
  ~WebSocketSendDeadline() {
    { std::lock_guard lock(mutex_); stopping_ = true; }
    changed_.notify_all();
    worker_.join();
  }
  void arm(Clock::time_point deadline) {
    std::lock_guard lock(mutex_);
    if (interrupted_) throw std::runtime_error("websocket connection retired after interrupted send");
    deadline_ = deadline; armed_ = true; ++revision_; changed_.notify_all();
  }
  void disarm() {
    std::lock_guard lock(mutex_);
    armed_ = false; ++revision_; changed_.notify_all();
  }
  [[nodiscard]] bool finish() {
    std::lock_guard lock(mutex_);
    if (!interrupted_ && armed_ && Clock::now() < deadline_) {
      armed_ = false; ++revision_; changed_.notify_all(); return true;
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
    interrupted_ = true; armed_ = false; ++revision_;
    // Keep descriptor retirement synchronized with the owner's disarm/join.
    // This callback must be noexcept, shutdown-only and must not use libcurl.
    interrupt_(); changed_.notify_all();
  }
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
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::function<void()> interrupt_;
  Clock::time_point deadline_{};
  std::size_t revision_{0};
  bool stopping_{false}, armed_{false}, interrupted_{false};
  std::thread worker_;
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
