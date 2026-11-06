#include "mine_teleop/bench/owned_callback_guard.hpp"
#include <atomic>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

using mine_teleop::detail::OwnedCallbackGuard;
using namespace std::chrono_literals;
namespace {
void require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
struct Owner { int calls = 0; };

void unbound_and_retired_guards_never_invoke() {
  OwnedCallbackGuard<Owner> guard;
  bool invoked = false;
  require(!guard.call([&](Owner&) { invoked = true; }) && !invoked, "an unbound guard invoked the callback");
  Owner owner;
  guard.bind(&owner);
  require(guard.call([&](Owner& value) { ++value.calls; }) && owner.calls == 1, "a bound guard did not invoke the callback");
  guard.retire();
  require(!guard.call([&](Owner& value) { ++value.calls; }) && owner.calls == 1, "a retired guard invoked the callback");
}

void retire_waits_for_an_in_flight_callback() {
  Owner owner;
  OwnedCallbackGuard<Owner> guard(&owner);
  std::atomic<bool> entered{false}, release{false}, retire_returned{false};
  std::thread callback([&] {
    guard.call([&](Owner& value) {
      entered = true;
      while (!release) std::this_thread::sleep_for(1ms);
      ++value.calls;  // the owner is still alive here because retire() is blocked
    });
  });
  while (!entered) std::this_thread::sleep_for(1ms);
  std::thread retirer([&] { guard.retire(); retire_returned = true; });
  std::this_thread::sleep_for(100ms);
  const bool still_blocked = !retire_returned;  // assert only after the threads are joined
  release = true;
  callback.join(); retirer.join();
  require(still_blocked, "retire() returned while a callback was still running on the owner");
  require(retire_returned && owner.calls == 1, "the in-flight callback did not complete before retire() returned");
  require(!guard.call([&](Owner& value) { ++value.calls; }) && owner.calls == 1, "a callback ran after retire()");
}
}  // namespace
int main() {
  try {
    unbound_and_retired_guards_never_invoke();
    retire_waits_for_an_in_flight_callback();
    std::cout << "PASS owned callback guard\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
