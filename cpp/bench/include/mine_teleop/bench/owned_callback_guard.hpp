#pragma once

#include <mutex>
#include <utility>

namespace mine_teleop::detail {

// Lets a C callback that arrives on a foreign thread (for example libnice's)
// use an owner that may be destroyed concurrently. The owner is dereferenced
// only while the lock is held, so retire() both waits for an in-flight call and
// guarantees that no later call can touch the owner. Hand the callback a
// shared_ptr to the guard so the guard outlives the owner.
template <typename Owner>
class OwnedCallbackGuard {
 public:
  explicit OwnedCallbackGuard(Owner* owner = nullptr) : owner_(owner) {}
  OwnedCallbackGuard(const OwnedCallbackGuard&) = delete;
  OwnedCallbackGuard& operator=(const OwnedCallbackGuard&) = delete;

  void bind(Owner* owner) { std::lock_guard lock(mutex_); owner_ = owner; }
  void retire() { std::lock_guard lock(mutex_); owner_ = nullptr; }

  // Returns false without invoking the function when unbound or retired.
  template <typename Function>
  bool call(Function&& function) {
    std::lock_guard lock(mutex_);
    if (owner_ == nullptr) return false;
    std::forward<Function>(function)(*owner_);
    return true;
  }

 private:
  std::mutex mutex_;
  Owner* owner_;
};

}  // namespace mine_teleop::detail
